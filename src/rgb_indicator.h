#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "scan_control.h"

void rgb_indicator_init(QueueHandle_t triggerQueue);

// Not yet wired to any actual fault condition, no system-error
// source has been defined in either project document. Exposed so a
// future fault check has somewhere to report to.
void rgb_report_error();
void rgb_clear_error();
