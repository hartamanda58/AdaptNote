#include "core/store.h"

#include "sync/vv.h"

#include <QAtomicInt>
#include <QCryptographicHash>
#include <QDateTime>
#include <QHash>
#include <QSet>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QVariant>

#include <algorithm>

namespace an {
namespace {

QString quadsToJson(const Markup& m)
{
    // Hot path (every note write): build the tiny array by hand instead of going
    // through QJsonDocument, which allocates several temporaries per quad.
    QString out;
    out.reserve(m.quads.size() * 120 + 2);
    out += QLatin1Char('[');
    for (int i = 0; i < m.quads.size(); ++i) {
        const Quad& q = m.quads.at(i);
        if (i) out += QLatin1Char(',');
        out += QStringLiteral("{\"ul\":[%1,%2],\"ur\":[%3,%4],\"ll\":[%5,%6],\"lr\":[%7,%8]}")
                   .arg(q.ul.x(), 0, 'g', 10)
                   .arg(q.ul.y(), 0, 'g', 10)
                   .arg(q.ur.x(), 0, 'g', 10)
                   .arg(q.ur.y(), 0, 'g', 10)
                   .arg(q.ll.x(), 0, 'g', 10)
                   .arg(q.ll.y(), 0, 'g', 10)
                   .arg(q.lr.x(), 0, 'g', 10)
                   .arg(q.lr.y(), 0, 'g', 10);
    }
    out += QLatin1Char(']');
    return out;
}

QVector<Quad> quadsFromJson(const QString& s)
{
    QVector<Quad> out;
    const QJsonArray arr = QJsonDocument::fromJson(s.toUtf8()).array();
    for (const QJsonValue& v : arr) out.append(Quad::fromJson(v.toObject()));
    return out;
}

QString tagsToText(const QStringList& tags)
{
    return tags.join(QLatin1Char(','));
}

QStringList tagsFromText(const QString& s)
{
    QStringList out;
    for (const QString& t : s.split(QLatin1Char(','), Qt::SkipEmptyParts)) {
        const QString trimmed = t.trimmed();
        if (!trimmed.isEmpty()) out.append(trimmed);
    }
    out.removeDuplicates();
    return out;
}

Markup readMarkup(const QSqlQuery& q)
{
    Markup m;
    m.page = q.value(0).toInt();
    m.type = markupTypeFromName(q.value(1).toString());
    m.color = QColor(q.value(2).toString());
    if (!m.color.isValid()) m.color = QColor(255, 213, 0);
    m.quads = quadsFromJson(q.value(3).toString());
    m.text = q.value(4).toString();
    m.comment = q.value(5).toString();
    m.created = q.value(6).toLongLong();
    m.updated = q.value(7).toLongLong();
    m.writtenToPdf = q.value(8).toInt() != 0;
    m.id = q.value(9).toString();
    m.docPath = q.value(10).toString();
    m.tags = tagsFromText(q.value(11).toString());
    m.imagePath = q.value(12).toString();
    m.vv = q.value(13).toString();
    m.deviceId = q.value(14).toString();
    m.deleted = q.value(15).toInt() != 0;
    m.conflictedFrom = q.value(16).toString();
    for (const QJsonValue& v : QJsonDocument::fromJson(q.value(17).toString().toUtf8()).array())
        m.ink.append(InkPoint::fromJson(v.toArray()));
    m.mindGroup = q.value(18).toString();
    m.mindOrder = q.value(19).toInt();
    m.anchorChapter = q.value(20).toString();
    m.anchorOffset = q.value(21).toString().isEmpty() ? -1 : q.value(21).toInt();
    return m;
}

const char* kSelectCols =
    "select page,type,color,quads,text,comment,created,updated,written,id,docPath,tags,imagePath,"
    "vv,deviceId,deleted,conflictedFrom,ink,mindGroup,mindOrder,anchorChapter,anchorOffset"
    " from markups";

// QString() is null and binds as SQL NULL; every "? = ''" style guard in this
// file assumes an empty string, so normalize before binding.
QString nonNull(const QString& s)
{
    return s.isNull() ? QString::fromLatin1("") : s;
}

QString likePattern(const QString& q)
{
    QString escaped = q;
    escaped.replace(QLatin1Char('\\'), QLatin1String("\\\\"));
    escaped.replace(QLatin1Char('%'), QLatin1String("\\%"));
    escaped.replace(QLatin1Char('_'), QLatin1String("\\_"));
    return QLatin1Char('%') + escaped + QLatin1Char('%');
}

}  // namespace

void Store::repairSearchIndex()
{
    // note: called from ensureSchema(), i.e. before open_ becomes true
    if (!fts_ || !QSqlDatabase::contains(conn_)) return;
    QSqlQuery q(QSqlDatabase::database(conn_));

    // cheap guard: only walk the tables when the row counts disagree
    auto count = [&](const char* sql) {
        qint64 n = -1;
        if (q.exec(QString::fromLatin1(sql)) && q.next()) n = q.value(0).toLongLong();
        return n;
    };
    const qint64 live = count("select count(*) from notes where deleted=0");
    const qint64 indexed = count("select count(*) from notes_fts");
    if (live < 0 || indexed < 0 || live == indexed) return;

    // drop rows of notes that are gone (or tombstoned), then index whatever is missing
    q.exec("delete from notes_fts where id not in (select id from notes where deleted=0)");
    if (!q.exec("insert into notes_fts(id,docName,text,comment,tags)"
                " select id, docName, text, comment, tags from notes"
                " where deleted=0 and id not in (select id from notes_fts)"))
        return;

    indexedFingerprint_.clear();
    knownIndexLoaded_ = false;
    knownIndexIds_.clear();
}

void Store::loadKnownIndexIds() const
{
    if (knownIndexLoaded_) return;
    knownIndexLoaded_ = true;
    if (!fts_) return;
    QSqlQuery q(QSqlDatabase::database(conn_));
    if (!q.exec("select id from notes_fts")) return;
    while (q.next()) knownIndexIds_.insert(q.value(0).toString());
}

void Store::setFastImport(bool on)
{
    if (!open_ || fastImport_ == on) return;
    fastImport_ = on;
    QSqlQuery q(QSqlDatabase::database(conn_));
    if (on) {
        q.exec("pragma synchronous=off");
        q.exec("pragma temp_store=memory");
    } else {
        // back to the normal WAL settings
        q.exec("pragma synchronous=normal");
        q.exec("pragma temp_store=default");
        q.exec("pragma wal_checkpoint(truncate)");
    }
}

QSqlQuery* Store::prepared(const QString& sql)
{
    auto it = stmtCache_.find(sql);
    if (it != stmtCache_.end()) {
        it.value()->finish();
        return it.value();
    }
    auto* q = new QSqlQuery(QSqlDatabase::database(conn_));
    if (!q->prepare(sql)) {
        delete q;
        return nullptr;
    }
    stmtCache_.insert(sql, q);
    return q;
}

void Store::clearPrepared()
{
    for (QSqlQuery* q : stmtCache_) delete q;
    stmtCache_.clear();
}

QString Store::dataDir() const
{
    return QFileInfo(dbPath_).absolutePath();
}

QString Store::imagesDir() const
{
    return dataDir() + "/images";
}

bool Store::open(const QString& dbPath, QString* error)
{
    dbPath_ = dbPath;
    static QAtomicInt counter(0);
    conn_ = QStringLiteral("adaptnote-store-%1").arg(counter.fetchAndAddOrdered(1));
    QDir().mkpath(dataDir());
    QDir().mkpath(imagesDir());

    QSqlDatabase db = QSqlDatabase::contains(conn_) ? QSqlDatabase::database(conn_)
                                                    : QSqlDatabase::addDatabase("QSQLITE", conn_);
    db.setDatabaseName(dbPath_);
    if (!db.open()) {
        if (error) *error = db.lastError().text();
        return false;
    }

    QSqlQuery pragma(db);
    pragma.exec("pragma journal_mode=WAL");
    pragma.exec("pragma synchronous=normal");

    if (!ensureSchema(error)) return false;
    open_ = true;
    return true;
}

void Store::ensureColumn(const QString& table, const QString& column, const QString& type)
{
    QSqlDatabase db = QSqlDatabase::database(conn_);
    QSqlQuery info(db);
    if (!info.exec(QStringLiteral("pragma table_info(%1)").arg(table))) return;
    bool found = false;
    while (info.next()) {
        if (info.value(1).toString() == column) {
            found = true;
            break;
        }
    }
    if (!found) {
        QSqlQuery alter(db);
        alter.exec(QStringLiteral("alter table %1 add column %2 %3").arg(table, column, type));
    }
}

bool Store::ensureSchema(QString* error)
{
    QSqlDatabase db = QSqlDatabase::database(conn_);
    QSqlQuery q(db);

    const QStringList ddl = {
        "create table if not exists documents ("
        " path text primary key, title text, pages integer, lastPage integer,"
        " lastOffset real, openedAt integer, fileSize integer, fileMtime integer)",
        "create table if not exists markups ("
        " id text primary key, docPath text, page integer, type text, color text,"
        " quads text, text text, comment text, created integer, updated integer,"
        " written integer, tags text, imagePath text, vv text, deviceId text,"
        " deleted integer default 0, conflictedFrom text, ink text,"
        " mindGroup text, mindOrder integer default 0,"
        " anchorChapter text, anchorOffset integer default -1)",
        "create index if not exists idx_markups_doc on markups(docPath, page)",
        "create index if not exists idx_markups_updated on markups(updated)",
        "create table if not exists settings (key text primary key, value text)",
        "create table if not exists mind_groups ("
        " id text primary key, docPath text, name text, parentId text, [order] integer default 0,"
        " updated integer, deleted integer default 0, vv text, deviceId text)",
        "create index if not exists idx_groups_doc on mind_groups(docPath)",
        "create index if not exists idx_groups_parent on mind_groups(parentId)",
    };
    for (const QString& s : ddl) {
        if (!q.exec(s)) {
            if (error) *error = q.lastError().text();
            return false;
        }
    }

    // migrations for databases created by v0.1
    ensureColumn("documents", "lastOffset", "real");
    ensureColumn("documents", "fileSize", "integer");
    ensureColumn("documents", "fileMtime", "integer");
    ensureColumn("markups", "tags", "text");
    ensureColumn("markups", "imagePath", "text");
    ensureColumn("markups", "vv", "text");
    ensureColumn("markups", "deviceId", "text");
    ensureColumn("markups", "deleted", "integer default 0");
    ensureColumn("markups", "conflictedFrom", "text");
    ensureColumn("markups", "ink", "text");
    ensureColumn("markups", "mindGroup", "text");
    ensureColumn("markups", "mindOrder", "integer default 0");
    ensureColumn("mind_groups", "parentId", "text");
    ensureColumn("markups", "anchorChapter", "text");
    ensureColumn("markups", "anchorOffset", "integer default -1");

    // v0.6 stored the group *name* in markups.mindGroup; from v0.7 it stores a group id.
    // Adopt any leftover name into a real group row (deterministic id so two devices
    // migrating the same data converge) and rewrite the notes.
    {
        QSqlQuery legacy(db);
        if (legacy.exec("select distinct docPath, mindGroup from markups"
                        " where mindGroup is not null and mindGroup <> ''")) {
            QVector<QPair<QString, QString>> adopt;
            while (legacy.next())
                adopt.append({legacy.value(0).toString(), legacy.value(1).toString()});
            for (const auto& row : adopt) {
                const QString docPath = row.first;
                const QString value = row.second;
                QSqlQuery known(db);
                known.prepare("select 1 from mind_groups where id=? or (docPath=? and name=?)");
                known.addBindValue(value);
                known.addBindValue(docPath);
                known.addBindValue(value);
                if (known.exec() && known.next()) continue;   // already an id or known name

                const QString id = QString::fromLatin1(
                    QCryptographicHash::hash((docPath + "|" + value).toUtf8(),
                                             QCryptographicHash::Sha1).toHex().left(16));
                QSqlQuery ins(db);
                ins.prepare("insert into mind_groups(id,docPath,name,[order],updated,deleted,vv,deviceId)"
                            " values(?,?,?,0,?,0,'','') on conflict(id) do nothing");
                ins.addBindValue(id);
                ins.addBindValue(docPath);
                ins.addBindValue(value);
                ins.addBindValue(QDateTime::currentMSecsSinceEpoch());
                ins.exec();

                QSqlQuery upd(db);
                upd.prepare("update markups set mindGroup=? where docPath=? and mindGroup=?");
                upd.addBindValue(id);
                upd.addBindValue(docPath);
                upd.addBindValue(value);
                upd.exec();
            }
        }
    }

    // FTS5 with the trigram tokenizer: works for CJK substring search, not just words.
    QSqlQuery fts(db);
    fts_ = fts.exec("create virtual table if not exists notes_fts using fts5("
                    "id unindexed, docName unindexed, text, comment, tags,"
                    " tokenize='trigram')");
    if (!fts_) return true;   // search falls back to LIKE; never fail the whole open here

    // Repair a missing or stale index. Without this, a database that predates the FTS table
    // keeps its notes invisible to search: queries of 3+ characters take the FTS path and
    // silently return nothing for them (the LIKE fallback is not consulted).
    repairSearchIndex();
    return true;
}

void Store::close()
{
    clearPrepared();
    indexedFingerprint_.clear();
    knownIndexIds_.clear();
    knownIndexLoaded_ = false;
    deviceIdCache_.clear();
    if (QSqlDatabase::contains(conn_)) {
        {
            QSqlDatabase db = QSqlDatabase::database(conn_, false);
            if (db.isOpen()) db.close();
        }
        QSqlDatabase::removeDatabase(conn_);
    }
    open_ = false;
}

void Store::touchDocument(const QString& path, const QString& title, int pages)
{
    if (!open_) return;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("insert into documents(path,title,pages,lastPage,lastOffset,openedAt) values(?,?,?,0,0,?)"
              " on conflict(path) do update set title=excluded.title, pages=excluded.pages,"
              " openedAt=excluded.openedAt");
    q.addBindValue(path);
    q.addBindValue(title);
    q.addBindValue(pages);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.exec();
}

void Store::setReadingState(const QString& path, int page, double offsetPt)
{
    if (!open_ || path.isEmpty()) return;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("update documents set lastPage=?, lastOffset=? where path=?");
    q.addBindValue(page);
    q.addBindValue(offsetPt);
    q.addBindValue(path);
    q.exec();
}

int Store::lastPage(const QString& path) const
{
    if (!open_) return 0;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("select lastPage from documents where path=?");
    q.addBindValue(path);
    if (q.exec() && q.next()) return q.value(0).toInt();
    return 0;
}

double Store::lastOffset(const QString& path) const
{
    if (!open_) return 0;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("select lastOffset from documents where path=?");
    q.addBindValue(path);
    if (q.exec() && q.next()) return q.value(0).toDouble();
    return 0;
}

bool Store::indexMarkup(const Markup& m)
{
    if (!fts_) return true;
    QSqlDatabase db = QSqlDatabase::database(conn_);

    // Re-indexing on every write (colour change, page move, sync touch) is expensive
    // and pointless: compare the searchable fields first. A bulk insert of new notes
    // must not pay for an FTS lookup per row, so the id set is loaded once.
    const QString fingerprint = m.text + QLatin1Char('\x1f') + m.comment +
                                QLatin1Char('\x1f') + tagsToText(m.tags);
    loadKnownIndexIds();
    if (indexedFingerprint_.value(m.id) == fingerprint) return true;

    const bool alreadyIndexed = knownIndexIds_.contains(m.id);
    indexedFingerprint_.insert(m.id, fingerprint);
    if (!alreadyIndexed) {
        knownIndexIds_.insert(m.id);
    } else {
        // compare against what the table holds before rewriting it
        QSqlQuery existing(db);
        existing.prepare("select text, comment, tags from notes_fts where id=?");
        existing.addBindValue(m.id);
        if (existing.exec() && existing.next()) {
            if (existing.value(0).toString() == m.text &&
                existing.value(1).toString() == m.comment &&
                existing.value(2).toString() == tagsToText(m.tags)) {
                return true;
            }
        }
    }

    // Both statements are hot (one pair per written note): keep them prepared.
    QSqlQuery* del = prepared(QStringLiteral("delete from notes_fts where id=?"));
    QSqlQuery* ins = prepared(
        QStringLiteral("insert into notes_fts(id,docName,text,comment,tags) values(?,?,?,?,?)"));
    if (!del || !ins) return false;

    del->addBindValue(m.id);
    del->exec();

    ins->addBindValue(m.id);
    ins->addBindValue(QFileInfo(m.docPath).completeBaseName());
    ins->addBindValue(m.text);
    ins->addBindValue(m.comment);
    ins->addBindValue(tagsToText(m.tags));
    return ins->exec();
}

void Store::unindexMarkup(const QString& id)
{
    if (!fts_) return;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("delete from notes_fts where id=?");
    q.addBindValue(id);
    q.exec();
}

bool Store::upsertMarkup(const Markup& m, QString* error, bool writeMirror)
{
    return upsertMarkupRow(m, error, writeMirror, true);
}

bool Store::upsertMarkups(const QVector<Markup>& markups, QString* error)
{
    if (!open_) {
        if (error) *error = QStringLiteral("store not open");
        return false;
    }
    if (markups.isEmpty()) return true;

    QSqlDatabase db = QSqlDatabase::database(conn_);
    db.transaction();
    bool ok = true;
    for (const Markup& m : markups) {
        if (!upsertMarkupRow(m, error, false, true)) {
            ok = false;
            break;
        }
    }
    if (!ok) {
        db.rollback();
        return false;
    }
    if (!db.commit()) {
        if (error) *error = db.lastError().text();
        return false;
    }
    // one mirror per affected document
    QStringList docsSeen;
    for (const Markup& m : markups) {
        if (m.docPath.isEmpty() || docsSeen.contains(m.docPath)) continue;
        docsSeen.append(m.docPath);
        exportMirror(m.docPath);
    }
    return true;
}

bool Store::upsertMarkupRow(const Markup& m, QString* error, bool writeMirror, bool indexFts)
{
    if (!open_) {
        if (error) *error = QStringLiteral("store not open");
        return false;
    }

    static const char* kUpsertSql =
        "insert into markups(id,docPath,page,type,color,quads,text,comment,created,updated,"
        "written,tags,imagePath,vv,deviceId,deleted,conflictedFrom,ink,mindGroup,mindOrder,"
        "anchorChapter,anchorOffset) values(?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)"
        " on conflict(id) do update set page=excluded.page, type=excluded.type,"
        " color=excluded.color, quads=excluded.quads, text=excluded.text,"
        " comment=excluded.comment, updated=excluded.updated, written=excluded.written,"
        " tags=excluded.tags, imagePath=excluded.imagePath, vv=excluded.vv,"
        " deviceId=excluded.deviceId, deleted=excluded.deleted,"
        " conflictedFrom=excluded.conflictedFrom, ink=excluded.ink,"
        " mindGroup=excluded.mindGroup, mindOrder=excluded.mindOrder,"
        " anchorChapter=excluded.anchorChapter, anchorOffset=excluded.anchorOffset";

    QSqlQuery* qp = prepared(QString::fromLatin1(kUpsertSql));
    if (!qp) {
        if (error) *error = QStringLiteral("cannot prepare the markup upsert");
        return false;
    }
    QSqlQuery& q = *qp;

    QJsonArray pts;
    for (const InkPoint& p : m.ink) pts.append(p.toJson());

    q.addBindValue(m.id);
    q.addBindValue(m.docPath);
    q.addBindValue(m.page);
    q.addBindValue(markupTypeName(m.type));
    q.addBindValue(m.color.name(QColor::HexRgb));
    q.addBindValue(quadsToJson(m));
    q.addBindValue(m.text);
    q.addBindValue(m.comment);
    q.addBindValue(m.created);
    q.addBindValue(m.updated);
    q.addBindValue(m.writtenToPdf ? 1 : 0);
    q.addBindValue(tagsToText(m.tags));
    q.addBindValue(m.imagePath);
    q.addBindValue(m.vv);
    q.addBindValue(m.deviceId);
    q.addBindValue(m.deleted ? 1 : 0);
    q.addBindValue(m.conflictedFrom);
    q.addBindValue(m.ink.isEmpty()
                       ? QString()
                       : QString::fromUtf8(QJsonDocument(pts).toJson(QJsonDocument::Compact)));
    q.addBindValue(m.mindGroup);
    q.addBindValue(m.mindOrder);
    q.addBindValue(m.anchorChapter);
    q.addBindValue(m.anchorOffset);
    if (!q.exec()) {
        if (error) *error = q.lastError().text();
        return false;
    }

    if (indexFts) indexMarkup(m);
    if (writeMirror) exportMirror(m.docPath);
    return true;
}

QVector<Markup> Store::markups(const QString& docPathIn, int page) const
{
    QVector<Markup> out;
    if (!open_) return out;
    const QString docPath = nonNull(docPathIn);
    QSqlQuery q(QSqlDatabase::database(conn_));
    if (page < 0) {
        q.prepare(QString::fromLatin1(kSelectCols) +
                  " where docPath=? and deleted=0 order by created desc");
        q.addBindValue(docPath);
    } else {
        q.prepare(QString::fromLatin1(kSelectCols) +
                  " where docPath=? and page=? and deleted=0 order by created desc");
        q.addBindValue(docPath);
        q.addBindValue(page);
    }
    if (!q.exec()) return out;
    while (q.next()) out.append(readMarkup(q));
    return out;
}

QVector<Markup> Store::searchMarkups(const QString& query, const QString& docPathIn) const
{
    QVector<Markup> out;
    if (!open_) return out;
    const QString docPath = nonNull(docPathIn);
    const QString needle = query.trimmed();
    if (needle.isEmpty()) return docPath.isEmpty() ? allMarkups() : markups(docPath);

    QSqlDatabase db = QSqlDatabase::database(conn_);
    QSqlQuery q(db);

    const bool useFts = fts_ && needle.size() >= 3;
    if (useFts) {
        QString escaped = needle;
        escaped.replace(QLatin1Char('"'), QLatin1String("\"\""));
        q.prepare(QString::fromLatin1(kSelectCols) +
                  " where id in (select id from notes_fts where notes_fts match ?)"
                  " and (? = '' or docPath = ?) order by updated desc");
        q.addBindValue(QStringLiteral("\"%1\"").arg(escaped));
        q.addBindValue(docPath);
        q.addBindValue(docPath);
        if (q.exec()) {
            while (q.next()) out.append(readMarkup(q));
            return out;
        }
    }

    // LIKE fallback: short queries (< 3 chars, below trigram), or FTS unavailable.
    const QString pattern = likePattern(needle);
    q.prepare(QString::fromLatin1(kSelectCols) +
              " where (text like ? escape '\\' or comment like ? escape '\\'"
              " or tags like ? escape '\\') and (? = '' or docPath = ?)"
              " order by updated desc");
    q.addBindValue(pattern);
    q.addBindValue(pattern);
    q.addBindValue(pattern);
    q.addBindValue(docPath);
    q.addBindValue(docPath);
    if (!q.exec()) return out;
    while (q.next()) out.append(readMarkup(q));
    return out;
}

QVector<Markup> Store::allMarkups(int limit) const
{
    QVector<Markup> out;
    if (!open_) return out;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare(QString::fromLatin1(kSelectCols) + " where deleted=0 order by updated desc limit ?");
    q.addBindValue(limit);
    if (!q.exec()) return out;
    while (q.next()) out.append(readMarkup(q));
    return out;
}

QVector<QPair<QString, int>> Store::tagCounts(const QString& docPathIn) const
{
    QHash<QString, int> counts;
    const QString docPath = nonNull(docPathIn);
    // An empty scope means "the whole library", not "document with an empty path".
    const QVector<Markup> all = docPath.isEmpty() ? allMarkups(100000) : markups(docPath);
    for (const Markup& m : all)
        for (const QString& t : m.tags) counts[t] += 1;

    QVector<QPair<QString, int>> out;
    out.reserve(counts.size());
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it) out.append({it.key(), it.value()});
    std::sort(out.begin(), out.end(), [](const QPair<QString, int>& a, const QPair<QString, int>& b) {
        if (a.second != b.second) return a.second > b.second;
        return a.first < b.first;
    });
    return out;
}

