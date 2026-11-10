#include "translator.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUuid>
#include <QUrlQuery>

namespace an {
namespace ai {

namespace {

QString firstTranslationText(const QByteArray& json, const char* listKey = "translations")
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) return {};
    const QJsonArray root = doc.array();
    if (root.isEmpty() || !root.at(0).isObject()) return {};
    const QJsonArray items = root.at(0).toObject().value(QLatin1String(listKey)).toArray();
    if (items.isEmpty() || !items.at(0).isObject()) return {};
    return items.at(0).toObject().value(QStringLiteral("text")).toString().trimmed();
}

}  // namespace

Translator::Translator(QObject* parent)
    : QObject(parent)
    , net_(new QNetworkAccessManager(this))
{
}

Translator::~Translator()
{
    cancel();
}

QString Translator::targetCode(Provider provider, const QString& uiTarget)
{
    const QString t = uiTarget.trimmed();
    const bool traditional = t.contains(QStringLiteral("繁體")) ||
                             t.contains(QStringLiteral("繁体")) ||
                             t.contains(QStringLiteral("Traditional"));
    const bool chinese = traditional || t.contains(QStringLiteral("中文")) ||
                         t.contains(QStringLiteral("Chinese"));
    const bool english = t.contains(QStringLiteral("English")) ||
                         t.contains(QStringLiteral("英文"));
    const bool japanese = t.contains(QStringLiteral("日本語")) ||
                          t.contains(QStringLiteral("日语")) ||
                          t.contains(QStringLiteral("日文"));
    const bool korean = t.contains(QStringLiteral("한국")) ||
                        t.contains(QStringLiteral("韩语")) ||
                        t.contains(QStringLiteral("韓語"));

    switch (provider) {
    case Provider::DeepL:
    case Provider::DeepLX:
    case Provider::DeepLFree:
        if (traditional) return QStringLiteral("ZH-HANT");
        if (chinese) return QStringLiteral("ZH");
        if (english) return QStringLiteral("EN");
        if (japanese) return QStringLiteral("JA");
        if (korean) return QStringLiteral("KO");
        return QStringLiteral("ZH");
    case Provider::Bing:
        if (traditional) return QStringLiteral("zh-Hant");
        if (chinese) return QStringLiteral("zh-Hans");
        if (english) return QStringLiteral("en");
        if (japanese) return QStringLiteral("ja");
        if (korean) return QStringLiteral("ko");
        return QStringLiteral("zh-Hans");
    case Provider::Auto:
    case Provider::Google:
    default:
        if (traditional) return QStringLiteral("zh-TW");
        if (chinese) return QStringLiteral("zh-CN");
        if (english) return QStringLiteral("en");
        if (japanese) return QStringLiteral("ja");
        if (korean) return QStringLiteral("ko");
        return QStringLiteral("zh-CN");
    }
}

bool Translator::looksChinese(const QString& text)
{
    int han = 0;
    int visible = 0;
    for (const QChar ch : text) {
        if (ch.isSpace()) continue;
        ++visible;
        const ushort u = ch.unicode();
        if ((u >= 0x4E00 && u <= 0x9FFF) || (u >= 0x3400 && u <= 0x4DBF))
            ++han;
    }
    return visible > 0 && han * 2 >= visible;
}

QString Translator::providerName(Provider provider)
{
    switch (provider) {
    case Provider::Google: return QStringLiteral("Google");
    case Provider::Bing: return QStringLiteral("Bing");
    case Provider::DeepL: return QStringLiteral("DeepL");
    case Provider::DeepLX: return QStringLiteral("DeepLX");
    case Provider::DeepLFree: return QStringLiteral("DeepL 免费");
    case Provider::Auto:
    default: return QStringLiteral("自动");
    }
}

