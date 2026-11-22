// Grind bench: the firmware's grind controller, its strategies and its scale
// code, run on the desktop against a simulated grinder and load cell.
//
// grind_bench_test.py builds this file together with the unmodified sources
// from src/controllers and src/hardware/circular_buffer_math. Only the motor
// driver, the HX711 driver, storage and FreeRTOS are stand-ins (see fakes/
// and shim/). One scenario runs per invocation:
//
//   bench <scenario> <printer vibration trace> <normal bench trace>
#include <Arduino.h>
#include <Preferences.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <functional>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "src/controllers/grind_controller.h"
#include "src/logging/grind_logging.h"
#include "src/system/statistics_manager.h"

// ---------------------------------------------------------------------------
// Clock, log and the firmware globals the controller links against
// ---------------------------------------------------------------------------
static uint32_t clock_ms = 1000;
unsigned long millis() { return clock_ms; }
int64_t esp_timer_get_time() { return static_cast<int64_t>(clock_ms) * 1000; }

std::string bench_log;
StatisticsManager statistics_manager;
GrindLogger grind_logger;

// What the grind logger was given for the session in progress or just ended.
struct SessionRecord {
    float start_weight = 0.0f;
    std::string result;
    float final_weight = 0.0f;
    int pulses = 0;
    std::vector<GrindEvent> events;
};
static SessionRecord session_record;

bool GrindLogger::init(Preferences*) {
    logging_active = false;
    return true;
}
void GrindLogger::start_grind_session(const GrindSessionDescriptor&, float start_weight) {
    logging_active = true;
    session_record = {};
    session_record.start_weight = start_weight;
}
void GrindLogger::end_grind_session(const char* final_result, float final_weight, uint8_t pulse_count, uint32_t) {
    logging_active = false;
    session_record.result = final_result;
    session_record.final_weight = final_weight;
    session_record.pulses = pulse_count;
}
void GrindLogger::discard_current_session() { logging_active = false; }
void GrindLogger::log_event(GrindEvent& event) { session_record.events.push_back(event); }
void GrindLogger::log_continuous_measurement(uint32_t, float, float, float, uint8_t, uint8_t, float) {}
void GrindLogger::send_current_session_via_serial() {}

// The last screen state the controller published, as the UI would receive it.
static GrindEventData shown_state{};
static void on_ui_event(const GrindEventData& event) {
    if (event.event != UIGrindEvent::PROGRESS_UPDATED && event.event != UIGrindEvent::BACKGROUND_CHANGE) {
        shown_state = event;
    }
}

// ---------------------------------------------------------------------------
// Checks
// ---------------------------------------------------------------------------
static const char* scenario_name = "";
static std::function<std::string()> describe_bench = [] { return std::string(); };

[[noreturn]] static void check_failed(const char* expression, int line) {
    std::fprintf(stderr, "\n[%s] check failed at bench.cpp:%d: %s\n%s", scenario_name, line, expression,
                 describe_bench().c_str());
    const size_t tail = bench_log.size() > 2500 ? bench_log.size() - 2500 : 0;
    std::fprintf(stderr, "--- end of the firmware log ---\n%s\n", bench_log.c_str() + tail);
    std::exit(1);
}
#define CHECK(condition)                                  \
    do {                                                  \
        if (!(condition)) check_failed(#condition, __LINE__); \
    } while (0)

static bool near(double value, double expected, double tolerance) {
    return std::fabs(value - expected) <= tolerance;
}

static bool log_contains(const char* text) { return bench_log.find(text) != std::string::npos; }

// Number that follows `label` in the firmware log, or NAN.
static double log_number_after(const char* label) {
    const size_t at = bench_log.rfind(label);
    if (at == std::string::npos) return NAN;
    return std::atof(bench_log.c_str() + at + std::strlen(label));
}

// ---------------------------------------------------------------------------
// Scale scatter: how far single readings sit from the true weight
// ---------------------------------------------------------------------------

// Scatter of a recorded trace: its readings between two times, less their mean.
static std::vector<float> scatter_of_trace(const char* path, uint32_t from_ms, uint32_t to_ms) {
    std::ifstream file(path);
    if (!file) {
        std::fprintf(stderr, "cannot read %s\n", path);
        std::exit(2);
    }
    std::vector<float> readings;
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#' || line[0] == 't') continue;
        const size_t comma = line.find(',');
        const uint32_t time_ms = static_cast<uint32_t>(std::atol(line.substr(0, comma).c_str()));
        if (time_ms >= from_ms && time_ms <= to_ms) {
            readings.push_back(static_cast<float>(std::atof(line.substr(comma + 1).c_str())));
        }
    }
    double sum = 0.0;
    for (float reading : readings) sum += reading;
    const float mean = static_cast<float>(sum / readings.size());
    for (float& reading : readings) reading -= mean;
    return readings;
}

// A load cell on a quiet bench: a few milligrams either way.
static std::vector<float> quiet_scatter() {
    std::vector<float> scatter;
    uint32_t state = 12345;
    for (int i = 0; i < 997; ++i) {
        state = state * 1664525U + 1013904223U;
        scatter.push_back((static_cast<float>(state >> 8) / 16777216.0f - 0.5f) * 0.008f);
    }
    return scatter;
}

// ---------------------------------------------------------------------------
// The bench: a grinder, a scale and the person using them
// ---------------------------------------------------------------------------
constexpr uint32_t kAdcPeriodMs = 80;      // The HX711 delivers about 12.5 samples a second
constexpr uint32_t kControlPeriodMs = SYS_TASK_GRIND_CONTROL_INTERVAL_MS;
constexpr int32_t kRawAtEmptyScale = 9000000;
constexpr double kPortafilterG = 410.0;
constexpr bool kShakyBenchHandling = true;  // The owner has switched it on

static bool is_result(GrindPhase phase) { return phase == GrindPhase::COMPLETED || phase == GrindPhase::TIMEOUT; }

struct GrindOutcome {
    GrindPhase phase = GrindPhase::IDLE;
    uint32_t duration_ms = 0;  // From the start request to the result
    double grounds_g = 0.0;    // What really landed in the vessel
    float shown_g = 0.0f;      // The weight on the result screen
    int pulses = 0;
    GrindSessionResult result = GrindSessionResult::UNKNOWN;
    std::string error_message;
    std::string error_title;
};

class Bench {
public:
    WeightSensor sensor;
    Grinder grinder;
    Preferences prefs;
    GrindController controller;

    // On the scale
    double tray_g = 0.0;     // A removable tray or holder: part of the "empty" scale
    double vessel_g = 0.0;
    double grounds_g = 0.0;
    double held_up_g = 0.0;  // Part of the vessel's weight carried by its holder
    double hand_g = 0.0;         // Force of a hand on the vessel (negative: carrying it)
    double hand_tremor_g = 0.0;  // That force wavers by this much either way
    int bad_conversions = 0;     // The next conversions read bad_conversion_g instead
    double bad_conversion_g = 0.0;
    uint32_t calm_until_ms = 0;  // Until then the bench holds still

    // The grinder
    double flow_gps = 1.7;
    uint32_t spin_up_ms = static_cast<uint32_t>(GRIND_MOTOR_RESPONSE_LATENCY_DEFAULT_MS);
    uint32_t spin_down_ms = 80;        // The burrs run on at half flow after the power is cut
    uint32_t transit_ms = 450;         // Burrs to basket
    double retained_drop_g = 0.0;      // Falls out in one go when a long run stops
    double trickle_gps = 0.0;          // Grounds that keep dribbling after a long run stops
    uint32_t trickle_ms = 0;

    explicit Bench(std::vector<float> scatter, size_t scatter_start = 0, bool shaky_bench_handling = false)
        : scatter_(std::move(scatter)), scatter_index_(scatter_start % scatter_.size()) {
        describe_bench = [this] { return describe(); };
        // Left unset, the grinder is as shipped: the handling is off.
        if (shaky_bench_handling) {
            prefs.values[GrindController::PREF_KEY_SHAKY_BENCH] = 1;
        }
    }