bool Store::setWrittenToPdf(const QString& id, bool written)
{
    if (!open_) return false;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("update markups set written=?, updated=? where id=?");
    q.addBindValue(written ? 1 : 0);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());
    q.addBindValue(id);
    return q.exec();
}

QString Store::mirrorPath(const QString& docPath) const
{
    const QString stem = QFileInfo(docPath).completeBaseName();
    return dataDir() + "/mirror/" + stem + "." + QFileInfo(docPath).suffix() + ".markups.json";
}

bool Store::exportMirror(const QString& docPath, QString* error) const
{
    if (docPath.isEmpty()) return false;
    const QString path = mirrorPath(docPath);
    QDir().mkpath(QFileInfo(path).absolutePath());

    QJsonObject root;
    root["docPath"] = docPath;
    root["docName"] = QFileInfo(docPath).fileName();
    root["exportedAt"] = QDateTime::currentMSecsSinceEpoch();
    root["schema"] = 2;

    QJsonArray list;
    const QVector<Markup> all = markups(docPath);
    for (const Markup& m : all) list.append(m.toJson());
    root["markups"] = list;

    QJsonArray groups;
    for (const MindGroup& g : mindGroups(docPath)) groups.append(g.toJson());
    if (!groups.isEmpty()) root["groups"] = groups;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}


