#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace chronolane {

using TimeNs = std::int64_t;
inline constexpr TimeNs kMs = 1'000'000;
inline constexpr TimeNs kSecond = 1'000'000'000;
inline constexpr TimeNs kPerfectWindow = 8 * kMs;
inline constexpr TimeNs kGoodWindow = 20 * kMs;
inline constexpr TimeNs kLaneLock = 40 * kMs;
inline constexpr TimeNs kEmptyDelayPenalty = 100 * kMs;
inline constexpr std::uint64_t kNoNote = UINT64_MAX;

enum class Stage : std::uint8_t { None, Calibration, Scored };

struct Config {
    std::uint32_t lanes = 4;
    double density = 24.0;
    void validate() const;
};

struct Timeline {
    static constexpr TimeNs calibration_start = kSecond;
    static constexpr TimeNs calibration_end = 6 * kSecond;
    static constexpr TimeNs calibration_tail_end = calibration_end + kGoodWindow;
    static constexpr TimeNs scored_start = calibration_tail_end + 5 * kSecond;
    static constexpr TimeNs scored_end = scored_start + 50 * kSecond;
    static constexpr TimeNs finish = scored_end + kGoodWindow;
    static constexpr TimeNs preview = kSecond;
    // Targets use [start,end). Input accounting includes the early/late windows.
    static Stage input_stage(TimeNs time_ns) noexcept;
};

struct Note {
    std::uint64_t id = 0;
    std::uint32_t lane = 0;
    TimeNs target_ns = 0;
    Stage stage = Stage::None;
    bool consumed = false;
};

class RandomSource {
public:
    virtual ~RandomSource() = default;
    virtual std::uint64_t next() = 0;
    virtual std::string source_label() const = 0;
};

// Throws when CPU RDSEED is unavailable or fails to supply entropy. No fallback.
class HardwareRandom final : public RandomSource {
public:
    HardwareRandom();
    std::uint64_t next() override;
    std::string source_label() const override;
};

struct GenerationStats {
    std::uint64_t attempts = 0;
    std::uint64_t emitted = 0;
    std::uint64_t dropped = 0;
};

struct Chart {
    std::vector<Note> notes;
    GenerationStats calibration;
    GenerationStats scored;
    std::string entropy_source;
};

Chart generate_chart(const Config& config, RandomSource& random);

enum class Judgment : std::uint8_t { Ignored, Perfect, Good, Empty, Invalid };

struct Feedback {
    TimeNs event_ns = 0;
    TimeNs delay_ns = 0; // Signed input minus target; zero for non-hits.
    std::uint64_t note_id = kNoNote;
    std::uint32_t lane = 0;
    Stage stage = Stage::None;
    Judgment kind = Judgment::Ignored;
};

using InputRecord = Feedback;

struct ScoreSummary {
    std::uint64_t notes = 0;
    std::uint64_t perfect = 0;
    std::uint64_t good = 0;
    std::uint64_t empty = 0;
    std::uint64_t missed = 0;
    std::int64_t raw_score = 0;
    TimeNs delay_sum_ns = 0;
    TimeNs hit_delay_sum_ns = 0;
    double normalized() const noexcept;
};

// Single-threaded core. Callers synchronize hit/read access externally.
// Runtime input/feedback never exposes scheduled note information externally;
// notes()/chart()/records() are private benchmark-side inspection interfaces.
class Engine {
public:
    static constexpr std::size_t kDefaultMaxEvents = 262'144;
    Engine(Config config, Chart chart, std::size_t max_events = kDefaultMaxEvents);
    Feedback hit(TimeNs time_ns, std::uint32_t lane);
    const Config& config() const noexcept { return config_; }
    const Chart& chart() const noexcept { return chart_; }
    const std::vector<Note>& notes() const noexcept { return chart_.notes; }
    const std::array<Feedback, 16>& last_feedback() const noexcept { return feedback_; }
    const std::vector<InputRecord>& records() const noexcept { return records_; }
    ScoreSummary results(Stage stage) const;
    bool valid() const noexcept { return invalid_reason_.empty(); }
    const std::string& invalid_reason() const noexcept { return invalid_reason_; }
    void invalidate(std::string reason);

private:
    Config config_;
    Chart chart_;
    std::size_t max_events_;
    std::vector<InputRecord> records_;
    std::array<std::vector<std::size_t>, 16> lane_notes_;
    std::array<std::size_t, 16> cursors_{};
    std::array<Feedback, 16> feedback_{};
    TimeNs last_input_ns_ = INT64_MIN;
    std::string invalid_reason_;
};

const char* stage_name(Stage stage) noexcept;
const char* judgment_name(Judgment judgment) noexcept;

} // namespace chronolane