    void power_up() {
        in_flight_.assign(transit_ms, 0.0);
        prefs.values[GrindController::PREF_KEY_GRINDER_MODE] = static_cast<int>(GrinderPurgeMode::PRIME);
        controller.init(&sensor, &grinder, &prefs);
        controller.set_ui_event_callback(on_ui_event);
        sensor.tareNoDelay();  // The firmware zeroes the scale once at start-up
        idle(3000);
        CHECK(!sensor.is_tare_in_progress());
    }

    void place_vessel(double weight_g) {
        vessel_g = weight_g;
        grounds_g = 0.0;
    }

    void lift_vessel() {
        vessel_g = 0.0;
        grounds_g = 0.0;
        held_up_g = 0.0;
        hand_g = 0.0;
        hand_tremor_g = 0.0;
    }

    // The person presses OK on the result (or the firmware returns by itself).
    void dismiss_result() { controller.return_to_idle(); }

    void idle(uint32_t duration_ms) {
        run(duration_ms, [] { return false; });
    }

    bool run_until_phase(GrindPhase phase, uint32_t max_ms = 70000) {
        return run(max_ms, [&] { return controller.get_phase() == phase; });
    }

    template <typename Until>
    bool run(uint32_t max_ms, Until until) {
        for (uint32_t elapsed = 0; elapsed < max_ms; ++elapsed) {
            ++clock_ms;
            step_grinder();
            if (clock_ms % kAdcPeriodMs == 0) {
                sample_scale();
            }
            if (clock_ms % kControlPeriodMs == 0) {
                controller.update();
                step_ui();
                if (until()) return true;
            }
        }
        return false;
    }

    void start(float target_g, GrindMode mode = GrindMode::WEIGHT, uint32_t time_ms = 0) {
        grind_started_ms_ = clock_ms;
        pulses_before_ = grinder.pulses;
        shown_state = {};
        CHECK(controller.start_grind(target_g, time_ms, mode));
    }

    GrindOutcome finish() {
        CHECK(run(70000, [&] { return is_result(controller.get_phase()); }));
        GrindOutcome outcome;
        outcome.duration_ms = clock_ms - grind_started_ms_;
        outcome.grounds_g = grounds_g;
        idle(200);  // The UI side collects the result
        outcome.phase = controller.get_phase();
        outcome.pulses = grinder.pulses - pulses_before_;
        outcome.result = controller.get_last_session_result();
        outcome.shown_g = outcome.phase == GrindPhase::COMPLETED ? shown_state.final_weight : shown_state.error_weight;
        outcome.error_message = shown_state.error_message;
        outcome.error_title = shown_state.error_title;
        CHECK(!grinder.output_high());
        if (std::getenv("GRIND_BENCH_VERBOSE")) {
            std::printf("[%s] %u ms, %.2f g landed, %.2f g shown, %d pulses\n%s", scenario_name,
                        outcome.duration_ms, outcome.grounds_g, outcome.shown_g, outcome.pulses,
                        describe().c_str());
        }
        return outcome;
    }

    GrindOutcome grind(float target_g, GrindMode mode = GrindMode::WEIGHT, uint32_t time_ms = 0) {
        start(target_g, mode, time_ms);
        return finish();
    }

    // Longest time any one visit to `phase` took in the session just logged.
    uint32_t longest(GrindPhase phase) const {
        uint32_t longest_ms = 0;
        for (const GrindEvent& event : session_record.events) {
            if (event.phase_id == static_cast<uint8_t>(phase)) longest_ms = max(longest_ms, event.duration_ms);
        }
        return longest_ms;
    }

    // Soonest that grounds from a motor start can reach the vessel.
    uint32_t first_grounds_ms() const { return spin_up_ms + transit_ms; }

    // Time spent after the main run: settling, deciding, pulsing and the final weight.
    uint32_t time_after_main_run() const {
        uint32_t total_ms = 0;
        bool after_main_run = false;
        for (const GrindEvent& event : session_record.events) {
            if (after_main_run) total_ms += event.duration_ms;
            if (event.phase_id == static_cast<uint8_t>(GrindPhase::PREDICTIVE)) after_main_run = true;
        }
        return total_ms;
    }

    // With shaky-bench handling on a bench that shakes all through the grind,
    // no wait for a still scale outlasts its limit, give or take a few
    // readings until the average of those that act on a weight is steady.
    void check_settle_limits() const {
        const uint32_t slack_ms = 2 * kControlPeriodMs;
        const uint32_t steadying_ms = 600;
        const uint32_t landing_ms =
            static_cast<uint32_t>(controller.get_grind_latency_ms()) + GRIND_MOTOR_SETTLING_TIME_MS;
        CHECK(longest(GrindPhase::TARE_CONFIRM) <= GRIND_TARE_SETTLE_LIMIT_MS + slack_ms);
        CHECK(longest(GrindPhase::PRIME_SETTLING) <= GRIND_PRIME_SETTLE_LIMIT_MS + slack_ms);
        CHECK(longest(GrindPhase::PULSE_SETTLING) <= landing_ms + GRIND_MOTOR_SETTLE_LIMIT_MS + slack_ms);
        CHECK(longest(GrindPhase::PULSE_DECISION) <= GRIND_WEIGHT_SETTLE_LIMIT_MS + steadying_ms);
        CHECK(longest(GrindPhase::FINAL_SETTLING) <= GRIND_WEIGHT_SETTLE_LIMIT_MS + steadying_ms);
    }

    std::string describe() const {
        static const char* const names[] = {
            "IDLE", "INITIALIZING", "SETUP", "TARING", "TARE_CONFIRM", "PREDICTIVE", "PULSE_DECISION",
            "PULSE_EXECUTE", "PULSE_SETTLING", "FINAL_SETTLING", "TIME_GRINDING", "MANUAL_GRINDING",
            "TIME_ADDITIONAL_PULSE", "COMPLETED", "TIMEOUT", "PRIME", "PRIME_SETTLING", "PURGE_CONFIRM"};
        std::ostringstream out;
        out << "clock " << clock_ms << " ms, phase " << names[static_cast<int>(controller.get_phase())]
            << ", vessel " << vessel_g << " g, grounds " << grounds_g << " g, motor "
            << (grinder.output_high() ? "on" : "off") << ", pulses " << grinder.pulses << "\n"
            << "shown: final " << shown_state.final_weight << " g, error '" << shown_state.error_title << "' / '"
            << shown_state.error_message << "' at " << shown_state.error_weight << " g\n";
        for (const GrindEvent& event : session_record.events) {
            out << "  " << names[event.phase_id] << " " << event.duration_ms << " ms, " << event.start_weight
                << " -> " << event.end_weight << " g\n";
        }
        return out.str();
    }

private:
    std::vector<float> scatter_;
    size_t scatter_index_ = 0;
    std::deque<double> in_flight_;  // Grounds per millisecond on their way down
    uint32_t motor_on_ms_ = 0;
    uint32_t spin_down_left_ms_ = 0;
    uint32_t trickle_left_ms_ = 0;
    bool hand_up_ = false;
    uint32_t grind_started_ms_ = 0;
    int pulses_before_ = 0;

    void step_grinder() {
        const bool motor_on = grinder.output_high();
        double leaving_g = 0.0;
        if (motor_on) {
            ++motor_on_ms_;
            if (motor_on_ms_ > spin_up_ms) leaving_g = flow_gps / 1000.0;
        } else {
            if (motor_on_ms_ > spin_up_ms) {
                spin_down_left_ms_ = spin_down_ms;
            }
            // Grounds held back in the chute let go when a long run stops.
            if (motor_on_ms_ > 3000) {
                leaving_g = retained_drop_g;
                trickle_left_ms_ = trickle_ms;
            }
            motor_on_ms_ = 0;
            if (spin_down_left_ms_ > 0) {
                --spin_down_left_ms_;
                leaving_g += flow_gps / 2000.0;
            }
            if (trickle_left_ms_ > 0) {
                --trickle_left_ms_;
                leaving_g += trickle_gps / 1000.0;
            }
        }
        in_flight_.push_back(leaving_g);
        const double landing_g = in_flight_.front();
        in_flight_.pop_front();
        if (vessel_g > 0.0) grounds_g += landing_g;
    }

