#pragma once

// Pieces every network role shares: the link to the run coordinator, phase
// timers, stream counters, verification against the oracle, and the single
// RESULT line each process prints.

#include "Canonical.hpp"
#include "Common.hpp"
#include "Frames.hpp"
#include "Scenario.hpp"
#include "engine/Network.hpp"
#include "engine/Snapshot.hpp"

#include <map>
#include <memory>
#include <set>
#include <string>

namespace rb {

// The endpoint's side of the out-of-band coordinator: a REQ socket for
// reports and a SUB socket for phase releases. It carries no gameplay state
// and is the same for every strategy.
class CoordLink {
public:
    CoordLink(const Options &o, std::string name);

    bool WaitBeacon(int timeoutMs);
    bool Report(const std::string &verb, const std::string &fields = {});
    // READY with a clock probe: the coordinator's reply time must fall
    // between this process's send and receive times.
    bool Ready(const std::string &fields);
    // 1 released, 0 not yet, -1 aborted. Releases seen early are remembered.
    int Poll(const std::string &phase, int timeoutMs, int64_t *coordNs = nullptr);
    bool Wait(const std::string &phase, int timeoutMs, int64_t *coordNs = nullptr);

    bool probeOk = false;
    int64_t probeRttNs = 0;
    int64_t reportNs = 0;          // time spent in coordinator round trips
    long long reports = 0;
    std::string abortReason;

private:
    std::string name;
    engine::net::Socket req{engine::net::SocketType::Request};
    engine::net::Socket sub{engine::net::SocketType::Subscribe};
    std::map<std::string, int64_t> released;
    bool aborted = false;
    int subTimeout = -2;
};

struct PhaseTimes {
    int64_t wait = 0;      // blocked on flow control or on data
    int64_t sim = 0;       // simulation ticks (and fixture/density updates)
    int64_t encode = 0;    // SnapshotEncoder::Encode
    int64_t decode = 0;    // SnapshotDecoder::Apply + World::Flush
    int64_t frame = 0;     // framing, checksums
    int64_t send = 0;      // socket sends; for REQ, the round trip to the reply
    int64_t verify = 0;    // checkpoint verification (validation runs only)
};
std::string PhaseTimesJson(const PhaseTimes &t);

struct ControlCounters {
    Counters out[3];
    Counters in[3];
};

class Endpoint {
public:
    Endpoint(const Options &o, std::string name);
    virtual ~Endpoint() = default;

    // The common lifecycle: setup handshake, READY, warm-up, MEASURE, DONE,
    // FINISH, verification, RESULT. Returns the process exit code.
    int Run();

protected:
    // Role hooks.
    virtual bool Setup(std::string &error) = 0;                    // sockets, world, subscription proof
    virtual void SetupIdle() {}                                     // while waiting for a release in setup
    // Produce and consume everything for blocks (from, to]; false on failure.
    virtual bool RunPhase(long long from, long long to, uint8_t phase, std::string &error) = 0;
    // Keep servicing the protocol after DONE, until FINISH (or the deadline).
    virtual void Service(int timeoutMs) { (void)timeoutMs; }
    virtual void Shutdown() {}
    virtual void VerifyAt(long long block) = 0;                     // all objects this endpoint holds at `block`
    virtual std::string RoleJson() const { return "{}"; }

    // Helpers for the roles.
    void Fail(const std::string &why) { if (error.empty()) error = why; }
    bool Verifying() const { return oracleLoaded; }
    bool CheckpointDue(long long block) const {
        return oracleLoaded && opts.checkpointEvery > 0 && block % opts.checkpointEvery == 0;
    }
    uint32_t RunTag() const { return runTag; }
    int64_t &T(int64_t PhaseTimes::*field) { return times[current].*field; }
    void Note(const std::string &what);
    ControlCounters &Control(const std::string &kind) { return control[kind]; }
    void CountOut(Counters &c, const std::string &message, size_t payload) {
        ++c.frames;
        c.frameBytes += message.size();
        c.payloadBytes += payload;
    }

    const Options &opts;
    std::string name;
    std::unique_ptr<Scenario> scenario;
    engine::World world;
    CoordLink coord;
    OracleFile oracle;
    bool oracleLoaded = false;
    VerifyResult verify;           // all checkpoints
    VerifyResult verifyFinal;      // the final state only
    std::map<uint16_t, InStream> in;
    std::map<uint16_t, OutStream> out;
    std::map<std::string, ControlCounters> control;
    PhaseTimes times[3];
    uint8_t current = PHASE_SETUP;
    uint32_t runTag = 0;
    std::string error;
    std::vector<std::string> notes;
    long long ticksSimulated[3] = {0, 0, 0};
    long long blocksProduced[3] = {0, 0, 0};

    // Measured-phase timestamps (steady clock, ns).
    int64_t tStart = 0, tReady = 0, tWarmRelease = 0, tWarmed = 0, tMeasureRelease = 0, tMeasureCoord = 0;
    int64_t tProducedFinal = 0, tDone = 0, tFinish = 0;
    Usage usageStart, usageDone;
};

// The InStream for `id`, created on first use.
InStream &Stream(std::map<uint16_t, InStream> &streams, uint16_t id);
OutStream &Stream(std::map<uint16_t, OutStream> &streams, uint16_t id);

// Encode with timing and counters, then frame. Returns the 0MQ message.
std::string EncodeData(engine::SnapshotEncoder &encoder, const engine::World &world, uint32_t block,
                       const engine::ObjectFilter &include, Frame &frame, OutStream &out, PhaseTimes &times,
                       uint8_t phase);

// Admit, decode and account for one data frame. Returns true if applied.
bool ApplyData(engine::SnapshotDecoder &decoder, engine::World &world, InStream &stream, const Frame &frame,
               const engine::ApplyOptions &apply, PhaseTimes &times);

int RunCoordinator(const Options &o);
int RunServer(const Options &o);
int RunClient(const Options &o);
int RunPeer(const Options &o);
int RunOracle(const Options &o);

} // namespace rb
