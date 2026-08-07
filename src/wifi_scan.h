#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// controlQueue: scan triggers arrive here from the button task.
void wifi_scan_init(QueueHandle_t controlQueue);
QueueHandle_t wifi_scan_get_event_queue();
