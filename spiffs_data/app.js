/* ═══════════════════════════════════════════
   WATCH TOWER — Device Console · app.js
   All settings auto-save on change (debounced)
   ═══════════════════════════════════════════ */
"use strict";
const S = {
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
  timers: {},
};
const _db = {};
function debounce(key, fn, ms) {
  clearTimeout(_db[key]);
  _db[key] = setTimeout(fn, ms || 500);
}
async function apiGet(path) {
  try {
    const r = await fetch(path);
    if (!r.ok) throw new Error(r.status);
    return await r.json();
  } catch (e) {
    console.warn("GET", path, e.message);
    return null;
  }
}
async function apiPost(path, data) {
  try {
    const r = await fetch(path, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(data),
    });
    if (!r.ok) throw new Error(r.status);
    return await r.json();
  } catch (e) {
    console.warn("POST", path, e.message);
    return null;
  }
}
function el(id) {
  return document.getElementById(id);
}
function setText(id, v) {
  const e = el(id);
  if (e) e.textContent = v;
}
function fmtBytes(b) {
  if (b == null) return "—";
  if (b < 1024) return b + "B";
  if (b < 1048576) return (b / 1024).toFixed(1) + "KB";
  return (b / 1048576).toFixed(2) + "MB";
}
function fmtUptime(s) {
  if (!s && s !== 0) return "—";
  const d = Math.floor(s / 86400),
    h = Math.floor((s % 86400) / 3600),
    m = Math.floor((s % 3600) / 60),
    sc = s % 60;
  if (d > 0) return d + "d " + h + "h " + m + "m";
  if (h > 0) return h + "h " + m + "m " + sc + "s";
  return m + "m " + sc + "s";
}
function pad2(n) {
  return String(n).padStart(2, "0");
}
function clamp(v, min, max) {
  return Math.max(min, Math.min(max, v));
}
function esc(s) {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}
function updateBrightnessUi(v) {
  const value = clamp(parseInt(v, 10) || 0, 0, 100);
  const slider = el("dp-brt");
  if (slider) {
    slider.value = String(value);
    slider.style.setProperty("--pct", value + "%");
  }
  setText("brt-val", value + "%");
}
function toast(msg, type, ms) {
  type = type || "info";
  ms = ms || 2600;
  const icons = { ok: "✓", err: "✕", warn: "⚠", info: "ℹ" };
  const t = document.createElement("div");
  t.className = "toast " + type;
  t.innerHTML =
    "<span>" + (icons[type] || "ℹ") + "</span><span>" + msg + "</span>";
  el("toasts").appendChild(t);
  setTimeout(function () {
    t.style.cssText = "opacity:0;transform:translateX(16px);transition:0.3s";
    setTimeout(function () {
      t.remove();
    }, 310);
  }, ms);
}

