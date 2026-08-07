#include "dashboard_server.h"
#include "esp_http_server.h"
#include "esp_littlefs.h"
#include "rf_calibration.h"
#include <cstring>
#include <cstdio>

// Bounded history caps. Overwrite-oldest ring buffers, not
// unbounded logs, see the WARNING recorded in this session's
// design discussion: an ESP32 has roughly 300KB of usable heap once
// WiFi and Bluetooth stacks are running, and an ever-growing log
// across a multi-hour exam session is a slow memory leak by design.
#define DASHBOARD_MAX_WIFI_SIGS      50
#define DASHBOARD_MAX_WIFI_HOTSPOTS  50
#define DASHBOARD_MAX_BT_DEVICES     50
#define DASHBOARD_MAX_CELL_EVENTS    100
#define DASHBOARD_MAX_WS_CLIENTS     4
#define DASHBOARD_BT_NAME_LEN        32

typedef struct {
    uint8_t addr[6];
    int8_t rssi;
    uint32_t timestamp_ms;
} DashAddrEntry;

typedef struct {
    uint8_t addr[6];
    int8_t rssi;
    char ssid[33];
    bool ssid_present;
    uint32_t timestamp_ms;
} DashHotspotEntry;

typedef struct {
    uint8_t addr[6];
    int8_t rssi;
    char name[DASHBOARD_BT_NAME_LEN];
    char addr_class[16];
    char name_mechanism[16];
    uint32_t timestamp_ms;
} DashBtEntry;

typedef struct {
    uint32_t timestamp_ms;
    uint32_t peak_mv;
    uint8_t  strength_pct;
} DashCellEntry;

typedef struct {
    bool wifi_active;
    bool bt_active;
    uint32_t wifi_sig_count;
    uint32_t wifi_hotspot_count;
    uint32_t bt_count;

    DashAddrEntry wifi_sigs[DASHBOARD_MAX_WIFI_SIGS];
    uint32_t wifi_sigs_head;
    uint32_t wifi_sigs_len;

    DashHotspotEntry wifi_hotspots[DASHBOARD_MAX_WIFI_HOTSPOTS];
    uint32_t wifi_hotspots_head;
    uint32_t wifi_hotspots_len;

    DashBtEntry bt_devices[DASHBOARD_MAX_BT_DEVICES];
    uint32_t bt_devices_head;
    uint32_t bt_devices_len;

    DashCellEntry cell_events[DASHBOARD_MAX_CELL_EVENTS];
    uint32_t cell_events_head;
    uint32_t cell_events_len;
} DashboardState;

static DashboardState state;
static SemaphoreHandle_t stateMutex;

static httpd_handle_t server = NULL;
static int wsClients[DASHBOARD_MAX_WS_CLIENTS];
static SemaphoreHandle_t clientsMutex;

// ---- small helpers ----

static void macToStr(const uint8_t addr[6], char *out) {
    snprintf(out, 18, "%02X:%02X:%02X:%02X:%02X:%02X",
        addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);
}

// BLE device names are attacker-controlled input reaching a browser.
// A nearby device could advertise a name containing quotes or
// backslashes specifically to break out of a naive JSON string, or
// (client-side) to inject markup if the frontend ever rendered it
// via innerHTML instead of textContent. Escaped here on the way
// into JSON; the frontend written alongside this must also use
// textContent, never innerHTML, for this field. Both sides matter,
// fixing only one is not sufficient.
static String jsonEscape(const char *s) {
    String out;
    for (const char *p = s; *p; p++) {
        char c = *p;
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if ((uint8_t)c >= 0x20) { out += c; }
        // control characters below 0x20 dropped, not escaped
    }
    return out;
}

// ---- ring buffer pushes (one per type, deliberately not generic,
// see design note in the session's Planner discussion: kept
// concrete rather than templated for readability in a project that
// will be defended at viva) ----

