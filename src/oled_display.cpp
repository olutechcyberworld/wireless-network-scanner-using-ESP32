#include "oled_display.h"
#include "rf_calibration.h"
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>

#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
#define OLED_RESET -1

// Confirm this against an I2C bus scan at physical bring-up, per
// the standard practice already agreed for this project, 0x3D is
// the common alternative on some SSD1306 modules.
#define OLED_I2C_ADDRESS 0x3C

#define I2C_SDA 21
#define I2C_SCL 22

typedef struct {
    RadioMode highlighted_mode;
    RadioMode active_mode;
    uint32_t wifi_count;
    uint32_t wifi_hotspot_count;
    bool     wifi_sweep_active;
    uint32_t bt_count;
    // Cellular-specific: RF only samples during Cellular mode now
    // (see main.cpp), so these are only ever populated/relevant
    // while active_mode == RADIO_MODE_CELLULAR, unlike
    // last_cellular_event_ms below which used to be shown on every
    // screen back when RF ran in the background everywhere.
    uint32_t cellular_event_count;
    uint32_t last_cellular_peak_mv;
    uint8_t  last_cellular_strength_pct;
    uint32_t last_cellular_event_ms;
} SystemStatus;

static Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, OLED_RESET);
// active_mode defaults to STOP: nothing scans until the user
// confirms a selection via long press. WiFi and Bluetooth tasks
// stay idle at boot, the display must not claim otherwise.
// highlighted_mode defaults to WIFI as the initial cursor position
// only, this has no effect on what's actually running.
static SystemStatus status = { RADIO_MODE_WIFI, RADIO_MODE_STOP, 0, 0, false, 0, 0, 0, 0, 0 };
static SemaphoreHandle_t statusMutex;

static const char *modeLabel(RadioMode m) {
    switch (m) {
        case RADIO_MODE_WIFI:      return "WiFi";
        case RADIO_MODE_BLUETOOTH: return "Bluetooth";
        case RADIO_MODE_CELLULAR:  return "Cellular";
        default:                   return "Stopped";
    }
}

