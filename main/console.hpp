#pragma once

#include <stdio.h>

#include "esp_err.h"

enum class Power4CommandSource {
    Serial,
    Tcp,
};

esp_err_t power4_console_start(void);
esp_err_t power4_console_execute(const char *command,
                                 Power4CommandSource source,
                                 FILE *input,
                                 FILE *output,
                                 int *command_result);
Power4CommandSource power4_console_command_source(void);