/* ═══════════════════════════════════════════
   7-SEGMENT RENDERER — device-backed masks
═══════════════════════════════════════════ */
const SEG_BIT = { A: 1 << 0, B: 1 << 1, C: 1 << 2, D: 1 << 3, E: 1 << 4, F: 1 << 5, G: 1 << 6 };
const MASK_TO_CHAR = {
  0: " ",
  63: "0",
  6: "1",
  91: "2",
  79: "3",
  102: "4",
  109: "5",
  125: "6",
  7: "7",
  127: "8",
  111: "9",
  64: "-",
  8: "_",
  72: "=",
};
function hSeg(ctx, x, y, w, th, r) {
  if (w <= 0) return;
  ctx.beginPath();
  ctx.moveTo(x + r, y);
  ctx.lineTo(x + w - r, y);
  ctx.lineTo(x + w, y + th / 2);
  ctx.lineTo(x + w - r, y + th);
  ctx.lineTo(x + r, y + th);
  ctx.lineTo(x, y + th / 2);
  ctx.closePath();
  ctx.fill();
}
function vSeg(ctx, x, y, tw, h, r) {
  if (h <= 0) return;
  ctx.beginPath();
  ctx.moveTo(x + tw / 2, y);
  ctx.lineTo(x + tw, y + r);
  ctx.lineTo(x + tw, y + h - r);
  ctx.lineTo(x + tw / 2, y + h);
  ctx.lineTo(x, y + h - r);
  ctx.lineTo(x, y + r);
  ctx.closePath();
  ctx.fill();
}
function drawMask(ctx, mask, x, y, dw, dh, color, alpha) {
  alpha = alpha !== undefined ? alpha : 1;
  const sw = Math.max(4, Math.round(dw * 0.11));
  const g = Math.max(4, Math.round(sw * 0.95));
  const r = Math.round(sw * 0.38);
  const hl = dw - g * 2 - sw,
    vl = dh / 2 - g - sw * 0.5;
  const sa = function (on) {
    ctx.globalAlpha = on ? alpha : alpha * 0.05;
    ctx.fillStyle = color;
  };
  sa((mask & SEG_BIT.A) !== 0);
  hSeg(ctx, x + g + sw / 2, y + g, hl, sw, r);
  sa((mask & SEG_BIT.B) !== 0);
  vSeg(ctx, x + dw - g - sw, y + g + sw / 2, sw, vl, r);
  sa((mask & SEG_BIT.C) !== 0);
  vSeg(ctx, x + dw - g - sw, y + dh / 2 + sw / 2, sw, vl, r);
  sa((mask & SEG_BIT.D) !== 0);
  hSeg(ctx, x + g + sw / 2, y + dh - g - sw, hl, sw, r);
  sa((mask & SEG_BIT.E) !== 0);
  vSeg(ctx, x + g, y + dh / 2 + sw / 2, sw, vl, r);
  sa((mask & SEG_BIT.F) !== 0);
  vSeg(ctx, x + g, y + g + sw / 2, sw, vl, r);
  sa((mask & SEG_BIT.G) !== 0);
  hSeg(ctx, x + g + sw / 2, y + dh / 2 - sw / 2, hl, sw, r);
  ctx.globalAlpha = 1;
}
function rgbTripletToCss(rgb) {
  if (!rgb || rgb.length !== 3) return S.color;
  return "rgb(" + rgb[0] + "," + rgb[1] + "," + rgb[2] + ")";
}
function drawMaskExact(ctx, mask, segColors, x, y, dw, dh) {
  const sw = Math.max(4, Math.round(dw * 0.11));
  const g = Math.max(4, Math.round(sw * 0.95));
  const r = Math.round(sw * 0.38);
  const hl = dw - g * 2 - sw,
    vl = dh / 2 - g - sw * 0.5;
  const seg = function (bit, color, painter) {
    ctx.fillStyle = color;
    ctx.globalAlpha = 1;
    painter();
  };
  seg(SEG_BIT.A, rgbTripletToCss(segColors && segColors[0]), function () {
    hSeg(ctx, x + g + sw / 2, y + g, hl, sw, r);
  });
  seg(SEG_BIT.B, rgbTripletToCss(segColors && segColors[1]), function () {
    vSeg(ctx, x + dw - g - sw, y + g + sw / 2, sw, vl, r);
  });
  seg(SEG_BIT.C, rgbTripletToCss(segColors && segColors[2]), function () {
    vSeg(ctx, x + dw - g - sw, y + dh / 2 + sw / 2, sw, vl, r);
  });
  seg(SEG_BIT.D, rgbTripletToCss(segColors && segColors[3]), function () {
    hSeg(ctx, x + g + sw / 2, y + dh - g - sw, hl, sw, r);
  });
  seg(SEG_BIT.E, rgbTripletToCss(segColors && segColors[4]), function () {
    vSeg(ctx, x + g, y + dh / 2 + sw / 2, sw, vl, r);
  });
  seg(SEG_BIT.F, rgbTripletToCss(segColors && segColors[5]), function () {
    vSeg(ctx, x + g, y + g + sw / 2, sw, vl, r);
  });
  seg(SEG_BIT.G, rgbTripletToCss(segColors && segColors[6]), function () {
    hSeg(ctx, x + g + sw / 2, y + dh / 2 - sw / 2, hl, sw, r);
  });
  ctx.globalAlpha = 1;
}
function displayText(state) {
  if (!state || !state.digits) return "--:--";
  const chars = state.digits.map(function (mask) {
    return Object.prototype.hasOwnProperty.call(MASK_TO_CHAR, mask)
      ? MASK_TO_CHAR[mask]
      : "?";
  });
  return chars[0] + chars[1] + (state.colon ? ":" : " ") + chars[2] + chars[3];
}
function renderDisplay(cid, opts) {
  const canvas = el(cid);
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  const W = canvas.width,
    H = canvas.height;
  const state = opts || S.display || {};
  const color = state.color || S.color;
  const brt = (state.brightness != null ? state.brightness : S.brightness) / 100;
  const colon = state.colon != null ? state.colon : S.colonOn;
  const digits =
    state.digits && state.digits.length === 4 ? state.digits : [0, 0, 0, 0];
  const digitColors = state.digit_colors || null;
  const colonColor = state.colon_color || null;
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = "#050508";
  ctx.fillRect(0, 0, W, H);
  const DH = Math.floor(H * 0.84),
    DW = Math.floor(DH * 0.68),
    CW = Math.floor(DW * 0.3);
  const totalW = DW + 4 + DW + 4 + CW + DW + 4 + DW;
  let cx = Math.round((W - totalW) / 2);
  const cy = Math.round((H - DH) / 2);
  if (digitColors) drawMaskExact(ctx, digits[0], digitColors[0], cx, cy, DW, DH);
  else drawMask(ctx, digits[0], cx, cy, DW, DH, color, brt);
  cx += DW + 4;
  if (digitColors) drawMaskExact(ctx, digits[1], digitColors[1], cx, cy, DW, DH);
  else drawMask(ctx, digits[1], cx, cy, DW, DH, color, brt);
  cx += DW + 4;
  if (colon) {
    ctx.fillStyle = rgbTripletToCss(colonColor) || color;
    ctx.globalAlpha = digitColors ? 1 : brt;
    const dr = Math.max(3, Math.round(DH * 0.057));
    ctx.beginPath();
    ctx.arc(cx + CW / 2, cy + DH * 0.29, dr, 0, Math.PI * 2);
    ctx.fill();
    ctx.beginPath();
    ctx.arc(cx + CW / 2, cy + DH * 0.71, dr, 0, Math.PI * 2);
    ctx.fill();
    ctx.globalAlpha = 1;
  }
  cx += CW;
  if (digitColors) drawMaskExact(ctx, digits[2], digitColors[2], cx, cy, DW, DH);
  else drawMask(ctx, digits[2], cx, cy, DW, DH, color, brt);
  cx += DW + 4;
  if (digitColors) drawMaskExact(ctx, digits[3], digitColors[3], cx, cy, DW, DH);
  else drawMask(ctx, digits[3], cx, cy, DW, DH, color, brt);
}
function resizeCanvas(cid) {
  const canvas = el(cid);
  if (!canvas) return;
  const wrap = canvas.parentElement;
  if (!wrap) return;
  const W = wrap.clientWidth - 44;
  const H = Math.max(60, Math.round(W * 0.22));
  if (W > 0 && (canvas.width !== W || canvas.height !== H)) {
    canvas.width = W;
    canvas.height = H;
  }
}
function resizeAll() {
  resizeCanvas("vd-canvas");
  resizeCanvas("prev-canvas");
}
function tick() {
  resizeAll();
  renderDisplay("vd-canvas");
  renderDisplay("prev-canvas");
}

