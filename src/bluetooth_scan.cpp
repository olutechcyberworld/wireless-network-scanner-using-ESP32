#include "bluetooth_scan.h"
#include "scan_control.h"
#include "oled_display.h"
#include "dashboard_server.h"
#include "ble_gatt_name.h"
#include "esp_bt.h"
#include "esp_bt_main.h"
#include "esp_gap_bt_api.h"
#include "esp_gap_ble_api.h"
#include <cstring>
#include <set>

typedef struct {
    uint8_t  mac[6];
    int8_t   rssi;
    char     name[32];
    bool     name_present;
    bool     is_name_only_update;  // true = resolved name for an already-known device, not a new discovery
    uint32_t timestamp_ms;
    // Address-type classification, set on initial discovery only
    // (unused/irrelevant on a name-only update, that event patches
    // an existing entry rather than creating one). See
    // classifyBleAddress() below for what each value means and why
    // this exists: a resolved name tells you an entry is a real
    // signal, it does not tell you whether two different MAC
    // entries are the same physical device. Address-type
    // classification is the thing that actually speaks to that.
    char     addr_class[16];
    // Only meaningful for BLE-origin, non-name-only-update events.
    // connectable reflects the advertising PDU type (ble_evt_type):
    // only ESP_BLE_EVT_CONN_ADV / ESP_BLE_EVT_CONN_DIR_ADV can ever
    // accept a GATT connection, by protocol definition, everything
    // else (non-connectable/scannable-only advertising, which is
    // exactly how modern phones commonly broadcast their idle-state
    // BLE presence for privacy) will always refuse a connect
    // attempt. raw_addr_type carries the stack's own reported
    // address type through to the GATT module, which needs it
    // verbatim for esp_ble_gattc_open()'s remote_addr_type argument,
    // this is not the same thing as this event's addr_class string,
    // which is derived independently from the raw address bits.
    bool     connectable;
    esp_ble_addr_type_t raw_addr_type;
    // True for BLE-origin events, false for Classic. Needed
    // specifically because Classic events also default connectable
    // to false (never set true anywhere in bt_gap_cb, Classic has no
    // equivalent concept), so connectable alone can't distinguish
    // "Classic device, name resolution attempted regardless" from
    // "BLE device that structurally can never yield a name because
    // it's idle and non-connectable." The dashboard needs that
    // distinction to label these two very different situations
    // differently rather than showing an identical generic
    // "(no name)" for both.
    bool     is_ble;
} BluetoothAdvEvent;

// Bluetooth Core Specification, Vol 6, Part B: for a random device
// address, the two most significant bits of the first octet (as
// stored/printed here, addr[0], matching this project's existing
// MAC display convention) indicate the address subtype. A public
// address is a separate address-type flag entirely, not a bit
// pattern, and is manufacturer-fixed, it never rotates.
//
//   11xxxxxx  static random        fixed for the current power cycle
//   01xxxxxx  resolvable private   rotates periodically (commonly ~15 min)
//   00xxxxxx  non-resolvable priv. rotates, unresolvable by any observer
//   10xxxxxx  reserved             not expected in practice
//
// The reported address-type enum from the stack (BLE_ADDR_TYPE_*)
// is checked first for the public case, but is not trusted alone to
// distinguish resolvable-private from plain random beyond that: a
// documented ESP-IDF inconsistency (Espressif forum, esp32.com
// t=7657) shows a genuine RPA sometimes surfacing as plain
// BLE_ADDR_TYPE_RANDOM rather than BLE_ADDR_TYPE_RPA_RANDOM. Reading
// the raw address bits directly is the more reliable of the two,
// and is the same manual check already performed by hand in Session
// 4 ("checked by hand against the address-type bits each time").
static const char *classifyBleAddress(const uint8_t addr[6], esp_ble_addr_type_t reportedType) {
    if (reportedType == BLE_ADDR_TYPE_PUBLIC) {
        return "public";
    }
    uint8_t topBits = addr[0] & 0xC0;
    if (topBits == 0xC0) return "static";
    if (topBits == 0x40) return "resolvable";
    if (topBits == 0x00) return "nonresolvable";
    return "reserved";
}

