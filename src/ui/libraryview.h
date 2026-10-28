#pragma once

#include "core/model.h"
#include "core/store.h"

#include <QDialog>
#include <QHash>
#include <QSet>
#include <QVector>

class QComboBox;
class QListWidget;
class QTabWidget;
class QLabel;
class QLineEdit;
class QTableWidget;

namespace an {

// Library overview: every known document with its note counts, plus a cross-document
// note browser (full text search + tag filter). Emits requests instead of touching
// the reader, so it stays testable and can be opened from anywhere.
// Smart filters over the document list.
enum class DocFilter {
    All,
    Recent,        // opened and not finished, newest first
    Unread,        // never read and no notes
    InProgress,    // 0 < progress < total pages
    WithNotes,
    WithConflicts,
    ByTag,         // uses the tag selected in the notes filter
};

class LibraryView : public QDialog {
    Q_OBJECT
public:
    explicit LibraryView(Store& store, QWidget* parent = nullptr);

    void reload();

    int documentCount() const;
    int noteCount() const;              // notes currently listed
    QStringList listedNoteIds() const;
    void setSearchText(const QString& text);
    void setTagFilter(const QString& tag);
    void setWholeLibrary(bool on);
    void setDocFilter(DocFilter filter);
    DocFilter docFilter() const { return filter_; }
    int documentRowCount() const;   // documents currently listed
    bool selectDocumentRow(int row);
    void activateCurrentNote();         // same as double-clicking it

    // ---- document management (also used by tests) ----
    QStringList selectedDocumentPaths() const;
    bool renameSelected(const QString& newTitle);      // first selected document
    bool forgetSelected(int* notesRemoved = nullptr);  // drop from library, keep the file


    // ---- reading topics (cross-document) ----
    int topicCount() const;
    QStringList topicNames() const;
    QString createTopic(const QString& name);          // returns the new id ("" on failure)
    bool renameSelectedTopic(const QString& newName);
    bool deleteSelectedTopic();
    bool addSelectedDocumentsToTopic(QString* error = nullptr);
    bool removeSelectedDocumentsFromTopic();
    bool selectTopicRow(int row);
    QString currentTopicId() const;
    int topicDocumentCount() const;                    // documents in the selected topic

signals:
    void openTopicRequested(const QString& topicId);
    void openDocumentRequested(const QString& docPath);
    void openNoteRequested(const QString& docPath, const QString& markupId);
    void exportRequested(const QStringList& docPaths, const QString& format);
    void libraryChanged();

private:
    QString scopePath() const;          // "" = whole library
    QVector<DocRow> filteredRows() const;
    void updateNotes();

    void reloadTopics();

    Store& store_;
    QTabWidget* leftTabs_ = nullptr;
    QListWidget* topics_ = nullptr;
    QVector<Topic> topicRows_;
    QVector<DocRow> allRows_;   // everything the store knows
    QVector<DocRow> rows_;      // after the smart filter
    DocFilter filter_ = DocFilter::All;
    QComboBox* viewBox_ = nullptr;
    QSet<QString> taggedDocs_;   // documents holding the selected tag (ByTag view)
    QHash<QString, Markup> shown_;

    QTableWidget* docs_ = nullptr;
    QLineEdit* search_ = nullptr;
    QComboBox* tags_ = nullptr;
    QListWidget* notes_ = nullptr;
    QLabel* summary_ = nullptr;
    bool wholeLibrary_ = true;
};

}  // namespace an
