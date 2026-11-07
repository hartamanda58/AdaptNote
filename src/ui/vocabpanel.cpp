#include "ui/vocabpanel.h"

#include "core/store.h"
#include "export/exporter.h"
#include "vocab/vocab.h"

#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QStyledItemDelegate>
#include <QTextBrowser>
#include <QToolButton>
#include <QVBoxLayout>

#include <algorithm>

#ifdef ADAPT_HAVE_TTS
#include <QTextToSpeech>
#endif

namespace an {
namespace {

const bool kHeadless = qEnvironmentVariableIsSet("ADAPT_HEADLESS");

QString htmlEscape(QString s)
{
    s.replace(QLatin1Char('&'), QStringLiteral("&amp;"));
    s.replace(QLatin1Char('<'), QStringLiteral("&lt;"));
    s.replace(QLatin1Char('>'), QStringLiteral("&gt;"));
    return s;
}

// Card row: word / phonetic / meaning with the review state on the right. Same
// visual language as the excerpt outline (rounded rows, no grid lines).
class WordDelegate : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem& option, const QModelIndex&) const override
    {
        return QSize(option.rect.width(), 58);
    }

    void paint(QPainter* painter, const QStyleOptionViewItem& option,
               const QModelIndex& index) const override
    {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, true);
        const QRect row = option.rect.adjusted(4, 2, -5, -2);
        const bool selected = option.state & QStyle::State_Selected;
        const bool hovered = option.state & QStyle::State_MouseOver;
        if (selected || hovered) {
            painter->setPen(selected ? QColor("#b9d4ff") : Qt::NoPen);
            painter->setBrush(selected ? QColor("#dfeaff") : QColor("#f2f4f7"));
            painter->drawRoundedRect(row, 9, 9);
        }

        const QString word = index.data(Qt::UserRole).toString();
        const QString phonetic = index.data(Qt::UserRole + 1).toString();
        const QString meaning = index.data(Qt::UserRole + 2).toString();
        const QString due = index.data(Qt::UserRole + 3).toString();
        const bool isDue = index.data(Qt::UserRole + 4).toBool();

        int left = row.left() + 10;
        QFont wordFont = option.font;
        wordFont.setPointSizeF(11.5);
        wordFont.setWeight(QFont::DemiBold);
        painter->setFont(wordFont);
        painter->setPen(QColor("#1d1d1f"));
        const QFontMetrics wm(wordFont);
        painter->drawText(QRect(left, row.top() + 7, row.width() - 80, 18),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          wm.elidedText(word, Qt::ElideRight, row.width() - 90));

        QFont small = option.font;
        small.setPointSizeF(8.5);
        painter->setFont(small);
        painter->setPen(QColor("#8a8a8e"));
        QString meta = phonetic;
        if (!meaning.isEmpty()) meta += (meta.isEmpty() ? QString() : QStringLiteral("  ")) + meaning;
        painter->drawText(QRect(left, row.top() + 27, row.width() - 80, 16),
                          Qt::AlignLeft | Qt::AlignVCenter,
                          painter->fontMetrics().elidedText(meta, Qt::ElideRight, row.width() - 96));

        QFont badgeFont = option.font;
        badgeFont.setPointSizeF(8.0);
        painter->setFont(badgeFont);
        const int badgeWidth = painter->fontMetrics().horizontalAdvance(due) + 14;
        const QRect badge(row.right() - badgeWidth - 8, row.top() + 9, badgeWidth, 18);
        painter->setPen(Qt::NoPen);
        painter->setBrush(isDue ? QColor("#ffe3b0") : QColor("#eceef2"));
        painter->drawRoundedRect(badge, 9, 9);
        painter->setPen(isDue ? QColor("#8a5a00") : QColor("#8a8a8e"));
        painter->drawText(badge, Qt::AlignCenter, due);

        // Box dots: five boxes, filled up to the current one.
        const int box = index.data(Qt::UserRole + 5).toInt();
        for (int i = 0; i < 5; ++i) {
            const QRect dot(row.right() - 8 - (5 - i) * 9, row.top() + 36, 5, 5);
            painter->setPen(Qt::NoPen);
            painter->setBrush(i < box ? QColor("#0a84ff") : QColor("#d9dde3"));
            painter->drawEllipse(dot);
        }
        painter->restore();
    }
};

