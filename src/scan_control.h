#pragma once
#include <cstdint>

typedef enum {
    RADIO_MODE_WIFI,
    RADIO_MODE_BLUETOOTH,
    RADIO_MODE_CELLULAR,
    RADIO_MODE_STOP
} RadioMode;

// Sent to WiFi, Bluetooth, and RGB whenever a long press confirms a
// mode switch. Each radio task checks whether it is now the active
// one and starts or stops accordingly, enforcing mutual exclusivity
// between WiFi and Bluetooth scanning, confirmed necessary by the
// coexistence contention test.
typedef struct {
    RadioMode active_mode;
    uint32_t timestamp_ms;
} ModeActivation;