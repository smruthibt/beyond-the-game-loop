// A client-server client.
//
//   native input  sends its buttons for each block; applies the server's world.
//   native state  simulates its own player from its buttons, sends that state;
//                 applies the server's world (its own player excepted).
//   relay         as native state, but the level is local and the server
//                 forwards every other player's state as it arrives.
//
// Upstream is a synchronous REQ to this client's own REP socket on the server,
// as in the engine's Client. Downstream is a SUB on the server's PUB.
#include "Endpoint.hpp"

#include <unistd.h>

#include <algorithm>
#include <thread>

namespace rb {

using namespace engine;

namespace {

class Client final : public Endpoint {
public:
    explicit Client(const Options &o)
        : Endpoint(o, "client" + std::to_string(o.id)),
          up(o.InputUp() ? SnapshotOptions(o.encoding, o.upDelta, o.keyframe, {InputState::TYPE})
                         : SnapshotOptions(o.encoding, o.upDelta, o.keyframe)) {}

protected:
    bool Setup(std::string &err) override;
    bool RunPhase(long long from, long long to, uint8_t phase, std::string &err) override;
    void Service(int ms) override;
    void VerifyAt(long long block) override {
        const bool simulates = !opts.InputUp();
        const int me = opts.id;
        Verify(oracle, block, world, [](int) { return true; },
               [simulates, me](const GameObject &o) { return simulates && OwnerOf(o) == me && o.Has<InputState>(); },
               verify);
    }
    std::string RoleJson() const override {
        Json j;
        j.Int("ack_only_frames", ackOnly).Int("own_player", ownId);
        return j.Text();
    }

private:
    bool Receive(int timeoutMs, std::string &message);
    void Handle(const std::string &message);
    bool Produce(long long block, uint8_t phase, std::string &err);
    bool SendUp(Frame &f, const std::string &message, Counters *dataCounters, std::string &err);
    std::vector<Ack> Acks() const;
    int64_t AckLag() const;   // largest applied-but-unacknowledged progress over consumed streams
    bool Allowed(long long block) const {
        return (opts.Native() ? appliedWorld : credit) >= block - opts.Window();
    }
    bool Consumed(long long to) const {
        if (opts.Native()) return appliedWorld >= to;
        for (int j = 0; j < opts.players; ++j) {
            if (j != opts.id && appliedForward[static_cast<size_t>(j)] < to) return false;
        }
        return true;
    }

