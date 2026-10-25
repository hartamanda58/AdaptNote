#include "ai/prompts.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace an {
namespace ai {

QString actionLabel(PromptAction action)
{
    switch (action) {
    case PromptAction::Explain: return QStringLiteral("解释");
    case PromptAction::Translate: return QStringLiteral("翻译");
    case PromptAction::Summarize: return QStringLiteral("总结成卡片");
    case PromptAction::Questions: return QStringLiteral("自测问题");
    case PromptAction::Custom: return QStringLiteral("自定义");
    }
    return {};
}

QString buildUserPrompt(PromptAction action, const QString& selection, const QString& bookTitle,
                        int page, const QString& extra)
{
    const QString trimmed = selection.trimmed();
    const QString where = QStringLiteral("《%1》第 %2 页").arg(bookTitle).arg(page + 1);

    switch (action) {
    case PromptAction::Explain:
        return QStringLiteral("以下内容摘自%1，请用中文解释它的意思，"
                              "指出它想回答的问题、容易误解的地方，并给一个具体例子。"
                              "不要复述原文。\n\n【原文】\n%2")
            .arg(where, trimmed);
    case PromptAction::Translate:
        return QStringLiteral("把下面的内容翻译成%s，只输出译文，不要解释或回答文本中的问题。\n\n%1")
            .arg(extra.trimmed().isEmpty() ? QStringLiteral("中文") : extra.trimmed(), trimmed);
    case PromptAction::Summarize:
        return QStringLiteral("以下是我在%1划的重点，请压缩成一张“知识卡片”："
                              "1) 一句话结论 2) 三条支撑要点 3) 一个能记住的类比。\n\n【摘录】\n%2")
            .arg(where, trimmed);
    case PromptAction::Questions:
        return QStringLiteral("针对%1的这段内容，出 3 道能检验理解的问题（不要答案），"
                              "难度递增，覆盖概念、应用和反例。\n\n【内容】\n%2")
            .arg(where, trimmed);
    case PromptAction::Custom:
        return QStringLiteral("【上下文】%1\n\n【选中内容】\n%2\n\n【我的要求】\n%3")
            .arg(where, trimmed, extra.trimmed().isEmpty() ? QStringLiteral("请帮我理解这段内容。")
                                                           : extra.trimmed());
    }
    return trimmed;
}

QVector<Role> defaultRoles()
{
    return {
        {QStringLiteral("深度阅读者"),
         QStringLiteral("你是一个严谨的深度阅读助手。回答要具体、可验证，指出原文的限定条件；"
                        "不确定时明确说不确定，不要编造引用。"),
         true},
        {QStringLiteral("耐心的讲解者"),
         QStringLiteral("你像一位耐心的老师：先用大白话给出直觉，再补充精确表述，"
                        "最后给一个贴近日常的例子。避免术语堆砌。"),
         true},
        {QStringLiteral("翻译家"),
         QStringLiteral("你是专业译者：只输出译文，保留原文层次，不解释、不回答文中提出的问题；"
                        "遇到术语时按学科惯例处理。"),
         true},
        {QStringLiteral("提问教练"),
         QStringLiteral("你擅长用问题检验理解：问题必须能从给定文本推出或证伪，"
                        "避免“你觉得呢”这类空问题。"),
         true},
    };
}

QVector<Role> loadRoles(const QString& path, QString* error)
{
    QFile f(path);
    if (!f.exists()) {
        const QVector<Role> defaults = defaultRoles();
        QString ignored;
        saveRoles(path, defaults, &ignored);
        return defaults;
    }
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = f.errorString();
        return defaultRoles();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isArray()) {
        if (error) *error = QStringLiteral("roles file is not a JSON array");
        return defaultRoles();
    }
    QVector<Role> roles;
    for (const QJsonValue& v : doc.array()) {
        const QJsonObject o = v.toObject();
        Role r;
        r.name = o.value("name").toString();
        r.content = o.value("content").toString();
        r.builtin = o.value("builtin").toBool();
        if (!r.content.isEmpty()) roles.append(r);
    }
    if (roles.isEmpty()) return defaultRoles();
    return roles;
}

bool saveRoles(const QString& path, const QVector<Role>& roles, QString* error)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonArray arr;
    for (const Role& r : roles) {
        QJsonObject o;
        o["name"] = r.name;
        o["content"] = r.content;
        o["builtin"] = r.builtin;
        arr.append(o);
    }
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(arr).toJson(QJsonDocument::Indented));
    return true;
}

}  // namespace ai
}  // namespace an
