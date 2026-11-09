// P5 AI test: prompt building + SSE parsing (pure) and a real streaming round trip
// against scripts/fake_openai_server.py (auth header, request body, deltas, cancel).
#include "ai/ai_client.h"
#include "ai/prompts.h"
#include "ai/translator.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QTemporaryDir>
#include <QTimer>
#include <QUrlQuery>

#include <cstdio>

namespace {
// Windows installs expose the interpreter as "python" (or "py"), Unix as "python3".
#ifdef Q_OS_WIN
const char* kPython = "python";
#else
const char* kPython = "python3";
#endif
}  // namespace
#include <functional>

using namespace an::ai;

namespace {

int g_fail = 0;

void check(bool ok, const QString& what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) ++g_fail;
}

// Waits for `condition` while pumping the event loop.
bool waitFor(const std::function<bool()>& cond, int timeoutMs)
{
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > timeoutMs) return false;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

QJsonObject fetchLastRequest(const QString& base, int timeoutMs = 3000)
{
    QNetworkAccessManager nam;
    QNetworkReply* reply = nam.get(QNetworkRequest(QUrl(base + "/last-request")));
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    loop.exec();
    const QByteArray body = reply->readAll();
    reply->deleteLater();
    return QJsonDocument::fromJson(body).object();
}

}  // namespace

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    const QString serverScript =
        argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("scripts/fake_openai_server.py");

    // ---------------------------------------------------------------- 1. pure helpers
    std::printf("\n=== prompt + body helpers ===\n");
    {
        check(AiClient::endpointFor(QStringLiteral("https://api.deepseek.com")) ==
                  QStringLiteral("https://api.deepseek.com/v1/chat/completions"),
              QStringLiteral("endpoint from bare host"));
        check(AiClient::endpointFor(QStringLiteral("https://api.siliconflow.cn/")) ==
                  QStringLiteral("https://api.siliconflow.cn/v1/chat/completions"),
              QStringLiteral("endpoint strips trailing slash"));
        check(AiClient::endpointFor(QStringLiteral("http://127.0.0.1:1234/v1")) ==
                  QStringLiteral("http://127.0.0.1:1234/v1/chat/completions"),
              QStringLiteral("endpoint keeps an explicit /v1"));

        const QByteArray body = AiClient::buildBody(QStringLiteral("deepseek-chat"),
                                                    QStringLiteral("be terse"),
                                                    QStringLiteral("解释一下"));
        const QJsonObject o = QJsonDocument::fromJson(body).object();
        check(o.value("model").toString() == QStringLiteral("deepseek-chat"),
              QStringLiteral("body carries the model"));
        check(o.value("stream").toBool(), QStringLiteral("body asks for streaming"));
        const QJsonArray msgs = o.value("messages").toArray();
        check(msgs.size() == 2, QStringLiteral("body has system + user (%1)").arg(msgs.size()));
        check(msgs.at(0).toObject().value("role").toString() == QStringLiteral("system") &&
                  msgs.at(0).toObject().value("content").toString() == QStringLiteral("be terse"),
              QStringLiteral("system role first"));
        check(msgs.at(1).toObject().value("role").toString() == QStringLiteral("user"),
              QStringLiteral("user message last"));

        QByteArray noSystem = AiClient::buildBody(QStringLiteral("m"), QString(), QStringLiteral("hi"));
        check(QJsonDocument::fromJson(noSystem).object().value("messages").toArray().size() == 1,
              QStringLiteral("empty system role is omitted"));

        const QByteArray vision = AiClient::buildVisionBody(
            QStringLiteral("gpt-4o-mini"), QStringLiteral("ocr"), QStringLiteral("读出文字"),
            {QStringLiteral("data:image/png;base64,AAAA")});
        const QJsonObject vo = QJsonDocument::fromJson(vision).object();
        const QJsonArray vmsgs = vo.value("messages").toArray();
        const QJsonArray parts = vmsgs.last().toObject().value("content").toArray();
        check(!vo.value("stream").toBool(), QStringLiteral("vision body is non-streaming"));
        check(parts.size() == 2 &&
                  parts.at(0).toObject().value("type").toString() == QStringLiteral("text") &&
                  parts.at(1).toObject().value("image_url").toObject().value("url").toString()
                          .startsWith(QStringLiteral("data:image/png;base64,")),
              QStringLiteral("vision body carries text + data URL image"));

        QString content, reasoning;
        bool done = false;
        check(AiClient::parseSseData(R"({"choices":[{"delta":{"content":"Hi"}}]})", &content,
                                     &reasoning, &done) &&
                  content == QStringLiteral("Hi"),
              QStringLiteral("parse content delta"));
        check(AiClient::parseSseData(
                  R"({"choices":[{"delta":{"reasoning_content":"hmm"}}]})", &content, &reasoning,
                  &done) &&
                  reasoning == QStringLiteral("hmm"),
              QStringLiteral("parse reasoning_content delta"));
        check(!AiClient::parseSseData("[DONE]", &content, &reasoning, &done) && done,
              QStringLiteral("[DONE] marks the end of the stream"));
        done = false;
        check(!AiClient::parseSseData("not json", &content, &reasoning, &done) && !done,
              QStringLiteral("garbage frame is ignored"));
    }

    std::printf("\n=== prompts / roles ===\n");
    {
        const QVector<Role> roles = defaultRoles();
        check(roles.size() == 4, QStringLiteral("4 built-in roles (%1)").arg(roles.size()));
        bool allNamed = true;
        for (const Role& r : roles)
            if (r.name.isEmpty() || r.content.isEmpty()) allNamed = false;
        check(allNamed, QStringLiteral("every built-in role has a name and instructions"));

        const QString prompt = buildUserPrompt(PromptAction::Explain, QStringLiteral("标注即思考"),
                                               QStringLiteral("测试书"), 6);
        check(prompt.contains(QStringLiteral("标注即思考")), QStringLiteral("prompt embeds the selection"));
        check(prompt.contains(QStringLiteral("第 7 页")), QStringLiteral("prompt carries the page (1-based)"));
        check(prompt.contains(QStringLiteral("测试书")), QStringLiteral("prompt carries the book title"));

        const QString tr = buildUserPrompt(PromptAction::Translate, QStringLiteral("hello"),
                                           QStringLiteral("b"), 0, QStringLiteral("英文"));
        check(tr.contains(QStringLiteral("英文")), QStringLiteral("translate honours the target language"));
        check(tr.contains(QStringLiteral("hello")) && tr.contains(QStringLiteral("%s")) == false,
              QStringLiteral("translation prompt substitutes selected text in its format string"));

        QTemporaryDir tmp;
        const QString path = tmp.path() + "/roles.json";
        QString err;
        check(saveRoles(path, roles, &err), QStringLiteral("save roles: %1").arg(err));
        const QVector<Role> loaded = loadRoles(path, &err);
        check(loaded.size() == roles.size() && loaded.first().name == roles.first().name,
              QStringLiteral("roles round-trip through JSON"));
        check(!QFile::exists(tmp.path() + "/missing.json"), QStringLiteral("seed path unused"));
        const QVector<Role> seeded = loadRoles(tmp.path() + "/missing.json", &err);
        check(QFile::exists(tmp.path() + "/missing.json") && seeded.size() == 4,
              QStringLiteral("missing roles file is seeded with defaults"));
    }

    // ---------------------------------------------------------------- 2. streaming round trip
    std::printf("\n=== streaming over HTTP ===\n");
    QProcess server;
    server.setProcessChannelMode(QProcess::MergedChannels);
    server.start(QString::fromLatin1(kPython), {serverScript});
    if (!server.waitForStarted(5000) || !server.waitForReadyRead(8000)) {
        std::printf("fatal: cannot start %s\n", qPrintable(serverScript));
        return 1;
    }
    const QString ready = QString::fromUtf8(server.readAllStandardOutput()).trimmed();
    const QString port = ready.section(QLatin1Char(' '), 1, 1);
    check(ready.startsWith(QStringLiteral("READY")) && !port.isEmpty(),
          QStringLiteral("fake server ready: %1").arg(ready));
    const QString base = QStringLiteral("http://127.0.0.1:%1").arg(port);

    {
        AiClient client;
        AiClient::Settings s;
        s.baseUrl = base;
        s.apiKey = QStringLiteral("sk-test-123");
        s.model = QStringLiteral("test-model");
        s.systemRole = QStringLiteral("be terse");
        client.setSettings(s);

        int deltaCount = 0;
        QString streamed;
        QString reasoning;
        QString error = QStringLiteral("<none>");
        bool done = false;

        QObject::connect(&client, &AiClient::delta, [&](const QString& c, const QString& r) {
            ++deltaCount;
            streamed += c;
            reasoning += r;
        });
        QObject::connect(&client, &AiClient::finished,
                         [&](const QString& c, const QString& r, const QString& e) {
                             streamed = c;
                             reasoning = r;
                             error = e;
                             done = true;
                         });

        client.send(QStringLiteral("解释这段内容"));
        check(waitFor([&]() { return done; }, 15000), QStringLiteral("stream finished"));
        check(error.isEmpty(), QStringLiteral("no error (%1)").arg(error));
        check(streamed == QStringLiteral("Hello from fake server"),
              QStringLiteral("content assembled from deltas: \"%1\"").arg(streamed));
        check(reasoning.contains(QStringLiteral("thinking")),
              QStringLiteral("reasoning stream separated: \"%1\"").arg(reasoning.left(24)));
        check(deltaCount >= 3, QStringLiteral("deltas emitted incrementally (%1)").arg(deltaCount));

        const QJsonObject last = fetchLastRequest(base);
        const QJsonObject headers = last.value("headers").toObject();
        const QJsonObject body = last.value("body").toObject();
        check(headers.value("authorization").toString() == QStringLiteral("Bearer sk-test-123"),
              QStringLiteral("Authorization header sent (%1)").arg(headers.value("authorization").toString()));
        check(body.value("model").toString() == QStringLiteral("test-model"),
              QStringLiteral("model sent"));
        check(body.value("stream").toBool(), QStringLiteral("stream flag sent"));
        const QJsonArray msgs = body.value("messages").toArray();
        check(msgs.size() == 2 &&
                  msgs.at(1).toObject().value("content").toString() == QStringLiteral("解释这段内容"),
              QStringLiteral("user prompt reached the server"));
    }

    // ---- error path (401 with OpenAI-style error body) ----
    {
        AiClient client;
        AiClient::Settings s;
        s.baseUrl = base;
        s.apiKey = QStringLiteral("bad");
        s.model = QStringLiteral("fail-model");
        client.setSettings(s);

        bool done = false;
        QString error;
        QObject::connect(&client, &AiClient::finished,
                         [&](const QString&, const QString&, const QString& e) {
                             error = e;
                             done = true;
                         });
        client.send(QStringLiteral("hi"));
        check(waitFor([&]() { return done; }, 15000), QStringLiteral("error stream finished"));
        check(error.contains(QStringLiteral("401")) && error.contains(QStringLiteral("invalid api key")),
              QStringLiteral("api error surfaced: %1").arg(error));
    }

    // ---- cancel path ----
    {
        AiClient client;
        AiClient::Settings s;
        s.baseUrl = base;
        s.apiKey = QStringLiteral("sk-test");
        s.model = QStringLiteral("slow-model");
        client.setSettings(s);

        bool done = false;
        QString error;
        QObject::connect(&client, &AiClient::finished,
                         [&](const QString&, const QString&, const QString& e) {
                             error = e;
                             done = true;
                         });
        client.send(QStringLiteral("stream forever"));
        waitFor([&]() { return client.busy(); }, 3000);
        QCoreApplication::processEvents();
        client.cancel();
        check(waitFor([&]() { return done; }, 5000), QStringLiteral("cancel finishes the request"));
        check(error.contains(QStringLiteral("取消")), QStringLiteral("cancel reported: %1").arg(error));
        check(!client.busy(), QStringLiteral("client is idle after cancel"));
    }

    server.kill();
    server.waitForFinished(3000);

    // ---- web translators: pure parsing / URL building (no network) ----
    {
        using an::ai::Translator;
        check(Translator::targetCode(Translator::Provider::Google, QStringLiteral("简体中文")) ==
                  QStringLiteral("zh-CN"),
              QStringLiteral("Google target code for simplified Chinese"));
        check(Translator::targetCode(Translator::Provider::Bing, QStringLiteral("繁體中文")) ==
                  QStringLiteral("zh-Hant"),
              QStringLiteral("Bing target code for traditional Chinese"));
        check(Translator::targetCode(Translator::Provider::DeepL, QStringLiteral("English")) ==
                  QStringLiteral("EN"),
              QStringLiteral("DeepL target code is upper-case"));

        const QUrl url = Translator::googleUrl(QStringLiteral("hello world"), QStringLiteral("简体中文"));
        check(url.host() == QStringLiteral("translate.googleapis.com") &&
                  QUrlQuery(url).queryItemValue(QStringLiteral("tl")) == QStringLiteral("zh-CN") &&
                  QUrlQuery(url).queryItemValue(QStringLiteral("q")) == QStringLiteral("hello world"),
              QStringLiteral("Google URL carries text and target"));

        const QByteArray google =
            R"([[["你好","hello",null,null,10]],null,"en"])";
        check(Translator::parseGoogle(google) == QStringLiteral("你好"),
              QStringLiteral("Google response parser"));
        const QByteArray googleLines =
            R"([[["Hello ","你好"],["world","世界"]],null,"zh-CN"])";
        check(Translator::parseGoogle(googleLines) == QStringLiteral("Hello world"),
              QStringLiteral("Google multi-segment parser keeps order"));

        const QByteArray bing = R"([{"translations":[{"text":"你好","to":"zh-Hans"}]}])";
        check(Translator::parseBing(bing) == QStringLiteral("你好"),
              QStringLiteral("Bing response parser"));
        const QByteArray deepl =
            R"({"translations":[{"detected_source_language":"EN","text":"你好"}]})";
        check(Translator::parseDeepL(deepl) == QStringLiteral("你好"),
              QStringLiteral("DeepL response parser"));
        const QString body = QString::fromUtf8(
            Translator::deeplBody(QStringLiteral("hello world"), QStringLiteral("简体中文")));
        check(body.contains(QStringLiteral("target_lang=ZH")) &&
                  body.contains(QStringLiteral("text=hello")),
              QStringLiteral("DeepL form body"));
        check(Translator::providerName(Translator::Provider::DeepL) == QStringLiteral("DeepL"),
              QStringLiteral("translator provider label"));
        check(Translator::looksChinese(QStringLiteral("阅读的本质不是把书看完")) &&
                  !Translator::looksChinese(QStringLiteral("hello world")),
              QStringLiteral("Chinese source detection for target switching"));
    }

    std::printf("\n%s (%d failure(s))\n", g_fail == 0 ? "AI CHECKS PASSED" : "AI CHECKS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
