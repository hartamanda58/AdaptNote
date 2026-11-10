// P5 AI test: prompt building + SSE parsing (pure) and a real streaming round trip
// against scripts/fake_openai_server.py (auth header, request body, deltas, cancel).
#include "ai/ai_client.h"
#include "ai/prompts.h"
#include "ai/translator.h"
#include "update/updater.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
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

    // ---------------------------------------------------------------- 4. DeepLX round trip
    {
        const QString dlxScript =
            QFileInfo(serverScript).absoluteDir().filePath(QStringLiteral("fake_deeplx_server.py"));
        if (!QFileInfo::exists(dlxScript)) {
            check(false, QStringLiteral("fake DeepLX server script exists"));
        } else {
            QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
            if (python.isEmpty())
                python = QStandardPaths::findExecutable(QStringLiteral("python"));
            QProcess dlx;
            dlx.start(python,
                      {dlxScript, QStringLiteral("18089"), QStringLiteral("secret")});
            check(dlx.waitForStarted(3000), QStringLiteral("fake DeepLX server starts"));
            QElapsedTimer startup;
            startup.start();
            while (startup.elapsed() < 400) QCoreApplication::processEvents();

            using an::ai::Translator;
            Translator translator;
            QString text;
            QString provider;
            QString error;
            bool done = false;
            QObject::connect(&translator, &Translator::finished,
                             [&](const QString& t, const QString& p, const QString& e) {
                                 text = t;
                                 provider = p;
                                 error = e;
                                 done = true;
                             });
            translator.translate(QStringLiteral("hello"), QStringLiteral("简体中文"),
                                 Translator::Provider::DeepLX, QString(),
                                 QStringLiteral("http://127.0.0.1:18089/translate"),
                                 QStringLiteral("secret"));
            check(waitFor([&]() { return done; }, 5000),
                  QStringLiteral("DeepLX request completes"));
            check(error.isEmpty() && text == QStringLiteral("DLX[ZH] hello") &&
                      provider == QStringLiteral("DeepLX"),
                  QStringLiteral("DeepLX translated via the local server (%1 / %2)")
                      .arg(provider, error));

            done = false;
            text.clear();
            provider.clear();
            error.clear();
            translator.translate(QStringLiteral("hello"), QStringLiteral("简体中文"),
                                 Translator::Provider::DeepLX, QString(),
                                 QStringLiteral("http://127.0.0.1:18089/translate"), QString());
            check(waitFor([&]() { return done; }, 5000),
                  QStringLiteral("DeepLX auth failure completes"));
            check(!error.isEmpty() && text.isEmpty(),
                  QStringLiteral("DeepLX reports a token failure: %1").arg(error));

            Translator::setFreeEndpointForTest(
                QStringLiteral("http://127.0.0.1:18089/v1/translate"));
            done = false;
            text.clear();
            provider.clear();
            error.clear();
            translator.translate(QStringLiteral("hello"), QStringLiteral("简体中文"),
                                 Translator::Provider::DeepLFree);
            check(waitFor([&]() { return done; }, 5000),
                  QStringLiteral("built-in free DeepL completes"));
            check(error.isEmpty() && text == QStringLiteral("FREE[zh-Hans] hello") &&
                      provider == QStringLiteral("DeepL 免费"),
                  QStringLiteral("built-in free DeepL translated via oneshot (%1 / %2)")
                      .arg(provider, error));

            dlx.kill();
            dlx.waitForFinished(2000);
        }
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

        // ---- DeepLX (free/self-hosted DeepL proxy) ----
        check(Translator::targetCode(Translator::Provider::DeepLX, QStringLiteral("简体中文")) ==
                  QStringLiteral("ZH"),
              QStringLiteral("DeepLX uses DeepL target codes"));
        check(Translator::normalizeDeepLXUrl(QStringLiteral("http://127.0.0.1:1188")) ==
                  QStringLiteral("http://127.0.0.1:1188/translate"),
              QStringLiteral("DeepLX URL gets /translate appended"));
        check(Translator::normalizeDeepLXUrl(QStringLiteral("https://example.test/translate")) ==
                  QStringLiteral("https://example.test/translate"),
              QStringLiteral("DeepLX URL with a path is kept"));
        check(Translator::parseDeepLX(
                  R"({"code":200,"message":"success","data":"你好"})") == QStringLiteral("你好"),
              QStringLiteral("DeepLX {code,data} response parser"));
        check(Translator::parseDeepLX(
                  R"({"translations":[{"text":"你好"}]})") == QStringLiteral("你好"),
              QStringLiteral("DeepLX translations response parser"));
        check(Translator::parseDeepLX("plain text") == QStringLiteral("plain text"),
              QStringLiteral("DeepLX plain-text fallback"));
        const QString dlxBody = QString::fromUtf8(
            Translator::deeplxBody(QStringLiteral("hello"), QStringLiteral("简体中文")));
        check(dlxBody.contains(QStringLiteral("\"source_lang\":\"auto\"")) &&
                  dlxBody.contains(QStringLiteral("\"target_lang\":\"ZH\"")),
              QStringLiteral("DeepLX request body is JSON with auto source"));

        // ---- updater parsing / script generation ----
        {
            using an::update::Updater;
            const QByteArray release = R"({
              "tag_name": "gui-abcdef1",
              "name": "AdaptNote abcdef1",
              "body": "notes",
              "published_at": "2026-06-01T10:00:00Z",
              "assets": [
                {"name":"AdaptNote-demo-pack-abcdef1.zip","size":10,
                 "browser_download_url":"https://example.test/demo.zip",
                 "digest":"sha256:1111"},
                {"name":"AdaptNote-windows-x64-gui.zip","size":123456,
                 "browser_download_url":"https://example.test/app.zip",
                 "digest":"sha256:abcdef0123456789"}
              ]
            })";
            const an::update::ReleaseInfo info = Updater::parseRelease(release);
            check(info.tag == QStringLiteral("gui-abcdef1") &&
                      info.commit == QStringLiteral("abcdef1") &&
                      info.assetName == QStringLiteral("AdaptNote-windows-x64-gui.zip") &&
                      info.sha256 == QStringLiteral("abcdef0123456789") && info.size == 123456,
                  QStringLiteral("updater picks the Windows zip and its sha256"));
            check(Updater::commitFromTag(QStringLiteral("ui-40403e3")) ==
                          QStringLiteral("40403e3") &&
                      Updater::commitFromTag(QStringLiteral("v1.2.3")).isEmpty(),
                  QStringLiteral("updater extracts the commit from a release tag"));
            check(!Updater::isNewer(info, QStringLiteral("abcdef1")) &&
                      !Updater::isNewer(info, QStringLiteral("abcdef")) &&
                      !Updater::isNewer(info, QStringLiteral("unknown")),
                  QStringLiteral("updater does not reinstall the same commit"));
            check(Updater::isNewer(info, QStringLiteral("1234567"),
                                   QStringLiteral("2025-01-01T00:00:00Z")) &&
                      !Updater::isNewer(info, QStringLiteral("1234567"),
                                        QStringLiteral("2027-01-01T00:00:00Z")),
                  QStringLiteral("updater respects release publish time (dev builds newer)"));
            const QString script = Updater::buildWindowsUpdateScript(
                4242, QStringLiteral("C:\\Apps\\AdaptNote"), QStringLiteral("C:\\Temp\\u.zip"));
            check(script.contains(QStringLiteral("Wait-Process -Id 4242")) &&
                      script.contains(QStringLiteral("/XD data")) &&
                      script.contains(QStringLiteral("C:\\Apps\\AdaptNote")) &&
                      script.contains(QStringLiteral("C:\\Temp\\u.zip")) &&
                      script.contains(QStringLiteral("bin\\adaptnote.exe")),
                  QStringLiteral("update script waits, keeps data, restarts"));
        }

        // ---- Bing web fallback (ttranslatev3 scrape) ----
        {
            const QByteArray page =
                "<html><script>IG:\"0DAC47E8CA0A443688D8FD1C01F46B18\","
                "params_AbusePreventionHelper = [1794312772922,"
                "\"xxqoCAv9khb7R7SHb1WOlP6qqPgayFaW\",3600000]</script></html>";
            QString ig;
            QString key;
            QString token;
            check(Translator::parseBingPage(page, &ig, &key, &token) &&
                      ig == QStringLiteral("0DAC47E8CA0A443688D8FD1C01F46B18") &&
                      key == QStringLiteral("1794312772922") &&
                      token == QStringLiteral("xxqoCAv9khb7R7SHb1WOlP6qqPgayFaW"),
                  QStringLiteral("Bing web page exposes IG/key/token"));
            check(!Translator::parseBingPage("<html>nope</html>", &ig, &key, &token),
                  QStringLiteral("Bing web parser rejects a changed page"));
        }

        // ---- built-in free DeepL (oneshot) ----
        check(Translator::freeTargetCode(QStringLiteral("简体中文")) == QStringLiteral("zh-Hans") &&
                  Translator::freeTargetCode(QStringLiteral("English")) == QStringLiteral("en-US") &&
                  Translator::freeTargetCode(QStringLiteral("日本語")) == QStringLiteral("ja"),
              QStringLiteral("free DeepL target codes are BCP-47-ish"));
        const QString freeBody = QString::fromUtf8(
            Translator::freeBody(QStringLiteral("hello"), QStringLiteral("简体中文")));
        check(freeBody.contains(QStringLiteral("\"text\":[\"hello\"]")) &&
                  freeBody.contains(QStringLiteral("\"target_lang\":\"zh-Hans\"")) &&
                  freeBody.contains(QStringLiteral("\"usage_type\":\"translate\"")) &&
                  freeBody.contains(QStringLiteral("\"os\":\"iOS\"")),
              QStringLiteral("free DeepL body mirrors the iOS oneshot profile"));
        check(Translator::parseFreeDeepL(
                  R"({"translations":[{"detected_source_language":"en","text":"你好"}]})") ==
                  QStringLiteral("你好"),
              QStringLiteral("free DeepL response parser"));
        const QString longText(4000, QLatin1Char('a'));
        const QVector<QString> chunks = Translator::splitFreeText(longText, 1400);
        int total = 0;
        bool bounded = true;
        QString joined;
        for (const QString& chunk : chunks) {
            total += chunk.size();
            joined += chunk;
            if (chunk.size() > 1400) bounded = false;
        }
        check(chunks.size() >= 3 && bounded && total == longText.size() && joined == longText,
              QStringLiteral("free DeepL chunks long text without losing characters"));
    }

    std::printf("\n%s (%d failure(s))\n", g_fail == 0 ? "AI CHECKS PASSED" : "AI CHECKS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
