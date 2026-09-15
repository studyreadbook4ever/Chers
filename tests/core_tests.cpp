#include "chronolane/core.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

using namespace chronolane;

namespace {
int checks = 0;

void check(bool condition, const char* description) {
    ++checks;
    if (!condition) throw std::runtime_error(description);
}

template<class F> void rejects(F&& operation, const char* description) {
    bool rejected = false;
    try { operation(); }
    catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, description);
}

class TestRandom final : public RandomSource {
public:
    explicit TestRandom(std::uint64_t seed) : generator_(seed) {}
    std::uint64_t next() override { return generator_(); }
    std::string source_label() const override { return "deterministic test fixture (not benchmark entropy)"; }
private:
    std::mt19937_64 generator_;
};

Chart chart_with(std::initializer_list<Note> notes) {
    Chart chart;
    chart.notes = notes;
    chart.entropy_source = "synthetic unit-test fixture";
    return chart;
}

void validation() {
    Config{2, 4}.validate();
    Config{16, 384}.validate();
    Config{7, 14.25}.validate();
    rejects([] { Config{1, 10}.validate(); }, "reject fewer than 2 lanes");
    rejects([] { Config{17, 50}.validate(); }, "reject more than 16 lanes");
    rejects([] { Config{4, 7.99}.validate(); }, "reject below minimum density");
    rejects([] { Config{4, 96.01}.validate(); }, "reject above maximum density");
    rejects([] { Config{4, std::numeric_limits<double>::quiet_NaN()}.validate(); }, "reject NaN density");
    rejects([] { Config{4, std::numeric_limits<double>::infinity()}.validate(); }, "reject infinity density");
    rejects([] { Engine e({2, 4}, {}, 0); }, "reject zero log capacity");
    const auto t = Timeline::scored_start;
    rejects([&] { Engine e({2, 4}, chart_with({{0, 2, t, Stage::Scored}})); }, "reject chart lane outside configuration");
    rejects([&] { Engine e({2, 4}, chart_with({{0, 0, t, Stage::Calibration}})); }, "reject chart stage mismatch");
    rejects([&] { Engine e({2, 4}, chart_with({{0, 0, t, Stage::Scored, true}})); }, "reject consumed chart");
    rejects([&] { Engine e({2, 4}, chart_with({{0, 0, t, Stage::Scored}, {0, 1, t, Stage::Scored}})); }, "reject duplicate IDs");
    rejects([&] { Engine e({2, 4}, chart_with({{0, 0, t + 1, Stage::Scored}, {1, 1, t, Stage::Scored}})); }, "reject unsorted chart");
    rejects([&] { Engine e({2, 4}, chart_with({{0, 0, t, Stage::Scored}, {1, 0, t + kLaneLock - 1, Stage::Scored}})); }, "reject lock violation by one nanosecond");
    rejects([] { Engine e({2, 4}, chart_with({{0, 0, Timeline::scored_end, Stage::Scored}})); }, "exclude target exactly at stage end");
    Engine e({2, 4}, {});
    check(std::isnan(e.results(Stage::Scored).normalized()), "no notes has undefined normalized score");
    rejects([&] { e.hit(t, 2); }, "reject out-of-range input lane");
}

void judgment_boundaries() {
    const TimeNs t = Timeline::scored_start + kSecond;
    const std::pair<TimeNs, Judgment> cases[] = {
        {-20*kMs-1, Judgment::Empty}, {-20*kMs, Judgment::Good},
        {-8*kMs-1, Judgment::Good}, {-8*kMs, Judgment::Perfect},
        {-1, Judgment::Perfect}, {0, Judgment::Perfect}, {1, Judgment::Perfect},
        {8*kMs, Judgment::Perfect}, {8*kMs+1, Judgment::Good},
        {20*kMs, Judgment::Good}, {20*kMs+1, Judgment::Empty},
    };
    for (const auto& [delay, expected] : cases) {
        Engine e({2, 4}, chart_with({{42, 0, t, Stage::Scored}}));
        const auto result = e.hit(t + delay, 0);
        check(result.kind == expected, "inclusive Perfect/Good boundary judgment");
        const auto score = e.results(Stage::Scored);
        if (expected == Judgment::Empty) {
            check(!e.notes()[0].consumed, "outside-window input does not consume");
            check(result.note_id == kNoNote, "empty input has no note ID");
            check(score.raw_score == -5 && score.delay_sum_ns == 100*kMs, "empty input penalties");
        } else {
            check(e.notes()[0].consumed && result.note_id == 42, "successful input consumes matched note");
            check(result.delay_ns == delay && score.delay_sum_ns == std::abs(delay), "preserve nanosecond timing precision");
            check(score.raw_score == (expected == Judgment::Perfect ? 2 : 1), "success score contribution");
        }
    }
}

