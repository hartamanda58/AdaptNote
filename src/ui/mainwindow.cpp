#include "ui/mainwindow.h"

#include "core/textselect.h"
#include "import/import.h"

#include <QCryptographicHash>
#include "migrate/migrate.h"
#include "export/exporter.h"
#include "ink/ink.h"
#include "mind/mindmap.h"
#include "read/reader.h"
#include "ui/conflictdialog.h"
#include "ui/libraryview.h"
#include "ui/pageview.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDateTime>
#include <QColorDialog>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDockWidget>
#include <QFormLayout>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
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
#include <QTabWidget>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

namespace an {
namespace {

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
        p.setPen(QPen(QColor(0xc7, 0x78, 0x00), 1));
        p.setBrush(QColor(0xff, 0x98, 0x00));
        p.drawRoundedRect(QRectF(3.5, 3.5, 13, 13), 2, 2);
        p.setPen(Qt::white);
        QFont f;
        f.setPixelSize(11);
        f.setBold(true);
        p.setFont(f);
        p.drawText(QRectF(3.5, 3.5, 13, 13), Qt::AlignCenter, "!");
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

// ---------------------------------------------------------------- construction

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
{
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
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
    setWindowTitle(QStringLiteral("AdaptNote — 阅读与标注（自用版 0.2）"));
    resize(1400, 920);

    view_ = new PageView(this);
    view_->setEngine(&engine_);
    setCentralWidget(view_);

    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view_, &QWidget::customContextMenuRequested, this, &MainWindow::showViewContextMenu);
    connect(view_, &PageView::pageChanged, this, [this](int) {
        updateStatus();
        saveReadingState();
    });
    connect(view_, &PageView::zoomChanged, this, [this](float z) {
        updateStatus();
        if (zoomBox_) {
            QSignalBlocker b(zoomBox_);
            zoomBox_->setCurrentIndex(2);
            zoomBox_->setItemText(2, QStringLiteral("%1%").arg(qRound(z * 100)));
        }
    });
    connect(view_, &PageView::selectionChanged, this, [this]() { updateStatus(); });
    connect(view_, &PageView::regionSelected, this, &MainWindow::onRegionSelected);
    connect(view_->verticalScrollBar(), &QScrollBar::valueChanged, this,
            [this](int) { stateTimer_->start(); });
    connect(stateTimer_, &QTimer::timeout, this, &MainWindow::saveReadingState);

    buildToolbar();
    buildFindBar();
    buildDocks();
    buildAiDock();
    buildStatusBar();

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
    tb->setObjectName("mainToolbar");
    tb->setMovable(false);
    tb->setIconSize(QSize(20, 20));
    tb->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);

    QAction* open = tb->addAction(glyphIcon("open"), QStringLiteral("打开"));
    open->setShortcut(QKeySequence::Open);
    connect(open, &QAction::triggered, this, &MainWindow::chooseFile);

    QAction* undoAction = undo_->createUndoAction(this, QStringLiteral("撤销"));
    undoAction->setIcon(glyphIcon("undo"));
    undoAction->setShortcut(QKeySequence::Undo);
    tb->addAction(undoAction);
    QAction* redoAction = undo_->createRedoAction(this, QStringLiteral("重做"));
    redoAction->setIcon(glyphIcon("redo"));
    redoAction->setShortcuts({QKeySequence::Redo, QKeySequence(Qt::CTRL | Qt::SHIFT | Qt::Key_Z)});
    tb->addAction(redoAction);

    tb->addSeparator();
    QAction* prev = tb->addAction(glyphIcon("prev"), QString());
    prev->setShortcut(QKeySequence(Qt::CTRL | Qt::Key_Left));
    connect(prev, &QAction::triggered, this, [this]() { changePage(-1); });
    pageLabel_ = new QLabel(QStringLiteral(" - / - "));
    pageLabel_->setMinimumWidth(84);
    pageLabel_->setAlignment(Qt::AlignCenter);
    tb->addWidget(pageLabel_);
    QAction* next = tb->addAction(glyphIcon("next"), QString());
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

    tb->addWidget(new QLabel(QStringLiteral(" 版式 ")));
    modeBox_ = new QComboBox;
    modeBox_->addItems({QStringLiteral("单页"), QStringLiteral("连续"), QStringLiteral("双页")});
    modeBox_->setCurrentIndex(1);
    tb->addWidget(modeBox_);
    connect(modeBox_, &QComboBox::activated, this, [this](int idx) {
        view_->setLayoutMode(idx == 0 ? PageLayoutMode::Single
                                      : (idx == 1 ? PageLayoutMode::Continuous
                                                  : PageLayoutMode::Double));
    });