QUrl Translator::googleUrl(const QString& text, const QString& uiTarget)
{
    QUrl url(QStringLiteral("https://translate.googleapis.com/translate_a/single"));
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("client"), QStringLiteral("gtx"));
    query.addQueryItem(QStringLiteral("sl"), QStringLiteral("auto"));
    query.addQueryItem(QStringLiteral("tl"), targetCode(Provider::Google, uiTarget));
    query.addQueryItem(QStringLiteral("dt"), QStringLiteral("t"));
    query.addQueryItem(QStringLiteral("q"), text);
    url.setQuery(query);
    return url;
}

QString Translator::parseGoogle(const QByteArray& json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isArray()) return {};
    const QJsonArray root = doc.array();
    if (root.isEmpty() || !root.at(0).isArray()) return {};
    QString out;
    for (const QJsonValue& value : root.at(0).toArray()) {
        const QJsonArray line = value.toArray();
        if (!line.isEmpty() && line.at(0).isString()) out += line.at(0).toString();
    }
    return out.trimmed();
}

QString Translator::parseBing(const QByteArray& json)
{
    return firstTranslationText(json);
}

QByteArray Translator::deeplBody(const QString& text, const QString& uiTarget)
{
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("text"), text);
    query.addQueryItem(QStringLiteral("target_lang"), targetCode(Provider::DeepL, uiTarget));
    return query.toString(QUrl::FullyEncoded).toUtf8();
}

namespace {
QString freeInstanceId()
{
    static const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return id;
}
QString freeSessionId()
{
    static const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
    return id;
}
QString& freeEndpointRef()
{
    static QString endpoint = QStringLiteral("https://oneshot-free.www.deepl.com/v1/translate");
    return endpoint;
}
QString freeEndpoint()
{
    return freeEndpointRef();
}
}  // namespace

void Translator::setFreeEndpointForTest(const QString& url)
{
    // The fake server in tests uses this; production never calls it.
    freeEndpointRef() = url;
}

QString Translator::freeTargetCode(const QString& uiTarget)
{
    const QString code = targetCode(Provider::DeepL, uiTarget);
    if (code == QStringLiteral("ZH")) return QStringLiteral("zh-Hans");
    if (code == QStringLiteral("ZH-HANT")) return QStringLiteral("zh-Hant");
    if (code == QStringLiteral("EN")) return QStringLiteral("en-US");
    if (code == QStringLiteral("PT")) return QStringLiteral("pt-BR");
    return code.toLower();
}

