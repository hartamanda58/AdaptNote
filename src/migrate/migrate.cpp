#include "migrate/migrate.h"

#include "core/textselect.h"

#include <QSet>

namespace an {
namespace {

QString compact(const QString& s)
{
    QString out;
    out.reserve(s.size());
    for (const QChar c : s)
        if (!c.isSpace()) out.append(c);
    return out;
}

// Similarity of two compacted strings: characters in the shorter one that can be
// aligned greedily inside the longer one.
double similarity(const QString& a, const QString& b)
{
    if (a.isEmpty() || b.isEmpty()) return 0.0;
    const QString& needle = a.size() <= b.size() ? a : b;
    const QString& hay = a.size() <= b.size() ? b : a;
    if (needle == hay) return 1.0;
    int matched = 0;
    int cursor = 0;
    for (const QChar c : needle) {
        const int found = hay.indexOf(c, cursor);
        if (found < 0) continue;
        ++matched;
        cursor = found + 1;
    }
    return static_cast<double>(matched) / needle.size();
}

QString needleOf(const Markup& m)
{
    const QString flat = compact(m.text);
    if (flat.size() <= 24) return flat;
    return flat.left(24);
}

// Best match of a note inside one page, or -1 when the text is not there.
int matchOnPage(const TextPage& page, const QString& needle, const QString& full,
                int* startChar, double* score)
{
    if (page.chars.isEmpty() || needle.isEmpty()) return -1;
    const QString pageText = compact(textForRange(page, 0, page.chars.size() - 1));

    // Page text is compacted, so the needle search works on the compacted form; the
    // resulting index is a compacted index -> map it back through the char list.
    const int hit = pageText.indexOf(needle);
    if (hit < 0) return -1;

    // walk the real characters to the hit-th non-space character
    int nonSpace = -1;
    int firstChar = -1;
    for (int i = 0; i < page.chars.size(); ++i) {
        const QChar c(static_cast<ushort>(page.chars.at(i).code));
        if (c.isSpace()) continue;
        ++nonSpace;
        if (nonSpace == hit) {
            firstChar = i;
            break;
        }
    }
    if (firstChar < 0) return -1;

    // span = as many characters as the original excerpt had (whitespace ignored)
    const int spanNonSpace = qMax(1, full.size());
    int endChar = firstChar;
    int counted = 0;
    for (int i = firstChar; i < page.chars.size(); ++i) {
        const QChar c(static_cast<ushort>(page.chars.at(i).code));
        endChar = i;
        if (!c.isSpace()) ++counted;
        if (counted >= spanNonSpace) break;
    }

    const QString found = compact(textForRange(page, firstChar, endChar));
    if (startChar) *startChar = firstChar;
    if (score) *score = similarity(full, found);
    return endChar;
}

}  // namespace

QString MigrationReport::summary() const
{
    return QStringLiteral("迁移：移动 %1，原位刷新 %2，未找到 %3，无需处理 %4")
        .arg(moved.size())
        .arg(inPlace.size())
        .arg(lost.size())
        .arg(unchanged.size());
}

MigrationReport migrateByText(IDocumentEngine& engine, const QVector<Markup>& markups)
{
    MigrationReport report;
    if (!engine.isOpen()) return report;

    const int pages = engine.pageCount();

    for (const Markup& m : markups) {
        if (m.deleted) continue;
        if (m.type == MarkupType::Image || m.type == MarkupType::Ink) continue;  // no text to match
        const QString full = compact(m.text);
        const QString needle = needleOf(m);
        if (needle.isEmpty()) {
            report.unchanged.append({m.id, m.page, m.page, m.quads, 1.0});
            continue;
        }

        Relocation best;
        best.markupId = m.id;
        best.oldPage = m.page;

        // 1) the old page first: most documents only shift some pages
        if (m.page >= 0 && m.page < pages) {
            const TextPage* tp = engine.textPage(m.page);
            if (tp) {
                int start = -1;
                double score = 0.0;
                const int end = matchOnPage(*tp, needle, full, &start, &score);
                if (end >= 0 && score >= 0.8) {
                    best.newPage = m.page;
                    best.score = score;
                    best.quads = quadsForRange(*tp, start, end);
                }
            }
        }

        // 2) otherwise scan the whole document and take the best scoring page
        if (best.newPage < 0) {
            for (int p = 0; p < pages; ++p) {
                if (p == m.page) continue;
                const TextPage* tp = engine.textPage(p);
                if (!tp) continue;
                int start = -1;
                double score = 0.0;
                const int end = matchOnPage(*tp, needle, full, &start, &score);
                if (end >= 0 && score > best.score) {
                    best.newPage = p;
                    best.score = score;
                    best.quads = quadsForRange(*tp, start, end);
                }
            }
        }

        if (best.newPage < 0 || best.quads.isEmpty() || best.score < 0.6) {
            report.lost.append(m.id);
            continue;
        }
        if (best.newPage == m.page && best.score >= 0.95) {
            // same page, essentially the same text: only refresh the geometry
            report.inPlace.append(best);
        } else if (best.newPage == m.page) {
            report.inPlace.append(best);
        } else {
            report.moved.append(best);
        }
    }

    return report;
}

}  // namespace an
