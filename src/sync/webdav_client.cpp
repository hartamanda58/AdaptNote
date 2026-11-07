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

QByteArray sha256Bytes(const QByteArray& data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256);
}

QString sha256Hex(const QByteArray& data)
{
    return QString::fromLatin1(sha256Bytes(data).toHex());
}

QByteArray hmacSha256(const QByteArray& key, const QByteArray& data)
{
    constexpr int kBlock = 64;
    QByteArray normalised = key.size() > kBlock ? sha256Bytes(key) : key;
    // QByteArray::resize() leaves grown bytes uninitialised; HMAC requires zero padding.
    if (normalised.size() < kBlock)
        normalised.append(QByteArray(kBlock - normalised.size(), '\0'));
    QByteArray outer(kBlock, '\x5c');
    QByteArray inner(kBlock, '\x36');
    for (int i = 0; i < kBlock; ++i) {
        outer[i] = static_cast<char>(outer.at(i) ^ normalised.at(i));
        inner[i] = static_cast<char>(inner.at(i) ^ normalised.at(i));
    }
    return sha256Bytes(outer + sha256Bytes(inner + data));
}

QString uriEncode(const QString& text, bool encodeSlash)
{
    static const char* hex = "0123456789ABCDEF";
    QByteArray out;
    const QByteArray bytes = text.toUtf8();
    for (char ch : bytes) {
        const unsigned char c = static_cast<unsigned char>(ch);
        const bool unreserved = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                                c == '.' || c == '~';
        if (unreserved || (c == '/' && !encodeSlash)) {
            out.append(ch);
        } else {
            out.append('%');
            out.append(hex[c >> 4]);
            out.append(hex[c & 0x0f]);
        }
    }
    return QString::fromLatin1(out);
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
    s3_ = false;
    localRoot_.clear();
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
    s3_ = false;
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

void WebDavClient::setS3(const QString& endpoint, const QString& region, const QString& bucket,
                         const QString& accessKey, const QString& secretKey)
{
    QString root = endpoint.trimmed();
    while (root.endsWith(QLatin1Char('/'))) root.chop(1);
    baseUrl_ = root;
    localRoot_.clear();
    s3_ = true;
    s3Region_ = region.trimmed().isEmpty() ? QStringLiteral("us-east-1") : region.trimmed();
    s3Bucket_ = bucket.trimmed();
    user_ = accessKey.trimmed();
    password_ = secretKey;
}

QString WebDavClient::s3Uri(const QString& relPath) const
{
    QString rel = relPath;
    while (rel.startsWith(QLatin1Char('/'))) rel.remove(0, 1);
    const QString bucket = uriEncode(s3Bucket_, false);
    if (rel.isEmpty()) return QLatin1Char('/') + bucket + QLatin1Char('/');
    return QLatin1Char('/') + bucket + QLatin1Char('/') + uriEncode(rel, false);
}

QByteArray WebDavClient::awsV4Authorization(const QByteArray& method,
                                            const QString& canonicalUri,
                                            const QString& canonicalQuery,
                                            const QMap<QString, QString>& headers,
                                            const QByteArray& payload, const QString& amzDate,
                                            const QString& region, const QString& service,
                                            const QString& accessKey, const QString& secretKey)
{
    QStringList names = headers.keys();
    names.sort();
    QString canonicalHeaders;
    for (const QString& name : names)
        canonicalHeaders += name + QLatin1Char(':') + headers.value(name).trimmed() + QLatin1Char('\n');
    const QString signedHeaders = names.join(QLatin1Char(';'));

    const QString canonicalRequest = QString::fromLatin1(method) + QLatin1Char('\n') +
                                     canonicalUri + QLatin1Char('\n') + canonicalQuery +
                                     QLatin1Char('\n') + canonicalHeaders + QLatin1Char('\n') +
                                     signedHeaders + QLatin1Char('\n') + sha256Hex(payload);

    const QString dateStamp = amzDate.left(8);
    const QString scope = dateStamp + QLatin1Char('/') + region + QLatin1Char('/') + service +
                          QStringLiteral("/aws4_request");
    const QString stringToSign = QStringLiteral("AWS4-HMAC-SHA256\n") + amzDate +
                                 QLatin1Char('\n') + scope + QLatin1Char('\n') +
                                 sha256Hex(canonicalRequest.toUtf8());

    const QByteArray kDate = hmacSha256(QByteArrayLiteral("AWS4") + secretKey.toUtf8(),
                                        dateStamp.toUtf8());
    const QByteArray kRegion = hmacSha256(kDate, region.toUtf8());
    const QByteArray kService = hmacSha256(kRegion, service.toUtf8());
    const QByteArray kSigning = hmacSha256(kService, QByteArrayLiteral("aws4_request"));
    const QString signature = QString::fromLatin1(hmacSha256(kSigning, stringToSign.toUtf8()).toHex());

    return QStringLiteral("AWS4-HMAC-SHA256 Credential=%1/%2, SignedHeaders=%3, Signature=%4")
        .arg(accessKey, scope, signedHeaders, signature)
        .toUtf8();
}

QNetworkRequest WebDavClient::s3Request(const QByteArray& method, const QString& relPath,
                                        const QByteArray& payload) const
{
    const QString uri = s3Uri(relPath);
    QUrl url(baseUrl_);
    url.setPath(uri);

    const QString amzDate = QDateTime::currentDateTimeUtc()
                                .toString(QStringLiteral("yyyyMMdd'T'HHmmss'Z'"));
    QString host = url.host();
    if (url.port() > 0) host += QLatin1Char(':') + QString::number(url.port());

    QMap<QString, QString> headers;
    headers.insert(QStringLiteral("host"), host);
    headers.insert(QStringLiteral("x-amz-content-sha256"), sha256Hex(payload));
    headers.insert(QStringLiteral("x-amz-date"), amzDate);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/octet-stream"));
    request.setRawHeader("x-amz-date", amzDate.toUtf8());
    request.setRawHeader("x-amz-content-sha256", sha256Hex(payload).toUtf8());
    request.setRawHeader("Authorization",
                         awsV4Authorization(method, uri, QString(), headers, payload, amzDate,
                                            s3Region_, QStringLiteral("s3"), user_, password_));
    request.setTransferTimeout(timeoutMs_);
    return request;
}

bool WebDavClient::s3Get(const QString& relPath, QByteArray* out, QString* etag, QString* error,
                         bool* notFound)
{
    if (notFound) *notFound = false;
    QNetworkReply* reply = nam_->get(s3Request(QByteArrayLiteral("GET"), relPath, QByteArray()));
    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);
    QObject::connect(&timer, &QTimer::timeout, &loop, [&]() { reply->abort(); });
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    timer.start(timeoutMs_);
    loop.exec();

    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();
    const QString responseEtag = QString::fromLatin1(reply->rawHeader("ETag"));
    reply->deleteLater();

    if (status == 404) {
        if (notFound) *notFound = true;
        if (error) *error = QStringLiteral("404 not found: %1").arg(relPath);
        return false;
    }
    if (status < 200 || status >= 300) {
        if (error) *error = QStringLiteral("S3 GET %1 -> HTTP %2：%3")
                                .arg(relPath).arg(status)
                                .arg(QString::fromUtf8(body.left(300)));
        return false;
    }
    if (out) *out = body;
    if (etag) *etag = responseEtag;
    return true;
}

