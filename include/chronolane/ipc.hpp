#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace chronolane::ipc {
inline constexpr std::uint32_t width = 640;
inline constexpr std::uint32_t height = 360;
inline constexpr std::uint32_t stride = width * 4;
inline constexpr std::size_t frame_bytes = stride * height;
inline constexpr std::size_t max_poll_messages = 4096;

enum class Kind { Tap, Start, Stop, Disconnect, ProtocolError, Overflow };
struct Event {
    Kind kind;
    std::uint32_t lane = 0;
    std::uint64_t receive_ns = 0;
    std::string detail;
};

std::string default_endpoint();
std::uint64_t monotonic_ns();

// One external model connection; complete RGBA pixels and transport metadata
// only. Methods synchronize publication against socket servicing. Call poll
// regularly (~1 ms) from the benchmark's input loop. User callbacks execute
// outside the transport mutex. A malformed/overflowing connection is closed and
// explicitly reported so the application can invalidate the run.
class Server {
public:
    explicit Server(std::string endpoint = default_endpoint());
    ~Server();
    Server(const Server &) = delete;
    Server &operator=(const Server &) = delete;
    void publish(const std::uint8_t *rgba, std::size_t source_stride = stride);
    void poll(const std::function<void(const Event &)> &callback,
              std::size_t max_messages = max_poll_messages);
    bool connected() const;
    const std::string &endpoint() const noexcept;
    std::uint64_t frame_count() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace chronolane::ipc
