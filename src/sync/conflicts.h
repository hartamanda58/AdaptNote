#pragma once

#include "core/model.h"

#include <QVector>

namespace an {
namespace conflicts {

// A conflict produced by the sync merge: `copy` is the secondary version that was
// forked off `original` (copy.conflictedFrom == original.id).
struct Pair {
    Markup original;
    Markup copy;
};

// Pairs up notes that carry a `conflict` tag with the note they were forked from.
// Notes whose original is gone are returned with an empty `original.id` so the UI can
// still show them.
QVector<Pair> find(const QVector<Markup>& notes);

// Resolution actions (applied by the caller through the store).
enum class Resolution {
    KeepOriginal,   // drop the conflict copy
    KeepCopy,       // replace the original's content with the copy's, drop the copy
    KeepBoth,       // just clear the conflict tag on both
};

// Produces the notes that must be written for `action`, plus the id to delete (empty
// when nothing should be deleted).
struct Plan {
    QVector<Markup> write;
    QString deleteId;
};

Plan plan(const Pair& pair, Resolution action, qint64 nowMs);

}  // namespace conflicts
}  // namespace an
