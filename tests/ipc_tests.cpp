#include "chers/client.h"
#include "chronolane/ipc.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#define REQUIRE(x) do { if (!(x)) throw std::runtime_error(std::string(__FILE__) + ":" + std::to_string(__LINE__) + " " #x); } while (false)
using namespace std::chrono_literals;
namespace ipc = chronolane::ipc;

namespace {
constexpr std::uint32_t magic = 0x434c4e31;
struct Packet { std::uint32_t magic_value, op; std::uint64_t value, extra; };
struct Hello { std::uint32_t magic_value, abi, width, height, stride, slots; std::uint64_t bytes; };

struct Raw {
    int socket = -1, shared = -1;
    const std::uint8_t *pixels = nullptr;
    ~Raw() {
        if (pixels) ::munmap(const_cast<std::uint8_t *>(pixels), CL_FRAME_BYTES * 3);
        if (shared >= 0) ::close(shared);
        if (socket >= 0) ::close(socket);
    }
    explicit Raw(const char *path) {
        socket = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        REQUIRE(socket >= 0);
        timeval timeout{2, 0};
        REQUIRE(::setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        REQUIRE(std::strlen(path) < sizeof(address.sun_path));
        std::strcpy(address.sun_path, path);
        REQUIRE(::connect(socket, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == 0);
        Hello hello{};
        iovec vector{&hello, sizeof(hello)};
        alignas(cmsghdr) std::array<char, CMSG_SPACE(sizeof(int))> control{};
        msghdr message{};
        message.msg_iov = &vector; message.msg_iovlen = 1;
        message.msg_control = control.data(); message.msg_controllen = control.size();
        REQUIRE(::recvmsg(socket, &message, MSG_CMSG_CLOEXEC) == sizeof(hello));
        REQUIRE(hello.magic_value == magic && hello.abi == 1 && hello.width == 640 &&
                hello.height == 360 && hello.stride == 2560 && hello.slots == 3 &&
                hello.bytes == CL_FRAME_BYTES * 3);
        auto *header = CMSG_FIRSTHDR(&message);
        REQUIRE(header && header->cmsg_level == SOL_SOCKET && header->cmsg_type == SCM_RIGHTS);
        std::memcpy(&shared, CMSG_DATA(header), sizeof(shared));
        REQUIRE((::fcntl(shared, F_GETFL) & O_ACCMODE) == O_RDONLY);
        auto *address_ptr = ::mmap(nullptr, hello.bytes, PROT_READ, MAP_SHARED, shared, 0);
        REQUIRE(address_ptr != MAP_FAILED);
        pixels = static_cast<const std::uint8_t *>(address_ptr);
    }
    void send(Packet packet) { REQUIRE(::send(socket, &packet, sizeof(packet), MSG_NOSIGNAL) == sizeof(packet)); }
    Packet receive() {
        Packet packet{};
        REQUIRE(::recv(socket, &packet, sizeof(packet), 0) == sizeof(packet));
        REQUIRE(packet.magic_value == magic && packet.op == 6 && packet.value < 3 && packet.extra > 0);
        return packet;
    }
};

int child_abi(const char *path) {
    cl_client *client = nullptr;
    REQUIRE(cl_connect(path, 2000, &client) == CL_OK);
    REQUIRE(cl_tap(client, 16) == CL_ERROR_ARGUMENT);
    REQUIRE(cl_start(client) == CL_OK);
    // A second client must not observe or control this running connection.
    cl_client *second = nullptr;
    REQUIRE(cl_connect(path, 2000, &second) < 0);
    REQUIRE(second == nullptr);
    std::vector<std::uint8_t> pixels(CL_FRAME_BYTES);
    cl_frame_info info{};
    REQUIRE(cl_read_frame(client, pixels.data(), 1, &info, 1) == CL_ERROR_ARGUMENT);
    std::uint64_t previous = 0;
    for (unsigned n = 0; n < 80; ++n) {
        int result = cl_read_frame(client, pixels.data(), pixels.size(), &info, n == 0 ? 0 : 2000);
        if (result == CL_TIMEOUT) result = cl_read_frame(client, pixels.data(), pixels.size(), &info, 2000);
        REQUIRE(result == CL_OK);
        REQUIRE(info.abi_version == 1 && info.width == 640 && info.height == 360 && info.stride == 2560);
        REQUIRE(info.sequence > previous);
        previous = info.sequence;
        const auto value = static_cast<std::uint8_t>(info.sequence & 255);
        REQUIRE(std::all_of(pixels.begin(), pixels.end(), [value](auto byte) { return byte == value; }));
        REQUIRE(cl_tap(client, n % 16) == CL_OK);
    }
    REQUIRE(cl_stop(client) == CL_OK);
    cl_close(client);
    std::this_thread::sleep_for(30ms);
    REQUIRE(cl_connect(path, 2000, &client) == CL_OK);
    REQUIRE(cl_read_frame(client, pixels.data(), pixels.size(), &info, 2000) == CL_OK);
    REQUIRE(cl_tap(client, 15) == CL_OK);
    cl_close(client);
    return 0;
}

int child_lease(const char *path) {
    Raw client(path);
    std::uint8_t byte = 77;
    REQUIRE(::pwrite(client.shared, &byte, 1, 0) == -1);
    REQUIRE(errno == EBADF || errno == EPERM);
    auto *writable = ::mmap(nullptr, CL_FRAME_BYTES * 3, PROT_READ | PROT_WRITE, MAP_SHARED, client.shared, 0);
    REQUIRE(writable == MAP_FAILED);
    client.send({magic, 1, 0, 0});
    const auto first = client.receive();
    const auto *frame = client.pixels + first.value * CL_FRAME_BYTES;
    const auto value = static_cast<std::uint8_t>(first.extra & 255);
    for (unsigned n = 0; n < 30; ++n) {
        REQUIRE(std::all_of(frame, frame + CL_FRAME_BYTES, [value](auto item) { return item == value; }));
        std::this_thread::sleep_for(5ms);
    }
    client.send({magic, 2, first.extra, first.value});
    client.send({magic, 1, first.extra, 0});
    const auto latest = client.receive();
    REQUIRE(latest.extra > first.extra + 20);
    client.send({magic, 2, latest.extra, latest.value});
    client.send({magic, 3, 7, 0});
    return 0;
}

int child_malformed(const char *path, const std::string &mode) {
    Raw client(path);
    if (mode == "malformed") {
        const char bad = 'A';
        REQUIRE(::send(client.socket, &bad, 1, MSG_NOSIGNAL) == 1);
    } else if (mode == "overlap") {
        client.send({magic, 1, 0, 0});
        const auto ignored = client.receive();
        (void)ignored;
        client.send({magic, 1, 0, 0});
    } else if (mode == "badlane") client.send({magic, 3, 16, 0});
    else if (mode == "overflow") {
        for (unsigned n = 0; n < 100; ++n) {
            const Packet packet{magic, 3, n % 16, 0};
            const auto sent = ::send(client.socket, &packet, sizeof(packet), MSG_NOSIGNAL);
            if (sent < 0 && (errno == EPIPE || errno == ECONNRESET)) break;
            REQUIRE(sent == sizeof(packet));
        }
    }
    char byte;
    const auto result = ::recv(client.socket, &byte, 1, 0);
    REQUIRE(result == 0 || (result < 0 && errno == ECONNRESET));
    return 0;
}

pid_t spawn(const std::string &mode, const std::string &path) {
    const auto pid = ::fork();
    REQUIRE(pid >= 0);
    if (pid == 0) {
        // exec removes all inherited mappings and closes CLOEXEC server FDs.
        ::execl("/proc/self/exe", "ipc_tests", "--child", mode.c_str(), path.c_str(), nullptr);
        ::_exit(127);
    }
    return pid;
}

std::vector<ipc::Event> run_child(ipc::Server &server, const std::string &mode) {
    const auto child = spawn(mode, server.endpoint());
    std::vector<ipc::Event> events;
    std::vector<std::uint8_t> pixels(CL_FRAME_BYTES);
    const auto deadline = std::chrono::steady_clock::now() + 8s;
    bool done = false;
    int status = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        const auto value = static_cast<std::uint8_t>((server.frame_count() + 1) & 255);
        std::fill(pixels.begin(), pixels.end(), value);
        server.publish(pixels.data());
        server.poll([&](const auto &event) { events.push_back(event); }, mode == "overflow" ? 4 : 4096);
        if (::waitpid(child, &status, WNOHANG) == child) { done = true; break; }
        std::this_thread::sleep_for(500us);
    }
    if (!done) { ::kill(child, SIGKILL); ::waitpid(child, &status, 0); }
    REQUIRE(done);
    REQUIRE(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    for (unsigned n = 0; n < 4; ++n) {
        server.poll([&](const auto &event) { events.push_back(event); });
        std::this_thread::sleep_for(1ms);
    }
    REQUIRE(!server.connected());
    return events;
}
std::size_t descriptor_count() {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator("/proc/self/fd"), {}));
}
} // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 4 && std::string(argv[1]) == "--child") {
            const std::string mode = argv[2];
            if (mode == "abi") return child_abi(argv[3]);
            if (mode == "lease") return child_lease(argv[3]);
            return child_malformed(argv[3], mode);
        }
        REQUIRE(cl_connect(nullptr, -1, nullptr) == CL_ERROR_ARGUMENT);
        REQUIRE(cl_tap(nullptr, 0) == CL_ERROR_ARGUMENT);
        REQUIRE(cl_start(nullptr) == CL_ERROR_ARGUMENT);
        cl_close(nullptr);
        char directory[] = "/tmp/chronolane-ipc-test-XXXXXX";
        REQUIRE(::mkdtemp(directory));
        const std::string path = std::string(directory) + "/bench.sock";
        const auto before = descriptor_count();
        {
            ipc::Server server(path);
            REQUIRE(std::filesystem::exists(path));
            bool duplicate_rejected = false;
            try { ipc::Server duplicate(path); } catch (const std::system_error &) { duplicate_rejected = true; }
            REQUIRE(duplicate_rejected && std::filesystem::exists(path));
            const auto events = run_child(server, "abi");
            std::vector<std::uint32_t> lanes;
            unsigned starts = 0, stops = 0;
            std::uint64_t previous_time = 0;
            for (const auto &event : events) {
                REQUIRE(event.kind != ipc::Kind::ProtocolError && event.kind != ipc::Kind::Overflow);
                REQUIRE(event.receive_ns >= previous_time);
                previous_time = event.receive_ns;
                if (event.kind == ipc::Kind::Tap) lanes.push_back(event.lane);
                if (event.kind == ipc::Kind::Start) ++starts;
                if (event.kind == ipc::Kind::Stop) ++stops;
            }
            REQUIRE(starts == 1 && stops == 1 && lanes.size() == 81);
            for (unsigned n = 0; n < 80; ++n) REQUIRE(lanes[n] == n % 16);
            REQUIRE(lanes.back() == 15);
            const auto lease_events = run_child(server, "lease");
            REQUIRE(std::any_of(lease_events.begin(), lease_events.end(), [](const auto &event) {
                return event.kind == ipc::Kind::Tap && event.lane == 7;
            }));
            for (const auto *mode : {"malformed", "overlap", "badlane", "overflow"}) {
                const auto bad_events = run_child(server, mode);
                const auto expected = std::string(mode) == "overflow" ? ipc::Kind::Overflow : ipc::Kind::ProtocolError;
                REQUIRE(std::count_if(bad_events.begin(), bad_events.end(), [expected](const auto &event) {
                    return event.kind == expected;
                }) == 1);
            }
        }
        REQUIRE(!std::filesystem::exists(path));
        REQUIRE(descriptor_count() == before);
        std::filesystem::remove(directory);
        std::cout << "IPC: fork/exec C ABI, immutable leases, latest frames, readonly FD, input order,\n"
                     "timeouts, reconnect, exclusive client, malformed/overflow rejection, cleanup passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "IPC TEST FAILED: " << error.what() << '\n';
        return 1;
    }
}
