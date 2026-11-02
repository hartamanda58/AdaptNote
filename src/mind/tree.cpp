#include "mind/tree.h"

#include <QFileInfo>

#include <algorithm>
#include <functional>

#include <QSet>
#include <QStringList>

namespace an {
namespace mind {

Node buildFromNotes(const QString& docTitle, const QVector<Markup>& markups,
                    const QVector<MindGroup>& groups, bool useAutomaticPageBuckets)
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
        // The card shows the full excerpt below the title, so the title only needs to be a
        // short label: cut at the first sentence/clause break, never mid-word.
        if (!m.text.simplified().isEmpty()) {
            QString head = m.text.simplified();
            static const QString stops = QStringLiteral("。！？；，、.!?;,");
            for (int i = 0; i < head.size() && i < 30; ++i) {
                if (stops.contains(head.at(i))) {
                    head = head.left(i);
                    break;
                }
            }
            if (head.size() > 22) head = head.left(22);
            if (!head.isEmpty()) text = head;
        }
        if (text.size() > 60) text = text.left(60) + QStringLiteral("…");
        leaf.text = m.title.trimmed().isEmpty() ? text : m.title.trimmed();
        leaf.page = m.page;
        leaf.markupId = m.id;
        leaf.parentMarkupId = m.parentMarkupId;
        leaf.groupKey = m.mindGroup.isEmpty() ? QString()
                                              : QStringLiteral("group:%1").arg(m.mindGroup);
        // Card body: quoted source text, the reader's own words, and where it came from.
        leaf.excerpt = m.text.simplified();
        leaf.comment = m.comment.simplified();
        if (m.page >= 0) leaf.source = QStringLiteral("第 %1 页").arg(m.page + 1);
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

    // Validate note-parent edges before grouping. Reject missing/cross-document parents and
    // break cycles deterministically; rejected nodes fall back to their prior group/page.
    QHash<QString, Markup> noteById;
    for (const Markup& m : live) noteById.insert(m.id, m);
    QHash<QString, QString> noteParent;
    for (const Markup& m : live) {
        QString parent = m.parentMarkupId;
        if (parent == m.id || !noteById.contains(parent) || noteById.value(parent).docPath != m.docPath)
            parent.clear();
        else {
            const Markup parentNote = noteById.value(parent);
            if (parentNote.mindGroup != m.mindGroup ||
                (m.mindGroup.isEmpty() && parentNote.page != m.page)) parent.clear();
        }
        noteParent.insert(m.id, parent);
    }
    for (const Markup& m : live) {
        QStringList chain;
        QString cursor = m.id;
        while (!cursor.isEmpty()) {
            if (chain.contains(cursor)) {
                QString smallest = chain.first();
                for (const QString& id : chain) if (id < smallest) smallest = id;
                noteParent[smallest].clear();
                break;
            }
            chain.append(cursor);
            cursor = noteParent.value(cursor);
        }
    }
    QHash<QString, QVector<QString>> childrenOfNote;
    for (auto it = noteParent.cbegin(); it != noteParent.cend(); ++it)
        if (!it.value().isEmpty()) childrenOfNote[it.value()].append(it.key());
    auto sortNoteIds = [&](QVector<QString>& ids) {
        std::sort(ids.begin(), ids.end(), [&](const QString& a, const QString& b) {
            const Markup x = noteById.value(a), y = noteById.value(b);
            if (x.mindOrder != y.mindOrder) return x.mindOrder < y.mindOrder;
            if (x.page != y.page) return x.page < y.page;
            return a < b;
        });
    };
    for (auto it = childrenOfNote.begin(); it != childrenOfNote.end(); ++it) sortNoteIds(it.value());
    std::function<Node(const QString&, const QString&)> buildNote =
        [&](const QString& id, const QString& inheritedGroupKey) {
            Node node = leafOf(noteById.value(id));
            if (node.groupKey.isEmpty()) node.groupKey = inheritedGroupKey;
            for (const QString& childId : childrenOfNote.value(id))
                node.children.append(buildNote(childId, node.groupKey));
            return node;
        };
    auto appendNoteForest = [&](Node& parent, const QVector<Markup>& notes) {
        QVector<QString> roots;
        for (const Markup& m : notes)
            if (noteParent.value(m.id).isEmpty()) roots.append(m.id);
        sortNoteIds(roots);
        for (const QString& id : roots) parent.children.append(buildNote(id, parent.groupKey));
    };

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
        node.isGroup = true;
        emitted.insert(id);
        for (const QString& childId : childrenOf.value(id)) {
            if (emitted.contains(childId)) continue;
            node.children.append(buildGroup(childId));
        }
        appendNoteForest(node, notesOf.value(id));
        return node;
    };

    for (const QString& topId : childrenOf.value(QString())) {
        if (emitted.contains(topId)) continue;
        root.children.append(buildGroup(topId));
    }

    // ---- notes without an explicit group: page buckets, sorted by page ----
    if (!useAutomaticPageBuckets) {
        appendNoteForest(root, orphaned);
        return root;
    }
    sortNotes(orphaned);
    QVector<Markup> pageSorted = orphaned;
    std::sort(pageSorted.begin(), pageSorted.end(), [](const Markup& a, const Markup& b) {
        if (a.page != b.page) return a.page < b.page;
        return a.boundingRect().top() < b.boundingRect().top();
    });
    QHash<int, QVector<Markup>> notesByPage;
    for (const Markup& m : pageSorted) notesByPage[m.page].append(m);
    for (auto it = notesByPage.cbegin(); it != notesByPage.cend(); ++it) {
        Node bucket;
        bucket.text = QStringLiteral("第 %1 页").arg(it.key() + 1);
        bucket.page = it.key();
        bucket.groupKey = QStringLiteral("page:%1").arg(it.key());
        bucket.isGroup = true;
        appendNoteForest(bucket, it.value());
        root.children.append(bucket);
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
        if (rest.at(i).mindOrder != newOrder || !rest.at(i).parentMarkupId.isEmpty())
            changes.append({rest.at(i).id, newOrder});
    }
    return changes;
}

Node buildFromTopic(const QString& topicName, const QVector<TopicDocInput>& docs,
                    const QVector<MindGroup>& groups)
{
    Node root;
    root.text = topicName.isEmpty() ? QStringLiteral("阅读主题") : topicName;
    root.isGroup = true;

    for (const TopicDocInput& doc : docs) {
        // Reuse the single-document builder so the per-document structure (groups, page
        // buckets, ordering) stays byte-for-byte what the reader shows on its own.
        QVector<MindGroup> own;
        for (const MindGroup& g : groups)
            if (g.docPath == doc.docPath) own.append(g);

        Node branch = buildFromNotes(doc.title, doc.markups, own);
        if (branch.children.isEmpty()) continue;   // nothing to show for that document
        branch.text = doc.title.isEmpty() ? QFileInfo(doc.docPath).completeBaseName() : doc.title;
        // Page buckets from different books must not share layout/collapse keys.
        std::function<void(Node&)> scopePages = [&](Node& node) {
            if (node.groupKey.startsWith(QStringLiteral("page:")))
                node.groupKey = QStringLiteral("docpage:") + doc.docPath + QLatin1Char(':') + node.groupKey;
            for (Node& child : node.children) scopePages(child);
        };
        scopePages(branch);
        branch.isGroup = true;
        branch.docPath = doc.docPath;
        branch.markupId.clear();
        branch.page = -1;
        root.children.append(branch);
    }
    return root;
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
