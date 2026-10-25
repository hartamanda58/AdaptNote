#include "export/exporter.h"

#include "export/zip.h"

#include "mind/tree.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>

namespace an {
namespace exports {
namespace {

QString typeLabel(MarkupType t)
{
    switch (t) {
    case MarkupType::Highlight: return QStringLiteral("高亮");
    case MarkupType::Underline: return QStringLiteral("下划线");
    case MarkupType::Squiggly: return QStringLiteral("波浪线");
    case MarkupType::Note: return QStringLiteral("批注");
    case MarkupType::Image: return QStringLiteral("摘录图");
    case MarkupType::Ink: return QStringLiteral("手写");
    }
    return {};
}

QString docTitleOf(const ExportContext& ctx)
{
    if (!ctx.docTitle.trimmed().isEmpty()) return ctx.docTitle.trimmed();
    return QFileInfo(ctx.docPath).completeBaseName();
}

QString bookNameOf(const ExportContext& ctx, const Markup& m)
{
    if (!ctx.groupByDocument) return docTitleOf(ctx);
    const QString base = QFileInfo(m.docPath).completeBaseName();
    return base.isEmpty() ? docTitleOf(ctx) : base;
}

QVector<Markup> sorted(const ExportContext& ctx)
{
    QVector<Markup> v = ctx.markups;
    std::sort(v.begin(), v.end(), [](const Markup& a, const Markup& b) {
        if (a.page != b.page) return a.page < b.page;
        return a.boundingRect().top() < b.boundingRect().top();
    });
    return v;
}

QString assetDirFor(const QString& outPath)
{
    const QFileInfo fi(outPath);
    return fi.absolutePath() + "/" + fi.completeBaseName() + ".assets";
}

bool copyImage(const Markup& m, const QString& dir, QString* relativeName, QString* error)
{
    if (m.imagePath.isEmpty() || !QFile::exists(m.imagePath)) {
        if (error) *error = QStringLiteral("missing image: %1").arg(m.imagePath);
        return false;
    }
    QDir().mkpath(dir);
    const QString name = QFileInfo(m.imagePath).fileName();
    const QString target = dir + "/" + name;
    if (!QFile::exists(target) && !QFile::copy(m.imagePath, target)) {
        if (error) *error = QStringLiteral("cannot copy image to %1").arg(target);
        return false;
    }
    *relativeName = name;
    return true;
}

QString mdEscape(QString s)
{
    // keep it readable: only escape characters that would break list/quote structure
    s.replace(QLatin1String("&"), QLatin1String("&amp;"));
    s.replace(QLatin1String("<"), QLatin1String("&lt;"));
    s.replace(QLatin1String(">"), QLatin1String("&gt;"));
    return s;
}

QString xmlEscape(QString s)
{
    s.replace(QLatin1String("&"), QLatin1String("&amp;"));
    s.replace(QLatin1String("<"), QLatin1String("&lt;"));
    s.replace(QLatin1String(">"), QLatin1String("&gt;"));
    s.replace(QLatin1String("\""), QLatin1String("&quot;"));
    s.replace(QLatin1String("'"), QLatin1String("&apos;"));
    return s;
}

bool writeFile(const QString& path, const QByteArray& data, QString* error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = f.errorString();
        return false;
    }
    if (f.write(data) != data.size()) {
        if (error) *error = QStringLiteral("short write");
        return false;
    }
    return true;
}

QString imageDataUri(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return {};
    const QByteArray bytes = f.readAll();
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
}

// ------------------------------------------------------------------ anki bits

QString ankiTags(const QStringList& tags)
{
    // NOTE: must be a non-null QString — a null QString binds as SQL NULL and
    // notes.tags is NOT NULL in Anki's schema.
    if (tags.isEmpty()) return QString::fromLatin1("");
    return QStringLiteral(" %1 ").arg(tags.join(QLatin1Char(' ')));
}

int ankiChecksum(const QString& sfld)
{
    const QByteArray sha1 =
        QCryptographicHash::hash(sfld.toUtf8(), QCryptographicHash::Sha1).toHex().left(8);
    bool ok = false;
    const uint v = sha1.toUInt(&ok, 16);
    return ok ? static_cast<int>(v) : 0;
}

QByteArray ankiModelsJson(qint64 modelId, qint64 deckId, qint64 nowSec)
{
    QJsonObject field0;
    field0["name"] = "Front";
    field0["ord"] = 0;
    field0["sticky"] = false;
    field0["rtl"] = false;
    field0["font"] = "Arial";
    field0["size"] = 20;
    field0["media"] = QJsonArray();

    QJsonObject field1 = field0;
    field1["name"] = "Back";
    field1["ord"] = 1;

    QJsonObject tmpl;
    tmpl["name"] = "Card 1";
    tmpl["ord"] = 0;
    tmpl["qfmt"] = "{{Front}}";
    tmpl["afmt"] = "{{FrontSide}}<hr id=answer>{{Back}}";
    tmpl["bqfmt"] = "";
    tmpl["bafmt"] = "";
    tmpl["did"] = QJsonValue::Null;
    tmpl["bfont"] = "";
    tmpl["bsize"] = 0;

    QJsonObject model;
    model["id"] = static_cast<double>(modelId);
    model["name"] = "AdaptNote Basic";
    model["type"] = 0;
    model["mod"] = static_cast<double>(nowSec);
    model["usn"] = 0;
    model["sortf"] = 0;
    model["did"] = static_cast<double>(deckId);
    model["tags"] = QJsonArray();
    model["vers"] = QJsonArray();
    model["css"] = ".card{font-family:-apple-system,'Noto Sans CJK SC',sans-serif;"
                  "font-size:18px;text-align:left;color:#222;background:#fff;line-height:1.5;}"
                  ".src{color:#888;font-size:13px;}";
    model["flds"] = QJsonArray{field0, field1};
    model["tmpls"] = QJsonArray{tmpl};
    model["req"] = QJsonArray{QJsonArray{0, QStringLiteral("any"), QJsonArray{0}}};
    model["latexPre"] = "";
    model["latexPost"] = "";

    QJsonObject models;
    models[QString::number(modelId)] = model;
    return QJsonDocument(models).toJson(QJsonDocument::Compact);
}

QByteArray ankiDecksJson(qint64 deckId, const QString& name, qint64 nowSec)
{
    QJsonObject deck;
    deck["id"] = static_cast<double>(deckId);
    deck["name"] = name;
    deck["desc"] = "";
    deck["mod"] = static_cast<double>(nowSec);
    deck["usn"] = 0;
    deck["collapsed"] = false;
    deck["browserCollapsed"] = false;
    deck["newToday"] = QJsonArray{0, 0};
    deck["revToday"] = QJsonArray{0, 0};
    deck["lrnToday"] = QJsonArray{0, 0};
    deck["timeToday"] = QJsonArray{0, 0};
    deck["conf"] = 1;
    deck["dyn"] = 0;
    deck["extendNew"] = 10;
    deck["extendRev"] = 50;

    QJsonObject decks;
    decks[QString::number(deckId)] = deck;
    return QJsonDocument(decks).toJson(QJsonDocument::Compact);
}

QByteArray ankiDconfJson()
{
    QJsonObject dconf;
    dconf["id"] = 1;
    dconf["name"] = "Default";
    QJsonObject nw{{"delays", QJsonArray{1, 10}},
                   {"ints", QJsonArray{1, 4, 7}},
                   {"initialFactor", 2500},
                   {"order", 1},
                   {"perDay", 20}};
    dconf["new"] = nw;
    QJsonObject lapse{{"delays", QJsonArray{10}},
                      {"mult", 0},
                      {"minInt", 1},
                      {"leechFails", 8},
                      {"leechAction", 1}};
    dconf["lapse"] = lapse;
    QJsonObject rev{{"perDay", 200},
                    {"ease4", 1.3},
                    {"fuzz", 0.05},
                    {"minSpace", 1},
                    {"ivlFct", 1},
                    {"maxIvl", 36500},
                    {"bury", false}};
    dconf["rev"] = rev;
    dconf["maxTaken"] = 60;
    dconf["timer"] = 0;
    dconf["autoplay"] = true;
    dconf["replayq"] = true;
    dconf["mod"] = 0;
    dconf["usn"] = 0;
    dconf["dyn"] = false;

    QJsonObject all;
    all["1"] = dconf;
    return QJsonDocument(all).toJson(QJsonDocument::Compact);
}

QString ankiFrontHtml(const Markup& m, const QString& docTitle)
{
    QString front;
    if (m.type == MarkupType::Image && !m.imagePath.isEmpty()) {
        front = QStringLiteral("<img src=\"%1\">").arg(QFileInfo(m.imagePath).fileName());
    } else {
        QString text = m.text;
        text.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
        front = QStringLiteral("<div class=\"excerpt\">%1</div>").arg(text.toHtmlEscaped());
    }
    front += QStringLiteral("<div class=\"src\">%1 · 第 %2 页</div>")
                 .arg(docTitle.toHtmlEscaped())
                 .arg(m.page + 1);
    return front;
}

QString ankiBackHtml(const Markup& m)
{
    QString back;
    if (!m.comment.isEmpty())
        back += QStringLiteral("<div>%1</div>").arg(m.comment.toHtmlEscaped().replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    if (!m.tags.isEmpty())
        back += QStringLiteral("<div class=\"src\">#%1</div>").arg(m.tags.join(QStringLiteral(" #")).toHtmlEscaped());
    if (back.isEmpty())
        back = QStringLiteral("<div class=\"src\">(%1)</div>").arg(typeLabel(m.type));
    return back;
}

bool buildAnkiCollection(const ExportContext& ctx, const QString& dbPath,
                         QVector<QPair<QString, QString>>* media, QString* error)
{
    const QString conn = QStringLiteral("anki-export-%1").arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    bool ok = true;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", conn);
        db.setDatabaseName(dbPath);
        if (!db.open()) {
            if (error) *error = db.lastError().text();
            QSqlDatabase::removeDatabase(conn);
            return false;
        }
        QSqlQuery q(db);
        const QStringList ddl = {
            "CREATE TABLE col (id INTEGER PRIMARY KEY, crt INTEGER NOT NULL, mod INTEGER NOT NULL,"
            " scm INTEGER NOT NULL, ver INTEGER NOT NULL, dty INTEGER NOT NULL, usn INTEGER NOT NULL,"
            " ls INTEGER NOT NULL, conf TEXT NOT NULL, models TEXT NOT NULL, decks TEXT NOT NULL,"
            " dconf TEXT NOT NULL, tags TEXT NOT NULL)",
            "CREATE TABLE notes (id INTEGER PRIMARY KEY, guid TEXT NOT NULL, mid INTEGER NOT NULL,"
            " mod INTEGER NOT NULL, usn INTEGER NOT NULL, tags TEXT NOT NULL, flds TEXT NOT NULL,"
            " sfld TEXT NOT NULL, csum INTEGER NOT NULL, flags INTEGER NOT NULL, data TEXT NOT NULL)",
            "CREATE TABLE cards (id INTEGER PRIMARY KEY, nid INTEGER NOT NULL, did INTEGER NOT NULL,"
            " ord INTEGER NOT NULL, mod INTEGER NOT NULL, usn INTEGER NOT NULL, type INTEGER NOT NULL,"
            " queue INTEGER NOT NULL, due INTEGER NOT NULL, ivl INTEGER NOT NULL, factor INTEGER NOT NULL,"
            " reps INTEGER NOT NULL, lapses INTEGER NOT NULL, [left] INTEGER NOT NULL,"
            " odue INTEGER NOT NULL, odid INTEGER NOT NULL, flags INTEGER NOT NULL, data TEXT NOT NULL)",
            "CREATE TABLE revlog (id INTEGER PRIMARY KEY, cid INTEGER NOT NULL, usn INTEGER NOT NULL,"
            " ease INTEGER NOT NULL, ivl INTEGER NOT NULL, lastIvl INTEGER NOT NULL,"
            " factor INTEGER NOT NULL, time INTEGER NOT NULL, type INTEGER NOT NULL)",
            "CREATE TABLE graves (usn INTEGER NOT NULL, oid INTEGER NOT NULL, type INTEGER NOT NULL)",
            "CREATE INDEX ix_notes_usn ON notes (usn)",
            "CREATE INDEX ix_cards_usn ON cards (usn)",
            "CREATE INDEX ix_cards_nid ON cards (nid)",
            "CREATE INDEX ix_cards_sched ON cards (did, queue, due)",
            "CREATE INDEX ix_revlog_usn ON revlog (usn)",
            "CREATE INDEX ix_revlog_cid ON revlog (cid)",
            "CREATE INDEX ix_notes_csum ON notes (csum)",
        };
        for (const QString& s : ddl) {
            if (!q.exec(s)) {
                if (error) *error = q.lastError().text();
                ok = false;
                break;
            }
        }

        const qint64 nowSec = QDateTime::currentSecsSinceEpoch();
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const qint64 modelId = 1700000000000LL;
        const qint64 deckId = 1;

        if (ok) {
            QSqlQuery col(db);
            col.prepare("INSERT INTO col (id,crt,mod,scm,ver,dty,usn,ls,conf,models,decks,dconf,tags)"
                        " VALUES (1,?,?,?,11,0,0,0,'{}',?,?,'{}','{}')");
            col.addBindValue(nowSec);
            col.addBindValue(nowMs);
            col.addBindValue(nowMs);
            col.addBindValue(QString::fromUtf8(ankiModelsJson(modelId, deckId, nowSec)));
            col.addBindValue(QString::fromUtf8(ankiDecksJson(deckId, ctx.deckName, nowSec)));
            if (!col.exec()) {
                if (error) *error = col.lastError().text();
                ok = false;
            }
        }

        int index = 0;
        for (const Markup& m : sorted(ctx)) {
            const QString docTitle = bookNameOf(ctx, m);
            if (!ok) break;
            ++index;
            const qint64 noteId = nowMs + index;
            const QString front = ankiFrontHtml(m, docTitle);
            const QString back = ankiBackHtml(m);
            const QString sfld = m.text.isEmpty() ? docTitle : m.text.left(120);

            if (m.type == MarkupType::Image && !m.imagePath.isEmpty() && ctx.includeImages) {
                if (media && QFile::exists(m.imagePath))
                    media->append({QFileInfo(m.imagePath).fileName(), m.imagePath});
            }

            QSqlQuery n(db);
            n.prepare("INSERT INTO notes (id,guid,mid,mod,usn,tags,flds,sfld,csum,flags,data)"
                      " VALUES (?,?,?,?,0,?,?,?,?,0,'')");
            n.addBindValue(noteId);
            n.addBindValue(QUuid::createUuid().toString(QUuid::WithoutBraces).left(10));
            n.addBindValue(modelId);
            n.addBindValue(nowSec);
            n.addBindValue(ankiTags(m.tags));
            n.addBindValue(front + QLatin1Char('\x1f') + back);
            n.addBindValue(sfld);
            n.addBindValue(ankiChecksum(sfld));
            if (!n.exec()) {
                if (error) *error = n.lastError().text();
                ok = false;
                break;
            }

            QSqlQuery c(db);
            c.prepare("INSERT INTO cards (id,nid,did,ord,mod,usn,type,queue,due,ivl,factor,reps,"
                      "lapses,[left],odue,odid,flags,data)"
                      " VALUES (?,?,?,0,?,0,0,0,?,0,2500,0,0,0,0,0,0,'')");
            c.addBindValue(noteId + 1);
            c.addBindValue(noteId);
            c.addBindValue(deckId);
            c.addBindValue(nowSec);
            c.addBindValue(index);
            if (!c.exec()) {
                if (error) *error = c.lastError().text();
                ok = false;
                break;
            }
        }
        db.close();
    }
    QSqlDatabase::removeDatabase(conn);
    return ok;
}

}  // namespace

namespace {

const Markup& lookupMarkup(const ExportContext& ctx, const QString& id)
{
    static const Markup kEmpty;
    for (const Markup& m : ctx.markups)
        if (m.id == id) return m;
    return kEmpty;
}

// Tree(s) to render for structured exports: one root per document.
QVector<mind::Node> mindRoots(const ExportContext& ctx)
{
    QVector<mind::Node> roots;

    // group the notes by document (library-wide exports mix several books)
    QVector<QString> docOrder;
    QHash<QString, QVector<Markup>> byDoc;
    for (const Markup& m : sorted(ctx)) {
        if (!byDoc.contains(m.docPath)) docOrder.append(m.docPath);
        byDoc[m.docPath].append(m);
    }

    for (const QString& doc : docOrder) {
        const QString title =
            ctx.groupByDocument ? QFileInfo(doc).completeBaseName() : docTitleOf(ctx);
        const QVector<MindGroup> groups = ctx.groupsByDoc.value(doc);
        roots.append(mind::buildFromNotes(title, byDoc.value(doc), groups));
    }
    if (roots.isEmpty()) roots.append(mind::buildFromNotes(docTitleOf(ctx), ctx.markups,
                                                          ctx.groupsByDoc.value(ctx.docPath)));
    return roots;
}

// XMind topic: title + optional note, then children recursively.
void appendTopic(QString& xml, const mind::Node& node, int depth)
{
    Q_UNUSED(depth);
    auto id = []() {
        return QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'));
    };
    xml += QStringLiteral("%1<topic id=\"%2\" timestamp=\"%3\">\n")
               .arg(QString(depth * 2 + 6, QLatin1Char(' ')), id())
               .arg(QDateTime::currentMSecsSinceEpoch());
    xml += QStringLiteral("%1<title>%2</title>\n")
               .arg(QString(depth * 2 + 8, QLatin1Char(' ')), xmlEscape(node.text));
    if (node.children.isEmpty()) {
        xml += QStringLiteral("%1</topic>\n").arg(QString(depth * 2 + 6, QLatin1Char(' ')));
        return;
    }
    xml += QStringLiteral("%1<children>\n%2<topics type=\"attached\">\n")
               .arg(QString(depth * 2 + 8, QLatin1Char(' ')), QString(depth * 2 + 10, QLatin1Char(' ')));
    for (const mind::Node& child : node.children) appendTopic(xml, child, depth + 1);
    xml += QStringLiteral("%1</topics>\n%2</children>\n")
               .arg(QString(depth * 2 + 10, QLatin1Char(' ')), QString(depth * 2 + 8, QLatin1Char(' ')));
    xml += QStringLiteral("%1</topic>\n").arg(QString(depth * 2 + 6, QLatin1Char(' ')));
}

void appendOutline(QString& out, const mind::Node& node, int depth,
                   const QHash<QString, Markup>* notes = nullptr)
{
    const QString pad(depth * 2, QLatin1Char(' '));
    out += QStringLiteral("%1<outline text=\"%2\"").arg(pad, xmlEscape(node.text));
    if (node.page >= 0) out += QStringLiteral(" page=\"%1\"").arg(node.page + 1);

    // note leaves carry the markup metadata (type, tags, comment, image)
    if (notes && !node.markupId.isEmpty() && notes->contains(node.markupId)) {
        const Markup& m = notes->value(node.markupId);
        out += QStringLiteral(" type=\"%1\"").arg(markupTypeName(m.type));
        if (!m.tags.isEmpty())
            out += QStringLiteral(" tags=\"%1\"").arg(xmlEscape(m.tags.join(QLatin1Char(','))));
        if (!m.comment.isEmpty())
            out += QStringLiteral(" _note=\"%1\"").arg(xmlEscape(m.comment));
        if (m.type == MarkupType::Image && !m.imagePath.isEmpty())
            out += QStringLiteral(" _image=\"%1\"").arg(xmlEscape(QFileInfo(m.imagePath).fileName()));
    }

    if (node.children.isEmpty()) {
        out += QStringLiteral("/>\n");
        return;
    }
    out += QStringLiteral(">\n");
    for (const mind::Node& child : node.children) appendOutline(out, child, depth + 1, notes);
    out += QStringLiteral("%1</outline>\n").arg(pad);
}

}  // namespace

// ------------------------------------------------------------------- markdown

bool toMarkdown(const ExportContext& ctx, QString* error)
{
    const QString assets = assetDirFor(ctx.outPath);
    QString out;
    out += QStringLiteral("# %1\n\n").arg(docTitleOf(ctx));
    out += QStringLiteral("> 由 AdaptNote 导出 · %1 · 共 %2 条笔记\n\n")
               .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")))
               .arg(ctx.markups.size());

    // one note as a markdown bullet, shared by both layouts
    auto renderNote = [&](const Markup& m, const QString& indent) {
        out += indent + QStringLiteral("- **%1**").arg(typeLabel(m.type));
        if (!m.tags.isEmpty())
            out += QStringLiteral(" · #%1").arg(m.tags.join(QStringLiteral(" #")));
        out += QLatin1Char('\n');
        if (!m.text.isEmpty()) {
            for (const QString& line : m.text.split(QLatin1Char('\n')))
                out += indent + QStringLiteral("  > %1\n").arg(mdEscape(line));
        }
        if (!m.comment.isEmpty())
            out += indent + QStringLiteral("  - 批注：%1\n").arg(mdEscape(m.comment));
        if (m.type == MarkupType::Image && ctx.includeImages && !m.imagePath.isEmpty()) {
            QString rel;
            QString copyErr;
            if (copyImage(m, assets, &rel, &copyErr))
                out += indent + QStringLiteral("  - ![摘录](%1/%2)\n")
                                     .arg(QFileInfo(assets).fileName(), rel);
        }
        out += QLatin1Char('\n');
    };

    if (ctx.byGroups) {
        // the mind map decides the structure: groups nest, notes hang under them
        for (const mind::Node& rootNode : mindRoots(ctx)) {
            if (ctx.groupByDocument)
                out += QStringLiteral("\n## %1\n\n").arg(mdEscape(rootNode.text));
            std::function<void(const mind::Node&, int)> walk = [&](const mind::Node& node,
                                                                  int depth) {
                if (!node.markupId.isEmpty()) {
                    const Markup m = lookupMarkup(ctx, node.markupId);
                    if (!m.id.isEmpty()) renderNote(m, QString());
                    return;
                }
                if (!node.text.isEmpty()) {
                    const int level = qBound(2, depth + 2, 5);
                    out += QStringLiteral("\n%1 %2\n\n")
                               .arg(QString(level, QLatin1Char('#')), mdEscape(node.text));
                }
                for (const mind::Node& child : node.children) walk(child, depth + 1);
            };
            for (const mind::Node& child : rootNode.children) walk(child, 0);
        }
        return writeFile(ctx.outPath, out.toUtf8(), error);
    }

    int page = -1;
    QString book;
    for (const Markup& m : sorted(ctx)) {
        const QString thisBook = bookNameOf(ctx, m);
        if (ctx.groupByDocument && thisBook != book) {
            book = thisBook;
            page = -1;
            out += QStringLiteral("\n## %1\n\n").arg(mdEscape(book));
        }
        if (m.page != page) {
            page = m.page;
            out += QStringLiteral("\n### 第 %1 页\n\n").arg(page + 1);
        }
        renderNote(m, QString());
    }
    return writeFile(ctx.outPath, out.toUtf8(), error);
}

// ----------------------------------------------------------------------- html

bool toHtml(const ExportContext& ctx, QString* error)
{
    QString out;
    out += QStringLiteral(
        "<!DOCTYPE html>\n<html lang=\"zh-CN\">\n<head>\n<meta charset=\"utf-8\">\n"
        "<title>%1 · 笔记</title>\n<style>\n"
        ":root{--ink:#222;--muted:#7b838d;--line:#e3e6ea;--accent:#3d8bfd;}\n"
        "body{margin:0;padding:40px 24px;background:#f6f7f9;color:var(--ink);"
        "font-family:-apple-system,'Noto Sans CJK SC','Segoe UI',sans-serif;line-height:1.65;}\n"
        ".wrap{max-width:820px;margin:0 auto;background:#fff;border:1px solid var(--line);"
        "border-radius:14px;padding:36px 40px;box-shadow:0 6px 24px rgba(20,30,50,.06);}\n"
        "h1{margin:0 0 6px;font-size:26px;}\n"
        ".meta{color:var(--muted);font-size:13px;margin-bottom:28px;}\n"
        "h2{font-size:15px;color:var(--muted);font-weight:600;margin:32px 0 12px;"
        "border-bottom:1px solid var(--line);padding-bottom:6px;}\n"
        ".item{margin:0 0 18px;padding-left:14px;border-left:3px solid var(--accent);}\n"
        ".type{font-size:12px;color:#fff;background:var(--accent);border-radius:9px;"
        "padding:1px 8px;margin-right:8px;}\n"
        ".excerpt{margin:6px 0;}\n"
        ".comment{color:#0b5;margin:6px 0;}\n"
        ".tags{color:var(--muted);font-size:12px;margin-top:4px;}\n"
        ".img{margin-top:8px;max-width:100%%;border:1px solid var(--line);border-radius:8px;}\n"
        ".src{color:var(--muted);font-size:12px;margin-top:4px;}\n"
        "</style>\n</head>\n<body>\n<div class=\"wrap\">\n")
               .arg(docTitleOf(ctx).toHtmlEscaped());

    out += QStringLiteral("<h1>%1</h1>\n").arg(docTitleOf(ctx).toHtmlEscaped());
    out += QStringLiteral("<div class=\"meta\">由 AdaptNote 导出 · %1 · 共 %2 条笔记</div>\n")
               .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm")))
               .arg(ctx.markups.size());

    // one note as an HTML block, shared by both layouts
    auto renderNote = [&](const Markup& m) {
        out += QStringLiteral("<div class=\"item\">\n<span class=\"type\">%1</span>")
                   .arg(typeLabel(m.type));
        if (!m.text.isEmpty()) {
            QString text = m.text.toHtmlEscaped();
            text.replace(QLatin1Char('\n'), QStringLiteral("<br>"));
            out += QStringLiteral("<div class=\"excerpt\">%1</div>\n").arg(text);
        }
        if (!m.comment.isEmpty())
            out += QStringLiteral("<div class=\"comment\">批注：%1</div>\n")
                       .arg(m.comment.toHtmlEscaped());
        if (m.type == MarkupType::Image && ctx.includeImages && !m.imagePath.isEmpty()) {
            const QString uri = imageDataUri(m.imagePath);
            if (!uri.isEmpty())
                out += QStringLiteral("<img class=\"img\" src=\"%1\">\n").arg(uri);
        }
        if (!m.tags.isEmpty())
            out += QStringLiteral("<div class=\"tags\">#%1</div>\n")
                       .arg(m.tags.join(QStringLiteral(" #")).toHtmlEscaped());
        out += QStringLiteral("</div>\n");
    };

    if (ctx.byGroups) {
        for (const mind::Node& rootNode : mindRoots(ctx)) {
            if (ctx.groupByDocument)
                out += QStringLiteral("<h2>%1</h2>\n").arg(rootNode.text.toHtmlEscaped());
            std::function<void(const mind::Node&, int)> walk = [&](const mind::Node& node,
                                                                  int depth) {
                if (!node.markupId.isEmpty()) {
                    const Markup m = lookupMarkup(ctx, node.markupId);
                    if (!m.id.isEmpty()) renderNote(m);
                    return;
                }
                if (!node.text.isEmpty()) {
                    const int level = qBound(2, depth + 2, 5);
                    out += QStringLiteral("<h%1>%2</h%1>\n")
                               .arg(level)
                               .arg(node.text.toHtmlEscaped());
                }
                for (const mind::Node& child : node.children) walk(child, depth + 1);
            };
            for (const mind::Node& child : rootNode.children) walk(child, 0);
        }
        out += QStringLiteral("</div>\n</body>\n</html>\n");
        return writeFile(ctx.outPath, out.toUtf8(), error);
    }

    int page = -1;
    QString book;
    for (const Markup& m : sorted(ctx)) {
        const QString thisBook = bookNameOf(ctx, m);
        if (ctx.groupByDocument && thisBook != book) {
            book = thisBook;
            page = -1;
            out += QStringLiteral("<h2>%1</h2>\n").arg(book.toHtmlEscaped());
        }
        if (m.page != page) {
            page = m.page;
            out += ctx.groupByDocument ? QStringLiteral("<h3>第 %1 页</h3>\n").arg(page + 1)
                                       : QStringLiteral("<h2>第 %1 页</h2>\n").arg(page + 1);
        }
        renderNote(m);
    }

    out += QStringLiteral("</div>\n</body>\n</html>\n");
    return writeFile(ctx.outPath, out.toUtf8(), error);
}

// ----------------------------------------------------------------------- opml

bool toOpml(const ExportContext& ctx, QString* error)
{
    QString out;
    out += QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<opml version=\"2.0\">\n");
    out += QStringLiteral("  <head>\n    <title>%1</title>\n  </head>\n")
               .arg(xmlEscape(docTitleOf(ctx)));
    out += QStringLiteral("  <body>\n");

