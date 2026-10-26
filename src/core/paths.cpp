#include "core/paths.h"

#include <QDir>
#include <QFileInfo>
#include <QStandardPaths>
#include <QString>

namespace an {

QString resolveDataDir(const QString& exeDir, const QString& envOverride, const QString& fallback)
{
    // 1) explicit override always wins (tests, scripts, unusual installs)
    if (!envOverride.trimmed().isEmpty()) {
        const QString p = envOverride.trimmed();
        const QFileInfo fi(p);
        if (fi.isAbsolute()) return QDir::cleanPath(fi.absoluteFilePath());
        // relative paths are relative to the executable, which keeps `data\` inside the
        // portable folder even when the launcher is started from another directory
        return QDir::cleanPath(QDir(exeDir).absoluteFilePath(p));
    }

    // 2) portable layout: a `data` directory next to the executable
    if (!exeDir.isEmpty()) {
        const QString portable = QDir(exeDir).absoluteFilePath(QStringLiteral("data"));
        if (QFileInfo(portable).isDir()) return QDir::cleanPath(portable);
    }

    // 3) normal per-user location
    return fallback;
}

QString defaultDataDir(const QString& exeDir)
{
    const QString fallback =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    return resolveDataDir(exeDir, qEnvironmentVariable("ADAPT_DATA_DIR"), fallback);
}

}  // namespace an