    QAction* rot = tb->addAction(glyphIcon("rotate"), QString());
    rot->setToolTip(QStringLiteral("顺时针旋转 90°"));
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

    inkAction_ = tb->addAction(glyphIcon("note", QColor("#1f2328")), QStringLiteral("手写"));
    inkAction_->setToolTip(QStringLiteral("手写笔迹（P）：拖拽书写，再按一次退出"));
    inkAction_->setCheckable(true);
    inkAction_->setShortcut(QKeySequence(Qt::Key_P));
    connect(inkAction_, &QAction::toggled, this, [this](bool on) {
        view_->setInkMode(on);
        statusBar()->showMessage(on ? QStringLiteral("手写模式：拖拽书写（P 退出）")
                                    : QStringLiteral("已退出手写模式"), 2500);
    });

    eraseAction_ = tb->addAction(glyphIcon("delete", QColor("#d93025")), QStringLiteral("橡皮"));
    eraseAction_->setToolTip(QStringLiteral("橡皮（E）：拖过手写笔迹即可擦除"));
    eraseAction_->setCheckable(true);
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

    QAction* mindAction = tb->addAction(glyphIcon("image", QColor("#2a6fd0")), QStringLiteral("脑图"));
    mindAction->setToolTip(QStringLiteral("把当前文档的笔记做成脑图（M）"));
    mindAction->setShortcut(QKeySequence(Qt::Key_M));
    connect(mindAction, &QAction::triggered, this, &MainWindow::showMindMap);

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

    QAction* excerpt = tb->addAction(glyphIcon("image"), QStringLiteral("摘录图"));
    excerpt->setToolTip(QStringLiteral("拖拽框选页面区域，保存为图片摘录（X）"));
    excerpt->setShortcut(QKeySequence(Qt::Key_X));
    connect(excerpt, &QAction::triggered, this, &MainWindow::captureImageExcerpt);

    QAction* color = tb->addAction(glyphIcon("color", markupColor_), QString());
    color->setToolTip(QStringLiteral("选择颜色"));
    connect(color, &QAction::triggered, this, [this]() {
        QStringList names;
        for (const char* c : kColors) names << QString::fromLatin1(c);
        bool ok = false;
        const QString s = QInputDialog::getItem(this, QStringLiteral("标注颜色"),
                                                QStringLiteral("选择颜色："), names, 0, false, &ok);
        if (ok && !s.isEmpty()) markupColor_ = QColor(s);
    });

    QAction* del = tb->addAction(glyphIcon("delete"), QString());
    del->setToolTip(QStringLiteral("删除选中的笔记（Del）"));
    del->setShortcut(QKeySequence::Delete);
    connect(del, &QAction::triggered, this, [this]() {
        if (notesList_ && notesList_->currentItem())
            deleteMarkupById(notesList_->currentItem()->data(Qt::UserRole).toString());
    });

    // export menu
    auto* exportBtn = new QToolButton(tb);
    exportBtn->setText(QStringLiteral("导出"));
    exportBtn->setIcon(glyphIcon("save", QColor("#34A853")));
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
            const auto answer = QMessageBox::question(
                this, QStringLiteral("导入到哪本书"),
                QStringLiteral("把导入的笔记挂到当前打开的《%1》吗？\n\n"
                               "选「No」则按文件里记录的书名另建一条文档记录。")
                    .arg(QFileInfo(docPath_).completeBaseName()),
                QMessageBox::Yes | QMessageBox::No, QMessageBox::Yes);
            intoCurrent = (answer == QMessageBox::Yes);
        }

        QString err;
        if (!importNotes(path, intoCurrent, &err))
            QMessageBox::warning(this, QStringLiteral("导入失败"), err);
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
}

