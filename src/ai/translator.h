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
    enum class Provider { Auto, Google, Bing, DeepL, DeepLX, DeepLFree };

    explicit Translator(QObject* parent = nullptr);
    ~Translator() override;

    void translate(const QString& text, const QString& uiTarget, Provider provider,
                   const QString& deeplKey = {}, const QString& deeplxUrl = {},
                   const QString& deeplxToken = {});
    void cancel();

    // Pure helpers, unit tested without network access.
    static QString targetCode(Provider provider, const QString& uiTarget);
    static QUrl googleUrl(const QString& text, const QString& uiTarget);
    static QString parseGoogle(const QByteArray& json);
    static QString parseBing(const QByteArray& json);
    static bool parseBingPage(const QByteArray& html, QString* ig, QString* key,
                              QString* token);
    static QByteArray deeplBody(const QString& text, const QString& uiTarget);
    static QString parseDeepL(const QByteArray& json);
    static QString normalizeDeepLXUrl(const QString& url);
    static QString freeTargetCode(const QString& uiTarget);
    static QByteArray freeBody(const QString& text, const QString& uiTarget);
    static QString parseFreeDeepL(const QByteArray& json);
    static QVector<QString> splitFreeText(const QString& text, int max = 1400);
    static void setFreeEndpointForTest(const QString& url);
    static QByteArray deeplxBody(const QString& text, const QString& uiTarget);
    static QString parseDeepLX(const QByteArray& body);
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
    void requestDeepLX();
    void handleDeepLX();
    void requestFreeDeepL();
    void handleFreeDeepL();
    void handleGoogle();
    void handleBingToken();
    void handleBing();
    void requestBingWebPage();
    void handleBingWebPage();
    void requestBingWebTranslate();
    void handleBingWebTranslate();
    void startBingWebFallback(const QString& reason);
    void handleDeepL();
    void failOrFallback(const QString& error);
    void finishWith(const QString& text, const QString& provider);
    void dropReply(QNetworkReply* reply);

    QNetworkAccessManager* net_ = nullptr;
    QPointer<QNetworkReply> reply_;
    QString text_;
    QString target_;
    QString deeplKey_;
    QString bingIg_;
    QString bingKey_;
    QString bingToken_;
    bool bingWebTried_ = false;
    QString deeplxUrl_;
    QString deeplxToken_;
    QVector<QString> freeChunks_;
    int freeIndex_ = 0;
    QString freeResult_;
    Provider provider_ = Provider::Auto;
    bool autoFellBack_ = false;
};

}  // namespace ai
}  // namespace an