// Focused review session: reveal the meaning, then grade. Keyboard: Space reveals,
// 1 / ← = 不认识, 2 / → = 认识, Esc closes. Deliberately small and modal so the
// reader's context is not lost for long.
class ReviewDialog : public QDialog {
public:
    ReviewDialog(Store& store, QVector<WordEntry> queue, QWidget* parent)
        : QDialog(parent)
        , store_(store)
        , queue_(std::move(queue))
    {
        setWindowTitle(QStringLiteral("生词复习"));
        resize(460, 380);
        auto* layout = new QVBoxLayout(this);
        layout->setSpacing(8);

        progress_ = new QLabel(this);
        progress_->setObjectName("panelHint");
        layout->addWidget(progress_);

        word_ = new QLabel(this);
        word_->setAlignment(Qt::AlignCenter);
        word_->setWordWrap(true);
        word_->setStyleSheet(QStringLiteral("font-size:26px;font-weight:650;color:#1d1d1f;"));
        layout->addWidget(word_);

        phonetic_ = new QLabel(this);
        phonetic_->setAlignment(Qt::AlignCenter);
        phonetic_->setStyleSheet(QStringLiteral("color:#8a8a8e;font-size:13px;"));
        layout->addWidget(phonetic_);

        answer_ = new QTextBrowser(this);
        answer_->setMinimumHeight(130);
        answer_->setOpenExternalLinks(false);
        layout->addWidget(answer_, 1);

        hint_ = new QLabel(QStringLiteral("空格显示答案 · 1 不认识 · 2 认识 · Esc 关闭"), this);
        hint_->setObjectName("panelHint");
        hint_->setAlignment(Qt::AlignCenter);
        layout->addWidget(hint_);

        auto* row = new QHBoxLayout;
        reveal_ = new QPushButton(QStringLiteral("显示答案"), this);
        reveal_->setObjectName("primaryButton");
        unknown_ = new QPushButton(QStringLiteral("1 · 不认识"), this);
        known_ = new QPushButton(QStringLiteral("2 · 认识"), this);
        known_->setObjectName("primaryButton");
        row->addWidget(reveal_, 1);
        row->addWidget(unknown_, 1);
        row->addWidget(known_, 1);
        layout->addLayout(row);

        connect(reveal_, &QPushButton::clicked, this, &ReviewDialog::revealAnswer);
        connect(unknown_, &QPushButton::clicked, this, [this]() { grade(false); });
        connect(known_, &QPushButton::clicked, this, [this]() { grade(true); });

        showCurrent();
    }

protected:
    void keyPressEvent(QKeyEvent* event) override
    {
        if (finished_) {
            QDialog::keyPressEvent(event);
            return;
        }
        if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return) {
            if (!revealed_) revealAnswer();
            return;
        }
        if (event->key() == Qt::Key_1 || event->key() == Qt::Key_Left) {
            if (revealed_) grade(false);
            return;
        }
        if (event->key() == Qt::Key_2 || event->key() == Qt::Key_Right) {
            if (revealed_) grade(true);
            return;
        }
        QDialog::keyPressEvent(event);
    }