    void sample_scale() {
        hand_up_ = !hand_up_;
        const double scatter_g = clock_ms < calm_until_ms ? 0.0 : scatter_[scatter_index_];
        double load_g = tray_g + vessel_g + grounds_g - held_up_g + hand_g +
                        (hand_up_ ? hand_tremor_g : -hand_tremor_g) + scatter_g;
        if (bad_conversions > 0) {
            --bad_conversions;
            load_g = bad_conversion_g;
        }
        scatter_index_ = (scatter_index_ + 1) % scatter_.size();
        sensor.conversion_raw = kRawAtEmptyScale + static_cast<int32_t>(std::lround(load_g * sensor.cal_factor));
        sensor.conversion_ready = true;
        CHECK(sensor.sample_and_feed_filter());
    }

    // What the UI task does between control cycles.
    void step_ui() {
        if (controller.get_phase() == GrindPhase::INITIALIZING) {
            controller.ui_acknowledge_phase_transition();
        }
        controller.process_queued_flash_operations();
        controller.process_queued_log_messages();
        controller.process_queued_ui_events();
    }
};

// ---------------------------------------------------------------------------
// Scenarios
// ---------------------------------------------------------------------------
static std::vector<float> printer_scatter;  // 3D printer running on the same bench
static std::vector<float> normal_scatter;   // The same bench on an ordinary day

// Power up with an empty scale, then put the portafilter on.
static void ready_with_portafilter(Bench& bench) {
    bench.power_up();
    bench.idle(3000);
    bench.place_vessel(kPortafilterG);
    bench.idle(2000);
}

// Result looked at, portafilter taken to the machine and brought back empty.
static void take_away_and_return(Bench& bench) {
    bench.idle(1500);
    bench.lift_vessel();
    bench.idle(1500);
    bench.dismiss_result();
    bench.idle(5000);
    bench.place_vessel(kPortafilterG + 0.4);  // A few grounds stay in the basket
    bench.idle(2000);
}

static void quiet_bench_grind() {
    Bench bench(quiet_scatter());
    ready_with_portafilter(bench);
    const GrindOutcome outcome = bench.grind(18.0f);

    CHECK(outcome.phase == GrindPhase::COMPLETED);
    CHECK(near(outcome.grounds_g, 18.0, 0.10));
    CHECK(near(outcome.shown_g, outcome.grounds_g, 0.03));
    CHECK(outcome.duration_ms < 25000);
    CHECK(outcome.pulses >= 1);
    // A quiet scale reads still every time: none of the limits comes into play.
    CHECK(!log_contains("Not still within"));
    CHECK(near(log_number_after("Vessel reference: "), kPortafilterG, 0.5));
    // Coffee counts as flowing on the flow rate alone, as it always did: the
    // weight it takes to show that rate, seen one scale reading later at most.
    const double first_grounds_ms = bench.first_grounds_ms();
    const double flow_shows_ms = GRIND_FLOW_DETECTION_THRESHOLD_GPS * 0.5 / bench.flow_gps * 1000.0;
    CHECK(bench.controller.get_grind_latency_ms() >= first_grounds_ms);
    CHECK(bench.controller.get_grind_latency_ms() <=
          first_grounds_ms + flow_shows_ms + kAdcPeriodMs + kControlPeriodMs);
}

static void normal_bench_grind() {
    Bench bench(normal_scatter);
    ready_with_portafilter(bench);
    const GrindOutcome outcome = bench.grind(18.0f);

    CHECK(outcome.phase == GrindPhase::COMPLETED);
    CHECK(near(outcome.grounds_g, 18.0, 0.10));
    CHECK(near(outcome.shown_g, outcome.grounds_g, 0.03));
    CHECK(outcome.duration_ms < 45000);
}

static void normal_bench_with_the_handling_on() {
    // The printer is off but the handling was left on. This load cell's own
    // noise keeps most half seconds from reading still, so a grind may well
    // take the bench for a shaking one and go by averages. It has to come
    // out as accurate as it does without the handling, from any moment.
    int averaged = 0;
    for (size_t start = 0; start < 16; ++start) {
        clock_ms = 1000;
        bench_log.clear();
        Bench bench(normal_scatter, start * 5, kShakyBenchHandling);
        ready_with_portafilter(bench);
        const GrindOutcome outcome = bench.grind(18.0f);

        CHECK(outcome.phase == GrindPhase::COMPLETED);
        CHECK(near(outcome.grounds_g, 18.0, 0.10));
        CHECK(near(outcome.shown_g, outcome.grounds_g, 0.03));
        CHECK(outcome.duration_ms < 45000);
        CHECK(bench.controller.get_grind_latency_ms() >= bench.first_grounds_ms());
        if (log_contains("using the 2.0s average")) ++averaged;
        bench.dismiss_result();
    }
    if (std::getenv("GRIND_BENCH_VERBOSE")) {
        std::printf("[%s] %d of 16 grinds went by an average\n", scenario_name, averaged);
    }
}

static void printer_vibration_grind() {
    // The grind that used to end as "Timeout:PULS" after 60 s.
    Bench bench(printer_scatter, 0, kShakyBenchHandling);
    ready_with_portafilter(bench);
    const GrindOutcome outcome = bench.grind(18.0f);

    CHECK(outcome.phase == GrindPhase::COMPLETED);
    CHECK(outcome.duration_ms < 45000);
    CHECK(near(outcome.grounds_g, 18.0, 0.15));
    CHECK(near(outcome.shown_g, outcome.grounds_g, 0.05));
    // The scale never read still, so the grind went on with averages.
    CHECK(log_contains("Not still within"));
    CHECK(log_contains("average"));
    CHECK(near(log_number_after("Vessel reference: "), kPortafilterG, 0.5));
    bench.check_settle_limits();
    // The last decision already waited for the weight: no second wait for the result.
    CHECK(bench.longest(GrindPhase::FINAL_SETTLING) <= 2 * kControlPeriodMs);
}

static void printer_vibration_is_recognised_from_any_moment() {
    // Whether a grind takes its bench for a shaking one depends on what the
    // scale happens to read after taring and after the prime. Start the same
    // grind at 40 different moments of the recorded vibration: a grind that
    // misses it waits for a still reading, as the firmware always did.
    int finished = 0;
    for (size_t start = 0; start < 40; ++start) {
        clock_ms = 1000;
        bench_log.clear();
        Bench bench(printer_scatter, start * 9, kShakyBenchHandling);
        ready_with_portafilter(bench);
        const GrindOutcome outcome = bench.grind(18.0f);
        if (outcome.phase == GrindPhase::COMPLETED) {
            ++finished;
            CHECK(near(outcome.grounds_g, 18.0, 0.25));
            CHECK(near(outcome.shown_g, outcome.grounds_g, 0.05));
            // A shaken reading was not taken for the first grounds.
            CHECK(bench.controller.get_grind_latency_ms() >= bench.first_grounds_ms());
        }
        bench.dismiss_result();
    }
    if (std::getenv("GRIND_BENCH_VERBOSE")) {
        std::printf("[%s] %d of 40 grinds finished\n", scenario_name, finished);
    }
    CHECK(finished >= 36);
}

static void a_later_grind_measures_its_flow_start_afresh() {
    // On a shaking bench coffee counts as flowing once the averaged weight
    // has risen over the lowest it has been in that run. A portafilter
    // emptied during one grind must not lower that mark for the next, which
    // starts its main run with the prime's grounds already in the basket.
    for (size_t start = 0; start < 30; ++start) {
        clock_ms = 1000;
        bench_log.clear();
        Bench bench(printer_scatter, start * 11, kShakyBenchHandling);
        ready_with_portafilter(bench);
        bench.prefs.values[GrindController::PREF_KEY_GRINDER_MODE] = static_cast<int>(GrinderPurgeMode::PURGE);
        bench.start(18.0f);
        CHECK(bench.run_until_phase(GrindPhase::PURGE_CONFIRM));
        bench.idle(1000);
        bench.lift_vessel();
        bench.idle(3000);
        bench.place_vessel(kPortafilterG);  // Back, emptied
        bench.idle(1500);
        bench.controller.continue_from_purge();
        CHECK(is_result(bench.finish().phase));
        take_away_and_return(bench);

        // The grounds are fresh now: no prompt, and the prime stays in.
        const GrindOutcome outcome = bench.grind(18.0f);
        CHECK(!log_contains("VESSEL REMOVAL"));
        if (outcome.phase == GrindPhase::COMPLETED) {
            CHECK(bench.controller.get_grind_latency_ms() >= bench.first_grounds_ms());
            CHECK(near(outcome.grounds_g, 18.0, 0.25));
        }
        bench.dismiss_result();
    }
}

