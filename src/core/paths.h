#pragma once

#include <QString>

namespace an {

// Where the library database, mirror and image excerpts live.
//
// Flatpak/portable rules on Linux use XDG_DATA_HOME, but Qt **ignores XDG_* on Windows**, so
// the portable zip must not rely on it: without this the Windows build happily writes into
// %APPDATA% and nothing stays inside the extracted folder.
//
//  1. ADAPT_DATA_DIR (absolute, or relative to the executable)
//  2. <exeDir>/data when that directory exists (portable layout)
//  3. the usual per-user location
//
// Kept dependency-light and parameterised so it can be unit tested without touching the
// real environment.
QString resolveDataDir(const QString& exeDir, const QString& envOverride, const QString& fallback);

// Same, using the real environment and QCoreApplication::applicationDirPath().
QString defaultDataDir(const QString& exeDir = QString());

}  // namespace an