    // The outline mirrors the mind map: groups nest, notes without a group fall back
    // to page buckets.
    const QVector<mind::Node> roots = mindRoots(ctx);
    QHash<QString, Markup> noteIndex;
    for (const Markup& m : ctx.markups) noteIndex.insert(m.id, m);

    if (ctx.groupByDocument && roots.size() > 1) {
        for (const mind::Node& rootNode : roots) {
            QString inner;
            for (const mind::Node& child : rootNode.children)
                appendOutline(inner, child, 3, &noteIndex);
            out += QStringLiteral("    <outline text=\"%1\">\n").arg(xmlEscape(rootNode.text));
            out += inner;
            out += QStringLiteral("    </outline>\n");
        }
    } else {
        for (const mind::Node& rootNode : roots)
            for (const mind::Node& child : rootNode.children)
                appendOutline(out, child, 2, &noteIndex);
    }

    out += QStringLiteral("  </body>\n</opml>\n");
    return writeFile(ctx.outPath, out.toUtf8(), error);
}

// ---------------------------------------------------------------------- xmind

bool toXmind(const ExportContext& ctx, QString* error)
{
    const QVector<mind::Node> roots = mindRoots(ctx);

    QString xml;
    xml += QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n");
    xml += QStringLiteral(
        "<xmap-content xmlns=\"urn:xmind:xmap:xmlns:content:2.0\" "
        "xmlns:fo=\"http://www.w3.org/1999/XSL/Format\" "
        "xmlns:svg=\"http://www.w3.org/2000/svg\" "
        "xmlns:xhtml=\"http://www.w3.org/1999/xhtml\" "
        "xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
        "modified-by=\"AdaptNote\" timestamp=\"%1\" version=\"2.0\">\n")
               .arg(QDateTime::currentMSecsSinceEpoch());

    auto topicId = []() {
        return QUuid::createUuid().toString(QUuid::WithoutBraces).remove(QLatin1Char('-'));
    };

    // one sheet per document; the sheet's root topic is the book title
    for (const mind::Node& rootNode : roots) {
        xml += QStringLiteral("  <sheet id=\"%1\" theme=\"0kdeemiijde6nuk97e4t0vpp54\" timestamp=\"%2\">\n")
                   .arg(topicId())
                   .arg(QDateTime::currentMSecsSinceEpoch());
        xml += QStringLiteral("    <topic id=\"%1\" structure-class=\"org.xmind.ui.map.unbalanced\" timestamp=\"%2\">\n")
                   .arg(topicId())
                   .arg(QDateTime::currentMSecsSinceEpoch());
        xml += QStringLiteral("      <title>%1</title>\n").arg(xmlEscape(rootNode.text));
        if (!rootNode.children.isEmpty()) {
            xml += QStringLiteral("      <children>\n        <topics type=\"attached\">\n");
            for (const mind::Node& child : rootNode.children) appendTopic(xml, child, 4);
            xml += QStringLiteral("        </topics>\n      </children>\n");
        }
        xml += QStringLiteral("    </topic>\n");
        xml += QStringLiteral("    <title>%1</title>\n")
                   .arg(xmlEscape(rootNode.text.isEmpty() ? QStringLiteral("Sheet 1")
                                                          : rootNode.text));
        xml += QStringLiteral("  </sheet>\n");
    }
    xml += QStringLiteral("</xmap-content>\n");

    const QString manifest = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
        "<manifest xmlns=\"urn:xmind:xmap:xmlns:manifest:1.0\">\n"
        "  <file-entry full-path=\"content.xml\" media-type=\"text/xml\"/>\n"
        "  <file-entry full-path=\"meta.xml\" media-type=\"text/xml\"/>\n"
        "  <file-entry full-path=\"META-INF/\" media-type=\"\"/>\n"
        "  <file-entry full-path=\"META-INF/manifest.xml\" media-type=\"text/xml\"/>\n"
        "</manifest>\n");
    const QString meta = QStringLiteral(
        "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
        "<meta xmlns=\"urn:xmind:xmap:xmlns:meta:2.0\" version=\"2.0\">\n"
        "  <Author><Name>AdaptNote</Name></Author>\n</meta>\n");

    ZipWriter zw(ctx.outPath);
    if (!zw.isOpen()) {
        if (error) *error = QStringLiteral("cannot create %1").arg(ctx.outPath);
        return false;
    }
    if (!zw.addFile(QStringLiteral("content.xml"), xml.toUtf8()) ||
        !zw.addFile(QStringLiteral("meta.xml"), meta.toUtf8()) ||
        !zw.addFile(QStringLiteral("META-INF/manifest.xml"), manifest.toUtf8()) ||
        !zw.close(error)) {
        if (error && error->isEmpty()) *error = QStringLiteral("cannot write xmind archive");
        return false;
    }
    return true;
}

