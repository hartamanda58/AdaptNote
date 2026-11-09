#include "mind/mindmap.h"

#include "mind/layout.h"

#include <QFontMetricsF>
#include <QGraphicsPathItem>
#include <QGraphicsRectItem>
#include <QGraphicsPathItem>
#include <QLinearGradient>
#include <QGraphicsSimpleTextItem>
#include <QGraphicsTextItem>
#include <QFocusEvent>
#include <QMetaObject>
#include <QMimeData>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QTextCursor>
#include <QPainter>
#include <QPainterPath>
#include <QCursor>
#include <QApplication>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QScrollBar>
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
    setDragMode(QGraphicsView::NoDrag);
    setBackgroundBrush(QColor(0xfa, 0xfb, 0xfd));
    setFrameShape(QFrame::NoFrame);
    setAcceptDrops(true);
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
        // Text and decoration items are separate scene items, not children of the card.
        // Their node key is the reliable route back to the corresponding tree node.
        QString key;
        for (QGraphicsItem* hit = item; hit && key.isEmpty(); hit = hit->parentItem())
            key = hit->data(4).toString();
        if (key.isEmpty()) continue;
        QString found;
        std::function<void(const Node&)> walk = [&](const Node& node) {
            if (nodeKey(node) == key) { found = node.groupKey; return; }
            for (const Node& child : node.children) if (found.isEmpty()) walk(child);
        };
        walk(root_);
        if (!found.isEmpty()) return found;
    }
    return {};
}

bool MindMapView::dropTargetAt(const QPointF& scenePos, QString* groupKey,
                               QString* beforeId) const
{
    if (!scene_) return false;
    if (groupKey) groupKey->clear();
    if (beforeId) beforeId->clear();

    // A note under the cursor: insert before it when dropping in its upper half. The note
    // being dragged follows the cursor and would otherwise be its own drop target.
    for (QGraphicsItem* item : scene_->items(scenePos)) {
        QString targetId;
        QRectF box;
        for (QGraphicsItem* card = item; card; card = card->parentItem()) {
            const QString key = card->data(4).toString();
            if (!card->data(7).toBool() || !key.startsWith(QStringLiteral("note:"))) continue;
            targetId = key.mid(5);
            box = card->sceneBoundingRect();
            break;
        }
        if (targetId.isEmpty() || (!dragMarkupId_.isEmpty() && targetId == dragMarkupId_)) continue;
        const bool upperHalf = scenePos.y() < box.center().y();
        if (groupKey) *groupKey = groupKeyAt(scenePos);
        if (beforeId) *beforeId = upperHalf ? targetId : QString();
        return true;
    }

    // A group node (or page bucket): append to it. groupKeyAt() must ignore the dragged
    // card as well, otherwise it reports the note's current group.
    QString group;
    for (QGraphicsItem* item : scene_->items(scenePos)) {
        for (QGraphicsItem* card = item; card; card = card->parentItem()) {
            if (!card->data(7).toBool()) continue;
            if (card->data(1).isValid() && !dragMarkupId_.isEmpty() &&
                card->data(1).toString() == dragMarkupId_) continue;
            group = card->data(2).toString();
            break;
        }
        if (!group.isEmpty()) break;
    }
    if (!group.isEmpty()) {
        if (groupKey) *groupKey = group;
        return true;
    }
    // Page/document containers are valid hierarchy targets even though they are not
    // user-created MindGroup records.
    for (QGraphicsItem* item : scene_->items(scenePos)) {
        const QString key = item->data(4).toString();
        if (item->data(7).toBool() && (key.startsWith(QStringLiteral("page:")) ||
                                       key.startsWith(QStringLiteral("docpage:")) ||
                                       key.startsWith(QStringLiteral("document:")))) {
            if (groupKey) *groupKey = key;
            return true;
        }
    }
    return false;
}

