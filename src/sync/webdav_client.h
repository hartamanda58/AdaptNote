#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QString>
#include <QVector>

class QNetworkAccessManager;

namespace an {

struct DavEntry {
    QString href;          // host-relative, percent-decoded
    QString name;          // last path segment
    bool isCollection = false;
    qint64 contentLength = -1;
    QString etag;
    QDateTime lastModified;
};

// Small blocking WebDAV client (PROPFIND / GET / PUT / MKCOL / DELETE) on top of
// QNetworkAccessManager. Every call runs its own local event loop with a timeout.
class WebDavClient {
public:
    WebDavClient();
    ~WebDavClient();

    void setBaseUrl(const QString& url);
    QString baseUrl() const { return baseUrl_; }
    void setCredentials(const QString& user, const QString& password);
    void setTimeoutMs(int ms) { timeoutMs_ = ms; }

    bool list(const QString& relPath, QVector<DavEntry>* out, QString* error);
    bool get(const QString& relPath, QByteArray* out, QString* etag, QString* error,
             bool* notFound = nullptr);
    bool put(const QString& relPath, const QByteArray& data, const QString& ifMatchEtags,
             QString* error, bool* preconditionFailed = nullptr);
    bool mkcol(const QString& relPath, QString* error);
    bool remove(const QString& relPath, QString* error);

private:
    QString urlFor(const QString& relPath) const;

    QNetworkAccessManager* nam_ = nullptr;
    QString baseUrl_;
    QString user_;
    QString password_;
    int timeoutMs_ = 20000;
};

}  // namespace an
