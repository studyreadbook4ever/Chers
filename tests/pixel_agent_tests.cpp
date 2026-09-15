// Exercise the pixel diagnostic against synthetic images and timing tracks.
// The production diagnostic still links only the public client library.
#define main pixel_agent_program_main
#include "../examples/pixel_agent.cpp"
#undef main

namespace {
void require(bool condition, const char* label) {
    if (!condition) throw std::runtime_error(label);
}

void draw_digit(std::vector<std::uint32_t>& image, int digit, int x, int y,
                int scale, std::uint32_t color) {
    for (int row = 0; row < 7; ++row) for (int col = 0; col < 5; ++col) {
        if ((digits[static_cast<std::size_t>(digit)][static_cast<std::size_t>(row)] & (1U << (4 - col))) == 0) continue;
        for (int dy = 0; dy < scale; ++dy) for (int dx = 0; dx < scale; ++dx)
            image[static_cast<std::size_t>(y + row * scale + dy) * width + static_cast<std::size_t>(x + col * scale + dx)] = color;
    }
}

void draw_phase(std::vector<std::uint32_t>& image, VisualPhase phase) {
    const auto text = phase_names[static_cast<std::size_t>(phase)];
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto& glyph = letters[static_cast<std::size_t>(text[i] - 'A')];
        for (int row = 0; row < 7; ++row) for (int col = 0; col < 5; ++col)
            if ((glyph[static_cast<std::size_t>(row)] & (1U << (4 - col))) != 0)
                image[static_cast<std::size_t>(11 + row) * width + 8 + i * 6 + static_cast<std::size_t>(col)] = phase_color;
    }
}

void test_phases_and_notes() {
    std::vector<std::uint32_t> image(width * height, pixel(9, 16, 28));
    require(read_phase(image) == VisualPhase::unknown, "blank image is not a known phase");
    require(read_phase({}) == VisualPhase::unknown, "undersized image cannot be read as a phase");
    for (std::size_t phase = 1; phase < phase_names.size(); ++phase) {
        if (phase == static_cast<std::size_t>(VisualPhase::scored)) continue;
        std::fill(image.begin(), image.end(), pixel(9, 16, 28));
        const auto text = phase_names[phase];
        draw_phase(image, static_cast<VisualPhase>(phase));
        require(read_phase(image) == static_cast<VisualPhase>(phase),
                "remaining visual labels identify phases and completion without a clock");
        // Flip the observed text-color membership, regardless of whether the
        // first glyph starts with a foreground or background pixel.
        image[11 * width + 8] = (letters[static_cast<std::size_t>(text[0] - 'A')][0] & 16U) != 0
            ? pixel(9, 16, 28) : phase_color;
        require(read_phase(image) == VisualPhase::unknown, "damaged phase is rejected instead of inferred from time");
    }
    for (int lanes : {2, 3, 16}) {
        std::fill(image.begin(), image.end(), pixel(9, 16, 28));
        for (int lane = 0; lane < lanes; ++lane) for (int center_y : {0, 40, 53, 330, 337}) {
            for (int y = std::max(0, center_y - 3); y <= center_y + 2; ++y)
                for (int x = lane * width / lanes + 4; x < (lane + 1) * width / lanes - 4; ++x)
                    image[static_cast<std::size_t>(y) * width + static_cast<std::size_t>(x)] = y == center_y ? note_center_color : note_body_color;
        }
        for (int lane = 0; lane < lanes; ++lane) {
            const auto observed = note_centers(image, lane, lanes);
            require(observed == std::vector<int>({337, 330, 53, 40, 0}), "note centers include top and late-window edges in temporal order");
        }
    }
}

