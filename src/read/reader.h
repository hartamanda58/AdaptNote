#pragma once

#include "core/model.h"
#include "render/engine.h"

#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

class QTimer;

namespace an {
namespace read {

struct Sentence {
    int page = -1;
    int startChar = 0;
    int endChar = 0;
    QString text;
    QVector<Quad> quads;   // page space, merged per line
};

// Splits a page's text into sentences using CJK + latin terminators.
QVector<Sentence> splitSentences(const TextPage& page, int pageIndex, int fromChar = 0,
                                 int toChar = -1);

// Drives read-aloud: sentence iterator + pacing. When a TTS engine is available the
// voice drives the pace, otherwise a timer does (reading mode without audio).
class Reader : public QObject {
    Q_OBJECT
public:
    using PageProvider = std::function<const TextPage*(int)>;

    explicit Reader(QObject* parent = nullptr);
    ~Reader() override;

    void setPageProvider(PageProvider provider, int pageCount);
    void setWordsPerMinute(int wpm);   // pacing for the silent mode
    bool ttsAvailable() const;

    void load(int page, int fromChar = 0, int lastPage = -1);  // build the sentence list
    void start();
    void pause();
    void stop();
    bool active() const { return active_; }
    bool paused() const { return active_ && !playing_; }

    void next();
    void previous();

    int sentenceCount() const { return sentences_.size(); }
    int currentIndex() const { return index_; }
    Sentence current() const;

signals:
    void sentenceChanged(const an::read::Sentence& sentence);
    void finished();

private:
    void speakCurrent();
    void scheduleNext();
    void advance();

    PageProvider provider_;
    int pageCount_ = 0;
    QVector<Sentence> sentences_;
    int index_ = -1;
    int wpm_ = 240;
    bool active_ = false;
    bool playing_ = false;
    QTimer* timer_ = nullptr;
    struct Tts;
    Tts* tts_ = nullptr;
};

}  // namespace read
}  // namespace an
