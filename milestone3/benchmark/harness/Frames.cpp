#include "Frames.hpp"

#include <cstring>

namespace rb {

namespace {

void Put16(std::string &out, uint16_t v) {
    out += static_cast<char>(v & 0xff);
    out += static_cast<char>((v >> 8) & 0xff);
}

void Put32(std::string &out, uint32_t v) {
    for (int i = 0; i < 4; ++i) out += static_cast<char>((v >> (8 * i)) & 0xff);
}

uint16_t Get16(const std::string &in, size_t at) {
    return static_cast<uint16_t>(static_cast<unsigned char>(in[at]) | (static_cast<unsigned char>(in[at + 1]) << 8));
}

uint32_t Get32(const std::string &in, size_t at) {
    uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<uint32_t>(static_cast<unsigned char>(in[at + i])) << (8 * i);
    return v;
}

} // namespace

std::string EncodeFrame(const Frame &f) {
    std::string out;
    out.reserve(FRAME_FIXED + f.acks.size() * 6 + f.payload.size());
    out += f.topic[0];
    out += f.topic[1];
    out += static_cast<char>(f.type);
    out += static_cast<char>(f.phase);
    Put16(out, f.source);
    Put16(out, f.stream);
    Put32(out, f.seq);
    Put32(out, f.block);
    Put32(out, f.run);
    out += static_cast<char>(static_cast<uint8_t>(f.acks.size()));
    for (const Ack &a : f.acks) {
        Put16(out, a.stream);
        Put32(out, a.block);
    }
    Put32(out, static_cast<uint32_t>(f.payload.size()));
    Put32(out, Crc32c(f.payload.data(), f.payload.size()));
    out += f.payload;
    return out;
}

FrameError DecodeFrame(const std::string &m, Frame &f) {
    if (m.size() < FRAME_FIXED) return FrameError::Short;
    f.topic[0] = m[0];
    f.topic[1] = m[1];
    f.type = static_cast<uint8_t>(m[2]);
    f.phase = static_cast<uint8_t>(m[3]);
    f.source = Get16(m, 4);
    f.stream = Get16(m, 6);
    f.seq = Get32(m, 8);
    f.block = Get32(m, 12);
    f.run = Get32(m, 16);
    const size_t acks = static_cast<unsigned char>(m[20]);
    size_t at = 21;
    if (m.size() < FRAME_FIXED + acks * 6) return FrameError::Short;
    f.acks.resize(acks);
    for (size_t i = 0; i < acks; ++i) {
        f.acks[i].stream = Get16(m, at);
        f.acks[i].block = Get32(m, at + 2);
        at += 6;
    }
    const uint32_t length = Get32(m, at);
    const uint32_t checksum = Get32(m, at + 4);
    at += 8;
    if (m.size() - at != length) return FrameError::Length;
    if (Crc32c(m.data() + at, length) != checksum) return FrameError::Checksum;
    f.payload.assign(m, at, length);
    return FrameError::None;
}

const char *FrameErrorName(FrameError e) {
    switch (e) {
    case FrameError::None: return "none";
    case FrameError::Short: return "short";
    case FrameError::Length: return "length";
    case FrameError::Checksum: return "checksum";
    }
    return "?";
}

std::string CountersJson(const Counters &c) {
    Json j;
    j.Int("frames", static_cast<long long>(c.frames))
        .Int("payload_bytes", static_cast<long long>(c.payloadBytes))
        .Int("frame_bytes", static_cast<long long>(c.frameBytes))
        .Int("applied", static_cast<long long>(c.applied))
        .Int("keyframes", static_cast<long long>(c.keyframes))
        .Int("duplicates", static_cast<long long>(c.duplicates))
        .Int("stale", static_cast<long long>(c.stale))
        .Int("gaps", static_cast<long long>(c.gaps))
        .Int("missing", static_cast<long long>(c.missing))
        .Int("unsynced", static_cast<long long>(c.unsynced))
        .Int("malformed", static_cast<long long>(c.malformed))
        .Int("send_failures", static_cast<long long>(c.sendFailures))
        .Int("decode_ns", static_cast<long long>(c.decodeNs))
        .Int("encode_ns", static_cast<long long>(c.encodeNs))
        .Int("objects", static_cast<long long>(c.objects))
        .Int("removals", static_cast<long long>(c.removals));
    return j.Text();
}

Admit AdmitFrame(InStream &s, const Frame &f, bool keyframe) {
    Counters &c = s.c[f.phase < 3 ? f.phase : 2];
    const uint32_t expected = s.started ? s.lastSeq + 1 : 0;
    if (s.started && f.seq == s.lastSeq) {
        ++c.duplicates;
        return Admit::Duplicate;
    }
    if (s.started && f.seq < s.lastSeq) {
        ++c.stale;
        return Admit::Stale;
    }
    if (f.seq > expected) {
        ++c.gaps;
        c.missing += f.seq - expected;
        if (!keyframe && s.synced) {
            s.synced = false;
            s.unsyncOpen = true;
            s.unsyncFrom = expected;
        }
    }
    s.started = true;
    s.lastSeq = f.seq;
    if (!keyframe && !s.synced) {
        ++c.unsynced;
        if (!s.unsyncOpen) {
            s.unsyncOpen = true;
            s.unsyncFrom = f.seq;
        }
        return Admit::Unsynced;
    }
    return Admit::Apply;
}

void MarkApplied(InStream &s, const Frame &f, bool keyframe) {
    Counters &c = s.c[f.phase < 3 ? f.phase : 2];
    ++c.applied;
    if (keyframe) {
        ++c.keyframes;
        if (s.unsyncOpen) {
            s.unsyncHistory.emplace_back(s.unsyncFrom, f.seq);
            s.unsyncOpen = false;
        }
        s.synced = true;
    }
    s.appliedBlock = f.block;
}

std::string InStreamJson(const InStream &s) {
    std::string history = "[";
    for (size_t i = 0; i < s.unsyncHistory.size(); ++i) {
        if (i) history += ',';
        history += "[" + std::to_string(s.unsyncHistory[i].first) + "," + std::to_string(s.unsyncHistory[i].second) + "]";
    }
    history += "]";
    Json j;
    j.Int("stream", s.id)
        .Int("last_seq", s.started ? static_cast<long long>(s.lastSeq) : -1)
        .Int("applied_block", s.appliedBlock)
        .Bool("synced", s.synced)
        .Bool("unsync_open", s.unsyncOpen)
        .Raw("unsync_history", history)
        .Raw("setup", CountersJson(s.c[0]))
        .Raw("warmup", CountersJson(s.c[1]))
        .Raw("measure", CountersJson(s.c[2]));
    return j.Text();
}

std::string OutStreamJson(const OutStream &s) {
    Json j;
    j.Int("stream", s.id)
        .Int("next_seq", s.nextSeq)
        .Raw("setup", CountersJson(s.c[0]))
        .Raw("warmup", CountersJson(s.c[1]))
        .Raw("measure", CountersJson(s.c[2]));
    return j.Text();
}

} // namespace rb