QByteArray Translator::freeBody(const QString& text, const QString& uiTarget)
{
    QJsonObject app;
    app.insert(QStringLiteral("os"), QStringLiteral("iOS"));
    app.insert(QStringLiteral("os_version"), QStringLiteral("26.0"));
    app.insert(QStringLiteral("app_version"), QStringLiteral("26.52"));
    app.insert(QStringLiteral("app_build"), QStringLiteral("5443737"));
    app.insert(QStringLiteral("instance_id"), freeInstanceId());

    QJsonObject body;
    body.insert(QStringLiteral("text"), QJsonArray{text});
    body.insert(QStringLiteral("target_lang"), freeTargetCode(uiTarget));
    body.insert(QStringLiteral("usage_type"), QStringLiteral("translate"));
    body.insert(QStringLiteral("app_information"), app);
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QString Translator::parseFreeDeepL(const QByteArray& json)
{
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    const QJsonArray items = doc.object().value(QStringLiteral("translations")).toArray();
    if (items.isEmpty() || !items.at(0).isObject()) return {};
    return items.at(0).toObject().value(QStringLiteral("text")).toString().trimmed();
}

QVector<QString> Translator::splitFreeText(const QString& text, int max)
{
    // The anonymous oneshot endpoint rejects >1500 UTF-16 code units. Split at
    // sentence/clause boundaries so paragraphs still translate in one click.
    QVector<QString> parts;
    int pos = 0;
    static const QString stops = QStringLiteral("。！？!?；;.\n");
    while (pos < text.size()) {
        int len = qMin(max, text.size() - pos);
        if (pos + len < text.size()) {
            for (int i = pos + len; i > pos + max / 2; --i) {
                if (stops.contains(text.at(i - 1))) {
                    len = i - pos;
                    break;
                }
            }
        }
        parts.append(text.mid(pos, len));
        pos += len;
    }
    if (parts.isEmpty()) parts.append(QString());
    return parts;
}

QString Translator::normalizeDeepLXUrl(const QString& url)
{
    const QString trimmed = url.trimmed();
    if (trimmed.isEmpty()) return QStringLiteral("http://127.0.0.1:1188/translate");
    QUrl parsed(trimmed);
    if (!parsed.isValid() || parsed.scheme().isEmpty()) return trimmed;
    if (parsed.path().isEmpty() || parsed.path() == QStringLiteral("/"))
        parsed.setPath(QStringLiteral("/translate"));
    return parsed.toString();
}

QByteArray Translator::deeplxBody(const QString& text, const QString& uiTarget)
{
    QJsonObject body;
    body.insert(QStringLiteral("text"), text);
    body.insert(QStringLiteral("source_lang"), QStringLiteral("auto"));
    body.insert(QStringLiteral("target_lang"), targetCode(Provider::DeepLX, uiTarget));
    return QJsonDocument(body).toJson(QJsonDocument::Compact);
}

QString Translator::parseDeepLX(const QByteArray& body)
{
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isObject()) {
        const QJsonObject object = doc.object();
        const QJsonValue data = object.value(QStringLiteral("data"));
        if (data.isString() && !data.toString().trimmed().isEmpty())
            return data.toString().trimmed();
        const QJsonArray items = object.value(QStringLiteral("translations")).toArray();
        if (!items.isEmpty() && items.at(0).isObject())
            return items.at(0).toObject().value(QStringLiteral("text")).toString().trimmed();
        return {};
    }
    if (doc.isArray()) {
        const QJsonArray root = doc.array();
        if (!root.isEmpty() && root.at(0).isObject())
            return root.at(0).toObject().value(QStringLiteral("text")).toString().trimmed();
    }
    // Some self-hosted forks return the translation as plain text.
    const QString plain = QString::fromUtf8(body).trimmed();
    if (!plain.startsWith(QLatin1Char('{')) && !plain.startsWith(QLatin1Char('[')))
        return plain;
    return {};
}

QString Translator::parseDeepL(const QByteArray& json)
{
    // DeepL wraps translations in an object, unlike Bing/Google's array envelope.
    const QJsonDocument doc = QJsonDocument::fromJson(json);
    if (!doc.isObject()) return {};
    const QJsonArray items =
        doc.object().value(QStringLiteral("translations")).toArray();
    if (items.isEmpty() || !items.at(0).isObject()) return {};
    return items.at(0).toObject().value(QStringLiteral("text")).toString().trimmed();
}

void Translator::cancel()
{
    QNetworkReply* reply = reply_.data();
    reply_ = nullptr;
    if (!reply) return;
    reply->disconnect(this);
    reply->abort();
    reply->deleteLater();
}

void Translator::translate(const QString& text, const QString& uiTarget, Provider provider,
                           const QString& deeplKey, const QString& deeplxUrl,
                           const QString& deeplxToken)
{
    cancel();
    text_ = text;
    target_ = uiTarget;
    // Same rule as the AI translator: if the source is already Chinese but the user
    // asked for Chinese, return English instead of echoing the input back.
    if (targetCode(Provider::Google, uiTarget).startsWith(QStringLiteral("zh")) &&
        looksChinese(text))
        target_ = QStringLiteral("English");
    deeplKey_ = deeplKey.trimmed();
    deeplxUrl_ = normalizeDeepLXUrl(deeplxUrl);
    deeplxToken_ = deeplxToken.trimmed();
    provider_ = provider;
    autoFellBack_ = false;
    freeChunks_.clear();
    freeIndex_ = 0;
    freeResult_.clear();
    switch (provider_) {
    case Provider::Google: requestGoogle(); break;
    case Provider::Bing: requestBingToken(); break;
    case Provider::DeepL: requestDeepL(); break;
    case Provider::DeepLX: requestDeepLX(); break;
    case Provider::DeepLFree:
        freeChunks_ = splitFreeText(text_);
        requestFreeDeepL();
        break;
    // Bing's Edge endpoint is reachable from mainland China more often than Google,
    // so "auto" tries Bing first and falls back to Google.
    case Provider::Auto: requestBingToken(); break;
    }
}