/* ═════ PAGE / SECTION NAV ════════════════ */
function showPage(id) {
  document.querySelectorAll(".page").forEach(function (p) {
    p.classList.remove("active");
  });
  document.querySelectorAll(".nb").forEach(function (b) {
    b.classList.remove("active");
  });
  const pg = el("pg-" + id);
  if (pg) pg.classList.add("active");
  const nb = document.querySelector('.nb[data-page="' + id + '"]');
  if (nb) nb.classList.add("active");
  if (id === "settings") {
    refreshSys();
    refreshFW();
  }
  if (id === "dashboard") {
    refreshDash();
  }
  try {
    localStorage.setItem("wt-page", id);
  } catch (e) {}
}
function showSec(id) {
  document.querySelectorAll(".spane").forEach(function (p) {
    p.classList.remove("active");
  });
  document.querySelectorAll(".snb").forEach(function (b) {
    b.classList.remove("active");
  });
  const pane = el("sec-" + id);
  if (pane) pane.classList.add("active");
  const nb = document.querySelector('.snb[data-s="' + id + '"]');
  if (nb) nb.classList.add("active");
  if (id === "logs") startLogPoll();
  if (id === "wifi") refreshWifi();
  if (id === "sys") refreshSys();
  if (id === "fw") refreshFW();
  if (id === "power") refreshPower();
  if (id === "display") {
    resizeCanvas("prev-canvas");
    renderDisplay("prev-canvas");
  }
  try {
    localStorage.setItem("wt-sec", id);
  } catch (e) {}
}
function toggleTheme() {
  const html = document.documentElement;
  const next = html.dataset.theme === "dark" ? "light" : "dark";
  html.dataset.theme = next;
  try {
    localStorage.setItem("wt-theme", next);
  } catch (e) {}
}
function initTheme() {
  try {
    const t = localStorage.getItem("wt-theme");
    if (t) document.documentElement.dataset.theme = t;
  } catch (e) {}
}

