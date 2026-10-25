#include "ai/ai_client.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QTimer>
#include <QUrl>

namespace an {
namespace ai {

AiClient::AiClient(QObject* parent)
    : QObject(parent)
    , nam_(new QNetworkAccessManager(this))
{
}

AiClient::~AiClient()
{
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
    }
}

QString AiClient::endpointFor(const QString& baseUrl)
{
    QString base = baseUrl.trimmed();
    while (base.endsWith(QLatin1Char('/'))) base.chop(1);
    if (base.isEmpty()) base = QStringLiteral("https://api.deepseek.com");
    if (base.endsWith(QLatin1String("/chat/completions"))) return base;
    if (base.contains(QLatin1String("/v1"))) return base + QStringLiteral("/chat/completions");
    return base + QStringLiteral("/v1/chat/completions");
}

QByteArray AiClient::buildBody(const QString& model, const QString& systemRole,
                               const QString& userPrompt)
{
    QJsonObject root;
    root["model"] = model;
    root["stream"] = true;

    QJsonArray messages;
    if (!systemRole.trimmed().isEmpty()) {
        QJsonObject sys;
        sys["role"] = "system";
        sys["content"] = systemRole;
        messages.append(sys);
    }
    QJsonObject user;
    user["role"] = "user";
    user["content"] = userPrompt;
    messages.append(user);

    root["messages"] = messages;
    return QJsonDocument(root).toJson(QJsonDocument::Compact);
}

bool AiClient::parseSseData(const QByteArray& payload, QString* content, QString* reasoning,
                            bool* done)
{
    if (done) *done = false;
    const QByteArray trimmed = payload.trimmed();
    if (trimmed.isEmpty()) return false;
    if (trimmed == "[DONE]") {
        if (done) *done = true;
        return false;
    }

    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(trimmed, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;

    const QJsonArray choices = doc.object().value("choices").toArray();
    if (choices.isEmpty()) return false;
    const QJsonObject choice = choices.first().toObject();
    const QJsonObject deltaObj = choice.value("delta").toObject();

    bool got = false;
    if (deltaObj.contains("content")) {
        const QString piece = deltaObj.value("content").toString();
        if (!piece.isEmpty()) {
            if (content) content->append(piece);
            got = true;
        }
    }
    // reasoning models stream their chain of thought in a separate field
    if (deltaObj.contains("reasoning_content")) {
        const QString piece = deltaObj.value("reasoning_content").toString();
        if (!piece.isEmpty()) {
            if (reasoning) reasoning->append(piece);
            got = true;
        }
    }
    if (choice.value("finish_reason").isString()) {
        if (done) *done = true;
    }
    return got;
}

void AiClient::send(const QString& userPrompt)
{
    if (busy()) cancel();

    content_.clear();
    reasoning_.clear();
    buffer_.clear();

    QNetworkRequest req{QUrl(endpointFor(settings_.baseUrl))};
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!settings_.apiKey.isEmpty())
        req.setRawHeader("Authorization", "Bearer " + settings_.apiKey.toUtf8());
    req.setRawHeader("Accept", "text/event-stream");
    req.setTransferTimeout(settings_.timeoutMs);

    reply_ = nam_->post(req, buildBody(settings_.model, settings_.systemRole, userPrompt));

    connect(reply_, &QNetworkReply::readyRead, this, [this]() {
        if (!reply_) return;
        buffer_.append(reply_->readAll());
        handleBuffer();
    });
    connect(reply_, &QNetworkReply::finished, this, [this]() {
        if (!reply_) return;
        const int status = reply_->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QNetworkReply::NetworkError netErr = reply_->error();
        QString err;
        if (netErr != QNetworkReply::NoError && status != 200) {
            err = reply_->errorString();
            if (status > 0) err = QStringLiteral("HTTP %1：%2").arg(status).arg(err);
            // The error body is usually small and readyRead may already have eaten it.
            QByteArray body = buffer_;
            const QByteArray rest = reply_->readAll();
            if (!rest.isEmpty()) body.append(rest);
            if (!body.isEmpty()) {
                const QJsonObject o = QJsonDocument::fromJson(body).object();
                const QString apiErr = o.value("error").toObject().value("message").toString();
                if (!apiErr.isEmpty()) err = QStringLiteral("HTTP %1：%2").arg(status).arg(apiErr);
            }
        }

        reply_->deleteLater();
        reply_ = nullptr;
        emit finished(content_, reasoning_, err);
    });

    emit started();
}

void AiClient::handleBuffer()
{
    // SSE frames are separated by a blank line; each payload line starts with "data:"
    int sep = -1;
    while ((sep = buffer_.indexOf("\n\n")) >= 0) {
        const QByteArray frame = buffer_.left(sep);
        buffer_.remove(0, sep + 2);
        for (const QByteArray& line : frame.split('\n')) {
            const QByteArray trimmed = line.trimmed();
            if (!trimmed.startsWith("data:")) continue;
            const QByteArray payload = trimmed.mid(5).trimmed();
            bool done = false;
            const QString before = content_;
            const QString beforeR = reasoning_;
            parseSseData(payload, &content_, &reasoning_, &done);
            const QString dc = content_.mid(before.size());
            const QString dr = reasoning_.mid(beforeR.size());
            if (!dc.isEmpty() || !dr.isEmpty()) emit delta(dc, dr);
            if (done && content_.isEmpty() && reasoning_.isEmpty()) {
                // nothing else to emit
            }
        }
    }
}

void AiClient::cancel()
{
    if (!reply_) return;
    reply_->disconnect(this);
    reply_->abort();
    reply_->deleteLater();
    reply_ = nullptr;
    emit finished(content_, reasoning_, QStringLiteral("已取消"));
}

void AiClient::finishWith(const QString& error)
{
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    emit finished(content_, reasoning_, error);
}

}  // namespace ai
}  // namespace an