bool WebDavClient::s3Put(const QString& relPath, const QByteArray& data,
                         const QString& ifMatchEtags, QString* error, bool* preconditionFailed)
{
    if (preconditionFailed) *preconditionFailed = false;
    // S3 conditional PUT support varies by provider; checking first is portable and the
    // objects are small JSON snapshots.
    if (!ifMatchEtags.isEmpty()) {
        QByteArray current;
        QString currentEtag;
        QString getError;
        bool missing = false;
        if (!s3Get(relPath, &current, &currentEtag, &getError, &missing) || missing ||
            currentEtag != ifMatchEtags) {
            if (preconditionFailed) *preconditionFailed = true;
            if (error) *error = QStringLiteral("412 precondition failed: %1").arg(relPath);
            return false;
        }
    }

    QNetworkReply* reply = nam_->put(s3Request(QByteArrayLiteral("PUT"), relPath, data), data);
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
    if (status == 412) {
        if (preconditionFailed) *preconditionFailed = true;
        if (error) *error = QStringLiteral("412 precondition failed: %1").arg(relPath);
        return false;
    }
    if (status < 200 || status >= 300) {
        if (error) *error = QStringLiteral("S3 PUT %1 -> HTTP %2：%3")
                                .arg(relPath).arg(status)
                                .arg(QString::fromUtf8(body.left(300)));
        return false;
    }
    return true;
}

bool WebDavClient::s3Remove(const QString& relPath, QString* error)
{
    QNetworkReply* reply = nam_->deleteResource(s3Request(QByteArrayLiteral("DELETE"), relPath, QByteArray()));
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
    if ((status >= 200 && status < 300) || status == 404) return true;
    if (error) *error = QStringLiteral("S3 DELETE %1 -> HTTP %2：%3")
                            .arg(relPath).arg(status)
                            .arg(QString::fromUtf8(body.left(300)));
    return false;
}

bool WebDavClient::list(const QString& relPath, QVector<DavEntry>* out, QString* error)
{
    if (isS3()) {
        // SyncManager does not list; keep the contract honest instead of faking it.
        if (error) *error = QStringLiteral("S3 同步不需要目录列表");
        return false;
    }
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
    if (isS3()) return s3Get(relPath, out, etag, error, notFound);
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
    if (isS3()) return s3Put(relPath, data, ifMatchEtag, error, preconditionFailed);
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
    if (isS3()) {
        Q_UNUSED(relPath);
        Q_UNUSED(error);
        return true;   // buckets are created out of band
    }
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
    if (isS3()) return s3Remove(relPath, error);
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
