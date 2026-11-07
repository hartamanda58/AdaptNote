#pragma once

#include "core/model.h"
#include "core/store.h"
#include "render/mupdf_engine.h"
#include "ai/ai_client.h"
#include "ai/prompts.h"
#include "read/reader.h"
#include "mind/mindmap.h"
#include "ui/libraryview.h"
#include "sync/sync_manager.h"
#include "vocab/vocab.h"

#include <QColor>
#include <QHash>
#include <QMainWindow>
#include <QVector>

class QCheckBox;
class QComboBox;
class QSplitter;
class QToolBar;
class QDockWidget;
class QMenu;
class QLabel;
class QLineEdit;
class QListWidget;
class QTabWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QTabBar;
class QUndoStack;
class QTextBrowser;
class QPushButton;

namespace an {

class PageView;
class VocabPanel;

class MindGroupCommand;
class MindLinkCommand;

class MainWindow : public QMainWindow {
    friend class MindGroupCommand;
    friend class MindLinkCommand;
    Q_OBJECT
public:
    enum class CmdKind { Add, Remove, Modify };

    explicit MainWindow(QWidget* parent = nullptr);

    bool openDocument(const QString& path);
    void applyDemoMarkups();
    PageView* view() const { return view_; }

    // Undo/redo plumbing (also used by the headless feature test).
    void applyCommand(CmdKind kind, const Markup& before, const Markup& after, bool undo);
    void storeUpsertMarkup(const Markup& m);
    void storeUpsertStampedMarkup(const Markup& m);   // stamps a local version first
    void storeRemoveMarkup(const QString& id);
    void refreshFromStore();
    void applyMindLinkCommand(int kind, const MindLink& before, const MindLink& after, bool undo);