static void a_chance_still_moment_does_not_make_the_bench_quiet() {
    // Now and then a shaking scale reads still for half a second by chance.
    // That ends the wait it falls in, but the bench is still a shaking one:
    // the corrections must not go back to waiting for a still reading.
    const GrindPhase waits[] = {GrindPhase::TARE_CONFIRM, GrindPhase::PRIME_SETTLING};
    for (GrindPhase wait : waits) {
        clock_ms = 1000;
        bench_log.clear();
        Bench bench(printer_scatter, 0, kShakyBenchHandling);
        ready_with_portafilter(bench);
        bench.start(18.0f);
        CHECK(bench.run_until_phase(wait));
        bench.idle(2200);
        CHECK(bench.controller.get_phase() == wait);
        bench.calm_until_ms = clock_ms + 700;
        bench.idle(700);
        CHECK(bench.controller.get_phase() != wait);  // The calm spell ended the wait

        const GrindOutcome outcome = bench.finish();
        CHECK(outcome.phase == GrindPhase::COMPLETED);
        CHECK(log_contains("using the 2.0s average"));
        CHECK(near(outcome.grounds_g, 18.0, 0.20));
        bench.dismiss_result();
    }
}

static void a_touch_while_taring_is_not_zeroed_in() {
    // On a shaking bench the zero is moved to the average of the readings.
    // A hand that comes to rest on the portafilter after the tare is not
    // vibration: it must not become part of the zero.
    Bench bench(printer_scatter, 0, kShakyBenchHandling);
    ready_with_portafilter(bench);
    bench.start(18.0f);
    CHECK(bench.run_until_phase(GrindPhase::TARE_CONFIRM));
    bench.idle(1700);
    CHECK(!bench.sensor.is_tare_in_progress());
    bench.hand_g = 2.0;
    bench.hand_tremor_g = 0.05;
    CHECK(bench.run_until_phase(GrindPhase::PRIME));
    bench.hand_g = 0.0;
    bench.hand_tremor_g = 0.0;

    const GrindOutcome outcome = bench.finish();
    CHECK(outcome.phase == GrindPhase::COMPLETED);
    CHECK(!log_contains("Zero moved"));
    CHECK(near(outcome.grounds_g, 18.0, 0.25));
}

static void printer_vibration_without_the_handling_waits_as_before() {
    // Shaky-bench handling is off unless it is switched on. Without it the
    // grind waits for a still reading that never comes, as it always did;
    // only the words at the end are new.
    Bench bench(printer_scatter);
    ready_with_portafilter(bench);
    const GrindOutcome outcome = bench.grind(18.0f);

    CHECK(outcome.phase == GrindPhase::TIMEOUT);
    CHECK(outcome.error_title == "TIMEOUT");
    CHECK(outcome.error_message == "Target not reached" || outcome.error_message == "Could not tare");
    CHECK(!log_contains("using the 2.0s average"));
    CHECK(!log_contains("Zero moved"));
    CHECK(!log_contains("Shaky-bench handling is on"));
    // No wait was cut short. (The one after the prime always went on after 10 s.)
    CHECK(!log_contains("in TARE_CONFIRM; carrying on"));
    CHECK(!log_contains("in PULSE_SETTLING; carrying on"));
    bench.dismiss_result();
    bench.idle(2000);

    // A time grind runs its time, then waits for a still reading to report.
    bench_log.clear();
    const GrindOutcome timed = bench.grind(0.0f, GrindMode::TIME, 5000);
    CHECK(timed.phase == GrindPhase::TIMEOUT);
    CHECK(timed.error_title == "TIMEOUT");
    CHECK(timed.error_message == "Time limit reached");
    CHECK(!log_contains("using the 2.0s average"));
}

static void hand_after_the_prime_is_waited_out_as_before() {
    // The wait after the prime is the one that always had a limit: ten
    // seconds. Without shaky-bench handling it is still ten, not four.
    {
        Bench bench(quiet_scatter());
        ready_with_portafilter(bench);
        bench.start(18.0f);
        CHECK(bench.run_until_phase(GrindPhase::PRIME_SETTLING));
        bench.hand_g = 2.0;
        bench.hand_tremor_g = 0.3;
        bench.idle(6000);
        CHECK(bench.controller.get_phase() == GrindPhase::PRIME_SETTLING);
        CHECK(!bench.grinder.output_high());
        bench.hand_g = 0.0;
        bench.hand_tremor_g = 0.0;

        const GrindOutcome outcome = bench.finish();
        CHECK(outcome.phase == GrindPhase::COMPLETED);
        CHECK(near(outcome.grounds_g, 18.0, 0.10));
        CHECK(!log_contains("carrying on"));
        bench.dismiss_result();
    }

    // A hand that stays longer than that does not hold the grind for good.
    clock_ms = 1000;
    bench_log.clear();
    Bench bench(quiet_scatter());
    ready_with_portafilter(bench);
    bench.start(18.0f);
    CHECK(bench.run_until_phase(GrindPhase::PRIME_SETTLING));
    bench.hand_g = 2.0;
    bench.hand_tremor_g = 0.3;
    bench.idle(GRIND_SCALE_SETTLING_TIMEOUT_MS - 500);
    CHECK(bench.controller.get_phase() == GrindPhase::PRIME_SETTLING);
    bench.idle(1000);
    CHECK(bench.controller.get_phase() == GrindPhase::PREDICTIVE);
    CHECK(log_contains("in PRIME_SETTLING; carrying on"));
    bench.controller.stop_grind();
}

static void hand_resting_as_the_main_run_starts() {
    // Purge mode on a shaking bench: the portafilter goes back on and
    // "continue" is pressed with a hand still resting on it, which lets go a
    // moment later. The grind must not take the hand's weight for its
    // starting point and then wait seconds for coffee it has already seen.
    for (double hand_g : {3.0, 10.0}) {
        clock_ms = 1000;
        bench_log.clear();
        Bench bench(printer_scatter, 0, kShakyBenchHandling);
        ready_with_portafilter(bench);
        bench.prefs.values[GrindController::PREF_KEY_GRINDER_MODE] = static_cast<int>(GrinderPurgeMode::PURGE);
        bench.start(18.0f);
        CHECK(bench.run_until_phase(GrindPhase::PURGE_CONFIRM));
        bench.idle(1000);
        bench.lift_vessel();
        bench.idle(3000);
        bench.place_vessel(kPortafilterG);
        bench.hand_g = hand_g;
        bench.hand_tremor_g = 0.05;
        bench.idle(1500);
        bench.controller.continue_from_purge();
        bench.idle(200);
        bench.hand_g = 0.0;
        bench.hand_tremor_g = 0.0;

        const GrindOutcome outcome = bench.finish();
        CHECK(outcome.phase == GrindPhase::COMPLETED);
        CHECK(near(outcome.grounds_g, 18.0, 0.25));
        CHECK(bench.controller.get_grind_latency_ms() < 1500.0f);
        bench.dismiss_result();
    }
}

static void over_target_ends_the_grind(Bench& bench, bool shaking_bench, uint32_t allowed_after_main_run_ms) {
    // The chute lets go of 1.2 g when the main run stops (not after the prime).
    bench.retained_drop_g = 1.2;
    ready_with_portafilter(bench);
    const GrindOutcome outcome = bench.grind(18.0f);

    CHECK(outcome.phase == GrindPhase::COMPLETED);
    CHECK(outcome.grounds_g > 18.3);
    CHECK(outcome.pulses == 0);
    CHECK(outcome.result == GrindSessionResult::OVERSHOOT);
    CHECK(near(outcome.shown_g, outcome.grounds_g, 0.05));
    CHECK(bench.time_after_main_run() <= allowed_after_main_run_ms);
    if (shaking_bench) bench.check_settle_limits();
}