/* ═════ DASHBOARD  /api/status ════════════ */
async function refreshDash() {
  const d = await apiGet("/api/status");
  if (!d) return;
  setText("d-uptime", fmtUptime(d.uptime_s));
  setText("d-heap", fmtBytes(d.free_heap));
  setText("d-rssi", d.rssi ? d.rssi + " dBm" : "—");
  setText("d-ip", d.sta_ip || d.ap_ip || "—");
  setText("d-temp", d.temperature ? d.temperature.toFixed(1) + "°C" : "—");
  setText("d-fw", d.app_version || "—");
  const dot = el("wdot"),
    lbl = el("wifi-label");
  if (dot) dot.className = "wdot " + (d.sta_connected ? "up" : "down");
  if (lbl)
    lbl.textContent = d.sta_connected ? d.sta_ssid || "Connected" : "Offline";
  updateDispInfo();
}
function updateDispInfo() {
  const st = S.display;
  setText(
    "dinfo-fmt",
    st && st.mode ? st.mode.toUpperCase() : S.displayMode.toUpperCase(),
  );
  setText(
    "dinfo-blink",
    st ? (st.colon_blink ? "BLINK ON" : "BLINK OFF") : S.blink ? "BLINK ON" : "BLINK OFF",
  );
  setText("dinfo-brt", "BRT " + S.brightness + "%");
  const dc = el("dinfo-color");
  if (dc) {
    dc.textContent = "● " + S.color.toUpperCase();
    dc.style.color = S.color;
  }
}
function syncDisplayModeInputs() {
  const mode = (el("dp-mode") && el("dp-mode").value) || S.displayMode || "time";
  const vw = el("dp-value-wrap");
  const tw = el("dp-text-wrap");
  if (vw) vw.style.display = mode === "number" ? "" : "none";
  if (tw) tw.style.display = mode === "text" ? "" : "none";
}

async function refreshDisplay() {
  const d = await apiGet("/api/display");
  if (!d || d.available === false) return;
  S.display = d;
  if (d.mode) S.displayMode = d.mode;
  if (d.brightness != null) S.brightness = d.brightness;
  setText("live-time", displayText(d));
  updateDispInfo();
  resizeAll();
  renderDisplay("vd-canvas");
  renderDisplay("prev-canvas");
}
async function manualDisplayRefresh() {
  await Promise.allSettled([loadSettings(), refreshDisplay()]);
  toast("Display refreshed", "ok", 1200);
}

/* ═════ SYSTEM ════════════════════════════ */
async function refreshSys() {
  const d = await apiGet("/api/status");
  if (!d) return;
  const ramPct =
    d.total_heap && d.free_heap
      ? Math.round((1 - d.free_heap / d.total_heap) * 100)
      : 0;
  setBar("ram", ramPct, "ram-pct");
  setBar("cpu", d.cpu_usage || 0, "cpu-pct");
  setBar("flash", d.flash_used_pct || 0, "flash-pct");
  setBar("spiffs", d.spiffs_used_pct || 0, "spiffs-pct");
  setText("i-chip", d.chip_model || "—");
  setText("i-cores", d.cpu_cores || "—");
  setText("i-freq", d.cpu_freq_mhz ? d.cpu_freq_mhz + " MHz" : "—");
  setText("i-flash", fmtBytes(d.flash_size));
  setText("i-heap", fmtBytes(d.free_heap));
  setText("i-minheap", fmtBytes(d.min_free_heap));
  setText("i-uptime", fmtUptime(d.uptime_s));
  setText("i-temp", d.temperature ? d.temperature.toFixed(1) + "°C" : "—");
  setText("i-reset", d.reset_reason || "—");
}
function setBar(id, pct, labelId) {
  const f = el(id + "-bar");
  if (f) f.style.width = Math.min(100, Math.round(pct)) + "%";
  if (labelId) setText(labelId, Math.round(pct) + "%");
}
async function refreshFW() {
  const d = await apiGet("/api/status");
  if (!d) return;
  setText("fw-ver", d.app_version || "—");
  setText("fw-date", d.build_date || "—");
  setText("fw-idf", d.idf_version || "—");
  setText("fw-slot", d.ota_slot || "—");
  setText("fw-app0", d.app0_state || "—");
  setText("fw-app1", d.app1_state || "—");
  setText(
    "fw-spiffs",
    d.spiffs_used_pct != null ? d.spiffs_used_pct + "%" : "—",
  );
}

async function doRefresh(silent) {
  const activePage = document.querySelector(".page.active");
  const activeSec = document.querySelector(".snb.active");
  const jobs = [refreshDash(), refreshDisplay()];
  if (activePage && activePage.id === "pg-settings") {
    const sid = activeSec ? activeSec.dataset.s : "sys";
    if (sid === "sys") jobs.push(refreshSys());
    if (sid === "fw") jobs.push(refreshFW());
    if (sid === "logs") jobs.push(pollLogs());
    if (sid === "power") jobs.push(refreshPower());
    if (sid === "wifi") jobs.push(refreshWifi());
    if (sid === "display")
      jobs.push(refreshDisplay());
  }
  await Promise.allSettled(jobs);
  tick();
  if (!silent) toast("Data refreshed", "ok", 1200);
}

