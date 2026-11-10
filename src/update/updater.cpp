#include "update/updater.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>

namespace an {
namespace update {
namespace {

QString versionMacro()
{
#ifdef ADAPT_VERSION
    return QStringLiteral(ADAPT_VERSION);
#else
    return QStringLiteral("dev");
#endif
}

QString commitMacro()
{
#ifdef ADAPT_COMMIT
    return QStringLiteral(ADAPT_COMMIT);
#else
    return QString();
#endif
}

QString buildTimeMacro()
{
#ifdef ADAPT_BUILD_TIME
    return QStringLiteral(ADAPT_BUILD_TIME);
#else
    return QString();
#endif
}

QString escapePowerShell(const QString& value)
{
    QString escaped = value;
    escaped.replace(QLatin1Char('\''), QStringLiteral("''"));
    return escaped;
}

}  // namespace

Updater::Updater(QObject* parent)
    : QObject(parent), net_(new QNetworkAccessManager(this))
{
}

QString Updater::currentVersion()
{
    return versionMacro();
}

QString Updater::currentCommit()
{
    return commitMacro();
}

QString Updater::currentBuildTime()
{
    return buildTimeMacro();
}

QString Updater::commitFromTag(const QString& tag)
{
    const int dash = tag.lastIndexOf(QLatin1Char('-'));
    if (dash < 0) return {};
    const QString tail = tag.mid(dash + 1);
    static const QRegularExpression hex(QStringLiteral("^[0-9a-fA-F]{7,40}$"));
    return hex.match(tail).hasMatch() ? tail.toLower() : QString();
}

ReleaseInfo Updater::parseRelease(const QByteArray& json)
{
    ReleaseInfo info;
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return info;
    const QJsonObject release = doc.object();
    info.tag = release.value(QStringLiteral("tag_name")).toString();
    info.name = release.value(QStringLiteral("name")).toString();
    info.notes = release.value(QStringLiteral("body")).toString();
    info.published = release.value(QStringLiteral("published_at")).toString();
    info.commit = commitFromTag(info.tag);

    const QJsonArray assets = release.value(QStringLiteral("assets")).toArray();
    for (const QJsonValue& value : assets) {
        const QJsonObject asset = value.toObject();
        const QString name = asset.value(QStringLiteral("name")).toString();
        if (!name.contains(QStringLiteral("windows-x64"), Qt::CaseInsensitive) ||
            !name.endsWith(QStringLiteral(".zip"), Qt::CaseInsensitive))
            continue;
        info.assetName = name;
        info.assetUrl = QUrl(asset.value(QStringLiteral("browser_download_url")).toString());
        const QString digest = asset.value(QStringLiteral("digest")).toString();
        if (digest.startsWith(QStringLiteral("sha256:")))
            info.sha256 = digest.mid(7).trimmed().toLower();
        info.size = static_cast<qint64>(
            asset.value(QStringLiteral("size")).toDouble());
        break;
    }
    return info;
}

bool Updater::isNewer(const ReleaseInfo& info, const QString& currentCommit,
                      const QString& currentBuildTime)
{
    // Tags are `<name>-<short-sha>`, but a dev build can be newer than the latest
    // release even when the sha differs. Only offer an update when the release was
    // published after this binary was built.
    const QString latest = info.commit.toLower();
    const QString current = currentCommit.toLower();
    if (!latest.isEmpty() && !current.isEmpty() && current != QStringLiteral("unknown")) {
        if (latest == current) return false;
        if (latest.startsWith(current) || current.startsWith(latest)) return false;
    }
    const QString stamp = currentBuildTime.isEmpty() ? buildTimeMacro() : currentBuildTime;
    const QDateTime built = QDateTime::fromString(stamp, Qt::ISODate);
    const QDateTime published = QDateTime::fromString(info.published, Qt::ISODate);
    if (built.isValid() && published.isValid())
        return published > built.addSecs(60);
    // No build timestamp (old binary): fall back to "different commit = newer".
    return !latest.isEmpty() && !current.isEmpty() && current != QStringLiteral("unknown");
}


QString Updater::buildWindowsUpdateScript(qint64 appPid, const QString& installDir,
                                          const QString& zipPath)
{
    // robocopy excludes the user's data/ directory; stale launchers are removed.
    // The script waits for this process to exit before touching bin/.
    return QStringLiteral(
               "$ErrorActionPreference = 'Stop'\n"
               "$log = Join-Path $env:TEMP 'adaptnote-update.log'\n"
               "function Write-Log([string]$m) { "
               "Add-Content -LiteralPath $log -Value ('[{0}] {1}' -f (Get-Date -Format o), $m) }\n"
               "try {\n"
               "  Write-Log 'waiting for AdaptNote to exit'\n"
               "  Wait-Process -Id %1 -Timeout 90 -ErrorAction SilentlyContinue\n"
               "  Start-Sleep -Milliseconds 600\n"
               "  $work = Join-Path $env:TEMP ('adaptnote-extract-' + [guid]::NewGuid().ToString('N'))\n"
               "  Expand-Archive -LiteralPath '%2' -DestinationPath $work -Force\n"
               "  $root = Get-ChildItem -LiteralPath $work -Directory | Select-Object -First 1\n"
               "  if (-not $root) { $root = Get-Item -LiteralPath $work }\n"
               "  Write-Log ('copying ' + $root.FullName + ' -> ' + '%3')\n"
               "  robocopy $root.FullName '%3' /E /XD data /NFL /NDL /NJH /NJS /NP | Out-Null\n"
               "  Remove-Item -LiteralPath (Join-Path '%3' 'AdaptNote.bat') "
               "-Force -ErrorAction SilentlyContinue\n"
               "  Start-Process -FilePath (Join-Path '%3' 'bin\\adaptnote.exe')\n"
               "  Write-Log 'restarted'\n"
               "  Remove-Item -LiteralPath $work -Recurse -Force -ErrorAction SilentlyContinue\n"
               "  Remove-Item -LiteralPath '%2' -Force -ErrorAction SilentlyContinue\n"
               "} catch {\n"
               "  Write-Log ('ERROR: ' + $_.Exception.Message)\n"
               "  Start-Process -FilePath (Join-Path '%3' 'bin\\adaptnote.exe') "
               "-ErrorAction SilentlyContinue\n"
               "}\n")
        .arg(appPid)
        .arg(escapePowerShell(zipPath), escapePowerShell(installDir));
}

void Updater::check()
{
    if (reply_) return;
    QNetworkRequest request{
        QUrl(QStringLiteral("https://api.github.com/repos/hartamanda58/AdaptNote/releases/latest"))};
    request.setRawHeader("Accept", "application/vnd.github+json");
    request.setRawHeader("User-Agent", "AdaptNote/" + currentVersion().toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(20000);
    reply_ = net_->get(request);
    connect(reply_, &QNetworkReply::finished, this, &Updater::onCheckFinished);
}

void Updater::onCheckFinished()
{
    QNetworkReply* reply = reply_.data();
    if (!reply) return;
    reply_ = nullptr;
    const QByteArray body = reply->readAll();
    const QNetworkReply::NetworkError error = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString message = reply->errorString();
    reply->deleteLater();

    if (error != QNetworkReply::NoError || status >= 400) {
        if (status > 0) message += QStringLiteral("（HTTP %1）").arg(status);
        if (status == 403)
            message = QStringLiteral("GitHub 接口限流了，过一会儿再检查");
        emit checkFinished(ReleaseInfo(), false, message);
        return;
    }
    const ReleaseInfo info = parseRelease(body);
    if (info.tag.isEmpty()) {
        emit checkFinished(info, false, QStringLiteral("无法解析最新版本信息"));
        return;
    }
    if (!info.assetUrl.isValid() || info.assetName.isEmpty()) {
        emit checkFinished(info, false, QStringLiteral("最新发布里没有 Windows 便携包"));
        return;
    }
    emit checkFinished(info, isNewer(info, currentCommit()), QString());
}

void Updater::downloadAndInstall(const ReleaseInfo& info)
{
    if (downloading_) return;
#ifndef Q_OS_WIN
    emit failed(QStringLiteral("自动更新目前只支持 Windows 便携版"));
    return;
#else
    const QDir binDir(QCoreApplication::applicationDirPath());
    QDir rootDir = binDir;
    rootDir.cdUp();
    if (!QFileInfo::exists(rootDir.filePath(QStringLiteral("portable.flag")))) {
        emit failed(QStringLiteral("这不是便携包，找不到 portable.flag；请手动下载新版"));
        return;
    }
    if (info.sha256.isEmpty() || info.assetUrl.isEmpty()) {
        emit failed(QStringLiteral("发布缺少 sha256 或下载地址，已停止更新"));
        return;
    }

    info_ = info;
    installDir_ = rootDir.absolutePath();
    temp_ = new QTemporaryDir(QDir::tempPath() + QStringLiteral("/adaptnote-update-XXXXXX"));
    if (!temp_->isValid()) {
        emit failed(QStringLiteral("无法创建更新临时目录"));
        cleanupTemp();
        return;
    }
    zipPath_ = temp_->filePath(QStringLiteral("update.zip"));
    file_ = new QFile(zipPath_);
    if (!file_->open(QIODevice::WriteOnly)) {
        emit failed(QStringLiteral("无法写入更新包"));
        cleanupTemp();
        return;
    }
    hash_.reset();
    received_ = 0;
    downloading_ = true;

    QNetworkRequest request(info.assetUrl);
    request.setRawHeader("Accept", "application/octet-stream");
    request.setRawHeader("User-Agent", "AdaptNote/" + currentVersion().toUtf8());
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
    request.setTransferTimeout(300000);
    reply_ = net_->get(request);
    connect(reply_, &QNetworkReply::readyRead, this, &Updater::onDownloadReadyRead);
    connect(reply_, &QNetworkReply::downloadProgress, this,
            [this](qint64 received, qint64 total) {
                emit downloadProgress(received, total);
            });
    connect(reply_, &QNetworkReply::finished, this, &Updater::onDownloadFinished);
#endif
}

void Updater::onDownloadReadyRead()
{
    if (!reply_ || !file_) return;
    const QByteArray chunk = reply_->readAll();
    if (chunk.isEmpty()) return;
    file_->write(chunk);
    hash_.addData(chunk);
    received_ += chunk.size();
}

void Updater::onDownloadFinished()
{
    if (!reply_) return;
    QNetworkReply* reply = reply_.data();
    onDownloadReadyRead();   // drain the tail while reply_ is still set
    reply_ = nullptr;
    const QNetworkReply::NetworkError error = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString message = reply->errorString();
    reply->deleteLater();

    if (file_) {
        file_->flush();
        file_->close();
        file_->deleteLater();
        file_ = nullptr;
    }
    if (error != QNetworkReply::NoError || status >= 400) {
        if (status > 0) message += QStringLiteral("（HTTP %1）").arg(status);
        downloading_ = false;
        cleanupTemp();
        emit failed(QStringLiteral("下载更新失败：%1").arg(message));
        return;
    }
    if (info_.size > 0 && received_ != info_.size) {
        downloading_ = false;
        cleanupTemp();
        emit failed(QStringLiteral("更新包大小不对（收到 %1 / 应为 %2 字节）")
                        .arg(received_)
                        .arg(info_.size));
        return;
    }
    const QString digest = QString::fromLatin1(hash_.result().toHex());
    if (digest.compare(info_.sha256, Qt::CaseInsensitive) != 0) {
        downloading_ = false;
        cleanupTemp();
        emit failed(QStringLiteral("更新包 sha256 校验失败，已丢弃"));
        return;
    }
    if (!launchInstaller()) {
        downloading_ = false;
        cleanupTemp();
        emit failed(QStringLiteral("无法启动更新脚本（PowerShell 不可用？）"));
        return;
    }
    downloading_ = false;
    emit downloadFinished();
}

bool Updater::launchInstaller()
{
#ifdef Q_OS_WIN
    const QString script = buildWindowsUpdateScript(
        QCoreApplication::applicationPid(), installDir_, zipPath_);
    const QString scriptPath = temp_->filePath(QStringLiteral("update.ps1"));
    QFile file(scriptPath);
    if (!file.open(QIODevice::WriteOnly)) return false;
    file.write("\xEF\xBB\xBF");   // BOM: Windows PowerShell reads .ps1 as ANSI otherwise
    file.write(script.toUtf8());
    file.close();

    // Keep the zip/script alive after this process exits; the script removes them.
    temp_->setAutoRemove(false);
    const bool started = QProcess::startDetached(
        QStringLiteral("powershell.exe"),
        {QStringLiteral("-NoProfile"), QStringLiteral("-ExecutionPolicy"),
         QStringLiteral("Bypass"), QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
         QStringLiteral("-File"), scriptPath});
    if (!started) temp_->setAutoRemove(true);
    return started;
#else
    Q_UNUSED(installDir_);
    Q_UNUSED(zipPath_);
    return false;
#endif
}

void Updater::abort()
{
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    if (file_) {
        file_->close();
        delete file_;
        file_ = nullptr;
    }
    downloading_ = false;
    cleanupTemp();
}

void Updater::cleanupTemp()
{
    if (!temp_) return;
    if (temp_->autoRemove()) {
        delete temp_;   // QTemporaryDir removes its tree
        temp_ = nullptr;
    } else {
        QDir(temp_->path()).removeRecursively();
        delete temp_;
        temp_ = nullptr;
    }
}

}  // namespace update
}  // namespace an
