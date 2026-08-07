(function () {
  "use strict";

  var MAX_DISPLAYED = 100;
  var MAX_WAVEFORM_POINTS = 100;
  var MAX_CONSOLE_LINES = 200;

  // ---- element refs ----
  var clockEl = document.getElementById("clock");
  var connStatusEl = document.getElementById("connStatus");
  var appContentEl = document.getElementById("appContent");
  var msgCountEl = document.getElementById("msgCount");
  var lastMsgAgoEl = document.getElementById("lastMsgAgo");
  var reconnectCountEl = document.getElementById("reconnectCount");

  var modeWifiEl = document.getElementById("modeWifi");
  var modeBluetoothEl = document.getElementById("modeBluetooth");
  var modeCellularEl = document.getElementById("modeCellular");

  var wifiSweepBadgeEl = document.getElementById("wifiSweepBadge");
  var wifiSigCountEl = document.getElementById("wifiSigCount");
  var wifiHotspotCountEl = document.getElementById("wifiHotspotCount");
  var wifiSigListEl = document.getElementById("wifiSigList");
  var wifiHotspotListEl = document.getElementById("wifiHotspotList");

  var btCountEl = document.getElementById("btCount");
  var btListEl = document.getElementById("btList");

  var cellStrengthEl = document.getElementById("cellStrength");
  var cellEventCountEl = document.getElementById("cellEventCount");
  var cellListEl = document.getElementById("cellList");
  var cellWaveformEl = document.getElementById("cellWaveform");
  var cellWaveformCtx = cellWaveformEl ? cellWaveformEl.getContext("2d") : null;

  var consoleLogEl = document.getElementById("consoleLog");

  var disconnectOverlayEl = document.getElementById("disconnectOverlay");
  var disconnectBoxEl = document.getElementById("disconnectBox");
  var disconnectTitleEl = document.getElementById("disconnectTitle");
  var disconnectMessageEl = document.getElementById("disconnectMessage");

  // ---- diagnostics state (this is the "built-in debugging tool"
  // part: every message bumps these, so if the count stops moving,
  // the problem is server-side delivery, not client-side rendering) ----
  var totalMessages = 0;
  var lastMessageAt = null;
  var reconnectAttempts = 0;

  function tickClock() {
    var now = new Date();
    var pad = function (n) { return String(n).padStart(2, "0"); };
    clockEl.textContent = pad(now.getHours()) + ":" + pad(now.getMinutes()) + ":" + pad(now.getSeconds());

    if (lastMessageAt === null) {
      lastMsgAgoEl.textContent = "\u2014";
    } else {
      var secs = Math.floor((Date.now() - lastMessageAt) / 1000);
      lastMsgAgoEl.textContent = secs < 1 ? "just now" : secs + "s ago";
    }
  }
  setInterval(tickClock, 1000);
  tickClock();

  // ---- console / activity feed: narrates the same structured
  // events already driving the panels above, in a terminal-log
  // style. Not a separate data source, deliberately, everything it
  // shows is something the panels already received. ----
  function consoleLine(tagClass, tagLabel, text) {
    var line = document.createElement("div");
    line.className = "line";
    var clockMark = document.createElement("span");
    clockMark.className = "clock-mark";
    var now = new Date();
    var pad = function (n) { return String(n).padStart(2, "0"); };
    clockMark.textContent = pad(now.getHours()) + ":" + pad(now.getMinutes()) + ":" + pad(now.getSeconds()) + " ";
    var tag = document.createElement("span");
    tag.className = tagClass;
    tag.textContent = "[" + tagLabel + "] ";
    var body = document.createElement("span");
    body.textContent = text;
    line.appendChild(clockMark);
    line.appendChild(tag);
    line.appendChild(body);
    consoleLogEl.insertBefore(line, consoleLogEl.firstChild);
    while (consoleLogEl.children.length > MAX_CONSOLE_LINES) {
      consoleLogEl.removeChild(consoleLogEl.lastChild);
    }
  }

  function setConnStatus(connected) {
    connStatusEl.textContent = connected ? "CONNECTED" : "RECONNECTING";
    connStatusEl.className = "conn-status " + (connected ? "connected" : "disconnected");
  }

  // ---- disconnect overlay, three variants ----
  var pendingReason = null; // "sweep" | "cellular" | null
  var pendingReasonTimeoutId = null;
  var SWEEP_FLAG_TIMEOUT_MS = 6000;

  function setPendingReason(reason, timeoutMs) {
    pendingReason = reason;
    if (pendingReasonTimeoutId !== null) clearTimeout(pendingReasonTimeoutId);
    if (timeoutMs) {
      pendingReasonTimeoutId = setTimeout(function () {
        pendingReason = null;
        pendingReasonTimeoutId = null;
      }, timeoutMs);
    }
  }

  function showDisconnectOverlay(reason) {
    disconnectBoxEl.classList.remove("sweeping", "cellular");
    if (reason === "sweep") {
      disconnectBoxEl.classList.add("sweeping");
      disconnectTitleEl.textContent = "Full channel sweep in progress";
      disconnectMessageEl.textContent =
        "The detector is briefly leaving this connection to complete a scheduled " +
        "full-channel scan. Expected, happens periodically while WiFi monitoring " +
        "is active. Reconnecting automatically in a few seconds.";
    } else if (reason === "cellular") {
      disconnectBoxEl.classList.add("cellular");
      disconnectTitleEl.textContent = "Cellular mode active";
      disconnectMessageEl.textContent =
        "The WiFi radio is intentionally switched off while Cellular mode runs, " +
        "to keep it from interfering with the RF reading. The dashboard stays " +
        "offline until the device is switched to another mode, then reconnects " +
        "automatically.";
    } else {
      disconnectTitleEl.textContent = "Connection lost";
      disconnectMessageEl.textContent =
        "The connection to the detector was lost. Reconnecting automatically...";
    }
    disconnectOverlayEl.classList.add("visible");
    appContentEl.classList.add("blurred");
  }

  function hideDisconnectOverlay() {
    disconnectOverlayEl.classList.remove("visible");
    appContentEl.classList.remove("blurred");
  }

  // ---- WiFi/BT list rendering helpers ----
  function makeEntryLi(macText, metaText) {
    var li = document.createElement("li");
    var macSpan = document.createElement("span");
    macSpan.className = "entry-mac";
    macSpan.textContent = macText;
    var metaSpan = document.createElement("span");
    metaSpan.className = "entry-meta";
    metaSpan.textContent = metaText;
    li.appendChild(macSpan);
    li.appendChild(metaSpan);
    return li;
  }

  function addrClassLabel(cls) {
    switch (cls) {
      case "public": return { text: "fixed address", css: "addr-stable" };
      case "static": return { text: "static (this session)", css: "addr-stable" };
      case "resolvable": return { text: "rotates (~15 min)", css: "addr-rotating" };
      case "nonresolvable": return { text: "rotates, unresolvable", css: "addr-rotating" };
      default: return { text: "address type unknown", css: "addr-rotating" };
    }
  }

  // Distinguishes "this device structurally cannot yield a name by
  // any mechanism this project has" from "no name yet, but one
  // might still resolve." A non-connectable BLE advertisement, the
  // common way a phone idling in someone's pocket broadcasts, is the
  // former: it refuses connections at the link layer, so the GATT
  // name read this project relies on can never reach it. Showing
  // both cases as an identical generic "(no name)" would erase that
  // distinction, which matters for reading the device list honestly.
  function nameMechanismText(mechanism) {
    switch (mechanism) {
      case "ble_idle": return "idle, not connectable";
      case "ble_connectable": return "connectable, resolving";
      default: return "no name";
    }
  }

  function makeBtEntryLi(mac, name, rssi, addrClass, nameMechanism) {
    var label = addrClassLabel(addrClass);
    var nameText = name ? name : "(" + nameMechanismText(nameMechanism) + ")";
    var li = makeEntryLi(mac, nameText + " \u00b7 rssi " + rssi);
    li.dataset.mac = mac;
    li.dataset.rssi = rssi;
    var tag = document.createElement("span");
    tag.className = "addr-class-tag " + label.css;
    tag.textContent = label.text;
    li.appendChild(tag);
    return li;
  }

  function updateBtDeviceName(mac, name) {
    var items = btListEl.querySelectorAll("li");
    for (var i = 0; i < items.length; i++) {
      if (items[i].dataset.mac === mac) {
        var metaSpan = items[i].querySelector(".entry-meta");
        if (metaSpan) metaSpan.textContent = (name || "(no name)") + " \u00b7 rssi " + items[i].dataset.rssi;
        break;
      }
    }
  }

  function prependCapped(listEl, li) {
    listEl.insertBefore(li, listEl.firstChild);
    while (listEl.children.length > MAX_DISPLAYED) listEl.removeChild(listEl.lastChild);
  }

  function clearList(listEl) {
    while (listEl.firstChild) listEl.removeChild(listEl.firstChild);
  }

  function msToClock(ms) {
    var totalSec = Math.floor(ms / 1000);
    var h = String(Math.floor(totalSec / 3600)).padStart(2, "0");
    var m = String(Math.floor((totalSec % 3600) / 60)).padStart(2, "0");
    var s = String(totalSec % 60).padStart(2, "0");
    return h + ":" + m + ":" + s;
  }

  function setModeChip(el, active) {
    if (active) el.classList.add("active"); else el.classList.remove("active");
  }

  // ---- cellular waveform: plotted by index, not real elapsed time,
  // this buffer can span a Cellular-mode session boundary (offline
  // the whole gap in between), a strict time axis would show a
  // large, meaningless blank stretch instead. ----
  var cellHistory = [];

  function drawWaveform() {
    if (!cellWaveformCtx) return;
    var w = cellWaveformEl.width, h = cellWaveformEl.height;
    cellWaveformCtx.clearRect(0, 0, w, h);
    cellWaveformCtx.strokeStyle = "rgba(74, 222, 128, 0.25)";
    cellWaveformCtx.lineWidth = 1;
    cellWaveformCtx.beginPath();
    cellWaveformCtx.moveTo(0, h / 2);
    cellWaveformCtx.lineTo(w, h / 2);
    cellWaveformCtx.stroke();

    if (cellHistory.length < 2) return;

    cellWaveformCtx.strokeStyle = "#22d3ee";
    cellWaveformCtx.lineWidth = 2;
    cellWaveformCtx.beginPath();
    var stepX = w / (MAX_WAVEFORM_POINTS - 1);
    var startIdx = MAX_WAVEFORM_POINTS - cellHistory.length;
    for (var i = 0; i < cellHistory.length; i++) {
      var x = (startIdx + i) * stepX;
      var y = h - (cellHistory[i].strength_pct / 100) * h;
      if (i === 0) cellWaveformCtx.moveTo(x, y); else cellWaveformCtx.lineTo(x, y);
    }
    cellWaveformCtx.stroke();
  }

  function pushWaveformPoint(strength_pct) {
    cellHistory.push({ strength_pct: strength_pct });
    while (cellHistory.length > MAX_WAVEFORM_POINTS) cellHistory.shift();
    drawWaveform();
  }

  // ---- snapshot ----
  function applySnapshot(msg) {
    wifiSigCountEl.textContent = msg.wifi_sig_count;
    wifiHotspotCountEl.textContent = msg.wifi_hotspot_count;
    btCountEl.textContent = msg.bt_count;
    setModeChip(modeWifiEl, msg.wifi_active);
    setModeChip(modeBluetoothEl, msg.bt_active);
    modeCellularEl.classList.remove("cellular-active");
    wifiSweepBadgeEl.classList.add("hidden");

    clearList(wifiSigListEl);
    msg.wifi_sigs.slice().reverse().forEach(function (e) {
      prependCapped(wifiSigListEl, makeEntryLi(e.mac, "rssi " + e.rssi));
    });

    clearList(wifiHotspotListEl);
    msg.wifi_hotspots.slice().reverse().forEach(function (e) {
      prependCapped(wifiHotspotListEl, makeEntryLi(e.bssid, (e.ssid || "(hidden)") + " \u00b7 rssi " + e.rssi));
    });

    clearList(btListEl);
    msg.bt_devices.slice().reverse().forEach(function (e) {
      prependCapped(btListEl, makeBtEntryLi(e.mac, e.name, e.rssi, e.addr_class, e.name_mechanism));
    });

    clearList(cellListEl);
    msg.cell_events.slice().reverse().forEach(function (e) {
      prependCapped(cellListEl, makeEntryLi(msToClock(e.ts), e.strength_pct + "% (" + e.peak_mv + " mV)"));
    });
    cellEventCountEl.textContent = msg.cell_events.length;
    if (msg.cell_events.length > 0) {
      cellStrengthEl.textContent = msg.cell_events[msg.cell_events.length - 1].strength_pct + "%";
    }
    cellHistory = msg.cell_events.slice(-MAX_WAVEFORM_POINTS).map(function (e) {
      return { strength_pct: e.strength_pct };
    });
    drawWaveform();

    consoleLine("tag-sys", "SYS", "Snapshot received: " + msg.wifi_sig_count + " WiFi sig, " +
      msg.bt_count + " BT, " + msg.cell_events.length + " cellular events on file");
  }

  function handleMessage(msg) {
    switch (msg.type) {
      case "snapshot":
        applySnapshot(msg);
        break;
      case "wifi_signature":
        wifiSigCountEl.textContent = msg.total;
        prependCapped(wifiSigListEl, makeEntryLi(msg.mac, "rssi " + msg.rssi));
        consoleLine("tag-wifi", "WIFI", "New signature " + msg.mac + " (rssi " + msg.rssi + ")");
        break;
      case "wifi_hotspot":
        wifiHotspotCountEl.textContent = msg.total;
        prependCapped(wifiHotspotListEl, makeEntryLi(msg.bssid, (msg.ssid || "(hidden)") + " \u00b7 rssi " + msg.rssi));
        consoleLine("tag-wifi", "WIFI", "New hotspot " + (msg.ssid || "(hidden)") + " " + msg.bssid);
        break;
      case "bt_device":
        btCountEl.textContent = msg.total;
        prependCapped(btListEl, makeBtEntryLi(msg.mac, msg.name, msg.rssi, msg.addr_class, msg.name_mechanism));
        consoleLine("tag-bt", "BT", "New device " + msg.mac + (msg.name ? " (" + msg.name + ")" : ""));
        break;
      case "bt_device_name_update":
        updateBtDeviceName(msg.mac, msg.name);
        consoleLine("tag-bt", "BT", "Name resolved for " + msg.mac + ": " + msg.name);
        break;
      case "cellular_event":
        prependCapped(cellListEl, makeEntryLi(msToClock(msg.ts), msg.strength_pct + "% (" + msg.peak_mv + " mV)"));
        pushWaveformPoint(msg.strength_pct);
        cellStrengthEl.textContent = msg.strength_pct + "%";
        cellEventCountEl.textContent = (parseInt(cellEventCountEl.textContent, 10) || 0) + 1;
        consoleLine("tag-cell", "RF", "Cellular event, " + msg.strength_pct + "% (" + msg.peak_mv + " mV)");
        break;
      case "mode_active":
        if (msg.mode === "wifi") setModeChip(modeWifiEl, msg.active);
        if (msg.mode === "bluetooth") setModeChip(modeBluetoothEl, msg.active);
        consoleLine("tag-sys", "SYS", msg.mode + (msg.active ? " activated" : " deactivated"));
        break;
      case "sweep_warning":
        setPendingReason("sweep", SWEEP_FLAG_TIMEOUT_MS);
        wifiSweepBadgeEl.classList.remove("hidden");
        consoleLine("tag-sys", "SYS", "Full channel sweep starting, brief disconnect expected");
        break;
      case "cellular_mode_entering":
        setPendingReason("cellular", null);
        modeCellularEl.classList.add("cellular-active");
        consoleLine("tag-sys", "SYS", "Entering Cellular mode, WiFi radio going down");
        break;
    }
  }

  // ---- WebSocket lifecycle ----
  var ws = null;
  var reconnectDelayMs = 1000;
  var MAX_RECONNECT_DELAY_MS = 10000;

  function connect() {
    var url = "ws://" + window.location.host + "/ws";
    ws = new WebSocket(url);

    ws.onopen = function () {
      setConnStatus(true);
      reconnectDelayMs = 1000;
      hideDisconnectOverlay();
      wifiSweepBadgeEl.classList.add("hidden");
      modeCellularEl.classList.remove("cellular-active");
      consoleLine("tag-sys", "SYS", "WebSocket connected");
    };

    ws.onmessage = function (evt) {
      totalMessages++;
      lastMessageAt = Date.now();
      msgCountEl.textContent = totalMessages;
      try {
        handleMessage(JSON.parse(evt.data));
      } catch (e) {
        consoleLine("tag-sys", "SYS", "Malformed frame ignored");
      }
    };

    ws.onclose = function () {
      setConnStatus(false);
      showDisconnectOverlay(pendingReason);
      consoleLine("tag-sys", "SYS", "WebSocket closed" + (pendingReason ? " (" + pendingReason + ")" : ""));
      scheduleReconnect();
    };

    ws.onerror = function () { ws.close(); };
  }

  function scheduleReconnect() {
    setTimeout(function () {
      reconnectAttempts++;
      reconnectCountEl.textContent = reconnectAttempts;
      connect();
      reconnectDelayMs = Math.min(reconnectDelayMs * 1.5, MAX_RECONNECT_DELAY_MS);
    }, reconnectDelayMs);
  }

  drawWaveform();
  connect();
})();