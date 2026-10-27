#include "ui/pageview.h"

#include "core/textselect.h"
#include "ink/ink.h"

#include <QLineF>
#include <QTabletEvent>
#include <QtMath>
#include <QMouseEvent>
#include <QPainter>
#include <QImage>
#include <QPainterPath>
#include <QScrollBar>
#include <QWheelEvent>
#include <QtMath>

namespace an {
namespace {
constexpr int kPageGap = 14;
constexpr int kMargin = 14;
constexpr int kCacheLimit = 32;
}  // namespace

PageView::PageView(QWidget* parent)
    : QAbstractScrollArea(parent)
{
    setFrameShape(QFrame::NoFrame);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    viewport()->setAutoFillBackground(true);
    viewport()->setMouseTracking(false);
    setFocusPolicy(Qt::StrongFocus);
}

void PageView::setEngine(IDocumentEngine* engine)
{
    engine_ = engine;
    hits_.clear();
    hitCurrent_ = -1;
    currentPage_ = 0;
    sel_.clear();
    geoms_.clear();
    markups_.clear();
    invalidateCache();
    relayout();
}

void PageView::invalidateCache()
{
    cache_.clear();
    cacheLru_.clear();
    cacheBytes_ = 0;
}

void PageView::setLayoutMode(PageLayoutMode m)
{
    layoutMode_ = m;
    relayout();
}

void PageView::setZoomMode(ZoomMode m)
{
    zoomMode_ = m;
    relayout();
    emit zoomChanged(scale_);
}

void PageView::setZoom(float scale)
{
    if (scale <= 0.05f || scale > 12.f) return;
    zoomMode_ = ZoomMode::Custom;
    scale_ = scale;
    relayout();
    emit zoomChanged(scale_);
}

void PageView::setRotation(int degrees)
{
    const int next = ((degrees % 360) + 360) % 360;
    if (next % 90 != 0) return;
    // Keep the reading position: remember where we were inside the page.
    const double frac = pageOffsetFraction(currentPage_);
    rotation_ = next;
    invalidateCache();
    relayout();
    scrollToPageFraction(currentPage_, frac);
}

QSizeF PageView::displaySize(const QSizeF& pt) const
{
    const bool swap = (rotation_ == 90 || rotation_ == 270);
    return QSizeF((swap ? pt.height() : pt.width()) * scale_,
                  (swap ? pt.width() : pt.height()) * scale_);
}

float PageView::computeScale() const
{
    if (!engine_ || !engine_->isOpen()) return 1.f;

    auto pagePt = [this](int i) { return engine_->pageSize(i); };

    switch (zoomMode_) {
    case ZoomMode::Actual:
        return 1.f;
    case ZoomMode::Custom:
        return scale_;
    case ZoomMode::FitPage: {
        const QSizeF pt = pagePt(currentPage_);
        const bool swap = (rotation_ == 90 || rotation_ == 270);
        const double w = swap ? pt.height() : pt.width();
        const double h = swap ? pt.width() : pt.height();
        if (w <= 0 || h <= 0) return 1.f;
        const double sx = (viewport()->width() - 2.0 * kMargin) / w;
        const double sy = (viewport()->height() - 2.0 * kMargin) / h;
        return static_cast<float>(qBound(0.1, qMin(sx, sy), 8.0));
    }
    case ZoomMode::FitWidth:
    default: {
        const QSizeF pt = pagePt(currentPage_);
        const bool swap = (rotation_ == 90 || rotation_ == 270);
        const double w = swap ? pt.height() : pt.width();
        if (w <= 0) return 1.f;
        return static_cast<float>(qBound(0.1, (viewport()->width() - 2.0 * kMargin) / w, 8.0));
    }
    }
}

void PageView::relayout()
{
    geoms_.clear();
    if (!engine_ || !engine_->isOpen()) {
        updateScrollbars();
        viewport()->update();
        return;
    }

    // Reflowable documents (EPUB) are laid out to the current view width.
    if (engine_->isReflowable()) {
        const float w = qMax(200.f, static_cast<float>(viewport()->width() - 2 * kMargin) /
                                          qMax(0.1f, zoomMode_ == ZoomMode::Custom ? scale_ : 1.f));
        engine_->setLayout(w, 800.f, 13.f);
    }

    scale_ = computeScale();

    const int n = engine_->pageCount();
    const int cols = (layoutMode_ == PageLayoutMode::Double) ? 2 : 1;
    const int first = (layoutMode_ == PageLayoutMode::Single) ? currentPage_ : 0;
    const int last = (layoutMode_ == PageLayoutMode::Single) ? currentPage_ : n - 1;

    double maxW = 0;
    QVector<QSizeF> displays;
    for (int i = first; i <= last; ++i) {
        const QSizeF d = displaySize(engine_->pageSize(i));
        displays.append(d);
        maxW = qMax(maxW, d.width());
    }

    const double rowWidth = cols == 2 ? maxW * 2 + kPageGap : maxW;

    geoms_.resize(n);
    double y = kMargin;
    int idx = 0;
    for (int i = first; i <= last; i += cols) {
        double rowH = 0;
        for (int c = 0; c < cols && i + c <= last; ++c) {
            const QSizeF d = displays.at(idx++);
            rowH = qMax(rowH, d.height());
            double x = kMargin;
            if (cols == 2) {
                x = (c == 0) ? kMargin + (maxW - d.width()) / 2.0
                             : kMargin + maxW + kPageGap + (maxW - d.width()) / 2.0;
                if (qAbs(d.width() - maxW) < 0.5) x = (c == 0) ? kMargin : kMargin + maxW + kPageGap;
            } else {
                x = kMargin + (maxW - d.width()) / 2.0;
            }
            PageGeom g;
            g.rect = QRectF(x, y, d.width(), d.height());
            g.ptSize = engine_->pageSize(i + c);
            g.valid = true;
            geoms_[i + c] = g;
        }
        y += rowH + kPageGap;
    }
    Q_UNUSED(rowWidth);

    updateScrollbars();
    viewport()->update();
}

void PageView::updateScrollbars()
{
    double contentW = 0, contentH = 0;
    for (const PageGeom& g : geoms_) {
        if (!g.valid) continue;
        contentW = qMax(contentW, g.rect.right() + kMargin);
        contentH = qMax(contentH, g.rect.bottom() + kMargin);
    }
    verticalScrollBar()->setRange(0, qMax(0, static_cast<int>(contentH) - viewport()->height()));
    verticalScrollBar()->setPageStep(viewport()->height());
    verticalScrollBar()->setSingleStep(48);
    horizontalScrollBar()->setRange(0, qMax(0, static_cast<int>(contentW) - viewport()->width()));
    horizontalScrollBar()->setPageStep(viewport()->width());
    horizontalScrollBar()->setSingleStep(48);
}

void PageView::resizeEvent(QResizeEvent* e)
{
    QAbstractScrollArea::resizeEvent(e);
    const float before = scale_;
    relayout();
    if (!qFuzzyCompare(before, scale_)) emit zoomChanged(scale_);
}

QPointF PageView::pageToView(int page, const QPointF& p) const
{
    if (page < 0 || page >= geoms_.size() || !geoms_.at(page).valid) return {};
    const QSizeF ps = geoms_.at(page).ptSize;
    QPointF v;
    switch (rotation_) {
    case 90: v = QPointF(ps.height() - p.y(), p.x()); break;
    case 180: v = QPointF(ps.width() - p.x(), ps.height() - p.y()); break;
    case 270: v = QPointF(p.y(), ps.width() - p.x()); break;
    default: v = p; break;
    }
    return geoms_.at(page).rect.topLeft() + v * scale_;
}

QPointF PageView::viewToPage(int page, const QPointF& viewPt) const
{
    if (page < 0 || page >= geoms_.size() || !geoms_.at(page).valid) return {};
    const QSizeF ps = geoms_.at(page).ptSize;
    const QPointF v = (viewPt - geoms_.at(page).rect.topLeft()) / scale_;
    switch (rotation_) {
    case 90: return QPointF(v.y(), ps.height() - v.x());
    case 180: return QPointF(ps.width() - v.x(), ps.height() - v.y());
    case 270: return QPointF(ps.width() - v.y(), v.x());
    default: return v;
    }
}

QRectF PageView::quadToView(int page, const Quad& q) const
{
    QPolygonF poly;
    poly << pageToView(page, q.ul) << pageToView(page, q.ur) << pageToView(page, q.lr)
         << pageToView(page, q.ll);
    return poly.boundingRect();
}

int PageView::pageAt(const QPoint& viewPos) const
{
    const QPointF content = QPointF(viewPos) + QPointF(horizontalScrollBar()->value(),
                                                      verticalScrollBar()->value());
    for (int i = 0; i < geoms_.size(); ++i) {
        if (geoms_.at(i).valid && geoms_.at(i).rect.contains(content)) return i;
    }
    return -1;
}

void PageView::setCacheBudgetBytes(qint64 bytes)
{
    cacheBudget_ = qMax<qint64>(1ll * 1024 * 1024, bytes);
    evictCache();
}

void PageView::touchCache(quint64 key)
{
    cacheLru_.removeAll(key);
    cacheLru_.append(key);   // most recently used at the back
}

void PageView::evictCache()
{
    // drop least-recently-used pages until both limits are satisfied
    while (!cacheLru_.isEmpty() &&
           (cache_.size() > kCacheMaxEntries || cacheBytes_ > cacheBudget_)) {
        const quint64 victim = cacheLru_.takeFirst();
        auto it = cache_.find(victim);
        if (it == cache_.end()) continue;
        cacheBytes_ -= it.value().bytes;
        cache_.erase(it);
    }
}

bool PageView::isPageCached(int page) const
{
    for (auto it = cache_.constBegin(); it != cache_.constEnd(); ++it)
        if (static_cast<int>(it.key() >> 32) == page) return true;
    return false;
}

QImage PageView::imageFor(int page)
{
    if (!engine_ || !engine_->isOpen()) return {};

    const float dpr = viewDpr();
    const int bucket = static_cast<int>(scale_ * 100.f);
    const int dprBucket = static_cast<int>(dpr * 100.f + 0.5f);
    const quint64 key = (static_cast<quint64>(page) << 32) |
                        (static_cast<quint64>(bucket & 0xffff) << 16) |
                        (static_cast<quint64>(dprBucket & 0x3fff) << 2) |
                        static_cast<quint64>((rotation_ / 90) & 0x3);

    auto it = cache_.constFind(key);
    if (it != cache_.constEnd()) {
        touchCache(key);
        return it.value().image;
    }

    // Render at scale * dpr physical pixels. The painter lays it into the logical page
    // rectangle, so on a 2x screen the bitmap maps 1:1 to device pixels instead of being
    // stretched up from a 1x image.
    float renderScale = scale_ * dpr;
    const QSizeF pt = engine_->pageSize(page);
    if (pt.width() > 0 && pt.height() > 0) {
        // Guard against absurd allocations on very large pages at high zoom.
        constexpr double kMaxPixels = 40.0 * 1000.0 * 1000.0;
        const double px = static_cast<double>(pt.width()) * renderScale;
        const double py = static_cast<double>(pt.height()) * renderScale;
        if (px * py > kMaxPixels) {
            const double shrink = std::sqrt(kMaxPixels / (px * py));
            renderScale = static_cast<float>(renderScale * shrink);
        }
    }

    const QImage img = engine_->renderPage(page, renderScale, rotation_);
    if (img.isNull()) return img;

    CacheEntry entry;
    entry.image = img;
    entry.bytes = static_cast<qint64>(img.sizeInBytes());
    cache_.insert(key, entry);
    cacheBytes_ += entry.bytes;
    touchCache(key);
    evictCache();
    return entry.image;
}

float PageView::viewDpr() const
{
    if (dprOverride_ > 0.0) return static_cast<float>(dprOverride_);
    QWidget* w = viewport();
    const qreal dpr = w ? w->devicePixelRatioF() : 1.0;
    // Below 1.0 there is nothing to gain; above 4.0 the memory cost stops being worth it.
    return static_cast<float>(qBound<qreal>(1.0, dpr > 0.0 ? dpr : 1.0, 4.0));
}

QPointF PageView::mapPageToViewport(int page, const QPointF& pagePt) const
{
    return pageToView(page, pagePt) -
           QPointF(horizontalScrollBar()->value(), verticalScrollBar()->value());
}

QRectF PageView::pageRectInViewport(int page) const
{
    if (page < 0 || page >= geoms_.size() || !geoms_.at(page).valid) return {};
    return geoms_.at(page).rect.translated(-horizontalScrollBar()->value(),
                                           -verticalScrollBar()->value());
}

QImage PageView::renderPageToImage(int page)
{
    if (!engine_ || !engine_->isOpen()) return {};
    // Export path: keep the historical 1x semantics so exported PNGs do not silently
    // change resolution; a caller that wants more passes a bigger scale explicitly.
    return engine_->renderPage(page, scale_, rotation_);
}

void PageView::setMarkups(const QVector<Markup>& markups)
{
    markups_ = markups;
    viewport()->update();
}

void PageView::clearSelection()
{
    sel_.clear();
    selPage_ = -1;
    selecting_ = false;
    emit selectionChanged();
    viewport()->update();
}

void PageView::setSelection(const Selection& sel)
{
    sel_ = sel;
    emit selectionChanged();
    viewport()->update();
}

int PageView::markupIndexAt(const QPoint& viewPos) const
{
    const int page = pageAt(viewPos);
    if (page < 0) return -1;
    const QPointF content = QPointF(viewPos) + QPointF(horizontalScrollBar()->value(),
                                                      verticalScrollBar()->value());
    for (int i = markups_.size() - 1; i >= 0; --i) {
        const Markup& m = markups_.at(i);
        if (m.page != page) continue;
        for (const Quad& q : m.quads) {
            if (quadToView(page, q).adjusted(-2, -2, 2, 2).contains(content)) return i;
        }
    }
    return -1;
}

void PageView::setSearchHits(const QVector<TextHit>& hits, int current)
{
    hits_ = hits;
    hitCurrent_ = current;
    viewport()->update();
}

void PageView::setCaptureMode(bool on)
{
    capture_ = on;
    rubber_ = false;
    viewport()->setCursor(on ? Qt::CrossCursor : Qt::IBeamCursor);
    if (on) clearSelection();
}

void PageView::setInkMode(bool on)
{
    inkMode_ = on;
    inking_ = false;
    inkStroke_.clear();
    if (on) {
        eraseMode_ = false;
        erasing_ = false;
        erasePath_.clear();
        inkClock_.start();
        inkLastMs_ = 0;
        clearSelection();
    }
    viewport()->setCursor((on || eraseMode_) ? Qt::CrossCursor : Qt::IBeamCursor);
}

void PageView::setEraseMode(bool on)
{
    eraseMode_ = on;
    erasing_ = false;
    erasePath_.clear();
    eraseHits_.clear();
    if (on) {
        inkMode_ = false;
        inking_ = false;
        inkStroke_.clear();
        clearSelection();
    }
    viewport()->setCursor((on || inkMode_) ? Qt::CrossCursor : Qt::IBeamCursor);
}

void PageView::inkInputBegin(const QPointF& viewPos, double pressure, bool fromStylus)
{
    inkInputBegin(viewPos, pressure, fromStylus, 0.0, 0.0);
}

void PageView::inkInputBegin(const QPointF& viewPos, double pressure, bool fromStylus, double tilt,
                             double angle)
{
    if (!engine_ || !engine_->isOpen() || !inkMode_) return;
    const int page = pageAt(viewPos.toPoint());
    if (page < 0) return;

    inking_ = true;
    inkPage_ = page;
    inkStroke_.clear();
    inkClock_.restart();
    inkLastMs_ = 0;
    stylusStroke_ = fromStylus;

    InkPoint p;
    p.pos = viewToPage(page, viewPos + QPointF(horizontalScrollBar()->value(),
                                              verticalScrollBar()->value()));
    p.pressure = fromStylus ? ink::pressureFromTablet(pressure) : 0.55;
    p.tilt = fromStylus ? qBound(0.0, tilt, 1.0) : 0.0;
    p.angle = fromStylus ? angle : 0.0;
    inkStroke_.append(p);
    viewport()->update();
}

void PageView::inkInputMove(const QPointF& viewPos, double pressure, bool fromStylus)
{
    inkInputMove(viewPos, pressure, fromStylus, 0.0, 0.0);
}

void PageView::inkInputMove(const QPointF& viewPos, double pressure, bool fromStylus, double tilt,
                            double angle)
{
    if (!inking_ || inkPage_ < 0) return;

    const QPointF pagePt = viewToPage(inkPage_, viewPos + QPointF(horizontalScrollBar()->value(),
                                                                verticalScrollBar()->value()));
    const qint64 now = inkClock_.elapsed();
    const double dt = qMax<qint64>(1, now - inkLastMs_);
    inkLastMs_ = now;

    InkPoint p;
    p.pos = pagePt;
    if (fromStylus) {
        // real stylus: honour the reported pressure and orientation
        p.pressure = ink::pressureFromTablet(pressure);
        p.tilt = qBound(0.0, tilt, 1.0);
        p.angle = angle;
    } else if (!inkStroke_.isEmpty()) {
        // mouse / trackpad: derive the width from the pointer speed
        const double dist = QLineF(inkStroke_.last().pos, p.pos).length();
        const double target = ink::pressureFromSpeed(dist / dt);
        p.pressure = qBound(0.3, inkStroke_.last().pressure * 0.7 + target * 0.3, 1.0);
    } else {
        p.pressure = 0.7;
    }

    if (!inkStroke_.isEmpty() && QLineF(inkStroke_.last().pos, p.pos).length() < 0.4) {
        viewport()->update();
        return;
    }
    inkStroke_.append(p);
    viewport()->update();
}

void PageView::inkInputEnd()
{
    if (!inking_) return;
    inking_ = false;
    stylusStroke_ = false;
    const int page = inkPage_;
    QVector<InkPoint> stroke = ink::simplify(inkStroke_, 0.6);
    inkStroke_.clear();
    inkPage_ = -1;
    viewport()->update();
    if (page >= 0 && stroke.size() >= 2 && ink::length(stroke) > 3.0)
        emit inkStrokeReady(page, stroke);
}

void PageView::tabletEvent(QTabletEvent* e)
{
    // A stylus reports pressure; Qt may also synthesise mouse events for the same
    // gesture, so a tablet stroke owns the input until it ends.
    if (!inkMode_) {
        QAbstractScrollArea::tabletEvent(e);
        return;
    }
    if (inking_ && !stylusStroke_) return;   // ignore stylus events during a mouse stroke

    const QPointF pos = e->position();
    const double tilt = ink::tiltFromDegrees(e->xTilt(), e->yTilt());
    // Qt reports the pen rotation in degrees; -1 means "not supported"
    const double angleDeg = e->rotation() >= 0.0 ? e->rotation()
                                                 : (qRadiansToDegrees(std::atan2(e->yTilt(),
                                                                                 e->xTilt())));
    const double angle = angleDeg * an::kPi / 180.0;
    switch (e->type()) {
    case QEvent::TabletPress:
        inkInputBegin(pos, e->pressure(), true, tilt, angle);
        e->accept();
        return;
    case QEvent::TabletMove:
        if (inking_ && stylusStroke_) {
            inkInputMove(pos, e->pressure(), true, tilt, angle);
            e->accept();
            return;
        }
        break;
    case QEvent::TabletRelease:
        if (inking_ && stylusStroke_) {
            inkInputMove(pos, e->pressure(), true, tilt, angle);
            inkInputEnd();
            e->accept();
            return;
        }
        break;
    default:
        break;
    }
    QAbstractScrollArea::tabletEvent(e);
}

void PageView::setReadingHighlight(int page, const QVector<Quad>& quads)
{
    readingPage_ = page;
    readingQuads_ = quads;
    viewport()->update();
}

double PageView::pageOffsetFraction(int page) const
{
    if (page < 0 || page >= geoms_.size() || !geoms_.at(page).valid) return 0.0;
    const QRectF r = geoms_.at(page).rect;
    if (r.height() <= 0) return 0.0;
    const double y = verticalScrollBar()->value();
    return qBound(0.0, (y - r.top()) / r.height(), 1.0);
}

void PageView::scrollToPageFraction(int page, double fraction)
{
    if (page < 0 || page >= geoms_.size() || !geoms_.at(page).valid) return;
    const QRectF r = geoms_.at(page).rect;
    const double y = r.top() + qBound(0.0, fraction, 1.0) * r.height();
    verticalScrollBar()->setValue(qMax(0, static_cast<int>(y)));
}

void PageView::drawMarkup(QPainter& p, const Markup& m)
{
    if (m.page < 0 || m.page >= geoms_.size() || !geoms_.at(m.page).valid) return;

    QColor c = m.color;
    for (const Quad& q : m.quads) {
        const QRectF r = quadToView(m.page, q);
        switch (m.type) {
        case MarkupType::Highlight:
            p.fillRect(r, QColor(c.red(), c.green(), c.blue(), 90));
            break;
        case MarkupType::Underline: {
            p.setPen(QPen(c, qMax(1.0, 1.4 * scale_ / 1.5)));
            p.drawLine(r.bottomLeft(), r.bottomRight());
            break;
        }
        case MarkupType::Squiggly: {
            p.setPen(QPen(c, qMax(1.0, 1.0 * scale_ / 1.5)));
            const double step = 4.0;
            QPainterPath path;
            path.moveTo(r.bottomLeft());
            bool up = true;
            for (double x = r.left(); x <= r.right(); x += step) {
                path.lineTo(x, r.bottom() - (up ? 0 : 2.0));
                up = !up;
            }
            p.drawPath(path);
            break;
        }
        case MarkupType::Ink: {
            if (m.ink.isEmpty()) break;
            // Stamp-based rendering: radius follows the sample pressure, which the
            // capture path derives from pointer speed (slow = thicker), so strokes
            // taper naturally even with a mouse.
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(Qt::NoPen);
            p.setBrush(c);
            const QVector<ink::Stamp> stamps = ink::stamps(m.ink, inkWidth_);
            for (const ink::Stamp& st : stamps) {
                const QPointF centre = pageToView(m.page, st.pos);
                const double minor = qMax(0.5, st.radius * scale_);
                const double major = minor * qMax(1.0, st.elongation);
                if (qAbs(st.angle) < 0.01 && st.elongation <= 1.01) {
                    p.drawEllipse(centre, minor, minor);
                } else {
                    // tilted pen: an ellipse aligned with the nib direction
                    p.save();
                    p.translate(centre);
                    p.rotate(st.angle * 180.0 / an::kPi);
                    p.drawEllipse(QPointF(0, 0), major, minor);
                    p.restore();
                }
            }
            break;
        }
        case MarkupType::Image: {
            p.setPen(QPen(c.darker(115), qMax(1.0, 1.2 * scale_ / 1.5), Qt::DashLine));
            p.setBrush(QColor(c.red(), c.green(), c.blue(), 26));
            p.drawRect(r);
            break;
        }
        case MarkupType::Note: {
            const double s = qBound(10.0, 14.0 * scale_ / 1.5, 26.0);
            const QRectF icon(r.right() - s, r.top() - s * 0.6, s, s);
            p.setRenderHint(QPainter::Antialiasing, true);
            p.setPen(QPen(c.darker(120), 1));
            p.setBrush(c);
            p.drawRoundedRect(icon, 2, 2);
            p.setPen(Qt::white);
            QFont f = p.font();
            f.setPixelSize(static_cast<int>(s * 0.7));
            f.setBold(true);
            p.setFont(f);
            p.drawText(icon, Qt::AlignCenter, QStringLiteral("!"));
            p.setRenderHint(QPainter::Antialiasing, false);
            break;
        }
        }
    }
}

void PageView::paintEvent(QPaintEvent*)
{
    // A window dragged to a screen with a different scale factor keeps its logical layout
    // but needs new bitmaps; drop the cache so pages are re-rendered at the new density.
    const qreal dpr = viewDpr();
    if (!qFuzzyCompare(lastDpr_, dpr)) {
        lastDpr_ = dpr;
        invalidateCache();
    }

    QPainter p(viewport());
    // Pages are usually downscaled a little (or a lot at high DPI); without this Qt uses
    // nearest-neighbour and small text looks ragged even from a correct bitmap.
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.fillRect(viewport()->rect(), QColor(0x2b, 0x2e, 0x33));

    if (!engine_ || !engine_->isOpen()) {
        p.setPen(QColor(0xcf, 0xd3, 0xd8));
        p.drawText(viewport()->rect(), Qt::AlignCenter,
                   QStringLiteral("打开一本书开始阅读（Ctrl+O）"));
        return;
    }

    p.translate(-horizontalScrollBar()->value(), -verticalScrollBar()->value());

    const QRect viewRect(horizontalScrollBar()->value(), verticalScrollBar()->value(),
                         viewport()->width(), viewport()->height());

    for (int i = 0; i < geoms_.size(); ++i) {
        const PageGeom& g = geoms_.at(i);
        if (!g.valid || !g.rect.intersects(QRectF(viewRect))) continue;

        // page background + soft shadow
        p.fillRect(g.rect.adjusted(2, 2, 3, 3), QColor(0, 0, 0, 60));
        p.fillRect(g.rect, Qt::white);

        const QImage img = imageFor(i);
        if (!img.isNull()) p.drawImage(g.rect, img);
        else {
            p.setPen(QColor(0x99, 0x99, 0x99));
            p.drawText(g.rect, Qt::AlignCenter, QStringLiteral("…"));
        }
    }

    for (const Markup& m : markups_) drawMarkup(p, m);

    // search hits
    for (int i = 0; i < hits_.size(); ++i) {
        const TextHit& h = hits_.at(i);
        if (h.page < 0 || h.page >= geoms_.size() || !geoms_.at(h.page).valid) continue;
        const bool cur = (i == hitCurrent_);
        p.setPen(QPen(cur ? QColor(0xff, 0x6d, 0x00) : QColor(0xff, 0xc1, 0x07), cur ? 2.0 : 1.0));
        p.setBrush(cur ? QColor(0xff, 0xb3, 0x00, 110) : QColor(0xff, 0xe0, 0x66, 70));
        for (const Quad& q : h.quads) p.drawRect(quadToView(h.page, q));
        p.setBrush(Qt::NoBrush);
    }

    if (readingPage_ >= 0 && readingPage_ < geoms_.size() && !readingQuads_.isEmpty()) {
        p.setPen(QPen(QColor(0x2a, 0x6f, 0xd0), 1.2));
        p.setBrush(QColor(0x2a, 0x6f, 0xd0, 38));
        for (const Quad& q : readingQuads_) p.drawRect(quadToView(readingPage_, q));
        p.setBrush(Qt::NoBrush);
    }

    if (inking_ && !inkStroke_.isEmpty() && inkPage_ >= 0) {
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(Qt::NoPen);
        p.setBrush(inkColor_);
        for (const ink::Stamp& st : ink::stamps(inkStroke_, inkWidth_)) {
            const QPointF centre = pageToView(inkPage_, st.pos);
            const double minor = qMax(0.5, st.radius * scale_);
            const double major = minor * qMax(1.0, st.elongation);
            if (qAbs(st.angle) < 0.01 && st.elongation <= 1.01) {
                p.drawEllipse(centre, minor, minor);
            } else {
                p.save();
                p.translate(centre);
                p.rotate(st.angle * 180.0 / an::kPi);
                p.drawEllipse(QPointF(0, 0), major, minor);
                p.restore();
            }
        }
    }

    if (erasing_ && erasePath_.size() >= 2) {
        QPainterPath probe;
        probe.moveTo(erasePath_.first());
        for (int i = 1; i < erasePath_.size(); ++i) probe.lineTo(erasePath_.at(i));
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setPen(QPen(QColor(0xd9, 0x30, 0x25), qMax(1.5, 2.0 * scale_), Qt::DashLine));
        p.setBrush(Qt::NoBrush);
        p.drawPath(probe);
    }

    if (rubber_ && rubberPage_ >= 0 && rubberPage_ < geoms_.size()) {
        p.setPen(QPen(QColor(0x3d, 0x8b, 0xfd), 1.4, Qt::DashLine));
        p.setBrush(QColor(0x3d, 0x8b, 0xfd, 40));
        p.drawRect(QRectF(rubberStart_, rubberCur_).normalized());
        p.setBrush(Qt::NoBrush);
    }

    if (sel_.isValid() && sel_.page >= 0 && sel_.page < geoms_.size()) {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x3d, 0x8b, 0xfd, 70));
        for (const Quad& q : sel_.quads) p.drawRect(quadToView(sel_.page, q));
        p.setBrush(Qt::NoBrush);
    }
}

