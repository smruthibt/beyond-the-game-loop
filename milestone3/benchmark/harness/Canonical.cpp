#include "Canonical.hpp"

#include "Common.hpp"
#include "Game.hpp"
#include "engine/Components.hpp"
#include "engine/Serialize.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace rb {

using namespace engine;

namespace {

// An engine Writer that produces typed, space-separated tokens.
class CanonicalWriter final : public Writer {
public:
    void U8(uint8_t v) override { Token("u" + std::to_string(v)); }
    void U16(uint16_t v) override { Token("u" + std::to_string(v)); }
    void U32(uint32_t v) override { Token("u" + std::to_string(v)); }
    void I32(int32_t v) override { Token("i" + std::to_string(v)); }
    void I64(int64_t v) override { Token("i" + std::to_string(v)); }
    void F32(float v) override {
        char buf[48];
        std::snprintf(buf, sizeof buf, "f%a", static_cast<double>(v));
        Token(buf);
    }
    void Str(const std::string &v) override {
        std::string t = "s";
        for (char c : v) {
            if (c == ' ' || c == '\n' || c == '%') {
                char buf[8];
                std::snprintf(buf, sizeof buf, "%%%02x", static_cast<unsigned char>(c));
                t += buf;
            } else {
                t += c;
            }
        }
        Token(t);
    }
    void Tag(const ComponentInfo &info) override { Token(std::string("#") + info.name); }
    void Raw(const std::string &encoded) override { Token(encoded); }
    const std::string &Data() const override { return out; }
    std::string Take() override { return std::move(out); }
    void Clear() override { out.clear(); }

    void Token(const std::string &t) {
        if (!out.empty()) out += ' ';
        out += t;
    }

private:
    std::string out;
};

std::string Kind(const std::string &kind) {
    std::string k = kind.empty() ? "-" : kind;
    std::replace(k.begin(), k.end(), ' ', '_');
    return k;
}

std::vector<std::string> Split(const std::string &s) {
    std::vector<std::string> tokens;
    std::istringstream in(s);
    std::string t;
    while (in >> t) tokens.push_back(t);
    return tokens;
}

// Equal, or the absolute difference of the largest differing float field.
bool CompareLines(const std::string &a, const std::string &b, double &floatError) {
    if (a == b) return true;
    const std::vector<std::string> x = Split(a), y = Split(b);
    floatError = 0.0;
    if (x.size() != y.size()) {
        floatError = INFINITY;
        return false;
    }
    for (size_t i = 0; i < x.size(); ++i) {
        if (x[i] == y[i]) continue;
        if (!x[i].empty() && !y[i].empty() && x[i][0] == 'f' && y[i][0] == 'f') {
            const double p = std::strtod(x[i].c_str() + 1, nullptr), q = std::strtod(y[i].c_str() + 1, nullptr);
            const double d = std::fabs(p - q);
            floatError = std::max(floatError, std::isnan(d) ? INFINITY : d);
        } else {
            floatError = INFINITY;   // a non-float field differs
        }
    }
    return false;
}

} // namespace

std::string CanonicalObject(const GameObject &object) {
    CanonicalWriter w;
    w.Token(std::to_string(object.Id()));
    w.Token(Kind(object.Kind()));
    w.Token(std::to_string(OwnerOf(object)));
    std::vector<std::pair<std::string, const Component *>> parts;
    for (const GameObject::Slot &slot : object.Components()) {
        const char *name = slot.component->TypeName();
        if (!name || slot.key == KeyOf<NetIdentity>()) continue;
        if (!ComponentRegistry::ByKey(slot.key)) continue;   // never travels
        parts.emplace_back(name, slot.component.get());
    }
    std::sort(parts.begin(), parts.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &p : parts) {
        w.Token("#" + p.first);
        p.second->Write(w);
    }
    return w.Take();
}

std::string CanonicalRuntime(const GameObject &object) {
    const RigidBody *body = object.Get<RigidBody>();
    const PlatformerController *pc = object.Get<PlatformerController>();
    if (!body && !pc) return {};
    CanonicalWriter w;
    w.Token("R");
    w.Token(std::to_string(object.Id()));
    if (pc) w.Token(pc->jumpLatched ? "latch1" : "latch0");
    if (body) {
        w.Token("g" + std::to_string(body->ground));
        w.F32(body->groundLast.x);
        w.F32(body->groundLast.y);
    }
    if (const clockwork::Racer *racer = object.Get<clockwork::Racer>()) w.Token(racer->restartHeld ? "rh1" : "rh0");
    return w.Take();
}

