#include "Endpoint.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <sstream>
#include <thread>

namespace rb {

using namespace engine;

// --- coordinator link ----------------------------------------------------------------

CoordLink::CoordLink(const Options &o, std::string endpointName) : name(std::move(endpointName)) {
    req.SetReceiveTimeoutMs(15000);
    req.Connect(Connect(CoordinatorReqPort(o)));
    sub.SetReceiveQueue(10000);
    sub.Subscribe("R_");
    sub.Connect(Connect(CoordinatorPubPort(o)));
}

int CoordLink::Poll(const std::string &phase, int timeoutMs, int64_t *coordNs) {
    auto found = [&]() -> int {
        if (aborted) return -1;
        auto it = released.find(phase);
        if (it == released.end()) return 0;
        if (coordNs) *coordNs = it->second;
        return 1;
    };
    if (int r = found()) return r;
    const int64_t deadline = NowNs() + static_cast<int64_t>(timeoutMs) * 1000000;
    std::string m;
    while (true) {
        const int64_t left = (deadline - NowNs()) / 1000000;
        const int wait = left > 0 ? static_cast<int>(left) : 0;
        if (wait != subTimeout) {
            sub.SetReceiveTimeoutMs(wait);
            subTimeout = wait;
        }
        if (!sub.Receive(m)) return found();
        if (m.rfind("R_ABORT", 0) == 0) {
            aborted = true;
            abortReason = m.size() > 8 ? m.substr(8) : "aborted";
        } else if (m != "R_B") {
            std::istringstream in(m.substr(2));
            std::string p;
            long long ns = 0;
            in >> p >> ns;
            released.emplace(p, ns);
        }
        if (int r = found()) return r;
        if (wait == 0) return 0;
    }
}

bool CoordLink::Wait(const std::string &phase, int timeoutMs, int64_t *coordNs) {
    return Poll(phase, timeoutMs, coordNs) == 1;
}

bool CoordLink::WaitBeacon(int timeoutMs) {
    const int64_t deadline = NowNs() + static_cast<int64_t>(timeoutMs) * 1000000;
    sub.SetReceiveTimeoutMs(100);
    subTimeout = 100;
    std::string m;
    while (NowNs() < deadline) {
        if (!sub.Receive(m)) continue;
        if (m == "R_B") return true;
        if (m.rfind("R_ABORT", 0) == 0) {
            aborted = true;
            return false;
        }
    }
    return false;
}

bool CoordLink::Report(const std::string &verb, const std::string &fields) {
    const int64_t t0 = NowNs();
    std::string reply;
    const bool ok = req.Send(verb + " " + name + (fields.empty() ? "" : " " + fields)) && req.Receive(reply);
    reportNs += NowNs() - t0;
    ++reports;
    if (!ok) return false;
    if (reply.rfind("ABORT", 0) == 0) {
        aborted = true;
        abortReason = reply;
        return false;
    }
    return reply.rfind("OK", 0) == 0;
}

bool CoordLink::Ready(const std::string &fields) {
    const int64_t t1 = NowNs();
    std::string reply;
    const bool ok = req.Send("READY " + name + " " + fields) && req.Receive(reply);
    const int64_t t2 = NowNs();
    ++reports;
    reportNs += t2 - t1;
    if (!ok || reply.rfind("OK", 0) != 0) {
        if (ok) abortReason = reply;
        return false;
    }
    const long long coordinatorNs = std::atoll(reply.c_str() + 3);
    probeOk = t1 <= coordinatorNs && coordinatorNs <= t2;
    probeRttNs = t2 - t1;
    return true;
}

// --- shared helpers --------------------------------------------------------------------

std::string PhaseTimesJson(const PhaseTimes &t) {
    Json j;
    j.Int("wait_ns", t.wait)
        .Int("sim_ns", t.sim)
        .Int("encode_ns", t.encode)
        .Int("decode_ns", t.decode)
        .Int("frame_ns", t.frame)
        .Int("send_ns", t.send)
        .Int("verify_ns", t.verify);
    return j.Text();
}

InStream &Stream(std::map<uint16_t, InStream> &streams, uint16_t id) {
    InStream &s = streams[id];
    s.id = id;
    return s;
}

OutStream &Stream(std::map<uint16_t, OutStream> &streams, uint16_t id) {
    OutStream &s = streams[id];
    s.id = id;
    return s;
}

std::string EncodeData(SnapshotEncoder &encoder, const World &world, uint32_t block, const ObjectFilter &include,
                       Frame &f, OutStream &out, PhaseTimes &times, uint8_t phase) {
    const int64_t t0 = NowNs();
    f.payload = encoder.Encode(world, block, include);
    const int64_t t1 = NowNs();
    const SnapshotStats &s = encoder.LastStats();
    Counters &c = out.c[phase];
    c.encodeNs += static_cast<uint64_t>(t1 - t0);
    times.encode += t1 - t0;
    c.objects += s.objects;
    c.removals += s.removed;
    if (s.keyframe) ++c.keyframes;
    f.type = MSG_DATA;
    f.phase = phase;
    f.stream = out.id;
    f.seq = out.nextSeq++;
    f.block = block;
    std::string message = EncodeFrame(f);
    times.frame += NowNs() - t1;
    ++c.frames;
    c.payloadBytes += f.payload.size();
    c.frameBytes += message.size();
    return message;
}

bool ApplyData(SnapshotDecoder &decoder, World &world, InStream &s, const Frame &f, const ApplyOptions &apply,
               PhaseTimes &times) {
    Counters &c = s.c[f.phase < 3 ? f.phase : 2];
    ++c.frames;
    c.payloadBytes += f.payload.size();
    c.frameBytes += FRAME_FIXED + f.acks.size() * 6 + f.payload.size();
    const bool keyframe = IsKeyframe(f.payload);
    if (AdmitFrame(s, f, keyframe) != Admit::Apply) return false;
    const int64_t t0 = NowNs();
    const ApplyResult r = decoder.Apply(world, f.payload, apply);
    world.Flush();
    const int64_t dt = NowNs() - t0;
    c.decodeNs += static_cast<uint64_t>(dt);
    times.decode += dt;
    if (!r.ok) {
        ++c.malformed;
        return false;
    }
    if (r.skipped) {
        ++c.unsynced;
        return false;
    }
    c.objects += r.created + r.updated;
    c.removals += r.removed;
    MarkApplied(s, f, keyframe);
    return true;
}

// --- the endpoint lifecycle ------------------------------------------------------------

Endpoint::Endpoint(const Options &o, std::string endpointName)
    : opts(o), name(std::move(endpointName)), coord(o, name), runTag(Fnv32(o.run.data(), o.run.size())) {}

void Endpoint::Note(const std::string &what) {
    if (notes.size() < 32) notes.push_back(what);
}

int Endpoint::Run() {
    tStart = NowNs();
    if (opts.delayStartMs > 0) std::this_thread::sleep_for(std::chrono::milliseconds(opts.delayStartMs));
    scenario = Scenario::Make(opts);

    std::string setupError;
    if (!opts.oracle.empty()) {
        if (oracle.Load(opts.oracle, setupError)) oracleLoaded = true;
        else Fail(setupError);
    }
    // A process that misses the setup beacons (a late joiner) gives up.
    if (error.empty() && !coord.WaitBeacon(std::min(60, opts.timeoutS) * 1000)) Fail("no coordinator beacon (late join?)");
    if (error.empty() && !Setup(setupError)) Fail("setup: " + setupError);

    bool initialOk = true;
    uint64_t levelHash = 1469598103934665603ull;
    if (error.empty()) {
        // Native CS clients hold only what the server replicates, so compare that.
        world.Each<Transform>([&](const GameObject &o, const Transform &) {
            if (opts.Native() && !o.Has<NetIdentity>()) return;
            levelHash = Fnv64(CanonicalObject(o) + "\n", levelHash);
        });
        if (oracleLoaded) {
            const int64_t t0 = NowNs();
            VerifyResult initial;
            std::swap(initial, verify);
            VerifyAt(0);
            std::swap(initial, verify);
            initialOk = initial.Ok();
            verify.checks += initial.checks;
            verify.compared += initial.compared;
            verify.mismatched += initial.mismatched;
            verify.missing += initial.missing;
            verify.extra += initial.extra;
            verify.runtimeCompared += initial.runtimeCompared;
            verify.runtimeMismatched += initial.runtimeMismatched;
            verify.unavailable += initial.unavailable;
            for (const std::string &f : initial.first) if (verify.first.size() < 8) verify.first.push_back("initial: " + f);
            times[PHASE_SETUP].verify += NowNs() - t0;
        }
    }

    auto abortedOrFailed = [&]() { return !error.empty(); };
    if (error.empty()) {
        char fields[256];
        std::snprintf(fields, sizeof fields, "%s %s %s %s %d", Hex64(Fnv64(opts.Describe())).c_str(),
                      Hex64(SchemaHash()).c_str(), Hex64(scenario->TraceHash()).c_str(), Hex64(levelHash).c_str(),
                      initialOk ? 1 : 0);
        if (!coord.Ready(fields)) Fail("READY refused: " + coord.abortReason);
        tReady = NowNs();
    }

    // Warm-up: the same protocol as the measurement, untimed.
    if (!abortedOrFailed()) {
        const int64_t deadline = NowNs() + 120LL * 1000000000LL;
        int r = 0;
        while ((r = coord.Poll("WARMUP", 1)) == 0 && NowNs() < deadline) SetupIdle();
        if (r != 1) Fail("no WARMUP release" + (coord.abortReason.empty() ? std::string() : ": " + coord.abortReason));
    }
    if (!abortedOrFailed()) {
        tWarmRelease = NowNs();
        current = PHASE_WARMUP;
        std::string phaseError;
        if (!RunPhase(0, opts.warmupBlocks, PHASE_WARMUP, phaseError)) Fail("warm-up: " + phaseError);
    }
    if (!abortedOrFailed()) {
        const int64_t t0 = NowNs();
        if (oracleLoaded) VerifyAt(opts.warmupBlocks);
        times[PHASE_WARMUP].verify += NowNs() - t0;
        tWarmed = NowNs();
        if (!coord.Report("WARMED")) Fail("WARMED refused: " + coord.abortReason);
    }
    if (!abortedOrFailed()) {
        const int64_t deadline = NowNs() + 120LL * 1000000000LL;
        int r = 0;
        while ((r = coord.Poll("MEASURE", 2, &tMeasureCoord)) == 0 && NowNs() < deadline) Service(0);
        if (r != 1) Fail("no MEASURE release" + (coord.abortReason.empty() ? std::string() : ": " + coord.abortReason));
    }

    // The measurement.
    if (!abortedOrFailed()) {
        tMeasureRelease = NowNs();
        usageStart = ReadUsage();
        current = PHASE_MEASURE;
        std::string phaseError;
        if (!RunPhase(opts.warmupBlocks, opts.FinalBlock(), PHASE_MEASURE, phaseError)) Fail("measure: " + phaseError);
        tDone = NowNs();
        usageDone = ReadUsage();
        if (tProducedFinal == 0) tProducedFinal = tDone;
    }
    if (!abortedOrFailed() && !coord.Report("DONE")) Fail("DONE refused: " + coord.abortReason);
    if (!abortedOrFailed()) {
        const int64_t deadline = NowNs() + 120LL * 1000000000LL;
        int r = 0;
        while ((r = coord.Poll("FINISH", 1)) == 0 && NowNs() < deadline) Service(5);
        if (r != 1) Fail("no FINISH release" + (coord.abortReason.empty() ? std::string() : ": " + coord.abortReason));
        tFinish = NowNs();
    }
    if (!abortedOrFailed() && oracleLoaded) {
        VerifyResult before = verify;
        VerifyAt(opts.FinalBlock());
        verifyFinal.checks = verify.checks - before.checks;
        verifyFinal.compared = verify.compared - before.compared;
        verifyFinal.mismatched = verify.mismatched - before.mismatched;
        verifyFinal.missing = verify.missing - before.missing;
        verifyFinal.extra = verify.extra - before.extra;
        verifyFinal.runtimeCompared = verify.runtimeCompared - before.runtimeCompared;
        verifyFinal.runtimeMismatched = verify.runtimeMismatched - before.runtimeMismatched;
        verifyFinal.unavailable = verify.unavailable - before.unavailable;
        verifyFinal.maxFloatError = verify.maxFloatError;
    }
    if (!error.empty() && coord.abortReason.empty()) coord.Report("FAIL", JsonEscape(error));

    // RESULT.
    std::string inJson = "[", outJson = "[", controlJson = "{";
    for (const auto &s : in) inJson += (inJson.size() > 1 ? "," : "") + InStreamJson(s.second);
    for (const auto &s : out) outJson += (outJson.size() > 1 ? "," : "") + OutStreamJson(s.second);
    for (const auto &c : control) {
        Json k;
        k.Raw("out_warmup", CountersJson(c.second.out[PHASE_WARMUP]))
            .Raw("out_measure", CountersJson(c.second.out[PHASE_MEASURE]))
            .Raw("in_warmup", CountersJson(c.second.in[PHASE_WARMUP]))
            .Raw("in_measure", CountersJson(c.second.in[PHASE_MEASURE]))
            .Raw("out_setup", CountersJson(c.second.out[PHASE_SETUP]))
            .Raw("in_setup", CountersJson(c.second.in[PHASE_SETUP]));
        controlJson += (controlJson.size() > 1 ? ",\"" : "\"") + c.first + "\":" + k.Text();
    }
    inJson += "]";
    outJson += "]";
    controlJson += "}";
    std::string notesJson = "[";
    for (size_t i = 0; i < notes.size(); ++i) notesJson += (i ? ",\"" : "\"") + JsonEscape(notes[i]) + "\"";
    notesJson += "]";

    const int64_t wall = tDone > 0 ? tDone - tMeasureRelease : 0;
    Json measure;
    measure.Int("wall_ns", wall)
        .Int("busy_ns", wall - times[PHASE_MEASURE].wait)
        .Int("drain_ns", tDone > 0 ? tDone - tProducedFinal : 0)
        .Int("cpu_user_ns", usageDone.userNs - usageStart.userNs)
        .Int("cpu_sys_ns", usageDone.sysNs - usageStart.sysNs)
        .Int("max_rss_bytes", usageDone.maxRssBytes)
        .Int("ticks", ticksSimulated[PHASE_MEASURE])
        .Int("blocks_produced", blocksProduced[PHASE_MEASURE])
        .Raw("times", PhaseTimesJson(times[PHASE_MEASURE]));
    Json warm;
    warm.Int("wall_ns", tWarmed > 0 ? tWarmed - tWarmRelease : 0)
        .Int("ticks", ticksSimulated[PHASE_WARMUP])
        .Int("blocks_produced", blocksProduced[PHASE_WARMUP])
        .Raw("times", PhaseTimesJson(times[PHASE_WARMUP]));
    Json t;
    t.Int("start", tStart).Int("ready", tReady).Int("warm_release", tWarmRelease).Int("warmed", tWarmed)
        .Int("measure_release", tMeasureRelease).Int("measure_release_coord", tMeasureCoord)
        .Int("produced_final", tProducedFinal).Int("done", tDone).Int("finish", tFinish);
    Json probe;
    probe.Bool("ok", coord.probeOk).Int("rtt_ns", coord.probeRttNs).Int("coordinator_ns", coord.reportNs)
        .Int("coordinator_reports", coord.reports);

    Json result;
    result.Str("run", opts.run)
        .Str("campaign", opts.campaign)
        .Str("role", opts.role)
        .Str("name", name)
        .Int("id", opts.id)
        .Str("options", opts.Describe())
        .Str("options_hash", Hex64(Fnv64(opts.Describe())))
        .Str("schema_hash", Hex64(SchemaHash()))
        .Str("trace_hash", scenario ? Hex64(scenario->TraceHash()) : "")
        .Str("level_hash", Hex64(levelHash))
        .Str("status", error.empty() ? "ok" : "failed")
        .Str("error", error)
        .Raw("t", t.Text())
        .Raw("measure", measure.Text())
        .Raw("warmup", warm.Text())
        .Raw("setup_times", PhaseTimesJson(times[PHASE_SETUP]))
        .Raw("in", inJson)
        .Raw("out", outJson)
        .Raw("control", controlJson)
        .Raw("verify", verify.ToJson())
        .Raw("verify_final", verifyFinal.ToJson())
        .Bool("oracle", oracleLoaded)
        .Raw("probe", probe.Text())
        .Raw("counts", scenario ? scenario->Counts(world) : "{}")
        .Raw("role_info", RoleJson())
        .Raw("notes", notesJson);
    std::printf("RESULT %s\n", result.Text().c_str());
    std::fflush(stdout);

    if (error.empty()) coord.Report("BYE");
    Shutdown();
    if (!error.empty()) return 3;
    if (oracleLoaded && !verify.Ok()) return 4;
    return 0;
}

} // namespace rb