static void over_target_on_a_quiet_bench() {
    Bench bench(quiet_scatter());
    over_target_ends_the_grind(bench, false, 2500);
}

static void over_target_with_printer_vibration() {
    Bench bench(printer_scatter, 0, kShakyBenchHandling);
    over_target_ends_the_grind(bench, true, 4800);
}

static void second_grind_knows_the_portafilter(Bench& bench, GrindPhase lift_in_phase) {
    ready_with_portafilter(bench);
    CHECK(bench.grind(18.0f).phase == GrindPhase::COMPLETED);
    take_away_and_return(bench);

    // The scale was zeroed with the portafilter on, so it reads next to nothing.
    bench_log.clear();
    CHECK(std::fabs(bench.sensor.get_weight_smoothed(500)) < 3.0f);
    bench.start(18.0f);
    CHECK(bench.run_until_phase(lift_in_phase));
    CHECK(!log_contains("No vessel reference"));
    CHECK(near(log_number_after("Vessel reference: "), kPortafilterG + 0.4, 0.5));

    bench.idle(600);
    CHECK(!is_result(bench.controller.get_phase()));
    bench.lift_vessel();
    const uint32_t lifted_ms = clock_ms;

    const GrindOutcome outcome = bench.finish();
    CHECK(outcome.phase == GrindPhase::TIMEOUT);
    CHECK(outcome.error_message == "Portafilter removed");
    CHECK(outcome.error_title == "STOPPED");
    // Stopped within a few scale samples of the lift, and no pulse after it.
    CHECK(clock_ms - lifted_ms <= 400 + 200);
    CHECK(log_contains("VESSEL REMOVAL FAILSAFE"));
}

static void removal_stops_the_second_grind_while_grinding() {
    Bench bench(quiet_scatter());
    second_grind_knows_the_portafilter(bench, GrindPhase::PREDICTIVE);
}

static void removal_stops_the_second_grind_while_settling() {
    // Motor off, waiting on a vibrating bench: the case reported from the grinder.
    Bench bench(printer_scatter, 0, kShakyBenchHandling);
    second_grind_knows_the_portafilter(bench, GrindPhase::PULSE_DECISION);
}

static void pushed_up_portafilter_is_not_a_removal() {
    Bench bench(quiet_scatter());
    ready_with_portafilter(bench);
    CHECK(bench.grind(18.0f).phase == GrindPhase::COMPLETED);
    take_away_and_return(bench);

    bench.start(18.0f);
    CHECK(bench.run_until_phase(GrindPhase::PREDICTIVE));
    bench.idle(1500);
    bench.held_up_g = 300.0;  // Wedged in its holder for a second
    bench.idle(1000);
    bench.held_up_g = 0.0;
    bench.idle(500);
    CHECK(!log_contains("VESSEL REMOVAL"));
    CHECK(bench.controller.get_phase() != GrindPhase::TIMEOUT);
    bench.controller.stop_grind();
}

static void removal_stops_a_time_grind_on_a_scale_zeroed_with_the_portafilter() {
    Bench bench(quiet_scatter());
    ready_with_portafilter(bench);
    CHECK(bench.grind(18.0f).phase == GrindPhase::COMPLETED);
    take_away_and_return(bench);

    // A time grind does not tare: it starts from the 0.4 g the scale reads.
    bench_log.clear();
    bench.start(0.0f, GrindMode::TIME, 8000);
    CHECK(bench.run_until_phase(GrindPhase::TIME_GRINDING));
    bench.idle(2500);
    CHECK(bench.controller.get_phase() == GrindPhase::TIME_GRINDING);
    CHECK(near(log_number_after("Vessel reference: "), kPortafilterG + 0.4, 0.5));

    bench.lift_vessel();
    const GrindOutcome outcome = bench.finish();
    CHECK(outcome.phase == GrindPhase::TIMEOUT);
    CHECK(outcome.error_message == "Portafilter removed");
    CHECK(outcome.duration_ms < 4000);
}

static void purge_mode_lets_the_portafilter_be_emptied() {
    // The default mode: after the prime the grinder waits while the stale
    // grounds are tipped out. Lifting the portafilter then is not a removal.
    Bench bench(printer_scatter, 0, kShakyBenchHandling);
    ready_with_portafilter(bench);
    bench.prefs.values[GrindController::PREF_KEY_GRINDER_MODE] = static_cast<int>(GrinderPurgeMode::PURGE);
    bench.start(18.0f);
    CHECK(bench.run_until_phase(GrindPhase::PURGE_CONFIRM));
    CHECK(!bench.grinder.output_high());

    bench.idle(1000);
    bench.lift_vessel();
    bench.idle(4000);
    bench.place_vessel(kPortafilterG);  // Back, emptied
    bench.idle(1500);
    CHECK(bench.controller.get_phase() == GrindPhase::PURGE_CONFIRM);
    bench.controller.continue_from_purge();

    const GrindOutcome outcome = bench.finish();
    CHECK(outcome.phase == GrindPhase::COMPLETED);
    CHECK(!log_contains("VESSEL REMOVAL"));
    // The scale was zeroed before the prime, so the dose is counted from there.
    CHECK(near(outcome.grounds_g, 18.0, 0.15));
    CHECK(near(outcome.shown_g, outcome.grounds_g, 0.05));
    // The time spent emptying the portafilter does not count towards the limit.
    CHECK(outcome.duration_ms < 45000);
    bench.check_settle_limits();
}

static void manual_grind_keeps_its_safety_limit_and_removal_stop() {
    Bench bench(quiet_scatter());
    ready_with_portafilter(bench);
    CHECK(bench.grind(18.0f).phase == GrindPhase::COMPLETED);
    take_away_and_return(bench);

    // Left running, a manual grind stops by itself with its own words.
    bench.flow_gps = 0.0;
    GrindOutcome outcome = bench.grind(0.0f, GrindMode::MANUAL);
    CHECK(outcome.phase == GrindPhase::TIMEOUT);
    CHECK(outcome.error_message == "30s safety limit");
    CHECK(outcome.error_title.empty());
    CHECK(near(outcome.duration_ms, MANUAL_GRIND_TIMEOUT_SEC * 1000.0, 1000.0));
    bench.dismiss_result();
    bench.idle(2000);

    // On a scale zeroed with the portafilter on, lifting it stops the motor.
    bench.flow_gps = 1.7;
    bench_log.clear();
    bench.start(0.0f, GrindMode::MANUAL);
    CHECK(bench.run_until_phase(GrindPhase::MANUAL_GRINDING));
    bench.idle(2000);
    CHECK(bench.controller.get_phase() == GrindPhase::MANUAL_GRINDING);
    bench.lift_vessel();
    outcome = bench.finish();
    CHECK(outcome.phase == GrindPhase::TIMEOUT);
    CHECK(outcome.error_message == "Portafilter removed");
    CHECK(outcome.duration_ms < 3500);
}

// ---------------------------------------------------------------------------
// Things a person does to a grind. The first version of the settle limits got
// each of these wrong; a review caught them.
// ---------------------------------------------------------------------------

static void nudge_while_taring_does_not_shift_the_zero() {
    // Still letting go of the handle a moment after the start.
    for (double held_g : {20.0, -20.0}) {
        clock_ms = 1000;
        Bench bench(quiet_scatter());
        ready_with_portafilter(bench);
        bench.start(18.0f);
        bench.idle(600);
        bench.held_up_g = held_g;
        bench.idle(400);
        bench.held_up_g = 0.0;
        const GrindOutcome outcome = bench.finish();

        CHECK(outcome.phase == GrindPhase::COMPLETED);
        CHECK(near(outcome.grounds_g, 18.0, 0.10));
        CHECK(near(outcome.shown_g, outcome.grounds_g, 0.03));
        bench.dismiss_result();
    }
}

