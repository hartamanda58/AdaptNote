#include "ui/mainwindow.h"

#include "core/paths.h"
#include "sync/vv.h"
#include "core/textselect.h"
#include "import/import.h"

#include <QCryptographicHash>
#include "migrate/migrate.h"
#include "export/exporter.h"
#include "ink/ink.h"
#include "mind/mindmap.h"
#include "mind/layout.h"
#include "read/reader.h"
#include "ui/conflictdialog.h"
#include "ui/libraryview.h"
#include "ui/pageview.h"
#include "ui/vocabpanel.h"
#include "ocr/ocr.h"

#include <QAction>
#include <QActionGroup>
#include <QApplication>
#include <QClipboard>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QJsonArray>
#include <QJsonDocument>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QFrame>
#include <QGraphicsDropShadowEffect>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImage>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QListView>
#include <QIcon>
#include <QMenu>
#include <QSplitter>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QRegularExpression>
#include <QScrollBar>
#include <QStandardPaths>
#include <QCheckBox>
#include <QInputDialog>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QTextBrowser>
#include <QStatusBar>
#include <QStyledItemDelegate>
#include <QTabWidget>
#include <QTabBar>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QDrag>
#include <QMimeData>
#include <QDropEvent>
#include <QDragMoveEvent>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

namespace an {
namespace {

class NotebookOutlineDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex& index) const override
    {
        const bool group = index.data(Qt::UserRole + 8).toBool();
        const QSize base = QStyledItemDelegate::sizeHint(option, index);
        return QSize(base.width(), group ? 34 : 50);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        painter->save();
        const QRect row = option.rect.adjusted(4, 1, -5, -1);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            QColor bg = selected ? QColor("#dfeaff") : QColor("#f2f4f7");
            painter->setPen(selected ? QColor("#b9d4ff") : Qt::NoPen);
            painter->setBrush(bg);
            painter->drawRoundedRect(row, 8, 8);
        }

        int left = option.rect.left() + 7;
        const QIcon icon = qvariant_cast<QIcon>(index.data(Qt::DecorationRole));
        if (!icon.isNull()) {
            const QRect iconRect(left, option.rect.top() + 9, 16, 16);
            icon.paint(painter, iconRect);
            left = iconRect.right() + 8;
        }

        const QString title = index.data(Qt::DisplayRole).toString();
        const QString preview = index.data(Qt::UserRole + 6).toString();
        const QString source = index.data(Qt::UserRole + 7).toString();
        const bool group = index.data(Qt::UserRole + 8).toBool();
        const QColor primary = selected ? QColor("#0a5bd3") : QColor("#1d1d1f");
        painter->setPen(primary);
        QFont titleFont = option.font;
        titleFont.setPointSizeF(group ? 10.5 : 10.0);
        titleFont.setWeight(group ? QFont::DemiBold : QFont::Medium);
        painter->setFont(titleFont);
        const QRect titleRect(left, option.rect.top() + (group ? 8 : 7),
                              qMax(20, option.rect.right() - left - 10), group ? 20 : 18);
        painter->drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(title, Qt::ElideRight, titleRect.width()));
        if (!group) {
            const int sourceWidth = source.isEmpty() ? 0 : 52;
            QRect previewRect(left, option.rect.top() + 27,
                              qMax(20, option.rect.right() - left - sourceWidth - 14), 16);
            QFont metaFont = option.font;
            metaFont.setPointSizeF(8.5);
            painter->setFont(metaFont);
            painter->setPen(QColor("#6b6b70"));
            const QString sub = preview.isEmpty() ? QStringLiteral("自由笔记") : preview;
            painter->drawText(previewRect, Qt::AlignLeft | Qt::AlignVCenter,
                              painter->fontMetrics().elidedText(sub, Qt::ElideRight, previewRect.width()));
            if (!source.isEmpty()) {
                QRect sourceRect(option.rect.right() - 62, option.rect.top() + 27, 54, 16);
                painter->setPen(QColor("#9a9aa0"));
                painter->drawText(sourceRect, Qt::AlignRight | Qt::AlignVCenter, source);
            }
        }
        painter->restore();
    }
};

// Never use QTreeWidget's internal move: SQLite and the undo stack own the structure.
class NotebookOutlineTree : public QTreeWidget {
public:
    using QTreeWidget::QTreeWidget;
    std::function<bool(const QString&, QTreeWidgetItem*, bool)> moveNote;
    std::function<bool(const QString&, QTreeWidgetItem*)> moveGroup;
protected:
    void startDrag(Qt::DropActions) override
    {
        if (!currentItem()) return;
        const QString note = currentItem()->data(0, Qt::UserRole).toString();
        const QString group = currentItem()->data(0, Qt::UserRole + 2).toString();
        const QString id = note.isEmpty() ? QStringLiteral("group:") + group : note;
        if (note.isEmpty() && group.isEmpty()) return;
        QDrag drag(this);
        auto* mime = new QMimeData;
        mime->setData("application/x-adaptnote-note", id.toUtf8());
        drag.setMimeData(mime);
        drag.exec(Qt::MoveAction);
    }
    void dragEnterEvent(QDragEnterEvent* event) override
    {
        if (event->source() == this && event->mimeData()->hasFormat("application/x-adaptnote-note"))
            event->acceptProposedAction();
        else event->ignore();
    }
    void dragLeaveEvent(QDragLeaveEvent* event) override
    {
        dropTarget_ = nullptr;
        viewport()->update();
        QTreeWidget::dragLeaveEvent(event);
    }
    void dragMoveEvent(QDragMoveEvent* event) override
    {
        auto* target = itemAt(event->position().toPoint());
        if (event->source() == this && target) {
            dropTarget_ = target;
            dropNest_ = nestFor(target, event->position().toPoint());
            viewport()->setToolTip(dropNest_
                ? QStringLiteral("放开：成为这条笔记的子笔记")
                : QStringLiteral("放开：插入到这条笔记之前"));
            viewport()->update();
            event->acceptProposedAction();
        } else {
            dropTarget_ = nullptr;
            viewport()->update();
            event->ignore();
        }
    }
    void dropEvent(QDropEvent* event) override
    {
        if (event->source() != this || !moveNote) { event->ignore(); return; }
        const QString id = QString::fromUtf8(event->mimeData()->data("application/x-adaptnote-note"));
        auto* target = itemAt(event->position().toPoint());
        const bool nestUnderParent = nestFor(target, event->position().toPoint());
        dropTarget_ = nullptr;
        viewport()->update();
        const bool moved = id.startsWith(QStringLiteral("group:"))
                               ? (moveGroup && moveGroup(id.mid(6), target))
                               : moveNote(id, target, nestUnderParent);
        if (moved) event->acceptProposedAction();
        else event->ignore();
    }

    void paintEvent(QPaintEvent* event) override
    {
        QTreeWidget::paintEvent(event);
        if (!dropTarget_) return;
        QPainter painter(viewport());
        painter.setRenderHint(QPainter::Antialiasing, true);
        const QRect row = visualItemRect(dropTarget_);
        if (row.isEmpty()) return;
        painter.setPen(QPen(QColor("#0a84ff"), 2));
        if (dropNest_) {
            painter.setBrush(QColor(10, 132, 255, 28));
            painter.drawRoundedRect(row.adjusted(2, 1, -2, -1), 8, 8);
        } else {
            painter.drawLine(row.left() + 4, row.top() + 1, row.right() - 4, row.top() + 1);
        }
    }

private:
    bool nestFor(QTreeWidgetItem* target, const QPoint& pos) const
    {
        if (!target) return false;
        const QRect row = visualItemRect(target);
        return !row.isEmpty() && pos.y() >= row.center().y() + row.height() / 4;
    }

    QTreeWidgetItem* dropTarget_ = nullptr;
    bool dropNest_ = false;
};

// Reader-side excerpt card: colour stripe, type + page, quote, then the user's note
// or tags. MarginNote keeps excerpts as cards rather than spreadsheet rows.
class ExcerptDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override
    {
        return QSize(qMax(120, option.rect.width()), 66);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect row = option.rect.adjusted(3, 2, -4, -2);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            painter->setPen(selected ? QColor("#b9d4ff") : Qt::NoPen);
            painter->setBrush(selected ? QColor("#dfeaff") : QColor("#f2f4f7"));
            painter->drawRoundedRect(row, 9, 9);
        }

        const QColor color = index.data(Qt::UserRole + 6).value<QColor>();
        if (color.isValid()) {
            painter->setPen(Qt::NoPen);
            painter->setBrush(color);
            painter->drawRoundedRect(QRect(row.left() + 6, row.top() + 8, 3, row.height() - 16),
                                     1.5, 1.5);
        }

        const int left = row.left() + 16;
        const int right = row.right() - 8;
        QFont meta = option.font;
        meta.setPointSizeF(8.5);
        painter->setFont(meta);
        painter->setPen(QColor("#8a8a8e"));
        const QString kind = index.data(Qt::UserRole + 2).toString();
        const QString page = QStringLiteral("P%1").arg(index.data(Qt::UserRole + 1).toInt());
        painter->drawText(QRect(left, row.top() + 6, right - left - 44, 15),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(kind, Qt::ElideRight, right - left - 48));
        painter->drawText(QRect(right - 42, row.top() + 6, 42, 15),
                          Qt::AlignRight | Qt::AlignVCenter, page);

        QFont quoteFont = option.font;
        quoteFont.setPointSizeF(11.0);
        quoteFont.setWeight(QFont::DemiBold);
        painter->setFont(quoteFont);
        painter->setPen(QColor("#1d1d1f"));
        const QString quote = index.data(Qt::UserRole + 3).toString();
        painter->drawText(QRect(left, row.top() + 22, right - left, 20),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(quote, Qt::ElideRight, right - left));

        painter->setFont(meta);
        painter->setPen(QColor("#6b6b70"));
        const QString comment = index.data(Qt::UserRole + 4).toString();
        const QString tags = index.data(Qt::UserRole + 5).toString();
        QString second = comment.isEmpty() ? tags : comment;
        if (second.isEmpty()) second = index.data(Qt::UserRole + 7).toString();
        painter->drawText(QRect(left, row.top() + 43, right - left, 16),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(second, Qt::ElideRight, right - left));
        painter->restore();
    }
};

// Unattended runs (CI, scripts) set ADAPT_HEADLESS=1: modal dialogs would block the
// event loop forever under QT_QPA_PLATFORM=offscreen.
const bool kHeadless = qEnvironmentVariableIsSet("ADAPT_HEADLESS");

bool confirm(QWidget* parent, const QString& title, const QString& text)
{
    if (kHeadless) return false;   // "No" by default: nothing destructive happens
    return QMessageBox::question(parent, title, text, QMessageBox::Yes | QMessageBox::No,
                                 QMessageBox::No) == QMessageBox::Yes;
}

void inform(QWidget* parent, const QString& title, const QString& text)
{
    if (!kHeadless) QMessageBox::information(parent, title, text);
}

void warnUser(QWidget* parent, const QString& title, const QString& text)
{
    if (!kHeadless) QMessageBox::warning(parent, title, text);
}

QString askText(QWidget* parent, const QString& title, const QString& label,
                const QString& initial = {})
{
    if (kHeadless) return initial;
    bool ok = false;
    const QString s = QInputDialog::getText(parent, title, label, QLineEdit::Normal, initial, &ok);
    return ok ? s : QString();
}

QString askMultiline(QWidget* parent, const QString& title, const QString& label,
                     const QString& initial = {})
{
    if (kHeadless) return initial;
    bool ok = false;
    const QString s = QInputDialog::getMultiLineText(parent, title, label, initial, &ok);
    return ok ? s : QString();
}

// Yes/No with an explicit default: headless runs pick the default instead of blocking.
bool askYes(QWidget* parent, const QString& title, const QString& text, bool defaultYes)
{
    if (kHeadless) return defaultYes;
    return QMessageBox::question(parent, title, text, QMessageBox::Yes | QMessageBox::No,
                                 defaultYes ? QMessageBox::Yes : QMessageBox::No)
           == QMessageBox::Yes;
}

// Single choice from a list. Headless runs keep the current entry.
QString askItem(QWidget* parent, const QString& title, const QString& label,
                const QStringList& items, int current = 0)
{
    if (kHeadless) return items.value(current);
    bool ok = false;
    const QString s =
        QInputDialog::getItem(parent, title, label, items, current, false, &ok);
    return ok ? s : QString();
}


const char* kColors[] = {"#FFD500", "#34A853", "#D93025", "#3D8BFD", "#FF9800"};

// File identity used to remember "we already asked about this revision".
QString identityKey(const QString& docPath)
{
    const QFileInfo fi(docPath);
    return QStringLiteral("annotPrompt.%1")
        .arg(QString::fromLatin1(
            QCryptographicHash::hash((fi.absoluteFilePath() + QLatin1Char(':') +
                                      QString::number(fi.size()) + QLatin1Char(':') +
                                      QString::number(fi.lastModified().toMSecsSinceEpoch()))
                                         .toUtf8(),
                                     QCryptographicHash::Sha1)
                .toHex()
                .left(16)));
}

QIcon glyphIcon(const QString& kind, const QColor& accent = QColor(0x3d, 0x8b, 0xfd))
{
    QPixmap pm(20, 20);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);

    if (kind == "open") {
        p.setPen(QPen(QColor(0x55, 0x5b, 0x66), 1.4));
        p.setBrush(QColor(0xf2, 0xc4, 0x4d));
        QPainterPath path;
        path.moveTo(3, 6);
        path.lineTo(8, 6);
        path.lineTo(9.5, 8);
        path.lineTo(17, 8);
        path.lineTo(17, 16);
        path.lineTo(3, 16);
        path.closeSubpath();
        p.drawPath(path);
    } else if (kind == "highlight") {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0xff, 0xd5, 0x00, 190));
        p.drawRect(3, 10, 14, 5);
        p.setPen(QPen(QColor(0x8a, 0x6d, 0x00), 2));
        p.drawLine(4, 6, 16, 6);
    } else if (kind == "underline") {
        p.setPen(QPen(QColor(0x33, 0x33, 0x33), 2));
        p.drawLine(3, 7, 17, 7);
        p.setPen(QPen(QColor(0x34, 0xa8, 0x53), 2.4));
        p.drawLine(3, 13, 17, 13);
    } else if (kind == "squiggly") {
        p.setPen(QPen(QColor(0x33, 0x33, 0x33), 2));
        p.drawLine(3, 5, 17, 5);
        p.setPen(QPen(QColor(0xd9, 0x30, 0x25), 1.6));
        QPainterPath path;
        path.moveTo(3, 12);
        for (int x = 3; x <= 17; x += 3) {
            path.lineTo(x + 1.5, 9.5);
            path.lineTo(x + 3, 12);
        }
        p.drawPath(path);
    } else if (kind == "note") {
        p.setPen(QPen(QColor(0xa66a, 0x3a, 0x00), 1));
        p.setBrush(QColor(0xff, 0xf0, 0xb3));
        p.drawRoundedRect(QRectF(3, 3, 14, 14), 2, 2);
        p.setPen(QPen(QColor(0x9b, 0x76, 0x16), 1));
        p.drawLine(6, 7, 14, 7); p.drawLine(6, 10, 14, 10); p.drawLine(6, 13, 11, 13);
    } else if (kind == "pen") {
        p.setPen(QPen(QColor(0x4b, 0x55, 0x63), 1.2));
        p.setBrush(QColor(0xd9, 0xe7, 0xfb));
        QPolygonF nib{{4, 15}, {14, 4}, {17, 7}, {7, 17}};
        p.drawPolygon(nib);
        p.setPen(QPen(accent, 2)); p.drawLine(3, 18, 8, 16);
    } else if (kind == "eraser") {
        p.setPen(QPen(QColor(0xa0, 0x4a, 0x4a), 1.1));
        p.setBrush(QColor(0xf7, 0xce, 0xd0));
        QPolygonF body{{4, 13}, {12, 4}, {18, 9}, {10, 17}};
        p.drawPolygon(body);
        p.setPen(QPen(QColor(0xa0, 0x4a, 0x4a), 1)); p.drawLine(8, 13, 13, 17);
    } else if (kind == "export") {
        p.setPen(QPen(QColor(0x3d, 0x6f, 0x55), 1.3));
        p.setBrush(QColor(0xe6, 0xf3, 0xe9));
        p.drawRoundedRect(QRectF(3, 8, 14, 9), 1, 1);
        p.setBrush(QColor(0xff, 0xff, 0xff)); p.drawRect(QRectF(6, 3, 8, 10));
        p.setPen(QPen(QColor(0x45, 0x8b, 0x5b), 1)); p.drawLine(8, 6, 12, 6); p.drawLine(8, 8, 12, 8);
    } else if (kind == "image") {
        p.setPen(QPen(QColor(0x55, 0x5b, 0x66), 1.2));
        p.setBrush(QColor(0xdfe6f0));
        p.drawRect(QRectF(3, 5, 14, 11));
        p.setBrush(QColor(0x9ec1f0));
        p.setPen(Qt::NoPen);
        p.drawPolygon(QPolygonF{{4, 15}, {8, 9}, {11, 13}, {13, 10}, {16, 15}});
    } else if (kind == "undo" || kind == "redo") {
        p.setPen(QPen(QColor(0x55, 0x5b, 0x66), 2));
        const bool left = kind == "undo";
        p.drawArc(QRectF(4, 5, 13, 12), left ? 30 * 16 : 150 * 16, left ? 220 * 16 : 220 * 16);
        p.setBrush(QColor(0x55, 0x5b, 0x66));
        p.setPen(Qt::NoPen);
        if (left)
            p.drawPolygon(QPolygonF{{3.0, 4.0}, {8.0, 6.0}, {3.5, 9.5}});
        else
            p.drawPolygon(QPolygonF{{17.0, 4.0}, {12.0, 6.0}, {16.5, 9.5}});
    } else if (kind == "delete") {
        p.setPen(QPen(QColor(0xd9, 0x30, 0x25), 2));
        p.drawLine(5, 5, 15, 15);
        p.drawLine(15, 5, 5, 15);
    } else if (kind == "color") {
        p.setPen(QPen(QColor(0x99, 0x99, 0x99), 1));
        p.setBrush(accent);
        p.drawEllipse(QRectF(4, 4, 12, 12));
    } else if (kind == "rotate") {
        p.setPen(QPen(QColor(0x55, 0x5b, 0x66), 2));
        p.drawArc(QRectF(4, 4, 12, 12), 40 * 16, 280 * 16);
        p.setBrush(QColor(0x55, 0x5b, 0x66));
        p.setPen(Qt::NoPen);
        p.drawPolygon(QPolygonF{{14.5, 3.0}, {17.5, 6.5}, {13.0, 7.0}});
    } else if (kind == "save") {
        p.setPen(QPen(QColor(0x55, 0x5b, 0x66), 1.4));
        p.setBrush(QColor(0xdfe3e8));
        p.drawRoundedRect(QRectF(3, 3, 14, 14), 2, 2);
        p.setBrush(accent);
        p.drawRect(QRectF(6, 11, 8, 5));
    } else if (kind == "find") {
        p.setPen(QPen(QColor(0x55, 0x5b, 0x66), 1.8));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(QRectF(4, 4, 9, 9));
        p.drawLine(QPointF(11.5, 11.5), QPointF(16, 16));
    } else if (kind == "sync") {
        p.setPen(QPen(QColor(0x2a, 0x6f, 0xd0), 2));
        p.setBrush(Qt::NoBrush);
        p.drawArc(QRectF(3.5, 3.5, 13, 13), 30 * 16, 150 * 16);
        p.drawArc(QRectF(3.5, 3.5, 13, 13), 210 * 16, 150 * 16);
        p.setBrush(QColor(0x2a, 0x6f, 0xd0));
        p.setPen(Qt::NoPen);
        p.drawPolygon(QPolygonF{{12.5, 3.0}, {17.0, 6.0}, {12.0, 7.5}});
        p.drawPolygon(QPolygonF{{7.5, 17.0}, {3.0, 14.0}, {8.0, 12.5}});
    } else if (kind == "read" || kind == "organize" || kind == "mind") {
        p.setPen(QPen(QColor(0x4b, 0x5b, 0x73), 1.4));
        p.setBrush(QColor(0xf7, 0xf9, 0xfc));
        if (kind == "read") {
            p.drawRoundedRect(QRectF(3, 3, 14, 14), 2, 2);
            p.drawLine(10, 4, 10, 16);
            p.drawLine(5, 7, 8, 7); p.drawLine(12, 7, 15, 7);
            p.drawLine(5, 10, 8, 10); p.drawLine(12, 10, 15, 10);
        } else if (kind == "organize") {
            p.drawRoundedRect(QRectF(2.5, 5, 6, 5), 1, 1);
            p.drawRoundedRect(QRectF(11.5, 3, 6, 5), 1, 1);
            p.drawRoundedRect(QRectF(11.5, 12, 6, 5), 1, 1);
            p.drawLine(8.5, 7.5, 10, 7.5); p.drawLine(10, 5.5, 10, 14.5);
            p.drawLine(10, 5.5, 11.5, 5.5); p.drawLine(10, 14.5, 11.5, 14.5);
        } else {
            p.drawLine(4, 5, 10, 3); p.drawLine(10, 3, 16, 5);
            p.drawLine(4, 5, 4, 14); p.drawLine(16, 5, 16, 14);
            p.drawLine(4, 14, 10, 17); p.drawLine(16, 14, 10, 17);
            p.drawEllipse(QRectF(7, 1, 6, 5));
            p.drawEllipse(QRectF(1, 12, 6, 6)); p.drawEllipse(QRectF(13, 12, 6, 6));
        }
    } else if (kind == "prev" || kind == "next") {
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x55, 0x5b, 0x66));
        QPolygonF tri;
        if (kind == "prev")
            tri << QPointF(13, 4) << QPointF(13, 16) << QPointF(6, 10);
        else
            tri << QPointF(7, 4) << QPointF(7, 16) << QPointF(14, 10);
        p.drawPolygon(tri);
    }
    p.end();
    return QIcon(pm);
}

QIcon swatchIcon(const QColor& c)
{
    QPixmap pm(12, 12);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setPen(QPen(c.darker(130), 1));
    p.setBrush(c);
    p.drawRoundedRect(QRectF(1, 1, 10, 10), 2, 2);
    p.end();
    return QIcon(pm);
}

MarkupType markupTypeForAction(const QString& key)
{
    if (key == "underline") return MarkupType::Underline;
    if (key == "squiggly") return MarkupType::Squiggly;
    if (key == "note") return MarkupType::Note;
    return MarkupType::Highlight;
}

QString markupTypeLabel(MarkupType t)
{
    switch (t) {
    case MarkupType::Highlight: return QStringLiteral("高亮");
    case MarkupType::Underline: return QStringLiteral("下划线");
    case MarkupType::Squiggly: return QStringLiteral("波浪线");
    case MarkupType::Note: return QStringLiteral("批注");
    case MarkupType::Image: return QStringLiteral("摘录图");
    case MarkupType::Ink: return QStringLiteral("手写");
    }
    return {};
}

QString excerptOf(const Markup& m, int max = 42)
{
    QString s = m.text.simplified();
    if (m.type == MarkupType::Image && s.isEmpty()) s = QStringLiteral("(图片摘录)");
    if (s.size() > max) s = s.left(max) + QStringLiteral("…");
    return s;
}

// The sentence that contains a looked-up word, used when no dictionary example exists.
QString sentenceAround(const QString& text, const QString& word)
{
    if (word.trimmed().isEmpty() || text.isEmpty()) return {};
    const int pos = text.indexOf(word.trimmed(), 0, Qt::CaseInsensitive);
    if (pos < 0) return {};
    static const QString ends = QStringLiteral("。！？!?;；\n");
    int start = pos;
    while (start > 0 && !ends.contains(text.at(start - 1))) --start;
    int end = pos + word.trimmed().size();
    while (end < text.size() && !ends.contains(text.at(end))) ++end;
    if (end < text.size()) ++end;
    return text.mid(start, end - start).simplified().left(300);
}

}  // namespace

// ---------------------------------------------------------------- undo commands

class MarkupCommand : public QUndoCommand {
public:
    MarkupCommand(MainWindow* w, MainWindow::CmdKind kind, const Markup& before, const Markup& after,
                  const QString& text)
        : w_(w), kind_(kind), before_(before), after_(after)
    {
        setText(text);
    }

    void undo() override { w_->applyCommand(kind_, before_, after_, true); }
    void redo() override { w_->applyCommand(kind_, before_, after_, false); }

private:
    MainWindow* w_;
    MainWindow::CmdKind kind_;
    Markup before_;
    Markup after_;
};

// Reordering touches several notes at once; undo must restore all of them in one step.
class MultiMarkupCommand : public QUndoCommand {
public:
    MultiMarkupCommand(MainWindow* w, const QVector<QPair<Markup, Markup>>& pairs,
                       const QString& text)
        : w_(w)
        , pairs_(pairs)
    {
        setText(text);
    }

    void undo() override
    {
        for (const auto& p : pairs_) w_->storeUpsertStampedMarkup(p.first);
        w_->refreshFromStore();
    }
    void redo() override
    {
        for (const auto& p : pairs_) w_->storeUpsertStampedMarkup(p.second);
        w_->refreshFromStore();
    }

private:
    MainWindow* w_;
    QVector<QPair<Markup, Markup>> pairs_;
};

// A mind-map group is its own entity, so its own undo command: create, rename, delete and
// re-parent all restore by writing the stored row back.
class MindGroupCommand : public QUndoCommand {
public:
    enum Kind { Add, Remove, Modify };

    MindGroupCommand(MainWindow* w, Kind kind, const MindGroup& before, const MindGroup& after,
                     const QString& text)
        : w_(w), kind_(kind), before_(before), after_(after)
    {
        setText(text);
        if (kind_ == Remove) {
            for (const auto& note : w_->store_.markups(before_.docPath))
                if (note.mindGroup == before_.id) removedNotes_.append(note);
            for (const auto& group : w_->store_.mindGroups(before_.docPath))
                if (group.parentId == before_.id) removedChildren_.append(group);
        }
    }

    void undo() override { apply(before_, true); }
    void redo() override { apply(after_, false); }

private:
    void apply(const MindGroup& state, bool undoing)
    {
        MindGroup row = state;
        switch (kind_) {
        case Add:
            // redo = create (alive), undo = tombstone it again
            row.deleted = undoing;
            w_->store_.upsertMindGroup(w_->store_.stampGroupForLocalWrite(row));
            break;
        case Remove:
            // the row we stored is an alive snapshot; undoing brings it back
            if (!undoing) {
                w_->store_.removeMindGroup(row.id);
            } else {
                row.deleted = false;
                w_->store_.upsertMindGroup(w_->store_.stampGroupForLocalWrite(row));
                for (const auto& note : removedNotes_) w_->storeUpsertStampedMarkup(note);
                for (const auto& child : removedChildren_)
                    w_->store_.upsertMindGroup(w_->store_.stampGroupForLocalWrite(child));
            }
            break;
        case Modify:
            w_->store_.upsertMindGroup(w_->store_.stampGroupForLocalWrite(row));
            break;
        }
        w_->refreshFromStore();
        w_->rebuildMindMapForTest();
    }

    MainWindow* w_;
    Kind kind_;
    MindGroup before_;
    MindGroup after_;
    QVector<Markup> removedNotes_;
    QVector<MindGroup> removedChildren_;
};

class MindLinkCommand : public QUndoCommand {
public:
    enum Kind { Add, Remove, Modify };

    MindLinkCommand(MainWindow* w, Kind kind, const MindLink& before, const MindLink& after,
                    const QString& text)
        : w_(w), kind_(kind), before_(before), after_(after)
    {
        setText(text);
    }

    void undo() override { apply(before_, true); }
    void redo() override { apply(after_, false); }

private:
    void apply(const MindLink& state, bool undoing)
    {
        MindLink row = (kind_ == Add) ? after_ : state;
        if (kind_ == Add) row.deleted = undoing;
        else if (kind_ == Remove) row.deleted = !undoing;
        VersionVector vv = VersionVector::parse(row.vv);
        for (const MindLink& current : w_->store_.mindLinks(row.topicId, true))
            if (current.id == row.id) vv.join(VersionVector::parse(current.vv));
        vv.bump(w_->store_.deviceId(), static_cast<quint64>(QDateTime::currentMSecsSinceEpoch()));
        row.vv = vv.toString();
        row.deviceId = w_->store_.deviceId();
        row.updated = QDateTime::currentMSecsSinceEpoch();
        w_->store_.upsertMindLink(row);
        w_->rebuildMindMap();
        w_->refreshNotebookWorkspace();
    }

    MainWindow* w_;
    Kind kind_;
    MindLink before_;
    MindLink after_;
};

// ---------------------------------------------------------------- construction

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    // ADAPT_DATA_DIR / a `data` folder next to the executable keeps the portable build
    // self-contained on Windows, where Qt ignores XDG_DATA_HOME.
    const QString dataDir = an::defaultDataDir(QCoreApplication::applicationDirPath());
    QString err;
    if (!store_.open(dataDir + "/library.db", &err))
        qWarning("store open failed: %s", qPrintable(err));

    undo_ = new QUndoStack(this);

    findDebounce_ = new QTimer(this);
    findDebounce_->setSingleShot(true);
    findDebounce_->setInterval(220);

    stateTimer_ = new QTimer(this);
    stateTimer_->setSingleShot(true);
    stateTimer_->setInterval(600);

    buildUi();
}

