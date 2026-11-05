// The client-server server, in two modes.
//
//   native  The server owns the level and replicates it: every block it applies
//           each client's update (inputs, or the client's simulated player
//           state), simulates what it owns, and publishes one world snapshot.
//   relay   The level is built locally everywhere (controlled topology
//           comparison). Each client's player state is applied and forwarded
//           to the other clients as soon as it arrives, with its source block.
//
// Upstream uses the engine's pattern: one REP socket and thread per client
// slot, each a synchronous pair with that client's REQ. Downstream is one PUB.
#include "Endpoint.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace rb {

using namespace engine;

namespace {

constexpr uint16_t SERVER_SOURCE = 0xFFFF;

class Server final : public Endpoint {
public:
    explicit Server(const Options &o) : Endpoint(o, "server") {}

protected:
    bool Setup(std::string &err) override;
    bool RunPhase(long long from, long long to, uint8_t phase, std::string &err) override;
    void Service(int ms) override {
        if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
    }
    void Shutdown() override {
        running = false;
        for (std::thread &t : threads) if (t.joinable()) t.join();
    }
    void VerifyAt(long long block) override {
        const bool simulatesPlayers = opts.Native() && opts.InputUp();
        Verify(oracle, block, world, [](int) { return true; },
               [simulatesPlayers](const GameObject &o) { return simulatesPlayers && o.Has<InputState>(); }, verify);
    }
    std::string RoleJson() const override {
        Json j;
        j.Int("credit_frames", creditFrames).Int("max_queued", maxQueued).Int("rep_rejected", repRejected);
        return j.Text();
    }

private:
    struct Queued {
        int client = 0;
        Frame frame;
    };
    struct Slot {
        std::deque<Frame> data;               // native: this client's updates, in order
        int64_t ackWorld = -1;                // native: the latest world block it applied
        std::vector<int64_t> ackForward;      // relay: per source, the latest forward it applied
        bool synced = false;
    };

    void RepLoop(int client);
    bool NativePhase(long long from, long long to, uint8_t phase, std::string &err);
    bool RelayPhase(long long from, long long to, uint8_t phase, std::string &err);
    bool WaitFor(std::unique_lock<std::mutex> &lock, const std::function<bool()> &ready, std::string &err);
    void PublishCredits(uint8_t phase, long long to);
    void Publish(const std::string &message, Counters &c);

    net::Socket pub{net::SocketType::Publish};
    std::vector<std::thread> threads;
    std::atomic<bool> running{true};
    std::atomic<int> bound{0};
    std::atomic<bool> bindFailed{false};

    std::mutex m;
    std::condition_variable cv;
    std::vector<Slot> slots;
    std::deque<Queued> arrivals;              // relay: every client's updates, in arrival order
    bool acksChanged = false;

