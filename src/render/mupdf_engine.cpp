#include "render/mupdf_engine.h"

#include <QFile>
#include <QFileInfo>
#include "ink/ink.h"

#include <QImage>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

extern "C" {
#include <mupdf/fitz.h>
#include <mupdf/pdf.h>
}

namespace an {
namespace {

fz_quad toFzQuad(const Quad& q)
{
    fz_quad fq;
    fq.ul = fz_make_point(q.ul.x(), q.ul.y());
    fq.ur = fz_make_point(q.ur.x(), q.ur.y());
    fq.ll = fz_make_point(q.ll.x(), q.ll.y());
    fq.lr = fz_make_point(q.lr.x(), q.lr.y());
    return fq;
}

Quad fromFzQuad(const fz_quad& q)
{
    Quad r;
    r.ul = QPointF(q.ul.x, q.ul.y);
    r.ur = QPointF(q.ur.x, q.ur.y);
    r.ll = QPointF(q.ll.x, q.ll.y);
    r.lr = QPointF(q.lr.x, q.lr.y);
    return r;
}

// PDF annotation subtype -> our note type; returns false for subtypes we do not model.
bool markupTypeFromAnnot(enum pdf_annot_type type, MarkupType* out)
{
    switch (type) {
    case PDF_ANNOT_HIGHLIGHT: *out = MarkupType::Highlight; return true;
    case PDF_ANNOT_UNDERLINE: *out = MarkupType::Underline; return true;
    case PDF_ANNOT_SQUIGGLY: *out = MarkupType::Squiggly; return true;
    case PDF_ANNOT_TEXT:
    case PDF_ANNOT_FREE_TEXT: *out = MarkupType::Note; return true;
    case PDF_ANNOT_INK: *out = MarkupType::Ink; return true;
    case PDF_ANNOT_SQUARE:
    case PDF_ANNOT_STRIKE_OUT: *out = MarkupType::Highlight; return true;
    default: return false;
    }
}

enum pdf_annot_type annotTypeFor(MarkupType t)
{
    switch (t) {
    case MarkupType::Highlight: return PDF_ANNOT_HIGHLIGHT;
    case MarkupType::Underline: return PDF_ANNOT_UNDERLINE;
    case MarkupType::Squiggly: return PDF_ANNOT_SQUIGGLY;
    case MarkupType::Note: return PDF_ANNOT_TEXT;
    case MarkupType::Ink: return PDF_ANNOT_INK;
    case MarkupType::Image: return PDF_ANNOT_SQUARE;   // not used, see writeAnnotations
    }
    return PDF_ANNOT_HIGHLIGHT;
}

void collectOutline(fz_context* ctx, fz_document* doc, fz_outline* node, QVector<OutlineItem>& out)
{
    for (; node; node = node->next) {
        OutlineItem item;
        item.title = QString::fromUtf8(node->title ? node->title : "");
        item.page = (node->page.page >= 0) ? fz_page_number_from_location(ctx, doc, node->page) : -1;
        if (node->down) collectOutline(ctx, doc, node->down, item.children);
        out.append(item);
    }
}

}  // namespace

MuPdfEngine::MuPdfEngine() = default;

MuPdfEngine::~MuPdfEngine()
{
    close();
}

void MuPdfEngine::clearTextCache()
{
    for (TextPage* p : textCache_) delete p;
    textCache_.clear();
}

void MuPdfEngine::close()
{
    clearTextCache();
    if (ctx_ && doc_) {
        fz_drop_document(ctx_, doc_);
        doc_ = nullptr;
    }
    if (ctx_) {
        fz_drop_context(ctx_);
        ctx_ = nullptr;
    }
    path_.clear();
    pageCount_ = 0;
    reflowable_ = false;
}

bool MuPdfEngine::open(const QString& path, QString* error)
{
    close();

    // MuPDF expects a UTF-8 path; QFile::encodeName would use the ANSI codepage on
    // Windows and break books with non-ASCII names.
    const QByteArray native = path.toUtf8();
    const bool exists = QFileInfo::exists(path);
    if (!exists) {
        if (error) *error = QStringLiteral("file not found: %1").arg(path);
        return false;
    }

    ctx_ = fz_new_context(nullptr, nullptr, FZ_STORE_DEFAULT);
    if (!ctx_) {
        if (error) *error = QStringLiteral("cannot create MuPDF context");
        return false;
    }
    fz_register_document_handlers(ctx_);

    bool ok = true;
    QString err;
    fz_try(ctx_)
    {
        doc_ = fz_open_document(ctx_, native.constData());
        if (!doc_) ok = false;
    }
    fz_catch(ctx_)
    {
        ok = false;
        err = QString::fromUtf8(fz_caught_message(ctx_));
    }

    if (!ok || !doc_) {
        if (error) *error = err.isEmpty() ? QStringLiteral("cannot open document") : err;
        close();
        return false;
    }

    reflowable_ = fz_is_document_reflowable(ctx_, doc_) != 0;
    if (reflowable_) {
        fz_layout_document(ctx_, doc_, layoutW_, layoutH_, layoutEm_);
    }

    int pages = 0;
    fz_try(ctx_) { pages = fz_count_pages(ctx_, doc_); }
    fz_catch(ctx_) { pages = 0; }

    if (pages <= 0) {
        if (error) *error = QStringLiteral("document has no pages");
        close();
        return false;
    }

    path_ = path;
    pageCount_ = pages;
    return true;
}

QSizeF MuPdfEngine::pageSize(int page) const
{
    if (!ctx_ || !doc_ || page < 0 || page >= pageCount_) return {};
    fz_page* pg = nullptr;
    fz_rect b = fz_empty_rect;
    fz_try(ctx_)
    {
        pg = fz_load_page(ctx_, doc_, page);
        b = fz_bound_page(ctx_, pg);
    }
    fz_catch(ctx_) { }
    if (pg) fz_drop_page(ctx_, pg);
    return QSizeF(b.x1 - b.x0, b.y1 - b.y0);
}

void MuPdfEngine::setLayout(float widthPt, float heightPt, float emPt)
{
    if (!ctx_ || !doc_ || !reflowable_) return;
    layoutW_ = widthPt;
    layoutH_ = heightPt;
    layoutEm_ = emPt;
    clearTextCache();
    fz_try(ctx_)
    {
        fz_layout_document(ctx_, doc_, layoutW_, layoutH_, layoutEm_);
        pageCount_ = fz_count_pages(ctx_, doc_);
    }
    fz_catch(ctx_) { }
}

QImage MuPdfEngine::renderPage(int page, float scale, int rotation)
{
    QImage result;
    if (!ctx_ || !doc_ || page < 0 || page >= pageCount_) return result;
    if (scale <= 0.f) return result;

    fz_page* pg = nullptr;
    fz_pixmap* pix = nullptr;

    fz_matrix ctm = fz_scale(scale, scale);
    if (rotation) ctm = fz_pre_rotate(ctm, static_cast<float>(rotation));

    fz_try(ctx_)
    {
        pg = fz_load_page(ctx_, doc_, page);
        pix = fz_new_pixmap_from_page(ctx_, pg, ctm, fz_device_rgb(ctx_), 0);
    }
    fz_catch(ctx_)
    {
        pix = nullptr;
    }

    if (pix && pix->w > 0 && pix->h > 0 && pix->samples) {
        if (pix->n == 3) {
            result = QImage(pix->samples, pix->w, pix->h, pix->stride, QImage::Format_RGB888).copy();
        } else if (pix->n == 4) {
            result = QImage(pix->samples, pix->w, pix->h, pix->stride, QImage::Format_RGBA8888).copy();
        }
    }

    if (pix) fz_drop_pixmap(ctx_, pix);
    if (pg) fz_drop_page(ctx_, pg);
    return result;
}

const TextPage* MuPdfEngine::textPage(int page)
{
    if (!ctx_ || !doc_ || page < 0 || page >= pageCount_) return nullptr;
    auto it = textCache_.constFind(page);
    if (it != textCache_.constEnd()) return it.value();

    auto* tp = new TextPage();
    tp->pageSize = pageSize(page);

    fz_page* pg = nullptr;
    fz_stext_page* stext = nullptr;

    fz_try(ctx_)
    {
        pg = fz_load_page(ctx_, doc_, page);
        stext = fz_new_stext_page_from_page(ctx_, pg, nullptr);
    }
    fz_catch(ctx_)
    {
        stext = nullptr;
    }

    if (stext) {
        int lineIndex = 0;
        for (fz_stext_block* block = stext->first_block; block; block = block->next) {
            if (block->type != FZ_STEXT_BLOCK_TEXT) continue;
            for (fz_stext_line* line = block->u.t.first_line; line; line = line->next) {
                for (fz_stext_char* ch = line->first_char; ch; ch = ch->next) {
                    CharBox cb;
                    cb.code = static_cast<uint>(ch->c);
                    cb.quad = fromFzQuad(ch->quad);
                    cb.origin = QPointF(ch->origin.x, ch->origin.y);
                    cb.size = ch->size;
                    cb.line = lineIndex;
                    tp->chars.append(cb);
                }
                ++lineIndex;
            }
        }
        fz_drop_stext_page(ctx_, stext);
    }
    if (pg) fz_drop_page(ctx_, pg);

    textCache_.insert(page, tp);
    return tp;
}

namespace {

// Fallback table of contents for documents without a native outline (most EPUB 3 files
// and a lot of PDFs): treat "visually larger than body text" lines as headings.
// Bounded to the first pages so that huge books stay cheap.
QVector<OutlineItem> headingOutline(MuPdfEngine* self, int pageCount, int maxPages)
{
    QVector<OutlineItem> items;
    const int pages = qMin(pageCount, maxPages);
    if (pages <= 0) return items;

    QVector<double> sizes;
    for (int i = 0; i < pages; ++i) {
        const TextPage* tp = self->textPage(i);
        if (!tp) continue;
        for (const CharBox& c : tp->chars)
            if (c.size > 0) sizes.append(c.size);
    }
    if (sizes.isEmpty()) return items;
    std::sort(sizes.begin(), sizes.end());
    const double median = sizes.at(sizes.size() / 2);
    const double threshold = median * 1.35;

    for (int i = 0; i < pages; ++i) {
        const TextPage* tp = self->textPage(i);
        if (!tp || tp->isEmpty()) continue;

        int line = -1;
        double bestSize = 0;
        QString bestText;
        QString text;
        double lineMax = 0;

        auto flush = [&]() {
            if (line >= 0 && lineMax >= threshold && bestSize < lineMax && !text.trimmed().isEmpty()) {
                bestSize = lineMax;
                bestText = text.trimmed();
            }
        };

        for (const CharBox& c : tp->chars) {
            if (c.line != line) {
                flush();
                line = c.line;
                lineMax = 0;
                text.clear();
            }
            lineMax = qMax(lineMax, static_cast<double>(c.size));
            text.append(c.code <= 0xFFFF ? QChar(static_cast<ushort>(c.code))
                                         : QChar(static_cast<ushort>(c.code - 0x10000)));
        }
        flush();

        if (!bestText.isEmpty()) {
            OutlineItem item;
            item.title = bestText.left(80).normalized(QString::NormalizationForm_KC);
            item.page = i;
            items.append(item);
        }
    }
    return items;
}

}  // namespace

QVector<OutlineItem> MuPdfEngine::outline()
{
    QVector<OutlineItem> items;
    if (!ctx_ || !doc_) return items;

    fz_outline* ol = nullptr;
    fz_try(ctx_) { ol = fz_load_outline(ctx_, doc_); }
    fz_catch(ctx_) { ol = nullptr; }

    if (ol) {
        collectOutline(ctx_, doc_, ol, items);
        fz_drop_outline(ctx_, ol);
    }

    if (items.isEmpty()) items = headingOutline(this, pageCount_, reflowable_ ? 400 : 80);
    return items;
}

int MuPdfEngine::chapterOfPage(int page)
{
    if (!ctx_ || !doc_ || page < 0 || page >= pageCount_) return -1;
    int chapter = -1;
    fz_location loc = fz_location_from_page_number(ctx_, doc_, page);
    chapter = loc.chapter;
    return chapter;
}

int MuPdfEngine::chapterOffsetOf(int page, int charIndex)
{
    if (!supportsAnchors() || page < 0 || page >= pageCount_) return -1;
    const int chapter = chapterOfPage(page);
    if (chapter < 0) return -1;

    int total = 0;
    for (int p = 0; p < page; ++p) {
        if (chapterOfPage(p) != chapter) continue;
        const TextPage* tp = textPage(p);
        if (tp) total += tp->chars.size();
    }
    return total + qMax(0, charIndex);
}

int MuPdfEngine::pageForChapterOffset(int chapter, int offset, int* intraChar)
{
    if (intraChar) *intraChar = -1;
    if (!supportsAnchors() || offset < 0) return -1;

    int consumed = 0;
    for (int p = 0; p < pageCount_; ++p) {
        if (chapterOfPage(p) != chapter) continue;
        const TextPage* tp = textPage(p);
        const int count = tp ? tp->chars.size() : 0;
        if (offset < consumed + count) {
            if (intraChar) *intraChar = offset - consumed;
            return p;
        }
        consumed += count;
    }
    return -1;
}

bool MuPdfEngine::supportsAnnotations() const
{
    if (!ctx_ || !doc_) return false;
    fz_try(ctx_) { return pdf_specifics(ctx_, doc_) != nullptr; }
    fz_catch(ctx_) { return false; }
    return false;
}

int MuPdfEngine::annotationCount(int page)
{
    if (!supportsAnnotations() || page < 0 || page >= pageCount_) return -1;
    int count = -1;
    fz_page* pg = nullptr;
    fz_try(ctx_)
    {
        pg = fz_load_page(ctx_, doc_, page);
        pdf_page* pp = pdf_page_from_fz_page(ctx_, pg);
        count = 0;
        for (pdf_annot* a = pdf_first_annot(ctx_, pp); a; a = pdf_next_annot(ctx_, a)) ++count;
    }
    fz_catch(ctx_) { count = -1; }
    if (pg) fz_drop_page(ctx_, pg);
    return count;
}

// Reads one annotation into a note. Called inside its own fz_try: MuPDF throws for
// annotations that lack the keys we ask for (e.g. QuadPoints on a sticky note).
bool readOneAnnot(fz_context* ctx, pdf_annot* annot, int page, fz_matrix annotCtm, Markup* out)
{
    MarkupType type = MarkupType::Highlight;
    if (!markupTypeFromAnnot(pdf_annot_type(ctx, annot), &type)) return false;

    Markup m;
    m.page = page;
    m.type = type;
    m.id = newId();
    m.created = m.updated = QDateTime::currentMSecsSinceEpoch();

    float color[4] = {1, 0.83f, 0, 0};
    int n = 0;
    fz_try(ctx) { pdf_annot_color(ctx, annot, &n, color); }
    fz_catch(ctx) { n = 0; }
    if (n >= 3)
        m.color = QColor::fromRgbF(color[0], color[1], color[2]);
    else if (n == 1)
        m.color = QColor::fromRgbF(color[0], color[0], color[0]);
    else
        m.color = QColor(255, 213, 0);

    const char* contents = pdf_annot_contents(ctx, annot);
    if (contents && *contents) m.comment = QString::fromUtf8(contents);
    const char* author = pdf_annot_author(ctx, annot);
    if (author && *author) m.tags << QString::fromUtf8(author);

    if (type == MarkupType::Ink) {
        const int strokes = pdf_annot_ink_list_count(ctx, annot);
        for (int i = 0; i < strokes; ++i) {
            const int vertices = pdf_annot_ink_list_stroke_count(ctx, annot, i);
            for (int k = 0; k < vertices; ++k) {
                fz_point p = pdf_annot_ink_list_stroke_vertex(ctx, annot, i, k);
                if (!fz_is_identity(annotCtm)) p = fz_transform_point(p, annotCtm);
                InkPoint ip;
                ip.pos = QPointF(p.x, p.y);
                ip.pressure = 1.0;
                m.ink.append(ip);
            }
        }
        if (m.ink.size() < 2) return false;
        m.quads.append(Quad::fromRect(ink::bounds(m.ink, 2.0)));
    } else {
        int quads = 0;
        fz_try(ctx) { quads = pdf_annot_quad_point_count(ctx, annot); }
        fz_catch(ctx) { quads = 0; }
        for (int i = 0; i < quads; ++i) {
            fz_quad q = pdf_annot_quad_point(ctx, annot, i);
            if (!fz_is_identity(annotCtm)) q = fz_transform_quad(q, annotCtm);
            m.quads.append(fromFzQuad(q));
        }
        if (m.quads.isEmpty()) {
            fz_rect r = pdf_bound_annot(ctx, annot);
            if (!fz_is_identity(annotCtm)) r = fz_transform_rect(r, annotCtm);
            QRectF box(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
            if (box.width() < 1 || box.height() < 1) box = QRectF(r.x0, r.y0, 12.0, 12.0);
            m.quads.append(Quad::fromRect(box));
        }
    }

    if (out) *out = m;
    return true;
}

QVector<Markup> MuPdfEngine::readAnnotations(int page)
{
    QVector<Markup> out;
    if (!supportsAnnotations() || page < 0 || page >= pageCount_) return out;

    fz_page* pg = nullptr;
    bool failed = false;
    fz_try(ctx_) { pg = fz_load_page(ctx_, doc_, page); }
    fz_catch(ctx_) { failed = true; }
    if (failed || !pg) return out;

    pdf_page* pp = nullptr;
    fz_rect mediabox = fz_empty_rect;
    fz_matrix pageCtm = fz_identity;
    fz_matrix annotCtm = fz_identity;
    fz_try(ctx_)
    {
        pp = pdf_page_from_fz_page(ctx_, pg);
        if (pp) {
            pdf_page_transform(ctx_, pp, &mediabox, &pageCtm);
            annotCtm = fz_invert_matrix(pageCtm);
        }
    }
    fz_catch(ctx_) { pp = nullptr; }

    if (pp) {
        fz_try(ctx_)
        {
            for (pdf_annot* annot = pdf_first_annot(ctx_, pp); annot;
                 annot = pdf_next_annot(ctx_, annot)) {
                // One malformed annotation must not abort the whole page: each annotation
                // is read inside its own fz_try below (MuPDF throws on missing keys).
                Markup parsed;
                bool ok = true;
                fz_try(ctx_) { ok = readOneAnnot(ctx_, annot, page, annotCtm, &parsed); }
                fz_catch(ctx_) { ok = false; }
                if (ok) out.append(parsed);
                continue;
            }
        }
        fz_catch(ctx_) { }
    }

    fz_drop_page(ctx_, pg);
    return out;
}

bool MuPdfEngine::writeAnnotations(const QVector<Markup>& markups, QString* error)
{
    if (!supportsAnnotations()) {
        if (error) *error = QStringLiteral("document does not support PDF annotations");
        return false;
    }

    pdf_document* pdoc = nullptr;
    fz_try(ctx_) { pdoc = pdf_specifics(ctx_, doc_); }
    fz_catch(ctx_)
    {
        pdoc = nullptr;
    }
    if (!pdoc) {
        if (error) *error = QStringLiteral("not a PDF document");
        return false;
    }

    // Group by page so that every page is only loaded once.
    QHash<int, QVector<Markup>> byPage;
    for (const Markup& m : markups) {
        if (!m.isValid()) continue;
        byPage[m.page].append(m);
    }

    QVector<int> pages = byPage.keys();
    std::sort(pages.begin(), pages.end());

    int written = 0;
    QString err;

    for (int pageIndex : pages) {
        fz_page* pg = nullptr;
        bool pageFailed = false;
        fz_try(ctx_) { pg = fz_load_page(ctx_, doc_, pageIndex); }
        fz_catch(ctx_)
        {
            err = QString::fromUtf8(fz_caught_message(ctx_));
            pageFailed = true;
        }
        if (pageFailed || !pg) continue;

        pdf_page* pp = nullptr;
        fz_rect mediabox = fz_empty_rect;
        fz_matrix pageCtm = fz_identity;
        fz_try(ctx_) { pp = pdf_page_from_fz_page(ctx_, pg); }
        fz_catch(ctx_) { pp = nullptr; }
        if (!pp) {
            fz_drop_page(ctx_, pg);
            continue;
        }

        fz_try(ctx_) { pdf_page_transform(ctx_, pp, &mediabox, &pageCtm); }
        fz_catch(ctx_) { pageCtm = fz_identity; }
        // pdf_page_transform maps PDF user space -> fz page space (it includes the
        // y-flip). Annotation geometry must live in PDF user space, so invert it.
        fz_matrix annotCtm = fz_identity;
        fz_try(ctx_) { annotCtm = fz_invert_matrix(pageCtm); }
        fz_catch(ctx_) { annotCtm = fz_identity; }

        for (const Markup& m : byPage.value(pageIndex)) {
            // Handwriting: MuPDF 1.21 writes a valid /Subtype /Ink annotation with
            // an /InkList, but it does not generate the appearance stream, so the
            // stroke is invisible in any MuPDF-rendered page. Until a backend that
            // generates ink appearances is used, ink stays app-side (stored, synced,
            // exported) instead of being written into the PDF.
            if (m.type == MarkupType::Ink) continue;

            std::vector<fz_quad> quads;
            quads.reserve(static_cast<size_t>(m.quads.size()));
            for (const Quad& q : m.quads) {
                fz_quad fq = toFzQuad(q);
                if (!fz_is_identity(annotCtm)) fq = fz_transform_quad(fq, annotCtm);
                quads.push_back(fq);
            }
            if (quads.empty()) continue;

            pdf_annot* annot = nullptr;
            bool failed = false;
            fz_try(ctx_)
            {
                annot = pdf_create_annot(ctx_, pp, annotTypeFor(m.type));
                if (m.type == MarkupType::Note) {
                    // Sticky notes are positioned by their rect and have no QuadPoints;
                    // setting a quad would make MuPDF throw and lose the contents.
                    fz_rect r = fz_rect_from_quad(quads.front());
                    pdf_set_annot_rect(ctx_, annot, r);
                } else {
                    pdf_set_annot_quad_points(ctx_, annot, static_cast<int>(quads.size()),
                                              quads.data());
                }
                const float col[3] = {static_cast<float>(m.color.redF()),
                                      static_cast<float>(m.color.greenF()),
                                      static_cast<float>(m.color.blueF())};
                pdf_set_annot_color(ctx_, annot, 3, col);
                if (!m.comment.isEmpty())
                    pdf_set_annot_contents(ctx_, annot, m.comment.toUtf8().constData());
                pdf_update_annot(ctx_, annot);
            }
            fz_catch(ctx_)
            {
                failed = true;
                err = QString::fromUtf8(fz_caught_message(ctx_));
            }
            if (!failed) ++written;
        }

        fz_drop_page(ctx_, pg);
    }

    if (written == 0) {
        if (error) *error = err.isEmpty() ? QStringLiteral("no annotations written") : err;
        return false;
    }

    // Incremental save keeps the original file's structure and history intact.
    const QByteArray native = path_.toUtf8();   // UTF-8: see open()
    pdf_write_options opts = pdf_default_write_options;
    opts.do_incremental = 1;
    bool saveFailed = false;
    fz_try(ctx_) { pdf_save_document(ctx_, pdoc, native.constData(), &opts); }
    fz_catch(ctx_)
    {
        saveFailed = true;
        err = QString::fromUtf8(fz_caught_message(ctx_));
    }
    if (saveFailed) {
        if (error) *error = err;
        return false;
    }
    return true;
}

}  // namespace an