void PageView::mousePressEvent(QMouseEvent* e)
{
    if (e->button() != Qt::LeftButton || !engine_ || !engine_->isOpen()) {
        QAbstractScrollArea::mousePressEvent(e);
        return;
    }
    const int page = pageAt(e->position().toPoint());

    if (eraseMode_) {
        if (page < 0) return;
        erasing_ = true;
        erasePage_ = page;
        erasePath_.clear();
        eraseHits_.clear();
        erasePath_.append(e->position() + QPointF(horizontalScrollBar()->value(),
                                                 verticalScrollBar()->value()));
        viewport()->update();
        return;
    }

    if (inkMode_) {
        if (inking_ && stylusStroke_) return;   // stylus owns this gesture
        inkInputBegin(e->position(), 0.0, false);
        return;
    }

    if (capture_) {
        if (page < 0) return;
        rubber_ = true;
        rubberPage_ = page;
        rubberStart_ = e->position() + QPointF(horizontalScrollBar()->value(),
                                               verticalScrollBar()->value());
        rubberCur_ = rubberStart_;
        viewport()->update();
        return;
    }

    if (page < 0) {
        clearSelection();
        return;
    }
    const QPointF content = e->position() + QPointF(horizontalScrollBar()->value(),
                                                  verticalScrollBar()->value());
    selAnchor_ = viewToPage(page, content);
    selPage_ = page;
    selecting_ = true;
    sel_.clear();
    viewport()->update();
}

