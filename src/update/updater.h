#pragma once

#include <QCryptographicHash>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QFile;
class QNetworkAccessManager;
class QNetworkReply;
class QTemporaryDir;

namespace an {
namespace update {

struct ReleaseInfo {
    QString tag;
    QString name;
    QString notes;
    QString assetName;
    QUrl assetUrl;
    QString sha256;
    qint64 size = 0;
    QString commit;
    QString published;
};

// Checks GitHub Releases for a Windows portable zip, verifies its sha256 and
// hands off to a small PowerShell script that replaces the app after it exits.
class Updater : public QObject {
    Q_OBJECT
public:
    explicit Updater(QObject* parent = nullptr);

    static QString currentVersion();
    static QString currentCommit();
    static QString currentBuildTime();
    static QString commitFromTag(const QString& tag);
    static ReleaseInfo parseRelease(const QByteArray& json);
    static bool isNewer(const ReleaseInfo& info, const QString& currentCommit,
                        const QString& currentBuildTime = QString());
    static QString buildWindowsUpdateScript(qint64 appPid, const QString& installDir,
                                            const QString& zipPath);

    void check();
    void downloadAndInstall(const ReleaseInfo& info);
    void abort();
    bool busy() const { return downloading_; }

signals:
    void checkFinished(const an::update::ReleaseInfo& info, bool available,
                       const QString& error);
    void downloadProgress(qint64 received, qint64 total);
    void downloadFinished();
    void failed(const QString& error);

private:
    void onCheckFinished();
    void onDownloadReadyRead();
    void onDownloadFinished();
    bool launchInstaller();
    void cleanupTemp();

    QNetworkAccessManager* net_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QFile* file_ = nullptr;
    QCryptographicHash hash_{QCryptographicHash::Sha256};
    ReleaseInfo info_;
    QTemporaryDir* temp_ = nullptr;
    QString zipPath_;
    QString installDir_;
    qint64 received_ = 0;
    bool downloading_ = false;
};

}  // namespace update
}  // namespace an
