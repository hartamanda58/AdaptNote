// Import test: our own export must survive a round trip, plus BookxNote-style markdown
// and the JSON mirror.
#include "core/store.h"
#include "core/textselect.h"
#include "export/exporter.h"
#include "import/import.h"
#include "render/mupdf_engine.h"

#include <QFile>
#include <QGuiApplication>
#include <QTextStream>
#include <QTemporaryDir>

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

int countOf(const QVector<Markup>& notes, MarkupType type)
{
    int n = 0;
    for (const Markup& m : notes)
        if (m.type == type) ++n;
    return n;
}

}  // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QString pdf = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("test.pdf");

    QTemporaryDir tmp;
    check(tmp.isValid(), QStringLiteral("temp dir"));
    if (!tmp.isValid()) return 1;

    QString err;
    MuPdfEngine eng;
    check(eng.open(pdf, &err), QStringLiteral("open %1: %2").arg(pdf, err));
    if (!eng.isOpen()) return 1;

    // ---- fixtures: two highlights, one with tags, plus a note with a comment ----
    QVector<Markup> notes;
    const Selection s1 = lineSelection(eng, 0, 3);
    const Selection s2 = lineSelection(eng, 0, 6);
    check(s1.isValid() && s2.isValid(), QStringLiteral("fixtures selected"));

    Markup a;
    a.id = newId();
    a.docPath = QFileInfo(pdf).absoluteFilePath();
    a.page = 0;
    a.type = MarkupType::Highlight;
    a.text = s1.text;
    a.quads = s1.quads;
    a.tags = {QStringLiteral("方法论")};
    Markup b = a;
    b.id = newId();
    b.type = MarkupType::Underline;
    b.text = s2.text;
    b.quads = s2.quads;
    Markup c = a;
    c.id = newId();
    c.type = MarkupType::Note;
    c.text = s2.text;
    c.quads = s2.quads;
    c.comment = QStringLiteral("这里值得抄一遍");
    notes << a << b << c;

    // ---- 1. markdown round trip through our own exporter ----
    ExportContext ctx;
    ctx.docPath = a.docPath;
    ctx.docTitle = QStringLiteral("导入测试书");
    ctx.outPath = tmp.path() + "/roundtrip.md";
    ctx.markups = notes;
    check(exports::toMarkdown(ctx, &err), QStringLiteral("export markdown: %1").arg(err));

    const ImportedDocument back = imports::detect(ctx.outPath, a.docPath, &err);
    check(back.markups.size() == notes.size(),
          QStringLiteral("markdown round trip kept %1/%2 notes")
              .arg(back.markups.size()).arg(notes.size()));
    check(countOf(back.markups, MarkupType::Highlight) == 1 &&
              countOf(back.markups, MarkupType::Underline) == 1 &&
              countOf(back.markups, MarkupType::Note) == 1,
          QStringLiteral("types survived (highlight/underline/note)"));
    {
        bool tagsKept = false;
        bool commentKept = false;
        bool textKept = false;
        for (const Markup& m : back.markups) {
            if (m.tags.contains(QStringLiteral("方法论"))) tagsKept = true;
            if (m.comment.contains(QStringLiteral("值得抄一遍"))) commentKept = true;
            if (m.text.contains(s1.text.left(8))) textKept = true;
        }
        check(tagsKept, QStringLiteral("tags survived"));
        check(commentKept, QStringLiteral("comment survived"));
        check(textKept, QStringLiteral("excerpt text survived"));
    }
    check(back.markups.first().page == 0, QStringLiteral("page number survived"));

    // ---- 2. JSON mirror round trip (lossless) ----
    Store store;
    check(store.open(tmp.path() + "/import.db", &err), QStringLiteral("open store: %1").arg(err));
    store.touchDocument(a.docPath, QStringLiteral("导入测试书"), 3);
    for (const Markup& m : notes) store.upsertMarkup(store.stampForLocalWrite(m));
    const QString mirror = store.mirrorPath(a.docPath);
    check(QFile::exists(mirror), QStringLiteral("json mirror written"));

    const ImportedDocument fromJson = imports::detect(mirror, a.docPath, &err);
    check(fromJson.markups.size() == notes.size(),
          QStringLiteral("json round trip kept %1 notes").arg(fromJson.markups.size()));
    {
        bool quadsKept = false;
        for (const Markup& m : fromJson.markups)
            if (!m.quads.isEmpty()) {
                const QRectF r = m.quads.first().boundingRect();
                if (r.width() > 1.0) quadsKept = true;
            }
        check(quadsKept, QStringLiteral("json keeps the geometry (markdown cannot)"));
    }

    // ---- 3. hand written BookxNote style markdown ----
    {
        const QString path = tmp.path() + "/bookxnote.md";
        QFile f(path);
        f.open(QIODevice::WriteOnly);
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        ts << "# 别人的书\n\n"
              "> 导出自 BookxNote Pro\n\n"
              "## 第 3 页\n\n"
              "- **高亮** · #方法论 #待复习\n"
              "  > 阅读的本质不是把书看完\n"
              "\n"
              "- **批注**\n"
              "  > 标注即思考\n"
              "  - 批注：这条要记住\n"
              "\n"
              "## 第 7 页\n\n"
              "- **下划线**\n"
              "  > 结构先于细节\n";
        ts.flush();
        f.close();

        const ImportedDocument bn = imports::detect(path, QString(), &err);
        check(bn.markups.size() == 3, QStringLiteral("parsed %1 notes from foreign markdown")
                                          .arg(bn.markups.size()));
        check(countOf(bn.markups, MarkupType::Highlight) == 1 &&
                  countOf(bn.markups, MarkupType::Note) == 1 &&
                  countOf(bn.markups, MarkupType::Underline) == 1,
              QStringLiteral("foreign types parsed"));
        int pages = 0;
        QStringList tagsSeen;
        QString commentSeen;
        for (const Markup& m : bn.markups) {
            pages = qMax(pages, m.page);
            tagsSeen += m.tags;
            if (!m.comment.isEmpty()) commentSeen = m.comment;
        }
        check(pages == 6, QStringLiteral("page headings respected (last page %1+1)").arg(pages));
        check(tagsSeen.contains(QStringLiteral("方法论")) &&
                  tagsSeen.contains(QStringLiteral("待复习")),
              QStringLiteral("foreign tags parsed: %1").arg(tagsSeen.join(QLatin1Char(','))));
        check(commentSeen.contains(QStringLiteral("这条要记住")),
              QStringLiteral("foreign comment parsed"));
        check(!bn.docPath.isEmpty(), QStringLiteral("target document derived (%1)")
                                          .arg(QFileInfo(bn.docPath).fileName()));
    }

    // ---- 4. commit into a store ----
    {
        const ImportedDocument bn = imports::detect(tmp.path() + "/bookxnote.md", a.docPath, &err);
        const ImportReport report = imports::commit(store, {bn}, &err);
        check(report.notes == 3 && report.documents == 1,
              QStringLiteral("commit: %1").arg(report.summary()));
        check(store.markups(a.docPath).size() >= notes.size() + 3,
              QStringLiteral("store now holds the imported notes (%1)")
                  .arg(store.markups(a.docPath).size()));
        check(store.searchMarkups(QStringLiteral("结构先于细节"), a.docPath).size() >= 1,
              QStringLiteral("imported notes are searchable"));
    }

    // ---- 5. embedded PDF annotations are readable (write -> read loop) ----
    {
        const QString pdfCopy = tmp.path() + "/annotated.pdf";
        check(QFile::copy(pdf, pdfCopy), QStringLiteral("copy the pdf for annotations"));

        MuPdfEngine writer;
        check(writer.open(pdfCopy, &err), QStringLiteral("open the copy: %1").arg(err));

        QVector<Markup> toWrite;
        // highlight with a colour, an underline, and a note with a comment
        Markup h = a;
        h.id = newId();
        h.page = 0;
        h.type = MarkupType::Highlight;
        h.color = QColor(255, 213, 0);
        toWrite << h;
        Markup u = b;
        u.id = newId();
        u.page = 1;
        u.type = MarkupType::Underline;
        u.color = QColor(52, 168, 83);
        toWrite << u;
        Markup n = c;
        n.id = newId();
        n.page = 2;
        n.type = MarkupType::Note;
        n.color = QColor(217, 48, 37);
        n.comment = QStringLiteral("嵌入标注的批注内容");
        toWrite << n;

        check(writer.writeAnnotations(toWrite, &err), QStringLiteral("write annotations: %1").arg(err));

        const ImportedDocument read = imports::fromPdfAnnotations(writer, pdfCopy);
        for (const Markup& m : read.markups)
        for (const Markup& m : read.markups)
        for (const Markup& m : read.markups)
        check(read.markups.size() == 3, QStringLiteral("read %1 embedded annotations")
                                            .arg(read.markups.size()));
        check(countOf(read.markups, MarkupType::Highlight) == 1 &&
                  countOf(read.markups, MarkupType::Underline) == 1 &&
                  countOf(read.markups, MarkupType::Note) == 1,
              QStringLiteral("annotation types round-tripped"));

        bool sawComment = false;
        bool sawColour = false;
        bool sawQuads = false;
        for (const Markup& m : read.markups) {
            if (m.comment.contains(QStringLiteral("嵌入标注的批注内容"))) sawComment = true;
            if (m.color.green() > 120 && m.color.red() < 120) sawColour = true;
            if (!m.quads.isEmpty() && m.quads.first().boundingRect().width() > 5) sawQuads = true;
        }
        check(sawComment, QStringLiteral("annotation contents became the note comment"));
        check(sawColour, QStringLiteral("annotation colour round-tripped"));
        check(sawQuads, QStringLiteral("annotation geometry round-tripped"));

        // importing again on top of the same notes must not duplicate them
        const ImportedDocument again =
            imports::fromPdfAnnotations(writer, pdfCopy, read.markups);
        check(again.markups.isEmpty(), QStringLiteral("second read skips known annotations (%1 new)")
                                           .arg(again.markups.size()));

        // and committing them lands in the store like any other imported note
        const ImportReport report = imports::commit(store, {read}, &err);
        check(report.notes == 3, QStringLiteral("embedded annotations imported (%1)").arg(report.summary()));
        check(store.searchMarkups(QStringLiteral("嵌入标注的批注内容"), pdfCopy).size() == 1,
              QStringLiteral("imported annotation is searchable"));
    }

    std::printf("\n%s (%d failure(s))\n",
                g_fail == 0 ? "IMPORT CHECKS PASSED" : "IMPORT CHECKS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
