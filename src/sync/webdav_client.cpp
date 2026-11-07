#include "sync/webdav_client.h"

#include <QEventLoop>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>
#include <QXmlStreamReader>

namespace an {
namespace {

const char* kPropfindBody =
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
    "<d:propfind xmlns:d=\"DAV:\"><d:prop>"
    "<d:resourcetype/><d:getcontentlength/><d:getetag/><d:getlastmodified/>"
    "</d:prop></d:propfind>";

QString lastSegment(const QString& path)
{
    QString p = path;
    while (p.endsWith(QLatin1Char('/'))) p.chop(1);
    const int slash = p.lastIndexOf(QLatin1Char('/'));
    return slash >= 0 ? p.mid(slash + 1) : p;
}

}  // namespace

WebDavClient::WebDavClient()
    : nam_(new QNetworkAccessManager())
{
}

WebDavClient::~WebDavClient()
{
    delete nam_;
}

void WebDavClient::setBaseUrl(const QString& url)
{
    baseUrl_ = url.trimmed();
    while (baseUrl_.endsWith(QLatin1Char('/'))) baseUrl_.chop(1);
}

void WebDavClient::setCredentials(const QString& user, const QString& password)
{
    user_ = user;
    password_ = password;
}

QString WebDavClient::urlFor(const QString& relPath) const
{
    QString p = relPath;
    while (p.startsWith(QLatin1Char('/'))) p.remove(0, 1);
    return baseUrl_ + QLatin1Char('/') + p;
}

void WebDavClient::setLocalRoot(const QString& root)
{
    localRoot_ = root.trimmed();
    while (localRoot_.endsWith(QLatin1Char('/'))) localRoot_.chop(1);
}

QString WebDavClient::localPath(const QString& relPath) const
{
    QString p = relPath;
    while (p.startsWith(QLatin1Char('/'))) p.remove(0, 1);
    return localRoot_ + QLatin1Char('/') + p;
}

QString WebDavClient::localEtag(const QString& path) const
{
    const QFileInfo info(path);
    return QStringLiteral("%1-%2").arg(info.size()).arg(info.lastModified().toMSecsSinceEpoch());
}

bool WebDavClient::list(const QString& relPath, QVector<DavEntry>* out, QString* error)
{
    if (isLocal()) {
        QDir dir(localPath(relPath));
        if (!dir.exists()) {
            if (error) *error = QStringLiteral("PROPFIND %1 -> 404").arg(relPath);
            return false;
        }
        if (out) {
            out->clear();
            const QFileInfoList entries = dir.entryInfoList(
                QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDir::DirsFirst | QDir::Name);
            for (const QFileInfo& info : entries) {
                DavEntry entry;
                entry.name = info.fileName();
                entry.href = relPath + QLatin1Char('/') + entry.name;
                entry.isCollection = info.isDir();
                entry.contentLength = info.isDir() ? -1 : info.size();
                entry.etag = localEtag(info.absoluteFilePath());
                entry.lastModified = info.lastModified();
                out->append(entry);
            }
        }
        return true;
    }
    QNetworkRequest req(QUrl(urlFor(relPath)));
    req.setRawHeader("Depth", "1");
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/xml; charset=utf-8"));
    if (!user_.isEmpty())
        req.setRawHeader("Authorization",
                         "Basic " + QStringLiteral("%1:%2").arg(user_, password_).toUtf8().toBase64());

    QNetworkReply* reply =
        nam_->sendCustomRequest(req, QByteArrayLiteral("PROPFIND"), QByteArray(kPropfindBody));

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() { reply->abort(); });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs_);
    loop.exec();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    reply->deleteLater();

    if (status < 200 || status >= 300) {
        if (error) *error = QStringLiteral("PROPFIND %1 -> HTTP %2").arg(relPath).arg(status);
        return false;
    }

    if (!out) return true;
    out->clear();

    QXmlStreamReader xml(body);
    DavEntry current;
    bool inResponse = false;
    bool inPropstatOk = false;
    while (!xml.atEnd()) {
        xml.readNext();
        if (xml.isStartElement()) {
            const QString name = xml.name().toString().toLower();
            if (name == QLatin1String("response")) {
                current = DavEntry();
                inResponse = true;
            } else if (name == QLatin1String("status")) {
                const QString statusText = xml.readElementText();
                inPropstatOk = statusText.contains(QLatin1String(" 200"));
            } else if (name == QLatin1String("href") && inResponse) {
                const QString href = QUrl::fromPercentEncoding(xml.readElementText().toUtf8());
                current.href = href;
                current.name = lastSegment(href);
            } else if (name == QLatin1String("getcontentlength") && inPropstatOk) {
                const QString text = xml.readElementText().trimmed();
                bool ok = false;
                const qint64 v = text.toLongLong(&ok);
                current.contentLength = ok ? v : -1;
            } else if (name == QLatin1String("getetag") && inPropstatOk) {
                current.etag = xml.readElementText().trimmed();
            } else if (name == QLatin1String("getlastmodified") && inPropstatOk) {
                current.lastModified = QDateTime::fromString(xml.readElementText().trimmed(), Qt::RFC2822Date);
            } else if (name == QLatin1String("collection")) {
                current.isCollection = true;
            }
        } else if (xml.isEndElement()) {
            const QString name = xml.name().toString().toLower();
            if (name == QLatin1String("response")) {
                if (inResponse) out->append(current);
                inResponse = false;
            } else if (name == QLatin1String("propstat")) {
                inPropstatOk = false;
            }
        }
    }
    if (xml.hasError() && error) *error = QStringLiteral("PROPFIND XML parse: %1").arg(xml.errorString());
    return true;
}

