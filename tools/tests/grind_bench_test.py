"""Run the firmware's grind controller against a simulated grinder and scale.

The controller, both grind strategies and the scale filter are compiled
unmodified. Around them stand a bench motor, a simulated load-cell ADC whose
samples go through the firmware's own WeightSensor code, and an in-memory
settings store (tools/tests/grind_bench). Scale scatter for the vibration
scenarios comes from readings recorded on a real grinder
(tools/tests/fixtures).
"""
from pathlib import Path
import os
import shutil
import subprocess
import tempfile
import unittest

from controller_serialization_test import function

ROOT = Path(__file__).resolve().parents[2]
TESTS = Path(__file__).resolve().parent
BENCH = TESTS / "grind_bench"
PRINTER_TRACE = TESTS / "fixtures" / "scale_trace_printer_vibration.csv"
NORMAL_TRACE = TESTS / "fixtures" / "scale_trace_normal_bench.csv"

# Firmware code the bench scale runs as it is written in src/hardware.
SENSOR_HEADER_CODE = (
    "float raw_to_weight(int32_t raw_adc_value) const",
    "int32_t weight_to_raw_threshold(float weight_threshold) const",
    "bool start_nonblocking_tare()",
    "bool is_tare_in_progress() const",
    "bool has_recent_sample() const",
    "uint32_t get_last_sample_ms() const",
    "int32_t get_zero_offset() const",
)
SENSOR_SOURCE_CODE = (
    "void WeightSensor::tareNoDelay()",
    "bool WeightSensor::is_settled(uint32_t window_ms)",
    "bool WeightSensor::check_settling_complete(uint32_t window_ms, float* settled_weight_out)",
    "float WeightSensor::get_weight_low_latency() const",
    "float WeightSensor::get_display_weight()",
    "float WeightSensor::get_weight_high_latency() const",
    "float WeightSensor::get_weight_smoothed(uint32_t window_ms) const",
    "float WeightSensor::get_untared_level(uint32_t window_ms) const",
    "bool WeightSensor::get_shaken_weight(uint32_t window_ms, float* weight_out) const",
    "float WeightSensor::get_standard_deviation_g(uint32_t window_ms) const",
    "float WeightSensor::get_instant_weight() const",
    "int32_t WeightSensor::get_raw_adc_smoothed(uint32_t window_ms) const",
    "void WeightSensor::set_zero_offset(int32_t offset)",
    "float WeightSensor::get_flow_rate(uint32_t window_ms) const",
    "float WeightSensor::get_flow_rate_95th_percentile(uint32_t window_ms) const",
    "float WeightSensor::get_calibration_factor()",
    "bool WeightSensor::sample_and_feed_filter()",
)

FIRMWARE_SOURCES = (
    "controllers/grind_controller.cpp",
    "controllers/weight_grind_strategy.cpp",
    "controllers/time_grind_strategy.cpp",
    "hardware/circular_buffer_math/circular_buffer_math.cpp",
    "hardware/WeightSensor.cpp",
)

SCENARIOS = (
    "quiet_bench_grind",
    "normal_bench_grind",
    "normal_bench_with_the_handling_on",
    "printer_vibration_grind",
    "printer_vibration_is_recognised_from_any_moment",
    "a_later_grind_measures_its_flow_start_afresh",
    "printer_vibration_without_the_handling_waits_as_before",
    "hand_after_the_prime_is_waited_out_as_before",
    "hand_resting_as_the_main_run_starts",
    "a_chance_still_moment_does_not_make_the_bench_quiet",
    "a_touch_while_taring_is_not_zeroed_in",
    "over_target_on_a_quiet_bench",
    "over_target_with_printer_vibration",
    "removal_stops_the_second_grind_while_grinding",
    "removal_stops_the_second_grind_while_settling",
    "pushed_up_portafilter_is_not_a_removal",
    "removal_stops_a_time_grind_on_a_scale_zeroed_with_the_portafilter",
    "purge_mode_lets_the_portafilter_be_emptied",
    "manual_grind_keeps_its_safety_limit_and_removal_stop",
    "nudge_while_taring_does_not_shift_the_zero",
    "hand_on_a_quiet_bench_decides_nothing",
    "hand_on_a_shaking_bench_decides_nothing",
    "brief_touch_does_not_end_the_grind_short",
    "trickling_grounds_are_weighed_to_the_end",
    "time_grind_with_a_long_chute",
    "tray_taken_off_and_put_back",
    "a_tare_by_hand_starts_the_weight_afresh",
    "a_tare_by_hand_drops_the_carried_weight",
    "portafilter_reseated_while_taring_is_guarded_next_time",
    "second_dose_without_lifting_is_guarded_as_before",
    "bad_scale_readings_are_not_a_removal",
    "lift_after_the_prime_does_not_restart_the_motor",
    "a_tare_in_progress_carries_no_weight",
    "early_lift_before_the_purge_prompt",
    "power_up_with_the_portafilter_on",
    "empty_hopper_times_out_in_plain_words",
    "time_grind_on_a_quiet_bench",
    "time_grind_with_printer_vibration",
    "grinds_across_grinders_doses_and_benches",
)


