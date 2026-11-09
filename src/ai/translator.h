#pragma once

#include <QByteArray>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QUrl>

class QNetworkAccessManager;
class QNetworkReply;

namespace an {
namespace ai {

// Keyless Google / Bing translation plus DeepL (API key). The AI path stays in AiClient;
// this class only speaks the three web translation APIs so it can work without a model.
class Translator : public QObject {
    Q_OBJECT
public:
    enum class Provider { Auto, Google, Bing, DeepL };

    explicit Translator(QObject* parent = nullptr);
    ~Translator() override;

    void translate(const QString& text, const QString& uiTarget, Provider provider,
                   const QString& deeplKey = {});
    void cancel();

    // Pure helpers, unit tested without network access.
    static QString targetCode(Provider provider, const QString& uiTarget);
    static QUrl googleUrl(const QString& text, const QString& uiTarget);
    static QString parseGoogle(const QByteArray& json);
    static QString parseBing(const QByteArray& json);
    static QByteArray deeplBody(const QString& text, const QString& uiTarget);
    static QString parseDeepL(const QByteArray& json);
    static QString providerName(Provider provider);
    static bool looksChinese(const QString& text);

signals:
    // Empty `error` means success. `provider` names the engine that answered.
    void finished(const QString& text, const QString& provider, const QString& error);

private:
    void requestGoogle();
    void requestBingToken();
    void requestBing(const QString& token);
    void requestDeepL();
    void handleGoogle();
    void handleBingToken();
    void handleBing();
    void handleDeepL();
    void failOrFallback(const QString& error);
    void finishWith(const QString& text, const QString& provider);
    void dropReply(QNetworkReply* reply);

    QNetworkAccessManager* net_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QString text_;
    QString target_;
    QString deeplKey_;
    Provider provider_ = Provider::Auto;
    bool autoFellBack_ = false;
};

}  // namespace ai
}  // namespace an