    QVector<Markup> visibleMarkups() const { return markups_; }
    QVector<Markup> notesShown() const;
    QStringList availableTags() const;
    QVector<TextHit> currentHits() const { return hits_; }
    int currentHitIndex() const { return hitIndex_; }
    void findText(const QString& needle);   // used by tests + find bar
    void nextHit(int direction);
    bool addImageExcerpt(int page, const QRectF& pageRect);  // used by tests
    bool addInkStroke(int page, const QVector<InkPoint>& points);
    // Erases ink strokes near a page-space point; returns how many were removed.
    int eraseInkAt(int page, const QPointF& pagePoint, double tolerance);
    void toggleInkMode();
    void showMindMap();
    // 0 = reading only, 1 = reading + mind map, 2 = mind map only
    void setWorkspaceMode(int mode);
    int workspaceMode() const { return workspaceMode_; }
    bool notesDockVisibleForTest() const;
    bool aiDockVisibleForTest() const;
    void translateSelectionForTest();
    QString translationForTest() const;
    QString statusMessageForTest() const;
    // The mind-map panel (null until the workspace is built); tests use it to assert that
    // the map really shows the same notes the reader sees.
    mind::MindMapView* mindMapView() const { return mindView_; }
    // Thin hooks so tests can drive the map without opening dialogs.
    QString createMindGroupForTest(const QString& name) { return createMindGroup(name, QString()); }
    bool renameMindGroupForTest(const QString& key, const QString& name)
    {
        return renameMindGroup(key, name);
    }
    bool deleteMindGroupForTest(const QString& key) { return deleteMindGroup(key); }
    bool removeMindLinkForTest(const QString& id);
    bool modifyMindLinkForTest(const MindLink& link);
    QVector<MindLink> mindLinksForTest() const { return store_.mindLinks(mindScopeKey()); }
    // Test hooks for the topic-aware search path.
    void openTopicForTest(const QString& topicId) { currentTopic_ = topicId; updateNotesPanel(); refreshNotebookWorkspace(); }
    int outlineRootCountForTest() const;
    bool openNotebook(const QString& topicId);
    QString createNotebook(const QString& name);
    bool addNotebookDocument(const QString& path);
    bool createFreeNoteForTest(const QString& title, const QString& comment);
    bool createFreeNoteForPageForTest(const QString& title, const QString& comment, int page);
    void storeSettingForTest(const QString& key, const QString& value) { store_.setSetting(key, value); }
    bool moveNotebookNote(const QString& id, const QString& targetPath,
                          const QString& groupKey, const QString& beforeId = {},
                          const QString& parentMarkupId = {}, bool nestUnderParent = false);
    bool moveNotebookGroup(const QString& id, const QString& targetPath, const QString& parentId);
    void showNotebookMindMap();
    void refreshNotebookWorkspace();
    void refreshBookshelf();
    int notebookDocumentCountForTest() const;
    QStringList notebookNoteIdsForTest() const;
    bool activateNotebookNote(const QString& id);
    void setFindScopeForTest(int scope);
    int notesPanelCountForTest() const;
    QString notesPanelTextForTest(int row) const;
    int bookshelfCountForTest() const;
    bool storeUpsertForTest(const Markup& m) { return store_.upsertMarkup(m); }
    QString createTopicForTest(const QString& name)
    {
        Topic t = store_.stampTopicForLocalWrite(Topic{});
        t.name = name;
        return store_.upsertTopic(t) ? t.id : QString();
    }
    bool addTopicDocForTest(const QString& topicId, const QString& path)
    {
        return store_.addTopicDoc(topicId, path);
    }
    void rebuildMindMapForTest() { rebuildMindMap(); }
    bool undoForTest();
    bool redoForTest();
    QString undoTextForTest() const;
    void showLibrary();
    void showConflicts();
    // Mind map: regroup a note (empty group = back to page grouping). Undoable.
    bool applyMindReparent(const QString& markupId, const QString& groupKey);
    bool applyMindContainerReparent(const QString& markupId, const QString& containerKey);
    bool reparentNoteWithinDocument(const QString& markupId, const QString& parentMarkupId);
    bool moveMarkupToContainer(const QString& markupId, const QString& sourcePath,
                               const QString& targetPath, const QString& targetKey);
    void setInkColor(const QColor& c);
    // Mind map groups (first class entities).
    QString createMindGroup(const QString& name, const QString& parentId = {});
    bool moveMindGroupUp(const QString& groupKey);
    // Manual ordering inside a group (or page bucket). Undoable as one step.
    bool applyMindReorder(const QString& markupId, const QString& groupKey,
                          const QString& beforeMarkupId);
    bool renameMindGroup(const QString& groupKey, const QString& newName);
    bool deleteMindGroup(const QString& groupKey);
    QVector<an::MindGroup> mindGroups() const;   // live groups of the open document
    void toggleReading();
    void askAi(an::ai::PromptAction action);
    QString aiOutput() const;
    void deleteMarkupById(const QString& id);
    void pushAddMarkup(const Markup& m);     // undo-stack aware insert (tests)
    void undoCommand();
    void redoCommand();
    int undoDepth() const;
    void saveReadingState();
    bool exportMarkups(const QString& format, const QString& path, QString* error = nullptr,
                       bool onlyFiltered = false);
    bool exportLibrary(const QString& format, const QString& path, QString* error = nullptr);
    // Export only the notes of the given documents (library view selection).
    bool exportDocuments(const QStringList& docPaths, const QString& format,
                         const QString& path, QString* error = nullptr);
    void setTagFilter(const QString& tag);   // "" = clear (used by tests + scripting)
    void setReaderSpeed(int wpm);
    // Bulk import helper: relaxes fsync in the store (speed over durability).
    void setFastImport(bool on);
    // Re-lays out a reflowable document and re-anchors its notes (used by tests too).
    void setReflowLayout(float widthPt, float heightPt, float emPt);
    void addMarkupFromSelectionForTest(MarkupType type) { addMarkupFromSelection(type); }
    an::MuPdfEngine* engineForTest() { return &engine_; }
    int relocateAnchors();
    // PDF revision support: notes follow their text when the file was replaced.
    int migrateAfterRevision();
    bool documentFileChanged() const;
    // Import notes from another reader (markdown) or our own JSON mirror.
    bool importNotes(const QString& path, bool intoCurrentDocument, QString* error = nullptr);
    // Reads the annotations already embedded in the open PDF and adds them as notes.
    int importEmbeddedAnnotations();
    // How many embedded annotations still look un-imported (0 when there is nothing
    // to do, or when this exact file revision was already offered).
    int pendingEmbeddedAnnotations();
    // Disables only the modal prompt (tests / scripted use); detection still works.
    void setEmbeddedPromptEnabled(bool on) { embeddedPrompt_ = on; }

private:
    void buildUi();
    void buildToolbar();
    void buildFindBar();
    void buildDocks();
    void buildStatusBar();