// ---------------------------------------------------------------- reconstructed API
// (these were accidentally removed while refactoring upsertMarkupRow)

bool Store::removeMarkup(const QString& id, QString* error)
{
    // Soft delete: keep a tombstone with a bumped version vector so the deletion
    // propagates to other devices instead of the note coming back on next sync.
    if (!open_) return false;
    Markup current;
    bool found = false;
    {
        QSqlQuery q(QSqlDatabase::database(conn_));
        q.prepare(QString::fromLatin1(kSelectCols) + " where id=?");
        q.addBindValue(id);
        if (q.exec() && q.next()) {
            current = readMarkup(q);
            found = true;
        }
    }
    if (!found) return true;
    Markup tomb = stampForLocalWrite(current);
    tomb.deleted = true;
    return upsertMarkupRow(tomb, error, true, true);
}

bool Store::purgeMarkup(const QString& id)
{
    if (!open_) return false;
    QString docPath;
    {
        QSqlQuery q(QSqlDatabase::database(conn_));
        q.prepare("select docPath from markups where id=?");
        q.addBindValue(id);
        if (q.exec() && q.next()) docPath = q.value(0).toString();
    }
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("delete from markups where id=?");
    q.addBindValue(id);
    if (!q.exec()) return false;
    unindexMarkup(id);
    if (!docPath.isEmpty()) exportMirror(docPath);
    return true;
}