def assemble_sources(sandbox):
    """Lay out the firmware sources with the bench stand-ins beside them."""
    src = sandbox / "src"
    shutil.copytree(ROOT / "src/config", src / "config")
    shutil.copytree(ROOT / "src/controllers", src / "controllers")
    shutil.copytree(ROOT / "src/hardware/circular_buffer_math", src / "hardware/circular_buffer_math")
    (src / "logging").mkdir()
    shutil.copy(ROOT / "src/logging/grind_logging.h", src / "logging")
    (src / "system").mkdir()
    shutil.copy(ROOT / "src/system/operation_interlock.h", src / "system")
    shutil.copy(BENCH / "fakes/hardware/grinder.h", src / "hardware")
    for header in (BENCH / "fakes/system").glob("*.h"):
        shutil.copy(header, src / "system")

    sensor_header = (ROOT / "src/hardware/WeightSensor.h").read_text()
    sensor_source = (ROOT / "src/hardware/WeightSensor.cpp").read_text()
    inline_code = "\n".join("    " + function(sensor_header, signature) for signature in SENSOR_HEADER_CODE)
    template = (BENCH / "fakes/hardware/WeightSensor.h.in").read_text()
    (src / "hardware/WeightSensor.h").write_text(template.replace("@PRODUCTION_INLINE@", inline_code))
    methods = "\n\n".join(function(sensor_source, signature) for signature in SENSOR_SOURCE_CODE)
    (src / "hardware/WeightSensor.cpp").write_text(
        '#include "WeightSensor.h"\n#include <math.h>\n\n'
        + function(sensor_source, "namespace {") + "\n\n" + methods + "\n")


class GrindBenchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.folder = Path(tempfile.mkdtemp(prefix="smart-grind-bench-"))
        sandbox = cls.folder / "sandbox"
        assemble_sources(sandbox)
        compile_command = ["g++", "-std=c++17", "-O1", "-g", "-DSMART_GRIND_SIM",
                           "-fsanitize=address,undefined", "-I", str(BENCH / "shim"), "-I", str(sandbox)]
        objects = []
        for index, source in enumerate(FIRMWARE_SOURCES):
            # Warnings in the firmware sources belong to the firmware build.
            objects.append(cls.folder / f"firmware_{index}.o")
            subprocess.run(compile_command + ["-w", "-c", str(sandbox / "src" / source),
                                              "-o", str(objects[-1])], check=True)
        objects.append(cls.folder / "bench.o")
        subprocess.run(compile_command + ["-Wall", "-Wextra", "-c", str(BENCH / "bench.cpp"),
                                          "-o", str(objects[-1])], check=True)
        cls.binary = cls.folder / "bench"
        subprocess.run(compile_command + [str(path) for path in objects] + ["-o", str(cls.binary)], check=True)

    @classmethod
    def tearDownClass(cls):
        shutil.rmtree(cls.folder, ignore_errors=True)

    def run_scenario(self, name):
        result = subprocess.run([str(self.binary), name, str(PRINTER_TRACE), str(NORMAL_TRACE)],
                                capture_output=True, text=True, timeout=120)
        if os.environ.get("GRIND_BENCH_VERBOSE"):
            print(result.stdout)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_every_bench_scenario_is_run(self):
        bench = (BENCH / "bench.cpp").read_text()
        table = bench.split("scenarios = {", 1)[1].split("};", 1)[0]
        self.assertEqual(sorted(SCENARIOS), sorted(line.split('"')[1] for line in table.splitlines() if '"' in line))


def scenario_test(name):
    def test(self):
        self.run_scenario(name)
    return test


for scenario in SCENARIOS:
    setattr(GrindBenchTest, f"test_{scenario}", scenario_test(scenario))


if __name__ == "__main__":
    unittest.main()
