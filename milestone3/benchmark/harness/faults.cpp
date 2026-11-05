// rerun_faults: in-process delta and fault acceptance tests (plan section 7).
//
// Faults are injected at the application-message boundary: real engine
// snapshots, framed exactly as in a benchmark run, delivered to a consumer
// with chosen messages dropped, duplicated, reordered or damaged. The
// consumer is the same code a benchmark endpoint uses (DecodeFrame,
// AdmitFrame, ApplyData). Live-transport faults - delayed startup, slow
// receiver, saturated queue, disconnect, late join - are run by
// tools/campaign.py against real processes.
//
// Prints one JSON line per case and a summary; exits 0 only if every case passes.
#include "Endpoint.hpp"

#include <cstdio>
#include <functional>
#include <map>
#include <string>
#include <vector>

using namespace engine;
using namespace rb;

namespace {

using State = std::map<uint32_t, std::string>;

State Capture(const World &w) {
    State s;
    w.Each<NetIdentity>([&](const GameObject &o, const NetIdentity &) { s[o.Id()] = CanonicalObject(o); });
    return s;
}

std::string Diff(const State &a, const State &b) {
    for (const auto &e : a) {
        auto it = b.find(e.first);
        if (it == b.end()) return "object " + std::to_string(e.first) + " missing";
        if (it->second != e.second) return "object " + std::to_string(e.first) + " differs";
    }
    for (const auto &e : b) if (!a.count(e.first)) return "extra object " + std::to_string(e.first);
    return "";
}

// A deterministic server-side stream: block 0 is the bootstrap keyframe.
// Block 3 creates a marker, block 5 changes a static platform's colour once
// (then it never changes again), block 7 removes another static platform.
struct Produced {
    std::vector<std::string> messages;   // framed, by block
    std::vector<State> states;           // the server's state after each block
    std::vector<bool> keyframes;
    ObjectId changed = 0, removed = 0, created = 0;
};

Produced Produce(bool delta, int blocks, int keyframe, uint16_t stream = STREAM_WORLD,
                 std::vector<std::string> only = {}) {
    Options o;
    o.role = "server";
    o.players = 1;
    o.moving = 2;
    o.statics = 3;
    o.seed = 7;
    std::unique_ptr<Scenario> scenario = Scenario::Make(o);
    World w;
    scenario->BuildLevel(w, true);
    scenario->AddPlayers(w);
    SnapshotEncoder encoder(SnapshotOptions(Encoding::Binary, delta, keyframe, only));
    Produced p;
    p.changed = 2;   // first static platform
    p.removed = 3;   // second static platform
    p.created = FIXTURE_BASE + 99;
    OutStream out;
    out.id = stream;
    PhaseTimes times;
    for (int b = 0; b <= blocks; ++b) {
        if (b > 0) {
            for (long long t = 2LL * b - 1; t <= 2LL * b; ++t) scenario->Tick(w, t, {});
            if (b == 3) {
                GameObject &m = w.CreateWithId(p.created, "marker");
                m.Add<Transform>(5.0f, 5.0f, 5.0f, 5.0f);
                m.Add<NetIdentity>();
            }
            if (b == 5) w.Find(p.changed)->Get<Renderable>()->color.r = 250;
            if (b == 7) w.Destroy(p.removed);
            w.Flush();
        }
        Frame f;
        f.topic[0] = 'W';
        f.topic[1] = '_';
        f.source = 0xFFFF;
        f.run = 1;
        p.messages.push_back(EncodeData(encoder, w, static_cast<uint32_t>(b), {}, f, out, times, PHASE_MEASURE));
        p.keyframes.push_back(encoder.LastStats().keyframe);
        p.states.push_back(Capture(w));
    }
    return p;
}

struct Consumer {
    World w;
    SnapshotDecoder d;
    InStream s;
    PhaseTimes times;
    long long frameErrors = 0;
    bool guard = true;   // false: hand every received payload straight to the engine decoder

