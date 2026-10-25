// Generates a multi-page test PDF used by the headless self-test and screenshots.
// Page size is A4 in points (72 dpi), so PDF coordinates == painter coordinates.
#include <QGuiApplication>
#include <QFont>
#include <QFontMetrics>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QRectF>
#include <QTextOption>

namespace {

const char* kCnLatin =
    "Reading is not about finishing the book, but about turning what you read into "
    "something of your own: highlights, excerpts, retelling and links are the real work.";

const char* kCn =
    "阅读的本质不是把书看完，而是把书里的东西变成自己的。"
    "所以标注、摘录、复述、连接，才是读书真正的工作量所在。";

const char* kEn =
    "The point of a reading tool is not to display pages, but to capture what you "
    "understood while you understood it. Highlights, notes and structure are the "
    "artifacts that survive after the book is closed.";

QString wrap(const QString& text, const QFont& font, double width)
{
    QFontMetricsF fm(font);
    QString out;
    QString line;
    for (const QChar& ch : text) {
        if (ch == QLatin1Char('\n')) {
            out += line + QLatin1Char('\n');
            line.clear();
            continue;
        }
        const QString candidate = line + ch;
        if (fm.horizontalAdvance(candidate) > width && !line.isEmpty()) {
            out += line + QLatin1Char('\n');
            line = ch;
        } else {
            line = candidate;
        }
    }
    out += line;
    return out;
}

void drawParagraph(QPainter& p, double& y, const QString& text, const QFont& font,
                   const QRectF& box, const QColor& color = QColor(0x1f, 0x23, 0x28))
{
    p.setFont(font);
    p.setPen(color);
    const QString wrapped = wrap(text, font, box.width());
    const QFontMetricsF fm(font);
    const QStringList lines = wrapped.split(QLatin1Char('\n'));
    for (const QString& line : lines) {
        const QRectF r(box.left(), y, box.width(), fm.height());
        p.drawText(r, Qt::AlignLeft | Qt::AlignTop | Qt::TextDontClip, line);
        y += fm.lineSpacing() * 1.15;
    }
    y += fm.height() * 0.35;
}

}  // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QStringList args = QCoreApplication::arguments();
    const bool blankFirst = args.contains(QStringLiteral("--blank-first"));
    // ASCII-only content: used by tests that must not depend on a CJK font being
    // installed (CI runners substitute fonts, which changes the text layer).
    const bool latinOnly = args.contains(QStringLiteral("--latin"));
    const QString out = args.size() > 1 ? args.at(1) : QStringLiteral("test.pdf");

    QPdfWriter writer(out);
    writer.setPageSize(QPageSize(QPageSize::A4));
    writer.setResolution(72);
    writer.setTitle(QStringLiteral("AdaptNote 测试文档"));
    writer.setCreator(QStringLiteral("AdaptNote mkpdf"));

    QPainter p(&writer);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);

    const QRectF page(0, 0, 595, 842);

    const QRectF body(page.left() + 62, page.top() + 90, page.width() - 124, page.height());

    const QString family = latinOnly ? QStringLiteral("DejaVu Sans")
                                     : QStringLiteral("Noto Sans CJK SC");
    QFont fTitle(family, 22, QFont::Bold);
    QFont fHead(family, 14, QFont::DemiBold);
    QFont fBody(family, 10.5);
    QFont fSmall(family, 9);

    if (blankFirst) {
        // a leading cover page: the pagination of everything below shifts by one,
        // which is exactly what the migration test needs
        p.setPen(QColor(0x99, 0x99, 0x99));
        p.setFont(fHead);
        p.drawText(QRectF(62, 56, 470, 30), Qt::AlignLeft,
                   latinOnly ? QStringLiteral("cover page added by the revised edition")
                             : QStringLiteral("修订版新增的封面页（仅有这一行文字）"));
        writer.newPage();
    }

    // ---------- page 1 ----------
    p.setPen(QColor(0x1f, 0x23, 0x28));
    p.setFont(fTitle);
    p.drawText(QRectF(62, 56, 470, 40), Qt::AlignLeft | Qt::AlignVCenter,
               latinOnly ? QStringLiteral("AdaptNote sample document")
                         : QStringLiteral("AdaptNote 测试文档"));
    p.setPen(QColor(0x8a, 0x90, 0x99));
    p.setFont(fSmall);
    p.drawText(QRectF(62, 96, 470, 20), Qt::AlignLeft,
               latinOnly ? QStringLiteral("fixture for rendering / selection / annotations")
                         : QStringLiteral("用于验证渲染 / 选择 / 标注 / 持久化"));

    double y = 150;
    drawParagraph(p, y,
                  latinOnly ? QString::fromUtf8(kCnLatin)
                            : QStringLiteral("第一段：%1").arg(QString::fromUtf8(kCn)),
                  fBody, body);
    drawParagraph(p, y, QString::fromUtf8(kEn), fBody, body);
    drawParagraph(p, y,
                  latinOnly ? QStringLiteral(
                                  "Third paragraph: coordinates must be decoupled from the view. "
                                  "Zoom, rotation and two-page mode only change the display "
                                  "matrix; annotations stay in PDF page space.")
                            : QStringLiteral("第三段：坐标必须与视图解耦。放大、旋转、双页只改变显示矩阵，"
                                             "标注数据始终保存在 PDF 页面坐标系里。这样重新打开文件时，"
                                             "缩放与旋转不会让高亮错位。"),
                  fBody, body);

    // ---------- page 2: two columns ----------
    writer.newPage();
    p.setFont(fHead);
    p.setPen(QColor(0x1f, 0x23, 0x28));
    p.drawText(QRectF(62, 56, 470, 30), Qt::AlignLeft,
               latinOnly ? QStringLiteral("Page two - two columns")
                         : QStringLiteral("第二页 · 双栏排版"));

    const double colGap = 24;
    const double colW = (page.width() - 124 - colGap) / 2;
    auto column = [&](double x, const QString& title, const QString& text, const QFont& f) {
        p.setFont(fHead);
        p.setPen(QColor(0x2a, 0x6f, 0xd0));
        p.drawText(QRectF(x, 110, colW, 24), Qt::AlignLeft, title);
        double cy = 146;
        drawParagraph(p, cy, text, f, QRectF(x, cy, colW, 600));
    };
    if (latinOnly) {
        column(62, QStringLiteral("Left column"),
               QStringLiteral("Two columns are the usual way to test selection quality: "
                              "word rectangles bleed across the gutter, character quads do not. "
                              "%1").arg(QString::fromUtf8(kCnLatin)), fBody);
        column(62 + colW + colGap, QStringLiteral("Right column"),
               QStringLiteral("The right column keeps the same text so matching can be "
                              "verified page by page. %1").arg(QString::fromUtf8(kEn)), fBody);
    } else {
        column(62, QStringLiteral("左栏 · 摘录"),
               QStringLiteral("双栏排版是检验文本选择质量的常用场景：如果按词矩形取选，"
                              "跨栏时会出现越界的高亮。按字符 quad 取选则可以稳定贴字。"
                              "%1").arg(QString::fromUtf8(kCn)), fBody);
        column(62 + colW + colGap, QStringLiteral("右栏 · Notes"),
               QStringLiteral("Column two mixes English and 中文 to make sure line breaking, "
                              "baseline snapping and character quads behave the same way in "
                              "both writing systems. %1").arg(QString::fromUtf8(kEn)), fBody);
    }

    // ---------- page 3: hierarchy ----------
    writer.newPage();
    p.setFont(fHead);
    p.setPen(QColor(0x1f, 0x23, 0x28));
    p.drawText(QRectF(62, 56, 470, 30), Qt::AlignLeft,
               latinOnly ? QStringLiteral("Page three - hierarchy")
                         : QStringLiteral("第三页 · 层级与列表"));

    double ly = 120;
    const QVector<QPair<QString, QFont>> levels = {
        {QStringLiteral("一、阅读层：PDF/EPUB 渲染、缩放、旋转、双页"), fHead},
        {QStringLiteral("    1.1 文本层：按字符 quad 提取，供选择与摘录"), fBody},
        {QStringLiteral("    1.2 标注层：高亮 / 下划线 / 波浪线 / 批注"), fBody},
        {QStringLiteral("二、笔记层：大纲、标签、全文搜索（后续阶段）"), fHead},
        {QStringLiteral("三、同步层：WebDAV + 版本向量合并（后续阶段）"), fHead},
    };
    for (const auto& row : levels) {
        p.setFont(row.second);
        p.setPen(QColor(0x1f, 0x23, 0x28));
        const QFontMetricsF fm(row.second);
        p.drawText(QRectF(62, ly, 470, fm.height()), Qt::AlignLeft, row.first);
        ly += fm.lineSpacing() * 1.6;
    }
    p.setFont(fSmall);
    p.setPen(QColor(0x8a, 0x90, 0x99));
    p.drawText(QRectF(62, 780, 470, 20), Qt::AlignLeft,
               QStringLiteral("AdaptNote · 用 MuPDF 渲染，Qt6 Widgets 交互，标注与视图解耦"));

    p.end();
    return 0;
}
