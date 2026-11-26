#pragma once
#include "../config/constants.h"
#include "../hardware/WeightSensor.h"
#include "../hardware/grinder.h"
#include "../logging/grind_logging.h"
#include "grind_mode.h"
#include "grind_ui_events.h"
#include "grind_session.h"
#include "grind_session_result.h"
#include "grind_strategy.h"
#include "coast_learning.h"
#include "net_weight_guard.h"
#include "settle_wait_policy.h"
#include "vessel_weight_tracker.h"
#include "weight_grind_strategy.h"
#include "time_grind_strategy.h"
#include "../system/operation_interlock.h"
#include <Preferences.h>
#include <LittleFS.h>
#include <mutex>
#include <freertos/FreeRTOS.h>
#include <freertos/queue.h>

class DiagnosticsController;

// Flash operation request structure for Core 0 → Core 1 communication
struct FlashOpRequest {
    enum Type {
        START_GRIND_SESSION,
        END_GRIND_SESSION,
        UPDATE_MANUAL_RUNTIME,
        SAVE_LEARNED_COAST
    };
    
    Type operation_type;
    GrindSessionDescriptor descriptor; // For START_GRIND_SESSION
    char result_string[32];  // "COMPLETE", "TIMEOUT", "OVERSHOOT", etc. (for END_GRIND_SESSION)
    float start_weight;      // For START_GRIND_SESSION (pre-tare snapshot)
    float final_weight;      // For END_GRIND_SESSION
    uint8_t pulse_count;     // For END_GRIND_SESSION
    uint32_t completed_at_ms; // Absolute clock at terminal phase entry, not save time
    uint32_t motor_runtime_ms; // For UPDATE_MANUAL_RUNTIME
    float learned_coast_ms;    // For SAVE_LEARNED_COAST
    uint32_t learned_coast_grinds;
};

// Log message structure for Core 0 → Core 1 communication
struct LogMessage {
    char message[128];  // Pre-formatted log message
};

// Calculated values for a single update cycle - passed to methods to avoid redundant calculations
struct GrindLoopData {
    float current_weight;       // For control logic (low_latency)
    float display_weight;       // For UI display (always calculated)
    uint32_t timestamp_ms;
    float weight_delta;
    float flow_rate;
    uint8_t motor_is_on;
    uint8_t phase_id;
    unsigned long now;
};

struct PulseReport {
    float start_weight;
    float end_weight;
    float duration_ms;
};



// Controls the grinding process with predictive weight stopping and precision pulse corrections
class GrindController {
private:
    // Serialize complete state transitions, not just individual field writes.
    // Recursive because strategies/getters call back into the same controller.
    mutable std::recursive_mutex control_mutex_;
    OperationInterlock::Token operation_token_ = 0;
    friend class WeightGrindStrategy;
    friend class TimeGrindStrategy;

    WeightSensor* weight_sensor;
    Grinder* grinder;
    Preferences* preferences;
    float target_weight;
    uint32_t target_time_ms;
    GrindPhase phase;
    unsigned long start_time;
    unsigned long phase_start_time;
    unsigned long time_grind_start_ms;
    
    float tolerance;
    GrindMode mode;
    GrinderPurgeMode grinder_purge_mode_for_session;
    float grinder_purge_amount_g_for_session;

    // Timeout tracking
    GrindPhase timeout_phase;   // Phase when timeout occurred
    unsigned long timeout_pause_start;  // When we entered a paused state (PURGE_CONFIRM)
    unsigned long timeout_offset_ms;    // Accumulated time in paused states to exclude from timeout
    
    int pulse_attempts;
    unsigned long pulse_start_time;
    float current_pulse_duration_ms;
    
    
    
    float predictive_end_weight;
    volatile float grind_latency_ms;        // Thread-safe for Core 0 access
    PulseReport pulse_history[GRIND_MAX_PULSE_ATTEMPTS];
    volatile float motor_stop_target_weight; // Thread-safe for Core 0 access
    float final_weight; // Stores the final settled weight from final_measurement()

    // Flow detection confirmation variables
    bool flow_start_confirmed;
    // Dynamic pulse algorithm variables
    volatile float pulse_flow_rate;    // Thread-safe for Core 0 access
    
    // Loop counter variables for performance tracking
    volatile uint16_t current_phase_loop_count;  // Thread-safe for Core 0 access

    // Logging support
    uint8_t current_profile_id;
    GrindEvent event_in_progress; // Used to build data for the current phase event
    
    // State tracking for measurement calculations (eliminates calculations in logger)
    float last_logged_weight;       // Previous weight for delta calculation
    unsigned long last_logged_time; // Previous timestamp for relative timing
    bool force_measurement_log;     // Flag to force measurement logging on next update cycle

