// Visual regression: verifies that the annotation overlay really sits on top of the
// rendered text, in every zoom / rotation / page layout. Runs headless (offscreen).
#include "ui/mainwindow.h"
#include "ui/pageview.h"

#include <QApplication>
#include <QColor>
#include <QFile>
#include <QImage>
#include <QPixmap>

#include <cmath>
#include <cstdio>

using namespace an;

namespace {

int g_fail = 0;

void check(bool ok, const QString& what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) ++g_fail;
}

void settle()
{
    for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
}

int saturatedPixels(const QImage& img, const QRect& r)
{
    int n = 0;
    const QRect a = r.intersected(img.rect());
    for (int y = a.top(); y <= a.bottom(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = a.left(); x <= a.right(); ++x) {
            if (QColor::fromRgb(line[x]).saturation() > 60) ++n;
        }
    }
    return n;
}

int darkPixels(const QImage& img, const QRect& r)
{
    int n = 0;
    const QRect a = r.intersected(img.rect());
    for (int y = a.top(); y <= a.bottom(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = a.left(); x <= a.right(); ++x) {
            const QColor c = QColor::fromRgb(line[x]);
            if (c.lightness() < 140) ++n;
        }
    }
    return n;
}

QImage grabView(MainWindow& w)
{
    settle();
    return w.view()->grab().toImage().convertToFormat(QImage::Format_RGB32);
}

QRect quadsRect(PageView* v, int page, const QVector<Quad>& quads)
{
    QRectF r;
    for (const Quad& q : quads) {
        const QPointF a = v->mapPageToViewport(page, q.ul);
        const QPointF b = v->mapPageToViewport(page, q.lr);
        const QPointF c = v->mapPageToViewport(page, q.ur);
        const QPointF d = v->mapPageToViewport(page, q.ll);
        const QRectF one = QRectF(a, b).normalized().united(QRectF(c, d).normalized());
        r = r.isNull() ? one : r.united(one);
    }
    return r.toAlignedRect().adjusted(-1, -1, 1, 1);
}

