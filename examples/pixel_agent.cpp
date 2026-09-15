// A deterministic VISUAL diagnostic, not a trained-model benchmark result.
// The only benchmark observation used here is cl_read_frame's RGBA image.
// This file deliberately includes no engine, renderer, IPC internals or chart
// headers. Its fixed font/geometry knowledge is part of the public task rules.
#include "chers/client.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#include <time.h>

namespace {
namespace fs = std::filesystem;
constexpr std::int64_t ms = 1'000'000;
constexpr int width = 640;
constexpr int height = 360;
constexpr std::uint32_t pixel(unsigned r, unsigned g, unsigned b) {
    return r | (g << 8U) | (b << 16U) | (255U << 24U);
}
constexpr auto phase_color = pixel(130, 151, 175);
constexpr auto note_center_color = pixel(204, 252, 255);
constexpr auto note_body_color = pixel(40, 220, 240);
constexpr auto perfect_color = pixel(137, 245, 172);
constexpr auto good_color = pixel(255, 208, 121);
constexpr std::array<std::array<std::uint8_t, 7>, 10> digits{{
    {{14,17,19,21,25,17,14}}, {{4,12,4,4,4,4,14}},
    {{14,17,1,2,4,8,31}}, {{30,1,1,14,1,1,30}},
    {{2,6,10,18,31,2,2}}, {{31,16,16,30,1,1,30}},
    {{14,16,16,30,17,17,14}}, {{31,1,2,4,8,8,8}},
    {{14,17,17,14,17,17,14}}, {{14,17,17,15,1,1,14}},
}};
constexpr std::array<std::array<std::uint8_t, 7>, 26> letters{{
    {{14,17,17,31,17,17,17}}, {{30,17,17,30,17,17,30}},
    {{14,17,16,16,16,17,14}}, {{30,17,17,17,17,17,30}},
    {{31,16,16,30,16,16,31}}, {{31,16,16,30,16,16,16}},
    {{14,17,16,23,17,17,15}}, {{17,17,17,31,17,17,17}},
    {{14,4,4,4,4,4,14}}, {{7,2,2,2,18,18,12}},
    {{17,18,20,24,20,18,17}}, {{16,16,16,16,16,16,31}},
    {{17,27,21,21,17,17,17}}, {{17,25,21,19,17,17,17}},
    {{14,17,17,17,17,17,14}}, {{30,17,17,30,16,16,16}},
    {{14,17,17,17,21,18,13}}, {{30,17,17,30,20,18,17}},
    {{15,16,16,14,1,1,30}}, {{31,4,4,4,4,4,4}},
    {{17,17,17,17,17,17,14}}, {{17,17,17,17,17,10,4}},
    {{17,17,17,21,21,21,10}}, {{17,17,10,4,10,17,17}},
    {{17,17,10,4,4,4,4}}, {{31,1,2,4,8,16,31}},
}};

enum class VisualPhase { unknown, ready, preroll, calibration, waiting, scored, finished };
constexpr std::array<std::string_view, 7> phase_names{{
    "UNKNOWN", "READY", "PREROLL", "CALIBRATION", "WAIT", "SCORED", "FINISHED"
}};

VisualPhase read_phase(const std::vector<std::uint32_t>& image) {
    if (image.size() != static_cast<std::size_t>(width * height)) return VisualPhase::unknown;
    for (std::size_t phase = 1; phase < phase_names.size(); ++phase) {
        // Scored gameplay deliberately has no top-left phase label.
        if (phase == static_cast<std::size_t>(VisualPhase::scored)) continue;
        bool matches = true;
        const auto text = phase_names[phase];
        for (std::size_t i = 0; i < text.size() && matches; ++i) {
            const auto& glyph = letters[static_cast<std::size_t>(text[i] - 'A')];
            for (int row = 0; row < 7 && matches; ++row) for (int col = 0; col < 5; ++col) {
                const bool expected = (glyph[static_cast<std::size_t>(row)] & (1U << (4 - col))) != 0;
                const bool observed = image[static_cast<std::size_t>(11 + row) * width +
                                            8 + i * 6 + static_cast<std::size_t>(col)] == phase_color;
                if (expected != observed) { matches = false; break; }
            }
        }
        if (matches) return static_cast<VisualPhase>(phase);
    }
    return VisualPhase::unknown;
}

struct PhaseObservation {
    VisualPhase phase{VisualPhase::unknown};
    bool inferred{};
};

class PhaseTracker {
public:
    PhaseObservation observe(const std::vector<std::uint32_t>& image, int lanes) {
        const auto visible = read_phase(image);
        if (visible != VisualPhase::unknown) {
            previous_ = visible;
            return {visible, false};
        }
        // The public sequence is WAIT -> unlabeled scored play -> FINISHED.
        // Infer that transition only after actually observing WAIT, and only
        // in an intact playfield with no remaining phase-label ink. This is
        // local visual history, not synchronization to a hidden game epoch.
        // Retaining previous_ across a damaged frame permits later recovery.
        if ((previous_ == VisualPhase::waiting || previous_ == VisualPhase::scored) &&
            unlabeled_playfield(image, lanes)) {
            previous_ = VisualPhase::scored;
            return {VisualPhase::scored, true};
        }
        return {};
    }

private:
    static bool unlabeled_playfield(const std::vector<std::uint32_t>& image, int lanes) {
        if (image.size() != static_cast<std::size_t>(width * height) || lanes < 2 || lanes > 16)
            return false;
        // Every label is overlaid on the notes. Damaged but still visible
        // lettering must not become an inferred scored phase.
        for (int y = 11; y < 18; ++y) for (int x = 8; x < 8 + 11 * 6; ++x)
            if (image[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] == phase_color)
                return false;
        // Lane edges preserve the full-width judgement line even when a note
        // overlaps it; note bars are inset and lane accents are centered.
        for (int lane = 0; lane < lanes; ++lane) {
            const int x = lane * width / lanes;
            if (image[330 * width + static_cast<std::size_t>(x)] != pixel(133, 156, 179)) return false;
        }
        return true;
    }