// ----------------------------------------------------------------------- anki

bool toAnki(const ExportContext& ctx, QString* error)
{
    QTemporaryDir tmp;
    if (!tmp.isValid()) {
        if (error) *error = QStringLiteral("cannot create temporary dir");
        return false;
    }
    const QString dbPath = tmp.path() + "/collection.anki2";
    QVector<QPair<QString, QString>> media;
    if (!buildAnkiCollection(ctx, dbPath, &media, error)) return false;

    QJsonObject mediaJson;
    for (int i = 0; i < media.size(); ++i) mediaJson[QString::number(i)] = media.at(i).first;

    ZipWriter zw(ctx.outPath);
    if (!zw.isOpen()) {
        if (error) *error = QStringLiteral("cannot create %1").arg(ctx.outPath);
        return false;
    }
    if (!zw.addFileFromPath(QStringLiteral("collection.anki2"), dbPath)) {
        if (error) *error = QStringLiteral("cannot add collection to archive");
        return false;
    }
    for (int i = 0; i < media.size(); ++i)
        zw.addFileFromPath(QString::number(i), media.at(i).second);
    zw.addFile(QStringLiteral("media"),
               QJsonDocument(mediaJson).toJson(QJsonDocument::Compact));
    if (!zw.close(error)) return false;
    return true;
}

// ------------------------------------------------------------------- dispatch

bool byExtension(const ExportContext& ctx, QString* error)
{
    const QString ext = QFileInfo(ctx.outPath).suffix().toLower();
    if (ext == QStringLiteral("md") || ext == QStringLiteral("markdown")) return toMarkdown(ctx, error);
    if (ext == QStringLiteral("html") || ext == QStringLiteral("htm")) return toHtml(ctx, error);
    if (ext == QStringLiteral("opml")) return toOpml(ctx, error);
    if (ext == QStringLiteral("xmind")) return toXmind(ctx, error);
    if (ext == QStringLiteral("apkg")) return toAnki(ctx, error);
    if (error) *error = QStringLiteral("unsupported export format: %1").arg(ext);
    return false;
}

}  // namespace exports
}  // namespace an
