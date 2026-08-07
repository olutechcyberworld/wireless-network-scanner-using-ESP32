#include "wifi_scan.h"
#include "scan_control.h"
#include "oled_display.h"
#include "dashboard_server.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"
#include <cstring>
#include <set>

typedef enum {
    WIFI_FRAME_PROBE_REQUEST,  // client-originated, address = client MAC
    WIFI_FRAME_BEACON          // AP-originated, address = BSSID
} WifiFrameType;

typedef struct {
    WifiFrameType frame_type;
    uint8_t addr[6];
    int8_t  rssi;
    uint32_t timestamp_ms;
    char ssid[33];      // only populated for WIFI_FRAME_BEACON
    bool ssid_present;  // false also covers a legitimately hidden/broadcast SSID, not just parse failure
} WifiMgmtEvent;

static QueueHandle_t wifiEventQueue;
static QueueHandle_t inControlQueue;
static volatile uint32_t rawMgmtFrameCount = 0;

static void IRAM_ATTR wifi_promiscuous_cb(void *buf, wifi_promiscuous_pkt_type_t type) {
    if (type != WIFI_PKT_MGMT) return;
    rawMgmtFrameCount++;

    wifi_promiscuous_pkt_t *pkt = (wifi_promiscuous_pkt_t *)buf;
    uint8_t *payload = pkt->payload;
    uint8_t frameControl = payload[0] & 0xFC;

    WifiMgmtEvent evt;
    evt.ssid_present = false;
    evt.ssid[0] = '\0';

    if (frameControl == 0x40) {
        // Probe request: Address2 (offset 10) is the transmitter,
        // i.e. the client's own MAC.
        evt.frame_type = WIFI_FRAME_PROBE_REQUEST;
        memcpy(evt.addr, payload + 10, 6);
    } else if (frameControl == 0x80) {
        // Beacon: Address3 (offset 16) is the BSSID field. Address2
        // equals the same value in practice for a real AP, but
        // Address3 is the semantically correct field to read.
        evt.frame_type = WIFI_FRAME_BEACON;
        memcpy(evt.addr, payload + 16, 6);

        // Beacon fixed fields (Timestamp 8 + Beacon Interval 2 +
        // Capability Info 2 = 12 bytes) sit right after the 24-byte
        // MAC header, so the SSID information element starts at
        // offset 36: 1 byte element ID (0x00 for SSID), 1 byte
        // length, then the SSID text itself. Bounds-checked against
        // the actual captured length, promiscuous buffers aren't
        // guaranteed to hold the full frame.
        int sigLen = pkt->rx_ctrl.sig_len;
        const int SSID_IE_OFFSET = 36;
        if (sigLen > SSID_IE_OFFSET + 1) {
            uint8_t elementId = payload[SSID_IE_OFFSET];
            uint8_t ssidLen = payload[SSID_IE_OFFSET + 1];
            if (elementId == 0x00 && ssidLen > 0 && ssidLen <= 32 &&
                sigLen >= SSID_IE_OFFSET + 2 + ssidLen) {
                memcpy(evt.ssid, payload + SSID_IE_OFFSET + 2, ssidLen);
                evt.ssid[ssidLen] = '\0';
                evt.ssid_present = true;
            }
            // ssidLen == 0 with elementId == 0x00 is a legitimately
            // hidden/broadcast SSID, not a parse failure, ssid_present
            // correctly stays false.
        }
    } else {
        return;  // not a frame type this project tracks
    }

    evt.rssi = pkt->rx_ctrl.rssi;
    evt.timestamp_ms = millis();

    xQueueSend(wifiEventQueue, &evt, 0);
}

