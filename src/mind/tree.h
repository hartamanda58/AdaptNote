#pragma once

#include "core/model.h"

#include <QPair>
#include <QString>
#include <QVector>

namespace an {
namespace mind {

// One node of the generated mind map. This part has no dependency on QtWidgets so the
// exporters (adapt_core) can build the same tree the view shows.
struct Node {
    QString text;       // card title (group name, or a short headline for a note)
    int page = -1;      // >= 0 means "this node points at a place in a document"
    QString markupId;
    QString groupKey;   // stable key of the group (empty for notes without one)
    QVector<Node> children;

    // Extra card content. The view shows these as separate rows (excerpt block, own
    // comment); exporters may ignore them. Keeping them here means "what you export" and
    // "what you see" are built from the same tree instead of two parallel models.
    QString excerpt;    // the quoted source text (may be empty)
    QString comment;    // the reader's own note (may be empty)
    QString source;     // human readable origin, e.g. "第 3 页" / "第 2 章"
    bool isGroup = false;
};

// One document inside a reading topic (cross-document mind map).
struct TopicDocInput {
    QString title;      // document title shown as the branch label
    QString docPath;
    QVector<Markup> markups;
};

// Root = topic name; one branch per document, each containing that document's groups and
// notes. Documents with no notes are skipped so the map only shows what carries content.
Node buildFromTopic(const QString& topicName, const QVector<TopicDocInput>& docs,
                    const QVector<MindGroup>& groups);

// Root = document title, then the group tree (MindGroup::parentId), notes as leaves.
// Notes without a group fall back to page buckets, so an unorganised document still
// shows a useful outline.
Node buildFromNotes(const QString& docTitle, const QVector<Markup>& markups,
                    const QVector<MindGroup>& groups = {});

int nodeCount(const Node& node);
int depthOf(const Node& node);

// Plans the manual ordering inside one group (or one page bucket).
QVector<QPair<QString, int>> planReorder(const QVector<Markup>& siblings, const QString& movedId,
                                         const QString& beforeId);

}  // namespace mind
}  // namespace an
