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

#include <QColor>
#include <QHash>
#include <QMainWindow>
#include <QVector>

class QComboBox;
class QSplitter;
class QLabel;
class QLineEdit;
class QListWidget;
class QTabWidget;
class QTimer;
class QToolButton;
class QTreeWidget;
class QTreeWidgetItem;
class QUndoStack;
class QTextBrowser;
class QPushButton;

namespace an {

class PageView;

class MainWindow : public QMainWindow {
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
    // The mind-map panel (null until the workspace is built); tests use it to assert that
    // the map really shows the same notes the reader sees.
    mind::MindMapView* mindMapView() const { return mindView_; }
    void showLibrary();
    void showConflicts();
    // Mind map: regroup a note (empty group = back to page grouping). Undoable.
    bool applyMindReparent(const QString& markupId, const QString& groupKey);
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
    QWidget* mindPanel_ = nullptr;
    mind::MindMapView* mindView_ = nullptr;
    int workspaceMode_ = 0;
    void ensureMindPanel();
    void rebuildMindMap();
    QTreeWidget* outlineTree_ = nullptr;
    QListWidget* notesList_ = nullptr;
    QTabWidget* leftTabs_ = nullptr;
    QLineEdit* noteSearchEdit_ = nullptr;
    QComboBox* tagFilter_ = nullptr;
    QToolButton* colorButtons_[5] = {};
    QLineEdit* findEdit_ = nullptr;
    QLabel* findLabel_ = nullptr;
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
    QToolButton* aiReasoningToggle_ = nullptr;
    QPushButton* aiRunButton_ = nullptr;
    QPushButton* aiInsertButton_ = nullptr;
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