void PageView::mouseMoveEvent(QMouseEvent* e)
{
    if (erasing_) {
        erasePath_.append(e->position() + QPointF(horizontalScrollBar()->value(),
                                                 verticalScrollBar()->value()));
        const QPointF pagePt = viewToPage(erasePage_, erasePath_.last());
        const QPointF prevPt = viewToPage(erasePage_, erasePath_.at(erasePath_.size() - 2));
        for (const Markup& m : markups_) {
            if (m.type != MarkupType::Ink || m.page != erasePage_) continue;
            if (eraseHits_.contains(m.id)) continue;
            // sample a few points between events so fast drags still hit thin strokes
            for (int k = 0; k <= 4; ++k) {
                const QPointF probe = prevPt + (pagePt - prevPt) * (k / 4.0);
                if (ink::hits(m.ink, probe, qMax(4.0, inkWidth_ * 2.0 / qMax(0.2f, scale_)))) {
                    eraseHits_.append(m.id);
                    break;
                }
            }
        }
        viewport()->update();
        return;
    }

    if (inking_) {
        if (stylusStroke_) return;   // stylus events drive the stroke
        inkInputMove(e->position(), 0.0, false);
        return;
    }

    if (rubber_) {
        rubberCur_ = e->position() + QPointF(horizontalScrollBar()->value(),
                                            verticalScrollBar()->value());
        viewport()->update();
        return;
    }
    if (!selecting_ || selPage_ < 0) {
        QAbstractScrollArea::mouseMoveEvent(e);
        return;
    }
    const QPointF content = e->position() + QPointF(horizontalScrollBar()->value(),
                                                  verticalScrollBar()->value());
    const QPointF cur = viewToPage(selPage_, content);
    const TextPage* tp = engine_->textPage(selPage_);
    if (!tp) return;
    sel_ = selectRange(*tp, selPage_, selAnchor_, cur);
    emit selectionChanged();
    viewport()->update();
}

