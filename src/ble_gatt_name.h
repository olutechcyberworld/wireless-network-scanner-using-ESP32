#pragma once
#include <Arduino.h>
#include "esp_gap_ble_api.h"

// Background GATT-based name resolution for BLE devices whose
// advertisement and scan response never carried a name. Connects on
// demand, reads the standard Generic Access service's Device Name
// characteristic (service UUID 0x1800, characteristic UUID 0x2A00),
// and patches the existing dashboard entry via
// dashboard_notify_bt_device_name_update() on success, the same
// mechanism Classic BT's asynchronous name resolution already uses
// (bluetooth_scan.cpp).
//
// Deliberately one attempt at a time, not a connection pool: this
// build's IRAM/heap budget is already tight (this project hit a
// 12KB IRAM overflow this session from far smaller additions than a
// concurrent GATT connection manager would need), and a serial
// queue with a bounded per-attempt timeout is simpler to reason
// about and defend at viva than a concurrent one, at the cost of
// resolving names more slowly when several nameless devices queue
// up at once. Each address is attempted at most once per BLE-mode
// activation, a failure or timeout is not retried, matching this
// project's existing Classic BT name-resolution philosophy
// (SESSION_4_ADDENDUM.md Section 1.6: "a single failure isn't worth
// spamming repeat requests over").
//
// IMPORTANT, unverified assumption, flag this in Chapter 3 as
// unconfirmed until a real log says otherwise: this assumes the
// ESP32 BT controller can hold an active GATT connection attempt
// while BLE scanning continues in the background, without the scan
// silently stalling or the connect attempt starving. This project
// has already been caught once by an unverified concurrency
// assumption between two radio roles (WiFi channel-hopping vs. a
// connected softAP client, SESSION_4_ADDENDUM.md Section 1.4).
// Watch the first real test log for the same category of problem
// here before trusting this.
void ble_gatt_name_init();

// Queue a connectable, nameless BLE device for name resolution.
// Only call this for devices already confirmed connectable (the
// ble_evt_type check in bluetooth_scan.cpp); queuing a
// non-connectable device wastes a full timeout window on a connect
// attempt that cannot succeed by protocol definition, non-connectable
// advertising exists specifically to refuse this.
void ble_gatt_name_request(const uint8_t addr[6], esp_ble_addr_type_t addr_type);

// Drives the pending queue and the per-attempt timeout. Call
// frequently (this project calls it from bluetoothProcessingTask's
// existing 10ms loop) rather than giving this module its own
// FreeRTOS task, to avoid adding another task's stack to an
// already IRAM/heap-constrained build.
void ble_gatt_name_tick();

// Aborts any in-progress attempt and clears the pending queue and
// the per-session attempted-set. Call when BLE mode deactivates, so
// a stale connect attempt or a queued address from a previous
// activation doesn't carry across a mode switch.
void ble_gatt_name_reset();