static void pushWifiSig(const uint8_t addr[6], int8_t rssi) {
    uint32_t idx = (state.wifi_sigs_head + state.wifi_sigs_len) % DASHBOARD_MAX_WIFI_SIGS;
    if (state.wifi_sigs_len < DASHBOARD_MAX_WIFI_SIGS) {
        state.wifi_sigs_len++;
    } else {
        state.wifi_sigs_head = (state.wifi_sigs_head + 1) % DASHBOARD_MAX_WIFI_SIGS;
    }
    memcpy(state.wifi_sigs[idx].addr, addr, 6);
    state.wifi_sigs[idx].rssi = rssi;
    state.wifi_sigs[idx].timestamp_ms = millis();
}

static void pushWifiHotspot(const uint8_t addr[6], int8_t rssi, const char *ssid) {
    uint32_t idx = (state.wifi_hotspots_head + state.wifi_hotspots_len) % DASHBOARD_MAX_WIFI_HOTSPOTS;
    if (state.wifi_hotspots_len < DASHBOARD_MAX_WIFI_HOTSPOTS) {
        state.wifi_hotspots_len++;
    } else {
        state.wifi_hotspots_head = (state.wifi_hotspots_head + 1) % DASHBOARD_MAX_WIFI_HOTSPOTS;
    }
    memcpy(state.wifi_hotspots[idx].addr, addr, 6);
    state.wifi_hotspots[idx].rssi = rssi;
    if (ssid && ssid[0] != '\0') {
        strncpy(state.wifi_hotspots[idx].ssid, ssid, sizeof(state.wifi_hotspots[idx].ssid) - 1);
        state.wifi_hotspots[idx].ssid[sizeof(state.wifi_hotspots[idx].ssid) - 1] = '\0';
        state.wifi_hotspots[idx].ssid_present = true;
    } else {
        state.wifi_hotspots[idx].ssid[0] = '\0';
        state.wifi_hotspots[idx].ssid_present = false;
    }
    state.wifi_hotspots[idx].timestamp_ms = millis();
}

static void pushBtDevice(const uint8_t addr[6], int8_t rssi, const char *name, const char *addr_class, const char *name_mechanism) {
    uint32_t idx = (state.bt_devices_head + state.bt_devices_len) % DASHBOARD_MAX_BT_DEVICES;
    if (state.bt_devices_len < DASHBOARD_MAX_BT_DEVICES) {
        state.bt_devices_len++;
    } else {
        state.bt_devices_head = (state.bt_devices_head + 1) % DASHBOARD_MAX_BT_DEVICES;
    }
    memcpy(state.bt_devices[idx].addr, addr, 6);
    state.bt_devices[idx].rssi = rssi;
    strncpy(state.bt_devices[idx].name, name ? name : "", DASHBOARD_BT_NAME_LEN - 1);
    state.bt_devices[idx].name[DASHBOARD_BT_NAME_LEN - 1] = '\0';
    strncpy(state.bt_devices[idx].addr_class, addr_class ? addr_class : "reserved", sizeof(state.bt_devices[idx].addr_class) - 1);
    state.bt_devices[idx].addr_class[sizeof(state.bt_devices[idx].addr_class) - 1] = '\0';
    strncpy(state.bt_devices[idx].name_mechanism, name_mechanism ? name_mechanism : "classic", sizeof(state.bt_devices[idx].name_mechanism) - 1);
    state.bt_devices[idx].name_mechanism[sizeof(state.bt_devices[idx].name_mechanism) - 1] = '\0';
    state.bt_devices[idx].timestamp_ms = millis();
}

static void pushCellEvent(uint32_t timestamp_ms, uint32_t peak_mv) {
    uint32_t idx = (state.cell_events_head + state.cell_events_len) % DASHBOARD_MAX_CELL_EVENTS;
    if (state.cell_events_len < DASHBOARD_MAX_CELL_EVENTS) {
        state.cell_events_len++;
    } else {
        state.cell_events_head = (state.cell_events_head + 1) % DASHBOARD_MAX_CELL_EVENTS;
    }
    state.cell_events[idx].timestamp_ms = timestamp_ms;
    state.cell_events[idx].peak_mv = peak_mv;
    state.cell_events[idx].strength_pct = rf_strength_percent(peak_mv);
}