QString Store::setting(const QString& key, const QString& fallback) const
{
    if (!open_) return fallback;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("select value from settings where key=?");
    q.addBindValue(key);
    if (q.exec() && q.next()) return q.value(0).toString();
    return fallback;
}

void Store::setSetting(const QString& key, const QString& value)
{
    if (!open_) return;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("insert into settings(key,value) values(?,?)"
              " on conflict(key) do update set value=excluded.value");
    q.addBindValue(key);
    q.addBindValue(value);
    q.exec();
}

QString Store::deviceId()
{
    if (!deviceIdCache_.isEmpty()) return deviceIdCache_;
    QString dev = setting(QStringLiteral("deviceId"));
    if (dev.isEmpty()) {
        dev = newId().left(8);
        setSetting(QStringLiteral("deviceId"), dev);
    }
    deviceIdCache_ = dev;
    return dev;
}

Markup Store::stampForLocalWrite(const Markup& m)
{
    Markup out = m;
    VersionVector vv = VersionVector::parse(m.vv);
    vv.bump(deviceId(), static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()));
    out.vv = vv.toString();
    out.deviceId = deviceId();
    out.updated = QDateTime::currentMSecsSinceEpoch();
    return out;
}


QVector<Markup> Store::markupsIncludingDeleted(const QString& docPath) const
{
    QVector<Markup> out;
    if (!open_) return out;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare(QString::fromLatin1(kSelectCols) + " where docPath=? order by created desc");
    q.addBindValue(nonNull(docPath));
    if (!q.exec()) return out;
    while (q.next()) out.append(readMarkup(q));
    return out;
}

