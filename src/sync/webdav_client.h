#pragma once

#include <QByteArray>
#include <QDateTime>
#include <QMap>
#include <QString>
#include <QVector>

class QNetworkAccessManager;
class QNetworkRequest;

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
    // Local-folder transport: the same snapshot layout, backed by the filesystem.
    // Useful for USB drives and NAS mounts, and it keeps the merge logic untouched.
    void setLocalRoot(const QString& root);
    bool isLocal() const { return !localRoot_.isEmpty(); }
    // S3 / MinIO with path-style addressing and SigV4. `endpoint` is the server root
    // (https://s3.amazonaws.com or http://127.0.0.1:9000); `bucket` is signed into the
    // path; `remotePath` keeps working as the key prefix.
    void setS3(const QString& endpoint, const QString& region, const QString& bucket,
               const QString& accessKey, const QString& secretKey);
    bool isS3() const { return s3_; }
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

    // Pure SigV4 helper. `headers` maps lowercase header name -> value and must include
    // host and x-amz-date. Exposed so the AWS get-vanilla test vector can pin it down
    // without a network round trip.
    static QByteArray awsV4Authorization(const QByteArray& method, const QString& canonicalUri,
                                         const QString& canonicalQuery,
                                         const QMap<QString, QString>& headers,
                                         const QByteArray& payload, const QString& amzDate,
                                         const QString& region, const QString& service,
                                         const QString& accessKey, const QString& secretKey);

private:
    QString urlFor(const QString& relPath) const;
    QString localPath(const QString& relPath) const;
    QString localEtag(const QString& path) const;
    QString s3Uri(const QString& relPath) const;
    QNetworkRequest s3Request(const QByteArray& method, const QString& relPath,
                              const QByteArray& payload) const;
    bool s3Get(const QString& relPath, QByteArray* out, QString* etag, QString* error,
               bool* notFound);
    bool s3Put(const QString& relPath, const QByteArray& data, const QString& ifMatchEtags,
               QString* error, bool* preconditionFailed);
    bool s3Remove(const QString& relPath, QString* error);

    QNetworkAccessManager* nam_ = nullptr;
    QString baseUrl_;
    QString localRoot_;
    bool s3_ = false;
    QString s3Bucket_;
    QString s3Region_ = QStringLiteral("us-east-1");
    QString user_;
    QString password_;
    int timeoutMs_ = 20000;
};

}  // namespace an