    std::unique_ptr<SnapshotEncoder> worldEncoder;
    std::vector<std::unique_ptr<SnapshotEncoder>> forwardEncoders;
    SnapshotDecoder upstream;
    std::vector<int64_t> appliedUp;           // relay: per client, the latest block applied and forwarded
    std::vector<int64_t> credits;             // relay: credits last published
    long long creditFrames = 0, maxQueued = 0, repRejected = 0;
};

void Server::RepLoop(int client) {
    net::Socket rep(net::SocketType::Reply);
    rep.SetReceiveQueue(opts.hwm);
    rep.SetSendQueue(opts.hwm);
    rep.SetReceiveTimeoutMs(50);
    if (!rep.Bind(Bind(ServerRepPort(opts, client)))) {
        bindFailed = true;
        ++bound;
        return;
    }
    ++bound;
    std::string message;
    while (running) {
        if (!rep.Receive(message)) continue;
        Frame f;
        const FrameError e = DecodeFrame(message, f);
        Frame reply;
        reply.topic[0] = 'K';
        reply.topic[1] = '_';
        reply.type = MSG_REPLY;
        reply.source = SERVER_SOURCE;
        reply.run = RunTag();
        std::string answer;
        {
            std::lock_guard<std::mutex> lock(m);
            const uint8_t ph = e == FrameError::None && f.phase < 3 ? f.phase : current;
            if (e != FrameError::None || f.run != RunTag() || f.source != client) {
                ++repRejected;
                ++Control("rejected").in[ph].malformed;
                Note("rejected upstream message from client " + std::to_string(client) + ": " +
                     (e != FrameError::None ? FrameErrorName(e) : "wrong run or source"));
            } else {
                reply.phase = f.phase;
                reply.stream = f.stream;
                reply.seq = f.seq;
                reply.block = f.block;
                Slot &s = slots[static_cast<size_t>(client)];
                for (const Ack &a : f.acks) {
                    if (a.stream == STREAM_WORLD) s.ackWorld = std::max<int64_t>(s.ackWorld, a.block);
                    for (int j = 0; j < opts.players; ++j) {
                        if (a.stream == StreamForward(j)) s.ackForward[static_cast<size_t>(j)] =
                            std::max<int64_t>(s.ackForward[static_cast<size_t>(j)], a.block);
                    }
                }
                if (!f.acks.empty()) acksChanged = true;
                if (f.type == MSG_SYNC) {
                    s.synced = true;
                    CountOut(Control("sync").in[ph], message, 0);
                } else if (f.type == MSG_ACK) {
                    CountOut(Control("ack").in[ph], message, 0);
                } else if (f.type == MSG_DATA) {
                    if (opts.Native()) s.data.push_back(f);
                    else arrivals.push_back(Queued{client, f});
                    maxQueued = std::max<long long>(maxQueued, static_cast<long long>(opts.Native() ? s.data.size() : arrivals.size()));
                }
            }
            answer = EncodeFrame(reply);
            CountOut(Control("reply").out[ph], answer, 0);
        }
        cv.notify_all();
        rep.Send(answer);
    }
}

bool Server::Setup(std::string &err) {
    scenario->BuildLevel(world, opts.Native());
    scenario->AddPlayers(world);
    slots.resize(static_cast<size_t>(opts.players));
    for (Slot &s : slots) s.ackForward.assign(static_cast<size_t>(opts.players), -1);
    appliedUp.assign(static_cast<size_t>(opts.players), 0);
    credits.assign(static_cast<size_t>(opts.players), -1);
    Stream(out, STREAM_WORLD);

    pub.SetSendQueue(opts.hwm);
    if (!pub.Bind(Bind(ServerPubPort(opts)))) {
        err = "cannot bind the world PUB: " + pub.LastError();
        return false;
    }
    for (int i = 0; i < opts.players; ++i) threads.emplace_back(&Server::RepLoop, this, i);
    while (bound < opts.players) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (bindFailed) {
        err = "cannot bind a client REP port";
        return false;
    }

    // Subscription proof: beacon until every client has heard one and said so.
    const int64_t deadline = NowNs() + 60LL * 1000000000LL;
    while (true) {
        {
            std::lock_guard<std::mutex> lock(m);
            if (std::all_of(slots.begin(), slots.end(), [](const Slot &s) { return s.synced; })) break;
        }
        if (NowNs() > deadline) {
            err = "clients did not complete the subscription handshake";
            return false;
        }
        Frame beacon;
        beacon.topic[0] = 'S';
        beacon.topic[1] = '_';
        beacon.type = MSG_SYNC;
        beacon.source = SERVER_SOURCE;
        beacon.run = RunTag();
        const std::string b = EncodeFrame(beacon);
        std::lock_guard<std::mutex> lock(m);
        Publish(b, Control("sync").out[PHASE_SETUP]);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    const Encoding enc = opts.encoding;
    if (opts.Native()) {
        worldEncoder = std::make_unique<SnapshotEncoder>(SnapshotOptions(enc, opts.downDelta, opts.keyframe));
        // Bootstrap: the complete world, block 0. A delta stream's first
        // snapshot is always a keyframe.
        Frame f;
        f.topic[0] = 'W';
        f.topic[1] = '_';
        f.source = SERVER_SOURCE;
        f.run = RunTag();
        const std::string message = EncodeData(*worldEncoder, world, 0, {}, f, out[STREAM_WORLD], times[PHASE_SETUP], PHASE_SETUP);
        if (!pub.Send(message)) ++out[STREAM_WORLD].c[PHASE_SETUP].sendFailures;
    } else {
        for (int i = 0; i < opts.players; ++i) {
            forwardEncoders.push_back(std::make_unique<SnapshotEncoder>(SnapshotOptions(enc, opts.downDelta, opts.keyframe)));
            Stream(out, StreamForward(i));
        }
    }
    return true;
}

void Server::Publish(const std::string &message, Counters &c) {
    if (!pub.Send(message)) ++c.sendFailures;
    CountOut(c, message, 0);
}

bool Server::WaitFor(std::unique_lock<std::mutex> &lock, const std::function<bool()> &ready, std::string &err) {
    if (ready()) return true;
    const int64_t t0 = NowNs();
    const int64_t deadline = t0 + static_cast<int64_t>(opts.timeoutS) * 1000000000LL;
    while (!ready()) {
        cv.wait_for(lock, std::chrono::milliseconds(100));
        if (ready()) break;
        lock.unlock();
        const int aborted = coord.Poll("ABORT_NEVER", 0);
        lock.lock();
        if (aborted < 0) {
            err = "aborted by the coordinator: " + coord.abortReason;
            T(&PhaseTimes::wait) += NowNs() - t0;
            return false;
        }
        if (NowNs() > deadline) {
            err = "timed out waiting for clients";
            T(&PhaseTimes::wait) += NowNs() - t0;
            return false;
        }
    }
    T(&PhaseTimes::wait) += NowNs() - t0;
    return true;
}

bool Server::RunPhase(long long from, long long to, uint8_t phase, std::string &err) {
    return opts.Native() ? NativePhase(from, to, phase, err) : RelayPhase(from, to, phase, err);
}

bool Server::NativePhase(long long from, long long to, uint8_t phase, std::string &err) {
    const bool input = opts.InputUp();
    const ObjectFilter authority = input ? ObjectFilter{} : OwnedBy(-1);
    const int window = opts.Window();
    std::vector<Frame> updates(static_cast<size_t>(opts.players));
    OutStream &worldOut = out[STREAM_WORLD];

    auto haveUpdates = [&](long long b, bool &protocolError) {
        for (const Slot &s : slots) {
            if (s.data.empty()) return false;
            if (s.data.front().block != static_cast<uint32_t>(b)) protocolError = true;
        }
        return true;
    };
    auto windowOpen = [&](long long b) {
        for (const Slot &s : slots) if (s.ackWorld < b - window) return false;
        return true;
    };
    auto take = [&](std::unique_lock<std::mutex> &lock, long long b) -> bool {
        bool protocolError = false;
        if (!WaitFor(lock, [&] { return haveUpdates(b, protocolError) || protocolError; }, err)) return false;
        if (protocolError) {
            err = "an upstream update arrived out of block order";
            return false;
        }
        for (size_t i = 0; i < slots.size(); ++i) {
            updates[i] = std::move(slots[i].data.front());
            slots[i].data.pop_front();
        }
        return true;
    };
    auto applyUpdates = [&]() {
        for (int i = 0; i < opts.players; ++i) {
            ApplyOptions apply;
            apply.source = i;
            apply.onlyOwner = i;
            apply.createMissing = false;
            const Frame &f = updates[static_cast<size_t>(i)];
            if (!ApplyData(upstream, world, Stream(in, StreamUp(i)), f, apply, times[phase])) {
                Note("upstream update not applied: client " + std::to_string(i) + " block " + std::to_string(f.block));
            }
        }
    };

    for (long long b = from + 1; b <= to; ++b) {
        {
            std::unique_lock<std::mutex> lock(m);
            if (!WaitFor(lock, [&] { return windowOpen(b); }, err)) return false;
            if (input && !take(lock, b)) return false;
        }
        if (input) applyUpdates();
        const int64_t s0 = NowNs();
        for (long long t = 2 * b - 1; t <= 2 * b; ++t) {
            scenario->Tick(world, t, authority);
            if (opts.fixtures) scenario->Fixtures(world, t, -1);
            world.Flush();
            ++ticksSimulated[phase];
        }
        scenario->Density(world, b);
        T(&PhaseTimes::sim) += NowNs() - s0;
        if (!input) {
            std::unique_lock<std::mutex> lock(m);
            if (!take(lock, b)) return false;
        }
        if (!input) applyUpdates();

        Frame f;
        f.topic[0] = 'W';
        f.topic[1] = '_';
        f.source = SERVER_SOURCE;
        f.run = RunTag();
        const std::string message = EncodeData(*worldEncoder, world, static_cast<uint32_t>(b), {}, f, worldOut, times[phase], phase);
        const int64_t p0 = NowNs();
        if (!pub.Send(message)) {
            ++worldOut.c[phase].sendFailures;
            err = "world publication failed at block " + std::to_string(b);
            return false;
        }
        T(&PhaseTimes::send) += NowNs() - p0;
        ++blocksProduced[phase];
        if (b == opts.FinalBlock()) tProducedFinal = NowNs();
        if (CheckpointDue(b)) {
            const int64_t v0 = NowNs();
            VerifyAt(b);
            T(&PhaseTimes::verify) += NowNs() - v0;
        }
    }
    return true;
}

void Server::PublishCredits(uint8_t phase, long long to) {
    bool changed = false;
    std::vector<int64_t> next(credits.size());
    for (int i = 0; i < opts.players; ++i) {
        int64_t c = appliedUp[static_cast<size_t>(i)];
        for (int j = 0; j < opts.players; ++j) {
            if (j != i) c = std::min(c, slots[static_cast<size_t>(j)].ackForward[static_cast<size_t>(i)]);
        }
        next[static_cast<size_t>(i)] = c;
        // Batched like acknowledgements: republish once a credit has
        // advanced by half the window, or reached the end of the phase.
        const int64_t last = credits[static_cast<size_t>(i)];
        if (last < 0 || c - last >= opts.AckBatch() || (c >= to && last < to)) changed = true;
    }
    if (!changed) return;
    credits = next;
    Frame f;
    f.topic[0] = 'C';
    f.topic[1] = '_';
    f.type = MSG_CREDIT;
    f.phase = phase;
    f.source = SERVER_SOURCE;
    f.run = RunTag();
    for (int i = 0; i < opts.players; ++i) {
        f.acks.push_back(Ack{StreamUp(i), static_cast<uint32_t>(std::max<int64_t>(0, credits[static_cast<size_t>(i)]))});
    }
    Publish(EncodeFrame(f), Control("credit").out[phase]);
    ++creditFrames;
}

bool Server::RelayPhase(long long, long long to, uint8_t phase, std::string &err) {
    auto finished = [&]() {
        for (int64_t a : appliedUp) if (a < to) return false;
        return true;
    };
    std::deque<Queued> batch;
    while (!finished()) {
        {
            std::unique_lock<std::mutex> lock(m);
            if (!WaitFor(lock, [&] { return !arrivals.empty() || acksChanged; }, err)) return false;
            batch.swap(arrivals);
            acksChanged = false;
        }
        for (Queued &q : batch) {
            const int i = q.client;
            const Frame &f = q.frame;
            if (f.stream != StreamUp(i) || f.block != appliedUp[static_cast<size_t>(i)] + 1) {
                err = "relay: client " + std::to_string(i) + " sent block " + std::to_string(f.block) + " out of order";
                return false;
            }
            // The relay is owner-authoritative, as P2P is: a client's own new
            // objects are accepted and forwarded. (Native mode keeps the
            // engine's rule that clients cannot create objects on the server.)
            ApplyOptions apply;
            apply.source = i;
            apply.onlyOwner = i;
            apply.createMissing = true;
            if (!ApplyData(upstream, world, Stream(in, StreamUp(i)), f, apply, times[phase])) {
                err = "relay: an update from client " + std::to_string(i) + " was not applied";
                return false;
            }
            appliedUp[static_cast<size_t>(i)] = f.block;
            Frame fwd;
            fwd.topic[0] = 'F';
            fwd.topic[1] = static_cast<char>('0' + i);
            fwd.source = static_cast<uint16_t>(i);
            fwd.run = RunTag();
            const std::string message = EncodeData(*forwardEncoders[static_cast<size_t>(i)], world, f.block, OwnedBy(i), fwd,
                                                   out[StreamForward(i)], times[phase], phase);
            const int64_t p0 = NowNs();
            if (!pub.Send(message)) {
                ++out[StreamForward(i)].c[phase].sendFailures;
                err = "relay: forward publication failed";
                return false;
            }
            T(&PhaseTimes::send) += NowNs() - p0;
            ++blocksProduced[phase];
            if (CheckpointDue(f.block)) {
                const int64_t v0 = NowNs();
                Verify(oracle, f.block, world, [i](int owner) { return owner == i; }, {}, verify);
                T(&PhaseTimes::verify) += NowNs() - v0;
            }
        }
        batch.clear();
        std::lock_guard<std::mutex> lock(m);
        PublishCredits(phase, to);
    }
    if (to == opts.FinalBlock()) tProducedFinal = NowNs();
    return true;
}

} // namespace

int RunServer(const Options &o) {
    Server s(o);
    return s.Run();
}

} // namespace rb