static QueueHandle_t bluetoothEventQueue;
static QueueHandle_t inControlQueue;
static volatile bool btDiscoveryRunning = false;

// Classic BT inquiry results often arrive without a name, EIR data
// carrying the name is inconsistent across devices. This tracks
// which addresses already have an esp_bt_gap_read_remote_name()
// request outstanding, so a device reappearing across multiple
// inquiry results (normal during a single discovery window) doesn't
// trigger a duplicate request each time. Only ever touched from
// bt_gap_cb, which runs on a single Bluedroid callback thread, so
// no mutex needed.
static std::set<uint64_t> requestedNames;

static void bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param) {
    if (event == ESP_BT_GAP_DISC_STATE_CHANGED_EVT) {
        btDiscoveryRunning = (param->disc_st_chg.state == ESP_BT_GAP_DISCOVERY_STARTED);
        Serial.printf("[Bluetooth] Classic discovery state: %s\n",
            btDiscoveryRunning ? "STARTED" : "STOPPED");
        return;
    }

    if (event == ESP_BT_GAP_READ_REMOTE_NAME_EVT) {
        char macbuf[18];
        snprintf(macbuf, sizeof(macbuf), "%02X:%02X:%02X:%02X:%02X:%02X",
            param->read_rmt_name.bda[0], param->read_rmt_name.bda[1], param->read_rmt_name.bda[2],
            param->read_rmt_name.bda[3], param->read_rmt_name.bda[4], param->read_rmt_name.bda[5]);

        if (param->read_rmt_name.stat == ESP_BT_STATUS_SUCCESS) {
            BluetoothAdvEvent evt = {};
            memcpy(evt.mac, param->read_rmt_name.bda, 6);
            evt.timestamp_ms = millis();
            evt.is_name_only_update = true;

            size_t len = strlen((const char *)param->read_rmt_name.rmt_name);
            if (len > sizeof(evt.name) - 1) len = sizeof(evt.name) - 1;
            memcpy(evt.name, param->read_rmt_name.rmt_name, len);
            evt.name[len] = '\0';
            evt.name_present = true;

            Serial.printf("[Bluetooth] REMOTE NAME RESOLVED  mac=%s  name=%s\n", macbuf, evt.name);
            xQueueSend(bluetoothEventQueue, &evt, 0);
        } else {
            // Not retried, the device stays "(none)" for this
            // session, a single failure isn't worth spamming repeat
            // requests over. Logged so a lack of resolved names is
            // distinguishable from "no Classic devices were nearby"
            // rather than silently indistinguishable from it.
            Serial.printf("[Bluetooth] REMOTE NAME FAILED  mac=%s  status=%d\n", macbuf, (int)param->read_rmt_name.stat);
        }
        return;
    }

    if (event != ESP_BT_GAP_DISC_RES_EVT) {
        Serial.printf("[Bluetooth] Classic GAP event %d (unhandled)\n", (int)event);
        return;
    }

    BluetoothAdvEvent evt = {};
    memcpy(evt.mac, param->disc_res.bda, 6);
    evt.timestamp_ms = millis();
    evt.name_present = false;
    evt.is_name_only_update = false;
    // Classic BT has no equivalent to BLE's LE Privacy feature; the
    // BD_ADDR returned by inquiry is the device's address for the
    // duration it stays discoverable, not a rotating identifier.
    // Classified as "public" on that protocol basis, not inferred
    // from the address bits the way the BLE path below has to.
    strncpy(evt.addr_class, "public", sizeof(evt.addr_class) - 1);

    for (int i = 0; i < param->disc_res.num_prop; i++) {
        esp_bt_gap_dev_prop_t *p = &param->disc_res.prop[i];
        if (p->type == ESP_BT_GAP_DEV_PROP_RSSI) {
            evt.rssi = *(int8_t *)(p->val);
        } else if (p->type == ESP_BT_GAP_DEV_PROP_BDNAME) {
            size_t len = p->len < sizeof(evt.name) - 1 ? p->len : sizeof(evt.name) - 1;
            memcpy(evt.name, p->val, len);
            evt.name[len] = '\0';
            evt.name_present = true;
        }
    }

    xQueueSend(bluetoothEventQueue, &evt, 0);

    if (!evt.name_present) {
        uint64_t key = 0;
        memcpy(&key, evt.mac, 6);
        if (requestedNames.find(key) == requestedNames.end()) {
            requestedNames.insert(key);
            Serial.printf("[Bluetooth] REMOTE NAME REQUESTED  mac=%02X:%02X:%02X:%02X:%02X:%02X\n",
                evt.mac[0], evt.mac[1], evt.mac[2], evt.mac[3], evt.mac[4], evt.mac[5]);
            esp_bt_gap_read_remote_name(evt.mac);
        }
    }
}