QRect markupRect(PageView* v, const Markup& m)
{
    return quadsRect(v, m.page, m.quads);
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AdaptNote"));
    QCoreApplication::setApplicationName(QStringLiteral("AdaptNoteVisualTest"));
    app.setStyle(QStringLiteral("Fusion"));
    QFile qss(QStringLiteral(":/app.qss"));
    if (qss.open(QIODevice::ReadOnly)) app.setStyleSheet(QString::fromUtf8(qss.readAll()));

    const QString pdf = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("test.pdf");

    MainWindow w;
    w.resize(1400, 950);
    w.show();
    settle();

    check(w.openDocument(pdf), QStringLiteral("open %1").arg(pdf));
    w.view()->setZoomMode(ZoomMode::FitWidth);
    w.view()->setLayoutMode(PageLayoutMode::Continuous);
    settle();

    w.applyDemoMarkups();
    settle();

    PageView* v = w.view();
    const QVector<Markup> markups = v->markups();
    check(markups.size() == 6,
          QStringLiteral("demo markups = %1 (4 text + 1 image + 1 ink)").arg(markups.size()));

    // ---- render resolution follows the device pixel ratio ----
    // Before this was honoured the page was rendered at 1x and stretched by the painter,
    // which is what made text look soft on 125%/150%/200% Windows scaling.
    {
        const QRectF pageRect = v->pageRectInViewport(0);
        v->setDevicePixelRatioOverride(1.0);
        const QImage at1 = v->cachedImageForTest(0);
        v->setDevicePixelRatioOverride(2.0);
        const QImage at2 = v->cachedImageForTest(0);
        v->setDevicePixelRatioOverride(3.0);
        const QImage at3 = v->cachedImageForTest(0);

        check(!at1.isNull() && !at2.isNull() && !at3.isNull(),
              QStringLiteral("pages render at every dpr"));
        const double ratio2 = at1.isNull() ? 0.0 : double(at2.width()) / double(at1.width());
        const double ratio3 = at1.isNull() ? 0.0 : double(at3.width()) / double(at1.width());
        check(qAbs(ratio2 - 2.0) < 0.06,
              QStringLiteral("dpr 2 renders ~2x pixels (%1x -> %2x, ratio %3)")
                  .arg(at1.width()).arg(at2.width()).arg(ratio2, 0, 'f', 2));
        check(qAbs(ratio3 - 3.0) < 0.06,
              QStringLiteral("dpr 3 renders ~3x pixels (ratio %1)").arg(ratio3, 0, 'f', 2));
        check(at2.width() >= pageRect.width() * 1.9,
              QStringLiteral("2x bitmap is not smaller than the logical page (%1 vs %2)")
                  .arg(at2.width()).arg(pageRect.width()));
        v->setDevicePixelRatioOverride(0.0);   // back to the real screen
    }

    QImage img = grabView(w);
    check(!img.isNull() && img.width() > 600 && img.height() > 400,
          QStringLiteral("window grabbed %1x%2").arg(img.width()).arg(img.height()));
    check(QColor(img.pixel(3, 3)) == QColor(0x2b, 0x2e, 0x33),
          QStringLiteral("viewport background is dark grey (%1)").arg(QColor(img.pixel(3, 3)).name()));

    const QRect page0 = v->pageRectInViewport(0).toAlignedRect();
    check(page0.isValid() && img.rect().contains(page0.topLeft()),
          QStringLiteral("page 1 rect on screen: %1,%2 %3x%4")
              .arg(page0.x()).arg(page0.y()).arg(page0.width()).arg(page0.height()));

    // Every markup overlay must (a) be visible as coloured pixels and (b) cover text.
    for (const Markup& m : markups) {
        const QRect r = markupRect(v, m);
        const int sat = saturatedPixels(img, r);
        const int dark = darkPixels(img, r);
        check(r.isValid() && page0.intersects(r),
              QStringLiteral("%1 on page %2 maps into the page rect %3,%4 %5x%6")
                  .arg(markupTypeName(m.type)).arg(m.page + 1).arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height()));
        if (m.type == MarkupType::Ink)
            check(dark > 3, QStringLiteral("ink overlay visible (%1 dark px)").arg(dark));
        else
            check(sat > 0, QStringLiteral("%1 overlay visible (%2 saturated px)").arg(markupTypeName(m.type)).arg(sat));
        if (m.type != MarkupType::Note)
            check(dark > 3, QStringLiteral("%1 sits on text (%2 dark px)").arg(markupTypeName(m.type)).arg(dark));
    }

    // Handwriting overlay must be visible where it was drawn.
    {
        QVector<InkPoint> stroke;
        for (int i = 0; i < 60; ++i) {
            InkPoint p;
            p.pos = QPointF(120 + i * 2.5, 300 + 12 * std::sin(i * 0.5));
            stroke.append(p);
        }
        w.addInkStroke(0, stroke);
        settle();
        img = grabView(w);
        QVector<Quad> inkQuads;
        for (const Markup& m : v->markups())
            if (m.type == MarkupType::Ink) inkQuads = m.quads;
        check(!inkQuads.isEmpty(), QStringLiteral("ink markup present after add"));
        if (!inkQuads.isEmpty()) {
            const QRect ir = quadsRect(v, 0, inkQuads);
            check(darkPixels(img, ir) > 5,
                  QStringLiteral("ink overlay drawn (%1 dark px inside the stroke box)")
                      .arg(darkPixels(img, ir)));
        }
    }

    // In-document search overlay must be visible too.
    w.findText(QStringLiteral("阅读"));
    settle();
    const QVector<TextHit> hits = w.currentHits();
    check(!hits.isEmpty(), QStringLiteral("search '阅读' -> %1 hit(s)").arg(hits.size()));
    if (!hits.isEmpty()) {
        img = grabView(w);
        const QRect hr = quadsRect(v, hits.first().page, hits.first().quads);
        check(saturatedPixels(img, hr) > 0,
              QStringLiteral("search hit overlay visible (%1 saturated px)").arg(saturatedPixels(img, hr)));
        check(img.rect().intersects(hr), QStringLiteral("search hit is on screen"));
    }
    w.findText(QString());

    // Rotation must move the overlay with the page, not leave it behind.
    // Pick the highlight explicitly: newest markups include the ink stroke, which is thin.
    Markup first = markups.first();
    for (const Markup& m : v->markups())
        if (m.type == MarkupType::Highlight) first = m;
    const QRect before = markupRect(v, first);
    v->setRotation(90);
    settle();
    img = grabView(w);
    const QRect after = markupRect(v, first);
    const QRect pageRot = v->pageRectInViewport(0).toAlignedRect();
    check(pageRot.width() > pageRot.height(),
          QStringLiteral("rotated page is landscape (%1x%2)").arg(pageRot.width()).arg(pageRot.height()));
    check(after != before, QStringLiteral("overlay follows rotation (%1,%2) -> (%3,%4)")
                                .arg(before.x()).arg(before.y()).arg(after.x()).arg(after.y()));
    check(img.rect().intersects(after),
          QStringLiteral("rotated overlay stays inside the viewport (%1,%2)")
              .arg(after.x()).arg(after.y()));
    check(saturatedPixels(img, after) > 0 && darkPixels(img, after) > 3,
          QStringLiteral("rotated overlay still on text (sat %1, dark %2)")
              .arg(saturatedPixels(img, after)).arg(darkPixels(img, after)));

    // Double page layout: page 1 and 2 side by side.
    v->setRotation(0);
    v->setLayoutMode(PageLayoutMode::Double);
    v->setZoomMode(ZoomMode::FitPage);
    settle();
    const QRect p0 = v->pageRectInViewport(0).toAlignedRect();
    const QRect p1 = v->pageRectInViewport(1).toAlignedRect();
    check(p0.isValid() && p1.isValid() && p1.left() > p0.right(),
          QStringLiteral("double page: p1 x=%1 w=%2, p2 x=%3 (side by side)")
              .arg(p0.x()).arg(p0.width()).arg(p1.x()));
    check(qAbs(p0.top() - p1.top()) <= 2, QStringLiteral("double page: tops aligned"));

    std::printf("\n%s (%d failure(s))\n", g_fail == 0 ? "VISUAL CHECKS PASSED" : "VISUAL CHECKS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