    void chooseFile();
    void reloadOutline();
    void reloadMarkups();
    void updateNotesPanel();
    void updateFilterWidgets();
    void updateStatus();
    void addMarkupFromSelection(MarkupType type);
    void editMarkupComment(const QString& id);
    void editMarkupTags(const QString& id);
    void jumpToMarkup(const QString& id);
    void saveAnnotationsToPdf();
    void showViewContextMenu(const QPoint& pos);
    void changePage(int delta);

    void doExport(const QString& format);
    void doExportLibrary(const QString& format);
    void buildAiDock();
    void buildAiSettings();
    void editAiRoles();
    void buildReaderMenu();
    void runAiWithSelection();
    void askAiQuestion();
    void showSelectionTranslation();
    void updateSelectionTranslationAction();
    // Vocabulary: local ECDICT first, then the OpenAI-compatible model.
    void lookupSelectionWord();
    void lookupWordForVocab(const QString& raw);
    void saveVocabDefinition(const vocab::Definition& def);
    // OCR: selected region (or clipboard image) -> tesseract, vision model fallback.
    void ocrSelection();
    void ocrCurrentPage();
    void runOcrOnImage(const QImage& image);
    void openOcrSettings();
    void showOcrResult(const QString& text);
    void insertAiAsNote();
    void onInkStroke(int page, const QVector<InkPoint>& points);
    void onReaderSentence(const an::read::Sentence& sentence);
    void openSyncSettings();
    void runSync();
    void runNoteSearch();
    void captureImageExcerpt();
    void onRegionSelected(int page, const QRectF& pageRect);
    void restoreReadingState();
    void closeEvent(QCloseEvent* e) override;
    Markup* findMarkup(const QString& id);
    const Markup* findMarkup(const QString& id) const;
    bool tagPasses(const Markup& m) const;
    bool colorPasses(const Markup& m) const;

