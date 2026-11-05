// A peer in the direct P2P mesh: one PUB bound by each peer, one SUB
// connected to every other peer, as in the engine's Peer. The level is built
// locally by every peer.
//
//   state  each peer simulates only its own player and publishes its state.
//   input  each peer publishes its buttons; every peer simulates every player
//          from everyone's buttons (lockstep at block granularity). The owner
//          also publishes a full-state correction every `keyframe` blocks on a
//          separate stream; receivers check it against their own simulation.
//
// Acknowledgements ride on each peer's own updates, or go alone when the peer
// is blocked or has finished.
#include "Endpoint.hpp"

#include <unistd.h>

#include <algorithm>
#include <deque>
#include <thread>

namespace rb {

using namespace engine;

namespace {

class Peer final : public Endpoint {
public:
    explicit Peer(const Options &o)
        : Endpoint(o, "peer" + std::to_string(o.id)),
          up(o.InputUp() ? SnapshotOptions(o.encoding, o.upDelta, o.keyframe, {InputState::TYPE})
                         : SnapshotOptions(o.encoding, o.upDelta, o.keyframe)),
          correction(SnapshotOptions(o.encoding)) {}

protected:
    bool Setup(std::string &err) override;
    void SetupIdle() override;
    bool RunPhase(long long from, long long to, uint8_t phase, std::string &err) override;
    void Service(int ms) override;
    void VerifyAt(long long block) override {
        const bool all = opts.InputUp();
        const int me = opts.id;
        Verify(oracle, block, world, [](int) { return true; },
               [all, me](const GameObject &o) { return o.Has<InputState>() && (all || OwnerOf(o) == me); }, verify);
    }
    std::string RoleJson() const override {
        Json j;
        j.Int("ack_only_frames", ackOnly)
            .Int("corrections_checked", correctionsChecked)
            .Int("corrections_mismatched", correctionsMismatched)
            .Int("corrections_unchecked", correctionsUnchecked);
        return j.Text();
    }

private:
    bool Receive(int timeoutMs, std::string &message);
    void Handle(const std::string &message);
    void Publish(const std::string &message, Counters &c, std::string *err);
    void SendAcks(uint8_t phase);
    int64_t AckLag() const;
    std::vector<Ack> Acks() const;
    int64_t Credit() const;
    bool ProduceState(long long block, uint8_t phase, std::string &err);
    bool PublishInput(long long block, uint8_t phase, std::string &err);
    bool SimulateInputBlock(long long block, uint8_t phase, std::string &err);
    void CheckCorrection(int sender, long long block, const std::string &line);
    size_t P(int j) const { return static_cast<size_t>(j); }