void MainWindow::buildUi()
{
    setWindowTitle(QStringLiteral("AdaptNote — 阅读 · 标注 · 脑图 · 生词"));
    resize(1440, 920);

    view_ = new PageView(this);
    view_->setEngine(&engine_);
    // Reading area and mind map live side by side instead of in a modal dialog: jumping to
    // the source must not throw away the map the user is working in.
    splitter_ = new QSplitter(Qt::Horizontal, this);
    splitter_->addWidget(view_);
    auto* workspace = new QWidget(this);
    auto* workspaceLayout = new QVBoxLayout(workspace);
    workspaceLayout->setContentsMargins(0, 0, 0, 0);
    workspaceLayout->setSpacing(0);
    notebookTabs_ = new QTabBar(workspace);
    notebookTabs_->setObjectName("notebookTabs");
    notebookTabs_->setExpanding(false);
    notebookTabs_->setTabsClosable(true);
    notebookTabs_->addTab(QStringLiteral("主页"));
    notebookTabs_->setTabButton(0, QTabBar::RightSide, nullptr);
    auto* applicationRail = new QWidget(workspace);
    applicationRail->setObjectName("applicationRail");
    auto* applicationRailLayout = new QHBoxLayout(applicationRail);
    applicationRailLayout->setContentsMargins(18, 4, 14, 4);
    applicationRailLayout->setSpacing(18);
    auto* brand = new QLabel(QStringLiteral("AdaptNote"), applicationRail);
    brand->setObjectName("applicationBrand");
    applicationRailLayout->addWidget(brand);
    applicationRailLayout->addWidget(notebookTabs_, 1);
    workspaceLayout->addWidget(applicationRail);
    connect(notebookTabs_, &QTabBar::currentChanged, this, [this](int index) {
        if (index == 0) { showLibrary(); return; }
        const QString id = notebookTabs_->tabData(index).toString();
        if (!id.isEmpty() && id != currentTopic_) openNotebook(id);
    });
    connect(notebookTabs_, &QTabBar::tabCloseRequested, this, [this](int index) {
        if (index <= 0) return;
        const QSignalBlocker block(notebookTabs_);
        notebookTabs_->removeTab(index);
        // Closing a tab only closes its UI entry, never deletes the notebook.
        if (notebookTabs_->count() > 1) {
            const QString id = notebookTabs_->tabData(notebookTabs_->currentIndex()).toString();
            if (!id.isEmpty()) openNotebook(id);
        } else {
            currentTopic_.clear();
            refreshNotebookWorkspace();
            showLibrary();
        }
    });
    notebookTitle_ = new QLabel(QStringLiteral("AdaptNote · 文档阅读"), workspace);
    notebookTitle_->setObjectName("notebookHeader");
    notebookTitle_->hide(); // selected notebook is already named in the app-level tab
    notebookDocuments_ = new QTabBar(workspace);
    notebookDocuments_->setObjectName("notebookDocuments");
    notebookDocuments_->setExpanding(false);
    notebookDocuments_->setDocumentMode(true);
    notebookDocuments_->setUsesScrollButtons(true);
    auto* documentStrip = new QWidget(workspace);
    documentStrip->setObjectName("documentStrip");
    auto* documentStripLayout = new QHBoxLayout(documentStrip);
    documentStripLayout->setContentsMargins(14, 5, 14, 5);
    documentStripLayout->setSpacing(6);
    auto* notebookName = new QLabel(notebookTitle_->text(), documentStrip);
    notebookName->setObjectName("currentNotebookTitle");
    documentStripLayout->addWidget(notebookName);
    documentStripLayout->setSpacing(6);
    documentStripLayout->addWidget(notebookDocuments_, 1);
    auto* newNotebookButton = new QToolButton(documentStrip);
    newNotebookButton->setText(QStringLiteral("+ 笔记本"));
    newNotebookButton->setToolTip(QStringLiteral("新建笔记本"));
    auto* addDocumentButton = new QToolButton(documentStrip);
    addDocumentButton->setText(QStringLiteral("+ 资料"));
    addDocumentButton->setToolTip(QStringLiteral("添加 PDF 或 EPUB 到当前笔记本"));
    auto* mindWindowButton = new QToolButton(documentStrip);
    mindWindowButton->setText(QStringLiteral("脑图 ↗"));
    mindWindowButton->setToolTip(QStringLiteral("在独立窗口查看当前笔记本脑图"));
    documentStripLayout->addWidget(newNotebookButton);
    documentStripLayout->addWidget(addDocumentButton);
    documentStripLayout->addWidget(mindWindowButton);
    auto* appearance = new QComboBox(documentStrip);
    appearance->setObjectName("readerAppearance");
    appearance->addItems({QStringLiteral("纸白"), QStringLiteral("暖纸"), QStringLiteral("夜间")});
    documentStripLayout->addWidget(appearance);
    connect(appearance, &QComboBox::currentIndexChanged, this, [this](int theme) {
        view_->setAppearanceTheme(theme);
        store_.setSetting(QStringLiteral("reader.appearance"), QString::number(theme));
    });
    appearance->setCurrentIndex(store_.setting(QStringLiteral("reader.appearance"), QStringLiteral("0")).toInt());
    view_->setAppearanceTheme(appearance->currentIndex());
    workspaceLayout->addWidget(documentStrip);
    connect(mindWindowButton, &QToolButton::clicked, this, &MainWindow::showNotebookMindMap);
    connect(newNotebookButton, &QToolButton::clicked, this, [this]() {
        const QString name = askText(this, QStringLiteral("新建笔记本"), QStringLiteral("名称："));
        if (!name.trimmed().isEmpty()) createNotebook(name);
    });
    connect(addDocumentButton, &QToolButton::clicked, this, [this]() {
        if (currentTopic_.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("请先新建或打开笔记本，再添加资料"), 4000);
            return;
        }
        if (kHeadless) return;
        const QStringList paths = QFileDialog::getOpenFileNames(this, QStringLiteral("添加阅读资料"),
                                    QString(), QStringLiteral("阅读资料 (*.pdf *.epub)"));
        for (const QString& path : paths) {
            if (!addNotebookDocument(path)) {
                statusBar()->showMessage(QStringLiteral("添加失败：%1；此前已成功添加的资料会保留").arg(path), 5000);
                break;
            }
        }
    });
    workspaceLayout->addWidget(splitter_, 1);
    setCentralWidget(workspace);
    connect(notebookDocuments_, &QTabBar::currentChanged, this, [this](int index) {
        const QString path = notebookDocuments_->tabData(index).toString();
        if (path.isEmpty() || path == docPath_) return;
        saveReadingState();
        if (!openDocument(path)) refreshNotebookWorkspace();
    });
    ensureMindPanel();

    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view_, &QWidget::customContextMenuRequested, this, &MainWindow::showViewContextMenu);
    connect(view_, &PageView::pageChanged, this, [this](int) {
        updateStatus();
        saveReadingState();
        if (pageThumbs_ && view_) {
            const int page = view_->currentPage();
            if (page >= 0 && page < pageThumbs_->count()) {
                pageThumbs_->setCurrentRow(page);
                if (QListWidgetItem* item = pageThumbs_->currentItem())
                    pageThumbs_->scrollToItem(item, QAbstractItemView::EnsureVisible);
            }
        }
    });
    connect(view_, &PageView::zoomChanged, this, [this](float z) {
        updateSelectionTranslationAction();
        updateStatus();
        if (zoomBox_) {
            QSignalBlocker b(zoomBox_);
            zoomBox_->setCurrentIndex(2);
            zoomBox_->setItemText(2, QStringLiteral("%1%").arg(qRound(z * 100)));
        }
    });
    connect(view_, &PageView::selectionChanged, this, [this]() { updateStatus(); updateSelectionTranslationAction(); });
    connect(view_, &PageView::regionSelected, this, &MainWindow::onRegionSelected);
    connect(view_->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { stateTimer_->start(); updateSelectionTranslationAction(); });
    connect(view_->horizontalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { updateSelectionTranslationAction(); });
    connect(stateTimer_, &QTimer::timeout, this, &MainWindow::saveReadingState);

    buildToolbar();
    buildFindBar();
    buildDocks();
    buildAiDock();
    // MarginNote-style floating pill: annotation tools, lookup and OCR stay next to the
    // selection instead of forcing a trip to the toolbar.
    selectionBar_ = new QWidget(view_->viewport());
    selectionBar_->setObjectName("selectionBar");
    auto* barLayout = new QHBoxLayout(selectionBar_);
    barLayout->setContentsMargins(6, 5, 6, 5);
    barLayout->setSpacing(2);
    auto addBarButton = [&](const QString& label, const QString& tip,
                            const std::function<void()>& fn) -> QToolButton* {
        auto* b = new QToolButton(selectionBar_);
        b->setText(label);
        b->setToolTip(tip);
        b->setCursor(Qt::PointingHandCursor);
        b->setToolButtonStyle(Qt::ToolButtonTextOnly);
        connect(b, &QToolButton::clicked, this, fn);
        barLayout->addWidget(b);
        return b;
    };
    addBarButton(QStringLiteral("高亮"), QStringLiteral("高亮选中文字 · H"),
                 [this]() { addMarkupFromSelection(MarkupType::Highlight); });
    addBarButton(QStringLiteral("下划线"), QStringLiteral("下划线 · U"),
                 [this]() { addMarkupFromSelection(MarkupType::Underline); });
    addBarButton(QStringLiteral("批注"), QStringLiteral("添加个人批注 · N"),
                 [this]() { addMarkupFromSelection(MarkupType::Note); });
    auto* barDivider = new QFrame(selectionBar_);
    barDivider->setFrameShape(QFrame::VLine);
    barDivider->setObjectName("selectionBarDivider");
    barLayout->addWidget(barDivider);
    addBarButton(QStringLiteral("查词"), QStringLiteral("查词并加入生词本（本地词典优先，其次 AI）"),
                 [this]() { lookupSelectionWord(); });
    translateSelectionButton_ = addBarButton(QStringLiteral("翻译"),
                                             QStringLiteral("划词翻译（流式）"),
                                             [this]() { showSelectionTranslation(); });
    translateSelectionButton_->setObjectName("selectionTranslateButton");
    addBarButton(QStringLiteral("AI"), QStringLiteral("对选中内容向 AI 提问"), [this]() {
        if (aiPanel_ && leftTabs_) leftTabs_->setCurrentWidget(aiPanel_);
        runAiWithSelection();
    });
    addBarButton(QStringLiteral("OCR"), QStringLiteral("识别选中区域/剪贴板图片中的文字"),
                 [this]() { ocrSelection(); });
    auto* barShadow = new QGraphicsDropShadowEffect(selectionBar_);
    barShadow->setBlurRadius(22);
    barShadow->setOffset(0, 5);
    barShadow->setColor(QColor(0, 0, 0, 45));
    selectionBar_->setGraphicsEffect(barShadow);
    selectionBar_->hide();
    buildStatusBar();
    // Reading is the default: keep the page and notes visible, hide optional panels.
    setWorkspaceMode(0);
    const auto restoredTabs = QJsonDocument::fromJson(store_.setting(QStringLiteral("workspace.openNotebooks")).toUtf8()).array();
    {
        const QSignalBlocker block(notebookTabs_);
        for (const auto& value : restoredTabs) {
            for (const auto& topic : store_.topics()) {
                if (topic.id != value.toString()) continue;
                const int index = notebookTabs_->addTab(topic.name);
                notebookTabs_->setTabData(index, topic.id);
            }
        }
    }
    if (!kHeadless) {
        const QString active = store_.setting(QStringLiteral("workspace.activeNotebook"));
        if (!active.isEmpty()) QTimer::singleShot(0, this, [this, active]() {
            if (docPath_.isEmpty()) openNotebook(active);
        });
    }

    // ---- P5: handwriting + read aloud ----
    connect(view_, &PageView::inkStrokeReady, this, &MainWindow::onInkStroke);
    connect(view_, &PageView::inkEraseRequested, this, &MainWindow::onInkEraseRequested);

    reader_.setPageProvider([this](int page) { return engine_.textPage(page); },
                            engine_.pageCount());
    connect(&reader_, &read::Reader::sentenceChanged, this, &MainWindow::onReaderSentence);
    connect(&reader_, &read::Reader::finished, this, [this]() {
        readAction_->setChecked(false);
        statusBar()->showMessage(QStringLiteral("朗读结束"), 3000);
    });
}

void MainWindow::buildToolbar()
{
    auto* tb = addToolBar(QStringLiteral("主工具栏"));
    mainToolbar_ = tb;
    tb->setObjectName("mainToolbar");
    tb->setMovable(false);
    tb->setIconSize(QSize(18, 18));
    tb->setToolButtonStyle(Qt::ToolButtonIconOnly);

    QAction* open = tb->addAction(glyphIcon("open"), QStringLiteral("打开"));
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, &MainWindow::chooseFile);

    // Short, stable labels: createUndoAction() appends the command name ("撤销 摘录图片"),
    // which made the button grow and shift the whole toolbar. The command name goes into
    // the tooltip instead.
    QAction* undoAction = new QAction(glyphIcon("undo"), QStringLiteral("撤销"), this);
    undoAction->setShortcut(QKeySequence::Undo);
    undoAction->setEnabled(false);
    connect(undoAction, &QAction::triggered, undo_, &QUndoStack::undo);
    connect(undo_, &QUndoStack::canUndoChanged, undoAction, &QAction::setEnabled);
    connect(undo_, &QUndoStack::undoTextChanged, undoAction, [undoAction](const QString& t) {
        undoAction->setToolTip(t.isEmpty() ? QStringLiteral("没有可撤销的操作")
                                           : QStringLiteral("撤销：%1").arg(t));
    });
    tb->addAction(undoAction);

    QAction* redoAction = new QAction(glyphIcon("redo"), QStringLiteral("重做"), this);
    redoAction->setShortcuts({QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
    redoAction->setEnabled(false);
    connect(redoAction, &QAction::triggered, undo_, &QUndoStack::redo);
    connect(undo_, &QUndoStack::canRedoChanged, redoAction, &QAction::setEnabled);
    connect(undo_, &QUndoStack::redoTextChanged, redoAction, [redoAction](const QString& t) {
        redoAction->setToolTip(t.isEmpty() ? QStringLiteral("没有可重做的操作")
                                           : QStringLiteral("重做：%1").arg(t));
    });
    tb->addAction(redoAction);

    tb->addSeparator();
    QAction* prev = tb->addAction(glyphIcon("prev"), QString());
    prev->setToolTip(QStringLiteral("上一页 · Ctrl+←"));
    prev->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left));
    connect(prev, &QAction::triggered, this, [this]() { changePage(-1); });
    pageLabel_ = new QLabel(QStringLiteral(" - / - "));
    pageLabel_->setMinimumWidth(84);
    pageLabel_->setAlignment(Qt::AlignCenter);
    tb->addWidget(pageLabel_);
    QAction* next = tb->addAction(glyphIcon("next"), QString());
    next->setToolTip(QStringLiteral("下一页 · Ctrl+→"));
    next->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Right));
    connect(next, &QAction::triggered, this, [this]() { changePage(1); });

    tb->addSeparator();
    tb->addWidget(new QLabel(QStringLiteral(" 缩放 ")));
    zoomBox_ = new QComboBox;
    zoomBox_->addItems({QStringLiteral("适合宽度"), QStringLiteral("适合页面"),
                        QStringLiteral("100%"), QStringLiteral("150%"), QStringLiteral("200%")});
    zoomBox_->setEditable(true);
    zoomBox_->setMinimumWidth(108);
    tb->addWidget(zoomBox_);
    connect(zoomBox_, &QComboBox::activated, this, [this](int idx) {
        switch (idx) {
        case 0: view_->setZoomMode(ZoomMode::FitWidth); break;
        case 1: view_->setZoomMode(ZoomMode::FitPage); break;
        case 2: view_->setZoomMode(ZoomMode::Actual); break;
        case 3: view_->setZoom(1.5f); break;
        case 4: view_->setZoom(2.0f); break;
        default: break;
        }
    });

    // Layout and rotation are "set once" preferences, not moment-to-moment tools, so they
    // live in the 更多 menu instead of taking a third of the toolbar.
    auto* layoutMenu = new QMenu(QStringLiteral("版式"), this);
    auto* layoutGroup = new QActionGroup(layoutMenu);
    const struct { const char* label; PageLayoutMode mode; } layouts[] = {
        {"单页", PageLayoutMode::Single},
        {"连续", PageLayoutMode::Continuous},
        {"双页", PageLayoutMode::Double},
    };
    for (const auto& l : layouts) {
        QAction* a = layoutMenu->addAction(QString::fromUtf8(l.label));
        a->setCheckable(true);
        a->setActionGroup(layoutGroup);
        if (l.mode == PageLayoutMode::Continuous) a->setChecked(true);
        connect(a, &QAction::triggered, this, [this, mode = l.mode]() { view_->setLayoutMode(mode); });
    }
    QAction* rot = new QAction(glyphIcon("rotate"), QStringLiteral("旋转 90°"), this);
    rot->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_R));
    connect(rot, &QAction::triggered, this, [this]() { view_->setRotation(view_->rotation() + 90); });

    tb->addSeparator();
    auto addMarkupAction = [&](const QString& key, const QString& label, const QKeySequence& sc) {
        QAction* a = tb->addAction(glyphIcon(key), label);
        a->setShortcut(sc);
        const MarkupType t = markupTypeForAction(key);
        connect(a, &QAction::triggered, this, [this, t]() { addMarkupFromSelection(t); });
    };
    addMarkupAction("highlight", QStringLiteral("高亮"), QKeySequence(Qt::Key_H));
    addMarkupAction("underline", QStringLiteral("下划线"), QKeySequence(Qt::Key_U));
    addMarkupAction("squiggly", QStringLiteral("波浪线"), QKeySequence(Qt::Key_S));
    addMarkupAction("note", QStringLiteral("批注"), QKeySequence(Qt::Key_N));
    for (QAction* action : tb->actions()) {
        if (!action || action->isSeparator()) continue;
        if (action->text() == QStringLiteral("高亮")) action->setToolTip(QStringLiteral("黄色高亮 · H"));
        else if (action->text() == QStringLiteral("下划线")) action->setToolTip(QStringLiteral("下划线标注 · U"));
        else if (action->text() == QStringLiteral("波浪线")) action->setToolTip(QStringLiteral("波浪线标注 · S"));
        else if (action->text() == QStringLiteral("批注")) action->setToolTip(QStringLiteral("添加个人批注 · N"));
    }

    auto* toolGroup = new QActionGroup(this);
    toolGroup->setExclusive(true);
    inkAction_ = tb->addAction(glyphIcon("pen", QColor("#1f2328")), QStringLiteral("手写"));
    inkAction_->setToolTip(QStringLiteral("手写笔迹（P）：拖拽书写，再按一次退出"));
    inkAction_->setCheckable(true);
    inkAction_->setActionGroup(toolGroup);
    inkAction_->setShortcut(QKeySequence(Qt::Key_P));
    connect(inkAction_, &QAction::toggled, this, [this](bool on) {
        view_->setInkMode(on);
        statusBar()->showMessage(on ? QStringLiteral("手写模式：拖拽书写（P 退出）")
                                    : QStringLiteral("已退出手写模式"), 2500);
    });

    eraseAction_ = tb->addAction(glyphIcon("eraser", QColor("#d93025")), QStringLiteral("橡皮"));
    eraseAction_->setToolTip(QStringLiteral("橡皮（E）：拖过手写笔迹即可擦除"));
    eraseAction_->setCheckable(true);
    eraseAction_->setActionGroup(toolGroup);
    eraseAction_->setShortcut(QKeySequence(Qt::Key_E));
    connect(eraseAction_, &QAction::toggled, this, [this](bool on) {
        view_->setEraseMode(on);
        if (on && inkAction_ && inkAction_->isChecked()) inkAction_->setChecked(false);
        statusBar()->showMessage(on ? QStringLiteral("橡皮模式：拖过笔迹擦除（E 退出）")
                                    : QStringLiteral("已退出橡皮模式"), 2500);
    });

    // ink width presets live in the ink button's context menu
    if (auto* inkBtn = qobject_cast<QToolButton*>(tb->widgetForAction(inkAction_))) {
        auto* widthMenu = new QMenu(inkBtn);
        const QVector<QPair<QString, double>> presets = {
            {QStringLiteral("细"), 1.2}, {QStringLiteral("中"), 2.0}, {QStringLiteral("粗"), 3.2}};
        for (const auto& p : presets) {
            const double w = p.second;
            widthMenu->addAction(p.first, [this, w]() { setInkWidthPreset(w); });
        }
        widthMenu->addSeparator();
        widthMenu->addAction(QStringLiteral("笔迹颜色…"), [this]() {
            const QColor chosen =
                QColorDialog::getColor(inkColor_, this, QStringLiteral("笔迹颜色"),
                                       QColorDialog::ShowAlphaChannel);
            if (chosen.isValid()) setInkColor(chosen);
        });
        inkBtn->setMenu(widthMenu);
        inkBtn->setPopupMode(QToolButton::DelayedPopup);
        inkBtn->setContextMenuPolicy(Qt::ActionsContextMenu);
    }
    setInkWidthPreset(store_.setting(QStringLiteral("ink.width"), QStringLiteral("2.0")).toDouble());
    {
        const QColor saved(store_.setting(QStringLiteral("ink.color"), QStringLiteral("#1f2328")));
        setInkColor(saved.isValid() ? saved : QColor(0x1f, 0x23, 0x28));
    }

    // Workspace modes: the mind map is a panel beside the page, not a modal dialog.
    tb->addSeparator();
    auto* modeGroup = new QActionGroup(this);
    modeGroup->setExclusive(true);
    struct ModeDef {
        const char* label;
        const char* tip;
        int mode;
        QKeySequence key;
    };
    const ModeDef modes[] = {
        {"阅读", "专注阅读：隐藏脑图（Ctrl+1）", 0, QKeySequence(QStringLiteral("Ctrl+1"))},
        {"整理", "阅读 + 脑图并排（Ctrl+2）", 1, QKeySequence(QStringLiteral("Ctrl+2"))},
        {"脑图", "专注脑图（Ctrl+3 / M）", 2, QKeySequence(QStringLiteral("Ctrl+3"))},
    };
    for (const ModeDef& def : modes) {
        const QString modeIcon = def.mode == 0 ? QStringLiteral("read")
                                 : def.mode == 1 ? QStringLiteral("organize") : QStringLiteral("mind");
        QAction* a = tb->addAction(glyphIcon(modeIcon, QColor("#2a6fd0")),
                                   QString::fromUtf8(def.label));
        a->setToolTip(QString::fromUtf8(def.tip));
        a->setCheckable(true);
        a->setShortcut(def.key);
        a->setActionGroup(modeGroup);
        a->setData(def.mode);
        if (def.mode == workspaceMode_) a->setChecked(true);
        connect(a, &QAction::triggered, this, [this, mode = def.mode]() { setWorkspaceMode(mode); });
        // Named modes read better than three abstract glyphs (MarginNote uses words too).
        if (auto* modeButton = qobject_cast<QToolButton*>(tb->widgetForAction(a))) {
            modeButton->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
            modeButton->setAutoRaise(true);
        }
    }
    {
        auto* mindShortcut = new QAction(this);
        mindShortcut->setShortcut(QKeySequence(Qt::Key_M));
        mindShortcut->setShortcutContext(Qt::WindowShortcut);
        addAction(mindShortcut);
        connect(mindShortcut, &QAction::triggered, this, &MainWindow::showMindMap);
    }

    QAction* libraryAction = tb->addAction(glyphIcon("open", QColor("#2a6fd0")),
                                          QStringLiteral("书库"));
    libraryAction->setToolTip(QStringLiteral("书库：全部文档与跨库笔记（L）"));
    libraryAction->setShortcut(QKeySequence(Qt::Key_L));
    connect(libraryAction, &QAction::triggered, this, &MainWindow::showLibrary);

    readAction_ = tb->addAction(glyphIcon("prev"), QStringLiteral("朗读"));
    readAction_->setToolTip(QStringLiteral("逐句朗读（R）：有语音引擎就出声，没有则逐句高亮"));
    readAction_->setCheckable(true);
    readAction_->setShortcut(QKeySequence(Qt::Key_R));
    connect(readAction_, &QAction::toggled, this, [this](bool) { toggleReading(); });
    buildReaderMenu();

    QAction* excerpt = new QAction(glyphIcon("image"), QStringLiteral("摘录图片"), this);
    excerpt->setToolTip(QStringLiteral("拖拽框选页面区域，保存为图片摘录（X）"));
    excerpt->setShortcut(QKeySequence(Qt::Key_X));
    connect(excerpt, &QAction::triggered, this, &MainWindow::captureImageExcerpt);

    QAction* color = new QAction(glyphIcon("color", markupColor_), QStringLiteral("标注颜色…"), this);
    color->setToolTip(QStringLiteral("选择颜色"));
    connect(color, &QAction::triggered, this, [this]() {
        QStringList names;
        for (const char* c : kColors) names << QString::fromLatin1(c);
        const QString s = askItem(this, QStringLiteral("标注颜色"),
                                  QStringLiteral("选择颜色："), names, 0);
        if (!s.isEmpty()) markupColor_ = QColor(s);
    });

    QAction* del = new QAction(glyphIcon("delete"), QStringLiteral("删除选中标注"), this);
    del->setToolTip(QStringLiteral("删除选中的笔记（Del）"));
    del->setShortcut(QKeySequence::Delete);
    connect(del, &QAction::triggered, this, [this]() {
        if (notesList_ && notesList_->currentItem())
            deleteMarkupById(notesList_->currentItem()->data(Qt::UserRole).toString());
    });

    // export menu
    auto* exportBtn = new QToolButton(tb);
    exportBtn->setText(QStringLiteral("导出"));
    exportBtn->setIcon(glyphIcon("export", QColor("#34A853")));
    exportBtn->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    exportBtn->setPopupMode(QToolButton::InstantPopup);
    auto* exportMenu = new QMenu(exportBtn);
    const QVector<QPair<QString, QString>> formats = {
        {QStringLiteral("Markdown (.md)"), QStringLiteral("md")},
        {QStringLiteral("HTML (.html)"), QStringLiteral("html")},
        {QStringLiteral("OPML 大纲 (.opml)"), QStringLiteral("opml")},
        {QStringLiteral("XMind 脑图 (.xmind)"), QStringLiteral("xmind")},
        {QStringLiteral("Anki 卡包 (.apkg)"), QStringLiteral("apkg")},
    };
    for (const auto& f : formats) {
        const QString ext = f.second;
        exportMenu->addAction(f.first, [this, ext]() { doExport(ext); });
    }
    exportMenu->addSeparator();
    auto* libraryMenu = exportMenu->addMenu(QStringLiteral("导出整个书库"));
    for (const auto& f : formats) {
        const QString ext = f.second;
        libraryMenu->addAction(f.first, [this, ext]() { doExportLibrary(ext); });
    }

    auto* filteredAction = exportMenu->addAction(QStringLiteral("只导出筛选结果"));
    filteredAction->setCheckable(true);
    filteredAction->setChecked(exportFiltered_);
    connect(filteredAction, &QAction::toggled, this, [this](bool on) { exportFiltered_ = on; });

    auto* byGroupsAction = exportMenu->addAction(QStringLiteral("按脑图分组输出（MD/HTML）"));
    byGroupsAction->setCheckable(true);
    byGroupsAction->setChecked(exportByGroups_);
    connect(byGroupsAction, &QAction::toggled, this, [this](bool on) { exportByGroups_ = on; });
    exportBtn->setMenu(exportMenu);
    tb->addWidget(exportBtn);

    auto* importAction = tb->addAction(glyphIcon("save", QColor("#7a5af8")),
                                      QStringLiteral("导入"));
    importAction->setToolTip(
        QStringLiteral("从 Markdown（BookxNote 等）或本程序的 JSON 镜像导入笔记"));
    connect(importAction, &QAction::triggered, this, [this]() {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("导入笔记"), QString(),
            QStringLiteral("笔记文件 (*.md *.markdown *.json);;所有文件 (*)"));
        if (path.isEmpty()) return;

        bool intoCurrent = false;
        if (!docPath_.isEmpty()) {
            const bool answer = askYes(
                this, QStringLiteral("导入到哪本书"),
                QStringLiteral("把导入的笔记挂到当前打开的《%1》吗？\n\n"
                               "选「No」则按文件里记录的书名另建一条文档记录。")
                    .arg(QFileInfo(docPath_).completeBaseName()),
                    true);
            intoCurrent = answer;
        }

        QString err;
        if (!importNotes(path, intoCurrent, &err))
            warnUser(this, QStringLiteral("导入失败"), err);
    });

    auto* conflictAction = tb->addAction(glyphIcon("note", QColor("#d93025")),
                                        QStringLiteral("冲突"));
    conflictAction->setToolTip(QStringLiteral("处理同步冲突：两端都改过的笔记"));
    connect(conflictAction, &QAction::triggered, this, &MainWindow::showConflicts);

    auto* importPdfAction = tb->addAction(glyphIcon("highlight"), QStringLiteral("读标注"));
    importPdfAction->setToolTip(QStringLiteral("把当前 PDF 里已有的标注读进来变成笔记"));
    connect(importPdfAction, &QAction::triggered, this,
            [this]() { importEmbeddedAnnotations(); });

    QAction* syncAction = tb->addAction(glyphIcon("sync"), QStringLiteral("同步"));
    syncAction->setToolTip(QStringLiteral("与 WebDAV 服务器双向同步笔记（按钮右键：设置）"));
    connect(syncAction, &QAction::triggered, this, &MainWindow::runSync);
    if (auto* syncBtn = qobject_cast<QToolButton*>(tb->widgetForAction(syncAction))) {
        auto* settingsAction = new QAction(QStringLiteral("同步设置…"), syncBtn);
        connect(settingsAction, &QAction::triggered, this, &MainWindow::openSyncSettings);
        syncBtn->addAction(settingsAction);
        syncBtn->setContextMenuPolicy(Qt::ActionsContextMenu);
    }

    tb->addSeparator();
    QAction* save = tb->addAction(glyphIcon("save"), QStringLiteral("写入 PDF"));
    save->setToolTip(QStringLiteral("把标注写回 PDF（增量保存，Ctrl+S）"));
    save->setShortcut(QKeySequence::Save);
    connect(save, &QAction::triggered, this, &MainWindow::saveAnnotationsToPdf);

    // ---- 更多：低频操作收进菜单，工具栏才能保持一行不溢出 ----
    // Rules from docs/reader-redesign/ui-spec.md: one row, task-essential actions only,
    // shortcuts and menus keep the rest reachable.
    auto* moreBtn = new QToolButton(tb);
    moreBtn->setText(QStringLiteral("更多"));
    moreBtn->setToolTip(QStringLiteral("书库、导入、同步、导出等"));
    moreBtn->setPopupMode(QToolButton::InstantPopup);
    moreBtn->setToolButtonStyle(Qt::ToolButtonTextOnly);
    auto* more = new QMenu(moreBtn);
    for (QAction* a : {libraryAction, readAction_, excerpt, importAction, conflictAction,
                       importPdfAction, syncAction, save}) {
        if (!a) continue;
        tb->removeAction(a);
        more->addAction(a);
    }
    more->addSeparator();
    more->addMenu(layoutMenu);
    more->addAction(rot);
    more->addSeparator();
    for (QAction* a : {color, del}) {
        if (!a) continue;
        tb->removeAction(a);
        more->addAction(a);
    }
    moreMenu_ = more;
    moreBtn->setMenu(more);
    tb->addWidget(moreBtn);
    // Compact labelled controls retain discoverability while keeping low-frequency
    // mode/export controls reachable without the native overflow extension.
    tb->setStyleSheet(QStringLiteral("QToolButton{padding:3px 3px;min-width:28px;} QToolBar{spacing:1px;}"));
    for (QAction* action : tb->actions()) {
        if (!action || action->isSeparator() || action->text().isEmpty()) continue;
        if (action->toolTip().isEmpty()) action->setToolTip(action->text());
    }
}