QVector<QString> Store::documentPaths() const
{
    QVector<QString> out;
    if (!open_) return out;
    QSet<QString> seen;
    QSqlQuery q(QSqlDatabase::database(conn_));
    if (q.exec("select path from documents")) {
        while (q.next()) {
            const QString p = q.value(0).toString();
            if (!p.isEmpty() && !seen.contains(p)) {
                seen.insert(p);
                out.append(p);
            }
        }
    }
    if (q.exec("select distinct docPath from markups")) {
        while (q.next()) {
            const QString p = q.value(0).toString();
            if (!p.isEmpty() && !seen.contains(p)) {
                seen.insert(p);
                out.append(p);
            }
        }
    }
    return out;
}

QVector<DocRow> Store::documents() const
{
    QVector<DocRow> out;
    if (!open_) return out;
    QSqlQuery q(QSqlDatabase::database(conn_));
    const QString sql =
        "select p.path, coalesce(d.title,''), coalesce(d.pages,0), coalesce(d.lastPage,0),"
        " coalesce(d.openedAt,0),"
        " (select count(*) from markups m where m.docPath=p.path and m.deleted=0),"
        " (select count(*) from markups m2 where m2.docPath=p.path and m2.deleted=0"
        "  and (',' || m2.tags || ',') like '%,conflict,%'),"
        " coalesce(d.fileSize,0), coalesce(d.fileMtime,0)"
        " from (select path from documents union select distinct docPath from markups) p"
        " left join documents d on d.path=p.path"
        " order by coalesce(d.openedAt,0) desc, p.path";
    if (!q.exec(sql)) return out;
    while (q.next()) {
        DocRow row;
        row.path = q.value(0).toString();
        row.title = q.value(1).toString();
        if (row.title.isEmpty()) row.title = QFileInfo(row.path).completeBaseName();
        row.pages = q.value(2).toInt();
        row.lastPage = q.value(3).toInt();
        row.openedAt = q.value(4).toLongLong();
        row.noteCount = q.value(5).toInt();
        row.conflictCount = q.value(6).toInt();
        row.fileSize = q.value(7).toLongLong();
        row.fileMtime = q.value(8).toLongLong();
        out.append(row);
    }
    return out;
}

