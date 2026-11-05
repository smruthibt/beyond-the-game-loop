#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>

namespace engine {

// Section 1: an explicit representation of time.
//
// A Timeline reports elapsed time in "tics". It is anchored to another
// Timeline, to real time (a null anchor), or to a counter such as the
// engine's loop count. Anchoring composes: a game timeline anchored to real
// time can itself anchor a slow-motion timeline, and pausing the game
// timeline stops everything below it.
//
// Time is accumulated incrementally rather than computed as
// (now - start), because scale can change mid-run. Each sample advances the
// accumulator by (anchor delta * scale), so a scale change applies from the
// moment it is made and never retroactively rewrites time already elapsed.
//
// Every timeline in a chain accumulates in the root's units - microseconds of
// real time, or counts - and tic only divides that for GetTime(). A child
// reads its parent's accumulated time, never its tics, so changing a tic
// anywhere in a chain never moves time on any timeline.
class Timeline {
public:
    static constexpr double MIN_SCALE = 0.5;
    static constexpr double MAX_SCALE = 2.0;

    // Real time is measured in microseconds, so a `tic` of 1000 on a
    // real-time-anchored timeline means one tic is one millisecond.
    static constexpr int64_t MICROSECONDS_PER_SECOND = 1000000;

    // `anchor` == nullptr anchors this timeline to real time.
    // `tic` is how many anchor tics make up one tic of this timeline.
    explicit Timeline(Timeline *anchor = nullptr, int64_t tic = 1);

    // Anchored to a counter instead of a clock: one anchor tic per increment.
    // The counter must only ever increase and must outlive this timeline.
    explicit Timeline(const std::atomic<int64_t> &counter, int64_t tic = 1);

    // Elapsed tics, excluding time spent paused, with each scale change
    // applied from the point it was made.
    int64_t GetTime() const;

    // The same elapsed time in seconds, whatever this timeline's tic is and
    // however deep it sits in an anchor chain. This is what lets a system be
    // handed *any* timeline - the engine's game time, or a local one a game
    // made for a single object - without knowing its units. Zero for a
    // timeline anchored to a counter: counts are not seconds.
    double GetSeconds() const;

    // How many seconds one tic of this timeline represents. Walks the anchor
    // chain, since a child's tic is counted in its parent's tics.
    double SecondsPerTic() const;

    void Pause();
    void Unpause();
    bool IsPaused() const;

    // 1.0 runs at the anchor's rate. Clamped to [MIN_SCALE, MAX_SCALE].
    void SetScale(double newScale);
    double GetScale() const;

    void SetTic(int64_t newTic);
    int64_t GetTic() const;

    // The clock a null-anchored timeline reads, in microseconds. Exposed so
    // that processes can agree on a shared origin: one reads RealTimeNow(),
    // tells the others, and every one of them calls SetOrigin() with it.
    static int64_t RealTimeNow();

    // Re-zeroes this timeline so GetTime() measures from `anchorTime`, a value
    // on the anchor's own clock - RealTimeNow() for a real-time anchor, the
    // counter's value for a counter, the parent's GetTime() otherwise. Time
    // already accumulated is discarded and the span since `anchorTime` is
    // credited at the current scale.
    //
    // This is what lets separate processes hold clocks that read the same
    // number with no one in charge of the time: each anchors to real time and
    // each is given the same origin. Call it on an unpaused timeline.
    void SetOrigin(int64_t anchorTime);

private:
    // Where the anchor is now, in root units.
    int64_t ReadAnchor() const;

    // Settles and returns `accumulated`. This is what a child anchored here reads.
    int64_t Accumulated() const;

    // How many root units one tic of this timeline is.
    int64_t RootUnitsPerTic() const;

    // Advances `accumulated` to the present. Caller must hold `mutex`.
    void Accumulate() const;

    Timeline *anchor = nullptr;
    const std::atomic<int64_t> *counter = nullptr;
    int64_t tic;

    // Both in root units. `accumulated` is scaled time owed to this
    // timeline; `lastSample` is where the anchor was when we last looked.
    mutable int64_t accumulated;
    mutable int64_t lastSample;

    // The part of a root unit the last scaled step earned but could not credit.
    mutable double carry = 0.0;

    bool paused;
    double scale;

    mutable std::mutex mutex;
};

} // namespace engine
