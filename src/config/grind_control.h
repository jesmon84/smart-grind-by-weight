#pragma once

//==============================================================================
// GRIND CONTROL CONFIGURATION
//==============================================================================
// This file contains all grind control related configuration constants
// including timing parameters, accuracy settings, flow detection, and
// pulse control algorithms.

//------------------------------------------------------------------------------
// GRINDER PURGE/PRIME
//------------------------------------------------------------------------------
// Grinder saturation modes
enum class GrinderPurgeMode {
    PRIME = 0,  // Saturate grinder, keep coffee, continue immediately
    PURGE = 1   // Saturate grinder, prompt user to discard stale grinds
};

// Grinder saturation defaults and ranges
#define GRIND_PURGE_MODE_DEFAULT static_cast<int>(GrinderPurgeMode::PURGE)
#define GRIND_PURGE_AMOUNT_DEFAULT_G 1.0f
#define GRIND_PURGE_AMOUNT_MIN_G 0.1f
#define GRIND_PURGE_AMOUNT_MAX_G 2.5f

// Grind freshness tracking
#define GRIND_FRESHNESS_DEFAULT_HOURS 8.0f

//------------------------------------------------------------------------------
// GRIND CONTROL TUNING
//------------------------------------------------------------------------------
// Main accuracy and timeout settings
#define GRIND_ACCURACY_TOLERANCE_G 0.03f                                  // Final target accuracy tolerance
#define GRIND_TIMEOUT_SEC 60                                              // Maximum time for grind operation
#define MANUAL_GRIND_TIMEOUT_SEC 30                                       // Safety cutoff for target-free manual grinding
#define GRIND_MAX_PULSE_ATTEMPTS 10                                       // Maximum pulse corrections before stopping

// Flow rate detection
#define GRIND_FLOW_DETECTION_THRESHOLD_GPS 0.5f                           // Minimum coffee flow rate to establish first grinds reachinig the cup = latency
#define GRIND_FLOW_START_RISE_G 0.25f                                     // On a shaking bench the averaged weight must also have risen this much

// Undershoot strategy - determine when to stop grinding during the predictive phase
#define GRIND_UNDERSHOOT_TARGET_G 1.0f                                    // Default conservative undershoot target
#define GRIND_LATENCY_TO_COAST_RATIO_DEFAULT 1.0f                          // Preserve the existing coast calculation unless the user changes it
#define GRIND_LATENCY_TO_COAST_RATIO_MIN 0.7f                              // Minimum coast ratio (most conservative, stops earliest)
#define GRIND_LATENCY_TO_COAST_RATIO_MAX 1.5f                              // Maximum coast ratio (stops earliest, most undershoot margin)
#define GRIND_LATENCY_TO_COAST_RATIO GRIND_LATENCY_TO_COAST_RATIO_DEFAULT  // Backward-compatible alias

// Prime phase behavior
#define GRIND_PRIME_TARGET_WEIGHT_G 1.0f                                   // Amount of coffee delivered during chute priming
#define GRIND_PRIME_MAX_DURATION_MS 5000                                   // Safety timeout for chute priming run

//------------------------------------------------------------------------------
// SCALE CALIBRATION AND SETTLING
//------------------------------------------------------------------------------
// Tare and settling behavior  
#define GRIND_SCALE_SETTLING_TOLERANCE_G 0.010f                           // Maximum standard deviation for settled reading. Used to determine if scale is settled. Increase value if you have a noisy load cell.

//------------------------------------------------------------------------------
// TIME MODE PULSE SETTINGS
//------------------------------------------------------------------------------
#define GRIND_TIME_PULSE_DURATION_MS 100                                        // Duration of additional pulses in time mode (milliseconds)



//------------------------------------------------------------------------------
// FLOW RATE PARAMETERS
//------------------------------------------------------------------------------
#define GRIND_FLOW_RATE_MIN_SANE_GPS 1.0f                                         // Minimum reasonable flow rate
#define GRIND_FLOW_RATE_MAX_SANE_GPS 3.0f                                         // Maximum reasonable flow rate
#define GRIND_PULSE_FLOW_RATE_FALLBACK_GPS 1.5f                                   // Fallback pulse flow rate when measured rate is invalid or too low

//------------------------------------------------------------------------------
// TIMING CONSTRAINTS (Hardware-dependent)
//------------------------------------------------------------------------------
// Motor response latency - runtime configurable via auto-tune
#define GRIND_MOTOR_RESPONSE_LATENCY_DEFAULT_MS 50.0f                             // Safe default motor response latency
#define GRIND_MOTOR_MAX_PULSE_DURATION_MS 250.0f                                  // Maximum pulse duration above latency (latency + GRIND_MOTOR_MAX_PULSE_DURATION_MS)

// Motor timing
#define GRIND_MOTOR_SETTLING_TIME_MS 200                                          // Motor vibration settling time

// Mechanical instability detection
#define GRIND_MECHANICAL_DROP_THRESHOLD_G 0.4f                                    // Weight drop considered mechanical instability
#define GRIND_MECHANICAL_EVENT_COOLDOWN_MS 200                                    // Minimum time between detecting drops
#define GRIND_MECHANICAL_EVENT_REQUIRED_COUNT 3                                   // Events required to flag diagnostic

