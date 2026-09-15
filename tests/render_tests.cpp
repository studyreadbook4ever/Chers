#include "chronolane/render.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <cstring>
#include <vector>

namespace {

void check(bool condition, const char* description) {
    if (!condition) {
        std::cerr << "FAIL: " << description << '\n';
        std::exit(1);
    }
}

void full_frame_lane_geometry() {
    using namespace chronolane;
    constexpr auto sentinel = std::uint32_t{0x12345678};
    for (const int lanes : {2, 3, 16}) {
        std::vector<std::uint32_t> pixels(kModelWidth * kModelHeight + 17, sentinel);
        std::array<RenderNote, 16> notes{};
        for (int lane = 0; lane < lanes; ++lane) notes[std::size_t(lane)] = {static_cast<std::uint8_t>(lane), 1'500'000'000};
        RenderState state{};
        state.lanes = lanes;
        state.now_ns = 1'000'000'000;
        state.phase = RenderPhase::scored;
        state.notes = std::span(notes).first(static_cast<std::size_t>(lanes));
        check(render_rgba(pixels, kModelWidth, kModelHeight, state), "valid render accepted");
        check(std::all_of(pixels.begin(), pixels.begin() + kModelWidth * kModelHeight,
                          [](auto pixel) { return (pixel >> 24U) == 255; }), "full frame opaque with no unwritten margins");
        check(std::all_of(pixels.begin() + kModelWidth * kModelHeight, pixels.end(),
                          [=](auto pixel) { return pixel == sentinel; }), "trailing storage is untouched");
        check(lane_left(0, kModelWidth, lanes) == 0, "first lane touches left image edge");
        check(lane_left(lanes, kModelWidth, lanes) == kModelWidth, "last lane touches right image edge");
        int narrowest = kModelWidth;
        int widest = 0;
        for (int lane = 0; lane < lanes; ++lane) {
            const int left = lane_left(lane, kModelWidth, lanes);
            const int right = lane_left(lane + 1, kModelWidth, lanes);
            narrowest = std::min(narrowest, right - left);
            widest = std::max(widest, right - left);
            for (int x = left; x < right; ++x) {
                const bool cyan = pixels[166 * kModelWidth + x] == kNoteBodyColor;
                check(cyan == (x >= left + 4 && x < right - 4), "each lane contains exactly its own note body");
            }
        }
        check(widest - narrowest <= 1, "non-divisible widths split evenly to within one pixel");
    }
}

void time_geometry_and_visibility() {
    using namespace chronolane;
    check(judgement_y(360) == 330, "canonical judgement line y330");
    check(judgement_y(720) == 660, "human view scales judgement line");
    check(note_y(2'000'000'000, 1'000'000'000, 360) == 0, "one second lead starts at top edge");
    check(note_y(2'000'000'000, 1'500'000'000, 360) == 165, "half second remaining is halfway");
    check(note_y(2'000'000'000, 2'000'000'000, 360) == 330, "due note center aligns with judgement line");
    check(note_y(2'000'000'000, 2'000'000'000, 720) == 660, "due note scales exactly");
    check(note_y(2'000'000'000, 2'020'000'000, 360) == 337, "late judgement window remains visible below line");
    check(note_y(2'000'000'000, 1'500'000'000, 360) -
          note_y(2'040'000'000, 1'500'000'000, 360) >= 13,
          "forty millisecond spacing leaves distinct six pixel notes");

    std::vector<std::uint32_t> pixels(kModelWidth * kModelHeight);
    RenderNote note{0, 2'000'000'000};
    RenderState state{};
    state.phase = RenderPhase::scored;
    state.notes = std::span(&note, 1);
    state.now_ns = 999'999'999;
    check(render_rgba(pixels, kModelWidth, kModelHeight, state), "future test render");
    check(std::find(pixels.begin(), pixels.end(), kNoteBodyColor) == pixels.end(), "future notes never leak before one second lookahead");
    state.now_ns = 1'000'000'000;
    check(render_rgba(pixels, kModelWidth, kModelHeight, state), "top-edge render");
    check(pixels[1 * kModelWidth + 160] == kNoteBodyColor, "initial clipped note appears at top edge");
    state.now_ns = 2'000'000'000;
    check(render_rgba(pixels, kModelWidth, kModelHeight, state), "due render");
    check(pixels[331 * kModelWidth + 160] == kNoteBodyColor, "due note retains body below center reference");
    state.now_ns = 2'020'000'001;
    check(render_rgba(pixels, kModelWidth, kModelHeight, state), "expired render");
    check(std::find(pixels.begin(), pixels.end(), kNoteBodyColor) == pixels.end(), "expired notes disappear after late window");
}

bool text_present(const std::vector<std::uint32_t>& pixels, int width, int height,
                  const char* value, int x, int y, int scale, std::uint32_t color) {
    using namespace chronolane;
    for (int glyph = 0; value[glyph] != '\0'; ++glyph) {
        for (int row = 0; row < 7; ++row) {
            const auto bits = font_row(value[glyph], row);
            for (int column = 0; column < 5; ++column) {
                for (int dy = 0; dy < scale; ++dy) {
                    for (int dx = 0; dx < scale; ++dx) {
                        const int px = x + glyph * 6 * scale + column * scale + dx;
                        const int py = y + row * scale + dy;
                        if (px < 0 || py < 0 || px >= width || py >= height) return false;
                        const bool ink = (bits & (1U << (4 - column))) != 0;
                        const auto pixel = pixels[std::size_t(py) * std::size_t(width) + std::size_t(px)];
                        if ((pixel == color) != ink) return false;
                    }
                }
            }
        }
    }
    return true;
}

void no_clock_and_pixel_format() {
    using namespace chronolane;
    check(std::endian::native == std::endian::little, "supported target is little endian");
    const auto bytes = std::bit_cast<std::array<std::uint8_t, 4>>(rgba(17, 29, 43, 59));
    check(bytes == std::array<std::uint8_t, 4>{17, 29, 43, 59}, "native pixel memory is RGBA");
    constexpr std::array phases{RenderPhase::ready, RenderPhase::preroll, RenderPhase::calibration,
                                RenderPhase::waiting, RenderPhase::scored, RenderPhase::finished};
    constexpr std::array names{"READY", "PREROLL", "CALIBRATION", "WAIT", "SCORED", "FINISHED"};
    for (const auto dimensions : {std::array{640, 360}, std::array{1280, 720}}) {
        const int width = dimensions[0];
        const int height = dimensions[1];
        const int scale = font_resolution_scale(width, height);
        std::vector<std::uint32_t> pixels(std::size_t(width) * std::size_t(height));
        RenderState state{};
        for (std::size_t phase = 0; phase < phases.size(); ++phase) {
            state.phase = phases[phase];
            for (const std::int64_t now : {1'000'000'000LL, 123'456'999'999LL}) {
                state.now_ns = now;
                state.phase_end_ns = now + 1'500'000'000;
                check(render_rgba(pixels, width, height, state), "clock-free HUD renders in every phase");
                for (int y = 0; y < 32 * scale; ++y) {
                    for (int x = width / 4; x < width * 3 / 4; ++x) {
                        check(pixels[std::size_t(y) * std::size_t(width) + std::size_t(x)] != kTextColor,
                              "elapsed clock never appears in the upper center of either image");
                    }
                }
                if (state.phase == RenderPhase::scored) {
                    for (int y = hud_y(height) + 3 * scale; y < hud_y(height) + 10 * scale; ++y)
                        for (int x = 8 * scale; x < 74 * scale; ++x)
                            check(pixels[std::size_t(y) * std::size_t(width) + std::size_t(x)] != rgba(130, 151, 175),
                                  "scored phase leaves no top-left label ink at model or human resolution");
                } else {
                    check(text_present(pixels, width, height, names[phase], 8 * scale,
                                       hud_y(height) + 3 * scale, scale, rgba(130, 151, 175)),
                          "all other phase labels stay at their public pixel location");
                }
                if (state.phase != RenderPhase::finished) {
                    check(text_present(pixels, width, height, "1.5 S", width - 38 * scale,
                                       hud_y(height) + 3 * scale, scale, rgba(130, 151, 175)),
                          "phase countdown remains at the upper right");
                }
            }
        }
    }
    check(font_row('a', 3) == font_row('A', 3), "lowercase glyphs map consistently");
    check(font_row('0', -1) == 0 && font_row('0', 7) == 0, "font rows are bounds checked");
    check(font_row('(', 3) == 8 && font_row(')', 3) == 2, "average parentheses have visible glyphs");
}

void result_delay_average() {
    using namespace chronolane;
    struct Case {
        RenderSummary summary;
        const char* total;
        const char* average;
    };
    const std::array cases{
        Case{{8, 2, 5, 3, 13, -7, 510'000'000, 10'000'000},
             "DELAYSUM  510.000 MS", "(AVERAGE 1.000 MS)"},
        Case{{16'126, 0, 0, 0, 16'126, 32'252, 4'244'998'000, 4'244'998'000},
             "DELAYSUM  4244.998 MS", "(AVERAGE 0.263 MS)"},
        Case{{1, 1, 0, 0, 2, 3, 1'999'001, 1'999'001},
             "DELAYSUM  1.999 MS", "(AVERAGE 1.000 MS)"},
        Case{{1, 1, 0, 0, 2, 3, 1'998'998, 1'998'998},
             "DELAYSUM  1.999 MS", "(AVERAGE 0.999 MS)"},
        Case{{0, 0, 2, 7, 7, -10, 200'000'000, 0},
             "DELAYSUM  200.000 MS", "(AVERAGE N/A)"},
        Case{{1, 0, 0, 0, 1, 2, std::numeric_limits<std::uint64_t>::max(),
              std::numeric_limits<std::uint64_t>::max()},
             "DELAYSUM  18446744073709.551 MS", "(AVERAGE 18446744073709.551 MS)"},
    };
    for (const auto dimensions : {std::array{640, 360}, std::array{1280, 720}}) {
        const int width = dimensions[0];
        const int height = dimensions[1];
        const int scale = font_resolution_scale(width, height);
        std::vector<std::uint32_t> pixels(std::size_t(width) * std::size_t(height));
        RenderState state{};
        state.phase = RenderPhase::finished;
        for (const auto& sample : cases) {
            state.summary = sample.summary;
            check(render_rgba(pixels, width, height, state), "result with inline average renders");
            const int total_width = static_cast<int>(std::strlen(sample.total)) * 12 * scale - 2 * scale;
            const int average_width = static_cast<int>(std::strlen(sample.average)) * 6 * scale - scale;
            const int combined_width = total_width + 12 * scale + average_width;
            const int left = (width - combined_width) / 2;
            check(left >= 8 * scale && left + combined_width <= width - 8 * scale,
                  "total and average fit together without clipping at either resolution");
            check(text_present(pixels, width, height, sample.total, left,
                               height * 46 / 100, 2 * scale, kTextColor),
                  "delay sum preserves penalties and three decimal places");
            check(text_present(pixels, width, height, sample.average,
                               left + total_width + 12 * scale,
                               height * 46 / 100 + 7 * scale, scale, rgba(130, 151, 175)),
                  "average excludes empty penalties, includes GOOD, rounds to three decimals, and handles zero hits");
        }
    }
}

void invalid_inputs_and_phase_overlays() {
    using namespace chronolane;
    std::array<std::uint32_t, 4> small{1, 2, 3, 4};
    const auto original = small;
    RenderState state{};
    check(!render_rgba(small, 640, 360, state), "undersized output rejected");
    check(!render_rgba(small, 0, 360, state), "zero width rejected");
    state.lanes = 17;
    check(!render_rgba(small, 2, 2, state), "seventeen lanes rejected");
    check(small == original, "invalid calls leave output unchanged");
    std::vector<std::uint32_t> pixels(kModelWidth * kModelHeight);
    state.lanes = 16;
    std::array<LaneFeedback, 16> feedback{};
    for (std::size_t lane = 0; lane < feedback.size(); ++lane) {
        feedback[lane] = {1'000'000'000, static_cast<std::int64_t>(lane) * 1'000'000 - 8'000'000,
                          lane % 3 == 0 ? RenderFeedbackKind::poor : RenderFeedbackKind::perfect};
    }
    state.feedback = feedback;
    state.now_ns = 1'010'000'000;
    state.phase_end_ns = 5'000'000'000;
    state.summary = {8, 2, 5, 3, 13, -7, 510'000'000};
    for (const auto phase : {RenderPhase::ready, RenderPhase::preroll, RenderPhase::calibration,
                             RenderPhase::waiting, RenderPhase::scored, RenderPhase::finished}) {
        state.phase = phase;
        check(render_rgba(pixels, kModelWidth, kModelHeight, state), "all phase and signed summary displays render");
    }
}

void zero_note_and_invalid_results() {
    using namespace chronolane;
    std::vector<std::uint32_t> pixels(kModelWidth * kModelHeight);
    RenderState state{};
    state.phase = RenderPhase::finished;
    const auto text_present = [&](const char* value, int x, int y, int scale,
                                  std::uint32_t color) {
        for (int glyph = 0; value[glyph] != '\0'; ++glyph) {
            for (int row = 0; row < 7; ++row) {
                const auto bits = font_row(value[glyph], row);
                for (int column = 0; column < 5; ++column) {
                    if ((bits & (1U << (4 - column))) == 0) continue;
                    const int px = x + glyph * 6 * scale + column * scale;
                    const int py = y + row * scale;
                    if (pixels[static_cast<std::size_t>(py) * kModelWidth + static_cast<std::size_t>(px)] != color)
                        return false;
                }
            }
        }
        return true;
    };
    check(render_rgba(pixels, kModelWidth, kModelHeight, state), "zero-note summary renders");
    check(text_present("SCORE  N/A", 261, 129, 2, rgba(130, 151, 175)),
          "zero-note denominator is visibly N/A");
    check(std::find(pixels.begin(), pixels.end(), rgba(137, 245, 172)) == pixels.end(),
          "zero-note result never paints the numeric-score success color");
    state.valid = false;
    state.summary = {8, 2, 0, 0, 10, 18, 5'000'000};
    check(render_rgba(pixels, kModelWidth, kModelHeight, state), "invalid populated summary renders");
    check(text_present("INVALID RUN", 222, 82, 3, rgba(255, 116, 138)),
          "invalid result has an explicit red headline");
    check(text_present("SCORE  N/A", 261, 129, 2, rgba(255, 116, 138)),
          "invalid result suppresses otherwise valid numeric score");
    state.phase = RenderPhase::scored;
    check(render_rgba(pixels, kModelWidth, kModelHeight, state), "active invalid run renders");
    check(text_present("INVALID RUN", 8, 24, 1, rgba(255, 116, 138)),
          "active invalidity is visibly indicated");
}

} // namespace

int main() {
    full_frame_lane_geometry();
    time_geometry_and_visibility();
    no_clock_and_pixel_format();
    result_delay_average();
    invalid_inputs_and_phase_overlays();
    zero_note_and_invalid_results();
    std::cout << "renderer tests passed\n";
}