// ---- async WS send (required pattern: esp_http_server frames may
// only be sent from the server's own task context, so other tasks
// queue work rather than calling httpd_ws_send_frame_async
// directly) ----

typedef struct {
    httpd_handle_t hd;
    int fd;
    char *payload;   // heap-allocated, freed here after send
    size_t len;
} AsyncSendArg;

static void removeClientFd(int fd) {
    xSemaphoreTake(clientsMutex, portMAX_DELAY);
    for (int i = 0; i < DASHBOARD_MAX_WS_CLIENTS; i++) {
        if (wsClients[i] == fd) wsClients[i] = -1;
    }
    xSemaphoreGive(clientsMutex);
}

void dashboard_clear_ws_clients() {
    // Called right before the WiFi radio goes down for Cellular
    // mode. Every currently tracked fd is about to become invalid
    // regardless, deliberately not attempting to close() them
    // through the socket layer here, that layer is what's about to
    // be torn down by esp_wifi_stop() immediately after this call,
    // just drop the bookkeeping so nothing tries to send to them in
    // the meantime.
    xSemaphoreTake(clientsMutex, portMAX_DELAY);
    for (int i = 0; i < DASHBOARD_MAX_WS_CLIENTS; i++) {
        wsClients[i] = -1;
    }
    xSemaphoreGive(clientsMutex);
}

static void wsAsyncSend(void *arg) {
    AsyncSendArg *a = (AsyncSendArg *)arg;
    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.type = HTTPD_WS_TYPE_TEXT;
    pkt.payload = (uint8_t *)a->payload;
    pkt.len = a->len;

    esp_err_t err = httpd_ws_send_frame_async(a->hd, a->fd, &pkt);
    if (err != ESP_OK) {
        // Client almost certainly disconnected (e.g. during a WiFi
        // channel-hop induced drop, see item 2). Stop tracking it
        // rather than repeatedly failing to send to a dead socket.
        removeClientFd(a->fd);
    }
    free(a->payload);
    free(a);
}

static void broadcastJson(const String &json) {
    xSemaphoreTake(clientsMutex, portMAX_DELAY);
    for (int i = 0; i < DASHBOARD_MAX_WS_CLIENTS; i++) {
        int fd = wsClients[i];
        if (fd < 0) continue;

        AsyncSendArg *arg = (AsyncSendArg *)malloc(sizeof(AsyncSendArg));
        if (!arg) {
            Serial.println("[Dashboard] broadcastJson: malloc(AsyncSendArg) failed, heap pressure, dropping this client for this message");
            continue;
        }
        arg->hd = server;
        arg->fd = fd;
        arg->len = json.length();
        arg->payload = (char *)malloc(arg->len + 1);
        if (!arg->payload) {
            Serial.printf("[Dashboard] broadcastJson: malloc(%u) for payload failed, heap pressure, dropping this client for this message\n", (unsigned)(arg->len + 1));
            free(arg);
            continue;
        }
        memcpy(arg->payload, json.c_str(), arg->len + 1);

        if (httpd_queue_work(server, wsAsyncSend, arg) != ESP_OK) {
            Serial.printf("[Dashboard] broadcastJson: httpd_queue_work failed for fd=%d, message dropped\n", fd);
            free(arg->payload);
            free(arg);
        }
    }
    xSemaphoreGive(clientsMutex);
}

// ---- snapshot builder, sent once to a client on connect so it can
// resync recent history rather than starting from a blank screen ----

