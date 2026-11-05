#pragma once

#include "engine/Network.hpp"
#include "engine/Timeline.hpp"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace engine {

// Section 5: peer-to-peer networking, as an engine feature.
//
// A Peer is one process in a mesh of up to maxPeers. Every peer binds a PUB
// socket on portBase + id and subscribes to every other slot, so:
//
//   - the PORT IS THE ID. Claiming it by binding is atomic, so ids are unique
//     with nobody handing them out;
//   - there is no discovery. 0MQ connects before the far end has bound and
//     retries on its own, so peers may start in any order;
//   - everything a peer says goes to everyone, and nothing passes through a
//     server.
//
// On that mesh the Peer offers two things a game cannot easily build alone:
//
//   SHARED CLOCK   A Timeline whose origin is an EPOCH the peers agree on by
//                  gossiping the minimum: each proposes "now", adopts anything
//                  earlier it hears, ties break on the lower id. One rule, run
//                  identically everywhere, converging within a gossip period.
//                  A world that is a pure function of this clock - Maze Run's
//                  patrolling enemies - is the same world on every peer.
//
//   EVENT LOG      For worlds that CHANGE. Each peer appends its own events,
//                  and every gossip period re-sends every event it knows -
//                  anyone's, not just its own; receivers deduplicate by (owner,
//                  seq). PUB/SUB is lossy and peers join late, even into a slot
//                  a departed peer held, and this is what makes all of it
//                  harmless: within a period everyone holds the same log. A
//                  world that is a pure function of (clock, log) is the same
//                  world everywhere even as it changes - see mz_peer.
//
// The hybrid model (a server owns the world, peers exchange player state)
// uses only the mesh: Claim(id-from-server), ConnectAll(), Publish(), Tick().
class Peer {
public:
    struct Config {
        int portBase = 0;
        int maxPeers = 4;
        int gossipMs = 250;
        std::vector<std::string> topics;   // game topics to subscribe to, besides the engine's own
    };

    explicit Peer(Config config);

    // Bind this peer's publisher and so claim an id. `requested` < 0 takes
    // the first free slot; otherwise that exact slot or -1. Main thread,
    // before anything else - and before opening a window, so a full table
    // does not flash one and die.
    int Claim(int requested = -1);
    void ConnectAll();

    int Id() const { return id; }
    int MaxPeers() const { return config.maxPeers; }
    const std::string &LastError() const { return lastError; }

    // --- shared clock --------------------------------------------------------
    void EnableSharedClock();
    Timeline &SharedClock() { return sharedClock; }
    double SharedSeconds();                 // convenience: SharedClock().GetSeconds()
    int64_t Epoch() const;
    int EpochProposer() const;
    int EpochAdoptions() const;
    bool HeardFrom(int peerId) const;       // any EPOCH from this peer yet
    bool AgreesOnEpoch(int peerId) const;   // its last EPOCH equals mine

    // --- event log -------------------------------------------------------------
    struct Event {
        int owner = -1;
        int64_t seq = 0;
        std::string payload;
    };
    // Any thread. Stamps a seq, records it, and it goes out on the next gossip.
    void AppendEvent(const std::string &payload);

    // This process's first seq: its start time in microseconds. Seqs count up
    // from here, so a peer taking a departed peer's slot never reuses its ids,
    // and an event from this slot with a lower seq was fired by a predecessor.
    int64_t FirstSeq() const { return firstSeq; }
    // True once after new events arrived (from anyone), then false until more do.
    bool TakeEventsDirty();
    std::vector<Event> Events() const;      // snapshot of everything known
    size_t EventCount() const;

    // --- talking -------------------------------------------------------------
    // Worker thread. Sent to every other peer immediately.
    void Publish(const std::string &topic, const std::string &payload);

    // Worker thread, once per iteration. Drains everything queued: EPOCH and
    // EVENTS are handled here (and EPOCH is also passed on, body positioned
    // after the engine's fields, so a game can read extras it attached).
    // Everything else goes to onMessage(topic, body, fromId) - fromId is the
    // sender when the engine knows it, else -1. Then, if a gossip period has
    // elapsed on WALL time: EPOCH (with epochExtras() appended if given), every
    // event I know as one EVENTS message per owner, and each string
    // extraGossip() returns, verbatim.
    void Tick(const std::function<void(const std::string &topic, std::istringstream &body, int fromId)> &onMessage,
              const std::function<std::string()> &epochExtras = nullptr,
              const std::function<std::vector<std::string>()> &extraGossip = nullptr);

    int MessagesHeard() const { return messagesHeard; }

private:
    void OnEpoch(std::istringstream &in, const std::function<void(const std::string &, std::istringstream &, int)> &onMessage);
    void OnEvents(std::istringstream &in);
    std::vector<std::string> EncodeKnownEvents() const;

    Config config;
    int id = -1;
    std::string lastError;

    net::Socket pub{net::SocketType::Publish};
    net::Socket sub{net::SocketType::Subscribe};

    // Guards everything below that main and worker both touch.
    mutable std::mutex mutex;

    bool clockEnabled = false;
    Timeline sharedClock{nullptr, 1};
    int64_t epoch = 0;
    int epochProposer = -1;
    int epochAdoptions = 0;
    std::vector<int64_t> peerEpoch;
    std::vector<bool> peerHeard;

    std::vector<Event> events;
    std::set<std::pair<int, int64_t>> eventIds;
    const int64_t firstSeq = Timeline::RealTimeNow();
    int64_t nextSeq = firstSeq;
    bool eventsDirty = false;

    long long lastGossipMs = 0;
    std::atomic<int> messagesHeard{0};
};

} // namespace engine
