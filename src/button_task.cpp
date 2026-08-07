#include "button_task.h"
#include "scan_control.h"
#include "oled_display.h"

#define BUTTON_PIN 4
#define DEBOUNCE_MS 50
#define LONG_PRESS_THRESHOLD_MS 700

static QueueHandle_t wifiOut;
static QueueHandle_t btOut;
static QueueHandle_t rgbOut;
static QueueHandle_t rfOut;
static RadioMode highlighted = RADIO_MODE_WIFI;

static RadioMode nextMode(RadioMode m) {
    switch (m) {
        case RADIO_MODE_WIFI:      return RADIO_MODE_BLUETOOTH;
        case RADIO_MODE_BLUETOOTH: return RADIO_MODE_CELLULAR;
        case RADIO_MODE_CELLULAR:  return RADIO_MODE_STOP;
        default:                   return RADIO_MODE_WIFI;
    }
}

static const char *modeName(RadioMode m) {
    switch (m) {
        case RADIO_MODE_WIFI:      return "WIFI";
        case RADIO_MODE_BLUETOOTH: return "BLUETOOTH";
        case RADIO_MODE_CELLULAR:  return "CELLULAR";
        default:                   return "STOP";
    }
}

static void buttonTask(void *pvParameters) {
    pinMode(BUTTON_PIN, INPUT_PULLUP);
    int lastState = HIGH;
    uint32_t pressStart = 0;

    oled_set_highlighted_mode(highlighted);

    for (;;) {
        int reading = digitalRead(BUTTON_PIN);

        if (lastState == HIGH && reading == LOW) {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            if (digitalRead(BUTTON_PIN) == LOW) {
                pressStart = millis();
                Serial.println("[Button] press detected");
            }
        } else if (lastState == LOW && reading == HIGH) {
            vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
            if (digitalRead(BUTTON_PIN) == HIGH && pressStart != 0) {
                uint32_t heldMs = millis() - pressStart;

                if (heldMs >= LONG_PRESS_THRESHOLD_MS) {
                    ModeActivation act;
                    act.active_mode = highlighted;
                    act.timestamp_ms = millis();

                    Serial.printf("[Button] released, held %ums -> CONFIRM %s\n",
                        heldMs, modeName(highlighted));

                    xQueueSend(wifiOut, &act, 0);
                    xQueueSend(btOut, &act, 0);
                    xQueueSend(rgbOut, &act, 0);
                    xQueueSend(rfOut, &act, 0);
                    oled_set_active_mode(highlighted);
                } else {
                    highlighted = nextMode(highlighted);
                    Serial.printf("[Button] released, held %ums -> CYCLE to %s\n",
                        heldMs, modeName(highlighted));
                    oled_set_highlighted_mode(highlighted);
                }

                pressStart = 0;
            }
        }

        lastState = reading;
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void button_task_init(QueueHandle_t wifiQueue, QueueHandle_t btQueue, QueueHandle_t rgbQueue, QueueHandle_t rfQueue) {
    wifiOut = wifiQueue;
    btOut = btQueue;
    rgbOut = rgbQueue;
    rfOut = rfQueue;
    xTaskCreatePinnedToCore(buttonTask, "ButtonTask", 2048, NULL, 1, NULL, 1);  // Core 1
}