#include "mind/mindmap.h"

#include "mind/layout.h"

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
    // Keep the user's zoom while editing: re-fitting on every change made the map jump
    // around and shrink to unreadable size inside a narrow panel. The first tree is fitted.
    if (!didInitialFit_) {
        didInitialFit_ = true;
        fitToContent();
    }
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

QString MindMapView::keyAt(const QPointF& scenePos) const
{
    if (!scene_) return {};
    for (QGraphicsItem* item : scene_->items(scenePos)) {
        // Cards carry their nodeKey in data(4) and so do their text children, so the whole
        // card is a hit target rather than just the painted rectangle.
        const QVariant key = item->data(4);
        if (key.isValid() && !key.toString().isEmpty()) return key.toString();
    }
    return {};
}

QRectF MindMapView::rectForKey(const QString& key) const
{
    if (!scene_ || key.isEmpty()) return {};
    for (QGraphicsItem* item : scene_->items()) {
        const QVariant k = item->data(4);
        if (k.isValid() && k.toString() == key) return item->sceneBoundingRect();
    }
    return {};
}

bool MindMapView::selectForMarkup(const QString& markupId)
{
    if (markupId.isEmpty()) return false;

    QStringList path;
    QString foundKey;
    std::function<bool(const Node&, bool)> walk = [&](const Node& n, bool inPath) -> bool {
        const QString key = nodeKey(n);
        if (!n.markupId.isEmpty() && n.markupId == markupId) {
            foundKey = key;
            if (inPath) path.append(key);
            return true;
        }
        for (const Node& c : n.children) {
            if (walk(c, true)) {
                if (inPath) path.append(key);
                return true;
            }
        }
        return false;
    };
    if (!walk(root_, false)) return false;

    bool expanded = false;
    for (const QString& key : path)
        if (collapsed_.remove(key)) expanded = true;
    if (expanded) rebuild();

    setSelectedKey(foundKey);
    const QRectF rect = rectForKey(foundKey);
    if (!rect.isNull()) ensureVisible(rect.adjusted(-40, -40, 40, 40), 20, 20);
    return true;
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

void MindMapView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (scene_) {
        const QString key = keyAt(mapToScene(event->pos()));
        if (!key.isEmpty()) {
            setSelectedKey(key);
            emit renameRequested(key);
            event->accept();
            return;
        }
    }
    QGraphicsView::mouseDoubleClickEvent(event);
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
    // Editing keys act on the selected card.
    if (!selectedKey_.isEmpty()) {
        switch (event->key()) {
        case Qt::Key_F2:
            emit renameRequested(selectedKey_);
            event->accept();
            return;
        case Qt::Key_Delete:
        case Qt::Key_Backspace:
            emit deleteRequested(selectedKey_);
            event->accept();
            return;
        case Qt::Key_Tab:
            emit childRequested(selectedKey_);
            event->accept();
            return;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            emit siblingRequested(selectedKey_);
            event->accept();
            return;
        case Qt::Key_Space:
            setCollapsed(selectedKey_, !isCollapsed(selectedKey_));
            event->accept();
            return;
        default:
            break;
        }
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
        const QString key = keyAt(scenePos);
        if (!key.isEmpty()) setSelectedKey(key);
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
    fitInView(r.adjusted(-24, -24, 24, 24), Qt::KeepAspectRatio);
    // Cards must stay readable: below 85% the titles start to blur, so scroll instead.
    if (transform().m11() < 0.85) {
        resetTransform();
        scale(0.85, 0.85);
    }
    zoom_ = transform().m11();
    didInitialFit_ = true;
    // Start at the root instead of whatever scroll offset the previous layout left behind.
    const double z = qMax(0.2, zoom_);
    centerOn(r.left() + viewport()->width() / (2.0 * z), r.center().y());
}

// One card: rounded box with a title, an excerpt block, the reader's own comment and a
// source footer. Heights are measured first so the layout can place variable-size cards
// without overlaps (see mind/layout.h).
namespace {

struct CardMetrics {
    double width = 0.0;
    double height = 0.0;
    double titleH = 0.0;
    double excerptH = 0.0;
    double commentH = 0.0;
    int excerptLines = 0;
};

QStringList wrapToLines(const QString& text, const QFontMetricsF& fm, double width, int maxLines)
{
    QStringList lines;
    QString line;
    bool truncated = false;
    for (const QChar& ch : text) {
        const QString candidate = line + ch;
        if (fm.horizontalAdvance(candidate) > width && !line.isEmpty()) {
            // Prefer a space boundary so Latin words are not split; CJK has no spaces and
            // breaks per character, which is the expected behaviour there.
            const int spaceAt = line.lastIndexOf(QLatin1Char(' '));
            if (spaceAt > 0 && ch.isLetterOrNumber() && line.at(spaceAt + 1).isLetterOrNumber()) {
                lines.append(line.left(spaceAt));
                line = line.mid(spaceAt + 1) + ch;
            } else {
                lines.append(line);
                line = ch;
            }
            if (lines.size() >= maxLines) {
                truncated = true;
                break;
            }
        } else {
            line = candidate;
        }
    }
    if (!line.isEmpty() && lines.size() < maxLines) {
        lines.append(line);
    } else if (!line.isEmpty()) {
        truncated = true;
    }
    if (truncated && !lines.isEmpty()) lines.last() += QStringLiteral("…");
    return lines;
}

CardMetrics measureCard(const Node& n, double width, bool isRoot)
{
    QFont titleFont;
    titleFont.setPixelSize(isRoot ? 15 : 13);
    titleFont.setBold(true);
    QFont bodyFont;
    bodyFont.setPixelSize(12);
    QFont smallFont;
    smallFont.setPixelSize(11);

    const QFontMetricsF titleFm(titleFont);
    const QFontMetricsF bodyFm(bodyFont);
    const QFontMetricsF smallFm(smallFont);

    CardMetrics m;
    m.width = width;
    m.titleH = qMin(2, wrapToLines(n.text, titleFm, width - 2 * kPadX, 2).size()) *
               titleFm.height();

    double body = 0;
    if (!n.excerpt.isEmpty() && !n.isGroup) {
        // The excerpt is a quote block: 2px rule on the left, muted text, max 3 lines.
        const QStringList lines = wrapToLines(n.excerpt, bodyFm, width - 2 * kPadX - 8, 3);
        m.excerptLines = lines.size();
        m.excerptH = lines.size() * bodyFm.height() + 8;
        body += m.excerptH + 6;
    }
    if (!n.comment.isEmpty()) {
        const QStringList lines = wrapToLines(n.comment, bodyFm, width - 2 * kPadX, 3);
        m.commentH = lines.size() * bodyFm.height();
        body += m.commentH + 4;
    } else if (n.isGroup && !n.children.isEmpty()) {
        body += bodyFm.height() + 4;   // "N 个节点" hint
    }
    m.height = 2 * kPadY + m.titleH + body + smallFm.height() + 6;
    return m;
}

}  // namespace

void MindMapView::setCollapsed(const QString& key, bool collapsed)
{
    if (key.isEmpty()) return;
    const bool was = collapsed_.contains(key);
    if (was == collapsed) return;
    if (collapsed) collapsed_.insert(key); else collapsed_.remove(key);
    rebuild();
}

void MindMapView::setSelectedKey(const QString& key)
{
    if (selectedKey_ == key) return;
    selectedKey_ = key;
    rebuild();
}

void MindMapView::rebuild()
{
    scene_->clear();
    nodeItems_ = 0;
    if (root_.text.isEmpty()) return;

    // Flatten first: heights must be known before the layout runs, and the layout must see
    // exactly the nodes that are visible (collapsed subtrees are skipped).
    QHash<QString, const Node*> byKey;
    QHash<QString, double> heights;
    QVector<const Node*> order;

    std::function<void(const Node&, int)> measure = [&](const Node& n, int depth) {
        const QString key = nodeKey(n);
        const double width = depth == 0 ? 190.0 : 206.0;
        const CardMetrics m = measureCard(n, width, depth == 0);
        byKey.insert(key, &n);
        heights.insert(key, m.height);
        order.append(&n);
        if (collapsed_.contains(key)) return;
        for (const Node& c : n.children) measure(c, depth + 1);
    };
    measure(root_, 0);

    LayoutOptions opts;
    opts.nodeWidth = 206.0;
    opts.columnGap = 34.0;
    opts.rowGap = 14.0;
    opts.padding = 16.0;

    // The root card is narrower; the layout uses one column width, so keep the wider value
    // for placement and let the root card sit left-aligned inside it.
    QSizeF canvas;
    const QVector<PlacedNode> placed =
        layoutTree(root_, heights, 64.0, opts, collapsed_, &canvas);

    QHash<QString, QRectF> rectOf;
    for (const PlacedNode& p : placed) { rectOf.insert(p.key, p.rect); }

    // Links first so cards paint over them.
    std::function<void(const Node&)> drawLinks = [&](const Node& n) {
        const QRectF from = rectOf.value(nodeKey(n));
        if (collapsed_.contains(nodeKey(n))) return;
        for (const Node& c : n.children) {
            const QRectF to = rectOf.value(nodeKey(c));
            if (from.isNull() || to.isNull()) continue;
            QPainterPath path;
            const QPointF a(from.right(), from.center().y());
            const QPointF b(to.left(), to.center().y());
            const double mx = (a.x() + b.x()) / 2.0;
            path.moveTo(a);
            path.cubicTo(QPointF(mx, a.y()), QPointF(mx, b.y()), b);
            auto* item = scene_->addPath(path, QPen(QColor(0xc6, 0xd0, 0xde), 1.6));
            item->setZValue(1);
            drawLinks(c);
        }
    };
    drawLinks(root_);

    // Cards.
    std::function<void(const Node&, int)> drawCards = [&](const Node& n, int depth) {
        const QString key = nodeKey(n);
        const QRectF rect = rectOf.value(key);
        if (rect.isNull()) return;

        QFont titleFont;
        titleFont.setPixelSize(depth == 0 ? 15 : 13);
        titleFont.setBold(true);
        QFont bodyFont;
        bodyFont.setPixelSize(12);
        QFont smallFont;
        smallFont.setPixelSize(11);
        const QFontMetricsF titleFm(titleFont);
        const QFontMetricsF bodyFm(bodyFont);

        const bool selected = selectedKey_ == key;
        QPen border(selected ? QColor(0x2f, 0x6b, 0xd8) : QColor(0xe3, 0xe7, 0xee),
                    selected ? 1.6 : 1.0);
        QColor fill = QColor(0xff, 0xff, 0xff);
        if (n.isGroup) fill = QColor(0xf2, 0xf7, 0xff);
        if (depth == 0) fill = QColor(0x2f, 0x6b, 0xd8);

        auto* card = scene_->addRect(rect, border, QBrush(fill));
        card->setZValue(2);
        card->setData(0, n.page);
        card->setData(2, n.groupKey);
        card->setData(4, key);   // nodeKey(): hit target for select/rename/keys
        if (!n.markupId.isEmpty()) {
            card->setData(1, n.markupId);
            card->setFlag(QGraphicsItem::ItemIsMovable, true);
        }
        if (selected) card->setData(3, 1);   // hit-test helper: selected marker

        double y = rect.top() + kPadY;
        const QColor titleColor = depth == 0 ? QColor(Qt::white) : QColor(0x1d, 0x21, 0x29);
        const QColor bodyColor = depth == 0 ? QColor(0xe8, 0xef, 0xfb) : QColor(0x5b, 0x64, 0x72);
        const QColor accent = depth == 0 ? QColor(0xc9, 0xdc, 0xfb) : QColor(0x2f, 0x6b, 0xd8);

        for (const QString& line : wrapToLines(n.text, titleFm, rect.width() - 2 * kPadX, 2)) {
            auto* t = scene_->addSimpleText(line, titleFont);
            t->setBrush(QBrush(titleColor));
            t->setPos(rect.left() + kPadX, y);
            t->setZValue(3);
            if (!n.markupId.isEmpty()) t->setData(1, n.markupId);
            t->setData(4, key);
            y += titleFm.height();
        }

        if (!n.excerpt.isEmpty() && !n.isGroup) {
            y += 3;
            const double textW = rect.width() - 2 * kPadX - 8;
            const QStringList lines = wrapToLines(n.excerpt, bodyFm, textW, 3);
            const double blockH = lines.size() * bodyFm.height() + 8;
            auto* rule = scene_->addRect(QRectF(rect.left() + kPadX, y + 2, 2, blockH - 4),
                                         QPen(Qt::NoPen), QBrush(QColor(0xd7, 0xe1, 0xf2)));
            rule->setZValue(3);
            double ly = y + 4;
            for (const QString& line : lines) {
                auto* t = scene_->addSimpleText(line, bodyFont);
                t->setBrush(QBrush(bodyColor));
                t->setPos(rect.left() + kPadX + 8, ly);
                t->setZValue(3);
                t->setData(4, key);
                ly += bodyFm.height();
            }
            y += blockH + 3;
        }

        if (!n.comment.isEmpty()) {
            for (const QString& line : wrapToLines(n.comment, bodyFm, rect.width() - 2 * kPadX, 3)) {
                auto* t = scene_->addSimpleText(line, bodyFont);
                t->setBrush(QBrush(depth == 0 ? bodyColor : QColor(0x1d, 0x21, 0x29)));
                t->setPos(rect.left() + kPadX, y);
                t->setZValue(3);
                t->setData(4, key);
                y += bodyFm.height();
            }
            y += 2;
        } else if (n.isGroup && !n.children.isEmpty()) {
            auto* t = scene_->addSimpleText(
                QStringLiteral("%1 个子节点").arg(n.children.size()), bodyFont);
            t->setBrush(QBrush(bodyColor));
            t->setPos(rect.left() + kPadX, y);
            t->setZValue(3);
            y += bodyFm.height() + 2;
        }

        // footer: source on the left, affordance on the right. Page buckets are containers,
        // not quotations, so they do not claim to be "原文".
        const bool isBucket = key.startsWith(QStringLiteral("page:"));
        if (!isBucket && (!n.source.isEmpty() || n.page >= 0)) {
            auto* foot = scene_->addSimpleText(n.source.isEmpty() ? QStringLiteral("原文")
                                                                  : n.source,
                                               smallFont);
            foot->setBrush(QBrush(accent));
            foot->setPos(rect.left() + kPadX, rect.bottom() - kPadY - foot->boundingRect().height());
            foot->setZValue(3);
        }
        if (!n.children.isEmpty()) {
            auto* badge = scene_->addSimpleText(QStringLiteral("%1").arg(n.children.size()),
                                                smallFont);
            badge->setBrush(QBrush(accent));
            badge->setPos(rect.right() - kPadX - badge->boundingRect().width(),
                          rect.bottom() - kPadY - badge->boundingRect().height());
            badge->setZValue(3);
        }

        ++nodeItems_;
        if (!collapsed_.contains(key))
            for (const Node& c : n.children) drawCards(c, depth + 1);
    };
    drawCards(root_, 0);

    scene_->setSceneRect(QRectF(0, 0, qMax(canvas.width(), 320.0), qMax(canvas.height(), 240.0)));
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
