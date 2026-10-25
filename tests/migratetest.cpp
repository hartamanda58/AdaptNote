// PDF revision migration: a note must follow its text when the file is re-paginated.
#include "core/store.h"
#include "core/textselect.h"
#include "migrate/migrate.h"
#include "render/mupdf_engine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QMap>
#include <QProcess>
#include <QTemporaryDir>

#include <cstdio>

using namespace an;

namespace {

int g_fail = 0;

void check(bool ok, const QString& what)
{
    std::printf("%s  %s\n", ok ? "PASS" : "FAIL", qPrintable(what));
    if (!ok) ++g_fail;
}

Selection lineSelection(MuPdfEngine& eng, int page, int line)
{
    const TextPage* tp = eng.textPage(page);
    if (!tp) return {};
    int first = -1, last = -1;
    for (int i = 0; i < tp->chars.size(); ++i) {
        if (tp->chars.at(i).line != line) continue;
        if (first < 0) first = i;
        last = i;
    }
    if (first < 0) return {};
    return selectRange(*tp, page, tp->chars.at(first).origin, tp->chars.at(last).origin);
}


// Picks up to `count` lines whose text is mostly ASCII. The CI runner may lack the
// CJK font used by the fixture generator, in which case Qt substitutes a font and the
// two PDFs can map glyphs differently - that would test the font stack, not migration.
QVector<Selection> asciiLineSelections(MuPdfEngine& eng, int page, int count)
{
    QVector<Selection> out;
    const TextPage* tp = eng.textPage(page);
    if (!tp) return out;

    QMap<int, QString> byLine;
    for (const CharBox& c : tp->chars)
        byLine[c.line].append(c.code <= 0xFFFF ? QChar(static_cast<ushort>(c.code)) : QChar('?'));

    for (auto it = byLine.constBegin(); it != byLine.constEnd() && out.size() < count; ++it) {
        const QString text = it.value().trimmed();
        if (text.size() < 12) continue;
        int ascii = 0;
        for (const QChar ch : text)
            if (ch.unicode() < 0x80) ++ascii;
        if (static_cast<double>(ascii) / text.size() < 0.85) continue;

        const Selection sel = lineSelection(eng, page, it.key());
        if (sel.isValid()) out.append(sel);
    }
    return out;
}

}  // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QString pdf = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("test.pdf");

    QTemporaryDir tmp;
    check(tmp.isValid(), QStringLiteral("temp dir"));
    if (!tmp.isValid()) return 1;

    QString err;

    const QString mkpdf =
#ifdef Q_OS_WIN
        QFileInfo(QCoreApplication::applicationDirPath() + QStringLiteral("/mkpdf.exe"))
            .absoluteFilePath();
#else
        QFileInfo(QCoreApplication::applicationDirPath() + QStringLiteral("/mkpdf"))
            .absoluteFilePath();
