#pragma once
#include <cstdint>

// Shared between dashboard_server.cpp and oled_display.cpp so the
// two displays of the same underlying reading can never drift out
// of sync with each other. Bounds locked in SESSION_2_ADDENDUM.md
// Section 2.2: the weakest signal this circuit is intended to
// detect targets roughly 200-300mV at the LM358 output, the
// strongest realistic signal roughly 1200-1300mV. This is not a
// calibrated RF power measurement, a discrete diode envelope
// detector has no claim to that, it is this project's own
// bench-calibrated range expressed as a bounded, readable
// percentage instead of a raw millivolt figure that means nothing
// to a non-technical viewer.
#define RF_STRENGTH_FLOOR_MV   250
#define RF_STRENGTH_CEILING_MV 1250

static inline uint8_t rf_strength_percent(uint32_t peak_mv) {
    if (peak_mv <= RF_STRENGTH_FLOOR_MV) return 0;
    if (peak_mv >= RF_STRENGTH_CEILING_MV) return 100;
    return (uint8_t)(((peak_mv - RF_STRENGTH_FLOOR_MV) * 100) /
                      (RF_STRENGTH_CEILING_MV - RF_STRENGTH_FLOOR_MV));
}