void Translator::dropReply(QNetworkReply* reply)
{
    if (reply) reply->deleteLater();
}

void Translator::finishWith(const QString& text, const QString& provider)
{
    emit finished(text, provider, QString());
}

void Translator::failOrFallback(const QString& error)
{
    if (provider_ == Provider::Auto && !autoFellBack_) {
        autoFellBack_ = true;
        requestGoogle();
        return;
    }
    emit finished(QString(), providerName(provider_), error);
}

void Translator::requestGoogle()
{
    QNetworkRequest request{googleUrl(text_, target_)};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AdaptNote/0.22"));
    request.setTransferTimeout(15000);
    reply_ = net_->get(request);
    connect(reply_, &QNetworkReply::finished, this, &Translator::handleGoogle);
}

void Translator::handleGoogle()
{
    QNetworkReply* reply = reply_.data();
    if (!reply) return;
    reply_ = nullptr;
    const QByteArray body = reply->readAll();
    const QNetworkReply::NetworkError error = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString netError = reply->errorString();
    dropReply(reply);
    if (error != QNetworkReply::NoError) {
        const QString detail = QString::fromUtf8(body).trimmed().left(140);
        if (status > 0) netError += QStringLiteral("（HTTP %1）").arg(status);
        if (!detail.isEmpty()) netError += QStringLiteral("：%1").arg(detail);
        else if (netError.isEmpty()) netError = QStringLiteral("网络被拦截或超时");
        failOrFallback(QStringLiteral("Google 请求失败：%1").arg(netError));
        return;
    }
    const QString text = parseGoogle(body);
    if (text.isEmpty()) {
        failOrFallback(QStringLiteral("Google 没有返回译文"));
        return;
    }
    finishWith(text, QStringLiteral("Google"));
}

void Translator::requestBingToken()
{
    QNetworkRequest request{QUrl(QStringLiteral("https://edge.microsoft.com/translate/auth"))};
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AdaptNote/0.22"));
    request.setTransferTimeout(15000);
    reply_ = net_->get(request);
    connect(reply_, &QNetworkReply::finished, this, &Translator::handleBingToken);
}

void Translator::handleBingToken()
{
    QNetworkReply* reply = reply_.data();
    if (!reply) return;
    reply_ = nullptr;
    const QByteArray body = reply->readAll();
    const QNetworkReply::NetworkError error = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString netError = reply->errorString();
    dropReply(reply);
    if (error != QNetworkReply::NoError) {
        const QString detail = QString::fromUtf8(body).trimmed().left(140);
        if (status > 0) netError += QStringLiteral("（HTTP %1）").arg(status);
        if (!detail.isEmpty()) netError += QStringLiteral("：%1").arg(detail);
        else if (netError.isEmpty()) netError = QStringLiteral("网络被拦截或超时");
        failOrFallback(QStringLiteral("Bing 获取授权失败：%1").arg(netError));
        return;
    }
    const QString token = QString::fromUtf8(body).trimmed();
    if (token.isEmpty() || token.count(QLatin1Char('.')) != 2) {
        failOrFallback(QStringLiteral("Bing 授权返回异常"));
        return;
    }
    requestBing(token);
}

