#include "import/import.h"

#include "core/store.h"

#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace an {

QString ImportReport::summary() const
{
    return QStringLiteral("导入：%1 个文档 / %2 条笔记（跳过 %3 行）")
        .arg(documents)
        .arg(notes)
        .arg(skipped);
}

namespace imports {
namespace {

const QRegularExpression& pageHeading()
{
    static const QRegularExpression re(QStringLiteral("^#{2,3}\\s*(?:第\\s*(\\d+)\\s*页|Page\\s*(\\d+))"),
                                       QRegularExpression::CaseInsensitiveOption);
    return re;
}

const QRegularExpression& bookHeading()
{
    static const QRegularExpression re(QStringLiteral("^#\\s+(.+)$"));
    return re;
}

// "- **高亮** · #tag  #tag2"  /  "- **Highlight**"
const QRegularExpression& bullet()
{
    static const QRegularExpression re(QStringLiteral("^\\s*[-*]\\s*\\*\\*(.+?)\\*\\*(.*)$"));
    return re;
}

MarkupType typeFromLabel(const QString& label)
{
    const QString l = label.trimmed().toLower();
    if (l.contains(QStringLiteral("下划线")) || l.contains(QLatin1String("underline")))
        return MarkupType::Underline;
    if (l.contains(QStringLiteral("波浪")) || l.contains(QLatin1String("squiggly")))
        return MarkupType::Squiggly;
    if (l.contains(QStringLiteral("批注")) || l.contains(QStringLiteral("笔记")) ||
        l.contains(QLatin1String("note")))
        return MarkupType::Note;
    if (l.contains(QStringLiteral("摘录图")) || l.contains(QStringLiteral("图片")) ||
        l.contains(QLatin1String("image")))
        return MarkupType::Image;
    if (l.contains(QStringLiteral("手写")) || l.contains(QLatin1String("ink")))
        return MarkupType::Ink;
    return MarkupType::Highlight;
}

QStringList tagsFromRest(const QString& rest)
{
    QStringList tags;
    static const QRegularExpression re(QStringLiteral("#([^#\\s·]+)"));
    auto it = re.globalMatch(rest);
    while (it.hasNext()) tags.append(it.next().captured(1));
    return tags;
}

QString stripMarkdown(const QString& line)
{
    QString s = line;
    s.remove(QRegularExpression(QStringLiteral("^\\s*>\\s?")));     // blockquote
    s.replace(QLatin1String("&lt;"), QLatin1String("<"));
    s.replace(QLatin1String("&gt;"), QLatin1String(">"));
    s.replace(QLatin1String("&amp;"), QLatin1String("&"));
    return s;
}

}  // namespace

ImportedDocument fromAdaptNoteJson(const QString& path, QString* error)
{
    ImportedDocument out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return out;
    }
    const QByteArray bytes = f.readAll();
    const QJsonDocument doc = QJsonDocument::fromJson(bytes);
    if (!doc.isObject()) {
        if (error) *error = QStringLiteral("not a JSON object");
        return out;
    }
    const QJsonObject root = doc.object();
    out.title = root.value("docName").toString(QFileInfo(path).completeBaseName());
    const QString docPath = root.value("docPath").toString();
    if (!docPath.isEmpty()) out.docPath = docPath;

    const QJsonArray list = root.value("markups").toArray();
    for (const QJsonValue& v : list) {
        Markup m = Markup::fromJson(v.toObject());
        if (m.id.isEmpty()) m.id = newId();
        if (m.created == 0) m.created = QDateTime::currentMSecsSinceEpoch();
        if (m.updated == 0) m.updated = m.created;
        if (m.quads.isEmpty() && m.type != MarkupType::Ink) {
            out.warnings << QStringLiteral("a note without geometry kept by page");
        }
        out.markups.append(m);
    }
    return out;
}

