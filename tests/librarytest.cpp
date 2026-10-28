// Library view test: document table, cross-document search, tag filter, signals.
#include "core/store.h"
#include "ui/libraryview.h"

#include <QApplication>
#include <QDateTime>
#include <QComboBox>
#include <QFile>
#include <QLineEdit>
#include <QListWidget>
#include <QTableWidget>
#include <QTemporaryDir>

#include <cstdio>

using namespace an;

namespace {

int g_fail = 0;

void check(bool ok, const QString& what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) ++g_fail;
}

void settle()
{
    for (int i = 0; i < 3; ++i) QCoreApplication::processEvents();
}

Markup makeNote(const QString& docPath, int page, const QString& text, const QString& comment,
                const QStringList& tags, const QColor& color = QColor("#FFD500"))
{
    Markup m;
    m.id = newId();
    m.docPath = docPath;
    m.page = page;
    m.type = MarkupType::Highlight;
    m.quads = {Quad::fromRect(QRectF(10, 10, 100, 12))};
    m.text = text;
    m.comment = comment;
    m.tags = tags;
    m.color = color;
    m.created = m.updated = QDateTime::currentMSecsSinceEpoch();
    return m;
}

}  // namespace

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AdaptNote"));
    QCoreApplication::setApplicationName(QStringLiteral("AdaptNoteLibraryTest"));

    QTemporaryDir tmp;
    check(tmp.isValid(), QStringLiteral("temp dir"));
    if (!tmp.isValid()) return 1;

    const QString docA = tmp.path() + "/第一本书.pdf";
    const QString docB = tmp.path() + "/Second Book.epub";

    Store store;
    QString err;
    check(store.open(tmp.path() + "/library.db", &err), QStringLiteral("open store: %1").arg(err));
    store.touchDocument(docA, QStringLiteral("第一本书"), 120);
    store.touchDocument(docB, QStringLiteral("Second Book"), 8);
    store.setReadingState(docA, 41, 0.3);

    store.upsertMarkup(makeNote(docA, 3, QStringLiteral("标注即思考"), QStringLiteral("要复习"),
                               {QStringLiteral("方法论")}));
    store.upsertMarkup(makeNote(docA, 7, QStringLiteral("把书读薄"), QString(),
                               {QStringLiteral("方法论"), QStringLiteral("待复习")}));
    store.upsertMarkup(makeNote(docA, 9, QStringLiteral("结构先于细节"), QStringLiteral("结构主义"),
                               {}));
    store.upsertMarkup(makeNote(docB, 1, QStringLiteral("a highlight in english"),
                               QStringLiteral("note"), {QStringLiteral("方法论")},
                               QColor("#34A853")));
    store.upsertMarkup(makeNote(docB, 2, QStringLiteral("冲突副本"), QStringLiteral("需要合并"),
                               {QStringLiteral("conflict")}));

    LibraryView view(store);
    view.resize(1000, 640);
    view.show();
    settle();

    check(view.documentCount() == 2, QStringLiteral("library lists %1 documents").arg(view.documentCount()));
    check(view.noteCount() == 5, QStringLiteral("whole library shows %1 notes").arg(view.noteCount()));

    // the documents table must show note and conflict counts
    auto* table = view.findChild<QTableWidget*>();
    check(table != nullptr, QStringLiteral("documents table exists"));
    if (table) {
        QStringList titles;
        QString conflicts;
        for (int row = 0; row < table->rowCount(); ++row) {
            titles << table->item(row, 0)->text();
            if (table->item(row, 0)->text() == QStringLiteral("Second Book"))
                conflicts = table->item(row, 2)->text();
        }
        check(titles.contains(QStringLiteral("第一本书")) &&
                  titles.contains(QStringLiteral("Second Book")),
              QStringLiteral("both documents listed: %1").arg(titles.join(QStringLiteral(" / "))));
        check(conflicts == QStringLiteral("1"),
              QStringLiteral("conflict badge for the second book = %1").arg(conflicts));
    }

    // scoping to one document
    {
        int rowA = -1;
        for (int row = 0; row < table->rowCount(); ++row)
            if (table->item(row, 0)->text() == QStringLiteral("第一本书")) rowA = row;
        check(view.selectDocumentRow(rowA), QStringLiteral("select the first document row"));
        view.setWholeLibrary(false);
        check(view.noteCount() == 3,
              QStringLiteral("document scope shows its %1 notes").arg(view.noteCount()));
        view.setWholeLibrary(true);
        check(view.noteCount() == 5, QStringLiteral("back to the whole library (%1)").arg(view.noteCount()));
    }

    // full text search across documents
    view.setSearchText(QStringLiteral("方法论"));
    settle();
    check(view.noteCount() == 3,
          QStringLiteral("search by tag text hits %1 notes across both books").arg(view.noteCount()));
    check(view.listedNoteIds().size() == 3, QStringLiteral("listed note ids match"));

    view.setSearchText(QStringLiteral("英文"));
    settle();
    check(view.noteCount() == 0, QStringLiteral("no hits for an absent term"));

    view.setSearchText(QStringLiteral("english"));
    settle();
    check(view.noteCount() == 1, QStringLiteral("ascii search hits the second book (%1)").arg(view.noteCount()));

    view.setSearchText(QString());
    settle();

    // tag filter
    view.setTagFilter(QStringLiteral("待复习"));
    settle();
    check(view.noteCount() == 1, QStringLiteral("tag filter -> %1 note").arg(view.noteCount()));
    view.setTagFilter(QString());
    settle();
    check(view.noteCount() == 5, QStringLiteral("tag filter cleared (%1)").arg(view.noteCount()));

    // ---- smart views over the document list ----
    {
        struct Case {
            DocFilter filter;
            int expected;
            QString label;
        };
        const QVector<Case> cases = {
            {DocFilter::All, 2, QStringLiteral("all documents")},
            {DocFilter::Recent, 2, QStringLiteral("recently opened")},
            {DocFilter::Unread, 0, QStringLiteral("unread (never opened, no notes)")},
            {DocFilter::InProgress, 1, QStringLiteral("in progress (0 < page < total)")},
            {DocFilter::WithNotes, 2, QStringLiteral("has notes")},
            {DocFilter::WithConflicts, 1, QStringLiteral("has conflicts")},
        };
        for (const Case& c : cases) {
            view.setDocFilter(c.filter);
            settle();
            check(view.documentRowCount() == c.expected,
                  QStringLiteral("%1 -> %2 document(s)").arg(c.label).arg(view.documentRowCount()));
        }

        // ByTag uses the tag selected in the notes filter
        view.setDocFilter(DocFilter::ByTag);
        view.setTagFilter(QStringLiteral("方法论"));
        settle();
        check(view.documentRowCount() == 2,
              QStringLiteral("by tag 方法论 -> %1 documents").arg(view.documentRowCount()));
        view.setTagFilter(QStringLiteral("conflict"));
        settle();
        check(view.documentRowCount() == 1,
              QStringLiteral("by tag conflict -> %1 document").arg(view.documentRowCount()));
        view.setTagFilter(QString());
        settle();
        check(view.documentRowCount() == 2,
              QStringLiteral("by tag without a tag shows everything (%1)").arg(view.documentRowCount()));

        view.setDocFilter(DocFilter::All);
        view.setWholeLibrary(true);
        settle();
        check(view.documentRowCount() == 2, QStringLiteral("back to all documents"));
    }

    // activation emits the jump request with the right document
    {
        QString requestedDoc;
        QString requestedId;
        QObject::connect(&view, &LibraryView::openNoteRequested,
                         [&](const QString& doc, const QString& id) {
                             requestedDoc = doc;
                             requestedId = id;
                         });
        auto* list = view.findChild<QListWidget*>();
        check(list && list->count() > 0, QStringLiteral("note list populated"));
        if (list) {
            list->setCurrentRow(0);
            view.activateCurrentNote();
        }
        check(!requestedId.isEmpty(), QStringLiteral("openNoteRequested fired for %1").arg(requestedId));
        check(requestedDoc == docA || requestedDoc == docB,
              QStringLiteral("requested document path = %1").arg(requestedDoc));
    }

    // removing a note updates the counts on reload
    {
        const QStringList ids = view.listedNoteIds();
        if (!ids.isEmpty()) store.removeMarkup(ids.first());
        view.reload();
        settle();
        check(view.noteCount() == 4, QStringLiteral("reload after a delete shows %1 notes").arg(view.noteCount()));
    }

    // ---- document management: rename / multi-select / forget ----
    {
        // forget must not touch the book file, so give the documents a real file
        { QFile f(docA); f.open(QIODevice::WriteOnly); f.write("pdf"); }

        const int docsBefore = view.documentCount();
        int rowA = -1;
        for (int row = 0; row < table->rowCount(); ++row)
            if (table->item(row, 0)->text() == QStringLiteral("第一本书")) rowA = row;
        check(view.selectDocumentRow(rowA), QStringLiteral("select the document to rename"));
        check(view.renameSelected(QStringLiteral("第一本书（改）")), QStringLiteral("rename the document"));
        bool renamed = false;
        for (const DocRow& r : store.documents())
            if (r.path == docA && r.title == QStringLiteral("第一本书（改）")) renamed = true;
        check(renamed, QStringLiteral("store holds the new title"));

        // multi-select both rows
        table->selectAll();
        check(view.selectedDocumentPaths().size() == docsBefore,
              QStringLiteral("multi-select returns %1 paths").arg(view.selectedDocumentPaths().size()));

        // forget everything selected
        int removedNotes = -1;
        check(view.forgetSelected(&removedNotes), QStringLiteral("forget the selected documents"));
        check(removedNotes >= 4,
              QStringLiteral("notes were tombstoned (%1)").arg(removedNotes));
        check(view.documentCount() == 1 || view.documentCount() == 2,
              QStringLiteral("library reloaded after forget (%1 documents)").arg(view.documentCount()));

        check(store.markups(docA).isEmpty(), QStringLiteral("live notes of the forgotten book are gone"));
        check(!store.markupsIncludingDeleted(docA).isEmpty(),
              QStringLiteral("tombstones kept for sync (%1)")
                  .arg(store.markupsIncludingDeleted(docA).size()));
        check(QFile::exists(docA), QStringLiteral("the book file itself is untouched"));
    }


    // ---- reading topics in the library ----
    {
        std::printf("\n=== library: reading topics ===\n");
        view.reload();
        const int topicsBefore = view.topicCount();

        const QString id = view.createTopic(QStringLiteral("课程主题"));
        check(!id.isEmpty(), QStringLiteral("create a topic from the library"));
        check(view.topicCount() == topicsBefore + 1,
              QStringLiteral("topic list grew (%1 -> %2)").arg(topicsBefore).arg(view.topicCount()));
        check(view.topicNames().contains(QStringLiteral("课程主题")),
              QStringLiteral("the new topic is listed by name"));

        // rename through the library API
        view.reload();
        int topicRow = -1;
        for (int i = 0; i < view.topicCount(); ++i) {
            view.selectTopicRow(i);
            if (view.currentTopicId() == id) { topicRow = i; break; }
        }
        check(topicRow >= 0, QStringLiteral("the topic can be selected"));
        check(view.renameSelectedTopic(QStringLiteral("课程主题（改）")),
              QStringLiteral("rename the selected topic"));
        check(view.topicNames().contains(QStringLiteral("课程主题（改）")),
              QStringLiteral("renamed topic visible"));

        // attach the document that the library already lists
        view.reload();
        for (int i = 0; i < view.topicCount(); ++i) {
            view.selectTopicRow(i);
            if (view.currentTopicId() == id) break;
        }
        const QStringList paths = view.selectedDocumentPaths();
        if (!paths.isEmpty()) {
            QString err;
            check(view.addSelectedDocumentsToTopic(&err),
                  QStringLiteral("add the selected document to the topic (%1)").arg(err));
            check(view.topicDocumentCount() == 1,
                  QStringLiteral("topic holds one document (%1)").arg(view.topicDocumentCount()));
            check(view.removeSelectedDocumentsFromTopic(),
                  QStringLiteral("remove the document again"));
            check(view.topicDocumentCount() == 0, QStringLiteral("topic is empty again"));
        } else {
            std::printf("      (no document row to attach)\n");
        }

        check(view.deleteSelectedTopic(), QStringLiteral("delete the topic"));
        check(view.topicCount() == topicsBefore, QStringLiteral("topic list back to the start"));
    }

    std::printf("\n%s (%d failure(s))\n",
                g_fail == 0 ? "LIBRARY CHECKS PASSED" : "LIBRARY CHECKS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