/* ═════ LOGS  /api/logs?seq=N ════════════ */
function detectLevel(line) {
  if (/\[E\]|ERROR/.test(line)) return "ERROR";
  if (/\[W\]|WARN/i.test(line)) return "WARN";
  if (/\[I\]|INFO/i.test(line)) return "INFO";
  return "DEBUG";
}
function renderLogLine(line, cid) {
  const c = el(cid);
  if (!c) return;
  const lv = detectLevel(line);
  if (S.logFilter !== "ALL" && lv !== S.logFilter) return;
  const div = document.createElement("div");
  div.className = "le " + lv[0];
  div.textContent = line;
  c.appendChild(div);
  while (c.children.length > 500) c.removeChild(c.firstChild);
  const as = el("log-as");
  if (!as || as.checked) c.scrollTop = c.scrollHeight;
  setText("log-count", S.logs.length + " entries");
}
function addLogEntry(entry) {
  const line =
    typeof entry === "string"
      ? entry
      : "[" +
        (entry.lvl || "I") +
        "] " +
        (entry.tag ? entry.tag + ": " : "") +
        (entry.msg || "");
  S.logs.push(line);
  if (S.logs.length > 2000) S.logs.shift();
  renderLogLine(line, "log-viewer");
  renderLogLine(line, "dash-logs");
}
async function pollLogs() {
  const d = await apiGet("/api/logs?seq=" + S.lastSeq);
  if (!d) return;
  const entries = d.logs || d.entries || [];
  if (entries.length) {
    entries.forEach(addLogEntry);
    S.lastSeq = d.next_seq || S.lastSeq + entries.length;
    setText("log-ts", "Updated " + new Date().toLocaleTimeString());
  }
}
function startLogPoll() {
  if (S.timers.logs) return;
  pollLogs();
  S.timers.logs = setInterval(pollLogs, 2000);
}
function setLogFilter(lv) {
  S.logFilter = lv;
  document.querySelectorAll(".lfb").forEach(function (b) {
    b.classList.toggle("active", b.dataset.lv === lv);
  });
  const v = el("log-viewer");
  if (v) {
    v.innerHTML = "";
    S.logs.forEach(function (l) {
      renderLogLine(l, "log-viewer");
    });
  }
}
function clearLogs() {
  const v = el("log-viewer");
  if (v) v.innerHTML = "";
}
function exportLogs() {
  const blob = new Blob([S.logs.join("\n")], { type: "text/plain" });
  const a = document.createElement("a");
  a.href = URL.createObjectURL(blob);
  a.download = "wt-logs-" + Date.now() + ".txt";
  a.click();
}

/* ═════ DISPLAY SETTINGS ════════════════ */
function onDispChange() {
  S.color = (el("dp-color") && el("dp-color").value) || "#e8e4de";
  S.brightness = parseInt((el("dp-brt") && el("dp-brt").value) || "80");
  S.blink = el("dp-blink") ? el("dp-blink").checked : true;
  S.scroll = el("dp-scroll") ? el("dp-scroll").checked : false;
  S.pulse = el("dp-pulse") ? el("dp-pulse").checked : false;
  S.transition = el("dp-trans") ? el("dp-trans").checked : true;
  S.displayMode = (el("dp-mode") && el("dp-mode").value) || "time";
  S.displayValue = clamp(
    parseInt((el("dp-value") && el("dp-value").value) || "0", 10) || 0,
    0,
    9999,
  );
  S.displayText = ((el("dp-text") && el("dp-text").value) || "")
    .toUpperCase()
    .replace(/[^A-Z0-9 _-]/g, "")
    .slice(0, 4);
  updateBrightnessUi(S.brightness);
  if (el("dp-value")) el("dp-value").value = String(S.displayValue);
  if (el("dp-text")) el("dp-text").value = S.displayText;
  syncDisplayModeInputs();
  markActiveSwatch();
  updateDispInfo();
  debounce("disp", sendDispSettings, 400);
}
async function sendDispSettings() {
  const react = el("dp-react");
  const r = await apiPost("/api/settings", {
    color: S.color,
    brightness: S.brightness,
    anim_colon: S.blink,
    anim_scroll: S.scroll,
    anim_pulse: S.pulse,
    anim_transition: S.transition,
    reaction_effect: react ? react.value : "none",
    display_mode: S.displayMode,
    display_value: S.displayValue,
    display_text: S.displayText,
  });
  if (r && r.status === "ok") {
    await refreshDisplay();
    toast("Display updated", "ok");
  }
}
async function forceSendDispSettings() {
  clearTimeout(_db.disp);
  onDispChange();
  clearTimeout(_db.disp);
  await sendDispSettings();
}
function pickColor(hex) {
  const p = el("dp-color");
  if (p) p.value = hex;
  onDispChange();
}
function markActiveSwatch() {
  const target = (S.color || "").toLowerCase();
  document.querySelectorAll(".swatches .sw").forEach(function (sw) {
    const c = (sw.style.background || sw.style.backgroundColor || "")
      .replace(/\s+/g, "")
      .toLowerCase();
    sw.classList.toggle("active", c === target);
  });
}

