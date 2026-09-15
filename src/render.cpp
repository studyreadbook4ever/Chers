#include "chronolane/render.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <limits>

namespace chronolane {
namespace {

constexpr std::array<std::array<std::uint8_t, 7>, 10> digits{{
    {{14, 17, 19, 21, 25, 17, 14}}, // 0
    {{4, 12, 4, 4, 4, 4, 14}},
    {{14, 17, 1, 2, 4, 8, 31}},
    {{30, 1, 1, 14, 1, 1, 30}},
    {{2, 6, 10, 18, 31, 2, 2}},
    {{31, 16, 16, 30, 1, 1, 30}},
    {{14, 16, 16, 30, 17, 17, 14}},
    {{31, 1, 2, 4, 8, 8, 8}},
    {{14, 17, 17, 14, 17, 17, 14}},
    {{14, 17, 17, 15, 1, 1, 14}},
}};

constexpr std::array<std::array<std::uint8_t, 7>, 26> letters{{
    {{14, 17, 17, 31, 17, 17, 17}}, // A
    {{30, 17, 17, 30, 17, 17, 30}},
    {{14, 17, 16, 16, 16, 17, 14}},
    {{30, 17, 17, 17, 17, 17, 30}},
    {{31, 16, 16, 30, 16, 16, 31}},
    {{31, 16, 16, 30, 16, 16, 16}},
    {{14, 17, 16, 23, 17, 17, 15}},
    {{17, 17, 17, 31, 17, 17, 17}},
    {{14, 4, 4, 4, 4, 4, 14}},
    {{7, 2, 2, 2, 18, 18, 12}},
    {{17, 18, 20, 24, 20, 18, 17}},
    {{16, 16, 16, 16, 16, 16, 31}},
    {{17, 27, 21, 21, 17, 17, 17}},
    {{17, 25, 21, 19, 17, 17, 17}},
    {{14, 17, 17, 17, 17, 17, 14}},
    {{30, 17, 17, 30, 16, 16, 16}},
    {{14, 17, 17, 17, 21, 18, 13}},
    {{30, 17, 17, 30, 20, 18, 17}},
    {{15, 16, 16, 14, 1, 1, 30}},
    {{31, 4, 4, 4, 4, 4, 4}},
    {{17, 17, 17, 17, 17, 17, 14}},
    {{17, 17, 17, 17, 17, 10, 4}},
    {{17, 17, 17, 21, 21, 21, 10}},
    {{17, 17, 10, 4, 10, 17, 17}},
    {{17, 17, 10, 4, 4, 4, 4}},
    {{31, 1, 2, 4, 8, 16, 31}},
}};

constexpr std::array<std::uint32_t, 16> accents{{
    rgba(81, 169, 255), rgba(128, 146, 255), rgba(176, 128, 255), rgba(231, 128, 242),
    rgba(255, 131, 174), rgba(255, 154, 125), rgba(255, 185, 112), rgba(247, 213, 111),
    rgba(205, 227, 123), rgba(151, 227, 151), rgba(104, 223, 185), rgba(93, 214, 216),
    rgba(111, 204, 251), rgba(150, 187, 255), rgba(191, 167, 244), rgba(225, 151, 209),
}};

constexpr auto text_color = kTextColor;
constexpr auto muted_color = rgba(130, 151, 175);
constexpr auto shadow_color = rgba(5, 11, 20);

class Canvas {
public:
    Canvas(std::span<std::uint32_t> pixels, int width, int height) noexcept
        : pixels_(pixels), width_(width), height_(height) {}

    void rect(int x, int y, int width, int height, std::uint32_t color) noexcept {
        if (width <= 0 || height <= 0) return;
        const auto left = std::clamp<std::int64_t>(x, 0, width_);
        const auto top = std::clamp<std::int64_t>(y, 0, height_);
        const auto right = std::clamp<std::int64_t>(std::int64_t(x) + width, 0, width_);
        const auto bottom = std::clamp<std::int64_t>(std::int64_t(y) + height, 0, height_);
        if (left >= right || top >= bottom) return;
        for (auto row = top; row < bottom; ++row) {
            const auto begin = std::size_t(row) * std::size_t(width_) + std::size_t(left);
            std::fill_n(pixels_.data() + begin, std::size_t(right - left), color);
        }
    }

    void text(const char* value, int x, int y, int scale, std::uint32_t color) noexcept {
        for (; *value != '\0'; ++value, x += 6 * scale) {
            for (int row = 0; row < 7; ++row) {
                const auto bits = font_row(*value, row);
                for (int column = 0; column < 5; ++column) {
                    if ((bits & (1U << (4 - column))) != 0) {
                        rect(x + column * scale, y + row * scale, scale, scale, color);
                    }
                }
            }
        }
    }

