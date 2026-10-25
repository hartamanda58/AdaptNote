// Performance bench: generates a large PDF, then measures open / render / text / store /
// search / export / merge. Prints a markdown table so the numbers can go into the README.
#include "core/store.h"
#include "core/textselect.h"
#include "export/exporter.h"
#include "render/mupdf_engine.h"
#include "sync/sync_manager.h"

#include <QCommandLineParser>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdio>

using namespace an;

namespace {

double ms(qint64 us)
{
    return static_cast<double>(us) / 1000.0;
}

QString makePdf(const QString& path, int pages)
{
    QPdfWriter writer(path);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setResolution(72);
    QPainter p(&writer);
    QFont body(QStringLiteral("Noto Sans CJK SC"), 10);
    QFont head(QStringLiteral("Noto Sans CJK SC"), 15, QFont::Bold);

    const QString para =
        QStringLiteral("阅读的本质不是把书看完，而是把书里的东西变成自己的。"
                       "标注、摘录、复述、连接，才是读书真正的工作量所在。"
                       "The point of a reading tool is to capture what you understood "
                       "while you understood it.");

    for (int page = 0; page < pages; ++page) {
        if (page > 0) writer.newPage();
        p.setFont(head);
        p.setPen(QColor(0x1f, 0x23, 0x28));
        p.drawText(QRectF(60, 50, 470, 30), Qt::AlignLeft,
                   QStringLiteral("第 %1 章 · Benchmark").arg(page + 1));
        p.setFont(body);
        double y = 100;
        for (int block = 0; block < 12; ++block) {
            const QRectF box(60, y, 470, 200);
            p.drawText(box, Qt::TextWordWrap,
                       QStringLiteral("%1 [%2.%3]").arg(para).arg(page + 1).arg(block + 1));
            y += 58;
        }
        p.setFont(QFont(QStringLiteral("Noto Sans CJK SC"), 8));
        p.setPen(QColor(0x8a, 0x90, 0x99));
        p.drawText(QRectF(60, 800, 470, 20), Qt::AlignLeft,
                   QStringLiteral("AdaptNote bench · page %1/%2").arg(page + 1).arg(pages));
    }
    p.end();
    return path;
}

struct Row {
    QString name;
    QString value;
    QString note;
};
QVector<Row> rows;

void addRow(const QString& name, const QString& value, const QString& note = {})
{
    rows.append({name, value, note});
    std::printf("%-42s %14s  %s\n", qPrintable(name), qPrintable(value), qPrintable(note));
}

}  // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AdaptNote"));
    QCoreApplication::setApplicationName(QStringLiteral("AdaptNoteBench"));

    QCommandLineParser parser;
    const QCommandLineOption pagesOpt("pages", "pages to generate", "n", "300");
    const QCommandLineOption notesOpt("notes", "notes to insert", "n", "3000");
    const QCommandLineOption keepOpt("keep", "keep the generated pdf in this path", "path");
    const QCommandLineOption fastOpt("fast-import", "relax fsync during the bulk insert");
    const QCommandLineOption repeatOpt("repeat", "repeat the insert N times (each into a longer table)", "n", "1");
    parser.addOption(pagesOpt);
    parser.addOption(notesOpt);
    parser.addOption(keepOpt);
    parser.addOption(fastOpt);
    parser.addOption(repeatOpt);
    parser.process(app);

    const int pages = parser.value(pagesOpt).toInt();
    const int noteTarget = parser.value(notesOpt).toInt();

    QTemporaryDir tmp;
    const QString pdfPath = parser.isSet(keepOpt) ? parser.value(keepOpt)
                                                  : tmp.path() + "/bench.pdf";
    QDir().mkpath(QFileInfo(pdfPath).absolutePath());

    std::printf("\nAdaptNote bench — %d pages, %d notes\n\n", pages, noteTarget);
    std::printf("%-42s %14s  %s\n", "metric", "value", "note");

    // ---------------- generate ----------------
    QElapsedTimer t;
    t.start();
    makePdf(pdfPath, pages);
    addRow(QStringLiteral("generate PDF"), QStringLiteral("%1 ms").arg(ms(t.nsecsElapsed() / 1000)),
           QStringLiteral("%1 MB").arg(QFileInfo(pdfPath).size() / 1024.0 / 1024.0, 0, 'f', 1));

    // ---------------- open + render ----------------
    MuPdfEngine eng;
    QString err;
    t.restart();
    const bool opened = eng.open(pdfPath, &err);
    addRow(QStringLiteral("open document"), QStringLiteral("%1 ms").arg(ms(t.nsecsElapsed() / 1000)),
           opened ? QStringLiteral("%1 pages").arg(eng.pageCount()) : err);
    if (!opened) return 1;

    t.restart();
    const QImage first = eng.renderPage(0, 1.5f, 0);
    addRow(QStringLiteral("render page 1 @1.5x (cold)"),
           QStringLiteral("%1 ms").arg(ms(t.nsecsElapsed() / 1000)),
           QStringLiteral("%1x%2").arg(first.width()).arg(first.height()));

    t.restart();
    for (int i = 0; i < 10; ++i) eng.renderPage(0, 1.5f, 0);
    addRow(QStringLiteral("render same page x10"), QStringLiteral("%1 ms").arg(ms(t.nsecsElapsed() / 1000)),
           QStringLiteral("includes rasterisation each time"));

    t.restart();
    for (int i = 1; i <= 20; ++i) eng.renderPage(i * 3 % eng.pageCount(), 1.5f, 0);
    addRow(QStringLiteral("render 20 different pages"),
           QStringLiteral("%1 ms").arg(ms(t.nsecsElapsed() / 1000)),
           QStringLiteral("%1 ms/page").arg(ms(t.nsecsElapsed() / 1000) / 20.0, 0, 'f', 1));

    // ---------------- text ----------------
    t.restart();
    const TextPage* tp = eng.textPage(0);
    addRow(QStringLiteral("text extraction page 1"),
           QStringLiteral("%1 ms").arg(ms(t.nsecsElapsed() / 1000)),
           QStringLiteral("%1 chars").arg(tp ? tp->chars.size() : 0));

    if (tp && !tp->chars.isEmpty()) {
        t.restart();
        Selection sel;
        for (int i = 0; i < 200; ++i) {
            const int a = (i * 37) % tp->chars.size();
            const int b = qMin(tp->chars.size() - 1, a + 40);
            sel = selectRange(*tp, 0, tp->chars.at(a).origin, tp->chars.at(b).origin);
        }
        addRow(QStringLiteral("selection x200"), QStringLiteral("%1 ms").arg(ms(t.nsecsElapsed() / 1000)),
               QStringLiteral("%1 quads").arg(sel.quads.size()));
    }

    // ---------------- store ----------------
    Store store;
    if (!store.open(tmp.path() + "/bench.db", &err)) {
        std::printf("store: %s\n", qPrintable(err));
        return 1;
    }
    store.touchDocument(pdfPath, QStringLiteral("bench"), eng.pageCount());

    QVector<Markup> notes;
    notes.reserve(noteTarget);
    const TextPage* tpl = tp;
    for (int i = 0; i < noteTarget; ++i) {
        Markup m;
        m.id = newId();
        m.docPath = pdfPath;
        m.page = i % eng.pageCount();
        m.type = MarkupType::Highlight;
        m.color = QColor("#FFD500");
        if (tpl && !tpl->chars.isEmpty()) {
            const int a = (i * 13) % tpl->chars.size();
            const int b = qMin(tpl->chars.size() - 1, a + 30);
            m.quads = quadsForRange(*tpl, a, b);
            m.text = textForRange(*tpl, a, b);
        }
        if (i % 3 == 0) m.tags = {QStringLiteral("方法论"), QStringLiteral("待复习")};
        if (i % 5 == 0) m.comment = QStringLiteral("第 %1 条批注：需要复习").arg(i);
        m.created = m.updated = QDateTime::currentMSecsSinceEpoch();
        notes.append(store.stampForLocalWrite(m));
    }

    const int repeats = qMax(1, parser.value(repeatOpt).toInt());
    auto median = [](QVector<double> v) {
        std::sort(v.begin(), v.end());
        return v.at(v.size() / 2);
    };

    if (parser.isSet(fastOpt)) store.setFastImport(true);

    // Every repetition inserts *fresh* notes: re-writing the same ids would measure
    // updates instead of inserts (and look absurdly fast).
    auto freshNotes = [&](int round) {
        QVector<Markup> copy = notes;
        for (Markup& m : copy) {
            m.id = newId();
            m.created = m.updated = QDateTime::currentMSecsSinceEpoch() + round;
        }
        return copy;
    };

    QVector<double> insertMs;
    QVector<Markup> inserted;   // ids of the last batch, reused for the rewrite test
    bool bulk = true;
    for (int r = 0; r < repeats; ++r) {
        QVector<Markup> batch = freshNotes(r);
        t.restart();
        bulk = bulk && store.upsertMarkups(batch, &err);
        insertMs.append(ms(t.nsecsElapsed() / 1000));
        inserted = batch;
    }
    const double insertMedian = median(insertMs);
    addRow(QStringLiteral("insert %1 notes (batch)").arg(noteTarget),
           QStringLiteral("%1 ms").arg(insertMedian),
           bulk ? QStringLiteral("%1 notes/s  (median of %2, table grows per round)")
                      .arg(noteTarget * 1000.0 / qMax(0.001, insertMedian), 0, 'f', 0)
                      .arg(repeats)
                : err);

    // rewrite the notes that were just inserted: same ids, unchanged text -> no re-index
    QVector<double> rewriteMs;
    for (int r = 0; r < repeats; ++r) {
        t.restart();
        for (const Markup& m : inserted) store.upsertMarkup(m, nullptr, false);
        rewriteMs.append(ms(t.nsecsElapsed() / 1000));
    }
    addRow(QStringLiteral("re-write same %1 notes (FTS skip)").arg(inserted.size()),
           QStringLiteral("%1 ms").arg(median(rewriteMs)),
           QStringLiteral("no text change -> no re-index"));

    if (store.fastImport()) {
        store.setFastImport(false);
        t.restart();
        store.upsertMarkup(notes.first(), nullptr, true);   // force a checkpoint
        addRow(QStringLiteral("checkpoint after fast import"),
               QStringLiteral("%1 ms").arg(ms(t.nsecsElapsed() / 1000)));
    }

    t.restart();
    const QVector<Markup> loaded = store.markups(pdfPath);
    addRow(QStringLiteral("load all notes"), QStringLiteral("%1 ms").arg(ms(t.nsecsElapsed() / 1000)),
           QStringLiteral("%1 rows").arg(loaded.size()));

    t.restart();
    const QVector<Markup> hits = store.searchMarkups(QStringLiteral("方法论"), pdfPath);
    addRow(QStringLiteral("FTS search (tag)"), QStringLiteral("%1 ms").arg(ms(t.nsecsElapsed() / 1000)),
           QStringLiteral("%1 hits").arg(hits.size()));

    t.restart();
    const QVector<Markup> textHits = store.searchMarkups(QStringLiteral("阅读的本质"), pdfPath);
    addRow(QStringLiteral("FTS search (excerpt text)"),
           QStringLiteral("%1 ms").arg(ms(t.nsecsElapsed() / 1000)),
           QStringLiteral("%1 hits").arg(textHits.size()));

    // ---------------- export ----------------
    ExportContext ctx;
    ctx.docPath = pdfPath;
    ctx.docTitle = QStringLiteral("bench");
    ctx.outPath = tmp.path() + "/bench.md";
    ctx.markups = loaded;

    t.restart();
    const bool mdOk = exports::toMarkdown(ctx, &err);
    addRow(QStringLiteral("export markdown"), QStringLiteral("%1 ms").arg(ms(t.nsecsElapsed() / 1000)),
           mdOk ? QStringLiteral("%1 KB").arg(QFileInfo(ctx.outPath).size() / 1024.0, 0, 'f', 0) : err);

    ctx.outPath = tmp.path() + "/bench.xmind";
    t.restart();
    const bool xmOk = exports::toXmind(ctx, &err);
    addRow(QStringLiteral("export xmind"), QStringLiteral("%1 ms").arg(ms(t.nsecsElapsed() / 1000)),
           xmOk ? QStringLiteral("%1 KB").arg(QFileInfo(ctx.outPath).size() / 1024.0, 0, 'f', 0) : err);

    // ---------------- sync payload + merge ----------------
    t.restart();
    const QByteArray payload = SyncManager::documentJson(pdfPath, QStringLiteral("benchkey"), loaded);
    addRow(QStringLiteral("build sync payload"),
           QStringLiteral("%1 ms").arg(ms(t.nsecsElapsed() / 1000)),
           QStringLiteral("%1 KB").arg(payload.size() / 1024.0, 0, 'f', 0));

    QVector<Markup> remote = loaded;
    for (Markup& m : remote) {   // simulate another device having slightly newer copies
        m = store.stampForLocalWrite(m);
        m.comment += QStringLiteral("（远端）");
    }

    t.restart();
    const SyncManager::MergeResult merged =
        SyncManager::merge(loaded, remote, QStringLiteral("bench"), QDateTime::currentMSecsSinceEpoch());
    addRow(QStringLiteral("merge 2x%1 notes").arg(loaded.size()),
           QStringLiteral("%1 ms").arg(ms(t.nsecsElapsed() / 1000)),
           QStringLiteral("%1 merged, %2 conflicts").arg(merged.merged.size()).arg(merged.conflicts));

    std::printf("\n");
    for (const Row& r : rows) {
        std::printf("| %s | %s | %s |\n", qPrintable(r.name), qPrintable(r.value), qPrintable(r.note));
    }
    std::printf("\n");
    return 0;
}