static String buildSnapshotJson() {
    String s = "{\"type\":\"snapshot\",";
    s += "\"wifi_active\":"; s += (state.wifi_active ? "true" : "false"); s += ",";
    s += "\"bt_active\":"; s += (state.bt_active ? "true" : "false"); s += ",";
    s += "\"wifi_sig_count\":"; s += state.wifi_sig_count; s += ",";
    s += "\"wifi_hotspot_count\":"; s += state.wifi_hotspot_count; s += ",";
    s += "\"bt_count\":"; s += state.bt_count; s += ",";

    char macbuf[18];

    s += "\"wifi_sigs\":[";
    for (uint32_t i = 0; i < state.wifi_sigs_len; i++) {
        uint32_t idx = (state.wifi_sigs_head + i) % DASHBOARD_MAX_WIFI_SIGS;
        macToStr(state.wifi_sigs[idx].addr, macbuf);
        if (i > 0) s += ",";
        s += "{\"mac\":\""; s += macbuf; s += "\",\"rssi\":"; s += state.wifi_sigs[idx].rssi;
        s += ",\"ts\":"; s += state.wifi_sigs[idx].timestamp_ms; s += "}";
    }
    s += "],";

    s += "\"wifi_hotspots\":[";
    for (uint32_t i = 0; i < state.wifi_hotspots_len; i++) {
        uint32_t idx = (state.wifi_hotspots_head + i) % DASHBOARD_MAX_WIFI_HOTSPOTS;
        macToStr(state.wifi_hotspots[idx].addr, macbuf);
        if (i > 0) s += ",";
        s += "{\"bssid\":\""; s += macbuf; s += "\",\"rssi\":"; s += state.wifi_hotspots[idx].rssi;
        s += ",\"ssid\":\""; s += jsonEscape(state.wifi_hotspots[idx].ssid); s += "\"";
        s += ",\"ts\":"; s += state.wifi_hotspots[idx].timestamp_ms; s += "}";
    }
    s += "],";

    s += "\"bt_devices\":[";
    for (uint32_t i = 0; i < state.bt_devices_len; i++) {
        uint32_t idx = (state.bt_devices_head + i) % DASHBOARD_MAX_BT_DEVICES;
        macToStr(state.bt_devices[idx].addr, macbuf);
        if (i > 0) s += ",";
        s += "{\"mac\":\""; s += macbuf; s += "\",\"rssi\":"; s += state.bt_devices[idx].rssi;
        s += ",\"name\":\""; s += jsonEscape(state.bt_devices[idx].name); s += "\"";
        s += ",\"addr_class\":\""; s += state.bt_devices[idx].addr_class; s += "\"";
        s += ",\"name_mechanism\":\""; s += state.bt_devices[idx].name_mechanism; s += "\"";
        s += ",\"ts\":"; s += state.bt_devices[idx].timestamp_ms; s += "}";
    }
    s += "],";

    s += "\"cell_events\":[";
    for (uint32_t i = 0; i < state.cell_events_len; i++) {
        uint32_t idx = (state.cell_events_head + i) % DASHBOARD_MAX_CELL_EVENTS;
        if (i > 0) s += ",";
        s += "{\"ts\":"; s += state.cell_events[idx].timestamp_ms;
        s += ",\"peak_mv\":"; s += state.cell_events[idx].peak_mv;
        s += ",\"strength_pct\":"; s += state.cell_events[idx].strength_pct; s += "}";
    }
    s += "]}";

    return s;
}

// ---- public broadcast API ----

void dashboard_notify_wifi_signature(const uint8_t addr[6], int8_t rssi, uint32_t total) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    pushWifiSig(addr, rssi);
    state.wifi_sig_count = total;
    xSemaphoreGive(stateMutex);

    char macbuf[18];
    macToStr(addr, macbuf);
    String s = "{\"type\":\"wifi_signature\",\"mac\":\""; s += macbuf;
    s += "\",\"rssi\":"; s += rssi; s += ",\"total\":"; s += total;
    s += ",\"ts\":"; s += millis(); s += "}";
    broadcastJson(s);
}

