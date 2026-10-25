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
    QString text;
    int page = -1;
    QString markupId;
    QString groupKey;   // stable key of the group (empty for notes without one)
    QVector<Node> children;
};

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
