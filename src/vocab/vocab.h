#pragma once

#include "core/model.h"

#include <QString>
#include <QVector>

namespace an {
namespace vocab {

// Five-box schedule inherited from MN (server/index.js): a correct answer moves the
// card one box forward, a wrong answer sends it back to box 0. Field `box` is the
// current box; the gap table is the only place the timing lives.
constexpr int kReviewGapsDays[6] = {0, 1, 2, 4, 7, 15};

inline int gapDaysForBox(int box) { return kReviewGapsDays[qBound(0, box, 5)]; }

// One dictionary entry. `source` is "ecdict", "ai" or "manual".
struct Definition {
    QString word;
    QString phonetic;
    QString meaning;
    QString example;
    QString source;
    bool ok = false;
    QString error;
};

// A whole sentence is usually selected before looking a word up; this picks the
// English token (or a short CJK term) worth storing.
QString normalizeQuery(const QString& selection);

// Prompt sent to the OpenAI-compatible model when no local dictionary is present.
QString promptForLookup(const QString& word);

// Parses the three-line answer (音标 / 释义 / 例句). Falls back to using the whole
// answer as the meaning, because models sometimes ignore the requested format.
bool parseAiDefinition(const QString& answer, Definition* out);

// ECDICT / StarDict SQLite lookup. Missing file or wrong schema returns ok = false
// with an empty error, so the caller can fall back to AI without alarming the user.
Definition lookupLocal(const QString& dbPath, const QString& word);

// TSV (Excel/Anki-readable) or Anki "basic" import text. Written with a UTF-8 BOM
// because Anki and Excel both mis-detect the encoding otherwise.
bool exportWords(const QVector<WordEntry>& words, const QString& path, bool anki,
                 QString* error = nullptr);

QString dueLabel(const WordEntry& w, qint64 nowMs = 0);

}  // namespace vocab
}  // namespace an
