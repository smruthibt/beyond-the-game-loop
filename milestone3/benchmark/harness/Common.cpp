#include "Common.hpp"

#include <sys/resource.h>
#if defined(__ARM_FEATURE_CRC32)
#include <arm_acle.h>
#endif

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace rb {

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

uint64_t Rng::Next() {
    uint64_t z = (state += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float Rng::Uniform(float lo, float hi) {
    const float unit = static_cast<float>(Next() >> 40) * (1.0f / 16777216.0f);
    return lo + (hi - lo) * unit;
}

uint32_t Rng::Below(uint32_t n) {
    return static_cast<uint32_t>((Next() >> 32) % n);
}

uint64_t Fnv64(const std::string &data, uint64_t hash) {
    for (unsigned char c : data) {
        hash ^= c;
        hash *= 1099511628211ull;
    }
    return hash;
}

uint32_t Fnv32(const char *data, size_t size) {
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < size; ++i) {
        hash ^= static_cast<unsigned char>(data[i]);
        hash *= 16777619u;
    }
    return hash;
}

uint32_t Crc32c(const char *data, size_t size) {
    uint32_t crc = 0xFFFFFFFFu;
#if defined(__ARM_FEATURE_CRC32)
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        uint64_t word;
        std::memcpy(&word, data + i, 8);
        crc = __crc32cd(crc, word);
    }
    for (; i < size; ++i) crc = __crc32cb(crc, static_cast<uint8_t>(data[i]));
#else
    for (size_t i = 0; i < size; ++i) {
        crc ^= static_cast<unsigned char>(data[i]);
        for (int k = 0; k < 8; ++k) crc = (crc >> 1) ^ (0x82F63B78u & (0u - (crc & 1u)));
    }
#endif
    return crc ^ 0xFFFFFFFFu;
}

std::string Hex64(uint64_t v) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(v));
    return buf;
}

std::string Options::Describe() const {
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "scenario=%s topology=%s world=%s upstream=%s encoding=%s down_delta=%d up_delta=%d keyframe=%d "
                  "players=%d moving=%d static=%d density=%d seed=%d warmup_blocks=%d measure_blocks=%d window=%d "
                  "fixtures=%d checkpoint_every=%d hwm=%d flow=%d",
                  scenario.c_str(), topology.c_str(), Cs() ? world.c_str() : "local", upstream.c_str(),
                  engine::EncodingName(encoding), downDelta ? 1 : 0, upDelta ? 1 : 0, keyframe, players, moving, statics,
                  density, seed, warmupBlocks, measureBlocks, window, fixtures ? 1 : 0, checkpointEvery, hwm,
                  noFlowControl ? 0 : 1);
    return buf;
}

namespace {

bool ToInt(const std::string &s, long long lo, long long hi, int &out) {
    char *end = nullptr;
    const long long v = std::strtoll(s.c_str(), &end, 10);
    if (s.empty() || *end != '\0' || v < lo || v > hi) return false;
    out = static_cast<int>(v);
    return true;
}

bool ToBool(const std::string &s, bool &out) {
    if (s == "on" || s == "1" || s == "true") { out = true; return true; }
    if (s == "off" || s == "0" || s == "false") { out = false; return true; }
    return false;
}

} // namespace

