// P5 tests: handwriting geometry (ink), mind map layout, read-aloud sentence iterator.
#include "core/textselect.h"
#include "ink/ink.h"
#include "mind/layout.h"
#include "mind/mindmap.h"
#include "read/reader.h"
#include "render/mupdf_engine.h"

#include <QApplication>
#include <QThread>
#include <QElapsedTimer>
#include <QImage>
#include <QtMath>

#include <cstdio>
#include <functional>

using namespace an;

namespace {

int g_fail = 0;

void check(bool ok, const QString& what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) ++g_fail;
}

bool waitFor(const std::function<bool()>& cond, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    }
    return true;
}

QVector<InkPoint> straightLine(int count, QPointF from, QPointF to)
{
    QVector<InkPoint> pts;
    for (int i = 0; i < count; ++i) {
        const double t = count == 1 ? 0.0 : double(i) / (count - 1);
        InkPoint p;
        p.pos = from + (to - from) * t;
        p.pressure = 0.7;
        pts.append(p);
    }
    return pts;
}

int coloredPixels(const QImage& img)
{
    int n = 0;
    for (int y = 0; y < img.height(); y += 2) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); x += 2)
            if (QColor::fromRgb(line[x]).saturation() > 40) ++n;
    }
    return n;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AdaptNote"));
    QCoreApplication::setApplicationName(QStringLiteral("AdaptNoteP5Test"));

    const QString pdf = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("test.pdf");

    // ---------------------------------------------------------------- 1. ink geometry
    std::printf("\n=== handwriting (ink) ===\n");
    {
        const QVector<InkPoint> line = straightLine(200, QPointF(10, 20), QPointF(210, 20));
        const QVector<InkPoint> simple = ink::simplify(line, 0.6);
        check(simple.size() <= 4,
              QStringLiteral("collinear stroke simplified 200 -> %1 points").arg(simple.size()));
        check(qAbs(simple.first().pos.x() - 10) < 0.01 &&
                  qAbs(simple.last().pos.x() - 210) < 0.01,
              QStringLiteral("simplify keeps both endpoints"));
        check(qAbs(ink::length(line) - 200.0) < 0.5,
              QStringLiteral("stroke length = %1").arg(ink::length(line)));
        const QRectF lineBounds = ink::bounds(line, 2.0);
        check(lineBounds.width() > 195 && lineBounds.height() > 3,
              QStringLiteral("stroke bounds = %1x%2 (zero-height rects must still union)")
                  .arg(lineBounds.width()).arg(lineBounds.height()));

        const QPainterPath path = ink::smoothPath(line, 3.0);
        const QRectF br = path.boundingRect();
        check(!path.isEmpty(), QStringLiteral("smoothed path is not empty"));
        check(br.left() <= 10.5 && br.right() >= 209.5,
              QStringLiteral("path spans the stroke x=[%1,%2]").arg(br.left()).arg(br.right()));
        check(br.width() > 190 && br.height() < 30,
              QStringLiteral("straight stroke stays thin (%1 x %2)").arg(br.width()).arg(br.height()));

        // jittered stroke: simplification must keep the shape, not flatten it
        QVector<InkPoint> jitter = line;
        for (int i = 0; i < jitter.size(); ++i)
            jitter[i].pos.setY(20 + (i % 2 ? 4.0 : -4.0));
        const QVector<InkPoint> jSimple = ink::simplify(jitter, 0.5);
        const QRectF jBounds = ink::smoothPath(jSimple, 2.0).boundingRect();
        check(jSimple.size() > 10 && jBounds.height() > 6,
              QStringLiteral("zig-zag keeps its amplitude (%1 points, h=%2)")
                  .arg(jSimple.size()).arg(jBounds.height()));

        check(!ink::smoothPath({}, 2.0).isEmpty() == false,
              QStringLiteral("empty stroke yields an empty path"));

        // pressure -> width, tapered stamping, hit testing
        InkPoint light;
        light.pos = QPointF(0, 0);
        light.pressure = 0.3;
        InkPoint heavy = light;
        heavy.pressure = 1.0;
        check(ink::widthAt(light, 3.0) < ink::widthAt(heavy, 3.0),
              QStringLiteral("pressure controls width (%1 < %2)")
                  .arg(ink::widthAt(light, 3.0), 0, 'f', 2)
                  .arg(ink::widthAt(heavy, 3.0), 0, 'f', 2));

        QVector<InkPoint> tapered = straightLine(20, QPointF(0, 0), QPointF(100, 0));
        for (int i = 0; i < tapered.size(); ++i)
            tapered[i].pressure = 1.0 - 0.7 * (double(i) / (tapered.size() - 1));
        const QVector<ink::Stamp> st = ink::stamps(tapered, 4.0);
        check(st.size() > tapered.size(),
              QStringLiteral("stamps interpolate the stroke (%1 stamps for %2 points)")
                  .arg(st.size()).arg(tapered.size()));
        check(st.first().radius > st.last().radius,
              QStringLiteral("stroke tapers with pressure (%1 -> %2)")
                  .arg(st.first().radius, 0, 'f', 2)
                  .arg(st.last().radius, 0, 'f', 2));

        // stylus pressure mapping (renderer range) and the mouse fallback
        {
            check(qAbs(ink::pressureFromTablet(1.0) - 1.0) < 0.01,
                  QStringLiteral("full stylus pressure maps to 1.0"));
            check(ink::pressureFromTablet(0.0) >= 0.15,
                  QStringLiteral("zero pressure still leaves a hairline (%1)")
                      .arg(ink::pressureFromTablet(0.0), 0, 'f', 2));
            check(ink::pressureFromTablet(0.5) < ink::pressureFromTablet(0.9),
                  QStringLiteral("pressure is monotonic"));
            check(ink::pressureFromTablet(-1.0) >= 0.15 && ink::pressureFromTablet(5.0) <= 1.0,
                  QStringLiteral("out of range values are clamped"));

            check(ink::pressureFromSpeed(0.0) > ink::pressureFromSpeed(1.0),
                  QStringLiteral("slow mouse strokes are thicker than fast ones (%1 > %2)")
                      .arg(ink::pressureFromSpeed(0.0), 0, 'f', 2)
                      .arg(ink::pressureFromSpeed(1.0), 0, 'f', 2));
            check(ink::pressureFromSpeed(10.0) >= 0.3,
                  QStringLiteral("extremely fast strokes keep a minimum weight"));
        }

        // stylus tilt: wider footprint and an elliptical, oriented stamp
        {
            check(qAbs(ink::tiltFromDegrees(0, 0)) < 0.01,
                  QStringLiteral("an upright pen has no tilt"));
            check(ink::tiltFromDegrees(60, 0) > 0.9,
                  QStringLiteral("60 degrees of tilt is nearly flat (%1)")
                      .arg(ink::tiltFromDegrees(60, 0), 0, 'f', 2));
            check(ink::tiltFromDegrees(30, 30) > ink::tiltFromDegrees(30, 0),
                  QStringLiteral("tilt combines both axes"));

            InkPoint upright;
            upright.pos = QPointF(0, 0);
            upright.pressure = 0.8;
            InkPoint flat = upright;
            flat.tilt = 1.0;

            check(ink::widthAt(flat, 3.0) > ink::widthAt(upright, 3.0) * 1.5,
                  QStringLiteral("a flat pen writes wider (%1 vs %2)")
                      .arg(ink::widthAt(flat, 3.0), 0, 'f', 2)
                      .arg(ink::widthAt(upright, 3.0), 0, 'f', 2));

            QVector<InkPoint> stroked = straightLine(12, QPointF(0, 0), QPointF(60, 0));
            for (InkPoint& p : stroked) {
                p.tilt = 0.8;
                p.angle = an::kPi / 4.0;
            }
            const QVector<ink::Stamp> tilted = ink::stamps(stroked, 3.0);
            check(!tilted.isEmpty() && qAbs(tilted.first().angle - an::kPi / 4.0) < 0.01,
                  QStringLiteral("stamps carry the nib angle"));
            check(!tilted.isEmpty() && tilted.first().elongation > 1.3,
                  QStringLiteral("a tilted stamp is elliptical (%1)")
                      .arg(tilted.isEmpty() ? 0.0 : tilted.first().elongation, 0, 'f', 2));

            const QVector<ink::Stamp> uprightStamps = ink::stamps(straightLine(12, QPointF(0, 0),
                                                                              QPointF(60, 0)), 3.0);
            check(!uprightStamps.isEmpty() && qAbs(uprightStamps.first().elongation - 1.0) < 0.01,
                  QStringLiteral("an upright pen stamps circles"));
        }

        // partial erase: cut the stroke in the middle, at one end, and everywhere
        {
            const QVector<InkPoint> line2 = straightLine(60, QPointF(0, 0), QPointF(300, 0));
            const QVector<QPointF> midPath{QPointF(150, -20), QPointF(150, 20)};
            const double radius = 6.0;
            const auto mid = ink::splitStroke(line2, midPath, radius);
            check(mid.size() == 2, QStringLiteral("erase in the middle splits into %1 pieces").arg(mid.size()));
            if (mid.size() == 2) {
                check(mid.at(0).first().pos.x() < 10 && mid.at(1).last().pos.x() > 290,
                      QStringLiteral("both ends survive"));

                // the cut is interpolated at the eraser edge, so the gap is ~2*radius
                const double leftEdge = mid.at(0).last().pos.x();
                const double rightEdge = mid.at(1).first().pos.x();
                const double gap = rightEdge - leftEdge;
                check(qAbs(gap - 2 * radius) < 1.5,
                      QStringLiteral("gap = %1 pt (expected ~%2)").arg(gap).arg(2 * radius));
                check(qAbs(leftEdge - (150 - radius)) < 1.5 && qAbs(rightEdge - (150 + radius)) < 1.5,
                      QStringLiteral("cut points sit on the eraser edge (%1 / %2)")
                          .arg(leftEdge).arg(rightEdge));

                const double kept = ink::length(mid.at(0)) + ink::length(mid.at(1));
                check(qAbs(kept - (300 - 2 * radius)) < 2.0,
                      QStringLiteral("kept length = %1 (expected ~%2)")
                          .arg(kept).arg(300 - 2 * radius));
            }

            const QVector<QPointF> endPath{QPointF(-10, 0), QPointF(10, 0)};
            const auto atEnd = ink::splitStroke(line2, endPath, 6.0);
            check(atEnd.size() == 1 && atEnd.first().size() < line2.size(),
                  QStringLiteral("erasing at one end leaves a single shorter piece"));

            const QVector<QPointF> allPath{QPointF(-50, 0), QPointF(400, 0)};
            check(ink::splitStroke(line2, allPath, 8.0).isEmpty(),
                  QStringLiteral("erasing along the whole stroke removes it"));

            const QVector<QPointF> farPath{QPointF(0, 400), QPointF(300, 400)};
            const auto untouched = ink::splitStroke(line2, farPath, 5.0);
            check(untouched.size() == 1 && untouched.first().size() == line2.size(),
                  QStringLiteral("a far eraser keeps the stroke intact"));
        }

        check(ink::hits(tapered, QPointF(50, 0.5), 2.0), QStringLiteral("hit test on the stroke"));
        check(!ink::hits(tapered, QPointF(50, 30), 2.0),
              QStringLiteral("hit test misses far from the stroke"));
        InkPoint dot;
        dot.pos = QPointF(5, 5);
        check(!ink::smoothPath({dot}, 2.0).isEmpty(), QStringLiteral("single tap becomes a dot"));
    }

    // ---------------------------------------------------------------- 2. mind map
    std::printf("\n=== mind map ===\n");
    {
        Markup a;
        a.id = QStringLiteral("a");
        a.page = 0;
        a.text = QStringLiteral("阅读的本质是改变自己");
        Markup b = a;
        b.id = QStringLiteral("b");
        b.text = QStringLiteral("标注即思考");
        Markup c = a;
        c.id = QStringLiteral("c");
        c.page = 2;
        c.text = QStringLiteral("笔记的最小结构");
        c.type = MarkupType::Note;
        c.text.clear();
        c.comment = QStringLiteral("位置是索引");
        Markup gone = a;
        gone.id = QStringLiteral("d");
        gone.deleted = true;   // tombstones must not appear

        const mind::Node root =
            mind::buildFromNotes(QStringLiteral("测试书"), {a, b, c, gone});
        check(mind::nodeCount(root) == 6,
              QStringLiteral("tree = root + 2 pages + 3 live notes, tombstone skipped (%1 nodes)")
                  .arg(mind::nodeCount(root)));
        check(mind::depthOf(root) == 3, QStringLiteral("tree depth = %1").arg(mind::depthOf(root)));
        check(root.children.size() == 2 && root.children.first().page == 0 &&
                  root.children.last().page == 2,
              QStringLiteral("pages grouped and ordered"));
        check(root.children.first().children.size() == 2,
              QStringLiteral("page 1 has %1 notes").arg(root.children.first().children.size()));
        check(root.children.last().children.first().text.contains(QStringLiteral("位置")),
              QStringLiteral("note falls back to its comment when there is no excerpt"));

        // explicit groups (set by dragging nodes) take over from page grouping
        {
            MindGroup core;
            core.id = QStringLiteral("core-id");
            core.name = QStringLiteral("核心概念");
            Markup g1 = a;
            g1.id = QStringLiteral("g1");
            g1.mindGroup = core.id;          // notes reference the group entity
            g1.mindOrder = 1;
            Markup g2 = b;
            g2.id = QStringLiteral("g2");
            g2.mindGroup = core.id;
            g2.mindOrder = 0;
            Markup p1 = a;
            p1.id = QStringLiteral("p1");   // no group -> page bucket

            const mind::Node grouped =
                mind::buildFromNotes(QStringLiteral("书"), {g1, g2, p1}, {core});
            check(grouped.children.size() == 2,
                  QStringLiteral("explicit group + page bucket (%1 groups)").arg(grouped.children.size()));
            const mind::Node& first = grouped.children.first();
            check(first.text == QStringLiteral("核心概念") && first.children.size() == 2,
                  QStringLiteral("group node '%1' holds %2 notes")
                      .arg(first.text).arg(first.children.size()));
            check(first.children.first().markupId == QStringLiteral("g2"),
                  QStringLiteral("mindOrder sorts inside a group (first = g2)"));
            check(grouped.children.last().text.startsWith(QStringLiteral("第 ")),
                  QStringLiteral("ungrouped notes fall back to a page node"));
            check(first.groupKey == QStringLiteral("group:core-id"),
                  QStringLiteral("group key = %1").arg(first.groupKey));
        }

        // a parentId cycle must not make groups (and their notes) vanish from the tree.
        // Sync merge can produce one: device A moves X under Y, device B moves Y under X.
        {
            MindGroup x;
            x.id = QStringLiteral("cyc-a");
            x.name = QStringLiteral("循环 A");
            x.parentId = QStringLiteral("cyc-b");
            MindGroup y;
            y.id = QStringLiteral("cyc-b");
            y.name = QStringLiteral("循环 B");
            y.parentId = QStringLiteral("cyc-a");

            Markup inA = a;
            inA.id = QStringLiteral("cyc-note");
            inA.mindGroup = QStringLiteral("cyc-a");

            const mind::Node cyc =
                mind::buildFromNotes(QStringLiteral("书"), {inA}, {x, y});
            check(cyc.children.size() == 1,
                  QStringLiteral("a parentId cycle keeps one top-level group (%1)")
                      .arg(cyc.children.size()));
            int found = 0;
            std::function<void(const mind::Node&)> walk = [&](const mind::Node& n) {
                if (n.markupId == QStringLiteral("cyc-note")) ++found;
                for (const mind::Node& c : n.children) walk(c);
            };
            walk(cyc);
            check(found == 1,
                  QStringLiteral("the note under a cycling group still appears (%1)").arg(found));
            check(mind::nodeCount(cyc) == 4,
                  QStringLiteral("cycle tree has %1 nodes (root + 2 groups + note = 4)")
                      .arg(mind::nodeCount(cyc)));
        }

        // with first class groups the label comes from the entity, not the note
        {
            MindGroup group;
            group.id = QStringLiteral("gid-1");
            group.name = QStringLiteral("高频考点");
            Markup g = a;
            g.id = QStringLiteral("g3");
            g.mindGroup = QStringLiteral("gid-1");

            const mind::Node withEntity =
                mind::buildFromNotes(QStringLiteral("书"), {g}, {group});
            check(withEntity.children.size() == 1 &&
                      withEntity.children.first().text == QStringLiteral("高频考点"),
                  QStringLiteral("group label comes from the MindGroup entity (%1)")
                      .arg(withEntity.children.first().text));

            // a dangling id (group deleted elsewhere) falls back to the page bucket
            Markup d = g;
            d.mindGroup = QStringLiteral("gone-group");
            d.page = 2;
            const mind::Node dangling =
                mind::buildFromNotes(QStringLiteral("书"), {d}, {group});
            bool sawPageBucket = false;
            for (const mind::Node& node : dangling.children)
                if (node.text.startsWith(QStringLiteral("第 "))) sawPageBucket = true;
            check(sawPageBucket, QStringLiteral("dangling group id falls back to a page bucket"));
        }

        // nested groups: 顶层 -> 子级 -> note
        {
            MindGroup top;
            top.id = QStringLiteral("top-id");
            top.name = QStringLiteral("顶层");
            MindGroup child;
            child.id = QStringLiteral("child-id");
            child.name = QStringLiteral("子级");
            child.parentId = top.id;

            Markup inChild = a;
            inChild.id = QStringLiteral("deep");
            inChild.mindGroup = child.id;
            Markup inTop = b;
            inTop.id = QStringLiteral("shallow");
            inTop.mindGroup = top.id;

            const mind::Node nested =
                mind::buildFromNotes(QStringLiteral("书"), {inChild, inTop}, {top, child});
            check(nested.children.size() == 1, QStringLiteral("one root group"));
            const mind::Node& topNode = nested.children.first();
            check(topNode.text == QStringLiteral("顶层"), QStringLiteral("root group name"));
            check(topNode.children.size() == 2,
                  QStringLiteral("root group has a child group and a note (%1 children)")
                      .arg(topNode.children.size()));
            check(topNode.children.first().text == QStringLiteral("子级"),
                  QStringLiteral("child group comes first"));
            check(topNode.children.first().children.size() == 1 &&
                      topNode.children.first().children.first().markupId == QStringLiteral("deep"),
                  QStringLiteral("note nested under the child group"));
            check(mind::depthOf(nested) == 4,
                  QStringLiteral("depth = root/group/group/note (%1)").arg(mind::depthOf(nested)));
        }

        // manual ordering inside a group
        {
            auto mkAt = [](const QString& id, int order) {
                Markup m;
                m.id = id;
                m.page = 0;
                m.mindOrder = order;
                m.text = id;
                m.quads = {Quad::fromRect(QRectF(0, 0, 10, 10))};
                return m;
            };
            const QVector<Markup> siblings{mkAt(QStringLiteral("a"), 10), mkAt(QStringLiteral("b"), 20),
                                           mkAt(QStringLiteral("c"), 30)};

            auto front = mind::planReorder(siblings, QStringLiteral("c"), QStringLiteral("a"));
            QStringList order;
            for (const auto& p : front) order << QStringLiteral("%1=%2").arg(p.first).arg(p.second);
            check(front.size() == 3 && front.first().first == QStringLiteral("c") &&
                      front.first().second == 10,
                  QStringLiteral("drag to the front renumbers 10/20/30 (%1)")
                      .arg(order.join(QStringLiteral(", "))));

            auto back = mind::planReorder(siblings, QStringLiteral("a"), QString());
            check(!back.isEmpty() && back.last().first == QStringLiteral("a") && back.last().second == 30,
                  QStringLiteral("drag to the end puts it last (%1=%2)")
                      .arg(back.last().first).arg(back.last().second));

            auto same = mind::planReorder(siblings, QStringLiteral("b"), QStringLiteral("c"));
            check(same.isEmpty(), QStringLiteral("dropping in place changes nothing (%1)").arg(same.size()));

            check(mind::planReorder(siblings, QStringLiteral("nope"), QString()).isEmpty(),
                  QStringLiteral("unknown note yields no plan"));
        }

        mind::MindMapView view;
        view.resize(900, 600);
        view.setTree(root);
        check(view.sceneNodeCount() == 6,
              QStringLiteral("view created %1 node items").arg(view.sceneNodeCount()));

        const QImage img = view.renderToImage(QSize(900, 600));
        check(!img.isNull() && img.width() == 900, QStringLiteral("mind map renders to image"));
        check(coloredPixels(img) > 50,
              QStringLiteral("rendered map has visible nodes (%1 colored samples)").arg(coloredPixels(img)));

        // hit testing used by drag-and-drop regroup
        {
            const QPointF centre = view.nodeCenter(QStringLiteral("a"));
            check(!centre.isNull(), QStringLiteral("node centre found for a note"));
            check(view.markupIdAt(centre) == QStringLiteral("a"),
                  QStringLiteral("markupIdAt resolves the dragged note"));
            const QString groupAtNote = view.groupKeyAt(centre);
            check(groupAtNote.startsWith(QStringLiteral("page:")),
                  QStringLiteral("a page-grouped note reports its group key (%1)").arg(groupAtNote));
        }

        view.setZoom(10.0);
        check(view.zoom() <= 4.0, QStringLiteral("zoom clamped to %1").arg(view.zoom()));
        view.setZoom(0.01);
        check(view.zoom() >= 0.2, QStringLiteral("zoom lower clamp = %1").arg(view.zoom()));
    }

    // ---------------------------------------------------------------- 3. read aloud
    std::printf("\n=== read aloud ===\n");
    {
        MuPdfEngine eng;
        QString err;
        check(eng.open(pdf, &err), QStringLiteral("open %1: %2").arg(pdf, err));
        if (!eng.isOpen()) return 1;

        const TextPage* tp = eng.textPage(0);
        check(tp && !tp->chars.isEmpty(), QStringLiteral("fixture page has text"));
        const QVector<read::Sentence> sentences = read::splitSentences(*tp, 0);
        check(sentences.size() >= 3,
              QStringLiteral("page 1 split into %1 sentences").arg(sentences.size()));
        bool allGood = true;
        for (const read::Sentence& s : sentences) {
            if (s.text.trimmed().isEmpty() || s.quads.isEmpty() || s.page != 0 || s.endChar < s.startChar)
                allGood = false;
        }
        check(allGood, QStringLiteral("every sentence has text, quads and a page"));
        if (!sentences.isEmpty())
            check(sentences.first().text.size() > 3,
                  QStringLiteral("first sentence: \"%1\"…").arg(sentences.first().text.left(18)));
        if (sentences.size() >= 2)
            check(sentences.at(1).startChar > sentences.first().startChar,
                  QStringLiteral("sentences advance through the page"));

        read::Reader reader;
        reader.setPageProvider([&eng](int page) { return eng.textPage(page); }, eng.pageCount());
        check(!reader.ttsAvailable(),
              QStringLiteral("TTS engine available: %1 (falls back to timed pacing)")
                  .arg(reader.ttsAvailable() ? QStringLiteral("yes") : QStringLiteral("no")));

        reader.load(0);
        check(reader.sentenceCount() > 3,
              QStringLiteral("reader loaded %1 sentences").arg(reader.sentenceCount()));
        check(reader.currentIndex() == 0, QStringLiteral("starts at the first sentence"));

        int changes = 0;
        QStringList seen;
        QObject::connect(&reader, &read::Reader::sentenceChanged, [&](const read::Sentence& s) {
            ++changes;
            seen << s.text.left(10);
        });

        reader.setWordsPerMinute(900);
        reader.start();
        check(reader.active(), QStringLiteral("reader is active after start()"));
        const bool advanced = waitFor([&]() { return changes >= 3; }, 8000);
        check(advanced, QStringLiteral("auto-advance produced %1 sentence events").arg(changes));
        check(reader.currentIndex() >= 1,
              QStringLiteral("current index moved to %1").arg(reader.currentIndex()));

        reader.pause();
        const int frozen = reader.currentIndex();
        QCoreApplication::processEvents();
        QThread::msleep(150);
        QCoreApplication::processEvents();
        check(reader.currentIndex() == frozen && reader.paused(),
              QStringLiteral("pause stops advancing at %1").arg(frozen));

        reader.next();
        check(reader.currentIndex() == frozen + 1, QStringLiteral("manual next() to %1").arg(reader.currentIndex()));
        reader.previous();
        check(reader.currentIndex() == frozen, QStringLiteral("manual previous() back to %1").arg(reader.currentIndex()));

        reader.stop();
        check(!reader.active(), QStringLiteral("stop() deactivates the reader"));
    }


    // ---- mind-map layout: the rules the view and the exporters both rely on ----
    {
        std::printf("\n=== 6. mind map layout ===\n");
        mind::Node root;
        root.text = QStringLiteral("书");
        mind::Node g1;
        g1.text = QStringLiteral("组一");
        g1.groupKey = QStringLiteral("group:g1");
        mind::Node g2;
        g2.text = QStringLiteral("组二");
        g2.groupKey = QStringLiteral("group:g2");
        mind::Node leaf1;
        leaf1.text = QStringLiteral("摘录一");
        leaf1.page = 0;
        leaf1.markupId = QStringLiteral("m1");
        mind::Node leaf2;
        leaf2.text = QStringLiteral("摘录二");
        leaf2.page = 1;
        leaf2.markupId = QStringLiteral("m2");
        mind::Node leaf3;
        leaf3.text = QStringLiteral("摘录三");
        leaf3.page = 2;
        leaf3.markupId = QStringLiteral("m3");
        g1.children = {leaf1, leaf2};
        g2.children = {leaf3};
        root.children = {g1, g2};

        QHash<QString, double> heights;
        heights[QStringLiteral("group:g1")] = 58;
        heights[QStringLiteral("group:g2")] = 58;
        heights[QStringLiteral("page:0")] = 84;   // a long excerpt is a taller card
        heights[QStringLiteral("page:1")] = 40;
        heights[QStringLiteral("page:2")] = 40;

        QSizeF canvas;
        const QVector<mind::PlacedNode> placed = mind::layoutTree(root, heights, 64.0, {}, {}, &canvas);
        check(placed.size() == 6, QStringLiteral("one rect per visible node (%1)").arg(placed.size()));

        // columns follow depth
        QHash<QString, QRectF> byKey;
        for (const mind::PlacedNode& p : placed) {
            const QString key = p.groupId.isEmpty() ? QString() : QStringLiteral("group:") + p.groupId;
            if (!key.isEmpty()) byKey.insert(key, p.rect);
        }
        check(byKey.contains(QStringLiteral("group:g1")) &&
                  byKey.value(QStringLiteral("group:g1")).left() >
                      placed.first().rect.left(),
              QStringLiteral("children sit in a column right of the root"));

        // no two cards in the same column may overlap
        bool overlap = false;
        for (int i = 0; i < placed.size(); ++i) {
            for (int j = i + 1; j < placed.size(); ++j) {
                if (placed[i].rect.intersects(placed[j].rect)) overlap = true;
            }
        }
        check(!overlap, QStringLiteral("cards never overlap"));

        // a parent with children is vertically centred on them
        const QRectF g1r = byKey.value(QStringLiteral("group:g1"));
        double top = 1e9, bottom = -1e9;
        for (const mind::PlacedNode& p : placed) {
            if (p.groupId.startsWith(QStringLiteral("page"))) continue;
            if (p.depth == 2) {
                top = qMin(top, p.rect.top());
                bottom = qMax(bottom, p.rect.bottom());
            }
        }
        const double g1center = g1r.center().y();
        check(g1center > top - 40 && g1center < bottom + 40,
              QStringLiteral("parent is near the vertical centre of its subtree"));

        // measured height is respected (variable card sizes)
        double tall = 0;
        for (const mind::PlacedNode& p : placed)
            if (p.rect.height() > tall) tall = p.rect.height();
        check(qAbs(tall - 84.0) < 0.01,
              QStringLiteral("layout uses the measured card height (%1)").arg(tall));
        check(canvas.height() >= 84 && canvas.width() > 400,
              QStringLiteral("canvas covers the tree (%1x%2)")
                  .arg(canvas.width()).arg(canvas.height()));

        // collapsing a group hides its subtree
        QSet<QString> collapsed;
        collapsed.insert(QStringLiteral("group:g1"));
        QSizeF shrunken;
        const QVector<mind::PlacedNode> folded =
            mind::layoutTree(root, heights, 64.0, {}, collapsed, &shrunken);
        check(folded.size() == 4,
              QStringLiteral("collapsed group hides its children (%1 rects)").arg(folded.size()));
        check(shrunken.height() < canvas.height(),
              QStringLiteral("collapsing shrinks the canvas (%1 < %2)")
                  .arg(shrunken.height()).arg(canvas.height()));
    }

    std::printf("\n%s (%d failure(s))\n", g_fail == 0 ? "P5 CHECKS PASSED" : "P5 CHECKS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
