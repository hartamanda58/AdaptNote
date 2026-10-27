#pragma once

#include "core/model.h"
#include "render/engine.h"

#include <QAbstractScrollArea>
#include <QElapsedTimer>
#include <QHash>
#include <QImage>
#include <QStringList>
#include <QVector>

class QMouseEvent;
class QTabletEvent;
class QWheelEvent;

namespace an {

// Renders pages and hosts the annotation layer. All markup geometry lives in PDF
// page space; this widget converts to/from view space (zoom + rotation) so that
// stored coordinates never depend on the current view.
class PageView : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit PageView(QWidget* parent = nullptr);

    void setEngine(IDocumentEngine* engine);  // not owned
    IDocumentEngine* engine() const { return engine_; }

    void setLayoutMode(PageLayoutMode m);
    PageLayoutMode layoutMode() const { return layoutMode_; }
    void setZoomMode(ZoomMode m);
    ZoomMode zoomMode() const { return zoomMode_; }
    void setZoom(float scale);
    float zoom() const { return scale_; }
    void setRotation(int degrees);
    int rotation() const { return rotation_; }

    int currentPage() const { return currentPage_; }
    void gotoPage(int page);

    void setMarkups(const QVector<Markup>& markups);
    const QVector<Markup>& markups() const { return markups_; }
    int markupIndexAt(const QPoint& viewPos) const;

    // In-document search overlay.
    void setSearchHits(const QVector<TextHit>& hits, int current);
    int searchHitCount() const { return hits_.size(); }

    // Region capture (image excerpt).
    void setCaptureMode(bool on);
    bool captureMode() const { return capture_; }

    // Handwriting mode: drags produce ink strokes instead of text selections.
    void setInkMode(bool on);
    bool inkMode() const { return inkMode_; }
    void setInkWidth(double w) { inkWidth_ = w; }
    double inkWidth() const { return inkWidth_; }
    void setInkColor(const QColor& c) { inkColor_ = c; }

    // Eraser mode: dragging deletes the ink strokes it touches (undoable upstream).
    void setEraseMode(bool on);
    bool eraseMode() const { return eraseMode_; }

    // Ink input seam. Real stylus events (and the mouse fallback) both funnel through
    // these three calls, which makes the pressure handling testable without hardware.
    void inkInputBegin(const QPointF& viewPos, double pressure, bool fromStylus);
    void inkInputBegin(const QPointF& viewPos, double pressure, bool fromStylus, double tilt,
                       double angle);
    void inkInputMove(const QPointF& viewPos, double pressure, bool fromStylus);
    void inkInputMove(const QPointF& viewPos, double pressure, bool fromStylus, double tilt,
                      double angle);
    void inkInputEnd();

    // Sentence highlight for read-aloud.
    void setReadingHighlight(int page, const QVector<Quad>& quads);

    // Fine grained reading position, as a fraction of the page height.
    double pageOffsetFraction(int page) const;
    void scrollToPageFraction(int page, double fraction);

    Selection selection() const { return sel_; }
    void clearSelection();
    void setSelection(const Selection& sel);

    int pageAt(const QPoint& viewPos) const;
    QImage renderPageToImage(int page);  // for tests / export

    // Maps a page-space point to viewport pixels (used by the visual regression test).
    QPointF mapPageToViewport(int page, const QPointF& pagePt) const;
    QRectF pageRectInViewport(int page) const;

signals:
    void pageChanged(int page);
    void regionSelected(int page, const QRectF& pageRect);
    void inkStrokeReady(int page, const QVector<InkPoint>& points);
    void inkEraseRequested(int page, const QStringList& markupIds,
                           const QVector<QPointF>& pagePath);
    void selectionChanged();
    void zoomChanged(float scale);
    void markupsChanged();

protected:
    void paintEvent(QPaintEvent* e) override;
    void resizeEvent(QResizeEvent* e) override;
    void mousePressEvent(QMouseEvent* e) override;
    void mouseMoveEvent(QMouseEvent* e) override;
    void mouseReleaseEvent(QMouseEvent* e) override;
    void tabletEvent(QTabletEvent* e) override;
    void wheelEvent(QWheelEvent* e) override;

private:
    struct PageGeom {
        QRectF rect;   // content coordinates
        QSizeF ptSize; // page size in points (unrotated)
        bool valid = false;
    };

