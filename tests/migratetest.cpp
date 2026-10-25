// PDF revision migration: a note must follow its text when the file is re-paginated.
#include "core/store.h"
#include "core/textselect.h"
#include "migrate/migrate.h"
#include "render/mupdf_engine.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
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

}  // namespace

int main(int argc, char** argv)
{
    QGuiApplication app(argc, argv);
    const QString pdf = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("test.pdf");

    QTemporaryDir tmp;
    check(tmp.isValid(), QStringLiteral("temp dir"));
    if (!tmp.isValid()) return 1;

    QString err;
    MuPdfEngine eng;
    check(eng.open(pdf, &err), QStringLiteral("open %1: %2").arg(pdf, err));
    if (!eng.isOpen()) return 1;

    // ---- notes on pages 1 and 3 of the original (page 1 = index 0) ----
    QVector<Markup> notes;
    for (int line : {3, 6}) {
        const Selection sel = lineSelection(eng, 0, line);
        if (!sel.isValid()) continue;
        Markup m;
        m.id = newId();
        m.page = 0;
        m.type = MarkupType::Highlight;
        m.text = sel.text;
        m.quads = sel.quads;
        notes.append(m);
    }
    const Selection lastPage = lineSelection(eng, 2, 2);
    if (lastPage.isValid()) {
        Markup m;
        m.id = newId();
        m.page = 2;
        m.type = MarkupType::Highlight;
        m.text = lastPage.text;
        m.quads = lastPage.quads;
        notes.append(m);
    }
    check(notes.size() == 3, QStringLiteral("prepared %1 notes").arg(notes.size()));

    // ---- baseline: migrating against the unchanged file must keep everything ----
    {
        const MigrationReport same = migrateByText(eng, notes);
        check(same.moved.isEmpty() && same.lost.isEmpty(),
              QStringLiteral("unchanged document: %1").arg(same.summary()));
        check(same.inPlace.size() == notes.size(),
              QStringLiteral("all notes matched in place (%1)").arg(same.inPlace.size()));
    }

    // ---- build a re-paginated revision: same text, one extra leading page ----
    const QString revision = tmp.path() + "/revised.pdf";
    {
#ifdef Q_OS_WIN
        const QString mkpdfName = QStringLiteral("mkpdf.exe");
#else
        const QString mkpdfName = QStringLiteral("mkpdf");
#endif
        const QString mkpdf =
            QFileInfo(QCoreApplication::applicationDirPath() + QLatin1Char('/') + mkpdfName)
                .absoluteFilePath();
        QProcess gen;
        gen.start(mkpdf, {revision, QStringLiteral("--blank-first")});
        gen.waitForFinished(60000);
        check(gen.exitStatus() == QProcess::NormalExit && QFileInfo::exists(revision),
              QStringLiteral("generated the revised pdf (%1)").arg(mkpdf));
    }

    MuPdfEngine revised;
    check(revised.open(revision, &err), QStringLiteral("open revision: %1").arg(err));
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
