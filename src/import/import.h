#pragma once

#include "core/model.h"
#include "render/engine.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace an {

class Store;
class IDocumentEngine;

// One imported document worth of notes (the notes still point at `docPath`).
struct ImportedDocument {
    QString docPath;      // target document path (may be empty -> keep source hint)
    QString title;
    QVector<Markup> markups;
    QStringList warnings;
};

struct ImportReport {
    int notes = 0;
    int documents = 0;
    int skipped = 0;
    QStringList warnings;

    QString summary() const;
};

namespace imports {

// Our own JSON mirror (mirror/<book>.markups.json) or another reader's JSON export
// that follows the same shape ({markups:[...]}).
ImportedDocument fromAdaptNoteJson(const QString& path, QString* error);

// Markdown as produced by BookxNote and by our own Markdown exporter:
//   # book / ## 第 N 页 / - **高亮** · #tag / > excerpt / - 批注：...
ImportedDocument fromMarkdown(const QString& path, QString* error);

// Sniffs the file and picks a parser. `targetDoc` overrides the document the notes are
// attached to (the export usually carries its own name only).
ImportedDocument detect(const QString& path, const QString& targetDoc = {},
                        QString* error = nullptr);

// Reads the annotations embedded in a PDF (written by us or by another reader).
// `existing` is used to skip notes that are already in the library: a note counts as
// duplicate when page, type and quad geometry match closely.
ImportedDocument fromPdfAnnotations(IDocumentEngine& engine, const QString& docPath,
                                    const QVector<Markup>& existing = {});

// Writes the imported notes into the store (batch, one transaction).
ImportReport commit(Store& store, const QVector<ImportedDocument>& documents,
                    QString* error = nullptr);

}  // namespace imports
}  // namespace an
