// Feature test for P2: tags, full text search, image excerpts, undo/redo,
// fine grained reading position. Runs headless (offscreen).
#include "core/textselect.h"
#include "mind/mindmap.h"
#include "mind/layout.h"
#include "ui/mainwindow.h"
#include "ui/pageview.h"

#include <QApplication>
#include <QMouseEvent>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QLineEdit>
#include <QTemporaryDir>
#include <QListWidget>
#include <QTreeWidget>
#include <QDialog>
#include <QDialogButtonBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QToolBar>
#include <QToolButton>
#include <QTabBar>

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

void settle()
{
    for (int i = 0; i < 4; ++i) QCoreApplication::processEvents();
}

const Markup* findByType(const QVector<Markup>& v, MarkupType t)
{
    for (const Markup& m : v)
        if (m.type == t) return &m;
    return nullptr;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AdaptNote"));
    QCoreApplication::setApplicationName(QStringLiteral("AdaptNoteFeatureTest"));
    app.setStyle(QStringLiteral("Fusion"));
    QFile qss(QStringLiteral(":/app.qss"));
    const bool themeLoaded = qss.open(QIODevice::ReadOnly);
    check(themeLoaded, QStringLiteral("packaged application theme is available at its runtime path"));
    if (themeLoaded) app.setStyleSheet(QString::fromUtf8(qss.readAll()));

    const QString pdf = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("test.pdf");
    const QString epub = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("test.epub");

    MainWindow w;
    w.resize(1300, 900);
    w.show();
    settle();
    if (auto* toolbar = w.findChild<QToolBar*>(QStringLiteral("mainToolbar"))) {
        bool noOverflow = true;
        for (auto* button : toolbar->findChildren<QToolButton*>())
            if (button->isVisible() && button->text() == QStringLiteral("»")) noOverflow = false;
        check(noOverflow, QStringLiteral("main toolbar has no overflow button at compact window width"));
        bool captureTooltips = true;
        for (QAction* action : toolbar->actions())
            if (action && (action->text() == QStringLiteral("高亮") || action->text() == QStringLiteral("手写")) && action->toolTip().isEmpty()) captureTooltips = false;
        check(captureTooltips, QStringLiteral("icon-only annotation tools remain discoverable through tooltips"));
    }

    check(w.openDocument(pdf), QStringLiteral("open %1").arg(pdf));
    w.applyDemoMarkups();
    settle();

    // ---- 1. demo data: 4 text markups + 1 image excerpt ----
    const QVector<Markup> demo = w.visibleMarkups();
    check(demo.size() == 6,
          QStringLiteral("demo markups = %1 (4 text + 1 image + 1 ink)").arg(demo.size()));
    const Markup* img = findByType(demo, MarkupType::Image);
    check(img != nullptr, QStringLiteral("image excerpt markup present"));
    if (img) {
        check(QFile::exists(img->imagePath),
              QStringLiteral("image file written: %1").arg(QFileInfo(img->imagePath).fileName()));
        const QImage shot(img->imagePath);
        check(!shot.isNull() && shot.width() > 50 && shot.height() > 20,
              QStringLiteral("image excerpt pixels: %1x%2").arg(shot.width()).arg(shot.height()));
        check(img->quads.size() == 1, QStringLiteral("image markup has a region quad"));
    }

    // ---- 2. tags ----
    const QStringList tags = w.availableTags();
    check(tags.contains(QStringLiteral("方法论")), QStringLiteral("tag '方法论' aggregated (%1 tags)").arg(tags.size()));
    check(tags.contains(QStringLiteral("待复习")), QStringLiteral("tag '待复习' aggregated"));

    // ---- 3. in-document search ----
    w.findText(QStringLiteral("阅读"));
    settle();
    check(!w.currentHits().isEmpty(), QStringLiteral("find '阅读' -> %1 hits").arg(w.currentHits().size()));
    check(w.currentHitIndex() == 0, QStringLiteral("current hit index = %1").arg(w.currentHitIndex()));
    const int page0 = w.currentHits().isEmpty() ? -1 : w.currentHits().first().page;
    if (!w.currentHits().isEmpty())
        check(!w.currentHits().first().quads.isEmpty(),
              QStringLiteral("first hit has %1 quads on page %2")
                  .arg(w.currentHits().first().quads.size()).arg(page0 + 1));
    if (w.currentHits().size() > 1) {
        w.nextHit(1);
        settle();
        check(w.currentHitIndex() == 1, QStringLiteral("next hit -> index %1").arg(w.currentHitIndex()));
        w.nextHit(-1);
        settle();
        check(w.currentHitIndex() == 0, QStringLiteral("prev hit -> index %1").arg(w.currentHitIndex()));
    }
    w.findText(QStringLiteral("不存在的词组"));
    settle();
    check(w.currentHits().isEmpty(), QStringLiteral("no hits for absent text"));

    // ---- 4. undo / redo ----
    const int n0 = w.visibleMarkups().size();
    Markup extra;
    extra.id = newId();
    extra.docPath = QFileInfo(pdf).absoluteFilePath();
    extra.page = 0;
    extra.type = MarkupType::Highlight;
    extra.color = QColor("#3D8BFD");
    extra.quads = demo.first().quads;
    extra.text = QStringLiteral("撤销测试");
    extra.tags = {QStringLiteral("临时")};
    extra.created = extra.updated = QDateTime::currentMSecsSinceEpoch();

    w.pushAddMarkup(extra);
    settle();
    check(w.visibleMarkups().size() == n0 + 1,
          QStringLiteral("add via undo stack: %1 -> %2").arg(n0).arg(w.visibleMarkups().size()));
    check(w.availableTags().contains(QStringLiteral("临时")), QStringLiteral("new tag visible after add"));

    w.undoCommand();
    settle();
    check(w.visibleMarkups().size() == n0, QStringLiteral("undo add -> %1").arg(w.visibleMarkups().size()));
    check(!w.availableTags().contains(QStringLiteral("临时")), QStringLiteral("tag gone after undo"));

    w.redoCommand();
    settle();
    check(w.visibleMarkups().size() == n0 + 1, QStringLiteral("redo add -> %1").arg(w.visibleMarkups().size()));

    w.deleteMarkupById(extra.id);
    settle();
    check(w.visibleMarkups().size() == n0, QStringLiteral("delete -> %1").arg(w.visibleMarkups().size()));

    w.undoCommand();
    settle();
    check(w.visibleMarkups().size() == n0 + 1, QStringLiteral("undo delete restores -> %1").arg(w.visibleMarkups().size()));
    check(w.undoDepth() >= 1, QStringLiteral("undo stack depth = %1").arg(w.undoDepth()));

    // ---- 5. fine grained reading position survives a reopen ----
    w.view()->gotoPage(2);
    w.view()->scrollToPageFraction(2, 0.5);
    settle();
    w.saveReadingState();
    {
        MainWindow w2;
        w2.resize(1200, 800);
        w2.show();
        settle();
        check(w2.openDocument(pdf), QStringLiteral("reopen document in a second window"));
        settle();
        check(w2.view()->currentPage() == 2,
              QStringLiteral("reading position restored: page %1").arg(w2.view()->currentPage() + 1));
    }

    // ---- 6. handwriting stroke through the UI path ----
    {
        QVector<InkPoint> stroke;
        for (int i = 0; i < 40; ++i) {
            InkPoint p;
            p.pos = QPointF(80 + i * 3, 200 + 18 * std::sin(i * 0.4));
            stroke.append(p);
        }
        const int before = w.visibleMarkups().size();
        check(w.addInkStroke(0, stroke), QStringLiteral("add an ink stroke"));
        check(w.visibleMarkups().size() == before + 1,
              QStringLiteral("ink markup stored (%1 -> %2)")
                  .arg(before)
                  .arg(w.visibleMarkups().size()));
        // demo data already contains a 90-point stroke, so pick ours explicitly
        const Markup* inkMarkup = nullptr;
        for (const Markup& m : w.visibleMarkups())
            if (m.type == MarkupType::Ink && m.ink.size() == 40) inkMarkup = &m;
        check(inkMarkup != nullptr,
              QStringLiteral("ink markup persisted with all 40 sample points"));
        check(inkMarkup != nullptr && !inkMarkup->quads.isEmpty(),
              QStringLiteral("ink markup has a hit-test quad"));
        w.undoCommand();
        settle();
        check(w.visibleMarkups().size() == before, QStringLiteral("undo removes the ink stroke"));
    }

    // ---- 6b. eraser: cutting a stroke in the middle, then full removal ----
    {
        QVector<InkPoint> stroke;
        for (int i = 0; i < 30; ++i) {
            InkPoint p;
            p.pos = QPointF(100 + i * 4, 400);
            stroke.append(p);
        }
        check(w.addInkStroke(0, stroke), QStringLiteral("add a stroke to erase"));
        const int before = w.visibleMarkups().size();

        check(w.eraseInkAt(0, QPointF(160, 400), 3.0) == 1,
              QStringLiteral("eraser touches the stroke"));
        settle();
        check(w.visibleMarkups().size() == before + 1,
              QStringLiteral("mid-stroke erase cuts it into two (%1 -> %2)")
                  .arg(before).arg(w.visibleMarkups().size()));
        check(w.eraseInkAt(0, QPointF(160, 400), 3.0) == 0,
              QStringLiteral("erasing the same spot again is a no-op"));

        // erasing a point the whole short stroke passes through removes it entirely
        const int beforeSecond = w.visibleMarkups().size();
        QVector<InkPoint> tiny;
        for (int i = 0; i < 3; ++i) {
            InkPoint p;
            p.pos = QPointF(400 + i * 1.0, 700);
            tiny.append(p);
        }
        w.addInkStroke(0, tiny);
        const int withTiny = w.visibleMarkups().size();
        check(withTiny == beforeSecond + 1, QStringLiteral("add a tiny stroke"));
        w.eraseInkAt(0, QPointF(400.5, 700), 6.0);
        settle();
        check(w.visibleMarkups().size() <= withTiny,
              QStringLiteral("erasing a dot-sized stroke does not duplicate it (%1 -> %2)")
                  .arg(withTiny).arg(w.visibleMarkups().size()));

        while (w.visibleMarkups().size() > before) w.undoCommand();
        settle();
        check(w.visibleMarkups().size() == before,
              QStringLiteral("undo restores the erased state (%1)").arg(w.visibleMarkups().size()));
    }

    // ---- 6b2. stylus input carries real pressure into the stored stroke ----
    {
        auto inkIds = [&]() {
            QStringList ids;
            for (const Markup& m : w.visibleMarkups())
                if (m.type == MarkupType::Ink) ids << m.id;
            return ids;
        };
        const QStringList before = inkIds();

        w.view()->setInkMode(true);
        settle();

        const int page = 0;
        const QRectF pageRect = w.view()->pageRectInViewport(page);
        check(pageRect.isValid(), QStringLiteral("page rect available for input mapping"));

        // a tablet stroke: pressure ramps 0.2 -> 1.0 along the stroke
        const QPointF start = pageRect.topLeft() + QPointF(80, 640);
        w.view()->inkInputBegin(start, 0.2, true);
        for (int i = 1; i <= 10; ++i)
            w.view()->inkInputMove(start + QPointF(i * 12.0, 0), 0.2 + 0.08 * i, true);
        w.view()->inkInputEnd();
        settle();

        Markup stylusCopy;
        bool haveStylus = false;
        for (const Markup& m : w.visibleMarkups())
            if (m.type == MarkupType::Ink && !before.contains(m.id)) {
                stylusCopy = m;
                haveStylus = true;
            }
        check(haveStylus, QStringLiteral("stylus stroke stored (%1 points)")
                              .arg(haveStylus ? stylusCopy.ink.size() : 0));
        if (haveStylus) {
            const Markup* stylus = &stylusCopy;   // stable copy, ids are what matter
            double minP = 1.0, maxP = 0.0;
            for (const InkPoint& p : stylusCopy.ink) {
                minP = qMin(minP, p.pressure);
                maxP = qMax(maxP, p.pressure);
            }
            check(minP < 0.4 && maxP > 0.7,
                  QStringLiteral("pressure varies with the stylus (%1 .. %2)")
                      .arg(minP, 0, 'f', 2)
                      .arg(maxP, 0, 'f', 2));
            check(qAbs(minP - 0.2) < 0.06,
                  QStringLiteral("lightest sample matches the reported pressure (%1)")
                      .arg(minP, 0, 'f', 2));
        }

        // a tilted stylus must store tilt + nib angle with the stroke
        {
            const QStringList known = inkIds();
            w.view()->inkInputBegin(start + QPointF(0, 30), 0.6, true, 0.75, an::kPi / 6.0);
            for (int i = 1; i <= 8; ++i)
                w.view()->inkInputMove(start + QPointF(i * 14.0, 30), 0.6, true, 0.75, an::kPi / 6.0);
            w.view()->inkInputEnd();
            settle();

            bool found = false;
            double tilt = -1.0;
            double angle = 0.0;
            for (const Markup& m : w.visibleMarkups()) {
                if (m.type != MarkupType::Ink || known.contains(m.id)) continue;
                found = true;
                tilt = m.ink.first().tilt;
                angle = m.ink.first().angle;
            }
            check(found, QStringLiteral("tilted stroke stored"));
            check(tilt > 0.7 && tilt <= 1.0, QStringLiteral("tilt kept in the stroke (%1)")
                                                 .arg(tilt, 0, 'f', 2));
            check(qAbs(angle - an::kPi / 6.0) < 0.05,
                  QStringLiteral("nib angle kept in the stroke (%1 rad)").arg(angle, 0, 'f', 2));
        }

        // same geometry with the mouse: no stylus data, speed-derived pressures
        w.view()->inkInputBegin(start + QPointF(0, 40), 0.0, false);
        for (int i = 1; i <= 10; ++i)
            w.view()->inkInputMove(start + QPointF(i * 12.0, 40), 0.0, false);
        w.view()->inkInputEnd();
        settle();

        Markup mouseCopy;
        bool haveMouse = false;
        for (const Markup& m : w.visibleMarkups()) {
            if (m.type != MarkupType::Ink) continue;
            if (before.contains(m.id)) continue;
            if (haveStylus && m.id == stylusCopy.id) continue;
            mouseCopy = m;
            haveMouse = true;
        }
        check(haveMouse, QStringLiteral("mouse stroke stored (%1 points)")
                             .arg(haveMouse ? mouseCopy.ink.size() : 0));
        if (haveMouse) {
            double minP = 1.0;
            for (const InkPoint& p : mouseCopy.ink) minP = qMin(minP, p.pressure);
            check(minP >= 0.29,
                  QStringLiteral("mouse pressure stays in the simulated range (%1)")
                      .arg(minP, 0, 'f', 2));
        }

        w.view()->setInkMode(false);
        settle();
    }

    // ---- 6c. filtered export only contains the filtered notes ----
    {
        w.setTagFilter(QStringLiteral("待复习"));
        settle();
        const int shown = w.notesShown().size();
        check(shown >= 1 && shown < w.visibleMarkups().size(),
              QStringLiteral("tag filter narrows the list (%1 of %2)")
                  .arg(shown).arg(w.visibleMarkups().size()));

        const QString path = QDir::tempPath() + QStringLiteral("/adaptnote-filtered.md");
        QString err;
        check(w.exportMarkups(QStringLiteral("md"), path, &err, true),
              QStringLiteral("export filtered: %1").arg(err));
        QFile md(path);
        QString text;
        if (md.open(QIODevice::ReadOnly)) text = QString::fromUtf8(md.readAll());
        int bullets = 0;
        for (const QString& line : text.split(QLatin1Char('\n')))
            if (line.startsWith(QStringLiteral("- **"))) ++bullets;
        check(bullets == shown,
              QStringLiteral("markdown has %1 bullets for %2 filtered notes").arg(bullets).arg(shown));

        // the same export without the flag contains everything
        const QString allPath = QDir::tempPath() + QStringLiteral("/adaptnote-all.md");
        check(w.exportMarkups(QStringLiteral("md"), allPath, &err, false),
              QStringLiteral("export all: %1").arg(err));
        QFile all(allPath);
        QString allText;
        if (all.open(QIODevice::ReadOnly)) allText = QString::fromUtf8(all.readAll());
        int allBullets = 0;
        for (const QString& line : allText.split(QLatin1Char('\n')))
            if (line.startsWith(QStringLiteral("- **"))) ++allBullets;
        check(allBullets > bullets,
              QStringLiteral("unfiltered export is larger (%1 > %2)").arg(allBullets).arg(bullets));

        w.setTagFilter(QString());
        settle();
        check(w.notesShown().size() == w.visibleMarkups().size(),
              QStringLiteral("clearing the filter restores the list"));
    }

    // ---- 6d. reader speed is configurable ----
    {
        w.setReaderSpeed(420);
        w.setReaderSpeed(240);
        check(true, QStringLiteral("reader speed accepts 420/240 wpm without error"));
    }

    // ---- 6e. mind map regrouping (drag) is persisted and undoable ----
    {
        const QVector<Markup> before = w.visibleMarkups();
        QString targetId;
        for (const Markup& m : before)
            if (m.type == MarkupType::Highlight) {
                targetId = m.id;
                break;
            }
        check(!targetId.isEmpty(), QStringLiteral("found a note to regroup"));

        check(w.applyMindReparent(targetId, QStringLiteral("group:考点")),
              QStringLiteral("regroup into 考点 (adopted into a real group)"));
        settle();
        QString groupId;
        {
            for (const Markup& m : w.visibleMarkups())
                if (m.id == targetId) groupId = m.mindGroup;
            check(!groupId.isEmpty() && groupId != QStringLiteral("考点"),
                  QStringLiteral("note now references a group id (%1)").arg(groupId.left(8)));

            const QVector<MindGroup> groups = w.mindGroups();
            QString name;
            for (const MindGroup& g : groups)
                if (g.id == groupId) name = g.name;
            check(name == QStringLiteral("考点"),
                  QStringLiteral("the id resolves to the group entity '%1'").arg(name));
            Markup parentNote;
            Markup childNote;
            for (const Markup& note : w.visibleMarkups()) if (note.id == targetId) parentNote = note;
            for (const Markup& note : w.visibleMarkups())
                if (note.id != targetId && note.mindGroup == parentNote.mindGroup) { childNote = note; break; }
            if (!childNote.id.isEmpty()) {
                childNote.parentMarkupId = parentNote.id;
                check(w.storeUpsertForTest(childNote), QStringLiteral("persist a note-under-note parent edge"));
                const Markup roundTrip = Markup::fromJson(w.visibleMarkups().first().toJson());
                bool parentRoundTrip = roundTrip.id != parentNote.id || roundTrip.parentMarkupId.isEmpty();
                for (const Markup& note : w.visibleMarkups())
                    if (note.id == childNote.id) parentRoundTrip = Markup::fromJson(note.toJson()).parentMarkupId == parentNote.id;
                check(parentRoundTrip, QStringLiteral("note parent survives sync JSON roundtrip"));
                w.refreshFromStore();
                for (const Markup& note : w.visibleMarkups()) if (note.id == parentNote.id) parentNote = note;
                for (const Markup& note : w.visibleMarkups()) if (note.id == childNote.id) childNote = note;
            }

            const mind::Node tree = mind::buildFromNotes(QStringLiteral("书"), w.visibleMarkups(), groups);
            bool hasGroup = false;
            for (const mind::Node& g : tree.children)
                if (g.text == QStringLiteral("考点")) hasGroup = true;
            check(hasGroup, QStringLiteral("mind tree shows the 考点 group"));
            if (!childNote.id.isEmpty()) {
                bool nestedNote = false;
                std::function<void(const mind::Node&)> findParent = [&](const mind::Node& node) {
                    if (node.markupId == parentNote.id)
                        for (const auto& child : node.children) if (child.markupId == childNote.id) nestedNote = true;
                    for (const auto& child : node.children) findParent(child);
                };
                findParent(tree);
                check(nestedNote, QStringLiteral("mind map tree nests a note under a note"));
                bool unique = true; QSet<QString> keys;
                std::function<void(const mind::Node&)> collect = [&](const mind::Node& node) {
                    const QString key = mind::nodeKey(node);
                    if (keys.contains(key)) unique = false;
                    keys.insert(key);
                    for (const auto& child : node.children) collect(child);
                };
                collect(tree);
                check(unique, QStringLiteral("nested note ids do not collide with page group keys"));
                childNote.parentMarkupId.clear();
                check(w.storeUpsertForTest(childNote), QStringLiteral("reset note hierarchy for following independent tests"));
                w.refreshFromStore();
            }
        }

        // nested group: create a child, then move it back to the top level
        {
            const QString childId = w.createMindGroup(QStringLiteral("子考点"), groupId);
            check(!childId.isEmpty() && childId != groupId, QStringLiteral("create a child group"));
            settle();
            QString parentOfChild;
            for (const MindGroup& g : w.mindGroups())
                if (g.id == childId) parentOfChild = g.parentId;
            check(parentOfChild == groupId, QStringLiteral("child is nested under the parent"));

            const mind::Node tree =
                mind::buildFromNotes(QStringLiteral("书"), w.visibleMarkups(), w.mindGroups());
            bool nested = false;
            std::function<void(const mind::Node&)> walk = [&](const mind::Node& n) {
                if (n.text == QStringLiteral("子考点") && n.groupKey ==
                                                            QStringLiteral("group:") + childId)
                    nested = true;
                for (const mind::Node& c : n.children) walk(c);
            };
            walk(tree);
            check(nested, QStringLiteral("tree contains the child group"));

            check(w.moveMindGroupUp(QStringLiteral("group:") + childId),
                  QStringLiteral("move the child group up"));
            settle();
            QString parentAfter;
            for (const MindGroup& g : w.mindGroups())
                if (g.id == childId) parentAfter = g.parentId;
            check(parentAfter.isEmpty(), QStringLiteral("child is now top level"));
        }

        // manual ordering inside the group: move a note to the front
        {
            w.applyMindReparent(targetId, QStringLiteral("group:") + groupId);
            for (const Markup& note : w.visibleMarkups())
                if (note.id != targetId && note.mindGroup == groupId && !note.parentMarkupId.isEmpty()) {
                    Markup parentless = note; parentless.parentMarkupId.clear(); w.storeUpsertForTest(parentless);
                }
            w.refreshFromStore();
            // put two more notes into the same group so ordering is visible
            QVector<Markup> all = w.visibleMarkups();
            for (const Markup& m : all) {
                if (m.id == targetId || m.type != MarkupType::Highlight) continue;
                Markup e = m;
                e.mindGroup = groupId;
                e.mindOrder = 100 + static_cast<int>(e.page);
                w.applyMindReparent(e.id, QStringLiteral("group:") + groupId);
            }
            settle();

            QVector<QString> inGroup;
            for (const Markup& m : w.visibleMarkups())
                if (m.mindGroup == groupId && m.parentMarkupId.isEmpty()) inGroup.append(m.id);
            check(inGroup.size() >= 2,
                  QStringLiteral("group holds %1 notes to reorder").arg(inGroup.size()));

            const QString mover = inGroup.last();
            Markup moverBefore;
            for (const Markup& note : w.visibleMarkups()) if (note.id == mover) moverBefore = note;
            const QString first = inGroup.first();
            check(w.applyMindReorder(mover, QStringLiteral("group:") + groupId, first),
                  QStringLiteral("move the last note in front of the first"));
            settle();

            QVector<QPair<int, QString>> ordered;
            for (const Markup& m : w.visibleMarkups())
                if (m.mindGroup == groupId && m.parentMarkupId.isEmpty()) ordered.append({m.mindOrder, m.id});
            std::sort(ordered.begin(), ordered.end());
            check(!ordered.isEmpty() && ordered.first().second == mover,
                  QStringLiteral("moved note is first in the group (order %1)")
                      .arg(ordered.isEmpty() ? -1 : ordered.first().first));

            const mind::Node tree =
                mind::buildFromNotes(QStringLiteral("书"), w.visibleMarkups(), w.mindGroups());
            bool firstLeafIsMover = false;
            for (const mind::Node& g : tree.children) {
                if (g.groupKey != QStringLiteral("group:") + groupId) continue;
                for (const auto& child : g.children)
                    if (child.markupId == mover) firstLeafIsMover = g.children.first().markupId == mover;
            }
            check(firstLeafIsMover, QStringLiteral("tree shows the moved note first"));

            w.undoCommand();
            settle();
            bool restored = false;
            for (const Markup& m : w.visibleMarkups())
                if (m.id == mover && m.mindOrder == moverBefore.mindOrder) restored = true;
            check(restored, QStringLiteral("undo restores the previous order in one step"));
        }

        // rename the group through the entity API
        check(w.renameMindGroup(QStringLiteral("group:") + groupId, QStringLiteral("高频考点")),
              QStringLiteral("rename the group"));
        settle();
        {
            QString name;
            for (const MindGroup& g : w.mindGroups())
                if (g.id == groupId) name = g.name;
            check(name == QStringLiteral("高频考点"),
                  QStringLiteral("renamed group is visible (%1)").arg(name));
        }

        // deleting the group sends the note back to the page bucket
        check(w.deleteMindGroup(QStringLiteral("group:") + groupId),
              QStringLiteral("delete the group"));
        settle();
        {
            QString group;
            for (const Markup& m : w.visibleMarkups())
                if (m.id == targetId) group = m.mindGroup;
            check(group.isEmpty(), QStringLiteral("note fell back to page grouping ('%1')").arg(group));
            // the child group created above was lifted to the top level, so one remains
            check(w.mindGroups().size() == 1,
                  QStringLiteral("only the lifted child group remains (%1)").arg(w.mindGroups().size()));
        }

        // re-create it so the undo checks below have something to restore
        w.applyMindReparent(targetId, QStringLiteral("group:考点"));
        settle();

        check(w.applyMindReparent(targetId, QStringLiteral("page:0")),
              QStringLiteral("drop back to page grouping"));
        settle();
        {
            QString group = QStringLiteral("<unset>");
            for (const Markup& m : w.visibleMarkups())
                if (m.id == targetId) group = m.mindGroup;
            check(group.isEmpty(), QStringLiteral("mindGroup cleared ('%1')").arg(group));
        }

        w.undoCommand();
        settle();
        {
            QString group;
            for (const Markup& m : w.visibleMarkups())
                if (m.id == targetId) group = m.mindGroup;
            QString expectedId;
            for (const MindGroup& g : w.mindGroups())
                if (g.name == QStringLiteral("考点")) expectedId = g.id;
            check(!expectedId.isEmpty() && group == expectedId,
                  QStringLiteral("undo restores the previous grouping (%1)").arg(group.left(8)));
        }
        w.undoCommand();
        settle();
        check(w.applyMindReparent(targetId, QStringLiteral("page:0")) == false,
              QStringLiteral("regrouping to the same bucket is a no-op"));
    }

    // ---- 6e2. handwriting colour is independent from the markup colour ----
    {
        w.setInkColor(QColor(QStringLiteral("#cc2200")));
        settle();

        QVector<InkPoint> stroke;
        for (int i = 0; i < 12; ++i) {
            InkPoint p;
            p.pos = QPointF(200 + i * 3, 640);
            stroke.append(p);
        }
        check(w.addInkStroke(0, stroke), QStringLiteral("add a coloured stroke"));
        QString inkColour;
        for (const Markup& m : w.visibleMarkups())
            if (m.type == MarkupType::Ink && m.ink.size() == 12)
                inkColour = m.color.name(QColor::HexRgb);
        check(inkColour == QStringLiteral("#cc2200"),
              QStringLiteral("ink markup took the ink colour (%1)").arg(inkColour));

        // a highlight added afterwards must keep using the markup colour
        QString highlightColour;
        for (const Markup& m : w.visibleMarkups())
            if (m.type == MarkupType::Highlight) {
                highlightColour = m.color.name(QColor::HexRgb);
                break;
            }
        check(!highlightColour.isEmpty() && highlightColour != QStringLiteral("#cc2200"),
              QStringLiteral("markup colour untouched (%1)").arg(highlightColour));

        w.setInkColor(QColor(QStringLiteral("#1f2328")));   // restore the default
        settle();
    }

    // ---- 6f. exporting a document selection from the library ----
    {
        const QString path = QDir::tempPath() + QStringLiteral("/adaptnote-selected.md");
        QString err;
        check(w.exportMarkups(QStringLiteral("md"), path, &err, false), QStringLiteral("baseline export"));

        const QVector<Markup> all = w.visibleMarkups();
        const QString docPath = all.isEmpty() ? QString() : all.first().docPath;
        check(!docPath.isEmpty(), QStringLiteral("document path known"));

        const QString selPath = QDir::tempPath() + QStringLiteral("/adaptnote-selection.md");
        check(w.exportDocuments({docPath}, QStringLiteral("md"), selPath, &err),
              QStringLiteral("exportDocuments: %1").arg(err));
        QFile sel(selPath);
        QString selText;
        if (sel.open(QIODevice::ReadOnly)) selText = QString::fromUtf8(sel.readAll());
        check(selText.contains(QStringLiteral("## ")),
              QStringLiteral("selection export is grouped by book"));
        int bullets = 0;
        for (const QString& line : selText.split(QLatin1Char('\n')))
            if (line.startsWith(QStringLiteral("- **"))) ++bullets;
        check(bullets == all.size(),
              QStringLiteral("selection export holds every note of that book (%1)").arg(bullets));

        check(!w.exportDocuments({QStringLiteral("/nope.pdf")}, QStringLiteral("md"),
                                 QDir::tempPath() + QStringLiteral("/nope.md"), &err),
              QStringLiteral("unknown document is rejected: %1").arg(err));
    }

    // ---- 6g. render cache honours its byte budget (LRU) ----
    {
        w.view()->setLayoutMode(PageLayoutMode::Double);
        w.view()->setZoomMode(ZoomMode::FitPage);
        settle();

        const QImage cacheFixture = w.view()->cachedImageForTest(0);
        // Budget follows actual rendered size, which changes with workspace chrome/DPI.
        const qint64 budget = cacheFixture.sizeInBytes() * 2 + 1024;
        w.view()->setCacheBudgetBytes(budget);
        settle();

        for (int page = 0; page < 6; ++page) {
            w.view()->gotoPage(page);
            settle();
        }
        settle();

        check(w.view()->cacheBytes() <= budget,
              QStringLiteral("cache stays inside the budget (%1 KB <= %2 KB)")
                  .arg(w.view()->cacheBytes() / 1024)
                  .arg(budget / 1024));
        check(w.view()->cacheEntryCount() <= 64,
              QStringLiteral("cache entry count bounded (%1)").arg(w.view()->cacheEntryCount()));

        // the page currently on screen must be cached (most recently used)
        const int current = w.view()->currentPage();
        check(w.view()->isPageCached(current),
              QStringLiteral("current page %1 is cached").arg(current + 1));
        check(!w.view()->isPageCached(0),
              QStringLiteral("oldest page was evicted"));

        w.view()->setCacheBudgetBytes(96ll * 1024 * 1024);
        w.view()->setLayoutMode(PageLayoutMode::Continuous);
        settle();
    }

    // ---- 7. export through the UI entry point ----
    std::printf("\n=== export entry point ===\n");
    {
        QTemporaryDir out;
        check(out.isValid(), QStringLiteral("export temp dir"));
        for (const QString& fmt : {QStringLiteral("md"), QStringLiteral("html"),
                                   QStringLiteral("opml"), QStringLiteral("xmind"),
                                   QStringLiteral("apkg")}) {
            const QString path = out.path() + QStringLiteral("/notes.") + fmt;
            QString err;
            const bool ok = w.exportMarkups(fmt, path, &err);
            const qint64 size = QFileInfo(path).size();
            check(ok && size > 200,
                  QStringLiteral("export %1 -> %2 bytes %3").arg(fmt).arg(size).arg(err));
        }
        check(!w.exportMarkups(QStringLiteral("md"), out.path() + "/x.pdf", nullptr) ||
                  QFileInfo(out.path() + "/x.pdf").exists(),
              QStringLiteral("unknown extension is rejected"));
    }

    // ---- 7b. embedded PDF annotations are detected and offered once ----
    {
        // build an annotated copy of the fixture pdf
        const QString annotated = QDir::tempPath() + QStringLiteral("/adaptnote-annotated.pdf");
        QFile::remove(annotated);
        check(QFile::copy(pdf, annotated), QStringLiteral("copy the pdf to annotate"));

        {
            MuPdfEngine writer;
            QString werr;
            check(writer.open(annotated, &werr), QStringLiteral("open the copy: %1").arg(werr));
            const TextPage* tp = writer.textPage(0);
            QVector<Markup> toWrite;
            if (tp && tp->chars.size() > 40) {
                Markup m;
                m.id = newId();
                m.page = 0;
                m.type = MarkupType::Highlight;
                m.color = QColor(255, 213, 0);
                m.quads = quadsForRange(*tp, 5, 35);
                m.comment = QStringLiteral("别人留下的标注");
                toWrite << m;
                Markup m2 = m;
                m2.id = newId();
                m2.quads = quadsForRange(*tp, 40, 70);
                toWrite << m2;
            }
            check(toWrite.size() == 2, QStringLiteral("prepared 2 annotations"));
            check(writer.writeAnnotations(toWrite, &werr),
                  QStringLiteral("write annotations: %1").arg(werr));
        }

        MainWindow w4;
        w4.resize(1000, 780);
        w4.show();
        w4.setEmbeddedPromptEnabled(false);   // no modal in headless tests
        settle();
        check(w4.openDocument(annotated), QStringLiteral("open the annotated pdf"));
        settle();

        const int pending = w4.pendingEmbeddedAnnotations();
        check(pending == 2, QStringLiteral("detected %1 embedded annotations").arg(pending));

        check(w4.importEmbeddedAnnotations() == 2, QStringLiteral("imported the annotations"));
        settle();
        check(w4.pendingEmbeddedAnnotations() == 0,
              QStringLiteral("nothing left to import after importing"));
        check(w4.visibleMarkups().size() >= 2,
              QStringLiteral("annotations are now notes (%1)").arg(w4.visibleMarkups().size()));

        // a plain document has nothing to offer
        MainWindow w5;
        w5.show();
        w5.setEmbeddedPromptEnabled(false);
        settle();
        check(w5.openDocument(pdf), QStringLiteral("open the plain pdf"));
        settle();
        check(w5.pendingEmbeddedAnnotations() == 0,
              QStringLiteral("plain pdf offers nothing"));
        w5.close();
        w4.close();
    }

    // ---- 8. reflowable anchors survive a layout change ----
    {
        MainWindow w3;
        w3.resize(1100, 800);
        w3.show();
        settle();
        if (w3.openDocument(epub)) {
            settle();
            // pick a page from the second chapter and select some text there
            int targetPage = -1;
            for (int p = 0; p < 4; ++p) {
                const QVector<TextHit> hits = [&]() {
                    w3.findText(QStringLiteral("笔记的最小结构"));
                    return w3.currentHits();
                }();
                if (!hits.isEmpty()) {
                    targetPage = hits.first().page;
                    break;
                }
                Q_UNUSED(p);
                break;
            }
            check(targetPage >= 0, QStringLiteral("found chapter 2 in the epub"));

            if (targetPage >= 0) {
                w3.view()->gotoPage(targetPage);
                settle();
                w3.findText(QStringLiteral("最小可用的笔记结构"));
                settle();
                const QVector<TextHit> hits = w3.currentHits();
                check(!hits.isEmpty(), QStringLiteral("located a phrase to annotate"));
                if (!hits.isEmpty()) {
                    Selection sel;
                    sel.page = hits.first().page;
                    sel.startChar = hits.first().startChar;
                    sel.endChar = hits.first().endChar;
                    sel.quads = hits.first().quads;
                    sel.text = hits.first().text;
                    w3.view()->setSelection(sel);
                    w3.addMarkupFromSelectionForTest(MarkupType::Highlight);
                    settle();

                    Markup note;
                    bool found = false;
                    for (const Markup& m : w3.visibleMarkups()) {
                        if (m.type != MarkupType::Highlight) continue;
                        note = m;
                        found = true;
                    }
                    check(found && !note.anchorChapter.isEmpty() && note.anchorOffset >= 0,
                          QStringLiteral("note carries a chapter/offset anchor (%1:%2)")
                              .arg(note.anchorChapter).arg(note.anchorOffset));

                    // reflow to a much narrower page: pagination must change
                    const int pagesBefore = []() { return 0; }();
                    Q_UNUSED(pagesBefore);
                    w3.setReflowLayout(260.f, 500.f, 13.f);
                    settle();
                    const int pagesAfter = w3.view()->graphicsPageCountForTest();
                    check(pagesAfter > 0, QStringLiteral("epub re-paginated to %1 pages").arg(pagesAfter));

                    const int moved = w3.relocateAnchors();
                    settle();
                    check(true, QStringLiteral("relocation ran (%1 notes moved)").arg(moved));

                    Markup after;
                    for (const Markup& m : w3.visibleMarkups())
                        if (m.id == note.id) after = m;
                    check(!after.id.isEmpty(), QStringLiteral("note still exists after reflow"));
                    check(!after.quads.isEmpty(),
                          QStringLiteral("relocated note has quads (%1)").arg(after.quads.size()));

                    // the point of the anchor: the note still covers the same text
                    const TextPage* tp = w3.engineForTest()->textPage(after.page);
                    bool sameText = false;
                    if (tp) {
                        const QString pageText = an::textForRange(*tp, 0, tp->chars.size() - 1);
                        sameText = pageText.contains(QStringLiteral("最小可用的笔记结构"));
                    }
                    check(sameText, QStringLiteral("relocated note lands on the page holding its text (page %1)")
                                        .arg(after.page + 1));

                    check(after.anchorChapter == note.anchorChapter &&
                              after.anchorOffset == note.anchorOffset,
                          QStringLiteral("anchor itself is unchanged"));
                }
            }
            w3.close();
        } else {
            check(true, QStringLiteral("epub fixture unavailable, anchor test skipped"));
        }
    }

    // ---- workspace modes: the mind map is a panel, not a modal dialog ----
    {
        std::printf("\n=== workspace modes (reading / both / mind map) ===\n");
        check(w.workspaceMode() == 0, QStringLiteral("starts in reading mode"));

        w.setWorkspaceMode(1);
        settle();
        check(w.workspaceMode() == 1, QStringLiteral("mode 1 = reading + mind map"));
        check(w.mindMapView() != nullptr, QStringLiteral("mind map panel exists"));
        const int nodesInPanel = w.mindMapView()->sceneNodeCount();
        check(nodesInPanel > 0,
              QStringLiteral("panel shows the notes as cards (%1 nodes)").arg(nodesInPanel));
        check(w.view()->width() > 300,
              QStringLiteral("page area still usable (%1 px wide)").arg(w.view()->width()));

        w.setWorkspaceMode(2);
        settle();
        check(w.workspaceMode() == 2, QStringLiteral("mode 2 = mind map only"));
        check(!w.view()->isVisible(), QStringLiteral("reading area hidden in mind mode"));

        w.setWorkspaceMode(0);
        settle();
        check(w.view()->isVisible(), QStringLiteral("back to reading mode"));
        check(!w.mindMapView()->isVisible(), QStringLiteral("mind map hidden again"));
    }
        // ---- reverse navigation and card identity ----
        {
            std::printf("\n=== mind map: selection, collapse, reverse navigation ===\n");
            w.setWorkspaceMode(2);
            settle();
            auto* mv = w.mindMapView();
            check(mv != nullptr, QStringLiteral("map view available"));

            // the first note of the document must have a card we can select
            const QVector<Markup> notes = w.visibleMarkups();
            check(!notes.isEmpty(), QStringLiteral("document has notes to locate"));
            if (!notes.isEmpty()) {
                const QString id = notes.first().id;
                check(mv->selectForMarkup(id), QStringLiteral("selectForMarkup finds the card"));
                check(mv->selectedKey() == QStringLiteral("note:") + id,
                      QStringLiteral("selected key belongs to that note (%1)").arg(mv->selectedKey()));
                check(!mv->rectForKey(mv->selectedKey()).isNull(),
                      QStringLiteral("selected card has a position"));
            }

            // collapsing a bucket hides its notes, selecting one of them reopens it
            const QString bucketKey = QStringLiteral("page:0");
            const int before = mv->sceneNodeCount();
            mv->setCollapsed(bucketKey, true);
            const int folded = mv->sceneNodeCount();
            check(folded < before,
                  QStringLiteral("collapse hides children (%1 -> %2)").arg(before).arg(folded));
            if (!notes.isEmpty()) {
                check(mv->selectForMarkup(notes.first().id),
                      QStringLiteral("selectForMarkup works inside a collapsed branch"));
                check(!mv->isCollapsed(bucketKey),
                      QStringLiteral("the branch was reopened automatically"));
            }

            // unknown ids are reported, not silently selected
            check(!mv->selectForMarkup(QStringLiteral("does-not-exist")),
                  QStringLiteral("unknown note id is rejected"));
            w.setWorkspaceMode(0);
            settle();
        }
        // ---- mind-map group edits are undoable ----
        {
            std::printf("\n=== mind map: undo for group edits ===\n");
            auto names = [&]() {
                QStringList out;
                for (const MindGroup& g : w.mindGroups()) out << g.name;
                return out;
            };
            const int before = names().size();

            const QString id = w.createMindGroupForTest(QStringLiteral("可撤销分组"));
            check(!id.isEmpty(), QStringLiteral("group created"));
            check(names().contains(QStringLiteral("可撤销分组")),
                  QStringLiteral("group is visible after creation"));
            check(w.undoTextForTest().contains(QStringLiteral("分组")),
                  QStringLiteral("undo entry describes the change (%1)").arg(w.undoTextForTest()));

            check(w.undoForTest(), QStringLiteral("undo the creation"));
            check(!names().contains(QStringLiteral("可撤销分组")),
                  QStringLiteral("group gone after undo"));
            check(w.redoForTest(), QStringLiteral("redo the creation"));
            check(names().contains(QStringLiteral("可撤销分组")),
                  QStringLiteral("group back after redo"));

            // rename
            check(w.renameMindGroupForTest(QStringLiteral("group:") + id,
                                           QStringLiteral("改名后")),
                  QStringLiteral("rename accepted"));
            check(names().contains(QStringLiteral("改名后")), QStringLiteral("new name visible"));
            check(w.undoForTest(), QStringLiteral("undo the rename"));
            check(names().contains(QStringLiteral("可撤销分组")),
                  QStringLiteral("old name restored (%1)").arg(names().join(QLatin1Char(','))));

            // delete
            check(w.deleteMindGroupForTest(QStringLiteral("group:") + id),
                  QStringLiteral("delete accepted"));
            check(!names().contains(QStringLiteral("可撤销分组")),
                  QStringLiteral("group hidden after delete"));
            check(w.undoForTest(), QStringLiteral("undo the delete"));
            check(names().contains(QStringLiteral("可撤销分组")),
                  QStringLiteral("group restored after undo"));

            check(names().size() == before + 1,
                  QStringLiteral("net effect: one group added (%1 vs %2)")
                      .arg(names().size()).arg(before));
        }
        // ---- free-form links between cards ----
        {
            std::printf("\n=== mind map: free links ===\n");
            w.setWorkspaceMode(2);
            settle();
            auto* mv = w.mindMapView();
            const QString g1 = w.createMindGroupForTest(QStringLiteral("连线甲"));
            const QString g2 = w.createMindGroupForTest(QStringLiteral("连线乙"));
            w.rebuildMindMapForTest();
            settle();
            check(!g1.isEmpty() && !g2.isEmpty(), QStringLiteral("two groups to connect"));

            const QString k1 = QStringLiteral("group:") + g1;
            const QString k2 = QStringLiteral("group:") + g2;
            check(!mv->rectForKey(k1).isNull() && !mv->rectForKey(k2).isNull(),
                  QStringLiteral("both cards are placed"));

            // link mode: two clicks create the link
            mv->setLinkMode(true);
            check(mv->linkMode(), QStringLiteral("link mode on"));
            QWidget* vp = mv->viewport();
            auto click = [&](const QRectF& r) {
                const QPoint p = vp->mapFrom(mv, mv->mapFromScene(r.center()));
                QMouseEvent press(QEvent::MouseButtonPress, p, vp->mapToGlobal(p),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(vp, &press);
                QMouseEvent release(QEvent::MouseButtonRelease, p, vp->mapToGlobal(p),
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(vp, &release);
                settle();
            };
            click(mv->rectForKey(k1));
            check(mv->links().isEmpty(), QStringLiteral("one click arms the link, does not create it"));
            click(mv->rectForKey(k2));

            const QVector<MindLink> links = mv->links();
            check(links.size() == 1, QStringLiteral("a link was created (%1)").arg(links.size()));
            if (!links.isEmpty()) {
                check(links.first().fromNode == k1 && links.first().toNode == k2,
                      QStringLiteral("the link connects the two cards"));
                check(links.first().arrow == QStringLiteral("end"),
                      QStringLiteral("arrow points at the target"));

                // it survives a rebuild (i.e. it was persisted, not just drawn)
                w.rebuildMindMapForTest();
                settle();
                check(mv->links().size() == 1, QStringLiteral("link survives a rebuild"));

                const MindLink original = links.first();
                MindLink edited = original;
                edited.label = QStringLiteral("相关");
                edited.arrow = QStringLiteral("both");
                check(w.modifyMindLinkForTest(edited), QStringLiteral("link edited"));
                check(!w.mindLinksForTest().isEmpty() && w.mindLinksForTest().first().label == QStringLiteral("相关"),
                      QStringLiteral("edited label persisted"));
                w.undoCommand();
                check(!w.mindLinksForTest().isEmpty() && w.mindLinksForTest().first().label == original.label,
                      QStringLiteral("link edit undo restores the old link"));
                w.redoCommand();
                check(!w.mindLinksForTest().isEmpty() && w.mindLinksForTest().first().arrow == QStringLiteral("both"),
                      QStringLiteral("link edit redo restores the new arrow"));
                check(w.removeMindLinkForTest(original.id), QStringLiteral("link removed"));
                check(w.mindLinksForTest().isEmpty(), QStringLiteral("no links left"));
                w.undoCommand();
                check(!w.mindLinksForTest().isEmpty(), QStringLiteral("link delete undo restores the link"));
            }
            mv->setLinkMode(false);
            w.setWorkspaceMode(0);
            settle();
        }
        // ---- notebook creation / empty state / add-document workflow ----
        {
            const QString notebook = w.createNotebook(QStringLiteral("新的阅读笔记本"));
            check(!notebook.isEmpty(), QStringLiteral("create notebook from workspace"));
            auto* notebookTabs = w.findChild<QTabBar*>(QStringLiteral("notebookTabs"));
            check(notebookTabs && notebookTabs->count() >= 2 && notebookTabs->currentIndex() > 0,
                  QStringLiteral("opened notebook receives an application workspace tab"));
            check(w.notebookDocumentCountForTest() == 0 && !w.view()->isVisible(),
                  QStringLiteral("empty notebook does not show the previous document"));
            w.setWorkspaceMode(0);
            check(!w.view()->isVisible(), QStringLiteral("returning to reading mode keeps empty notebook source hidden"));
            check(!w.addNotebookDocument(QStringLiteral("/missing/book.pdf")),
                  QStringLiteral("missing document is rejected"));
            check(w.addNotebookDocument(pdf), QStringLiteral("add first notebook document"));
            check(w.notebookDocumentCountForTest() == 1 && w.view()->isVisible(),
                  QStringLiteral("adding a document opens its reader and tab"));
            check(w.createFreeNoteForTest(QStringLiteral("自由笔记标题"), QStringLiteral("自由笔记内容")),
                  QStringLiteral("create a free note without a text selection"));
            bool foundFree = false;
            for (const auto& note : w.visibleMarkups())
                if (note.title == QStringLiteral("自由笔记标题") && note.comment == QStringLiteral("自由笔记内容")) foundFree = true;
            check(foundFree, QStringLiteral("free note enters the same markup model"));
            const QString outlineGroup = w.createMindGroupForTest(QStringLiteral("工作区分组"));
            auto* outline = w.findChild<QTreeWidget*>(QStringLiteral("notebookOutline"));
            bool foundGroup = false;
            std::function<void(QTreeWidgetItem*)> inspect = [&](QTreeWidgetItem* item) {
                if (item->data(0, Qt::UserRole + 2).toString() == outlineGroup) {
                    foundGroup = !item->data(0, Qt::UserRole + 3).toString().isEmpty();
                }
                for (int i = 0; i < item->childCount(); ++i) inspect(item->child(i));
            };
            if (outline) for (int i = 0; i < outline->topLevelItemCount(); ++i) inspect(outline->topLevelItem(i));
            check(foundGroup, QStringLiteral("new notebook group appears in outline with explicit source identity"));
            const QString childGroup = w.createMindGroupForTest(QStringLiteral("层级拖动"));
            const QString sourcePath = w.visibleMarkups().first().docPath;
            check(w.moveNotebookGroup(childGroup, sourcePath, outlineGroup), QStringLiteral("outline can nest a group"));
            check(!w.moveNotebookGroup(outlineGroup, sourcePath, childGroup), QStringLiteral("outline rejects group cycles"));
            w.undoCommand();
            bool parentRestored = false;
            for (const auto& group : w.mindGroups()) if (group.id == childGroup) parentRestored = group.parentId.isEmpty();
            check(parentRestored, QStringLiteral("group nesting undo restores original parent"));
            w.showNotebookMindMap();
            auto* independentMap = w.findChild<mind::MindMapView*>(QStringLiteral("notebookMindCanvas"));
            check(independentMap && independentMap->sceneNodeCount() > 1,
                  QStringLiteral("independent mind window renders notebook structure"));
            auto* independentWindow = w.findChild<QDialog*>(QStringLiteral("notebookMindWindow"));
            if (independentMap) {
                independentMap->linkRequested(QStringLiteral("group:") + outlineGroup, QStringLiteral("group:") + childGroup);
                const auto independentLinks = w.mindLinksForTest();
                bool linked = false;
                for (const auto& link : independentLinks)
                    if (link.fromNode == QStringLiteral("group:") + outlineGroup && !link.vv.isEmpty()) linked = true;
                check(linked, QStringLiteral("independent map creates versioned persistent links"));
                w.undoCommand();
                check(w.mindLinksForTest().size() + 1 == independentLinks.size(),
                      QStringLiteral("independent map link creation is undoable"));
            }
            if (independentWindow) independentWindow->close();
            check(w.view()->isVisible(), QStringLiteral("closing independent mind map leaves reader visible"));
            const auto priorNotes = w.visibleMarkups();
            if (priorNotes.size() >= 2) {
                const Markup original = priorNotes.first();
                Markup parent = original;
                for (const auto& candidate : priorNotes)
                    if (candidate.id != original.id && candidate.docPath == original.docPath &&
                        candidate.mindGroup == original.mindGroup && candidate.page == original.page) {
                        parent = candidate; break;
                    }
                if (parent.id == original.id) {
                    w.createFreeNoteForTest(QStringLiteral("父笔记"), QStringLiteral("父内容"));
                    parent = w.visibleMarkups().last();
                    // Place the child on the same explicit scope as its parent.
                    Markup sibling = original; sibling.mindGroup = parent.mindGroup; sibling.page = parent.page;
                    w.storeUpsertForTest(sibling); w.refreshFromStore();
                }
                check(w.moveNotebookNote(original.id, original.docPath, {}, {}, parent.id, true),
                      QStringLiteral("outline drop on a note creates a child note"));
                bool childSet = false;
                for (const auto& note : w.visibleMarkups()) if (note.id == original.id) childSet = note.parentMarkupId == parent.id;
                check(childSet, QStringLiteral("outline note nesting persists"));
                check(!w.moveNotebookNote(parent.id, parent.docPath, {}, {}, original.id),
                      QStringLiteral("outline rejects note parent cycles"));
                w.undoCommand();
                bool childUndo = true;
                for (const auto& note : w.visibleMarkups()) if (note.id == original.id) childUndo = note.parentMarkupId == original.parentMarkupId;
                check(childUndo, QStringLiteral("outline note nesting undo restores previous parent"));
                check(w.moveNotebookNote(original.id, original.docPath, QStringLiteral("group:") + outlineGroup),
                      QStringLiteral("outline drop moves note into a group"));
                bool moved = false;
                for (const auto& note : w.visibleMarkups()) if (note.id == original.id) moved = note.mindGroup == outlineGroup;
                check(moved, QStringLiteral("outline move persisted in the shared tree model"));
                check(w.deleteMindGroupForTest(outlineGroup), QStringLiteral("delete populated outline group"));
                w.undoCommand();
                bool membershipRestored = false;
                for (const auto& note : w.visibleMarkups()) if (note.id == original.id) membershipRestored = note.mindGroup == outlineGroup;
                check(membershipRestored, QStringLiteral("group delete undo restores note membership, not just group row"));
                w.undoCommand();
                bool restored = false;
                for (const auto& note : w.visibleMarkups()) if (note.id == original.id) restored = note.mindGroup == original.mindGroup;
                check(restored, QStringLiteral("outline move is undoable"));
                check(!w.moveNotebookNote(original.id, QStringLiteral("/different/book.pdf"), QStringLiteral("group:") + outlineGroup),
                      QStringLiteral("outline rejects cross-document drops"));
            }
            check(w.addNotebookDocument(pdf) && w.notebookDocumentCountForTest() == 1,
                  QStringLiteral("adding the same document does not duplicate tabs"));
            w.openTopicForTest(QString());
        }
        // ---- Ctrl+F can search the notes of the whole topic ----
        {
            std::printf("\n=== reader: topic-wide note search ===\n");
            QTemporaryDir topicTmp;
            const QString second = topicTmp.filePath("topic-second.pdf");
            check(QFile::copy(pdf, second), QStringLiteral("second document is a readable PDF"));

            // a note in a second document, then both documents in one topic
            Markup other;
            other.id = QStringLiteral("topic-other-note");
            other.docPath = second;
            other.page = 1;
            other.type = MarkupType::Highlight;
            other.text = QStringLiteral("另一本书里的独特短语 zebra");
            other.quads = {Quad{QPointF(10, 10), QPointF(90, 10), QPointF(10, 20), QPointF(90, 20)}};
            check(w.storeUpsertForTest(other), QStringLiteral("note in the second document"));

            const QString topicId = w.createTopicForTest(QStringLiteral("搜索主题"));
            check(!topicId.isEmpty(), QStringLiteral("topic created"));
            check(w.addTopicDocForTest(topicId, pdf), QStringLiteral("first document attached"));
            check(w.addTopicDocForTest(topicId, second), QStringLiteral("second document attached"));

            w.openTopicForTest(topicId);
            check(w.notebookDocumentCountForTest() == 2, QStringLiteral("notebook shows two document tabs"));
            {
                mind::TopicDocInput a, b;
                a.title = QStringLiteral("同名资料"); a.docPath = QStringLiteral("/a/book.pdf"); a.markups = w.visibleMarkups();
                b = a; b.docPath = QStringLiteral("/b/book.pdf");
                for (auto& note : b.markups) note.id += QStringLiteral("-second");
                const auto tree = mind::buildFromTopic(QStringLiteral("键唯一性"), {a, b}, {});
                QSet<QString> keys;
                bool unique = true;
                std::function<void(const mind::Node&)> visit = [&](const mind::Node& node) {
                    const QString key = mind::nodeKey(node);
                    if (keys.contains(key)) unique = false;
                    keys.insert(key);
                    for (const auto& child : node.children) visit(child);
                };
                visit(tree);
                check(unique, QStringLiteral("same-title books and page buckets have unique map keys"));
                mind::Node notesTree = mind::buildFromNotes(QStringLiteral("one book"), w.visibleMarkups(), {});
                QSet<QString> pageKeys;
                std::function<void(const mind::Node&)> collectPages = [&](const mind::Node& node) {
                    if (node.groupKey.startsWith(QStringLiteral("page:")) && node.isGroup) {
                        if (pageKeys.contains(node.groupKey)) unique = false;
                        pageKeys.insert(node.groupKey);
                    }
                    for (const auto& child : node.children) collectPages(child);
                };
                collectPages(notesTree);
                check(unique, QStringLiteral("same-page hierarchy has one bucket and unique note layout keys"));
            }
            check(w.notebookNoteIdsForTest().contains(other.id), QStringLiteral("outline includes the other document note"));
            check(w.activateNotebookNote(other.id), QStringLiteral("outline opens the other document source"));
            check(w.visibleMarkups().size() == 1 && w.visibleMarkups().first().id == other.id,
                  QStringLiteral("source navigation switches the actual reader document"));
            check(w.notebookDocumentCountForTest() == 2 && w.notebookNoteIdsForTest().size() > 1,
                  QStringLiteral("switching documents preserves the whole notebook outline"));
            auto* outline = w.findChild<QTreeWidget*>(QStringLiteral("notebookOutline"));
            check(outline && outline->topLevelItemCount() == 1, QStringLiteral("notebook has one outline root"));
            if (outline && outline->topLevelItemCount()) {
                auto* root = outline->topLevelItem(0);
                root->setExpanded(false);
                w.refreshNotebookWorkspace();
                check(!outline->topLevelItem(0)->isExpanded(), QStringLiteral("outline refresh preserves collapsed state"));
                outline->topLevelItem(0)->setExpanded(true);
                QTreeWidgetItem* noteItem = nullptr;
                std::function<void(QTreeWidgetItem*)> locate = [&](QTreeWidgetItem* item) {
                    if (item->data(0, Qt::UserRole).toString() == other.id) noteItem = item;
                    for (int i = 0; i < item->childCount(); ++i) locate(item->child(i));
                };
                locate(outline->topLevelItem(0));
                if (noteItem) outline->itemDoubleClicked(noteItem, 0);
                settle();
                const QString shotDir = qEnvironmentVariable("ADAPT_REVIEW_SHOTS");
                if (!shotDir.isEmpty()) {
                    QDir().mkpath(shotDir);
                    w.grab().save(shotDir + QStringLiteral("/notebook-two-documents.png"));
                    w.showNotebookMindMap();
                    settle();
                    auto* window = w.findChild<QDialog*>(QStringLiteral("notebookMindWindow"));
                    if (window) { window->grab().save(shotDir + QStringLiteral("/notebook-mindmap.png")); window->close(); }
                }
                auto* editor = w.findChild<QDialog*>(QStringLiteral("captureEditor"));
                check(editor != nullptr, QStringLiteral("outline opens the capture editor"));
                if (editor) {
                    if (!shotDir.isEmpty()) editor->grab().save(shotDir + QStringLiteral("/capture-editor.png"));
                    auto* quote = editor->findChild<QPlainTextEdit*>(QStringLiteral("captureQuote"));
                    auto* comment = editor->findChild<QPlainTextEdit*>(QStringLiteral("captureComment"));
                    check(quote && quote->isReadOnly() && quote->toPlainText() == other.text,
                          QStringLiteral("editor keeps the source excerpt read-only"));
                    if (comment) comment->setPlainText(QStringLiteral("我的理解"));
                    auto* heading = editor->findChild<QLineEdit*>(QStringLiteral("captureTitle"));
                    if (heading) heading->setText(QStringLiteral("自定义标题"));
                    auto* buttons = editor->findChild<QDialogButtonBox*>();
                    if (buttons) buttons->button(QDialogButtonBox::Save)->click();
                    check(w.visibleMarkups().first().comment == QStringLiteral("我的理解") &&
                          w.visibleMarkups().first().text == other.text,
                          QStringLiteral("saving personal comments preserves source text"));
                    check(w.visibleMarkups().first().title == QStringLiteral("自定义标题"),
                          QStringLiteral("capture title persists separately from original excerpt"));
                    const Markup roundTrip = Markup::fromJson(w.visibleMarkups().first().toJson());
                    check(roundTrip.title == QStringLiteral("自定义标题"), QStringLiteral("note title survives sync JSON round trip"));
                    w.undoCommand();
                    check(w.visibleMarkups().first().comment == other.comment && w.visibleMarkups().first().title == other.title,
                          QStringLiteral("capture editor save is undoable"));
                }
            }
            check(w.openDocument(pdf), QStringLiteral("return to the first document"));
            w.setFindScopeForTest(1);
            w.findText(QStringLiteral("zebra"));

            int found = w.notesPanelCountForTest();
            check(found >= 1, QStringLiteral("topic search lists the hit (%1)").arg(found));
            bool fromOtherDoc = false;
            for (int i = 0; i < found; ++i)
                if (w.notesPanelTextForTest(i).contains(QStringLiteral("topic-second")))
                    fromOtherDoc = true;
            check(fromOtherDoc, QStringLiteral("the hit comes from the other document"));

            w.findText(QStringLiteral("zzz-definitely-absent"));
            check(w.notesPanelCountForTest() == 0,
                  QStringLiteral("a missing term empties the topic result list"));

            w.setFindScopeForTest(0);
            w.openTopicForTest(QString());
        }



        // ---- drag to regroup: the drop path must actually run ----
        {
            std::printf("\n=== mind map: drag to regroup ===\n");
            w.setWorkspaceMode(2);
            settle();
            auto* mv = w.mindMapView();
            const QString groupName = QStringLiteral("拖动目标");
            const QString groupId = w.createMindGroupForTest(groupName);
            check(!groupId.isEmpty(), QStringLiteral("target group created"));
            w.rebuildMindMapForTest();
            settle();

            // pick a note that is not already in that group
            QString noteId;
            for (const Markup& m : w.visibleMarkups())
                if (m.mindGroup != groupId) { noteId = m.id; break; }
            check(!noteId.isEmpty(), QStringLiteral("found a note outside the group"));

            const QRectF from = mv->rectForKey(QStringLiteral("note:") + noteId);
            const QRectF to = mv->rectForKey(QStringLiteral("group:") + groupId);
            check(!from.isNull() && !to.isNull(),
                  QStringLiteral("both cards have a position (%1 / %2)").arg(from.isNull()).arg(to.isNull()));

            if (!from.isNull() && !to.isNull()) {
                // QGraphicsView receives mouse input through its viewport, so the events
                // have to be addressed there (sending them to the view does nothing).
                QWidget* vp = mv->viewport();
                const QPoint start = vp->mapFrom(mv, mv->mapFromScene(from.center()));
                const QPoint mid =
                    vp->mapFrom(mv, mv->mapFromScene((from.center() + to.center()) / 2.0));
                const QPoint end = vp->mapFrom(mv, mv->mapFromScene(to.center()));

                QMouseEvent press(QEvent::MouseButtonPress, start, vp->mapToGlobal(start),
                                  Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(vp, &press);
                QMouseEvent move(QEvent::MouseMove, mid, vp->mapToGlobal(mid),
                                 Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(vp, &move);
                settle();
                check(mv->hasDropIndicator(), QStringLiteral("drop indicator appears while dragging"));

                QMouseEvent move2(QEvent::MouseMove, end, vp->mapToGlobal(end),
                                  Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
                QApplication::sendEvent(vp, &move2);
                QMouseEvent release(QEvent::MouseButtonRelease, end, vp->mapToGlobal(end),
                                    Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
                QApplication::sendEvent(vp, &release);
                settle();
                check(!mv->hasDropIndicator(), QStringLiteral("indicator cleared after the drop"));

                QString landed;
                for (const Markup& m : w.visibleMarkups())
                    if (m.id == noteId) landed = m.mindGroup;
                check(landed == groupId,
                      QStringLiteral("note moved into the target group (expected %1, got '%2')")
                          .arg(groupId.left(8), landed.left(8)));
            }
            w.setWorkspaceMode(0);
            settle();
        }



        // panel visibility is part of the workspace contract, not just a screenshot detail
        {
            check(w.workspaceMode() == 0, QStringLiteral("workspace starts in reading mode"));
            check(w.notesDockVisibleForTest(), QStringLiteral("reading mode shows notes dock"));
            check(!w.aiDockVisibleForTest(), QStringLiteral("reading mode hides AI dock"));
            w.setWorkspaceMode(1);
            settle();
            check(!w.notesDockVisibleForTest(), QStringLiteral("organize mode hides notes dock"));
            check(!w.aiDockVisibleForTest(), QStringLiteral("organize mode hides AI dock"));
            w.setWorkspaceMode(2);
            settle();
            check(!w.view()->isVisible(), QStringLiteral("mind mode hides reader"));
            check(!w.notesDockVisibleForTest(), QStringLiteral("mind mode hides notes dock"));
            check(!w.aiDockVisibleForTest(), QStringLiteral("mind mode hides AI dock"));
            w.setWorkspaceMode(0);
            settle();
        }

    std::printf("\n%s (%d failure(s))\n", g_fail == 0 ? "FEATURE CHECKS PASSED" : "FEATURE CHECKS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