private:
    void showCurrent()
    {
        if (index_ >= queue_.size()) {
            finish();
            return;
        }
        const WordEntry& w = queue_.at(index_);
        progress_->setText(QStringLiteral("%1 / %2 · 第 %3 盒")
                               .arg(index_ + 1)
                               .arg(queue_.size())
                               .arg(w.box));
        word_->setText(w.word);
        phonetic_->setText(w.phonetic.isEmpty() ? QString()
                                                : QStringLiteral("[%1]").arg(w.phonetic));
        QString html;
        if (!w.meaning.isEmpty())
            html += QStringLiteral("<div style=\"font-size:15px\">%1</div>")
                        .arg(htmlEscape(w.meaning).replace(QLatin1Char('\n'), QStringLiteral("<br>")));
        if (!w.example.isEmpty())
            html += QStringLiteral("<div style=\"margin-top:8px;color:#6b6b70;font-size:12px\">%1</div>")
                        .arg(htmlEscape(w.example));
        if (html.isEmpty())
            html = QStringLiteral("<div style=\"color:#8a8a8e\">（这个词还没有释义，可在面板里编辑）</div>");
        answer_->setHtml(html);
        answer_->hide();
        reveal_->show();
        unknown_->hide();
        known_->hide();
        revealed_ = false;
    }

    void revealAnswer()
    {
        if (finished_ || revealed_) return;
        revealed_ = true;
        answer_->show();
        reveal_->hide();
        unknown_->show();
        known_->show();
        known_->setFocus();
    }

    void grade(bool remembered)
    {
        if (finished_ || !revealed_ || index_ >= queue_.size()) return;
        store_.reviewWord(queue_.at(index_).id, remembered);
        if (remembered)
            ++knownCount_;
        else
            ++unknownCount_;
        ++index_;
        showCurrent();
    }

    void finish()
    {
        finished_ = true;
        progress_->setText(QStringLiteral("本轮完成"));
        word_->setText(QStringLiteral("复习了 %1 张 · 认识 %2 · 不认识 %3")
                           .arg(queue_.size())
                           .arg(knownCount_)
                           .arg(unknownCount_));
        word_->setStyleSheet(QStringLiteral("font-size:17px;font-weight:600;color:#1d1d1f;"));
        phonetic_->clear();
        answer_->hide();
        reveal_->hide();
        unknown_->hide();
        known_->hide();
        auto* close = new QPushButton(QStringLiteral("完成"), this);
        close->setObjectName("primaryButton");
        connect(close, &QPushButton::clicked, this, &QDialog::accept);
        static_cast<QVBoxLayout*>(layout())->addWidget(close);
        close->setFocus();
    }

    Store& store_;
    QVector<WordEntry> queue_;
    int index_ = 0;
    int knownCount_ = 0;
    int unknownCount_ = 0;
    bool revealed_ = false;
    bool finished_ = false;
    QLabel* progress_ = nullptr;
    QLabel* word_ = nullptr;
    QLabel* phonetic_ = nullptr;
    QLabel* hint_ = nullptr;
    QTextBrowser* answer_ = nullptr;
    QPushButton* reveal_ = nullptr;
    QPushButton* unknown_ = nullptr;
    QPushButton* known_ = nullptr;
};

}  // namespace

