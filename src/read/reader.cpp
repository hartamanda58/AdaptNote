#include "read/reader.h"

#include "core/textselect.h"

#include <QTimer>
#include <QtGlobal>

#ifdef ADAPT_HAVE_TTS
#include <QTextToSpeech>
#endif

namespace an {
namespace read {
namespace {

bool isTerminator(QChar c)
{
    static const QString kMarks = QStringLiteral("。！？!?;；…\n");
    return kMarks.contains(c);
}

}  // namespace

QVector<Sentence> splitSentences(const TextPage& page, int pageIndex, int fromChar, int toChar)
{
    QVector<Sentence> out;
    const int total = page.chars.size();
    if (total == 0) return out;

    fromChar = qBound(0, fromChar, total - 1);
    toChar = (toChar < 0 || toChar >= total) ? total - 1 : toChar;

    int start = fromChar;
    auto flush = [&](int end) {
        if (end < start) return;
        // trim leading/trailing whitespace in the character range
        int s = start, e = end;
        while (s <= e && QString(QChar(static_cast<ushort>(page.chars.at(s).code))).trimmed().isEmpty()) ++s;
        while (e >= s && QString(QChar(static_cast<ushort>(page.chars.at(e).code))).trimmed().isEmpty()) --e;
        if (e < s) {
            start = end + 1;
            return;
        }

        Sentence sent;
        sent.page = pageIndex;
        sent.startChar = s;
        sent.endChar = e;
        sent.text = textForRange(page, s, e);
        sent.quads = quadsForRange(page, s, e);
        // a sentence made only of punctuation is noise
        if (!sent.text.trimmed().isEmpty() && !sent.quads.isEmpty()) out.append(sent);
        start = end + 1;
    };

    for (int i = fromChar; i <= toChar; ++i) {
        const QChar c(static_cast<ushort>(page.chars.at(i).code));
        if (isTerminator(c)) {
            // keep trailing quotes/brackets with the sentence
            int end = i;
            while (end + 1 <= toChar) {
                const QChar n(static_cast<ushort>(page.chars.at(end + 1).code));
                if (n == QChar(0x201D) || n == QLatin1Char('"') || n == QChar(0xFF09) ||
                    n == QLatin1Char(')') || n == QChar(0x300F) || n == QChar(0x3011)) {
                    ++end;
                } else {
                    break;
                }
            }
            flush(end);
            i = end;
            continue;
        }
        // Guard against a runaway "sentence" on text without punctuation.
        if (i - start > 220) flush(i);
    }
    if (start <= toChar) flush(toChar);
    return out;
}

// ------------------------------------------------------------------ TTS adapter

struct Reader::Tts {
#ifdef ADAPT_HAVE_TTS
    QTextToSpeech* speech = nullptr;
#endif
};

Reader::Reader(QObject* parent)
    : QObject(parent)
    , timer_(new QTimer(this))
    , tts_(new Tts)
{
    timer_->setSingleShot(true);
    connect(timer_, &QTimer::timeout, this, &Reader::advance);

#ifdef ADAPT_HAVE_TTS
    if (!QTextToSpeech::availableEngines().isEmpty()) {
        tts_->speech = new QTextToSpeech(this);
        connect(tts_->speech, &QTextToSpeech::stateChanged, this, [this](QTextToSpeech::State s) {
            if (!active_ || !playing_) return;
            if (s == QTextToSpeech::State::Ready) scheduleNext();
        });
    }
#endif
}

Reader::~Reader()
{
    delete tts_;
}

bool Reader::ttsAvailable() const
{
#ifdef ADAPT_HAVE_TTS
    return tts_ && tts_->speech != nullptr;
#else
    return false;
#endif
}

void Reader::setPageProvider(PageProvider provider, int pageCount)
{
    provider_ = std::move(provider);
    pageCount_ = pageCount;
}

void Reader::setWordsPerMinute(int wpm)
{
    wpm_ = qBound(60, wpm, 900);
}

void Reader::load(int page, int fromChar, int lastPage)
{
    sentences_.clear();
    index_ = -1;
    if (!provider_) return;
    if (lastPage < 0 || lastPage >= pageCount_) lastPage = pageCount_ - 1;

    for (int p = qMax(0, page); p <= lastPage; ++p) {
        const TextPage* tp = provider_(p);
        if (!tp || tp->isEmpty()) continue;
        const QVector<Sentence> pageSentences =
            splitSentences(*tp, p, p == page ? qMax(0, fromChar) : 0);
        sentences_ += pageSentences;
    }
    if (!sentences_.isEmpty()) index_ = 0;
}

Sentence Reader::current() const
{
    if (index_ < 0 || index_ >= sentences_.size()) return {};
    return sentences_.at(index_);
}

void Reader::start()
{
    if (sentences_.isEmpty()) return;
    if (index_ < 0) index_ = 0;
    active_ = true;
    playing_ = true;
    emit sentenceChanged(current());
    speakCurrent();
}

void Reader::pause()
{
    if (!active_) return;
    playing_ = false;
    timer_->stop();
#ifdef ADAPT_HAVE_TTS
    if (tts_ && tts_->speech) tts_->speech->stop();
#endif
}

void Reader::stop()
{
    active_ = false;
    playing_ = false;
    timer_->stop();
#ifdef ADAPT_HAVE_TTS
    if (tts_ && tts_->speech) tts_->speech->stop();
#endif
}

void Reader::speakCurrent()
{
    const Sentence s = current();
    if (s.text.isEmpty()) {
        advance();
        return;
    }
#ifdef ADAPT_HAVE_TTS
    if (tts_ && tts_->speech) {
        tts_->speech->say(s.text);
        return;
    }
#endif
    // silent reading mode: pace by characters per minute
    const int chars = qMax(1, s.text.size());
    const double charsPerSecond = (wpm_ * 2.5) / 60.0;  // ~2.5 chars per "word" for CJK
    const int ms = qBound(400, static_cast<int>(chars / charsPerSecond * 1000.0), 20000);
    timer_->start(ms);
}

void Reader::scheduleNext()
{
    if (!active_ || !playing_) return;
    timer_->start(120);
}

void Reader::advance()
{
    if (!active_ || !playing_) return;
    if (index_ + 1 >= sentences_.size()) {
        active_ = false;
        playing_ = false;
        emit finished();
        return;
    }
    ++index_;
    emit sentenceChanged(current());
    speakCurrent();
}

void Reader::next()
{
    if (sentences_.isEmpty()) return;
    if (index_ + 1 >= sentences_.size()) return;
    ++index_;
    emit sentenceChanged(current());
    if (active_ && playing_) speakCurrent();
}

void Reader::previous()
{
    if (sentences_.isEmpty()) return;
    if (index_ > 0) --index_;
    emit sentenceChanged(current());
    if (active_ && playing_) speakCurrent();
}

}  // namespace read
}  // namespace an