// A hand takes hold as the main run stops and stays for `held_ms`. Nothing may
// be decided on those readings: no pulse and no result. The grind carries on
// once the hand is gone and still delivers its dose.
static void hold_through_the_corrections(const std::vector<float>& scatter, bool shaky_bench_handling,
                                         double hand_g, double tremor_g, double dose_tolerance_g) {
    clock_ms = 1000;
    bench_log.clear();
    Bench bench(scatter, 0, shaky_bench_handling);
    ready_with_portafilter(bench);
    bench.start(18.0f);
    CHECK(bench.run_until_phase(GrindPhase::PULSE_SETTLING));
    bench.hand_g = hand_g;
    bench.hand_tremor_g = tremor_g;
    bench.idle(9000);
    CHECK(bench.grinder.pulses == 0);
    CHECK(!is_result(bench.controller.get_phase()));
    if (!shaky_bench_handling) {
        // As it always was: the grounds are not judged while the scale moves.
        CHECK(bench.controller.get_phase() == GrindPhase::PULSE_SETTLING);
    }

    bench.hand_g = 0.0;
    bench.hand_tremor_g = 0.0;
    const GrindOutcome outcome = bench.finish();
    CHECK(outcome.phase == GrindPhase::COMPLETED);
    CHECK(near(outcome.grounds_g, 18.0, dose_tolerance_g));
    CHECK(near(outcome.shown_g, outcome.grounds_g, 0.05));
    bench.dismiss_result();
}

static void hand_on_a_quiet_bench_decides_nothing() {
    // Without shaky-bench handling every wait is for a still reading, as it
    // always was. With it on, the scale read still after taring and after
    // the prime, so this grind has no reason to think its bench shakes.
    // Either way it waits, however steady the hand is.
    const double hands[][2] = {
        {-3.0, 0.3},   // carrying 3 g, shakily
        {-3.0, 0.05},  // carrying 3 g, steadily
        {2.0, 0.5},    // pressing down
        {0.5, 0.1},    // resting lightly on the handle
        {-0.8, 0.1},   // taking a little of the weight
    };
    for (const auto& hand : hands) {
        for (bool handling : {false, kShakyBenchHandling}) {
            hold_through_the_corrections(quiet_scatter(), handling, hand[0], hand[1], 0.10);
            CHECK(log_contains("waiting for a still reading"));
        }
    }
}

static void hand_on_a_shaking_bench_decides_nothing() {
    // Here the grind does go by averages. A hand still gives itself away: it
    // scatters the readings more than the bench does, or it lowers the
    // weight, which coffee never does.
    const double hands[][2] = {{-3.0, 0.3}, {-3.0, 0.05}, {2.0, 0.5}};
    for (const auto& hand : hands) {
        hold_through_the_corrections(printer_scatter, kShakyBenchHandling, hand[0], hand[1], 0.15);
        CHECK(log_contains("no steady average to use"));
    }
}

static void brief_touch_does_not_end_the_grind_short() {
    // Resting on the handle for a moment pushes the reading over the target.
    Bench bench(quiet_scatter());
    ready_with_portafilter(bench);
    bench.start(18.0f);
    CHECK(bench.run_until_phase(GrindPhase::PULSE_SETTLING));
    bench.idle(600);
    bench.hand_g = 0.6;
    bench.hand_tremor_g = 0.2;
    bench.idle(900);
    bench.hand_g = 0.0;
    bench.hand_tremor_g = 0.0;
    const GrindOutcome outcome = bench.finish();

    CHECK(outcome.phase == GrindPhase::COMPLETED);
    CHECK(outcome.pulses >= 1);
    CHECK(near(outcome.grounds_g, 18.0, 0.10));
    CHECK(near(outcome.shown_g, outcome.grounds_g, 0.03));
}

static void trickling_grounds_are_weighed_to_the_end() {
    // Grounds keep dribbling for five seconds after the main run. Nothing is
    // decided or reported from an average while the weight is still climbing.
    const std::vector<float> benches[] = {quiet_scatter(), printer_scatter};
    for (const std::vector<float>& scatter : benches) {
        clock_ms = 1000;
        Bench bench(scatter, 0, &scatter == &benches[1]);
        bench.trickle_gps = 0.15;
        bench.trickle_ms = 5000;
        ready_with_portafilter(bench);
        const GrindOutcome outcome = bench.grind(18.0f);

        CHECK(outcome.phase == GrindPhase::COMPLETED);
        CHECK(outcome.pulses == 0);  // The trickle alone takes it over the target
        CHECK(near(outcome.shown_g, outcome.grounds_g, 0.08));
        bench.idle(3000);
        CHECK(near(bench.grounds_g, outcome.grounds_g, 0.02));  // All of it had landed
        bench.dismiss_result();
    }
}

static void time_grind_with_a_long_chute() {
    // The final weight of a time grind is taken once the grounds are in.
    Bench bench(printer_scatter, 0, kShakyBenchHandling);
    bench.transit_ms = 1500;
    ready_with_portafilter(bench);
    const GrindOutcome outcome = bench.grind(0.0f, GrindMode::TIME, 6000);

    CHECK(outcome.phase == GrindPhase::COMPLETED);
    CHECK(near(outcome.shown_g, kPortafilterG + outcome.grounds_g, 0.08));
    bench.idle(3000);
    CHECK(near(bench.grounds_g, outcome.grounds_g, 0.02));
}

static void tray_taken_off_and_put_back() {
    // Wiping under the tray makes the tracker take the lighter scale for the
    // empty one. The weight measured at a grind's own zero is not fooled.
    Bench bench(quiet_scatter());
    bench.tray_g = 150.0;
    bench.power_up();
    bench.idle(3000);
    bench.tray_g = 0.0;
    bench.idle(5000);
    bench.tray_g = 150.0;
    bench.idle(3000);
    bench.place_vessel(kPortafilterG);
    bench.idle(2000);

    // First grind, scale zeroed empty: the reading is the portafilter.
    bench.start(18.0f);
    CHECK(bench.run_until_phase(GrindPhase::PREDICTIVE));
    CHECK(near(log_number_after("Vessel reference: "), kPortafilterG, 0.5));
    CHECK(bench.finish().phase == GrindPhase::COMPLETED);
    take_away_and_return(bench);

    // Later grind, scale zeroed with the portafilter on: carried over.
    bench_log.clear();
    bench.start(18.0f);
    CHECK(bench.run_until_phase(GrindPhase::PREDICTIVE));
    CHECK(near(log_number_after("Vessel reference: "), kPortafilterG + 0.4, 0.5));
    bench.idle(2500);
    bench.lift_vessel();
    const GrindOutcome outcome = bench.finish();
    CHECK(outcome.phase == GrindPhase::TIMEOUT);
    CHECK(outcome.error_message == "Portafilter removed");
}

static void a_tare_by_hand_starts_the_weight_afresh() {
    // The weight carried from a grind's zero is only good on that zero.
    Bench bench(quiet_scatter());
    ready_with_portafilter(bench);
    CHECK(bench.grind(18.0f).phase == GrindPhase::COMPLETED);
    bench.idle(1500);
    bench.lift_vessel();
    bench.idle(1500);
    bench.dismiss_result();
    bench.sensor.tareNoDelay();  // Tare pressed with the scale empty
    bench.idle(4000);
    bench.place_vessel(kPortafilterG + 0.4);
    bench.idle(2000);

    bench_log.clear();
    bench.start(18.0f);
    CHECK(bench.run_until_phase(GrindPhase::PREDICTIVE));
    CHECK(near(log_number_after("Vessel reference: "), kPortafilterG + 0.4, 0.5));
    bench.idle(1500);
    bench.lift_vessel();
    const GrindOutcome outcome = bench.finish();
    CHECK(outcome.phase == GrindPhase::TIMEOUT);
    CHECK(outcome.error_message == "Portafilter removed");
}

static void a_tare_by_hand_drops_the_carried_weight() {
    // Portafilter ground into, then swapped for a 60 g cup that is tared by
    // hand. The portafilter's weight belongs to the zero the grind took, not
    // to this one: the cup is what the tracker says it is.
    Bench bench(quiet_scatter());
    ready_with_portafilter(bench);
    CHECK(bench.grind(18.0f).phase == GrindPhase::COMPLETED);
    bench.idle(1500);
    bench.lift_vessel();
    bench.idle(1500);
    bench.dismiss_result();
    bench.idle(2000);
    bench.place_vessel(60.0);
    bench.idle(2000);
    bench.sensor.tareNoDelay();
    bench.idle(4000);

    bench_log.clear();
    bench.start(18.0f);
    CHECK(bench.run_until_phase(GrindPhase::PREDICTIVE));
    CHECK(near(log_number_after("Vessel reference: "), 60.0, 0.5));
    bench.controller.stop_grind();
}

