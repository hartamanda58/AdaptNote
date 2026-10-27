#include "mind/layout.h"

#include <QHash>
#include <QSet>
#include <QStringList>

#include <algorithm>
#include <functional>
#include <limits>

namespace an {
namespace mind {

QString nodeKey(const Node& n)
{
    if (!n.groupKey.isEmpty()) {
        // group keys look like "group:<id>"; keep the raw key so callers can measure by it
        return n.groupKey;
    }
    return QStringLiteral("page:%1").arg(n.page);
}

namespace {

struct Row {
    QString key;
    double height = 0.0;
    double y = 0.0;
    int depth = 0;
    QVector<int> children;
    bool hasChildren = false;
    bool collapsed = false;
};

// Flattens the tree into rows, skipping children of collapsed nodes.
int flatten(const Node& node, int depth, QHash<QString, double>& heights, double fallback,
            const QSet<QString>& collapsedKeys, QVector<Row>& rows)
{
    Row row;
    row.key = nodeKey(node);
    row.depth = depth;
    row.height = heights.value(row.key, fallback);
    row.hasChildren = !node.children.isEmpty();
    row.collapsed = collapsedKeys.contains(row.key);
    const int index = rows.size();
    rows.append(row);

    if (!row.collapsed) {
        for (const Node& child : node.children) {
            const int childIndex = flatten(child, depth + 1, heights, fallback, collapsedKeys, rows);
            if (childIndex >= 0) rows[index].children.append(childIndex);
        }
    }
    return index;
}

}  // namespace

QVector<PlacedNode> layoutTree(const Node& root, const QHash<QString, double>& measured,
                               double defaultHeight, const LayoutOptions& opts,
                               const QSet<QString>& collapsedKeys, QSizeF* canvasSize)
{
    QVector<PlacedNode> out;
    QHash<QString, double> heights = measured;   // copy: flatten may look up by key only
    QVector<Row> rows;
    flatten(root, 0, heights, defaultHeight, collapsedKeys, rows);
    if (rows.isEmpty()) {
        if (canvasSize) *canvasSize = QSizeF(0, 0);
        return out;
    }

    // 1) children first: stack every level from the deepest up so a parent can be centred
    //    on its children. Rows are stored parent-before-child, so walk backwards.
    QHash<int, double> y;
    int maxDepth = 0;
    for (const Row& r : rows) maxDepth = std::max(maxDepth, r.depth);

    for (int depth = maxDepth; depth >= 0; --depth) {
        double cursor = opts.padding;
        for (int i = 0; i < rows.size(); ++i) {
            if (rows[i].depth != depth) continue;
            double want = cursor;
            if (!rows[i].children.isEmpty()) {
                double top = std::numeric_limits<double>::max();
                double bottom = std::numeric_limits<double>::lowest();
                for (int c : rows[i].children) {
                    const double cy = y.value(c, cursor);
                    top = std::min(top, cy);
                    bottom = std::max(bottom, cy + rows[c].height);
                }
                if (top <= bottom) want = std::max(cursor, (top + bottom) / 2.0 - rows[i].height / 2.0);
            }
            y[i] = want;
            cursor = want + rows[i].height + opts.rowGap;
        }
    }

    // 2) x from the depth; the canvas is as tall as the deepest column needs
    double tallest = 0.0;
    for (int i = 0; i < rows.size(); ++i)
        tallest = std::max(tallest, y.value(i) + rows[i].height);
    const double width = opts.padding * 2.0 + opts.nodeWidth * (maxDepth + 1) +
                         opts.columnGap * maxDepth;

    out.reserve(rows.size());
    for (int i = 0; i < rows.size(); ++i) {
        PlacedNode p;
        p.groupId = rows[i].key.startsWith(QStringLiteral("group:"))
                        ? rows[i].key.mid(QStringLiteral("group:").size())
                        : QString();
        p.depth = rows[i].depth;
        p.collapsed = rows[i].collapsed;
        p.hasChildren = rows[i].hasChildren;
        p.rect = QRectF(opts.padding + rows[i].depth * (opts.nodeWidth + opts.columnGap),
                        y.value(i), opts.nodeWidth, rows[i].height);
        out.append(p);
    }

    if (canvasSize) *canvasSize = QSizeF(width, tallest + opts.padding);
    return out;
}

}  // namespace mind
}  // namespace an