    VisualPhase previous_{VisualPhase::unknown};
};

std::int64_t monotonic_ns() {
    timespec now{};
    if (::clock_gettime(CLOCK_MONOTONIC, &now) != 0) throw std::runtime_error("clock_gettime failed");
    return static_cast<std::int64_t>(now.tv_sec) * 1'000'000'000 + now.tv_nsec;
}

void sleep_until(std::int64_t target) {
    if (target < 0) return;
    timespec deadline{target / 1'000'000'000, target % 1'000'000'000};
    while (::clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) == EINTR) {}
}

int match_digit(const std::vector<std::uint32_t>& image, int x, int y,
                int scale, std::uint32_t color) {
    for (int digit = 0; digit < 10; ++digit) {
        bool matches = true;
        for (int row = 0; row < 7 && matches; ++row) {
            for (int col = 0; col < 5; ++col) {
                const bool expected = (digits[static_cast<std::size_t>(digit)][static_cast<std::size_t>(row)] & (1U << (4 - col))) != 0;
                const bool observed = image[static_cast<std::size_t>(y + row * scale) * width + static_cast<std::size_t>(x + col * scale)] == color;
                if (expected != observed) { matches = false; break; }
            }
        }
        if (matches) return digit;
    }
    return -1;
}

// Read the signed one-decimal feedback displayed under the judgement line.
// Returns integer tenths of milliseconds, or INT_MIN if no successful-hit
// feedback is visible. Neither hidden timing data nor result files are read.
int read_feedback(const std::vector<std::uint32_t>& image, int lane, int lanes) {
    const int left = lane * width / lanes;
    const int right = (lane + 1) * width / lanes;
    const int center = (left + right) / 2;
    for (const auto color : {perfect_color, good_color}) {
        for (const int length : {4, 5}) {
            const int x = center - (length * 6 - 1) / 2;
            const int y = 334;
            // Both signs have a horizontal bar in row 3. A plus additionally
            // has a vertical stem in rows 2 and 4.
            if (image[static_cast<std::size_t>(y + 3) * width + static_cast<std::size_t>(x + 1)] != color ||
                image[static_cast<std::size_t>(y + 3) * width + static_cast<std::size_t>(x + 3)] != color) continue;
            const int sign = image[static_cast<std::size_t>(y + 2) * width + static_cast<std::size_t>(x + 2)] == color ? 1 : -1;
            const int first = match_digit(image, x + 6, y, 1, color);
            if (first < 0) continue;
            int whole = first;
            if (length == 5) {
                const int second = match_digit(image, x + 12, y, 1, color);
                if (second < 0) continue;
                whole = whole * 10 + second;
            }
            const int decimal_x = x + (length - 2) * 6;
            if (image[static_cast<std::size_t>(y + 6) * width + static_cast<std::size_t>(decimal_x + 2)] != color) continue;
            const int fraction = match_digit(image, x + (length - 1) * 6, y, 1, color);
            if (fraction >= 0 && whole <= 20) return sign * (whole * 10 + fraction);
        }
    }
    return std::numeric_limits<int>::min();
}