void PageView::mouseReleaseEvent(QMouseEvent* e)
{
    if (erasing_) {
        erasing_ = false;
        const int page = erasePage_;
        QVector<QPointF> pagePath;
        pagePath.reserve(erasePath_.size());
        for (const QPointF& p : erasePath_) pagePath.append(viewToPage(page, p));
        const QStringList hits = eraseHits_;
        erasePath_.clear();
        eraseHits_.clear();
        erasePage_ = -1;
        viewport()->update();
        if (!hits.isEmpty() && page >= 0) emit inkEraseRequested(page, hits, pagePath);
        return;
    }

    if (inking_) {
        inkInputEnd();
        return;
    }
    if (rubber_) {
        rubber_ = false;
        const QRectF viewRect = QRectF(rubberStart_, rubberCur_).normalized();
        const int page = rubberPage_;
        if (page >= 0 && viewRect.width() > 6 && viewRect.height() > 6) {
            const QPointF a = viewToPage(page, viewRect.topLeft());
            const QPointF b = viewToPage(page, viewRect.bottomRight());
            emit regionSelected(page, QRectF(a, b).normalized());
        }
        rubberPage_ = -1;
        viewport()->update();
        return;
    }
    selecting_ = false;
    QAbstractScrollArea::mouseReleaseEvent(e);
}

void PageView::wheelEvent(QWheelEvent* e)
{
    if (e->modifiers() & Qt::ControlModifier) {
        const double factor = e->angleDelta().y() > 0 ? 1.1 : 1 / 1.1;
        setZoom(static_cast<float>(scale_ * factor));
        e->accept();
        return;
    }
    QAbstractScrollArea::wheelEvent(e);
}

void PageView::gotoPage(int page)
{
    if (!engine_ || !engine_->isOpen()) return;
    page = qBound(0, page, engine_->pageCount() - 1);
    currentPage_ = page;

    if (layoutMode_ == PageLayoutMode::Single) {
        relayout();
    } else if (page < geoms_.size() && geoms_.at(page).valid) {
        const QRectF r = geoms_.at(page).rect;
        verticalScrollBar()->setValue(qMax(0, static_cast<int>(r.top()) - kMargin));
    }
    emit pageChanged(page);
    viewport()->update();
}

}  // namespace an