QString MindMapView::keyAt(const QPointF& scenePos) const
{
    if (!scene_) return {};
    for (QGraphicsItem* item : scene_->items(scenePos)) {
        for (QGraphicsItem* hit = item; hit; hit = hit->parentItem()) {
            const QString key = hit->data(4).toString();
            if (!key.isEmpty()) return key;
        }
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

void MindMapView::clearDropIndicator()
{
    if (dropIndicator_) {
        if (scene_) scene_->removeItem(dropIndicator_);
        delete dropIndicator_;
        dropIndicator_ = nullptr;
    }
}

void MindMapView::updateDropIndicator(const QPointF& scenePos)
{
    clearDropIndicator();
    if (!scene_ ||
        (dragMarkupId_.isEmpty() && dragGroupKey_.isEmpty() && !externalDropActive_) ||
        (!dragMoved_ && !externalDropActive_))
        return;

    if (!dragGroupKey_.isEmpty()) {
        const QColor accent(0x2f, 0x6b, 0xd8);
        const QString target = groupKeyAt(scenePos);
        if (target.isEmpty() || target == dragGroupKey_) {
            QFont font;
            font.setPixelSize(11);
            auto* hint = scene_->addSimpleText(QStringLiteral("放开：移到分组末尾"), font);
            hint->setBrush(QBrush(QColor(0x98, 0xa2, 0xb3)));
            hint->setPos(scenePos + QPointF(14, 8));
            hint->setZValue(20);
            dropIndicator_ = hint;
            return;
        }
        const QRectF box = rectForKey(target);
        if (!box.isNull()) {
            auto* line = scene_->addLine(box.left() - 2, box.top() - 6, box.right() + 2,
                                         box.top() - 6, QPen(accent, 2.6));
            line->setZValue(20);
            dropIndicator_ = line;
        }
        return;
    }

    QString group;
    QString beforeId;
    const bool hasTarget = dropTargetAt(scenePos, &group, &beforeId);
    const QColor accent(0x2f, 0x6b, 0xd8);

    if (!hasTarget || group.isEmpty()) {
        // Empty canvas: the note falls back to its page bucket. Say so instead of drawing
        // nothing, otherwise the drop looks like it failed.
        if (dropIndicator_) return;
        QFont font;
        font.setPixelSize(11);
        auto* hint = scene_->addSimpleText(QStringLiteral("放开：回到原页分组"), font);
        hint->setBrush(QBrush(QColor(0x98, 0xa2, 0xb3)));
        hint->setPos(scenePos + QPointF(14, 8));
        hint->setZValue(20);
        dropIndicator_ = hint;
        return;
    }

    if (!beforeId.isEmpty()) {
        // Insertion line above the note it will go in front of.
        for (QGraphicsItem* item : scene_->items()) {
            const QVariant id = item->data(1);
            if (!id.isValid() || id.toString() != beforeId) continue;
            const QRectF box = item->sceneBoundingRect();
            auto* line = scene_->addLine(box.left() - 2, box.top() - 4, box.right() + 2,
                                         box.top() - 4, QPen(accent, 2.4));
            line->setZValue(20);
            dropIndicator_ = line;
            return;
        }
    }

    // Joining a group: outline the card so the destination is unambiguous.
    const QRectF groupRect = rectForKey(group);
    if (!groupRect.isNull()) {
        QPen pen(accent, 1.8, Qt::DashLine);
        auto* box = scene_->addRect(groupRect.adjusted(-3, -3, 3, 3), pen,
                                    QBrush(QColor(0x2f, 0x6b, 0xd8, 26)));
        box->setZValue(20);
        dropIndicator_ = box;
    }
}

void MindMapView::mouseMoveEvent(QMouseEvent* event)
{
    if (bandActive_ && bandItem_ && scene_) {
        const QRectF band = QRectF(bandStart_, mapToScene(event->pos())).normalized();
        bandItem_->setRect(band);
        selectedKeys_ = bandAdditive_ ? bandBase_ : QVector<QString>();
        for (QGraphicsItem* item : scene_->items()) {
            const QString key = item->data(4).toString();
            if (!key.startsWith(QStringLiteral("note:"))) continue;
            auto* card = dynamic_cast<QAbstractGraphicsShapeItem*>(item);
            if (!card || !card->data(7).toBool()) continue;
            if (band.intersects(rectForKey(key)) && !selectedKeys_.contains(key))
                selectedKeys_.append(key);
        }
        selectedKey_ = selectedKeys_.isEmpty() ? QString() : selectedKeys_.last();
        syncSelectionBorders();
        event->accept();
        return;
    }
    if (!panActive_ && event->buttons().testFlag(Qt::MiddleButton)) {
        panActive_ = true;
        panStart_ = event->pos();
        panLast_ = event->pos();
        panStartH_ = horizontalScrollBar()->value();
        panStartV_ = verticalScrollBar()->value();
        setCursor(Qt::ClosedHandCursor);
    }
    if (panActive_) {
        const QPoint delta = event->pos() - panLast_;
        panLast_ = event->pos();
        // Translate the canvas itself instead of relying only on scrollbar range: small
        // maps still pan naturally when they initially fit inside the viewport.
        const double factor = qMax(0.2, zoom_);
        translate(delta.x() / factor, delta.y() / factor);
        horizontalScrollBar()->setValue(horizontalScrollBar()->value() - delta.x());
        verticalScrollBar()->setValue(verticalScrollBar()->value() - delta.y());
        event->accept();
        return;
    }
    if (nodePressActive_ && (event->pos() - pressStart_).manhattanLength() >= QApplication::startDragDistance()) {
        if (!dragMarkupId_.isEmpty() || !dragGroupKey_.isEmpty()) {
            dragStartScene_ = mapToScene(pressStart_);
            dragMoved_ = true;
            nodePressActive_ = false;
        } else {
            panActive_ = true;
            panStart_ = pressStart_;
            panLast_ = pressStart_;
            panStartH_ = horizontalScrollBar()->value();
            panStartV_ = verticalScrollBar()->value();
            setCursor(Qt::ClosedHandCursor);
            nodePressActive_ = false;
        }
    }
    if (!dragMarkupId_.isEmpty() || !dragGroupKey_.isEmpty()) {
        const QPointF scenePos = mapToScene(event->pos());
        if (!dragMoved_ && (scenePos - dragStartScene_).manhattanLength() > 6.0)
            dragMoved_ = true;
        if (dragMoved_) updateDropIndicator(scenePos);
    }
    QGraphicsView::mouseMoveEvent(event);
}

void MindMapView::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() != Qt::LeftButton) { QGraphicsView::mouseDoubleClickEvent(event); return; }
    if (renameEditor_) {
        const QPointF scenePos = mapToScene(event->pos());
        if (renameEditor_->sceneBoundingRect().contains(scenePos)) {
            QGraphicsView::mouseDoubleClickEvent(event);   // select a word, not a new rename
            return;
        }
    }
    if (scene_) {
        const QString key = keyAt(mapToScene(event->pos()));
        if (!key.isEmpty()) {
            setSelectedKey(key);
            if (key.startsWith(QStringLiteral("group:")))
                emit renameRequested(key);
            else
                beginInlineRename(key);
            event->accept();
            return;
        }
    }
    QGraphicsView::mouseDoubleClickEvent(event);
}

namespace {
// Editing happens on the card itself. Deleting a QGraphicsTextItem from inside its own
// keyPressEvent would be use-after-free, so the item only hides and the commit is queued
// on the view; by the time it runs the event has unwound and the scene can be rebuilt.
class InlineCardEditor : public QGraphicsTextItem {
public:
    explicit InlineCardEditor(MindMapView* host)
        : host_(host)
    {
    }

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
            // Accept before committing: an unaccepted key would bubble to the view and
            // trigger its "create sibling" shortcut while the rename dialog is closing.
            event->accept();
            queueCommit(true);
            return;
        }
        if (event->key() == Qt::Key_Escape) {
            event->accept();
            queueCommit(false);
            return;
        }
        QGraphicsTextItem::keyPressEvent(event);
    }
    void focusOutEvent(QFocusEvent* event) override
    {
        QGraphicsTextItem::focusOutEvent(event);
        queueCommit(true);
    }