void dashboard_notify_wifi_hotspot(const uint8_t addr[6], int8_t rssi, const char *ssid, uint32_t total) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    pushWifiHotspot(addr, rssi, ssid);
    state.wifi_hotspot_count = total;
    xSemaphoreGive(stateMutex);

    char macbuf[18];
    macToStr(addr, macbuf);
    String s = "{\"type\":\"wifi_hotspot\",\"bssid\":\""; s += macbuf;
    s += "\",\"rssi\":"; s += rssi;
    s += ",\"ssid\":\""; s += jsonEscape(ssid ? ssid : ""); s += "\"";
    s += ",\"total\":"; s += total;
    s += ",\"ts\":"; s += millis(); s += "}";
    broadcastJson(s);
}

void dashboard_notify_bt_device(const uint8_t addr[6], int8_t rssi, const char *name, const char *addr_class, const char *name_mechanism, uint32_t total) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    pushBtDevice(addr, rssi, name, addr_class, name_mechanism);
    state.bt_count = total;
    xSemaphoreGive(stateMutex);

    char macbuf[18];
    macToStr(addr, macbuf);
    String s = "{\"type\":\"bt_device\",\"mac\":\""; s += macbuf;
    s += "\",\"rssi\":"; s += rssi;
    s += ",\"name\":\""; s += jsonEscape(name ? name : ""); s += "\"";
    s += ",\"addr_class\":\""; s += (addr_class ? addr_class : "reserved"); s += "\"";
    s += ",\"name_mechanism\":\""; s += (name_mechanism ? name_mechanism : "classic"); s += "\"";
    s += ",\"total\":"; s += total; s += ",\"ts\":"; s += millis(); s += "}";
    broadcastJson(s);
}

void dashboard_notify_bt_device_name_update(const uint8_t addr[6], const char *name) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    for (uint32_t i = 0; i < state.bt_devices_len; i++) {
        uint32_t idx = (state.bt_devices_head + i) % DASHBOARD_MAX_BT_DEVICES;
        if (memcmp(state.bt_devices[idx].addr, addr, 6) == 0) {
            strncpy(state.bt_devices[idx].name, name ? name : "", DASHBOARD_BT_NAME_LEN - 1);
            state.bt_devices[idx].name[DASHBOARD_BT_NAME_LEN - 1] = '\0';
            break;
        }
    }
    xSemaphoreGive(stateMutex);

    char macbuf[18];
    macToStr(addr, macbuf);
    String s = "{\"type\":\"bt_device_name_update\",\"mac\":\""; s += macbuf;
    s += "\",\"name\":\""; s += jsonEscape(name ? name : ""); s += "\"}";
    broadcastJson(s);
}

void dashboard_notify_cellular_event(uint32_t timestamp_ms, uint32_t peak_mv) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    pushCellEvent(timestamp_ms, peak_mv);
    xSemaphoreGive(stateMutex);

    String s = "{\"type\":\"cellular_event\",\"ts\":"; s += timestamp_ms;
    s += ",\"peak_mv\":"; s += peak_mv;
    s += ",\"strength_pct\":"; s += rf_strength_percent(peak_mv); s += "}";
    broadcastJson(s);
}

void dashboard_set_wifi_active(bool active) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    state.wifi_active = active;
    if (active) { state.wifi_sig_count = 0; state.wifi_hotspot_count = 0; }
    xSemaphoreGive(stateMutex);

    String s = "{\"type\":\"mode_active\",\"mode\":\"wifi\",\"active\":";
    s += (active ? "true" : "false"); s += "}";
    broadcastJson(s);
}

void dashboard_set_bt_active(bool active) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    state.bt_active = active;
    if (active) { state.bt_count = 0; }
    xSemaphoreGive(stateMutex);

    String s = "{\"type\":\"mode_active\",\"mode\":\"bluetooth\",\"active\":";
    s += (active ? "true" : "false"); s += "}";
    broadcastJson(s);
}

void dashboard_notify_wifi_sweep_warning() {
    broadcastJson("{\"type\":\"sweep_warning\"}");
}

void dashboard_notify_cellular_mode_entering() {
    broadcastJson("{\"type\":\"cellular_mode_entering\"}");
}

// ---- HTTP handlers ----

