#include "chers/client.h"
#include "chronolane/ipc.hpp"

#include <array>
#include <cerrno>
#include <climits>
#include <cstring>
#include <fcntl.h>
#include <mutex>
#include <new>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace {
// Wire ABI v1; paired with src/ipc.cpp. Fixed-width native-endian messages are
// local-machine only, not a network serialization format.
constexpr std::uint32_t magic = 0x434c4e31;
constexpr std::size_t slots = 3;
enum Op : std::uint32_t { Request = 1, Release, Tap, Start, Stop, Frame };
struct Packet { std::uint32_t magic_value, op; std::uint64_t value, extra; };
struct Hello {
    std::uint32_t magic_value, abi, width_value, height_value, stride_value, slot_count;
    std::uint64_t mapping_bytes;
};
static_assert(sizeof(Packet) == 24 && sizeof(Hello) == 32);

std::uint64_t now_ns() {
    timespec now{};
    ::clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(now.tv_nsec);
}
std::uint64_t deadline_for(int timeout_ms) {
    return now_ns() + static_cast<std::uint64_t>(timeout_ms) * 1000000ULL;
}
int wait_fd(int fd, short events, std::uint64_t deadline) {
    for (;;) {
        const auto now = now_ns();
        const auto remaining = deadline > now ? (deadline - now + 999999) / 1000000 : 0;
        const int milliseconds = static_cast<int>(remaining > INT_MAX ? INT_MAX : remaining);
        pollfd item{fd, events, 0};
        const int ready = ::poll(&item, 1, milliseconds);
        if (ready < 0 && errno == EINTR) continue;
        if (ready < 0) return CL_ERROR_SYSTEM;
        if (ready == 0) return CL_TIMEOUT;
        if (item.revents & events) return CL_OK;
        if (item.revents & (POLLHUP | POLLERR | POLLNVAL)) return CL_ERROR_DISCONNECTED;
    }
}
int send_raw(int fd, const Packet &packet) {
    for (;;) {
        const auto result = ::send(fd, &packet, sizeof(packet), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (result == static_cast<ssize_t>(sizeof(packet))) return CL_OK;
        if (result < 0 && errno == EINTR) continue;
        if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return CL_WOULD_BLOCK;
        if (result < 0 && (errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN))
            return CL_ERROR_DISCONNECTED;
        return CL_ERROR_SYSTEM;
    }
}
} // namespace

struct cl_client {
    int fd = -1;
    const std::uint8_t *mapping = nullptr;
    std::mutex send_mutex;
    std::mutex read_mutex;
    std::uint64_t last_sequence = 0;
    bool request_pending = false;
    bool release_pending = false;
    Packet release_packet{};
    ~cl_client() {
        if (mapping) ::munmap(const_cast<std::uint8_t *>(mapping), CL_FRAME_BYTES * slots);
        if (fd >= 0) ::close(fd);
    }
    int send(const Packet &packet) {
        std::lock_guard lock(send_mutex);
        return send_raw(fd, packet);
    }
    int send_until(const Packet &packet, std::uint64_t deadline) {
        for (;;) {
            const auto result = send(packet);
            if (result != CL_WOULD_BLOCK) return result;
            const auto ready = wait_fd(fd, POLLOUT, deadline);
            if (ready != CL_OK) return ready;
        }
    }
};

extern "C" int cl_connect(const char *endpoint, int timeout_ms, cl_client **out) {
    if (!out || timeout_ms < 0) return CL_ERROR_ARGUMENT;
    *out = nullptr;
    try {
        const auto path = endpoint ? std::string(endpoint) : chronolane::ipc::default_endpoint();
        if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path)) return CL_ERROR_ARGUMENT;
        auto client = std::make_unique<cl_client>();
        client->fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (client->fd < 0) return CL_ERROR_SYSTEM;
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
        const auto deadline = deadline_for(timeout_ms);
        if (::connect(client->fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) != 0) {
            if (errno != EINPROGRESS) return CL_ERROR_SYSTEM;
            const int ready = wait_fd(client->fd, POLLOUT, deadline);
            if (ready != CL_OK) return ready;
            int error = 0;
            socklen_t length = sizeof(error);
            if (::getsockopt(client->fd, SOL_SOCKET, SO_ERROR, &error, &length) != 0 || error != 0)
                return CL_ERROR_SYSTEM;
        }
        const int ready = wait_fd(client->fd, POLLIN, deadline);
        if (ready != CL_OK) return ready;
        Hello hello{};
        iovec vector{&hello, sizeof(hello)};
        alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int) * 4)> control{};
        msghdr message{};
        message.msg_iov = &vector; message.msg_iovlen = 1;
        message.msg_control = control.data(); message.msg_controllen = control.size();
        const auto bytes = ::recvmsg(client->fd, &message, MSG_DONTWAIT | MSG_CMSG_CLOEXEC | MSG_TRUNC);
        std::array<int, 4> descriptors{-1, -1, -1, -1};
        std::size_t descriptor_count = 0;
        for (auto *header = CMSG_FIRSTHDR(&message); header; header = CMSG_NXTHDR(&message, header)) {
            if (header->cmsg_level != SOL_SOCKET || header->cmsg_type != SCM_RIGHTS ||
                header->cmsg_len < CMSG_LEN(0)) continue;
            const auto count = (header->cmsg_len - CMSG_LEN(0)) / sizeof(int);
            for (std::size_t i = 0; i < count; ++i) {
                int received = -1;
                std::memcpy(&received, CMSG_DATA(header) + i * sizeof(int), sizeof(int));
                if (descriptor_count < descriptors.size()) descriptors[descriptor_count++] = received;
                else ::close(received);
            }
        }
        const auto close_descriptors = [&] {
            for (auto descriptor : descriptors) if (descriptor >= 0) ::close(descriptor);
        };
        if (bytes == 0) { close_descriptors(); return CL_ERROR_DISCONNECTED; }
        if (bytes != static_cast<ssize_t>(sizeof(hello)) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
            descriptor_count != 1 || hello.magic_value != magic || hello.abi != CL_ABI_VERSION ||
            hello.width_value != CL_WIDTH || hello.height_value != CL_HEIGHT ||
            hello.stride_value != CL_STRIDE || hello.slot_count != slots ||
            hello.mapping_bytes != CL_FRAME_BYTES * slots) {
            close_descriptors(); return CL_ERROR_PROTOCOL;
        }
        struct stat status{};
        const int flags = ::fcntl(descriptors[0], F_GETFL);
        if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY ||
            ::fstat(descriptors[0], &status) != 0 || status.st_size != static_cast<off_t>(hello.mapping_bytes)) {
            close_descriptors(); return CL_ERROR_PROTOCOL;
        }
        void *mapping = ::mmap(nullptr, CL_FRAME_BYTES * slots, PROT_READ, MAP_SHARED, descriptors[0], 0);
        close_descriptors();
        if (mapping == MAP_FAILED) return CL_ERROR_SYSTEM;
        client->mapping = static_cast<const std::uint8_t *>(mapping);
        *out = client.release();
        return CL_OK;
    } catch (...) { return CL_ERROR_SYSTEM; }
}

