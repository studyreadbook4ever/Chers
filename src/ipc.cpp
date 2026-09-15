#include "chronolane/ipc.hpp"

#include <array>
#include <cerrno>
#include <cstring>
#include <cstdlib>
#include <fcntl.h>
#include <mutex>
#include <stdexcept>
#include <system_error>
#include <vector>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

namespace chronolane::ipc {
namespace {
constexpr std::uint32_t magic = 0x434c4e31;
constexpr std::uint32_t version = 1;
constexpr std::size_t slots = 3;
enum Op : std::uint32_t { Request = 1, Release, Tap, Start, Stop, Frame };
struct Packet { std::uint32_t magic_value, op; std::uint64_t value, extra; };
struct Hello {
    std::uint32_t magic_value, abi, width_value, height_value, stride_value, slot_count;
    std::uint64_t mapping_bytes;
};
static_assert(sizeof(Packet) == 24 && sizeof(Hello) == 32);

[[noreturn]] void fail(const char *operation) {
    throw std::system_error(errno, std::generic_category(), operation);
}
void close_fd(int &fd) { if (fd >= 0) { ::close(fd); fd = -1; } }
} // namespace

std::uint64_t monotonic_ns() {
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) fail("clock_gettime");
    return static_cast<std::uint64_t>(now.tv_sec) * 1000000000ULL +
           static_cast<std::uint64_t>(now.tv_nsec);
}

std::string default_endpoint() {
    const auto uid = std::to_string(::getuid());
    const char *runtime = ::getenv("XDG_RUNTIME_DIR");
    struct stat status{};
    if (runtime && runtime[0] == '/' && ::lstat(runtime, &status) == 0 &&
        S_ISDIR(status.st_mode) && status.st_uid == ::getuid() &&
        (status.st_mode & 0077) == 0)
        return std::string(runtime) + "/chers-" + uid + ".sock";
    const std::string directory = "/tmp/chers-" + uid;
    if (::mkdir(directory.c_str(), 0700) != 0 && errno != EEXIST) fail("mkdir runtime directory");
    if (::lstat(directory.c_str(), &status) != 0) fail("stat runtime directory");
    if (!S_ISDIR(status.st_mode) || status.st_uid != ::getuid() || (status.st_mode & 0077) != 0)
        throw std::runtime_error("unsafe CHERS runtime directory");
    return directory + "/bench.sock";
}

struct Server::Impl {
    mutable std::mutex mutex;
    std::mutex poll_mutex;
    std::string path;
    int listener = -1, client = -1, memfd = -1, readonly_fd = -1;
    std::uint8_t *mapping = nullptr;
    ino_t endpoint_inode = 0;
    dev_t endpoint_device = 0;
    std::array<std::uint64_t, slots> slot_sequence{};
    int latest_slot = -1, leased_slot = -1;
    std::uint64_t sequence = 0, leased_sequence = 0, request_after = 0;
    bool request_pending = false;
    bool frame_reply_pending = false;
    Packet frame_reply{};
    std::vector<Event> deferred;
    std::vector<Event> delivery;