void repeated_inputs_and_ties() {
    const TimeNs t = Timeline::scored_start + kSecond;
    Engine e({2, 4}, chart_with({{10, 0, t, Stage::Scored}, {11, 0, t+40*kMs, Stage::Scored}}));
    const auto first = e.hit(t+20*kMs, 0);
    check(first.note_id == 10 && first.delay_ns == 20*kMs, "exact tie goes to earlier note");
    const auto second = e.hit(t+20*kMs, 0);
    check(second.note_id == 11 && second.delay_ns == -20*kMs, "next identical-time command can hit next eligible note");
    for (int n = 0; n < 1000; ++n)
        check(e.hit(t+20*kMs, 0).kind == Judgment::Empty, "every repeated command is an independent empty hit");
    const auto score = e.results(Stage::Scored);
    check(score.good == 2 && score.empty == 1000 && score.missed == 0, "no input debounce or accidental rematching");
    check(score.raw_score == -4998, "all 1000 empty inputs receive minus five");
    check(score.delay_sum_ns == 100040*kMs, "all 1000 empty inputs receive plus100ms delay penalty");
    check(score.normalized() == -1249.5, "negative scores remain negative without clamping");
    check(e.records().size() == 1002, "log retains every scored input");

    Engine nearby({2, 4}, chart_with({{0, 0, t, Stage::Scored}, {1, 0, t+40*kMs, Stage::Scored}}));
    check(nearby.hit(t+19*kMs, 0).kind == Judgment::Good, "first close-spaced real hit");
    check(nearby.hit(t+21*kMs, 0).kind == Judgment::Good, "input cooldown does not reject second close-spaced real hit");

    Engine duplicate({2, 4}, chart_with({{0, 0, t, Stage::Scored}}));
    duplicate.hit(t, 0);
    check(duplicate.hit(t, 0).kind == Judgment::Empty, "consumed note is immediately unavailable");
    check(duplicate.last_feedback()[0].kind == Judgment::Empty, "lane feedback reflects latest input");
}

