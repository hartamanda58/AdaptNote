// P4 sync test: version-vector merge unit cases + two real devices syncing over
// a real HTTP WebDAV server (scripts/dav_server.py, started as a subprocess).
#include "core/store.h"
#include "core/textselect.h"
#include "render/mupdf_engine.h"
#include "sync/sync_manager.h"
#include "sync/vv.h"
#include "sync/webdav_client.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QThread>

#include <cstdio>

namespace {
// Windows installs expose the interpreter as "python" (or "py"), Unix as "python3".
#ifdef Q_OS_WIN
const char* kPython = "python";
#else
const char* kPython = "python3";
#endif
}  // namespace

using namespace an;

namespace {

int g_fail = 0;

void check(bool ok, const QString& what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) ++g_fail;
}

Markup mk(const QString& id, const QString& text, const QString& vv, const QString& device,
          qint64 updated, bool deleted = false, const QStringList& tags = {})
{
    Markup m;
    m.id = id;
    m.page = 0;
    m.type = MarkupType::Highlight;
    m.quads = {Quad::fromRect(QRectF(10, 10, 100, 12))};
    m.text = text;
    m.comment = text;
    m.tags = tags;
    m.vv = vv;
    m.deviceId = device;
    m.created = updated;
    m.updated = updated;
    m.deleted = deleted;
    return m;
}

int countLive(const QVector<Markup>& v)
{
    int n = 0;
    for (const Markup& m : v)
        if (!m.deleted) ++n;
    return n;
}

bool hasText(const QVector<Markup>& v, const QString& text)
{
    for (const Markup& m : v)
        if (!m.deleted && m.comment.contains(text)) return true;
    return false;
}