std::vector<int> note_centers(const std::vector<std::uint32_t>& image, int lane, int lanes) {
    const int left = lane * width / lanes + 5;
    const int right = (lane + 1) * width / lanes - 5;
    const int threshold = std::max(3, (right - left) / 4);
    std::vector<int> centers;
    centers.reserve(26);
    // Descending screen y means increasing target time: newly visible tracks
    // can be appended while retaining temporal order within each lane.
    for (int y = 337; y >= 0; --y) {
        int white = 0;
        for (int x = left; x < right; ++x)
            white += image[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] == note_center_color;
        if (white < threshold) continue;
        int cyan = 0;
        for (int x = left; x < right; ++x)
            cyan += image[static_cast<std::size_t>(y + 1) * width + static_cast<std::size_t>(x)] == note_body_color;
        if (cyan >= threshold) centers.push_back(y);
    }
    return centers;
}

void write_ppm(const fs::path& path, const std::vector<std::uint32_t>& image) {
    std::vector<char> rgb(static_cast<std::size_t>(width * height * 3));
    for (std::size_t index = 0; index < image.size(); ++index) {
        rgb[index * 3] = static_cast<char>(image[index]);
        rgb[index * 3 + 1] = static_cast<char>(image[index] >> 8U);
        rgb[index * 3 + 2] = static_cast<char>(image[index] >> 16U);
    }
    std::ofstream out(path, std::ios::binary);
    out << "P6\n640 360\n255\n";
    out.write(rgb.data(), static_cast<std::streamsize>(rgb.size()));
    if (!out) throw std::runtime_error("cannot write visual evidence frame");
}

struct Track {
    std::uint64_t id{};
    double target_ms{};
    std::uint32_t observations{};
    double first_receipt_ms{};
    double last_receipt_ms{};
    bool tapped{};
};

struct TapRecord {
    std::uint64_t id{};
    int lane{};
    double estimated_target_ms{};
    std::int64_t scheduled_ns{};
    std::int64_t sent_ns{};
    std::int64_t local_origin_ns{};
    std::int64_t correction_ns{};
    int result{};
};

struct ObservationRecord {
    std::uint64_t sequence{};
    std::int64_t receipt_ns{};
    double local_elapsed_ms{};
    VisualPhase phase{VisualPhase::unknown};
    std::size_t note_bars{};
    bool tracking_used{};
    bool phase_inferred{};
};

struct EvidenceFrame {
    fs::path filename;
    std::vector<std::uint32_t> pixels;
    bool captured{};
};

struct Shared {
    std::mutex mutex;
    std::condition_variable changed;
    std::array<std::vector<Track>, 16> tracks;
    std::array<std::size_t, 16> next_track{};
    std::int64_t local_origin_ns{};
    std::int64_t correction_ns{};
    bool stop{};
    bool calibration_applied{};
    std::uint64_t next_id{};
};