static void ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
    // Previously this function discarded every event except
    // SCAN_RESULT_EVT on its very first line, which meant it was
    // structurally impossible to ever see whether scan params were
    // actually applied or whether a scan actually started, both are
    // reported asynchronously through events this code never looked
    // at. esp_ble_gap_start_scanning()'s return value only confirms
    // the command was accepted, not that scanning is genuinely
    // running, that's this event, with its own status field.
    if (event == ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT) {
        Serial.printf("[Bluetooth] BLE scan param set complete, status=%d\n",
            (int)param->scan_param_cmpl.status);
        return;
    }
    if (event == ESP_GAP_BLE_SCAN_START_COMPLETE_EVT) {
        Serial.printf("[Bluetooth] BLE scan start complete, status=%d%s\n",
            (int)param->scan_start_cmpl.status,
            param->scan_start_cmpl.status == ESP_BT_STATUS_SUCCESS ? "" : "  <-- scanning did NOT actually start");
        return;
    }
    if (event == ESP_GAP_BLE_SCAN_RESULT_EVT) {
        if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT) {
            // Fires for housekeeping sub-events too (scan window
            // complete, procedure cancelled, etc.), not just "found
            // a device". Logging the raw search_evt value here is
            // temporary diagnostic verbosity: if this callback is
            // firing at all but every occurrence reports something
            // other than INQ_RES, that's the actual finding, not
            // "nothing is happening at the radio," something is
            // happening, just not what this code currently acts on.
            Serial.printf("[Bluetooth] BLE scan_rst event, search_evt=%d (not INQ_RES)\n",
                (int)param->scan_rst.search_evt);
            return;
        }
    } else {
        // Catches every other BLE GAP event this callback receives,
        // including ones this code has never had a name for. If BLE
        // scanning is genuinely working, this line should stay
        // essentially silent once past the two startup events
        // already handled above; anything appearing here repeatedly
        // during an active scan is new information.
        Serial.printf("[Bluetooth] BLE GAP event %d (unhandled)\n", (int)event);
        return;
    }

    BluetoothAdvEvent evt = {};
    memcpy(evt.mac, param->scan_rst.bda, 6);
    evt.rssi = param->scan_rst.rssi;
    evt.timestamp_ms = millis();
    evt.name_present = false;
    evt.is_name_only_update = false;
    evt.is_ble = true;
    const char *cls = classifyBleAddress(evt.mac, param->scan_rst.ble_addr_type);
    strncpy(evt.addr_class, cls, sizeof(evt.addr_class) - 1);
    evt.raw_addr_type = param->scan_rst.ble_addr_type;
    // Only CONN_ADV (undirected connectable) and CONN_DIR_ADV
    // (directed connectable) accept a GATT connection attempt.
    // NON_CONN_ADV and DISC_ADV are scannable-or-observable but
    // explicitly refuse connection at the link layer, this is a
    // protocol-level fact, not a firmware limitation on this
    // project's side.
    evt.connectable = (param->scan_rst.ble_evt_type == ESP_BLE_EVT_CONN_ADV ||
                        param->scan_rst.ble_evt_type == ESP_BLE_EVT_CONN_DIR_ADV);

    uint8_t name_len = 0;
    uint8_t *adv_name = esp_ble_resolve_adv_data(
        param->scan_rst.ble_adv, ESP_BLE_AD_TYPE_NAME_CMPL, &name_len);
    if (adv_name != NULL && name_len > 0) {
        size_t len = name_len < sizeof(evt.name) - 1 ? name_len : sizeof(evt.name) - 1;
        memcpy(evt.name, adv_name, len);
        evt.name[len] = '\0';
        evt.name_present = true;
    }

    // Verification logging for the adv/scan-response merge question
    // raised this session: adv_data_len and scan_rsp_len are
    // reported separately even though esp_ble_resolve_adv_data()
    // searches both halves of the same combined ble_adv buffer. If
    // a device is ever seen with scan_rsp_len > 0 and name_present
    // == false, that specific device's scan response did not carry
    // AD_TYPE_NAME_CMPL, not a parsing failure on this end. If every
    // observed device shows scan_rsp_len == 0, active scanning is
    // not actually soliciting responses on this hardware/antenna and
    // needs its own investigation before trusting the merge at all.
    Serial.printf("[Bluetooth] BLE scan result  mac=%02X:%02X:%02X:%02X:%02X:%02X  addr_class=%s  connectable=%s  evt_type=%d  adv_len=%u  scan_rsp_len=%u  name_present=%s\n",
        evt.mac[0], evt.mac[1], evt.mac[2], evt.mac[3], evt.mac[4], evt.mac[5],
        evt.addr_class, evt.connectable ? "true" : "false", (int)param->scan_rst.ble_evt_type,
        (unsigned)param->scan_rst.adv_data_len, (unsigned)param->scan_rst.scan_rsp_len,
        evt.name_present ? "true" : "false");

    xQueueSend(bluetoothEventQueue, &evt, 0);
}