    // UI event system - thread-safe Core 0 → Core 1 communication
    GrindUIEventMailbox ui_events_;
    portMUX_TYPE ui_event_lock_ = portMUX_INITIALIZER_UNLOCKED;
    
    bool control_loop_paused_;      // Indicates control loop is suspended (e.g., purge confirmation)

    // Time mode pause state
    bool grind_paused_;
    uint32_t pause_start_ms_;
    uint32_t total_pause_ms_;
    
    // Flash operation queue - thread-safe Core 0 → Core 1 communication
    QueueHandle_t flash_op_queue;
    static const int FLASH_OP_QUEUE_SIZE = 5;
    
    // Log message queue - thread-safe Core 0 → Core 1 communication
    QueueHandle_t log_queue;
    static const int LOG_QUEUE_SIZE = 20;
    
    // Time mode pulse tracking
    int additional_pulse_count;
    uint32_t pulse_duration_ms;
    
    void (*ui_event_callback)(const GrindEventData&) = nullptr;
    bool ui_ready_for_setup = false; // Flag to track UI acknowledgment of INITIALIZING phase
    
    // Flag to prevent repeated flash operations for terminal phases (COMPLETED/TIMEOUT)
    bool session_end_flash_queued = false;
    char last_error_message[32];
    char last_error_title[16];

    GrindSessionDescriptor session_descriptor;
    GrindStrategyContext strategy_context;
    IGrindStrategy* active_strategy = nullptr;
    WeightGrindStrategy weight_strategy;
    TimeGrindStrategy time_strategy;

    // Mechanical instability tracking
    int mechanical_anomaly_count_ = 0;
    unsigned long last_mechanical_event_ms_ = 0;
    float last_mechanical_weight_ = 0.0f;
    bool mechanical_monitor_initialized_ = false;

    // Detect actual cup/portafilter removal without reacting to isolated
    // negative load-cell spikes.
    NetWeightRemovalGuard net_weight_removal_guard_;
    // Scale sample that guard last judged: the loop runs several times per sample.
    uint32_t guard_sample_ms_ = 0;
    // What stood on the scale when a grind last zeroed it, and that zero. A
    // later grind on the same zero reads the vessel as about nothing, but its
    // weight is still this plus the reading (see vessel_weight_estimate()).
    float vessel_at_grind_zero_g_ = 0.0f;
    int32_t grind_zero_offset_ = 0;
    bool grind_zero_known_ = false;
    // Stands in when no grind's zero tells, for instance after power-up with
    // the vessel already on the scale.
    VesselWeightTracker vessel_tracker_;
    unsigned long last_vessel_track_ms_ = 0;
    float vessel_track_cal_factor_ = 0.0f;

    // Start of the current wait for a still scale (see settle_wait_policy.h),
    // and whether it has already logged that it is waiting on past its limit.
    unsigned long settle_wait_start_ms_ = 0;
    bool settle_wait_reported_ = false;
    // Shaky-bench handling for this grind (see settle_wait_policy.h), read
    // from the preferences as it starts.
    bool shaky_bench_handling_ = false;
    // Whether this grind found the scale at rest after taring and after the
    // prime; if neither, its bench is shaking (bench_is_shaking()).
    bool quiet_after_tare_ = false;
    bool quiet_after_prime_ = false;
    // Lowest averaged weight of the main run so far, which is its weight
    // before any of its coffee lands.
    float flow_baseline_weight_ = 0.0f;
    bool flow_baseline_known_ = false;

    // The time coffee keeps landing after the motor stops, learnt from past
    // grinds (coast_learning.h), and the main run's flow when it last measured
    // it, which sets the stop and, once the grounds are in, the lesson.
    CoastLearner coast_learner_;
    float main_run_flow_gps_ = 0.0f;
    bool coast_save_pending_ = false;

    DiagnosticsController* diagnostics_controller_ = nullptr;

    // Motor response latency - runtime configurable
    float motor_response_latency_ms;

    // Coast ratio - runtime configurable
    float coast_ratio_;

    // Grind freshness tracking
    bool grinder_purged_since_boot;      // Tracks if grinder has been used since boot (RAM only)
    uint64_t last_purge_runtime_ms;      // Runtime when last grind completed (persisted)

public:
    using GrindSessionResult = ::GrindSessionResult;

private:
    GrindSessionResult last_session_result_ = GrindSessionResult::UNKNOWN;

public:
    // For compound callers and task suspension. Never hold this while waiting
    // for a UI acknowledgement or invoking UI callbacks.
    std::unique_lock<std::recursive_mutex> lock_control() const {
        return std::unique_lock<std::recursive_mutex>(control_mutex_);
    }

