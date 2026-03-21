"use strict";

/* ═════════════ STATE ═════════════ */
const wt_S = {
  color: "#e8e4de",
  brightness: 80,
  fmt: 24,
  blink: true,
  scroll: false,
  pulse: false,
  transition: true,
  colonOn: true,
  logFilter: "ALL",
  logs: [],
  lastSeq: 0,
  wifi: null,
  display: null,
  displayMode: "time",
  displayValue: 1234,
  displayText: "HELO",
  conn: { state: "pend", lastSeen: 0 },
  timers: {},
  ws: null,
  wsConnected: false,
  wsRetryMs: 3000,
};

const wt_db = {};

/* ═════════════ DOM HELPERS ═════════════ */
const wt_$ = id => document.getElementById(id);

const wt_UI = {
  text(id, v) {
    const e = wt_$(id);
    if (e) e.textContent = v;
  },
  value(id, v) {
    const e = wt_$(id);
    if (e && document.activeElement !== e) e.value = v;
  },
  checked(id, v) {
    const e = wt_$(id);
    if (e && document.activeElement !== e) e.checked = !!v;
  }
};

/* ═════════════ UTILS ═════════════ */
const wt_clamp = (v, min, max) => Math.max(min, Math.min(max, v));

function wt_fmtBytes(b) {
  if (b == null) return "—";
  if (b < 1024) return b + "B";
  if (b < 1048576) return (b / 1024).toFixed(1) + "KB";
  return (b / 1048576).toFixed(2) + "MB";
}

function wt_fmtUptime(s) {
  if (!s && s !== 0) return "—";
  const d = Math.floor(s / 86400),
    h = Math.floor((s % 86400) / 3600),
    m = Math.floor((s % 3600) / 60),
    sc = s % 60;
  if (d > 0) return `${d}d ${h}h ${m}m`;
  if (h > 0) return `${h}h ${m}m ${sc}s`;
  return `${m}m ${sc}s`;
}

function wt_fmtTemperature(v) {
  return Number.isFinite(v) ? v.toFixed(1) + "°C" : "—";
}

function wt_debounce(key, fn, ms = 500) {
  clearTimeout(wt_db[key]);
  wt_db[key] = setTimeout(fn, ms);
}

/* ═════════════ RENDER LOOP ═════════════ */
let wt_renderQueued = false;
let wt_renderDirty = true;

function wt_requestRender() {
  wt_renderDirty = true;
  if (wt_renderQueued) return;

  wt_renderQueued = true;
  requestAnimationFrame(() => {
    wt_renderQueued = false;
    if (!wt_renderDirty) return;
    wt_renderDirty = false;

    wt_renderDisplay("vd-canvas");
    wt_renderDisplay("prev-canvas");
  });
}

/* ═════════════ DISPLAY ═════════════ */
function wt_displayText(state) {
  if (!state || !state.digits) return "--:--";
  const map = { 63:"0",6:"1",91:"2",79:"3",102:"4",109:"5",125:"6",7:"7",127:"8",111:"9",0:" " };
  const c = state.digits.map(m => map[m] ?? "?");
  return c[0] + c[1] + (state.colon ? ":" : " ") + c[2] + c[3];
}

function wt_renderDisplay(id) {
  const canvas = wt_$(id);
  if (!canvas) return;
  const ctx = canvas.getContext("2d");

  const W = canvas.width, H = canvas.height;
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = "#050508";
  ctx.fillRect(0, 0, W, H);

  const txt = wt_displayText(wt_S.display);

  ctx.fillStyle = wt_S.color;
  ctx.font = `${Math.floor(H*0.6)}px monospace`;
  ctx.textAlign = "center";
  ctx.textBaseline = "middle";
  ctx.fillText(txt, W/2, H/2);
}

/* ═════════════ CONNECTION ═════════════ */
function wt_markConnectionSeen() {
  wt_S.conn.lastSeen = Date.now();
  wt_S.conn.state = "up";
}