/* ═════ CLOCK SETTINGS ══════════════════ */
function setFmt(n) {
  S.fmt = n;
  const b24 = el("fmt-24"),
    b12 = el("fmt-12");
  if (b24) b24.classList.toggle("active", n === 24);
  if (b12) b12.classList.toggle("active", n === 12);
  onClockChange();
}
function onClockChange() {
  debounce("clock", sendClockSettings, 500);
}
async function sendClockSettings() {
  const r = await apiPost("/api/settings", {
    time_format: S.fmt,
    timezone: el("tz-sel") ? el("tz-sel").value : "UTC0",
    ntp_server: el("ntp-srv") ? el("ntp-srv").value : "pool.ntp.org",
    alarm1_time: el("al1-t") ? el("al1-t").value : "07:00",
    alarm1_en: el("al1-en") ? el("al1-en").checked : false,
    alarm2_time: el("al2-t") ? el("al2-t").value : "22:00",
    alarm2_en: el("al2-en") ? el("al2-en").checked : false,
    notif_type: el("notif-type") ? el("notif-type").value : "flash",
    notif_sound: el("notif-sound") ? el("notif-sound").value : "beep",
  });
  if (r && r.status === "ok") toast("Clock saved", "ok");
}
async function syncNTP() {
  const srv = el("ntp-srv") ? el("ntp-srv").value : "pool.ntp.org";
  const r = await apiPost("/api/ntp/sync", { server: srv });
  if (r) {
    toast("NTP sync triggered", "ok");
    setText("ntp-hint", "Last sync: " + new Date().toLocaleTimeString());
  } else toast("NTP sync failed", "err");
}
async function rebootDevice() {
  const ok = window.confirm("Reboot the device now?");
  if (!ok) return;
  const r = await apiPost("/api/reboot", {});
  if (r && (r.ok || r.status === "ok")) {
    toast("Rebooting device…", "ok", 2200);
  } else {
    toast("Reboot failed", "err");
  }
}

/* ═════ POWER ════════════════════════════ */
async function refreshPower() {
  const d = await apiGet("/api/power");
  if (!d) return;
  const pct = d.battery_pct || 0;
  const bar = el("batt-fill");
  if (bar) {
    bar.style.width = Math.min(100, pct) + "%";
    bar.className = "batt-fill" + (pct < 20 ? " low" : pct < 50 ? " mid" : "");
  }
  setText("batt-txt", pct + "%");
  setText("batt-v", d.voltage ? d.voltage.toFixed(2) + "V" : "—V");
  setText("batt-ma", d.current ? d.current + "mA" : "—mA");
  setText("pwr-src", d.source || "—");
  setText("batt-eta", d.eta_hours ? d.eta_hours.toFixed(1) + "h" : "—h");
  if (d.sleep_mode && el("sl-mode")) el("sl-mode").value = d.sleep_mode;
  if (d.sleep_timeout != null && el("sl-timeout"))
    el("sl-timeout").value = d.sleep_timeout;
  if (d.batt_alert_pct != null && el("batt-alert"))
    el("batt-alert").value = d.batt_alert_pct;
  if (d.ps_dim != null && el("ps-dim")) el("ps-dim").checked = d.ps_dim;
  if (d.ps_wifi != null && el("ps-wifi")) el("ps-wifi").checked = d.ps_wifi;
}
function onPowerChange() {
  debounce("power", sendPowerSettings, 500);
}
async function sendPowerSettings() {
  const r = await apiPost("/api/settings", {
    sleep_mode: el("sl-mode") ? el("sl-mode").value : "none",
    sleep_timeout: el("sl-timeout") ? parseInt(el("sl-timeout").value) : 30,
    batt_alert_pct: el("batt-alert") ? parseInt(el("batt-alert").value) : 20,
    ps_dim: el("ps-dim") ? el("ps-dim").checked : true,
    ps_wifi: el("ps-wifi") ? el("ps-wifi").checked : false,
  });
  if (r && r.status === "ok") toast("Power settings saved", "ok");
}

