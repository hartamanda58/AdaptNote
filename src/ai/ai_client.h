#pragma once

#include <QByteArray>
#include <QObject>
#include <QString>

class QNetworkAccessManager;
class QNetworkReply;

namespace an {
namespace ai {

// Streaming client for OpenAI-compatible chat completions (DeepSeek / SiliconFlow /
// any compatible gateway). Handles `stream: true` SSE frames and the optional
// `reasoning_content` field used by reasoning models.
class AiClient : public QObject {
    Q_OBJECT
public:
    struct Settings {
        QString baseUrl = QStringLiteral("https://api.deepseek.com");
        QString apiKey;
        QString model = QStringLiteral("deepseek-chat");
        QString systemRole;
        int timeoutMs = 120000;
    };

    explicit AiClient(QObject* parent = nullptr);
    ~AiClient() override;

    void setSettings(const Settings& s) { settings_ = s; }
    Settings settings() const { return settings_; }

    bool busy() const { return reply_ != nullptr; }
    void send(const QString& userPrompt);
    void cancel();

    // --- pure helpers (unit tested without network) ---
    static QString endpointFor(const QString& baseUrl);
    static QByteArray buildBody(const QString& model, const QString& systemRole,
                                const QString& userPrompt);
    // Parses one `data: {...}` payload. Returns true when the frame carried a delta.
    static bool parseSseData(const QByteArray& payload, QString* content, QString* reasoning,
                             bool* done);

signals:
    void started();
    void delta(const QString& content, const QString& reasoning);
    void finished(const QString& content, const QString& reasoning, const QString& error);

private:
    void handleBuffer();
    void finishWith(const QString& error);

    QNetworkAccessManager* nam_ = nullptr;
    QNetworkReply* reply_ = nullptr;
    QByteArray buffer_;
    QString content_;
    QString reasoning_;
    Settings settings_;
};

}  // namespace ai
}  // namespace an
