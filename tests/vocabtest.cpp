// Vocabulary book: five-box schedule, case-insensitive identity, dictionary lookup,
// AI answer parsing and TSV/Anki export. The network path is covered by aitest.
#include "core/store.h"
#include "export/exporter.h"
#include "export/zip.h"
#include "ocr/ocr.h"
#include "sync/sync_manager.h"
#include "sync/webdav_client.h"
#include "vocab/vocab.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QMap>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>

#include <cstdio>
#include <functional>

using namespace an;

static int failures = 0;

static void check(bool ok, const QString& what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) ++failures;
}

static bool waitFor(const std::function<bool()>& condition, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (!condition()) {
        if (timer.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir tmp;
    check(tmp.isValid(), QStringLiteral("temp dir"));

    // ---- pure helpers ----
    check(vocab::normalizeQuery(QStringLiteral("The point of a reading tool"))
              == QStringLiteral("the"),
          QStringLiteral("normalizeQuery takes the first Latin word and lowercases it"));
    check(vocab::normalizeQuery(QStringLiteral("—— epiphany, really?")) == QStringLiteral("epiphany"),
          QStringLiteral("normalizeQuery ignores leading punctuation"));
    check(vocab::normalizeQuery(QStringLiteral("认识论是什么")) == QStringLiteral("认识论是什么"),
          QStringLiteral("normalizeQuery keeps a short CJK term"));
    check(vocab::normalizeQuery(QStringLiteral("   ")).isEmpty(),
          QStringLiteral("normalizeQuery rejects blank input"));

    vocab::Definition parsed;
    parsed.word = QStringLiteral("survive");
    const bool parseOk = vocab::parseAiDefinition(
        QStringLiteral("音标: səˈvaɪv\n释义: v. 幸存；比…活得长\n例句: The structure must survive."),
        &parsed);
    check(parseOk && parsed.phonetic == QStringLiteral("səˈvaɪv") &&
              parsed.meaning.contains(QStringLiteral("幸存")) &&
              parsed.example.contains(QStringLiteral("survive")),
          QStringLiteral("parseAiDefinition reads the three labelled lines"));

    vocab::Definition loose;
    loose.word = QStringLiteral("annotation");
    check(vocab::parseAiDefinition(QStringLiteral("n. 注释；标注"), &loose) &&
              loose.meaning.contains(QStringLiteral("注释")),
          QStringLiteral("parseAiDefinition falls back to the whole answer"));

    check(vocab::gapDaysForBox(0) == 0 && vocab::gapDaysForBox(3) == 4 &&
              vocab::gapDaysForBox(5) == 15 && vocab::gapDaysForBox(99) == 15,
          QStringLiteral("five-box gaps match MN (0/1/2/4/7/15 days)"));

    // ---- local dictionary ----
    const QString dictPath = tmp.path() + QStringLiteral("/ecdict.db");
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                   QStringLiteral("ecdict-seed"));
        db.setDatabaseName(dictPath);
        check(db.open(), QStringLiteral("create a fake ECDICT database"));
        QSqlQuery q(db);
        q.exec(QStringLiteral("create table stardict(word text, phonetic text,"
                              "translation text, definition text)"));
        q.prepare(QStringLiteral("insert into stardict values(?,?,?,?)"));
        q.addBindValue(QStringLiteral("Epistemology"));
        q.addBindValue(QStringLiteral("ɪˌpɪstɪˈmɒlədʒi"));
        q.addBindValue(QStringLiteral("n. 认识论\\nn. 知识论"));
        q.addBindValue(QString());
        q.exec();
        db.close();
    }
    QSqlDatabase::removeDatabase(QStringLiteral("ecdict-seed"));

    const vocab::Definition local =
        vocab::lookupLocal(dictPath, QStringLiteral("epistemology"));
    check(local.ok && local.source == QStringLiteral("ecdict") &&
              local.meaning.contains(QStringLiteral("认识论")) &&
              !local.meaning.contains(QLatin1Char('\n')),
          QStringLiteral("local dictionary is case-insensitive and flattens newlines"));
    check(!vocab::lookupLocal(tmp.path() + QStringLiteral("/missing.db"),
                              QStringLiteral("word"))
               .ok,
          QStringLiteral("missing dictionary degrades quietly"));

    // ---- store: identity, revive, review ----
    Store store;
    QString err;
    check(store.open(tmp.path() + QStringLiteral("/library.db"), &err),
          QStringLiteral("open library: %1").arg(err));

    WordEntry w;
    w.word = QStringLiteral("Survive");
    w.meaning = QStringLiteral("v. 幸存");
    w.docPath = QStringLiteral("/books/a.pdf");
    w.page = 4;
    check(store.upsertWord(w, &err), QStringLiteral("insert word: %1").arg(err));
    check(store.words().size() == 1, QStringLiteral("one live word"));

    WordEntry duplicate;
    duplicate.word = QStringLiteral("survive");
    check(store.upsertWord(duplicate, &err), QStringLiteral("re-add word"));
    check(store.words().size() == 1, QStringLiteral("same word (any case) is one card"));
    check(store.wordByText(QStringLiteral("SURVIVE")).meaning.contains(QStringLiteral("幸存")),
          QStringLiteral("re-adding does not clobber the existing meaning"));

    const QString id = store.wordByText(QStringLiteral("survive")).id;
    const qint64 now = 1700000000000LL;
    check(store.reviewWord(id, true, now), QStringLiteral("review correct"));
    WordEntry after = store.wordByText(QStringLiteral("survive"));
    check(after.box == 1 && after.dueAt == now + 86400000LL,
          QStringLiteral("correct answer -> box 1, due in one day"));
    for (int i = 0; i < 6; ++i) store.reviewWord(id, true, now);
    check(store.wordByText(QStringLiteral("survive")).box == 5,
          QStringLiteral("box caps at 5"));
    store.reviewWord(id, false, now);
    after = store.wordByText(QStringLiteral("survive"));
    check(after.box == 0 && after.dueAt == now,
          QStringLiteral("wrong answer -> box 0 and due immediately"));

    check(store.removeWord(id), QStringLiteral("remove word"));
    check(store.words().isEmpty() && store.words(true).size() == 1,
          QStringLiteral("removal is a tombstone, not a delete"));
    WordEntry revived;
    revived.word = QStringLiteral("survive");
    revived.meaning = QStringLiteral("v. 幸存");
    check(store.upsertWord(revived, &err) && store.words().size() == 1,
          QStringLiteral("re-adding revives the tombstone"));
    check(store.wordByText(QStringLiteral("survive")).page == 4,
          QStringLiteral("revived card keeps its source anchor"));

    // Two devices independently create the same spelling: the remote id must collapse
    // onto the local card instead of violating the unique word index.
    WordEntry remoteDuplicate;
    remoteDuplicate.id = QStringLiteral("remote-device-id");
    remoteDuplicate.word = QStringLiteral("SURVIVE");
    remoteDuplicate.meaning = QStringLiteral("v. 从…中活下来");
    remoteDuplicate.updated = store.wordByText(QStringLiteral("survive")).updated + 5000;
    check(store.upsertWord(remoteDuplicate, &err, true),
          QStringLiteral("merge a same-spelling card with a foreign id"));
    const WordEntry collapsed = store.wordByText(QStringLiteral("survive"));
    check(store.words().size() == 1 && collapsed.id == id &&
              collapsed.meaning.contains(QStringLiteral("活下来")),
          QStringLiteral("foreign duplicate collapses onto the local id and newer content"));

    WordEntry firstSpelling;
    firstSpelling.word = QStringLiteral("Zeitgeist");
    check(store.upsertWord(firstSpelling, &err), QStringLiteral("add a word with no id"));
    const QString deterministicId = store.wordByText(QStringLiteral("zeitgeist")).id;
    check(deterministicId.startsWith(QLatin1Char('w')) && deterministicId.size() == 17,
          QStringLiteral("new cards get a deterministic id"));
    WordEntry secondSpelling;
    secondSpelling.word = QStringLiteral("zeitgeist");
    store.upsertWord(secondSpelling, &err);
    check(store.words().size() == 2 &&
              store.wordByText(QStringLiteral("ZEITGEIST")).id == deterministicId,
          QStringLiteral("same spelling always maps to the same card"));

    // ---- SigV4 against the AWS get-vanilla test-suite vector ----
    {
        QMap<QString, QString> headers;
        headers.insert(QStringLiteral("host"), QStringLiteral("example.amazonaws.com"));
        headers.insert(QStringLiteral("x-amz-date"), QStringLiteral("20150830T123600Z"));
        const QByteArray auth = WebDavClient::awsV4Authorization(
            QByteArrayLiteral("GET"), QStringLiteral("/"), QString(), headers, QByteArray(),
            QStringLiteral("20150830T123600Z"), QStringLiteral("us-east-1"),
            QStringLiteral("service"), QStringLiteral("AKIDEXAMPLE"),
            QStringLiteral("wJalrXUtnFEMI/K7MDENG+bPxRfiCYEXAMPLEKEY"));
        check(auth.contains("Signature=5fa00fa31553b73ebf1942676e86291e8372ff2a2260956d9b8aae1d763fbf31"),
              QStringLiteral("SigV4 matches the AWS get-vanilla vector: %1")
                  .arg(QString::fromLatin1(auth)));
    }

    // ---- local-folder sync transport (same interface as WebDAV) ----
    WebDavClient dav;
    dav.setLocalRoot(tmp.path() + QStringLiteral("/sync"));
    check(dav.isLocal() && dav.mkcol(QStringLiteral("adaptnote"), &err),
          QStringLiteral("local sync creates its root"));
    const QByteArray payload = QByteArrayLiteral("{\"schema\":1}");
    check(dav.put(QStringLiteral("adaptnote/a.json"), payload, QString(), &err),
          QStringLiteral("local sync put: %1").arg(err));
    QByteArray back;
    QString etag;
    bool notFound = false;
    check(dav.get(QStringLiteral("adaptnote/a.json"), &back, &etag, &err, &notFound) &&
              back == payload && !etag.isEmpty() && !notFound,
          QStringLiteral("local sync get returns bytes and an etag"));
    bool precondition = false;
    check(!dav.put(QStringLiteral("adaptnote/a.json"), payload, QStringLiteral("stale"),
                   &err, &precondition) &&
              precondition,
          QStringLiteral("local sync honours If-Match"));
    check(dav.put(QStringLiteral("adaptnote/a.json"), payload, etag, &err),
          QStringLiteral("local sync accepts the current etag"));
    QVector<DavEntry> entries;
    check(dav.list(QStringLiteral("adaptnote"), &entries, &err) && entries.size() == 1 &&
              !entries[0].isCollection && entries[0].name == QStringLiteral("a.json"),
          QStringLiteral("local sync lists snapshot files"));
    check(dav.remove(QStringLiteral("adaptnote/a.json"), &err),
          QStringLiteral("local sync removes files"));
    check(!dav.get(QStringLiteral("adaptnote/a.json"), &back, &etag, &err, &notFound) &&
              notFound,
          QStringLiteral("missing local snapshot reports 404"));

    // ---- full local-folder sync between two libraries ----
    QTemporaryDir syncRoot;
    const QString bookPath = tmp.path() + QStringLiteral("/book.pdf");
    const QString bookPathB = tmp.path() + QStringLiteral("/library-copy.pdf");
    {
        const QByteArray bytes = QByteArrayLiteral("%PDF-1.4 fake but stable");
        QFile book(bookPath);
        book.open(QIODevice::WriteOnly);
        book.write(bytes);
        book.close();
        QFile copy(bookPathB);
        copy.open(QIODevice::WriteOnly);
        copy.write(bytes);
    }
    Store libraryA, libraryB;
    check(libraryA.open(tmp.path() + QStringLiteral("/a.db"), &err) &&
              libraryB.open(tmp.path() + QStringLiteral("/b.db"), &err),
          QStringLiteral("open two libraries for local sync"));
    Markup note;
    note.id = newId();
    note.docPath = bookPath;
    note.page = 2;
    note.type = MarkupType::Highlight;
    note.quads = {Quad::fromRect(QRectF(10, 20, 100, 14))};
    note.text = QStringLiteral("local folder sync works");
    note.created = note.updated = 1700000000000LL;
    check(libraryA.upsertMarkup(note), QStringLiteral("seed note in A"));
    // Both devices know the book locally; identity is its content hash, so paths may differ.
    libraryA.touchDocument(bookPath, QStringLiteral("book A"), 3);
    libraryB.touchDocument(bookPathB, QStringLiteral("book B"), 3);

    SyncSettings localSync;
    localSync.backend = QStringLiteral("folder");
    localSync.url = syncRoot.path();
    localSync.remotePath = QStringLiteral("adaptnote");
    SyncManager managerA(libraryA), managerB(libraryB);
    managerA.setSettings(localSync);
    managerB.setSettings(localSync);
    const SyncReport push = managerA.syncAll();
    check(push.ok && push.pushed > 0, QStringLiteral("A pushes a snapshot: %1").arg(push.error));
    const SyncReport pull = managerB.syncAll();
    check(pull.ok && pull.pulled > 0, QStringLiteral("B pulls the snapshot: %1").arg(pull.error));
    check(libraryB.markups(bookPathB).size() == 1 &&
              libraryB.markups(bookPathB).first().text == note.text,
          QStringLiteral("B received the note through the local folder"));
    const SyncReport noop = managerB.syncAll();
    check(noop.ok && noop.pushed == 0 && noop.conflicts == 0,
          QStringLiteral("second local sync is a no-op"));

    WordEntry vocabCard;
    vocabCard.word = QStringLiteral("serendipity");
    vocabCard.meaning = QStringLiteral("n. 意外发现");
    vocabCard.docPath = bookPath;
    vocabCard.page = 4;
    check(libraryA.upsertWord(vocabCard, &err) && managerA.syncAll().ok,
          QStringLiteral("A pushes a vocabulary card"));
    const SyncReport vocabPull = managerB.syncAll();
    check(vocabPull.ok && vocabPull.pulled > 0,
          QStringLiteral("B pulls the vocabulary card: %1").arg(vocabPull.error));
    const WordEntry pulledCard = libraryB.wordByText(QStringLiteral("serendipity"));
    check(pulledCard.meaning.contains(QStringLiteral("意外发现")),
          QStringLiteral("vocabulary meaning travelled with the card"));
    check(pulledCard.docPath == bookPathB && pulledCard.page == 4,
          QStringLiteral("vocabulary source anchor resolves on the peer by docKey: %1")
              .arg(pulledCard.docPath));

    check(libraryA.removeWord(pulledCard.id), QStringLiteral("A deletes the vocabulary card"));
    managerA.syncAll();
    managerB.syncAll();
    check(libraryB.words().isEmpty() && libraryB.words(true).size() == 1,
          QStringLiteral("vocabulary tombstone propagates to B"));

    // ---- S3 / MinIO round trip against scripts/fake_s3_server.py ----
    {
        const QString script = argc > 1 ? QString::fromLocal8Bit(argv[1])
                                        : QStringLiteral("../scripts/fake_s3_server.py");
        QProcess server;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("FAKE_S3_ROOT"), tmp.path() + QStringLiteral("/s3root"));
        server.setProcessEnvironment(env);
#ifdef Q_OS_WIN
        server.start(QStringLiteral("python"), {script});
#else
        server.start(QStringLiteral("python3"), {script});
#endif
        QString output;
        const bool listening = server.waitForStarted(5000) &&
                               waitFor([&]() {
                                   output += QString::fromUtf8(server.readAllStandardOutput());
                                   return output.contains(QStringLiteral("PORT "));
                               }, 8000);
        int port = -1;
        const int marker = output.indexOf(QStringLiteral("PORT "));
        if (marker >= 0)
            port = output.mid(marker + 5).section(QLatin1Char('\n'), 0, 0).trimmed().toInt();
        check(listening && port > 0,
              QStringLiteral("fake S3 server starts on port %1 (%2)").arg(port).arg(script));

        if (port > 0) {
            Store s3a, s3b;
            check(s3a.open(tmp.path() + QStringLiteral("/s3a.db"), &err) &&
                      s3b.open(tmp.path() + QStringLiteral("/s3b.db"), &err),
                  QStringLiteral("open two libraries for S3 sync"));
            s3a.touchDocument(bookPath, QStringLiteral("book A"), 3);
            s3b.touchDocument(bookPath, QStringLiteral("book B"), 3);

            Markup s3note = note;
            s3note.id = newId();
            s3note.text = QStringLiteral("s3 sync works");
            check(s3a.upsertMarkup(s3note), QStringLiteral("seed an S3 note"));
            WordEntry s3word;
            s3word.word = QStringLiteral("objectkey");
            s3word.meaning = QStringLiteral("n. 对象键");
            check(s3a.upsertWord(s3word, &err), QStringLiteral("seed an S3 vocabulary card"));

            SyncSettings s3cfg;
            s3cfg.backend = QStringLiteral("s3");
            s3cfg.url = QStringLiteral("http://127.0.0.1:%1").arg(port);
            s3cfg.region = QStringLiteral("us-east-1");
            s3cfg.bucket = QStringLiteral("adaptnote-test");
            s3cfg.user = QStringLiteral("AKIDEXAMPLE");
            s3cfg.password = QStringLiteral("test-secret");
            s3cfg.remotePath = QStringLiteral("adaptnote");
            SyncManager s3managerA(s3a), s3managerB(s3b);
            s3managerA.setSettings(s3cfg);
            s3managerB.setSettings(s3cfg);

            const SyncReport s3push = s3managerA.syncAll();
            check(s3push.ok && s3push.pushed > 0,
                  QStringLiteral("A pushes to S3: %1").arg(s3push.error));
            const SyncReport s3pull = s3managerB.syncAll();
            check(s3pull.ok && s3pull.pulled > 0,
                  QStringLiteral("B pulls from S3: %1").arg(s3pull.error));
            check(s3b.markups(bookPath).size() == 1 &&
                      s3b.markups(bookPath).first().text == s3note.text,
                  QStringLiteral("S3 delivered the note"));
            check(s3b.wordByText(QStringLiteral("objectkey")).meaning.contains(
                      QStringLiteral("对象键")),
                  QStringLiteral("S3 delivered the vocabulary card"));
            const SyncReport s3noop = s3managerB.syncAll();
            check(s3noop.ok && s3noop.pushed == 0 && s3noop.conflicts == 0,
                  QStringLiteral("second S3 sync is a no-op"));
        }
        server.kill();
        server.waitForFinished(3000);
    }

    // ---- OCR seam ----
    check(!ocr::recognize(QImage()).ok, QStringLiteral("OCR rejects an empty image"));
    QImage white(40, 30, QImage::Format_RGB32);
    white.fill(Qt::white);
    check(!ocr::pngBytes(white).isEmpty(), QStringLiteral("OCR encodes PNG bytes for vision fallback"));