// Scale settling timing
#define GRIND_SCALE_PRECISION_SETTLING_TIME_MS 500                                // High-precision settling time
#define GRIND_SCALE_SETTLING_TIMEOUT_MS 10000                                     // Maximum time to wait for settling

// Shaky-bench handling, for a grinder on a bench that vibrates (a 3D printer
// next to it). The scale never reads still there, and every wait for a still
// reading holds the grind until its timeout. With the handling on, the waits
// below have limits and averages may stand in for still readings (see
// src/controllers/settle_wait_policy.h). It is off until switched on under
// Menu → Grind Settings → Shaky Bench (preference "shaky_bench"), because with
// it on a hand kept on the portafilter can be weighed as coffee. A build made
// with -DGRIND_SHAKY_BENCH_DEFAULT=1 switches it on for a grinder that has no
// stored choice yet, and stores that.
#ifndef GRIND_SHAKY_BENCH_DEFAULT
#define GRIND_SHAKY_BENCH_DEFAULT 0
#endif

// Limits on waiting for a still scale with shaky-bench handling on.
#define GRIND_TARE_SETTLE_LIMIT_MS 4000                                           // From the start of taring until the motor may start
#define GRIND_PRIME_SETTLE_LIMIT_MS 4000                                          // After the prime (GRIND_SCALE_SETTLING_TIMEOUT_MS without the handling)
#define GRIND_MOTOR_SETTLE_LIMIT_MS 500                                           // After the motor stops, on top of the minimum wait
#define GRIND_WEIGHT_SETTLE_LIMIT_MS 2500                                         // Before a pulse is decided or the final weight is taken

// Past its limit, a wait that needs a weight may then take the average of the
// readings instead of a still one: bench vibration averages out. A hand on
// the portafilter does not, so a grind only does this once it has shown its
// bench to be shaking (the scale came to rest neither after taring nor after
// the prime), and only while the readings look like one weight being shaken.
// Otherwise the wait goes on.
#define GRIND_QUIET_BENCH_WINDOW_MS 1000                                          // A scale still for this long after taring stands on a quiet bench
#define GRIND_QUIET_BENCH_SETTLE_MS 1500                                          // So does one that reads still this soon after the prime
#define GRIND_UNSTEADY_AVERAGE_WINDOW_MS 2000                                     // Readings averaged; no longer than the weight limit above
#define GRIND_UNSTEADY_SCATTER_LIMIT_G 0.20f                                      // Largest standard deviation of those readings (a printer measured 0.07-0.12)
#define GRIND_UNSTEADY_DRIFT_LIMIT_G 0.06f                                        // Largest difference between the averages of their two halves (measured up to 0.04)
#define GRIND_UNSTEADY_DROP_LIMIT_G 0.30f                                         // A pulse needs an average no further than this below the last known weight
#define GRIND_UNSTEADY_ZERO_SHIFT_LIMIT_G 0.15f                                   // Furthest a shaken zero is moved to the average; more than that is not vibration

// Tare and calibration timing (hardware sample rate dependent)
#define GRIND_TARE_SAMPLE_WINDOW_MS 500                                           // Time window for tare sampling
#define GRIND_TARE_TIMEOUT_MS 3000                                                // Maximum tare completion time
#define GRIND_CALIBRATION_SAMPLE_WINDOW_MS 800                                    // Time window for calibration sampling  
#define GRIND_CALIBRATION_TIMEOUT_MS 2000                                         // Maximum calibration completion time

// Calculated sample counts based on hardware rate
#define GRIND_TARE_SAMPLE_COUNT (GRIND_TARE_SAMPLE_WINDOW_MS / HW_LOADCELL_SAMPLE_INTERVAL_MS)
#define GRIND_CALIBRATION_SAMPLE_COUNT (GRIND_CALIBRATION_SAMPLE_WINDOW_MS / HW_LOADCELL_SAMPLE_INTERVAL_MS)

//------------------------------------------------------------------------------
// MOTOR RESPONSE AUTO-TUNE ALGORITHM
//------------------------------------------------------------------------------
#define GRIND_AUTOTUNE_LATENCY_MIN_MS 30.0f                                       // Lower search bound for latency
#define GRIND_AUTOTUNE_LATENCY_MAX_MS 300.0f                                      // Upper search bound for latency
#define GRIND_MOTOR_LATENCY_MANUAL_STEP_MS 5.0f                                   // Manual UI/API adjustment increment
#define GRIND_AUTOTUNE_PRIMING_PULSE_MS 1000                                      // Initial chute priming pulse
#define GRIND_AUTOTUNE_TARGET_ACCURACY_MS 5.0f                                    // Target resolution
#define GRIND_AUTOTUNE_SUCCESS_RATE 0.80f                                         // 80% success threshold (4/5 pulses)
#define GRIND_AUTOTUNE_VERIFICATION_PULSES 5                                      // Verification attempts per candidate
#define GRIND_AUTOTUNE_MAX_ITERATIONS 50                                          // Hard stop safety limit
#define GRIND_AUTOTUNE_COLLECTION_DELAY_MS 1500                                   // Minimum wait after pulse for grounds to drop
#define GRIND_AUTOTUNE_SETTLING_TIMEOUT_MS 5000                                   // Max wait per pulse for scale settling
#define GRIND_AUTOTUNE_WEIGHT_THRESHOLD_G GRIND_SCALE_SETTLING_TOLERANCE_G        // 0.010g detection threshold
