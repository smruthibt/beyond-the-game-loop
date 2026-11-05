#include "engine/Network.hpp"

#include <zmq.hpp>

namespace engine {
namespace net {

namespace {

// One context per process. 0MQ contexts are thread-safe and comparatively
// expensive to create; sockets are neither, which is why every thread builds
// its own socket but they all share this.
zmq::context_t &Context() {
    static zmq::context_t context(1);
    return context;
}

zmq::socket_type ToZmq(SocketType type) {
    switch (type) {
        case SocketType::Request:   return zmq::socket_type::req;
        case SocketType::Reply:     return zmq::socket_type::rep;
        case SocketType::Publish:   return zmq::socket_type::pub;
        case SocketType::Subscribe: return zmq::socket_type::sub;
    }
    return zmq::socket_type::req;
}

} // namespace

struct Socket::Impl {
    zmq::socket_t socket;
    std::string lastError;
    explicit Impl(SocketType type) : socket(Context(), ToZmq(type)) {
        // 0MQ defaults LINGER to infinite: on shutdown the context blocks
        // until every queued message has been delivered. A message queued for
        // a peer that never arrives is therefore enough to hang the process
        // at exit and make it ignore Ctrl-C. Drop undelivered messages instead
        // - for per-frame state updates a stale message is worthless anyway.
        socket.set(zmq::sockopt::linger, 0);
    }
};

Socket::Socket(SocketType type) : impl(std::make_unique<Impl>(type)) {}
Socket::~Socket() = default;
Socket::Socket(Socket &&) noexcept = default;
Socket &Socket::operator=(Socket &&) noexcept = default;

bool Socket::Bind(const std::string &endpoint) {
    try {
        impl->socket.bind(endpoint);
        return true;
    } catch (const zmq::error_t &e) {
        impl->lastError = std::string(e.what()) + " (" + endpoint + ")";
        return false;
    }
}

bool Socket::Connect(const std::string &endpoint) {
    try {
        impl->socket.connect(endpoint);
        return true;
    } catch (const zmq::error_t &e) {
        impl->lastError = std::string(e.what()) + " (" + endpoint + ")";
        return false;
    }
}

const std::string &Socket::LastError() const {
    return impl->lastError;
}

void Socket::Subscribe(const std::string &topic) {
    impl->socket.set(zmq::sockopt::subscribe, topic);
}

void Socket::SetReceiveTimeoutMs(int milliseconds) {
    impl->socket.set(zmq::sockopt::rcvtimeo, milliseconds);
}

// A REQ socket whose reply timed out refuses every later send, and 0MQ throws
// for that. Report it rather than abort: the socket stays refused, so a client
// whose server has gone quietly stops sending instead of crashing.
bool Socket::Send(const std::string &payload) {
    try {
        zmq::message_t message(payload.begin(), payload.end());
        return impl->socket.send(message, zmq::send_flags::none).has_value();
    } catch (const zmq::error_t &e) {
        impl->lastError = e.what();
        return false;
    }
}

bool Socket::Receive(std::string &out) {
    try {
        zmq::message_t message;
        if (!impl->socket.recv(message, zmq::recv_flags::none).has_value()) {
            return false; // timed out
        }
        out.assign(static_cast<const char *>(message.data()), message.size());
        return true;
    } catch (const zmq::error_t &e) {
        impl->lastError = e.what();
        return false;
    }
}

} // namespace net
} // namespace engine