extern "C" int cl_read_frame(cl_client *client, void *rgba, size_t capacity,
                              cl_frame_info *info, int timeout_ms) {
    if (!client || !rgba || capacity < CL_FRAME_BYTES || !info || timeout_ms < 0) return CL_ERROR_ARGUMENT;
    std::lock_guard lock(client->read_mutex);
    const auto deadline = deadline_for(timeout_ms);
    if (client->release_pending) {
        const auto result = client->send_until(client->release_packet, deadline);
        if (result != CL_OK) return result;
        client->release_pending = false;
    }
    if (!client->request_pending) {
        const Packet request{magic, Request, client->last_sequence, 0};
        const auto result = client->send_until(request, deadline);
        if (result != CL_OK) return result;
        client->request_pending = true;
    }
    for (;;) {
        const auto ready = wait_fd(client->fd, POLLIN, deadline);
        if (ready != CL_OK) return ready;
        Packet packet{};
        const auto bytes = ::recv(client->fd, &packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC);
        if (bytes < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (bytes == 0 || (bytes < 0 && errno == ECONNRESET)) return CL_ERROR_DISCONNECTED;
        if (bytes < 0) return CL_ERROR_SYSTEM;
        if (bytes != static_cast<ssize_t>(sizeof(packet)) || packet.magic_value != magic ||
            packet.op != Frame || packet.value >= slots || packet.extra <= client->last_sequence)
            return CL_ERROR_PROTOCOL;
        // The server holds this slot immutable until it receives Release. It
        // continues rendering into the other two slots even if this process is
        // descheduled here for an arbitrarily long time.
        std::memcpy(rgba, client->mapping + packet.value * CL_FRAME_BYTES, CL_FRAME_BYTES);
        client->last_sequence = packet.extra;
        client->request_pending = false;
        client->release_packet = {magic, Release, packet.extra, packet.value};
        client->release_pending = true;
        const auto release_result = client->send(client->release_packet);
        if (release_result == CL_OK) client->release_pending = false;
        else if (release_result != CL_WOULD_BLOCK) return release_result;
        *info = {CL_ABI_VERSION, CL_WIDTH, CL_HEIGHT, CL_STRIDE, packet.extra};
        return CL_OK;
    }
}

extern "C" int cl_tap(cl_client *client, uint32_t lane) {
    if (!client || lane > 15) return CL_ERROR_ARGUMENT;
    return client->send({magic, Tap, lane, 0});
}
extern "C" int cl_start(cl_client *client) {
    return client ? client->send({magic, Start, 0, 0}) : CL_ERROR_ARGUMENT;
}
extern "C" int cl_stop(cl_client *client) {
    return client ? client->send({magic, Stop, 0, 0}) : CL_ERROR_ARGUMENT;
}
extern "C" void cl_close(cl_client *client) { delete client; }
extern "C" const char *cl_result_string(int result) {
    switch (result) {
    case CL_OK: return "ok";
    case CL_TIMEOUT: return "timeout";
    case CL_WOULD_BLOCK: return "not sent: transport would block";
    case CL_ERROR_ARGUMENT: return "invalid argument";
    case CL_ERROR_SYSTEM: return "operating system error";
    case CL_ERROR_PROTOCOL: return "protocol violation";
    case CL_ERROR_DISCONNECTED: return "disconnected";
    default: return "unknown result";
    }
}
