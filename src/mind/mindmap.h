#pragma once

#include "mind/tree.h"

#include <QGraphicsItem>
#include <QGraphicsView>
#include <QSet>
#include <QImage>
#include <QString>
#include <QVector>

class QGraphicsTextItem;

namespace an {
namespace mind {

// Read-only mind map view: tree layout (logic-right), pan + zoom, export to image.
class MindMapView : public QGraphicsView {
    Q_OBJECT
public:
    explicit MindMapView(QWidget* parent = nullptr);

    void setTree(const Node& root);
    // Free-form links between cards. Keys are nodeKey() values, so they survive rebuilds.
    void setLinks(const QVector<MindLink>& links);
    QVector<MindLink> links() const { return links_; }
    // Link mode: the first click picks the source card, the second emits linkRequested.
    void setLinkMode(bool on);
    bool linkMode() const { return linkMode_; }
    const Node& tree() const { return root_; }
    int sceneNodeCount() const;
    void setZoom(double factor);
    double zoom() const { return zoom_; }
    void fitToContent();

    QImage renderToImage(const QSize& size);

    // In-place card rename (double-click / F2). commitInlineRename is invoked through the
    // event loop by the editor item, so callers never see a deleted item mid-event.
    void beginInlineRename(const QString& key);
    void commitInlineRename(bool commit);
    // Test seams for the in-place card editor.
    bool inlineRenameActive() const { return renameEditor_ != nullptr; }
    QString inlineRenameKeyForTest() const { return renameKey_; }
    void setInlineRenameTextForTest(const QString& text);

    // Collapse/expand a group (or a page bucket) by nodeKey(); the selection is the card
    // that gets the accent outline.
    void setCollapsed(const QString& key, bool collapsed);
    bool isCollapsed(const QString& key) const { return collapsed_.contains(key); }
    // Bulk fold: keep the root level visible, or show everything.
    void collapseAllBranches();
    void expandAllBranches();
    void setSelectedKey(const QString& key);
    QString selectedKey() const { return selectedKey_; }
    // Multi-selection (Ctrl+click toggles, Shift+drag draws a band, Ctrl+A selects notes).
    void toggleSelectedKey(const QString& key);
    void clearSelectionKeys();
    QStringList selectedKeys() const { return selectedKeys_; }
    bool isKeySelected(const QString& key) const { return selectedKeys_.contains(key); }
    void selectAllForTest() { selectAllNotes(); }
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
    void markupActivated(const QString& markupId);
    void nodeSelected(const QString& key);
    // Keyboard / double-click editing requests; the window owns the dialogs and the undo
    // stack, so the view only reports intent.
    void renameRequested(const QString& key);
    // In-place rename finished (Enter or click away). The window applies it to the model.
    void renameCommitted(const QString& key, const QString& text);
    // Batch operations started from the selection (notes only; groups keep single actions).
    void deleteManyRequested(const QStringList& markupIds);
    void selectionMenuRequested(const QStringList& keys, const QPoint& globalPos);
    void selectionReparentRequested(const QStringList& markupIds, const QString& groupKey);
    // Batch reorder inside one group/page bucket: insert `markupIds` before `beforeId`
    // (empty = append), preserving their current relative order.
    void selectionReorderRequested(const QStringList& markupIds, const QString& groupKey,
                                   const QString& beforeId);
    // An excerpt dragged out of the document was dropped on a group/page/empty canvas.
    void excerptDropped(const QByteArray& payload, const QString& groupKey);
    void deleteRequested(const QString& key);
    void childRequested(const QString& key);
    void siblingRequested(const QString& key);
    // Two cards picked in link mode.
    void linkRequested(const QString& fromKey, const QString& toKey);
    // Right click on an existing link (its id), or on a card while not in link mode.
    void linkMenuRequested(const QString& linkId, const QPoint& globalPos);
    void noteMenuRequested(const QString& markupId, const QPoint& globalPos);
    // A note was dragged onto a group (or onto another note's group) -> regroup it.
    // An empty groupKey means "ungroup" (dropped on empty canvas).
    void nodeReparented(const QString& markupId, const QString& groupKey);
    void containerReparentRequested(const QString& sourceKey, const QString& sourcePath,
                                    const QString& targetKey, const QString& targetPath);
    // Right click on a group node: the key is the same "group:<id>" string.
    void groupMenuRequested(const QString& groupKey, const QPoint& globalPos);
    void pageMenuRequested(const QString& pageKey, const QString& docPath, const QPoint& globalPos);
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
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void handleCanvasContextMenu(QContextMenuEvent* event);
    void keyPressEvent(QKeyEvent* event) override;

private:
    void rebuild();
    QString nodeText(const QString& key) const;
    QStringList selectedNoteIds() const;
    QString groupKeyOfNote(const QString& markupId) const;
    QString collapseControlAt(const QPointF& scenePos) const;
    void syncSelectionBorders();
    void selectAllNotes();

    QVector<QString> selectedKeys_;
    QGraphicsRectItem* bandItem_ = nullptr;
    bool bandActive_ = false;
    bool bandAdditive_ = false;
    QPointF bandStart_;
    QVector<QString> bandBase_;
    // External (document -> map) drag in progress; updateDropIndicator uses it to draw
    // feedback for a drag that has no internal dragMarkupId_.
    bool externalDropActive_ = false;

    QGraphicsTextItem* renameEditor_ = nullptr;
    QGraphicsItem* renameBackdrop_ = nullptr;
    QString renameKey_;

    Node root_;
    double zoom_ = 1.0;
    QString dragMarkupId_;
    QString dragContainerKey_;
    QString dragContainerPath_;
    QString pressKey_;
    QPoint pressStart_;
    QPoint panStart_;
    QPoint panLast_;
    int panStartH_ = 0;
    int panStartV_ = 0;
    bool nodePressActive_ = false;
    bool panActive_ = false;
    QPointF dragStartScene_;
    bool dragMoved_ = false;
    bool didInitialFit_ = false;
    // Drop feedback: a line where the note will be inserted, or a box around the group it
    // will join. Recreated on every move, removed on release.
    QGraphicsItem* dropIndicator_ = nullptr;
    QVector<MindLink> links_;
    bool linkMode_ = false;
    QString pendingLinkFrom_;
    // drawn link items, keyed by link id, for hit testing and highlight
    QHash<QString, QGraphicsItem*> linkItems_;
    QHash<QString, int> pageByKey_;
    void drawFreeLinks(const QHash<QString, QRectF>& rectOf);
    void clearDropIndicator();
    void updateDropIndicator(const QPointF& scenePos);
    QGraphicsScene* scene_ = nullptr;
    int nodeItems_ = 0;
    QSet<QString> collapsed_;   // nodeKey() values whose subtree is hidden
    QString selectedKey_;       // nodeKey() of the selected card (empty = none)
};

}  // namespace mind
}  // namespace an
