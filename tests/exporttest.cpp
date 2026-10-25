// Export test: md / html / opml / xmind / apkg, read back with independent parsers
// (ZipReader, QXmlStreamReader, SQLite) instead of trusting the writer.
#include "core/store.h"
#include "core/textselect.h"
#include "export/exporter.h"
#include "export/zip.h"
#include "render/mupdf_engine.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QXmlStreamReader>

#include <cstdio>

using namespace an;

namespace {

int g_fail = 0;

void check(bool ok, const QString& what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) ++g_fail;
}

QString readAll(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    return QString::fromUtf8(f.readAll());
}

Markup makeMarkup(const QVector<Quad>& quads, const QString& text, MarkupType type,
                  const QString& comment, const QStringList& tags, int page = 0)
{
    Markup m;
    m.id = newId();
    m.page = page;
    m.type = type;
    m.quads = quads;
    m.text = text;
    m.comment = comment;
    m.tags = tags;
    m.color = QColor("#FFD500");
    m.created = m.updated = QDateTime::currentMSecsSinceEpoch();
    return m;
}

int countXmlElements(const QString& xml, const QString& name)
{
    QXmlStreamReader r(xml);
    int n = 0;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement() && r.name().toString() == name) ++n;
    }
    return n;
}

int maxElementDepth(const QString& xml, const QString& name)
{
    QXmlStreamReader r(xml);
    int depth = 0;
    int maxDepth = 0;
    while (!r.atEnd()) {
        r.readNext();
        if (r.isStartElement() && r.name().toString() == name) {
            ++depth;
            maxDepth = qMax(maxDepth, depth);
        } else if (r.isEndElement() && r.name().toString() == name) {
            --depth;
        }
    }
    return maxDepth;
}