// All times in tracks are relative to an arbitrary client-local origin. This
// origin is never synchronized to benchmark game time. A received note gives
// its remaining travel time directly through its position and the public
// one-second travel rule. Repeated observations reduce pixel quantization;
// calibration learns the constant capture/transport/input delay.
void observe(Shared& shared, int lane, int center_y, double receipt_ms) {
    const double target = receipt_ms +
                          static_cast<double>(330 - center_y) * 1000.0 / 330.0;
    auto& tracks = shared.tracks[static_cast<std::size_t>(lane)];
    auto found = std::lower_bound(tracks.begin(), tracks.end(), target,
                                  [](const Track& track, double time) { return track.target_ms < time; });
    if (found == tracks.end() || std::abs(found->target_ms - target) >= 18.0) {
        if (found != tracks.begin() && std::abs(std::prev(found)->target_ms - target) < 18.0) --found;
        else {
            // A note seen for the first time already late cannot be recovered
            // reliably. Do not invent a new tap for stale pixels of a hit.
            if (target < receipt_ms - 8.0) return;
            Track track{shared.next_id++, target, 1, receipt_ms, receipt_ms, false};
            // Under normal streaming new notes append. Inserting recovers
            // from a rare visual occlusion without assuming hidden note order.
            const auto index = static_cast<std::size_t>(found - tracks.begin());
            tracks.insert(found, track);
            if (index < shared.next_track[static_cast<std::size_t>(lane)])
                shared.next_track[static_cast<std::size_t>(lane)] = index;
            return;
        }
    }
    if (found->last_receipt_ms != receipt_ms) {
        found->target_ms += (target - found->target_ms) / static_cast<double>(found->observations + 1);
        ++found->observations;
        found->last_receipt_ms = receipt_ms;
    }
}

// A client stall makes the age of the first returned image uncertain. Do not
// turn that stale image into shifted/new tracks. Already predicted targets
// continue on the independent scheduler; the next timely frame resumes
// tracking. This uses client receipt times only, not a hidden capture clock.
bool use_for_tracking(std::int64_t receipt_ns, std::int64_t previous_receipt_ns) {
    return previous_receipt_ns == 0 || receipt_ns - previous_receipt_ns <= 25 * ms;
}

