#include "vocab/vocab.h"

#include <QAtomicInt>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTextStream>

namespace an {
namespace vocab {
namespace {

QString stripMarkdown(const QString& s)
{
    QString out = s;
    out.remove(QLatin1Char('`'));
    out.remove(QLatin1Char('*'));
    return out.trimmed();
}

QString field(const QString& text, const QStringList& labels)
{
    for (const QString& label : labels) {
        // Match "音标: xxx" and "音标：xxx" (models mix full-width colons).
        const QRegularExpression re(QStringLiteral("(?:^|\\n)\\s*%1\\s*[:：]\\s*(.+)")
                                        .arg(QRegularExpression::escape(label)));
        const auto m = re.match(text);
        if (m.hasMatch()) return stripMarkdown(m.captured(1)).trimmed();
    }
    return {};
}

QString oneLine(QString s)
{
    s.replace(QRegularExpression(QStringLiteral("\\s*\\n+\\s*")), QStringLiteral("; "));
    return s.trimmed();
}

}  // namespace

QString normalizeQuery(const QString& selection)
{
    const QString text = selection.simplified();
    if (text.isEmpty()) return {};

    // Prefer a Latin word: selections in an English book are often whole sentences.
    static const QRegularExpression latin(QStringLiteral("[A-Za-z][A-Za-z'’\\-]{1,45}"));
    const auto m = latin.match(text);
    if (m.hasMatch()) return m.captured(0).toLower();

    // Otherwise keep a short CJK term rather than the whole paragraph.
    return text.left(40);
}

QString promptForLookup(const QString& word)
{
    return QStringLiteral("查询词条「%1」。严格输出三行，不要解释，不要 Markdown：\n"
                          "音标: ...\n释义: ...\n例句: ...\n"
                          "若这个词有多个常见词性，用分号分隔放在“释义”一行。"
                          "例句给一句自然的英文原句并附中文翻译。")
        .arg(word);
}

bool parseAiDefinition(const QString& answer, Definition* out)
{
    if (!out) return false;
    const QString text = answer.trimmed();
    if (text.isEmpty()) return false;

    const QString phonetic = field(text, {QStringLiteral("音标"), QStringLiteral("phonetic")});
    const QString meaning = field(text, {QStringLiteral("释义"), QStringLiteral("meaning"),
                                         QStringLiteral("意思")});
    const QString example = field(text, {QStringLiteral("例句"), QStringLiteral("example")});

    out->phonetic = phonetic;
    out->example = oneLine(example);
    if (!meaning.isEmpty()) {
        out->meaning = oneLine(meaning);
    } else {
        // No labels at all: keep the answer, but drop the phonetic line if present.
        QString fallback = text;
        fallback.remove(QRegularExpression(QStringLiteral("(?:^|\\n)\\s*音标\\s*[:：].*")));
        out->meaning = oneLine(fallback);
    }
    out->ok = !out->meaning.isEmpty();
    if (!out->ok) out->error = QStringLiteral("模型没有返回可解析的释义");
    return out->ok;
}

Definition lookupLocal(const QString& dbPath, const QString& word)
{
    Definition def;
    def.word = word;
    if (!QFileInfo::exists(dbPath) || word.trimmed().isEmpty()) return def;

    static QAtomicInt counter(0);
    const QString conn = QStringLiteral("adaptnote-ecdict-%1").arg(counter.fetchAndAddOrdered(1));
    {
        QSqlDatabase db = QSqlDatabase::addDatabase(QStringLiteral("QSQLITE"), conn);
        db.setDatabaseName(dbPath);
        db.setConnectOptions(QStringLiteral("QSQLITE_OPEN_READONLY"));
        if (db.open()) {
            QSqlQuery q(db);
            q.prepare(QStringLiteral(
                "select word, phonetic, translation, definition from stardict "
                "where word = ? collate nocase limit 1"));
            q.addBindValue(word);
            if (q.exec() && q.next()) {
                def.word = q.value(0).toString();
                def.phonetic = q.value(1).toString().trimmed();
                const QString translation = q.value(2).toString();
                const QString definition = q.value(3).toString();
                def.meaning = oneLine(translation.isEmpty() ? definition : translation);
                def.source = QStringLiteral("ecdict");
                def.ok = !def.meaning.isEmpty();
            }
            db.close();
        }
        // The connection handle must be gone before removeDatabase, otherwise Qt warns.
    }
    QSqlDatabase::removeDatabase(conn);
    return def;
}

bool exportWords(const QVector<WordEntry>& words, const QString& path, bool anki,
                 QString* error)
{
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = f.errorString();
        return false;
    }
    QTextStream out(&f);
    out.setEncoding(QStringConverter::Utf8);
    out.setGenerateByteOrderMark(true);

    auto clean = [](QString s) {
        s.replace(QLatin1Char('\t'), QLatin1Char(' '));
        s.replace(QLatin1Char('\n'), QLatin1Char(' '));
        return s;
    };

    if (anki) {
        // Anki's "basic" import: front \t back. Phonetic and example go on the back.
        for (const WordEntry& w : words) {
            QString back = w.meaning;
            if (!w.phonetic.isEmpty()) back = QStringLiteral("[%1] ").arg(w.phonetic) + back;
            if (!w.example.isEmpty()) back += QStringLiteral("<br>") + w.example;
            out << clean(w.word) << '\t' << clean(back) << '\n';
        }
    } else {
        out << QStringLiteral("单词\t音标\t释义\t例句\n");
        for (const WordEntry& w : words) {
            out << clean(w.word) << '\t' << clean(w.phonetic) << '\t'
                << clean(w.meaning) << '\t' << clean(w.example) << '\n';
        }
    }
    return true;
}

QString dueLabel(const WordEntry& w, qint64 nowMs)
{
    if (nowMs <= 0) nowMs = QDateTime::currentMSecsSinceEpoch();
    if (w.dueAt <= nowMs) return QStringLiteral("待复习");
    const qint64 days = (w.dueAt - nowMs + 86399999) / 86400000;
    if (days <= 1) return QStringLiteral("明天");
    return QStringLiteral("%1 天后").arg(days);
}

}  // namespace vocab
}  // namespace an