void MainWindow::buildFindBar()
{
    auto* tb = addToolBar(QStringLiteral("查找"));
    tb->setObjectName("findToolbar");
    tb->setMovable(false);
    tb->setIconSize(QSize(18, 18));
    // A permanently visible search bar steals width from the page for something used
    // occasionally; Ctrl+F reveals it and Escape hides it again.
    tb->hide();
    findBar_ = tb;
    tb->addWidget(new QLabel(QStringLiteral(" 查找 ")));
    findEdit_ = new QLineEdit;
    findEdit_->setPlaceholderText(QStringLiteral("在本书中查找…（Ctrl+F）"));
    findEdit_->setClearButtonEnabled(true);
    findEdit_->setFixedWidth(220);
    tb->addWidget(findEdit_);

    // Scope: this page's text, or the notes of the whole reading topic. Page search
    // highlights inside the document; topic search fills the notes panel so results can
    // come from several books.
    findScope_ = new QComboBox;
    findScope_->addItem(QStringLiteral("本书"));
    findScope_->addItem(QStringLiteral("主题笔记"));
    findScope_->setToolTip(QStringLiteral("「主题笔记」需要先打开一个阅读主题"));
    tb->addWidget(findScope_);
    findLabel_ = new QLabel(QStringLiteral("0/0"));
    findLabel_->setMinimumWidth(64);
    findLabel_->setAlignment(Qt::AlignCenter);
    tb->addWidget(findLabel_);

    auto* prevBtn = new QToolButton;
    prevBtn->setText(QStringLiteral("▲"));
    prevBtn->setToolTip(QStringLiteral("上一个（Shift+Enter）"));
    tb->addWidget(prevBtn);
    auto* nextBtn = new QToolButton;
    nextBtn->setText(QStringLiteral("▼"));
    nextBtn->setToolTip(QStringLiteral("下一个（Enter）"));
    tb->addWidget(nextBtn);

    connect(prevBtn, &QToolButton::clicked, this, [this]() { nextHit(-1); });
    connect(nextBtn, &QToolButton::clicked, this, [this]() { nextHit(1); });
    connect(findEdit_, &QLineEdit::textChanged, this, [this](const QString& t) {
        findDebounce_->start();
        Q_UNUSED(t);
    });
    connect(findDebounce_, &QTimer::timeout, this,
            [this]() { findText(findEdit_->text()); });
    connect(findScope_, &QComboBox::currentIndexChanged, this, [this](int) {
        findText(findEdit_ ? findEdit_->text() : QString());
    });
    connect(findEdit_, &QLineEdit::returnPressed, this, [this]() { nextHit(1); });

    auto* findAction = new QAction(this);
    findAction->setShortcut(QKeySequence::Find);
    addAction(findAction);
    connect(findAction, &QAction::triggered, this, [this]() {
        if (findBar_) findBar_->show();
        findEdit_->setFocus();
        findEdit_->selectAll();
    });

    auto* escAction = new QAction(this);
    escAction->setShortcut(QKeySequence(Qt::Key_Escape));
    addAction(escAction);
    connect(escAction, &QAction::triggered, this, [this]() {
        if (findBar_ && findBar_->isVisible() && !findEdit_->hasFocus()) findBar_->hide();
        if (view_->captureMode()) {
            view_->setCaptureMode(false);
            statusBar()->showMessage(QStringLiteral("已取消框选"), 1500);
            return;
        }
        findEdit_->clear();
        view_->setSearchHits({}, -1);
        hits_.clear();
        hitIndex_ = -1;
        findLabel_->setText(QStringLiteral("0/0"));
    });
}

void MainWindow::buildDocks()
{
    // Left bookshelf rail: keep document browsing inside the main reading workspace.
    auto* shelf = new QDockWidget(QStringLiteral("书架"), this);
    bookshelfDock_ = shelf;
    shelf->setObjectName("bookshelfDock");
    shelf->setAllowedAreas(Qt::LeftDockWidgetArea);
    bookshelfContent_ = new QWidget(shelf);
    auto* shelfLayout = new QVBoxLayout(bookshelfContent_);
    shelfLayout->setContentsMargins(8, 8, 8, 8);
    auto* shelfTabs = new QTabWidget(bookshelfContent_);
    shelfTabs->setDocumentMode(true);

    auto* booksPane = new QWidget(shelfTabs);
    auto* booksLayout = new QVBoxLayout(booksPane);
    booksLayout->setContentsMargins(4, 8, 4, 4);
    auto* addBook = new QPushButton(QStringLiteral("+ 打开资料"), booksPane);
    addBook->setObjectName("shelfAddBook");
    booksLayout->addWidget(addBook);
    bookshelfList_ = new QListWidget(booksPane);
    bookshelfList_->setObjectName("bookshelfList");
    booksLayout->addWidget(bookshelfList_, 1);
    shelfTabs->addTab(booksPane, QStringLiteral("书架"));

    auto* topicsPane = new QWidget(shelfTabs);
    auto* topicsLayout = new QVBoxLayout(topicsPane);
    topicsLayout->setContentsMargins(4, 8, 4, 4);
    auto* addTopic = new QPushButton(QStringLiteral("+ 新建笔记本"), topicsPane);
    topicsLayout->addWidget(addTopic);
    topicShelfList_ = new QListWidget(topicsPane);
    topicShelfList_->setObjectName("topicShelfList");
    topicsLayout->addWidget(topicShelfList_, 1);
    shelfTabs->addTab(topicsPane, QStringLiteral("笔记本"));

    pageOutlineOnShelf_ = new QTreeWidget(shelfTabs);
    pageOutlineOnShelf_->setObjectName("shelfPageOutline");
    outlineTree_ = pageOutlineOnShelf_;
    pageOutlineOnShelf_->setHeaderHidden(true);
    shelfTabs->addTab(pageOutlineOnShelf_, QStringLiteral("目录"));

    // Page thumbnails, rendered in small batches so a 600-page book does not freeze the UI.
    pageThumbs_ = new QListWidget(shelfTabs);
    pageThumbs_->setObjectName("pageThumbs");
    pageThumbs_->setViewMode(QListView::IconMode);
    pageThumbs_->setIconSize(QSize(108, 144));
    pageThumbs_->setGridSize(QSize(124, 178));
    pageThumbs_->setResizeMode(QListView::Adjust);
    pageThumbs_->setMovement(QListView::Static);
    pageThumbs_->setUniformItemSizes(true);
    pageThumbs_->setWordWrap(false);
    pageThumbs_->setSpacing(4);
    pageThumbs_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    shelfTabs->addTab(pageThumbs_, QStringLiteral("页面"));
    connect(pageThumbs_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        const int page = item->data(Qt::UserRole).toInt();
        if (page >= 0) {
            view_->gotoPage(page);
            updateStatus();
        }
    });
    const QString savedShelfTab = store_.setting(QStringLiteral("ui.shelfTab"));
    if (!savedShelfTab.isEmpty()) {
        for (int i = 0; i < shelfTabs->count(); ++i) {
            if (shelfTabs->tabText(i) == savedShelfTab) {
                shelfTabs->setCurrentIndex(i);
                break;
            }
        }
    }
    connect(shelfTabs, &QTabWidget::currentChanged, this, [this, shelfTabs](int index) {
        if (!shelfTabs || index < 0 || index >= shelfTabs->count()) return;
        store_.setSetting(QStringLiteral("ui.shelfTab"), shelfTabs->tabText(index));
    });
    thumbTimer_ = new QTimer(this);
    thumbTimer_->setInterval(0);
    connect(thumbTimer_, &QTimer::timeout, this, &MainWindow::pumpPageThumbnails);


    shelfLayout->addWidget(shelfTabs, 1);

    connect(addBook, &QPushButton::clicked, this, [this]() { if (!kHeadless) chooseFile(); });
    connect(bookshelfList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (item) openDocument(item->data(Qt::UserRole).toString());
    });
    connect(bookshelfList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        const QString path = item->data(Qt::UserRole).toString();
        if (!currentTopic_.isEmpty()) addNotebookDocument(path); else openDocument(path);
    });
    connect(addTopic, &QPushButton::clicked, this, [this]() {
        const QString name = askText(this, QStringLiteral("新建笔记本"), QStringLiteral("名称："));
        if (!name.trimmed().isEmpty()) createNotebook(name);
    });
    connect(topicShelfList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (item) openNotebook(item->data(Qt::UserRole).toString());
    });
    connect(pageOutlineOnShelf_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        if (!item) return;
        const int page = item->data(0, Qt::UserRole).toInt();
        if (page >= 0) { view_->gotoPage(page); updateStatus(); }
    });
    shelf->setWidget(bookshelfContent_);
    shelf->setMinimumWidth(210);
    addDockWidget(Qt::LeftDockWidgetArea, shelf);
    resizeDocks({shelf}, {250}, Qt::Horizontal);

    auto* dock = new QDockWidget(QStringLiteral("摘录 / 笔记 / 生词 / 脑图"), this);
    notesDock_ = dock;
    dock->setObjectName("notesDock");
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    readerSideTabs_ = new QTabWidget(dock);
    readerSideTabs_->setObjectName("readerSideTabs");
    readerSideTabs_->setDocumentMode(true);
    leftTabs_ = readerSideTabs_;
    auto* notebookTree = new NotebookOutlineTree(leftTabs_);
    notebookOutline_ = notebookTree;
    notebookTree->setDragEnabled(true);
    notebookTree->setAcceptDrops(true);
    notebookTree->setDropIndicatorShown(true);
    notebookTree->setDragDropMode(QAbstractItemView::DragDrop);
    notebookTree->setObjectName("notebookOutline");
    notebookTree->moveGroup = [this](const QString& id, QTreeWidgetItem* target) {
        return target && moveNotebookGroup(id, target->data(0, Qt::UserRole + 3).toString(),
                                           target->data(0, Qt::UserRole + 2).toString());
    };
    notebookTree->moveNote = [this](const QString& id, QTreeWidgetItem* target, bool nestUnderParent) {
        if (!target) return false;
        const QString targetId = target->data(0, Qt::UserRole).toString();
        if (!targetId.isEmpty())
            return moveNotebookNote(id, target->data(0, Qt::UserRole + 3).toString(),
                                    target->data(0, Qt::UserRole + 4).toString(),
                                    nestUnderParent ? QString() : targetId,
                                    nestUnderParent ? targetId : target->data(0, Qt::UserRole + 5).toString(),
                                    nestUnderParent);
        return moveNotebookNote(id, target->data(0, Qt::UserRole + 3).toString(),
                                target->data(0, Qt::UserRole + 4).toString());
    };
    notebookOutline_->setObjectName("notebookOutline");
    notebookOutline_->setColumnCount(1);
    notebookOutline_->setItemDelegate(new NotebookOutlineDelegate(notebookOutline_));
    notebookOutline_->setHeaderHidden(true);
    notebookOutline_->header()->setStretchLastSection(false);
    notebookOutline_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
    notebookOutline_->setIndentation(18);
    notebookOutline_->setWordWrap(true);
    leftTabs_->addTab(notebookOutline_, QStringLiteral("笔记"));
    notebookOutline_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(notebookOutline_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        auto* item = notebookOutline_->itemAt(pos);
        if (!item) return;
        const QString noteId = item->data(0, Qt::UserRole).toString();
        const QString groupId = item->data(0, Qt::UserRole + 2).toString();
        const QString sourcePath = item->data(0, Qt::UserRole + 3).toString();
        const QString groupName = item->text(0);
        QMenu menu(notebookOutline_);
        if (!noteId.isEmpty()) {
            menu.addAction(QStringLiteral("定位原文"), [this, noteId]() { activateNotebookNote(noteId); });
            menu.addAction(QStringLiteral("编辑摘录批注…"), [this, noteId]() {
                if (activateNotebookNote(noteId)) editMarkupComment(noteId);
            });
            menu.addAction(QStringLiteral("删除笔记与标注"), [this, noteId]() {
                if (!activateNotebookNote(noteId)) return;
                if (confirm(this, QStringLiteral("删除笔记"), QStringLiteral("同时删除原文标注，可通过撤销恢复。是否继续？")))
                    deleteMarkupById(noteId);
            });
        } else if (!sourcePath.isEmpty()) {
            const QString parentMarkupId = item->data(0, Qt::UserRole + 5).toString();
            menu.addAction(QStringLiteral("新建自由笔记…"), [this, sourcePath, groupId, parentMarkupId]() {
                if (sourcePath != docPath_) { saveReadingState(); if (!openDocument(sourcePath)) return; }
                auto* editor = new QDialog(this);
                editor->setAttribute(Qt::WA_DeleteOnClose);
                editor->setWindowTitle(QStringLiteral("新建自由笔记"));
                editor->resize(520, 360);
                auto* form = new QFormLayout(editor);
                auto* title = new QLineEdit(editor);
                auto* body = new QPlainTextEdit(editor);
                title->setPlaceholderText(QStringLiteral("例如：这一章的核心问题"));
                body->setPlaceholderText(QStringLiteral("写下自己的理解、问题或下一步行动……"));
                form->addRow(QStringLiteral("标题"), title); form->addRow(QStringLiteral("内容"), body);
                auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, editor);
                form->addRow(buttons);
                connect(buttons, &QDialogButtonBox::rejected, editor, &QDialog::reject);
                connect(buttons, &QDialogButtonBox::accepted, editor, [this, editor, title, body, groupId, parentMarkupId]() {
                    if (title->text().trimmed().isEmpty() || docPath_.isEmpty()) return;
                    Markup note;
                    note.id = newId(); note.docPath = docPath_; note.page = view_->currentPage();
                    note.type = MarkupType::Note; note.title = title->text().trimmed(); note.comment = body->toPlainText();
                    note.mindGroup = groupId; note.parentMarkupId = parentMarkupId;
                    note.created = QDateTime::currentMSecsSinceEpoch(); note.updated = note.created;
                    undo_->push(new MarkupCommand(this, CmdKind::Add, Markup(), note, QStringLiteral("新建自由笔记")));
                    editor->accept();
                });
                editor->show();
            });
            menu.addSeparator();
            menu.addAction(groupId.isEmpty() ? QStringLiteral("新建分组…") : QStringLiteral("新建子分组…"),
                           [this, sourcePath, groupId]() {
                const QString name = askText(this, QStringLiteral("新建分组"), QStringLiteral("名称："));
                if (name.trimmed().isEmpty()) return;
                if (sourcePath != docPath_) { saveReadingState(); if (!openDocument(sourcePath)) return; }
                createMindGroup(name.trimmed(), groupId);
            });
            if (!groupId.isEmpty()) {
                menu.addAction(QStringLiteral("重命名分组…"), [this, sourcePath, groupId, groupName]() {
                    const QString name = askText(this, QStringLiteral("重命名分组"), QStringLiteral("名称："), groupName);
                    if (name.trimmed().isEmpty()) return;
                    if (sourcePath != docPath_) { saveReadingState(); if (!openDocument(sourcePath)) return; }
                    renameMindGroup(groupId, name);
                });
                menu.addAction(QStringLiteral("删除分组（保留笔记）"), [this, sourcePath, groupId]() {
                    if (!confirm(this, QStringLiteral("删除分组"),
                                 QStringLiteral("删除此分组，笔记保留并回到按页分组；子分组会提升一级。可通过撤销恢复分组。是否继续？"))) return;
                    if (sourcePath != docPath_) { saveReadingState(); if (!openDocument(sourcePath)) return; }
                    deleteMindGroup(groupId);
                });
            }
        }
        if (!menu.isEmpty() && !kHeadless) menu.exec(notebookOutline_->viewport()->mapToGlobal(pos));
    });
    connect(notebookOutline_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        if (item) activateNotebookNote(item->data(0, Qt::UserRole).toString());
    });
    notebookOutline_->setDragDropOverwriteMode(false);
    connect(notebookOutline_, &QTreeWidget::itemDoubleClicked, this, [this](QTreeWidgetItem* item, int) {
        if (!item) return;
        const QString id = item->data(0, Qt::UserRole).toString();
        if (activateNotebookNote(id)) editMarkupComment(id);
    });

    // ---- notes tab: search + tag filter + colour filter + list ----
    auto* notesTab = new QWidget(leftTabs_);
    notesTab->setObjectName(QStringLiteral("excerptsPanel"));
    auto* vbox = new QVBoxLayout(notesTab);
    vbox->setContentsMargins(8, 8, 8, 8);
    vbox->setSpacing(6);

    noteSearchEdit_ = new QLineEdit(notesTab);
    noteSearchEdit_->setPlaceholderText(QStringLiteral("搜索笔记（全文 / 标签）"));
    noteSearchEdit_->setClearButtonEnabled(true);
    vbox->addWidget(noteSearchEdit_);

    auto* row = new QHBoxLayout;
    row->setSpacing(6);
    tagFilter_ = new QComboBox(notesTab);
    tagFilter_->setMinimumWidth(120);
    row->addWidget(tagFilter_, 1);

    auto* allDocsBtn = new QToolButton(notesTab);
    allDocsBtn->setText(QStringLiteral("全部文档"));
    allDocsBtn->setCheckable(true);
    allDocsBtn->setToolTip(QStringLiteral("在整个书库里搜索笔记"));
    row->addWidget(allDocsBtn);
    vbox->addLayout(row);

    auto* colorRow = new QHBoxLayout;
    colorRow->setSpacing(4);
    colorRow->addWidget(new QLabel(QStringLiteral("颜色"), notesTab));
    for (int i = 0; i < 5; ++i) {
        auto* b = new QToolButton(notesTab);
        b->setCheckable(true);
        b->setFixedSize(20, 20);
        const QColor c(QString::fromLatin1(kColors[i]));
        b->setStyleSheet(QStringLiteral("QToolButton{background:%1;border:1px solid %2;border-radius:4px;}"
                                        "QToolButton:checked{border:2px solid #1b4fa8;}")
                             .arg(c.name(), c.darker(130).name()));
        b->setProperty("hex", c.name(QColor::HexRgb));
        b->setToolTip(QStringLiteral("只看 %1").arg(c.name()));
        colorButtons_[i] = b;
        colorRow->addWidget(b);
        connect(b, &QToolButton::toggled, this, [this](bool) { updateNotesPanel(); });
    }
    auto* clearBtn = new QToolButton(notesTab);
    clearBtn->setText(QStringLiteral("清除"));
    colorRow->addWidget(clearBtn);
    colorRow->addStretch(1);
    vbox->addLayout(colorRow);
    connect(clearBtn, &QToolButton::clicked, this, [this]() {
        for (auto* b : colorButtons_)
            if (b) b->setChecked(false);
        if (tagFilter_) tagFilter_->setCurrentIndex(0);
        if (noteSearchEdit_) noteSearchEdit_->clear();
        updateNotesPanel();
    });

    notesList_ = new QListWidget(notesTab);
    notesList_->setObjectName("excerptList");
    notesList_->setItemDelegate(new ExcerptDelegate(notesList_));
    notesList_->setMouseTracking(true);
    notesList_->setAlternatingRowColors(false);
    vbox->addWidget(notesList_, 1);

    connect(noteSearchEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
        updateNotesPanel();
    });
    connect(tagFilter_, &QComboBox::currentIndexChanged, this, [this](int) { updateNotesPanel(); });
    connect(allDocsBtn, &QToolButton::toggled, this, [this](bool on) {
        searchAllDocs_ = on;
        if (allDocsBtn_)
            allDocsBtn_->setToolTip(on && !currentTopic_.isEmpty()
                                        ? QStringLiteral("搜索范围：当前阅读主题")
                                        : QStringLiteral("搜索范围：全部文档"));
        updateNotesPanel();
    });
    allDocsBtn_ = allDocsBtn;

    connect(notesList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        const QString id = item->data(Qt::UserRole).toString();
        jumpToMarkup(id);
        // Reverse direction: the mind map should follow the annotation the user picked.
        if (mindView_ && mindPanel_->isVisible()) mindView_->selectForMarkup(id);
    });
    connect(notesList_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (item) editMarkupComment(item->data(Qt::UserRole).toString());
    });
    notesList_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(notesList_, &QWidget::customContextMenuRequested, this, [this](const QPoint& pos) {
        QListWidgetItem* item = notesList_->itemAt(pos);
        if (!item) return;
        const QString id = item->data(Qt::UserRole).toString();
        QMenu menu(this);
        menu.addAction(QStringLiteral("跳转"), [this, id]() { jumpToMarkup(id); });
        menu.addAction(QStringLiteral("编辑批注…"), [this, id]() { editMarkupComment(id); });
        menu.addAction(QStringLiteral("编辑标签…"), [this, id]() { editMarkupTags(id); });
        menu.addSeparator();
        menu.addAction(QStringLiteral("删除"), [this, id]() { deleteMarkupById(id); });
        if (!kHeadless) menu.exec(notesList_->mapToGlobal(pos));
    });

    leftTabs_->addTab(notesTab, QStringLiteral("摘录"));
    vocabPanel_ = new VocabPanel(store_, leftTabs_);
    vocabPanel_->setObjectName("vocabPanel");
    connect(vocabPanel_, &VocabPanel::lookupRequested, this, &MainWindow::lookupWordForVocab);
    connect(vocabPanel_, &VocabPanel::statusMessage, this,
            [this](const QString& text) { statusBar()->showMessage(text, 5000); });
    connect(vocabPanel_, &VocabPanel::openSourceRequested, this,
            [this](const QString& path, int page) {
                if (path != docPath_) {
                    currentTopic_.clear();
                    if (!openDocument(path)) return;
                }
                view_->gotoPage(page);
                updateStatus();
            });
    leftTabs_->addTab(vocabPanel_, QStringLiteral("生词"));
    QWidget* mapTab = new QWidget(leftTabs_);
    mapTab->setObjectName(QStringLiteral("sideMindPanel"));
    auto* mapTabLayout = new QVBoxLayout(mapTab);
    mapTabLayout->setContentsMargins(0, 0, 0, 0);
    auto* mapActions = new QHBoxLayout;
    auto* mapFit = new QPushButton(QStringLiteral("适配"), mapTab);
    auto* mapOpen = new QPushButton(QStringLiteral("独立窗口 ↗"), mapTab);
    mapActions->addWidget(mapFit); mapActions->addWidget(mapOpen); mapActions->addStretch();
    mapTabLayout->addLayout(mapActions);
    auto* rightMindView = new mind::MindMapView(mapTab);
    rightMindView->setObjectName("rightMindMap");
    shelfMindView_ = rightMindView;
    mapTabLayout->addWidget(rightMindView, 1);
    connect(mapFit, &QPushButton::clicked, rightMindView, &mind::MindMapView::fitToContent);
    connect(mapOpen, &QPushButton::clicked, this, &MainWindow::showNotebookMindMap);
    leftTabs_->addTab(mapTab, QStringLiteral("脑图"));
    // ReaderNote/MN order: excerpts first, then the note outline, words and the map.
    // The page-outline tree is constructed before the excerpt list, so move it behind.
    leftTabs_->tabBar()->moveTab(leftTabs_->indexOf(notesTab), 0);
    // moveTab keeps the previously current tab; excerpts should be the default view.
    leftTabs_->setCurrentWidget(notesTab);

    dock->setWidget(leftTabs_);
    dock->setMinimumWidth(300);
    addDockWidget(Qt::RightDockWidgetArea, dock);
    resizeDocks({shelf, dock}, {220, 360}, Qt::Horizontal);
}

void MainWindow::buildStatusBar()
{
    infoLabel_ = new QLabel;
    statusBar()->addPermanentWidget(infoLabel_);
    statusBar()->showMessage(
        QStringLiteral("拖选文字：浮出高亮/下划线/批注/查词/翻译/AI/OCR；"
                       "Ctrl+F 查找；Ctrl+Z 撤销；Ctrl+1/2/3 切换阅读/整理/脑图"));
    updateStatus();
}

// ---------------------------------------------------------------- document

bool MainWindow::openDocument(const QString& path)
{
    if (path.isEmpty()) return false;
    QString err;
    if (!engine_.open(path, &err)) {
        warnUser(this, QStringLiteral("打开失败"), err);
        return false;
    }
    docPath_ = QFileInfo(path).absoluteFilePath();
    store_.touchDocument(docPath_, QFileInfo(path).completeBaseName(), engine_.pageCount());

    view_->setEngine(&engine_);
    reloadOutline();
    rebuildPageThumbnails();
    reloadMarkups();
    refreshBookshelf();
    hits_.clear();
    hitIndex_ = -1;
    view_->setSearchHits({}, -1);
    if (findLabel_) findLabel_->setText(QStringLiteral("0/0"));

    setWindowTitle(QStringLiteral("AdaptNote — %1").arg(QFileInfo(path).fileName()));
    if (engine_.supportsAnchors()) {
        relocateAnchors();
    } else if (documentFileChanged()) {
        migrateAfterRevision();
    }
    store_.setDocumentFileIdentity(docPath_, QFileInfo(path).size(),
                                   QFileInfo(path).lastModified().toMSecsSinceEpoch());
    restoreReadingState();

    // Offer to pull in annotations that are already inside the PDF (once per revision).
    const int pending = embeddedPrompt_ ? pendingEmbeddedAnnotations() : 0;
    if (pending > 0) {
        store_.setSetting(identityKey(docPath_), QStringLiteral("asked"));
        if (confirm(this, QStringLiteral("发现内嵌标注"),
                    QStringLiteral("这个 PDF 里已经有 %1 条标注（可能来自其他阅读器）。\n"
                                   "要把它们读进来变成笔记吗？")
                        .arg(pending)))
            importEmbeddedAnnotations();
    }
    updateStatus();
    refreshNotebookWorkspace();
    return true;
}

int MainWindow::outlineRootCountForTest() const
{
    return notebookOutline_ ? notebookOutline_->topLevelItemCount() : 0;
}

int MainWindow::notebookDocumentCountForTest() const
{
    return notebookDocuments_ ? notebookDocuments_->count() : 0;
}

QStringList MainWindow::notebookNoteIdsForTest() const
{
    QStringList ids;
    if (!notebookOutline_) return ids;
    std::function<void(QTreeWidgetItem*)> visit = [&](QTreeWidgetItem* item) {
        const QString id = item->data(0, Qt::UserRole).toString();
        if (!id.isEmpty()) ids.append(id);
        for (int i = 0; i < item->childCount(); ++i) visit(item->child(i));
    };
    for (int i = 0; i < notebookOutline_->topLevelItemCount(); ++i)
        visit(notebookOutline_->topLevelItem(i));
    return ids;
}

bool MainWindow::moveNotebookGroup(const QString& id, const QString& targetPath, const QString& parentId)
{
    if (id.isEmpty() || targetPath.isEmpty() || id == parentId) return false;
    const QStringList paths = currentTopic_.isEmpty() ? QStringList{docPath_} : currentTopicDocPaths();
    if (!paths.contains(targetPath) && QFileInfo(targetPath).absoluteFilePath() != docPath_) return false;
    MindGroup before;
    bool validParent = parentId.isEmpty();
    for (const MindGroup& group : store_.mindGroups(targetPath)) {
        if (group.id == id) before = group;
        if (group.id == parentId) validParent = true;
    }
    if (before.id.isEmpty() || !validParent || store_.wouldCycle(id, parentId)) return false;
    if (before.parentId == parentId) return false;
    if (!store_.setMindGroupParent(id, parentId)) return false;
    for (const MindGroup& after : store_.mindGroups(targetPath)) {
        if (after.id != id) continue;
        undo_->push(new MindGroupCommand(this, MindGroupCommand::Modify, before, after,
                                         QStringLiteral("调整大纲层级")));
        return true;
    }
    return false;
}