QSet<QString> Store::documentPathsWithTag(const QString& tag) const
{
    QSet<QString> out;
    if (!open_ || tag.trimmed().isEmpty()) return out;
    QSqlQuery q(QSqlDatabase::database(conn_));
    // tags are stored comma separated between two commas: ",a,b,"
    q.prepare("select distinct docPath from markups"
              " where deleted=0 and (',' || tags || ',') like ?");
    q.addBindValue(QStringLiteral("%,") + tag.trimmed() + QStringLiteral(",%"));
    if (!q.exec()) return out;
    while (q.next()) out.insert(q.value(0).toString());
    return out;
}

void Store::setDocumentFileIdentity(const QString& path, qint64 size, qint64 mtimeMs)
{
    if (!open_ || path.isEmpty()) return;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("update documents set fileSize=?, fileMtime=? where path=?");
    q.addBindValue(size);
    q.addBindValue(mtimeMs);
    q.addBindValue(nonNull(path));
    q.exec();
}

qint64 Store::documentFileSize(const QString& path) const
{
    if (!open_) return 0;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("select fileSize from documents where path=?");
    q.addBindValue(nonNull(path));
    if (q.exec() && q.next()) return q.value(0).toLongLong();
    return 0;
}

qint64 Store::documentFileMtime(const QString& path) const
{
    if (!open_) return 0;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("select fileMtime from documents where path=?");
    q.addBindValue(path.isNull() ? QString::fromLatin1("") : path);
    if (q.exec() && q.next()) return q.value(0).toLongLong();
    return 0;
}