    MuPdfEngine engine_;
    Store store_;
    SyncManager sync_{store_};
    PageView* view_ = nullptr;
    QSplitter* splitter_ = nullptr;
    QToolBar* mainToolbar_ = nullptr;
    QMenu* moreMenu_ = nullptr;
    QTabBar* notebookTabs_ = nullptr;
    QLabel* notebookTitle_ = nullptr;
    QTabBar* notebookDocuments_ = nullptr;
    QTreeWidget* notebookOutline_ = nullptr;
    QDockWidget* notesDock_ = nullptr;
    QDockWidget* bookshelfDock_ = nullptr;
    QWidget* bookshelfContent_ = nullptr;
    QListWidget* bookshelfList_ = nullptr;
    QListWidget* topicShelfList_ = nullptr;
    QTreeWidget* pageOutlineOnShelf_ = nullptr;
    QListWidget* pageThumbs_ = nullptr;
    QTimer* thumbTimer_ = nullptr;
    int thumbNext_ = 0;
    QString thumbDocPath_;
    void rebuildPageThumbnails();
    void pumpPageThumbnails();
    mind::MindMapView* shelfMindView_ = nullptr;
    QTabWidget* readerSideTabs_ = nullptr;
    QWidget* aiPanel_ = nullptr;
    QWidget* selectionBar_ = nullptr;
    QToolBar* findBar_ = nullptr;
    QDialog* notebookMindWindow_ = nullptr;
    mind::MindMapView* notebookMindView_ = nullptr;
    QWidget* mindPanel_ = nullptr;
    mind::MindMapView* mindView_ = nullptr;
    int workspaceMode_ = 0;
    QString currentTopic_;   // non-empty = the mind map shows that reading topic
    QString mindFittedScope_;   // scope whose map was already fitted into the viewport
    void ensureMindPanel();
    void rebuildMindMap();
    QStringList currentTopicDocPaths() const;   // documents of the open topic (empty = none)
    QString mindScopeKey() const;   // reading topic id, else the current document path
    static QString noteIdOfKey(const QString& key);
    QString parentGroupOfNote(const QString& key) const;
    QTreeWidget* outlineTree_ = nullptr;
    QListWidget* notesList_ = nullptr;
    QTabWidget* leftTabs_ = nullptr;
    QLineEdit* noteSearchEdit_ = nullptr;
    QComboBox* tagFilter_ = nullptr;
    QToolButton* colorButtons_[5] = {};
    QLineEdit* findEdit_ = nullptr;
    QLabel* findLabel_ = nullptr;
    QToolButton* allDocsBtn_ = nullptr;
    QComboBox* findScope_ = nullptr;   // 0 = page text, 1 = notes of the open topic
    class LibraryView* library_ = nullptr;
    QComboBox* zoomBox_ = nullptr;
    QComboBox* modeBox_ = nullptr;
    QLabel* pageLabel_ = nullptr;
    QLabel* infoLabel_ = nullptr;
    QUndoStack* undo_ = nullptr;
    QTimer* findDebounce_ = nullptr;
    QTimer* stateTimer_ = nullptr;

    // P5: ink / mind map / read aloud / AI
    QAction* inkAction_ = nullptr;
    QAction* eraseAction_ = nullptr;
    void setInkWidthPreset(double w);
    void onInkEraseRequested(int page, const QStringList& ids, const QVector<QPointF>& pagePath);
    void applyEraser(int page, const QStringList& ids, const QVector<QPointF>& pagePath);
    QAction* readAction_ = nullptr;
    an::read::Reader reader_;
    an::ai::AiClient ai_;
    QVector<an::ai::Role> aiRoles_;
    QComboBox* aiRoleCombo_ = nullptr;
    QComboBox* aiActionCombo_ = nullptr;
    QLineEdit* aiExtraEdit_ = nullptr;
    QTextBrowser* aiOutput_ = nullptr;
    QTextBrowser* aiReasoning_ = nullptr;
    QCheckBox* aiReasoningToggle_ = nullptr;
    QPushButton* aiRunButton_ = nullptr;
    QPushButton* aiInsertButton_ = nullptr;
    QToolButton* translateSelectionButton_ = nullptr;
    QDialog* translationDialog_ = nullptr;
    QTextBrowser* translationOutput_ = nullptr;
    bool translationRequest_ = false;
    // A lookup/OCR request owns the next AI reply, so the generic answer pane is not fed
    // with dictionary prose.
    QString pendingWord_;
    bool pendingOcr_ = false;
    QLineEdit* aiQuestionEdit_ = nullptr;
    VocabPanel* vocabPanel_ = nullptr;
    QString aiContent_;
    QString aiReasoningText_;
    bool exportFiltered_ = false;
    bool exportByGroups_ = false;   // MD/HTML organised by the mind map
    bool embeddedPrompt_ = true;   // ask once per file revision

    QString docPath_;
    QVector<Markup> markups_;
    QHash<QString, Markup> shown_;   // id -> markup currently listed in the notes panel
    QVector<TextHit> hits_;
    int hitIndex_ = -1;
    bool searchAllDocs_ = false;
    QColor markupColor_{255, 213, 0};
    QColor inkColor_{0x1f, 0x23, 0x28};   // handwriting has its own colour
};

}  // namespace an
