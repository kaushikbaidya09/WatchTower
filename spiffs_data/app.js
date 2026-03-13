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
function esc(s) {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
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
   7-SEGMENT RENDERER — HH:MM only, wide segs
═══════════════════════════════════════════ */
const SEG_PAT = [
  [1, 1, 1, 1, 1, 1, 0],
  [0, 1, 1, 0, 0, 0, 0],
  [1, 1, 0, 1, 1, 0, 1],
  [1, 1, 1, 1, 0, 0, 1],
  [0, 1, 1, 0, 0, 1, 1],
  [1, 0, 1, 1, 0, 1, 1],
  [1, 0, 1, 1, 1, 1, 1],
  [1, 1, 1, 0, 0, 0, 0],
  [1, 1, 1, 1, 1, 1, 1],
  [1, 1, 1, 1, 0, 1, 1],
];
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
function drawDigit(ctx, digit, x, y, dw, dh, color, alpha) {
  alpha = alpha !== undefined ? alpha : 1;
  const p = SEG_PAT[digit] || SEG_PAT[0];
  const sw = Math.max(5, Math.round(dw * 0.13));
  const g = Math.max(2, Math.round(sw * 0.5));
  const r = Math.round(sw * 0.38);
  const hl = dw - g * 2 - sw,
    vl = dh / 2 - g - sw * 0.5;
  const sa = function (on) {
    ctx.globalAlpha = on ? alpha : alpha * 0.05;
    ctx.fillStyle = color;
  };
  sa(p[0]);
  hSeg(ctx, x + g + sw / 2, y + g, hl, sw, r);
  sa(p[1]);
  vSeg(ctx, x + dw - g - sw, y + g + sw / 2, sw, vl, r);
  sa(p[2]);
  vSeg(ctx, x + dw - g - sw, y + dh / 2 + sw / 2, sw, vl, r);
  sa(p[3]);
  hSeg(ctx, x + g + sw / 2, y + dh - g - sw, hl, sw, r);
  sa(p[4]);
  vSeg(ctx, x + g, y + dh / 2 + sw / 2, sw, vl, r);
  sa(p[5]);
  vSeg(ctx, x + g, y + g + sw / 2, sw, vl, r);
  sa(p[6]);
  hSeg(ctx, x + g + sw / 2, y + dh / 2 - sw / 2, hl, sw, r);
  ctx.globalAlpha = 1;
}
function renderClock(cid, opts) {
  const canvas = el(cid);
  if (!canvas) return;
  const ctx = canvas.getContext("2d");
  const W = canvas.width,
    H = canvas.height;
  const color = (opts && opts.color) || S.color;
  const brt = (opts && opts.brt != null ? opts.brt : S.brightness) / 100;
  const fmt24 = opts && opts.fmt != null ? opts.fmt === 24 : S.fmt === 24;
  const colon = opts && opts.colon != null ? opts.colon : S.colonOn;
  const pulse = opts && opts.pulse != null ? opts.pulse : S.pulse;
  ctx.clearRect(0, 0, W, H);
  ctx.fillStyle = "#050508";
  ctx.fillRect(0, 0, W, H);
  if (pulse) {
    const p2 = 0.5 + 0.5 * Math.sin(Date.now() / 600);
    const hex = color.replace("#", "");
    const rc = parseInt(hex.slice(0, 2), 16),
      gc = parseInt(hex.slice(2, 4), 16),
      bc = parseInt(hex.slice(4, 6), 16);
    const gr = ctx.createRadialGradient(
      W / 2,
      H / 2,
      0,
      W / 2,
      H / 2,
      W * 0.55,
    );
    gr.addColorStop(
      0,
      "rgba(" + rc + "," + gc + "," + bc + "," + 0.12 * p2 + ")",
    );
    gr.addColorStop(1, "transparent");
    ctx.fillStyle = gr;
    ctx.fillRect(0, 0, W, H);
  }
  const now = new Date();
  let hh = now.getHours(),
    mm = now.getMinutes(),
    ampm = "";
  if (!fmt24) {
    ampm = hh >= 12 ? "PM" : "AM";
    hh = hh % 12 || 12;
  }
  const h1 = fmt24 ? Math.floor(hh / 10) : hh >= 10 ? Math.floor(hh / 10) : -1;
  const h2 = hh % 10,
    m1 = Math.floor(mm / 10),
    m2 = mm % 10;
  const DH = Math.floor(H * 0.84),
    DW = Math.floor(DH * 0.68),
    CW = Math.floor(DW * 0.3);
  const amW = ampm ? Math.floor(DH * 0.28) + 6 : 0;
  const showH1 = h1 >= 0;
  const totalW = (showH1 ? DW + 4 : 0) + DW + 4 + CW + DW + 4 + DW + amW;
  let cx = Math.round((W - totalW) / 2);
  const cy = Math.round((H - DH) / 2);
  ctx.shadowBlur = Math.round(20 * brt);
  ctx.shadowColor = color;
  if (showH1) {
    drawDigit(ctx, h1, cx, cy, DW, DH, color, brt);
    cx += DW + 4;
  }
  drawDigit(ctx, h2, cx, cy, DW, DH, color, brt);
  cx += DW + 4;
  if (colon) {
    ctx.fillStyle = color;
    ctx.globalAlpha = brt;
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
  drawDigit(ctx, m1, cx, cy, DW, DH, color, brt);
  cx += DW + 4;
  drawDigit(ctx, m2, cx, cy, DW, DH, color, brt);
  cx += DW + 6;
  if (ampm) {
    ctx.shadowBlur = 8;
    ctx.fillStyle = color;
    ctx.globalAlpha = brt * 0.75;
    ctx.font = "bold " + Math.floor(DH * 0.22) + "px 'Bebas Neue',monospace";
    ctx.textBaseline = "middle";
    ctx.fillText(ampm, cx, cy + DH / 2);
    ctx.globalAlpha = 1;
  }
  ctx.shadowBlur = 0;
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
let _cf = 0;
function tick() {
  _cf++;
  const blinkOn = el("dp-blink") ? el("dp-blink").checked : true;
  S.colonOn = blinkOn ? _cf % 2 === 0 : true;
  const now = new Date();
  setText(
    "live-time",
    pad2(now.getHours()) +
      ":" +
      pad2(now.getMinutes()) +
      ":" +
      pad2(now.getSeconds()),
  );
  resizeAll();
  renderClock("vd-canvas");
  renderClock("prev-canvas");
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
    renderClock("prev-canvas");
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
  setText("dinfo-fmt", S.fmt === 24 ? "24H" : "12H");
  setText("dinfo-blink", S.blink ? "BLINK ON" : "BLINK OFF");
  setText("dinfo-brt", "BRT " + S.brightness + "%");
  const dc = el("dinfo-color");
  if (dc) {
    dc.textContent = "● " + S.color.toUpperCase();
    dc.style.color = S.color;
  }
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
  const jobs = [refreshDash()];
  if (activePage && activePage.id === "pg-settings") {
    const sid = activeSec ? activeSec.dataset.s : "sys";
    if (sid === "sys") jobs.push(refreshSys());
    if (sid === "fw") jobs.push(refreshFW());
    if (sid === "logs") jobs.push(pollLogs());
    if (sid === "power") jobs.push(refreshPower());
    if (sid === "wifi") jobs.push(refreshWifi());
    if (sid === "display")
      jobs.push(Promise.resolve(renderClock("prev-canvas")));
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
  const bv = el("brt-val");
  if (bv) bv.textContent = S.brightness + "%";
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
  });
  if (r && r.status === "ok") toast("Display updated", "ok");
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
      '<button class="pi-del" onclick="delProfile(' +
      i +
      ')">Remove</button>';
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
    if (el("dp-brt")) el("dp-brt").value = d.brightness;
    setText("brt-val", d.brightness + "%");
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
    renderClock("vd-canvas");
    renderClock("prev-canvas");
  });
  await loadSettings();
  initNavState();
  await doRefresh(true);
  startLogPoll();
  S.timers.dash = setInterval(refreshDash, 10000);
  S.timers.wifi = setInterval(refreshWifi, 8000);
  S.timers.power = setInterval(refreshPower, 30000);
  setInterval(tick, 500);
  tick();
}
document.addEventListener("DOMContentLoaded", init);
