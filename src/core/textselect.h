#pragma once

#include "render/engine.h"

namespace an {

// Index of the character nearest to a page-space point (-1 if the page has no text).
int charIndexAt(const TextPage& page, const QPointF& pt);

// Build a selection between two page-space points (both ends snapped to characters).
Selection selectRange(const TextPage& page, int pageIndex, const QPointF& from, const QPointF& to);

// Text of the selection, lines joined with '\n'.
QString selectionText(const TextPage& page, const Selection& sel);

// Character range helpers (0-based, inclusive) — used by in-document search too.
QVector<Quad> quadsForRange(const TextPage& page, int from, int to);
QString textForRange(const TextPage& page, int from, int to);
// Page text as one string; index i maps to chars.at(i) for BMP characters.
QString pageText(const TextPage& page, int* charCount = nullptr);

}  // namespace an
