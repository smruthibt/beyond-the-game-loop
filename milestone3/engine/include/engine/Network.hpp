#pragma once

#include <memory>
#include <string>

namespace engine {
namespace net {

enum class SocketType {
    Request,    // REQ - the asking end of a synchronous pair
    Reply,      // REP - the answering end of a synchronous pair
    Publish,    // PUB - one-to-many broadcast
    Subscribe,  // SUB - the receiving end of a broadcast
};

// A deliberately thin wrapper over cppzmq. It standardises socket lifetime,
// timeouts and string framing for every game on this engine, and keeps
// the 0MQ headers out of everything that includes it. It is not trying to be
// a networking abstraction - the 0MQ socket types are still the vocabulary.
//
// Note what is absent: ROUTER and DEALER. Asynchrony in this engine is built
// out of one thread per peer holding a synchronous pair, not out of 0MQ's
// async socket types.
class Socket {
public:
    explicit Socket(SocketType type);
    ~Socket();

    Socket(Socket &&) noexcept;
    Socket &operator=(Socket &&) noexcept;
    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;

    // "tcp://*:5555" to listen; "tcp://localhost:5555" to reach out.
    //
    // Both return false instead of throwing. Bind fails for one reason in
    // practice - the port is already taken, usually by another instance of
    // the same program - and an uncaught 0MQ exception turns that into an
    // opaque "Abort trap" with no hint of which port or why. LastError()
    // says. Connect almost never fails: 0MQ connects lazily and is happy to
    // connect before the far end has bound.
    bool Bind(const std::string &endpoint);
    bool Connect(const std::string &endpoint);
    const std::string &LastError() const;

    // SUB sockets only, and required for them: a SUB that subscribes to
    // nothing receives nothing. An empty topic takes everything.
    void Subscribe(const std::string &topic = "");

    // Without a timeout, a blocking Receive() hangs forever if the peer dies
    // mid-exchange - which for a REQ socket also wedges the pair permanently,
    // since REQ/REP must strictly alternate.
    void SetReceiveTimeoutMs(int milliseconds);

    // How many messages may wait in this socket's queue (per peer) before
    // 0MQ starts dropping (PUB) or holding back (everything else). Set before
    // Bind/Connect. For world snapshots a short queue is right: a snapshot
    // that is already stale when it would be sent is worth nothing, and a long
    // queue of them only makes everything behind it late.
    void SetSendQueue(int messages);
    void SetReceiveQueue(int messages);

    // Neither throws. False from Send() is an error LastError() describes.
    bool Send(const std::string &payload);

    // False is usually a timeout; an error, if any, is in LastError().
    bool Receive(std::string &out);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace net
} // namespace engine