static void oledTask(void *pvParameters) {
    for (;;) {
        SystemStatus local;
        xSemaphoreTake(statusMutex, portMAX_DELAY);
        local = status;
        xSemaphoreGive(statusMutex);

        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);

        if (local.active_mode == RADIO_MODE_STOP) {
            // Idle / selection screen: unchanged from prior design,
            // minus the Cell: line, RF only samples during Cellular
            // mode now, showing it here would just always read
            // "idle" and mean nothing.
            display.setCursor(0, 0);
            display.println("NETWORK DETECTOR");

            display.setCursor(0, 14);
            display.print("Select: ");
            display.println(modeLabel(local.highlighted_mode));

            display.setCursor(0, 26);
            display.print("Active: ");
            display.println(modeLabel(local.active_mode));

            display.setCursor(0, 38);
            display.println("System idle");

        } else if (local.active_mode == RADIO_MODE_WIFI) {
            // WiFi active screen: signature count and hotspot count
            // are independent figures, not a corrected single count.
            // Select: is included here so short-press cycling remains
            // visible while a scan is running, not just at idle.
            display.setCursor(0, 0);
            display.println(local.wifi_sweep_active ? "WIFI SWEEP..." : "WIFI SCANNING...");

            display.setCursor(0, 14);
            display.print("Select: ");
            display.println(modeLabel(local.highlighted_mode));

            display.setCursor(0, 26);
            display.print("Sigs: ");
            display.println(local.wifi_count);

            display.setCursor(0, 38);
            display.print("Hotspots: ");
            display.println(local.wifi_hotspot_count);

        } else if (local.active_mode == RADIO_MODE_BLUETOOTH) {
            // Bluetooth active screen: no AP-role equivalent exists
            // for Bluetooth advertisements, so no second count line.
            display.setCursor(0, 0);
            display.println("BLUETOOTH SCANNING...");

            display.setCursor(0, 14);
            display.print("Select: ");
            display.println(modeLabel(local.highlighted_mode));

            display.setCursor(0, 26);
            display.print("Devices: ");
            display.println(local.bt_count);

        } else if (local.active_mode == RADIO_MODE_CELLULAR) {
            // Cellular active screen: the one mode where the WiFi
            // radio is fully torn down (wifi_scan.cpp), so the
            // dashboard is genuinely unreachable, not just paused.
            // That has to be visible here, an invigilator standing
            // at the device with no dashboard access needs to know
            // that's expected, not a fault.
            bool recentEvent = local.last_cellular_event_ms != 0 &&
                (millis() - local.last_cellular_event_ms) < 3000;

            display.setCursor(0, 0);
            display.println("CELLULAR SENSING...");

            display.setCursor(0, 14);
            display.print("Select: ");
            display.println(modeLabel(local.highlighted_mode));

            display.setCursor(0, 26);
            display.print("Events: ");
            display.println(local.cellular_event_count);

            display.setCursor(0, 38);
            display.print("Signal: ");
            display.print(local.last_cellular_strength_pct);
            display.println(recentEvent ? "% *" : "%");

            display.setCursor(0, 52);
            display.println("Dashboard offline");
        }

        display.display();
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

void oled_display_init() {
    statusMutex = xSemaphoreCreateMutex();

    Wire.begin(I2C_SDA, I2C_SCL);
    if (!display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C_ADDRESS)) {
        Serial.println("[OLED] init failed, check I2C address and wiring");
        return;
    }

    // Boot splash. Intentionally blocking, this runs once during
    // setup() before any other subsystem needs the display, and
    // keeps this screen visible for a fixed, predictable duration
    // rather than racing the first real status render.
    //
    // The loading bar is cosmetic, its fill rate is just this loop's
    // own iteration count against BOOT_STEPS, it is not wired to any
    // real subsystem's initialization progress (WiFi/Bluetooth/
    // dashboard bring-up all happen later in setup(), after this
    // function returns). Making it track real progress would need
    // hooks into each subsystem's own init call; that's a legitimate
    // future improvement, not something to fake with a progress bar
    // that only looks like it means something.
    display.clearDisplay();
    display.setTextSize(1);
    display.setTextColor(SSD1306_WHITE);
    display.setCursor(0, 0);
    display.println("WIRELESS NETWORK");
    display.setCursor(0, 10);
    display.println("DETECTOR");
    display.setCursor(0, 24);
    display.println("Project by:");
    display.setCursor(0, 34);
    display.println("Fabiku Tolulope");
    display.setCursor(0, 44);
    // 21 characters at the default 6px-per-character font is 126px
    // against a 128px-wide display, this fits but with almost no
    // margin, confirm it isn't clipped on the actual panel at bring
    // -up, screen geometry can vary slightly between SSD1306 units.
    display.println("Aladetoyinbo Increase");

    const int barX = 4, barY = 56, barW = 120, barH = 6;
    const int BOOT_STEPS = 24;
    const int STEP_DELAY_MS = 60;  // 24 * 60 = 1440ms, close to the previous fixed 1500ms hold

    for (int step = 0; step <= BOOT_STEPS; step++) {
        // Fill only grows each iteration, safe to redraw without
        // clearing the bar first, the new fill fully covers the old.
        int fillW = (barW - 2) * step / BOOT_STEPS;
        display.drawRect(barX, barY, barW, barH, SSD1306_WHITE);
        display.fillRect(barX + 1, barY + 1, fillW, barH - 2, SSD1306_WHITE);
        display.display();
        delay(STEP_DELAY_MS);
    }

    display.clearDisplay();
    display.display();

    xTaskCreatePinnedToCore(oledTask, "OLEDTask", 4096, NULL, 1, NULL, 1);  // Core 1
}

void oled_set_highlighted_mode(RadioMode mode) {
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    status.highlighted_mode = mode;
    xSemaphoreGive(statusMutex);
}

void oled_set_active_mode(RadioMode mode) {
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    status.active_mode = mode;
    if (mode == RADIO_MODE_CELLULAR) {
        // Fresh counters on entering Cellular mode, matching the
        // existing pattern for WiFi/Bluetooth counts resetting on
        // (re)activation, so a stale count from a previous Cellular
        // session doesn't linger on screen.
        status.cellular_event_count = 0;
        status.last_cellular_peak_mv = 0;
        status.last_cellular_strength_pct = 0;
        status.last_cellular_event_ms = 0;
    }
    xSemaphoreGive(statusMutex);
}

void oled_set_wifi_count(uint32_t count) {
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    status.wifi_count = count;
    xSemaphoreGive(statusMutex);
}

void oled_set_wifi_hotspot_count(uint32_t count) {
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    status.wifi_hotspot_count = count;
    xSemaphoreGive(statusMutex);
}

void oled_set_wifi_sweep_active(bool active) {
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    status.wifi_sweep_active = active;
    xSemaphoreGive(statusMutex);
}

void oled_set_bt_count(uint32_t count) {
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    status.bt_count = count;
    xSemaphoreGive(statusMutex);
}

void oled_notify_cellular_event(uint32_t peak_mv) {
    xSemaphoreTake(statusMutex, portMAX_DELAY);
    status.last_cellular_event_ms = millis();
    status.last_cellular_peak_mv = peak_mv;
    status.last_cellular_strength_pct = rf_strength_percent(peak_mv);
    status.cellular_event_count++;
    xSemaphoreGive(statusMutex);
}