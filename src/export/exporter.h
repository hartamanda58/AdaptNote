#pragma once

#include "core/model.h"

#include <QHash>
#include <QString>
#include <QVector>

namespace an {

struct ExportContext {
    QString docPath;
    QString docTitle;
    QString outPath;
    QString deckName = QStringLiteral("AdaptNote");
    QVector<Markup> markups;
    bool includeImages = true;
    // Library-wide export: group by book (derived from each markup's docPath)
    // instead of a single document title.
    bool groupByDocument = false;
    // Markdown/HTML: organise by the mind map (groups/subgroups) instead of pages.
    bool byGroups = false;
    // Mind map structure per document (id -> groups). When present, XMind and OPML
    // exports follow the group tree instead of the flat page grouping.
    QHash<QString, QVector<MindGroup>> groupsByDoc;
};

namespace exports {

// All exporters return false and fill *error on failure.
bool toMarkdown(const ExportContext& ctx, QString* error = nullptr);
bool toHtml(const ExportContext& ctx, QString* error = nullptr);
bool toOpml(const ExportContext& ctx, QString* error = nullptr);
bool toXmind(const ExportContext& ctx, QString* error = nullptr);
bool toAnki(const ExportContext& ctx, QString* error = nullptr);

// Dispatch by extension: md | html | opml | xmind | apkg
bool byExtension(const ExportContext& ctx, QString* error = nullptr);

}  // namespace exports
}  // namespace an