bool MainWindow::moveNotebookNote(const QString& id, const QString& targetPath,
                                  const QString& groupKey, const QString& beforeId,
                                  const QString& parentMarkupId, bool nestUnderParent)
{
    if (id.isEmpty() || id == beforeId || id == parentMarkupId || targetPath.isEmpty()) return false;
    if (nestUnderParent && !parentMarkupId.isEmpty()) {
        if (parentMarkupId == id) return false;
        const QStringList paths = currentTopic_.isEmpty() ? QStringList{docPath_} : currentTopicDocPaths();
        Markup child, parent; bool haveChild = false, haveParent = false;
        for (const QString& path : paths) for (const Markup& note : store_.markups(path)) {
            if (note.id == id) { child = note; haveChild = true; }
            if (note.id == parentMarkupId) { parent = note; haveParent = true; }
        }
        if (!haveChild || !haveParent || child.docPath != parent.docPath ||
            child.mindGroup != parent.mindGroup || (child.mindGroup.isEmpty() && child.page != parent.page)) return false;
        QString cursor = parentMarkupId;
        while (!cursor.isEmpty()) {
            if (cursor == id) return false;
            QString next;
            for (const Markup& note : store_.markups(child.docPath)) if (note.id == cursor) next = note.parentMarkupId;
            cursor = next;
        }
        if (child.parentMarkupId == parentMarkupId) return false;
        if (child.docPath != docPath_) { saveReadingState(); if (!openDocument(child.docPath)) return false; }
        Markup after = child; after.parentMarkupId = parentMarkupId; after.mindOrder = 0;
        after.updated = QDateTime::currentMSecsSinceEpoch();
        undo_->push(new MarkupCommand(this, CmdKind::Modify, child, after, QStringLiteral("建立筆记层级")));
        return true;
    }
    if (!beforeId.isEmpty()) {
        const QStringList paths = currentTopic_.isEmpty() ? QStringList{docPath_} : currentTopicDocPaths();
        Markup child, target; bool haveChild = false, haveTarget = false;
        for (const QString& path : paths) for (const Markup& note : store_.markups(path)) {
            if (note.id == id) { child = note; haveChild = true; }
            if (note.id == beforeId) { target = note; haveTarget = true; }
        }
        if (!haveChild || !haveTarget || child.docPath != target.docPath || id == beforeId ||
            target.parentMarkupId != parentMarkupId || child.parentMarkupId != parentMarkupId ||
            child.mindGroup != target.mindGroup || (child.mindGroup.isEmpty() && child.page != target.page)) return false;
        if (child.docPath != docPath_) { saveReadingState(); if (!openDocument(child.docPath)) return false; }
        QVector<Markup> siblings;
        for (const Markup& note : markups_)
            if (!note.deleted && note.parentMarkupId == parentMarkupId && note.mindGroup == child.mindGroup &&
                (note.mindGroup.isEmpty() ? note.page == child.page : true)) siblings.append(note);
        const auto plan = mind::planReorder(siblings, id, beforeId);
        QVector<QPair<Markup, Markup>> pairs;
        for (const auto& step : plan) {
            Markup* current = findMarkup(step.first);
            if (!current || current->mindOrder == step.second) continue;
            Markup after = *current; after.mindOrder = step.second; after.updated = QDateTime::currentMSecsSinceEpoch();
            pairs.append({*current, after});
        }
        if (pairs.isEmpty()) return false;
        undo_->push(new MultiMarkupCommand(this, pairs, QStringLiteral("调整笔记同级顺序")));
        return true;
    }
    // Page buckets are views of source locations, not movable destinations.
    if (!groupKey.startsWith(QStringLiteral("group:"))) return false;
    QString sourcePath;
    const QStringList paths = currentTopic_.isEmpty() ? QStringList{docPath_} : currentTopicDocPaths();
    for (const QString& path : paths)
        for (const Markup& note : store_.markups(path)) if (note.id == id) sourcePath = path;
    if (sourcePath.isEmpty() || QFileInfo(sourcePath).absoluteFilePath() != QFileInfo(targetPath).absoluteFilePath()) {
        statusBar()->showMessage(QStringLiteral("不能跨资料移动原文笔记；请在所属资料内整理"), 3500);
        return false;
    }
    bool validGroup = false;
    for (const MindGroup& group : store_.mindGroups(sourcePath))
        if (QStringLiteral("group:") + group.id == groupKey) validGroup = true;
    if (!validGroup) return false;
    if (!beforeId.isEmpty()) {
        bool validSibling = false;
        for (const Markup& note : store_.markups(sourcePath))
            if (note.id == beforeId && QStringLiteral("group:") + note.mindGroup == groupKey)
                validSibling = true;
        if (!validSibling) return false;
    }
    if (sourcePath != docPath_) { saveReadingState(); if (!openDocument(sourcePath)) return false; }
    return applyMindReorder(id, groupKey, beforeId);
}

bool MainWindow::activateNotebookNote(const QString& id)
{
    if (id.isEmpty()) return false;
    const QStringList paths = currentTopic_.isEmpty() ? QStringList{docPath_} : currentTopicDocPaths();
    for (const QString& path : paths) {
        for (const Markup& m : store_.markups(path)) {
            if (m.id != id) continue;
            if (path != docPath_) {
                saveReadingState();
                if (!openDocument(path)) return false;
            }
            jumpToMarkup(id);
            return true;
        }
    }
    return false;
}

QString MainWindow::createNotebook(const QString& name)
{
    if (name.trimmed().isEmpty()) return {};
    Topic topic = store_.stampTopicForLocalWrite(Topic{});
    topic.name = name.trimmed();
    if (!store_.upsertTopic(topic)) return {};
    return openNotebook(topic.id) ? topic.id : QString();
}

bool MainWindow::createFreeNoteForTest(const QString& title, const QString& comment)
{
    return createFreeNoteForPageForTest(title, comment, view_ ? view_->currentPage() : 0);
}

bool MainWindow::createFreeNoteForPageForTest(const QString& title, const QString& comment, int page)
{
    if (docPath_.isEmpty() || title.trimmed().isEmpty()) return false;
    Markup note;
    note.id = newId();
    note.docPath = docPath_;
    note.page = qMax(0, page);
    note.type = MarkupType::Note;
    note.title = title.trimmed();
    note.comment = comment;
    note.created = QDateTime::currentMSecsSinceEpoch();
    note.updated = note.created;
    undo_->push(new MarkupCommand(this, CmdKind::Add, Markup(), note, QStringLiteral("新建自由笔记")));
    return true;
}

bool MainWindow::addNotebookDocument(const QString& path)
{
    if (currentTopic_.isEmpty() || !QFileInfo(path).isFile()) return false;
    saveReadingState();
    if (!openDocument(path)) return false;
    if (!store_.addTopicDoc(currentTopic_, docPath_)) return false;
    store_.setMindGroupTopic(docPath_, currentTopic_);
    setWorkspaceMode(0);
    view_->show();
    refreshNotebookWorkspace();
    return true;
}

bool MainWindow::openNotebook(const QString& topicId)
{
    bool exists = false;
    for (const Topic& topic : store_.topics()) if (topic.id == topicId) exists = true;
    if (!exists) return false;
    const auto docs = store_.topicDocs(topicId);
    if (docs.isEmpty()) {
        saveReadingState();
        currentTopic_ = topicId;
        setWorkspaceMode(0);
        view_->hide(); // do not show the previous notebook's document in an empty notebook
        refreshNotebookWorkspace();
        statusBar()->showMessage(QStringLiteral("笔记本尚无资料，请点击「添加资料」"), 5000);
        return true;
    }
    saveReadingState();
    const QString previous = currentTopic_;
    currentTopic_ = topicId;
    if (!openDocument(docs.first().docPath)) {
        currentTopic_ = previous;
        refreshNotebookWorkspace();
        return false;
    }
    setWorkspaceMode(0);
    refreshNotebookWorkspace();
    return true;
}

void MainWindow::refreshNotebookWorkspace()
{
    if (!notebookDocuments_ || !notebookOutline_) return;
    const QSignalBlocker tabsBlocked(notebookDocuments_);
    if (!currentTopic_.isEmpty() && notebookTabs_) {
        const QSignalBlocker block(notebookTabs_);
        int index = -1;
        for (int i = 1; i < notebookTabs_->count(); ++i)
            if (notebookTabs_->tabData(i).toString() == currentTopic_) index = i;
        QString name;
        for (const Topic& topic : store_.topics()) if (topic.id == currentTopic_) name = topic.name;
        if (index < 0) { index = notebookTabs_->addTab(name); notebookTabs_->setTabData(index, currentTopic_); }
        notebookTabs_->setTabText(index, name);
        notebookTabs_->setCurrentIndex(index);
    }
    while (notebookDocuments_->count()) notebookDocuments_->removeTab(0);
    QString title = QStringLiteral("文档阅读");
    QVector<mind::TopicDocInput> inputs;
    QVector<MindGroup> groups;
    QStringList paths;
    if (!currentTopic_.isEmpty()) {
        for (const Topic& t : store_.topics()) if (t.id == currentTopic_) title = t.name;
        for (const TopicDoc& d : store_.topicDocs(currentTopic_)) paths.append(d.docPath);
        // Older libraries may have topic_docs entries created before group ownership was
        // backfilled. Reconcile via the explicit document link, never by displayed title.
        for (const QString& path : paths) store_.syncDocumentGroupTopic(path);
        groups = store_.mindGroupsForTopic(currentTopic_);
    } else if (!docPath_.isEmpty()) {
        paths.append(docPath_);
        title = QFileInfo(docPath_).completeBaseName();
        groups = store_.mindGroups(docPath_);
    }
    for (const QString& storedPath : paths) {
        const QString path = storedPath.startsWith(QStringLiteral("unresolved:"))
                                 ? storedPath : QFileInfo(storedPath).absoluteFilePath();
        const int tab = notebookDocuments_->addTab(QFileInfo(path).fileName());
        notebookDocuments_->setTabData(tab, path);
        notebookDocuments_->setTabToolTip(tab, path);
        if (path == docPath_) notebookDocuments_->setCurrentIndex(tab);
        mind::TopicDocInput input;
        input.title = QFileInfo(path).completeBaseName();
        input.docPath = path;
        input.markups = store_.markups(path);
        inputs.append(input);
    }
    const QString headerText = currentTopic_.isEmpty()
        ? QStringLiteral("文档 · %1").arg(title)
        : QStringLiteral("%1    ·    %2 份资料").arg(title).arg(paths.size());
    notebookTitle_->setText(headerText);
    if (auto* name = findChild<QLabel*>(QStringLiteral("currentNotebookTitle"))) name->setText(headerText);
    if (bookshelfDock_) bookshelfDock_->setWindowTitle(currentTopic_.isEmpty()
        ? QStringLiteral("书架 · %1").arg(title) : QStringLiteral("书架 · %1").arg(title));
    QSet<QString> collapsedKeys;
    QString selectedKey;
    const bool hadTree = notebookOutline_->topLevelItemCount() > 0;
    std::function<void(QTreeWidgetItem*)> remember = [&](QTreeWidgetItem* item) {
        const QString key = item->data(0, Qt::UserRole + 1).toString();
        if (!item->isExpanded()) collapsedKeys.insert(key);
        if (item == notebookOutline_->currentItem()) selectedKey = key;
        for (int i = 0; i < item->childCount(); ++i) remember(item->child(i));
    };
    for (int i = 0; i < notebookOutline_->topLevelItemCount(); ++i)
        remember(notebookOutline_->topLevelItem(i));
    notebookOutline_->clear();
    const mind::Node root = currentTopic_.isEmpty() && !inputs.isEmpty()
                                ? mind::buildFromNotes(title, inputs.first().markups, groups, !engine_.outline().isEmpty(), inputs.first().docPath)
                                : mind::buildFromTopic(title, inputs, groups);
    QHash<QString, QString> groupPaths;
    for (const MindGroup& group : groups) groupPaths.insert(QStringLiteral("group:") + group.id, group.docPath);
    std::function<void(QTreeWidgetItem*, const mind::Node&)> fill =
        [&](QTreeWidgetItem* parent, const mind::Node& node) {
            auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(notebookOutline_);
            item->setText(0, node.text);
            QString preview = node.comment.simplified();
            if (preview.isEmpty()) preview = node.excerpt.simplified();
            item->setData(0, Qt::UserRole + 6, preview);
            item->setData(0, Qt::UserRole + 7, node.source);
            item->setData(0, Qt::UserRole + 8, node.isGroup);
            if (node.isGroup) {
                item->setIcon(0, glyphIcon("organize", QColor("#8bb8ff")));
                QFont font = item->font(0); font.setWeight(QFont::DemiBold); item->setFont(0, font);
                item->setForeground(0, QBrush(QColor("#d5dae3")));
            } else if (!node.markupId.isEmpty()) {
                item->setIcon(0, glyphIcon("note", QColor("#b3c7e6")));
            }
            item->setData(0, Qt::UserRole, node.markupId);
            item->setData(0, Qt::UserRole + 4, node.groupKey);
            item->setData(0, Qt::UserRole + 5, node.parentMarkupId);
            if (node.markupId.isEmpty() && groupPaths.contains(node.groupKey)) {
                item->setData(0, Qt::UserRole + 2, node.groupKey.mid(6));
                item->setData(0, Qt::UserRole + 3, groupPaths.value(node.groupKey));
            } else if (parent) {
                const QString source = node.docPath.isEmpty()
                                           ? parent->data(0, Qt::UserRole + 3).toString() : node.docPath;
                item->setData(0, Qt::UserRole + 3, source);
            }
            const QString key = parent ? parent->data(0, Qt::UserRole + 1).toString() + QLatin1Char('/') +
                                           (node.markupId.isEmpty() ? (node.docPath.isEmpty() ? node.groupKey + node.text : node.docPath) : node.markupId)
                                       : mindScopeKey();
            item->setData(0, Qt::UserRole + 1, key);
            item->setExpanded(!hadTree || !collapsedKeys.contains(key));
            if (key == selectedKey) notebookOutline_->setCurrentItem(item);
            item->setToolTip(0, node.excerpt + QStringLiteral("\n") + node.comment);
            for (const auto& child : node.children) fill(item, child);
        };
    fill(nullptr, root);
    refreshBookshelf();
    if (notebookMindView_) {
        notebookMindView_->setLinks(store_.mindLinks(mindScopeKey()));
        notebookMindView_->setTree(root);
    }

}

void MainWindow::restoreReadingState()
{
    const QString key = QStringLiteral("reader.zoom.") + docPath_;
    bool validZoom = false;
    const float savedZoom = store_.setting(key).toFloat(&validZoom);
    if (validZoom && savedZoom > 0) view_->setZoom(savedZoom);
    const int page = store_.lastPage(docPath_);
    const double frac = store_.lastOffset(docPath_);
    view_->gotoPage(page);
    QTimer::singleShot(0, this, [this, page, frac]() {
        view_->scrollToPageFraction(page, frac);
        updateStatus();
    });
}

void MainWindow::saveReadingState()
{
    if (!engine_.isOpen() || docPath_.isEmpty()) return;
    const int page = view_->currentPage();
    store_.setReadingState(docPath_, page, view_->pageOffsetFraction(page));
    store_.setSetting(QStringLiteral("reader.zoom.") + docPath_, QString::number(view_->zoom()));
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    saveReadingState();
    QJsonArray tabs;
    for (int i = 1; notebookTabs_ && i < notebookTabs_->count(); ++i)
        tabs.append(notebookTabs_->tabData(i).toString());
    store_.setSetting(QStringLiteral("workspace.openNotebooks"),
                      QString::fromUtf8(QJsonDocument(tabs).toJson(QJsonDocument::Compact)));
    store_.setSetting(QStringLiteral("workspace.activeNotebook"), currentTopic_);
    QMainWindow::closeEvent(e);
}

void MainWindow::reloadOutline()
{
    outlineTree_->clear();

    std::function<void(QTreeWidgetItem*, const QVector<OutlineItem>&)> fill =
        [&](QTreeWidgetItem* parent, const QVector<OutlineItem>& items) {
            for (const OutlineItem& it : items) {
                auto* w = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(outlineTree_);
                w->setText(0, it.title.isEmpty() ? QStringLiteral("(未命名)") : it.title);
                w->setData(0, Qt::UserRole, it.page);
                fill(w, it.children);
            }
        };
    fill(nullptr, engine_.outline());
    outlineTree_->expandToDepth(0);

    if (outlineTree_->topLevelItemCount() == 0) {
        auto* w = new QTreeWidgetItem(outlineTree_);
        w->setText(0, QStringLiteral("（该文档没有目录）"));
        w->setData(0, Qt::UserRole, -1);
        w->setFlags(Qt::NoItemFlags);
    }
}

void MainWindow::rebuildPageThumbnails()
{
    if (!pageThumbs_) return;
    if (thumbTimer_) thumbTimer_->stop();
    pageThumbs_->clear();
    thumbNext_ = 0;
    thumbDocPath_ = docPath_;
    if (!engine_.isOpen()) return;

    constexpr int kMaxThumbs = 500;
    const int pages = qMin(engine_.pageCount(), kMaxThumbs);
    pageThumbs_->setUpdatesEnabled(false);
    for (int p = 0; p < pages; ++p) {
        auto* item = new QListWidgetItem(pageThumbs_);
        item->setText(QString::number(p + 1));
        item->setData(Qt::UserRole, p);
        item->setSizeHint(QSize(116, 168));
        item->setTextAlignment(Qt::AlignHCenter | Qt::AlignBottom);
        item->setToolTip(QStringLiteral("第 %1 页").arg(p + 1));
    }
    if (engine_.pageCount() > kMaxThumbs) {
        auto* more = new QListWidgetItem(pageThumbs_);
        more->setText(QStringLiteral("…"));
        more->setData(Qt::UserRole, -1);
        more->setFlags(Qt::NoItemFlags);
        more->setToolTip(QStringLiteral("仅渲染前 %1 页").arg(kMaxThumbs));
    }
    pageThumbs_->setUpdatesEnabled(true);
    if (view_ && view_->currentPage() < pages) pageThumbs_->setCurrentRow(view_->currentPage());
    if (thumbTimer_) thumbTimer_->start();
}

void MainWindow::pumpPageThumbnails()
{
    if (!pageThumbs_ || !thumbTimer_ || !engine_.isOpen()) return;
    if (thumbDocPath_ != docPath_) {
        thumbTimer_->stop();
        return;
    }
    constexpr int kBatch = 6;
    int rendered = 0;
    const int total = pageThumbs_->count();
    while (thumbNext_ < total && rendered < kBatch) {
        QListWidgetItem* item = pageThumbs_->item(thumbNext_);
        const int page = item->data(Qt::UserRole).toInt();
        if (page >= 0 && page < engine_.pageCount()) {
            const QSizeF pt = engine_.pageSize(page);
            if (pt.width() > 0) {
                const float scale = 108.0f / static_cast<float>(pt.width());
                QImage image = engine_.renderPage(page, scale, 0);
                if (!image.isNull()) {
                    QPixmap pixmap = QPixmap::fromImage(image);
                    if (pixmap.width() > 108)
                        pixmap = pixmap.scaledToWidth(108, Qt::SmoothTransformation);
                    item->setIcon(QIcon(pixmap));
                }
            }
        }
        ++thumbNext_;
        ++rendered;
    }
    if (thumbNext_ >= total) thumbTimer_->stop();
}

void MainWindow::reloadMarkups()
{
    markups_ = store_.markups(docPath_);
    view_->setMarkups(markups_);
    updateFilterWidgets();
    updateNotesPanel();
    updateStatus();
    refreshNotebookWorkspace();
    // The panel shows the same notes the list shows, so it has to follow every change.
    if (mindPanel_ && mindPanel_->isVisible()) rebuildMindMap();
}

void MainWindow::refreshFromStore()
{
    reloadMarkups();
}

void MainWindow::storeUpsertMarkup(const Markup& m)
{
    QString err;
    if (!store_.upsertMarkup(m, &err))
        statusBar()->showMessage(QStringLiteral("保存失败：%1").arg(err), 4000);
}

void MainWindow::storeUpsertStampedMarkup(const Markup& m)
{
    storeUpsertMarkup(store_.stampForLocalWrite(m));
}

void MainWindow::storeRemoveMarkup(const QString& id)
{
    store_.removeMarkup(id);
}

Markup* MainWindow::findMarkup(const QString& id)
{
    for (Markup& m : markups_)
        if (m.id == id) return &m;
    return nullptr;
}

const Markup* MainWindow::findMarkup(const QString& id) const
{
    for (const Markup& m : markups_)
        if (m.id == id) return &m;
    return nullptr;
}

bool MainWindow::tagPasses(const Markup& m) const
{
    if (!tagFilter_) return true;
    const QString tag = tagFilter_->currentData().toString();
    return tag.isEmpty() || m.tags.contains(tag);
}

bool MainWindow::colorPasses(const Markup& m) const
{
    bool anyChecked = false;
    for (auto* b : colorButtons_) {
        if (!b || !b->isChecked()) continue;
        anyChecked = true;
        if (b->property("hex").toString() == m.color.name(QColor::HexRgb)) return true;
    }
    return !anyChecked;
}

void MainWindow::updateFilterWidgets()
{
    if (!tagFilter_) return;
    const QString keep = tagFilter_->currentData().toString();
    QSignalBlocker b(tagFilter_);
    tagFilter_->clear();
    tagFilter_->addItem(QStringLiteral("全部标签"), QString());
    for (const auto& t : store_.tagCounts(docPath_))
        tagFilter_->addItem(QStringLiteral("%1 (%2)").arg(t.first).arg(t.second), t.first);
    const int idx = tagFilter_->findData(keep);
    tagFilter_->setCurrentIndex(idx >= 0 ? idx : 0);
}

QVector<Markup> MainWindow::notesShown() const
{
    return shown_.values().toVector();
}

QStringList MainWindow::availableTags() const
{
    QStringList out;
    for (const auto& t : store_.tagCounts(docPath_)) out << t.first;
    return out;
}

void MainWindow::setFindScopeForTest(int scope)
{
    if (findScope_) findScope_->setCurrentIndex(scope);
}

int MainWindow::notesPanelCountForTest() const
{
    return notesList_ ? notesList_->count() : 0;
}

QString MainWindow::notesPanelTextForTest(int row) const
{
    if (!notesList_ || row < 0 || row >= notesList_->count()) return {};
    return notesList_->item(row)->text();
}

QStringList MainWindow::currentTopicDocPaths() const
{
    QStringList out;
    if (currentTopic_.isEmpty()) return out;
    for (const TopicDoc& d : store_.topicDocs(currentTopic_)) out.append(d.docPath);
    return out;
}

void MainWindow::updateNotesPanel()
{
    if (!notesList_) return;
    // Tab 0 is the excerpt list after the reorder in buildDocks; tab 1 is the outline.
    if (currentTopic_.isEmpty()) {
        if (leftTabs_) leftTabs_->setTabText(1, QStringLiteral("笔记"));
    } else if (leftTabs_) {
        leftTabs_->setTabText(1, QStringLiteral("笔记本大纲"));
    }
    const QString keepId = notesList_->currentItem()
                               ? notesList_->currentItem()->data(Qt::UserRole).toString()
                               : QString();
    notesList_->clear();
    shown_.clear();

    const QString query = noteSearchEdit_ ? noteSearchEdit_->text().trimmed() : QString();
    QVector<Markup> base;
    if (searchAllDocs_) {
        // "All documents" means the reading topic when one is open: searching every book on
        // the machine is rarely what the reader wants, and it hides the topic boundary.
        const QStringList topicPaths = currentTopicDocPaths();
        if (!topicPaths.isEmpty())
            base = store_.searchMarkupsInDocs(query, topicPaths);
        else
            base = store_.searchMarkups(query, QString());
    } else {
        base = query.isEmpty() ? markups_ : store_.searchMarkups(query, docPath_);
    }

    int row = -1, keepRow = -1;
    for (const Markup& m : base) {
        if (!tagPasses(m) || !colorPasses(m)) continue;
        shown_.insert(m.id, m);

        QString text;
        if (searchAllDocs_)
            text += QStringLiteral("[%1] ").arg(QFileInfo(m.docPath).completeBaseName());
        text += QStringLiteral("P%1 · %2 · %3").arg(m.page + 1).arg(markupTypeLabel(m.type),
                                                                excerptOf(m));
        if (!m.tags.isEmpty())
            text += QStringLiteral("  #%1").arg(m.tags.join(QStringLiteral(" #")));
        if (!m.comment.isEmpty())
            text += QStringLiteral("  〔%1〕").arg(m.comment.simplified().left(24));

        auto* item = new QListWidgetItem(text);
        item->setIcon(swatchIcon(m.color));
        item->setData(Qt::UserRole, m.id);
        item->setData(Qt::UserRole + 1, m.page + 1);
        item->setData(Qt::UserRole + 2, markupTypeLabel(m.type));
        const QString quote = !m.text.trimmed().isEmpty()
                                  ? m.text.simplified()
                                  : (!m.title.trimmed().isEmpty()
                                         ? m.title.simplified()
                                         : (m.type == MarkupType::Image
                                                ? QStringLiteral("图片摘录")
                                                : (m.type == MarkupType::Ink
                                                       ? QStringLiteral("手写标注")
                                                       : m.comment.simplified())));
        item->setData(Qt::UserRole + 3, quote);
        item->setData(Qt::UserRole + 4, m.comment.simplified());
        item->setData(Qt::UserRole + 5, m.tags.isEmpty()
                                            ? QString()
                                            : QStringLiteral("#") + m.tags.join(QStringLiteral(" #")));
        item->setData(Qt::UserRole + 6, m.color);
        item->setData(Qt::UserRole + 7, searchAllDocs_ ? QFileInfo(m.docPath).completeBaseName() : QString());
        item->setToolTip(QStringLiteral("%1\n%2\n标签：%3")
                             .arg(excerptOf(m, 200), m.comment,
                                  m.tags.isEmpty() ? QStringLiteral("—") : m.tags.join(", ")));
        notesList_->addItem(item);
        ++row;
        if (m.id == keepId) keepRow = row;
    }

    if (leftTabs_) leftTabs_->setTabText(0, QStringLiteral("摘录 (%1)").arg(shown_.size()));
    if (keepRow >= 0) notesList_->setCurrentRow(keepRow);
}

void MainWindow::updateStatus()
{
    const int total = engine_.isOpen() ? engine_.pageCount() : 0;
    const int page = view_->currentPage();
    if (pageLabel_) pageLabel_->setText(QStringLiteral(" %1 / %2 ").arg(page + 1).arg(total));

    if (infoLabel_) {
        QString hint;
        const Selection sel = view_->selection();
        if (sel.isValid())
            hint = QStringLiteral("已选 %1 字符（第 %2 页）").arg(sel.text.size()).arg(sel.page + 1);
        else if (view_->captureMode())
            hint = QStringLiteral("框选中…（Esc 取消）");
        else if (!hits_.isEmpty())
            hint = QStringLiteral("查找命中 %1 处").arg(hits_.size());
        infoLabel_->setText(QStringLiteral("缩放 %1%   %2").arg(qRound(view_->zoom() * 100)).arg(hint));
    }
}

void MainWindow::changePage(int delta)
{
    if (!engine_.isOpen()) return;
    view_->gotoPage(view_->currentPage() + delta);
    updateStatus();
}

void MainWindow::chooseFile()
{
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("打开书籍"), QString(),
        QStringLiteral("所有支持的文件 (*.pdf *.epub *.xps *.cbz *.fb2);;PDF (*.pdf);;EPUB (*.epub)"));
    if (!path.isEmpty()) openDocument(path);
}

// ---------------------------------------------------------------- markups

void MainWindow::addMarkupFromSelection(MarkupType type)
{
    const Selection sel = view_->selection();
    if (!sel.isValid()) {
        statusBar()->showMessage(QStringLiteral("请先拖选文字"), 2500);
        return;
    }
    Markup m;
    m.id = newId();
    m.docPath = docPath_;
    m.page = sel.page;
    m.type = type;
    m.color = markupColor_;
    m.quads = sel.quads;
    m.text = sel.text;
    m.created = QDateTime::currentMSecsSinceEpoch();
    m.updated = m.created;

    // reflowable documents: remember where the text is, not just which page
    if (engine_.supportsAnchors() && sel.startChar >= 0) {
        const int chapter = engine_.chapterOfPage(sel.page);
        const int offset = engine_.chapterOffsetOf(sel.page, sel.startChar);
        if (chapter >= 0 && offset >= 0) {
            m.anchorChapter = QString::number(chapter);
            m.anchorOffset = offset;
        }
    }

    if (type == MarkupType::Note) {
        m.comment = askText(this, QStringLiteral("批注"), QStringLiteral("批注内容："));
        if (m.comment.isNull()) return;
    }

    undo_->push(new MarkupCommand(this, CmdKind::Add, Markup(), m, QStringLiteral("添加%1").arg(markupTypeLabel(type))));
    view_->clearSelection();
    statusBar()->showMessage(QStringLiteral("已添加%1（共 %2 条）").arg(markupTypeLabel(type)).arg(markups_.size()), 2500);
}

void MainWindow::deleteMarkupById(const QString& id)
{
    if (id.isEmpty()) return;
    Markup before;
    bool found = false;
    if (const Markup* m = findMarkup(id)) {
        before = *m;
        found = true;
    } else if (shown_.contains(id)) {
        before = shown_.value(id);
        found = true;
    }
    if (!found) return;
    undo_->push(new MarkupCommand(this, CmdKind::Remove, before, Markup(), QStringLiteral("删除标注")));
    statusBar()->showMessage(QStringLiteral("已删除 1 条标注"), 2000);
}

void MainWindow::editMarkupComment(const QString& id)
{
    Markup* m = findMarkup(id);
    if (!m && shown_.contains(id)) {
        // markups of another document are read-only from here
        statusBar()->showMessage(QStringLiteral("请先打开该文档再编辑"), 2500);
        return;
    }
    if (!m) return;
    if (kHeadless) {
        statusBar()->showMessage(QStringLiteral("正在运行无窗口测试，编辑窗口不打开"), 2500);
        return;
    }
    // Modeless: keep the source visible while editing.
    auto* editor = new QDialog(this);
    editor->setAttribute(Qt::WA_DeleteOnClose);
    editor->setObjectName("captureEditor");
    editor->setWindowTitle(QStringLiteral("摘录与个人批注"));
    editor->resize(580, 520);
    auto* layout = new QVBoxLayout(editor);
            auto* source = new QLabel(m->text.isEmpty()
                                   ? QStringLiteral("自由笔记 · 无原文来源")
                                   : QStringLiteral("%1 · 第 %2 页").arg(QFileInfo(m->docPath).fileName()).arg(m->page + 1), editor);
    source->setWordWrap(true);
    layout->addWidget(source);
    layout->addWidget(new QLabel(QStringLiteral("我的标题"), editor));
    auto* heading = new QLineEdit(m->title, editor);
    heading->setObjectName("captureTitle");
    heading->setPlaceholderText(QStringLiteral("可选，用自己的话概括这条笔记"));
    layout->addWidget(heading);
    layout->addWidget(new QLabel(QStringLiteral("来源原文（只读）"), editor));
    auto* quote = new QPlainTextEdit(m->text, editor);
    quote->setObjectName("captureQuote");
    quote->setReadOnly(true);
    if (m->text.isEmpty()) quote->setPlaceholderText(QStringLiteral("这是一条自由笔记，没有原文摘录。"));
    layout->addWidget(quote, 1);
    layout->addWidget(new QLabel(QStringLiteral("我的批注"), editor));
    auto* comment = new QPlainTextEdit(m->comment, editor);
    comment->setObjectName("captureComment");
    comment->setPlaceholderText(QStringLiteral("写下你的理解、疑问或联系……"));
    layout->addWidget(comment, 2);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, editor);
    layout->addWidget(buttons);
    connect(buttons, &QDialogButtonBox::rejected, editor, &QDialog::reject);
    const QString sourcePath = m->docPath;
    const QString originalComment = m->comment;
    const QString originalTitle = m->title;
    connect(buttons, &QDialogButtonBox::accepted, editor,
            [this, editor, comment, heading, id, sourcePath, originalComment, originalTitle]() {
        // Do not write a snapshot over edits made while this modeless editor was open.
        Markup* current = findMarkup(id);
        if (!current || current->docPath != sourcePath || current->comment != originalComment || current->title != originalTitle) {
            statusBar()->showMessage(QStringLiteral("资料或笔记已变化，请关闭编辑器后重新打开；未覆盖原内容"), 5000);
            return;
        }
        if (current->comment != comment->toPlainText() || current->title != heading->text().trimmed()) {
            Markup before = *current;
            Markup after = before;
            after.comment = comment->toPlainText();
            after.title = heading->text().trimmed();
            after.updated = QDateTime::currentMSecsSinceEpoch();
            undo_->push(new MarkupCommand(this, CmdKind::Modify, before, after, QStringLiteral("编辑批注")));
        }
        editor->accept();
    });
    editor->show();
}

