#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace an {
namespace ai {

// What the user asked the model to do with the current selection.
enum class PromptAction { Explain, Translate, Summarize, Questions, Custom };

QString actionLabel(PromptAction action);

// Builds the user message for one action. `selection` is the excerpt, `extra` is the
// user's own instruction for Custom.
QString buildUserPrompt(PromptAction action, const QString& selection, const QString& bookTitle,
                        int page, const QString& extra = {});

// A reusable system role ("角色"), editable by the user and persisted as JSON.
struct Role {
    QString name;
    QString content;
    bool builtin = false;
};

QVector<Role> defaultRoles();

// Loads roles from `path`; writes the defaults there if the file does not exist yet.
QVector<Role> loadRoles(const QString& path, QString* error = nullptr);
bool saveRoles(const QString& path, const QVector<Role>& roles, QString* error = nullptr);

}  // namespace ai
}  // namespace an
