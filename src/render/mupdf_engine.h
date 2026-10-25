#pragma once

#include "render/engine.h"

#include <QHash>
#include <QString>

struct fz_context;
struct fz_document;

namespace an {

class MuPdfEngine : public IDocumentEngine {
public:
    MuPdfEngine();
    ~MuPdfEngine() override;

    bool open(const QString& path, QString* error) override;
    void close() override;
    bool isOpen() const override { return doc_ != nullptr; }
    QString path() const override { return path_; }

    int pageCount() const override { return pageCount_; }
    QSizeF pageSize(int page) const override;
    bool isReflowable() const override { return reflowable_; }
    void setLayout(float widthPt, float heightPt, float emPt) override;

    QImage renderPage(int page, float scale, int rotation) override;
    const TextPage* textPage(int page) override;
    QVector<OutlineItem> outline() override;

    bool supportsAnchors() const override { return reflowable_; }
    int chapterOfPage(int page) override;
    int chapterOffsetOf(int page, int charIndex) override;
    int pageForChapterOffset(int chapter, int offset, int* intraChar = nullptr) override;

    bool supportsAnnotations() const override;
    bool writeAnnotations(const QVector<Markup>& markups, QString* error) override;
    int annotationCount(int page) override;
    QVector<Markup> readAnnotations(int page) override;

private:
    void clearTextCache();

    ::fz_context* ctx_ = nullptr;
    ::fz_document* doc_ = nullptr;
    QString path_;
    int pageCount_ = 0;
    bool reflowable_ = false;
    float layoutW_ = 450.f;
    float layoutH_ = 650.f;
    float layoutEm_ = 12.f;
    QHash<int, TextPage*> textCache_;
};

}  // namespace an
