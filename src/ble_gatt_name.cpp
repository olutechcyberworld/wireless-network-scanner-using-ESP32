#include "ble_gatt_name.h"
#include "dashboard_server.h"
#include "esp_gattc_api.h"
#include "esp_gatt_defs.h"
#include <deque>
#include <set>
#include <cstring>

// Generic Access service (0x1800), Device Name characteristic
// (0x2A00). Both are 16-bit Bluetooth SIG-assigned UUIDs, present on
// essentially every BLE peripheral, and Device Name is very commonly
// left world-readable by peripheral firmware, it is not treated as
// sensitive by the spec or by common practice. This is exactly what
// a phone reads to show a device's name during ordinary pairing.
#define GAP_SERVICE_UUID        0x1800
#define DEVICE_NAME_CHAR_UUID   0x2A00
#define GATTC_APP_ID            0
#define GATT_ATTEMPT_TIMEOUT_MS 5000

typedef enum {
    GATT_STATE_IDLE,
    GATT_STATE_CONNECTING,
    GATT_STATE_SEARCHING,
    GATT_STATE_READING
} GattResolveState;

typedef struct {
    uint8_t addr[6];
    esp_ble_addr_type_t addr_type;
} PendingGattTarget;

static esp_gatt_if_t gattcIf = ESP_GATT_IF_NONE;
static bool gattcRegistered = false;
static GattResolveState state = GATT_STATE_IDLE;
static std::deque<PendingGattTarget> pendingQueue;
static std::set<uint64_t> attempted;
static uint8_t currentAddr[6];
static uint16_t currentConnId = 0;
static uint16_t serviceStartHandle = 0;
static uint16_t serviceEndHandle = 0;
static uint32_t attemptDeadline = 0;

static uint64_t macKey(const uint8_t addr[6]) {
    uint64_t key = 0;
    memcpy(&key, addr, 6);
    return key;
}

static void logCurrentAddr(const char *msg) {
    Serial.printf("[BLE-GATT] %s  mac=%02X:%02X:%02X:%02X:%02X:%02X\n", msg,
        currentAddr[0], currentAddr[1], currentAddr[2], currentAddr[3], currentAddr[4], currentAddr[5]);
}

// Closes a connection that belongs to an attempt this module has
// already abandoned (via timeout or ble_gatt_name_reset()) by the
// time its event arrived. Deliberately closes using the event's own
// conn_id, not currentConnId, currentConnId may already have been
// reassigned to a newer, legitimate attempt by the time a stale
// event surfaces, closing via that would tear down the wrong
// connection instead of the stray one this event actually belongs
// to.
static void closeStrayConnection(esp_gatt_if_t gattc_if, uint16_t conn_id, const char *reason) {
    Serial.printf("[BLE-GATT] stale event ignored (%s), closing stray conn_id=%u\n", reason, conn_id);
    esp_ble_gattc_close(gattc_if, conn_id);
}

// close_conn: false when the stack has already torn the connection
// down itself (we're reacting to ESP_GATTC_DISCONNECT_EVT), true
// when we are the ones ending it (a read succeeded/failed, or a
// search/characteristic step failed and we are giving up on this
// target voluntarily).
static void finishAttempt(bool close_conn) {
    if (close_conn && state != GATT_STATE_IDLE) {
        esp_ble_gattc_close(gattcIf, currentConnId);
    }
    state = GATT_STATE_IDLE;
    serviceStartHandle = 0;
    serviceEndHandle = 0;
}

static void startNext() {
    if (state != GATT_STATE_IDLE || !gattcRegistered) return;
    if (pendingQueue.empty()) return;

    PendingGattTarget target = pendingQueue.front();
    pendingQueue.pop_front();
    memcpy(currentAddr, target.addr, 6);

    esp_err_t err = esp_ble_gattc_open(gattcIf, target.addr, target.addr_type, true);
    if (err != ESP_OK) {
        logCurrentAddr("open() call failed");
        Serial.printf("[BLE-GATT] open() error: %s\n", esp_err_to_name(err));
        // Stays IDLE; startNext() is called again on the next tick
        // and will move on to whatever is next in the queue.
        return;
    }

    state = GATT_STATE_CONNECTING;
    attemptDeadline = millis() + GATT_ATTEMPT_TIMEOUT_MS;
    logCurrentAddr("connecting");
}