VocabPanel::VocabPanel(Store& store, QWidget* parent)
    : QWidget(parent)
    , store_(store)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(8, 8, 8, 8);
    layout->setSpacing(6);

    auto* head = new QHBoxLayout;
    auto* title = new QLabel(QStringLiteral("生词本"), this);
    title->setObjectName("panelTitle");
    stats_ = new QLabel(this);
    stats_->setObjectName("panelHint");
    head->addWidget(title);
    head->addStretch(1);
    head->addWidget(stats_);
    layout->addLayout(head);

    auto* lookupRow = new QHBoxLayout;
    input_ = new QLineEdit(this);
    input_->setPlaceholderText(QStringLiteral("输入单词或短语，回车查词"));
    input_->setClearButtonEnabled(true);
    lookupButton_ = new QPushButton(QStringLiteral("查词"), this);
    lookupButton_->setObjectName("primaryButton");
    lookupRow->addWidget(input_, 1);
    lookupRow->addWidget(lookupButton_);
    layout->addLayout(lookupRow);

    auto* tools = new QHBoxLayout;
    filterButton_ = new QToolButton(this);
    filterButton_->setText(QStringLiteral("待复习"));
    filterButton_->setCheckable(true);
    filterButton_->setToolTip(QStringLiteral("只显示到期需要复习的词"));
    reviewButton_ = new QPushButton(QStringLiteral("开始复习"), this);
    reviewButton_->setObjectName("primaryButton");
    reviewButton_->setToolTip(
        QStringLiteral("按五盒间隔重复复习到期生词：空格显示答案，1 不认识，2 认识"));
    tools->addWidget(reviewButton_);
    tools->addWidget(filterButton_);
    connect(reviewButton_, &QPushButton::clicked, this, &VocabPanel::startReview);
    exportButton_ = new QToolButton(this);
    exportButton_->setText(QStringLiteral("导出"));
    exportButton_->setPopupMode(QToolButton::InstantPopup);
    auto* menu = new QMenu(exportButton_);
    menu->addAction(QStringLiteral("TSV（Excel / 通用）"), this, [this]() { exportWords(false); });
    menu->addAction(QStringLiteral("Anki 导入文本"), this, [this]() { exportWords(true); });
    menu->addAction(QStringLiteral("Anki 卡包（.apkg）"), this, [this]() { exportApkg(); });
    exportButton_->setMenu(menu);
    tools->addWidget(exportButton_);
    tools->addStretch(1);
    layout->addLayout(tools);

    list_ = new QListWidget(this);
    list_->setObjectName("vocabList");
    list_->setItemDelegate(new WordDelegate(list_));
    list_->setMouseTracking(true);
    list_->setSelectionMode(QAbstractItemView::SingleSelection);
    list_->setWordWrap(false);
    layout->addWidget(list_, 1);

    detail_ = new QTextBrowser(this);
    detail_->setObjectName("vocabDetail");
    detail_->setMinimumHeight(86);
    detail_->setMaximumHeight(160);
    detail_->setPlaceholderText(QStringLiteral("选中一个词查看释义、例句和出处"));
    layout->addWidget(detail_);

    auto* actions = new QHBoxLayout;
    knownButton_ = new QPushButton(QStringLiteral("认识"), this);
    unknownButton_ = new QPushButton(QStringLiteral("不认识"), this);
    pronounceButton_ = new QPushButton(QStringLiteral("发音"), this);
    editButton_ = new QPushButton(QStringLiteral("编辑"), this);
    deleteButton_ = new QPushButton(QStringLiteral("删除"), this);
    sourceButton_ = new QPushButton(QStringLiteral("回原文"), this);
    actions->addWidget(knownButton_);
    actions->addWidget(unknownButton_);
    actions->addStretch(1);
    actions->addWidget(sourceButton_);
    actions->addWidget(pronounceButton_);
    actions->addWidget(editButton_);
    actions->addWidget(deleteButton_);
    layout->addLayout(actions);

    for (QPushButton* b : {pronounceButton_, editButton_, deleteButton_, sourceButton_,
                           knownButton_, unknownButton_})
        b->setEnabled(false);

    connect(lookupButton_, &QPushButton::clicked, this, [this]() {
        const QString term = input_->text().trimmed();
        if (term.isEmpty()) return;
        emit lookupRequested(term);
    });
    connect(input_, &QLineEdit::returnPressed, this, [this]() {
        const QString term = input_->text().trimmed();
        if (!term.isEmpty()) emit lookupRequested(term);
    });
    connect(filterButton_, &QToolButton::toggled, this, [this](bool on) {
        dueOnly_ = on;
        reload();
    });
    connect(list_, &QListWidget::currentItemChanged, this, [this](QListWidgetItem*, QListWidgetItem*) {
        updateDetail();
    });
    connect(knownButton_, &QPushButton::clicked, this, [this]() { reviewSelected(true); });
    connect(unknownButton_, &QPushButton::clicked, this, [this]() { reviewSelected(false); });
    connect(pronounceButton_, &QPushButton::clicked, this, &VocabPanel::pronounceSelected);
    connect(editButton_, &QPushButton::clicked, this, &VocabPanel::editSelected);
    connect(deleteButton_, &QPushButton::clicked, this, &VocabPanel::deleteSelected);
    connect(sourceButton_, &QPushButton::clicked, this, &VocabPanel::openSourceSelected);

#ifdef ADAPT_HAVE_TTS
    if (!QTextToSpeech::availableEngines().isEmpty()) speech_ = new QTextToSpeech(this);
#endif

    reload();
}

VocabPanel::~VocabPanel() = default;

void VocabPanel::focusLookup(const QString& seed)
{
    if (!seed.trimmed().isEmpty()) input_->setText(seed.trimmed());
    input_->setFocus();
    input_->selectAll();
}

void VocabPanel::selectWord(const QString& word)
{
    if (!list_ || word.trimmed().isEmpty()) return;
    for (int i = 0; i < list_->count(); ++i) {
        QListWidgetItem* item = list_->item(i);
        if (item->data(Qt::UserRole).toString().compare(word.trimmed(), Qt::CaseInsensitive) == 0) {
            list_->setCurrentItem(item);
            list_->scrollToItem(item);
            return;
        }
    }
}

