#pragma once

// Shared pieces of the rerun benchmark harness: run options, the common clock,
// a fully specified PRNG, hashing, JSON output and process resource usage.
//
// The harness uses the team engine unmodified. Everything here is benchmark
// protocol or bookkeeping layered on the engine's World, systems, snapshot
// encoder/decoder and 0MQ sockets.

#include "engine/Serialize.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace rb {

using engine::Encoding;

// Steady clock in nanoseconds: the common monotonic time domain of one host.
int64_t NowNs();

constexpr int TICKS_PER_BLOCK = 2;          // a publication every second tick
constexpr double TICK_SECONDS = 1.0 / 120.0;
constexpr float TICK_DT = 1.0f / 120.0f;

// splitmix64. Specified here rather than taken from <random>, so generated
// geometry and input traces do not depend on a standard library's
// distribution implementation.
struct Rng {
    uint64_t state;
    explicit Rng(uint64_t seed) : state(seed) {}
    uint64_t Next();
    float Uniform(float lo, float hi);   // [lo, hi), 24-bit resolution
    uint32_t Below(uint32_t n);
};

uint64_t Fnv64(const std::string &data, uint64_t hash = 1469598103934665603ull);
uint32_t Fnv32(const char *data, size_t size);
uint32_t Crc32c(const char *data, size_t size);   // frame checksum; hardware CRC32C where available
std::string Hex64(uint64_t v);

// Every setting a run varies. Each role parses the same command line, and
// Describe() is what all endpoints must agree on before warm-up.
struct Options {
    std::string role;                  // coordinator | server | client | peer | oracle
    std::string run = "adhoc";
    std::string campaign = "adhoc";
    std::string scenario = "engine";   // engine | game (Clockwork Dash)
    std::string topology = "cs";       // cs | p2p
    std::string world = "native";      // cs only: native (server replicates the level) | local
    std::string upstream = "input";    // input | state
    Encoding encoding = Encoding::Binary;
    bool downDelta = false;            // server -> client streams (world, relay forwards)
    bool upDelta = false;              // client -> server and peer -> peer streams
    int keyframe = 60;                 // delta streams: a keyframe every N publications
    int players = 2;
    int moving = 100;
    int statics = 100;
    int density = -1;                  // % of 100 eligible objects changed per publication; -1 = none
    int seed = 1;
    int warmupBlocks = 250;            // 500 ticks
    int measureBlocks = 50000;         // 100,000 ticks
    int window = 4;                    // flow-control window in blocks (8 ticks)
    int id = -1;
    int portBase = 20000;
    bool fixtures = false;             // validation-only scheduled object/component changes
    int checkpointEvery = 0;           // blocks; 0 = initial, end of warm-up and final only
    int hwm = 1000;                    // 0MQ queue limit (messages) on every data socket
    int timeoutS = 900;                // coordinator: longest allowed measured phase
    std::string oracle;                // canonical-state file to verify against
    std::string out;                   // oracle role: output file
    // Live fault fixtures.
    int delayStartMs = 0;
    int slowUs = 0;                    // consumer: extra delay per data message applied
    int killAtBlock = -1;              // exit abruptly after producing this block
    bool noFlowControl = false;        // fixture only: ignore the window

    int FinalBlock() const { return warmupBlocks + measureBlocks; }
    bool Cs() const { return topology == "cs"; }
    bool Native() const { return topology == "cs" && world == "native"; }
    bool InputUp() const { return upstream == "input"; }
    int Window() const { return noFlowControl ? (1 << 28) : window; }
    // Acknowledgement-only messages (and relay credit updates) are batched:
    // sent once unacknowledged progress reaches half the window, or when an
    // endpoint has nothing left to produce. With a window of at least 3
    // blocks this cannot deadlock (see METHODOLOGY.md).
    int AckBatch() const { return Window() / 2 > 1 ? Window() / 2 : 1; }
    std::string Describe() const;
};

bool ParseOptions(int argc, char **argv, Options &options, std::string &error);

// The block a tick belongs to, and the phase a block's messages are tagged with.
enum Phase : uint8_t { PHASE_SETUP = 0, PHASE_WARMUP = 1, PHASE_MEASURE = 2 };
inline uint8_t PhaseOf(const Options &o, int64_t block) {
    return block <= 0 ? PHASE_SETUP : block <= o.warmupBlocks ? PHASE_WARMUP : PHASE_MEASURE;
}

// Minimal JSON object builder for RESULT lines.
class Json {
public:
    Json &Str(const std::string &key, const std::string &value);
    Json &Int(const std::string &key, long long value);
    Json &Num(const std::string &key, double value);
    Json &Bool(const std::string &key, bool value);
    Json &Raw(const std::string &key, const std::string &json);
    std::string Text() const { return "{" + body + "}"; }

private:
    void Key(const std::string &key);
    std::string body;
};
std::string JsonEscape(const std::string &s);

struct Usage {
    int64_t userNs = 0;
    int64_t sysNs = 0;
    int64_t maxRssBytes = 0;
};
Usage ReadUsage();

// 0MQ endpoints for one run. Everything binds to the loopback interface.
std::string Bind(int port);
std::string Connect(int port);
inline int CoordinatorReqPort(const Options &o) { return o.portBase; }
inline int CoordinatorPubPort(const Options &o) { return o.portBase + 1; }
inline int ServerPubPort(const Options &o) { return o.portBase + 2; }
inline int ServerRepPort(const Options &o, int client) { return o.portBase + 10 + client; }
inline int PeerPubPort(const Options &o, int peer) { return o.portBase + 30 + peer; }

} // namespace rb
