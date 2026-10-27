#pragma once

#include "core/model.h"
#include "mind/tree.h"

#include <QHash>
#include <QRectF>
#include <QSet>
#include <QString>
#include <QVector>

namespace an {
namespace mind {

// Geometry for one mind-map node. The layout is computed from measured card sizes, so the
// same algorithm works whether a card shows a one-line title or a three-line excerpt.
struct PlacedNode {
    QString key;       // nodeKey(): "group:<id>" or "page:<n>" - matches the Node it came from
    QString groupId;   // group id, or empty for page buckets
    QRectF rect;       // in canvas coordinates
    int depth = 0;
    bool collapsed = false;
    bool hasChildren = false;
};

struct LayoutOptions {
    double nodeWidth = 206.0;
    double columnGap = 30.0;
    double rowGap = 13.0;
    double padding = 16.0;
};

// Lays out a group tree left-to-right. Input is the same Node tree the view and the
// exporters already build, so "what you export" and "what you see" stay identical.
//
// Rules (they are what makes the map readable, and each one is asserted by a test):
//   1. one column per depth level, equal widths;
//   2. the deepest level is stacked top-down, then every parent is centred on the vertical
//      span of its children;
//   3. a parent never overlaps the sibling above it (cursor + rowGap);
//   4. a node that is collapsed contributes nothing below it.
//
// `measured` supplies the card height per node key (group id, or "page:<n>"); a node that
// is missing from the map falls back to `defaultHeight`.
QVector<PlacedNode> layoutTree(const Node& root,
                               const QHash<QString, double>& measured,
                               double defaultHeight = 64.0,
                               const LayoutOptions& opts = {},
                               const QSet<QString>& collapsedKeys = {},
                               QSizeF* canvasSize = nullptr);

// Key used by the view when measuring a node: group id when present, else "page:<n>".
QString nodeKey(const Node& n);

}  // namespace mind
}  // namespace an