    void relayout();
    void updateScrollbars();
    float computeScale() const;
    QSizeF displaySize(const QSizeF& pt) const;
    QPointF pageToView(int page, const QPointF& pt) const;
    QPointF viewToPage(int page, const QPointF& viewPt) const;
    QRectF quadToView(int page, const Quad& q) const;
    QImage imageFor(int page);
    // Physical pixels per logical pixel. Rendering at this factor is what keeps text
    // crisp on 125%/150%/200% Windows scaling; drawing a 1x bitmap into a 2x device is
    // what made pages look blurry.
    float viewDpr() const;
    void drawMarkup(QPainter& p, const Markup& m);
    void invalidateCache();

public:
    // ---- render cache diagnostics (also used by tests) ----
    void setCacheBudgetBytes(qint64 bytes);
    // Test seam: pin the device pixel ratio so the offscreen tests can assert the render
    // resolution without depending on the host screen. 0 = use the real one.
    void setDevicePixelRatioOverride(qreal dpr) { dprOverride_ = dpr; invalidateCache(); }
    qreal devicePixelRatioOverride() const { return dprOverride_; }
    float effectiveDevicePixelRatio() const { return viewDpr(); }
    // Exposes the render path so tests can assert the bitmap resolution.
    QImage cachedImageForTest(int page) { return imageFor(page); }
    int cacheEntryCount() const { return cache_.size(); }
    qint64 cacheBytes() const { return cacheBytes_; }
    bool isPageCached(int page) const;
    int graphicsPageCountForTest() const { return engine_ ? engine_->pageCount() : 0; }

private:

    IDocumentEngine* engine_ = nullptr;
    PageLayoutMode layoutMode_ = PageLayoutMode::Continuous;
    ZoomMode zoomMode_ = ZoomMode::FitWidth;
    float scale_ = 1.f;
    int rotation_ = 0;
    int currentPage_ = 0;

    QVector<PageGeom> geoms_;
    QVector<Markup> markups_;
    struct CacheEntry {
        QImage image;
        qint64 bytes = 0;
    };
    QHash<quint64, CacheEntry> cache_;
    QList<quint64> cacheLru_;      // most recently used last
    qint64 cacheBytes_ = 0;
    qreal dprOverride_ = 0.0;      // tests only
    qreal lastDpr_ = 1.0;          // detected ratio of the last paint
    qint64 cacheBudget_ = 96ll * 1024 * 1024;
    static constexpr int kCacheMaxEntries = 64;

    void touchCache(quint64 key);
    void evictCache();

    Selection sel_;
    bool selecting_ = false;
    int selPage_ = -1;
    QPointF selAnchor_;

    QVector<TextHit> hits_;
    int hitCurrent_ = -1;

    bool inkMode_ = false;
    bool inking_ = false;
    int inkPage_ = -1;
    QVector<InkPoint> inkStroke_;
    double inkWidth_ = 2.0;
    QColor inkColor_{0x1f, 0x23, 0x28};
    QElapsedTimer inkClock_;
    qint64 inkLastMs_ = 0;
    bool stylusStroke_ = false;   // a tablet stroke owns the current ink

    bool eraseMode_ = false;
    bool erasing_ = false;
    int erasePage_ = -1;
    QVector<QPointF> erasePath_;
    QStringList eraseHits_;

    int readingPage_ = -1;
    QVector<Quad> readingQuads_;

    bool capture_ = false;
    bool rubber_ = false;
    int rubberPage_ = -1;
    QPointF rubberStart_;
    QPointF rubberCur_;
};

}  // namespace an
