#pragma once

#include "core/model.h"

#include <QVector>
#include <QWidget>

class QLabel;
class QLineEdit;
class QListWidget;
class QListWidgetItem;
class QPushButton;
class QTextBrowser;
class QToolButton;

#ifdef ADAPT_HAVE_TTS
class QTextToSpeech;
#endif

namespace an {

class Store;

// The vocabulary tab: lookup, five-box review, pronunciation and TSV/Anki export.
// Merged from MN's word book (server/index.js + public/app.js) into a Qt panel.
class VocabPanel : public QWidget {
    Q_OBJECT
public:
    explicit VocabPanel(Store& store, QWidget* parent = nullptr);
    ~VocabPanel() override;

    void reload();
    void focusLookup(const QString& seed = {});
    // Selects the card for a spelling (case-insensitive), used after lookup/save.
    void selectWord(const QString& word);
    bool dueOnly() const { return dueOnly_; }

signals:
    // Emitted for the panel to stay free of network/AI code: the window resolves the
    // definition (local ECDICT first, then the OpenAI-compatible model).
    void lookupRequested(const QString& term);
    void openSourceRequested(const QString& docPath, int page);
    void statusMessage(const QString& text);

private:
    void updateDetail();
    void reviewSelected(bool remembered);
    void pronounceSelected();
    void editSelected();
    void deleteSelected();
    void openSourceSelected();
    void exportWords(bool anki);
    WordEntry selectedWord() const;

    Store& store_;
    QVector<WordEntry> words_;
    bool dueOnly_ = false;

    QLabel* stats_ = nullptr;
    QLineEdit* input_ = nullptr;
    QPushButton* lookupButton_ = nullptr;
    QToolButton* exportButton_ = nullptr;
    QToolButton* filterButton_ = nullptr;
    QListWidget* list_ = nullptr;
    QTextBrowser* detail_ = nullptr;
    QPushButton* knownButton_ = nullptr;
    QPushButton* unknownButton_ = nullptr;
    QPushButton* pronounceButton_ = nullptr;
    QPushButton* editButton_ = nullptr;
    QPushButton* deleteButton_ = nullptr;
    QPushButton* sourceButton_ = nullptr;
#ifdef ADAPT_HAVE_TTS
    QTextToSpeech* speech_ = nullptr;
#endif
};

}  // namespace an