    void init(WeightSensor* lc, Grinder* gr, Preferences* prefs);
    bool start_grind(float target_weight, uint32_t target_time_ms, GrindMode grind_mode);
    void user_tare_request();
    void return_to_idle(); // Called by UI to acknowledge completion/timeout
    void stop_grind();
    void continue_from_purge(); // Called by UI to continue from PURGE_CONFIRM to PREDICTIVE
    void update(); // Core 0 main control method - runs at fixed RTOS interval
    
    // Time mode pulse functionality
    void start_additional_pulse(); // Start an additional 100ms pulse in time mode
    bool can_pulse() const; // Check if additional pulses are allowed
    int get_additional_pulse_count() const { const auto control_lock = lock_control(); return additional_pulse_count; }

    // Time mode pause/resume
    void pause_grind();
    void resume_grind();
    bool is_grind_paused() const { const auto control_lock = lock_control(); return grind_paused_; }
    
    // UI event system
    void set_ui_event_callback(void (*callback)(const GrindEventData&));
    GrindSessionResult get_last_session_result() const { const auto control_lock = lock_control(); return last_session_result_; }
    void ui_acknowledge_phase_transition(); // Called by UI to confirm phase transition
    void process_queued_ui_events(); // Core 1: Consume latest display snapshots
    
    // Flash operation system
    void process_queued_flash_operations(); // Core 1: Process flash ops from Core 0 queue
    bool queue_flash_operation(const FlashOpRequest& request); // Core 0: Queue flash operation
    
    // Log message system
    void process_queued_log_messages(); // Core 1: Process log messages from Core 0 queue
    void queue_log_message(const char* format, ...); // Core 0: Queue formatted log message
    
    bool is_active() const;
    bool is_control_loop_paused() const { const auto control_lock = lock_control(); return control_loop_paused_; }
    GrindPhase get_phase() const { const auto control_lock = lock_control(); return phase; }
    const char* get_current_phase_name() const { const auto control_lock = lock_control(); return get_phase_name(phase); }
    // The name of any phase, for a phase recorded earlier.
    const char* phase_name(GrindPhase p) const { return get_phase_name(p); }
    int get_current_progress_percent() const { const auto control_lock = lock_control(); return get_progress_percent(); }
    float get_target_weight() const { const auto control_lock = lock_control(); return target_weight; }
    uint32_t get_target_time_ms() const { const auto control_lock = lock_control(); return target_time_ms; }
    uint32_t get_elapsed_grind_ms() const;
    static constexpr const char* PREF_KEY_PRIME_ENABLED = "prime_enabled";
    static constexpr const char* PREF_KEY_GRINDER_MODE = "grinder_mode";
    // ESP32 NVS keys are limited to 15 characters. The previous
    // "grinder_amount_g" key was 16 characters, so Preferences::putFloat()
    // rejected every purge-amount save.
    static constexpr const char* PREF_KEY_GRINDER_AMOUNT_G = "purge_amount_g";
    static_assert(sizeof("purge_amount_g") - 1 <= 15, "NVS preference key is too long");
    static constexpr const char* PREF_KEY_GRIND_FRESHNESS_HOURS = "freshness_hrs";
    static constexpr const char* PREF_KEY_COAST_RATIO = "coast_ratio";
    static constexpr const char* PREF_KEY_LAST_GRIND_RUNTIME = "last_grind_ms";
    static constexpr const char* PREF_KEY_SHAKY_BENCH = "shaky_bench";
    static constexpr const char* PREF_KEY_COAST_LEARNT_MS = "coast_learn_ms";
    static constexpr const char* PREF_KEY_COAST_LEARNT_GRINDS = "coast_learn_n";
    GrindMode get_mode() const { const auto control_lock = lock_control(); return mode; }
    GrindSessionDescriptor get_session_descriptor() const { const auto control_lock = lock_control(); return session_descriptor; }
    
    // Grind logging functions
    void set_grind_profile_id(uint8_t profile_id) { const auto control_lock = lock_control(); if (phase != GrindPhase::IDLE) return; current_profile_id = profile_id; session_descriptor.profile_id = profile_id; }
    void send_measurements_data();           // Send structured measurement data via serial
    
    // Getter methods for logger (to eliminate calculations in logger)
    float get_current_flow_rate() const;
    float get_motor_stop_target_weight() const { const auto control_lock = lock_control(); return motor_stop_target_weight; }
    float get_grind_latency_ms() const { const auto control_lock = lock_control(); return grind_latency_ms; }
    float get_last_logged_weight() const { const auto control_lock = lock_control(); return last_logged_weight; }
    void set_last_logged_weight(float weight) { const auto control_lock = lock_control(); last_logged_weight = weight; } // Thread-safe setter

    int get_mechanical_anomaly_count() const { const auto control_lock = lock_control(); return mechanical_anomaly_count_; }
    void reset_mechanical_anomaly_count();