bool WebDavClient::get(const QString& relPath, QByteArray* out, QString* etag, QString* error,
                       bool* notFound)
{
    if (notFound) *notFound = false;
    if (isLocal()) {
        const QString path = localPath(relPath);
        QFile file(path);
        if (!file.exists()) {
            if (notFound) *notFound = true;
            if (error) *error = QStringLiteral("404 not found: %1").arg(relPath);
            return false;
        }
        if (!file.open(QIODevice::ReadOnly)) {
            if (error) *error = file.errorString();
            return false;
        }
        if (out) *out = file.readAll();
        if (etag) *etag = localEtag(path);
        return true;
    }
    QNetworkRequest req(QUrl(urlFor(relPath)));
    if (!user_.isEmpty())
        req.setRawHeader("Authorization",
                         "Basic " + QStringLiteral("%1:%2").arg(user_, password_).toUtf8().toBase64());

    QNetworkReply* reply = nam_->get(req);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() { reply->abort(); });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs_);
    loop.exec();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const QString et = QString::fromLatin1(reply->rawHeader("ETag"));
    reply->deleteLater();

    if (status == 404) {
        if (notFound) *notFound = true;
        if (etag) etag->clear();
        if (error) *error = QStringLiteral("GET %1 -> 404").arg(relPath);
        return false;
    }
    if (status < 200 || status >= 300) {
        if (error) *error = QStringLiteral("GET %1 -> HTTP %2").arg(relPath).arg(status);
        return false;
    }
    if (out) *out = body;
    if (etag) *etag = et;
    return true;
}

bool WebDavClient::put(const QString& relPath, const QByteArray& data, const QString& ifMatchEtag,
                       QString* error, bool* preconditionFailed)
{
    if (preconditionFailed) *preconditionFailed = false;
    if (isLocal()) {
        const QString path = localPath(relPath);
        if (!ifMatchEtag.isEmpty() && QFileInfo::exists(path) && localEtag(path) != ifMatchEtag) {
            if (preconditionFailed) *preconditionFailed = true;
            if (error) *error = QStringLiteral("412 precondition failed: %1").arg(relPath);
            return false;
        }
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            if (error) *error = file.errorString();
            return false;
        }
        if (file.write(data) != data.size()) {
            if (error) *error = QStringLiteral("写入不完整：%1").arg(path);
            return false;
        }
        return true;
    }
    QNetworkRequest req(QUrl(urlFor(relPath)));
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json; charset=utf-8"));
    if (!user_.isEmpty())
        req.setRawHeader("Authorization",
                         "Basic " + QStringLiteral("%1:%2").arg(user_, password_).toUtf8().toBase64());
    if (!ifMatchEtag.isEmpty()) req.setRawHeader("If-Match", ifMatchEtag.toLatin1());

    QNetworkReply* reply = nam_->put(req, data);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() { reply->abort(); });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs_);
    loop.exec();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    if (status == 412) {
        if (preconditionFailed) *preconditionFailed = true;
        if (error) *error = QStringLiteral("PUT %1 -> 412 precondition failed").arg(relPath);
        return false;
    }
    if (status < 200 || status >= 300) {
        if (error) *error = QStringLiteral("PUT %1 -> HTTP %2").arg(relPath).arg(status);
        return false;
    }
    return true;
}

bool WebDavClient::mkcol(const QString& relPath, QString* error)
{
    if (isLocal()) {
        if (QDir().mkpath(localPath(relPath))) return true;
        if (error) *error = QStringLiteral("无法创建目录：%1").arg(localPath(relPath));
        return false;
    }
    QNetworkRequest req(QUrl(urlFor(relPath)));
    if (!user_.isEmpty())
        req.setRawHeader("Authorization",
                         "Basic " + QStringLiteral("%1:%2").arg(user_, password_).toUtf8().toBase64());

    QNetworkReply* reply = nam_->sendCustomRequest(req, QByteArrayLiteral("MKCOL"), QByteArray());
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() { reply->abort(); });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs_);
    loop.exec();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();

    // 201 created, 405 already exists, 301/302 redirect to existing collection
    if (status == 201 || status == 405 || (status >= 300 && status < 400) || status == 200) return true;
    if (error) *error = QStringLiteral("MKCOL %1 -> HTTP %2").arg(relPath).arg(status);
    return false;
}

bool WebDavClient::remove(const QString& relPath, QString* error)
{
    if (isLocal()) {
        const QString path = localPath(relPath);
        const QFileInfo info(path);
        if (!info.exists()) return true;
        const bool ok = info.isDir() ? QDir(path).removeRecursively() : QFile::remove(path);
        if (!ok && error) *error = QStringLiteral("无法删除：%1").arg(path);
        return ok;
    }
    QNetworkRequest req(QUrl(urlFor(relPath)));
    if (!user_.isEmpty())
        req.setRawHeader("Authorization",
                         "Basic " + QStringLiteral("%1:%2").arg(user_, password_).toUtf8().toBase64());

    QNetworkReply* reply = nam_->deleteResource(req);
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() { reply->abort(); });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs_);
    loop.exec();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    reply->deleteLater();
    if ((status >= 200 && status < 300) || status == 404) return true;
    if (error) *error = QStringLiteral("DELETE %1 -> HTTP %2").arg(relPath).arg(status);
    return false;
}

}  // namespace an