#endif

    auto generate = [&](const QString& out, bool blankFirst) {
        QStringList args{out};
        args << QStringLiteral("--latin");
        if (blankFirst) args << QStringLiteral("--blank-first");
        QProcess p;
        p.start(mkpdf, args);
        p.waitForFinished(120000);
        return QFileInfo::exists(out);
    };

    const QString oldPdf = tmp.path() + "/old.pdf";
    const QString newPdf = tmp.path() + "/new.pdf";
    check(generate(oldPdf, false), QStringLiteral("generated the original pdf (%1)").arg(mkpdf));
    check(generate(newPdf, true), QStringLiteral("generated the revised pdf"));

    MuPdfEngine eng;
    check(eng.open(oldPdf, &err), QStringLiteral("open %1: %2").arg(oldPdf, err));
    if (!eng.isOpen()) return 1;

    // ---- notes on pages 1 and 3 of the original ----
    QVector<Markup> notes;
    for (const Selection& sel : asciiLineSelections(eng, 0, 2)) {
        Markup m;
        m.id = newId();
        m.page = 0;
        m.type = MarkupType::Highlight;
        m.text = sel.text;
        m.quads = sel.quads;
        notes.append(m);
    }
    for (const Selection& sel : asciiLineSelections(eng, 2, 1)) {
        Markup m;
        m.id = newId();
        m.page = 2;
        m.type = MarkupType::Highlight;
        m.text = sel.text;
        m.quads = sel.quads;
        notes.append(m);
    }
    check(notes.size() == 3, QStringLiteral("prepared %1 notes").arg(notes.size()));
    for (const Markup& m : notes)
        std::printf("      note p%d: \"%s\"\n", m.page + 1, qPrintable(m.text.left(48)));

    // ---- baseline: migrating against the unchanged file must keep everything ----
    {
        const MigrationReport same = migrateByText(eng, notes);
        check(same.moved.isEmpty() && same.lost.isEmpty(),
              QStringLiteral("unchanged document: %1").arg(same.summary()));
        check(same.inPlace.size() == notes.size(),
              QStringLiteral("all notes matched in place (%1)").arg(same.inPlace.size()));
    }

    // ---- the revision already exists (newPdf); it has one extra leading page ----
    MuPdfEngine revised;
    check(revised.open(newPdf, &err), QStringLiteral("open revision: %1").arg(err));
    if (!revised.isOpen()) {
        std::printf("\nMIGRATION CHECKS FAILED (revision unreadable)\n");
        return 1;
    }
    check(revised.pageCount() == eng.pageCount() + 1,
          QStringLiteral("revision has %1 pages (was %2)")
              .arg(revised.pageCount()).arg(eng.pageCount()));

    // ---- migration must move the notes to the page holding their text ----
    {
        const MigrationReport report = migrateByText(revised, notes);
        std::printf("      [diag] %s\n", qPrintable(report.summary()));
        if (!report.lost.isEmpty()) {
            for (const QString& id : report.lost) {
                for (const Markup& m : notes) {
                    if (m.id != id) continue;
                    std::printf("      [diag] lost note p%d text=\"%s\"\n", m.page + 1,
                                qPrintable(m.text.left(60)));
                }
            }
            const TextPage* p1 = revised.textPage(1);
            if (p1) {
                std::printf("      [diag] revised page2 text=\"%s\" (%d chars)\n",
                            qPrintable(textForRange(*p1, 0, qMin(p1->chars.size() - 1, 80))),
                            p1->chars.size());
            } else {
                std::printf("      [diag] revised page2 has no text page\n");
            }
        }
        check(report.lost.isEmpty(), QStringLiteral("no note was lost (%1)").arg(report.lost.size()));
        check(report.moved.size() == notes.size(),
              QStringLiteral("all %1 notes moved one page down").arg(report.moved.size()));

        for (const Relocation& r : report.moved) {
            check(r.newPage == r.oldPage + 1,
                  QStringLiteral("note page %1 -> %2").arg(r.oldPage + 1).arg(r.newPage + 1));
            check(r.score >= 0.9, QStringLiteral("match score %1").arg(r.score, 0, 'f', 2));
        }

        // the relocated quads must sit on the text in the new document
        const int movedPage = report.moved.isEmpty() ? -1 : report.moved.first().newPage;
        const TextPage* tp = movedPage >= 0 ? revised.textPage(movedPage) : nullptr;
        check(tp != nullptr, QStringLiteral("revision page 2 has text"));
        if (tp) {
            bool coversText = false;
            for (const Quad& q : report.moved.first().quads) {
                const QRectF box = q.boundingRect();
                for (const CharBox& c : tp->chars) {
                    if (box.intersects(c.quad.boundingRect())) {
                        coversText = true;
                        break;
                    }
                }
            }
            check(coversText, QStringLiteral("relocated quads cover characters on the new page"));
        }
    }

    // ---- a note whose text was removed must be reported as lost ----
    {
        Markup ghost;
        ghost.id = newId();
        ghost.page = 0;
        ghost.type = MarkupType::Highlight;
        ghost.text = QStringLiteral("这段文字在新版本里被删掉了，绝对不应该被匹配到任何页面上的内容");
        const MigrationReport report = migrateByText(revised, {ghost});
        check(report.lost.size() == 1 && report.moved.isEmpty(),
              QStringLiteral("missing text reported as lost (%1)").arg(report.summary()));
    }

    std::printf("\n%s (%d failure(s))\n",
                g_fail == 0 ? "MIGRATION CHECKS PASSED" : "MIGRATION CHECKS FAILED", g_fail);
    return g_fail == 0 ? 0 : 1;
}