private:
    void queueCommit(bool commit)
    {
        if (queued_) return;
        queued_ = true;
        setVisible(false);
        MindMapView* host = host_;
        QMetaObject::invokeMethod(
            host, [host, commit]() { host->commitInlineRename(commit); }, Qt::QueuedConnection);
    }

    MindMapView* host_ = nullptr;
    bool queued_ = false;
};
}  // namespace

QString MindMapView::nodeText(const QString& key) const
{
    QString result;
    bool found = false;
    std::function<void(const Node&)> walk = [&](const Node& node) {
        if (found) return;
        if (nodeKey(node) == key) {
            result = node.text;
            found = true;
            return;
        }
        for (const Node& child : node.children) walk(child);
    };
    walk(root_);
    return result;
}

void MindMapView::beginInlineRename(const QString& key)
{
    if (!scene_ || key.isEmpty()) return;
    if (renameEditor_) commitInlineRename(true);
    const QRectF rect = rectForKey(key);
    if (rect.isEmpty()) return;

    renameBackdrop_ = scene_->addRect(rect.adjusted(3, 3, -3, -3), QPen(QColor("#0a84ff"), 2),
                                      QBrush(Qt::white));
    renameBackdrop_->setZValue(100);

    auto* editor = new InlineCardEditor(this);
    editor->setPlainText(nodeText(key));
    editor->setTextWidth(qMax(80.0, rect.width() - 20));
    editor->setPos(rect.left() + 10, rect.top() + 6);
    editor->setZValue(101);
    editor->setDefaultTextColor(QColor("#1d1d1f"));
    QFont font = editor->font();
    font.setPointSizeF(10);
    editor->setFont(font);
    editor->setTextInteractionFlags(Qt::TextEditorInteraction);
    scene_->addItem(editor);
    editor->setFocus(Qt::MouseFocusReason);
    QTextCursor cursor = editor->textCursor();
    cursor.select(QTextCursor::Document);
    editor->setTextCursor(cursor);

    renameEditor_ = editor;
    renameKey_ = key;
}

void MindMapView::setInlineRenameTextForTest(const QString& text)
{
    if (renameEditor_) renameEditor_->setPlainText(text);
}

void MindMapView::commitInlineRename(bool commit)
{
    QGraphicsTextItem* editor = renameEditor_;
    QGraphicsItem* backdrop = renameBackdrop_;
    const QString key = renameKey_;
    renameEditor_ = nullptr;
    renameBackdrop_ = nullptr;
    renameKey_.clear();

    const QString text = editor ? editor->toPlainText().trimmed() : QString();
    if (editor) {
        if (editor->scene()) editor->scene()->removeItem(editor);
        delete editor;
    }
    if (backdrop) {
        if (backdrop->scene()) backdrop->scene()->removeItem(backdrop);
        delete backdrop;
    }
    if (commit && !key.isEmpty() && !text.isEmpty()) emit renameCommitted(key, text);
    setFocus(Qt::OtherFocusReason);
}

void MindMapView::mouseReleaseEvent(QMouseEvent* event)
{
    clearDropIndicator();
    if (bandActive_) {
        bandActive_ = false;
        if (bandItem_) {
            if (bandItem_->scene()) bandItem_->scene()->removeItem(bandItem_);
            delete bandItem_;
            bandItem_ = nullptr;
        }
        unsetCursor();
        emit nodeSelected(selectedKey_);
        event->accept();
        return;
    }
    if (panActive_) {
        panActive_ = false;
        setCursor(linkMode_ ? Qt::CrossCursor : Qt::ArrowCursor);
        event->accept();
        return;
    }
    if (!dragGroupKey_.isEmpty() && dragMoved_) {
        const QString dragged = dragGroupKey_;
        const QString target = groupKeyAt(mapToScene(event->pos()));
        dragGroupKey_.clear();
        dragMoved_ = false;
        nodePressActive_ = false;
        pressKey_.clear();
        dragContainerKey_.clear();
        if (target != dragged) emit groupReorderRequested(dragged, target);
        return;
    }
    if (nodePressActive_) {
        nodePressActive_ = false;
        const QString key = pressKey_;
        pressKey_.clear();
        dragMarkupId_.clear();
        if (!key.isEmpty()) {
            if (!isKeySelected(key)) setSelectedKey(key);
            emit nodeSelected(key);
            if (key.startsWith(QStringLiteral("note:"))) emit markupActivated(key.mid(5));
            else emit nodeActivated(pageByKey_.value(key, -1));
        }
        event->accept();
        return;
    }
    if (!dragMarkupId_.isEmpty() && dragMoved_) {
        const QString id = dragMarkupId_;
        const QPointF scenePos = mapToScene(event->pos());

        // Dragging one card of a multi-selection moves every selected note into the
        // target group (positions inside the group are re-laid out).
        const QStringList batch = selectedNoteIds();
        if (batch.size() > 1 && batch.contains(id)) {
            QString targetGroup;
            QString before;
            const bool hasTarget = dropTargetAt(scenePos, &targetGroup, &before);
            bool sameGroup = hasTarget && !targetGroup.isEmpty();
            if (sameGroup)
                for (const QString& noteId : batch)
                    if (groupKeyOfNote(noteId) != targetGroup) sameGroup = false;
            dragMarkupId_.clear();
            dragMoved_ = false;
            nodePressActive_ = false;
            pressKey_.clear();
            dragContainerKey_.clear();
            if (hasTarget && !targetGroup.isEmpty()) {
                if (sameGroup)
                    emit selectionReorderRequested(batch, targetGroup, before);
                else
                    emit selectionReparentRequested(batch, targetGroup);
            }
            return;
        }

        // Resolve the target *before* clearing the drag state: dropTargetAt() skips the
        // card being dragged, and that card sits under the cursor at release time.
        QString group;
        QString before;
        const bool hasTarget = dropTargetAt(scenePos, &group, &before);
        const QString sourceContainer = dragContainerKey_;

        dragMarkupId_.clear();
        dragGroupKey_.clear();
        dragMoved_ = false;
        nodePressActive_ = false;
        pressKey_.clear();
        dragContainerKey_.clear();

        if (!hasTarget) {
            emit nodeReparented(id, QString());   // empty canvas -> back to the page bucket
            return;
        }
            if (!before.isEmpty()) {
            emit noteReorderRequested(id, group, before);
            return;
        }
        if (!group.isEmpty()) {
            if (group.startsWith(QStringLiteral("page:")) || group.startsWith(QStringLiteral("docpage:")) ||
                group.startsWith(QStringLiteral("document:")))
                emit containerReparentRequested(id, dragContainerPath_, group, QString());
            else emit nodeReparented(id, group);
            return;
        }
        if (!sourceContainer.isEmpty()) emit containerReparentRequested(id, dragContainerPath_, sourceContainer, dragContainerPath_);
        else emit nodeReparented(id, QString());   // dropped on empty canvas -> page bucket
        return;
    }
    dragMarkupId_.clear();
    dragMoved_ = false;
    QGraphicsView::mouseReleaseEvent(event);
}

