#include "ui/conflictdialog.h"

#include "core/store.h"

#include <QDateTime>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QPushButton>
#include <QSplitter>
#include <QTextBrowser>
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

QString describe(const Markup& m)
{
    QStringList parts;
    parts << QStringLiteral("<b>%1</b>").arg(typeLabel(m.type));
    parts << QStringLiteral("第 %1 页").arg(m.page + 1);
    if (!m.tags.isEmpty())
        parts << QStringLiteral("<span style='color:#7b838d'>#%1</span>")
                     .arg(m.tags.join(QStringLiteral(" #")).toHtmlEscaped());
    parts << QStringLiteral("<span style='color:#7b838d'>%1</span>")
                 .arg(QDateTime::fromMSecsSinceEpoch(m.updated).toString(
                     QStringLiteral("MM-dd HH:mm")));

    QString html = parts.join(QStringLiteral(" · ")) + QStringLiteral("<hr>");
    if (!m.text.trimmed().isEmpty())
        html += QStringLiteral("<p style='white-space:pre-wrap'>%1</p>")
                    .arg(m.text.trimmed().toHtmlEscaped());
    if (!m.comment.trimmed().isEmpty())
        html += QStringLiteral("<p style='color:#0b5;white-space:pre-wrap'>批注：%1</p>")
                    .arg(m.comment.trimmed().toHtmlEscaped());
    if (m.text.trimmed().isEmpty() && m.comment.trimmed().isEmpty())
        html += QStringLiteral("<p style='color:#7b838d'>（没有文本内容）</p>");
    return html;
}

}  // namespace

ConflictDialog::ConflictDialog(Store& store, QWidget* parent)
    : QDialog(parent)
    , store_(store)
{
    setWindowTitle(QStringLiteral("同步冲突"));
    resize(1000, 620);

    auto* vbox = new QVBoxLayout(this);

    summary_ = new QLabel(this);
    summary_->setStyleSheet(QStringLiteral("color:#4a525c;"));
    vbox->addWidget(summary_);

    auto* splitter = new QSplitter(Qt::Horizontal, this);
    list_ = new QListWidget(splitter);
    list_->setMinimumWidth(240);
    splitter->addWidget(list_);

    auto* detail = new QWidget(splitter);
    auto* detailBox = new QVBoxLayout(detail);
    detailBox->setContentsMargins(0, 0, 0, 0);

    auto* columns = new QHBoxLayout;
    auto* leftCol = new QVBoxLayout;
    leftTitle_ = new QLabel(QStringLiteral("原始版本"), detail);
    leftTitle_->setStyleSheet(QStringLiteral("font-weight:600;color:#1b4fa8;"));
    leftCol->addWidget(leftTitle_);
    leftBody_ = new QTextBrowser(detail);
    leftCol->addWidget(leftBody_, 1);
    columns->addLayout(leftCol, 1);

    auto* rightCol = new QVBoxLayout;
    rightTitle_ = new QLabel(QStringLiteral("另一端版本"), detail);
    rightTitle_->setStyleSheet(QStringLiteral("font-weight:600;color:#2a6fd0;"));
    rightCol->addWidget(rightTitle_);
    rightBody_ = new QTextBrowser(detail);
    rightCol->addWidget(rightBody_, 1);
    columns->addLayout(rightCol, 1);

    detailBox->addLayout(columns, 1);

    auto* buttons = new QHBoxLayout;
    keepOriginal_ = new QPushButton(QStringLiteral("保留左侧"), detail);
    keepCopy_ = new QPushButton(QStringLiteral("保留右侧"), detail);
    keepBoth_ = new QPushButton(QStringLiteral("都保留"), detail);
    buttons->addWidget(keepOriginal_);
    buttons->addWidget(keepCopy_);
    buttons->addWidget(keepBoth_);
    buttons->addStretch(1);
    auto* close = new QPushButton(QStringLiteral("关闭"), detail);
    buttons->addWidget(close);
    detailBox->addLayout(buttons);

    splitter->addWidget(detail);
    splitter->setStretchFactor(1, 3);
    vbox->addWidget(splitter, 1);

    connect(list_, &QListWidget::currentRowChanged, this, [this](int) { updateDetails(); });
    connect(keepOriginal_, &QPushButton::clicked, this,
            [this]() { resolve(conflicts::Resolution::KeepOriginal); });
    connect(keepCopy_, &QPushButton::clicked, this,
            [this]() { resolve(conflicts::Resolution::KeepCopy); });
    connect(keepBoth_, &QPushButton::clicked, this,
            [this]() { resolve(conflicts::Resolution::KeepBoth); });
    connect(close, &QPushButton::clicked, this, &QDialog::close);

    reload();
}

