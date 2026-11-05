#include "engine/Timeline.hpp"

#include <chrono>

namespace engine {

namespace {

// Real time in microseconds. steady_clock rather than system_clock so the
// timeline never jumps backwards if the machine's wall clock is adjusted.
int64_t RealTimeMicroseconds() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::microseconds>(now).count();
}

int64_t ValidTic(int64_t tic) { return tic > 0 ? tic : 1; }

} // namespace

Timeline::Timeline(Timeline *anchorTimeline, int64_t ticSize)
    : anchor(anchorTimeline),
      tic(ValidTic(ticSize)),
      accumulated(0),
      lastSample(0),
      paused(false),
      scale(1.0) {
    lastSample = ReadAnchor();
}

Timeline::Timeline(const std::atomic<int64_t> &source, int64_t ticSize)
    : counter(&source),
      tic(ValidTic(ticSize)),
      accumulated(0),
      lastSample(0),
      paused(false),
      scale(1.0) {
    lastSample = ReadAnchor();
}

int64_t Timeline::ReadAnchor() const {
    if (anchor) return anchor->Accumulated();
    if (counter) return counter->load();
    return RealTimeMicroseconds();
}

int64_t Timeline::Accumulated() const {
    std::lock_guard<std::mutex> lock(mutex);
    Accumulate();
    return accumulated;
}

int64_t Timeline::RootUnitsPerTic() const {
    int64_t myTic;
    {
        std::lock_guard<std::mutex> lock(mutex);
        myTic = tic;
    }
    return anchor ? myTic * anchor->RootUnitsPerTic() : myTic;
}

void Timeline::Accumulate() const {
    const int64_t now = ReadAnchor();
    const int64_t delta = now - lastSample;
    lastSample = now;

    // While paused, lastSample still advances but nothing is credited, so the
    // paused span is skipped rather than paid out on unpause. An anchor that
    // was re-origined backwards is not followed back either.
    if (paused || delta <= 0) {
        return;
    }

    // Keep the fraction instead of dropping it, or a 0.5x timeline read once
    // per anchor unit would round every step to zero and never move.
    const double owed = static_cast<double>(delta) * scale + carry;
    const int64_t whole = static_cast<int64_t>(owed);
    carry = owed - static_cast<double>(whole);
    accumulated += whole;
}

int64_t Timeline::GetTime() const {
    const int64_t elapsed = Accumulated();
    return elapsed / RootUnitsPerTic();
}

void Timeline::Pause() {
    std::lock_guard<std::mutex> lock(mutex);
    Accumulate();
    paused = true;
}

void Timeline::Unpause() {
    std::lock_guard<std::mutex> lock(mutex);
    Accumulate();
    paused = false;
}

bool Timeline::IsPaused() const {
    std::lock_guard<std::mutex> lock(mutex);
    return paused;
}

void Timeline::SetScale(double newScale) {
    std::lock_guard<std::mutex> lock(mutex);

    // Settle time at the old rate first, so the change applies from here on
    // rather than rewriting history.
    Accumulate();

    if (newScale < MIN_SCALE) {
        newScale = MIN_SCALE;
    } else if (newScale > MAX_SCALE) {
        newScale = MAX_SCALE;
    }
    scale = newScale;
}

double Timeline::GetScale() const {
    std::lock_guard<std::mutex> lock(mutex);
    return scale;
}

void Timeline::SetTic(int64_t newTic) {
    std::lock_guard<std::mutex> lock(mutex);
    tic = ValidTic(newTic);
}

int64_t Timeline::GetTic() const {
    std::lock_guard<std::mutex> lock(mutex);
    return tic;
}

double Timeline::SecondsPerTic() const {
    int64_t myTic;
    {
        std::lock_guard<std::mutex> lock(mutex);
        myTic = tic;
    }
    // A null anchor counts in microseconds of real time; otherwise one of our
    // tics is `tic` of the anchor's, so the anchor decides what that is worth.
    if (anchor) return (double)myTic * anchor->SecondsPerTic();
    if (counter) return 0.0;
    return (double)myTic / (double)MICROSECONDS_PER_SECOND;
}

double Timeline::GetSeconds() const {
    return (double)GetTime() * SecondsPerTic();
}

int64_t Timeline::RealTimeNow() {
    return RealTimeMicroseconds();
}

void Timeline::SetOrigin(int64_t anchorTime) {
    // A parent's clock is read in its tics, but everything here counts root units.
    const int64_t origin = anchor ? anchorTime * anchor->RootUnitsPerTic() : anchorTime;

    std::lock_guard<std::mutex> lock(mutex);
    // The next Accumulate() credits (now - origin) * scale, so GetTime()
    // reads as elapsed-since-origin from here on.
    accumulated = 0;
    carry = 0.0;
    lastSample = origin;
}

} // namespace engine
