#pragma once

#include "core/model.h"

#include <QImage>
#include <QSizeF>
#include <QString>
#include <QVector>

namespace an {

// One glyph/character as reported by the document engine, positioned in page space.
struct CharBox {
    uint code = 0;      // unicode codepoint
    Quad quad;          // page space
    QPointF origin;     // page space baseline origin
    float size = 0;     // font size in points
    int line = 0;       // engine line index (for grouping)
};

struct TextPage {
    QVector<CharBox> chars;
    QSizeF pageSize;

    bool isEmpty() const { return chars.isEmpty(); }
    int lineOf(int charIndex) const { return chars.at(charIndex).line; }
};

enum class PageLayoutMode { Single, Continuous, Double };
enum class ZoomMode { FitWidth, FitPage, Actual, Custom };

// Engine abstraction. MuPDF is the first backend; a PDFium backend can be added
// later without touching UI or storage code.
class IDocumentEngine {
public:
    virtual ~IDocumentEngine() = default;

    virtual bool open(const QString& path, QString* error) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual QString path() const = 0;

    virtual int pageCount() const = 0;
    virtual QSizeF pageSize(int page) const = 0;
    virtual bool isReflowable() const = 0;

    // Reflowable documents (EPUB) need a layout box before page sizes/rendering are meaningful.
    virtual void setLayout(float widthPt, float heightPt, float emPt) = 0;

    virtual QImage renderPage(int page, float scale, int rotation) = 0;
    virtual const TextPage* textPage(int page) = 0;   // cached; owned by engine
    virtual QVector<OutlineItem> outline() = 0;

    // ---- stable anchors for reflowable documents ----
    // A note is anchored by (chapter, character offset inside that chapter), which
    // survives re-layout; the page number is only a hint for the UI.
    virtual bool supportsAnchors() const { return false; }
    virtual int chapterOfPage(int page) { return page >= 0 ? 0 : -1; }
    // Character offset of (page, charIndex) measured from the start of its chapter.
    virtual int chapterOffsetOf(int page, int charIndex)
    {
        Q_UNUSED(page);
        Q_UNUSED(charIndex);
        return -1;
    }
    // Inverse mapping: page + intra-page char index for a chapter offset.
    virtual int pageForChapterOffset(int chapter, int offset, int* intraChar = nullptr)
    {
        Q_UNUSED(chapter);
        Q_UNUSED(offset);
        if (intraChar) *intraChar = -1;
        return -1;
    }

    virtual bool supportsAnnotations() const = 0;
    virtual bool writeAnnotations(const QVector<Markup>& markups, QString* error) = 0;
    virtual int annotationCount(int page) = 0;
    // Reads the annotations embedded in the document (e.g. written earlier by this
    // app or by another reader) as note objects. Returns empty for other formats.
    virtual QVector<Markup> readAnnotations(int page)
    {
        Q_UNUSED(page);
        return {};
    }
};

}  // namespace an
