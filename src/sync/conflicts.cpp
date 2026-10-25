#include "sync/conflicts.h"

#include <QHash>

namespace an {
namespace conflicts {
namespace {

bool hasConflictTag(const Markup& m)
{
    return m.tags.contains(QStringLiteral("conflict"));
}

}  // namespace

QVector<Pair> find(const QVector<Markup>& notes)
{
    QHash<QString, Markup> byId;
    for (const Markup& m : notes)
        if (!m.deleted) byId.insert(m.id, m);

    QVector<Pair> out;
    for (const Markup& m : notes) {
        if (m.deleted) continue;
        if (!hasConflictTag(m) && m.conflictedFrom.isEmpty()) continue;

        Pair p;
        p.copy = m;
        if (!m.conflictedFrom.isEmpty() && byId.contains(m.conflictedFrom))
            p.original = byId.value(m.conflictedFrom);
        else {
            // the original itself got the tag (delete-vs-edit case): pair it with a
            // synthetic empty original so the UI can still offer "keep this version"
            p.original = m;
            p.original.id.clear();
        }
        out.append(p);
    }
    return out;
}

Plan plan(const Pair& pair, Resolution action, qint64 nowMs)
{
    Plan result;
    const bool haveOriginal = !pair.original.id.isEmpty();

    auto cleared = [nowMs](Markup m) {
        m.tags.removeAll(QStringLiteral("conflict"));
        m.updated = nowMs;
        return m;
    };

    switch (action) {
    case Resolution::KeepOriginal:
        if (haveOriginal) result.write.append(cleared(pair.original));
        result.deleteId = pair.copy.id;
        break;

    case Resolution::KeepCopy: {
        if (!haveOriginal) {
            // nothing to overwrite: the copy becomes the note
            Markup keep = cleared(pair.copy);
            keep.conflictedFrom.clear();
            result.write.append(keep);
            break;
        }
        Markup merged = pair.original;
        merged.text = pair.copy.text;
        merged.comment = pair.copy.comment;
        merged.tags = pair.copy.tags;
        merged.color = pair.copy.color;
        merged.quads = pair.copy.quads;
        merged.ink = pair.copy.ink;
        merged.page = pair.copy.page;
        merged.conflictedFrom.clear();
        result.write.append(cleared(merged));
        result.deleteId = pair.copy.id;
        break;
    }

    case Resolution::KeepBoth:
        if (haveOriginal) result.write.append(cleared(pair.original));
        {
            Markup keep = cleared(pair.copy);
            keep.conflictedFrom.clear();   // it is an independent note now
            result.write.append(keep);
        }
        break;
    }

    return result;
}

}  // namespace conflicts
}  // namespace an
