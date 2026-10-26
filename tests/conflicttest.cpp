// Conflict resolution: pairing, the three actions, and the dialog wiring.
#include "core/store.h"
#include "mind/tree.h"
#include "sync/conflicts.h"
#include "sync/sync_manager.h"
#include "ui/conflictdialog.h"

#include <QApplication>
#include <QDateTime>
#include <QFile>
#include <QTemporaryDir>

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

Markup makeNote(const QString& doc, const QString& id, const QString& text, const QString& vv,
                const QString& device, qint64 updated, const QStringList& tags = {})
{
    Markup m;
    m.id = id;
    m.docPath = doc;
    m.page = 3;
    m.type = MarkupType::Highlight;
    m.quads = {Quad::fromRect(QRectF(40, 40, 200, 16))};
    m.text = text;
    m.comment = text;
    m.tags = tags;
    m.vv = vv;
    m.deviceId = device;
    m.created = updated;
    m.updated = updated;
    return m;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AdaptNote"));
    QCoreApplication::setApplicationName(QStringLiteral("AdaptNoteConflictTest"));

    QTemporaryDir tmp;
    check(tmp.isValid(), QStringLiteral("temp dir"));
    if (!tmp.isValid()) return 1;

    const QString doc = tmp.path() + "/book.pdf";
    { QFile f(doc); f.open(QIODevice::WriteOnly); f.write("pdf"); }

    Store store;
    QString err;
    check(store.open(tmp.path() + "/library.db", &err), QStringLiteral("open store: %1").arg(err));

    // ---- produce a real conflict through the sync merge ----
    const QVector<Markup> local{makeNote(doc, QStringLiteral("n1"), QStringLiteral("本机改的版本"),
                                         QStringLiteral("A:300,B:100"), QStringLiteral("A"), 300)};
    const QVector<Markup> remote{makeNote(doc, QStringLiteral("n1"), QStringLiteral("远端改的版本"),
                                          QStringLiteral("A:100,B:400"), QStringLiteral("B"), 400)};
    const SyncManager::MergeResult merged =
        SyncManager::merge(local, remote, QStringLiteral("T"), 1000);
    check(merged.conflicts == 1 && merged.merged.size() == 2,
          QStringLiteral("merge produced %1 conflict(s) / %2 notes")
              .arg(merged.conflicts).arg(merged.merged.size()));

    // ---- a merge must never persist a parentId cycle (it silently hides groups + notes) ----
    {
        MindGroup a;
        a.id = QStringLiteral("cyc-a");
        a.docPath = doc;
        a.name = QStringLiteral("A");
        check(store.upsertMindGroup(a), QStringLiteral("created group A"));
        MindGroup b;
        b.id = QStringLiteral("cyc-b");
        b.docPath = doc;
        b.name = QStringLiteral("B");
        b.parentId = QStringLiteral("cyc-a");
        check(store.upsertMindGroup(b), QStringLiteral("created group B under A"));
        check(!store.setMindGroupParent(QStringLiteral("cyc-a"), QStringLiteral("cyc-b")),
              QStringLiteral("setMindGroupParent still rejects a cycle"));

        // sync path: upsertMindGroup with a cycling edge (what mergeGroups does)
        MindGroup a2 = a;
        a2.parentId = QStringLiteral("cyc-b");
        check(store.upsertMindGroup(a2), QStringLiteral("upsert of the cycling edge succeeded"));
        bool hoisted = false;
        for (const MindGroup& g : store.mindGroups(doc))
            if (g.id == QStringLiteral("cyc-a") && g.parentId.isEmpty()) hoisted = true;
        check(hoisted, QStringLiteral("cycling edge hoisted to the top level, not stored"));

        // and the tree keeps every group reachable
        Markup note = makeNote(doc, QStringLiteral("cyc-note"), QStringLiteral("在 B 组里"),
                               QStringLiteral("A:10,B:20"), QStringLiteral("A"), 10);
        note.mindGroup = QStringLiteral("cyc-b");
        store.upsertMarkup(store.stampForLocalWrite(note));
        const mind::Node tree =
            mind::buildFromNotes(QStringLiteral("书"), store.markups(doc), store.mindGroups(doc));
        int groupsSeen = 0;
        std::function<void(const mind::Node&)> walk = [&](const mind::Node& n) {
            if (n.page < 0 && !n.groupKey.isEmpty()) ++groupsSeen;   // group nodes carry page -1
            for (const mind::Node& ch : n.children) walk(ch);
        };
        walk(tree);
        check(groupsSeen == 2, QStringLiteral("both groups still on the tree (%1)").arg(groupsSeen));
    }

    for (const Markup& m : merged.merged) store.upsertMarkup(store.stampForLocalWrite(m));

    // ---- pairing ----
    const QVector<conflicts::Pair> pairs = conflicts::find(store.allMarkups());
    check(pairs.size() == 1, QStringLiteral("paired %1 conflict(s)").arg(pairs.size()));
    check(pairs.first().original.id == QStringLiteral("n1") && !pairs.first().copy.id.isEmpty(),
          QStringLiteral("the copy points back at the original"));

    // ---- dialog: shows the pair and resolves it ----
    {
        ConflictDialog dlg(store);
        dlg.show();
        QCoreApplication::processEvents();
        check(dlg.conflictCount() == 1, QStringLiteral("dialog lists %1 conflict").arg(dlg.conflictCount()));
        check(dlg.selectPair(0), QStringLiteral("selected the conflict"));

        const QString expectedText = pairs.first().original.text;
        const QStringList expectedTags = pairs.first().original.tags;

        check(dlg.resolve(conflicts::Resolution::KeepOriginal), QStringLiteral("keep the left version"));
        check(conflicts::find(store.allMarkups()).isEmpty(),
              QStringLiteral("conflict is gone after resolving"));
        const QVector<Markup> left = store.markups(doc);
        check(left.size() == 1 && left.first().text == expectedText,
              QStringLiteral("kept the original text \"%1\" (%2 note(s))")
                  .arg(left.isEmpty() ? QString() : left.first().text)
                  .arg(left.size()));
        check(!left.isEmpty() && !left.first().tags.contains(QStringLiteral("conflict")),
              QStringLiteral("the conflict tag was removed (tags: %1)")
                  .arg(left.isEmpty() ? QString() : left.first().tags.join(QLatin1Char(','))));
        Q_UNUSED(expectedTags);
    }

    // ---- keep both: two independent notes, no conflict tag ----
    {
        store.upsertMarkup(makeNote(doc, QStringLiteral("n2"), QStringLiteral("版本甲"),
                                    QStringLiteral("A:500,B:100"), QStringLiteral("A"), 500));
        store.upsertMarkup(makeNote(doc, QStringLiteral("n2c"), QStringLiteral("版本乙"),
                                    QStringLiteral("A:500,B:600"), QStringLiteral("B"), 600,
                                    {QStringLiteral("conflict")}));
        // the copy must point at the original
        {
            Markup c = store.markups(doc, 3).first();
            for (const Markup& m : store.markups(doc))
                if (m.id == QStringLiteral("n2c")) c = m;
            c.conflictedFrom = QStringLiteral("n2");
            store.upsertMarkup(store.stampForLocalWrite(c));
        }

        ConflictDialog dlg(store);
        dlg.show();
        QCoreApplication::processEvents();
        check(dlg.conflictCount() == 1, QStringLiteral("second conflict listed"));
        check(dlg.resolve(conflicts::Resolution::KeepBoth), QStringLiteral("keep both"));
        check(conflicts::find(store.allMarkups()).isEmpty(), QStringLiteral("no conflicts left"));
        int versions = 0;
        for (const Markup& m : store.markups(doc))
            if (m.text.startsWith(QStringLiteral("版本"))) ++versions;
        check(versions == 2, QStringLiteral("both versions kept as notes (%1)").arg(versions));
    }

    // ---- keep the copy: original takes the copy's content ----
    {
        store.upsertMarkup(makeNote(doc, QStringLiteral("n3"), QStringLiteral("旧内容"),
                                    QStringLiteral("A:700,B:100"), QStringLiteral("A"), 700));
        store.upsertMarkup(makeNote(doc, QStringLiteral("n3c"), QStringLiteral("新内容"),
                                    QStringLiteral("A:700,B:800"), QStringLiteral("B"), 800,
                                    {QStringLiteral("conflict")}));
        Markup c;
        for (const Markup& m : store.markups(doc))
            if (m.id == QStringLiteral("n3c")) c = m;
        c.conflictedFrom = QStringLiteral("n3");
        store.upsertMarkup(store.stampForLocalWrite(c));

        ConflictDialog dlg(store);
        dlg.show();
        QCoreApplication::processEvents();
        check(dlg.resolve(conflicts::Resolution::KeepCopy), QStringLiteral("keep the right version"));
        check(conflicts::find(store.allMarkups()).isEmpty(), QStringLiteral("conflict cleared"));
        QString text;
        for (const Markup& m : store.markups(doc))
            if (m.id == QStringLiteral("n3")) text = m.text;
        check(text == QStringLiteral("新内容"),
              QStringLiteral("the original now carries the copy content (%1)").arg(text));
    }

    std::printf("\n%s (%d failure(s))\n",
                g_fail == 0 ? "CONFLICT CHECKS PASSED" : "CONFLICT CHECKS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