/* ═════ WIFI  /api/wifi/status ══════════ */
async function refreshWifi() {
  const d = await apiGet("/api/wifi/status");
  if (!d) return;
  S.wifi = d;
  const dot = el("wsc-dot");
  if (dot) dot.className = "wsc-dot " + (d.connected ? "on" : "off");
  setText("wsc-ssid", d.connected ? d.ssid || "—" : "Not connected");
  setText(
    "wsc-detail",
    d.connected
      ? (d.ip || "—") +
          " · " +
          (d.rssi || "—") +
          " dBm" +
          (d.channel ? " · Ch" + d.channel : "")
      : "Searching…",
  );
  const ic = el("rssi-ic"),
    rv = el("rssi-v");
  if (rv) rv.textContent = d.rssi ? d.rssi + " dBm" : "— dBm";
  if (ic) {
    let s = 0;
    if (d.rssi >= -55) s = 4;
    else if (d.rssi >= -65) s = 3;
    else if (d.rssi >= -75) s = 2;
    else if (d.rssi) s = 1;
    ic.className = "rssi-ic" + (s ? " s" + s : "");
  }
  const hdot = el("wdot"),
    hlbl = el("wifi-label");
  if (hdot) hdot.className = "wdot " + (d.connected ? "up" : "down");
  if (hlbl) hlbl.textContent = d.connected ? d.ssid || "Connected" : "Offline";
  renderProfiles(d.profiles || []);
}
function renderProfiles(profiles) {
  const c = el("profiles-list");
  if (!c) return;
  if (!profiles.length) {
    c.innerHTML = '<div class="empty">No saved profiles</div>';
    return;
  }
  c.innerHTML = "";
  profiles.forEach(function (p, i) {
    const active = S.wifi && S.wifi.ssid === p.ssid;
    const div = document.createElement("div");
    div.className = "pitem" + (active ? " active-net" : "");
    div.innerHTML =
      '<span class="pi-name">' +
      esc(p.ssid) +
      "</span>" +
      (active ? '<span class="pi-badge">CONNECTED</span>' : "") +
      '<div class="pi-actions">' +
      (!active
        ? '<button class="bsm" onclick="connectProfile(' + i + ')">Connect</button>'
        : "") +
      '<button class="pi-del" onclick="delProfile(' +
      i +
      ')">Remove</button>' +
      "</div>";
    c.appendChild(div);
  });
}
function showAddWifi() {
  const f = el("add-wifi-form");
  if (f) f.classList.remove("hidden");
}
function hideAddWifi() {
  const f = el("add-wifi-form");
  if (f) f.classList.add("hidden");
  if (el("new-ssid")) el("new-ssid").value = "";
  if (el("new-pass")) el("new-pass").value = "";
}
async function addProfile() {
  const ssid = (el("new-ssid") && el("new-ssid").value.trim()) || "";
  const pass = (el("new-pass") && el("new-pass").value) || "";
  if (!ssid) {
    toast("SSID required", "warn");
    return;
  }
  const r = await apiPost("/api/wifi/profiles", { ssid: ssid, password: pass });
  if (r && r.status === "ok") {
    toast('"' + ssid + '" added', "ok");
    hideAddWifi();
    await refreshWifi();
  } else toast("Failed to add profile", "err");
}
async function delProfile(idx) {
  const r = await apiPost("/api/wifi/profile/delete", { index: idx });
  if (r && r.status === "ok") {
    toast("Profile removed", "ok");
    await refreshWifi();
  } else toast("Failed", "err");
}
async function connectProfile(idx) {
  const r = await apiPost("/api/wifi/connect", { index: idx });
  if (r && r.status === "ok") {
    toast("Connecting…", "ok");
    await refreshWifi();
  } else toast("Connect failed", "err");
}
function togglePw() {
  const f = el("new-pass");
  if (f) f.type = f.type === "password" ? "text" : "password";
}

/* ═════ OTA ══════════════════════════════ */
function uploadFW(file) {
  if (!file) return;
  if (!file.name.endsWith(".bin")) {
    toast("Need .bin file", "warn");
    return;
  }
  doUpload(
    file,
    "/api/ota/firmware",
    "fw-prog",
    "fw-fill",
    "fw-pct",
    function () {
      toast("Firmware updated! Rebooting…", "ok");
    },
    function () {
      toast("Upload failed", "err");
    },
  );
}
function uploadWA(file) {
  if (!file) return;
  doUpload(
    file,
    "/api/ota/webapp",
    "wa-prog",
    "wa-fill",
    "wa-pct",
    function () {
      toast("Web app updated!", "ok");
    },
    function () {
      toast("Upload failed", "err");
    },
  );
}
function doUpload(file, url, progId, fillId, pctId, onOk, onErr) {
  const pw = el(progId),
    pf = el(fillId);
  if (pw) pw.classList.remove("hidden");
  const xhr = new XMLHttpRequest();
  xhr.open("POST", url);
  xhr.upload.onprogress = function (e) {
    if (!e.lengthComputable) return;
    const p = Math.round((e.loaded / e.total) * 100);
    if (pf) pf.style.width = p + "%";
    setText(pctId, p + "%");
  };
  xhr.onload = function () {
    (xhr.status === 200 ? onOk : onErr)();
  };
  xhr.onerror = onErr;
  const fd = new FormData();
  fd.append("file", file);
  xhr.send(fd);
}
function setupDnD() {
  ["fw-dz", "wa-dz"].forEach(function (id) {
    const z = el(id);
    if (!z) return;
    z.addEventListener("dragover", function (e) {
      e.preventDefault();
      z.classList.add("drag-over");
    });
    z.addEventListener("dragleave", function () {
      z.classList.remove("drag-over");
    });
    z.addEventListener("drop", function (e) {
      e.preventDefault();
      z.classList.remove("drag-over");
      const f = e.dataTransfer.files[0];
      if (id === "fw-dz") uploadFW(f);
      else uploadWA(f);
    });
  });
}