bool ParseOptions(int argc, char **argv, Options &o, std::string &error) {
    if (argc < 2) {
        error = "usage: rerun_bench coordinator|server|client|peer|oracle [--flag value]...";
        return false;
    }
    o.role = argv[1];
    if (o.role != "coordinator" && o.role != "server" && o.role != "client" && o.role != "peer" && o.role != "oracle") {
        error = "unknown role " + o.role;
        return false;
    }
    for (int i = 2; i < argc; ++i) {
        const std::string flag = argv[i];
        if (flag.rfind("--", 0) != 0 || i + 1 >= argc) {
            error = "expected --flag value, got " + flag;
            return false;
        }
        const std::string v = argv[++i];
        bool ok = true;
        if (flag == "--run") o.run = v;
        else if (flag == "--campaign") o.campaign = v;
        else if (flag == "--scenario") { o.scenario = v; ok = v == "engine" || v == "game"; }
        else if (flag == "--topology") { o.topology = v; ok = v == "cs" || v == "p2p"; }
        else if (flag == "--world") { o.world = v; ok = v == "native" || v == "local"; }
        else if (flag == "--upstream") { o.upstream = v; ok = v == "input" || v == "state"; }
        else if (flag == "--encoding") ok = engine::ParseEncoding(v, o.encoding);
        else if (flag == "--down-delta") ok = ToBool(v, o.downDelta);
        else if (flag == "--up-delta") ok = ToBool(v, o.upDelta);
        else if (flag == "--keyframe") ok = ToInt(v, 1, 100000, o.keyframe);
        else if (flag == "--players") ok = ToInt(v, 1, 8, o.players);
        else if (flag == "--moving") ok = ToInt(v, 0, 100000, o.moving);
        else if (flag == "--static") ok = ToInt(v, 0, 100000, o.statics);
        else if (flag == "--density") ok = ToInt(v, -1, 100, o.density);
        else if (flag == "--seed") ok = ToInt(v, 0, 1000000000, o.seed);
        else if (flag == "--warmup-blocks") ok = ToInt(v, 1, 100000000, o.warmupBlocks);
        else if (flag == "--measure-blocks") ok = ToInt(v, 1, 100000000, o.measureBlocks);
        else if (flag == "--window") ok = ToInt(v, 1, 1000000, o.window);
        else if (flag == "--id") ok = ToInt(v, 0, 7, o.id);
        else if (flag == "--port-base") ok = ToInt(v, 1024, 64000, o.portBase);
        else if (flag == "--fixtures") ok = ToBool(v, o.fixtures);
        else if (flag == "--checkpoint-every") ok = ToInt(v, 0, 100000000, o.checkpointEvery);
        else if (flag == "--hwm") ok = ToInt(v, 1, 1000000, o.hwm);
        else if (flag == "--timeout") ok = ToInt(v, 1, 100000, o.timeoutS);
        else if (flag == "--oracle") o.oracle = v;
        else if (flag == "--out") o.out = v;
        else if (flag == "--delay-start-ms") ok = ToInt(v, 0, 600000, o.delayStartMs);
        else if (flag == "--slow-us") ok = ToInt(v, 0, 10000000, o.slowUs);
        else if (flag == "--kill-at-block") ok = ToInt(v, -1, 100000000, o.killAtBlock);
        else if (flag == "--no-flow-control") ok = ToBool(v, o.noFlowControl);
        else {
            error = "unknown flag " + flag;
            return false;
        }
        if (!ok) {
            error = "bad value for " + flag + ": " + v;
            return false;
        }
    }
    if (o.topology == "p2p") o.world = "local";
    if (o.scenario == "game" && (o.topology != "cs" || o.world != "native" || o.upstream != "input")) {
        error = "the Clockwork Dash scenario runs only client-server, native world, input upstream";
        return false;
    }
    if (o.density >= 0 && !o.Native()) {
        error = "change-density objects need the server-replicated (native) world";
        return false;
    }
    if (o.topology == "cs" && o.world == "local" && o.upstream != "state") {
        error = "the local-world (relay) CS mode exchanges player state; use --upstream state";
        return false;
    }
    if ((o.role == "client" || o.role == "peer") && (o.id < 0 || o.id >= o.players)) {
        error = "--id must name a player slot below --players";
        return false;
    }
    return true;
}

void Json::Key(const std::string &key) {
    if (!body.empty()) body += ',';
    body += '"';
    body += JsonEscape(key);
    body += "\":";
}

Json &Json::Str(const std::string &key, const std::string &value) {
    Key(key);
    body += '"' + JsonEscape(value) + '"';
    return *this;
}

Json &Json::Int(const std::string &key, long long value) {
    Key(key);
    body += std::to_string(value);
    return *this;
}

Json &Json::Num(const std::string &key, double value) {
    Key(key);
    if (!std::isfinite(value)) {
        body += "null";
        return *this;
    }
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.17g", value);
    body += buf;
    return *this;
}

Json &Json::Bool(const std::string &key, bool value) {
    Key(key);
    body += value ? "true" : "false";
    return *this;
}

Json &Json::Raw(const std::string &key, const std::string &json) {
    Key(key);
    body += json;
    return *this;
}

std::string JsonEscape(const std::string &s) {
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += static_cast<char>(c);
            }
        }
    }
    return out;
}

Usage ReadUsage() {
    rusage ru{};
    getrusage(RUSAGE_SELF, &ru);
    Usage u;
    u.userNs = static_cast<int64_t>(ru.ru_utime.tv_sec) * 1000000000LL + ru.ru_utime.tv_usec * 1000LL;
    u.sysNs = static_cast<int64_t>(ru.ru_stime.tv_sec) * 1000000000LL + ru.ru_stime.tv_usec * 1000LL;
    u.maxRssBytes = ru.ru_maxrss;   // bytes on macOS
    return u;
}

std::string Bind(int port) {
    return "tcp://127.0.0.1:" + std::to_string(port);
}

std::string Connect(int port) {
    return "tcp://127.0.0.1:" + std::to_string(port);
}

} // namespace rb
