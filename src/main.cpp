#include "core/paths.h"
#include "core/store.h"
#include "mind/mindmap.h"
#include "ui/libraryview.h"
#include "ui/mainwindow.h"
#include "ui/pageview.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QStandardPaths>
#include <QTimer>

int main(int argc, char** argv)
{
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("AdaptNote"));
    QCoreApplication::setApplicationName(QStringLiteral("AdaptNote"));
    app.setStyle(QStringLiteral("Fusion"));

    QFile qss(QStringLiteral(":/app.qss"));
    if (qss.open(QIODevice::ReadOnly)) app.setStyleSheet(QString::fromUtf8(qss.readAll()));

    QCommandLineParser parser;
    parser.setApplicationDescription(
        QStringLiteral("AdaptNote — 本地优先的 PDF/EPUB 阅读与标注工具"));
    parser.addHelpOption();
    const QCommandLineOption optDoc({"d", "doc"}, "open a document", "file");
    const QCommandLineOption optShot("screenshot", "save a window screenshot and exit", "png");
    const QCommandLineOption optDemo("demo", "seed demo markups (for screenshots)");
    const QCommandLineOption optPage("page", "jump to page (1-based)", "n");
    const QCommandLineOption optWidth("width", "window width", "px", "1360");
    const QCommandLineOption optHeight("height", "window height", "px", "900");
    const QCommandLineOption optMode("mode", "layout: single|continuous|double", "mode", "continuous");
    const QCommandLineOption optZoom("zoom", "zoom: fitwidth|fitpage|actual|<percent>", "zoom", "fitwidth");
    const QCommandLineOption optRotate("rotate", "rotation: 0|90|180|270", "deg", "0");
    const QCommandLineOption optFind("find", "run an in-document search before screenshotting", "text");
    const QCommandLineOption optExportDir("export-dir", "export all formats into DIR", "dir");
    const QCommandLineOption optMindMap("mindmap", "render the note mind map to a PNG and exit", "png");
    const QCommandLineOption optLibrary("library", "screenshot the library window to PNG and exit",
                                        "png");
    const QCommandLineOption optFastImport(
        "fast-import", "relax fsync while opening/syncing (speed over durability)");
    parser.addOption(optDoc);
    parser.addOption(optShot);
    parser.addOption(optDemo);
    parser.addOption(optPage);
    parser.addOption(optWidth);
    parser.addOption(optHeight);
    parser.addOption(optMode);
    parser.addOption(optZoom);
    parser.addOption(optRotate);
    parser.addOption(optFind);
    parser.addOption(optExportDir);
    parser.addOption(optMindMap);
    parser.addOption(optLibrary);
    parser.addOption(optFastImport);
    parser.process(app);

    an::MainWindow w;
    w.resize(parser.value(optWidth).toInt(), parser.value(optHeight).toInt());
    w.show();

    if (parser.isSet(optFastImport)) {
        w.setFastImport(true);
        qInfo("fast-import: fsync relaxed for this session");
    }

    if (parser.isSet(optLibrary)) {
        const QString out = parser.value(optLibrary);
        an::Store store;   // same data dir as the app
        const QString dataDir = an::defaultDataDir(QCoreApplication::applicationDirPath());
        QString err;
        if (!store.open(dataDir + "/library.db", &err)) {
            qWarning("library: cannot open store: %s", qPrintable(err));
            return 1;
        }
        an::LibraryView lib(store);
        lib.resize(1000, 640);
        lib.show();
        QCoreApplication::processEvents();
        const bool ok = lib.grab().save(out);
        qInfo("library screenshot %s: %s", ok ? "saved" : "FAILED", qPrintable(out));
        return ok ? 0 : 1;
    }

    if (parser.isSet(optDoc)) {
        if (!w.openDocument(parser.value(optDoc))) return 2;
        if (parser.isSet(optPage)) w.view()->gotoPage(parser.value(optPage).toInt() - 1);

        const QString mode = parser.value(optMode).toLower();
        w.view()->setLayoutMode(mode == "single" ? an::PageLayoutMode::Single
                                 : mode == "double" ? an::PageLayoutMode::Double
                                                    : an::PageLayoutMode::Continuous);
        const QString zoom = parser.value(optZoom).toLower();
        if (zoom == "fitwidth")
            w.view()->setZoomMode(an::ZoomMode::FitWidth);
        else if (zoom == "fitpage")
            w.view()->setZoomMode(an::ZoomMode::FitPage);
        else if (zoom == "actual")
            w.view()->setZoomMode(an::ZoomMode::Actual);
        else
            w.view()->setZoom(zoom.toFloat() / 100.f);
        w.view()->setRotation(parser.value(optRotate).toInt());

        if (parser.isSet(optDemo)) w.applyDemoMarkups();
        if (parser.isSet(optFind)) w.findText(parser.value(optFind));
        if (parser.isSet(optMindMap)) {
            const QString out = parser.value(optMindMap);
            an::mind::MindMapView view;
            // pass the groups too: the CLI image must match what the in-app export shows
            view.setTree(an::mind::buildFromNotes(QFileInfo(parser.value(optDoc)).completeBaseName(),
                                                 w.visibleMarkups(), w.mindGroups()));
            const QImage img = view.renderToImage(QSize(1200, 760));
            qInfo("mindmap %s: %s", img.isNull() ? "FAILED" : "saved", qPrintable(out));
            if (!img.isNull()) img.save(out);
            return 0;
        }
        if (parser.isSet(optExportDir)) {
            const QDir dir(parser.value(optExportDir));
            QDir().mkpath(dir.absolutePath());
            const QString base = dir.absolutePath() + "/notes";
            for (const QString& ext : {QStringLiteral("md"), QStringLiteral("html"),
                                       QStringLiteral("opml"), QStringLiteral("xmind"),
                                       QStringLiteral("apkg")}) {
                QString err;
                const bool ok = w.exportMarkups(ext, base + "." + ext, &err);
                qInfo("export %s: %s", qPrintable(ext), ok ? "ok" : qPrintable(err));
            }
            if (!parser.isSet(optShot)) QTimer::singleShot(0, &app, &QCoreApplication::quit);
        }
    }

    if (parser.isSet(optShot)) {
        const QString out = parser.value(optShot);
        QTimer::singleShot(700, &w, [&w, out]() {
            const bool ok = w.grab().save(out);
            qInfo("screenshot %s: %s", ok ? "saved" : "FAILED", qPrintable(out));
            QCoreApplication::quit();
        });
    }

    return app.exec();
}