ImportedDocument fromMarkdown(const QString& path, QString* error)
{
    ImportedDocument out;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return out;
    }
    const QString text = QString::fromUtf8(f.readAll());
    out.title = QFileInfo(path).completeBaseName();

    int page = -1;
    Markup current;
    bool inItem = false;
    int skipped = 0;

    auto flush = [&]() {
        if (!inItem) return;
        inItem = false;
        if (current.text.trimmed().isEmpty() && current.comment.trimmed().isEmpty() &&
            current.type != MarkupType::Note)
            return;
        if (current.page < 0) current.page = 0;
        current.id = newId();
        current.created = current.updated = QDateTime::currentMSecsSinceEpoch();
        if (current.quads.isEmpty())
            current.quads = {Quad::fromRect(QRectF(40, 40, 200, 16))};   // placeholder box
        out.markups.append(current);
        current = Markup();
    };

    for (const QString& raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw;
        if (line.trimmed().isEmpty()) continue;

        const auto bh = bookHeading().match(line);
        if (bh.hasMatch() && page < 0) {
            out.title = bh.captured(1).trimmed();
            continue;
        }
        const auto ph = pageHeading().match(line);
        if (ph.hasMatch()) {
            flush();
            const QString num = ph.captured(1).isEmpty() ? ph.captured(2) : ph.captured(1);
            page = num.toInt() - 1;
            continue;
        }
        const auto bl = bullet().match(line);
        if (bl.hasMatch()) {
            flush();
            inItem = true;
            current.page = page;
            current.type = typeFromLabel(bl.captured(1));
            current.tags = tagsFromRest(bl.captured(2));
            continue;
        }
        if (line.startsWith(QLatin1String("  >")) || line.startsWith(QLatin1String(">"))) {
            if (!inItem) {
                ++skipped;   // a quote without an owning bullet
                continue;
            }
            const QString piece = stripMarkdown(line).trimmed();
            if (!current.text.isEmpty()) current.text += QLatin1Char('\n');
            current.text += piece;
            continue;
        }
        if (line.contains(QStringLiteral("批注："))) {
            if (!inItem) {
                ++skipped;
                continue;
            }
            const QString piece = line.section(QStringLiteral("批注："), 1).trimmed();
            if (!current.comment.isEmpty()) current.comment += QLatin1Char('\n');
            current.comment += piece;
            continue;
        }
        ++skipped;
    }
    flush();

    if (out.markups.isEmpty() && error)
        *error = QStringLiteral("no notes found in the markdown");
    out.warnings << QStringLiteral("skipped %1 unrelated lines").arg(skipped);
    return out;
}

ImportedDocument detect(const QString& path, const QString& targetDoc, QString* error)
{
    const QFileInfo fi(path);
    if (!fi.exists()) {
        if (error) *error = QStringLiteral("file not found: %1").arg(path);
        return {};
    }

    ImportedDocument out;
    if (fi.suffix().compare(QLatin1String("json"), Qt::CaseInsensitive) == 0) {
        out = fromAdaptNoteJson(path, error);
    } else {
        out = fromMarkdown(path, error);
    }

    if (!targetDoc.isEmpty()) out.docPath = targetDoc;
    if (out.docPath.isEmpty()) out.docPath = fi.absolutePath() + QLatin1Char('/') + out.title;
    for (Markup& m : out.markups) m.docPath = out.docPath;
    return out;
}

ImportedDocument fromPdfAnnotations(IDocumentEngine& engine, const QString& docPath,
                                    const QVector<Markup>& existing)
{
    ImportedDocument out;
    out.docPath = docPath;
    out.title = QFileInfo(docPath).completeBaseName();
    if (!engine.isOpen()) return out;

    auto sameAsExisting = [&](const Markup& candidate) {
        for (const Markup& m : existing) {
            if (m.deleted || m.page != candidate.page || m.type != candidate.type) continue;
            if (m.quads.isEmpty() || candidate.quads.isEmpty()) continue;
            const QRectF a = m.quads.first().boundingRect();
            const QRectF b = candidate.quads.first().boundingRect();
            if (qAbs(a.x() - b.x()) < 2.0 && qAbs(a.y() - b.y()) < 2.0 &&
                qAbs(a.width() - b.width()) < 2.0 && qAbs(a.height() - b.height()) < 2.0)
                return true;
        }
        return false;
    };

    for (int page = 0; page < engine.pageCount(); ++page) {
        const QVector<Markup> pageNotes = engine.readAnnotations(page);
        for (Markup m : pageNotes) {
            if (sameAsExisting(m)) {
                out.warnings << QStringLiteral("skipped an already known annotation");
                continue;
            }
            m.docPath = docPath;
            out.markups.append(m);
        }
    }
    return out;
}

ImportReport commit(Store& store, const QVector<ImportedDocument>& documents, QString* error)
{
    ImportReport report;
    for (const ImportedDocument& doc : documents) {
        if (doc.markups.isEmpty()) continue;
        QVector<Markup> batch;
        batch.reserve(doc.markups.size());
        for (Markup m : doc.markups) {
            m.docPath = doc.docPath;
            m = store.stampForLocalWrite(m);
            batch.append(m);
        }
        if (!store.upsertMarkups(batch, error)) return report;
        store.touchDocument(doc.docPath, doc.title, 0);
        report.notes += batch.size();
        ++report.documents;
        report.warnings += doc.warnings;
    }
    return report;
}

}  // namespace imports
}  // namespace an