void MainWindow::editMarkupTags(const QString& id)
{
    Markup* m = findMarkup(id);
    if (!m) {
        if (shown_.contains(id))
            statusBar()->showMessage(QStringLiteral("请先打开该文档再编辑"), 2500);
        return;
    }
    const QString text = askText(this, QStringLiteral("标签"),
                                 QStringLiteral("标签（用逗号分隔，例如：方法论,待复习）："),
                                 m->tags.join(QStringLiteral(", ")));
    if (text.isNull()) return;
    QStringList tags;
    for (const QString& t : text.split(QRegularExpression(QStringLiteral("[,，;；\\s]+")),
                                      Qt::SkipEmptyParts)) {
        const QString trimmed = t.trimmed();
        if (!trimmed.isEmpty() && !tags.contains(trimmed)) tags.append(trimmed);
    }
    Markup after = *m;
    after.tags = tags;
    after.updated = QDateTime::currentMSecsSinceEpoch();
    undo_->push(new MarkupCommand(this, CmdKind::Modify, *m, after, QStringLiteral("编辑标签")));
}

void MainWindow::pushAddMarkup(const Markup& m)
{
    undo_->push(new MarkupCommand(this, CmdKind::Add, Markup(), m, QStringLiteral("添加标注")));
}

void MainWindow::undoCommand()
{
    undo_->undo();
}

void MainWindow::redoCommand()
{
    undo_->redo();
}

int MainWindow::undoDepth() const
{
    return undo_->count();
}

void MainWindow::applyCommand(CmdKind kind, const Markup& before, const Markup& after, bool undo)
{
    switch (kind) {
    case CmdKind::Add:
        if (undo)
            storeRemoveMarkup(after.id);
        else
            storeUpsertMarkup(store_.stampForLocalWrite(after));
        break;
    case CmdKind::Remove:
        if (undo)
            storeUpsertMarkup(store_.stampForLocalWrite(before));  // undo delete = new version
        else
            storeRemoveMarkup(before.id);                          // tombstone with bumped vv
        break;
    case CmdKind::Modify:
        storeUpsertMarkup(store_.stampForLocalWrite(undo ? before : after));
        break;
    }
    refreshFromStore();
}

void MainWindow::jumpToMarkup(const QString& id)
{
    Markup m;
    bool found = false;
    if (const Markup* cur = findMarkup(id)) {
        m = *cur;
        found = true;
    } else if (shown_.contains(id)) {
        m = shown_.value(id);
        found = true;
    }
    if (!found) return;

    if (m.docPath != docPath_ && QFileInfo::exists(m.docPath)) {
        if (!openDocument(m.docPath)) return;
    }
    view_->gotoPage(m.page);
    if (m.type != MarkupType::Image) {
        Selection sel;
        sel.page = m.page;
        sel.quads = m.quads;
        sel.text = m.text;
        view_->setSelection(sel);
    } else {
        view_->clearSelection();
    }
    updateStatus();
}

void MainWindow::showViewContextMenu(const QPoint& pos)
{
    const int idx = view_->markupIndexAt(pos);
    QMenu menu(this);
    if (idx >= 0 && idx < markups_.size()) {
        const QString id = markups_.at(idx).id;
        menu.addAction(QStringLiteral("编辑批注…"), [this, id]() { editMarkupComment(id); });
        menu.addAction(QStringLiteral("编辑标签…"), [this, id]() { editMarkupTags(id); });
        menu.addAction(QStringLiteral("删除此标注"), [this, id]() { deleteMarkupById(id); });
        menu.addSeparator();
    }
    if (view_->captureMode()) {
        menu.addAction(QStringLiteral("取消框选"), [this]() { view_->setCaptureMode(false); });
    } else {
        menu.addAction(QStringLiteral("框选摘录图片"), [this]() { captureImageExcerpt(); });
    }
    const Selection sel = view_->selection();
    if (sel.isValid()) {
        menu.addAction(QStringLiteral("高亮"), [this]() { addMarkupFromSelection(MarkupType::Highlight); });
        menu.addAction(QStringLiteral("下划线"), [this]() { addMarkupFromSelection(MarkupType::Underline); });
        menu.addAction(QStringLiteral("波浪线"), [this]() { addMarkupFromSelection(MarkupType::Squiggly); });
        menu.addAction(QStringLiteral("批注"), [this]() { addMarkupFromSelection(MarkupType::Note); });
    }
    if (!menu.isEmpty()) menu.exec(view_->mapToGlobal(pos));
}

void MainWindow::saveAnnotationsToPdf()
{
    if (!engine_.supportsAnnotations()) {
        statusBar()->showMessage(QStringLiteral("EPUB 等格式不支持把标注写入原文件（已存入笔记库）"), 3000);
        return;
    }
    QVector<Markup> pending;
    int skipped = 0;
    for (const Markup& m : markups_) {
        if (m.writtenToPdf) continue;
        if (m.type == MarkupType::Image || m.type == MarkupType::Ink) {
            ++skipped;   // binary/handwriting layers stay app-side
            continue;
        }
        pending.append(m);
    }
    if (pending.isEmpty()) {
        statusBar()->showMessage(
            skipped > 0 ? QStringLiteral("没有可写入的文字标注（图片/手写只在笔记库里，不写进 PDF）")
                        : QStringLiteral("没有待写入的新标注"),
            3500);
        return;
    }
    QString err;
    if (!engine_.writeAnnotations(pending, &err)) {
        warnUser(this, QStringLiteral("写入 PDF 失败"), err);
        return;
    }
    for (Markup& m : markups_) {
        if (!m.writtenToPdf && m.type != MarkupType::Image) {
            m.writtenToPdf = true;
            store_.setWrittenToPdf(m.id, true);
        }
    }
    view_->setMarkups(markups_);
    updateNotesPanel();
    statusBar()->showMessage(
        QStringLiteral("已把 %1 条标注写入 PDF（增量保存）").arg(pending.size()), 3000);
}

// ---------------------------------------------------------------- export

bool MainWindow::exportMarkups(const QString& format, const QString& path, QString* error,
                               bool onlyFiltered)
{
    if (!engine_.isOpen()) {
        if (error) *error = QStringLiteral("没有打开的文档");
        return false;
    }
    if (path.isEmpty()) {
        if (error) *error = QStringLiteral("输出路径为空");
        return false;
    }
    ExportContext ctx;
    ctx.docPath = docPath_;
    ctx.docTitle = QFileInfo(docPath_).completeBaseName();
    ctx.outPath = path;
    ctx.markups = onlyFiltered ? notesShown() : markups_;
    ctx.deckName = ctx.docTitle;
    ctx.byGroups = exportByGroups_;
    ctx.groupsByDoc.insert(docPath_, store_.mindGroups(docPath_));   // nested groups in exports
    Q_UNUSED(format);
    return exports::byExtension(ctx, error);
}

bool MainWindow::documentFileChanged() const
{
    const QFileInfo fi(docPath_);
    if (!fi.exists()) return false;
    const qint64 size = store_.documentFileSize(docPath_);
    const qint64 mtime = store_.documentFileMtime(docPath_);
    if (size == 0 && mtime == 0) return false;   // first sighting
    return size != fi.size() || mtime != fi.lastModified().toMSecsSinceEpoch();
}

int MainWindow::migrateAfterRevision()
{
    if (!engine_.isOpen()) return 0;
    const MigrationReport report = migrateByText(engine_, markups_);
    if (report.moved.isEmpty() && report.lost.isEmpty()) {
        store_.setDocumentFileIdentity(docPath_, QFileInfo(docPath_).size(),
                                       QFileInfo(docPath_).lastModified().toMSecsSinceEpoch());
        return 0;
    }

    QVector<QPair<Markup, Markup>> pairs;
    QVector<Markup> batch;
    auto apply = [&](const Relocation& r) {
        Markup* current = findMarkup(r.markupId);
        if (!current || r.newPage < 0 || r.quads.isEmpty()) return;
        Markup after = *current;
        after.page = r.newPage;
        after.quads = r.quads;
        after.updated = QDateTime::currentMSecsSinceEpoch();
        pairs.append({*current, after});
        batch.append(after);
    };
    for (const Relocation& r : report.moved) apply(r);
    for (const Relocation& r : report.inPlace) apply(r);

    if (!batch.isEmpty()) {
        store_.upsertMarkups(batch, nullptr);
        reloadMarkups();
    }
    store_.setDocumentFileIdentity(docPath_, QFileInfo(docPath_).size(),
                                   QFileInfo(docPath_).lastModified().toMSecsSinceEpoch());

    statusBar()->showMessage(report.summary(), 8000);
    if (!report.moved.isEmpty() || !report.lost.isEmpty()) {
        QString text = QStringLiteral("这本书的文件看起来换过版本，已按文字重新定位笔记：\n\n"
                                      "· 移动到新页：%1 条\n· 未找到（可能已被删除）：%2 条\n\n"
                                      "移动是可撤销的（Ctrl+Z）。")
                           .arg(report.moved.size())
                           .arg(report.lost.size());
        inform(this, QStringLiteral("已按文字迁移笔记"), text);
    }
    return report.moved.size();
}

void MainWindow::setReflowLayout(float widthPt, float heightPt, float emPt)
{
    if (!engine_.isOpen()) return;
    engine_.setLayout(widthPt, heightPt, emPt);
    view_->setEngine(&engine_);      // rebuild the page layout with the new pagination
    view_->setMarkups(markups_);
    updateStatus();
}

int MainWindow::relocateAnchors()
{
    if (!engine_.isOpen() || !engine_.supportsAnchors()) return 0;

    QVector<Markup> changed;
    for (const Markup& m : markups_) {
        if (m.anchorOffset < 0 || m.anchorChapter.isEmpty()) continue;
        const int chapter = m.anchorChapter.toInt();
        int intra = -1;
        const int page = engine_.pageForChapterOffset(chapter, m.anchorOffset, &intra);
        if (page < 0 || intra < 0) continue;
        if (page == m.page) continue;

        const TextPage* tp = engine_.textPage(page);
        if (!tp || intra >= tp->chars.size()) continue;

        // keep the same number of characters as the original excerpt
        const int span = qMax(1, m.text.size());
        const int end = qMin(tp->chars.size() - 1, intra + span - 1);

        Markup moved = m;
        moved.page = page;
        moved.quads = quadsForRange(*tp, intra, end);
        moved.updated = QDateTime::currentMSecsSinceEpoch();
        if (!moved.quads.isEmpty()) changed.append(moved);
    }

    if (changed.isEmpty()) return 0;
    store_.upsertMarkups(changed, nullptr);
    reloadMarkups();
    statusBar()->showMessage(
        QStringLiteral("已按锚点重新定位 %1 条笔记（版式变化）").arg(changed.size()), 4000);
    return changed.size();
}

void MainWindow::setFastImport(bool on)
{
    store_.setFastImport(on);
    if (on)
        statusBar()->showMessage(
            QStringLiteral("已开启导入加速：写库不再每次 fsync（异常断电可能丢最近的笔记）"), 6000);
}

void MainWindow::setTagFilter(const QString& tag)
{
    if (!tagFilter_) return;
    if (tag.isEmpty()) {
        tagFilter_->setCurrentIndex(0);
        return;
    }
    const int idx = tagFilter_->findData(tag);
    if (idx >= 0) tagFilter_->setCurrentIndex(idx);
    updateNotesPanel();
}

void MainWindow::setReaderSpeed(int wpm)
{
    reader_.setWordsPerMinute(wpm);
    store_.setSetting(QStringLiteral("read.wpm"), QString::number(wpm));
    statusBar()->showMessage(QStringLiteral("朗读语速：%1 字/分钟").arg(wpm), 2500);
}

bool MainWindow::exportLibrary(const QString& format, const QString& path, QString* error)
{
    const QVector<Markup> all = store_.allMarkups(100000);
    if (all.isEmpty()) {
        if (error) *error = QStringLiteral("书库里还没有笔记");
        return false;
    }
    ExportContext ctx;
    ctx.docPath = docPath_;                      // for the relative asset folder name
    ctx.docTitle = QStringLiteral("书库");
    ctx.outPath = path;
    ctx.markups = all;
    ctx.deckName = QStringLiteral("AdaptNote 书库");
    ctx.groupByDocument = true;
    ctx.byGroups = exportByGroups_;
    for (const QString& path : store_.documentPaths())
        ctx.groupsByDoc.insert(path, store_.mindGroups(path));
    Q_UNUSED(format);
    return exports::byExtension(ctx, error);
}

bool MainWindow::exportDocuments(const QStringList& docPaths, const QString& format,
                                 const QString& path, QString* error)
{
    const QVector<Markup> all = store_.allMarkups(100000);
    QVector<Markup> selected;
    for (const Markup& m : all)
        if (docPaths.contains(m.docPath)) selected.append(m);

    if (selected.isEmpty()) {
        if (error) *error = QStringLiteral("选中的文档没有笔记");
        return false;
    }

    ExportContext ctx;
    ctx.docPath = docPaths.isEmpty() ? docPath_ : docPaths.first();
    ctx.docTitle = QStringLiteral("选中文档");
    ctx.outPath = path;
    ctx.markups = selected;
    ctx.deckName = QStringLiteral("AdaptNote 选中文档");
    ctx.groupByDocument = true;
    ctx.byGroups = exportByGroups_;
    for (const QString& path : docPaths) ctx.groupsByDoc.insert(path, store_.mindGroups(path));
    Q_UNUSED(format);
    return exports::byExtension(ctx, error);
}

void MainWindow::doExportLibrary(const QString& format)
{
    const QVector<Markup> all = store_.allMarkups(100000);
    if (all.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("书库里还没有笔记"), 2500);
        return;
    }

    QString filter;
    if (format == QStringLiteral("md")) filter = QStringLiteral("Markdown (*.md)");
    else if (format == QStringLiteral("html")) filter = QStringLiteral("HTML (*.html)");
    else if (format == QStringLiteral("opml")) filter = QStringLiteral("OPML (*.opml)");
    else if (format == QStringLiteral("xmind")) filter = QStringLiteral("XMind (*.xmind)");
    else if (format == QStringLiteral("apkg")) filter = QStringLiteral("Anki (*.apkg)");

    const QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                         QStringLiteral("/书库.") + format;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出整个书库"), base,
                                                      filter);
    if (path.isEmpty()) return;

    QString err;
    if (!exportLibrary(format, path, &err)) {
        warnUser(this, QStringLiteral("导出失败"), err);
        return;
    }
    statusBar()->showMessage(
        QStringLiteral("已导出书库的 %1 条笔记到 %2").arg(all.size()).arg(path), 5000);
}

void MainWindow::doExport(const QString& format)
{
    if (!engine_.isOpen()) {
        statusBar()->showMessage(QStringLiteral("请先打开一本文档"), 2500);
        return;
    }
    if (markups_.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("当前文档还没有笔记可导出"), 2500);
        return;
    }

    QString filter;
    if (format == QStringLiteral("md")) filter = QStringLiteral("Markdown (*.md)");
    else if (format == QStringLiteral("html")) filter = QStringLiteral("HTML (*.html)");
    else if (format == QStringLiteral("opml")) filter = QStringLiteral("OPML (*.opml)");
    else if (format == QStringLiteral("xmind")) filter = QStringLiteral("XMind (*.xmind)");
    else if (format == QStringLiteral("apkg")) filter = QStringLiteral("Anki (*.apkg)");

    const QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                         "/" + QFileInfo(docPath_).completeBaseName() + "." + format;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出笔记"), base, filter);
    if (path.isEmpty()) return;

    QString err;
    const int count = exportFiltered_ ? notesShown().size() : markups_.size();
    if (!exportMarkups(format, path, &err, exportFiltered_)) {
        warnUser(this, QStringLiteral("导出失败"), err);
        return;
    }
    statusBar()->showMessage(QStringLiteral("已导出 %1 条笔记到 %2").arg(count).arg(path), 5000);
}

// ---------------------------------------------------------------- sync

void MainWindow::openSyncSettings()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("同步设置"));
    auto* form = new QFormLayout(&dlg);

    const SyncSettings current = sync_.settings();
    auto* backend = new QComboBox(&dlg);
    backend->addItem(QStringLiteral("WebDAV 服务器"), QStringLiteral("webdav"));
    backend->addItem(QStringLiteral("本地文件夹 / NAS"), QStringLiteral("folder"));
    backend->setCurrentIndex(current.backend == QLatin1String("folder") ? 1 : 0);

    auto* url = new QLineEdit(current.url, &dlg);
    auto* browse = new QToolButton(&dlg);
    browse->setText(QStringLiteral("选择…"));
    auto* urlRow = new QWidget(&dlg);
    auto* urlLayout = new QHBoxLayout(urlRow);
    urlLayout->setContentsMargins(0, 0, 0, 0);
    urlLayout->addWidget(url, 1);
    urlLayout->addWidget(browse);

    auto* user = new QLineEdit(current.user, &dlg);
    auto* pass = new QLineEdit(current.password, &dlg);
    pass->setEchoMode(QLineEdit::Password);
    auto* remote = new QLineEdit(current.remotePath, &dlg);
    remote->setPlaceholderText(QStringLiteral("adaptnote"));

    form->addRow(QStringLiteral("同步方式"), backend);
    form->addRow(QStringLiteral("服务器 / 文件夹"), urlRow);
    form->addRow(QStringLiteral("用户名"), user);
    form->addRow(QStringLiteral("密码 / 应用密码"), pass);
    form->addRow(QStringLiteral("同步子目录"), remote);

    auto applyBackend = [=]() {
        const bool folder = backend->currentData().toString() == QLatin1String("folder");
        url->setPlaceholderText(folder ? QStringLiteral("例如 D:/ReadingNotes 或 /mnt/nas/notes")
                                       : QStringLiteral("https://dav.jianguoyun.com/dav 或自建 WebDAV"));
        browse->setVisible(folder);
        user->setEnabled(!folder);
        pass->setEnabled(!folder);
    };
    connect(backend, &QComboBox::currentIndexChanged, &dlg, applyBackend);
    connect(browse, &QToolButton::clicked, &dlg, [&]() {
        const QString dir = QFileDialog::getExistingDirectory(
            &dlg, QStringLiteral("选择同步文件夹"), url->text());
        if (!dir.isEmpty()) url->setText(dir);
    });
    applyBackend();

    auto* hint = new QLabel(QStringLiteral(
        "WebDAV 与本地文件夹共用同一套快照与版本向量合并；本地文件夹适合 U 盘 / NAS。"
        "密码以明文保存在本地数据库（自用工具）。两端同时改同一条笔记会保留两个版本，"
        "并给副本打上 #conflict 标签。"), &dlg);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#8a8a8e;font-size:12px;"));
    form->addRow(hint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    SyncSettings s;
    s.backend = backend->currentData().toString();
    s.url = url->text().trimmed();
    s.user = s.backend == QLatin1String("folder") ? QString() : user->text().trimmed();
    s.password = s.backend == QLatin1String("folder") ? QString() : pass->text();
    s.remotePath = remote->text().trimmed().isEmpty() ? QStringLiteral("adaptnote")
                                                      : remote->text().trimmed();
    sync_.setSettings(s);
    statusBar()->showMessage(QStringLiteral("同步设置已保存"), 3000);
}

void MainWindow::runSync()
{
    if (!sync_.settings().configured()) {
        openSyncSettings();
        if (!sync_.settings().configured()) return;
    }

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const SyncReport rep = sync_.syncAll();
    QApplication::restoreOverrideCursor();

    reloadMarkups();

    if (!rep.ok) {
        statusBar()->showMessage(rep.summary(), 6000);
        warnUser(this, QStringLiteral("同步失败"), rep.error);
        return;
    }

    statusBar()->showMessage(rep.summary(), 6000);
    if (rep.conflicts > 0) {
        inform(this, QStringLiteral("同步完成（有冲突）"),
               QStringLiteral("有 %1 处冲突：两个版本都已保留，副本带 #conflict 标签。\n\n%2")
                   .arg(rep.conflicts)
                   .arg(rep.messages.join(QLatin1Char('\n'))));
    }
}

// ---------------------------------------------------------------- search

void MainWindow::findText(const QString& needle)
{
    hits_.clear();
    hitIndex_ = -1;
    const QString n = needle.trimmed();
    if (n.isEmpty() || !engine_.isOpen()) {
        view_->setSearchHits({}, -1);
        if (findLabel_) findLabel_->setText(QStringLiteral("0/0"));
        updateStatus();
        return;
    }

    if (findScope_ && findScope_->currentIndex() == 1) {
        // Topic scope: search the notes of every document in the topic and show them in the
        // notes panel. Page highlights stay empty because the hits may live in other books.
        const QStringList topicPaths = currentTopicDocPaths();
        if (topicPaths.isEmpty()) {
            statusBar()->showMessage(QStringLiteral("先打开一个阅读主题再按主题搜索"), 3000);
            view_->setSearchHits({}, -1);
            if (findLabel_) findLabel_->setText(QStringLiteral("0/0"));
            return;
        }
        if (noteSearchEdit_) {
            QSignalBlocker block(noteSearchEdit_);
            noteSearchEdit_->setText(n);
        }
        if (allDocsBtn_ && !allDocsBtn_->isChecked()) allDocsBtn_->setChecked(true);
        updateNotesPanel();
        view_->setSearchHits({}, -1);
        const int found = notesList_ ? notesList_->count() : 0;
        if (findLabel_) findLabel_->setText(QStringLiteral("%1 条").arg(found));
        statusBar()->showMessage(
            QStringLiteral("主题内命中 %1 条笔记（%2 本资料）").arg(found).arg(topicPaths.size()),
            3000);
        return;
    }

    const int cap = 800;
    for (int p = 0; p < engine_.pageCount() && hits_.size() < cap; ++p) {
        const TextPage* tp = engine_.textPage(p);
        if (!tp || tp->isEmpty()) continue;
        const QString text = pageText(*tp);
        int from = 0;
        while (hits_.size() < cap) {
            const int idx = text.indexOf(n, from, Qt::CaseInsensitive);
            if (idx < 0) break;
            TextHit h;
            h.page = p;
            h.startChar = idx;
            h.endChar = idx + n.size() - 1;
            h.quads = quadsForRange(*tp, h.startChar, h.endChar);
            h.text = text.mid(idx, n.size());
            if (!h.quads.isEmpty()) hits_.append(h);
            from = idx + qMax(1, n.size());
        }
    }

    if (!hits_.isEmpty()) hitIndex_ = 0;
    view_->setSearchHits(hits_, hitIndex_);
    if (hitIndex_ >= 0) {
        view_->gotoPage(hits_.at(hitIndex_).page);
        view_->scrollToPageFraction(hits_.at(hitIndex_).page, 0.15);
    }
    if (findLabel_)
        findLabel_->setText(QStringLiteral("%1/%2")
                                .arg(hitIndex_ >= 0 ? hitIndex_ + 1 : 0)
                                .arg(hits_.size()));
    updateStatus();
}

void MainWindow::nextHit(int direction)
{
    if (hits_.isEmpty()) return;
    const int n = hits_.size();
    hitIndex_ = ((hitIndex_ + direction) % n + n) % n;
    view_->setSearchHits(hits_, hitIndex_);
    view_->gotoPage(hits_.at(hitIndex_).page);
    view_->scrollToPageFraction(hits_.at(hitIndex_).page, 0.15);
    if (findLabel_)
        findLabel_->setText(QStringLiteral("%1/%2").arg(hitIndex_ + 1).arg(n));
    updateStatus();
}

// ---------------------------------------------------------------- image excerpt

void MainWindow::captureImageExcerpt()
{
    if (!engine_.isOpen()) return;
    if (view_->captureMode()) {
        view_->setCaptureMode(false);
        return;
    }
    view_->setCaptureMode(true);
    statusBar()->showMessage(QStringLiteral("拖拽框选要摘录的区域（Esc 取消）"), 5000);
    updateStatus();
}

bool MainWindow::addImageExcerpt(int page, const QRectF& pageRect)
{
    if (!engine_.isOpen() || page < 0 || page >= engine_.pageCount()) return false;
    const QSizeF ps = engine_.pageSize(page);
    QRectF rect = pageRect.intersected(QRectF(QPointF(0, 0), ps));
    if (rect.width() < 4 || rect.height() < 4) return false;

    const double zoom = 2.0;  // crisper than screen resolution
    const QImage pageImg = engine_.renderPage(page, static_cast<float>(zoom), 0);
    if (pageImg.isNull()) return false;

    const QRect crop(static_cast<int>(rect.x() * zoom), static_cast<int>(rect.y() * zoom),
                     static_cast<int>(rect.width() * zoom), static_cast<int>(rect.height() * zoom));
    const QImage cropImg = pageImg.copy(crop.intersected(pageImg.rect()));
    if (cropImg.isNull()) return false;

    const QString id = newId();
    const QString path = store_.imagesDir() + "/" + id + ".png";
    if (!cropImg.save(path)) return false;

    Markup m;
    m.id = id;
    m.docPath = docPath_;
    m.page = page;
    m.type = MarkupType::Image;
    m.color = markupColor_;
    m.quads = {Quad::fromRect(rect)};
    m.imagePath = path;
    m.created = m.updated = QDateTime::currentMSecsSinceEpoch();
    undo_->push(new MarkupCommand(this, CmdKind::Add, Markup(), m, QStringLiteral("摘录图片")));
    statusBar()->showMessage(QStringLiteral("已摘录图片（%1×%2）").arg(cropImg.width()).arg(cropImg.height()), 3000);
    return true;
}

void MainWindow::onRegionSelected(int page, const QRectF& pageRect)
{
    view_->setCaptureMode(false);
    addImageExcerpt(page, pageRect);
    updateStatus();
}

// ---------------------------------------------------------------- P5: AI panel

