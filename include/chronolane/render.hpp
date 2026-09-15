#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

namespace chronolane {

// A pixel is RGBA8 in memory on the supported little-endian Linux target.
// SDL consumers should therefore use SDL_PIXELFORMAT_RGBA32.
[[nodiscard]] constexpr std::uint32_t rgba(std::uint8_t r, std::uint8_t g,
                                          std::uint8_t b, std::uint8_t a = 255) noexcept {
    return std::uint32_t(r) | (std::uint32_t(g) << 8U) |
           (std::uint32_t(b) << 16U) | (std::uint32_t(a) << 24U);
}

inline constexpr int kModelWidth = 640;
inline constexpr int kModelHeight = 360;
inline constexpr std::int64_t kNoteLeadNs = 1'000'000'000;
inline constexpr std::uint32_t kNoteBodyColor = rgba(40, 220, 240);
inline constexpr std::uint32_t kTextColor = rgba(211, 225, 239);

enum class RenderPhase : std::uint8_t { ready, preroll, calibration, waiting, scored, finished };
enum class RenderFeedbackKind : std::uint8_t { none, perfect, good, poor };

struct RenderNote {
    std::uint8_t lane{};          // Zero based; A = 0, P = 15.
    std::int64_t target_ns{};     // Same monotonic time domain as RenderState::now_ns.
};

struct LaneFeedback {
    std::int64_t timestamp_ns{};
    std::int64_t delay_ns{};      // Input minus target: negative = early.
    RenderFeedbackKind kind{RenderFeedbackKind::none};
};

struct RenderSummary {
    std::uint64_t perfect{};
    std::uint64_t good{};
    std::uint64_t poor{};
    std::uint64_t missed{};
    std::uint64_t total{};
    std::int64_t score_units{};
    std::uint64_t delay_sum_ns{}; // Includes 100 ms per poor input.
    std::uint64_t hit_delay_sum_ns{}; // Successful hits only; used for the mean error.
};

struct RenderState {
    int lanes{2};
    std::int64_t now_ns{};
    std::int64_t phase_start_ns{};
    std::int64_t phase_end_ns{};
    RenderPhase phase{RenderPhase::ready};
    bool valid{true};             // Invalid runs never present a numeric score.
    std::span<const RenderNote> notes{}; // Only unconsumed notes should be supplied.
    std::span<const LaneFeedback> feedback{};
    RenderSummary summary{};
};

[[nodiscard]] constexpr int lane_left(int lane, int width, int lanes) noexcept {
    return lanes > 0 ? static_cast<int>((static_cast<std::int64_t>(lane) * width) / lanes) : 0;
}

[[nodiscard]] constexpr int judgement_y(int height) noexcept {
    return static_cast<int>((static_cast<std::int64_t>(height) * 11) / 12);
}

// The center of a note starts at y=0 exactly one second before its target and
// reaches judgement_y(height) at its target. Rasterization rounds to the nearest
// pixel; subpixel timing remains available by fitting movement across frames.
[[nodiscard]] int note_y(std::int64_t target_ns, std::int64_t now_ns, int height) noexcept;

// HUD text uses the ordinary 5x7 bitmap font below, with a one-column
// inter-glyph gap. At larger resolutions its scale follows the model image.
[[nodiscard]] constexpr int font_resolution_scale(int width, int height) noexcept {
    const int sx = width / kModelWidth;
    const int sy = height / kModelHeight;
    const int scale = sx < sy ? sx : sy;
    return scale > 0 ? scale : 1;
}
[[nodiscard]] constexpr int hud_y(int height) noexcept {
    return static_cast<int>((static_cast<std::int64_t>(height) * 8) / kModelHeight);
}

// Returns a five-bit row, most-significant bit on the left. Supports A-Z,
// 0-9, space and punctuation used by the GUI. Lowercase maps to uppercase.
[[nodiscard]] std::uint8_t font_row(char character, int row) noexcept;

// No allocation, locks, syscalls or mutable globals. Caller owns storage and
// must not mutate the state spans until this call returns. Writes exactly
// width*height pixels. Invalid dimensions/lane counts/storage return false
// without modifying the output. Every valid pixel has opaque alpha.
[[nodiscard]] bool render_rgba(std::span<std::uint32_t> pixels, int width,
                               int height, const RenderState& state) noexcept;

} // namespace chronolane
