#include "engine/Serialize.hpp"

#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>

namespace engine {

const char *EncodingName(Encoding encoding) {
    return encoding == Encoding::Binary ? "binary" : "text";
}

bool ParseEncoding(const std::string &name, Encoding &out) {
    if (name == "text") { out = Encoding::Text; return true; }
    if (name == "binary") { out = Encoding::Binary; return true; }
    return false;
}

namespace {

// --- Text ----------------------------------------------------------------------

// Strings may hold anything; in text they must be one token. Spaces, '%' and
// control bytes become %XX, and the empty string is a lone '%'.
void Escape(const std::string &s, std::string &out) {
    if (s.empty()) { out += '%'; return; }
    static const char HEX[] = "0123456789ABCDEF";
    for (unsigned char c : s) {
        if (c <= ' ' || c == '%' || c >= 0x7f) {
            out += '%';
            out += HEX[c >> 4];
            out += HEX[c & 0xf];
        } else {
            out += static_cast<char>(c);
        }
    }
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool Unescape(const std::string &data, size_t begin, size_t end, std::string &out) {
    out.clear();
    if (end - begin == 1 && data[begin] == '%') return true;
    for (size_t i = begin; i < end; ++i) {
        if (data[i] != '%') { out += data[i]; continue; }
        if (i + 2 >= end) return false;
        const int hi = HexDigit(data[i + 1]), lo = HexDigit(data[i + 2]);
        if (hi < 0 || lo < 0) return false;
        out += static_cast<char>(hi * 16 + lo);
        i += 2;
    }
    return true;
}

class TextWriter final : public Writer {
public:
    void U8(uint8_t v) override { Integer(v); }
    void U16(uint16_t v) override { Integer(v); }
    void U32(uint32_t v) override { Integer(v); }
    void I32(int32_t v) override { Integer(v); }
    void I64(int64_t v) override { Integer(v); }
    // The shortest digits that read back as exactly this float (std::to_chars
    // guarantees the round trip), so text and binary decode to the same bits.
    void F32(float v) override {
        char buf[32];
        const auto r = std::to_chars(buf, buf + sizeof buf, v);
        Separate();
        out.append(buf, static_cast<size_t>(r.ptr - buf));
    }
    void Str(const std::string &v) override {
        Separate();
        Escape(v, out);
    }
    void Tag(const ComponentInfo &info) override {
        Separate();
        out += info.name;
    }
    void Raw(const std::string &encoded) override {
        if (encoded.empty()) return;
        Separate();
        out += encoded;
    }
    const std::string &Data() const override { return out; }
    std::string Take() override { return std::move(out); }
    void Clear() override { out.clear(); }

private:
    template <class T>
    void Integer(T v) {
        char buf[24];
        const auto r = std::to_chars(buf, buf + sizeof buf, v);
        Separate();
        out.append(buf, static_cast<size_t>(r.ptr - buf));
    }
    void Separate() {
        if (!out.empty()) out += ' ';
    }
    std::string out;
};

// Reads tokens in place: numbers are parsed straight out of the message, with
// no temporary strings.
class TextReader final : public Reader {
public:
    TextReader(const std::string &d, size_t start) : data(d), pos(start) {}

    uint8_t U8() override { return static_cast<uint8_t>(Integer<uint64_t>(0xff)); }
    uint16_t U16() override { return static_cast<uint16_t>(Integer<uint64_t>(0xffff)); }
    uint32_t U32() override { return static_cast<uint32_t>(Integer<uint64_t>(0xffffffffull)); }
    int32_t I32() override { return static_cast<int32_t>(Integer<int64_t>(0)); }
    int64_t I64() override { return Integer<int64_t>(0); }
    float F32() override {
        size_t begin, end;
        if (!Next(begin, end)) return 0.0f;
        char *stop = nullptr;
        const float v = std::strtof(data.c_str() + begin, &stop);
        if (stop != data.c_str() + end) Fail();
        return v;
    }
    std::string Str() override {
        size_t begin, end;
        std::string s;
        if (!Next(begin, end)) return s;
        if (!Unescape(data, begin, end, s)) Fail();
        return s;
    }
    const ComponentInfo *Tag() override {
        size_t begin, end;
        if (!Next(begin, end)) return nullptr;
        const ComponentInfo *info = ComponentRegistry::ByName(data.c_str() + begin, end - begin);
        if (!info) Fail();
        return info;
    }
    bool AtEnd() const override {
        size_t p = pos;
        while (p < data.size() && data[p] == ' ') ++p;
        return p >= data.size();
    }

private:
    bool Next(size_t &begin, size_t &end) {
        while (pos < data.size() && data[pos] == ' ') ++pos;
        if (pos >= data.size()) { Fail(); return false; }
        begin = pos;
        while (pos < data.size() && data[pos] != ' ') ++pos;
        end = pos;
        return true;
    }
    // Unsigned types are checked against `max`; signed ones take 0 for "no limit".
    template <class T>
    T Integer(T max) {
        size_t begin, end;
        if (!Next(begin, end)) return 0;
        T v = 0;
        const char *first = data.c_str() + begin, *last = data.c_str() + end;
        const auto r = std::from_chars(first, last, v);
        if (r.ec != std::errc() || r.ptr != last || (max != 0 && v > max)) Fail();
        return v;
    }

    const std::string &data;
    size_t pos;
};

// --- Binary --------------------------------------------------------------------

class BinaryWriter final : public Writer {
public:
    void U8(uint8_t v) override { out += static_cast<char>(v); }
    void U16(uint16_t v) override { Little(v, 2); }
    void U32(uint32_t v) override { Little(v, 4); }
    void I32(int32_t v) override { Little(static_cast<uint32_t>(v), 4); }
    void I64(int64_t v) override { Little(static_cast<uint64_t>(v), 8); }
    void F32(float v) override {
        uint32_t bits;
        std::memcpy(&bits, &v, 4);
        Little(bits, 4);
    }
    void Str(const std::string &v) override {
        U16(static_cast<uint16_t>(v.size() > 0xffff ? 0xffff : v.size()));
        out.append(v, 0, v.size() > 0xffff ? 0xffff : v.size());
    }
    void Tag(const ComponentInfo &info) override { U16(info.wireId); }
    void Raw(const std::string &encoded) override { out += encoded; }
    const std::string &Data() const override { return out; }
    std::string Take() override { return std::move(out); }
    void Clear() override { out.clear(); }

private:
    // Byte by byte into a buffer, so the wire is little-endian whatever the
    // host is, then one append.
    void Little(uint64_t v, int bytes) {
        char buf[8];
        for (int i = 0; i < bytes; ++i) buf[i] = static_cast<char>((v >> (8 * i)) & 0xff);
        out.append(buf, static_cast<size_t>(bytes));
    }
    std::string out;
};

class BinaryReader final : public Reader {
public:
    BinaryReader(const std::string &d, size_t start) : data(d), pos(start) {}

    uint8_t U8() override { return static_cast<uint8_t>(Little(1)); }
    uint16_t U16() override { return static_cast<uint16_t>(Little(2)); }
    uint32_t U32() override { return static_cast<uint32_t>(Little(4)); }
    int32_t I32() override { return static_cast<int32_t>(static_cast<uint32_t>(Little(4))); }
    int64_t I64() override { return static_cast<int64_t>(Little(8)); }
    float F32() override {
        const uint32_t bits = static_cast<uint32_t>(Little(4));
        float v;
        std::memcpy(&v, &bits, 4);
        return v;
    }
    std::string Str() override {
        const size_t n = U16();
        if (!ok || pos + n > data.size()) { Fail(); return {}; }
        std::string s(data, pos, n);
        pos += n;
        return s;
    }
    const ComponentInfo *Tag() override {
        const uint16_t id = U16();
        if (!ok) return nullptr;
        const ComponentInfo *info = ComponentRegistry::ByWireId(id);
        if (!info) Fail();
        return info;
    }
    bool AtEnd() const override { return pos >= data.size(); }

private:
    uint64_t Little(int bytes) {
        if (pos + static_cast<size_t>(bytes) > data.size()) { Fail(); pos = data.size(); return 0; }
        uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) {
            v |= static_cast<uint64_t>(static_cast<unsigned char>(data[pos + static_cast<size_t>(i)])) << (8 * i);
        }
        pos += static_cast<size_t>(bytes);
        return v;
    }

    const std::string &data;
    size_t pos;
};

// --- Registry --------------------------------------------------------------------

// A deque, so a pointer handed out stays valid when a later type registers.
std::deque<ComponentInfo> &Table() {
    static std::deque<ComponentInfo> table;
    return table;
}

// FNV-1a, folded to 16 bits: the same name gives the same id in every process.
uint16_t WireIdFor(const char *name) {
    uint32_t h = 2166136261u;
    for (const char *p = name; *p; ++p) {
        h ^= static_cast<unsigned char>(*p);
        h *= 16777619u;
    }
    return static_cast<uint16_t>((h >> 16) ^ (h & 0xffff));
}

} // namespace

std::unique_ptr<Writer> MakeWriter(Encoding encoding) {
    if (encoding == Encoding::Binary) return std::make_unique<BinaryWriter>();
    return std::make_unique<TextWriter>();
}

std::unique_ptr<Reader> MakeReader(Encoding encoding, const std::string &data, size_t start) {
    if (encoding == Encoding::Binary) return std::make_unique<BinaryReader>(data, start);
    return std::make_unique<TextReader>(data, start);
}

void ComponentRegistry::Add(const char *name, ComponentKey key, std::unique_ptr<Component> (*make)()) {
    const uint16_t wireId = WireIdFor(name);
    for (const ComponentInfo &info : Table()) {
        if (info.key == key) return;   // already registered
        if (std::strcmp(info.name, name) == 0 || info.wireId == wireId) {
            fprintf(stderr, "ComponentRegistry: '%s' clashes with '%s' - rename one\n", name, info.name);
            std::abort();
        }
    }
    Table().push_back(ComponentInfo{name, wireId, key, make});
}

const ComponentInfo *ComponentRegistry::ByName(const std::string &name) {
    return ByName(name.data(), name.size());
}

const ComponentInfo *ComponentRegistry::ByName(const char *name, size_t length) {
    for (const ComponentInfo &info : Table()) {
        if (std::strlen(info.name) == length && std::memcmp(info.name, name, length) == 0) return &info;
    }
    return nullptr;
}

const ComponentInfo *ComponentRegistry::ByWireId(uint16_t wireId) {
    for (const ComponentInfo &info : Table()) {
        if (info.wireId == wireId) return &info;
    }
    return nullptr;
}

const ComponentInfo *ComponentRegistry::ByKey(ComponentKey key) {
    for (const ComponentInfo &info : Table()) {
        if (info.key == key) return &info;
    }
    return nullptr;
}

} // namespace engine