void test_unlabeled_scored_continuity() {
    for (const int lanes : {2, 8, 16}) {
        std::vector<std::uint32_t> playfield(width * height, pixel(9, 16, 28));
        for (int x = 0; x < width; ++x) playfield[330 * width + static_cast<std::size_t>(x)] = pixel(133, 156, 179);
        PhaseTracker tracker;
        require(tracker.observe(playfield, lanes).phase == VisualPhase::unknown,
                "unlabeled playfield alone cannot establish scored phase");
        auto labeled = playfield;
        draw_phase(labeled, VisualPhase::calibration);
        require(tracker.observe(labeled, lanes).phase == VisualPhase::calibration, "calibration label is observed");
        require(tracker.observe(playfield, lanes).phase == VisualPhase::unknown,
                "lost calibration label cannot skip the required observed WAIT");
        labeled = playfield;
        draw_phase(labeled, VisualPhase::waiting);
        const auto wait = tracker.observe(labeled, lanes);
        require(wait.phase == VisualPhase::waiting && !wait.inferred, "WAIT is actual visible phase evidence");

        std::vector<std::uint32_t> damaged(width * height, pixel(0, 0, 0));
        require(tracker.observe(damaged, lanes).phase == VisualPhase::unknown,
                "blank or missing playfield after WAIT is not scored gameplay");
        labeled[11 * width + 9] = phase_color; // Corrupt a background pixel of W.
        require(tracker.observe(labeled, lanes).phase == VisualPhase::unknown,
                "damaged WAIT lettering is rejected, not treated as disappearance");

        auto inferred = tracker.observe(playfield, lanes);
        require(inferred.phase == VisualPhase::scored && inferred.inferred,
                "intact unlabeled playfield following visible WAIT establishes scored phase");
        require(tracker.observe({}, lanes).phase == VisualPhase::unknown,
                "undersized unexpected frame is safely rejected during scored play");
        auto missing_lane_edge = playfield;
        missing_lane_edge[330 * width + static_cast<std::size_t>(width / lanes)] = pixel(0, 0, 0);
        require(tracker.observe(missing_lane_edge, lanes).phase == VisualPhase::unknown,
                "a damaged lane judgement reference cannot pass continuity checks");
        inferred = tracker.observe(playfield, lanes);
        require(inferred.phase == VisualPhase::scored && inferred.inferred,
                "scored tracking resumes after a damaged image without requiring a hidden clock");
        labeled = playfield;
        draw_phase(labeled, VisualPhase::finished);
        const auto finished = tracker.observe(labeled, lanes);
        require(finished.phase == VisualPhase::finished && !finished.inferred,
                "visible FINISHED terminates inferred scored phase");
        require(tracker.observe(playfield, lanes).phase == VisualPhase::unknown,
                "a frame after FINISHED cannot silently reenter scored play");
    }
}

void test_feedback_and_tracking() {
    for (int lanes : {2, 16}) {
        std::vector<std::uint32_t> image(width * height, pixel(9, 16, 28));
        const int center = (width / lanes) / 2;
        const int x = center - 14; // "-19.4", five glyphs.
        for (int col = 1; col <= 3; ++col) image[337 * width + x + col] = good_color;
        draw_digit(image, 1, x + 6, 334, 1, good_color);
        draw_digit(image, 9, x + 12, 334, 1, good_color);
        image[340 * width + x + 20] = good_color;
        draw_digit(image, 4, x + 24, 334, 1, good_color);
        require(read_feedback(image, 0, lanes) == -194, "signed visible feedback is parsed without hidden timing data");
    }
    Shared shared;
    // Three local-receipt observations of one note, then an adjacent 40 ms
    // note. No game epoch or displayed clock exists in these estimates.
    observe(shared, 0, 165, 1'000.25);
    observe(shared, 0, 169, 1'012.82);
    observe(shared, 0, 173, 1'025.13);
    observe(shared, 0, 160, 1'025.13);
    require(shared.tracks[0].size() == 2, "one visual track per note at 40 ms spacing");
    require(shared.tracks[0][0].observations == 3, "subpixel estimate averages repeated observations");
    shared.tracks[0][0].tapped = true;
    observe(shared, 0, 177, 1'037.75);
    require(shared.tracks[0].size() == 2 && shared.tracks[0][0].tapped,
            "late pixels of a consumed note cannot schedule a second tap");

    // Rasterization and normal receipt jitter are independent of an arbitrary
    // local origin. Fit two full one-second trajectories 40 ms apart.
    for (double local_origin : {0.0, 1'234'567.891}) {
        Shared fitted;
        for (int frame = 0; frame < 79; ++frame) {
            const double capture_ms = 1'000.0 + frame * 12.5;
            const double receipt_ms = capture_ms + 0.4 + (frame % 7) * 0.07;
            for (const double target_ms : {2'000.0, 2'040.0}) {
                const int y = static_cast<int>(std::lround((1.0 - (target_ms - capture_ms) / 1000.0) * 330.0));
                if (y >= 0) observe(fitted, 0, y, receipt_ms + local_origin);
            }
        }
        require(fitted.tracks[0].size() == 2, "quantized trajectories retain two distinct nearby notes");
        require(std::abs(fitted.tracks[0][0].target_ms - local_origin - 2'000.0 - 0.6) < 0.2,
                "repeated observations retain only constant transport delay for visual calibration");
        require(std::abs(fitted.tracks[0][1].target_ms - local_origin - 2'040.0 - 0.6) < 0.2,
                "local origin translation cannot change note timing accuracy");
    }
    require(use_for_tracking(1'000 * ms, 0), "first frame can initialize tracking");
    require(use_for_tracking(1'012 * ms, 1'000 * ms), "normal frame advances trajectories");
    require(!use_for_tracking(1'127 * ms, 1'000 * ms), "first image after a stalled consumer cannot create false shifted tracks");
    require(use_for_tracking(1'139 * ms, 1'127 * ms), "tracking resumes with next timely frame");
}
} // namespace

int main() {
    try {
        test_phases_and_notes();
        test_unlabeled_scored_continuity();
        test_feedback_and_tracking();
        std::cout << "pixel diagnostic tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