function wt_updateConnectionBadge() {
  const age = Date.now() - (wt_S.conn.lastSeen || 0);
  const state = age > 1500 ? (wt_S.conn.lastSeen ? "down" : "pend") : "up";

  const dot = wt_$("conn-dot");
  if (dot) dot.className = "conn-dot " + state;

  wt_UI.text(
    "conn-label",
    state === "up" ? "LIVE" :
    state === "down" ? "DISCONNECTED" :
    "CONNECTING"
  );
}

/* ═════════════ DISPLAY STATE APPLY ═════════════ */
function wt_applyDisplayState(d) {
  if (!d || d.available === false) return;

  wt_S.display = d;

  if (!wt_db.disp) {
    if (d.mode) wt_S.displayMode = d.mode;
    if (d.brightness != null) wt_S.brightness = d.brightness;
  }

  wt_UI.text("live-time", wt_displayText(d));
  wt_requestRender();
}

/* ═════════════ WEBSOCKET ═════════════ */
function wt_wsSend(cmd, data) {
  if (!wt_S.ws || wt_S.ws.readyState !== WebSocket.OPEN) return false;
  try {
    wt_S.ws.send(JSON.stringify({ cmd, ...(data || {}) }));
    return true;
  } catch {
    return false;
  }
}

function wt_handleWsMessage(d) {
  if (!d) return;

  wt_markConnectionSeen();

  if (d.type === "disp") {
    wt_applyDisplayState(d.display);
    return;
  }

  if (d.uptime_s != null) {
    const t = wt_fmtUptime(d.uptime_s);
    wt_UI.text("d-uptime", t);
    wt_UI.text("i-uptime", t);
  }

  if (d.free_heap != null) {
    const h = wt_fmtBytes(d.free_heap);
    wt_UI.text("d-heap", h);
    wt_UI.text("i-heap", h);
  }

  if (d.temperature != null) {
    const temp = wt_fmtTemperature(d.temperature);
    wt_UI.text("d-temp", temp);
    wt_UI.text("i-temp", temp);
  }

  if (d.display) wt_applyDisplayState(d.display);

  if (d.logs?.entries) {
    d.logs.entries.forEach(e => wt_S.logs.push(e));
  }

  wt_updateConnectionBadge();
}

/* ═════════════ WS INIT ═════════════ */
function wt_initWebSocket() {
  if (wt_S.ws) try { wt_S.ws.close(); } catch {}

  const url = (location.protocol === "https:" ? "wss:" : "ws:") + "//" + location.host + "/ws";

  try {
    wt_S.ws = new WebSocket(url);
  } catch {
    setTimeout(wt_initWebSocket, wt_S.wsRetryMs);
    return;
  }

  wt_S.ws.onopen = wt_markConnectionSeen;

  wt_S.ws.onmessage = e => {
    try {
      wt_handleWsMessage(JSON.parse(e.data));
    } catch {}
  };

  wt_S.ws.onclose = () => {
    setTimeout(wt_initWebSocket, wt_S.wsRetryMs);
  };
}

/* ═════════════ SETTINGS ═════════════ */
function wt_onDispChange() {
  wt_S.color = wt_$("dp-color")?.value || wt_S.color;
  wt_S.brightness = wt_clamp(parseInt(wt_$("dp-brt")?.value || 80), 0, 100);

  wt_debounce("disp", () => {
    wt_wsSend("settings", {
      color: wt_S.color,
      brightness: wt_S.brightness,
      display_mode: wt_S.displayMode,
      display_value: wt_S.displayValue,
      display_text: wt_S.displayText
    });
  }, 600);
}

/* ═════════════ INIT ═════════════ */
function wt_init() {
  wt_initWebSocket();
  setInterval(wt_updateConnectionBadge, 300);
  wt_requestRender();
}

document.addEventListener("DOMContentLoaded", wt_init);