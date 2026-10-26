#pragma once

#include "core/model.h"

#include <QPair>
#include <QHash>
#include <QSet>
#include <QString>
#include <QVector>

class QSqlQuery;

namespace an {

// Single source of truth for annotations: SQLite (fast queries + FTS5) plus a
// human readable JSON mirror per document (diff-able, git-able, survives DB loss).
// One row of the library view.
struct DocRow {
    QString path;
    QString title;
    int pages = 0;
    int lastPage = 0;
    qint64 openedAt = 0;
    int noteCount = 0;
    int conflictCount = 0;
    qint64 fileSize = 0;
    qint64 fileMtime = 0;
};

class Store {
public:
    bool open(const QString& dbPath, QString* error = nullptr);
    void close();
    bool isOpen() const { return open_; }
    QString dbPath() const { return dbPath_; }
    QString dataDir() const;
    QString imagesDir() const;

    void touchDocument(const QString& path, const QString& title, int pages);
    // File identity captured the last time the document was opened.
    void setDocumentFileIdentity(const QString& path, qint64 size, qint64 mtimeMs);
    qint64 documentFileSize(const QString& path) const;
    qint64 documentFileMtime(const QString& path) const;
    void setReadingState(const QString& path, int page, double offsetPt);
    int lastPage(const QString& path) const;
    double lastOffset(const QString& path) const;

    bool upsertMarkup(const Markup& m, QString* error = nullptr, bool writeMirror = true);
    // Bulk write in a single transaction (used by sync). Mirrors are written once.
    bool upsertMarkups(const QVector<Markup>& markups, QString* error = nullptr);
    // Import mode: relaxes fsync for the duration of a bulk import (much faster,
    // at the cost of durability if the machine dies mid-import). Always restore.
    void setFastImport(bool on);
    bool fastImport() const { return fastImport_; }
    bool removeMarkup(const QString& id, QString* error = nullptr);
    QVector<Markup> markups(const QString& docPathIn, int page = -1) const;
    QVector<Markup> allMarkups(int limit = 1000) const;

    // Full text search over excerpt / comment / tags. Empty docPath = whole library.
    QVector<Markup> searchMarkups(const QString& query, const QString& docPathIn = {}) const;
    QVector<QPair<QString, int>> tagCounts(const QString& docPathIn = {}) const;
    bool fullTextSearchEnabled() const { return fts_; }

    bool setWrittenToPdf(const QString& id, bool written);

    // ---- sync support ----
    QString deviceId();
    QString setting(const QString& key, const QString& fallback = {}) const;
    void setSetting(const QString& key, const QString& value);
    // Bumps the version vector for this device (used for every local change).
    Markup stampForLocalWrite(const Markup& m);
    QVector<Markup> markupsIncludingDeleted(const QString& docPath) const;
    QVector<QString> documentPaths() const;
    // Library view: every document we know about (opened or holding notes).
    QVector<DocRow> documents() const;
    // Documents that contain at least one live note carrying the tag.
    QSet<QString> documentPathsWithTag(const QString& tag) const;
    // Library management
    bool renameDocument(const QString& path, const QString& title);
    // Removes a document from the library: its notes become tombstones (so the
    // removal propagates through sync) and the documents row is dropped. The book
    // file itself is never touched.
    bool forgetDocument(const QString& path, int* notesRemoved = nullptr);

    // ---- mind map groups ----
    QVector<MindGroup> mindGroups(const QString& docPath) const;   // live only
    QVector<MindGroup> mindGroupsIncludingDeleted(const QString& docPath) const;
    bool upsertMindGroup(const MindGroup& g, QString* error = nullptr, bool writeMirror = true);
    // True when re-parenting `id` under `parentId` would create a loop.
    bool wouldCycle(const QString& id, const QString& parentId) const;
    bool removeMindGroup(const QString& id);                      // tombstone
    MindGroup stampGroupForLocalWrite(const MindGroup& g);
    // Returns the id of the group with that name, creating it when missing.
    QString ensureMindGroup(const QString& docPath, const QString& name,
                            const QString& parentId = {});
    // Moves a group under another one (empty = top level). Rejects cycles.
    bool setMindGroupParent(const QString& id, const QString& parentId);
    bool purgeMarkup(const QString& id);

    bool exportMirror(const QString& docPath, QString* error = nullptr) const;
    QString mirrorPath(const QString& docPath) const;

private:
    bool ensureSchema(QString* error);
    void ensureColumn(const QString& table, const QString& column, const QString& type);
    bool indexMarkup(const Markup& m);
    bool upsertMarkupRow(const Markup& m, QString* error = nullptr,
                         bool writeMirror = true, bool indexFts = true);
    // Reuses one prepared statement per SQL text (prepare is not cheap).
    QSqlQuery* prepared(const QString& sql);
    void clearPrepared();
    void unindexMarkup(const QString& id);

    bool open_ = false;
    bool fts_ = false;
    QString dbPath_;
    QString conn_;
    mutable QString deviceIdCache_;                       // deviceId() hits the DB once
    mutable QHash<QString, QString> indexedFingerprint_;  // id -> text|comment|tags
    mutable QSet<QString> knownIndexIds_;                  // ids present in notes_fts
    mutable bool knownIndexLoaded_ = false;
    bool fastImport_ = false;
    // Brings notes_fts back in sync with notes (upgrade, dropped index).
    void repairSearchIndex();

    void loadKnownIndexIds() const;
    mutable QHash<QString, QSqlQuery*> stmtCache_;        // prepared statement reuse
};

}  // namespace an