#ifndef Q_OS_WIN
    // Fake tesseract: proves the process pipeline (temp PNG, stdout, exit code) without
    // requiring a real install on the CI machine.
    const QString fakeTesseract = tmp.path() + QStringLiteral("/fake-tesseract.sh");
    {
        QFile script(fakeTesseract);
        script.open(QIODevice::WriteOnly);
        script.write("#!/bin/sh\necho 'Recognized text'\n");
        script.close();
        script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                              QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                              QFileDevice::ExeGroup);
    }
    qputenv("ADAPT_TESSERACT", fakeTesseract.toLocal8Bit());
    const ocr::Result fake = ocr::recognize(white);
    qunsetenv("ADAPT_TESSERACT");
    check(fake.ok && fake.text.contains(QStringLiteral("Recognized text")),
          QStringLiteral("OCR pipeline reads tesseract stdout: %1").arg(fake.error));
#endif

    // ---- export ----
    const QString tsv = tmp.path() + QStringLiteral("/words.tsv");
    check(vocab::exportWords(store.words(), tsv, false, &err),
          QStringLiteral("export TSV: %1").arg(err));
    QFile f(tsv);
    check(f.open(QIODevice::ReadOnly), QStringLiteral("read TSV"));
    const QByteArray tsvBytes = f.readAll();
    check(tsvBytes.startsWith("\xEF\xBB\xBF"), QStringLiteral("TSV carries a UTF-8 BOM"));
    check(tsvBytes.contains("单词\t音标\t释义\t例句") &&
              tsvBytes.toLower().contains("survive") && tsvBytes.contains("活下来"),
          QStringLiteral("TSV has header and fields"));

    const QString anki = tmp.path() + QStringLiteral("/words-anki.txt");
    check(vocab::exportWords(store.words(), anki, true, &err),
          QStringLiteral("export Anki"));
    QFile af(anki);
    check(af.open(QIODevice::ReadOnly), QStringLiteral("read Anki file"));
    const QByteArray ankiBytes = af.readAll();
    check(!ankiBytes.contains("单词\t音标"), QStringLiteral("Anki export has no header"));
    check(ankiBytes.toLower().contains("survive\t"), QStringLiteral("Anki export is front-tab-back"));

    // Real Anki package: same collection schema the note exporter uses.
    const QString apkg = tmp.path() + QStringLiteral("/words.apkg");
    check(exports::vocabToAnki(store.words(), apkg, QStringLiteral("AdaptNote Vocab"), &err),
          QStringLiteral("export vocabulary apkg: %1").arg(err));
    {
        ZipReader zr(apkg);
        check(zr.isOpen() && zr.contains(QStringLiteral("collection.anki2")),
              QStringLiteral("vocabulary apkg is a zip with collection.anki2"));
        const QString dbPath = tmp.path() + QStringLiteral("/vocab-collection.anki2");
        {
            QFile f(dbPath);
            f.open(QIODevice::WriteOnly);
            f.write(zr.read(QStringLiteral("collection.anki2")));
        }
        {
            QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"),
                                                       QStringLiteral("vocab-apkg"));
            db.setDatabaseName(dbPath);
            check(db.open(), QStringLiteral("vocabulary apkg collection opens"));
            QSqlQuery q(db);
            q.exec(QStringLiteral("select count(*) from notes"));
            const int notes = q.next() ? q.value(0).toInt() : -1;
            check(notes == store.words().size(),
                  QStringLiteral("vocabulary apkg has one note per word (%1)").arg(notes));
            q.exec(QStringLiteral("select flds from notes limit 1"));
            const QString fields = q.next() ? q.value(0).toString() : QString();
            check(fields.contains(QLatin1Char('\x1f')) && fields.toLower().contains("survive"),
                  QStringLiteral("vocabulary apkg front/back fields are populated"));
            db.close();
        }
        QSqlDatabase::removeDatabase(QStringLiteral("vocab-apkg"));
    }
    check(!exports::vocabToAnki({}, tmp.path() + QStringLiteral("/empty.apkg"), QString(), &err),
          QStringLiteral("empty vocabulary deck is rejected"));

    WordEntry later;
    const qint64 base = 1700000000000LL;
    later.dueAt = base + 2 * 86400000LL;
    check(vocab::dueLabel(later, base) == QStringLiteral("2 天后"),
          QStringLiteral("due label uses day granularity"));
    later.dueAt = base - 1;
    check(vocab::dueLabel(later, base) == QStringLiteral("待复习"),
          QStringLiteral("overdue words are labelled 待复习"));

    std::printf("%s (%d failure(s))\n", failures == 0 ? "VOCAB CHECKS PASSED" : "VOCAB FAILED",
                failures);
    return failures == 0 ? 0 : 1;
}
