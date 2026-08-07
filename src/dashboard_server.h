#pragma once
#include <Arduino.h>

// Starts the HTTP + WebSocket dashboard server and mounts LittleFS.
// Call after the softAP is already up (wifi_scan_init() already
// calls esp_wifi_start() in AP mode), since this server binds to
// that same network interface. Requires the dashboard's HTML/CSS/JS
// already uploaded to the LittleFS partition via
// `pio run --target uploadfs`, against the partition layout locked
// in SESSION_3_ADDENDUM.md Section 4.
void dashboard_server_init();

// Broadcast + history helpers. Each is internally mutex-protected
// and safe to call from any task on either core. Every call does
// two things: appends to a bounded, overwrite-oldest ring buffer
// (so a client connecting mid-session gets useful recent history
// via the snapshot message, not just live events from that point
// forward), and pushes the same event as a WebSocket frame to every
// currently connected client. History is capped, not unbounded,
// see the module's DASHBOARD_MAX_* constants; this is a deliberate
// RAM-budget decision, not an oversight.
void dashboard_notify_wifi_signature(const uint8_t addr[6], int8_t rssi, uint32_t total);
void dashboard_notify_wifi_hotspot(const uint8_t addr[6], int8_t rssi, const char *ssid, uint32_t total);
// addr_class is one of "public", "static", "resolvable",
// "nonresolvable", or "reserved" (BLE), or "public" unconditionally
// for Classic BT (no LE-Privacy equivalent exists on that
// transport). This is the basis for defending the Bluetooth device
// count against the same address-rotation overcounting risk already
// documented for WiFi: a resolved name confirms an entry is a real
// signal, it does not confirm two entries are two distinct physical
// devices, address classification does.
// name_mechanism is one of "classic", "ble_connectable", or
// "ble_idle", distinct from whether a name has actually resolved
// yet. "ble_idle" means this entry structurally can never yield a
// name by any mechanism this project has, it's a non-connectable
// BLE advertisement, exactly how a phone idling in a pocket commonly
// broadcasts. "ble_connectable" means a GATT name-resolution attempt
// either already ran or is queued to. "classic" always gets a
// resolution attempt regardless, Classic BT has no equivalent
// connectable/non-connectable gate. The frontend uses this to show
// a genuinely idle device differently from one that's merely still
// waiting on resolution, rather than an identical generic
// placeholder for both.
void dashboard_notify_bt_device(const uint8_t addr[6], int8_t rssi, const char *name, const char *addr_class, const char *name_mechanism, uint32_t total);

// Classic BT's remote-name resolution arrives asynchronously, after
// the device has already been counted and displayed. This patches
// the name on an already-known entry rather than adding a second
// list entry or touching the count.
void dashboard_notify_bt_device_name_update(const uint8_t addr[6], const char *name);
// strength_pct is peak_mv normalized into the 0-100 range using the
// calibration bounds already locked in SESSION_2_ADDENDUM.md Section
// 2.2 (weakest detectable signal targets ~200-300mV, strongest
// realistic signal ~1200-1300mV at the LM358 output). This exists
// because raw millivolts mean nothing to a non-technical invigilator
// reading the dashboard; a bounded percentage does. It is not a
// calibrated RF power measurement, a discrete envelope detector has
// no claim to that, it is this project's own bench-calibrated
// range expressed as a readable figure.
void dashboard_notify_cellular_event(uint32_t timestamp_ms, uint32_t peak_mv);
void dashboard_set_wifi_active(bool active);
void dashboard_set_bt_active(bool active);

// Explicitly drops every tracked WebSocket client without attempting
// to notify them first (there is nothing to notify them over, the
// radio carrying that connection is about to go down). Call this
// immediately before tearing down the WiFi radio for Cellular mode,
// rather than relying on the next failed send to discover each dead
// client reactively, since Cellular mode's entire premise is that no
// send will ever reach a client until the radio comes back up.
void dashboard_clear_ws_clients();

// Warns connected clients that a full 13-channel sweep is about to
// start and the connection will briefly drop, part of the burst-sweep
// design that resolves the conflict between channel-hopping and a
// connected softAP client (esp_wifi_set_channel() is refused while a
// station is associated). The client's existing reconnect logic
// handles the rest.
void dashboard_notify_wifi_sweep_warning();

// Broadcast immediately before the WiFi radio is torn down for
// Cellular mode (see wifi_scan.cpp), while the connection carrying
// this message still exists. Lets the dashboard distinguish "you're
// about to lose connection because Cellular mode is intentionally
// taking the radio down" from an ordinary/unexplained disconnect,
// the same distinction the sweep-warning already makes for burst
// sweeps.
void dashboard_notify_cellular_mode_entering();