static const char *contentTypeFor(const char *path) {
    size_t len = strlen(path);
    if (len >= 5 && strcmp(path + len - 5, ".html") == 0) return "text/html";
    if (len >= 4 && strcmp(path + len - 4, ".css")  == 0) return "text/css";
    if (len >= 3 && strcmp(path + len - 3, ".js")   == 0) return "application/javascript";
    return "text/plain";
}

static esp_err_t staticGetHandler(httpd_req_t *req) {
    char path[96] = "/littlefs";
    if (strcmp(req->uri, "/") == 0) {
        strncat(path, "/index.html", sizeof(path) - strlen(path) - 1);
    } else {
        strncat(path, req->uri, sizeof(path) - strlen(path) - 1);
    }

    FILE *f = fopen(path, "r");
    if (!f) {
        httpd_resp_send_404(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, contentTypeFor(path));

    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            fclose(f);
            httpd_resp_send_chunk(req, NULL, 0);
            return ESP_FAIL;
        }
    }
    fclose(f);
    httpd_resp_send_chunk(req, NULL, 0);
    return ESP_OK;
}

static esp_err_t wsHandler(httpd_req_t *req) {
    if (req->method == HTTP_GET) {
        // Handshake completing, register this client.
        int fd = httpd_req_to_sockfd(req);
        if (fd < 0) {
            // httpd_req_to_sockfd() can fail; a negative fd stored
            // into wsClients[] would look identical to an empty
            // slot (`wsClients[i] < 0`) to every other part of this
            // file, silently corrupting client tracking rather than
            // failing loudly. Bail out rather than store it.
            Serial.println("[Dashboard] WS handshake: httpd_req_to_sockfd failed, not registering client");
            return ESP_FAIL;
        }

        bool registered = false;
        xSemaphoreTake(clientsMutex, portMAX_DELAY);
        for (int i = 0; i < DASHBOARD_MAX_WS_CLIENTS; i++) {
            if (wsClients[i] < 0) { wsClients[i] = fd; registered = true; break; }
        }
        xSemaphoreGive(clientsMutex);
        if (!registered) {
            Serial.printf("[Dashboard] WS handshake: client table full (max %d), fd=%d not tracked, will not receive live updates\n",
                DASHBOARD_MAX_WS_CLIENTS, fd);
        }

        xSemaphoreTake(stateMutex, portMAX_DELAY);
        String snap = buildSnapshotJson();
        xSemaphoreGive(stateMutex);

        httpd_ws_frame_t pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.type = HTTPD_WS_TYPE_TEXT;
        pkt.payload = (uint8_t *)snap.c_str();
        pkt.len = snap.length();
        // This call previously had no error check at all: if it
        // failed, the client would see a clean-looking open
        // connection (the handshake itself already completed before
        // this handler even runs) that then received nothing,
        // exactly the "shows Connected, msgCount stays at 0" symptom
        // reported. Logging both the outcome and the payload size,
        // a snapshot built from four ring buffers at or near their
        // caps (50/50/50/100 entries) could plausibly be large
        // enough on this heap-constrained build to matter, that's a
        // real hypothesis worth ruling in or out from this line
        // rather than guessing further.
        esp_err_t sendErr = httpd_ws_send_frame(req, &pkt);
        if (sendErr != ESP_OK) {
            Serial.printf("[Dashboard] WS snapshot send FAILED  fd=%d  len=%u bytes  err=%s  free_heap=%u\n",
                fd, (unsigned)pkt.len, esp_err_to_name(sendErr), (unsigned)ESP.getFreeHeap());
        } else {
            Serial.printf("[Dashboard] WS snapshot sent  fd=%d  len=%u bytes  free_heap=%u\n",
                fd, (unsigned)pkt.len, (unsigned)ESP.getFreeHeap());
        }
        return ESP_OK;
    }

    // Incoming frames from the browser carry no application meaning
    // for this dashboard, but they still have to be fully drained,
    // or the connection desyncs. httpd_ws_recv_frame() with max_len
    // 0 only reads the frame header and reports pkt.len, it does
    // NOT consume the payload; that requires a second call with a
    // buffer sized to pkt.len (confirmed against Espressif's own
    // esp_http_server documentation and their wss_server example).
    // The previous version of this handler only made the first
    // call. Any frame with a nonzero-length payload, most commonly
    // the WebSocket Close frame the browser sends when app.js calls
    // ws.close() in its onerror handler, left that payload sitting
    // unread in the socket. The next frame parse then started
    // mid-payload instead of at a real frame header, which reliably
    // fails the client-frame masking check and cascades into a
    // repeating "WS frame is not properly masked" / recv-error loop
    // on that socket, this is a confirmed, previously-unflagged
    // defect, not a client or environment issue, and it recurs on
    // every client disconnect that sends a Close frame first, which
    // this project's burst-sweep design causes routinely.
    httpd_ws_frame_t pkt;
    memset(&pkt, 0, sizeof(pkt));
    esp_err_t ret = httpd_ws_recv_frame(req, &pkt, 0);
    if (ret != ESP_OK) {
        // Socket already gone; nothing left to drain.
        return ret;
    }

    if (pkt.len == 0) {
        // Zero-length frame (e.g. a Ping/Pong with no payload):
        // header already fully consumed by the call above, nothing
        // further to read.
        return ESP_OK;
    }

    uint8_t *buf = (uint8_t *)malloc(pkt.len + 1);
    if (!buf) {
        // Can't allocate a drain buffer; the payload stays unread
        // and the next parse on this socket will desync exactly as
        // described above, but this is a genuine allocation failure
        // under memory pressure, not something to silently pretend
        // succeeded.
        Serial.println("[Dashboard] WS drain malloc failed, incoming frame left unread");
        return ESP_ERR_NO_MEM;
    }
    pkt.payload = buf;
    ret = httpd_ws_recv_frame(req, &pkt, pkt.len);
    if (ret == ESP_OK && pkt.type == HTTPD_WS_TYPE_CLOSE) {
        // Expected and routine, not an error: the browser's own
        // clean-close handshake, triggered by app.js's onerror ->
        // ws.close(), or a normal tab close/navigation.
        Serial.println("[Dashboard] WS client sent Close frame");
    }
    free(buf);
    return ret;
}

