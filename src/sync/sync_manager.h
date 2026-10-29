#pragma once

#include "core/model.h"
#include "core/store.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace an {

struct SyncSettings {
    QString url;                              // e.g. https://dav.example.com/dav
    QString user;
    QString password;
    QString remotePath = QStringLiteral("adaptnote");
    bool configured() const { return !url.trimmed().isEmpty(); }
};

struct SyncReport {
    bool ok = false;
    int pulled = 0;
    int pushed = 0;
    int conflicts = 0;
    int documents = 0;
    QString error;
    QStringList messages;

    QString summary() const;
};

// Per-document file sync over WebDAV with version-vector merge.
// Wire format: one JSON document per book, containing every note *including*
// tombstones, so deletions propagate and nothing is silently lost.
class SyncManager {
public:
    explicit SyncManager(Store& store);

    SyncSettings settings() const;
    void setSettings(const SyncSettings& s);   // persisted in the store

    SyncReport syncDocument(const QString& docPath);
    SyncReport syncAll();

    // Remote file name for a document: stable across devices for the same book.
    static QString docKeyFor(const QString& docPath);

    struct MergeResult {
        QVector<Markup> merged;   // full set, tombstones included
        QVector<MindGroup> mergedGroups;
        int conflicts = 0;
        int groupConflicts = 0;
        int pulled = 0;
        int pushed = 0;
        QStringList messages;
    };

    // Pure function: the heart of the conflict model. Unit tested on its own.
    static MergeResult merge(const QVector<Markup>& local, const QVector<Markup>& remote,
                             const QString& deviceId, qint64 nowMs);

    // Groups merge with the same version rules, but a concurrent rename keeps the
    // newer name instead of forking a copy (duplicating a folder is not useful).
    struct GroupMergeResult {
        QVector<MindGroup> merged;
        int conflicts = 0;
        int pulled = 0;
        int pushed = 0;
    };
    static GroupMergeResult mergeGroups(const QVector<MindGroup>& local,
                                        const QVector<MindGroup>& remote,
                                        const QString& deviceId, qint64 nowMs);

    static QByteArray documentJson(const QString& docPath, const QString& docKey,
                                   const QVector<Markup>& markups,
                                   const QVector<MindGroup>& groups = {});
    static QVector<Markup> parseDocument(const QByteArray& json, QString* error);
    static QVector<MindGroup> parseGroups(const QByteArray& json, QString* error);

    // Topic bundle sync is separate from per-document JSON because a topic may reference
    // several books. TopicDoc.docPath is encoded as a stable document key on the wire and
    // resolved back to the local path on apply.
    struct TopicMergeResult {
        QVector<Topic> topics;
        QVector<TopicDoc> docs;
        QVector<MindLink> links;
        int conflicts = 0;
        int pulled = 0;
        int pushed = 0;
    };
    static QByteArray topicsJson(const QVector<Topic>& topics,
                                 const QVector<TopicDoc>& docs,
                                 const QVector<MindLink>& links);
    static void parseTopics(const QByteArray& json, QVector<Topic>* topics,
                            QVector<TopicDoc>* docs, QVector<MindLink>* links,
                            QString* error);
    static TopicMergeResult mergeTopics(const QVector<Topic>& local,
                                        const QVector<Topic>& remote,
                                        const QString& deviceId, qint64 nowMs);


private:
    Store& store_;
};

}  // namespace an
