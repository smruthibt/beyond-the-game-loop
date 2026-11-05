#pragma once

// The benchmark's message framing and per-stream receive bookkeeping.
//
// Every 0MQ message is one frame: a two-byte topic (0MQ subscription prefix),
// a fixed header, piggybacked acknowledgements, then an optional payload,
// which is an engine snapshot exactly as SnapshotEncoder produced it.
//
//   topic[2] type phase source:u16 stream:u16 seq:u32 block:u32 run:u32
//   ackCount:u8 { stream:u16 block:u32 } * ackCount  payloadLen:u32 checksum:u32  payload
//
// The header is identical for every strategy, so it never favours one side of
// a comparison; its bytes are reported separately as framing.

#include "Common.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace rb {

enum MsgType : uint8_t {
    MSG_DATA = 'D',     // payload is an engine snapshot
    MSG_ACK = 'A',      // acknowledgements only
    MSG_CREDIT = 'C',   // relay server -> clients: per-stream credit
    MSG_SYNC = 'S',     // setup beacon / subscription proof
    MSG_REPLY = 'K',    // REP reply to an upstream request
};

constexpr uint16_t STREAM_WORLD = 0x0100;
inline uint16_t StreamUp(int client) { return static_cast<uint16_t>(0x0200 + client); }
inline uint16_t StreamForward(int client) { return static_cast<uint16_t>(0x0300 + client); }
inline uint16_t StreamPeer(int peer) { return static_cast<uint16_t>(0x0400 + peer); }
inline uint16_t StreamCorrection(int peer) { return static_cast<uint16_t>(0x0500 + peer); }

struct Ack {
    uint16_t stream = 0;
    uint32_t block = 0;
};

struct Frame {
    char topic[2] = {'_', '_'};
    uint8_t type = MSG_DATA;
    uint8_t phase = 0;
    uint16_t source = 0;
    uint16_t stream = 0;
    uint32_t seq = 0;
    uint32_t block = 0;
    uint32_t run = 0;
    std::vector<Ack> acks;
    std::string payload;
};

constexpr size_t FRAME_FIXED = 2 + 1 + 1 + 2 + 2 + 4 + 4 + 4 + 1 + 4 + 4;

std::string EncodeFrame(const Frame &f);

enum class FrameError : uint8_t { None, Short, Length, Checksum };
FrameError DecodeFrame(const std::string &message, Frame &f);
const char *FrameErrorName(FrameError e);

// Counters kept per phase (setup, warm-up, measure) for one direction of one
// stream. Messages are attributed to the phase their origin tagged them with.
struct Counters {
    uint64_t frames = 0;
    uint64_t payloadBytes = 0;
    uint64_t frameBytes = 0;     // whole messages, header and acks included
    uint64_t applied = 0;
    uint64_t keyframes = 0;
    uint64_t duplicates = 0;
    uint64_t stale = 0;
    uint64_t gaps = 0;           // gap events
    uint64_t missing = 0;        // messages skipped over by gaps
    uint64_t unsynced = 0;       // deltas rejected while the stream lacked a valid base
    uint64_t malformed = 0;      // failed framing/checksum or engine parse
    uint64_t sendFailures = 0;
    uint64_t decodeNs = 0;
    uint64_t encodeNs = 0;
    uint64_t objects = 0;        // objects written (encoder) or created+updated (decoder)
    uint64_t removals = 0;
};

std::string CountersJson(const Counters &c);

// Receive-side state of one stream: in-order delivery, duplicates, stale
// messages, and delta dependency. A gap in a delta stream makes it
// unsynchronised until a keyframe arrives; a gap in a full-snapshot stream is
// a skipped complete state, counted as missing but not as a broken dependency.
struct InStream {
    uint16_t id = 0;
    bool started = false;
    uint32_t lastSeq = 0;
    bool synced = false;
    int64_t appliedBlock = -1;
    uint32_t unsyncFrom = 0;
    bool unsyncOpen = false;
    std::vector<std::pair<uint32_t, uint32_t>> unsyncHistory;   // [first rejected seq, restoring keyframe seq]
    Counters c[3];
};

enum class Admit { Apply, Duplicate, Stale, Unsynced };
Admit AdmitFrame(InStream &s, const Frame &f, bool keyframe);
void MarkApplied(InStream &s, const Frame &f, bool keyframe);
std::string InStreamJson(const InStream &s);

struct OutStream {
    uint16_t id = 0;
    uint32_t nextSeq = 0;
    Counters c[3];
};
std::string OutStreamJson(const OutStream &s);

} // namespace rb