void Translator::requestBing(const QString& token)
{
    QUrl url{QStringLiteral("https://api-edge.cognitive.microsofttranslator.com/translate")};
    QUrlQuery query;
    query.addQueryItem(QStringLiteral("api-version"), QStringLiteral("3.0"));
    query.addQueryItem(QStringLiteral("from"), QString());
    query.addQueryItem(QStringLiteral("to"), targetCode(Provider::Bing, target_));
    url.setQuery(query);

    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "Bearer " + token.toUtf8());
    request.setHeader(QNetworkRequest::UserAgentHeader,
                      QStringLiteral("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AdaptNote/0.22"));
    request.setTransferTimeout(20000);

    QJsonObject item;
    item.insert(QStringLiteral("Text"), text_);
    const QByteArray payload = QJsonDocument(QJsonArray{item}).toJson(QJsonDocument::Compact);
    reply_ = net_->post(request, payload);
    connect(reply_, &QNetworkReply::finished, this, &Translator::handleBing);
}

void Translator::handleBing()
{
    QNetworkReply* reply = reply_.data();
    if (!reply) return;
    reply_ = nullptr;
    const QByteArray body = reply->readAll();
    const QNetworkReply::NetworkError error = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString netError = reply->errorString();
    dropReply(reply);
    if (error != QNetworkReply::NoError) {
        const QString detail = QString::fromUtf8(body).trimmed().left(140);
        if (status > 0) netError += QStringLiteral("（HTTP %1）").arg(status);
        if (!detail.isEmpty()) netError += QStringLiteral("：%1").arg(detail);
        else if (netError.isEmpty()) netError = QStringLiteral("网络被拦截或超时");
        failOrFallback(QStringLiteral("Bing 请求失败：%1").arg(netError));
        return;
    }
    const QString text = parseBing(body);
    if (text.isEmpty()) {
        failOrFallback(QStringLiteral("Bing 没有返回译文"));
        return;
    }
    finishWith(text, QStringLiteral("Bing"));
}

void Translator::requestDeepL()
{
    if (deeplKey_.isEmpty()) {
        emit finished(QString(), QStringLiteral("DeepL"),
                      QStringLiteral("DeepL 需要 API Key（免费版 Key 以 “:fx” 结尾），"
                                     "请在翻译窗口里填写"));
        return;
    }
    const QString endpoint = deeplKey_.endsWith(QStringLiteral(":fx"), Qt::CaseInsensitive)
                                 ? QStringLiteral("https://api-free.deepl.com/v2/translate")
                                 : QStringLiteral("https://api.deepl.com/v2/translate");
    QNetworkRequest request{QUrl(endpoint)};
    request.setHeader(QNetworkRequest::ContentTypeHeader,
                      QStringLiteral("application/x-www-form-urlencoded"));
    request.setRawHeader("Authorization", "DeepL-Auth-Key " + deeplKey_.toUtf8());
    request.setTransferTimeout(20000);
    reply_ = net_->post(request, deeplBody(text_, target_));
    connect(reply_, &QNetworkReply::finished, this, &Translator::handleDeepL);
}

void Translator::requestDeepLX()
{
    QNetworkRequest request{QUrl(deeplxUrl_)};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!deeplxToken_.isEmpty())
        request.setRawHeader("Authorization", "Bearer " + deeplxToken_.toUtf8());
    request.setTransferTimeout(30000);
    reply_ = net_->post(request, deeplxBody(text_, target_));
    connect(reply_, &QNetworkReply::finished, this, &Translator::handleDeepLX);
}

void Translator::handleDeepLX()
{
    QNetworkReply* reply = reply_.data();
    if (!reply) return;
    reply_ = nullptr;
    const QByteArray body = reply->readAll();
    const QNetworkReply::NetworkError error = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString netError = reply->errorString();
    dropReply(reply);
    if (error != QNetworkReply::NoError) {
        const QString detail = QString::fromUtf8(body).trimmed().left(140);
        if (status > 0) netError += QStringLiteral("（HTTP %1）").arg(status);
        if (!detail.isEmpty()) netError += QStringLiteral("：%1").arg(detail);
        const QString hint =
            (status == 401 || status == 403)
                ? QStringLiteral("DeepLX 需要 Token，或实例不可用；也可以换成本机 http://127.0.0.1:1188/translate")
                : QStringLiteral("检查 DeepLX 地址是否已启动（Docker 默认 1188 端口）");
        emit finished(QString(), QStringLiteral("DeepLX"),
                      QStringLiteral("%1；%2").arg(netError, hint));
        return;
    }
    const QString text = parseDeepLX(body);
    if (text.isEmpty()) {
        emit finished(QString(), QStringLiteral("DeepLX"),
                      QStringLiteral("DeepLX 返回为空或格式无法识别"));
        return;
    }
    finishWith(text, QStringLiteral("DeepLX"));
}