    bool Deliver(const std::string &message) {
        Frame f;
        if (DecodeFrame(message, f) != FrameError::None) {
            ++frameErrors;
            return false;
        }
        if (!guard) {
            const ApplyResult r = d.Apply(w, f.payload, ApplyOptions{});
            w.Flush();
            return r.ok;
        }
        return ApplyData(d, w, s, f, ApplyOptions{}, times);
    }
    const Counters &C() const { return s.c[PHASE_MEASURE]; }
};

struct Case {
    std::string name;
    bool pass = false;
    std::string detail;
};

std::vector<Case> results;

void Report(const std::string &name, bool pass, const std::string &detail) {
    results.push_back(Case{name, pass, detail});
    Json j;
    j.Str("case", name).Bool("pass", pass).Str("detail", detail);
    std::printf("CASE %s\n", j.Text().c_str());
}

} // namespace

int main() {
    ComponentRegistry::RegisterBuiltins();
    const int K = 10;   // keyframe every 10 publications: blocks 0, 10, 20
    const Produced delta = Produce(true, 25, K);
    const Produced full = Produce(false, 25, K);

    // Sanity: every message delivered once, in order, reproduces the server.
    {
        Consumer c;
        bool ok = true;
        for (const std::string &m : delta.messages) ok = c.Deliver(m) && ok;
        const std::string diff = Diff(delta.states.back(), Capture(c.w));
        Report("baseline_in_order", ok && diff.empty() && c.C().applied == delta.messages.size(),
               diff.empty() ? "all 26 messages applied; final state equal" : diff);
    }

    // Duplicate message: rejected, no double application.
    {
        Consumer c;
        for (int b = 0; b <= 25; ++b) {
            c.Deliver(delta.messages[static_cast<size_t>(b)]);
            if (b == 4) c.Deliver(delta.messages[4]);
        }
        const std::string diff = Diff(delta.states.back(), Capture(c.w));
        Report("duplicate_message", c.C().duplicates == 1 && diff.empty(),
               "duplicates=" + std::to_string(c.C().duplicates) + (diff.empty() ? ", final state equal" : ", " + diff));
    }

    // Stale message after a removal: rejected; the removed object is not resurrected.
    {
        Consumer c;
        for (int b = 0; b <= 9; ++b) c.Deliver(delta.messages[static_cast<size_t>(b)]);
        c.Deliver(delta.messages[6]);   // from before the removal at block 7
        const bool gone = c.w.Find(delta.removed) == nullptr;
        const std::string diff = Diff(delta.states[9], Capture(c.w));
        Report("stale_message_no_resurrection", c.C().stale == 1 && gone && diff.empty(),
               "stale=" + std::to_string(c.C().stale) + (gone ? ", removed object stays removed" : ", RESURRECTED") +
                   (diff.empty() ? "" : ", " + diff));
    }

    // Truncated and corrupted messages: rejected before decoding; world unchanged.
    {
        Consumer c;
        for (int b = 0; b <= 4; ++b) c.Deliver(delta.messages[static_cast<size_t>(b)]);
        const State before = Capture(c.w);
        std::string truncated = delta.messages[5].substr(0, delta.messages[5].size() / 2);
        std::string flipped = delta.messages[5];
        flipped[flipped.size() - 3] ^= 0x5A;
        const bool r1 = c.Deliver(truncated), r2 = c.Deliver(flipped);
        const bool unchanged = Diff(before, Capture(c.w)).empty();
        Report("malformed_rejected", !r1 && !r2 && c.frameErrors == 2 && unchanged,
               "frame errors=" + std::to_string(c.frameErrors) + (unchanged ? ", world unchanged" : ", WORLD CHANGED"));
        // Then the real message still applies (the stream continues).
        const bool r3 = c.Deliver(delta.messages[5]);
        Report("malformed_then_valid", r3 && Diff(delta.states[5], Capture(c.w)).empty(),
               r3 ? "the valid copy of block 5 applied after the damaged ones" : "the valid copy did not apply");
    }

    // Engine finding: the engine decoder alone, given a truncated snapshot,
    // reports failure but may already have applied part of it.
    {
        World w;
        SnapshotDecoder d;
        Frame f;
        DecodeFrame(full.messages[0], f);
        const std::string cut = f.payload.substr(0, f.payload.size() / 2);
        const ApplyResult r = d.Apply(w, cut, ApplyOptions{});
        w.Flush();
        const size_t partial = w.Count();
        Report("engine_decoder_truncated_payload", true,
               std::string("engine Apply ok=") + (r.ok ? "true" : "false") + ", objects left in the world=" +
                   std::to_string(partial) + " of " + std::to_string(full.states[0].size()) +
                   " (recorded as a finding; the harness checksum rejects such payloads before decoding)");
    }

    // Missing initial keyframe (late join mid-stream): deltas rejected until the next keyframe.
    {
        Consumer c;
        for (int b = 13; b <= 25; ++b) c.Deliver(delta.messages[static_cast<size_t>(b)]);
        const std::string diff = Diff(delta.states.back(), Capture(c.w));
        const bool rejectedUntilKey = c.C().unsynced == 7;   // blocks 13-19
        Report("missing_initial_keyframe", rejectedUntilKey && diff.empty() && c.s.synced,
               "unsynced deltas rejected=" + std::to_string(c.C().unsynced) + ", synced at the block-20 keyframe" +
                   (diff.empty() ? ", final state equal" : ", " + diff));
    }

    // Missing intermediate delta carrying a one-time change: the stream is
    // unsynchronised from the gap, dependent deltas are rejected, and the
    // next keyframe restores the change. The divergence is recorded.
    {
        Consumer c;
        bool equalAtKeyframe = false;
        for (int b = 0; b <= 25; ++b) {
            if (b == 5) continue;   // the delta with the colour change
            c.Deliver(delta.messages[static_cast<size_t>(b)]);
            if (b == 10) equalAtKeyframe = Diff(delta.states[10], Capture(c.w)).empty();
        }
        const auto &h = c.s.unsyncHistory;
        const bool history = h.size() == 1 && h[0].first == 5 && h[0].second == 10;
        const GameObject *changed = c.w.Find(delta.changed);
        const bool restored = changed && changed->Get<Renderable>()->color.r == 250;
        Report("missing_delta_one_time_change",
               c.C().gaps == 1 && c.C().unsynced == 4 && equalAtKeyframe && restored && history,
               "gaps=" + std::to_string(c.C().gaps) + ", dependent deltas rejected=" + std::to_string(c.C().unsynced) +
                   ", state equal at keyframe=" + (equalAtKeyframe ? "yes" : "no") + ", change restored=" +
                   (restored ? "yes" : "no") + ", divergence history=" + (history ? "[5,10]" : "wrong"));
    }

    // The same loss without the guard: the engine decoder applies the later
    // deltas to a base that lacks the change; the change is missing until the
    // keyframe. Shown for contrast, not as a pass/fail of the engine.
    {
        Consumer c;
        c.guard = false;
        bool missingBeforeKey = false;
        for (int b = 0; b <= 25; ++b) {
            if (b == 5) continue;
            c.Deliver(delta.messages[static_cast<size_t>(b)]);
            if (b == 9) missingBeforeKey = c.w.Find(delta.changed)->Get<Renderable>()->color.r != 250;
        }
        Report("unguarded_engine_decoder_contrast", true,
               std::string("without gap detection the one-time change is ") + (missingBeforeKey ? "absent" : "present") +
                   " at block 9 and the engine decoder reports no error until the block-10 keyframe repairs it");
    }

    // Missing removal: after the keyframe the object is gone; a stale earlier
    // message cannot bring it back.
    {
        Consumer c;
        for (int b = 0; b <= 12; ++b) {
            if (b == 7) continue;
            c.Deliver(delta.messages[static_cast<size_t>(b)]);
        }
        const bool goneAfterKey = c.w.Find(delta.removed) == nullptr;
        c.Deliver(delta.messages[6]);
        const bool stillGone = c.w.Find(delta.removed) == nullptr;
        Report("missing_removal", goneAfterKey && stillGone && Diff(delta.states[12], Capture(c.w)).empty(),
               std::string("removed after keyframe=") + (goneAfterKey ? "yes" : "no") + ", stale message resurrects=" +
                   (stillGone ? "no" : "YES"));
    }

    // Full-snapshot stream: a gap is a skipped complete state, not a broken dependency.
    {
        Consumer c;
        for (int b = 0; b <= 25; ++b) {
            if (b == 5 || b == 6) continue;
            c.Deliver(full.messages[static_cast<size_t>(b)]);
        }
        const std::string diff = Diff(full.states.back(), Capture(c.w));
        Report("full_snapshot_gap_not_unsynced",
               c.C().gaps == 1 && c.C().missing == 2 && c.C().unsynced == 0 && c.s.unsyncHistory.empty() && diff.empty(),
               "gaps=" + std::to_string(c.C().gaps) + " missing=" + std::to_string(c.C().missing) +
                   " unsynced=" + std::to_string(c.C().unsynced) + (diff.empty() ? ", final state equal" : ", " + diff));
    }

    // Lost final update and failed send: both leave a detectable shortfall.
    {
        Consumer c;
        for (int b = 0; b < 25; ++b) c.Deliver(delta.messages[static_cast<size_t>(b)]);   // block 25 lost
        const bool incomplete = c.s.appliedBlock == 24 && c.C().applied == 25;
        Report("lost_final_update_detected", incomplete,
               "consumer stopped at block " + std::to_string(c.s.appliedBlock) + " of 25; the run validator requires every "
               "consumer to reach the final block and to have applied every produced message");
        Consumer d;
        for (int b = 0; b <= 25; ++b) {
            if (b == 15) continue;   // the producer's send failed
            d.Deliver(delta.messages[static_cast<size_t>(b)]);
        }
        Report("failed_send_detected", d.C().gaps == 1 && d.C().missing == 1 && d.C().unsynced == 4,
               "gap detected at the next message: missing=" + std::to_string(d.C().missing) +
                   ", dependent deltas rejected until the block-20 keyframe=" + std::to_string(d.C().unsynced));
    }

    // Interleaved streams (P2P input mode): input deltas and full-state
    // corrections travel as separate streams with their own sequences. A
    // lost input delta unsynchronises only the input stream; a correction
    // keyframe on the other stream does not resynchronise it.
    {
        const Produced inputs = Produce(true, 25, K, StreamPeer(0), {InputState::TYPE});
        const Produced corrections = Produce(false, 25, K, StreamCorrection(0));
        Consumer in, corr;
        bool inputStillUnsynced = false;
        for (int b = 0; b <= 25; ++b) {
            if (b != 12) in.Deliver(inputs.messages[static_cast<size_t>(b)]);
            if (b % 5 == 0) corr.Deliver(corrections.messages[static_cast<size_t>(b)]);
            if (b == 15) inputStillUnsynced = !in.s.synced;
        }
        Report("interleaved_corrections_and_inputs",
               inputStillUnsynced && in.s.synced && in.C().unsynced == 7 && corr.C().unsynced == 0 && corr.C().gaps == 5,
               "input stream unsynced from block 12 to its own keyframe at 20 (rejected=" + std::to_string(in.C().unsynced) +
                   "); correction stream skipped by design (gaps=" + std::to_string(corr.C().gaps) + ", unsynced=" +
                   std::to_string(corr.C().unsynced) + ")");
    }

    int passed = 0;
    for (const Case &c : results) passed += c.pass ? 1 : 0;
    Json summary;
    summary.Int("cases", static_cast<long long>(results.size())).Int("passed", passed);
    std::printf("SUMMARY %s\n", summary.Text().c_str());
    return passed == static_cast<int>(results.size()) ? 0 : 1;
}
