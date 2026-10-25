#include "mind/mindmap.h"

#include <QFontMetricsF>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsSimpleTextItem>
#include <QPainter>
#include <QPainterPath>
#include <QCursor>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QtMath>

#include <algorithm>
#include <functional>
#include <functional>

namespace an {
namespace mind {
namespace {

constexpr double kHSpace = 56;
constexpr double kVSpace = 14;
constexpr double kPadX = 12;
constexpr double kPadY = 7;

QColor colorForDepth(int depth)
{
    switch (depth) {
    case 0: return QColor(0x1b, 0x4f, 0xa8);
    case 1: return QColor(0x2a, 0x6f, 0xd0);
    case 2: return QColor(0x34, 0xa8, 0x53);
    default: return QColor(0x6b, 0x72, 0x80);
    }
}

QColor fillForDepth(int depth)
{
    switch (depth) {
    case 0: return QColor(0x1b, 0x4f, 0xa8);
    case 1: return QColor(0xe8, 0xf0, 0xfc);
    case 2: return QColor(0xea, 0xf7, 0xed);
    default: return QColor(0xf2, 0xf4, 0xf7);
    }
}

}  // namespace

MindMapView::MindMapView(QWidget* parent)
    : QGraphicsView(parent)
{
    scene_ = new QGraphicsScene(this);
    setScene(scene_);
    setRenderHint(QPainter::Antialiasing, true);
    setDragMode(QGraphicsView::ScrollHandDrag);
    setBackgroundBrush(QColor(0xfa, 0xfb, 0xfd));
    setFrameShape(QFrame::NoFrame);
}

void MindMapView::setTree(const Node& root)
{
    root_ = root;
    rebuild();
    fitToContent();
}

int MindMapView::sceneNodeCount() const
{
    return nodeItems_;
}

void MindMapView::setZoom(double factor)
{
    zoom_ = qBound(0.2, factor, 4.0);
    resetTransform();
    scale(zoom_, zoom_);
}

QString MindMapView::groupKeyAt(const QPointF& scenePos) const
{
    if (!scene_) return {};
    for (QGraphicsItem* item : scene_->items(scenePos)) {
        const QVariant group = item->data(2);
        if (group.isValid()) return group.toString();
        const QVariant id = item->data(1);
        if (id.isValid()) {
            // a note behaves like its group (groups can be nested now)
            std::function<QString(const Node&)> findLeaf = [&](const Node& node) -> QString {
                for (const Node& child : node.children) {
                    if (child.markupId == id.toString()) return child.groupKey;
                    const QString nested = findLeaf(child);
                    if (!nested.isNull()) return nested;
                }
                return QString();
            };
            const QString key = findLeaf(root_);
            if (!key.isNull()) return key;
        }
    }
    return {};
}

bool MindMapView::dropTargetAt(const QPointF& scenePos, QString* groupKey,
                               QString* beforeId) const
{
    if (!scene_) return false;
    if (groupKey) groupKey->clear();
    if (beforeId) beforeId->clear();

    // A note under the cursor: insert before it when dropping in its upper half.
    for (QGraphicsItem* item : scene_->items(scenePos)) {
        const QVariant id = item->data(1);
        if (!id.isValid()) continue;
        const QRectF box = item->sceneBoundingRect();
        const bool upperHalf = scenePos.y() < box.center().y();
        if (groupKey) *groupKey = groupKeyAt(scenePos);
        if (beforeId) {
            if (upperHalf) {
                *beforeId = id.toString();
            } else if (beforeId) {
                *beforeId = QString();   // append right after this note
            }
        }
        return true;
    }

    // A group node (or page bucket): append to it.
    const QString group = groupKeyAt(scenePos);
    if (!group.isEmpty()) {
        if (groupKey) *groupKey = group;
        return true;
    }
    return false;
}

QString MindMapView::markupIdAt(const QPointF& scenePos) const
{
    if (!scene_) return {};
    for (QGraphicsItem* item : scene_->items(scenePos)) {
        const QVariant id = item->data(1);
        if (id.isValid()) return id.toString();
    }
    return {};
}

QPointF MindMapView::nodeCenter(const QString& markupId) const
{
    if (!scene_ || markupId.isEmpty()) return {};
    for (QGraphicsItem* item : scene_->items()) {
        if (item->data(1).toString() == markupId) return item->sceneBoundingRect().center();
    }
    return {};
}

void MindMapView::mouseReleaseEvent(QMouseEvent* event)
{
    if (!dragMarkupId_.isEmpty() && dragMoved_) {
        const QString id = dragMarkupId_;
        dragMarkupId_.clear();
        dragMoved_ = false;
        const QPointF scenePos = mapToScene(event->pos());

        QString group;
        QString before;
        if (dropTargetAt(scenePos, &group, &before) && !before.isEmpty()) {
            emit noteReorderRequested(id, group, before);
            return;
        }
        if (!group.isEmpty()) {
            emit nodeReparented(id, group);
            return;
        }
        emit nodeReparented(id, QString());   // dropped on empty canvas -> page bucket
        return;
    }
    dragMarkupId_.clear();
    dragMoved_ = false;
    QGraphicsView::mouseReleaseEvent(event);
}

void MindMapView::keyPressEvent(QKeyEvent* event)
{
    // Alt+Up / Alt+Down: move the focused note inside its group.
    if ((event->modifiers() & Qt::AltModifier) &&
        (event->key() == Qt::Key_Up || event->key() == Qt::Key_Down)) {
        const QString id = markupIdAt(mapToScene(mapFromGlobal(QCursor::pos())));
        if (id.isEmpty()) {
            QGraphicsView::keyPressEvent(event);
            return;
        }

        // find the current group order
        QString group;
        QStringList siblings;
        for (const Node& g : root_.children) {
            for (int i = 0; i < g.children.size(); ++i) {
                if (g.children.at(i).markupId == id) {
                    group = g.groupKey;
                    for (const Node& leaf : g.children)
                        if (!leaf.markupId.isEmpty()) siblings.append(leaf.markupId);
                }
            }
        }
        const int index = siblings.indexOf(id);
        if (index < 0) {
            QGraphicsView::keyPressEvent(event);
            return;
        }
        if (event->key() == Qt::Key_Up && index > 0) {
            emit noteReorderRequested(id, group, siblings.at(index - 1));
        } else if (event->key() == Qt::Key_Down && index + 2 < siblings.size()) {
            emit noteReorderRequested(id, group, siblings.at(index + 2));
        }
        event->accept();
        return;
    }
    QGraphicsView::keyPressEvent(event);
}

void MindMapView::mousePressEvent(QMouseEvent* event)
{
    if (scene_) {
        const QPointF scenePos = mapToScene(event->pos());
        const QString id = markupIdAt(scenePos);
        if (!id.isEmpty()) {
            dragMarkupId_ = id;
            dragStartScene_ = scenePos;
            dragMoved_ = false;
            QGraphicsView::mousePressEvent(event);
            return;
        }
        for (QGraphicsItem* item : scene_->items(scenePos)) {
            const QVariant data = item->data(0);
            if (data.isValid()) {
                emit nodeActivated(data.toInt());
                event->accept();
                return;
            }
        }
    }
    QGraphicsView::mousePressEvent(event);
}

void MindMapView::contextMenuEvent(QContextMenuEvent* event)
{
    if (scene_) {
        const QString key = groupKeyAt(mapToScene(event->pos()));
        if (key.startsWith(QLatin1String("group:"))) {
            emit groupMenuRequested(key, event->globalPos());
            event->accept();
            return;
        }
    }
    QGraphicsView::contextMenuEvent(event);
}

void MindMapView::wheelEvent(QWheelEvent* event)
{
    if (event->modifiers() & Qt::ControlModifier) {
        setZoom(zoom_ * (event->angleDelta().y() > 0 ? 1.1 : 1 / 1.1));
        event->accept();
        return;
    }
    QGraphicsView::wheelEvent(event);
}

void MindMapView::fitToContent()
{
    if (!scene_) return;
    const QRectF r = scene_->itemsBoundingRect();
    if (r.isEmpty()) return;
    fitInView(r.adjusted(-30, -30, 30, 30), Qt::KeepAspectRatio);
    zoom_ = transform().m11();
}

void MindMapView::rebuild()
{
    scene_->clear();
    nodeItems_ = 0;
    if (root_.text.isEmpty()) return;

    QFont font;
    font.setPixelSize(13);
    QFontMetricsF metrics(font);
    font.setBold(true);
    QFontMetricsF rootMetrics(font);

    // --- pass 1: subtree heights (leaves are stacked vertically) ---
    std::function<double(const Node&, int)> height = [&](const Node& n, int depth) -> double {
        const QFontMetricsF& fm = depth == 0 ? rootMetrics : metrics;
        const double boxH = fm.height() + 2 * kPadY;
        if (n.children.isEmpty()) return boxH;
        double sum = 0;
        for (const Node& c : n.children) sum += height(c, depth + 1) + kVSpace;
        sum -= kVSpace;
        return qMax(boxH, sum);
    };

    struct Placed {
        QPointF topLeft;
        QSizeF size;
        int depth = 0;
    };
    QVector<Placed> placed;

    // --- pass 2: place boxes and draw ---
    std::function<void(const Node&, int, double, double)> place =
        [&](const Node& n, int depth, double x, double centerY) {
            const QFontMetricsF& fm = depth == 0 ? rootMetrics : metrics;
            QFont f = font;
            f.setBold(depth <= 1);
            f.setPixelSize(depth == 0 ? 15 : 13);
            const QFontMetricsF m2(f);
            const double w = qMin(320.0, m2.horizontalAdvance(n.text) + 2 * kPadX);
            const double h = m2.height() + 2 * kPadY;
            const QRectF box(x, centerY - h / 2, w, h);

            auto* rect = scene_->addRect(box, QPen(colorForDepth(depth), depth == 0 ? 0 : 1.2),
                                         QBrush(fillForDepth(depth)));
            rect->setZValue(2);
            if (n.page >= 0) rect->setData(0, n.page);   // clickable -> jump to page
            if (!n.groupKey.isEmpty()) rect->setData(2, n.groupKey);
            if (!n.markupId.isEmpty()) {
                rect->setData(1, n.markupId);
                rect->setFlag(QGraphicsItem::ItemIsMovable, true);   // drag to regroup
            }
            auto* text = scene_->addSimpleText(n.text, f);
            if (!n.markupId.isEmpty()) text->setData(1, n.markupId);
            text->setBrush(depth == 0 ? QBrush(Qt::white) : QBrush(QColor(0x23, 0x2a, 0x33)));
            text->setPos(box.left() + kPadX, box.top() + kPadY);
            text->setZValue(3);
            ++nodeItems_;
            placed.append({box.topLeft(), box.size(), depth});

            if (n.children.isEmpty()) return;

            double totalH = 0;
            for (const Node& c : n.children) totalH += height(c, depth + 1) + kVSpace;
            totalH -= kVSpace;
            double cursor = centerY - totalH / 2;
            const double childX = x + w + kHSpace;

            for (const Node& c : n.children) {
                const double ch = height(c, depth + 1);
                const double childCenter = cursor + ch / 2;

                QPainterPath link;
                const QPointF from(box.right(), centerY);
                const QPointF to(childX, childCenter);
                const double midX = (from.x() + to.x()) / 2;
                link.moveTo(from);
                link.cubicTo(QPointF(midX, from.y()), QPointF(midX, to.y()), to);
                auto* linkItem = scene_->addPath(link, QPen(colorForDepth(depth + 1).lighter(125), 1.4));
                linkItem->setZValue(1);

                place(c, depth + 1, childX, childCenter);
                cursor += ch + kVSpace;
            }
        };

    const double totalRoot = height(root_, 0);
    place(root_, 0, 0, totalRoot / 2);
}

QImage MindMapView::renderToImage(const QSize& size)
{
    if (!scene_ || size.isEmpty()) return {};
    QImage img(size, QImage::Format_RGB32);
    img.fill(QColor(0xfa, 0xfb, 0xfd));
    const QRectF bounds = scene_->itemsBoundingRect().adjusted(-20, -20, 20, 20);
    if (bounds.isEmpty()) return img;
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    scene_->render(&p, QRectF(img.rect()), bounds, Qt::KeepAspectRatio);
    p.end();
    return img;
}

bool MindMapView::exportPng(const QString& path, const QSize& size)
{
    const QImage img = renderToImage(size);
    if (img.isNull()) return false;
    return img.save(path);
}

}  // namespace mind
}  // namespace an