void MindMapView::keyPressEvent(QKeyEvent* event)
{
    // If the inline editor somehow does not receive the key (focus can move while the
    // scene is rebuilt), never let Enter fall through to "create sibling".
    if (renameEditor_) {
        if (event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter ||
            event->key() == Qt::Key_Escape) {
            const bool commit = event->key() != Qt::Key_Escape;
            event->accept();
            // Queue the commit: deleting the focused QGraphicsTextItem from inside a key
            // event that may still be travelling through the scene corrupts the heap.
            QMetaObject::invokeMethod(
                this, [this, commit]() { commitInlineRename(commit); }, Qt::QueuedConnection);
            return;
        }
        // Do not let map shortcuts (space/Tab/Delete...) rebuild the scene while renaming.
        event->accept();
        return;
    }
    if ((event->modifiers() & Qt::ControlModifier) && event->key() == Qt::Key_A) {
        selectAllNotes();
        event->accept();
        return;
    }
    if (event->key() == Qt::Key_Escape) {
        clearSelectionKeys();
        event->accept();
        return;
    }
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
            // Groups get the safe rename dialog; only note cards use the inline editor.
            if (selectedKey_.startsWith(QStringLiteral("group:")))
                emit renameRequested(selectedKey_);
            else
                beginInlineRename(selectedKey_);
            event->accept();
            return;
        case Qt::Key_Delete:
        case Qt::Key_Backspace: {
            const QStringList batch = selectedNoteIds();
            if (batch.size() > 1) emit deleteManyRequested(batch);
            else emit deleteRequested(selectedKey_);
            event->accept();
            return;
        }
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
    if (event->button() == Qt::RightButton) {
        QGraphicsView::mousePressEvent(event);
        return; // preserve QContextMenuEvent delivery on release
    }
    if (event->button() == Qt::MiddleButton) {
        panActive_ = true;
        panStart_ = event->pos();
        panLast_ = event->pos();
        panStartH_ = horizontalScrollBar()->value();
        panStartV_ = verticalScrollBar()->value();
        setCursor(Qt::ClosedHandCursor);
        event->accept();
        return;
    }
    if (event->button() != Qt::LeftButton) { QGraphicsView::mousePressEvent(event); return; }
    if (renameEditor_) {
        const QPointF scenePos = mapToScene(event->pos());
        // Clicks inside the editor must reach the text item so the caret can be placed;
        // clicks outside commit the rename and then proceed as a normal card click.
        if (renameEditor_->sceneBoundingRect().contains(scenePos) ||
            (renameBackdrop_ && renameBackdrop_->sceneBoundingRect().contains(scenePos))) {
            QGraphicsView::mousePressEvent(event);
            return;
        }
        commitInlineRename(true);
    }
    if (scene_) {
        const QPointF scenePos = mapToScene(event->pos());

        if (linkMode_) {
            const QString key = keyAt(scenePos);
            if (key.isEmpty()) {
                pendingLinkFrom_.clear();
                setSelectedKey(QString());
            } else if (pendingLinkFrom_.isEmpty()) {
                pendingLinkFrom_ = key;
                setSelectedKey(key);
            } else if (pendingLinkFrom_ != key) {
                const QString from = pendingLinkFrom_;
                pendingLinkFrom_.clear();
                emit linkRequested(from, key);
            }
            event->accept();
            return;
        }

        const QString collapseKey = collapseControlAt(scenePos);
        if (!collapseKey.isEmpty()) {
            setSelectedKey(collapseKey);
            setCollapsed(collapseKey, !isCollapsed(collapseKey));
            event->accept();
            return;
        }
        const QString key = keyAt(scenePos);
        const bool ctrl = (event->modifiers() & (Qt::ControlModifier | Qt::MetaModifier)) != 0;
        const bool shift = (event->modifiers() & Qt::ShiftModifier) != 0;
        if (key.isEmpty() && shift && scene_) {
            // Shift+drag draws a selection band; plain empty drag still pans.
            bandActive_ = true;
            bandAdditive_ = true;
            bandBase_ = selectedKeys_;
            bandStart_ = scenePos;
            bandItem_ = scene_->addRect(QRectF(scenePos, scenePos),
                                        QPen(QColor("#0a84ff"), 1, Qt::DashLine),
                                        QBrush(QColor(10, 132, 255, 30)));
            bandItem_->setZValue(90);
            setCursor(Qt::CrossCursor);
            event->accept();
            return;
        }
        if (!key.isEmpty()) {
            if (ctrl) {
                toggleSelectedKey(key);
                event->accept();
                return;
            }
            if (shift && !isKeySelected(key)) {
                selectedKeys_.append(key);
                selectedKey_ = key;
                syncSelectionBorders();
                event->accept();
                return;
            }
            if (!isKeySelected(key)) setSelectedKey(key);
            pressKey_ = key;
            pressStart_ = event->pos();
            nodePressActive_ = true;
            dragGroupKey_ = key.startsWith(QStringLiteral("group:")) ? key : QString();
            dragMarkupId_ = key.startsWith(QStringLiteral("note:")) ? key.mid(5) : markupIdAt(scenePos);
            dragContainerKey_ = (key.startsWith(QStringLiteral("page:")) ||
                                 key.startsWith(QStringLiteral("docpage:")) ||
                                 key.startsWith(QStringLiteral("document:"))) ? key : QString();
            const Node* sourceNode = nullptr;
            std::function<const Node*(const Node&)> findNode = [&](const Node& node) -> const Node* {
                if (nodeKey(node) == key) return &node;
                for (const Node& child : node.children) if (const Node* found = findNode(child)) return found;
                return nullptr;
            };
            sourceNode = findNode(root_);
            dragContainerPath_ = sourceNode ? sourceNode->docPath : root_.docPath;
            dragStartScene_ = scenePos;
            dragMoved_ = false;
            event->accept();
            return;
        }
        setSelectedKey(QString());
        dragGroupKey_.clear();
        if (event->button() == Qt::LeftButton) {
            pressKey_.clear();
            pressStart_ = event->pos();
            nodePressActive_ = true;
            event->accept();
            return;
        }
    }
    QGraphicsView::mousePressEvent(event);
}