void MainWindow::buildAiDock()
{
    // AI lives in the same right-hand tab strip as excerpts, notes and words: a second
    // floating dock made the panel too narrow and hid the notes while it was open.
    auto* panel = new QWidget(this);
    aiPanel_ = panel;
    panel->setObjectName("aiPanel");

    auto* vbox = new QVBoxLayout(panel);
    vbox->setContentsMargins(8, 8, 8, 8);
    vbox->setSpacing(6);

    aiRoles_ = ai::loadRoles(store_.dataDir() + "/ai_roles.json");

    auto* top = new QHBoxLayout;
    aiRoleCombo_ = new QComboBox(panel);
    for (const ai::Role& r : aiRoles_) aiRoleCombo_->addItem(r.name);
    const QString savedRole = store_.setting(QStringLiteral("ai.role"));
    const int roleIdx = aiRoleCombo_->findText(savedRole);
    if (roleIdx >= 0) aiRoleCombo_->setCurrentIndex(roleIdx);
    top->addWidget(aiRoleCombo_, 1);

    auto* rolesBtn = new QToolButton(panel);
    rolesBtn->setText(QStringLiteral("角色…"));
    rolesBtn->setToolTip(QStringLiteral("编辑系统角色（会写入 ai_roles.json）"));
    top->addWidget(rolesBtn);
    connect(rolesBtn, &QToolButton::clicked, this, &MainWindow::editAiRoles);

    auto* settingsBtn = new QToolButton(panel);
    settingsBtn->setText(QStringLiteral("设置"));
    settingsBtn->setToolTip(QStringLiteral("接口地址 / API Key / 模型"));
    top->addWidget(settingsBtn);
    connect(settingsBtn, &QToolButton::clicked, this, &MainWindow::buildAiSettings);
    vbox->addLayout(top);

    aiActionCombo_ = new QComboBox(panel);
    aiActionCombo_->addItems({ai::actionLabel(ai::PromptAction::Explain),
                              ai::actionLabel(ai::PromptAction::Translate),
                              ai::actionLabel(ai::PromptAction::Summarize),
                              ai::actionLabel(ai::PromptAction::Questions),
                              ai::actionLabel(ai::PromptAction::Custom)});
    vbox->addWidget(aiActionCombo_);

    aiExtraEdit_ = new QLineEdit(panel);
    aiExtraEdit_->setPlaceholderText(QStringLiteral("可选：目标语言 / 自定义要求"));
    vbox->addWidget(aiExtraEdit_);

    auto* askRow = new QHBoxLayout;
    aiQuestionEdit_ = new QLineEdit(panel);
    aiQuestionEdit_->setPlaceholderText(QStringLiteral("直接提问，例如：这一章的论证漏洞在哪？"));
    auto* askButton = new QPushButton(QStringLiteral("提问"), panel);
    askRow->addWidget(aiQuestionEdit_, 1);
    askRow->addWidget(askButton);
    vbox->addLayout(askRow);
    connect(askButton, &QPushButton::clicked, this, &MainWindow::askAiQuestion);
    connect(aiQuestionEdit_, &QLineEdit::returnPressed, this, &MainWindow::askAiQuestion);

    aiRunButton_ = new QPushButton(QStringLiteral("对选中的内容提问"), panel);
    vbox->addWidget(aiRunButton_);
    connect(aiRunButton_, &QPushButton::clicked, this, &MainWindow::runAiWithSelection);

    aiReasoningToggle_ = new QCheckBox(QStringLiteral("显示思考过程"), panel);
    aiReasoningToggle_->setChecked(false);
    vbox->addWidget(aiReasoningToggle_);

    aiReasoning_ = new QTextBrowser(panel);
    aiReasoning_->setMaximumHeight(140);
    aiReasoning_->setVisible(false);
    vbox->addWidget(aiReasoning_);
    connect(aiReasoningToggle_, &QCheckBox::toggled, this, [this](bool on) {
        if (aiReasoning_) aiReasoning_->setVisible(on);
    });

    aiOutput_ = new QTextBrowser(panel);
    aiOutput_->setPlaceholderText(QStringLiteral("选中一段文字，点上面的按钮开始"));
    vbox->addWidget(aiOutput_, 1);

    aiInsertButton_ = new QPushButton(QStringLiteral("插入为批注"), panel);
    aiInsertButton_->setEnabled(false);
    vbox->addWidget(aiInsertButton_);
    connect(aiInsertButton_, &QPushButton::clicked, this, &MainWindow::insertAiAsNote);

    if (leftTabs_) {
        leftTabs_->addTab(panel, QStringLiteral("AI"));
        leftTabs_->setTabToolTip(leftTabs_->indexOf(panel),
                                 QStringLiteral("AI 助手：解释 / 翻译 / 总结 / 提问"));
        // Reopen on the panel the reader last used (tab labels change with counts, so
        // the stable object name is what gets persisted).
        const QString savedTab = store_.setting(QStringLiteral("ui.sideTab"));
        if (!savedTab.isEmpty()) {
            for (int i = 0; i < leftTabs_->count(); ++i) {
                if (leftTabs_->widget(i)->objectName() == savedTab) {
                    leftTabs_->setCurrentIndex(i);
                    break;
                }
            }
        }
        connect(leftTabs_, &QTabWidget::currentChanged, this, [this](int index) {
            if (!leftTabs_ || index < 0 || index >= leftTabs_->count()) return;
            store_.setSetting(QStringLiteral("ui.sideTab"), leftTabs_->widget(index)->objectName());
        });
    }
    if (moreMenu_) {
        moreMenu_->addAction(QStringLiteral("AI 助手"), this, [this]() {
            if (notesDock_) notesDock_->show();
            if (leftTabs_ && aiPanel_) leftTabs_->setCurrentWidget(aiPanel_);
        });
        moreMenu_->addAction(QStringLiteral("生词本"), this, [this]() {
            if (notesDock_) notesDock_->show();
            if (leftTabs_ && vocabPanel_) leftTabs_->setCurrentWidget(vocabPanel_);
        });
        QAction* pageOcr = moreMenu_->addAction(QStringLiteral("整页 OCR"));
        pageOcr->setToolTip(QStringLiteral("识别当前页（tesseract 或视觉模型）"));
        connect(pageOcr, &QAction::triggered, this, &MainWindow::ocrCurrentPage);
        QAction* ocrSettings = moreMenu_->addAction(QStringLiteral("OCR 设置…"));
        connect(ocrSettings, &QAction::triggered, this, &MainWindow::openOcrSettings);
    }

    ai::AiClient::Settings s;
    s.baseUrl = store_.setting(QStringLiteral("ai.url"), QStringLiteral("https://api.deepseek.com"));
    s.apiKey = store_.setting(QStringLiteral("ai.key"));
    s.model = store_.setting(QStringLiteral("ai.model"), QStringLiteral("deepseek-chat"));
    ai_.setSettings(s);

    connect(&ai_, &ai::AiClient::delta, this, [this](const QString& content, const QString& reasoning) {
        aiContent_ += content;
        aiReasoningText_ += reasoning;
        if (translationRequest_ && translationOutput_) {
            translationOutput_->setPlainText(aiContent_);
        } else if (pendingWord_.isEmpty() && !pendingOcr_) {
            // Dictionary/OCR replies are handled as data, not pasted into the answer pane.
            if (aiOutput_) aiOutput_->setPlainText(aiContent_);
        }
        if (aiReasoning_) aiReasoning_->setPlainText(aiReasoningText_);
    });
    connect(&ai_, &ai::AiClient::finished,
            this, [this](const QString& content, const QString& reasoning, const QString& error) {
                aiContent_ = content;
                aiReasoningText_ = reasoning;
                if (aiReasoning_) aiReasoning_->setPlainText(reasoning);
                if (aiRunButton_) aiRunButton_->setEnabled(true);

                if (!pendingWord_.isEmpty()) {
                    const QString word = pendingWord_;
                    pendingWord_.clear();
                    vocab::Definition def;
                    def.word = word;
                    def.source = QStringLiteral("ai");
                    if (!error.isEmpty()) {
                        def.ok = false;
                        def.error = error;
                    } else {
                        vocab::parseAiDefinition(content, &def);
                    }
                    saveVocabDefinition(def);
                    return;
                }
                if (pendingOcr_) {
                    pendingOcr_ = false;
                    if (!error.isEmpty()) {
                        statusBar()->showMessage(QStringLiteral("云端 OCR 失败：%1").arg(error), 7000);
                    } else {
                        showOcrResult(content);
                    }
                    return;
                }

                if (aiOutput_) aiOutput_->setPlainText(content);
                if (aiInsertButton_) aiInsertButton_->setEnabled(!content.isEmpty());
                if (translationRequest_) {
                    translationRequest_ = false;
                    if (translationOutput_) {
                        translationOutput_->setPlainText(error.isEmpty() ? content : QStringLiteral("翻译失败：%1").arg(error));
                    }
                    if (!error.isEmpty()) statusBar()->showMessage(QStringLiteral("划词翻译失败：%1").arg(error), 6000);
                }
                if (!error.isEmpty())
                    statusBar()->showMessage(QStringLiteral("AI：%1").arg(error), 6000);
                else
                    statusBar()->showMessage(QStringLiteral("AI 回答完成（%1 字）").arg(content.size()), 4000);
            });
}

void MainWindow::buildAiSettings()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("AI 设置"));
    auto* form = new QFormLayout(&dlg);

    ai::AiClient::Settings s = ai_.settings();
    auto* url = new QLineEdit(s.baseUrl, &dlg);
    url->setPlaceholderText(QStringLiteral("https://api.deepseek.com"));
    auto* key = new QLineEdit(s.apiKey, &dlg);
    key->setEchoMode(QLineEdit::Password);
    auto* model = new QLineEdit(s.model, &dlg);
    model->setPlaceholderText(QStringLiteral("deepseek-chat / deepseek-reasoner"));

    form->addRow(QStringLiteral("接口地址"), url);
    form->addRow(QStringLiteral("API Key"), key);
    form->addRow(QStringLiteral("模型"), model);

    auto* hint = new QLabel(QStringLiteral("兼容 OpenAI 的 /v1/chat/completions 接口；"
                                           "Key 明文存在本地数据库（自用工具）。"), &dlg);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#6b7280;font-size:12px;"));
    form->addRow(hint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;

    s.baseUrl = url->text().trimmed();
    s.apiKey = key->text().trimmed();
    s.model = model->text().trimmed().isEmpty() ? QStringLiteral("deepseek-chat") : model->text().trimmed();
    ai_.setSettings(s);
    store_.setSetting(QStringLiteral("ai.url"), s.baseUrl);
    store_.setSetting(QStringLiteral("ai.key"), s.apiKey);
    store_.setSetting(QStringLiteral("ai.model"), s.model);
    statusBar()->showMessage(QStringLiteral("AI 设置已保存"), 3000);
}

void MainWindow::editAiRoles()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("AI 角色"));
    dlg.resize(620, 460);
    auto* hbox = new QHBoxLayout(&dlg);

    auto* list = new QListWidget(&dlg);
    list->setFixedWidth(180);
    for (const ai::Role& r : aiRoles_) list->addItem(r.name);
    hbox->addWidget(list);

    auto* right = new QVBoxLayout;
    auto* nameEdit = new QLineEdit(&dlg);
    nameEdit->setPlaceholderText(QStringLiteral("角色名"));
    right->addWidget(nameEdit);
    auto* contentEdit = new QPlainTextEdit(&dlg);
    contentEdit->setPlaceholderText(QStringLiteral("系统提示词（决定回答风格与约束）"));
    right->addWidget(contentEdit, 1);

    auto* btns = new QHBoxLayout;
    auto* addBtn = new QPushButton(QStringLiteral("新增"), &dlg);
    auto* delBtn = new QPushButton(QStringLiteral("删除"), &dlg);
    auto* resetBtn = new QPushButton(QStringLiteral("恢复内置"), &dlg);
    btns->addWidget(addBtn);
    btns->addWidget(delBtn);
    btns->addWidget(resetBtn);
    btns->addStretch(1);
    right->addLayout(btns);
    hbox->addLayout(right, 1);

    auto* box = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    right->addWidget(box);
    connect(box, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    auto loadInto = [&](int row) {
        if (row < 0 || row >= aiRoles_.size()) {
            nameEdit->clear();
            contentEdit->clear();
            return;
        }
        nameEdit->setText(aiRoles_.at(row).name);
        contentEdit->setPlainText(aiRoles_.at(row).content);
    };
    auto storeFromFields = [&](int row) {
        if (row < 0 || row >= aiRoles_.size()) return;
        aiRoles_[row].name = nameEdit->text().trimmed().isEmpty()
                                 ? aiRoles_.at(row).name
                                 : nameEdit->text().trimmed();
        aiRoles_[row].content = contentEdit->toPlainText();
        list->item(row)->setText(aiRoles_.at(row).name);
    };

    connect(list, &QListWidget::currentRowChanged, this, [&](int row) {
        storeFromFields(list->currentRow());
        loadInto(row);
    });
    connect(addBtn, &QPushButton::clicked, this, [&]() {
        storeFromFields(list->currentRow());
        ai::Role r;
        r.name = QStringLiteral("新角色 %1").arg(aiRoles_.size() + 1);
        r.content = QStringLiteral("你是一个…（在这里写系统提示词）");
        aiRoles_.append(r);
        list->addItem(r.name);
        list->setCurrentRow(aiRoles_.size() - 1);
        loadInto(aiRoles_.size() - 1);
    });
    connect(delBtn, &QPushButton::clicked, this, [&]() {
        const int row = list->currentRow();
        if (row < 0 || aiRoles_.size() <= 1) return;
        aiRoles_.removeAt(row);
        delete list->takeItem(row);
        if (list->count() > 0) list->setCurrentRow(qMin(row, list->count() - 1));
    });
    connect(resetBtn, &QPushButton::clicked, this, [&]() {
        aiRoles_ = ai::defaultRoles();
        list->clear();
        for (const ai::Role& r : aiRoles_) list->addItem(r.name);
        list->setCurrentRow(0);
        loadInto(0);
    });

    if (list->count() > 0) list->setCurrentRow(0);

    if (dlg.exec() != QDialog::Accepted) return;
    storeFromFields(list->currentRow());

    QString err;
    if (!ai::saveRoles(store_.dataDir() + QStringLiteral("/ai_roles.json"), aiRoles_, &err)) {
        warnUser(this, QStringLiteral("保存失败"), err);
        return;
    }
    if (aiRoleCombo_) {
        const QString current = aiRoleCombo_->currentText();
        aiRoleCombo_->clear();
        for (const ai::Role& r : aiRoles_) aiRoleCombo_->addItem(r.name);
        const int idx = aiRoleCombo_->findText(current);
        if (idx >= 0) aiRoleCombo_->setCurrentIndex(idx);
    }
    statusBar()->showMessage(QStringLiteral("已保存 %1 个角色").arg(aiRoles_.size()), 3000);
}

void MainWindow::updateSelectionTranslationAction()
{
    if (!selectionBar_ || !view_) return;
    if (translationDialog_ && translationDialog_->isVisible()) translationDialog_->hide();
    const Selection selection = view_->selection();
    if (!selection.isValid() || selection.text.trimmed().isEmpty() || ai_.busy()) {
        selectionBar_->hide();
        return;
    }
    QRectF bounds;
    for (const Quad& quad : selection.quads) {
        QRectF rect = view_->pageRectInViewport(selection.page);
        Q_UNUSED(rect);
        const QPointF a = view_->mapPageToViewport(selection.page, quad.ul);
        const QPointF b = view_->mapPageToViewport(selection.page, quad.lr);
        const QPointF c = view_->mapPageToViewport(selection.page, quad.ur);
        const QPointF d = view_->mapPageToViewport(selection.page, quad.ll);
        const QRectF one = QRectF(a,b).normalized().united(QRectF(c,d).normalized());
        bounds = bounds.isNull() ? one : bounds.united(one);
    }
    if (bounds.isNull()) { selectionBar_->hide(); return; }
    const QSize size = selectionBar_->sizeHint();
    QPoint pos(qBound(8, qRound(bounds.left()), view_->viewport()->width() - size.width() - 8),
               qMax(8, qRound(bounds.top()) - size.height() - 8));
    if (pos.y() < 8) pos.setY(qMin(view_->viewport()->height() - size.height() - 8,
                                   qRound(bounds.bottom()) + 8));
    selectionBar_->move(pos);
    selectionBar_->adjustSize();
    selectionBar_->raise();
    selectionBar_->show();
}

void MainWindow::translateSelectionForTest()
{
    showSelectionTranslation();
}

void MainWindow::showSelectionTranslation()
{
    const QString text = view_ ? view_->selection().text.trimmed() : QString();
    if (text.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("请先选择要翻译的文字"), 2500);
        return;
    }
    if (ai_.settings().apiKey.trimmed().isEmpty()) {
        statusBar()->showMessage(QStringLiteral("请先在“更多 → AI 助手 → 设置”中配置兼容 OpenAI 的 API Key"), 6000);
        return;
    }
    if (!translationDialog_) {
        translationDialog_ = new QDialog(this);
        translationDialog_->setObjectName("selectionTranslationDialog");
        translationDialog_->setAttribute(Qt::WA_DeleteOnClose, false);
        translationDialog_->setWindowTitle(QStringLiteral("划词翻译"));
        translationDialog_->resize(460, 250);
        auto* layout = new QVBoxLayout(translationDialog_);
        auto* source = new QLabel(translationDialog_);
        source->setObjectName("translationSource");
        source->setWordWrap(true);
        source->setStyleSheet(QStringLiteral("color:#747b86;font-size:12px;padding:4px 2px;"));
        layout->addWidget(source);
        auto* targetLanguage = new QComboBox(translationDialog_);
        targetLanguage->setObjectName("translationTargetLanguage");
        targetLanguage->addItems({QStringLiteral("简体中文"), QStringLiteral("English"),
                                  QStringLiteral("日本語"), QStringLiteral("한국어"),
                                  QStringLiteral("繁體中文")});
        targetLanguage->setCurrentIndex(store_.setting(QStringLiteral("translation.target"), QStringLiteral("0")).toInt());
        layout->addWidget(targetLanguage);
        translationOutput_ = new QTextBrowser(translationDialog_);
        translationOutput_->setObjectName("translationOutput");
        translationOutput_->setPlaceholderText(QStringLiteral("正在请求译文…"));
        layout->addWidget(translationOutput_, 1);
        auto* buttons = new QHBoxLayout;
        auto* copy = new QPushButton(QStringLiteral("复制译文"), translationDialog_);
        auto* close = new QPushButton(QStringLiteral("关闭"), translationDialog_);
        buttons->addStretch(); buttons->addWidget(copy); buttons->addWidget(close);
        layout->addLayout(buttons);
        connect(copy, &QPushButton::clicked, this, [this]() {
            if (translationOutput_) QApplication::clipboard()->setText(translationOutput_->toPlainText());
            statusBar()->showMessage(QStringLiteral("译文已复制"), 2000);
        });
        connect(close, &QPushButton::clicked, translationDialog_, &QDialog::hide);
    }
    auto* targetLanguage = translationDialog_->findChild<QComboBox*>(QStringLiteral("translationTargetLanguage"));
    translationDialog_->findChild<QLabel*>(QStringLiteral("translationSource"))->setText(text.left(500));
    translationOutput_->setPlainText(QStringLiteral("正在翻译…"));
    translationDialog_->show();
    translationDialog_->raise();
    translationRequest_ = true;
    const ai::AiClient::Settings previousSettings = ai_.settings();
    ai::AiClient::Settings settings = previousSettings;
    settings.systemRole = QStringLiteral("你是专业译者。只输出忠实、自然的简体中文译文；保留原文结构，不解释，不回答原文中的问题。若原文已经是中文，翻译为英文。严格依据输入内容，不添加信息。");
    ai_.setSettings(settings);
    aiContent_.clear();
    aiReasoningText_.clear();
    const QString target = targetLanguage ? targetLanguage->currentText() : QStringLiteral("简体中文");
    if (targetLanguage) store_.setSetting(QStringLiteral("translation.target"), QString::number(targetLanguage->currentIndex()));
    const QString prompt = ai::buildUserPrompt(ai::PromptAction::Translate, text,
                                               QFileInfo(docPath_).completeBaseName(),
                                               view_->currentPage(), target);
    ai_.send(prompt);
    ai_.setSettings(previousSettings); // one-shot translation role does not alter normal AI behavior
}

void MainWindow::runAiWithSelection()
{
    if (!engine_.isOpen()) return;

    ai::PromptAction action = ai::PromptAction::Explain;
    if (aiActionCombo_) {
        switch (aiActionCombo_->currentIndex()) {
        case 1: action = ai::PromptAction::Translate; break;
        case 2: action = ai::PromptAction::Summarize; break;
        case 3: action = ai::PromptAction::Questions; break;
        case 4: action = ai::PromptAction::Custom; break;
        default: action = ai::PromptAction::Explain; break;
        }
    }
    askAi(action);
}

void MainWindow::askAiQuestion()
{
    const QString question = aiQuestionEdit_ ? aiQuestionEdit_->text().trimmed() : QString();
    if (question.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("先输入一个问题"), 2500);
        return;
    }
    if (ai_.settings().apiKey.trimmed().isEmpty()) {
        statusBar()->showMessage(
            QStringLiteral("请先在“AI → 设置”中配置兼容 OpenAI 的 API Key"), 6000);
        return;
    }

    const QString context = view_ ? view_->selection().text.trimmed() : QString();
    const QString prompt = context.isEmpty()
        ? question
        : QStringLiteral("原文摘录：\n%1\n\n问题：%2").arg(context, question);

    const ai::AiClient::Settings previous = ai_.settings();
    ai::AiClient::Settings oneShot = previous;
    oneShot.systemRole = QStringLiteral(
        "你是阅读助手。回答简洁、具体；引用原文时说明依据，不确定就直说不确定。");
    ai_.setSettings(oneShot);
    aiContent_.clear();
    aiReasoningText_.clear();
    if (aiOutput_) aiOutput_->clear();
    if (aiReasoning_) aiReasoning_->clear();
    ai_.send(prompt);
    ai_.setSettings(previous);
    statusBar()->showMessage(QStringLiteral("正在提问…"), 2000);
}

void MainWindow::lookupSelectionWord()
{
    const QString text = view_ ? view_->selection().text.trimmed() : QString();
    if (text.isEmpty()) {
        if (notesDock_) notesDock_->show();
        if (leftTabs_ && vocabPanel_) leftTabs_->setCurrentWidget(vocabPanel_);
        if (vocabPanel_) vocabPanel_->focusLookup();
        statusBar()->showMessage(QStringLiteral("选中一个词再点“查词”，或直接输入"), 4000);
        return;
    }
    lookupWordForVocab(vocab::normalizeQuery(text));
}

void MainWindow::lookupWordForVocab(const QString& raw)
{
    const QString word = vocab::normalizeQuery(raw);
    if (word.isEmpty()) return;

    if (notesDock_) notesDock_->show();
    if (leftTabs_ && vocabPanel_) leftTabs_->setCurrentWidget(vocabPanel_);

    const WordEntry existing = store_.wordByText(word);
    if (!existing.id.isEmpty()) {
        if (vocabPanel_) {
            vocabPanel_->focusLookup(existing.word);
            vocabPanel_->selectWord(existing.word);
        }
        statusBar()->showMessage(QStringLiteral("「%1」已在生词本").arg(existing.word), 3000);
        return;
    }

    const vocab::Definition local = vocab::lookupLocal(store_.dataDir() + "/ecdict.db", word);
    if (local.ok) {
        saveVocabDefinition(local);
        statusBar()->showMessage(QStringLiteral("本地词典：%1").arg(local.word), 3500);
        return;
    }

    if (ai_.settings().apiKey.trimmed().isEmpty()) {
        vocab::Definition bare;
        bare.word = word;
        bare.meaning = QStringLiteral("（待补释义：未配置本地词典或 AI）");
        bare.source = QStringLiteral("manual");
        bare.ok = true;
        saveVocabDefinition(bare);
        statusBar()->showMessage(
            QStringLiteral("已加入生词本；未找到本地词典也未配置 AI，释义留空"), 6000);
        return;
    }

    pendingWord_ = word;
    aiContent_.clear();
    aiReasoningText_.clear();
    const ai::AiClient::Settings previous = ai_.settings();
    ai::AiClient::Settings oneShot = previous;
    oneShot.systemRole = QStringLiteral(
        "你是严谨的双语词典编辑。严格按要求的三行输出，不寒暄、不解释格式。");
    ai_.setSettings(oneShot);
    ai_.send(vocab::promptForLookup(word));
    ai_.setSettings(previous);
    statusBar()->showMessage(QStringLiteral("正在查词：%1…").arg(word), 3000);
}

void MainWindow::saveVocabDefinition(const vocab::Definition& def)
{
    if (def.word.trimmed().isEmpty()) return;
    WordEntry w = store_.wordByText(def.word);
    if (w.id.isEmpty()) w.id = newId();
    w.word = def.word.trimmed();
    if (!def.phonetic.isEmpty()) w.phonetic = def.phonetic;
    if (!def.meaning.isEmpty()) w.meaning = def.meaning;
    if (!def.example.isEmpty()) w.example = def.example;
    if (w.docPath.isEmpty() && !docPath_.isEmpty()) {
        w.docPath = docPath_;
        w.page = view_ ? view_->currentPage() : -1;
    }
    // No dictionary example? Keep the sentence the reader actually met the word in.
    if (w.example.isEmpty() && view_ && !docPath_.isEmpty() && w.docPath == docPath_) {
        if (const TextPage* tp = engine_.textPage(view_->currentPage())) {
            w.example = sentenceAround(pageText(*tp), w.word);
        }
    }
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (w.created <= 0) w.created = now;
    w.updated = now;
    if (w.dueAt <= 0) w.dueAt = now;

    QString err;
    if (!store_.upsertWord(w, &err)) {
        statusBar()->showMessage(QStringLiteral("生词保存失败：%1").arg(err), 6000);
        return;
    }
    if (vocabPanel_) {
        vocabPanel_->reload();
        vocabPanel_->selectWord(def.word);
    }
    if (def.ok) {
        statusBar()->showMessage(QStringLiteral("已加入生词本：%1").arg(def.word), 4000);
    } else {
        statusBar()->showMessage(
            QStringLiteral("已记住「%1」，但查词失败：%2").arg(def.word, def.error), 7000);
    }
}

void MainWindow::ocrSelection()
{
    QImage image;
    if (view_ && view_->selection().isValid()) {
        const Selection sel = view_->selection();
        const QSizeF pt = engine_.pageSize(sel.page);
        const float scale = 2.4f;
        QRectF box;
        for (const Quad& q : sel.quads)
            box = box.isNull() ? q.boundingRect() : box.united(q.boundingRect());
        if (pt.width() > 0) {
            const QImage page = engine_.renderPage(sel.page, scale, 0);
            if (!page.isNull()) {
                const QRect crop = QRectF(box.left() * scale, box.top() * scale,
                                          box.width() * scale, box.height() * scale)
                                       .adjusted(-6, -6, 6, 6)
                                       .toRect()
                                       .intersected(page.rect());
                if (crop.width() > 2 && crop.height() > 2) image = page.copy(crop);
            }
        }
    }
    if (image.isNull()) image = QApplication::clipboard()->image();
    if (image.isNull() && !kHeadless) {
        const QString path = QFileDialog::getOpenFileName(
            this, QStringLiteral("选择要识别的图片"), QString(),
            QStringLiteral("图片 (*.png *.jpg *.jpeg *.bmp *.webp)"));
        if (!path.isEmpty()) image = QImage(path);
    }
    if (image.isNull()) {
        statusBar()->showMessage(QStringLiteral("选中一块区域，或先把图片复制到剪贴板"), 5000);
        return;
    }
    runOcrOnImage(image);
}

void MainWindow::ocrCurrentPage()
{
    if (!engine_.isOpen()) {
        statusBar()->showMessage(QStringLiteral("先打开一份文档"), 3000);
        return;
    }
    const int page = view_ ? view_->currentPage() : 0;
    const QImage image = engine_.renderPage(page, 2.4f, 0);
    if (image.isNull()) {
        statusBar()->showMessage(QStringLiteral("当前页渲染失败"), 4000);
        return;
    }
    runOcrOnImage(image);
}

void MainWindow::runOcrOnImage(const QImage& image)
{
    const QString lang = store_.setting(QStringLiteral("ocr.lang"),
                                        QStringLiteral("eng+chi_sim"));
    QApplication::setOverrideCursor(Qt::WaitCursor);
    const ocr::Result result = ocr::recognize(image, lang);
    QApplication::restoreOverrideCursor();
    if (result.ok) {
        statusBar()->showMessage(QStringLiteral("OCR 完成（%1）").arg(result.engine), 3000);
        showOcrResult(result.text);
        return;
    }

    if (!ai_.settings().apiKey.trimmed().isEmpty()) {
        pendingOcr_ = true;
        aiContent_.clear();
        statusBar()->showMessage(QStringLiteral("本机 OCR 不可用，改用云端视觉模型…"), 3000);
        ai_.sendVision(QStringLiteral("只输出图片中的文字，不要解释，不要 Markdown。保留原换行。"),
                       {image});
        return;
    }

    warnUser(this, QStringLiteral("OCR 不可用"), result.error);
    statusBar()->showMessage(result.error, 7000);
}

void MainWindow::openOcrSettings()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("OCR 设置"));
    auto* form = new QFormLayout(&dlg);

    const QString exe = ocr::tesseractExecutable();
    auto* status = new QLabel(
        exe.isEmpty()
            ? QStringLiteral("未找到 tesseract。选中区域 OCR 会改用已配置的视觉模型。")
            : QStringLiteral("已找到：%1").arg(exe),
        &dlg);
    status->setWordWrap(true);
    status->setStyleSheet(QStringLiteral("color:#8a8a8e;font-size:12px;"));
    form->addRow(QStringLiteral("引擎"), status);

    auto* lang = new QLineEdit(
        store_.setting(QStringLiteral("ocr.lang"), QStringLiteral("eng+chi_sim")), &dlg);
    lang->setPlaceholderText(QStringLiteral("eng+chi_sim"));
    form->addRow(QStringLiteral("语言包"), lang);

    auto* hint = new QLabel(QStringLiteral(
        "语言包写法例如 eng+chi_sim（英文+简体）、chi_sim+eng、jpn、kor。\n"
        "Windows 默认安装在 C:/Program Files/Tesseract-OCR；其他位置可用环境变量\n"
        "ADAPT_TESSERACT 指向 tesseract 可执行文件。没有 tesseract 时，会在配置了\n"
        "AI Key 的前提下自动走视觉模型。"), &dlg);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#8a8a8e;font-size:12px;"));
    form->addRow(hint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;

    const QString value = lang->text().trimmed();
    store_.setSetting(QStringLiteral("ocr.lang"),
                      value.isEmpty() ? QStringLiteral("eng+chi_sim") : value);
    statusBar()->showMessage(QStringLiteral("OCR 设置已保存"), 3000);
}

void MainWindow::showOcrResult(const QString& text)
{
    const QString content = text.trimmed();
    if (content.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("没有识别到文字"), 3000);
        return;
    }
    if (kHeadless) {
        statusBar()->showMessage(QStringLiteral("OCR：%1").arg(content.left(60)), 5000);
        return;
    }

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("OCR 识别结果"));
    dlg.resize(560, 420);
    auto* layout = new QVBoxLayout(&dlg);
    auto* edit = new QPlainTextEdit(content, &dlg);
    layout->addWidget(edit, 1);
    auto* buttons = new QDialogButtonBox(&dlg);
    auto* insert = buttons->addButton(QStringLiteral("插入为摘录"), QDialogButtonBox::AcceptRole);
    auto* copy = buttons->addButton(QStringLiteral("复制"), QDialogButtonBox::ActionRole);
    buttons->addButton(QStringLiteral("关闭"), QDialogButtonBox::RejectRole);
    layout->addWidget(buttons);
    connect(copy, &QPushButton::clicked, &dlg, [&edit]() {
        QApplication::clipboard()->setText(edit->toPlainText());
    });
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    connect(insert, &QPushButton::clicked, &dlg, &QDialog::accept);
    if (dlg.exec() != QDialog::Accepted) return;

    const QString title = QStringLiteral("OCR 摘录");
    if (createFreeNoteForPageForTest(title, edit->toPlainText().trimmed(),
                                     view_ ? view_->currentPage() : 0))
        statusBar()->showMessage(QStringLiteral("OCR 文字已插入为摘录笔记"), 4000);
}

void MainWindow::askAi(ai::PromptAction action)
{
    QString selection = view_->selection().text;
    int page = view_->currentPage();
    if (selection.trimmed().isEmpty()) {
        // no selection: use the current page's text (first chunk)
        const TextPage* tp = engine_.textPage(page);
        if (tp && !tp->isEmpty()) selection = pageText(*tp).left(1500);
    }
    if (selection.trimmed().isEmpty()) {
        statusBar()->showMessage(QStringLiteral("没有可发送的文本"), 2500);
        return;
    }

    ai::AiClient::Settings s = ai_.settings();
    if (aiRoleCombo_ && aiRoleCombo_->currentIndex() >= 0 &&
        aiRoleCombo_->currentIndex() < aiRoles_.size()) {
        s.systemRole = aiRoles_.at(aiRoleCombo_->currentIndex()).content;
        store_.setSetting(QStringLiteral("ai.role"), aiRoles_.at(aiRoleCombo_->currentIndex()).name);
    }
    ai_.setSettings(s);

    aiContent_.clear();
    aiReasoningText_.clear();
    if (aiOutput_) aiOutput_->clear();
    if (aiReasoning_) aiReasoning_->clear();
    if (aiInsertButton_) aiInsertButton_->setEnabled(false);
    if (aiRunButton_) aiRunButton_->setEnabled(false);

    const QString prompt = ai::buildUserPrompt(action, selection,
                                              QFileInfo(docPath_).completeBaseName(), page,
                                              aiExtraEdit_ ? aiExtraEdit_->text() : QString());
    ai_.send(prompt);
    statusBar()->showMessage(QStringLiteral("已请求 AI（%1）…").arg(ai::actionLabel(action)), 3000);
}

QString MainWindow::aiOutput() const
{
    return aiContent_;
}