/* ═════ LOAD SETTINGS ═══════════════════ */
async function loadSettings() {
  const d = await apiGet("/api/settings");
  if (!d) return;
  if (d.color) {
    S.color = d.color;
    if (el("dp-color")) el("dp-color").value = d.color;
  }
  if (d.brightness != null) {
    S.brightness = d.brightness;
    updateBrightnessUi(d.brightness);
  }
  if (d.display_mode) {
    S.displayMode = d.display_mode;
    if (el("dp-mode")) el("dp-mode").value = d.display_mode;
  }
  if (d.display_value != null) {
    S.displayValue = d.display_value;
    if (el("dp-value")) el("dp-value").value = d.display_value;
  }
  if (d.display_text != null) {
    S.displayText = d.display_text;
    if (el("dp-text")) el("dp-text").value = d.display_text;
  }
  if (d.time_format) {
    S.fmt = d.time_format;
    setFmt(d.time_format);
  }
  const bmap = {
    "dp-blink": "anim_colon",
    "dp-scroll": "anim_scroll",
    "dp-pulse": "anim_pulse",
    "dp-trans": "anim_transition",
    "ps-dim": "ps_dim",
    "ps-wifi": "ps_wifi",
    "al1-en": "alarm1_en",
    "al2-en": "alarm2_en",
  };
  Object.keys(bmap).forEach(function (eid) {
    const k = bmap[eid];
    if (d[k] != null && el(eid)) el(eid).checked = d[k];
  });
  if (d.anim_colon != null) S.blink = d.anim_colon;
  if (d.anim_pulse != null) S.pulse = d.anim_pulse;
  if (d.reaction_effect && el("dp-react"))
    el("dp-react").value = d.reaction_effect;
  if (d.timezone && el("tz-sel")) el("tz-sel").value = d.timezone;
  if (d.ntp_server && el("ntp-srv")) el("ntp-srv").value = d.ntp_server;
  if (d.notif_type && el("notif-type")) el("notif-type").value = d.notif_type;
  if (d.notif_sound && el("notif-sound"))
    el("notif-sound").value = d.notif_sound;
  if (d.sleep_mode && el("sl-mode")) el("sl-mode").value = d.sleep_mode;
  if (d.alarm1_time && el("al1-t")) el("al1-t").value = d.alarm1_time;
  if (d.alarm2_time && el("al2-t")) el("al2-t").value = d.alarm2_time;
  if (d.sleep_timeout != null && el("sl-timeout"))
    el("sl-timeout").value = d.sleep_timeout;
  if (d.batt_alert_pct != null && el("batt-alert"))
    el("batt-alert").value = d.batt_alert_pct;
  markActiveSwatch();
  syncDisplayModeInputs();
  updateDispInfo();
}

function initNavState() {
  let page = "dashboard";
  let sec = "sys";
  try {
    page = localStorage.getItem("wt-page") || page;
    sec = localStorage.getItem("wt-sec") || sec;
  } catch (e) {}
  showPage(page === "settings" ? "settings" : "dashboard");
  if (page === "settings") {
    const target = document.querySelector('.snb[data-s="' + sec + '"]')
      ? sec
      : "sys";
    showSec(target);
  }
}

/* ═════ INIT ═════════════════════════════ */
async function init() {
  initTheme();
  setupDnD();
  window.addEventListener("resize", function () {
    resizeAll();
    renderDisplay("vd-canvas");
    renderDisplay("prev-canvas");
  });
  await loadSettings();
  updateBrightnessUi(S.brightness);
  initNavState();
  await doRefresh(true);
  startLogPoll();
  S.timers.dash = setInterval(refreshDash, 10000);
  S.timers.display = setInterval(refreshDisplay, 500);
  S.timers.wifi = setInterval(refreshWifi, 8000);
  S.timers.power = setInterval(refreshPower, 30000);
  setInterval(tick, 500);
  tick();
}
document.addEventListener("DOMContentLoaded", init);