void MindMapView::dragEnterEvent(QDragEnterEvent* event)
{
    if (event->mimeData()->hasFormat(QStringLiteral("application/x-adaptnote-excerpt"))) {
        externalDropActive_ = true;
        event->acceptProposedAction();
        return;
    }
    QGraphicsView::dragEnterEvent(event);
}

void MindMapView::dragMoveEvent(QDragMoveEvent* event)
{
    if (!event->mimeData()->hasFormat(QStringLiteral("application/x-adaptnote-excerpt"))) {
        QGraphicsView::dragMoveEvent(event);
        return;
    }
    externalDropActive_ = true;
    updateDropIndicator(mapToScene(event->position().toPoint()));
    event->acceptProposedAction();
}

void MindMapView::dragLeaveEvent(QDragLeaveEvent* event)
{
    externalDropActive_ = false;
    clearDropIndicator();
    QGraphicsView::dragLeaveEvent(event);
}

void MindMapView::dropEvent(QDropEvent* event)
{
    if (!event->mimeData()->hasFormat(QStringLiteral("application/x-adaptnote-excerpt"))) {
        QGraphicsView::dropEvent(event);
        return;
    }
    const QPointF scenePos = mapToScene(event->position().toPoint());
    QString group;
    QString before;
    dropTargetAt(scenePos, &group, &before);
    externalDropActive_ = false;
    clearDropIndicator();
    const QByteArray payload =
        event->mimeData()->data(QStringLiteral("application/x-adaptnote-excerpt"));
    event->acceptProposedAction();
    emit excerptDropped(payload, group);
}

void MindMapView::contextMenuEvent(QContextMenuEvent* event)
{
    if (scene_) {
        const QPointF scenePos = mapToScene(event->pos());
        const QString key = keyAt(scenePos);
        if (key.startsWith(QStringLiteral("note:"))) {
            if (selectedNoteIds().size() > 1 && isKeySelected(key)) {
                emit selectionMenuRequested(selectedKeys(), event->globalPos());
            } else {
                emit noteMenuRequested(key.mid(5), event->globalPos());
            }
            event->accept();
            return;
        }
        for (QGraphicsItem* item : scene_->items(scenePos)) {
            const QVariant id = item->data(5);
            if (id.isValid() && !id.toString().isEmpty()) {
                emit linkMenuRequested(id.toString(), event->globalPos());
                event->accept();
                return;
            }
        }
        if ((key.startsWith(QStringLiteral("page:")) || key.startsWith(QStringLiteral("docpage:"))) &&
            !rectForKey(key).isNull()) {
            QString path = root_.docPath;
            const QString prefix = QStringLiteral("docpage:");
            if (key.startsWith(prefix)) {
                const QString payload = key.mid(prefix.size());
                const int split = payload.lastIndexOf(QStringLiteral(":page:"));
                if (split >= 0) path = payload.left(split);
            }
            emit pageMenuRequested(key, path, event->globalPos());
            event->accept();
            return;
        }
    }
    handleCanvasContextMenu(event);
}