bool xmlWellFormed(const QString& xml)
{
    QXmlStreamReader r(xml);
    while (!r.atEnd()) r.readNext();
    return !r.hasError();
}

}  // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AdaptNote"));
    QCoreApplication::setApplicationName(QStringLiteral("AdaptNoteExportTest"));

    const QString pdf = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("test.pdf");
    QTemporaryDir tmp;
    check(tmp.isValid(), QStringLiteral("temp dir"));

    MuPdfEngine eng;
    QString err;
    check(eng.open(pdf, &err), QStringLiteral("open %1: %2").arg(pdf, err));
    if (!eng.isOpen()) return 1;

    // ---- build a small set of markups (2 highlights, 1 note with tags, 1 image) ----
    auto lineSel = [&](int page, int line) {
        const TextPage* tp = eng.textPage(page);
        int first = -1, last = -1;
        if (tp) {
            for (int i = 0; i < tp->chars.size(); ++i) {
                if (tp->chars.at(i).line != line) continue;
                if (first < 0) first = i;
                last = i;
            }
        }
        if (first < 0) return Selection{};
        return selectRange(*tp, page, tp->chars.at(first).origin, tp->chars.at(last).origin);
    };

    const Selection s1 = lineSel(0, 3);
    const Selection s2 = lineSel(0, 5);
    check(s1.isValid() && s2.isValid(), QStringLiteral("selection for export fixtures"));

    QVector<Markup> markups;
    markups.append(makeMarkup(s1.quads, s1.text, MarkupType::Highlight,
                              QStringLiteral("这段说的是标注的意义"), {QStringLiteral("方法论")}));
    markups.append(makeMarkup(s2.quads, s2.text, MarkupType::Underline, QString(),
                              {QStringLiteral("方法论"), QStringLiteral("待复习")}));
    markups.append(makeMarkup(s2.quads, QStringLiteral("<script>alert(1)</script> & 转义测试"),
                              MarkupType::Note, QStringLiteral("批注里要转义 < > &"),
                              {QStringLiteral("安全")}));

    // image excerpt: render page 1 and crop a region
    const QImage page0 = eng.renderPage(0, 2.0f, 0);
    const QImage crop = page0.copy(QRect(120, 300, 400, 120));
    const QString imgPath = tmp.path() + "/excerpt.png";
    check(crop.save(imgPath), QStringLiteral("fixture image saved"));
    Markup img = makeMarkup({Quad::fromRect(QRectF(60, 150, 200, 60))}, QString(),
                            MarkupType::Image, QStringLiteral("图注"), {QStringLiteral("图示")});
    img.imagePath = imgPath;
    markups.append(img);

    ExportContext ctx;
    ctx.docPath = QFileInfo(pdf).absoluteFilePath();
    ctx.docTitle = QStringLiteral("测试书 · 导出");
    for (Markup& m : markups) m.docPath = ctx.docPath;   // single-book fixtures
    ctx.markups = markups;
    ctx.deckName = QStringLiteral("AdaptNote 测试");

    // ---------------------------------------------------------------- markdown
    std::printf("\n=== markdown ===\n");
    ctx.outPath = tmp.path() + "/notes.md";
    check(exports::toMarkdown(ctx, &err), QStringLiteral("export md: %1").arg(err));
    const QString md = readAll(ctx.outPath);
    check(md.contains(QStringLiteral("# 测试书 · 导出")), QStringLiteral("md has title"));
    check(md.contains(QStringLiteral("## 第 1 页")), QStringLiteral("md groups by page"));
    check(md.contains(s1.text.left(8)), QStringLiteral("md contains excerpt text"));
    check(md.contains(QStringLiteral("批注：这段说的是标注的意义")), QStringLiteral("md contains comment"));
    check(md.contains(QStringLiteral("#方法论")), QStringLiteral("md contains tags"));
    check(md.contains(QStringLiteral("![摘录]")), QStringLiteral("md links the image"));
    const QString assetsDir = tmp.path() + "/notes.assets";
    check(QFile::exists(assetsDir + "/excerpt.png"), QStringLiteral("md asset copied"));

    // -------------------------------------------------------------------- html
    std::printf("\n=== html ===\n");
    ctx.outPath = tmp.path() + "/notes.html";
    check(exports::toHtml(ctx, &err), QStringLiteral("export html: %1").arg(err));
    const QString html = readAll(ctx.outPath);
    check(html.contains(QStringLiteral("<!DOCTYPE html>")), QStringLiteral("html doctype"));
    check(html.contains(QStringLiteral("data:image/png;base64,")), QStringLiteral("html inlines image as data URI"));
    check(html.contains(QStringLiteral("&lt;script&gt;alert(1)&lt;/script&gt;")),
          QStringLiteral("html escapes injected markup"));
    check(!html.contains(QStringLiteral("<script>alert(1)</script>")), QStringLiteral("no raw script tag leaked"));
    check(html.contains(QStringLiteral("第 1 页")), QStringLiteral("html has page heading"));

    // -------------------------------------------------------------------- opml
    std::printf("\n=== opml ===\n");
    ctx.outPath = tmp.path() + "/notes.opml";
    check(exports::toOpml(ctx, &err), QStringLiteral("export opml: %1").arg(err));
    const QString opml = readAll(ctx.outPath);
    check(xmlWellFormed(opml), QStringLiteral("opml is well-formed xml"));
    check(countXmlElements(opml, QStringLiteral("outline")) == 1 + markups.size(),
          QStringLiteral("opml outline count = %1").arg(countXmlElements(opml, QStringLiteral("outline"))));

    // ------------------------------------------------------------------- xmind
    std::printf("\n=== xmind ===\n");
    ctx.outPath = tmp.path() + "/notes.xmind";
    check(exports::toXmind(ctx, &err), QStringLiteral("export xmind: %1").arg(err));
    {
        ZipReader zr(ctx.outPath);
        check(zr.isOpen(), QStringLiteral("xmind is a readable zip"));
        check(zr.contains(QStringLiteral("content.xml")) &&
                  zr.contains(QStringLiteral("meta.xml")) &&
                  zr.contains(QStringLiteral("META-INF/manifest.xml")),
              QStringLiteral("xmind has content/meta/manifest (%1 entries)").arg(zr.names().size()));
        const QString content = QString::fromUtf8(zr.read(QStringLiteral("content.xml")));
        check(xmlWellFormed(content), QStringLiteral("xmind content.xml well-formed"));
        const int topics = countXmlElements(content, QStringLiteral("topic"));
        check(topics == 1 + 1 + markups.size(),
              QStringLiteral("xmind topics = %1 (root + page + %2 notes)").arg(topics).arg(markups.size()));
        check(content.contains(QStringLiteral("测试书 · 导出")), QStringLiteral("xmind root title present"));
        check(xmlWellFormed(QString::fromUtf8(zr.read(QStringLiteral("META-INF/manifest.xml")))),
              QStringLiteral("xmind manifest well-formed"));
    }

    // -------------------------------------------------------------------- anki
    std::printf("\n=== anki apkg ===\n");
    ctx.outPath = tmp.path() + "/notes.apkg";
    check(exports::toAnki(ctx, &err), QStringLiteral("export apkg: %1").arg(err));
    {
        ZipReader zr(ctx.outPath);
        check(zr.isOpen(), QStringLiteral("apkg is a readable zip"));
        check(zr.contains(QStringLiteral("collection.anki2")), QStringLiteral("apkg has collection.anki2"));
        check(zr.contains(QStringLiteral("media")), QStringLiteral("apkg has media map"));

        const QString dbPath = tmp.path() + "/collection.anki2";
        {
            QFile f(dbPath);
            f.open(QIODevice::WriteOnly);
            f.write(zr.read(QStringLiteral("collection.anki2")));
        }
        const QString conn = QStringLiteral("apkg-verify");
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn);
            db.setDatabaseName(dbPath);
            check(db.open(), QStringLiteral("collection.anki2 opens as sqlite"));
            QSqlQuery q(db);

            q.exec("pragma integrity_check");
            check(q.next() && q.value(0).toString() == QStringLiteral("ok"),
                  QStringLiteral("sqlite integrity_check ok"));

            q.exec("select count(*) from notes");
            const int notes = q.next() ? q.value(0).toInt() : -1;
            check(notes == markups.size(), QStringLiteral("notes = %1").arg(notes));

            q.exec("select count(*) from cards");
            const int cards = q.next() ? q.value(0).toInt() : -1;
            check(cards == markups.size(), QStringLiteral("cards = %1").arg(cards));

            q.exec("select ver, models, decks from col");
            check(q.next(), QStringLiteral("col row present"));
            const int ver = q.value(0).toInt();
            check(ver == 11, QStringLiteral("collection schema version = %1").arg(ver));

            const QJsonObject models = QJsonDocument::fromJson(q.value(1).toString().toUtf8()).object();
            const QJsonObject decks = QJsonDocument::fromJson(q.value(2).toString().toUtf8()).object();
            check(models.size() == 1, QStringLiteral("models json has 1 model"));
            check(decks.size() == 1 && decks.value(QStringLiteral("1")).toObject().value("name").toString() ==
                                           QStringLiteral("AdaptNote 测试"),
                  QStringLiteral("deck json carries the deck name"));
            if (!models.isEmpty()) {
                const QJsonObject model = models.begin().value().toObject();
                check(model.value("flds").toArray().size() == 2, QStringLiteral("model has 2 fields"));
                check(model.value("tmpls").toArray().size() == 1, QStringLiteral("model has 1 template"));
            }

            q.exec("select tags, flds, sfld, csum from notes order by id");
            int checked = 0;
            bool sawMethod = false, sawTwoFields = false;
            int sawChecksum = 0;
            while (q.next()) {
                const QString tags = q.value(0).toString();
                const QString flds = q.value(1).toString();
                const QString sfld = q.value(2).toString();
                const int csum = q.value(3).toInt();
                if (tags.contains(QStringLiteral("方法论"))) sawMethod = true;
                const QStringList parts = flds.split(QLatin1Char('\x1f'));
                if (parts.size() == 2 && !parts.at(0).isEmpty() && !parts.at(1).isEmpty())
                    sawTwoFields = true;
                const QByteArray expect =
                    QCryptographicHash::hash(sfld.toUtf8(), QCryptographicHash::Sha1).toHex().left(8);
                if (csum == static_cast<int>(expect.toUInt(nullptr, 16))) ++sawChecksum;
                ++checked;
            }
            check(checked == markups.size(), QStringLiteral("iterated %1 notes").arg(checked));
            check(sawMethod, QStringLiteral("at least one note carries the 方法论 tag"));
            check(sawTwoFields, QStringLiteral("every note has non-empty Front/Back fields"));
            check(sawChecksum, QStringLiteral("checksums match sha1 prefix (%1/%2)").arg(sawChecksum).arg(checked));

            // the image note must reference the media file by name
            q.exec("select flds from notes order by id");
            QString allFields;
            while (q.next()) allFields += q.value(0).toString();
            check(allFields.contains(QStringLiteral("<img src=\"excerpt.png\">")),
                  QStringLiteral("image note references media file"));

            db.close();
        }
        QSqlDatabase::removeDatabase(conn);

        const QJsonObject mediaMap = QJsonDocument::fromJson(zr.read(QStringLiteral("media"))).object();
        check(mediaMap.size() == 1, QStringLiteral("media map has 1 entry"));
        check(mediaMap.value(QStringLiteral("0")).toString() == QStringLiteral("excerpt.png"),
              QStringLiteral("media map 0 -> excerpt.png"));
        QFile imgFile(imgPath);
        const bool imgOpen = imgFile.open(QIODevice::ReadOnly);
        const QByteArray imgBytes = imgOpen ? imgFile.readAll() : QByteArray();
        check(imgOpen && !imgBytes.isEmpty() && zr.read(QStringLiteral("0")) == imgBytes,
              QStringLiteral("media entry 0 matches the source image (%1 bytes)").arg(imgBytes.size()));
    }

    // ---------------------------------------------------------------- library wide
    std::printf("\n=== library-wide export (grouped by book) ===\n");
    {
        ExportContext lib = ctx;
        lib.groupByDocument = true;
        lib.docTitle = QStringLiteral("书库");
        lib.markups.clear();

        QVector<Markup> first = markups;
        for (Markup& m : first) m.docPath = QStringLiteral("/books/第一本书.pdf");
        QVector<Markup> second = markups.mid(0, 2);
        for (Markup& m : second) {
            m.id = newId();
            m.docPath = QStringLiteral("/books/Second Book.epub");
            m.page = 4;
        }
        lib.markups = first + second;

        lib.outPath = tmp.path() + "/library.md";
        check(exports::toMarkdown(lib, &err), QStringLiteral("library md: %1").arg(err));
        const QString lmd = readAll(lib.outPath);
        check(lmd.contains(QStringLiteral("## 第一本书")),
              QStringLiteral("library md has the first book heading"));
        check(lmd.contains(QStringLiteral("## Second Book")),
              QStringLiteral("library md has the second book heading"));
        check(lmd.count(QStringLiteral("### 第 ")) == 2,
              QStringLiteral("library md nests one page heading per book page (%1)")
                  .arg(lmd.count(QStringLiteral("### 第 "))));
        const int firstBook = lmd.indexOf(QStringLiteral("## 第一本书"));
        const int firstPage = lmd.indexOf(QStringLiteral("### 第 "));
        const int secondBook = lmd.indexOf(QStringLiteral("## Second Book"));
        check(firstBook >= 0 && firstPage > firstBook && secondBook > firstPage,
              QStringLiteral("page headings sit between the two book headings"));

        lib.outPath = tmp.path() + "/library.opml";
        check(exports::toOpml(lib, &err), QStringLiteral("library opml: %1").arg(err));
        const QString lopml = readAll(lib.outPath);
        check(xmlWellFormed(lopml), QStringLiteral("library opml well-formed"));
        check(lopml.contains(QStringLiteral("text=\"第一本书\"")) &&
                  lopml.contains(QStringLiteral("text=\"Second Book\"")),
              QStringLiteral("library opml has book nodes"));

        lib.outPath = tmp.path() + "/library.apkg";
        check(exports::toAnki(lib, &err), QStringLiteral("library apkg: %1").arg(err));
        {
            ZipReader zr(lib.outPath);
            const QString dbPath = tmp.path() + "/library.anki2";
            QFile db(dbPath);
            db.open(QIODevice::WriteOnly);
            db.write(zr.read(QStringLiteral("collection.anki2")));
            db.close();

            const QString conn = QStringLiteral("apkg-library");
            {
                QSqlDatabase sq = QSqlDatabase::addDatabase("QSQLITE", conn);
                sq.setDatabaseName(dbPath);
                check(sq.open(), QStringLiteral("library apkg opens"));
                QSqlQuery q(sq);
                q.exec("select count(*) from notes");
                const int notes = q.next() ? q.value(0).toInt() : -1;
                check(notes == lib.markups.size(),
                      QStringLiteral("library apkg holds every note (%1)").arg(notes));
                q.exec("select flds from notes");
                QString all;
                while (q.next()) all += q.value(0).toString();
                check(all.contains(QStringLiteral("第一本书")) &&
                          all.contains(QStringLiteral("Second Book")),
                      QStringLiteral("library apkg front fields name the source book"));
                sq.close();
            }
            QSqlDatabase::removeDatabase(conn);
        }
    }

    // ---------------------------------------------------------------- nested groups
    std::printf("\n=== mind groups in exports ===\n");
    {
        ExportContext gctx = ctx;
        gctx.outPath = tmp.path() + "/groups.opml";

        MindGroup top;
        top.id = QStringLiteral("g-top");
        top.name = QStringLiteral("第一部分");
        top.docPath = ctx.docPath;
        MindGroup child;
        child.id = QStringLiteral("g-child");
        child.name = QStringLiteral("子主题");
        child.parentId = top.id;
        child.docPath = ctx.docPath;

        QVector<Markup> nested;
        Markup inTop = markups.first();
        inTop.id = newId();
        inTop.mindGroup = top.id;
        inTop.text = QStringLiteral("挂在第一部分");
        Markup inChild = markups.at(1);
        inChild.id = newId();
        inChild.mindGroup = child.id;
        inChild.text = QStringLiteral("挂在子主题");
        nested << inTop << inChild;
        gctx.markups = nested;
        gctx.groupsByDoc.insert(ctx.docPath, {top, child});

        check(exports::toOpml(gctx, &err), QStringLiteral("opml with groups: %1").arg(err));
        const QString opml = readAll(gctx.outPath);
        check(xmlWellFormed(opml), QStringLiteral("grouped opml is well formed"));
        // note leaves are self-closing, so the deepest *open* outline is the subgroup
        check(maxElementDepth(opml, QStringLiteral("outline")) == 3,
              QStringLiteral("opml nests group > subgroup (depth %1)")
                  .arg(maxElementDepth(opml, QStringLiteral("outline"))));
        // the note must sit inside the subgroup: its line is indented deeper than the group
        const int childGroupAt = opml.indexOf(QStringLiteral("子主题"));
        const int noteAt = opml.indexOf(QStringLiteral("挂在子主题"));
        check(childGroupAt >= 0 && noteAt > childGroupAt,
              QStringLiteral("note is written after (inside) its subgroup"));
        check(opml.contains(QStringLiteral("text=\"挂在子主题\"")),
              QStringLiteral("note line present in the grouped outline"));
        check(opml.contains(QStringLiteral("第一部分")) && opml.contains(QStringLiteral("子主题")),
              QStringLiteral("opml carries both group names"));

        gctx.outPath = tmp.path() + "/groups.xmind";
        check(exports::toXmind(gctx, &err), QStringLiteral("xmind with groups: %1").arg(err));
        {
            ZipReader zr(gctx.outPath);
            const QString content = QString::fromUtf8(zr.read(QStringLiteral("content.xml")));
            check(xmlWellFormed(content), QStringLiteral("grouped xmind is well formed"));
            check(countXmlElements(content, QStringLiteral("topic")) == 5,
                  QStringLiteral("xmind topics = root + group + subgroup + 2 notes (%1)")
                      .arg(countXmlElements(content, QStringLiteral("topic"))));
            check(maxElementDepth(content, QStringLiteral("topics")) == 3,
                  QStringLiteral("xmind nests topics three levels deep (%1)")
                      .arg(maxElementDepth(content, QStringLiteral("topics"))));
            check(content.contains(QStringLiteral("子主题")),
                  QStringLiteral("xmind keeps the subgroup"));
        }
    }

    // ---------------------------------------------------------------- by mind map
    std::printf("\n=== markdown/html organised by the mind map ===\n");
    {
        MindGroup top;
        top.id = QStringLiteral("gm-top");
        top.name = QStringLiteral("第一部分");
        top.docPath = ctx.docPath;
        MindGroup child;
        child.id = QStringLiteral("gm-child");
        child.name = QStringLiteral("子主题");
        child.parentId = top.id;
        child.docPath = ctx.docPath;

        Markup inTop = markups.first();
        inTop.id = newId();
        inTop.mindGroup = top.id;
        inTop.text = QStringLiteral("挂在第一部分的内容");
        Markup inChild = markups.at(1);
        inChild.id = newId();
        inChild.mindGroup = child.id;
        inChild.text = QStringLiteral("挂在子主题的内容");
        Markup loose = markups.at(2);
        loose.id = newId();
        loose.mindGroup.clear();   // no group -> page bucket

        ExportContext grouped = ctx;
        grouped.byGroups = true;
        grouped.markups = {inTop, inChild, loose};
        grouped.groupsByDoc.clear();
        grouped.groupsByDoc.insert(ctx.docPath, {top, child});

        grouped.outPath = tmp.path() + "/bygroups.md";
        check(exports::toMarkdown(grouped, &err), QStringLiteral("grouped md: %1").arg(err));
        const QString md = readAll(grouped.outPath);
        check(md.contains(QStringLiteral("## 第一部分")),
              QStringLiteral("md has the group heading"));
        check(md.contains(QStringLiteral("### 子主题")),
              QStringLiteral("md nests the subgroup heading"));
        check(md.contains(QStringLiteral("挂在第一部分的内容")) &&
                  md.contains(QStringLiteral("挂在子主题的内容")),
              QStringLiteral("md lists the notes under their groups"));
        check(!md.contains(QStringLiteral("### 第 1 页")),
              QStringLiteral("md does not put the grouped notes under a page heading"));
        int bullets = 0;
        for (const QString& line : md.split(QLatin1Char('\n')))
            if (line.startsWith(QStringLiteral("- **"))) ++bullets;
        check(bullets == 3, QStringLiteral("md kept all %1 notes").arg(bullets));

        grouped.outPath = tmp.path() + "/bygroups.html";
        check(exports::toHtml(grouped, &err), QStringLiteral("grouped html: %1").arg(err));
        const QString html = readAll(grouped.outPath);
        check(html.contains(QStringLiteral("<h2>第一部分</h2>")) &&
                  html.contains(QStringLiteral("<h3>子主题</h3>")),
              QStringLiteral("html mirrors the group nesting"));
        // the ungrouped note legitimately lands in a page bucket; the grouped ones must not
        const int groupAt = html.indexOf(QStringLiteral("<h2>第一部分</h2>"));
        const int firstPage = html.indexOf(QStringLiteral("第 1 页"));
        check(groupAt >= 0 && firstPage > groupAt,
              QStringLiteral("html lists groups before the leftover page bucket"));
        int items = 0;
        for (int i = html.indexOf(QStringLiteral("class=\"item\"")); i >= 0;
             i = html.indexOf(QStringLiteral("class=\"item\""), i + 1))
            ++items;
        check(items == 3, QStringLiteral("html kept all %1 notes").arg(items));
    }

    std::printf("\n%s (%d failure(s))\n", g_fail == 0 ? "EXPORT CHECKS PASSED" : "EXPORT CHECKS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
