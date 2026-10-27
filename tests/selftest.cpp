// Headless self-test: engine, selection, storage, PDF write-back, EPUB.
#include "core/paths.h"
#include "core/store.h"
#include "core/textselect.h"
#include "render/mupdf_engine.h"

#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QGuiApplication>
#include <QTemporaryDir>

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

Selection lineSelection(MuPdfEngine& eng, int page, int line)
{
    const TextPage* tp = eng.textPage(page);
    if (!tp) return {};
    int first = -1, last = -1;
    for (int i = 0; i < tp->chars.size(); ++i) {
        if (tp->chars.at(i).line != line) continue;
        if (first < 0) first = i;
        last = i;
    }
    if (first < 0) return {};
    return selectRange(*tp, page, tp->chars.at(first).origin, tp->chars.at(last).origin);
}

int saturatedPixels(const QImage& img)
{
    int n = 0;
    for (int y = 0; y < img.height(); ++y) {
        const QRgb* line = reinterpret_cast<const QRgb*>(img.constScanLine(y));
        for (int x = 0; x < img.width(); ++x)
            if (QColor::fromRgb(line[x]).saturation() > 60) ++n;
    }
    return n;
}

QString firstNonEmptyLine(MuPdfEngine& eng, int page)
{
    const TextPage* tp = eng.textPage(page);
    if (!tp || tp->isEmpty()) return {};
    const int line = tp->chars.first().line;
    QString s;
    for (const CharBox& c : tp->chars) {
        if (c.line != line) break;
        s.append(QChar(static_cast<ushort>(c.code)));
    }
    return s.trimmed();
}

}  // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AdaptNote"));
    QCoreApplication::setApplicationName(QStringLiteral("AdaptNoteTest"));

    const QString pdfPath = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("test.pdf");
    const QString epubPath = argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("test.epub");

    std::printf("\n=== 1. PDF engine ===\n");
    MuPdfEngine eng;
    QString err;
    check(eng.open(pdfPath, &err), QStringLiteral("open %1").arg(pdfPath));
    if (!eng.isOpen()) {
        std::printf("fatal: %s\n", qPrintable(err));
        return 1;
    }
    check(eng.pageCount() == 3, QStringLiteral("page count == 3 (got %1)").arg(eng.pageCount()));
    const QSizeF s = eng.pageSize(0);
    check(qAbs(s.width() - 595) < 2 && qAbs(s.height() - 842) < 2,
          QStringLiteral("A4 page size in points (%1 x %2)").arg(s.width()).arg(s.height()));

    const QImage img = eng.renderPage(1, 1.5f, 0);
    check(!img.isNull() && qAbs(img.width() - 892) <= 4 && qAbs(img.height() - 1263) <= 4,
          QStringLiteral("render page 2 @1.5x -> %1x%2").arg(img.width()).arg(img.height()));

    const QImage rotated = eng.renderPage(0, 1.0f, 90);
    check(!rotated.isNull() && rotated.width() == 842 && rotated.height() == 595,
          QStringLiteral("render rotated 90 -> %1x%2").arg(rotated.width()).arg(rotated.height()));

    const TextPage* tp = eng.textPage(1);
    check(tp && tp->chars.size() > 200,
          QStringLiteral("text extraction on page 2: %1 chars").arg(tp ? tp->chars.size() : -1));

    std::printf("\n=== 2. text selection ===\n");
    const Selection whole = lineSelection(eng, 1, 0);
    check(whole.isValid(), QStringLiteral("selection on page 2 line 0 -> %1 quads").arg(whole.quads.size()));
    check(!whole.text.isEmpty(), QStringLiteral("selection text = \"%1\"").arg(whole.text.left(40)));

    // two-column page: selecting inside the left column must not reach the right one
    const TextPage* tp2 = eng.textPage(1);
    double maxX = 0;
    for (const CharBox& c : tp2->chars)
        if (c.line == 4) maxX = qMax(maxX, c.quad.boundingRect().right());
    check(maxX > 0 && maxX < 300, QStringLiteral("left-column line stays inside column (right edge %1)").arg(maxX));

    std::printf("\n=== 3. store (sqlite + json mirror) ===\n");
    QTemporaryDir tmp;
    check(tmp.isValid(), QStringLiteral("temp dir %1").arg(tmp.path()));
    Store store;
    check(store.open(tmp.filePath("library.db"), &err), QStringLiteral("open store: %1").arg(err));

    Markup m;
    m.id = newId();
    m.docPath = QFileInfo(pdfPath).absoluteFilePath();
    m.page = 1;
    m.type = MarkupType::Highlight;
    m.color = QColor("#FFD500");
    m.quads = whole.quads;
    m.text = whole.text;
    m.comment = QStringLiteral("自测批注");
    m.created = m.updated = QDateTime::currentMSecsSinceEpoch();

    check(store.upsertMarkup(m, &err), QStringLiteral("insert markup: %1").arg(err));
    store.touchDocument(m.docPath, "test", eng.pageCount());

    const QVector<Markup> reloaded = store.markups(m.docPath);
    check(reloaded.size() == 1, QStringLiteral("markup count after reload = %1").arg(reloaded.size()));
    if (!reloaded.isEmpty()) {
        const Markup& r = reloaded.first();
        check(r.quads.size() == m.quads.size() && r.type == m.type && r.comment == m.comment,
              QStringLiteral("round-trip: %1 quads, type %2").arg(r.quads.size()).arg(markupTypeName(r.type)));
        const QRectF a = r.quads.first().boundingRect();
        const QRectF b = m.quads.first().boundingRect();
        check(qAbs(a.x() - b.x()) < 0.01 && qAbs(a.width() - b.width()) < 0.01,
              QStringLiteral("quad geometry preserved (%1,%2 %3x%4)").arg(a.x()).arg(a.y()).arg(a.width()).arg(a.height()));
    }
    const QString mirror = store.mirrorPath(m.docPath);
    check(QFile::exists(mirror), QStringLiteral("json mirror written: %1").arg(mirror));

    std::printf("\n=== 3b. tags / full text search / reading state ===\n");
    Markup tagged = m;  // reuse quads/page of the first markup
    tagged.id = newId();
    tagged.tags = {QStringLiteral("方法论"), QStringLiteral("待复习")};
    tagged.text = QStringLiteral("坐标必须与视图解耦");
    tagged.comment = QStringLiteral("解耦是这一切的前提");
    check(store.upsertMarkup(tagged, &err), QStringLiteral("insert tagged markup: %1").arg(err));

    const QVector<QPair<QString, int>> counts = store.tagCounts(m.docPath);
    check(counts.size() == 2, QStringLiteral("tag count entries = %1").arg(counts.size()));
    bool hasMethod = false, hasReview = false;
    for (const auto& t : counts) {
        if (t.first == QStringLiteral("方法论")) hasMethod = true;
        if (t.first == QStringLiteral("待复习")) hasReview = true;
    }
    check(hasMethod && hasReview, QStringLiteral("both tags aggregated (方法论 / 待复习)"));

    check(store.fullTextSearchEnabled(), QStringLiteral("FTS5 available (trigram tokenizer)"));

    QVector<Markup> hits = store.searchMarkups(QStringLiteral("视图解耦"), m.docPath);
    check(hits.size() == 1, QStringLiteral("FTS search '视图解耦' -> %1 hit(s)").arg(hits.size()));
    hits = store.searchMarkups(QStringLiteral("方法论"), m.docPath);
    check(hits.size() == 1, QStringLiteral("search by tag -> %1 hit(s)").arg(hits.size()));
    hits = store.searchMarkups(QStringLiteral("自测"), m.docPath);
    check(hits.size() == 1, QStringLiteral("search by comment -> %1 hit(s)").arg(hits.size()));
    hits = store.searchMarkups(QStringLiteral("方法论"), QStringLiteral("/nonexistent.pdf"));
    check(hits.isEmpty(), QStringLiteral("doc scope filter excludes other documents"));
    check(store.allMarkups().size() == 2, QStringLiteral("library-wide listing = %1").arg(store.allMarkups().size()));

    store.setReadingState(m.docPath, 2, 0.42);
    check(store.lastPage(m.docPath) == 2, QStringLiteral("reading page stored = %1").arg(store.lastPage(m.docPath)));
    check(qAbs(store.lastOffset(m.docPath) - 0.42) < 1e-6,
          QStringLiteral("reading offset stored = %1").arg(store.lastOffset(m.docPath)));

    // migration from the v0.1 schema (no tags / lastOffset columns)
    {
        const QString oldDb = tmp.filePath("old.db");
        {
            QSqlDatabase legacy = QSqlDatabase::addDatabase("QSQLITE", "legacy");
            legacy.setDatabaseName(oldDb);
            legacy.open();
            QSqlQuery q(legacy);
            q.exec("create table documents (path text primary key, title text, pages integer,"
                   " lastPage integer, openedAt integer)");
            q.exec("create table markups (id text primary key, docPath text, page integer,"
                   " type text, color text, quads text, text text, comment text,"
                   " created integer, updated integer, written integer)");
            legacy.close();
        }
        QSqlDatabase::removeDatabase("legacy");
        Store migrated;
        check(migrated.open(oldDb, &err), QStringLiteral("open v0.1 database: %1").arg(err));
        check(migrated.upsertMarkup(tagged, &err), QStringLiteral("migrated store accepts tags: %1").arg(err));
        check(migrated.markups(m.docPath).size() == 1, QStringLiteral("migrated store round-trip"));
        check(migrated.tagCounts(m.docPath).size() == 2, QStringLiteral("migrated store tags readable"));
    }

    // An index that never saw these notes (a database created before the FTS table existed,
    // or a rebuilt index) must be repaired on open. Otherwise every query of 3+ characters
    // takes the FTS path and silently returns nothing - the LIKE fallback is not consulted.
    {
        const QString dbPath = tmp.filePath("repair.db");
        {
            Store a;
            QString e2;
            check(a.open(dbPath, &e2), QStringLiteral("open index-repair store: %1").arg(e2));
            a.upsertMarkup(m);
            a.upsertMarkup(tagged);
            a.close();   // release the file before poking at it directly
        }

        bool dropped = false;
        {
            QSqlDatabase raw = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                          QStringLiteral("rawdrop"));
            raw.setDatabaseName(dbPath);
            if (raw.open()) {
                QSqlQuery q(raw);
                dropped = q.exec(QStringLiteral("drop table notes_fts"));
            }
            raw.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("rawdrop"));
        check(dropped, QStringLiteral("simulated a database without the FTS index"));

        Store b;
        QString e3;
        check(b.open(dbPath, &e3), QStringLiteral("reopen index-repair store: %1").arg(e3));
        check(b.fullTextSearchEnabled(), QStringLiteral("index table was recreated"));
        const QVector<Markup> again = b.searchMarkups(QStringLiteral("视图解耦"), m.docPath);
        check(again.size() == 1,
              QStringLiteral("notes searchable after the index repair (%1)").arg(again.size()));
        const QVector<Markup> byTag = b.searchMarkups(QStringLiteral("方法论"), m.docPath);
        check(byTag.size() == 1, QStringLiteral("tags re-indexed too (%1)").arg(byTag.size()));
    }

    // data directory resolution: on Windows Qt ignores XDG_DATA_HOME, so a portable build
    // must be steered explicitly or everything lands in %APPDATA%.
    {
        const QString exe = tmp.filePath("portable");
        QDir().mkpath(exe + "/data");
        const QString fallback = QStringLiteral("/fallback/appdata");
        const QString none;

        check(an::resolveDataDir(exe, none, fallback) == QDir::cleanPath(exe + "/data"),
              QStringLiteral("a data/ folder next to the exe wins (%1)")
                  .arg(an::resolveDataDir(exe, none, fallback)));

        check(an::resolveDataDir(exe, QStringLiteral("mydata"), fallback)
                  == QDir::cleanPath(exe + "/mydata"),
              QStringLiteral("relative ADAPT_DATA_DIR resolves against the exe"));
        check(an::resolveDataDir(exe, tmp.filePath("abs"), fallback)
                  == QDir::cleanPath(tmp.filePath("abs")),
              QStringLiteral("absolute ADAPT_DATA_DIR wins over the portable folder"));

        const QString empty = tmp.filePath("no-data-here");
        QDir().mkpath(empty);
        check(an::resolveDataDir(empty, none, fallback) == fallback,
              QStringLiteral("no portable folder -> per-user location"));
    }


    std::printf("\n=== 3c. mind groups (entities + legacy migration) ===\n");
    {
        const QString gid = store.ensureMindGroup(m.docPath, QStringLiteral("考点"));
        check(!gid.isEmpty(), QStringLiteral("ensureMindGroup creates a group (%1)").arg(gid.left(8)));
        check(store.ensureMindGroup(m.docPath, QStringLiteral("考点")) == gid,
              QStringLiteral("ensureMindGroup is idempotent by name"));
        check(store.mindGroups(m.docPath).size() == 1,
              QStringLiteral("one live group listed (%1)").arg(store.mindGroups(m.docPath).size()));

        MindGroup g = store.mindGroups(m.docPath).first();
        MindGroup renamed = g;
        renamed.name = QStringLiteral("高频考点");
        check(store.upsertMindGroup(store.stampGroupForLocalWrite(renamed)),
              QStringLiteral("rename the group"));
        check(store.mindGroups(m.docPath).first().name == QStringLiteral("高频考点"),
              QStringLiteral("rename persisted"));

        // a note points at the group; deleting the group clears the reference
        Markup inGroup = store.markups(m.docPath).value(0);
        inGroup.mindGroup = gid;
        store.upsertMarkup(store.stampForLocalWrite(inGroup));
        check(store.markups(m.docPath).value(0).mindGroup == gid,
              QStringLiteral("note references the group id"));

        check(store.removeMindGroup(gid), QStringLiteral("delete the group"));
        bool tombstoned = false;
        for (const MindGroup& x : store.mindGroupsIncludingDeleted(m.docPath))
            if (x.id == gid && x.deleted) tombstoned = true;
        check(tombstoned, QStringLiteral("group tombstone kept for sync"));
        check(store.mindGroups(m.docPath).isEmpty(), QStringLiteral("deleted group is hidden"));
        check(store.markups(m.docPath).value(0).mindGroup.isEmpty(),
              QStringLiteral("notes fall back to page grouping"));

        // ---- nested groups: parent links, cycle guard, delete reparents children ----
        {
            const QString top = store.ensureMindGroup(m.docPath, QStringLiteral("顶层"));
            const QString child = store.ensureMindGroup(m.docPath, QStringLiteral("子级"), top);
            check(!child.isEmpty(), QStringLiteral("create a child group"));

            QString parentOfChild;
            for (const MindGroup& g : store.mindGroups(m.docPath))
                if (g.id == child) parentOfChild = g.parentId;
            check(parentOfChild == top, QStringLiteral("child points at its parent"));

            check(!store.setMindGroupParent(top, top), QStringLiteral("a group cannot be its own parent"));
            check(store.setMindGroupParent(top, child) == false,
                  QStringLiteral("cycle rejected (child cannot become the parent of its parent)"));

            // deleting the parent lifts the child instead of orphaning it
            check(store.removeMindGroup(top), QStringLiteral("delete the parent group"));
            QString parentAfter;
            bool childAlive = false;
            for (const MindGroup& g : store.mindGroups(m.docPath)) {
                if (g.id != child) continue;
                childAlive = true;
                parentAfter = g.parentId;
            }
            check(childAlive && parentAfter.isEmpty(),
                  QStringLiteral("child survived and moved to the top level"));
        }

        // legacy databases stored the group *name* in markups.mindGroup
        const QString legacyPath = tmp.filePath("legacy-groups.db");
        {
            QSqlDatabase legacy = QSqlDatabase::addDatabase("QSQLITE", "legacy-groups");
            legacy.setDatabaseName(legacyPath);
            legacy.open();
            QSqlQuery q(legacy);
            q.exec("create table documents (path text primary key, title text, pages integer,"
                   " lastPage integer, openedAt integer)");
            q.exec("create table markups (id text primary key, docPath text, page integer,"
                   " type text, color text, quads text, text text, comment text, created integer,"
                   " updated integer, written integer, tags text, imagePath text, "
                   "mindGroup text)");
            q.prepare("insert into markups(id,docPath,page,type,color,quads,text,comment,created,"
                      "updated,written,tags,imagePath,mindGroup) values"
                      "('old-note',? ,0,'highlight','#FFD500','[]','legacy', '', 1,1,0,'','','旧分组')");
            q.addBindValue(m.docPath);
            q.exec();
            legacy.close();
        }
        QSqlDatabase::removeDatabase("legacy-groups");

        Store migrated;
        check(migrated.open(legacyPath, &err), QStringLiteral("open legacy groups db: %1").arg(err));
        const QVector<MindGroup> adopted = migrated.mindGroups(m.docPath);
        check(adopted.size() == 1 && adopted.first().name == QStringLiteral("旧分组"),
              QStringLiteral("legacy name adopted into a group row"));
        const QVector<Markup> notes = migrated.markups(m.docPath);
        check(notes.size() == 1 && notes.first().mindGroup == adopted.value(0).id,
              QStringLiteral("note rewritten to the group id"));
    }

    // ---- reading topics: cross-document organisation ----
    {
        std::printf("\n=== 3d. reading topics (cross-document) ===\n");

        Topic t = store.stampTopicForLocalWrite(Topic{});
        t.name = QStringLiteral("编译器课程");
        t.description = QStringLiteral("一组论文 + 教材");
        check(store.upsertTopic(t), QStringLiteral("create topic"));
        check(!t.id.isEmpty() && !t.vv.isEmpty(), QStringLiteral("topic id and vv assigned"));
        check(store.topics().size() == 1, QStringLiteral("one live topic (%1)").arg(store.topics().size()));

        // a second, different document
        const QString second = tmp.filePath("second.pdf");
        { QFile f(second); f.open(QIODevice::WriteOnly); f.write("pdf"); }

        check(store.addTopicDoc(t.id, m.docPath), QStringLiteral("attach document A"));
        check(store.addTopicDoc(t.id, second), QStringLiteral("attach document B"));
        check(store.topicDocs(t.id).size() == 2,
              QStringLiteral("topic holds two documents (%1)").arg(store.topicDocs(t.id).size()));
        check(store.topicsForDoc(second).contains(t.id),
              QStringLiteral("document B reports the topic"));

        // the same document may belong to another topic
        Topic t2 = store.stampTopicForLocalWrite(Topic{});
        t2.name = QStringLiteral("论文综述");
        check(store.upsertTopic(t2), QStringLiteral("second topic"));
        check(store.addTopicDoc(t2.id, second), QStringLiteral("B also joins the second topic"));
        check(store.topicsForDoc(second).size() == 2,
              QStringLiteral("a document can serve several topics (%1)")
                  .arg(store.topicsForDoc(second).size()));

        // groups get an owner and can be queried per topic
        check(store.setMindGroupTopic(m.docPath, t.id), QStringLiteral("assign groups to topic"));
        const QVector<MindGroup> forTopic = store.mindGroupsForTopic(t.id);
        check(!forTopic.isEmpty(), QStringLiteral("topic exposes its groups (%1)").arg(forTopic.size()));
        for (const MindGroup& g : forTopic)
            check(g.topicId == t.id, QStringLiteral("group carries the topic id"));

        // free-form link between two nodes, independent from the tree. The groups are
        // created here on purpose: this test must not depend on what earlier sections left
        // behind in the shared store.
        const QString linkA = store.ensureMindGroup(m.docPath, QStringLiteral("连线起点"));
        const QString linkB = store.ensureMindGroup(m.docPath, QStringLiteral("连线终点"));
        check(!linkA.isEmpty() && !linkB.isEmpty() && linkA != linkB,
              QStringLiteral("two distinct groups for the link test"));
        {
            MindLink l;
            l.id = newId();
            l.topicId = t.id;
            l.fromNode = linkA;
            l.toNode = linkB;
            l.arrow = QStringLiteral("both");
            l.points = QStringLiteral("[[10,20],[30,40]]");
            l.label = QStringLiteral("相关");
            check(store.upsertMindLink(l), QStringLiteral("create link"));
            const QVector<MindLink> links = store.mindLinks(t.id);
            check(links.size() == 1, QStringLiteral("one link stored (%1)").arg(links.size()));
            check(links.first().arrow == QStringLiteral("both") && !links.first().points.isEmpty(),
                  QStringLiteral("link keeps arrow and control points"));
            check(store.removeMindLink(l.id), QStringLiteral("delete link"));
            check(store.mindLinks(t.id).isEmpty(), QStringLiteral("deleted link is not listed"));
        }

        // detaching a document must not delete the other memberships
        check(store.removeTopicDoc(t.id, second), QStringLiteral("detach B from topic A"));
        check(store.topicDocs(t.id).size() == 1, QStringLiteral("topic A keeps one document"));
        check(store.topicsForDoc(second).size() == 1,
              QStringLiteral("B keeps its other topic (%1)").arg(store.topicsForDoc(second).size()));

        // removing a topic is a tombstone, not a delete
        check(store.removeTopic(t.id), QStringLiteral("remove topic A"));
        check(store.topics().size() == 1, QStringLiteral("only the live topic remains"));
        check(store.topicsIncludingDeleted().size() == 2,
              QStringLiteral("tombstone kept for sync (%1)").arg(store.topicsIncludingDeleted().size()));
    }

    std::printf("\n=== 4. PDF annotation write-back ===\n");
    const QString copy = tmp.filePath("annotated.pdf");
    check(QFile::copy(pdfPath, copy), QStringLiteral("copy for write-back"));
    {
        MuPdfEngine writer;
        check(writer.open(copy, &err), QStringLiteral("open copy: %1").arg(err));
        const int before = writer.annotationCount(0);
        check(before == 0, QStringLiteral("annotation count before = %1").arg(before));

        Markup w = m;
        w.page = 0;
        w.quads = lineSelection(writer, 0, 3).quads;
        w.comment = QStringLiteral("writeback");
        check(writer.writeAnnotations({w}, &err), QStringLiteral("writeAnnotations: %1").arg(err));
        const int after = writer.annotationCount(0);
        check(after == before + 1, QStringLiteral("annotation count after = %1").arg(after));
    }
    {
        MuPdfEngine verify;
        check(verify.open(copy, &err), QStringLiteral("reopen annotated pdf: %1").arg(err));
        check(verify.annotationCount(0) == 1, QStringLiteral("reopened annotation count = %1").arg(verify.annotationCount(0)));
        const TextPage* vp = verify.textPage(0);
        check(vp && !vp->chars.isEmpty(), QStringLiteral("original text still extractable"));

        // The written annotation must also be *visible* when the page is rendered.
        const QImage annotated = verify.renderPage(0, 1.0f, 0);
        const int sat = saturatedPixels(annotated);
        check(sat > 100, QStringLiteral("annotation renders as coloured highlight (%1 saturated px)").arg(sat));
    }

    // ---- handwriting must become a real PDF InkList annotation ----
    std::printf("\n=== 4b. ink annotation write-back ===\n");
    {
        const QString inkCopy = tmp.filePath("ink.pdf");
        check(QFile::copy(pdfPath, inkCopy), QStringLiteral("copy for ink write-back"));
        MuPdfEngine writer;
        check(writer.open(inkCopy, &err), QStringLiteral("open ink copy: %1").arg(err));

        const QSizeF ps = writer.pageSize(0);
        Markup stroke;
        stroke.id = newId();
        stroke.docPath = QFileInfo(inkCopy).absoluteFilePath();
        stroke.page = 0;
        stroke.type = MarkupType::Ink;
        stroke.color = QColor(20, 20, 20);
        for (int i = 0; i < 60; ++i) {
            InkPoint p;
            p.pos = QPointF(80 + i * 3.0, ps.height() * 0.94 + 8 * std::sin(i * 0.4));
            stroke.ink.append(p);
        }
        stroke.quads = {Quad::fromRect(QRectF(80, ps.height() * 0.94, 180, 20))};

        const int before = writer.annotationCount(0);
        const bool wroteInk = writer.writeAnnotations({stroke}, &err);
        // Documented behaviour: ink is intentionally NOT written into the PDF
        // (MuPDF 1.21 cannot generate an appearance stream for /Subtype /Ink, so
        // the stroke would be invisible). It must be refused, not silently lost.
        check(!wroteInk, QStringLiteral("ink write-back is refused (%1)").arg(err));
        check(writer.annotationCount(0) == before,
              QStringLiteral("no ink annot added (count stays %1)").arg(writer.annotationCount(0)));

    }

        std::printf("\n=== 5. EPUB (reflowable) ===\n");
    {
        MuPdfEngine epub;
        check(epub.open(epubPath, &err), QStringLiteral("open %1: %2").arg(epubPath, err));
        if (epub.isOpen()) {
            check(epub.isReflowable(), QStringLiteral("detected as reflowable"));
            check(epub.pageCount() > 0, QStringLiteral("epub pages = %1").arg(epub.pageCount()));
            const QVector<OutlineItem> ol = epub.outline();
            check(!ol.isEmpty(), QStringLiteral("epub outline items = %1").arg(ol.size()));
            if (!ol.isEmpty())
                check(!ol.first().title.isEmpty(),
                      QStringLiteral("first outline entry = \"%1\" (page %2)")
                          .arg(ol.first().title)
                          .arg(ol.first().page));
            const QImage eimg = epub.renderPage(0, 1.0f, 0);
            check(!eimg.isNull(), QStringLiteral("epub render -> %1x%2").arg(eimg.width()).arg(eimg.height()));
            const QString line = firstNonEmptyLine(epub, 0);
            check(!line.isEmpty(), QStringLiteral("epub first line: \"%1\"").arg(line.left(40)));
        }
    }

    std::printf("\n%s  (%d failure(s))\n", g_fail == 0 ? "ALL CHECKS PASSED" : "CHECKS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