void MainWindow::insertAiAsNote()
{
    if (aiContent_.trimmed().isEmpty()) return;
    const Selection sel = view_->selection();
    QVector<Quad> quads = sel.quads;
    int page = view_->currentPage();
    if (sel.isValid()) page = sel.page;
    if (quads.isEmpty()) {
        const QSizeF ps = engine_.pageSize(page);
        quads = {Quad::fromRect(QRectF(ps.width() * 0.1, ps.height() * 0.12, ps.width() * 0.8,
                                       24))};
    }

    Markup m;
    m.id = newId();
    m.docPath = docPath_;
    m.page = page;
    m.type = MarkupType::Note;
    m.color = QColor(QStringLiteral("#3D8BFD"));
    m.quads = quads;
    m.text = sel.text.left(200);
    m.comment = aiContent_.trimmed();
    m.tags = {QStringLiteral("ai")};
    m.created = m.updated = QDateTime::currentMSecsSinceEpoch();
    pushAddMarkup(m);
    statusBar()->showMessage(QStringLiteral("已把 AI 回答存为批注"), 3000);
}

// ---------------------------------------------------------------- P5: handwriting

void MainWindow::onInkStroke(int page, const QVector<InkPoint>& points)
{
    addInkStroke(page, points);
}

void MainWindow::setInkWidthPreset(double w)
{
    if (!view_) return;
    view_->setInkWidth(w);
    store_.setSetting(QStringLiteral("ink.width"), QString::number(w, 'f', 1));
    if (inkAction_) inkAction_->setToolTip(
        QStringLiteral("手写笔迹（P），当前笔宽 %1 pt；右键选粗细").arg(w, 0, 'f', 1));
}

int MainWindow::eraseInkAt(int page, const QPointF& pagePoint, double tolerance)
{
    QStringList ids;
    for (const Markup& m : markups_)
        if (m.type == MarkupType::Ink && m.page == page && ink::hits(m.ink, pagePoint, tolerance))
            ids.append(m.id);
    if (ids.isEmpty()) return 0;
    applyEraser(page, ids, {pagePoint});
    return ids.size();
}

void MainWindow::setInkColor(const QColor& c)
{
    if (!c.isValid()) return;
    inkColor_ = c;
    if (view_) view_->setInkColor(c);
    store_.setSetting(QStringLiteral("ink.color"), c.name(QColor::HexRgb));
    statusBar()->showMessage(QStringLiteral("笔迹颜色：%1").arg(c.name()), 2500);
}

void MainWindow::onInkEraseRequested(int page, const QStringList& ids, const QVector<QPointF>& pagePath)
{
    applyEraser(page, ids, pagePath);
}

void MainWindow::applyEraser(int page, const QStringList& ids, const QVector<QPointF>& pagePath)
{
    if (ids.isEmpty()) return;
    // The eraser radius follows the current pen width, in page units.
    const double radius = qMax(3.0, view_->inkWidth() * 1.6);

    int removed = 0;
    int split = 0;
    for (const QString& id : ids) {
        const Markup* found = findMarkup(id);
        if (!found || found->type != MarkupType::Ink) continue;
        const Markup before = *found;

        const QVector<QVector<InkPoint>> pieces = ink::splitStroke(before.ink, pagePath, radius);
        if (pieces.isEmpty()) {
            deleteMarkupById(id);   // whole stroke erased
            ++removed;
            continue;
        }
        if (pieces.size() == 1 && pieces.first().size() == before.ink.size()) continue;

        // Keep the first surviving piece in the original note (undoable), append the rest.
        // NOTE: a Modify command must keep the id, otherwise it inserts a new row and
        // the old stroke survives (that bug produced duplicate strokes).
        Markup tail = before;
        tail.ink = pieces.first();
        tail.quads = {Quad::fromRect(ink::bounds(tail.ink, 2.0))};
        tail.updated = QDateTime::currentMSecsSinceEpoch();
        undo_->push(new MarkupCommand(this, CmdKind::Modify, before, tail, QStringLiteral("擦除笔迹")));

        for (int i = 1; i < pieces.size(); ++i) {
            Markup extra = before;
            extra.id = newId();
            extra.ink = pieces.at(i);
            extra.quads = {Quad::fromRect(ink::bounds(extra.ink, 2.0))};
            extra.created = extra.updated = QDateTime::currentMSecsSinceEpoch();
            pushAddMarkup(extra);
        }
        if (pieces.size() > 1) ++split;
    }

    if (removed > 0 || split > 0)
        statusBar()->showMessage(
            QStringLiteral("已擦除 %1 条、切断 %2 条笔迹（Ctrl+Z 可撤销）").arg(removed).arg(split),
            2500);
}

bool MainWindow::addInkStroke(int page, const QVector<InkPoint>& points)
{
    if (!engine_.isOpen() || points.size() < 2) return false;

    Markup m;
    m.id = newId();
    m.docPath = docPath_;
    m.page = page;
    m.type = MarkupType::Ink;
    m.color = inkColor_;
    m.ink = points;
    m.quads = {Quad::fromRect(ink::bounds(points, 2.0))};
    m.created = m.updated = QDateTime::currentMSecsSinceEpoch();
    pushAddMarkup(m);
    statusBar()->showMessage(QStringLiteral("已添加手写笔迹（%1 个采样点）").arg(points.size()), 2000);
    return true;
}

void MainWindow::toggleInkMode()
{
    if (!inkAction_) return;
    inkAction_->setChecked(!inkAction_->isChecked());
}

// ---------------------------------------------------------------- P5: mind map

namespace {

QString groupIdOf(const QString& keyOrId)
{
    return keyOrId.startsWith(QLatin1String("group:")) ? keyOrId.mid(6) : keyOrId;
}

}  // namespace

QString MainWindow::createMindGroup(const QString& name, const QString& parentId)
{
    if (docPath_.isEmpty() || name.trimmed().isEmpty()) return {};
    const QString id = store_.ensureMindGroup(docPath_, name, parentId);
    if (id.isEmpty()) return {};
    store_.syncDocumentGroupTopic(docPath_);
    // Undo should remove exactly the row we created, so hand the command a snapshot.
    for (const MindGroup& g : store_.mindGroups(docPath_)) {
        if (g.id != id) continue;
        undo_->push(new MindGroupCommand(this, MindGroupCommand::Add, g, g,
                                         parentId.isEmpty() ? QStringLiteral("新建分组")
                                                            : QStringLiteral("新建子分组")));
        break;
    }
    statusBar()->showMessage(
        parentId.isEmpty() ? QStringLiteral("已创建分组「%1」").arg(name)
                           : QStringLiteral("已创建子分组「%1」").arg(name),
        2500);
    return id;
}

bool MainWindow::moveMindGroupUp(const QString& groupKey)
{
    const QString id = groupIdOf(groupKey);
    if (id.isEmpty()) return false;
    QString grandParent;
    bool found = false;
    for (const MindGroup& g : store_.mindGroups(docPath_)) {
        if (g.id != id) continue;
        found = true;
        for (const MindGroup& p : store_.mindGroups(docPath_))
            if (p.id == g.parentId) grandParent = p.parentId;
    }
    if (!found) return false;
    MindGroup before;
    for (const MindGroup& g : store_.mindGroups(docPath_))
        if (g.id == id) before = g;
    const bool ok = store_.setMindGroupParent(id, grandParent);
    if (ok) {
        MindGroup after = before;
        for (const MindGroup& g : store_.mindGroups(docPath_))
            if (g.id == id) after = g;
        undo_->push(new MindGroupCommand(this, MindGroupCommand::Modify, before, after,
                                         QStringLiteral("分组上移一级")));
        statusBar()->showMessage(QStringLiteral("分组已上移一级"), 2500);
    }
    return ok;
}

bool MainWindow::renameMindGroup(const QString& groupKey, const QString& newName)
{
    const QString id = groupIdOf(groupKey);
    if (id.isEmpty() || newName.trimmed().isEmpty()) return false;
    for (const MindGroup& g : store_.mindGroups(docPath_)) {
        if (g.id != id) continue;
        MindGroup after = g;
        after.name = newName.trimmed();
        after.updated = QDateTime::currentMSecsSinceEpoch();
        const MindGroup before = g;
        const bool ok = store_.upsertMindGroup(store_.stampGroupForLocalWrite(after));
        if (ok) {
            undo_->push(new MindGroupCommand(this, MindGroupCommand::Modify, before, after,
                                             QStringLiteral("重命名分组")));
            statusBar()->showMessage(QStringLiteral("分组已重命名为「%1」").arg(after.name), 2500);
        }
        return ok;
    }
    return false;
}

QVector<MindGroup> MainWindow::mindGroups() const
{
    return store_.mindGroups(docPath_);
}

bool MainWindow::deleteMindGroup(const QString& groupKey)
{
    const QString id = groupIdOf(groupKey);
    if (id.isEmpty()) return false;
    MindGroup before;
    for (const MindGroup& g : store_.mindGroups(docPath_))
        if (g.id == id) before = g;
    const bool ok = !before.id.isEmpty();
    if (ok)
        undo_->push(new MindGroupCommand(this, MindGroupCommand::Remove, before, before,
                                         QStringLiteral("删除分组")));
    if (ok) {
        refreshFromStore();   // notes fall back to page buckets
        statusBar()->showMessage(QStringLiteral("分组已删除，组内笔记回到按页分组"), 3000);
    }
    return ok;
}

bool MainWindow::applyMindReorder(const QString& markupId, const QString& groupKey,
                                const QString& beforeMarkupId)
{
    Markup* moved = findMarkup(markupId);
    if (!moved) return false;

    // a page bucket means "no explicit group"
    QString group;
    if (groupKey.startsWith(QLatin1String("group:")))
        group = groupKey.mid(6);
    if (group.isEmpty() && groupKey.startsWith(QLatin1String("page:"))) {
        const int page = groupKey.mid(5).section(QLatin1Char(':'), 0, 0).toInt();
        // keep the note on that page, drop the explicit group
        if (moved->page != page) {
            Markup shifted = *moved;
            shifted.page = page;
            shifted.mindGroup.clear();
            moved = findMarkup(markupId);
            *moved = shifted;
        }
    }

    // siblings = live notes that will live in that bucket after the move
    QVector<Markup> siblings;
    for (const Markup& m : markups_) {
        if (m.deleted || !m.parentMarkupId.isEmpty()) continue;
        const bool sameGroup = group.isEmpty() ? m.mindGroup.isEmpty() : (m.mindGroup == group);
        if (!sameGroup) continue;
        if (group.isEmpty() && groupKey.startsWith(QLatin1String("page:")) &&
            m.page != groupKey.mid(5).section(QLatin1Char(':'), 0, 0).toInt())
            continue;
        siblings.append(m);
    }
    // the dragged note must be a member (it may be moving in from another bucket)
    Markup candidate = *moved;
    candidate.mindGroup = group;
    candidate.parentMarkupId.clear();
    if (groupKey.startsWith(QLatin1String("page:")))
        candidate.page = groupKey.mid(5).section(QLatin1Char(':'), 0, 0).toInt();
    bool present = false;
    for (const Markup& m : siblings)
        if (m.id == candidate.id) present = true;
    if (!present) siblings.append(candidate);

    const QVector<QPair<QString, int>> plan = mind::planReorder(siblings, markupId, beforeMarkupId);
    if (plan.isEmpty()) return false;

    QVector<QPair<Markup, Markup>> pairs;
    for (const auto& step : plan) {
        Markup* current = findMarkup(step.first);
        if (!current) continue;
        Markup after = *current;
        after.mindOrder = step.second;
        if (after.id == markupId) { after.mindGroup = group; after.parentMarkupId.clear(); }
        if (after.id == markupId &&
            groupKey.startsWith(QLatin1String("page:")))
            after.page = groupKey.mid(5).section(QLatin1Char(':'), 0, 0).toInt();
        if (after.mindOrder == current->mindOrder && after.mindGroup == current->mindGroup &&
            after.parentMarkupId == current->parentMarkupId && after.page == current->page)
            continue;
        after.updated = QDateTime::currentMSecsSinceEpoch();
        pairs.append({*current, after});
    }
    if (pairs.isEmpty()) return false;

    undo_->push(new MultiMarkupCommand(this, pairs, QStringLiteral("调整脑图顺序")));
    statusBar()->showMessage(QStringLiteral("已调整顺序（Ctrl+Z 可撤销）"), 2500);
    return true;
}

bool MainWindow::applyMindReparent(const QString& markupId, const QString& groupKey)
{
    Markup* m = findMarkup(markupId);
    if (!m) return false;

    // Group keys travel as "group:<id>" / "page:<n>"; a page group means "no explicit
    // group", i.e. drop back to the automatic page outline. Anything that is not a
    // known group id is treated as a name and adopted into a real entity first.
    QString group;
    if (groupKey.startsWith(QLatin1String("group:")))
        group = groupKey.mid(6);
    else if (!groupKey.startsWith(QLatin1String("page:")))
        group = groupKey;

    if (!group.isEmpty()) {
        bool known = false;
        for (const MindGroup& g : store_.mindGroups(docPath_))
            if (g.id == group) known = true;
        if (!known) {
            const QString id = store_.ensureMindGroup(docPath_, group);
            if (!id.isEmpty()) group = id;
        }
    }

    if (m->mindGroup == group && m->parentMarkupId.isEmpty()) return false;

    int nextOrder = 0;
    for (const Markup& other : markups_) {
        if (other.id == m->id) continue;
        if (other.mindGroup == group && other.mindOrder >= nextOrder) nextOrder = other.mindOrder + 1;
    }

    Markup after = *m;
    after.mindGroup = group;
    after.parentMarkupId.clear();
    after.mindOrder = nextOrder;
    after.updated = QDateTime::currentMSecsSinceEpoch();
    undo_->push(new MarkupCommand(this, CmdKind::Modify, *m, after,
                                  QStringLiteral("调整脑图结构")));
    statusBar()->showMessage(group.isEmpty()
                                 ? QStringLiteral("已移回按页码分组")
                                 : QStringLiteral("已移动到分组「%1」").arg(group),
                             2500);
    return true;
}

bool MainWindow::applyMindContainerReparent(const QString& markupId, const QString& containerKey)
{
    return moveMarkupToContainer(markupId, docPath_, docPath_, containerKey);
}

bool MainWindow::reparentNoteWithinDocument(const QString& markupId, const QString& parentMarkupId)
{
    Markup* child = findMarkup(markupId);
    Markup* parent = findMarkup(parentMarkupId);
    if (!child || !parent || child->id == parent->id || child->docPath != parent->docPath ||
        child->mindGroup != parent->mindGroup || (child->mindGroup.isEmpty() && child->page != parent->page)) return false;
    QString cursor = parent->id;
    while (!cursor.isEmpty()) {
        if (cursor == child->id) return false;
        QString next;
        for (const Markup& note : markups_) if (note.id == cursor) next = note.parentMarkupId;
        cursor = next;
    }
    if (child->parentMarkupId == parent->id) return false;
    Markup after = *child; after.parentMarkupId = parent->id;
    after.updated = QDateTime::currentMSecsSinceEpoch();
    undo_->push(new MarkupCommand(this, CmdKind::Modify, *child, after, QStringLiteral("建立笔记父子关系")));
    return true;
}

bool MainWindow::moveMarkupToContainer(const QString& markupId, const QString& sourcePath,
                                       const QString& targetPath, const QString& targetKey)
{
    const QString pagePrefix = QStringLiteral("page:");
    QString containerDocument;
    QString pagePart;
    if (targetKey.startsWith(QStringLiteral("docpage:"))) {
        const QString payload = targetKey.mid(QStringLiteral("docpage:").size());
        const int split = payload.lastIndexOf(QStringLiteral(":page:"));
        if (split < 0) return false;
        containerDocument = payload.left(split);
        pagePart = payload.mid(split + QStringLiteral(":page:").size());
    } else if (targetKey.startsWith(pagePrefix)) {
        const int split = targetKey.indexOf(QLatin1Char(':'), pagePrefix.size());
        if (split >= 0) {
            pagePart = targetKey.mid(pagePrefix.size(), split - pagePrefix.size());
            containerDocument = targetKey.mid(split + 1);
        } else pagePart = targetKey.mid(pagePrefix.size());
    } else if (targetKey.startsWith(QStringLiteral("document:"))) {
        containerDocument = targetKey.mid(QStringLiteral("document:").size());
        pagePart = QStringLiteral("0");
    } else return false;

    const QString source = QFileInfo(sourcePath).absoluteFilePath();
    const QString destination = containerDocument.isEmpty() ? QFileInfo(targetPath).absoluteFilePath()
                                                            : QFileInfo(containerDocument).absoluteFilePath();
    if (source.isEmpty() || destination.isEmpty()) return false;
    Markup snapshot;
    bool found = false;
    for (const Markup& row : store_.markups(source)) if (row.id == markupId) { snapshot = row; found = true; break; }
    bool targetExists = false;
    for (const TopicDoc& doc : currentTopicDocPaths().isEmpty() ? QVector<TopicDoc>() : store_.topicDocs(currentTopic_))
        if (QFileInfo(doc.docPath).absoluteFilePath() == destination) targetExists = true;
    if (!currentTopic_.isEmpty() && !targetExists) return false;
    bool ok = false; const int page = pagePart.toInt(&ok);
    if (!found || !ok || page < 0) return false;
    Markup after = snapshot;
    after.docPath = destination;
    after.page = page;
    after.mindGroup.clear();
    after.parentMarkupId.clear();
    after.mindOrder = 0;
    after.updated = QDateTime::currentMSecsSinceEpoch();
    if (source != destination) {
        if (store_.markups(destination).size() >= 0) {
            after.id = newId();
            after.docPath = destination;
            after.parentMarkupId.clear();
            after.mindGroup.clear();
            undo_->push(new MarkupCommand(this, CmdKind::Remove, snapshot, Markup(),
                                          QStringLiteral("从原资料移出笔记")));
            undo_->push(new MarkupCommand(this, CmdKind::Add, Markup(), after,
                                          QStringLiteral("复制笔记到目标资料")));
            return true;
        }
    }
    if (source != docPath_ && !openDocument(source)) return false;
    undo_->push(new MarkupCommand(this, CmdKind::Modify, snapshot, after,
                                  QStringLiteral("移动笔记到第 %1 页分支").arg(page + 1)));
    return true;
}

void MainWindow::ensureMindPanel()
{
    if (mindPanel_ || !splitter_) return;

    mindPanel_ = new QWidget(splitter_);
    auto* vbox = new QVBoxLayout(mindPanel_);
    vbox->setContentsMargins(0, 0, 0, 0);
    vbox->setSpacing(0);

    auto* bar = new QWidget(mindPanel_);
    auto* row = new QHBoxLayout(bar);
    row->setContentsMargins(8, 4, 8, 4);
    row->setSpacing(6);
    auto* title = new QLabel(QStringLiteral("脑图"), bar);
    title->setStyleSheet(QStringLiteral("font-weight:600;"));
    row->addWidget(title);
    row->addStretch(1);

    auto* newGroupBtn = new QPushButton(QStringLiteral("新建分组"), bar);
    newGroupBtn->setToolTip(QStringLiteral("在选中节点下新建分组，未选中则建在顶层"));
    row->addWidget(newGroupBtn);

    auto* linkBtn = new QPushButton(QStringLiteral("连线"), bar);
    linkBtn->setCheckable(true);
    linkBtn->setToolTip(QStringLiteral("打开后依次点两张卡片，在它们之间画一条关联线"));
    row->addWidget(linkBtn);

    // Only the frequent actions stay visible; the rest live in a small menu so the panel
    // cannot demand enough width to squeeze the page area.
    auto* moreBtn = new QToolButton(bar);
    moreBtn->setText(QStringLiteral("⋯"));
    moreBtn->setToolTip(QStringLiteral("适配、折叠、导出"));
    moreBtn->setPopupMode(QToolButton::InstantPopup);
    auto* moreMenu = new QMenu(moreBtn);
    QAction* fitAct = moreMenu->addAction(QStringLiteral("适配全图"));
    QAction* collapseAct = moreMenu->addAction(QStringLiteral("折叠/展开选中节点"));
    moreMenu->addSeparator();
    QAction* pngAct = moreMenu->addAction(QStringLiteral("导出 PNG…"));
    moreBtn->setMenu(moreMenu);
    row->addWidget(moreBtn);
    vbox->addWidget(bar);

    mindView_ = new mind::MindMapView(mindPanel_);
    vbox->addWidget(mindView_, 1);

    auto* hint = new QLabel(
        QStringLiteral("左键点卡片：选中并定位原文　·　左键拖卡片：整理笔记　·　空白处左拖 / 中键拖：移动画布　·　双击：编辑　·　右键：菜单"),
        mindPanel_);
    hint->setStyleSheet(QStringLiteral("color:#6b7280;font-size:11px;padding:2px 8px 4px;"));
    hint->setWordWrap(true);
    vbox->addWidget(hint);

    splitter_->addWidget(mindPanel_);
    splitter_->setStretchFactor(0, 3);
    splitter_->setStretchFactor(1, 2);
    mindPanel_->setMinimumWidth(280);
    // Start in reading mode: opening a document must not silently shrink the page area.
    mindPanel_->hide();

    connect(newGroupBtn, &QPushButton::clicked, this, [this]() {
        const QString parent = mindView_ ? groupIdOf(mindView_->selectedKey()) : QString();
        const QString name = askText(mindView_, QStringLiteral("新建分组"),
                                     QStringLiteral("分组名："), QStringLiteral("新分组"));
        if (name.trimmed().isEmpty()) return;
        if (!createMindGroup(name.trimmed(), parent).isEmpty()) rebuildMindMap();
    });
    connect(fitAct, &QAction::triggered, this, [this]() {
        if (mindView_) mindView_->fitToContent();
    });
    connect(linkBtn, &QPushButton::toggled, this, [this](bool on) {
        if (mindView_) mindView_->setLinkMode(on);
        if (on)
            statusBar()->showMessage(QStringLiteral("连线模式：先后点击两张卡片（Esc 退出）"), 3000);
    });
    connect(collapseAct, &QAction::triggered, this, [this]() {
        if (!mindView_) return;
        const QString key = mindView_->selectedKey();
        if (key.isEmpty()) return;
        mindView_->setCollapsed(key, !mindView_->isCollapsed(key));
    });
    connect(pngAct, &QAction::triggered, this, [this]() {
        const QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                             QStringLiteral("/mindmap");
        const QString path = QFileDialog::getSaveFileName(mindPanel_, QStringLiteral("导出脑图 PNG"),
                                                          base, QStringLiteral("PNG (*.png)"));
        if (path.isEmpty()) return;
        if (mindView_ && mindView_->exportPng(path))
            inform(mindPanel_, QStringLiteral("导出完成"), path);
    });

    connect(mindView_, &mind::MindMapView::linkRequested, this,
            [this](const QString& fromKey, const QString& toKey) {
                const QString scope = mindScopeKey();
                if (scope.isEmpty() || fromKey == toKey) return;
                MindLink l;
                l.id = newId();
                l.topicId = scope;
                l.fromNode = fromKey;
                l.toNode = toKey;
                l.arrow = QStringLiteral("end");
                l.updated = QDateTime::currentMSecsSinceEpoch();
                l.deviceId = store_.deviceId();
                if (store_.upsertMindLink(l)) {
                    undo_->push(new MindLinkCommand(this, MindLinkCommand::Add, MindLink(), l,
                                                     QStringLiteral("添加关联线")));
                    rebuildMindMap();
                    statusBar()->showMessage(QStringLiteral("已添加关联线"), 2000);
                }
            });
    connect(mindView_, &mind::MindMapView::linkMenuRequested, this,
            [this](const QString& linkId, const QPoint& globalPos) {
                QMenu menu(mindView_);
                menu.addAction(QStringLiteral("编辑标签…"), [this, linkId]() {
                    for (const MindLink& current : store_.mindLinks(mindScopeKey())) {
                        if (current.id != linkId) continue;
                        const QString label = askText(mindView_, QStringLiteral("编辑关联线"),
                                                      QStringLiteral("标签："), current.label);
                        if (label == current.label) return;
                        MindLink after = current;
                        after.label = label;
                        after.updated = QDateTime::currentMSecsSinceEpoch();
                        after.deviceId = store_.deviceId();
                        undo_->push(new MindLinkCommand(this, MindLinkCommand::Modify, current, after,
                                                         QStringLiteral("编辑关联线标签")));
                        return;
                    }
                });
                menu.addSeparator();
                QMenu* arrows = menu.addMenu(QStringLiteral("箭头方向"));
                for (const QString& arrow : {QString(), QStringLiteral("start"),
                                              QStringLiteral("end"), QStringLiteral("both")}) {
                    QAction* a = arrows->addAction(arrow.isEmpty() ? QStringLiteral("无") : arrow);
                    connect(a, &QAction::triggered, this, [this, linkId, arrow]() {
                        for (const MindLink& current : store_.mindLinks(mindScopeKey())) {
                            if (current.id != linkId || current.arrow == arrow) continue;
                            MindLink after = current;
                            after.arrow = arrow;
                            after.updated = QDateTime::currentMSecsSinceEpoch();
                            after.deviceId = store_.deviceId();
                            undo_->push(new MindLinkCommand(this, MindLinkCommand::Modify, current, after,
                                                             QStringLiteral("修改关联线箭头")));
                            return;
                        }
                    });
                }
                menu.addAction(QStringLiteral("删除这条关联线"), [this, linkId]() {
                    for (const MindLink& current : store_.mindLinks(mindScopeKey())) {
                        if (current.id != linkId) continue;
                        MindLink deleted = current;
                        deleted.deleted = true;
                        deleted.updated = QDateTime::currentMSecsSinceEpoch();
                        deleted.deviceId = store_.deviceId();
                        undo_->push(new MindLinkCommand(this, MindLinkCommand::Remove, current, deleted,
                                                         QStringLiteral("删除关联线")));
                        return;
                    }
                });
                if (!kHeadless) menu.exec(globalPos);
            });
    connect(mindView_, &mind::MindMapView::containerReparentRequested, this,
            [this](const QString& markupId, const QString& sourcePath,
                   const QString& targetKey, const QString& targetPath) {
        if (!targetKey.startsWith(QStringLiteral("page:")) &&
            !targetKey.startsWith(QStringLiteral("docpage:")) &&
            !targetKey.startsWith(QStringLiteral("document:"))) {
            if (reparentNoteWithinDocument(markupId, targetKey)) rebuildMindMap();
            return;
        }
        if (moveMarkupToContainer(markupId, sourcePath, targetPath, targetKey)) rebuildMindMap();
    });
    connect(mindView_, &mind::MindMapView::nodeReparented, this,
            [this](const QString& markupId, const QString& groupKey) {
                if (applyMindReparent(markupId, groupKey)) rebuildMindMap();
            });
    connect(mindView_, &mind::MindMapView::noteReorderRequested, this,
            [this](const QString& markupId, const QString& groupKey, const QString& beforeId) {
                if (applyMindReorder(markupId, groupKey, beforeId)) rebuildMindMap();
            });
    connect(mindView_, &mind::MindMapView::noteMenuRequested, this,
            [this](const QString& noteId, const QPoint& globalPos) {
        QMenu menu(mindView_);
        menu.addAction(QStringLiteral("编辑标题与批注…"), [this, noteId]() { editMarkupComment(noteId); });
        menu.addAction(QStringLiteral("定位原文"), [this, noteId]() { jumpToMarkup(noteId); });
        menu.addAction(QStringLiteral("删除笔记"), [this, noteId]() {
            if (confirm(mindView_, QStringLiteral("删除笔记"), QStringLiteral("删除这条笔记？可用 Ctrl+Z 撤销。"))) deleteMarkupById(noteId);
        });
        if (!kHeadless) menu.exec(globalPos);
    });
    connect(mindView_, &mind::MindMapView::groupMenuRequested, this,
            [this](const QString& groupKey, const QPoint& globalPos) {
                QString name;
                for (const MindGroup& g : store_.mindGroups(docPath_))
                    if (groupKey.endsWith(g.id)) name = g.name;

                QMenu menu(mindView_);
                menu.addAction(QStringLiteral("重命名分组「%1」…").arg(name), [this, groupKey, name]() {
                    const QString text = askText(mindView_, QStringLiteral("重命名分组"),
                                                 QStringLiteral("分组名："), name);
                    if (text.trimmed().isEmpty()) return;
                    if (renameMindGroup(groupKey, text.trimmed())) rebuildMindMap();
                });
                menu.addAction(QStringLiteral("新建子分组…"), [this, groupKey]() {
                    const QString text = askText(mindView_, QStringLiteral("新建子分组"),
                                                 QStringLiteral("分组名："), QStringLiteral("子分组"));
                    if (text.trimmed().isEmpty()) return;
                    if (!createMindGroup(text.trimmed(), groupIdOf(groupKey)).isEmpty())
                        rebuildMindMap();
                });
                menu.addAction(QStringLiteral("上移一级"), [this, groupKey]() {
                    if (moveMindGroupUp(groupKey)) rebuildMindMap();
                });
                menu.addAction(QStringLiteral("删除分组"), [this, groupKey]() {
                    if (deleteMindGroup(groupKey)) {
                        rebuildMindMap();
                        updateNotesPanel();
                    }
                });
                if (!kHeadless) menu.exec(globalPos);
            });
    // ---- editing requests from the map: the window owns dialogs and undo ----
    connect(mindView_, &mind::MindMapView::renameRequested, this, [this](const QString& key) {
        if (key.startsWith(QStringLiteral("group:"))) {
            QString name;
            for (const MindGroup& g : store_.mindGroups(docPath_))
                if (key.endsWith(g.id)) name = g.name;
            const QString text =
                askText(mindView_, QStringLiteral("重命名分组"), QStringLiteral("分组名："), name);
            if (text.trimmed().isEmpty()) return;
            if (renameMindGroup(key, text.trimmed())) rebuildMindMap();
            return;
        }
        // A note card renames its own comment: the excerpt belongs to the document.
        const QString id = noteIdOfKey(key);
        if (!id.isEmpty()) editMarkupComment(id);
    });
    connect(mindView_, &mind::MindMapView::deleteRequested, this, [this](const QString& key) {
        if (key.startsWith(QStringLiteral("group:"))) {
            if (deleteMindGroup(key)) {
                rebuildMindMap();
                updateNotesPanel();
            }
            return;
        }
        const QString id = noteIdOfKey(key);
        if (!id.isEmpty() && confirm(mindView_, QStringLiteral("删除标注"),
                                     QStringLiteral("删除这条标注？可以用 Ctrl+Z 撤销。")))
            deleteMarkupById(id);
    });
    connect(mindView_, &mind::MindMapView::childRequested, this, [this](const QString& key) {
        if (key.startsWith(QStringLiteral("note:"))) {
            editMarkupComment(key.mid(5));
            return;
        }
        const QString parent = key.startsWith(QStringLiteral("group:")) ? groupIdOf(key)
                                                                        : parentGroupOfNote(key);
        const QString name = askText(mindView_, QStringLiteral("新建子分组"),
                                     QStringLiteral("分组名："), QStringLiteral("新分组"));
        if (name.trimmed().isEmpty()) return;
        if (!createMindGroup(name.trimmed(), parent).isEmpty()) rebuildMindMap();
    });
    connect(mindView_, &mind::MindMapView::siblingRequested, this, [this](const QString& key) {
        const QString name = askText(mindView_, QStringLiteral("新建同级分组"),
                                     QStringLiteral("分组名："), QStringLiteral("新分组"));
        if (name.trimmed().isEmpty()) return;
        QString parent;
        if (key.startsWith(QStringLiteral("group:"))) {
            const QString id = groupIdOf(key);
            for (const MindGroup& g : store_.mindGroups(docPath_))
                if (g.id == id) parent = g.parentId;
        } else {
            parent = parentGroupOfNote(key);
        }
        if (!createMindGroup(name.trimmed(), parent).isEmpty()) rebuildMindMap();
    });

    // Jump to the page but stay in the map: the old modal dialog closed on every click.
    connect(mindView_, &mind::MindMapView::markupActivated, this, [this](const QString& id) {
        jumpToMarkup(id);
        updateStatus();
    });
    connect(mindView_, &mind::MindMapView::nodeActivated, this, [this](int page) {
        if (page < 0) return;
        view_->gotoPage(page);
        updateStatus();
    });
}

