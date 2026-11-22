#pragma once

enum class DiagnosticCode {
    NONE = 0,
    HX711_NOT_CONNECTED,
    HX711_SAMPLE_RATE_INVALID,
    LOAD_CELL_NOT_CALIBRATED,
    LOAD_CELL_NOISY_SUSTAINED,
    MECHANICAL_INSTABILITY
};

class DiagnosticsController {
public:
    void reset_diagnostic(DiagnosticCode) {}
};