void VocabPanel::reload()
{
    const QString keepId = selectedWord().id;
    words_ = store_.words();
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    int due = 0;
    for (const WordEntry& w : words_)
        if (w.dueAt <= now) ++due;

    stats_->setText(QStringLiteral("%1 词 · %2 待复习").arg(words_.size()).arg(due));
    if (reviewButton_) {
        reviewButton_->setEnabled(due > 0);
        reviewButton_->setText(due > 0 ? QStringLiteral("开始复习 (%1)").arg(due)
                                       : QStringLiteral("开始复习"));
    }

    list_->clear();
    for (const WordEntry& w : words_) {
        if (dueOnly_ && w.dueAt > now) continue;
        auto* item = new QListWidgetItem(list_);
        item->setData(Qt::UserRole, w.word);
        item->setData(Qt::UserRole + 1, w.phonetic);
        item->setData(Qt::UserRole + 2, w.meaning);
        item->setData(Qt::UserRole + 3, vocab::dueLabel(w, now));
        item->setData(Qt::UserRole + 4, w.dueAt <= now);
        item->setData(Qt::UserRole + 5, w.box);
        item->setData(Qt::UserRole + 6, w.id);
        item->setSizeHint(QSize(0, 58));
        if (w.id == keepId) list_->setCurrentItem(item);
    }
    if (!list_->currentItem() && list_->count() > 0) list_->setCurrentRow(0);
    updateDetail();
}

WordEntry VocabPanel::selectedWord() const
{
    const QListWidgetItem* item = list_ ? list_->currentItem() : nullptr;
    if (!item) return {};
    const QString id = item->data(Qt::UserRole + 6).toString();
    for (const WordEntry& w : words_)
        if (w.id == id) return w;
    return {};
}

void VocabPanel::updateDetail()
{
    const WordEntry w = selectedWord();
    const bool has = !w.id.isEmpty();
    for (QPushButton* b : {pronounceButton_, editButton_, deleteButton_, knownButton_, unknownButton_})
        b->setEnabled(has);
    sourceButton_->setEnabled(has && !w.docPath.isEmpty() && w.page >= 0);

    if (!has) {
        detail_->clear();
        return;
    }
    QString html = QStringLiteral("<div style=\"font-size:15px;font-weight:600;color:#1d1d1f\">%1</div>")
                       .arg(htmlEscape(w.word));
    if (!w.phonetic.isEmpty())
        html += QStringLiteral("<div style=\"color:#8a8a8e;font-size:12px\">%1</div>")
                    .arg(htmlEscape(w.phonetic));
    html += QStringLiteral("<div style=\"margin-top:4px;color:#333\">%1</div>")
                .arg(htmlEscape(w.meaning).replace(QLatin1Char('\n'), QStringLiteral("<br>")));
    if (!w.example.isEmpty())
        html += QStringLiteral("<div style=\"margin-top:4px;color:#6b6b70;font-size:12px\">%1</div>")
                    .arg(htmlEscape(w.example));
    if (!w.docPath.isEmpty())
        html += QStringLiteral("<div style=\"margin-top:4px;color:#9a9aa0;font-size:11px\">%1 · 第 %2 页</div>")
                    .arg(htmlEscape(QFileInfo(w.docPath).fileName()))
                    .arg(w.page + 1);
    detail_->setHtml(html);
}

void VocabPanel::reviewSelected(bool remembered)
{
    const WordEntry w = selectedWord();
    if (w.id.isEmpty()) return;
    if (store_.reviewWord(w.id, remembered)) {
        reload();
        emit statusMessage(remembered
                               ? QStringLiteral("「%1」进入第 %2 盒").arg(w.word).arg(qMin(w.box + 1, 5))
                               : QStringLiteral("「%1」回到第 0 盒").arg(w.word));
    }
}

void VocabPanel::pronounceSelected()
{
    const WordEntry w = selectedWord();
    if (w.id.isEmpty()) return;
#ifdef ADAPT_HAVE_TTS
    if (speech_) {
        speech_->stop();
        speech_->say(w.word);
        return;
    }
#endif
    emit statusMessage(QStringLiteral("系统没有可用的语音引擎，无法发音"));
}