static void gattc_cb(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if, esp_ble_gattc_cb_param_t *param) {
    if (event == ESP_GATTC_REG_EVT) {
        if (param->reg.app_id == GATTC_APP_ID && param->reg.status == ESP_GATT_OK) {
            gattcIf = gattc_if;
            gattcRegistered = true;
            Serial.println("[BLE-GATT] client registered");
        } else {
            Serial.printf("[BLE-GATT] client registration failed, status=%d\n", param->reg.status);
        }
        return;
    }

    // Ignore events for a gattc_if that isn't ours; shouldn't occur
    // with only one app registered, guarded anyway.
    if (gattc_if != gattcIf) return;

    switch (event) {
        case ESP_GATTC_CONNECT_EVT: {
            // The one event that legitimately arrives while state is
            // still CONNECTING, this is what assigns the real
            // conn_id in the first place. If state is anything else,
            // we already gave up on this target (timeout or
            // ble_gatt_name_reset() fired first), and the stack is
            // only now telling us the connection went through
            // anyway, exactly what happened to 52:02:C6:D8:5F:E1 in
            // this session's log. Close it immediately by its own
            // conn_id rather than letting it linger untracked.
            if (state != GATT_STATE_CONNECTING) {
                closeStrayConnection(gattc_if, param->connect.conn_id, "CONNECT_EVT after abandonment");
                break;
            }
            currentConnId = param->connect.conn_id;
            break;  // real progress is judged from OPEN_EVT's status, not this event
        }

        case ESP_GATTC_OPEN_EVT: {
            if (state != GATT_STATE_CONNECTING || param->open.conn_id != currentConnId) {
                closeStrayConnection(gattc_if, param->open.conn_id, "OPEN_EVT after abandonment");
                break;
            }
            if (param->open.status != ESP_GATT_OK) {
                logCurrentAddr("open event failed");
                Serial.printf("[BLE-GATT] open status=%d\n", param->open.status);
                finishAttempt(true);
                break;
            }
            state = GATT_STATE_SEARCHING;
            esp_bt_uuid_t gapSvcUuid = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = GAP_SERVICE_UUID } };
            esp_ble_gattc_search_service(gattc_if, currentConnId, &gapSvcUuid);
            break;
        }

        case ESP_GATTC_SEARCH_RES_EVT: {
            if (state != GATT_STATE_SEARCHING || param->search_res.conn_id != currentConnId) {
                // No conn_id-owning connection to close here, this
                // event only carries search progress, not a
                // connection to tear down; safe to just ignore.
                break;
            }
            if (param->search_res.srvc_id.uuid.len == ESP_UUID_LEN_16 &&
                param->search_res.srvc_id.uuid.uuid.uuid16 == GAP_SERVICE_UUID) {
                serviceStartHandle = param->search_res.start_handle;
                serviceEndHandle = param->search_res.end_handle;
            }
            break;
        }

        case ESP_GATTC_SEARCH_CMPL_EVT: {
            if (state != GATT_STATE_SEARCHING || param->search_cmpl.conn_id != currentConnId) {
                closeStrayConnection(gattc_if, param->search_cmpl.conn_id, "SEARCH_CMPL_EVT after abandonment");
                break;
            }
            if (param->search_cmpl.status != ESP_GATT_OK || serviceStartHandle == 0) {
                logCurrentAddr("GAP service not found or search failed");
                finishAttempt(true);
                break;
            }

            uint16_t count = 0;
            esp_gatt_status_t st = esp_ble_gattc_get_attr_count(gattc_if, currentConnId,
                ESP_GATT_DB_CHARACTERISTIC, serviceStartHandle, serviceEndHandle, 0, &count);
            if (st != ESP_GATT_OK || count == 0) {
                logCurrentAddr("Device Name characteristic count query failed");
                finishAttempt(true);
                break;
            }

            esp_gattc_char_elem_t charResult;
            uint16_t resultCount = 1;
            esp_bt_uuid_t nameCharUuid = { .len = ESP_UUID_LEN_16, .uuid = { .uuid16 = DEVICE_NAME_CHAR_UUID } };
            st = esp_ble_gattc_get_char_by_uuid(gattc_if, currentConnId,
                serviceStartHandle, serviceEndHandle, nameCharUuid, &charResult, &resultCount);
            if (st != ESP_GATT_OK || resultCount == 0) {
                logCurrentAddr("Device Name characteristic not present on this device");
                finishAttempt(true);
                break;
            }

            state = GATT_STATE_READING;
            esp_ble_gattc_read_char(gattc_if, currentConnId, charResult.char_handle, ESP_GATT_AUTH_REQ_NONE);
            break;
        }

        case ESP_GATTC_READ_CHAR_EVT: {
            // The guard that matters most: without this check, a
            // read that completes after ble_gatt_name_reset() has
            // already run (BLE mode deactivated mid-attempt, exactly
            // what happened to 48:DF:0E:03:0F:E4 in this session's
            // log) would still patch a name onto the dashboard using
            // whatever currentAddr happens to hold at that moment,
            // which may no longer correspond to this read at all.
            // Losing an occasional late resolution here is the
            // correct tradeoff against silently mislabeling one.
            if (state != GATT_STATE_READING || param->read.conn_id != currentConnId) {
                closeStrayConnection(gattc_if, param->read.conn_id, "READ_CHAR_EVT after abandonment");
                break;
            }
            if (param->read.status == ESP_GATT_OK && param->read.value_len > 0) {
                char name[32];
                size_t len = param->read.value_len < sizeof(name) - 1 ? param->read.value_len : sizeof(name) - 1;
                memcpy(name, param->read.value, len);
                name[len] = '\0';
                dashboard_notify_bt_device_name_update(currentAddr, name);
                Serial.printf("[BLE-GATT] NAME RESOLVED  mac=%02X:%02X:%02X:%02X:%02X:%02X  name=%s\n",
                    currentAddr[0], currentAddr[1], currentAddr[2], currentAddr[3], currentAddr[4], currentAddr[5], name);
            } else {
                logCurrentAddr("Device Name read failed");
                Serial.printf("[BLE-GATT] read status=%d\n", param->read.status);
            }
            finishAttempt(true);
            break;
        }

        case ESP_GATTC_DISCONNECT_EVT: {
            // Covers the peer disconnecting, a link-layer connect
            // failure surfacing here instead of at OPEN_EVT, or a
            // non-connectable target that never really connects.
            // Only log/act if this is genuinely the attempt we're
            // still tracking; a stray/abandoned connection's own
            // disconnect needs no further action here, it's already
            // being (or has already been) closed by whichever guard
            // above caught it.
            if (state != GATT_STATE_IDLE && param->disconnect.conn_id == currentConnId) {
                logCurrentAddr("disconnected before attempt completed");
                finishAttempt(false);  // already torn down, don't close again
            }
            break;
        }

        default:
            break;
    }
}

void ble_gatt_name_init() {
    esp_ble_gattc_register_callback(gattc_cb);
    esp_ble_gattc_app_register(GATTC_APP_ID);
}

void ble_gatt_name_request(const uint8_t addr[6], esp_ble_addr_type_t addr_type) {
    uint64_t key = macKey(addr);
    if (attempted.find(key) != attempted.end()) return;
    attempted.insert(key);

    PendingGattTarget target;
    memcpy(target.addr, addr, 6);
    target.addr_type = addr_type;
    pendingQueue.push_back(target);
}

void ble_gatt_name_tick() {
    if (state != GATT_STATE_IDLE && millis() > attemptDeadline) {
        logCurrentAddr("attempt timed out");
        finishAttempt(true);
    }
    startNext();
}

void ble_gatt_name_reset() {
    pendingQueue.clear();
    attempted.clear();
    if (state != GATT_STATE_IDLE) {
        esp_ble_gattc_close(gattcIf, currentConnId);
        state = GATT_STATE_IDLE;
    }
}