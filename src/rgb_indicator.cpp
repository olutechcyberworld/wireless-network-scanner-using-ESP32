#include "rgb_indicator.h"

#define PIN_R 18
#define PIN_G 19
#define PIN_B 23
#define BLINK_INTERVAL_MS 300

typedef enum { RGB_IDLE, RGB_SCANNING, RGB_CELLULAR, RGB_ERROR } RgbState;

static volatile RgbState currentState = RGB_IDLE;
static QueueHandle_t inTriggerQueue;

static void setColor(bool r, bool g, bool b) {
    digitalWrite(PIN_R, r ? HIGH : LOW);
    digitalWrite(PIN_G, g ? HIGH : LOW);
    digitalWrite(PIN_B, b ? HIGH : LOW);
}

// Directly reflects current mode: WiFi or Bluetooth active -> blue
// blink, Cellular active -> amber blink (red+green together), STOP
// -> idle green. Cellular gets its own distinct color deliberately:
// it's the one mode where the softAP is torn down and the dashboard
// is unreachable, someone glancing at the device from across a room
// should be able to tell that apart from ordinary WiFi/Bluetooth
// scanning without walking over to read the OLED.
static void rgbTask(void *pvParameters) {
    pinMode(PIN_R, OUTPUT);
    pinMode(PIN_G, OUTPUT);
    pinMode(PIN_B, OUTPUT);
    setColor(false, true, false);

    bool blinkOn = false;
    uint32_t lastBlinkToggle = 0;

    for (;;) {
        ModeActivation act;
        if (xQueueReceive(inTriggerQueue, &act, 0) == pdTRUE && currentState != RGB_ERROR) {
            if (act.active_mode == RADIO_MODE_STOP) {
                currentState = RGB_IDLE;
                setColor(false, true, false);
            } else if (act.active_mode == RADIO_MODE_CELLULAR) {
                currentState = RGB_CELLULAR;
            } else {
                currentState = RGB_SCANNING;
            }
        }

        if (currentState == RGB_ERROR) {
            setColor(true, false, false);
        } else if (currentState == RGB_SCANNING) {
            if (millis() - lastBlinkToggle >= BLINK_INTERVAL_MS) {
                blinkOn = !blinkOn;
                lastBlinkToggle = millis();
                setColor(false, false, blinkOn);
            }
        } else if (currentState == RGB_CELLULAR) {
            if (millis() - lastBlinkToggle >= BLINK_INTERVAL_MS) {
                blinkOn = !blinkOn;
                lastBlinkToggle = millis();
                setColor(blinkOn, blinkOn, false);  // amber: red+green together
            }
        }

        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void rgb_indicator_init(QueueHandle_t triggerQueue) {
    inTriggerQueue = triggerQueue;
    xTaskCreatePinnedToCore(rgbTask, "RGBTask", 2048, NULL, 1, NULL, 1);  // Core 1
}

void rgb_report_error() { currentState = RGB_ERROR; }
void rgb_clear_error()  { currentState = RGB_IDLE; }