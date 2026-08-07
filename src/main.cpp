#include <Arduino.h>
#include "driver/adc.h"
#include "esp_adc_cal.h"
#include "nvs_flash.h"
#include "scan_control.h"
#include "wifi_scan.h"
#include "bluetooth_scan.h"
#include "button_task.h"
#include "rgb_indicator.h"
#include "oled_display.h"
#include "dashboard_server.h"

// Arduino-ESP32's startup releases Bluetooth controller memory
// before setup() runs unless this reports true. Without this
// override, esp_bt_controller_init() fails with
// ESP_ERR_INVALID_STATE.
extern "C" bool btInUse() { return true; }

// ---- RF detector (GPIO 34, Core 1). Runs while WiFi or Bluetooth
// is active, paused during STOP, mirroring the rest of the system's
// idle state rather than running independently of it. ----
#define RF_ADC_CHANNEL       ADC1_CHANNEL_6
#define RF_ADC_ATTEN         ADC_ATTEN_DB_2_5
#define RF_ADC_WIDTH         ADC_WIDTH_BIT_12
#define DEFAULT_VREF         1100
#define BURST_WINDOW_MS       50
#define BURST_SAMPLE_INTERVAL_MS 1
#define WINDOW_REPEAT_MS      100
#define DETECTION_THRESHOLD_MV 150

static esp_adc_cal_characteristics_t adc_chars;

static QueueHandle_t rfControlQueue;

// RF tracks Cellular mode specifically now, not "any mode but
// STOP" as it did before this session. Running RF passively during
// WiFi/Bluetooth modes was exactly the behavior that produced the
// interference-contaminated readings this project kept flagging
// (SESSION_3/4_ADDENDUM.md); Cellular mode exists specifically to
// give RF sensing a clean radio environment (softAP torn down, see
// wifi_scan.cpp), so continuing to also sample during WiFi/BT modes
// would both perpetuate that contamination and produce two
// different classes of "cellular event" on the dashboard, one
// trustworthy, one not, with no way for a viewer to tell them apart.
static void rfDetectionTask(void *pvParameters) {
    bool active = false;

    for (;;) {
        ModeActivation act;
        if (xQueueReceive(rfControlQueue, &act, 0) == pdTRUE) {
            active = (act.active_mode == RADIO_MODE_CELLULAR);
            Serial.printf("[RF] %s\n", active ? "RESUMED (Cellular mode)" : "PAUSED (not in Cellular mode)");
        }

        if (!active) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int peak_raw = 0;
        uint32_t window_start = millis();

        while (millis() - window_start < BURST_WINDOW_MS) {
            int raw = adc1_get_raw(RF_ADC_CHANNEL);
            if (raw > peak_raw) peak_raw = raw;
            vTaskDelay(pdMS_TO_TICKS(BURST_SAMPLE_INTERVAL_MS));
        }

        uint32_t peak_mv = esp_adc_cal_raw_to_voltage(peak_raw, &adc_chars);

        if (peak_mv >= DETECTION_THRESHOLD_MV) {
            oled_notify_cellular_event(peak_mv);
            dashboard_notify_cellular_event(millis(), peak_mv);
            Serial.printf("CELLULAR EVENT  t=%ums  peak=%umV\n", (unsigned)millis(), (unsigned)peak_mv);
        }

        vTaskDelay(pdMS_TO_TICKS(WINDOW_REPEAT_MS));
    }
}

// ---- Mode activation fan-out (button -> WiFi, Bluetooth, RGB) ----
static QueueHandle_t wifiControlQueue;
static QueueHandle_t btControlQueue;
static QueueHandle_t rgbControlQueue;

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("Network detector bring-up: all subsystems, mode-switch design");

    // RF detector, control queue created before the task since the
    // task reads from it starting on its very first loop iteration
    adc1_config_width(RF_ADC_WIDTH);
    adc1_config_channel_atten(RF_ADC_CHANNEL, RF_ADC_ATTEN);
    esp_adc_cal_characterize(ADC_UNIT_1, RF_ADC_ATTEN, RF_ADC_WIDTH, DEFAULT_VREF, &adc_chars);
    rfControlQueue = xQueueCreate(5, sizeof(ModeActivation));
    xTaskCreatePinnedToCore(rfDetectionTask, "RFDetectTask", 4096, NULL, 1, NULL, 1);  // Core 1

    // NVS must init before either radio driver.
    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_ret = nvs_flash_init();
    }

    wifiControlQueue = xQueueCreate(5, sizeof(ModeActivation));
    btControlQueue    = xQueueCreate(5, sizeof(ModeActivation));
    rgbControlQueue   = xQueueCreate(5, sizeof(ModeActivation));

    // Bluetooth controller before WiFi driver, both share the radio.
    bluetooth_scan_init(btControlQueue);
    wifi_scan_init(wifiControlQueue);

    // Dashboard server binds to the softAP's network interface,
    // must come after wifi_scan_init() has brought that interface
    // up. Connectivity behavior once WiFi enters continuous
    // channel-hopping mode is still an open item (Session 3 item 2,
    // not yet retested), the reconnect logic in app.js exists
    // specifically to cope with that.
    dashboard_server_init();

    oled_display_init();
    rgb_indicator_init(rgbControlQueue);
    button_task_init(wifiControlQueue, btControlQueue, rgbControlQueue, rfControlQueue);
}

void loop() {
    vTaskDelay(pdMS_TO_TICKS(500));
}