    net::Socket req{net::SocketType::Request};
    net::Socket sub{net::SocketType::Subscribe};
    int subTimeout = -2;
    SnapshotEncoder up;
    SnapshotDecoder down;
    ObjectId ownId = NO_OBJECT;
    int64_t appliedWorld = -1;
    std::vector<int64_t> appliedForward;
    std::vector<int64_t> ackedForward;    // what our last upstream message reported
    int64_t ackedWorld = -1;
    int64_t credit = 0;
    uint32_t upSeqData = 0;
    long long ackOnly = 0;
};

bool Client::Receive(int timeoutMs, std::string &message) {
    if (timeoutMs != subTimeout) {
        sub.SetReceiveTimeoutMs(timeoutMs);
        subTimeout = timeoutMs;
    }
    return sub.Receive(message);
}

bool Client::Setup(std::string &err) {
    if (!opts.Native()) {
        scenario->BuildLevel(world, false);
        scenario->AddPlayers(world);
    }
    appliedForward.assign(static_cast<size_t>(opts.players), -1);
    ackedForward.assign(static_cast<size_t>(opts.players), -1);
    Stream(out, StreamUp(opts.id));

    sub.SetReceiveQueue(opts.hwm);
    sub.Subscribe("S_");
    if (opts.Native()) {
        sub.Subscribe("W_");
    } else {
        sub.Subscribe("C_");
        for (int j = 0; j < opts.players; ++j) {
            if (j != opts.id) sub.Subscribe(std::string("F") + static_cast<char>('0' + j));
        }
    }
    sub.Connect(Connect(ServerPubPort(opts)));
    req.SetSendQueue(opts.hwm);
    req.SetReceiveQueue(opts.hwm);
    req.SetReceiveTimeoutMs(30000);
    req.Connect(Connect(ServerRepPort(opts, opts.id)));

    // Subscription proof, then the bootstrap world.
    const int64_t deadline = NowNs() + 60LL * 1000000000LL;
    std::string message;
    bool heard = false;
    while (!heard && NowNs() < deadline) {
        if (!Receive(100, message)) continue;
        Frame f;
        if (DecodeFrame(message, f) == FrameError::None && f.type == MSG_SYNC && f.run == RunTag()) heard = true;
        CountOut(Control("sync").in[PHASE_SETUP], message, 0);
    }
    if (!heard) {
        err = "no subscription beacon from the server";
        return false;
    }
    Frame hello;
    hello.topic[0] = 'U';
    hello.topic[1] = static_cast<char>('0' + opts.id);
    hello.type = MSG_SYNC;
    hello.source = static_cast<uint16_t>(opts.id);
    hello.stream = StreamUp(opts.id);
    hello.run = RunTag();
    const std::string h = EncodeFrame(hello);
    CountOut(Control("sync").out[PHASE_SETUP], h, 0);
    std::string reply;
    if (!req.Send(h) || !req.Receive(reply)) {
        err = "the server did not answer the subscription report";
        return false;
    }
    CountOut(Control("reply").in[PHASE_SETUP], reply, 0);

    if (opts.Native()) {
        while (appliedWorld < 0 && NowNs() < deadline) {
            if (Receive(100, message)) Handle(message);
        }
        if (appliedWorld < 0) {
            err = "no bootstrap world from the server";
            return false;
        }
    }
    world.Each<NetIdentity>([&](GameObject &o, NetIdentity &n) {
        if (n.owner == opts.id && o.Has<InputState>() && ownId == NO_OBJECT) ownId = o.Id();
    });
    if (ownId == NO_OBJECT) {
        err = "this client's player is not in the world";
        return false;
    }
    return true;
}

void Client::Handle(const std::string &message) {
    Frame f;
    const int64_t t0 = NowNs();
    const FrameError e = DecodeFrame(message, f);
    T(&PhaseTimes::frame) += NowNs() - t0;
    const uint8_t ph = e == FrameError::None && f.phase < 3 ? f.phase : current;
    if (e != FrameError::None || f.run != RunTag()) {
        ++Control("rejected").in[ph].malformed;
        Note(std::string("rejected downstream message: ") + (e != FrameError::None ? FrameErrorName(e) : "wrong run"));
        return;
    }
    if (f.type == MSG_SYNC) {
        CountOut(Control("sync").in[ph], message, 0);
        return;
    }
    if (f.type == MSG_CREDIT) {
        CountOut(Control("credit").in[ph], message, 0);
        for (const Ack &a : f.acks) {
            if (a.stream == StreamUp(opts.id)) credit = std::max<int64_t>(credit, a.block);
        }
        return;
    }
    if (f.type != MSG_DATA) return;
    if (opts.Native() && f.stream == STREAM_WORLD) {
        ApplyOptions apply;
        apply.source = -1;
        apply.skipOwner = opts.InputUp() ? ANY_OWNER : opts.id;   // sending state: my player is mine
        if (ApplyData(down, world, Stream(in, STREAM_WORLD), f, apply, times[current])) {
            appliedWorld = f.block;
            if (f.block > 0 && CheckpointDue(f.block)) {
                const int64_t v0 = NowNs();
                const int me = opts.id;
                const bool input = opts.InputUp();
                Verify(oracle, f.block, world, [me, input](int owner) { return input || owner != me; }, {}, verify);
                T(&PhaseTimes::verify) += NowNs() - v0;
            }
        }
    } else if (!opts.Native()) {
        for (int j = 0; j < opts.players; ++j) {
            if (j == opts.id || f.stream != StreamForward(j)) continue;
            ApplyOptions apply;
            apply.source = j;
            apply.onlyOwner = j;
            if (ApplyData(down, world, Stream(in, StreamForward(j)), f, apply, times[current])) {
                appliedForward[static_cast<size_t>(j)] = f.block;
                if (CheckpointDue(f.block)) {
                    const int64_t v0 = NowNs();
                    Verify(oracle, f.block, world, [j](int owner) { return owner == j; }, {}, verify);
                    T(&PhaseTimes::verify) += NowNs() - v0;
                }
            }
        }
    }
    if (opts.slowUs > 0) std::this_thread::sleep_for(std::chrono::microseconds(opts.slowUs));
}

std::vector<Ack> Client::Acks() const {
    std::vector<Ack> acks;
    if (opts.Native()) {
        acks.push_back(Ack{STREAM_WORLD, static_cast<uint32_t>(std::max<int64_t>(0, appliedWorld))});
    } else {
        for (int j = 0; j < opts.players; ++j) {
            if (j != opts.id) acks.push_back(Ack{StreamForward(j), static_cast<uint32_t>(std::max<int64_t>(0, appliedForward[static_cast<size_t>(j)]))});
        }
    }
    return acks;
}

int64_t Client::AckLag() const {
    if (opts.Native()) return appliedWorld - ackedWorld;
    int64_t lag = 0;
    for (int j = 0; j < opts.players; ++j) {
        if (j != opts.id) lag = std::max(lag, appliedForward[static_cast<size_t>(j)] - ackedForward[static_cast<size_t>(j)]);
    }
    return lag;
}

bool Client::SendUp(Frame &f, const std::string &message, Counters *dataCounters, std::string &err) {
    const int64_t t0 = NowNs();
    std::string reply;
    const bool sent = req.Send(message);
    const bool answered = sent && req.Receive(reply);
    T(&PhaseTimes::send) += NowNs() - t0;
    if (!sent) {
        if (dataCounters) ++dataCounters->sendFailures;
        err = "upstream send failed";
        return false;
    }
    if (!answered) {
        err = "the server did not acknowledge an upstream message";
        return false;
    }
    CountOut(Control("reply").in[f.phase], reply, 0);
    // Acknowledged state is now what the server knows.
    ackedWorld = appliedWorld;
    ackedForward = appliedForward;
    return true;
}

bool Client::Produce(long long b, uint8_t phase, std::string &err) {
    GameObject *me = world.Find(ownId);
    if (!me) {
        err = "own player missing";
        return false;
    }
    InputState &input = *me->Get<InputState>();
    const uint32_t buttons = scenario->Buttons(opts.id, b);
    uint32_t keep = input.buttons;
    if (!opts.InputUp()) {
        const int64_t s0 = NowNs();
        for (long long t = 2 * b - 1; t <= 2 * b; ++t) {
            input.buttons = buttons;
            scenario->Tick(world, t, OwnedBy(opts.id));
            if (opts.fixtures && !opts.Native()) scenario->Fixtures(world, t, opts.id);
            world.Flush();
            ++ticksSimulated[phase];
        }
        T(&PhaseTimes::sim) += NowNs() - s0;
        keep = buttons;
    }
    input.buttons = buttons;
    me->Get<NetIdentity>()->stampMicros = NowNs() / 1000;
    Frame f;
    f.topic[0] = 'U';
    f.topic[1] = static_cast<char>('0' + opts.id);
    f.source = static_cast<uint16_t>(opts.id);
    f.run = RunTag();
    f.acks = Acks();
    OutStream &o = out[StreamUp(opts.id)];
    const std::string message = EncodeData(up, world, static_cast<uint32_t>(b), OwnedBy(opts.id), f, o, times[phase], phase);
    input.buttons = keep;   // sending inputs: the world keeps the server's copy
    if (!SendUp(f, message, &o.c[phase], err)) return false;
    ++upSeqData;
    ++blocksProduced[phase];
    if (b == opts.FinalBlock()) tProducedFinal = NowNs();
    if (opts.killAtBlock >= 0 && b == opts.killAtBlock) _exit(9);   // fault fixture: vanish mid-run
    if (!opts.InputUp() && CheckpointDue(b)) {
        const int64_t v0 = NowNs();
        const int id = opts.id;
        Verify(oracle, b, world, [id](int owner) { return owner == id; },
               [id](const GameObject &o) { return OwnerOf(o) == id && o.Has<InputState>(); }, verify);
        T(&PhaseTimes::verify) += NowNs() - v0;
    }
    return true;
}

bool Client::RunPhase(long long from, long long to, uint8_t phase, std::string &err) {
    long long next = from + 1;
    const int64_t deadline = NowNs() + static_cast<int64_t>(opts.timeoutS) * 1000000000LL;
    std::string message;
    while (true) {
        while (Receive(0, message)) Handle(message);
        if (next <= to && Allowed(next)) {
            if (!Produce(next, phase, err)) return false;
            ++next;
            continue;
        }
        // Acknowledgements that cannot ride on an update: when there is
        // nothing left to send, or (relay, where the other clients' progress
        // depends on them) once half a window of progress is unacknowledged.
        const bool finished = next > to;
        const int64_t lag = AckLag();
        if (lag > 0 && (finished || (!opts.Native() && lag >= opts.AckBatch()))) {
            Frame f;
            f.topic[0] = 'U';
            f.topic[1] = static_cast<char>('0' + opts.id);
            f.type = MSG_ACK;
            f.phase = phase;
            f.source = static_cast<uint16_t>(opts.id);
            f.stream = StreamUp(opts.id);
            f.run = RunTag();
            f.acks = Acks();
            const std::string m = EncodeFrame(f);
            CountOut(Control("ack").out[phase], m, 0);
            ++ackOnly;
            if (!SendUp(f, m, nullptr, err)) return false;
            continue;
        }
        if (finished && Consumed(to)) return true;
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
            err = "timed out at block " + std::to_string(next) + " (world " + std::to_string(appliedWorld) + ", credit " +
                  std::to_string(credit) + ")";
            return false;
        }
    }
}

void Client::Service(int ms) {
    std::string message;
    while (Receive(ms, message)) {
        Handle(message);
        ms = 0;
    }
    if (!opts.Native() && AckLag() > 0) {
        Frame f;
        f.topic[0] = 'U';
        f.topic[1] = static_cast<char>('0' + opts.id);
        f.type = MSG_ACK;
        f.phase = current;
        f.source = static_cast<uint16_t>(opts.id);
        f.stream = StreamUp(opts.id);
        f.run = RunTag();
        f.acks = Acks();
        const std::string m = EncodeFrame(f);
        CountOut(Control("ack").out[current], m, 0);
        ++ackOnly;
        std::string ignored;
        SendUp(f, m, nullptr, ignored);
    }
}

} // namespace

int RunClient(const Options &o) {
    Client c(o);
    return c.Run();
}

} // namespace rb
