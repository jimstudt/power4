#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

enum class StateReportKind : uint8_t {
    Batteries,
    Banks,
    Relays,
    Inputs,
};

const char *state_report_command_name(StateReportKind kind);
const char *state_report_frame_type(StateReportKind kind);

// Allocates a NUL-terminated JSON report on the heap. The caller owns it and
// must release it with free().
esp_err_t state_report_build(StateReportKind kind, char **json, size_t *length);