Selection lineSel(MuPdfEngine& eng, int page, int line)
{
    const TextPage* tp = eng.textPage(page);
    int first = -1, last = -1;
    if (tp) {
        for (int i = 0; i < tp->chars.size(); ++i) {
            if (tp->chars.at(i).line != line) continue;
            if (first < 0) first = i;
            last = i;
        }
    }
    if (first < 0) return {};
    return selectRange(*tp, page, tp->chars.at(first).origin, tp->chars.at(last).origin);
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AdaptNote"));
    QCoreApplication::setApplicationName(QStringLiteral("AdaptNoteSyncTest"));

    const QString pdf = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("test.pdf");
    const QString serverScript =
        argc > 2 ? QString::fromLocal8Bit(argv[2]) : QStringLiteral("scripts/dav_server.py");

    QTemporaryDir tmp;
    check(tmp.isValid(), QStringLiteral("temp dir %1").arg(tmp.path()));
    if (!tmp.isValid()) return 1;

    // ---------------------------------------------------------------- 1. merge unit cases
    std::printf("\n=== version vector ===\n");
    {
        const VersionVector a = VersionVector::parse(QStringLiteral("A:100,B:50"));
        const VersionVector b = VersionVector::parse(QStringLiteral("A:100,B:60"));
        check(b.dominates(a) && !a.dominates(b), QStringLiteral("B dominates A"));
        check(a.concurrentWith(VersionVector::parse(QStringLiteral("A:101"))),
              QStringLiteral("A:100,B:50 vs A:101 is concurrent"));
        check(!a.concurrentWith(a), QStringLiteral("equal vectors are not concurrent"));
        VersionVector j = a;
        j.join(b);
        check(j.counter(QStringLiteral("A")) == 100 && j.counter(QStringLiteral("B")) == 60,
              QStringLiteral("join takes element-wise max"));
        auto atLeastAsNew = [](const VersionVector& x, const VersionVector& y) {
            return x.dominates(y) || x.equal(y);
        };
        check(atLeastAsNew(j, a) && atLeastAsNew(j, b),
              QStringLiteral("join is at least as new as both inputs"));
        check(VersionVector::parse(a.toString()).equal(a), QStringLiteral("serialize round-trip"));
    }

    std::printf("\n=== merge: dominance / concurrency / tombstones ===\n");
    {
        // remote newer wins
        const QVector<Markup> local{mk("n1", "local", "A:100", "A", 100)};
        const QVector<Markup> remote{mk("n1", "remote", "A:200", "A", 200)};
        auto r = SyncManager::merge(local, remote, QStringLiteral("T"), 1000);
        check(r.merged.size() == 1 && r.merged.first().comment == "remote" && r.pulled == 1,
              QStringLiteral("dominating remote version wins (pulled)"));

        // concurrent edits -> keep both, tag the loser
        const QVector<Markup> l2{mk("n2", "A-edit", "A:300,B:100", "A", 300)};
        const QVector<Markup> r2{mk("n2", "B-edit", "A:100,B:400", "B", 400)};
        auto c = SyncManager::merge(l2, r2, QStringLiteral("T"), 1000);
        check(c.conflicts == 1, QStringLiteral("concurrent edit counted as 1 conflict"));
        check(c.merged.size() == 2, QStringLiteral("both versions kept (%1 entries)").arg(c.merged.size()));
        check(hasText(c.merged, "A-edit") && hasText(c.merged, "B-edit"),
              QStringLiteral("no text lost in conflict"));
        bool tagged = false;
        QString copyId;
        for (const Markup& m : c.merged) {
            if (m.tags.contains(QStringLiteral("conflict"))) tagged = true;
            if (!m.conflictedFrom.isEmpty()) copyId = m.id;
        }
        check(tagged, QStringLiteral("conflict copy tagged 'conflict'"));
        check(!copyId.isEmpty() && copyId != QStringLiteral("n2"),
              QStringLiteral("conflict copy got a new id"));

        // idempotency: merging the merged set with itself must not create conflicts
        auto again = SyncManager::merge(c.merged, c.merged, QStringLiteral("T"), 2000);
        check(again.conflicts == 0 && again.merged.size() == c.merged.size(),
              QStringLiteral("merge is idempotent (%1 entries, %2 conflicts)")
                  .arg(again.merged.size())
                  .arg(again.conflicts));

        // delete vs edit -> deletion stands, edited content preserved as conflict copy
        const QVector<Markup> l3{mk("n3", "gone", "A:500,B:100", "A", 500, true)};
        const QVector<Markup> r3{mk("n3", "edited later", "A:100,B:600", "B", 600)};
        auto d = SyncManager::merge(l3, r3, QStringLiteral("T"), 1000);
        check(d.conflicts == 1, QStringLiteral("delete-vs-edit counted as conflict"));
        check(hasText(d.merged, "edited later"),
              QStringLiteral("edited content survives a competing delete"));
        bool tombstoneKept = false, liveKept = false;
        for (const Markup& m : d.merged) {
            if (m.id != QStringLiteral("n3")) continue;
            if (m.deleted) tombstoneKept = true;
            else liveKept = true;
        }
        check(!tombstoneKept && liveKept,
              QStringLiteral("delete-vs-edit keeps the live note under the original id"));

        // both deleted -> single tombstone, no conflict
        auto e = SyncManager::merge({mk("n4", "x", "A:700", "A", 700, true)},
                                    {mk("n4", "x", "A:800", "A", 800, true)},
                                    QStringLiteral("T"), 1000);
        check(e.conflicts == 0 && e.merged.size() == 1 && e.merged.first().deleted,
              QStringLiteral("both sides deleted -> one tombstone, no conflict"));
    }

    // ---------------------------------------------------------------- 2. start DAV server
    std::printf("\n=== webdav server ===\n");
    const QString davRoot = tmp.path() + "/dav";
    QProcess server;
    server.setProcessChannelMode(QProcess::MergedChannels);
    server.start(QString::fromLatin1(kPython),
                 {serverScript, QStringLiteral("--root"), davRoot,
                  QStringLiteral("--user"), QStringLiteral("bxn"),
                  QStringLiteral("--password"), QStringLiteral("test")});
    if (!server.waitForStarted(5000)) {
        std::printf("fatal: cannot start %s\n", qPrintable(serverScript));
        return 1;
    }
    if (!server.waitForReadyRead(8000)) {
        std::printf("fatal: server did not report READY\n");
        server.kill();
        return 1;
    }
    const QString ready = QString::fromUtf8(server.readAllStandardOutput()).trimmed();
    const QString portStr = ready.section(QLatin1Char(' '), 1, 1);
    check(ready.startsWith(QStringLiteral("READY")) && !portStr.isEmpty(),
          QStringLiteral("server ready: %1").arg(ready));
    const QString baseUrl = QStringLiteral("http://127.0.0.1:%1/dav").arg(portStr);

    // ---------------------------------------------------------------- 3. two devices
    std::printf("\n=== two devices over HTTP ===\n");
    QDir().mkpath(tmp.path() + "/A");
    QDir().mkpath(tmp.path() + "/B");
    const QString pdfA = tmp.path() + "/A/book.pdf";
    const QString pdfB = tmp.path() + "/B/book.pdf";
    check(QFile::copy(pdf, pdfA) && QFile::copy(pdf, pdfB), QStringLiteral("copy the book to both devices"));
    check(SyncManager::docKeyFor(pdfA) == SyncManager::docKeyFor(pdfB),
          QStringLiteral("same book -> same remote key (%1)").arg(SyncManager::docKeyFor(pdfA)));
    {
        const QString twin = tmp.path() + "/A/book-twin.pdf";
        QFile out(twin);
        check(out.open(QIODevice::WriteOnly), QStringLiteral("create a same-size different book"));
        QFile in(pdfA);
        check(in.open(QIODevice::ReadOnly), QStringLiteral("read the original book"));
        QByteArray bytes = in.readAll();
        if (!bytes.isEmpty()) bytes[bytes.size() / 2] = static_cast<char>(bytes.at(bytes.size() / 2) ^ 0x5A);
        out.write(bytes);
        out.close();
        check(QFileInfo(twin).size() == QFileInfo(pdfA).size(), QStringLiteral("twin keeps the same size"));
        check(SyncManager::docKeyFor(twin) != SyncManager::docKeyFor(pdfA),
              QStringLiteral("same name and size but different content stay separate"));
    }

    Store storeA;
    Store storeB;
    QString err;
    check(storeA.open(tmp.path() + "/A/library.db", &err), QStringLiteral("open store A: %1").arg(err));
    check(storeB.open(tmp.path() + "/B/library.db", &err), QStringLiteral("open store B: %1").arg(err));

    MuPdfEngine eng;
    check(eng.open(pdf, &err), QStringLiteral("open pdf for fixtures: %1").arg(err));

    storeA.touchDocument(pdfA, QStringLiteral("book"), eng.pageCount());
    // Opening a book registers it locally; that is what makes syncAll() visit it.
    storeB.touchDocument(pdfB, QStringLiteral("book"), eng.pageCount());

    // seed three notes on A
    QVector<Markup> seeded;
    for (int line = 3; line <= 5; ++line) {
        const Selection s = lineSel(eng, 0, line);
        if (!s.isValid()) continue;
        Markup m;
        m.id = newId();
        m.docPath = pdfA;
        m.page = 0;
        m.type = MarkupType::Highlight;
        m.quads = s.quads;
        m.text = s.text;
        m.comment = QStringLiteral("笔记-%1").arg(line);
        m.created = m.updated = QDateTime::currentMSecsSinceEpoch();
        m = storeA.stampForLocalWrite(m);
        storeA.upsertMarkup(m);
        seeded.append(m);
    }
    check(seeded.size() == 3, QStringLiteral("seeded %1 notes on device A").arg(seeded.size()));

    SyncManager syncA(storeA);
    SyncManager syncB(storeB);
    SyncSettings settings;
    settings.url = baseUrl;
    settings.user = QStringLiteral("bxn");
    settings.password = QStringLiteral("test");
    settings.remotePath = QStringLiteral("adaptnote");
    syncA.setSettings(settings);
    syncB.setSettings(settings);
    check(syncA.settings().configured(), QStringLiteral("settings persisted in the store"));

    auto rep = syncA.syncAll();
    check(rep.ok && rep.pushed == 3, QStringLiteral("A pushes 3 notes (%1)").arg(rep.summary()));

    rep = syncB.syncAll();
    check(rep.ok && rep.pulled == 3, QStringLiteral("B pulls 3 notes (%1)").arg(rep.summary()));
    check(storeB.markups(pdfB).size() == 3, QStringLiteral("B has %1 notes").arg(storeB.markups(pdfB).size()));

    // ---- an image excerpt must travel as a binary payload too ----
    {
        const QImage shot(60, 40, QImage::Format_RGB32);
        QDir().mkpath(storeA.imagesDir());
        const QString imgLocal = storeA.imagesDir() + "/excerpt-test.png";
        QImage canvas = shot;
        canvas.fill(QColor(200, 30, 30));
        check(canvas.save(imgLocal), QStringLiteral("create an excerpt image on A"));

        Markup im;
        im.id = newId();
        im.docPath = pdfA;
        im.page = 0;
        im.type = MarkupType::Image;
        im.imagePath = imgLocal;
        im.quads = {Quad::fromRect(QRectF(50, 50, 60, 40))};
        im.created = im.updated = QDateTime::currentMSecsSinceEpoch();
        storeA.upsertMarkup(storeA.stampForLocalWrite(im));

        rep = syncA.syncAll();
        check(rep.ok, QStringLiteral("A pushes the image note: %1").arg(rep.summary()));
        rep = syncB.syncAll();
        check(rep.ok, QStringLiteral("B pulls: %1").arg(rep.summary()));

        QString remoteName;
        for (const Markup& m : storeB.markups(pdfB))
            if (m.id == im.id) remoteName = m.imagePath;
        check(!remoteName.isEmpty(), QStringLiteral("B received the image markup"));
        check(remoteName.startsWith(storeB.imagesDir()),
              QStringLiteral("B re-attached the image to its own images dir"));
        const QFileInfo remoteInfo(remoteName);
        if (!remoteInfo.exists()) {
            std::printf("      [diag] B images dir listing: ");
            const QStringList names = QDir(storeB.imagesDir()).entryList(QDir::Files);
            for (const QString& n : names) std::printf("%s ", qPrintable(n));
            std::printf("\n");
            WebDavClient probe;
            probe.setBaseUrl(baseUrl);
            probe.setCredentials(QStringLiteral("bxn"), QStringLiteral("test"));
            QVector<DavEntry> entries;
            QString perr;
            const bool ok = probe.list(QStringLiteral("adaptnote/images"), &entries, &perr);
            std::printf("      [diag] remote images list ok=%d err=%s entries=%lld:",
                        ok, qPrintable(perr), (long long)entries.size());
            for (const DavEntry& e : entries) std::printf(" %s", qPrintable(e.name));
            std::printf("\n");
        }
        check(remoteInfo.exists(), QStringLiteral("image file downloaded: %1").arg(remoteInfo.fileName()));

        QFile a(imgLocal);
        QFile b(remoteName);
        const bool aOk = a.open(QIODevice::ReadOnly);
        const bool bOk = b.open(QIODevice::ReadOnly);
        const QByteArray bytesA = aOk ? a.readAll() : QByteArray();
        const QByteArray bytesB = bOk ? b.readAll() : QByteArray();
        check(!bytesA.isEmpty() && bytesA == bytesB,
              QStringLiteral("downloaded image is byte identical (%1 bytes)").arg(bytesA.size()));

        // the wire format must only carry the file name, not a device path
        WebDavClient dav;
        dav.setBaseUrl(baseUrl);
        dav.setCredentials(QStringLiteral("bxn"), QStringLiteral("test"));
        QByteArray docBytes;
        QString etag;
        QString derr;
        bool nf = false;
        if (dav.get(QStringLiteral("adaptnote/%1.json").arg(SyncManager::docKeyFor(pdfA)), &docBytes,
                    &etag, &derr, &nf)) {
            const QVector<Markup> wire = SyncManager::parseDocument(docBytes, &derr);
            QString wirePath;
            for (const Markup& m : wire)
                if (m.id == im.id) wirePath = m.imagePath;
            check(wirePath == remoteInfo.fileName(),
                  QStringLiteral("wire format carries only the file name (%1)").arg(wirePath));
        }
    }

    // ---- concurrent edit of the same note on both devices ----
    const QString targetId = seeded.first().id;
    auto editOn = [](Store& store, const QString& docPath, const QString& id, const QString& comment) {
        for (const Markup& m : store.markups(docPath)) {
            if (m.id != id) continue;
            Markup e = m;
            e.comment = comment;
            store.upsertMarkup(store.stampForLocalWrite(e));
            return true;
        }
        return false;
    };
    check(editOn(storeA, pdfA, targetId, QStringLiteral("A 端改的批注")), QStringLiteral("edit on A"));
    QThread::msleep(5);
    check(editOn(storeB, pdfB, targetId, QStringLiteral("B 端改的批注")), QStringLiteral("edit on B"));

    rep = syncA.syncAll();
    check(rep.ok, QStringLiteral("A syncs after its edit: %1").arg(rep.summary()));
    rep = syncB.syncAll();
    check(rep.ok && rep.conflicts == 1, QStringLiteral("B detects 1 conflict (%1)").arg(rep.summary()));
    rep = syncA.syncAll();
    check(rep.ok && rep.conflicts == 0, QStringLiteral("A converges without new conflicts (%1)").arg(rep.summary()));

    const QVector<Markup> aNotes = storeA.markups(pdfA);
    const QVector<Markup> bNotes = storeB.markups(pdfB);
    check(aNotes.size() == bNotes.size() && aNotes.size() == 5,
          QStringLiteral("both devices converged to %1 notes (A=%2 B=%3)")
              .arg(aNotes.size()).arg(aNotes.size()).arg(bNotes.size()));
    check(hasText(aNotes, QStringLiteral("A 端改的批注")) && hasText(aNotes, QStringLiteral("B 端改的批注")),
          QStringLiteral("A sees both versions after the conflict"));
    check(hasText(bNotes, QStringLiteral("A 端改的批注")) && hasText(bNotes, QStringLiteral("B 端改的批注")),
          QStringLiteral("B sees both versions after the conflict"));

    // ---- delete on A vs edit on B ----
    const QString secondId = seeded.at(1).id;
    check(storeA.removeMarkup(secondId), QStringLiteral("A deletes a note (tombstone)"));
    check(editOn(storeB, pdfB, secondId, QStringLiteral("B 在 A 删除后又编辑了这条")),
          QStringLiteral("B edits the note A deleted"));
    syncA.syncAll();
    rep = syncB.syncAll();
    syncA.syncAll();

    // Semantic: a delete that races a real edit loses (content is never destroyed),
    // the surviving note is kept under its original id and tagged "conflict".
    const QVector<Markup> aAll = storeA.markupsIncludingDeleted(pdfA);
    bool resurrected = false, tombForSecond = false;
    for (const Markup& m : aAll) {
        if (m.id != secondId) continue;
        if (m.deleted) tombForSecond = true;
        else if (m.tags.contains(QStringLiteral("conflict"))) resurrected = true;
    }
    check(!tombForSecond && resurrected,
          QStringLiteral("delete-vs-edit resurrects the note under its id (tagged conflict)"));
    check(hasText(storeA.markups(pdfA), QStringLiteral("B 在 A 删除后又编辑了这条")),
          QStringLiteral("edited text survived the competing delete"));
    check(storeA.markups(pdfA).size() == storeB.markups(pdfB).size(),
          QStringLiteral("devices converged after delete/edit (A=%1 B=%2)")
              .arg(storeA.markups(pdfA).size()).arg(storeB.markups(pdfB).size()));

    // ---- a plain deletion (no concurrent edit) must propagate ----
    const QString thirdId = seeded.at(2).id;
    check(storeA.removeMarkup(thirdId), QStringLiteral("A deletes a note nobody else touched"));
    syncA.syncAll();
    syncB.syncAll();
    syncA.syncAll();

    bool tombA = false;
    for (const Markup& m : storeA.markupsIncludingDeleted(pdfA))
        if (m.id == thirdId && m.deleted) tombA = true;
    check(tombA, QStringLiteral("tombstone kept locally"));
    bool seenOnB = false;
    for (const Markup& m : storeB.markups(pdfB))
        if (m.id == thirdId) seenOnB = true;
    check(!seenOnB, QStringLiteral("deleted note disappeared on device B"));
    check(storeA.markups(pdfA).size() == storeB.markups(pdfB).size(),
          QStringLiteral("converged after plain delete (A=%1 B=%2)")
              .arg(storeA.markups(pdfA).size()).arg(storeB.markups(pdfB).size()));

    // ---- remote file contains tombstones (so deletions propagate) ----
    {
        WebDavClient dav;
        dav.setBaseUrl(baseUrl);
        dav.setCredentials(QStringLiteral("bxn"), QStringLiteral("test"));
        QByteArray bytes;
        QString etag;
        QString gerr;
        bool notFound = false;
        const bool got = dav.get(QStringLiteral("adaptnote/%1.json")
                                     .arg(SyncManager::docKeyFor(pdfA)),
                                 &bytes, &etag, &gerr, &notFound);
        check(got, QStringLiteral("remote document fetchable (%1 bytes)").arg(bytes.size()));
        const QVector<Markup> remote = SyncManager::parseDocument(bytes, &gerr);
        int tombstones = 0;
        for (const Markup& m : remote)
            if (m.deleted) ++tombstones;
        check(tombstones >= 1, QStringLiteral("remote payload carries %1 tombstone(s)").arg(tombstones));

        // ---- If-Match precondition path ----
        QString werr;
        bool precond = false;
        std::printf("      [diag] etag from GET = [%s], payload %d bytes\n", qPrintable(etag), bytes.size());
        {
            QString etag2, gerr2;
            QByteArray bytes2;
            bool nf2 = false;
            dav.get(QStringLiteral("adaptnote/%1.json").arg(SyncManager::docKeyFor(pdfA)),
                    &bytes2, &etag2, &gerr2, &nf2);
            std::printf("      [diag] etag from 2nd GET = [%s]\n", qPrintable(etag2));
        }
        const bool stale = dav.put(QStringLiteral("adaptnote/%1.json").arg(SyncManager::docKeyFor(pdfA)),
                                   QByteArray("{}"), QStringLiteral("\"deadbeef\""), &werr, &precond);
        check(!stale && precond, QStringLiteral("stale If-Match is rejected with 412"));
        bool fresh = dav.put(QStringLiteral("adaptnote/%1.json").arg(SyncManager::docKeyFor(pdfA)),
                             bytes, etag, &werr, &precond);
        if (!fresh) {
            // retry once with a freshly read etag: isolates "stale connection" from "bad etag"
            QString etag3, gerr3;
            QByteArray bytes3;
            bool nf3 = false;
            if (dav.get(QStringLiteral("adaptnote/%1.json").arg(SyncManager::docKeyFor(pdfA)),
                        &bytes3, &etag3, &gerr3, &nf3)) {
                werr.clear();
                fresh = dav.put(QStringLiteral("adaptnote/%1.json")
                                    .arg(SyncManager::docKeyFor(pdfA)),
                                bytes3, etag3, &werr, &precond);
            }
        }
        check(fresh, QStringLiteral("correct If-Match is accepted: %1").arg(werr));
    }

    // ---- mind groups travel and merge like notes ----
    {
        // A creates a group and puts one note in it
        const QString gid = storeA.ensureMindGroup(pdfA, QStringLiteral("考点"));
        check(!gid.isEmpty(), QStringLiteral("A creates a mind group"));

        for (const Markup& m : storeA.markups(pdfA)) {
            Markup e = m;
            e.mindGroup = gid;
            storeA.upsertMarkup(storeA.stampForLocalWrite(e));
            break;
        }

        rep = syncA.syncAll();
        check(rep.ok, QStringLiteral("A syncs the group: %1").arg(rep.summary()));
        rep = syncB.syncAll();
        check(rep.ok, QStringLiteral("B pulls: %1").arg(rep.summary()));

        const QVector<MindGroup> groupsB = storeB.mindGroups(pdfB);
        check(groupsB.size() == 1 && groupsB.first().name == QStringLiteral("考点"),
              QStringLiteral("B received the group (%1)").arg(groupsB.size()));
        bool noteLinked = false;
        for (const Markup& m : storeB.markups(pdfB))
            if (m.mindGroup == gid) noteLinked = true;
        check(noteLinked, QStringLiteral("B's note references the group id"));

        // concurrent rename: newest name wins, no duplicate group
        auto renameOn = [](Store& st, const QString& doc, const QString& id, const QString& name) {
            for (const MindGroup& g : st.mindGroups(doc)) {
                if (g.id != id) continue;
                MindGroup e = g;
                e.name = name;
                return st.upsertMindGroup(st.stampGroupForLocalWrite(e));
            }
            return false;
        };
        check(renameOn(storeA, pdfA, gid, QStringLiteral("A 改的名")), QStringLiteral("A renames"));
        QThread::msleep(5);
        check(renameOn(storeB, pdfB, gid, QStringLiteral("B 改的名")), QStringLiteral("B renames"));

        syncA.syncAll();
        rep = syncB.syncAll();
        check(rep.ok, QStringLiteral("B syncs after the rename race: %1").arg(rep.summary()));
        syncA.syncAll();

        const QVector<MindGroup> afterA = storeA.mindGroups(pdfA);
        const QVector<MindGroup> afterB = storeB.mindGroups(pdfB);
        check(afterA.size() == 1 && afterB.size() == 1,
              QStringLiteral("still exactly one group on each device (A=%1 B=%2)")
                  .arg(afterA.size()).arg(afterB.size()));
        check(!afterA.isEmpty() && !afterB.isEmpty() && afterA.first().name == afterB.first().name,
              QStringLiteral("both devices agree on the name: %1")
                  .arg(afterA.isEmpty() ? QString() : afterA.first().name));

        // a group deletion propagates
        check(storeA.removeMindGroup(gid), QStringLiteral("A deletes the group"));
        syncA.syncAll();
        syncB.syncAll();
        check(storeB.mindGroups(pdfB).isEmpty(),
              QStringLiteral("B no longer lists the group (%1)").arg(storeB.mindGroups(pdfB).size()));
    }


    // ---- topic sync wire/merge core (network integration follows after path resolution) ----
    {
        std::printf("\n=== topic sync wire core ===\n");
        Topic local;
        local.id = QStringLiteral("topic-wire");
        local.name = QStringLiteral("本地主题");
        local.vv = QStringLiteral("A:2");
        local.deviceId = QStringLiteral("A");
        local.updated = 20;
        Topic remote = local;
        remote.name = QStringLiteral("远端主题");
        remote.vv = QStringLiteral("B:3");
        remote.deviceId = QStringLiteral("B");
        remote.updated = 30;

        const auto merged = SyncManager::mergeTopics({local}, {remote}, {}, {}, {}, {}, QStringLiteral("A"), 40);
        check(merged.topics.size() == 1, QStringLiteral("topic merge keeps one identity"));
        check(merged.conflicts == 1, QStringLiteral("concurrent topic edit is counted"));
        check(merged.topics.first().name == QStringLiteral("远端主题"),
              QStringLiteral("newer concurrent topic name is retained"));
        check(!merged.topics.first().vv.isEmpty(), QStringLiteral("merged topic has a joined vv"));

        TopicDoc td;
        td.topicId = local.id;
        td.docPath = QStringLiteral("/home/user/books/a.pdf");
        td.order = 2;
        td.updated = 10;
        td.vv = QStringLiteral("A:1");
        MindLink link;
        link.id = QStringLiteral("wire-link");
        link.topicId = local.id;
        link.fromNode = QStringLiteral("group:a");
        link.toNode = QStringLiteral("note:b");
        link.arrow = QStringLiteral("end");
        link.points = QStringLiteral("[[1,2],[3,4]]");
        const QByteArray wire = SyncManager::topicsJson({local}, {td}, {link});
        check(!wire.contains("/home/user"), QStringLiteral("wire omits absolute local paths"));
        QVector<Topic> parsedTopics;
        QVector<TopicDoc> parsedDocs;
        QVector<MindLink> parsedLinks;
        QString parseError;
        SyncManager::parseTopics(wire, &parsedTopics, &parsedDocs, &parsedLinks, &parseError);
        check(parseError.isEmpty() && parsedTopics.size() == 1 && parsedDocs.size() == 1 &&
                  parsedLinks.size() == 1,
              QStringLiteral("topic bundle round-trips (%1/%2/%3): %4")
                  .arg(parsedTopics.size()).arg(parsedDocs.size()).arg(parsedLinks.size()).arg(parseError));
        check(parsedDocs.first().docPath == QStringLiteral("a.pdf"),
              QStringLiteral("wire topic doc carries only the safe filename"));
    }

    // ---- topic bundle over HTTP ----
    {
        std::printf("\n=== topic bundle sync ===\n");
        Topic topic;
        topic.id = QStringLiteral("topic-sync");
        topic.name = QStringLiteral("同步主题");
        topic.vv = QStringLiteral("A:1");
        topic.deviceId = QStringLiteral("A");
        topic.updated = QDateTime::currentMSecsSinceEpoch();
        check(storeA.upsertTopic(topic), QStringLiteral("A creates a topic"));
        check(storeA.addTopicDoc(topic.id, pdfA), QStringLiteral("A attaches the book"));
        MindLink link;
        link.id = QStringLiteral("sync-link");
        link.topicId = topic.id;
        link.fromNode = QStringLiteral("group:a");
        link.toNode = QStringLiteral("note:b");
        link.arrow = QStringLiteral("end");
        check(storeA.upsertMindLink(link), QStringLiteral("A creates a free-form link"));

        const SyncReport topicPush = syncA.syncAll();
        check(topicPush.ok, QStringLiteral("A pushes the topic bundle: %1").arg(topicPush.error));
        const SyncReport topicPull = syncB.syncAll();
        check(topicPull.ok, QStringLiteral("B pulls the topic bundle: %1").arg(topicPull.error));
        const QVector<Topic> topicsB = storeB.topics();
        bool foundTopic = false;
        for (const Topic& t : topicsB) if (t.id == topic.id && t.name == topic.name) foundTopic = true;
        check(foundTopic, QStringLiteral("B received the topic"));
        bool foundDoc = false;
        for (const TopicDoc& d : storeB.topicDocs(topic.id)) if (d.docPath == pdfB) foundDoc = true;
        check(foundDoc, QStringLiteral("B resolves the synced book by content, not by absolute path"));
        bool foundLink = false;
        for (const MindLink& l : storeB.mindLinks(topic.id)) if (l.id == link.id) foundLink = true;
        check(foundLink, QStringLiteral("B received the free-form link"));
    }

    // ---- idempotency over the wire ----
    rep = syncA.syncAll();
    const int pushedAgain = rep.pushed;
    rep = syncB.syncAll();
    check(rep.ok && rep.conflicts == 0 && pushedAgain == 0,
          QStringLiteral("second round is a no-op (pushed=%1, conflicts=%2)")
              .arg(pushedAgain).arg(rep.conflicts));

    // ---- tombstones do not show up in the UI-facing query ----
    check(countLive(storeA.markups(pdfA)) == storeA.markups(pdfA).size(),
          QStringLiteral("markups() never returns tombstones"));

    server.kill();
    server.waitForFinished(3000);
    const QString serverLog = QString::fromUtf8(server.readAll());
    if (qEnvironmentVariableIsSet("DAV_VERBOSE") && !serverLog.isEmpty())
        std::printf("\n--- server log ---\n%s\n", qPrintable(serverLog));

    std::printf("\n%s (%d failure(s))\n", g_fail == 0 ? "SYNC CHECKS PASSED" : "SYNC CHECKS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