void stage_accounting_and_tail() {
    check(Timeline::calibration_end - Timeline::calibration_start == 5*kSecond, "five calibration target seconds");
    check(Timeline::scored_start - Timeline::calibration_tail_end == 5*kSecond, "five second wait after calibration tail");
    check(Timeline::scored_end - Timeline::scored_start == 50*kSecond, "fifty scored target seconds");
    check(Timeline::finish == 61'040*kMs, "full run with preroll and both tails lasts61.040s");
    for (const auto stage : {Stage::Calibration, Stage::Scored}) {
        const auto start = stage == Stage::Calibration ? Timeline::calibration_start : Timeline::scored_start;
        const auto end = stage == Stage::Calibration ? Timeline::calibration_end : Timeline::scored_end;
        check(Timeline::input_stage(start-kGoodWindow-1) == Stage::None, "before early-hit window ignored");
        check(Timeline::input_stage(start-kGoodWindow) == stage, "early-hit endpoint included");
        check(Timeline::input_stage(end+kGoodWindow-1) == stage, "tail covers final possible target plus20ms");
        check(Timeline::input_stage(end+kGoodWindow) == Stage::None, "after final possible hit ignored");
        Engine e({2, 4}, chart_with({{0, 0, start, stage}, {1, 0, end-1, stage}}));
        check(e.hit(start-kGoodWindow-1, 0).kind == Judgment::Ignored, "preroll/wait is unscored");
        check(e.hit(start-kGoodWindow, 0).kind == Judgment::Good, "first note supports exact minus20ms");
        check(e.hit(end-1+kGoodWindow, 0).kind == Judgment::Good, "last possible note supports exact plus20ms");
        check(e.hit(end+kGoodWindow, 0).kind == Judgment::Ignored, "after-tail command unscored");
        check(e.results(stage).good == 2 && e.records().size() == 2, "only active-stage hits logged");
    }

    Engine combined({2, 4}, chart_with({
        {0, 0, Timeline::calibration_start, Stage::Calibration},
        {1, 0, Timeline::scored_start, Stage::Scored},
        {2, 1, Timeline::scored_start, Stage::Scored},
    }));
    combined.hit(Timeline::calibration_start, 0);
    combined.hit(Timeline::calibration_start, 0);
    combined.hit(Timeline::calibration_tail_end+kSecond, 0);
    combined.hit(Timeline::scored_start+8*kMs, 0);
    const auto calibration = combined.results(Stage::Calibration);
    const auto scored = combined.results(Stage::Scored);
    check(calibration.raw_score == -3 && calibration.delay_sum_ns == 100*kMs, "calibration has independent penalties");
    check(scored.notes == 2 && scored.perfect == 1 && scored.missed == 1, "miss remains in real-note denominator");
    check(scored.raw_score == 2 && scored.normalized() == 0.5 && scored.delay_sum_ns == 8*kMs, "calibration and wait do not leak into scored result");

    const auto t = Timeline::scored_start;
    Engine skipped({2, 4}, chart_with({{0, 0, t, Stage::Scored}, {1, 0, t+100*kMs, Stage::Scored}}));
    check(skipped.hit(t+100*kMs, 0).note_id == 1, "skip expired note and match later note");
    check(skipped.results(Stage::Scored).missed == 1, "expired note counts as miss without penalty");
}

void invalidation() {
    const auto t = Timeline::scored_start;
    Engine overflow({2, 4}, chart_with({{0, 0, t, Stage::Scored}}), 2);
    overflow.hit(t, 0);
    overflow.hit(t, 0);
    check(overflow.hit(t, 0).kind == Judgment::Invalid, "overflow is explicit");
    check(!overflow.valid() && overflow.invalid_reason().find("capacity") != std::string::npos, "overflow invalidates official run");
    check(overflow.records().size() == 2, "overflow does not silently evict records");
    check(overflow.hit(t, 0).kind == Judgment::Invalid, "invalid run cannot resume silently");

    Engine backwards({2, 4}, {});
    backwards.hit(t, 0);
    check(backwards.hit(t-1, 0).kind == Judgment::Invalid && !backwards.valid(), "reject nonmonotonic event stream");
    Engine host({2, 4}, {});
    host.invalidate("transport overflow");
    host.invalidate("second failure");
    check(!host.valid() && host.invalid_reason() == "transport overflow", "host invalidation preserves first cause");
}

void generation() {
    for (const auto lanes : {2u, 3u, 8u, 16u}) {
        for (const double per_lane_density : {2.0, 12.5, 24.0}) {
            Config config{lanes, per_lane_density * lanes};
            TestRandom random(0x564953494f4eull + lanes + static_cast<unsigned>(per_lane_density));
            const auto chart = generate_chart(config, random);
            check(chart.entropy_source.find("test fixture") != std::string::npos, "generator retains entropy provenance");
            check(chart.calibration.attempts == chart.calibration.emitted+chart.calibration.dropped, "calibration attempt accounting");
            check(chart.scored.attempts == chart.scored.emitted+chart.scored.dropped, "scored attempt accounting");
            check(chart.notes.size() == chart.calibration.emitted+chart.scored.emitted, "denominator reflects emitted notes only");
            const double expected = config.density*55.0;
            const auto attempts = chart.calibration.attempts+chart.scored.attempts;
            check(std::abs(static_cast<double>(attempts)-expected) < 7*std::sqrt(expected), "Poisson total near configured mean without exact count enforcement");
            if (per_lane_density == 24.0) check(chart.scored.dropped > 0, "fully locked attempts dropped at high load");

            std::array<TimeNs, 16> last{};
            last.fill(INT64_MIN);
            std::array<bool, 16> visited{};
            std::array<std::uint64_t, 50> seconds{};
            TimeNs previous_target = -1;
            for (const auto& note : chart.notes) {
                check(note.lane < lanes, "generated lane is valid");
                check(note.target_ns > previous_target, "Poisson attempts remain time ordered");
                if (last[note.lane] != INT64_MIN)
                    check(note.target_ns-last[note.lane] >= 40*kMs, "generation obeys exact per-lane40ms lock");
                check(note.stage == Timeline::input_stage(note.target_ns), "generated target belongs to its stage");
                last[note.lane] = note.target_ns;
                previous_target = note.target_ns;
                visited[note.lane] = true;
                if (note.stage == Stage::Scored)
                    ++seconds[static_cast<std::size_t>((note.target_ns-Timeline::scored_start)/kSecond)];
            }
            for (std::uint32_t lane = 0; lane < lanes; ++lane) check(visited[lane], "all configured lanes used");
            check(*std::min_element(seconds.begin(), seconds.end()) != *std::max_element(seconds.begin(), seconds.end()), "one-second note counts vary");
            Engine oracle(config, chart);
            for (const auto& note : chart.notes)
                check(oracle.hit(note.target_ns, note.lane).kind == Judgment::Perfect, "exact-time oracle hits every generated note");
            const auto score = oracle.results(Stage::Scored);
            check(score.perfect == chart.scored.emitted && score.normalized() == 1.0 && score.delay_sum_ns == 0, "oracle proves generated charts are fully playable");
        }
    }
}

} // namespace

int main() {
    try {
        validation();
        judgment_boundaries();
        repeated_inputs_and_ties();
        stage_accounting_and_tail();
        invalidation();
        generation();
        std::cout << "CHERS core: " << checks << " checks passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