    net::Socket pub{net::SocketType::Publish};
    net::Socket sub{net::SocketType::Subscribe};
    int subTimeout = -2;
    SnapshotEncoder up;
    SnapshotEncoder correction;
    SnapshotDecoder down;
    std::vector<int64_t> applied;      // per peer: latest block of its stream applied (input mode: simulated)
    std::vector<int64_t> acked;        // what our last message reported
    std::vector<int64_t> ackFrom;      // per peer: its reported progress on our stream
    std::vector<bool> heard, heardMe;
    int64_t lastBeacon = 0;
    // Input mode.
    std::vector<std::deque<Frame>> inputs;                  // per peer, in order
    long long simulated = 0;
    std::vector<std::map<long long, std::string>> history; // per player: canonical line by block
    std::vector<std::map<long long, std::string>> pendingCorrections;
    long long correctionsChecked = 0, correctionsMismatched = 0, correctionsUnchecked = 0;
    long long ackOnly = 0;
};

bool Peer::Receive(int timeoutMs, std::string &message) {
    if (timeoutMs != subTimeout) {
        sub.SetReceiveTimeoutMs(timeoutMs);
        subTimeout = timeoutMs;
    }
    return sub.Receive(message);
}

bool Peer::Setup(std::string &err) {
    scenario->BuildLevel(world, false);
    scenario->AddPlayers(world);
    const size_t n = static_cast<size_t>(opts.players);
    applied.assign(n, 0);
    acked.assign(n, 0);
    ackFrom.assign(n, 0);
    heard.assign(n, false);
    heardMe.assign(n, false);
    inputs.resize(n);
    history.resize(n);
    pendingCorrections.resize(n);
    Stream(out, StreamPeer(opts.id));
    if (opts.InputUp()) Stream(out, StreamCorrection(opts.id));

    pub.SetSendQueue(opts.hwm);
    if (!pub.Bind(Bind(PeerPubPort(opts, opts.id)))) {
        err = "cannot bind this peer's PUB: " + pub.LastError();
        return false;
    }
    sub.SetReceiveQueue(opts.hwm);
    sub.Subscribe("");
    for (int j = 0; j < opts.players; ++j) {
        if (j != opts.id) sub.Connect(Connect(PeerPubPort(opts, j)));
    }
    heard[P(opts.id)] = heardMe[P(opts.id)] = true;
    const int64_t deadline = NowNs() + 60LL * 1000000000LL;
    while (NowNs() < deadline) {
        SetupIdle();
        if (std::all_of(heard.begin(), heard.end(), [](bool b) { return b; }) &&
            std::all_of(heardMe.begin(), heardMe.end(), [](bool b) { return b; })) {
            return true;
        }
    }
    err = "peers did not complete the subscription handshake";
    return false;
}

// Beacons until the warm-up release, so every peer can finish its handshake.
void Peer::SetupIdle() {
    if (NowNs() - lastBeacon > 2000000) {
        Frame beacon;
        beacon.topic[0] = 'P';
        beacon.topic[1] = static_cast<char>('0' + opts.id);
        beacon.type = MSG_SYNC;
        beacon.source = static_cast<uint16_t>(opts.id);
        beacon.run = RunTag();
        for (int j = 0; j < opts.players; ++j) {
            if (j != opts.id && heard[P(j)]) beacon.acks.push_back(Ack{StreamPeer(j), 1});
        }
        const std::string b = EncodeFrame(beacon);
        CountOut(Control("sync").out[PHASE_SETUP], b, 0);
        Publish(b, Control("sync").out[PHASE_SETUP], nullptr);
        lastBeacon = NowNs();
    }
    std::string message;
    while (Receive(1, message)) Handle(message);
}

void Peer::Publish(const std::string &message, Counters &c, std::string *err) {
    const int64_t t0 = NowNs();
    if (!pub.Send(message)) {
        ++c.sendFailures;
        if (err) *err = "publication failed";
    }
    T(&PhaseTimes::send) += NowNs() - t0;
}

void Peer::Handle(const std::string &message) {
    Frame f;
    const int64_t t0 = NowNs();
    const FrameError e = DecodeFrame(message, f);
    T(&PhaseTimes::frame) += NowNs() - t0;
    const uint8_t ph = e == FrameError::None && f.phase < 3 ? f.phase : current;
    if (e != FrameError::None || f.run != RunTag() || f.source >= opts.players || f.source == opts.id) {
        ++Control("rejected").in[ph].malformed;
        Note(std::string("rejected peer message: ") + (e != FrameError::None ? FrameErrorName(e) : "wrong run or source"));
        return;
    }
    const int j = f.source;
    for (const Ack &a : f.acks) {
        if (a.stream == StreamPeer(opts.id)) ackFrom[P(j)] = std::max<int64_t>(ackFrom[P(j)], a.block);
    }
    if (f.type == MSG_SYNC) {
        heard[P(j)] = true;
        for (const Ack &a : f.acks) if (a.stream == StreamPeer(opts.id)) heardMe[P(j)] = true;
        CountOut(Control("sync").in[ph], message, 0);
        return;
    }
    if (f.type == MSG_ACK) {
        CountOut(Control("ack").in[ph], message, 0);
        return;
    }
    if (f.type != MSG_DATA) return;
    if (f.stream == StreamCorrection(j)) {
        // Decode into a scratch world and compare with this peer's own
        // simulation of that player at the same block.
        InStream &s = Stream(in, f.stream);
        Counters &c = s.c[ph];
        ++c.frames;
        c.payloadBytes += f.payload.size();
        c.frameBytes += message.size();
        if (AdmitFrame(s, f, true) != Admit::Apply) return;
        const int64_t d0 = NowNs();
        World scratch;
        SnapshotDecoder decoder;
        ApplyOptions apply;
        apply.source = j;
        decoder.Apply(scratch, f.payload, apply);
        std::string line;
        scratch.Each<NetIdentity>([&](const GameObject &o, const NetIdentity &n) {
            if (n.owner == j && o.Has<InputState>()) line = CanonicalObject(o);
        });
        const int64_t dt = NowNs() - d0;
        c.decodeNs += static_cast<uint64_t>(dt);
        T(&PhaseTimes::decode) += dt;
        MarkApplied(s, f, true);
        CheckCorrection(j, f.block, line);
        return;
    }
    if (f.stream != StreamPeer(j)) return;
    if (opts.InputUp()) {
        inputs[P(j)].push_back(std::move(f));   // applied when that block is simulated
        return;
    }
    ApplyOptions apply;
    apply.source = j;
    apply.onlyOwner = j;
    if (ApplyData(down, world, Stream(in, StreamPeer(j)), f, apply, times[current])) {
        applied[P(j)] = f.block;
        if (CheckpointDue(f.block)) {
            const int64_t v0 = NowNs();
            Verify(oracle, f.block, world, [j](int owner) { return owner == j; }, {}, verify);
            T(&PhaseTimes::verify) += NowNs() - v0;
        }
    }
    if (opts.slowUs > 0) std::this_thread::sleep_for(std::chrono::microseconds(opts.slowUs));
}

void Peer::CheckCorrection(int sender, long long block, const std::string &line) {
    auto &h = history[P(sender)];
    auto it = h.find(block);
    if (it != h.end()) {
        ++correctionsChecked;
        if (it->second != line) {
            ++correctionsMismatched;
            Note("correction from peer " + std::to_string(sender) + " differs at block " + std::to_string(block));
        }
    } else if (block > simulated) {
        pendingCorrections[P(sender)][block] = line;
    } else {
        ++correctionsUnchecked;
    }
}

std::vector<Ack> Peer::Acks() const {
    std::vector<Ack> acks;
    for (int j = 0; j < opts.players; ++j) {
        if (j != opts.id) acks.push_back(Ack{StreamPeer(j), static_cast<uint32_t>(std::max<int64_t>(0, applied[P(j)]))});
    }
    return acks;
}

int64_t Peer::AckLag() const {
    int64_t lag = 0;
    for (int j = 0; j < opts.players; ++j) {
        if (j != opts.id) lag = std::max(lag, applied[P(j)] - acked[P(j)]);
    }
    return lag;
}

void Peer::SendAcks(uint8_t phase) {
    Frame f;
    f.topic[0] = 'P';
    f.topic[1] = static_cast<char>('0' + opts.id);
    f.type = MSG_ACK;
    f.phase = phase;
    f.source = static_cast<uint16_t>(opts.id);
    f.stream = StreamPeer(opts.id);
    f.run = RunTag();
    f.acks = Acks();
    const std::string m = EncodeFrame(f);
    CountOut(Control("ack").out[phase], m, 0);
    Publish(m, Control("ack").out[phase], nullptr);
    acked = applied;
    ++ackOnly;
}

int64_t Peer::Credit() const {
    int64_t c = INT64_MAX;
    for (int j = 0; j < opts.players; ++j) {
        if (j != opts.id) c = std::min(c, ackFrom[P(j)]);
    }
    return c;
}

bool Peer::ProduceState(long long b, uint8_t phase, std::string &err) {
    GameObject *me = world.Find(PLAYER_BASE + static_cast<ObjectId>(opts.id));
    if (!me) {
        err = "own player missing";
        return false;
    }
    const int64_t s0 = NowNs();
    for (long long t = 2 * b - 1; t <= 2 * b; ++t) {
        me->Get<InputState>()->buttons = scenario->Buttons(opts.id, b);
        scenario->Tick(world, t, OwnedBy(opts.id));
        if (opts.fixtures) scenario->Fixtures(world, t, opts.id);
        world.Flush();
        ++ticksSimulated[phase];
    }
    T(&PhaseTimes::sim) += NowNs() - s0;
    me->Get<NetIdentity>()->stampMicros = NowNs() / 1000;
    Frame f;
    f.topic[0] = 'P';
    f.topic[1] = static_cast<char>('0' + opts.id);
    f.source = static_cast<uint16_t>(opts.id);
    f.run = RunTag();
    f.acks = Acks();
    OutStream &o = out[StreamPeer(opts.id)];
    const std::string message = EncodeData(up, world, static_cast<uint32_t>(b), OwnedBy(opts.id), f, o, times[phase], phase);
    std::string sendError;
    Publish(message, o.c[phase], &sendError);
    if (!sendError.empty()) {
        err = sendError;
        return false;
    }
    acked = applied;
    ++blocksProduced[phase];
    if (b == opts.FinalBlock()) tProducedFinal = NowNs();
    if (opts.killAtBlock >= 0 && b == opts.killAtBlock) _exit(9);
    if (CheckpointDue(b)) {
        const int64_t v0 = NowNs();
        const int id = opts.id;
        Verify(oracle, b, world, [id](int owner) { return owner == id; },
               [id](const GameObject &o) { return OwnerOf(o) == id && o.Has<InputState>(); }, verify);
        T(&PhaseTimes::verify) += NowNs() - v0;
    }
    return true;
}

bool Peer::PublishInput(long long b, uint8_t phase, std::string &err) {
    GameObject *me = world.Find(PLAYER_BASE + static_cast<ObjectId>(opts.id));
    if (!me) {
        err = "own player missing";
        return false;
    }
    InputState &input = *me->Get<InputState>();
    const uint32_t keep = input.buttons;
    input.buttons = scenario->Buttons(opts.id, b);
    me->Get<NetIdentity>()->stampMicros = NowNs() / 1000;
    Frame f;
    f.topic[0] = 'P';
    f.topic[1] = static_cast<char>('0' + opts.id);
    f.source = static_cast<uint16_t>(opts.id);
    f.run = RunTag();
    f.acks = Acks();
    OutStream &o = out[StreamPeer(opts.id)];
    const std::string message = EncodeData(up, world, static_cast<uint32_t>(b), OwnedBy(opts.id), f, o, times[phase], phase);
    input.buttons = keep;   // the world's copy changes only when the block is simulated
    std::string sendError;
    Publish(message, o.c[phase], &sendError);
    if (!sendError.empty()) {
        err = sendError;
        return false;
    }
    acked = applied;
    ++blocksProduced[phase];
    if (opts.killAtBlock >= 0 && b == opts.killAtBlock) _exit(9);
    return true;
}

bool Peer::SimulateInputBlock(long long b, uint8_t phase, std::string &err) {
    for (int j = 0; j < opts.players; ++j) {
        if (j == opts.id) continue;
        Frame f = std::move(inputs[P(j)].front());
        inputs[P(j)].pop_front();
        if (f.block != static_cast<uint32_t>(b)) {
            err = "peer " + std::to_string(j) + " inputs out of block order";
            return false;
        }
        ApplyOptions apply;
        apply.source = j;
        apply.onlyOwner = j;
        if (!ApplyData(down, world, Stream(in, StreamPeer(j)), f, apply, times[phase])) {
            err = "inputs from peer " + std::to_string(j) + " were not applied at block " + std::to_string(b);
            return false;
        }
    }
    GameObject *me = world.Find(PLAYER_BASE + static_cast<ObjectId>(opts.id));
    const int64_t s0 = NowNs();
    const int self = opts.id;
    const ObjectFilter players = [self](const GameObject &o) { return OwnerOf(o) >= 0 && (OwnerOf(o) == self || o.Has<InputState>()); };
    for (long long t = 2 * b - 1; t <= 2 * b; ++t) {
        me->Get<InputState>()->buttons = scenario->Buttons(opts.id, b);
        scenario->Tick(world, t, players);
        world.Flush();
        ++ticksSimulated[phase];
    }
    T(&PhaseTimes::sim) += NowNs() - s0;
    simulated = b;
    for (int j = 0; j < opts.players; ++j) {
        if (j != opts.id) applied[P(j)] = b;
    }
    // Remember every player's state for the owners' corrections.
    world.Each<NetIdentity>([&](const GameObject &o, const NetIdentity &n) {
        if (n.owner < 0 || !o.Has<InputState>()) return;
        auto &h = history[P(n.owner)];
        h[b] = CanonicalObject(o);
        while (h.size() > 512) h.erase(h.begin());
        auto &pending = pendingCorrections[P(n.owner)];
        auto it = pending.find(b);
        if (it != pending.end()) {
            CheckCorrection(n.owner, b, it->second);
            pending.erase(it);
        }
    });
    if (b % opts.keyframe == 0) {
        Frame c;
        c.topic[0] = 'P';
        c.topic[1] = static_cast<char>('0' + opts.id);
        c.source = static_cast<uint16_t>(opts.id);
        c.run = RunTag();
        OutStream &o = out[StreamCorrection(opts.id)];
        const std::string message = EncodeData(correction, world, static_cast<uint32_t>(b), OwnedBy(opts.id), c, o, times[phase], phase);
        std::string sendError;
        Publish(message, o.c[phase], &sendError);
        if (!sendError.empty()) {
            err = sendError;
            return false;
        }
    }
    if (b == opts.FinalBlock()) tProducedFinal = NowNs();
    if (CheckpointDue(b)) {
        const int64_t v0 = NowNs();
        Verify(oracle, b, world, [](int) { return true; }, [](const GameObject &o) { return o.Has<InputState>(); }, verify);
        T(&PhaseTimes::verify) += NowNs() - v0;
    }
    return true;
}

bool Peer::RunPhase(long long from, long long to, uint8_t phase, std::string &err) {
    long long next = from + 1;
    const int64_t deadline = NowNs() + static_cast<int64_t>(opts.timeoutS) * 1000000000LL;
    const int window = opts.Window();
    std::string message;
    auto haveInputs = [&](long long b) {
        for (int j = 0; j < opts.players; ++j) {
            if (j != opts.id && (inputs[P(j)].empty() || inputs[P(j)].front().block > static_cast<uint32_t>(b))) return false;
        }
        return true;
    };
    while (true) {
        while (Receive(0, message)) Handle(message);
        const bool canProduce = next <= to && Credit() >= next - window;
        if (opts.InputUp()) {
            if (canProduce && next <= simulated + window + 1) {
                if (!PublishInput(next, phase, err)) return false;
                ++next;
                continue;
            }
            if (simulated < to && simulated + 1 < next && haveInputs(simulated + 1)) {
                if (!SimulateInputBlock(simulated + 1, phase, err)) return false;
                continue;
            }
        } else if (canProduce) {
            if (!ProduceState(next, phase, err)) return false;
            ++next;
            continue;
        }
        const int64_t lag = AckLag();
        if (lag > 0 && (next > to || lag >= opts.AckBatch())) {
            SendAcks(phase);
            continue;
        }
        bool consumed = true;
        for (int j = 0; j < opts.players; ++j) {
            if (j != opts.id && applied[P(j)] < to) consumed = false;
        }
        if (next > to && consumed && (!opts.InputUp() || simulated >= to)) return true;
        const int64_t w0 = NowNs();
        const bool got = Receive(100, message);
        T(&PhaseTimes::wait) += NowNs() - w0;
        if (got) {
            Handle(message);
            continue;
        }
        if (coord.Poll("ABORT_NEVER", 0) < 0) {
            err = "aborted by the coordinator: " + coord.abortReason;
            return false;
        }
        if (NowNs() > deadline) {
            err = "timed out at block " + std::to_string(next) + " (credit " + std::to_string(Credit()) + ")";
            return false;
        }
    }
}

void Peer::Service(int ms) {
    std::string message;
    while (Receive(ms, message)) {
        Handle(message);
        ms = 0;
    }
    if (AckLag() > 0) SendAcks(current);
}

} // namespace

int RunPeer(const Options &o) {
    Peer p(o);
    return p.Run();
}

} // namespace rb
