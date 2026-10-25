#include "ui/libraryview.h"

#include <QCheckBox>
#include <QInputDialog>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QToolButton>
#include <QComboBox>
#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPainter>
#include <QSplitter>
#include <QSet>
#include <QTableWidget>

#include <algorithm>
#include <QVBoxLayout>

namespace an {
namespace {

QString typeLabel(MarkupType t)
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

QIcon swatch(const QColor& c)
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

}  // namespace

LibraryView::LibraryView(Store& store, QWidget* parent)
    : QDialog(parent)
    , store_(store)
{
    setWindowTitle(QStringLiteral("书库 — AdaptNote"));
    resize(1000, 640);

    auto* vbox = new QVBoxLayout(this);
    auto* splitter = new QSplitter(Qt::Horizontal, this);
    vbox->addWidget(splitter, 1);

    // ---------------- documents ----------------
    auto* left = new QWidget(splitter);
    auto* leftBox = new QVBoxLayout(left);
    leftBox->setContentsMargins(0, 0, 0, 0);

    auto* leftHeader = new QLabel(QStringLiteral("文档"), left);
    leftHeader->setStyleSheet(QStringLiteral("font-weight:600;color:#4a525c;padding:4px;"));
    leftBox->addWidget(leftHeader);

    auto* viewRow = new QHBoxLayout;
    viewRow->addWidget(new QLabel(QStringLiteral("视图"), left));
    viewBox_ = new QComboBox(left);
    viewBox_->addItem(QStringLiteral("全部文档"), static_cast<int>(DocFilter::All));
    viewBox_->addItem(QStringLiteral("最近阅读"), static_cast<int>(DocFilter::Recent));
    viewBox_->addItem(QStringLiteral("未读"), static_cast<int>(DocFilter::Unread));
    viewBox_->addItem(QStringLiteral("读了一部分"), static_cast<int>(DocFilter::InProgress));
    viewBox_->addItem(QStringLiteral("有笔记"), static_cast<int>(DocFilter::WithNotes));
    viewBox_->addItem(QStringLiteral("有冲突"), static_cast<int>(DocFilter::WithConflicts));
    viewBox_->addItem(QStringLiteral("按标签"), static_cast<int>(DocFilter::ByTag));
    viewBox_->setToolTip(
        QStringLiteral("「按标签」使用右侧笔记筛选里选中的标签；未选标签时显示全部"));
    viewRow->addWidget(viewBox_, 1);
    leftBox->addLayout(viewRow);

    docs_ = new QTableWidget(left);
    docs_->setColumnCount(4);
    docs_->setHorizontalHeaderLabels(
        {QStringLiteral("书名"), QStringLiteral("笔记"), QStringLiteral("冲突"), QStringLiteral("上次读到")});
    docs_->verticalHeader()->setVisible(false);
    docs_->setSelectionBehavior(QAbstractItemView::SelectRows);
    docs_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    docs_->setEditTriggers(QAbstractItemView::NoEditTriggers);
    docs_->horizontalHeader()->setStretchLastSection(true);
    docs_->setColumnWidth(0, 240);
    docs_->setColumnWidth(1, 60);
    docs_->setColumnWidth(2, 60);
    leftBox->addWidget(docs_, 1);

    auto* manageRow = new QHBoxLayout;
    auto* renameBtn = new QPushButton(QStringLiteral("改名…"), left);
    auto* forgetBtn = new QPushButton(QStringLiteral("从书库移除…"), left);
    auto* exportBtn = new QToolButton(left);
    exportBtn->setText(QStringLiteral("导出选中…"));
    exportBtn->setPopupMode(QToolButton::InstantPopup);

    auto* exportMenu = new QMenu(exportBtn);
    const QVector<QPair<QString, QString>> formats = {
        {QStringLiteral("Markdown (.md)"), QStringLiteral("md")},
        {QStringLiteral("HTML (.html)"), QStringLiteral("html")},
        {QStringLiteral("OPML (.opml)"), QStringLiteral("opml")},
        {QStringLiteral("XMind (.xmind)"), QStringLiteral("xmind")},
        {QStringLiteral("Anki (.apkg)"), QStringLiteral("apkg")},
    };
    for (const auto& f : formats) {
        const QString ext = f.second;
        exportMenu->addAction(f.first, [this, ext]() {
            const QStringList paths = selectedDocumentPaths();
            if (paths.isEmpty()) return;
            emit exportRequested(paths, ext);
        });
    }
    exportBtn->setMenu(exportMenu);

    manageRow->addWidget(renameBtn);
    manageRow->addWidget(forgetBtn);
    manageRow->addWidget(exportBtn);
    manageRow->addStretch(1);
    leftBox->addLayout(manageRow);

    connect(renameBtn, &QPushButton::clicked, this, [this]() {
        const QStringList paths = selectedDocumentPaths();
        if (paths.isEmpty()) return;
        QString current;
        for (const DocRow& r : rows_)
            if (r.path == paths.first()) current = r.title;
        bool ok = false;
        const QString title = QInputDialog::getText(this, QStringLiteral("重命名"),
                                                    QStringLiteral("书名："), QLineEdit::Normal,
                                                    current, &ok);
        if (!ok || title.trimmed().isEmpty()) return;
        if (renameSelected(title.trimmed())) emit libraryChanged();
    });

    connect(forgetBtn, &QPushButton::clicked, this, [this]() {
        const QStringList paths = selectedDocumentPaths();
        if (paths.isEmpty()) return;
        const auto answer = QMessageBox::question(
            this, QStringLiteral("从书库移除"),
            QStringLiteral("从书库移除选中的 %1 个文档？\n\n"
                           "· 笔记会被标记删除（同步时其他设备也会移除）\n"
                           "· 书籍文件本身不会被删除\n"
                           "· 重新打开该文件即可恢复阅读与位置记录")
                .arg(paths.size()),
            QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
        if (answer != QMessageBox::Yes) return;
        if (forgetSelected()) emit libraryChanged();
    });

    splitter->addWidget(left);

    connect(viewBox_, &QComboBox::currentIndexChanged, this, [this](int) {
        filter_ = static_cast<DocFilter>(viewBox_->currentData().toInt());
        reload();
    });
    connect(docs_, &QTableWidget::itemSelectionChanged, this, [this]() { updateNotes(); });
    connect(docs_, &QTableWidget::itemDoubleClicked, this, [this](QTableWidgetItem* item) {
        if (!item) return;
        const int row = item->row();
        if (row >= 0 && row < rows_.size()) emit openDocumentRequested(rows_.at(row).path);
    });

    // ---------------- notes ----------------
    auto* right = new QWidget(splitter);
    auto* rightBox = new QVBoxLayout(right);
    rightBox->setContentsMargins(0, 0, 0, 0);

    auto* filterRow = new QHBoxLayout;
    search_ = new QLineEdit(right);
    search_->setPlaceholderText(QStringLiteral("在整个书库里搜索笔记（全文 / 标签）"));
    search_->setClearButtonEnabled(true);
    filterRow->addWidget(search_, 1);

    tags_ = new QComboBox(right);
    tags_->setMinimumWidth(130);
    filterRow->addWidget(tags_);

    auto* scopeBox = new QCheckBox(QStringLiteral("整个书库"), right);
    scopeBox->setChecked(true);
    filterRow->addWidget(scopeBox);
    rightBox->addLayout(filterRow);

    summary_ = new QLabel(right);
    summary_->setStyleSheet(QStringLiteral("color:#7b838d;font-size:12px;padding:2px 4px;"));
    rightBox->addWidget(summary_);

    notes_ = new QListWidget(right);
    notes_->setAlternatingRowColors(true);
    rightBox->addWidget(notes_, 1);
    splitter->addWidget(right);
    splitter->setStretchFactor(1, 3);

    connect(search_, &QLineEdit::textChanged, this, [this](const QString&) { updateNotes(); });
    connect(tags_, &QComboBox::currentIndexChanged, this, [this](int) {
        if (filter_ == DocFilter::ByTag) {
            reload();
        } else {
            updateNotes();
        }
    });
    connect(scopeBox, &QCheckBox::toggled, this, [this](bool on) {
        wholeLibrary_ = on;
        updateNotes();
    });
    connect(notes_, &QListWidget::itemDoubleClicked, this, [this](QListWidgetItem* item) {
        if (!item) return;
        const QString id = item->data(Qt::UserRole).toString();
        const QString doc = item->data(Qt::UserRole + 1).toString();
        if (!id.isEmpty()) emit openNoteRequested(doc, id);
    });

    auto* hint = new QLabel(QStringLiteral(
        "双击文档打开；双击笔记跳到原文位置。搜索覆盖摘录、批注与标签。"), this);
    hint->setStyleSheet(QStringLiteral("color:#7b838d;font-size:12px;"));
    vbox->addWidget(hint);

    reload();
}

void LibraryView::reload()
{
    const QString keepPath = [this]() {
        const int row = docs_->currentRow();
        return (row >= 0 && row < rows_.size()) ? rows_.at(row).path : QString();
    }();

    allRows_ = store_.documents();
    taggedDocs_.clear();
    if (filter_ == DocFilter::ByTag && tags_) {
        const QString tag = tags_->currentData().toString();
        if (!tag.isEmpty()) taggedDocs_ = store_.documentPathsWithTag(tag);
    }
    rows_ = filteredRows();

    docs_->setRowCount(rows_.size());
    int keepRow = -1;
    for (int i = 0; i < rows_.size(); ++i) {
        const DocRow& r = rows_.at(i);
        auto set = [&](int col, const QString& text) {
            auto* item = new QTableWidgetItem(text);
            item->setToolTip(r.path);
            docs_->setItem(i, col, item);
        };
        set(0, r.title);
        set(1, QString::number(r.noteCount));
        set(2, r.conflictCount > 0 ? QString::number(r.conflictCount) : QStringLiteral("—"));
        set(3, r.pages > 0
                    ? QStringLiteral("%1 / %2").arg(r.lastPage + 1).arg(r.pages)
                    : QStringLiteral("—"));
        if (r.path == keepPath) keepRow = i;
    }
    if (keepRow < 0 && !rows_.isEmpty()) keepRow = 0;
    if (keepRow >= 0) docs_->selectRow(keepRow);

    // tag list for the current scope (doc selection or whole library)
    const QString keepTag = tags_->currentData().toString();
    {
        QSignalBlocker b(tags_);
        tags_->clear();
        tags_->addItem(QStringLiteral("全部标签"), QString());
        for (const auto& t : store_.tagCounts(scopePath()))
            tags_->addItem(QStringLiteral("%1 (%2)").arg(t.first).arg(t.second), t.first);
        const int idx = tags_->findData(keepTag);
        tags_->setCurrentIndex(idx >= 0 ? idx : 0);
    }

    updateNotes();
}

QVector<DocRow> LibraryView::filteredRows() const
{
    if (filter_ == DocFilter::All) return allRows_;

    QVector<DocRow> out;
    for (const DocRow& r : allRows_) {
        const bool finished = r.pages > 0 && r.lastPage + 1 >= r.pages;
        const int readPages = r.lastPage + 1;
        switch (filter_) {
        case DocFilter::All:
            out.append(r);
            break;
        case DocFilter::Recent:
            if (r.openedAt > 0 && !finished) out.append(r);
            break;
        case DocFilter::Unread:
            if (r.openedAt == 0 && r.noteCount == 0) out.append(r);
            break;
        case DocFilter::InProgress:
            if (r.pages > 0 && readPages > 1 && !finished) out.append(r);
            break;
        case DocFilter::WithNotes:
            if (r.noteCount > 0) out.append(r);
            break;
        case DocFilter::WithConflicts:
            if (r.conflictCount > 0) out.append(r);
            break;
        case DocFilter::ByTag: {
            const QString tag = tags_ ? tags_->currentData().toString() : QString();
            if (tag.isEmpty()) {
                out = allRows_;   // no tag chosen -> behave like "all"
            } else if (taggedDocs_.contains(r.path)) {
                out.append(r);
            }
            break;
        }
        }
    }
    return out;
}

void LibraryView::setDocFilter(DocFilter filter)
{
    filter_ = filter;
    if (viewBox_) {
        const int idx = viewBox_->findData(static_cast<int>(filter));
        if (idx >= 0) {
            QSignalBlocker b(viewBox_);
            viewBox_->setCurrentIndex(idx);
        }
    }
    reload();
}

int LibraryView::documentRowCount() const
{
    return docs_ ? docs_->rowCount() : 0;
}

QString LibraryView::scopePath() const
{
    if (wholeLibrary_) return {};
    const int row = docs_->currentRow();
    if (row < 0 || row >= rows_.size()) return {};
    return rows_.at(row).path;
}

void LibraryView::updateNotes()
{
    if (!notes_) return;
    notes_->clear();
    shown_.clear();

    const QString query = search_ ? search_->text().trimmed() : QString();
    const QString tag = tags_ ? tags_->currentData().toString() : QString();
    const QString scope = scopePath();

    QVector<Markup> base = store_.searchMarkups(query, scope);
    int listed = 0;
    for (const Markup& m : base) {
        if (!tag.isEmpty() && !m.tags.contains(tag)) continue;
        shown_.insert(m.id, m);

        QString text = QStringLiteral("[%1] P%2 · %3 · %4")
                           .arg(QFileInfo(m.docPath).completeBaseName())
                           .arg(m.page + 1)
                           .arg(typeLabel(m.type))
                           .arg(m.text.simplified().left(60));
        if (m.type == MarkupType::Ink && m.text.simplified().isEmpty())
            text = QStringLiteral("[%1] P%2 · 手写标注")
                       .arg(QFileInfo(m.docPath).completeBaseName())
                       .arg(m.page + 1);
        if (!m.tags.isEmpty()) text += QStringLiteral("  #%1").arg(m.tags.join(QStringLiteral(" #")));
        if (!m.comment.isEmpty())
            text += QStringLiteral("  〔%1〕").arg(m.comment.simplified().left(24));

        auto* item = new QListWidgetItem(swatch(m.color), text);
        item->setData(Qt::UserRole, m.id);
        item->setData(Qt::UserRole + 1, m.docPath);
        item->setToolTip(QStringLiteral("%1\n%2").arg(m.text.simplified().left(300), m.comment));
        notes_->addItem(item);
        ++listed;
    }

    if (summary_) {
        summary_->setText(QStringLiteral("%1 个文档 · %2 条笔记%3")
                              .arg(rows_.size())
                              .arg(listed)
                              .arg(query.isEmpty() && tag.isEmpty() ? QString()
                                                                   : QStringLiteral("（已筛选）")));
    }
}

int LibraryView::documentCount() const
{
    return rows_.size();
}

int LibraryView::noteCount() const
{
    return notes_ ? notes_->count() : 0;
}

QStringList LibraryView::listedNoteIds() const
{
    QStringList ids;
    if (!notes_) return ids;
    for (int i = 0; i < notes_->count(); ++i)
        ids.append(notes_->item(i)->data(Qt::UserRole).toString());
    return ids;
}

void LibraryView::setSearchText(const QString& text)
{
    if (search_) search_->setText(text);
}

void LibraryView::setTagFilter(const QString& tag)
{
    if (!tags_) return;
    if (tag.isEmpty()) {
        tags_->setCurrentIndex(0);
        return;
    }
    const int idx = tags_->findData(tag);
    if (idx >= 0) tags_->setCurrentIndex(idx);
    updateNotes();
}

void LibraryView::setWholeLibrary(bool on)
{
    wholeLibrary_ = on;
    updateNotes();
}

bool LibraryView::selectDocumentRow(int row)
{
    if (!docs_ || row < 0 || row >= docs_->rowCount()) return false;
    docs_->selectRow(row);
    updateNotes();
    return true;
}

QStringList LibraryView::selectedDocumentPaths() const
{
    QStringList paths;
    if (!docs_) return paths;
    QSet<int> rows;
    for (const QModelIndex& idx : docs_->selectionModel()->selectedRows()) rows.insert(idx.row());
    QVector<int> sorted(rows.begin(), rows.end());
    std::sort(sorted.begin(), sorted.end());
    for (int row : sorted)
        if (row >= 0 && row < rows_.size()) paths.append(rows_.at(row).path);
    return paths;
}

bool LibraryView::renameSelected(const QString& newTitle)
{
    const QStringList paths = selectedDocumentPaths();
    if (paths.isEmpty() || newTitle.trimmed().isEmpty()) return false;
    if (!store_.renameDocument(paths.first(), newTitle.trimmed())) return false;
    reload();
    return true;
}

bool LibraryView::forgetSelected(int* notesRemoved)
{
    const QStringList paths = selectedDocumentPaths();
    if (paths.isEmpty()) return false;
    int removed = 0;
    bool ok = true;
    for (const QString& path : paths) {
        int n = 0;
        if (store_.forgetDocument(path, &n))
            removed += n;
        else
            ok = false;
    }
    if (notesRemoved) *notesRemoved = removed;
    reload();
    return ok;
}

void LibraryView::activateCurrentNote()
{
    if (!notes_ || !notes_->currentItem()) return;
    const QString id = notes_->currentItem()->data(Qt::UserRole).toString();
    const QString doc = notes_->currentItem()->data(Qt::UserRole + 1).toString();
    if (!id.isEmpty()) emit openNoteRequested(doc, id);
}

}  // namespace an