QString MainWindow::translationForTest() const
{
    return translationOutput_ ? translationOutput_->toPlainText() : QString();
}

QString MainWindow::statusMessageForTest() const
{
    return statusBar()->currentMessage();
}

bool MainWindow::notesDockVisibleForTest() const
{
    return notesDock_ && notesDock_->isVisible();
}

bool MainWindow::aiDockVisibleForTest() const
{
    return notesDock_ && notesDock_->isVisible() && leftTabs_ && aiPanel_ &&
           leftTabs_->currentWidget() == aiPanel_;
}

bool MainWindow::undoForTest()
{
    if (!undo_ || !undo_->canUndo()) return false;
    undo_->undo();
    return true;
}

bool MainWindow::redoForTest()
{
    if (!undo_ || !undo_->canRedo()) return false;
    undo_->redo();
    return true;
}

QString MainWindow::undoTextForTest() const
{
    return undo_ ? undo_->undoText() : QString();
}

QString MainWindow::noteIdOfKey(const QString& key)
{
    return key.startsWith(QStringLiteral("note:")) ? key.mid(5) : QString();
}

QString MainWindow::parentGroupOfNote(const QString& key) const
{
    const QString noteId = noteIdOfKey(key);
    if (noteId.isEmpty()) return {};
    for (const Markup& m : markups_)
        if (m.id == noteId) return m.mindGroup;
    return {};
}

void MainWindow::rebuildMindMap()
{
    if (!mindView_) return;

    if (!currentTopic_.isEmpty()) {
        // Topic view: one branch per document, so several books share a single map.
        QString topicName;
        for (const Topic& t : store_.topics())
            if (t.id == currentTopic_) topicName = t.name;

        QVector<mind::TopicDocInput> docs;
        for (const TopicDoc& d : store_.topicDocs(currentTopic_)) {
            mind::TopicDocInput in;
            in.title = QFileInfo(d.docPath).completeBaseName();
            for (const DocRow& row : store_.documents())
                if (row.path == d.docPath && !row.title.isEmpty()) in.title = row.title;
            in.docPath = d.docPath;
            // the currently open document is the freshest source of its own notes
            in.markups = (d.docPath == docPath_) ? markups_ : store_.markups(d.docPath);
            docs.append(in);
        }
        mindView_->setLinks(store_.mindLinks(currentTopic_));
        mindView_->setTree(mind::buildFromTopic(topicName, docs,
                                                store_.mindGroupsForTopic(currentTopic_)));
        return;
    }

    if (!engine_.isOpen() || markups_.isEmpty()) {
        mind::Node empty;
        mindView_->setTree(empty);
        return;
    }
    mindView_->setLinks(store_.mindLinks(mindScopeKey()));
    mindView_->setTree(mind::buildFromNotes(QFileInfo(docPath_).completeBaseName(), markups_,
                                            store_.mindGroups(docPath_), true, docPath_));
}

bool MainWindow::removeMindLinkForTest(const QString& id)
{
    for (const MindLink& current : store_.mindLinks(mindScopeKey())) {
        if (current.id != id) continue;
        MindLink deleted = current;
        deleted.deleted = true;
        deleted.updated = QDateTime::currentMSecsSinceEpoch();
        deleted.deviceId = store_.deviceId();
        undo_->push(new MindLinkCommand(this, MindLinkCommand::Remove, current, deleted,
                                        QStringLiteral("删除关联线")));
        return true;
    }
    return false;
}

bool MainWindow::modifyMindLinkForTest(const MindLink& link)
{
    for (const MindLink& current : store_.mindLinks(mindScopeKey())) {
        if (current.id != link.id) continue;
        undo_->push(new MindLinkCommand(this, MindLinkCommand::Modify, current, link,
                                        QStringLiteral("编辑关联线")));
        return true;
    }
    return false;
}

QString MainWindow::mindScopeKey() const
{
    // Links belong to whatever map is on screen: the reading topic when one is open,
    // otherwise this document. Stored in MindLink::topicId, which therefore holds a scope
    // key rather than strictly a topic id.
    return currentTopic_.isEmpty() ? docPath_ : currentTopic_;
}

void MainWindow::setWorkspaceMode(int mode)
{
    ensureMindPanel();
    workspaceMode_ = qBound(0, mode, 2);
    if (!splitter_ || !mindPanel_) return;

    // Space policy: reading is page-first; arranging is page+map; map mode is map-first.
    if (mainToolbar_) {
        for (QAction* action : mainToolbar_->actions()) {
            if (!action || !action->isCheckable() || !action->data().isValid()) continue;
            const int actionMode = action->data().toInt();
            if (actionMode >= 0 && actionMode <= 2)
                action->setChecked(actionMode == workspaceMode_);
        }
    }
    if (notesDock_) {
        if (!currentTopic_.isEmpty()) notesDock_->setWindowTitle(QStringLiteral("笔记本大纲"));
        else notesDock_->setWindowTitle(QStringLiteral("文档目录与笔记"));
        notesDock_->setVisible(workspaceMode_ == 0);
    }

    if (workspaceMode_ == 0) {
        mindPanel_->hide();
        view_->setVisible(currentTopic_.isEmpty() || !store_.topicDocs(currentTopic_).isEmpty());
    } else if (workspaceMode_ == 2) {
        view_->hide();
        mindPanel_->show();
        rebuildMindMap();
    } else {
        view_->show();
        mindPanel_->show();
        if (!mindPanel_->isVisible() || splitter_->sizes().value(0) < 200) {
            const int total = qMax(600, splitter_->width());
            // the page keeps the larger share; the map can be widened by dragging
            splitter_->setSizes({total * 62 / 100, total * 38 / 100});
        }
        rebuildMindMap();
    }
    // Fit once per document/topic; later mode switches keep the user's pan and zoom.
    if (workspaceMode_ != 0 && mindView_) {
        const QString scope = mindScopeKey();
        if (scope != mindFittedScope_) {
            mindFittedScope_ = scope;
            QTimer::singleShot(0, mindView_, [this]() {
                if (mindView_) mindView_->fitToContent();
            });
        }
    }
}

void MainWindow::showNotebookMindMap()
{
    if (!notebookMindWindow_) {
        notebookMindWindow_ = new QDialog(this);
        notebookMindWindow_->setObjectName("notebookMindWindow");
        notebookMindWindow_->resize(1100, 760);
        auto* layout = new QVBoxLayout(notebookMindWindow_);
        auto* actions = new QHBoxLayout;
        auto* fit = new QPushButton(QStringLiteral("适配全图"), notebookMindWindow_);
        auto* undo = new QPushButton(QStringLiteral("撤销"), notebookMindWindow_);
        auto* redo = new QPushButton(QStringLiteral("重做"), notebookMindWindow_);
        auto* link = new QPushButton(QStringLiteral("连线"), notebookMindWindow_);
        link->setCheckable(true);
        auto* exportImage = new QPushButton(QStringLiteral("导出 PNG"), notebookMindWindow_);
        actions->addWidget(fit); actions->addWidget(undo); actions->addWidget(redo);
        actions->addWidget(link); actions->addWidget(exportImage); actions->addStretch();
        auto* interactionHint = new QLabel(QStringLiteral("左键：选中/定位　·　双击：编辑　·　拖卡片：整理　·　中键拖：移动画布　·　右键：菜单"), notebookMindWindow_);
        interactionHint->setObjectName("mindInteractionHint");
        interactionHint->setStyleSheet(QStringLiteral("color:#7a8089;padding:2px 8px;font-size:11px;"));
        layout->addLayout(actions);
        layout->addWidget(interactionHint);
        notebookMindView_ = new mind::MindMapView(notebookMindWindow_);
        notebookMindView_->setObjectName("notebookMindCanvas");
        layout->addWidget(notebookMindView_, 1);
        connect(fit, &QPushButton::clicked, notebookMindView_, &mind::MindMapView::fitToContent);
        connect(link, &QPushButton::toggled, notebookMindView_, &mind::MindMapView::setLinkMode);
        connect(exportImage, &QPushButton::clicked, this, [this]() {
            if (kHeadless) return;
            const QString path = QFileDialog::getSaveFileName(notebookMindWindow_, QStringLiteral("导出脑图"),
                                                              QString(), QStringLiteral("PNG (*.png)"));
            if (!path.isEmpty()) notebookMindView_->exportPng(path);
        });
        connect(notebookMindView_, &mind::MindMapView::linkRequested, this, [this](const QString& from, const QString& to) {
            if (from == to || mindScopeKey().isEmpty()) return;
            MindLink row;
            row.id = newId(); row.topicId = mindScopeKey(); row.fromNode = from; row.toNode = to;
            row.arrow = QStringLiteral("end");
            undo_->push(new MindLinkCommand(this, MindLinkCommand::Add, row, row, QStringLiteral("添加关联线")));
            refreshNotebookWorkspace();
        });
        connect(notebookMindView_, &mind::MindMapView::linkMenuRequested, this,
                [this](const QString& id, const QPoint& pos) {
            QMenu menu(notebookMindWindow_);
            menu.addAction(QStringLiteral("删除关联线"), [this, id]() { removeMindLinkForTest(id); refreshNotebookWorkspace(); });
            menu.exec(pos);
        });
        connect(notebookMindView_, &mind::MindMapView::deleteRequested, this, [this](const QString& key) {
            if (key.startsWith(QStringLiteral("note:")) && activateNotebookNote(key.mid(5))) deleteMarkupById(key.mid(5));
        });
        connect(notebookMindView_, &mind::MindMapView::childRequested, this, [this](const QString& key) {
            if (key.startsWith(QStringLiteral("note:"))) { editMarkupComment(key.mid(5)); return; }
            const QString parent = key.startsWith(QStringLiteral("group:")) ? groupIdOf(key) : QString();
            const QString name = askText(notebookMindWindow_, QStringLiteral("新建子分组"), QStringLiteral("分组名："), QStringLiteral("新分组"));
            if (!name.trimmed().isEmpty()) createMindGroup(name.trimmed(), parent);
        });
        connect(undo, &QPushButton::clicked, this, &MainWindow::undoCommand);
        connect(redo, &QPushButton::clicked, this, &MainWindow::redoCommand);
        connect(notebookMindView_, &mind::MindMapView::markupActivated, this,
                [this](const QString& id) { activateNotebookNote(id); });
        connect(notebookMindView_, &mind::MindMapView::nodeSelected, this,
                [this](const QString& key) { notebookMindView_->setSelectedKey(key); });
        connect(notebookMindView_, &mind::MindMapView::nodeActivated, this,
                [this](int page) { if (page >= 0 && view_) view_->gotoPage(page); });
        connect(notebookMindView_, &mind::MindMapView::pageMenuRequested, this,
                [this](const QString& pageKey, const QString& sourcePath, const QPoint& pos) {
            QMenu menu(notebookMindWindow_);
            menu.addAction(QStringLiteral("在此页新建自由笔记…"), [this, pageKey, sourcePath]() {
                const int page = pageKey.mid(5).section(QLatin1Char(':'), 0, 0).toInt();
                if (sourcePath != docPath_) { saveReadingState(); if (!openDocument(sourcePath)) return; }
                if (page >= 0) view_->gotoPage(page);
                const QString title = askText(notebookMindWindow_, QStringLiteral("新建自由笔记"), QStringLiteral("标题："));
                if (title.trimmed().isEmpty()) return;
                createFreeNoteForPageForTest(title.trimmed(), {}, page);
            });
            if (!kHeadless) menu.exec(pos);
        });
        connect(notebookMindView_, &mind::MindMapView::noteMenuRequested, this,
                [this](const QString& noteId, const QPoint& pos) {
            QMenu menu(notebookMindWindow_);
            menu.addAction(QStringLiteral("编辑标题与批注…"), [this, noteId]() {
                if (activateNotebookNote(noteId)) editMarkupComment(noteId);
            });
            menu.addAction(QStringLiteral("定位原文"), [this, noteId]() { activateNotebookNote(noteId); });
            menu.addAction(QStringLiteral("删除笔记"), [this, noteId]() {
                if (activateNotebookNote(noteId) && confirm(notebookMindWindow_, QStringLiteral("删除笔记"),
                    QStringLiteral("删除这条笔记？可用 Ctrl+Z 撤销。"))) deleteMarkupById(noteId);
            });
            if (!kHeadless) menu.exec(pos);
        });
        connect(notebookMindView_, &mind::MindMapView::groupMenuRequested, this,
                [this](const QString& key, const QPoint& pos) {
            QMenu menu(notebookMindWindow_);
            menu.addAction(QStringLiteral("重命名分组…"), [this, key]() {
                const QString id = groupIdOf(key);
                for (const MindGroup& group : store_.mindGroups(docPath_)) if (group.id == id) {
                    const QString name = askText(notebookMindWindow_, QStringLiteral("重命名分组"), QStringLiteral("名称："), group.name);
                    if (!name.trimmed().isEmpty()) renameMindGroup(id, name);
                }
            });
            menu.addAction(QStringLiteral("新建子分组…"), [this, key]() {
                const QString name = askText(notebookMindWindow_, QStringLiteral("新建子分组"), QStringLiteral("名称："), QStringLiteral("子分组"));
                if (!name.trimmed().isEmpty()) createMindGroup(name, groupIdOf(key));
            });
            menu.exec(pos);
        });
        connect(notebookMindView_, &mind::MindMapView::renameRequested, this, [this](const QString& key) {
            if (key.startsWith(QStringLiteral("note:")) && activateNotebookNote(key.mid(5))) editMarkupComment(key.mid(5));
            if (key.startsWith(QStringLiteral("group:"))) {
                const auto groups = currentTopic_.isEmpty() ? store_.mindGroups(docPath_) : store_.mindGroupsForTopic(currentTopic_);
                for (const MindGroup& group : groups) {
                    if (key != QStringLiteral("group:") + group.id) continue;
                    const QString name = askText(this, QStringLiteral("重命名分组"), QStringLiteral("名称："), group.name);
                    if (name.trimmed().isEmpty()) return;
                    if (group.docPath != docPath_) { saveReadingState(); if (!openDocument(group.docPath)) return; }
                    renameMindGroup(group.id, name);
                    return;
                }
            }
        });
        connect(notebookMindView_, &mind::MindMapView::noteReorderRequested, this,
                [this](const QString& id, const QString& group, const QString& before) {
            for (const QString& path : (currentTopic_.isEmpty() ? QStringList{docPath_} : currentTopicDocPaths()))
                for (const Markup& note : store_.markups(path))
                    if (note.id == id) { moveNotebookNote(id, path, group, before); return; }
        });
    }
    const bool scopeChanged = notebookMindWindow_->property("scope").toString() != mindScopeKey();
    notebookMindWindow_->setProperty("scope", mindScopeKey());
    notebookMindWindow_->setWindowTitle(notebookTitle_->text() + QStringLiteral(" · 脑图"));
    refreshNotebookWorkspace();
    notebookMindWindow_->show();
    notebookMindWindow_->raise();
    // First paint happens after layout; keep the root in view rather than centering
    // the full (potentially very tall) tree off-screen.
    if (scopeChanged) QTimer::singleShot(0, notebookMindView_, [this]() {
        if (!notebookMindView_) return;
        notebookMindView_->fitToContent();
        const QString rootKey = mind::nodeKey(notebookMindView_->tree());
        const QRectF root = notebookMindView_->rectForKey(rootKey);
        if (!root.isEmpty()) notebookMindView_->centerOn(root.center().x() + 240, root.center().y());
    });
}

void MainWindow::showMindMap()
{
    if (!engine_.isOpen()) {
        statusBar()->showMessage(QStringLiteral("请先打开一本书"), 2500);
        return;
    }
    if (markups_.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("当前文档还没有笔记，脑图会是空的"), 3000);
        return;
    }
    setWorkspaceMode(workspaceMode_ == 2 ? 1 : 2);
}

// ---------------------------------------------------------------- library view

bool MainWindow::importNotes(const QString& path, bool intoCurrentDocument, QString* error)
{
    const QString target = (intoCurrentDocument && !docPath_.isEmpty()) ? docPath_ : QString();
    ImportedDocument doc = imports::detect(path, target, error);
    if (doc.markups.isEmpty()) {
        if (error && error->isEmpty()) *error = QStringLiteral("文件里没有可导入的笔记");
        return false;
    }
    const ImportReport report = imports::commit(store_, {doc}, error);
    if (report.notes == 0) return false;

    if (doc.docPath == docPath_) reloadMarkups();
    statusBar()->showMessage(report.summary(), 6000);
    return true;
}


int MainWindow::pendingEmbeddedAnnotations()
{
    if (!engine_.isOpen() || !engine_.supportsAnnotations()) return 0;
    if (docPath_.isEmpty()) return 0;

    // nothing if this revision was already offered
    const QFileInfo fi(docPath_);
    if (store_.setting(identityKey(docPath_)) == QStringLiteral("asked")) return 0;

    // count embedded annotations, capped: a 1000 page book should not stall the open
    const int pages = qMin(engine_.pageCount(), 50);
    int embedded = 0;
    for (int p = 0; p < pages && embedded < 500; ++p)
        embedded += qMax(0, engine_.annotationCount(p));
    if (embedded <= 0) return 0;

    // ... minus what we already have in the library
    ImportedDocument doc = imports::fromPdfAnnotations(engine_, docPath_, markups_);
    return doc.markups.size();
}

int MainWindow::importEmbeddedAnnotations()
{
    if (!engine_.isOpen() || !engine_.supportsAnnotations()) return 0;

    ImportedDocument doc = imports::fromPdfAnnotations(engine_, docPath_, markups_);
    if (doc.markups.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("PDF 里没有新的内嵌标注"), 3000);
        return 0;
    }
    QString err;
    const ImportReport report = imports::commit(store_, {doc}, &err);
    if (report.notes == 0) {
        statusBar()->showMessage(QStringLiteral("导入失败：%1").arg(err), 4000);
        return 0;
    }
    reloadMarkups();
    statusBar()->showMessage(
        QStringLiteral("已从 PDF 读入 %1 条内嵌标注").arg(report.notes), 5000);
    return report.notes;
}

void MainWindow::showConflicts()
{
    auto* dlg = new ConflictDialog(store_, this);
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(dlg, &ConflictDialog::conflictsChanged, this, [this]() {
        reloadMarkups();
        updateNotesPanel();
    });
    dlg->show();
    dlg->raise();
    dlg->activateWindow();
}

void MainWindow::refreshBookshelf()
{
    if (!bookshelfList_ || !topicShelfList_) return;
    bookshelfList_->clear();
    for (const DocRow& doc : store_.documents()) {
        auto* item = new QListWidgetItem(doc.title.isEmpty() ? QFileInfo(doc.path).fileName() : doc.title,
                                         bookshelfList_);
        item->setData(Qt::UserRole, doc.path);
        item->setToolTip(doc.path);
        item->setIcon(style()->standardIcon(QStyle::SP_FileIcon));
        if (doc.path == docPath_) item->setSelected(true);
    }
    topicShelfList_->clear();
    for (const Topic& topic : store_.topics()) {
        auto* item = new QListWidgetItem(topic.name, topicShelfList_);
        item->setData(Qt::UserRole, topic.id);
        item->setToolTip(QStringLiteral("%1 份资料").arg(store_.topicDocs(topic.id).size()));
    }
    if (pageOutlineOnShelf_) {
        pageOutlineOnShelf_->clear();
        if (!docPath_.isEmpty()) {
            std::function<void(QTreeWidgetItem*, const QVector<OutlineItem>&)> fill =
                [&](QTreeWidgetItem* parent, const QVector<OutlineItem>& nodes) {
                    for (const OutlineItem& node : nodes) {
                        auto* item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(pageOutlineOnShelf_);
                        item->setText(0, node.title);
                        item->setData(0, Qt::UserRole + 1, node.page);
                        fill(item, node.children);
                    }
                };
            fill(nullptr, engine_.outline());
        }
    }
}

void MainWindow::showLibrary()
{
    library_ = new LibraryView(store_, this);
    library_->setAttribute(Qt::WA_DeleteOnClose, true);

    connect(library_, &LibraryView::openDocumentRequested, this, [this](const QString& path) {
        currentTopic_.clear();   // opening a plain document leaves topic view
        if (!openDocument(path)) return;
        if (library_) library_->reload();
    });
    connect(library_, &LibraryView::openTopicRequested, this, [this](const QString& topicId) {
        // A notebook may be empty; opening it exposes the add-document entry point.
        QString name;
        for (const Topic& t : store_.topics())
            if (t.id == topicId) name = t.name;
        const QVector<TopicDoc> docs = store_.topicDocs(topicId);
        if (!openNotebook(topicId)) return;
        statusBar()->showMessage(QStringLiteral("主题「%1」· %2 份资料").arg(name).arg(docs.size()),
                                 3500);
        if (library_) library_->reload();
    });
    connect(library_, &LibraryView::openNoteRequested, this,
            [this](const QString& docPath, const QString& markupId) {
                if (docPath != docPath_ && !openDocument(docPath)) return;
                jumpToMarkup(markupId);
                if (library_) library_->close();
            });
    connect(library_, &LibraryView::exportRequested, this,
            [this](const QStringList& docs, const QString& format) {
                QString filter;
                if (format == QStringLiteral("md")) filter = QStringLiteral("Markdown (*.md)");
                else if (format == QStringLiteral("html")) filter = QStringLiteral("HTML (*.html)");
                else if (format == QStringLiteral("opml")) filter = QStringLiteral("OPML (*.opml)");
                else if (format == QStringLiteral("xmind")) filter = QStringLiteral("XMind (*.xmind)");
                else if (format == QStringLiteral("apkg")) filter = QStringLiteral("Anki (*.apkg)");

                const QString base =
                    QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                    QStringLiteral("/选中文档.") + format;
                const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出选中文档"),
                                                                  base, filter);
                if (path.isEmpty()) return;
                QString err;
                if (!exportDocuments(docs, format, path, &err)) {
                    warnUser(this, QStringLiteral("导出失败"), err);
                    return;
                }
                statusBar()->showMessage(
                    QStringLiteral("已导出 %1 个文档的笔记到 %2").arg(docs.size()).arg(path), 5000);
            });

    connect(library_, &LibraryView::libraryChanged, this, [this]() {
        reloadMarkups();   // titles / removed notes may affect the open document
        if (library_) library_->reload();
    });

    connect(library_, &QDialog::finished, this, [this]() { library_ = nullptr; });

    library_->show();
    library_->raise();
    library_->activateWindow();
}

// ---------------------------------------------------------------- P5: read aloud

void MainWindow::buildReaderMenu()
{
    if (!readAction_) return;
    auto* tb = qobject_cast<QToolBar*>(readAction_->parentWidget());
    if (!tb) return;
    auto* btn = qobject_cast<QToolButton*>(tb->widgetForAction(readAction_));
    if (!btn) return;

    auto* menu = new QMenu(btn);
    const QVector<QPair<QString, int>> speeds = {{QStringLiteral("慢（120 字/分）"), 120},
                                                 {QStringLiteral("中（240 字/分）"), 240},
                                                 {QStringLiteral("快（420 字/分）"), 420}};
    for (const auto& s : speeds) {
        const int wpm = s.second;
        menu->addAction(s.first, [this, wpm]() { setReaderSpeed(wpm); });
    }
    menu->addSeparator();
    menu->addAction(QStringLiteral("重读本句"), [this]() {
        if (!reader_.active()) return;
        reader_.previous();
        reader_.next();
    });
    menu->addAction(QStringLiteral("跳过本句"), [this]() {
        if (reader_.active()) reader_.next();
    });
    menu->addAction(QStringLiteral("停止"), [this]() { readAction_->setChecked(false); });

    btn->setMenu(menu);
    btn->setPopupMode(QToolButton::DelayedPopup);

    reader_.setWordsPerMinute(
        store_.setting(QStringLiteral("read.wpm"), QStringLiteral("240")).toInt());
}

void MainWindow::toggleReading()
{
    if (!engine_.isOpen()) {
        readAction_->setChecked(false);
        return;
    }
    if (reader_.active()) {
        reader_.stop();
        view_->setReadingHighlight(-1, {});
        statusBar()->showMessage(QStringLiteral("已停止朗读"), 2000);
        return;
    }

    reader_.setPageProvider([this](int page) { return engine_.textPage(page); },
                            engine_.pageCount());
    const Selection sel = view_->selection();
    const int page = sel.isValid() ? sel.page : view_->currentPage();
    reader_.load(page, 0);
    if (reader_.sentenceCount() == 0) {
        readAction_->setChecked(false);
        statusBar()->showMessage(QStringLiteral("这一页没有可朗读的文本"), 2500);
        return;
    }
    reader_.start();
    statusBar()->showMessage(
        reader_.ttsAvailable()
            ? QStringLiteral("朗读中（%1 句）").arg(reader_.sentenceCount())
            : QStringLiteral("逐句高亮模式（本机没有语音引擎，%1 句）").arg(reader_.sentenceCount()),
        4000);
}

void MainWindow::onReaderSentence(const read::Sentence& sentence)
{
    if (sentence.page < 0) return;
    if (sentence.page != view_->currentPage()) view_->gotoPage(sentence.page);
    view_->setReadingHighlight(sentence.page, sentence.quads);

    const QSizeF ps = engine_.pageSize(sentence.page);
    if (ps.height() > 0 && !sentence.quads.isEmpty()) {
        const double top = sentence.quads.first().boundingRect().top();
        view_->scrollToPageFraction(sentence.page, qBound(0.0, top / ps.height() - 0.1, 0.9));
    }
    updateStatus();
}

// ---------------------------------------------------------------- demo data

void MainWindow::applyDemoMarkups()
{
    if (!engine_.isOpen() || docPath_.isEmpty()) return;
    if (!markups_.isEmpty()) {
        statusBar()->showMessage(QStringLiteral("已存在标注，跳过演示数据注入"), 2000);
        return;
    }

    auto add = [&](int page, int lineIndex, MarkupType type, const QColor& color,
                   const QString& comment, const QStringList& tags) {
        const TextPage* tp = engine_.textPage(page);
        if (!tp || tp->isEmpty()) return;
        int first = -1, last = -1;
        for (int i = 0; i < tp->chars.size(); ++i) {
            if (tp->chars.at(i).line != lineIndex) continue;
            if (first < 0) first = i;
            last = i;
        }
        if (first < 0) return;
        const Selection sel = selectRange(*tp, page, tp->chars.at(first).origin,
                                          tp->chars.at(last).origin);
        if (!sel.isValid()) return;

        Markup m;
        m.id = newId();
        m.docPath = docPath_;
        m.page = page;
        m.type = type;
        m.color = color;
        m.quads = sel.quads;
        m.text = sel.text;
        m.comment = comment;
        m.tags = tags;
        m.created = m.updated = QDateTime::currentMSecsSinceEpoch();
        storeUpsertMarkup(m);
        markups_.prepend(m);
    };

    const TextPage* tp = engine_.textPage(0);
    QVector<QPair<int, int>> lines;
    if (tp) {
        for (const CharBox& c : tp->chars) {
            if (lines.isEmpty() || lines.last().first != c.line) lines.append({c.line, 0});
            lines.last().second += 1;
        }
    }
    QVector<int> candidates;
    for (const auto& l : lines)
        if (l.second >= 25) candidates.append(l.first);
    if (candidates.size() < 4) {
        candidates.clear();
        for (const auto& l : lines)
            if (l.second >= 12) candidates.append(l.first);
    }
    if (candidates.size() < 4) candidates = {1, 2, 3, 4};

    add(0, candidates.at(0), MarkupType::Highlight, QColor("#FFD500"), QString(),
        {QStringLiteral("方法论")});
    add(0, candidates.at(1), MarkupType::Underline, QColor("#34A853"), QString(),
        {QStringLiteral("方法论"), QStringLiteral("待复习")});
    add(0, candidates.at(2), MarkupType::Squiggly, QColor("#D93025"),
        QStringLiteral("这里值得抄一遍"), {});
    add(0, candidates.at(3), MarkupType::Note, QColor("#FF9800"),
        QStringLiteral("这条批注由程序自动生成，用于验证标注层、标签、筛选与持久化。"),
        {QStringLiteral("待复习")});

    // a handwriting stroke so the ink layer is visible in screenshots
    {
        QVector<InkPoint> stroke;
        const QSizeF ps = engine_.pageSize(0);
        for (int i = 0; i < 90; ++i) {
            InkPoint p;
            p.pos = QPointF(ps.width() * 0.12 + i * 3.2,
                            ps.height() * 0.60 + 14.0 * std::sin(i * 0.35));
            stroke.append(p);
        }
        if (stroke.size() >= 2) addInkStroke(0, stroke);
    }

    // an image excerpt on page 1 so the feature is visible in screenshots
    const QSizeF ps = engine_.pageSize(0);
    addImageExcerpt(0, QRectF(ps.width() * 0.1, ps.height() * 0.42, ps.width() * 0.5,
                              ps.height() * 0.12));

    reloadMarkups();
    updateNotesPanel();
    updateStatus();
}

}  // namespace an