    void set_diagnostics_controller(DiagnosticsController* diagnostics) { const auto control_lock = lock_control(); diagnostics_controller_ = diagnostics; }

    // Motor response latency accessors
    float get_motor_response_latency() const { const auto control_lock = lock_control(); return motor_response_latency_ms; }
    void set_motor_response_latency(float value);
    float get_min_pulse_duration() const { const auto control_lock = lock_control(); return motor_response_latency_ms; }
    float get_max_pulse_duration() const { const auto control_lock = lock_control(); return motor_response_latency_ms + GRIND_MOTOR_MAX_PULSE_DURATION_MS; }
    void load_motor_latency();
    bool save_motor_latency(float value);

    // Coast ratio accessors
    float get_coast_ratio() const { const auto control_lock = lock_control(); return coast_ratio_; }
    void set_coast_ratio(float value);
    void load_coast_ratio();
    bool save_coast_ratio(float value);

    // Shaky-bench handling (Menu → Grind Settings → Shaky Bench): the stored
    // choice, which a grind reads as it starts.
    bool shaky_bench_enabled() const;
    bool save_shaky_bench(bool enabled);

    // The after-run time learnt from past grinds; 0 until a grind has taught it.
    float get_learned_coast_ms() const { const auto control_lock = lock_control(); return coast_learner_.known() ? coast_learner_.coast_ms() : 0.0f; }

    // Grind freshness accessors
    bool get_grinder_purged_since_boot() const { const auto control_lock = lock_control(); return grinder_purged_since_boot; }
    uint64_t get_last_purge_runtime_ms() const { const auto control_lock = lock_control(); return last_purge_runtime_ms; }
    
    // Removed - predictive logic now inline in update_realtime()
    
    
private:
    bool queue_terminal_session();
    void switch_phase(GrindPhase new_phase, const GrindLoopData& loop_data = {});
    void final_measurement(const GrindLoopData& loop_data, float measured_weight);
    void monitor_mechanical_instability(const GrindLoopData& loop_data);

    // One step of a wait for a still scale; `past_limit` says what may end
    // it once the limit has passed. With USE_AVERAGE that is an average of
    // readings that look like one weight being shaken and is at least
    // `least_average_g`; `weight_out` then holds it in place of a settled
    // reading.
    SettleWait wait_for_still_scale(uint32_t still_window_ms, uint32_t limit_ms, PastLimit past_limit,
                                    const GrindLoopData& loop_data, float* weight_out = nullptr,
                                    float least_average_g = -1.0e9f);
    // Whether this grind goes by averages: shaky-bench handling is on and
    // the grind has shown its bench to be shaking.
    bool on_shaking_bench() const {
        return shaky_bench_handling_ && bench_is_shaking(quiet_after_tare_, quiet_after_prime_);
    }
    // What this grind's waits may do past their limits.
    PastLimit stillness_wait_past_limit() const { return ::stillness_wait_past_limit(shaky_bench_handling_); }
    PastLimit weight_wait_past_limit() const {
        return ::weight_wait_past_limit(shaky_bench_handling_, mode,
                                        bench_is_shaking(quiet_after_tare_, quiet_after_prime_));
    }
    // Whether the prime is followed by the prompt to empty the portafilter.
    bool purge_prompt_due() const;
    // Keeps the vessel tracker up to date while no grind is running.
    void track_resting_scale(unsigned long now);

    bool check_timeout() const;
    uint8_t get_current_phase_id() const;
    
    // UI event emission - thread-safe for Core 0
    void emit_ui_event(const GrindEventData& data);
    void emit_progress_update(const GrindLoopData& loop_data);
    
    // Core 0 control methods  
    bool should_log_measurements() const;
    
    // Internal state methods (moved from public to prevent polling)
    bool show_taring_text() const { return phase == GrindPhase::INITIALIZING || phase == GrindPhase::SETUP || phase == GrindPhase::TARING || phase == GrindPhase::TARE_CONFIRM; }
    bool is_completed() const { return phase == GrindPhase::COMPLETED; }
    bool is_timeout() const { return phase == GrindPhase::TIMEOUT; }
    int get_progress_percent() const;
    float get_grind_time() const;
    GrindPhase get_timeout_phase() const { return timeout_phase; }
    const char* get_phase_name(GrindPhase p = static_cast<GrindPhase>(-1)) const;

    // `title` replaces the "ERROR" heading on the error screen.
    void set_error_message(const char* message, const char* title = nullptr);
    void queue_manual_runtime(uint32_t now_ms);

    // Learnt after-run time: read at start-up, taught by each main run and
    // stored from the UI task through the flash queue.
    void load_learned_coast();
    void learn_coast_from_main_run(float settled_weight);
    void queue_learned_coast_save();
};
