#include "core/textselect.h"

#include <QPointF>
#include <QRectF>
#include <QtMath>

namespace an {
namespace {

double distSq(const QPointF& a, const QPointF& b)
{
    const double dx = a.x() - b.x();
    const double dy = a.y() - b.y();
    return dx * dx + dy * dy;
}

QRectF expanded(const QRectF& r, double d)
{
    return r.adjusted(-d, -d, d, d);
}

}  // namespace

int charIndexAt(const TextPage& page, const QPointF& pt)
{
    if (page.chars.isEmpty()) return -1;

    int best = -1;
    double bestDist = std::numeric_limits<double>::max();

    // Pass 1: inside a character box (prefer the character whose centre is closest).
    for (int i = 0; i < page.chars.size(); ++i) {
        const CharBox& c = page.chars.at(i);
        const QRectF box = expanded(c.quad.boundingRect(), c.size * 0.15 + 0.5);
        if (!box.contains(pt)) continue;
        const double d = distSq(pt, c.quad.boundingRect().center());
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    if (best >= 0) return best;

    // Pass 2: nearest baseline origin.
    for (int i = 0; i < page.chars.size(); ++i) {
        const CharBox& c = page.chars.at(i);
        const double d = distSq(pt, c.origin);
        if (d < bestDist) {
            bestDist = d;
            best = i;
        }
    }
    return best;
}

Selection selectRange(const TextPage& page, int pageIndex, const QPointF& from, const QPointF& to)
{
    Selection sel;
    if (page.chars.isEmpty()) return sel;

    int a = charIndexAt(page, from);
    int b = charIndexAt(page, to);
    if (a < 0 || b < 0) return sel;
    if (a > b) std::swap(a, b);

    sel.page = pageIndex;
    sel.startChar = a;
    sel.endChar = b;
    sel.quads = quadsForRange(page, a, b);
    sel.text = textForRange(page, a, b);
    return sel;
}

QVector<Quad> quadsForRange(const TextPage& page, int from, int to)
{
    QVector<Quad> quads;
    if (page.chars.isEmpty()) return quads;
    from = qBound(0, from, page.chars.size() - 1);
    to = qBound(0, to, page.chars.size() - 1);
    if (from > to) std::swap(from, to);

    int line = -1;
    QRectF acc;
    for (int i = from; i <= to; ++i) {
        const CharBox& c = page.chars.at(i);
        if (c.line != line) {
            if (line >= 0 && !acc.isNull()) quads.append(Quad::fromRect(acc));
            line = c.line;
            acc = c.quad.boundingRect();
        } else {
            acc = acc.united(c.quad.boundingRect());
        }
    }
    if (line >= 0 && !acc.isNull()) quads.append(Quad::fromRect(acc));
    return quads;
}

QString textForRange(const TextPage& page, int from, int to)
{
    if (page.chars.isEmpty()) return {};
    from = qBound(0, from, page.chars.size() - 1);
    to = qBound(0, to, page.chars.size() - 1);
    if (from > to) std::swap(from, to);

    QString text;
    int prevLine = -1;
    for (int i = from; i <= to; ++i) {
        const CharBox& c = page.chars.at(i);
        if (prevLine >= 0 && c.line != prevLine) text.append(QLatin1Char('\n'));
        prevLine = c.line;
        if (c.code <= 0xFFFF) {
            text.append(QChar(static_cast<ushort>(c.code)));
        } else {
            const uint v = c.code - 0x10000;
            text.append(QChar(static_cast<ushort>(0xD800 + (v >> 10))));
            text.append(QChar(static_cast<ushort>(0xDC00 + (v & 0x3FF))));
        }
    }
    // Some PDF fonts map CJK glyphs to Kangxi radicals / compatibility ideographs
    // (e.g. U+2F06 "⼆" instead of "二"). NFKC folds them back to normal text.
    return text.normalized(QString::NormalizationForm_KC);
}

QString pageText(const TextPage& page, int* charCount)
{
    QString s;
    s.reserve(page.chars.size());
    for (const CharBox& c : page.chars) {
        // Keep a 1:1 index mapping with chars: non-BMP codepoints get a placeholder.
        if (c.code > 0xFFFF)
            s.append(QChar(0xFFFD));
        else
            s.append(QChar(static_cast<ushort>(c.code)));
    }
    if (charCount) *charCount = page.chars.size();
    return s;
}

QString selectionText(const TextPage& page, const Selection& sel)
{
    Q_UNUSED(page);
    return sel.text;
}

}  // namespace an
