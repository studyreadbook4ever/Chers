#include "chronolane/core.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <unordered_set>

#if defined(__x86_64__) || defined(__i386__)
#include <cpuid.h>
#include <immintrin.h>
#endif

namespace chronolane {
namespace {

std::uint64_t uniform_below(RandomSource& random, std::uint64_t bound) {
    const std::uint64_t rejection_limit = -bound % bound;
    for (;;) {
        const auto word = random.next();
        if (word >= rejection_limit) return word % bound;
    }
}

#if defined(__x86_64__)
__attribute__((target("rdseed"))) bool hardware_word(std::uint64_t& word) {
    unsigned long long value;
    if (!_rdseed64_step(&value)) return false;
    word = static_cast<std::uint64_t>(value);
    return true;
}
#elif defined(__i386__)
__attribute__((target("rdseed"))) bool hardware_word(std::uint64_t& word) {
    unsigned int low, high;
    if (!_rdseed32_step(&low) || !_rdseed32_step(&high)) return false;
    word = (static_cast<std::uint64_t>(high) << 32) | low;
    return true;
}
#endif

bool target_in_stage(TimeNs time_ns, Stage stage) {
    if (stage == Stage::Calibration)
        return time_ns >= Timeline::calibration_start && time_ns < Timeline::calibration_end;
    if (stage == Stage::Scored)
        return time_ns >= Timeline::scored_start && time_ns < Timeline::scored_end;
    return false;
}

} // namespace

void Config::validate() const {
    if (lanes < 2 || lanes > 16)
        throw std::invalid_argument("lanes must be an integer from 2 through 16");
    if (!std::isfinite(density) || density < 2.0 * lanes || density > 24.0 * lanes)
        throw std::invalid_argument("density must be finite and between 2*lanes and 24*lanes attempts/second");
}

Stage Timeline::input_stage(TimeNs time_ns) noexcept {
    if (time_ns >= calibration_start - kGoodWindow && time_ns < calibration_tail_end)
        return Stage::Calibration;
    if (time_ns >= scored_start - kGoodWindow && time_ns < finish)
        return Stage::Scored;
    return Stage::None;
}

HardwareRandom::HardwareRandom() {
#if defined(__x86_64__) || defined(__i386__)
    unsigned int a, b, c, d;
    if (__get_cpuid_max(0, nullptr) >= 7 && __get_cpuid_count(7, 0, &a, &b, &c, &d) &&
        (b & (1u << 18))) return;
#endif
    throw std::runtime_error("hardware entropy unavailable: this build requires CPU RDSEED; no pseudorandom fallback is permitted");
}

std::uint64_t HardwareRandom::next() {
#if defined(__x86_64__) || defined(__i386__)
    std::uint64_t word;
    // Intel requires checking the carry flag and retrying: entropy can be busy.
    // This happens only during chart preparation, never on the input path.
    for (unsigned int retry = 0; retry < 1'000'000; ++retry) {
        if (hardware_word(word)) return word;
        _mm_pause();
    }
#endif
    throw std::runtime_error("RDSEED exhausted its retry budget while preparing the chart; run aborted");
}

std::string HardwareRandom::source_label() const {
    return "CPU RDSEED hardware entropy (no PRNG expansion)";
}

Chart generate_chart(const Config& config, RandomSource& random) {
    config.validate();
    Chart chart;
    chart.entropy_source = random.source_label();
    chart.notes.reserve(static_cast<std::size_t>(config.lanes) * 25 * 55);
    std::array<TimeNs, 16> next_available{};
    auto generate_stage = [&](Stage stage, TimeNs start, TimeNs end, GenerationStats& stats) {
        TimeNs target = start;
        for (;;) {
            // Uniform in [0,1), then the inverse CDF of an exponential wait.
            // A one-nanosecond floor ensures progress after integer rounding.
            const double uniform = static_cast<double>(random.next() >> 11) * 0x1.0p-53;
            const double wait_ns = -std::log1p(-uniform) * static_cast<double>(kSecond) / config.density;
            const auto wait = std::max<TimeNs>(1, static_cast<TimeNs>(std::llround(wait_ns)));
            if (wait >= end - target) break;
            target += wait;
            ++stats.attempts;
            std::array<std::uint32_t, 16> eligible{};
            std::uint64_t count = 0;
            for (std::uint32_t lane = 0; lane < config.lanes; ++lane)
                if (target >= next_available[lane]) eligible[count++] = lane;
            if (count == 0) {
                ++stats.dropped;
                continue;
            }
            const auto lane = eligible[uniform_below(random, count)];
            chart.notes.push_back({chart.notes.size(), lane, target, stage, false});
            next_available[lane] = target + kLaneLock;
            ++stats.emitted;
        }
    };
    generate_stage(Stage::Calibration, Timeline::calibration_start, Timeline::calibration_end, chart.calibration);
    generate_stage(Stage::Scored, Timeline::scored_start, Timeline::scored_end, chart.scored);
    return chart;
}

double ScoreSummary::normalized() const noexcept {
    return notes ? static_cast<double>(raw_score) / (2.0 * static_cast<double>(notes)) :
        std::numeric_limits<double>::quiet_NaN();
}

Engine::Engine(Config config, Chart chart, std::size_t max_events)
    : config_(config), chart_(std::move(chart)), max_events_(max_events) {
    config_.validate();
    if (max_events == 0) throw std::invalid_argument("input log capacity must be positive");
    // Keep the worst-case score and delay accumulator within signed 64 bits.
    if (max_events > static_cast<std::uint64_t>(INT64_MAX / kEmptyDelayPenalty))
        throw std::invalid_argument("input log capacity exceeds accumulator range");
    records_.reserve(max_events_);
    std::array<TimeNs, 16> previous{};
    previous.fill(INT64_MIN);
    TimeNs previous_target = INT64_MIN;
    std::unordered_set<std::uint64_t> ids;
    ids.reserve(chart_.notes.size());
    for (std::size_t index = 0; index < chart_.notes.size(); ++index) {
        const Note& note = chart_.notes[index];
        if (note.lane >= config_.lanes || note.consumed || !target_in_stage(note.target_ns, note.stage) ||
            note.target_ns < previous_target || note.id == kNoNote || !ids.insert(note.id).second)
            throw std::invalid_argument("chart has an invalid, consumed, duplicate, or unsorted note");
        if (previous[note.lane] != INT64_MIN && note.target_ns - previous[note.lane] < kLaneLock)
            throw std::invalid_argument("chart violates the 40ms per-lane note lock");
        previous[note.lane] = note.target_ns;
        previous_target = note.target_ns;
        lane_notes_[note.lane].push_back(index);
    }
}

Feedback Engine::hit(TimeNs time_ns, std::uint32_t lane) {
    if (lane >= config_.lanes) throw std::invalid_argument("input lane is outside this run's lane range");
    Feedback event{time_ns, 0, kNoNote, lane, Timeline::input_stage(time_ns), Judgment::Ignored};
    if (!valid()) {
        event.kind = Judgment::Invalid;
        return event;
    }
    if (time_ns < last_input_ns_) {
        invalid_reason_ = "input timestamps went backwards";
        event.kind = Judgment::Invalid;
        return event;
    }
    last_input_ns_ = time_ns;
    if (event.stage == Stage::None) return event;
    if (records_.size() == max_events_) {
        invalid_reason_ = "input log capacity exceeded; no valid benchmark score may be reported";
        event.kind = Judgment::Invalid;
        feedback_[lane] = event;
        return event;
    }

    auto& cursor = cursors_[lane];
    const auto& indices = lane_notes_[lane];
    while (cursor < indices.size()) {
        const auto& note = chart_.notes[indices[cursor]];
        if (!note.consumed && note.target_ns >= time_ns - kGoodWindow) break;
        ++cursor;
    }
    event.kind = Judgment::Empty;
    if (cursor < indices.size()) {
        auto& note = chart_.notes[indices[cursor]];
        if (note.stage == event.stage && note.target_ns <= time_ns + kGoodWindow) {
            event.note_id = note.id;
            event.delay_ns = time_ns - note.target_ns;
            event.kind = std::abs(event.delay_ns) <= kPerfectWindow ? Judgment::Perfect : Judgment::Good;
            note.consumed = true;
            ++cursor;
        }
    }
    // Reserve happened at construction: this cannot allocate on the input path.
    records_.push_back(event);
    feedback_[lane] = event;
    return event;
}

ScoreSummary Engine::results(Stage stage) const {
    ScoreSummary score;
    if (stage == Stage::None) return score;
    for (const auto& note : chart_.notes)
        if (note.stage == stage) ++score.notes;
    for (const auto& event : records_) {
        if (event.stage != stage) continue;
        if (event.kind == Judgment::Perfect || event.kind == Judgment::Good) {
            if (event.kind == Judgment::Perfect) {
                ++score.perfect;
                score.raw_score += 2;
            } else {
                ++score.good;
                ++score.raw_score;
            }
            score.hit_delay_sum_ns += std::abs(event.delay_ns);
        } else if (event.kind == Judgment::Empty) {
            ++score.empty;
            score.raw_score -= 5;
        }
    }
    score.missed = score.notes - score.perfect - score.good;
    score.delay_sum_ns = score.hit_delay_sum_ns + static_cast<TimeNs>(score.empty) * kEmptyDelayPenalty;
    return score;
}

void Engine::invalidate(std::string reason) {
    if (valid()) invalid_reason_ = reason.empty() ? "run invalidated by host" : std::move(reason);
}

const char* stage_name(Stage stage) noexcept {
    switch (stage) {
        case Stage::Calibration: return "calibration";
        case Stage::Scored: return "scored";
        default: return "none";
    }
}

const char* judgment_name(Judgment judgment) noexcept {
    switch (judgment) {
        case Judgment::Perfect: return "perfect";
        case Judgment::Good: return "good";
        case Judgment::Empty: return "empty";
        case Judgment::Invalid: return "invalid";
        default: return "ignored";
    }
}

} // namespace chronolane