    explicit Impl(std::string endpoint) : path(std::move(endpoint)) {
        try {
            if (path.empty() || path.size() >= sizeof(sockaddr_un::sun_path))
                throw std::invalid_argument("Unix socket endpoint is empty or too long");
            memfd = ::memfd_create("chers-visual-frames", MFD_CLOEXEC | MFD_ALLOW_SEALING);
            if (memfd < 0) fail("memfd_create");
            if (::ftruncate(memfd, static_cast<off_t>(frame_bytes * slots)) != 0) fail("ftruncate frames");
            void *address = ::mmap(nullptr, frame_bytes * slots, PROT_READ | PROT_WRITE,
                                   MAP_SHARED, memfd, 0);
            if (address == MAP_FAILED) fail("mmap frames");
            mapping = static_cast<std::uint8_t *>(address);
            std::memset(mapping, 0, frame_bytes * slots);
            // Keep our existing writable mapping, prohibit future writable maps,
            // growth/shrink, and changing the seals. The client receives O_RDONLY.
            if (::fcntl(memfd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK |
                         F_SEAL_FUTURE_WRITE | F_SEAL_SEAL) != 0) fail("seal frames");
            const auto fd_path = "/proc/self/fd/" + std::to_string(memfd);
            readonly_fd = ::open(fd_path.c_str(), O_RDONLY | O_CLOEXEC);
            if (readonly_fd < 0) fail("open read-only frames");
            listener = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            if (listener < 0) fail("socket");
            sockaddr_un address_un{};
            address_un.sun_family = AF_UNIX;
            std::memcpy(address_un.sun_path, path.c_str(), path.size() + 1);
            // Never unlink an existing endpoint: it may belong to a live run.
            if (::bind(listener, reinterpret_cast<sockaddr *>(&address_un), sizeof(address_un)) != 0)
                fail("bind (existing endpoints are never overwritten)");
            struct stat status{};
            if (::lstat(path.c_str(), &status) != 0) fail("stat socket");
            endpoint_inode = status.st_ino;
            endpoint_device = status.st_dev;
            if (::chmod(path.c_str(), 0600) != 0) fail("chmod socket");
            if (::listen(listener, 4) != 0) fail("listen");
            // Both queues keep their allocation while callbacks run outside the
            // transport mutex. Enough for a full drain plus disconnect/overflow.
            deferred.reserve(max_poll_messages + 2);
            delivery.reserve(max_poll_messages + 2);
        } catch (...) { cleanup(); throw; }
    }
    ~Impl() { cleanup(); }
    void cleanup() {
        close_fd(client); close_fd(listener); close_fd(readonly_fd); close_fd(memfd);
        if (mapping) { ::munmap(mapping, frame_bytes * slots); mapping = nullptr; }
        if (endpoint_inode) {
            struct stat status{};
            if (::lstat(path.c_str(), &status) == 0 && status.st_ino == endpoint_inode &&
                status.st_dev == endpoint_device) ::unlink(path.c_str());
        }
    }
    void disconnect(Kind kind, std::string detail, std::uint64_t now = 0) {
        if (client < 0) return;
        close_fd(client);
        leased_slot = -1;
        leased_sequence = 0;
        request_pending = frame_reply_pending = false;
        deferred.push_back({kind, 0, now ? now : monotonic_ns(), std::move(detail)});
    }
    void reply_if_ready() {
        if (client < 0) return;
        if (!frame_reply_pending && request_pending && latest_slot >= 0 && sequence > request_after) {
            leased_slot = latest_slot;
            leased_sequence = slot_sequence[static_cast<std::size_t>(leased_slot)];
            frame_reply = {magic, Frame, static_cast<std::uint64_t>(leased_slot), leased_sequence};
            frame_reply_pending = true;
            request_pending = false;
        }
        if (!frame_reply_pending) return;
        const auto sent = ::send(client, &frame_reply, sizeof(frame_reply), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (sent == static_cast<ssize_t>(sizeof(frame_reply))) frame_reply_pending = false;
        else if (sent < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
        else disconnect(Kind::Disconnect, "frame delivery disconnected");
    }
    bool send_hello(int fd) {
        Hello hello{magic, version, width, height, stride, slots, frame_bytes * slots};
        iovec vector{&hello, sizeof(hello)};
        alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
        msghdr message{};
        message.msg_iov = &vector; message.msg_iovlen = 1;
        message.msg_control = control.data(); message.msg_controllen = control.size();
        auto *header = CMSG_FIRSTHDR(&message);
        header->cmsg_level = SOL_SOCKET; header->cmsg_type = SCM_RIGHTS;
        header->cmsg_len = CMSG_LEN(sizeof(int));
        std::memcpy(CMSG_DATA(header), &readonly_fd, sizeof(int));
        return ::sendmsg(fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL) == static_cast<ssize_t>(sizeof(hello));
    }
    void accept_clients() {
        // Bound connection handling so connection spam cannot monopolize a tick.
        for (unsigned n = 0; n < 8; ++n) {
            int incoming = ::accept4(listener, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (incoming < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                fail("accept4");
            }
            ucred credentials{};
            socklen_t length = sizeof(credentials);
            if (client >= 0 || ::getsockopt(incoming, SOL_SOCKET, SO_PEERCRED,
                    &credentials, &length) != 0 || credentials.uid != ::getuid() || !send_hello(incoming)) {
                ::close(incoming);
                continue;
            }
            client = incoming;
        }
    }
    void handle(const Packet &packet, std::uint64_t now) {
        if (packet.magic_value != magic) { disconnect(Kind::ProtocolError, "invalid packet magic", now); return; }
        if (packet.op == Release) {
            if (leased_slot < 0 || frame_reply_pending || packet.value != leased_sequence ||
                packet.extra != static_cast<std::uint64_t>(leased_slot))
                disconnect(Kind::ProtocolError, "invalid frame release", now);
            else { leased_slot = -1; leased_sequence = 0; }
            return;
        }
        if (packet.extra != 0) { disconnect(Kind::ProtocolError, "unexpected packet field", now); return; }
        switch (packet.op) {
        case Request:
            if (request_pending || frame_reply_pending || leased_slot >= 0 || packet.value > sequence)
                disconnect(Kind::ProtocolError, "invalid or overlapping frame request", now);
            else { request_pending = true; request_after = packet.value; }
            break;
        case Tap:
            if (packet.value > 15) disconnect(Kind::ProtocolError, "lane outside A-P", now);
            else deferred.push_back({Kind::Tap, static_cast<std::uint32_t>(packet.value), now, {}});
            break;
        case Start:
        case Stop:
            if (packet.value != 0) disconnect(Kind::ProtocolError, "invalid control payload", now);
            else deferred.push_back({packet.op == Start ? Kind::Start : Kind::Stop, 0, now, {}});
            break;
        default: disconnect(Kind::ProtocolError, "unknown input operation", now); break;
        }
    }
};

Server::Server(std::string endpoint) : impl_(std::make_unique<Impl>(std::move(endpoint))) {}
Server::~Server() = default;

void Server::publish(const std::uint8_t *rgba, std::size_t source_stride) {
    if (!rgba || source_stride < stride) throw std::invalid_argument("invalid RGBA frame");
    std::lock_guard lock(impl_->mutex);
    int slot = (impl_->latest_slot + 1) % static_cast<int>(slots);
    if (slot == impl_->leased_slot) slot = (slot + 1) % static_cast<int>(slots);
    auto *destination = impl_->mapping + static_cast<std::size_t>(slot) * frame_bytes;
    if (source_stride == stride) std::memcpy(destination, rgba, frame_bytes);
    else for (std::size_t y = 0; y < height; ++y)
        std::memcpy(destination + y * stride, rgba + y * source_stride, stride);
    impl_->latest_slot = slot;
    impl_->slot_sequence[static_cast<std::size_t>(slot)] = ++impl_->sequence;
    impl_->reply_if_ready();
}

void Server::poll(const std::function<void(const Event &)> &callback, std::size_t max_messages) {
    if (!callback || max_messages == 0 || max_messages > max_poll_messages)
        throw std::invalid_argument("poll requires callback and capacity in [1, 4096]");
    std::lock_guard poll_lock(impl_->poll_mutex);
    {
        std::lock_guard lock(impl_->mutex);
        impl_->accept_clients();
        std::size_t received = 0;
        while (impl_->client >= 0 && received < max_messages) {
            Packet packet{};
            const auto bytes = ::recv(impl_->client, &packet, sizeof(packet), MSG_DONTWAIT | MSG_TRUNC);
            const auto now = monotonic_ns();
            if (bytes < 0) {
                if (errno == EINTR) continue;
                if (errno == EAGAIN || errno == EWOULDBLOCK) break;
                impl_->disconnect(Kind::Disconnect, "input delivery disconnected", now); break;
            }
            if (bytes == 0) { impl_->disconnect(Kind::Disconnect, "client closed", now); break; }
            ++received;
            if (bytes != static_cast<ssize_t>(sizeof(packet))) {
                impl_->disconnect(Kind::ProtocolError, "incorrect input packet size", now); break;
            }
            impl_->handle(packet, now);
        }
        if (impl_->client >= 0 && received == max_messages) {
            char byte;
            const auto pending = ::recv(impl_->client, &byte, 1, MSG_DONTWAIT | MSG_PEEK | MSG_TRUNC);
            if (pending > 0) impl_->disconnect(Kind::Overflow, "input drain capacity exceeded; run invalid");
        }
        impl_->reply_if_ready();
        impl_->delivery.swap(impl_->deferred);
    }
    try {
        for (const auto &event : impl_->delivery) callback(event);
    } catch (...) { impl_->delivery.clear(); throw; }
    impl_->delivery.clear();
}
bool Server::connected() const { std::lock_guard lock(impl_->mutex); return impl_->client >= 0; }
const std::string &Server::endpoint() const noexcept { return impl_->path; }
std::uint64_t Server::frame_count() const { std::lock_guard lock(impl_->mutex); return impl_->sequence; }
} // namespace chronolane::ipc
