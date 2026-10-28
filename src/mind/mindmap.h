#pragma once

#include "mind/tree.h"

#include <QGraphicsItem>
#include <QGraphicsView>
#include <QSet>
#include <QImage>
#include <QString>
#include <QVector>

namespace an {
namespace mind {

// Read-only mind map view: tree layout (logic-right), pan + zoom, export to image.
class MindMapView : public QGraphicsView {
    Q_OBJECT
public:
    explicit MindMapView(QWidget* parent = nullptr);

    void setTree(const Node& root);
    const Node& tree() const { return root_; }
    int sceneNodeCount() const;
    void setZoom(double factor);
    double zoom() const { return zoom_; }
    void fitToContent();

    QImage renderToImage(const QSize& size);

    // Collapse/expand a group (or a page bucket) by nodeKey(); the selection is the card
    // that gets the accent outline.
    void setCollapsed(const QString& key, bool collapsed);
    bool isCollapsed(const QString& key) const { return collapsed_.contains(key); }
    void setSelectedKey(const QString& key);
    QString selectedKey() const { return selectedKey_; }
    bool hasDropIndicator() const { return dropIndicator_ != nullptr; }

    // Hit testing helpers (also used by tests).
    QString groupKeyAt(const QPointF& scenePos) const;
    QString markupIdAt(const QPointF& scenePos) const;
    // Scene position of a note node (invalid point when the id is unknown).
    QPointF nodeCenter(const QString& markupId) const;
    // Resolves a drop position: the group it belongs to and the note it should be
    // inserted in front of. Returns false when the position carries no target.
    bool dropTargetAt(const QPointF& scenePos, QString* groupKey, QString* beforeId) const;
    // Renders the whole map to a PNG file (used by the dialog's export button).
    bool exportPng(const QString& path, const QSize& size = QSize(1600, 1000));

    // Selects the card belonging to a note and expands whatever hides it. Used for the
    // reverse direction (annotation -> mind map). Returns false when the note has no card.
    bool selectForMarkup(const QString& markupId);
    QString keyAt(const QPointF& scenePos) const;
    QRectF rectForKey(const QString& key) const;

signals:
    void nodeActivated(int page);
    // Keyboard / double-click editing requests; the window owns the dialogs and the undo
    // stack, so the view only reports intent.
    void renameRequested(const QString& key);
    void deleteRequested(const QString& key);
    void childRequested(const QString& key);
    void siblingRequested(const QString& key);   // emitted when a node with a page is clicked
    // A note was dragged onto a group (or onto another note's group) -> regroup it.
    // An empty groupKey means "ungroup" (dropped on empty canvas).
    void nodeReparented(const QString& markupId, const QString& groupKey);
    // Right click on a group node: the key is the same "group:<id>" string.
    void groupMenuRequested(const QString& groupKey, const QPoint& globalPos);
    // Dropped on/near a note: insert the dragged one in front of `beforeMarkupId`
    // (empty = append at the end of that group).
    void noteReorderRequested(const QString& markupId, const QString& groupKey,
                              const QString& beforeMarkupId);

protected:
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void rebuild();

    Node root_;
    double zoom_ = 1.0;
    QString dragMarkupId_;
    QPointF dragStartScene_;
    bool dragMoved_ = false;
    bool didInitialFit_ = false;
    // Drop feedback: a line where the note will be inserted, or a box around the group it
    // will join. Recreated on every move, removed on release.
    QGraphicsItem* dropIndicator_ = nullptr;
    void clearDropIndicator();
    void updateDropIndicator(const QPointF& scenePos);
    QGraphicsScene* scene_ = nullptr;
    int nodeItems_ = 0;
    QSet<QString> collapsed_;   // nodeKey() values whose subtree is hidden
    QString selectedKey_;       // nodeKey() of the selected card (empty = none)
};

}  // namespace mind
}  // namespace an
