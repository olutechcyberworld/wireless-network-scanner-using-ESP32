#pragma once
#include <Arduino.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Fans a ModeActivation out to all four consumers on confirm (long
// press). Cycle order on short press: WiFi -> Bluetooth -> Stop ->
// WiFi ...
void button_task_init(QueueHandle_t wifiQueue, QueueHandle_t btQueue, QueueHandle_t rgbQueue, QueueHandle_t rfQueue);