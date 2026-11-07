#pragma once

#include <QImage>
#include <QString>

namespace an {
namespace ocr {

struct Result {
    bool ok = false;
    QString text;
    QString engine;   // "tesseract" | "vision"
    QString error;
};

// Tesseract is looked up in ADAPT_TESSERACT, PATH and the default Windows install
// directory. An empty result means "not installed", which is a normal state: the
// caller can fall back to an OpenAI-compatible vision model.
QString tesseractExecutable();
bool available();

// Runs `tesseract <png> stdout -l <lang> --psm 6`. Blocking, bounded by timeoutMs.
Result recognize(const QImage& image, const QString& lang = QStringLiteral("eng+chi_sim"),
                 int timeoutMs = 60000);

// Shared with the vision fallback: PNG re-encoded as a data URL body fragment.
QByteArray pngBytes(const QImage& image);

}  // namespace ocr
}  // namespace an