void MindMapView::handleCanvasContextMenu(QContextMenuEvent* event)
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
    // Show the whole map whenever it is still legible; only stop shrinking at 60%
    // (titles stay readable at that size and the user can Ctrl+wheel in).
    if (transform().m11() < 0.60) {
        resetTransform();
        scale(0.60, 0.60);
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
    const bool noteLeaf = !n.isGroup && !isRoot;
    m.titleH = qMin(2, wrapToLines(n.text, titleFm,
                                   width - 2 * kPadX - (noteLeaf ? 6 : 0), 2).size()) *
               titleFm.height();

    double body = 0;
    if (!n.excerpt.isEmpty() && !n.isGroup) {
        // The excerpt is a quote block: 2px rule on the left, muted text, max 3 lines.
        const QStringList lines = wrapToLines(n.excerpt, bodyFm,
                                              width - 2 * kPadX - 14, 3);
        m.excerptLines = lines.size();
        m.excerptH = lines.size() * bodyFm.height() + 8;
        body += m.excerptH + 6;
    }
    if (!n.comment.isEmpty()) {
        const QStringList lines = wrapToLines(n.comment, bodyFm,
                                              width - 2 * kPadX - (noteLeaf ? 6 : 0), 3);
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

void MindMapView::collapseAllBranches()
{
    collapsed_.clear();
    std::function<void(const Node&, bool)> walk = [&](const Node& node, bool isRoot) {
        // Root stays open, otherwise "collapse all" would leave a single card and no
        // way to see which branches exist.
        if (!isRoot && !node.children.isEmpty()) collapsed_.insert(nodeKey(node));
        for (const Node& child : node.children) walk(child, false);
    };
    walk(root_, true);
    rebuild();
    fitToContent();
    emit nodeSelected(selectedKey_);
}

void MindMapView::expandAllBranches()
{
    if (collapsed_.isEmpty()) return;
    collapsed_.clear();
    rebuild();
    emit nodeSelected(selectedKey_);
}

void MindMapView::setSelectedKey(const QString& key)
{
    if (selectedKeys_.size() == 1 && selectedKeys_.first() == key) return;
    const QString oldPrimary = selectedKey_;
    selectedKeys_.clear();
    if (!key.isEmpty()) selectedKeys_.append(key);
    selectedKey_ = key;
    syncSelectionBorders();
    const QRectF oldRect = rectForKey(oldPrimary);
    const QRectF newRect = rectForKey(key);
    if (!oldRect.isNull()) viewport()->update(mapFromScene(oldRect).boundingRect());
    if (!newRect.isNull()) viewport()->update(mapFromScene(newRect).boundingRect());
}

void MindMapView::toggleSelectedKey(const QString& key)
{
    if (key.isEmpty()) return;
    const int index = selectedKeys_.indexOf(key);
    if (index >= 0) {
        selectedKeys_.removeAt(index);
        selectedKey_ = selectedKeys_.isEmpty() ? QString() : selectedKeys_.last();
    } else {
        selectedKeys_.append(key);
        selectedKey_ = key;
    }
    syncSelectionBorders();
    emit nodeSelected(selectedKey_);
}

void MindMapView::clearSelectionKeys()
{
    if (selectedKeys_.isEmpty()) return;
    selectedKeys_.clear();
    selectedKey_.clear();
    syncSelectionBorders();
}

QStringList MindMapView::selectedNoteIds() const
{
    QStringList ids;
    for (const QString& key : selectedKeys_)
        if (key.startsWith(QStringLiteral("note:"))) ids.append(key.mid(5));
    ids.removeDuplicates();
    return ids;
}

QString MindMapView::groupKeyOfNote(const QString& markupId) const
{
    QString result;
    bool found = false;
    std::function<void(const Node&)> walk = [&](const Node& node) {
        if (found) return;
        if (node.markupId == markupId) {
            result = node.groupKey;
            found = true;
            return;
        }
        for (const Node& child : node.children) walk(child);
    };
    walk(root_);
    return result;
}

QString MindMapView::collapseControlAt(const QPointF& scenePos) const
{
    if (!scene_) return {};
    for (QGraphicsItem* item : scene_->items(scenePos)) {
        if (!item->data(8).toBool()) continue;
        const QString key = item->data(4).toString();
        if (!key.isEmpty()) return key;
    }
    return {};
}

void MindMapView::syncSelectionBorders()
{
    if (!scene_) return;
    for (QGraphicsItem* item : scene_->items()) {
        const QString key = item->data(4).toString();
        if (key.isEmpty()) continue;
        auto* card = dynamic_cast<QAbstractGraphicsShapeItem*>(item);
        // data(7) marks the card background; text/decoration items are separate.
        if (!card || !card->data(7).toBool()) continue;
        const bool selected = selectedKeys_.contains(key);
        const bool rootCard = key.startsWith(QStringLiteral("root:"));
        const bool group = key.startsWith(QStringLiteral("group:"));
        QPen pen = card->pen();
        pen.setColor(selected ? QColor(0x2563eb)
                              : rootCard ? QColor(0x1d4ed8)
                              : group ? QColor(0xd3e2f8)
                                      : QColor(0xe6e9ef));
        pen.setWidthF(selected ? 2.0 : 1.0);
        card->setPen(pen);
    }
}

void MindMapView::selectAllNotes()
{
    selectedKeys_.clear();
    std::function<void(const Node&)> walk = [&](const Node& node) {
        if (!node.markupId.isEmpty()) selectedKeys_.append(QStringLiteral("note:") + node.markupId);
        for (const Node& child : node.children) walk(child);
    };
    walk(root_);
    selectedKey_ = selectedKeys_.isEmpty() ? QString() : selectedKeys_.last();
    syncSelectionBorders();
}

void MindMapView::setLinks(const QVector<MindLink>& links)
{
    links_ = links;
    rebuild();
}

void MindMapView::setLinkMode(bool on)
{
    if (linkMode_ == on) return;
    linkMode_ = on;
    pendingLinkFrom_.clear();
    setCursor(on ? Qt::CrossCursor : Qt::ArrowCursor);
    rebuild();   // shows/hides the "pick a card" hint
}

namespace {

QPointF linkAnchor(const QRectF& from, const QRectF& to)
{
    // start on the side facing the target, so arrows do not cut through the cards
    const QPointF a = from.center();
    const QPointF b = to.center();
    if (b.x() >= a.x()) return QPointF(from.right(), a.y());
    return QPointF(from.left(), a.y());
}

}  // namespace

void MindMapView::drawFreeLinks(const QHash<QString, QRectF>& rectOf)
{
    linkItems_.clear();
    for (const MindLink& l : links_) {
        const QRectF from = rectOf.value(l.fromNode);
        const QRectF to = rectOf.value(l.toNode);
        if (from.isNull() || to.isNull()) continue;   // one end is collapsed or gone

        const QPointF a = linkAnchor(from, to);
        const QPointF b = linkAnchor(to, from);
        const double dx = qMax(40.0, qAbs(b.x() - a.x()) / 2.0);
        QPainterPath path;
        path.moveTo(a);
        path.cubicTo(a + QPointF(dx, 0), b - QPointF(dx, 0), b);

        QPen pen(QColor(0x7a, 0x5a, 0xf8), 1.8, Qt::DashLine);
        auto* item = scene_->addPath(path, pen);
        item->setZValue(4);
        item->setToolTip(l.label.isEmpty() ? QStringLiteral("关联线") : l.label);
        item->setData(5, l.id);   // link id for hit testing
        if (l.arrow == QStringLiteral("end") || l.arrow == QStringLiteral("both")) {
            // simple arrow head pointing into the target card
            const QPointF back = b - QPointF(dx > 0 ? 10 : -10, 0);
            QPainterPath head;
            head.moveTo(b);
            head.lineTo(back + QPointF(0, -5));
            head.lineTo(back + QPointF(0, 5));
            head.closeSubpath();
            auto* arrow = scene_->addPath(head, QPen(Qt::NoPen), QBrush(QColor(0x7a, 0x5a, 0xf8)));
            arrow->setZValue(4);
            arrow->setData(5, l.id);
        }
        linkItems_.insert(l.id, item);
    }
}

void MindMapView::rebuild()
{
    // A rebuild clears every scene item, including a live inline editor. Drop the editor
    // pointers first so a queued commit cannot dereference a deleted QGraphicsTextItem.
    if (renameEditor_) {
        // scene_->clear() below owns and deletes these items; just forget the pointers so
        // a queued commit cannot dereference them after the clear.
        renameEditor_ = nullptr;
        renameBackdrop_ = nullptr;
        renameKey_.clear();
    }
    dropIndicator_ = nullptr;   // destroyed by clear() below
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
        const double width = depth == 0 ? 178.0 : 224.0;
        const CardMetrics m = measureCard(n, width, depth == 0);
        byKey.insert(key, &n);
        heights.insert(key, m.height);
        order.append(&n);
        if (collapsed_.contains(key)) return;
        for (const Node& c : n.children) measure(c, depth + 1);
    };
    measure(root_, 0);

    LayoutOptions opts;
    opts.nodeWidth = 224.0;
    opts.columnGap = 38.0;
    opts.rowGap = 14.0;
    opts.padding = 16.0;

    // The root card is narrower; the layout uses one column width, so keep the wider value
    // for placement and let the root card sit left-aligned inside it.
    QSizeF canvas;
    const QVector<PlacedNode> placed =
        layoutTree(root_, heights, 64.0, opts, collapsed_, &canvas);

    QHash<QString, QRectF> rectOf;
    pageByKey_.clear();
    for (const Node* node : order) pageByKey_.insert(nodeKey(*node), node->page);
    for (const PlacedNode& p : placed) rectOf.insert(p.key, p.rect);

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
    drawFreeLinks(rectOf);

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

        const bool selected = isKeySelected(key);
        const bool isRoot = depth == 0;
        const bool isGroup = n.isGroup && !isRoot;
        const bool isBucket = key.startsWith(QStringLiteral("page:")) ||
                              key.startsWith(QStringLiteral("docpage:")) ||
                              key.startsWith(QStringLiteral("document:"));
        const bool isNote = !isRoot && !isGroup && !isBucket && !n.markupId.isEmpty();
        const QColor noteAccent = n.accent.isValid() ? n.accent : QColor(0x3b82f6);

        // Rounded card with a soft two-layer shadow. QGraphicsDropShadowEffect would be
        // smoother but costs a render pass per card; on a 100-card map the flat + offset
        // paths are indistinguishable and much cheaper.
        const qreal radius = isRoot ? 14.0 : 12.0;
        QPainterPath shape;
        shape.addRoundedRect(rect, radius, radius);
        QPainterPath shadow = shape;
        shadow.translate(0, 3);
        auto* shade = scene_->addPath(shadow, QPen(Qt::NoPen), QBrush(QColor(15, 23, 42, 16)));
        shade->setZValue(1.70);
        QPainterPath tight = shape;
        tight.translate(0, 1);
        auto* shade2 = scene_->addPath(tight, QPen(Qt::NoPen), QBrush(QColor(15, 23, 42, 10)));
        shade2->setZValue(1.75);

        QColor fill = QColor(0xff, 0xff, 0xff);
        QColor stroke = QColor(0xe6, 0xe9, 0xef);
        if (isGroup) { fill = QColor(0xef, 0xf5, 0xff); stroke = QColor(0xd3, 0xe2, 0xf8); }
        if (isBucket) { fill = QColor(0xfa, 0xfb, 0xfd); stroke = QColor(0xe5, 0xe9, 0xf0); }
        QBrush fillBrush(fill);
        if (isRoot) {
            QLinearGradient grad(rect.topLeft(), rect.bottomLeft());
            grad.setColorAt(0.0, QColor(0x2f, 0x77, 0xf0));
            grad.setColorAt(1.0, QColor(0x1d, 0x4e, 0xd8));
            fillBrush = QBrush(grad);
            stroke = QColor(0x1d, 0x4e, 0xd8);
        }
        if (selected) {
            QPainterPath halo;
            halo.addRoundedRect(rect.adjusted(-3, -3, 3, 3), radius + 3, radius + 3);
            auto* glow = scene_->addPath(halo, QPen(Qt::NoPen),
                                         QBrush(QColor(0x25, 0x63, 0xeb, 28)));
            glow->setZValue(1.65);
            stroke = QColor(0x25, 0x63, 0xeb);
        }

        auto* card = scene_->addPath(shape, QPen(stroke, selected ? 2.0 : 1.0), fillBrush);
        card->setData(7, 1); // identifies the card background, distinct from its text children
        card->setZValue(2);
        card->setData(0, n.page);
        card->setData(2, n.groupKey);
        card->setData(4, key);   // nodeKey(): hit target for select/rename/keys
        if (!n.markupId.isEmpty()) card->setData(1, n.markupId);
        if (!n.docPath.isEmpty()) card->setData(6, n.docPath);
        if (selected) card->setData(3, 1);   // hit-test helper: selected marker

        const double textLeft = rect.left() + kPadX + (isNote ? 6 : 0);
        double y = rect.top() + kPadY;
        const QColor titleColor = isRoot ? QColor(Qt::white)
                                : isGroup ? QColor(0x1e, 0x40, 0x7a)
                                          : QColor(0x11, 0x18, 0x27);
        const QColor bodyColor = isRoot ? QColor(0xdc, 0xe9, 0xff) : QColor(0x5b, 0x66, 0x78);
        const QColor accent = isRoot ? QColor(0xd7, 0xe5, 0xff) : QColor(0x2f, 0x6b, 0xd8);

        if (isNote) {
            // Coloured spine: the same colour the note uses in the page/panel.
            QPainterPath spine;
            spine.addRoundedRect(QRectF(rect.left() + 7, rect.top() + 11, 3,
                                        qMax(10.0, rect.height() - 22)), 1.5, 1.5);
            auto* bar = scene_->addPath(spine, QPen(Qt::NoPen), QBrush(noteAccent));
            bar->setZValue(2.4);
            bar->setData(4, key);
        }

        for (const QString& line : wrapToLines(n.text, titleFm,
                                              rect.width() - 2 * kPadX - (isNote ? 6 : 0), 2)) {
            auto* t = scene_->addSimpleText(line, titleFont);
            t->setBrush(QBrush(titleColor));
            t->setPos(textLeft, y);
            t->setZValue(3);
            if (!n.markupId.isEmpty()) t->setData(1, n.markupId);
            t->setData(4, key);
            y += titleFm.height();
        }

        if (!n.excerpt.isEmpty() && !n.isGroup) {
            y += 3;
            const double textW = rect.width() - 2 * kPadX - (isNote ? 14 : 8);
            const QStringList lines = wrapToLines(n.excerpt, bodyFm, textW, 3);
            const double blockH = lines.size() * bodyFm.height() + 8;
            auto* rule = scene_->addRect(QRectF(textLeft - 5, y + 2, 2, blockH - 4),
                                         QPen(Qt::NoPen),
                                         QBrush(QColor(noteAccent.red(), noteAccent.green(),
                                                       noteAccent.blue(), 110)));
            rule->setZValue(3);
            double ly = y + 4;
            for (const QString& line : lines) {
                auto* t = scene_->addSimpleText(line, bodyFont);
                t->setBrush(QBrush(bodyColor));
                t->setPos(textLeft + 7, ly);
                t->setZValue(3);
                t->setData(4, key);
                ly += bodyFm.height();
            }
            y += blockH + 3;
        }

        if (!n.comment.isEmpty()) {
            for (const QString& line : wrapToLines(
                     n.comment, bodyFm, rect.width() - 2 * kPadX - (isNote ? 6 : 0), 3)) {
                auto* t = scene_->addSimpleText(line, bodyFont);
                t->setBrush(QBrush(depth == 0 ? bodyColor : QColor(0x1d, 0x21, 0x29)));
                t->setPos(textLeft, y);
                t->setZValue(3);
                t->setData(4, key);
                y += bodyFm.height();
            }
            y += 2;
        } else if (n.isGroup && !n.children.isEmpty()) {
            auto* t = scene_->addSimpleText(
                QStringLiteral("%1 个子节点").arg(n.children.size()), bodyFont);
            t->setBrush(QBrush(bodyColor));
            t->setPos(textLeft, y);
            t->setZValue(3);
            y += bodyFm.height() + 2;
        }

        // footer: source on the left, affordance on the right. Page buckets are containers,
        // not quotations, so they do not claim to be "原文".
        if (!isBucket && (!n.source.isEmpty() || n.page >= 0)) {
            auto* foot = scene_->addSimpleText(n.source.isEmpty() ? QStringLiteral("原文")
                                                                  : n.source,
                                               smallFont);
            foot->setBrush(QBrush(accent));
            foot->setPos(textLeft, rect.bottom() - kPadY - foot->boundingRect().height());
            foot->setZValue(3);
        }
        if (!n.children.isEmpty()) {
            // Clickable fold control: "−" expanded, "+" collapsed. The child count sits
            // just left of it so a folded branch still says how much is hidden.
            const bool folded = collapsed_.contains(key);
            auto* count = scene_->addSimpleText(QString::number(n.children.size()), smallFont);
            count->setBrush(QBrush(accent));
            count->setZValue(4);
            count->setData(8, 1);
            count->setData(4, key);

            const qreal d = 18;
            const QRectF circle(rect.right() - kPadX - d, rect.bottom() - kPadY - d, d, d);
            auto* bg = scene_->addEllipse(
                circle, QPen(isRoot ? QColor(255, 255, 255, 120) : QColor(0xd7, 0xdf, 0xea), 1.0),
                QBrush(isRoot ? QColor(255, 255, 255, 36) : QColor(0xff, 0xff, 0xff)));
            bg->setZValue(4);
            bg->setData(8, 1);
            bg->setData(4, key);

            auto* glyph = scene_->addSimpleText(folded ? QStringLiteral("+") : QStringLiteral("−"),
                                                smallFont);
            glyph->setBrush(QBrush(isRoot ? QColor(Qt::white) : QColor(0x25, 0x63, 0xeb)));
            glyph->setPos(circle.center() - QPointF(glyph->boundingRect().width() / 2.0,
                                                    glyph->boundingRect().height() / 2.0));
            glyph->setZValue(5);
            glyph->setData(8, 1);
            glyph->setData(4, key);

            count->setPos(circle.left() - 4 - count->boundingRect().width(), circle.top() + 1);
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
