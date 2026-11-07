#include "ocr/ocr.h"

#include <QBuffer>
#include <QCoreApplication>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

namespace an {
namespace ocr {

QString tesseractExecutable()
{
    QStringList candidates;
    const QByteArray override = qgetenv("ADAPT_TESSERACT");
    if (!override.isEmpty()) candidates << QString::fromLocal8Bit(override);
#ifdef Q_OS_WIN
    // Portable bundle layout: <bundle>/bin/adaptnote.exe + <bundle>/ocr/tesseract.exe.
    // The bundled copy wins over the machine install so a packaged OCR build stays
    // self-contained.
    candidates << QCoreApplication::applicationDirPath() +
                      QStringLiteral("/../ocr/tesseract.exe");
#endif
    const QString onPath = QStandardPaths::findExecutable(QStringLiteral("tesseract"));
    if (!onPath.isEmpty()) candidates << onPath;
#ifdef Q_OS_WIN
    candidates << QStringLiteral("C:/Program Files/Tesseract-OCR/tesseract.exe")
               << QStringLiteral("C:/Program Files (x86)/Tesseract-OCR/tesseract.exe");
#endif
    for (const QString& candidate : candidates) {
        if (!candidate.isEmpty() && QFileInfo(candidate).isExecutable()) return candidate;
    }
    return {};
}

bool available()
{
    return !tesseractExecutable().isEmpty();
}

QByteArray pngBytes(const QImage& image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

Result recognize(const QImage& image, const QString& lang, int timeoutMs)
{
    Result out;
    out.engine = QStringLiteral("tesseract");
    if (image.isNull()) {
        out.error = QStringLiteral("没有可识别的图片");
        return out;
    }

    const QString exe = tesseractExecutable();
    if (exe.isEmpty()) {
        out.error = QStringLiteral(
            "本机没有找到 tesseract。安装后会自动启用；也可以在“更多 → OCR 设置”里"
            "改用云端视觉模型（ADAPT_TESSERACT 可指定 exe 路径）。");
        return out;
    }

    QTemporaryDir dir;
    if (!dir.isValid()) {
        out.error = QStringLiteral("无法创建临时目录");
        return out;
    }
    const QString png = dir.path() + QStringLiteral("/page.png");
    if (!image.save(png, "PNG")) {
        out.error = QStringLiteral("无法写出临时图片");
        return out;
    }

    QProcess proc;
    QStringList args{png, QStringLiteral("stdout")};
    // Bundled installs keep tessdata next to the exe; make that explicit so the
    // language packs are found even when TESSDATA_PREFIX is not set.
    const QString tessdata = QFileInfo(exe).absolutePath() + QStringLiteral("/tessdata");
    if (QFileInfo::exists(tessdata))
        args << QStringLiteral("--tessdata-dir") << tessdata;
    args << QStringLiteral("-l") << lang << QStringLiteral("--psm") << QStringLiteral("6");
    proc.start(exe, args);
    if (!proc.waitForStarted(5000)) {
        out.error = QStringLiteral("无法启动 tesseract：%1").arg(proc.errorString());
        return out;
    }
    if (!proc.waitForFinished(timeoutMs)) {
        proc.kill();
        proc.waitForFinished(1000);
        out.error = QStringLiteral("OCR 超时（%1 秒）").arg(timeoutMs / 1000);
        return out;
    }

    const QString text = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
    if (proc.exitCode() != 0) {
        const QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        out.error = err.isEmpty() ? QStringLiteral("tesseract 退出码 %1").arg(proc.exitCode())
                                  : err.left(400);
        return out;
    }
    if (text.isEmpty()) {
        out.error = QStringLiteral("没有识别到文字（可尝试框选更紧或换语言包）");
        return out;
    }
    out.ok = true;
    out.text = text;
    return out;
}

}  // namespace ocr
}  // namespace an