void MainWindow::buildFindBar()
{
    auto* tb = addToolBar(QStringLiteral("查找"));
    tb->setObjectName("findToolbar");
    tb->setMovable(false);
    tb->setIconSize(QSize(18, 18));
    tb->addWidget(new QLabel(QStringLiteral(" 查找 ")));
    findEdit_ = new QLineEdit;
    findEdit_->setPlaceholderText(QStringLiteral("在本书中查找…（Ctrl+F）"));
    findEdit_->setClearButtonEnabled(true);
    findEdit_->setFixedWidth(240);
    tb->addWidget(findEdit_);
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
    connect(findEdit_, &QLineEdit::returnPressed, this, [this]() { nextHit(1); });

    auto* findAction = new QAction(this);
    findAction->setShortcut(QKeySequence::Find);
    addAction(findAction);
    connect(findAction, &QAction::triggered, this, [this]() {
        findEdit_->setFocus();
        findEdit_->selectAll();
    });

    auto* escAction = new QAction(this);
    escAction->setShortcut(QKeySequence(Qt::Key_Escape));
    addAction(escAction);
    connect(escAction, &QAction::triggered, this, [this]() {
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
    auto* dock = new QDockWidget(QStringLiteral("笔记"), this);
    dock->setObjectName("notesDock");
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    leftTabs_ = new QTabWidget(dock);

    outlineTree_ = new QTreeWidget(leftTabs_);
    outlineTree_->setHeaderHidden(true);
    outlineTree_->setColumnCount(1);
    connect(outlineTree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem* item, int) {
        if (!item) return;
        const int page = item->data(0, Qt::UserRole).toInt();
        if (page >= 0) {
            view_->gotoPage(page);
            updateStatus();
        }
    });
    leftTabs_->addTab(outlineTree_, QStringLiteral("大纲"));

    // ---- notes tab: search + tag filter + colour filter + list ----
    auto* notesTab = new QWidget(leftTabs_);
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
    notesList_->setAlternatingRowColors(true);
    vbox->addWidget(notesList_, 1);

    connect(noteSearchEdit_, &QLineEdit::textChanged, this, [this](const QString&) {
        updateNotesPanel();
    });
    connect(tagFilter_, &QComboBox::currentIndexChanged, this, [this](int) { updateNotesPanel(); });
    connect(allDocsBtn, &QToolButton::toggled, this, [this](bool on) {
        searchAllDocs_ = on;
        updateNotesPanel();
    });

    connect(notesList_, &QListWidget::itemClicked, this, [this](QListWidgetItem* item) {
        if (item) jumpToMarkup(item->data(Qt::UserRole).toString());
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
        menu.exec(notesList_->mapToGlobal(pos));
    });

    leftTabs_->addTab(notesTab, QStringLiteral("笔记 (0)"));

    dock->setWidget(leftTabs_);
    dock->setMinimumWidth(320);
    addDockWidget(Qt::LeftDockWidgetArea, dock);
}

void MainWindow::buildStatusBar()
{
    infoLabel_ = new QLabel;
    statusBar()->addPermanentWidget(infoLabel_);
    statusBar()->showMessage(
        QStringLiteral("拖选文字后 H 高亮 / U 下划线 / S 波浪线 / N 批注；X 框选摘录图；"
                       "Ctrl+F 查找；Ctrl+Z 撤销"));
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
    reloadMarkups();
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
    return true;
}

void MainWindow::restoreReadingState()
{
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
}

void MainWindow::closeEvent(QCloseEvent* e)
{
    saveReadingState();
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

void MainWindow::reloadMarkups()
{
    markups_ = store_.markups(docPath_);
    view_->setMarkups(markups_);
    updateFilterWidgets();
    updateNotesPanel();
    updateStatus();
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

void MainWindow::updateNotesPanel()
{
    if (!notesList_) return;
    const QString keepId = notesList_->currentItem()
                               ? notesList_->currentItem()->data(Qt::UserRole).toString()
                               : QString();
    notesList_->clear();
    shown_.clear();

    const QString query = noteSearchEdit_ ? noteSearchEdit_->text().trimmed() : QString();
    QVector<Markup> base;
    if (query.isEmpty())
        base = searchAllDocs_ ? store_.searchMarkups(QString(), QString()) : markups_;
    else
        base = store_.searchMarkups(query, searchAllDocs_ ? QString() : docPath_);

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
        item->setToolTip(QStringLiteral("%1\n%2\n标签：%3")
                             .arg(excerptOf(m, 200), m.comment,
                                  m.tags.isEmpty() ? QStringLiteral("—") : m.tags.join(", ")));
        notesList_->addItem(item);
        ++row;
        if (m.id == keepId) keepRow = row;
    }

    leftTabs_->setTabText(1, QStringLiteral("笔记 (%1)").arg(shown_.size()));
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
    const QString text = askMultiline(this, QStringLiteral("批注"), QStringLiteral("批注内容："),
                                      m->comment);
    if (text.isNull()) return;
    Markup after = *m;
    after.comment = text;
    after.updated = QDateTime::currentMSecsSinceEpoch();
    undo_->push(new MarkupCommand(this, CmdKind::Modify, *m, after, QStringLiteral("编辑批注")));
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
        QMessageBox::warning(this, QStringLiteral("导出失败"), err);
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
        QMessageBox::warning(this, QStringLiteral("导出失败"), err);
        return;
    }
    statusBar()->showMessage(QStringLiteral("已导出 %1 条笔记到 %2").arg(count).arg(path), 5000);
}

// ---------------------------------------------------------------- sync

void MainWindow::openSyncSettings()
{
    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("同步设置（WebDAV）"));
    auto* form = new QFormLayout(&dlg);

    const SyncSettings current = sync_.settings();
    auto* url = new QLineEdit(current.url, &dlg);
    url->setPlaceholderText(QStringLiteral("https://dav.jianguoyun.com/dav 或自建 WebDAV"));
    auto* user = new QLineEdit(current.user, &dlg);
    auto* pass = new QLineEdit(current.password, &dlg);
    pass->setEchoMode(QLineEdit::Password);
    auto* remote = new QLineEdit(current.remotePath, &dlg);
    remote->setPlaceholderText(QStringLiteral("adaptnote"));

    form->addRow(QStringLiteral("服务器地址"), url);
    form->addRow(QStringLiteral("用户名"), user);
    form->addRow(QStringLiteral("密码 / 应用密码"), pass);
    form->addRow(QStringLiteral("远端目录"), remote);

    auto* hint = new QLabel(QStringLiteral(
        "密码以明文保存在本地数据库（自用工具）。同步按书做文件级合并；"
        "两端同时改同一条笔记会保留两个版本，并给副本打上 #conflict 标签。"), &dlg);
    hint->setWordWrap(true);
    hint->setStyleSheet(QStringLiteral("color:#6b7280;font-size:12px;"));
    form->addRow(hint);

    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);

    if (dlg.exec() != QDialog::Accepted) return;

    SyncSettings s;
    s.url = url->text().trimmed();
    s.user = user->text().trimmed();
    s.password = pass->text();
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
        QMessageBox::warning(this, QStringLiteral("同步失败"), rep.error);
        return;
    }

    statusBar()->showMessage(rep.summary(), 6000);
    if (rep.conflicts > 0) {
        QMessageBox::information(
            this, QStringLiteral("同步完成（有冲突）"),
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
    auto* dock = new QDockWidget(QStringLiteral("AI 助手"), this);
    dock->setObjectName("aiDock");
    dock->setAllowedAreas(Qt::LeftDockWidgetArea | Qt::RightDockWidgetArea);

    auto* panel = new QWidget(dock);
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

    aiRunButton_ = new QPushButton(QStringLiteral("对选中的内容提问"), panel);
    vbox->addWidget(aiRunButton_);
    connect(aiRunButton_, &QPushButton::clicked, this, &MainWindow::runAiWithSelection);

    aiReasoningToggle_ = new QToolButton(panel);
    aiReasoningToggle_->setText(QStringLiteral("思考过程"));
    aiReasoningToggle_->setCheckable(true);
    aiReasoningToggle_->setChecked(false);
    vbox->addWidget(aiReasoningToggle_);

    aiReasoning_ = new QTextBrowser(panel);
    aiReasoning_->setMaximumHeight(140);
    aiReasoning_->setVisible(false);
    vbox->addWidget(aiReasoning_);
    connect(aiReasoningToggle_, &QToolButton::toggled, this, [this](bool on) {
        if (aiReasoning_) aiReasoning_->setVisible(on);
    });

    aiOutput_ = new QTextBrowser(panel);
    aiOutput_->setPlaceholderText(QStringLiteral("选中一段文字，点上面的按钮开始"));
    vbox->addWidget(aiOutput_, 1);

    aiInsertButton_ = new QPushButton(QStringLiteral("插入为批注"), panel);
    aiInsertButton_->setEnabled(false);
    vbox->addWidget(aiInsertButton_);
    connect(aiInsertButton_, &QPushButton::clicked, this, &MainWindow::insertAiAsNote);

    dock->setWidget(panel);
    dock->setMinimumWidth(300);
    addDockWidget(Qt::RightDockWidgetArea, dock);

    ai::AiClient::Settings s;
    s.baseUrl = store_.setting(QStringLiteral("ai.url"), QStringLiteral("https://api.deepseek.com"));
    s.apiKey = store_.setting(QStringLiteral("ai.key"));
    s.model = store_.setting(QStringLiteral("ai.model"), QStringLiteral("deepseek-chat"));
    ai_.setSettings(s);

    connect(&ai_, &ai::AiClient::delta, this, [this](const QString& content, const QString& reasoning) {
        aiContent_ += content;
        aiReasoningText_ += reasoning;
        if (aiOutput_) aiOutput_->setPlainText(aiContent_);
        if (aiReasoning_) aiReasoning_->setPlainText(aiReasoningText_);
    });
    connect(&ai_, &ai::AiClient::finished,
            this, [this](const QString& content, const QString& reasoning, const QString& error) {
                aiContent_ = content;
                aiReasoningText_ = reasoning;
                if (aiOutput_) aiOutput_->setPlainText(content);
                if (aiReasoning_) aiReasoning_->setPlainText(reasoning);
                if (aiRunButton_) aiRunButton_->setEnabled(true);
                if (aiInsertButton_) aiInsertButton_->setEnabled(!content.isEmpty());
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
        QMessageBox::warning(this, QStringLiteral("保存失败"), err);
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
    const QString id = store_.ensureMindGroup(docPath_, name, parentId);
    if (!id.isEmpty())
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
    const bool ok = store_.setMindGroupParent(id, grandParent);
    if (ok) statusBar()->showMessage(QStringLiteral("分组已上移一级"), 2500);
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
        const bool ok = store_.upsertMindGroup(store_.stampGroupForLocalWrite(after));
        if (ok) statusBar()->showMessage(QStringLiteral("分组已重命名为「%1」").arg(after.name), 2500);
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
    const bool ok = store_.removeMindGroup(id);
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
        const int page = groupKey.mid(5).toInt();
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
        if (m.deleted) continue;
        const bool sameGroup = group.isEmpty() ? m.mindGroup.isEmpty() : (m.mindGroup == group);
        if (!sameGroup) continue;
        if (group.isEmpty() && groupKey.startsWith(QLatin1String("page:")) &&
            m.page != groupKey.mid(5).toInt())
            continue;
        siblings.append(m);
    }
    // the dragged note must be a member (it may be moving in from another bucket)
    Markup candidate = *moved;
    candidate.mindGroup = group;
    if (groupKey.startsWith(QLatin1String("page:")))
        candidate.page = groupKey.mid(5).toInt();
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
        if (after.id == markupId) after.mindGroup = group;
        if (after.mindGroup == after.mindGroup && after.id == markupId &&
            groupKey.startsWith(QLatin1String("page:")))
            after.page = groupKey.mid(5).toInt();
        if (after.mindOrder == current->mindOrder && after.mindGroup == current->mindGroup &&
            after.page == current->page)
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

    if (m->mindGroup == group) return false;

    int nextOrder = 0;
    for (const Markup& other : markups_) {
        if (other.id == m->id) continue;
        if (other.mindGroup == group && other.mindOrder >= nextOrder) nextOrder = other.mindOrder + 1;
    }

    Markup after = *m;
    after.mindGroup = group;
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

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("脑图 — %1").arg(QFileInfo(docPath_).completeBaseName()));
    dlg.resize(1000, 700);
    auto* vbox = new QVBoxLayout(&dlg);

    auto* view = new mind::MindMapView(&dlg);
    auto rebuild = [this, view]() {
        view->setTree(mind::buildFromNotes(QFileInfo(docPath_).completeBaseName(), markups_,
                                          store_.mindGroups(docPath_)));
    };
    rebuild();
    vbox->addWidget(view, 1);

    auto* row = new QHBoxLayout;
    auto* newGroupBtn = new QPushButton(QStringLiteral("新建分组…"), &dlg);
    connect(newGroupBtn, &QPushButton::clicked, this, [this, rebuild]() {
        bool ok = false;
        const QString name = QInputDialog::getText(this, QStringLiteral("新建分组"),
                                                    QStringLiteral("分组名："), QLineEdit::Normal,
                                                    QStringLiteral("新分组"), &ok);
        if (!ok || name.trimmed().isEmpty()) return;
        if (!createMindGroup(name.trimmed()).isEmpty()) rebuild();
    });
    row->addWidget(newGroupBtn);

    auto* fitBtn = new QPushButton(QStringLiteral("适应窗口"), &dlg);
    connect(fitBtn, &QPushButton::clicked, view, [view]() { view->fitToContent(); });
    row->addWidget(fitBtn);
    auto* zoomIn = new QPushButton(QStringLiteral("放大"), &dlg);
    connect(zoomIn, &QPushButton::clicked, view, [view]() { view->setZoom(view->zoom() * 1.2); });
    row->addWidget(zoomIn);
    auto* zoomOut = new QPushButton(QStringLiteral("缩小"), &dlg);
    connect(zoomOut, &QPushButton::clicked, view, [view]() { view->setZoom(view->zoom() / 1.2); });
    row->addWidget(zoomOut);
    row->addStretch(1);
    auto* pngBtn = new QPushButton(QStringLiteral("导出 PNG…"), &dlg);
    connect(pngBtn, &QPushButton::clicked, this, [view, &dlg]() {
        const QString base = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) +
                             QStringLiteral("/mindmap");
        const QString path = QFileDialog::getSaveFileName(&dlg, QStringLiteral("导出脑图 PNG"), base,
                                                          QStringLiteral("PNG (*.png)"));
        if (path.isEmpty()) return;
        if (view->exportPng(path)) QMessageBox::information(&dlg, QStringLiteral("导出完成"), path);
    });
    row->addWidget(pngBtn);

    auto* exportBtn = new QPushButton(QStringLiteral("导出 XMind…"), &dlg);
    connect(exportBtn, &QPushButton::clicked, this, [this]() { doExport(QStringLiteral("xmind")); });
    row->addWidget(exportBtn);
    vbox->addLayout(row);

    auto* hint = new QLabel(
        QStringLiteral("点击节点跳回原文 · 拖动笔记可换分组或插到某条之前（Alt+↑/↓ 微调顺序）"),
        &dlg);
    hint->setStyleSheet(QStringLiteral("color:#6b7280;font-size:12px;"));
    vbox->addWidget(hint);

    connect(view, &mind::MindMapView::nodeReparented, this,
            [this, view, rebuild](const QString& markupId, const QString& groupKey) {
                if (!applyMindReparent(markupId, groupKey)) return;
                rebuild();
            });

    connect(view, &mind::MindMapView::noteReorderRequested, this,
            [this, rebuild](const QString& markupId, const QString& groupKey,
                            const QString& beforeId) {
                if (!applyMindReorder(markupId, groupKey, beforeId)) return;
                rebuild();
            });

    connect(view, &mind::MindMapView::groupMenuRequested, this,
            [this, view, rebuild](const QString& groupKey, const QPoint& globalPos) {
                QString name;
                for (const MindGroup& g : store_.mindGroups(docPath_))
                    if (groupKey.endsWith(g.id)) name = g.name;

                QMenu menu(view);
                menu.addAction(QStringLiteral("重命名分组「%1」…").arg(name), [this, view, groupKey,
                                                                          name, rebuild]() {
                    bool ok = false;
                    const QString text = QInputDialog::getText(view, QStringLiteral("重命名分组"),
                                                              QStringLiteral("分组名："),
                                                              QLineEdit::Normal, name, &ok);
                    if (!ok || text.trimmed().isEmpty()) return;
                    if (renameMindGroup(groupKey, text.trimmed())) rebuild();
                });
                menu.addAction(QStringLiteral("新建子分组…"), [this, view, groupKey, rebuild]() {
                    bool ok = false;
                    const QString text = QInputDialog::getText(view, QStringLiteral("新建子分组"),
                                                               QStringLiteral("分组名："),
                                                               QLineEdit::Normal,
                                                               QStringLiteral("子分组"), &ok);
                    if (!ok || text.trimmed().isEmpty()) return;
                    if (!createMindGroup(text.trimmed(), groupIdOf(groupKey)).isEmpty()) rebuild();
                });
                menu.addAction(QStringLiteral("上移一级"), [this, groupKey, rebuild]() {
                    if (moveMindGroupUp(groupKey)) rebuild();
                });
                menu.addAction(QStringLiteral("删除分组"), [this, groupKey, rebuild]() {
                    if (deleteMindGroup(groupKey)) {
                        rebuild();
                        updateNotesPanel();
                    }
                });
                menu.exec(globalPos);
            });

    connect(view, &mind::MindMapView::nodeActivated, this, [this, &dlg](int page) {
        if (page < 0) return;
        view_->gotoPage(page);
        updateStatus();
        dlg.accept();   // jump and close so the user sees the page
    });

    dlg.exec();
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

void MainWindow::showLibrary()
{
    library_ = new LibraryView(store_, this);
    library_->setAttribute(Qt::WA_DeleteOnClose, true);

    connect(library_, &LibraryView::openDocumentRequested, this, [this](const QString& path) {
        if (!openDocument(path)) return;
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
                    QMessageBox::warning(this, QStringLiteral("导出失败"), err);
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