static void portafilter_reseated_while_taring_is_guarded_next_time() {
    // Lifted and set down again while the grind takes its zero: the zero is
    // that of the empty scale, and the portafilter then reads its full
    // weight. On the next grind that reading is the reference, as it always
    // was, not the reading plus a weight carried from the spoilt zero.
    Bench bench(quiet_scatter());
    ready_with_portafilter(bench);
    bench.start(18.0f);
    CHECK(bench.run_until_phase(GrindPhase::TARE_CONFIRM));
    const double portafilter_g = bench.vessel_g;
    bench.vessel_g = 0.0;
    bench.idle(1700);
    bench.vessel_g = portafilter_g;
    const GrindOutcome spoilt = bench.finish();
    CHECK(is_result(spoilt.phase));
    bench.idle(1000);
    bench.dismiss_result();
    bench.idle(1000);

    // The reading: the portafilter and what little the spoilt grind put in it.
    bench_log.clear();
    bench.start(18.0f);
    CHECK(bench.run_until_phase(GrindPhase::TARE_CONFIRM));
    CHECK(near(log_number_after("Vessel reference: "), kPortafilterG + bench.grounds_g, 0.5));
    bench.controller.stop_grind();
}

static void second_dose_without_lifting_is_guarded_as_before() {
    // Grinding again into a portafilter that was never lifted: the scale
    // reads the first dose, and that reading is the reference, as it always
    // was. Lifting the portafilter still stops the grind.
    Bench bench(quiet_scatter());
    ready_with_portafilter(bench);
    const GrindOutcome first = bench.grind(9.0f);
    CHECK(first.phase == GrindPhase::COMPLETED);
    bench.idle(1500);
    bench.dismiss_result();
    bench.idle(1000);

    bench_log.clear();
    bench.start(9.0f);
    CHECK(bench.run_until_phase(GrindPhase::PREDICTIVE));
    CHECK(near(log_number_after("Vessel reference: "), first.grounds_g, 0.2));
    bench.idle(1500);
    bench.lift_vessel();
    const GrindOutcome outcome = bench.finish();
    CHECK(outcome.phase == GrindPhase::TIMEOUT);
    CHECK(outcome.error_message == "Portafilter removed");
}

static void bad_scale_readings_are_not_a_removal() {
    // One or two bad conversions in a row, far below the zero, at every
    // timing against the control loop.
    for (int trial = 0; trial < 4; ++trial) {
        const int bad_in_a_row = 1 + trial % 2;
        const double depth_g = trial < 2 ? 600.0 : 1000.0;
        for (uint32_t offset_ms = 0; offset_ms < kAdcPeriodMs; offset_ms += kControlPeriodMs) {
            clock_ms = 1000;
            bench_log.clear();
            Bench bench(quiet_scatter());
            ready_with_portafilter(bench);
            CHECK(bench.grind(18.0f).phase == GrindPhase::COMPLETED);
            take_away_and_return(bench);
            bench.start(18.0f);
            CHECK(bench.run_until_phase(GrindPhase::PREDICTIVE));
            bench.idle(2500 + offset_ms);
            bench.bad_conversion_g = bench.vessel_g - depth_g;
            bench.bad_conversions = bad_in_a_row;
            bench.idle(1000);

            CHECK(!log_contains("VESSEL REMOVAL"));
            CHECK(bench.controller.get_phase() != GrindPhase::TIMEOUT);
            bench.controller.stop_grind();
        }
    }
}

static void lift_after_the_prime_does_not_restart_the_motor() {
    // No purge prompt is due, so after the prime settles the motor starts
    // again for the main run. Not onto an empty scale.
    const uint32_t lift_after_ms[] = {200, GRIND_PRIME_SETTLE_LIMIT_MS - 150};
    for (uint32_t lift_ms : lift_after_ms) {
        clock_ms = 1000;
        bench_log.clear();
        Bench bench(printer_scatter, 0, kShakyBenchHandling);
        ready_with_portafilter(bench);
        CHECK(bench.grind(18.0f).phase == GrindPhase::COMPLETED);
        take_away_and_return(bench);

        bench.start(18.0f);
        CHECK(bench.run_until_phase(GrindPhase::PRIME_SETTLING));
        bench.idle(lift_ms);
        CHECK(bench.controller.get_phase() == GrindPhase::PRIME_SETTLING);
        const int starts_before = bench.grinder.continuous_starts;
        bench.lift_vessel();
        const GrindOutcome outcome = bench.finish();

        CHECK(outcome.phase == GrindPhase::TIMEOUT);
        CHECK(outcome.error_message == "Portafilter removed");
        CHECK(bench.grinder.continuous_starts == starts_before);
        bench.dismiss_result();
    }
}

static void a_tare_in_progress_carries_no_weight() {
    // A manual grind does not wait for a tare. While one runs, the old zero
    // says nothing about what is on the scale.
    Bench bench(quiet_scatter());
    ready_with_portafilter(bench);
    CHECK(bench.grind(18.0f).phase == GrindPhase::COMPLETED);
    bench.idle(1500);
    bench.lift_vessel();
    bench.idle(1500);
    bench.dismiss_result();
    bench.idle(3000);

    bench_log.clear();
    bench.sensor.tareNoDelay();
    bench.idle(100);
    CHECK(bench.sensor.is_tare_in_progress());
    bench.start(0.0f, GrindMode::MANUAL);
    CHECK(bench.run_until_phase(GrindPhase::MANUAL_GRINDING));
    CHECK(log_contains("No vessel reference"));
    bench.controller.stop_grind();
}

static void early_lift_before_the_purge_prompt() {
    // Purge mode on a later grind: the portafilter is taken off to be emptied
    // as soon as the prime stops, before the prompt is on the screen.
    Bench bench(printer_scatter, 0, kShakyBenchHandling);
    ready_with_portafilter(bench);
    CHECK(bench.grind(18.0f).phase == GrindPhase::COMPLETED);
    take_away_and_return(bench);
    bench.prefs.values[GrindController::PREF_KEY_GRINDER_MODE] = static_cast<int>(GrinderPurgeMode::PURGE);
    bench.prefs.values[GrindController::PREF_KEY_GRIND_FRESHNESS_HOURS] = 0.0;  // The grounds count as stale

    bench_log.clear();
    bench.start(18.0f);
    CHECK(bench.run_until_phase(GrindPhase::PRIME_SETTLING));
    bench.idle(1000);
    CHECK(bench.controller.get_phase() == GrindPhase::PRIME_SETTLING);
    bench.lift_vessel();
    CHECK(bench.run_until_phase(GrindPhase::PURGE_CONFIRM, 6000));
    CHECK(!log_contains("VESSEL REMOVAL"));

    bench.idle(3000);
    bench.place_vessel(kPortafilterG + 0.4);
    bench.idle(1500);
    bench.controller.continue_from_purge();
    const GrindOutcome outcome = bench.finish();
    CHECK(outcome.phase == GrindPhase::COMPLETED);
    CHECK(near(outcome.grounds_g, 18.0, 0.15));
}

static void power_up_with_the_portafilter_on() {
    Bench bench(quiet_scatter());
    bench.place_vessel(kPortafilterG);
    bench.power_up();
    bench.idle(3000);

    // Nothing to compare the portafilter with yet: no guard, as before.
    CHECK(bench.grind(18.0f).phase == GrindPhase::COMPLETED);
    CHECK(log_contains("No vessel reference"));

    // Lifted off once, and the next grind knows its weight.
    take_away_and_return(bench);
    bench_log.clear();
    CHECK(bench.grind(18.0f).phase == GrindPhase::COMPLETED);
    CHECK(near(log_number_after("Vessel reference: "), kPortafilterG + 0.4, 0.5));
}