void ConflictDialog::reload()
{
    pairs_ = conflicts::find(store_.allMarkups(100000));

    const QString keepId = (list_->currentRow() >= 0 && list_->currentRow() < pairs_.size())
                               ? pairs_.at(list_->currentRow()).copy.id
                               : QString();
    list_->clear();
    for (const conflicts::Pair& p : pairs_) {
        const QString book = QFileInfo(p.copy.docPath).completeBaseName();
        QString text = p.copy.text.simplified();
        if (text.isEmpty()) text = p.copy.comment.simplified();
        if (text.size() > 40) text = text.left(40) + QStringLiteral("…");
        auto* item = new QListWidgetItem(
            QStringLiteral("《%1》P%2 · %3").arg(book).arg(p.copy.page + 1).arg(text));
        item->setData(Qt::UserRole, p.copy.id);
        list_->addItem(item);
        if (p.copy.id == keepId) list_->setCurrentRow(list_->count() - 1);
    }
    if (list_->count() > 0 && list_->currentRow() < 0) list_->setCurrentRow(0);

    summary_->setText(pairs_.isEmpty()
                          ? QStringLiteral("没有待处理的冲突。两端同时改同一条笔记时才会计为冲突。")
                          : QStringLiteral("共 %1 处冲突：同一笔记在两端都被修改过（或一端删除、一端修改）。")
                                .arg(pairs_.size()));
    updateDetails();
}

bool ConflictDialog::selectPair(int index)
{
    if (index < 0 || index >= list_->count()) return false;
    list_->setCurrentRow(index);
    updateDetails();
    return true;
}

void ConflictDialog::updateDetails()
{
    const int row = list_->currentRow();
    const bool valid = row >= 0 && row < pairs_.size();
    const bool hasOriginal = valid && !pairs_.at(row).original.id.isEmpty();

    keepOriginal_->setEnabled(valid && hasOriginal);
    keepCopy_->setEnabled(valid);
    keepBoth_->setEnabled(valid);

    if (!valid) {
        leftTitle_->setText(QStringLiteral("原始版本"));
        rightTitle_->setText(QStringLiteral("另一端版本"));
        leftBody_->clear();
        rightBody_->clear();
        return;
    }

    const conflicts::Pair& p = pairs_.at(row);
    leftTitle_->setText(hasOriginal ? QStringLiteral("原始版本（本机）")
                                    : QStringLiteral("原始版本（已被删除）"));
    rightTitle_->setText(QStringLiteral("另一端版本（冲突副本）"));
    leftBody_->setHtml(hasOriginal ? describe(p.original)
                                   : QStringLiteral("<p style='color:#7b838d'>原始笔记已不存在。</p>"));
    rightBody_->setHtml(describe(p.copy));
}

bool ConflictDialog::resolve(conflicts::Resolution action)
{
    const int row = list_->currentRow();
    if (row < 0 || row >= pairs_.size()) return false;
    const conflicts::Pair pair = pairs_.at(row);

    const conflicts::Plan step =
        conflicts::plan(pair, action, QDateTime::currentMSecsSinceEpoch());
    if (step.write.isEmpty() && step.deleteId.isEmpty()) return false;

    QVector<Markup> batch;
    for (Markup m : step.write) batch.append(store_.stampForLocalWrite(m));
    if (!batch.isEmpty() && !store_.upsertMarkups(batch, nullptr)) return false;
    if (!step.deleteId.isEmpty()) store_.purgeMarkup(step.deleteId);

    reload();
    emit conflictsChanged();
    return true;
}

}  // namespace an