// Processing task, Core 0. Inactive by default at boot, WiFi only
// scans while it is the confirmed active mode (Section on mode
// switching), mutually exclusive with Bluetooth per the coexistence
// contention finding.
static void wifiProcessingTask(void *pvParameters) {
    std::set<uint64_t> seenMacs;
    std::set<uint64_t> seenBssids;
    bool active = false;
    uint8_t currentChannel = 1;
    uint32_t lastHop = 0;
    const uint32_t CHANNEL_DWELL_MS = 150;

    // Full sweep vs. connected-client conflict: esp_wifi_set_channel()
    // is refused by the driver whenever a station is associated to
    // the softAP, changing the AP's channel would break that
    // client's link. Locked resolution: while a client is connected,
    // stay on the current channel (single-channel capture) except
    // for periodic warned bursts, during which the client is
    // expected to drop and reconnect via the client's own retry
    // logic. Without a connected client, hop freely as before.
    enum class BurstPhase { IDLE, WARNING, SWEEPING };
    BurstPhase burstPhase = BurstPhase::IDLE;
    uint32_t burstPhaseStart = 0;
    uint8_t burstChannelsVisited = 0;
    uint32_t lastBurstCompletedAt = 0;
    uint32_t sweepStartedAt = 0;
    const uint32_t BURST_INTERVAL_MS = 30000;  // time between forced full sweeps while a client is connected
    const uint32_t BURST_WARNING_MS  = 2000;   // warning shown before a burst actually starts hopping
    // Hard backstop on a sweep that can't complete (e.g. a station
    // reassociating fast enough to keep triggering the driver's CSA
    // path instead of an immediate channel switch, see the re-deauth
    // logic below). Generous relative to the 13*150ms=1950ms a clean
    // sweep needs, but bounded, so a failing sweep gives up and
    // waits out the normal 30s interval instead of refiring every
    // few seconds, which is what happened before this fix, every
    // hop failed, the sweep never completed, and the only exit path
    // (a client fully disconnecting) never updated
    // lastBurstCompletedAt, so a new warning fired again almost
    // immediately once a client's own reconnect churn briefly
    // dropped it to zero stations.
    const uint32_t MAX_SWEEP_DURATION_MS = 6000;

    // Tracks the WiFi radio's up/down state itself, separate from
    // `active` (which tracks promiscuous scanning specifically).
    // True at boot: wifi_scan_init() brings the softAP up
    // unconditionally today. Cellular mode is the one case where the
    // radio needs to come down entirely, not just pause scanning,
    // see the teardown/restart block below.
    bool radioUp = true;

    for (;;) {
        ModeActivation act;
        if (xQueueReceive(inControlQueue, &act, 0) == pdTRUE) {
            bool shouldBeActive = (act.active_mode == RADIO_MODE_WIFI);
            bool shouldRadioBeUp = (act.active_mode != RADIO_MODE_CELLULAR);

            if (shouldRadioBeUp && !radioUp) {
                // Leaving Cellular mode: bring the WiFi radio, the
                // softAP, and dashboard connectivity back up.
                // esp_wifi_start() alone is expected to be
                // sufficient here, esp_wifi_stop() does not tear
                // down the AP mode configuration or the netif set up
                // once in wifi_scan_init(), only the active radio
                // state, matching ESP-IDF's stop()/start() as the
                // pause/resume pair versus the heavier one-time
                // init()/deinit(). UNVERIFIED against a real log:
                // whether the AP, DHCP server, and a freshly
                // (re)connecting dashboard client all come back
                // cleanly on the first attempt, watch this
                // specifically on the next real test rather than
                // assuming it from this comment.
                esp_err_t err = esp_wifi_start();
                if (err != ESP_OK) {
                    Serial.printf("[WiFi] radio restart failed: %s\n", esp_err_to_name(err));
                } else {
                    radioUp = true;
                    Serial.println("[WiFi] radio restarted (leaving Cellular mode)");
                }
            }

            if (shouldBeActive && !active) {
                seenMacs.clear();
                seenBssids.clear();
                currentChannel = 1;
                esp_wifi_set_channel(currentChannel, WIFI_SECOND_CHAN_NONE);
                lastHop = millis();
                burstPhase = BurstPhase::IDLE;
                lastBurstCompletedAt = millis();
                esp_wifi_set_promiscuous(true);
                active = true;
                oled_set_wifi_count(0);
                oled_set_wifi_hotspot_count(0);
                dashboard_set_wifi_active(true);
                Serial.println("[WiFi] ACTIVATED");
            } else if (!shouldBeActive && active) {
                esp_wifi_set_promiscuous(false);
                active = false;
                dashboard_set_wifi_active(false);
                oled_set_wifi_sweep_active(false);
                Serial.println("[WiFi] DEACTIVATED");
            }

            if (!shouldRadioBeUp && radioUp) {
                // Entering Cellular mode: full radio teardown, not
                // merely a pause of promiscuous scanning. This is
                // the actual fix for the RF self-interference
                // hypothesis carried since SESSION_4_ADDENDUM.md:
                // the ESP32's own always-on softAP, not scanning
                // behavior, was identified as the likely dominant
                // coupling source into the analog RF front end.
                // esp_wifi_stop() tears the radio down entirely, the
                // dashboard is genuinely unreachable for as long as
                // Cellular mode is active, a real and visible
                // consequence flagged explicitly on both the OLED
                // and the RGB indicator, not a bug to hide.
                dashboard_notify_cellular_mode_entering();
                dashboard_clear_ws_clients();
                esp_wifi_stop();
                radioUp = false;
                Serial.println("[WiFi] radio stopped (entering Cellular mode, softAP torn down)");
            }
        }

        if (active) {
            wifi_sta_list_t staList;
            esp_err_t staErr = esp_wifi_ap_get_sta_list(&staList);
            bool clientConnected = (staErr == ESP_OK && staList.num > 0);

            if (burstPhase == BurstPhase::SWEEPING && millis() - sweepStartedAt >= MAX_SWEEP_DURATION_MS) {
                Serial.printf("[WiFi] sweep abandoned after timeout, %u/13 channels visited\n",
                    (unsigned)burstChannelsVisited);
                burstPhase = BurstPhase::IDLE;
                lastBurstCompletedAt = millis();  // respect the 30s interval even on failure
                oled_set_wifi_sweep_active(false);
            }

            if (clientConnected) {
                switch (burstPhase) {
                    case BurstPhase::IDLE:
                        if (millis() - lastBurstCompletedAt >= BURST_INTERVAL_MS) {
                            burstPhase = BurstPhase::WARNING;
                            burstPhaseStart = millis();
                            dashboard_notify_wifi_sweep_warning();
                            oled_set_wifi_sweep_active(true);
                            Serial.println("[WiFi] Full sweep warning issued, client connected");
                        }
                        break;
                    case BurstPhase::WARNING:
                        if (millis() - burstPhaseStart >= BURST_WARNING_MS) {
                            // Entering SWEEPING alone changes nothing,
                            // esp_wifi_set_channel() is refused by the
                            // driver as long as a station stays
                            // associated regardless of this task's
                            // internal state. The station has to be
                            // genuinely disconnected first. AID isn't
                            // exposed per-MAC in this IDF version's
                            // wifi_sta_list_t, so every AID up to the
                            // default max AP connection count (4) is
                            // deauthed; calls for AIDs with no actual
                            // station attached simply fail harmlessly.
                            for (uint16_t aid = 1; aid <= 4; aid++) {
                                esp_wifi_deauth_sta(aid);
                            }
                            burstPhase = BurstPhase::SWEEPING;
                            burstChannelsVisited = 0;
                            sweepStartedAt = millis();
                            Serial.println("[WiFi] Full sweep starting, client force-disconnected");
                        }
                        break;
                    case BurstPhase::SWEEPING:
                        // Re-issued every cycle a station is found
                        // connected, not just once at the
                        // WARNING->SWEEPING transition. A station
                        // reassociating mid-sweep, its own fast
                        // reconnect logic, or a second client that
                        // was never the one originally deauthed,
                        // puts the driver back into "has an
                        // associated station" state, which defers
                        // esp_wifi_set_channel() to a CSA process
                        // this design's 150ms dwell can't wait out.
                        // A single deauth at sweep start wasn't
                        // enough against a client this quick to
                        // reconnect, confirmed by the last real test
                        // log showing 100% hop failure against two
                        // simultaneously-reconnecting clients.
                        for (uint16_t aid = 1; aid <= 4; aid++) {
                            esp_wifi_deauth_sta(aid);
                        }
                        break;  // hop attempt itself still handled by the block below
                }
            } else if (burstPhase == BurstPhase::WARNING) {
                // Client left before the sweep even started, nothing
                // to interrupt, safe to reset immediately rather
                // than waiting out a warning window for someone no
                // longer there.
                burstPhase = BurstPhase::IDLE;
                oled_set_wifi_sweep_active(false);
            }
            // If burstPhase == SWEEPING here, deliberately falls
            // through without resetting: hopping is already
            // unrestricted with zero stations connected (see
            // allowHopThisCycle just below), so a sweep already in
            // progress keeps running toward completion instead of
            // discarding its progress every time a client's own
            // reconnect logic produces a brief connected/disconnected
            // flicker, this was the direct cause of the rapid refire
            // loop seen in the last real test log: burstPhase reset
            // to IDLE on every flicker, but lastBurstCompletedAt was
            // never updated by that reset, so the very next tick's
            // IDLE-state check saw the 30s cooldown as already
            // expired and fired a new warning immediately.

            bool allowHopThisCycle = (!clientConnected) || (burstPhase == BurstPhase::SWEEPING);

            if (allowHopThisCycle && millis() - lastHop >= CHANNEL_DWELL_MS) {
                uint8_t nextChannel = (currentChannel % 13) + 1;
                esp_err_t hopErr = esp_wifi_set_channel(nextChannel, WIFI_SECOND_CHAN_NONE);
                lastHop = millis();

                if (hopErr == ESP_OK) {
                    currentChannel = nextChannel;
                    if (burstPhase == BurstPhase::SWEEPING) {
                        burstChannelsVisited++;
                        if (burstChannelsVisited >= 13) {
                            burstPhase = BurstPhase::IDLE;
                            lastBurstCompletedAt = millis();
                            oled_set_wifi_sweep_active(false);
                            Serial.println("[WiFi] Full sweep complete");
                        }
                    }
                } else if (burstPhase == BurstPhase::SWEEPING) {
                    // Genuinely failed, not just logged as if it
                    // succeeded, stays on currentChannel and retries
                    // next cycle rather than silently advancing.
                    Serial.printf("[WiFi] channel hop failed during sweep: %s\n", esp_err_to_name(hopErr));
                }
            }

            WifiMgmtEvent evt;
            while (xQueueReceive(wifiEventQueue, &evt, 0) == pdTRUE) {
                uint64_t key = 0;
                memcpy(&key, evt.addr, 6);

                if (evt.frame_type == WIFI_FRAME_PROBE_REQUEST) {
                    if (seenMacs.find(key) == seenMacs.end()) {
                        seenMacs.insert(key);
                        oled_set_wifi_count((uint32_t)seenMacs.size());
                        dashboard_notify_wifi_signature(evt.addr, evt.rssi, (uint32_t)seenMacs.size());
                        Serial.printf("[WiFi] NEW SIGNATURE  mac=%02X:%02X:%02X:%02X:%02X:%02X  rssi=%d  (total: %u)\n",
                            evt.addr[0], evt.addr[1], evt.addr[2], evt.addr[3], evt.addr[4], evt.addr[5],
                            evt.rssi, (unsigned)seenMacs.size());
                    }
                } else if (evt.frame_type == WIFI_FRAME_BEACON) {
                    if (seenBssids.find(key) == seenBssids.end()) {
                        seenBssids.insert(key);
                        oled_set_wifi_hotspot_count((uint32_t)seenBssids.size());
                        dashboard_notify_wifi_hotspot(evt.addr, evt.rssi,
                            evt.ssid_present ? evt.ssid : nullptr, (uint32_t)seenBssids.size());
                        Serial.printf("[WiFi] NEW HOTSPOT  bssid=%02X:%02X:%02X:%02X:%02X:%02X  rssi=%d  ssid=%s  (total: %u)\n",
                            evt.addr[0], evt.addr[1], evt.addr[2], evt.addr[3], evt.addr[4], evt.addr[5],
                            evt.rssi, evt.ssid_present ? evt.ssid : "(hidden)", (unsigned)seenBssids.size());
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void wifi_scan_init(QueueHandle_t controlQueue) {
    inControlQueue = controlQueue;
    wifiEventQueue = xQueueCreate(40, sizeof(WifiMgmtEvent));

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    // AP mode requires a registered netif before esp_wifi_start(),
    // or the WiFi driver has no lwIP interface to bind its
    // interface-level setup to (DHCP server start-up among other
    // things), and esp_wifi_start() crashes with "Invalid mbox"
    // rather than failing gracefully. esp_netif_init() and
    // esp_event_loop_create_default() are each safe to call even if
    // Arduino's own startup already called them, ESP_ERR_INVALID_STATE
    // means "already done", not a real failure, anything else is.
    esp_err_t netifErr = esp_netif_init();
    if (netifErr != ESP_OK && netifErr != ESP_ERR_INVALID_STATE) {
        Serial.printf("[WiFi] esp_netif_init failed: %s\n", esp_err_to_name(netifErr));
    }
    esp_err_t eventErr = esp_event_loop_create_default();
    if (eventErr != ESP_OK && eventErr != ESP_ERR_INVALID_STATE) {
        Serial.printf("[WiFi] esp_event_loop_create_default failed: %s\n", esp_err_to_name(eventErr));
    }
    esp_netif_create_default_wifi_ap();

    esp_wifi_init(&cfg);
    esp_wifi_set_mode(WIFI_MODE_AP);
    esp_wifi_start();

    esp_wifi_set_promiscuous_rx_cb(&wifi_promiscuous_cb);

    wifi_promiscuous_filter_t filter = { .filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT };
    esp_wifi_set_promiscuous_filter(&filter);

    esp_wifi_set_promiscuous(false);  // inactive until confirmed via button

    xTaskCreatePinnedToCore(wifiProcessingTask, "WiFiProcessTask", 4096, NULL, 1, NULL, 0);  // Core 0
}

QueueHandle_t wifi_scan_get_event_queue() {
    return wifiEventQueue;
}