// Processing task, Core 0. Inactive by default at boot, Bluetooth
// only scans while it is the confirmed active mode, mutually
// exclusive with WiFi per the coexistence contention finding.
static void bluetoothProcessingTask(void *pvParameters) {
    std::set<uint64_t> seenMacs;
    bool active = false;
    // Tracks whether this device's own Bluetooth radio is currently
    // set discoverable/connectable, separate from `active` (which
    // tracks scanning for other devices). Cellular mode needs this
    // off: bluetooth_scan_init() sets ESP_BT_CONNECTABLE +
    // ESP_BT_GENERAL_DISCOVERABLE once at boot and nothing since has
    // ever revisited it, meaning this device's own Classic BT radio
    // keeps doing periodic inquiry-scan and page-scan windows
    // continuously, through every mode, including the one mode
    // specifically meant to give the analog RF front end a clean
    // environment. Deliberately not a full esp_bt_controller_disable()
    // / esp_bluedroid_disable() cycle here, this project already has
    // one radio-restart path (wifi_scan.cpp's Cellular-mode teardown)
    // flagged as unverified this session; toggling scan mode is the
    // smaller, better-understood intervention that directly targets
    // the behavior actually implicated (this device's own discoverable/
    // connectable radio windows), without adding a second uncertain
    // full-stack restart on top of it. If RF saturation persists in
    // Cellular mode even with this in place, escalating to a full
    // controller disable/enable is the next thing to try, not the
    // first.
    bool discoverable = true;
    uint32_t lastRetrigger = 0;
    const uint8_t DISCOVERY_DURATION_SEC = 20;
    const uint32_t RETRIGGER_INTERVAL_MS = (uint32_t)DISCOVERY_DURATION_SEC * 1000;

    for (;;) {
        ModeActivation act;
        if (xQueueReceive(inControlQueue, &act, 0) == pdTRUE) {
            bool shouldBeActive = (act.active_mode == RADIO_MODE_BLUETOOTH);
            bool shouldBeDiscoverable = (act.active_mode != RADIO_MODE_CELLULAR);

            if (!shouldBeDiscoverable && discoverable) {
                esp_bt_gap_set_scan_mode(ESP_BT_NON_CONNECTABLE, ESP_BT_NON_DISCOVERABLE);
                discoverable = false;
                Serial.println("[Bluetooth] scan mode set non-discoverable/non-connectable (entering Cellular mode)");
            }

            if (shouldBeActive && !active) {
                seenMacs.clear();
                requestedNames.clear();
                ble_gatt_name_reset();
                // Previously unchecked: if either of these returns an
                // error, "ACTIVATED" below still prints, since it's
                // only this task's own state flag, not confirmation
                // that scanning genuinely started. That would look
                // exactly like "activated but nothing ever appears,"
                // which is the symptom being chased right now.
                esp_err_t discErr = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, DISCOVERY_DURATION_SEC, 0);
                if (discErr != ESP_OK) {
                    Serial.printf("[Bluetooth] esp_bt_gap_start_discovery FAILED: %s\n", esp_err_to_name(discErr));
                }
                // 0 = scan continuously until esp_ble_gap_stop_scanning()
                // is called, no periodic retrigger needed for BLE.
                // Classic discovery has no continuous option (duration
                // is capped by the stack), so it still needs the
                // retrigger loop below.
                esp_err_t bleScanErr = esp_ble_gap_start_scanning(0);
                if (bleScanErr != ESP_OK) {
                    Serial.printf("[Bluetooth] esp_ble_gap_start_scanning FAILED: %s\n", esp_err_to_name(bleScanErr));
                }
                lastRetrigger = millis();
                active = true;
                oled_set_bt_count(0);
                dashboard_set_bt_active(true);
                Serial.printf("[Bluetooth] ACTIVATED  (discovery_start=%s, ble_scan_start=%s)\n",
                    discErr == ESP_OK ? "OK" : esp_err_to_name(discErr),
                    bleScanErr == ESP_OK ? "OK" : esp_err_to_name(bleScanErr));
            } else if (!shouldBeActive && active) {
                esp_bt_gap_cancel_discovery();
                esp_ble_gap_stop_scanning();
                ble_gatt_name_reset();
                active = false;
                dashboard_set_bt_active(false);
                Serial.println("[Bluetooth] DEACTIVATED");
            }

            if (shouldBeDiscoverable && !discoverable) {
                esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
                discoverable = true;
                Serial.println("[Bluetooth] scan mode restored (leaving Cellular mode)");
            }
        }

        if (active) {
            ble_gatt_name_tick();

            if (!btDiscoveryRunning && millis() - lastRetrigger >= RETRIGGER_INTERVAL_MS) {
                esp_err_t retriggerErr = esp_bt_gap_start_discovery(ESP_BT_INQ_MODE_GENERAL_INQUIRY, DISCOVERY_DURATION_SEC, 0);
                if (retriggerErr != ESP_OK) {
                    Serial.printf("[Bluetooth] discovery retrigger FAILED: %s\n", esp_err_to_name(retriggerErr));
                }
                lastRetrigger = millis();
            }

            BluetoothAdvEvent evt;
            while (xQueueReceive(bluetoothEventQueue, &evt, 0) == pdTRUE) {
                uint64_t key = 0;
                memcpy(&key, evt.mac, 6);

                if (evt.is_name_only_update) {
                    // Only meaningful if this device was already
                    // counted, an unsolicited name arriving for an
                    // address we never saw a discovery result for
                    // shouldn't happen, but guard against it anyway.
                    if (seenMacs.find(key) != seenMacs.end()) {
                        dashboard_notify_bt_device_name_update(evt.mac, evt.name);
                        Serial.printf("[Bluetooth] NAME RESOLVED  mac=%02X:%02X:%02X:%02X:%02X:%02X  name=%s\n",
                            evt.mac[0], evt.mac[1], evt.mac[2], evt.mac[3], evt.mac[4], evt.mac[5], evt.name);
                    }
                    continue;
                }

                if (seenMacs.find(key) == seenMacs.end()) {
                    seenMacs.insert(key);
                    oled_set_bt_count((uint32_t)seenMacs.size());

                    // What can ever resolve a name for this entry,
                    // distinct from whether one has resolved yet.
                    // Classic always gets a resolution attempt
                    // regardless of any "connectable" concept (it
                    // has none), so it must be checked first, or a
                    // Classic entry (which defaults connectable to
                    // false, that field is never touched in
                    // bt_gap_cb) would be mislabeled identically to
                    // a genuinely unreachable idle BLE device.
                    const char *nameMechanism = evt.is_ble
                        ? (evt.connectable ? "ble_connectable" : "ble_idle")
                        : "classic";

                    dashboard_notify_bt_device(evt.mac, evt.rssi,
                        evt.name_present ? evt.name : nullptr, evt.addr_class, nameMechanism, (uint32_t)seenMacs.size());
                    Serial.printf("[Bluetooth] NEW DEVICE  mac=%02X:%02X:%02X:%02X:%02X:%02X  rssi=%d  name=%s  addr_class=%s  name_mechanism=%s  (total: %u)\n",
                        evt.mac[0], evt.mac[1], evt.mac[2], evt.mac[3], evt.mac[4], evt.mac[5],
                        evt.rssi, evt.name_present ? evt.name : "(none)", evt.addr_class, nameMechanism, (unsigned)seenMacs.size());

                    // GATT-based name resolution is BLE-only (Classic
                    // already has its own path via
                    // esp_bt_gap_read_remote_name, see the
                    // is_name_only_update branch above) and only
                    // worth attempting against a device that
                    // actually accepts connections; see
                    // BluetoothAdvEvent.connectable's comment for
                    // why a non-connectable target would just burn a
                    // timeout window for nothing.
                    if (!evt.name_present && !evt.is_name_only_update && evt.connectable) {
                        ble_gatt_name_request(evt.mac, evt.raw_addr_type);
                    }
                }
            }
        }

        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void bluetooth_scan_init(QueueHandle_t controlQueue) {
    inControlQueue = controlQueue;
    bluetoothEventQueue = xQueueCreate(40, sizeof(BluetoothAdvEvent));

    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_err_t err;

    err = esp_bt_controller_init(&bt_cfg);
    Serial.printf("esp_bt_controller_init: %s\n", esp_err_to_name(err));
    if (err != ESP_OK) return;

    err = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    Serial.printf("esp_bt_controller_enable: %s\n", esp_err_to_name(err));
    if (err != ESP_OK) return;

    err = esp_bluedroid_init();
    Serial.printf("esp_bluedroid_init: %s\n", esp_err_to_name(err));
    if (err != ESP_OK) return;

    err = esp_bluedroid_enable();
    Serial.printf("esp_bluedroid_enable: %s\n", esp_err_to_name(err));
    if (err != ESP_OK) return;

    esp_bt_gap_register_callback(bt_gap_cb);
    esp_err_t scanModeErr = esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
    Serial.printf("esp_bt_gap_set_scan_mode: %s\n", esp_err_to_name(scanModeErr));

    esp_ble_gap_register_callback(ble_gap_cb);
    ble_gatt_name_init();
    static esp_ble_scan_params_t ble_scan_params = {
        .scan_type          = BLE_SCAN_TYPE_ACTIVE,
        .own_addr_type      = BLE_ADDR_TYPE_PUBLIC,
        .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
        .scan_interval      = 0x50,
        .scan_window        = 0x30,
        .scan_duplicate     = BLE_SCAN_DUPLICATE_DISABLE,
    };
    esp_err_t scanParamsErr = esp_ble_gap_set_scan_params(&ble_scan_params);
    Serial.printf("esp_ble_gap_set_scan_params: %s\n", esp_err_to_name(scanParamsErr));
    // Scanning starts only once WiFi/Bluetooth mode is confirmed via
    // the button, not unconditionally here.

    xTaskCreatePinnedToCore(bluetoothProcessingTask, "BTProcessTask", 4096, NULL, 1, NULL, 0);  // Core 0
}

QueueHandle_t bluetooth_scan_get_event_queue() {
    return bluetoothEventQueue;
}