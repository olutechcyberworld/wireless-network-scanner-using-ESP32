#pragma once
#include <Arduino.h>
#include "scan_control.h"

void oled_display_init();

// Each setter is internally mutex-protected, safe to call from any
// task on either core. The OLED task reads a consistent snapshot
// periodically rather than being pushed individual events, since
// this is a "live gauge" display, not an event log.
void oled_set_highlighted_mode(RadioMode mode);
void oled_set_active_mode(RadioMode mode);
void oled_set_wifi_count(uint32_t count);
void oled_set_wifi_hotspot_count(uint32_t count);
void oled_set_wifi_sweep_active(bool active);
void oled_set_bt_count(uint32_t count);
void oled_notify_cellular_event(uint32_t peak_mv);