void WriteCheckpoint(std::FILE *out, long long block, const World &world) {
    std::fprintf(out, "B %lld\n", block);
    world.Each<NetIdentity>([&](const GameObject &o, const NetIdentity &) {
        std::fprintf(out, "%s\n", CanonicalObject(o).c_str());
    });
    world.Each<NetIdentity>([&](const GameObject &o, const NetIdentity &) {
        const std::string r = CanonicalRuntime(o);
        if (!r.empty()) std::fprintf(out, "%s\n", r.c_str());
    });
}

bool OracleFile::Load(const std::string &path, std::string &error) {
    std::ifstream in(path);
    if (!in) {
        error = "cannot read oracle " + path;
        return false;
    }
    Block *current = nullptr;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (line[0] == 'B' && line.size() > 2 && line[1] == ' ') {
            current = &blocks[std::atoll(line.c_str() + 2)];
            continue;
        }
        if (!current) {
            error = "oracle line before any block header";
            return false;
        }
        if (line[0] == 'R' && line.size() > 2 && line[1] == ' ') {
            const uint32_t id = static_cast<uint32_t>(std::strtoul(line.c_str() + 2, nullptr, 10));
            current->runtime[id] = line;
            continue;
        }
        const std::vector<std::string> t = Split(line);
        if (t.size() < 3) {
            error = "short oracle line: " + line;
            return false;
        }
        Line l;
        l.owner = std::atoi(t[2].c_str());
        l.text = line;
        current->objects[static_cast<uint32_t>(std::strtoul(t[0].c_str(), nullptr, 10))] = std::move(l);
    }
    return true;
}

const OracleFile::Block *OracleFile::Find(long long block) const {
    auto it = blocks.find(block);
    return it == blocks.end() ? nullptr : &it->second;
}

std::string VerifyResult::ToJson() const {
    std::string firstJson = "[";
    for (size_t i = 0; i < first.size(); ++i) {
        if (i) firstJson += ',';
        firstJson += '"' + JsonEscape(first[i]) + '"';
    }
    firstJson += "]";
    Json j;
    j.Int("checks", checks)
        .Int("compared", compared)
        .Int("mismatched", mismatched)
        .Int("missing", missing)
        .Int("extra", extra)
        .Int("runtime_compared", runtimeCompared)
        .Int("runtime_mismatched", runtimeMismatched)
        .Int("unavailable", unavailable)
        .Num("max_float_error", maxFloatError)
        .Bool("ok", Ok())
        .Raw("first", firstJson);
    return j.Text();
}

void Verify(const OracleFile &oracle, long long block, const World &world, const OwnerFilter &owners,
            const SimFilter &simulated, VerifyResult &r) {
    const OracleFile::Block *expected = oracle.Find(block);
    ++r.checks;
    if (!expected) {
        ++r.unavailable;
        if (r.first.size() < 8) r.first.push_back("no oracle checkpoint for block " + std::to_string(block));
        return;
    }
    auto note = [&](const std::string &what) {
        if (r.first.size() < 8) r.first.push_back("block " + std::to_string(block) + ": " + what);
    };
    std::map<uint32_t, bool> seen;
    world.Each<NetIdentity>([&](const GameObject &o, const NetIdentity &n) {
        if (!owners(n.owner)) return;
        const std::string line = CanonicalObject(o);
        auto it = expected->objects.find(o.Id());
        if (it == expected->objects.end() || !owners(it->second.owner)) {
            ++r.extra;
            note("extra object " + line.substr(0, 120));
            return;
        }
        seen[o.Id()] = true;
        ++r.compared;
        double err = 0.0;
        if (!CompareLines(line, it->second.text, err)) {
            ++r.mismatched;
            r.maxFloatError = std::max(r.maxFloatError, err);
            note("object differs: got [" + line.substr(0, 160) + "] want [" + it->second.text.substr(0, 160) + "]");
        }
        if (simulated && simulated(o)) {
            const std::string rt = CanonicalRuntime(o);
            if (rt.empty()) return;
            auto rit = expected->runtime.find(o.Id());
            ++r.runtimeCompared;
            double rerr = 0.0;
            if (rit == expected->runtime.end() || !CompareLines(rt, rit->second, rerr)) {
                ++r.runtimeMismatched;
                r.maxFloatError = std::max(r.maxFloatError, rerr);
                note("runtime differs: got [" + rt + "] want [" + (rit == expected->runtime.end() ? "" : rit->second) + "]");
            }
        }
    });
    for (const auto &entry : expected->objects) {
        if (!owners(entry.second.owner) || seen.count(entry.first)) continue;
        ++r.missing;
        note("missing object " + entry.second.text.substr(0, 120));
    }
}

} // namespace rb