void schedule(cl_client* client, Shared& shared, int lanes, std::string_view mode,
              std::vector<TapRecord>& actions, std::atomic<bool>& failed) {
    try {
        double blind_next_ms = 1'000.0;
        for (;;) {
            std::unique_lock lock(shared.mutex);
            if (shared.stop) return;
            if (mode == "idle" || shared.local_origin_ns == 0) {
                shared.changed.wait_for(lock, std::chrono::milliseconds(10));
                continue;
            }
            int chosen_lane = -1;
            std::size_t chosen_index = 0;
            double target_ms = std::numeric_limits<double>::infinity();
            if (mode == "blind40ms") {
                target_ms = blind_next_ms;
                chosen_lane = 0;
            } else {
                for (int lane = 0; lane < lanes; ++lane) {
                    auto& next = shared.next_track[static_cast<std::size_t>(lane)];
                    const auto& tracks = shared.tracks[static_cast<std::size_t>(lane)];
                    while (next < tracks.size() && tracks[next].tapped) ++next;
                    if (next < tracks.size() && tracks[next].target_ms < target_ms) {
                        target_ms = tracks[next].target_ms;
                        chosen_lane = lane;
                        chosen_index = next;
                    }
                }
            }
            if (chosen_lane < 0) {
                shared.changed.wait_for(lock, std::chrono::milliseconds(5));
                continue;
            }
            const auto origin = shared.local_origin_ns;
            const auto correction = shared.correction_ns;
            const auto deadline = origin + static_cast<std::int64_t>(std::llround(target_ms * static_cast<double>(ms))) + correction;
            const auto remaining = deadline - monotonic_ns();
            if (remaining > 300'000) {
                shared.changed.wait_for(lock, std::chrono::nanoseconds(std::min<std::int64_t>(remaining - 200'000, 5 * ms)));
                continue;
            }
            std::uint64_t id = std::numeric_limits<std::uint64_t>::max();
            if (mode == "blind40ms") blind_next_ms += 40.0;
            else {
                auto& track = shared.tracks[static_cast<std::size_t>(chosen_lane)][chosen_index];
                track.tapped = true;
                id = track.id;
            }
            lock.unlock();
            // Absolute sleep avoids drift; at most 100 us of busy waiting
            // closes the final scheduler wakeup uncertainty for this baseline.
            if (deadline - monotonic_ns() > 150'000) sleep_until(deadline - 100'000);
            while (monotonic_ns() < deadline) std::atomic_signal_fence(std::memory_order_seq_cst);
            const int end_lane = mode == "blind40ms" ? lanes : chosen_lane + 1;
            for (int lane = chosen_lane; lane < end_lane; ++lane) {
                auto sent = monotonic_ns();
                int result = cl_tap(client, static_cast<std::uint32_t>(lane));
                for (int retry = 0; result == CL_WOULD_BLOCK && retry < 10; ++retry) {
                    sleep_until(monotonic_ns() + 100'000);
                    sent = monotonic_ns();
                    result = cl_tap(client, static_cast<std::uint32_t>(lane));
                }
                actions.push_back({id, lane, target_ms, deadline, sent, origin, correction, result});
                if (result != CL_OK) { failed = true; return; }
            }
        }
    } catch (const std::exception& error) {
        std::cerr << "scheduler: " << error.what() << '\n';
        failed = true;
    }
}

int parse_lanes(std::string_view value) {
    int result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result < 2 || result > 16)
        throw std::invalid_argument("LANES must be an integer in [2,16]");
    return result;
}

int run(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
        std::cout << "usage: pixel_agent LANES [SOCKET|-] [OUTPUT_DIR] [pixel|blind40ms|idle]\n"
                  << "Deterministic pixel-only diagnostic; not a trained-model result.\n"
                  << "Uses note motion, visible phase labels and calibration feedback; no game clock.\n";
        return 0;
    }
    if (argc < 2 || argc > 5) {
        std::cerr << "usage: pixel_agent LANES [SOCKET|-] [OUTPUT_DIR] [pixel|blind40ms|idle]\n";
        return 1;
    }
    const int lanes = parse_lanes(argv[1]);
    const char* endpoint = argc >= 3 && std::string_view(argv[2]) != "-" ? argv[2] : nullptr;
    const fs::path output = argc >= 4 ? argv[3] : "pixel-agent-evidence";
    const std::string mode = argc >= 5 ? argv[4] : "pixel";
    if (mode != "pixel" && mode != "blind40ms" && mode != "idle") throw std::invalid_argument("unknown diagnostic mode");
    fs::create_directories(output);
    cl_client* client = nullptr;
    int result = cl_connect(endpoint, 5'000, &client);
    if (result != CL_OK) throw std::runtime_error(std::string("connect: ") + cl_result_string(result));
    struct Close { cl_client* client; ~Close() { cl_close(client); } } close{client};
    std::vector<std::uint32_t> image(CL_WIDTH * CL_HEIGHT);
    cl_frame_info info{};
    result = cl_read_frame(client, image.data(), CL_FRAME_BYTES, &info, 2'000);
    if (result != CL_OK) throw std::runtime_error(std::string("initial frame: ") + cl_result_string(result));
    write_ppm(output / "ready.ppm", image);

    // Evidence must not introduce disk or terminal stalls into frame receipt
    // measurements. Reserve logs and allocate/touch every sample image before
    // START. During play a sample is only a memcpy-sized in-memory copy; all
    // CSV formatting and PPM encoding/writes happen after the scheduler joins.
    std::vector<ObservationRecord> observation_records;
    observation_records.reserve(8'192); // 75 s safety limit * 80 fps, plus margin.
    constexpr std::array<int, 5> sample_times{{1'500, 5'000, 12'000, 30'000, 60'000}};
    std::vector<EvidenceFrame> sample_frames;
    sample_frames.reserve(sample_times.size() + 1);
    for (const auto time : sample_times) {
        sample_frames.push_back({"frame-" + std::to_string(time) + "ms.ppm",
                                 std::vector<std::uint32_t>(CL_WIDTH * CL_HEIGHT), false});
    }
    sample_frames.push_back({"finished.ppm", std::vector<std::uint32_t>(CL_WIDTH * CL_HEIGHT), false});
    const auto local_origin_ns = monotonic_ns();
    result = cl_start(client);
    if (result != CL_OK) throw std::runtime_error(std::string("start: ") + cl_result_string(result));

    Shared shared;
    shared.local_origin_ns = local_origin_ns;
    for (auto& lane : shared.tracks) lane.reserve(2'048);
    std::vector<TapRecord> actions;
    actions.reserve(30'000);
    std::atomic<bool> failed{false};
    std::jthread scheduler([&] { schedule(client, shared, lanes, mode, actions, failed); });
    struct Stop {
        Shared& shared;
        ~Stop() { std::lock_guard lock(shared.mutex); shared.stop = true; shared.changed.notify_all(); }
    } stop{shared};

    std::vector<int> calibration_errors;
    calibration_errors.reserve(2'048);
    std::array<int, 16> previous_feedback{};
    previous_feedback.fill(std::numeric_limits<int>::min());
    std::uint64_t frames = 0, ocr_failures = 0, frame_gap_over16 = 0;
    PhaseTracker phase_tracker;
    std::int64_t last_receipt = 0, maximum_gap = 0;
    const auto safety_deadline = monotonic_ns() + 75'000 * ms;
    std::size_t next_sample = 0;
    bool complete = false;
    while (!failed && monotonic_ns() < safety_deadline) {
        result = cl_read_frame(client, image.data(), CL_FRAME_BYTES, &info, 200);
        const auto receipt = monotonic_ns();
        if (result == CL_TIMEOUT) continue;
        if (result != CL_OK) { failed = true; break; }
        ++frames;
        const bool timely_frame = use_for_tracking(receipt, last_receipt);
        if (last_receipt) {
            const auto gap = receipt - last_receipt;
            maximum_gap = std::max(maximum_gap, gap);
            frame_gap_over16 += gap > 16 * ms;
        }
        last_receipt = receipt;
        const auto phase_observation = phase_tracker.observe(image, lanes);
        const auto phase = phase_observation.phase;
        const double elapsed_ms = static_cast<double>(receipt - local_origin_ns) / static_cast<double>(ms);
        if (phase == VisualPhase::unknown) {
            ++ocr_failures;
            // Retain failed OCR frames as well: timing distributions must be
            // reproducible from the log without filtering out slow/bad frames.
            observation_records.push_back({info.sequence, receipt, elapsed_ms, phase, 0, false});
            continue;
        }
        std::array<std::vector<int>, 16> visible;
        std::size_t count = 0;
        if (mode == "pixel") for (int lane = 0; lane < lanes; ++lane) {
            visible[static_cast<std::size_t>(lane)] = note_centers(image, lane, lanes);
            count += visible[static_cast<std::size_t>(lane)].size();
            if (phase == VisualPhase::calibration) {
                const int error = read_feedback(image, lane, lanes);
                if (error != std::numeric_limits<int>::min() && error != previous_feedback[static_cast<std::size_t>(lane)]) {
                    calibration_errors.push_back(error);
                    previous_feedback[static_cast<std::size_t>(lane)] = error;
                }
            }
        }
        {
            std::lock_guard lock(shared.mutex);
            if (timely_frame) for (int lane = 0; lane < lanes; ++lane)
                for (const int y : visible[static_cast<std::size_t>(lane)]) observe(shared, lane, y, elapsed_ms);
            if ((phase == VisualPhase::waiting || phase == VisualPhase::scored || phase == VisualPhase::finished) &&
                !shared.calibration_applied) {
                if (!calibration_errors.empty()) {
                    auto median = calibration_errors.begin() + static_cast<std::ptrdiff_t>(calibration_errors.size() / 2);
                    std::nth_element(calibration_errors.begin(), median, calibration_errors.end());
                    shared.correction_ns = -static_cast<std::int64_t>(*median) * 100'000;
                }
                shared.calibration_applied = true;
            }
            if (phase == VisualPhase::finished) { shared.stop = true; complete = true; }
        }
        shared.changed.notify_all();
        observation_records.push_back({info.sequence, receipt, elapsed_ms, phase, count,
                                       timely_frame && mode == "pixel", phase_observation.inferred});
        if (next_sample < sample_times.size() && elapsed_ms >= sample_times[next_sample]) {
            auto& sample = sample_frames[next_sample];
            std::copy(image.begin(), image.end(), sample.pixels.begin());
            sample.captured = true;
            ++next_sample;
        }
        if (complete) {
            auto& sample = sample_frames.back();
            std::copy(image.begin(), image.end(), sample.pixels.begin());
            sample.captured = true;
            break;
        }
    }
    {
        std::lock_guard lock(shared.mutex);
        shared.stop = true;
    }
    shared.changed.notify_all();
    scheduler.join();
    for (const auto& sample : sample_frames) {
        if (sample.captured) write_ppm(output / sample.filename, sample.pixels);
    }
    std::ofstream observations(output / "observations.csv");
    if (!observations) throw std::runtime_error("cannot create observation log");
    observations << "frame_sequence,receipt_monotonic_ns,local_elapsed_ms,visible_phase,note_bars,tracking_used,phase_inferred\n";
    observations << std::setprecision(12);
    for (const auto& observation : observation_records) {
        observations << observation.sequence << ',' << observation.receipt_ns << ','
                     << observation.local_elapsed_ms << ',' << phase_names[static_cast<std::size_t>(observation.phase)] << ','
                     << observation.note_bars << ',' << observation.tracking_used << ',' << observation.phase_inferred << '\n';
    }
    observations.close();
    if (!observations) throw std::runtime_error("failed writing observation log");
    std::ofstream taps(output / "taps.csv");
    taps << "visual_track_id,lane,estimated_target_local_ms,scheduled_monotonic_ns,sent_monotonic_ns,local_origin_ns,calibration_correction_ns,client_result\n";
    taps << std::setprecision(12);
    for (const auto& action : actions)
        taps << action.id << ',' << action.lane << ',' << action.estimated_target_ms << ',' << action.scheduled_ns << ','
             << action.sent_ns << ',' << action.local_origin_ns << ',' << action.correction_ns << ',' << action.result << '\n';
    std::ofstream tracks(output / "visual_tracks.csv");
    tracks << "visual_track_id,lane,estimated_target_local_ms,observations,first_receipt_local_ms,last_receipt_local_ms,tapped\n";
    tracks << std::setprecision(12);
    std::size_t track_count = 0;
    for (int lane = 0; lane < lanes; ++lane) for (const auto& track : shared.tracks[static_cast<std::size_t>(lane)]) {
        ++track_count;
        tracks << track.id << ',' << lane << ',' << track.target_ms << ',' << track.observations << ','
               << track.first_receipt_ms << ',' << track.last_receipt_ms << ',' << track.tapped << '\n';
    }
    std::ofstream summary(output / "agent.json");
    summary << "{\n  \"agent\":\"deterministic pixel-only diagnostic\",\n  \"mode\":\"" << mode << "\",\n"
            << "  \"observation\":\"only full RGBA pixels from public C ABI\",\n"
            << "  \"timing_method\":\"note position plus local receipt time; repeated trajectories; visual calibration\",\n"
            << "  \"phase_method\":\"visible labels; scored inferred from observed WAIT then intact unlabeled playfield; completion from FINISHED\",\n"
            << "  \"local_origin_ns\":" << local_origin_ns << ",\n"
            << "  \"completed_visually\":" << (complete ? "true" : "false") << ",\n"
            << "  \"transport_error\":" << (failed ? "true" : "false") << ",\n"
            << "  \"lanes\":" << lanes << ",\"frames\":" << frames << ",\"phase_ocr_failures\":" << ocr_failures << ",\n"
            << "  \"visual_tracks\":" << track_count << ",\"tap_commands\":" << actions.size() << ",\n"
            << "  \"calibration_samples\":" << calibration_errors.size()
            << ",\"calibration_correction_ms\":" << static_cast<double>(shared.correction_ns) / static_cast<double>(ms) << ",\n"
            << "  \"receipt_gap_max_ms\":" << static_cast<double>(maximum_gap) / static_cast<double>(ms)
            << ",\"receipt_gaps_over16ms\":" << frame_gap_over16 << "\n}\n";
    taps.close(); tracks.close(); summary.close();
    if (!taps || !tracks || !summary) throw std::runtime_error("failed writing diagnostic evidence");
    std::cout << "Visual calibration: " << calibration_errors.size() << " feedback samples; correction "
              << static_cast<double>(shared.correction_ns) / static_cast<double>(ms) << " ms\n";
    std::cout << "Visual diagnostic " << (complete ? "completed" : "incomplete") << ": " << frames << " frames, "
              << actions.size() << " taps, " << ocr_failures << " phase OCR failures. Evidence: " << output << '\n';
    return complete && !failed ? 0 : 2;
}
} // namespace

int main(int argc, char** argv) {
    try { return run(argc, argv); }
    catch (const std::exception& error) { std::cerr << "pixel_agent: " << error.what() << '\n'; return 1; }
}
