#include "mind/tree.h"

#include <algorithm>
#include <functional>

#include <QSet>
#include <QStringList>

namespace an {
namespace mind {

Node buildFromNotes(const QString& docTitle, const QVector<Markup>& markups,
                    const QVector<MindGroup>& groups)
{
    Node root;
    root.text = docTitle.isEmpty() ? QStringLiteral("未命名") : docTitle;

    QVector<Markup> live;
    for (const Markup& m : markups)
        if (!m.deleted) live.append(m);

    auto leafOf = [](const Markup& m) {
        Node leaf;
        QString text = m.text.simplified();
        if (text.isEmpty()) text = m.comment.simplified();
        if (text.isEmpty()) {
            switch (m.type) {
            case MarkupType::Image: text = QStringLiteral("(图片摘录)"); break;
            case MarkupType::Ink: text = QStringLiteral("(手写标注)"); break;
            default: text = QStringLiteral("(标注)"); break;
            }
        }
        if (text.size() > 60) text = text.left(60) + QStringLiteral("…");
        leaf.text = text;
        leaf.page = m.page;
        leaf.markupId = m.id;
        leaf.groupKey = m.mindGroup.isEmpty() ? QString()
                                              : QStringLiteral("group:%1").arg(m.mindGroup);
        return leaf;
    };

    // ---- group index (ids come from the MindGroup entities) ----
    QHash<QString, MindGroup> byId;
    QHash<QString, QVector<QString>> childrenOf;   // parentId -> child ids
    for (const MindGroup& g : groups) {
        if (g.deleted) continue;
        byId.insert(g.id, g);
    }
    // Sanitise parent edges before building: a cycle (two devices re-parenting the same
    // pair in opposite directions, or hand-edited JSON) would leave every group of the loop
    // unreachable from the root, so their notes would disappear from the mind map and from
    // "export by mind map" without any error. Hoist the smallest id of each loop instead.
    QHash<QString, QString> parentOf;
    for (auto it = byId.constBegin(); it != byId.constEnd(); ++it) {
        QString parent = byId.value(it.key()).parentId;
        if (!byId.contains(parent)) parent.clear();
        parentOf.insert(it.key(), parent);
    }
    for (auto it = byId.constBegin(); it != byId.constEnd(); ++it) {
        QStringList seen;
        QString cursor = it.key();
        while (!cursor.isEmpty()) {
            if (seen.contains(cursor)) {
                // loop detected on this chain: break it at the lexicographically smallest
                // member so the choice is deterministic across devices
                QString smallest = seen.first();
                for (const QString& id : seen)
                    if (id < smallest) smallest = id;
                parentOf[smallest] = QString();
                break;
            }
            seen.append(cursor);
            cursor = parentOf.value(cursor);
        }
    }
    for (auto it = parentOf.constBegin(); it != parentOf.constEnd(); ++it)
        childrenOf[it.value()].append(it.key());
    auto sortIds = [&](QVector<QString>& ids) {
        std::sort(ids.begin(), ids.end(), [&](const QString& a, const QString& b) {
            const MindGroup ga = byId.value(a);
            const MindGroup gb = byId.value(b);
            if (ga.order != gb.order) return ga.order < gb.order;
            return ga.name < gb.name;
        });
    };
    for (auto it = childrenOf.begin(); it != childrenOf.end(); ++it) sortIds(it.value());

    // ---- notes per group (dangling ids fall back to page buckets) ----
    QHash<QString, QVector<Markup>> notesOf;
    QVector<Markup> orphaned;
    for (const Markup& m : live) {
        if (!m.mindGroup.isEmpty() && byId.contains(m.mindGroup))
            notesOf[m.mindGroup].append(m);
        else
            orphaned.append(m);
    }
    auto sortNotes = [](QVector<Markup>& v) {
        std::sort(v.begin(), v.end(), [](const Markup& a, const Markup& b) {
            if (a.mindOrder != b.mindOrder) return a.mindOrder < b.mindOrder;
            if (a.page != b.page) return a.page < b.page;
            return a.boundingRect().top() < b.boundingRect().top();
        });
    };
    for (auto it = notesOf.begin(); it != notesOf.end(); ++it) sortNotes(it.value());

    QSet<QString> emitted;   // belt and braces: never visit a group twice
    std::function<Node(const QString&)> buildGroup = [&](const QString& id) -> Node {
        const MindGroup g = byId.value(id);
        Node node;
        node.text = g.name;
        node.groupKey = QStringLiteral("group:%1").arg(id);
        node.page = -1;
        emitted.insert(id);
        for (const QString& childId : childrenOf.value(id)) {
            if (emitted.contains(childId)) continue;
            node.children.append(buildGroup(childId));
        }
        for (const Markup& m : notesOf.value(id)) node.children.append(leafOf(m));
        return node;
    };

    for (const QString& topId : childrenOf.value(QString())) {
        if (emitted.contains(topId)) continue;
        root.children.append(buildGroup(topId));
    }

    // ---- notes without an explicit group: page buckets, sorted by page ----
    sortNotes(orphaned);
    QVector<Markup> pageSorted = orphaned;
    std::sort(pageSorted.begin(), pageSorted.end(), [](const Markup& a, const Markup& b) {
        if (a.page != b.page) return a.page < b.page;
        return a.boundingRect().top() < b.boundingRect().top();
    });
    int currentPage = -1;
    for (const Markup& m : pageSorted) {
        if (m.page != currentPage) {
            currentPage = m.page;
            Node bucket;
            bucket.text = QStringLiteral("第 %1 页").arg(currentPage + 1);
            bucket.page = currentPage;
            bucket.groupKey = QStringLiteral("page:%1").arg(currentPage);
            root.children.append(bucket);
        }
        Node leaf = leafOf(m);
        leaf.groupKey = root.children.last().groupKey;   // same key as its page bucket
        root.children.last().children.append(leaf);
    }

    return root;
}

QVector<QPair<QString, int>> planReorder(const QVector<Markup>& siblings, const QString& movedId,
                                         const QString& beforeId)
{
    QVector<Markup> ordered = siblings;
    std::sort(ordered.begin(), ordered.end(), [](const Markup& a, const Markup& b) {
        if (a.mindOrder != b.mindOrder) return a.mindOrder < b.mindOrder;
        if (a.page != b.page) return a.page < b.page;
        return a.boundingRect().top() < b.boundingRect().top();
    });

    // take the moved note out, remember where it should go
    Markup moved;
    bool found = false;
    QVector<Markup> rest;
    for (const Markup& m : ordered) {
        if (m.id == movedId) {
            moved = m;
            found = true;
            continue;
        }
        rest.append(m);
    }
    if (!found) return {};

    int insertAt = rest.size();
    if (!beforeId.isEmpty()) {
        for (int i = 0; i < rest.size(); ++i) {
            if (rest.at(i).id == beforeId) {
                insertAt = i;
                break;
            }
        }
    }
    rest.insert(insertAt, moved);

    QVector<QPair<QString, int>> changes;
    for (int i = 0; i < rest.size(); ++i) {
        const int newOrder = (i + 1) * 10;
        if (rest.at(i).mindOrder != newOrder) changes.append({rest.at(i).id, newOrder});
    }
    return changes;
}

int nodeCount(const Node& node)
{
    int n = 1;
    for (const Node& c : node.children) n += nodeCount(c);
    return n;
}

int depthOf(const Node& node)
{
    int d = 1;
    for (const Node& c : node.children) d = qMax(d, 1 + depthOf(c));
    return d;
}

}  // namespace mind
}  // namespace an