void dashboard_server_init() {
    stateMutex = xSemaphoreCreateMutex();
    clientsMutex = xSemaphoreCreateMutex();
    memset(&state, 0, sizeof(state));
    for (int i = 0; i < DASHBOARD_MAX_WS_CLIENTS; i++) wsClients[i] = -1;

    // Partition label "spiffs" matches partitions.csv's Name column
    // for the LittleFS-formatted data partition (label is just an
    // identifier, it doesn't need to match the filesystem type, see
    // SESSION_3_ADDENDUM.md Section 5 for why the format itself is
    // LittleFS despite this label).
    esp_vfs_littlefs_conf_t conf = {};
    conf.base_path = "/littlefs";
    conf.partition_label = "spiffs";
    conf.format_if_mount_failed = false;
    conf.dont_mount = false;

    esp_err_t mountErr = esp_vfs_littlefs_register(&conf);
    if (mountErr != ESP_OK) {
        Serial.printf("[Dashboard] LittleFS mount failed: %s. Was the filesystem image uploaded via `pio run --target uploadfs`?\n",
            esp_err_to_name(mountErr));
        return;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.stack_size = 8192;

    if (httpd_start(&server, &config) != ESP_OK) {
        Serial.println("[Dashboard] httpd_start failed");
        return;
    }

    httpd_uri_t wsUri = { .uri = "/ws", .method = HTTP_GET, .handler = wsHandler, .user_ctx = NULL, .is_websocket = true };
    httpd_register_uri_handler(server, &wsUri);

    httpd_uri_t staticUri = { .uri = "/*", .method = HTTP_GET, .handler = staticGetHandler, .user_ctx = NULL };
    httpd_register_uri_handler(server, &staticUri);

    Serial.println("[Dashboard] server started");
}