    void labeled_text(const char* value, int x, int y, int scale,
                      std::uint32_t color = text_color) noexcept {
        text(value, x + 1, y + 1, scale, shadow_color);
        text(value, x, y, scale, color);
    }

    void centered_text(const char* value, int y, int scale,
                       std::uint32_t color = text_color) noexcept {
        std::size_t length = 0;
        for (const char* p = value; *p != '\0'; ++p) ++length;
        const int text_width = static_cast<int>(length) * 6 * scale - scale;
        labeled_text(value, (width_ - text_width) / 2, y, scale, color);
    }

    void centered_inline_text(const char* value, const char* detail, int y,
                              int scale, int detail_scale) noexcept {
        int length = 0;
        int detail_length = 0;
        for (const char* p = value; *p != '\0'; ++p) ++length;
        for (const char* p = detail; *p != '\0'; ++p) ++detail_length;
        const int text_width = length * 6 * scale - scale;
        const int detail_width = detail_length * 6 * detail_scale - detail_scale;
        const int gap = 6 * scale;
        const int left = (width_ - text_width - gap - detail_width) / 2;
        labeled_text(value, left, y, scale);
        labeled_text(detail, left + text_width + gap,
                     y + 7 * (scale - detail_scale), detail_scale, muted_color);
    }

private:
    std::span<std::uint32_t> pixels_;
    int width_;
    int height_;
};

[[nodiscard]] const char* phase_name(RenderPhase phase) noexcept {
    switch (phase) {
    case RenderPhase::ready: return "READY";
    case RenderPhase::preroll: return "PREROLL";
    case RenderPhase::calibration: return "CALIBRATION";
    case RenderPhase::waiting: return "WAIT";
    case RenderPhase::scored: return "SCORED";
    case RenderPhase::finished: return "FINISHED";
    }
    return "READY";
}

[[nodiscard]] std::uint32_t feedback_color(RenderFeedbackKind kind) noexcept {
    switch (kind) {
    case RenderFeedbackKind::perfect: return rgba(137, 245, 172);
    case RenderFeedbackKind::good: return rgba(255, 208, 121);
    case RenderFeedbackKind::poor: return rgba(255, 116, 138);
    case RenderFeedbackKind::none: return muted_color;
    }
    return muted_color;
}

void draw_summary(Canvas& canvas, const RenderSummary& summary, bool valid, int height, int scale) noexcept {
    char buffer[100]{};
    canvas.centered_text(valid ? "RUN COMPLETE" : "INVALID RUN", height * 23 / 100,
                         3 * scale, valid ? text_color : rgba(255, 116, 138));
    if (valid && summary.total != 0) {
        const double percent = 50.0 * static_cast<double>(summary.score_units) /
                               static_cast<double>(summary.total);
        std::snprintf(buffer, sizeof(buffer), "SCORE  %.3f%%", percent);
        canvas.centered_text(buffer, height * 36 / 100, 2 * scale, rgba(137, 245, 172));
    } else {
        canvas.centered_text("SCORE  N/A", height * 36 / 100, 2 * scale,
                             valid ? muted_color : rgba(255, 116, 138));
    }
    std::snprintf(buffer, sizeof(buffer), "DELAYSUM  %.3f MS",
                  static_cast<double>(summary.delay_sum_ns) / 1'000'000.0);
    char average[64]{};
    const double hits = static_cast<double>(summary.perfect) + static_cast<double>(summary.good);
    if (hits > 0.0) {
        std::snprintf(average, sizeof(average), "(AVERAGE %.3f MS)",
                      static_cast<double>(summary.hit_delay_sum_ns) / 1'000'000.0 / hits);
    } else {
        std::snprintf(average, sizeof(average), "(AVERAGE N/A)");
    }
    canvas.centered_inline_text(buffer, average, height * 46 / 100, 2 * scale, scale);
    std::snprintf(buffer, sizeof(buffer), "PERFECT %llu   GOOD %llu",
                  static_cast<unsigned long long>(summary.perfect),
                  static_cast<unsigned long long>(summary.good));
    canvas.centered_text(buffer, height * 60 / 100, scale);
    std::snprintf(buffer, sizeof(buffer), "EMPTY %llu   MISS %llu   NOTES %llu",
                  static_cast<unsigned long long>(summary.poor),
                  static_cast<unsigned long long>(summary.missed),
                  static_cast<unsigned long long>(summary.total));
    canvas.centered_text(buffer, height * 67 / 100, scale);
}

} // namespace

std::uint8_t font_row(char character, int row) noexcept {
    if (row < 0 || row >= 7) return 0;
    if (character >= 'a' && character <= 'z') character = char(character - 'a' + 'A');
    if (character >= '0' && character <= '9') return digits[std::size_t(character - '0')][std::size_t(row)];
    if (character >= 'A' && character <= 'Z') return letters[std::size_t(character - 'A')][std::size_t(row)];
    switch (character) {
    case '-': return row == 3 ? 14 : 0;
    case '+': return row == 3 ? 14 : ((row == 2 || row == 4) ? 4 : 0);
    case '=': return (row == 2 || row == 4) ? 31 : 0;
    case '.': return row == 6 ? 4 : 0;
    case ':': return (row == 2 || row == 5) ? 4 : 0;
    case '/': return row >= 1 && row <= 5 ? std::uint8_t(1U << (row - 1)) : 0;
    case '(': {
        constexpr std::array<std::uint8_t, 7> glyph{{2, 4, 8, 8, 8, 4, 2}};
        return glyph[std::size_t(row)];
    }
    case ')': {
        constexpr std::array<std::uint8_t, 7> glyph{{8, 4, 2, 2, 2, 4, 8}};
        return glyph[std::size_t(row)];
    }
    case '%': {
        constexpr std::array<std::uint8_t, 7> glyph{{25, 26, 2, 4, 8, 11, 19}};
        return glyph[std::size_t(row)];
    }
    default: return 0;
    }
}

int note_y(std::int64_t target_ns, std::int64_t now_ns, int height) noexcept {
    // Convert before subtraction so invalid outside callers cannot cause signed
    // overflow. Runtime timestamps are comfortably within exact double range.
    const double remaining = static_cast<double>(target_ns) - static_cast<double>(now_ns);
    const double position = static_cast<double>(judgement_y(height)) *
                            (1.0 - remaining / static_cast<double>(kNoteLeadNs));
    if (position <= std::numeric_limits<int>::min()) return std::numeric_limits<int>::min();
    if (position >= std::numeric_limits<int>::max()) return std::numeric_limits<int>::max();
    return static_cast<int>(std::lround(position));
}

bool render_rgba(std::span<std::uint32_t> pixels, int width, int height,
                 const RenderState& state) noexcept {
    if (width <= 0 || height <= 0 || state.lanes < 2 || state.lanes > 16) return false;
    const auto w = static_cast<std::size_t>(width);
    const auto h = static_cast<std::size_t>(height);
    if (w > std::numeric_limits<std::size_t>::max() / h || pixels.size() < w * h) return false;
    // Restrict pathological dimensions before font and rectangle arithmetic.
    if (width > 32768 || height > 32768) return false;

    Canvas canvas(pixels, width, height);
    const int scale = font_resolution_scale(width, height);
    const int line_y = judgement_y(height);
    for (int lane = 0; lane < state.lanes; ++lane) {
        const int left = lane_left(lane, width, state.lanes);
        const int right = lane_left(lane + 1, width, state.lanes);
        for (int y = 0; y < height; ++y) {
            const auto gradient = static_cast<std::uint8_t>(6 * y / height);
            const auto extra = static_cast<std::uint8_t>((lane % 2) * 3);
            canvas.rect(left, y, right - left, 1,
                        rgba(std::uint8_t(9 + extra + gradient),
                             std::uint8_t(16 + extra + gradient),
                             std::uint8_t(28 + extra + gradient)));
        }
        if (lane > 0) canvas.rect(left, 0, std::max(1, width / 640), height, rgba(38, 51, 70));
    }
    // Reference lines remain subordinate to note pixels and occupy no margins.
    for (int index = 1; index < 11; ++index) {
        canvas.rect(0, height * index / 12, width, 1, rgba(25, 37, 53));
    }
    canvas.rect(0, line_y - 2 * scale, width, 5 * scale, rgba(29, 53, 74));
    canvas.rect(0, line_y, width, scale, rgba(133, 156, 179));

    for (int lane = 0; lane < state.lanes; ++lane) {
        const int left = lane_left(lane, width, state.lanes);
        const int right = lane_left(lane + 1, width, state.lanes);
        const int center = (left + right) / 2;
        canvas.rect(center - 2 * scale, line_y - 3 * scale, 5 * scale, 7 * scale, accents[std::size_t(lane)]);
        const char label[2]{static_cast<char>('A' + lane), '\0'};
        canvas.labeled_text(label, center - 5 * scale, height - 17 * scale, 2 * scale, accents[std::size_t(lane)]);
        if (std::size_t(lane) >= state.feedback.size()) continue;
        const auto& feedback = state.feedback[std::size_t(lane)];
        if (feedback.kind == RenderFeedbackKind::none || state.now_ns < feedback.timestamp_ns) continue;
        const auto age = static_cast<std::uint64_t>(state.now_ns) - static_cast<std::uint64_t>(feedback.timestamp_ns);
        if (age > 700'000'000ULL) continue;
        const auto color = feedback_color(feedback.kind);
        if (age < 100'000'000ULL) canvas.rect(left + scale, line_y + 2 * scale, right - left - 2 * scale, scale, color);
        char value[32]{};
        if (feedback.kind == RenderFeedbackKind::poor) {
            std::snprintf(value, sizeof(value), "POOR");
        } else {
            std::snprintf(value, sizeof(value), "%+.1f", static_cast<double>(feedback.delay_ns) / 1'000'000.0);
        }
        std::size_t length = 0;
        for (const char* p = value; *p != '\0'; ++p) ++length;
        const int feedback_width = static_cast<int>(length) * 6 * scale - scale;
        canvas.labeled_text(value, center - feedback_width / 2, line_y + 4 * scale, scale, color);
    }

    bool visible_notes = false;
    for (const auto& note : state.notes) {
        if (note.lane >= state.lanes) continue;
        const double remaining = static_cast<double>(note.target_ns) - static_cast<double>(state.now_ns);
        // Model must never see more than the published one-second lookahead.
        if (remaining > static_cast<double>(kNoteLeadNs) || remaining < -20'000'000.0) continue;
        visible_notes = true;
        const int left = lane_left(note.lane, width, state.lanes);
        const int right = lane_left(note.lane + 1, width, state.lanes);
        const int y = note_y(note.target_ns, state.now_ns, height);
        const int inset = std::max(2, width / 320);
        const int note_width = right - left - 2 * inset;
        if (note_width <= 0) continue;
        const int note_height = std::max(3, height / 60);
        const int top = y - note_height / 2;
        canvas.rect(left + inset, top, note_width, note_height, kNoteBodyColor);
        // A white center is the unambiguous timing reference; colored caps
        // distinguish lanes while the cyan body stays identical in all lanes.
        canvas.rect(left + inset, y, note_width, std::max(1, height / 360), rgba(204, 252, 255));
        canvas.rect(left + inset, top, std::max(1, width / 320), note_height, accents[note.lane]);
        canvas.rect(right - inset - std::max(1, width / 320), top,
                    std::max(1, width / 320), note_height, accents[note.lane]);
    }

    if (state.phase != RenderPhase::scored) {
        canvas.labeled_text(phase_name(state.phase), 8 * scale, hud_y(height) + 3 * scale, scale, muted_color);
    }
    if (!state.valid) {
        canvas.labeled_text("INVALID RUN", 8 * scale, hud_y(height) + 16 * scale,
                            scale, rgba(255, 116, 138));
    }
    if (state.phase != RenderPhase::finished && state.phase_end_ns > state.now_ns) {
        const double seconds = (static_cast<double>(state.phase_end_ns) - static_cast<double>(state.now_ns)) / 1'000'000'000.0;
        char countdown[32]{};
        std::snprintf(countdown, sizeof(countdown), "%.1f S", seconds);
        std::size_t length = 0;
        for (const char* p = countdown; *p != '\0'; ++p) ++length;
        canvas.labeled_text(countdown, width - static_cast<int>(length) * 6 * scale - 8 * scale,
                            hud_y(height) + 3 * scale, scale, muted_color);
    }
    if (state.phase == RenderPhase::ready) {
        canvas.centered_text("CHERS", height * 30 / 100, 4 * scale);
        canvas.centered_text("VISUAL TIMING BENCHMARK", height * 44 / 100, scale, muted_color);
        canvas.centered_text("CALIBRATE 5S  /  WAIT 5S  /  SCORE 50S", height * 57 / 100, scale);
        canvas.centered_text("PERFECT +/-8MS   GOOD +/-20MS", height * 64 / 100, scale, muted_color);
        canvas.centered_text("SPACE / ENTER TO START    ESC TO EXIT", height * 75 / 100, scale);
    } else if (state.phase == RenderPhase::preroll && !visible_notes) {
        canvas.centered_text("CALIBRATION STARTING", height * 38 / 100, 2 * scale);
    } else if (state.phase == RenderPhase::waiting && !visible_notes) {
        canvas.centered_text("CALIBRATION COMPLETE", height * 31 / 100, 2 * scale);
        canvas.centered_text("SCORED RUN STARTS NEXT", height * 43 / 100, scale, muted_color);
    } else if (state.phase == RenderPhase::finished) {
        draw_summary(canvas, state.summary, state.valid, height, scale);
    }
    return true;
}

} // namespace chronolane