bool Store::renameDocument(const QString& path, const QString& title)
{
    if (!open_ || path.isEmpty()) return false;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("update documents set title=? where path=?");
    q.addBindValue(title.trimmed());
    q.addBindValue(nonNull(path));
    if (!q.exec()) return false;
    if (q.numRowsAffected() == 0) {
        QSqlQuery ins(QSqlDatabase::database(conn_));
        ins.prepare("insert into documents(path,title,pages,lastPage,lastOffset,openedAt)"
                    " values(?,?,0,0,0,?) on conflict(path) do update set title=excluded.title");
        ins.addBindValue(nonNull(path));
        ins.addBindValue(title.trimmed());
        ins.addBindValue(QDateTime::currentMSecsSinceEpoch());
        return ins.exec();
    }
    return true;
}

bool Store::forgetDocument(const QString& path, int* notesRemoved)
{
    if (!open_ || path.isEmpty()) return false;
    const QString docPath = nonNull(path);

    int removed = 0;
    const QVector<Markup> live = markups(docPath);
    QVector<Markup> tombs;
    tombs.reserve(live.size());
    for (const Markup& m : live) {
        Markup tomb = stampForLocalWrite(m);
        tomb.deleted = true;
        tombs.append(tomb);
    }
    if (!tombs.isEmpty() && upsertMarkups(tombs, nullptr)) removed = tombs.size();
    exportMirror(docPath);

    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("delete from documents where path=?");
    q.addBindValue(docPath);
    q.exec();

    if (notesRemoved) *notesRemoved = removed;
    return true;
}

// ---------------------------------------------------------------- mind groups

namespace {

MindGroup readGroupRow(const QSqlQuery& q)
{
    MindGroup g;
    g.id = q.value(0).toString();
    g.docPath = q.value(1).toString();
    g.name = q.value(2).toString();
    g.order = q.value(3).toInt();
    g.updated = q.value(4).toLongLong();
    g.deleted = q.value(5).toInt() != 0;
    g.vv = q.value(6).toString();
    g.deviceId = q.value(7).toString();
    g.parentId = q.value(8).toString();
    return g;
}

const char* kGroupSelect =
    "select id,docPath,name,[order],updated,deleted,vv,deviceId,parentId from mind_groups";

}  // namespace

QVector<MindGroup> Store::mindGroups(const QString& docPath) const
{
    QVector<MindGroup> out;
    if (!open_) return out;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare(QString::fromLatin1(kGroupSelect) +
              " where docPath=? and deleted=0 order by [order], name");
    q.addBindValue(nonNull(docPath));
    if (!q.exec()) return out;
    while (q.next()) out.append(readGroupRow(q));
    return out;
}

QVector<MindGroup> Store::mindGroupsIncludingDeleted(const QString& docPath) const
{
    QVector<MindGroup> out;
    if (!open_) return out;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare(QString::fromLatin1(kGroupSelect) + " where docPath=? order by [order], name");
    q.addBindValue(nonNull(docPath));
    if (!q.exec()) return out;
    while (q.next()) out.append(readGroupRow(q));
    return out;
}

// True when making `parentId` the parent of `id` would close a loop.
bool Store::wouldCycle(const QString& id, const QString& parentId) const
{
    if (id.isEmpty() || parentId.isEmpty()) return false;
    if (id == parentId) return true;
    QString cursor = parentId;
    QSet<QString> seen;
    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("select parentId from mind_groups where id=?");
    for (int guard = 0; guard < 512 && !cursor.isEmpty(); ++guard) {
        if (cursor == id) return true;
        if (seen.contains(cursor)) return false;   // already broken upstream; stop walking
        seen.insert(cursor);
        q.bindValue(0, cursor);
        if (!q.exec() || !q.next()) return false;
        cursor = q.value(0).toString();
    }
    return false;
}