static void empty_hopper_times_out_in_plain_words() {
    Bench bench(quiet_scatter());
    bench.flow_gps = 0.0;
    ready_with_portafilter(bench);
    const GrindOutcome outcome = bench.grind(18.0f);

    CHECK(outcome.phase == GrindPhase::TIMEOUT);
    CHECK(outcome.result == GrindSessionResult::TIMEOUT);
    CHECK(outcome.error_title == "TIMEOUT");
    CHECK(outcome.error_message == "Target not reached");
    CHECK(near(outcome.duration_ms, GRIND_TIMEOUT_SEC * 1000.0, 1500.0));
    CHECK(log_contains("GRIND TIMEOUT in phase PREDICTIVE"));
}

static void time_grind_ends(Bench& bench, uint32_t allowed_after_motor_ms) {
    ready_with_portafilter(bench);
    const uint32_t grind_time_ms = 5000;
    const GrindOutcome outcome = bench.grind(0.0f, GrindMode::TIME, grind_time_ms);

    CHECK(outcome.phase == GrindPhase::COMPLETED);
    CHECK(outcome.duration_ms <= grind_time_ms + allowed_after_motor_ms);
    // Time grinds do not tare: the result is the portafilter with the grounds.
    CHECK(near(outcome.shown_g, kPortafilterG + outcome.grounds_g, 0.08));
    CHECK(bench.longest(GrindPhase::FINAL_SETTLING) <= GRIND_WEIGHT_SETTLE_LIMIT_MS + 2 * kControlPeriodMs);
}

static void time_grind_on_a_quiet_bench() {
    Bench bench(quiet_scatter());
    time_grind_ends(bench, 1800);
}

static void time_grind_with_printer_vibration() {
    Bench bench(printer_scatter, 0, kShakyBenchHandling);
    time_grind_ends(bench, GRIND_WEIGHT_SETTLE_LIMIT_MS + 400);
}

static void grinds_across_grinders_doses_and_benches() {
    // Slow and fast grinders, short and long chutes, small and large doses,
    // on a quiet and an ordinary bench as shipped, and on a vibrating one
    // with shaky-bench handling switched on. Whatever the scale does,
    // a grind has to finish, report what really landed and never give up
    // short of its target while it still has corrections left.
    const std::vector<float> benches[] = {quiet_scatter(), normal_scatter, printer_scatter};
    const double flows_gps[] = {1.2, 1.7, 2.4};
    const uint32_t transits_ms[] = {300, 450, 650};
    const float targets_g[] = {9.0f, 18.0f, 30.0f};

    for (const std::vector<float>& scatter : benches) {
        for (double flow_gps : flows_gps) {
            for (uint32_t transit_ms : transits_ms) {
                for (float target_g : targets_g) {
                    const bool shaking_bench = &scatter == &benches[2];
                    clock_ms = 1000;
                    bench_log.clear();
                    Bench bench(scatter, 0, shaking_bench);
                    bench.flow_gps = flow_gps;
                    bench.transit_ms = transit_ms;
                    ready_with_portafilter(bench);
                    const GrindOutcome outcome = bench.grind(target_g);

                    CHECK(outcome.phase == GrindPhase::COMPLETED);
                    CHECK(near(outcome.shown_g, outcome.grounds_g, 0.06));
                    CHECK(outcome.grounds_g > target_g - 0.12 || outcome.pulses >= GRIND_MAX_PULSE_ATTEMPTS);
                    CHECK(outcome.grounds_g < target_g + 0.60);
                    CHECK(!log_contains("VESSEL REMOVAL"));
                    if (shaking_bench) bench.check_settle_limits();
                    bench.dismiss_result();  // Frees the motor for the next bench
                }
            }
        }
    }
}

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: bench <scenario> <printer vibration trace> <normal bench trace>\n");
        return 2;
    }
    // Readings after the grounds had landed and with the motor off.
    printer_scatter = scatter_of_trace(argv[2], 30551, 60000);
    normal_scatter = scatter_of_trace(argv[3], 16006, 21486);

    const std::map<std::string, void (*)()> scenarios = {
        {"quiet_bench_grind", quiet_bench_grind},
        {"normal_bench_grind", normal_bench_grind},
        {"normal_bench_with_the_handling_on", normal_bench_with_the_handling_on},
        {"printer_vibration_grind", printer_vibration_grind},
        {"printer_vibration_is_recognised_from_any_moment", printer_vibration_is_recognised_from_any_moment},
        {"a_later_grind_measures_its_flow_start_afresh", a_later_grind_measures_its_flow_start_afresh},
        {"printer_vibration_without_the_handling_waits_as_before",
         printer_vibration_without_the_handling_waits_as_before},
        {"hand_after_the_prime_is_waited_out_as_before", hand_after_the_prime_is_waited_out_as_before},
        {"hand_resting_as_the_main_run_starts", hand_resting_as_the_main_run_starts},
        {"a_chance_still_moment_does_not_make_the_bench_quiet", a_chance_still_moment_does_not_make_the_bench_quiet},
        {"a_touch_while_taring_is_not_zeroed_in", a_touch_while_taring_is_not_zeroed_in},
        {"over_target_on_a_quiet_bench", over_target_on_a_quiet_bench},
        {"over_target_with_printer_vibration", over_target_with_printer_vibration},
        {"removal_stops_the_second_grind_while_grinding", removal_stops_the_second_grind_while_grinding},
        {"removal_stops_the_second_grind_while_settling", removal_stops_the_second_grind_while_settling},
        {"pushed_up_portafilter_is_not_a_removal", pushed_up_portafilter_is_not_a_removal},
        {"removal_stops_a_time_grind_on_a_scale_zeroed_with_the_portafilter",
         removal_stops_a_time_grind_on_a_scale_zeroed_with_the_portafilter},
        {"purge_mode_lets_the_portafilter_be_emptied", purge_mode_lets_the_portafilter_be_emptied},
        {"manual_grind_keeps_its_safety_limit_and_removal_stop",
         manual_grind_keeps_its_safety_limit_and_removal_stop},
        {"nudge_while_taring_does_not_shift_the_zero", nudge_while_taring_does_not_shift_the_zero},
        {"hand_on_a_quiet_bench_decides_nothing", hand_on_a_quiet_bench_decides_nothing},
        {"hand_on_a_shaking_bench_decides_nothing", hand_on_a_shaking_bench_decides_nothing},
        {"brief_touch_does_not_end_the_grind_short", brief_touch_does_not_end_the_grind_short},
        {"trickling_grounds_are_weighed_to_the_end", trickling_grounds_are_weighed_to_the_end},
        {"time_grind_with_a_long_chute", time_grind_with_a_long_chute},
        {"tray_taken_off_and_put_back", tray_taken_off_and_put_back},
        {"a_tare_by_hand_starts_the_weight_afresh", a_tare_by_hand_starts_the_weight_afresh},
        {"a_tare_by_hand_drops_the_carried_weight", a_tare_by_hand_drops_the_carried_weight},
        {"portafilter_reseated_while_taring_is_guarded_next_time",
         portafilter_reseated_while_taring_is_guarded_next_time},
        {"second_dose_without_lifting_is_guarded_as_before", second_dose_without_lifting_is_guarded_as_before},
        {"bad_scale_readings_are_not_a_removal", bad_scale_readings_are_not_a_removal},
        {"lift_after_the_prime_does_not_restart_the_motor", lift_after_the_prime_does_not_restart_the_motor},
        {"a_tare_in_progress_carries_no_weight", a_tare_in_progress_carries_no_weight},
        {"early_lift_before_the_purge_prompt", early_lift_before_the_purge_prompt},
        {"power_up_with_the_portafilter_on", power_up_with_the_portafilter_on},
        {"empty_hopper_times_out_in_plain_words", empty_hopper_times_out_in_plain_words},
        {"time_grind_on_a_quiet_bench", time_grind_on_a_quiet_bench},
        {"time_grind_with_printer_vibration", time_grind_with_printer_vibration},
        {"grinds_across_grinders_doses_and_benches", grinds_across_grinders_doses_and_benches},
    };
    const auto scenario = scenarios.find(argv[1]);
    if (scenario == scenarios.end()) {
        std::fprintf(stderr, "unknown scenario %s\n", argv[1]);
        return 2;
    }
    scenario_name = argv[1];
    scenario->second();
    return 0;
}
