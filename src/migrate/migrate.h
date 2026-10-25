#pragma once

#include "core/model.h"
#include "render/engine.h"

#include <QString>
#include <QStringList>
#include <QVector>

namespace an {

// One note that the migration tried to place in the (possibly re-paginated) document.
struct Relocation {
    QString markupId;
    int oldPage = -1;
    int newPage = -1;
    QVector<Quad> quads;
    double score = 0.0;    // 0..1 text similarity of the matched range
};

struct MigrationReport {
    QVector<Relocation> moved;    // page changed, text found elsewhere
    QVector<Relocation> inPlace;  // text still on the old page (quads refreshed)
    QStringList lost;             // text no longer present in the document
    QVector<Relocation> unchanged;  // nothing to do

    int scanned() const { return moved.size() + inPlace.size() + lost.size() + unchanged.size(); }
    QString summary() const;
};

// Re-locates notes by their excerpt text. Used when a PDF was replaced by a new
// revision (pages shift, line breaks change) so notes follow their text instead of
// staying on a stale page number.
MigrationReport migrateByText(IDocumentEngine& engine, const QVector<Markup>& markups);

}  // namespace an