void Translator::requestFreeDeepL()
{
    if (freeIndex_ < 0 || freeIndex_ >= freeChunks_.size()) {
        finishWith(freeResult_, QStringLiteral("DeepL 免费"));
        return;
    }
    QNetworkRequest request{QUrl(freeEndpoint())};
    request.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    request.setRawHeader("Authorization", "None");
    request.setRawHeader("User-Agent", "DeepL/26.52 CFNetwork/3826.600.41 Darwin/25.0.0");
    request.setRawHeader("x-app-os-version", "26.0");
    request.setRawHeader("x-app-instance-id", freeInstanceId().toUtf8());
    request.setRawHeader("x-app-session-id", freeSessionId().toUtf8());
    request.setTransferTimeout(30000);
    reply_ = net_->post(request, freeBody(freeChunks_.at(freeIndex_), target_));
    connect(reply_, &QNetworkReply::finished, this, &Translator::handleFreeDeepL);
}

void Translator::handleFreeDeepL()
{
    QNetworkReply* reply = reply_.data();
    if (!reply) return;
    reply_ = nullptr;
    const QByteArray body = reply->readAll();
    const QNetworkReply::NetworkError error = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    QString netError = reply->errorString();
    dropReply(reply);

    if (status == 429) {
        emit finished(QString(), QStringLiteral("DeepL 免费"),
                      QStringLiteral("DeepL 免费接口暂时限流了；等一会儿再试，或改用 DeepLX / 官方 Key"));
        return;
    }
    if (error != QNetworkReply::NoError) {
        const QString detail = QString::fromUtf8(body).trimmed().left(140);
        if (status > 0) netError += QStringLiteral("（HTTP %1）").arg(status);
        if (!detail.isEmpty()) netError += QStringLiteral("：%1").arg(detail);
        emit finished(QString(), QStringLiteral("DeepL 免费"),
                      QStringLiteral("DeepL 免费接口失败：%1").arg(netError));
        return;
    }
    const QString translated = parseFreeDeepL(body);
    if (translated.isEmpty()) {
        emit finished(QString(), QStringLiteral("DeepL 免费"),
                      QStringLiteral("DeepL 免费接口返回为空或格式无法识别"));
        return;
    }
    freeResult_ += translated;
    ++freeIndex_;
    if (freeIndex_ < freeChunks_.size()) {
        requestFreeDeepL();
        return;
    }
    finishWith(freeResult_, QStringLiteral("DeepL 免费"));
}

void Translator::handleDeepL()
{
    QNetworkReply* reply = reply_.data();
    if (!reply) return;
    reply_ = nullptr;
    const QByteArray body = reply->readAll();
    const QNetworkReply::NetworkError error = reply->error();
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    dropReply(reply);
    if (error != QNetworkReply::NoError) {
        const QString hint = (status == 403 || status == 401)
                                 ? QStringLiteral("DeepL 鉴权失败，请检查 API Key（免费版以 :fx 结尾）")
                                 : QStringLiteral("DeepL 请求失败：%1").arg(QString::fromUtf8(body));
        emit finished(QString(), QStringLiteral("DeepL"), hint);
        return;
    }
    const QString text = parseDeepL(body);
    if (text.isEmpty()) {
        emit finished(QString(), QStringLiteral("DeepL"), QStringLiteral("DeepL 没有返回译文"));
        return;
    }
    finishWith(text, QStringLiteral("DeepL"));
}

}  // namespace ai
}  // namespace an
