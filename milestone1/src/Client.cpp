#include "engine/Client.hpp"

#include <chrono>
#include <sstream>

namespace engine {

namespace {

// Closing a window must not hang on a server that has already gone.
constexpr int LEAVE_TIMEOUT_MS = 250;

long long NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

Client::Client(Config cfg) : config(cfg) {
    channel.SetReceiveTimeoutMs(config.timeoutMs);
    world.SetReceiveTimeoutMs(0);   // a poll, so Tick never blocks on the broadcast
    world.Subscribe(config.worldTopic);
    windowStartMs = NowMs();
}

Client::~Client() {
    Leave();
}

void Client::Leave() {
    if (id < 0 || left) return;
    left = true;
    channel.SetReceiveTimeoutMs(LEAVE_TIMEOUT_MS);
    std::string reply;
    if (channel.Send("BYE")) channel.Receive(reply);
}

bool Client::Join() {
    net::Socket join(net::SocketType::Request);
    join.SetReceiveTimeoutMs(config.timeoutMs);
    if (!join.Connect("tcp://localhost:" + std::to_string(config.joinPort))) {
        lastError = join.LastError();
        return false;
    }
    join.Send("JOIN");

    std::string welcome;
    if (!join.Receive(welcome)) {
        lastError = "no answer from the server on port " + std::to_string(config.joinPort) + " - is it running?";
        return false;
    }
    if (welcome.rfind("WELCOME", 0) != 0) {
        lastError = "server refused: " + welcome;
        return false;
    }

    std::istringstream in(welcome);
    std::string verb;
    if (!(in >> verb >> id >> channelPort >> maxClients)) {
        lastError = "malformed WELCOME: " + welcome;
        return false;
    }

    if (!channel.Connect("tcp://localhost:" + std::to_string(channelPort))) {
        lastError = channel.LastError();
        return false;
    }
    if (!world.Connect("tcp://localhost:" + std::to_string(config.worldPort))) {
        lastError = world.LastError();
        return false;
    }
    return true;
}

bool Client::Request(const std::string &request, std::string &reply) {
    if (!channel.Send(request)) return false;
    return channel.Receive(reply);
}

bool Client::Tick(float dt,
                  const std::function<void(const std::string &)> &onWorld,
                  const std::function<std::string()> &encodeState,
                  const std::function<void(const std::string &)> &onReply) {
    // 1. The world, downstream. Always - even while the game is paused. Drain
    //    everything queued so a client that fell behind catches up rather than
    //    rendering the world further and further in the past.
    std::string message;
    while (world.Receive(message)) {
        // Hearing the world is proof of connection too. Without this a paused
        // client - which by design sends nothing - would report itself
        // disconnected while it is demonstrably receiving.
        connected = true;
        if (onWorld) onWorld(message);
    }

    // 2. Upstream, paced by game time. Subtract rather than zero so the
    //    remainder carries over and the long-run rate is exactly 1/interval;
    //    then cap so a stall cannot bank several sends that all fire at once.
    bool sent = false;
    sendAccumulator += dt;
    // A capped backlog must not produce a final scheduled send while paused.
    if (dt > 0.0f && sendAccumulator >= config.sendIntervalSeconds) {
        sendAccumulator -= config.sendIntervalSeconds;
        if (sendAccumulator > config.sendIntervalSeconds) sendAccumulator = config.sendIntervalSeconds;

        if (encodeState) {
            const std::string payload = encodeState();
            std::string ack;
            const bool out = channel.Send(payload);
            if (out && channel.Receive(ack)) {
                ++roundTrips;
                connected = true;
                if (onReply) onReply(ack);
            } else {
                connected = false;
            }
            sent = true;
            if (out) ++sendsThisWindow;   // a refused send is not a message on the wire
        }
    }

    // 3. Once a WALL second, publish the measured send rate. Wall time on
    //    purpose: the point is to show game-time pacing moving a real rate.
    const long long now = NowMs();
    if (now - windowStartMs >= 1000) {
        sendsPerSecond = sendsThisWindow;
        sendsThisWindow = 0;
        windowStartMs = now;
    }
    return sent;
}

} // namespace engine