bool Store::upsertMindGroup(const MindGroup& gIn, QString* error, bool writeMirror)
{
    if (!open_) return false;

    // A merge can install parentId edges that were legal on each device but form a cycle
    // once combined (A moved under B on one side, B under A on the other). The mind map and
    // the "export by mind map" path walk parents, so a cycle would silently drop those
    // groups *and their notes* from the tree. Hoist the offending group to the top level
    // instead of storing a broken edge.
    MindGroup g = gIn;
    if (!g.parentId.isEmpty() && !g.deleted && wouldCycle(g.id, g.parentId))
        g.parentId.clear();

    QSqlQuery q(QSqlDatabase::database(conn_));
    q.prepare("insert into mind_groups(id,docPath,name,parentId,[order],updated,deleted,vv,deviceId)"
              " values(?,?,?,?,?,?,?,?,?)"
              " on conflict(id) do update set name=excluded.name, parentId=excluded.parentId,"
              " [order]=excluded.[order], updated=excluded.updated, deleted=excluded.deleted,"
              " vv=excluded.vv, deviceId=excluded.deviceId");
    q.addBindValue(g.id);
    q.addBindValue(nonNull(g.docPath));
    q.addBindValue(g.name);
    q.addBindValue(g.parentId);
    q.addBindValue(g.order);
    q.addBindValue(g.updated);
    q.addBindValue(g.deleted ? 1 : 0);
    q.addBindValue(g.vv);
    q.addBindValue(g.deviceId);
    if (!q.exec()) {
        if (error) *error = q.lastError().text();
        return false;
    }
    if (writeMirror) exportMirror(g.docPath);
    return true;
}

MindGroup Store::stampGroupForLocalWrite(const MindGroup& g)
{
    MindGroup out = g;
    VersionVector vv = VersionVector::parse(g.vv);
    vv.bump(deviceId(), static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()));
    out.vv = vv.toString();
    out.deviceId = deviceId();
    out.updated = QDateTime::currentMSecsSinceEpoch();
    return out;
}

bool Store::setMindGroupParent(const QString& id, const QString& parentId)
{
    if (!open_ || id.isEmpty()) return false;
    if (id == parentId) return false;

    const QVector<MindGroup> groups = mindGroups(QString::fromLatin1("")) ;
    Q_UNUSED(groups);

    MindGroup target;
    bool found = false;
    QString docPath;
    {
        QSqlQuery q(QSqlDatabase::database(conn_));
        q.prepare(QString::fromLatin1(kGroupSelect) + " where id=?");
        q.addBindValue(id);
        if (q.exec() && q.next()) {
            target = readGroupRow(q);
            found = true;
            docPath = target.docPath;
        }
    }
    if (!found) return false;

    const QVector<MindGroup> all = mindGroupsIncludingDeleted(docPath);
    // walk up from the prospective parent: the group itself must not be an ancestor
    QString cursor = parentId;
    int guard = 0;
    while (!cursor.isEmpty() && guard++ < 256) {
        if (cursor == id) return false;   // would create a cycle
        QString next;
        for (const MindGroup& g : all)
            if (g.id == cursor) next = g.parentId;
        cursor = next;
    }
    // sibling order: append at the end of the new parent's children
    int siblings = 0;
    for (const MindGroup& g : all)
        if (g.parentId == parentId && g.id != id) ++siblings;

    target.parentId = parentId;
    target.order = siblings;
    return upsertMindGroup(stampGroupForLocalWrite(target));
}

bool Store::removeMindGroup(const QString& id)
{
    if (!open_ || id.isEmpty()) return false;
    MindGroup target;
    bool found = false;
    {
        QSqlQuery q(QSqlDatabase::database(conn_));
        q.prepare(QString::fromLatin1(kGroupSelect) + " where id=?");
        q.addBindValue(id);
        if (q.exec() && q.next()) {
            target = readGroupRow(q);
            found = true;
        }
    }
    if (!found) return false;

    {
        QSqlQuery q(QSqlDatabase::database(conn_));
        q.prepare("update markups set mindGroup='' where mindGroup=?");
        q.addBindValue(id);
        q.exec();
    }
    // children move up one level instead of disappearing with their parent
    for (const MindGroup& child : mindGroupsIncludingDeleted(target.docPath)) {
        if (child.parentId != id || child.deleted) continue;
        MindGroup lifted = child;
        lifted.parentId = target.parentId;
        upsertMindGroup(stampGroupForLocalWrite(lifted), nullptr, false);
    }

    MindGroup tomb = stampGroupForLocalWrite(target);
    tomb.deleted = true;
    return upsertMindGroup(tomb);
}

QString Store::ensureMindGroup(const QString& docPath, const QString& name,
                              const QString& parentId)
{
    const QString trimmed = name.trimmed();
    if (!open_ || trimmed.isEmpty()) return {};
    for (const MindGroup& g : mindGroups(docPath))
        if (g.name == trimmed && g.parentId == parentId) return g.id;

    MindGroup g;
    g.id = newId();
    g.docPath = nonNull(docPath);
    g.name = trimmed;
    g.parentId = parentId;
    int siblings = 0;
    for (const MindGroup& other : mindGroups(docPath))
        if (other.parentId == parentId) ++siblings;
    g.order = siblings;
    g.updated = QDateTime::currentMSecsSinceEpoch();
    g = stampGroupForLocalWrite(g);
    if (!upsertMindGroup(g)) return {};
    return g.id;
}

}  // namespace an