void VocabPanel::editSelected()
{
    const WordEntry w = selectedWord();
    if (w.id.isEmpty() || kHeadless) return;

    QDialog dlg(this);
    dlg.setWindowTitle(QStringLiteral("编辑词条"));
    auto* form = new QFormLayout(&dlg);
    auto* word = new QLineEdit(w.word, &dlg);
    auto* phonetic = new QLineEdit(w.phonetic, &dlg);
    auto* meaning = new QPlainTextEdit(w.meaning, &dlg);
    meaning->setMinimumHeight(70);
    auto* example = new QPlainTextEdit(w.example, &dlg);
    example->setMinimumHeight(60);
    form->addRow(QStringLiteral("单词"), word);
    form->addRow(QStringLiteral("音标"), phonetic);
    form->addRow(QStringLiteral("释义"), meaning);
    form->addRow(QStringLiteral("例句"), example);
    auto* buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel, &dlg);
    form->addRow(buttons);
    connect(buttons, &QDialogButtonBox::accepted, &dlg, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dlg, &QDialog::reject);
    if (dlg.exec() != QDialog::Accepted) return;

    WordEntry edited = w;
    edited.word = word->text().trimmed();
    edited.phonetic = phonetic->text().trimmed();
    edited.meaning = meaning->toPlainText().trimmed();
    edited.example = example->toPlainText().trimmed();
    QString err;
    if (!store_.upsertWord(edited, &err)) {
        emit statusMessage(QStringLiteral("保存失败：%1").arg(err));
        return;
    }
    reload();
    emit statusMessage(QStringLiteral("词条已保存"));
}

void VocabPanel::deleteSelected()
{
    const WordEntry w = selectedWord();
    if (w.id.isEmpty()) return;
    if (!kHeadless &&
        QMessageBox::question(this, QStringLiteral("删除生词"),
                              QStringLiteral("从生词本删除「%1」？").arg(w.word))
            != QMessageBox::Yes)
        return;
    store_.removeWord(w.id);
    reload();
    emit statusMessage(QStringLiteral("已删除「%1」").arg(w.word));
}

void VocabPanel::openSourceSelected()
{
    const WordEntry w = selectedWord();
    if (!w.docPath.isEmpty() && w.page >= 0) emit openSourceRequested(w.docPath, w.page);
}

void VocabPanel::startReview()
{
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QVector<WordEntry> queue;
    for (const WordEntry& w : store_.words())
        if (w.dueAt <= now) queue.append(w);
    if (queue.isEmpty()) {
        emit statusMessage(QStringLiteral("没有到期生词；明天再来，或先查几个词"));
        return;
    }
    std::sort(queue.begin(), queue.end(), [](const WordEntry& a, const WordEntry& b) {
        if (a.dueAt != b.dueAt) return a.dueAt < b.dueAt;
        return a.word.compare(b.word, Qt::CaseInsensitive) < 0;
    });

    if (kHeadless) {
        emit statusMessage(QStringLiteral("%1 个生词待复习").arg(queue.size()));
        return;
    }
    ReviewDialog dialog(store_, queue, this);
    dialog.exec();
    reload();
}

void VocabPanel::exportApkg()
{
    const QVector<WordEntry> rows = store_.words();
    if (rows.isEmpty()) {
        emit statusMessage(QStringLiteral("生词本是空的，先查几个词"));
        return;
    }
    if (kHeadless) return;
    const QString path = QFileDialog::getSaveFileName(this, QStringLiteral("导出生词卡包"),
                                                      QStringLiteral("words.apkg"),
                                                      QStringLiteral("Anki 卡包 (*.apkg)"));
    if (path.isEmpty()) return;
    QString err;
    if (!exports::vocabToAnki(rows, path, QStringLiteral("AdaptNote 生词"), &err)) {
        emit statusMessage(QStringLiteral("导出失败：%1").arg(err));
        return;
    }
    emit statusMessage(QStringLiteral("已导出 %1 个词到 %2").arg(rows.size()).arg(path));
}

void VocabPanel::exportWords(bool anki)
{
    const QVector<WordEntry> rows = store_.words();
    if (rows.isEmpty()) {
        emit statusMessage(QStringLiteral("生词本是空的，先查几个词"));
        return;
    }
    if (kHeadless) return;
    const QString path = QFileDialog::getSaveFileName(
        this, QStringLiteral("导出 %1").arg(anki ? QStringLiteral("Anki") : QStringLiteral("TSV")),
        anki ? QStringLiteral("words-anki.txt") : QStringLiteral("words.tsv"),
        anki ? QStringLiteral("文本 (*.txt)") : QStringLiteral("TSV (*.tsv)"));
    if (path.isEmpty()) return;
    QString err;
    if (!vocab::exportWords(rows, path, anki, &err)) {
        emit statusMessage(QStringLiteral("导出失败：%1").arg(err));
        return;
    }
    emit statusMessage(QStringLiteral("已导出 %1 个词到 %2").arg(rows.size()).arg(path));
}

}  // namespace an
