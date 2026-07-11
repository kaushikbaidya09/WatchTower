/* ═══════════════════════════════════════════
   WATCH TOWER — Device Console · app.js
   All settings auto-save on change (debounced)
   ═══════════════════════════════════════════ */
'use strict';
/* Reconnect backoff: starts fast, doubles up to a cap so a rebooting
   device isn't hammered but the common transient drop recovers quickly. */
const WS_RETRY_MIN_MS = 1000;
const WS_RETRY_MAX_MS = 8000;
/* No message (display frames arrive every 50 ms while connected) for this
   long means the socket is dead even if the browser hasn't noticed yet —
   force a close so the reconnect path in initWebSocket() kicks in. */
const WS_WATCHDOG_MS = 5000;
const S = {
    color: '#e8e4de',
    brightness: 80,
    fmt: 24,
    blink: true,
    scroll: false,
    pulse: false,
    transition: true,
    colonOn: true,
    logFilter: 'ALL',
    logs: [],
    wifi: null,
    display: null,
    displayMode: 'time',
    displayValue: 1234,
    displayText: 'HELO',
    conn: {
        state: 'pend',
        lastSeen: 0,
        lastTry: 0,
    },
    timers: {},
    /* WebSocket */
    ws: null,
    wsConnected: false,
    wsRetryMs: WS_RETRY_MIN_MS,
    wsWatchdog: null,
};
const _db = {};
let renderQueued = false;
let renderDirty = true;
function debounce(key, fn, ms) {
    clearTimeout(_db[key]);
    _db[key] = setTimeout(fn, ms || 500);
}
/* All data I/O goes through the WebSocket (wsSend / handleWsMessage).
   apiGet / apiPost have been removed — the only HTTP requests left are
   the two OTA binary uploads which use XHR directly in doUpload(). */

function el(id) {
    return document.getElementById(id);
}
function setText(id, v) {
    const e = el(id);
    if (e) e.textContent = v;
}
function isFocused(id) {
    return document.activeElement === el(id);
}
function setValueIfIdle(id, value) {
    const node = el(id);
    if (!node || isFocused(id)) return;
    node.value = value;
}
function setCheckedIfIdle(id, value) {
    const node = el(id);
    if (!node || isFocused(id)) return;
    node.checked = !!value;
}
/* Read a form field back out, falling back to dflt when the element is
   missing — the read-side counterpart of setValueIfIdle/setCheckedIfIdle,
   used when building the object sent to the device. */
function fieldStr(id, dflt) {
    const node = el(id);
    return node ? node.value : dflt;
}
function fieldNum(id, dflt) {
    const node = el(id);
    return node ? parseInt(node.value, 10) : dflt;
}
function fieldBool(id, dflt) {
    const node = el(id);
    return node ? node.checked : dflt;
}
function fmtBytes(b) {
    if (b == null) return '—';
    if (b < 1024) return b + 'B';
    if (b < 1048576) return (b / 1024).toFixed(1) + 'KB';
    return (b / 1048576).toFixed(2) + 'MB';
}
function fmtUptime(s) {
    if (!s && s !== 0) return '—';
    const d = Math.floor(s / 86400),
        h = Math.floor((s % 86400) / 3600),
        m = Math.floor((s % 3600) / 60),
        sc = s % 60;
    if (d > 0) return d + 'd ' + h + 'h ' + m + 'm';
    if (h > 0) return h + 'h ' + m + 'm ' + sc + 's';
    return m + 'm ' + sc + 's';
}
function clamp(v, min, max) {
    return Math.max(min, Math.min(max, v));
}
function esc(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
}
function updateBrightnessUi(v) {
    const value = clamp(parseInt(v, 10) || 0, 0, 100);
    const slider = el('dp-brt');
    if (slider) {
        slider.value = String(value);
        slider.style.setProperty('--pct', value + '%');
    }
    setText('brt-val', value + '%');
}
function toast(msg, type, ms) {
    type = type || 'info';
    ms = ms || 2600;
    const icons = { ok: '✓', err: '✕', warn: '⚠', info: 'ℹ' };
    const t = document.createElement('div');
    t.className = 'toast ' + type;
    t.innerHTML = '<span>' + (icons[type] || 'ℹ') + '</span><span>' + msg + '</span>';
    el('toasts').appendChild(t);
    setTimeout(function () {
        t.style.cssText = 'opacity:0;transform:translateX(16px);transition:0.3s';
        setTimeout(function () {
            t.remove();
        }, 310);
    }, ms);
}

/* ═══════════════════════════════════════════
   7-SEGMENT RENDERER — device-backed masks
═══════════════════════════════════════════ */
const SEG_BIT = {
    A: 1 << 0,
    B: 1 << 1,
    C: 1 << 2,
    D: 1 << 3,
    E: 1 << 4,
    F: 1 << 5,
    G: 1 << 6,
};
const MASK_TO_CHAR = {
    0: ' ',
    63: '0',
    6: '1',
    91: '2',
    79: '3',
    102: '4',
    109: '5',
    125: '6',
    7: '7',
    127: '8',
    111: '9',
    64: '-',
    8: '_',
    72: '=',
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
    return 'rgb(' + rgb[0] + ',' + rgb[1] + ',' + rgb[2] + ')';
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
    if (!state || !state.digits) return '--:--';
    const chars = state.digits.map(function (mask) {
        return Object.prototype.hasOwnProperty.call(MASK_TO_CHAR, mask) ? MASK_TO_CHAR[mask] : '?';
    });
    return chars[0] + chars[1] + (state.colon ? ':' : ' ') + chars[2] + chars[3];
}
function renderDisplay(cid, opts) {
    const canvas = el(cid);
    if (!canvas) return;
    const ctx = canvas.getContext('2d');
    const W = canvas.width,
        H = canvas.height;
    const state = opts || S.display || {};
    const color = state.color || S.color;
    const brt = (state.brightness != null ? state.brightness : S.brightness) / 100;
    const colon = state.colon != null ? state.colon : S.colonOn;
    const digits = state.digits && state.digits.length === 4 ? state.digits : [0, 0, 0, 0];
    const digitColors = state.digit_colors || null;
    const colonColor = state.colon_color || null;
    ctx.clearRect(0, 0, W, H);
    ctx.fillStyle = '#050508';
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
    resizeCanvas('vd-canvas');
    resizeCanvas('prev-canvas');
    requestRender();
}

function requestRender() {
    renderDirty = true;
    if (renderQueued) return;
    renderQueued = true;
    requestAnimationFrame(function () {
        renderQueued = false;
        if (!renderDirty) return;
        renderDirty = false;
        renderDisplay('vd-canvas');
        renderDisplay('prev-canvas');
    });
}

/* ═════ PAGE / SECTION NAV ════════════════ */
/* Shared by showPage()/showSec() below — both swap which panel/nav-button
   pair is "active" and remember the choice, differing only in which
   selectors/prefixes/storage key they use. */
function activateTab(opts) {
    document.querySelectorAll(opts.panelSel).forEach(function (p) {
        p.classList.remove('active');
    });
    document.querySelectorAll(opts.navSel).forEach(function (b) {
        b.classList.remove('active');
    });
    if (opts.alsoClearNavSel) {
        document.querySelectorAll(opts.alsoClearNavSel).forEach(function (b) {
            b.classList.remove('active');
        });
    }
    const panel = el(opts.panelIdPrefix + opts.id);
    if (panel) panel.classList.add('active');
    const nb = document.querySelector(opts.navSel + '[' + opts.navAttr + '="' + opts.id + '"]');
    if (nb) nb.classList.add('active');
    const crumb = el('tb-current');
    if (crumb && nb) {
        const label = nb.querySelector('span');
        if (label) crumb.textContent = label.textContent;
    }
    try {
        localStorage.setItem(opts.storageKey, opts.id);
    } catch (e) {}
}
function showPage(id) {
    activateTab({
        panelSel: '.page',
        navSel: '.nb',
        alsoClearNavSel: '.snb',
        panelIdPrefix: 'pg-',
        navAttr: 'data-page',
        storageKey: 'wt-page',
        id: id,
    });
}
function showSec(id) {
    activateTab({
        panelSel: '.spane',
        navSel: '.snb',
        alsoClearNavSel: '.nb',
        panelIdPrefix: 'sec-',
        navAttr: 'data-s',
        storageKey: 'wt-sec',
        id: id,
    });
    if (id === 'display') resizeCanvas('prev-canvas');
}
function toggleSidebar() {
    const sb = el('sidebar');
    if (!sb) return;
    const collapsed = sb.classList.toggle('collapsed');
    try {
        localStorage.setItem('wt-sidebar', collapsed ? '1' : '0');
    } catch (e) {}
}
function initSidebarState() {
    try {
        if (localStorage.getItem('wt-sidebar') === '1') {
            const sb = el('sidebar');
            if (sb) sb.classList.add('collapsed');
        }
    } catch (e) {}
}
function toggleTheme() {
    const html = document.documentElement;
    const next = html.dataset.theme === 'dark' ? 'light' : 'dark';
    html.dataset.theme = next;
    try {
        localStorage.setItem('wt-theme', next);
    } catch (e) {}
}
function initTheme() {
    try {
        const t = localStorage.getItem('wt-theme');
        if (t) {
            document.documentElement.dataset.theme = t;
        } else if (window.matchMedia('(prefers-color-scheme: light)').matches) {
            document.documentElement.dataset.theme = 'light';
        }
    } catch (e) {}
}

function fmtTemperature(value) {
    return Number.isFinite(value) ? value.toFixed(1) + '°C' : '—';
}
function updateConnectionBadge() {
    const age = S.conn.lastSeen ? Date.now() - S.conn.lastSeen : Infinity;
    let state = S.conn.state;
    if (age > 1500) state = S.conn.lastSeen ? 'down' : 'pend';
    const dot = el('conn-dot');
    if (dot) dot.className = 'conn-dot ' + state;
    if (state === 'up') setText('conn-label', 'LIVE');
    else if (state === 'down') setText('conn-label', 'DISCONNECTED');
    else setText('conn-label', 'CONNECTING');
}
function markConnectionSeen() {
    S.conn.lastSeen = Date.now();
    S.conn.state = 'up';
    updateConnectionBadge();
}
function markConnectionLost() {
    if (!S.conn.lastSeen || Date.now() - S.conn.lastSeen > 1500) {
        S.conn.state = 'down';
        updateConnectionBadge();
    }
}
function applyFmtUi(n) {
    S.fmt = n;
    const b24 = el('fmt-24'),
        b12 = el('fmt-12');
    if (b24) b24.classList.toggle('active', n === 24);
    if (b12) b12.classList.toggle('active', n === 12);
}

/* updateDispInfo / syncDisplayModeInputs — pure UI helpers, kept */
function updateDispInfo() {
    const st = S.display;
    setText('dinfo-fmt', st && st.mode ? st.mode.toUpperCase() : S.displayMode.toUpperCase());
    setText(
        'dinfo-blink',
        st ? (st.colon_blink ? 'BLINK ON' : 'BLINK OFF') : S.blink ? 'BLINK ON' : 'BLINK OFF'
    );
    setText('dinfo-brt', 'BRT ' + S.brightness + '%');
    const dc = el('dinfo-color');
    if (dc) {
        dc.textContent = '● ' + S.color.toUpperCase();
        dc.style.color = S.color;
    }
}
function syncDisplayModeInputs() {
    const mode = fieldStr('dp-mode', null) || S.displayMode || 'time';
    const vw = el('dp-value-wrap');
    const tw = el('dp-text-wrap');
    if (vw) vw.style.display = mode === 'number' ? '' : 'none';
    if (tw) tw.style.display = mode === 'text' ? '' : 'none';
}
/* manualDisplayRefresh — sends a ping; the next WS push (≤500 ms) carries fresh display state */
function manualDisplayRefresh() {
    wsSend('ping', {});
    toast('Refreshing…', 'info', 800);
}

function setBar(id, pct, labelId) {
    const f = el(id + '-bar');
    if (f) f.style.width = Math.min(100, Math.round(pct)) + '%';
    if (labelId) setText(labelId, Math.round(pct) + '%');
}

function doRefresh(silent) {
    wsSend('ping', {});
    if (!silent) toast('Data refreshed', 'ok', 1200);
}

/* ═════ LOGS  /api/logs?seq=N ════════════ */
function detectLevel(line) {
    if (/\[E\]|ERROR/.test(line)) return 'ERROR';
    if (/\[W\]|WARN/i.test(line)) return 'WARN';
    if (/\[I\]|INFO/i.test(line)) return 'INFO';
    return 'DEBUG';
}
function renderLogLine(line, cid) {
    const c = el(cid);
    if (!c) return;
    const lv = detectLevel(line);
    if (S.logFilter !== 'ALL' && lv !== S.logFilter) return;
    const div = document.createElement('div');
    div.className = 'le ' + lv[0];
    div.textContent = line;
    c.appendChild(div);
    while (c.children.length > 500) c.removeChild(c.firstChild);
    const as = el('log-as');
    if (!as || as.checked) c.scrollTop = c.scrollHeight;
    setText('log-count', S.logs.length + ' entries');
}
function addLogEntry(entry) {
    const line =
        typeof entry === 'string'
            ? entry
            : '[' +
              (entry.lvl || 'I') +
              '] ' +
              (entry.tag ? entry.tag + ': ' : '') +
              (entry.msg || '');
    S.logs.push(line);
    if (S.logs.length > 2000) S.logs.shift();
    renderLogLine(line, 'log-viewer');
    renderLogLine(line, 'dash-logs');
}
function setLogFilter(lv) {
    S.logFilter = lv;
    document.querySelectorAll('.lfb').forEach(function (b) {
        b.classList.toggle('active', b.dataset.lv === lv);
    });
    const v = el('log-viewer');
    if (v) {
        v.innerHTML = '';
        S.logs.forEach(function (l) {
            renderLogLine(l, 'log-viewer');
        });
    }
}
function clearLogs() {
    const v = el('log-viewer');
    if (v) v.innerHTML = '';
}
function exportLogs() {
    const blob = new Blob([S.logs.join('\n')], { type: 'text/plain' });
    const a = document.createElement('a');
    a.href = URL.createObjectURL(blob);
    a.download = 'wt-logs-' + Date.now() + '.txt';
    a.click();
}

/* ═════ DISPLAY SETTINGS ════════════════ */
function onDispChange() {
    S.color = fieldStr('dp-color', '#e8e4de');
    S.brightness = fieldNum('dp-brt', 80);
    S.blink = fieldBool('dp-blink', true);
    S.scroll = fieldBool('dp-scroll', false);
    S.pulse = fieldBool('dp-pulse', false);
    S.transition = fieldBool('dp-trans', true);
    S.displayMode = fieldStr('dp-mode', 'time');
    S.displayValue = clamp(fieldNum('dp-value', 0) || 0, 0, 9999);
    S.displayText = fieldStr('dp-text', '')
        .toUpperCase()
        .replace(/[^A-Z0-9 _-]/g, '')
        .slice(0, 4);
    updateBrightnessUi(S.brightness);
    if (el('dp-value')) el('dp-value').value = String(S.displayValue);
    if (el('dp-text')) el('dp-text').value = S.displayText;
    syncDisplayModeInputs();
    markActiveSwatch();
    updateDispInfo();
    debounce('disp', sendDispSettings, 600);
}
async function sendDispSettings() {
    const r = await apiCmd('settings', {
        color: S.color,
        brightness: S.brightness,
        anim_colon: S.blink,
        anim_scroll: S.scroll,
        anim_pulse: S.pulse,
        anim_transition: S.transition,
        reaction_effect: fieldStr('dp-react', 'none'),
        display_mode: S.displayMode,
        display_value: S.displayValue,
        display_text: S.displayText,
    });
    if (r && r.status === 'ok') toast('Display updated', 'ok');
}
async function forceSendDispSettings() {
    clearTimeout(_db.disp);
    onDispChange();
    clearTimeout(_db.disp);
    await sendDispSettings();
}
function pickColor(hex) {
    const p = el('dp-color');
    if (p) p.value = hex;
    onDispChange();
}
function markActiveSwatch() {
    const target = (S.color || '').toLowerCase();
    document.querySelectorAll('.swatches .sw').forEach(function (sw) {
        const c = (sw.style.background || sw.style.backgroundColor || '')
            .replace(/\s+/g, '')
            .toLowerCase();
        sw.classList.toggle('active', c === target);
    });
}

/* ═════ CLOCK SETTINGS ══════════════════ */
function setFmt(n) {
    applyFmtUi(n);
    onClockChange();
}
function onClockChange() {
    debounce('clock', sendClockSettings, 600);
}
async function sendClockSettings() {
    const r = await apiCmd('settings', {
        time_format: S.fmt,
        timezone: fieldStr('tz-sel', 'UTC0'),
        ntp_server: fieldStr('ntp-srv', 'pool.ntp.org'),
        alarm1_time: fieldStr('al1-t', '07:00'),
        alarm1_en: fieldBool('al1-en', false),
        alarm2_time: fieldStr('al2-t', '22:00'),
        alarm2_en: fieldBool('al2-en', false),
        notif_type: fieldStr('notif-type', 'flash'),
        notif_sound: fieldStr('notif-sound', 'beep'),
    });
    if (r && r.status === 'ok') toast('Clock saved', 'ok');
}
async function syncNTP() {
    const srv = fieldStr('ntp-srv', 'pool.ntp.org');
    const r = await apiCmd('ntp_sync', { server: srv });
    if (r) {
        toast('NTP sync triggered', 'ok');
        setText('ntp-hint', 'Last sync: ' + new Date().toLocaleTimeString());
    } else toast('NTP sync failed', 'err');
}
async function rebootDevice() {
    const ok = window.confirm('Reboot the device now?');
    if (!ok) return;
    const r = await apiCmd('reboot', {});
    if (r && r.status === 'ok') toast('Rebooting device…', 'ok', 2200);
    else toast('Reboot failed', 'err');
}

function onPowerChange() {
    debounce('power', sendPowerSettings, 600);
}
async function sendPowerSettings() {
    const r = await apiCmd('settings', {
        sleep_mode: fieldStr('sl-mode', 'none'),
        sleep_timeout: fieldNum('sl-timeout', 30),
        batt_alert_pct: fieldNum('batt-alert', 20),
        ps_dim: fieldBool('ps-dim', true),
        ps_wifi: fieldBool('ps-wifi', false),
    });
    if (r && r.status === 'ok') toast('Power settings saved', 'ok');
}

function renderProfiles(profiles) {
    const c = el('profiles-list');
    if (!c) return;
    if (!profiles.length) {
        c.innerHTML = '<div class="empty">No saved profiles</div>';
        return;
    }
    c.innerHTML = '';
    profiles.forEach(function (p, i) {
        const active = S.wifi && S.wifi.ssid === p.ssid;
        const div = document.createElement('div');
        div.className = 'pitem' + (active ? ' active-net' : '');
        div.innerHTML =
            '<span class="pi-name">' +
            esc(p.ssid) +
            '</span>' +
            (active ? '<span class="pi-badge">CONNECTED</span>' : '') +
            '<div class="pi-actions">' +
            (!active
                ? '<button class="bsm" onclick="connectProfile(' + i + ')">Connect</button>'
                : '') +
            '<button class="pi-del" onclick="delProfile(' +
            i +
            ')">Remove</button>' +
            '</div>';
        c.appendChild(div);
    });
}
function showAddWifi() {
    const f = el('add-wifi-form');
    if (f) f.classList.remove('hidden');
}
function hideAddWifi() {
    const f = el('add-wifi-form');
    if (f) f.classList.add('hidden');
    if (el('new-ssid')) el('new-ssid').value = '';
    if (el('new-pass')) el('new-pass').value = '';
}
async function addProfile() {
    const ssid = fieldStr('new-ssid', '').trim();
    const pass = fieldStr('new-pass', '');
    if (!ssid) {
        toast('SSID required', 'warn');
        return;
    }
    if (wsSend('wifi', { op: 'add', ssid: ssid, password: pass })) {
        toast('"' + ssid + '" added', 'ok');
        hideAddWifi();
    } else {
        toast('Failed — not connected', 'err');
    }
}
async function delProfile(idx) {
    if (wsSend('wifi', { op: 'del', index: idx })) toast('Profile removed', 'ok');
    else toast('Failed — not connected', 'err');
}
async function connectProfile(idx) {
    if (wsSend('wifi', { op: 'connect', index: idx })) toast('Connecting…', 'ok');
    else toast('Connect failed — not connected', 'err');
}
function togglePw() {
    const f = el('new-pass');
    if (f) f.type = f.type === 'password' ? 'text' : 'password';
}

/* ═════ OTA ══════════════════════════════ */
function uploadFW(file) {
    if (!file) return;
    if (!file.name.endsWith('.bin')) {
        toast('Need .bin file', 'warn');
        return;
    }
    doUpload(
        file,
        '/api/ota?target=firmware',
        'fw-prog',
        'fw-fill',
        'fw-pct',
        function () {
            toast('Firmware updated! Rebooting…', 'ok');
        },
        function () {
            toast('Upload failed', 'err');
        }
    );
}
function uploadWA(file) {
    if (!file) return;
    var allowed = ['index.html', 'style.css', 'app.js'];
    if (allowed.indexOf(file.name) === -1) {
        toast('Must be index.html, style.css or app.js', 'warn');
        return;
    }
    doUpload(
        file,
        '/api/ota?target=' + encodeURIComponent(file.name),
        'wa-prog',
        'wa-fill',
        'wa-pct',
        function () {
            toast(file.name + ' updated!', 'ok');
        },
        function () {
            toast('Upload failed', 'err');
        }
    );
}
function doUpload(file, url, progId, fillId, pctId, onOk, onErr) {
    const pw = el(progId),
        pf = el(fillId);
    if (pw) pw.classList.remove('hidden');
    const xhr = new XMLHttpRequest();
    xhr.open('POST', url);
    xhr.upload.onprogress = function (e) {
        if (!e.lengthComputable) return;
        const p = Math.round((e.loaded / e.total) * 100);
        if (pf) pf.style.width = p + '%';
        setText(pctId, p + '%');
    };
    xhr.onload = function () {
        (xhr.status === 200 ? onOk : onErr)();
    };
    xhr.onerror = onErr;
    const fd = new FormData();
    fd.append('file', file);
    xhr.send(fd);
}
function setupDnD() {
    ['fw-dz', 'wa-dz'].forEach(function (id) {
        const z = el(id);
        if (!z) return;
        z.addEventListener('dragover', function (e) {
            e.preventDefault();
            z.classList.add('drag-over');
        });
        z.addEventListener('dragleave', function () {
            z.classList.remove('drag-over');
        });
        z.addEventListener('drop', function (e) {
            e.preventDefault();
            z.classList.remove('drag-over');
            const f = e.dataTransfer.files[0];
            if (id === 'fw-dz') uploadFW(f);
            else uploadWA(f);
        });
    });
}

function initNavState() {
    let page = 'dashboard';
    let sec = 'sys';
    try {
        page = localStorage.getItem('wt-page') || page;
        sec = localStorage.getItem('wt-sec') || sec;
    } catch (e) {}
    showPage(page === 'settings' ? 'settings' : 'dashboard');
    if (page === 'settings') {
        const target = document.querySelector('.snb[data-s="' + sec + '"]') ? sec : 'sys';
        showSec(target);
    }
}

/* ═════ WebSocket client ════════════════ */

/* Send a command frame to the device over the WebSocket.
   Returns true if the frame was queued successfully. */
function wsSend(cmd, data) {
    if (!S.ws || S.ws.readyState !== WebSocket.OPEN) return false;
    try {
        S.ws.send(JSON.stringify(Object.assign({ cmd: cmd }, data || {})));
        return true;
    } catch (e) {
        console.warn('wsSend failed:', e);
        return false;
    }
}

/* WS-only command sender. Returns {status:"ok"} optimistically, or null if not connected. */
async function apiCmd(cmd, data) {
    if (wsSend(cmd, data)) {
        markConnectionSeen();
        return { status: 'ok' };
    }
    console.warn('apiCmd: WebSocket not ready, cmd dropped:', cmd);
    toast('Not connected', 'err', 1500);
    return null;
}

function handleWsMessage(d) {
    if (!d) return;

    /* ── One-time frame sent right after the WS handshake ─────────── */
    if (d.type === 'hello') {
        console.info('WT hello: proto=' + d.proto + ' features=' + (d.features || []).join(','));
        return;
    }

    /* ── One-time boot-constant info (type:"info") — chip identity, build
        info, OTA slot, reset reason. Never repeats, so these DOM targets
        are only ever touched once per connection instead of every "full"
        tick. ──────────────────────────────────────────────────────── */
    if (d.type === 'info') {
        if (d.chip_model) setText('i-chip', d.chip_model);
        if (d.cpu_cores) setText('i-cores', d.cpu_cores);
        if (d.cpu_freq_mhz) setText('i-freq', d.cpu_freq_mhz + ' MHz');
        if (d.flash_size) setText('i-flash', fmtBytes(d.flash_size));
        if (d.reset_reason) setText('i-reset', d.reset_reason);
        if (d.app_version) {
            setText('d-fw', d.app_version);
            setText('fw-ver', d.app_version);
        }
        if (d.build_date) setText('fw-date', d.build_date);
        if (d.idf_version) setText('fw-idf', d.idf_version);
        if (d.ota_slot) setText('fw-slot', d.ota_slot);
        if (d.app0_state) setText('fw-app0', d.app0_state);
        if (d.app1_state) setText('fw-app1', d.app1_state);
        return;
    }

    /* ── Fast path: display-only frame (type:"disp", 20 fps) ──────── */
    if (d.type === 'disp') {
        if (d.display && d.display.available !== false) {
            S.display = d.display;
            if (!_db.disp) {
                if (d.display.mode) S.displayMode = d.display.mode;
                if (d.display.brightness != null) S.brightness = d.display.brightness;
            }
            setText('live-time', displayText(d.display));
            updateDispInfo();
            requestRender();
        }
        return;
    }
    /* ── Full-state frame (type:"full", 1 fps) — fall through ─────── */
    if (d.uptime_s != null) {
        setText('d-uptime', fmtUptime(d.uptime_s));
        setText('i-uptime', fmtUptime(d.uptime_s));
    }
    if (d.free_heap != null) {
        setText('d-heap', fmtBytes(d.free_heap));
        setText('i-heap', fmtBytes(d.free_heap));
    }
    if (d.rssi != null) setText('d-rssi', d.rssi + ' dBm');
    if (d.sta_ip || d.ap_ip) setText('d-ip', d.sta_ip || d.ap_ip || '—');
    if (d.temperature != null) {
        setText('d-temp', fmtTemperature(d.temperature));
        setText('i-temp', fmtTemperature(d.temperature));
    }
    if (d.sta_connected != null) {
        const dot = el('wdot'),
            lbl = el('wifi-label');
        if (dot) dot.className = 'wdot ' + (d.sta_connected ? 'up' : 'down');
        if (lbl) lbl.textContent = d.sta_connected ? d.sta_ssid || 'Connected' : 'Offline';
    }

    /* ── System info tab ── */
    if (d.total_heap && d.free_heap) {
        const ramPct = Math.round((1 - d.free_heap / d.total_heap) * 100);
        setBar('ram', ramPct, 'ram-pct');
    }
    setBar('cpu', d.cpu_usage || 0, 'cpu-pct');
    setBar('flash', d.flash_used_pct || 0, 'flash-pct');
    setBar('spiffs', d.spiffs_used_pct || 0, 'spiffs-pct');
    if (d.min_free_heap) setText('i-minheap', fmtBytes(d.min_free_heap));

    /* ── Firmware / OTA tab ── */
    if (d.spiffs_used_pct != null) setText('fw-spiffs', d.spiffs_used_pct + '%');

    /* ── Display state ── */
    if (d.display && d.display.available !== false) {
        S.display = d.display;
        /* Only sync display-derived S.* fields when no edit is in flight */
        if (!_db.disp) {
            if (d.display.mode) S.displayMode = d.display.mode;
            if (d.display.brightness != null) S.brightness = d.display.brightness;
        }
        setText('live-time', displayText(d.display));
        updateDispInfo();
        requestRender();
    }

    /* ── WiFi status + profiles ── */
    if (d.wifi) {
        const w = d.wifi;
        S.wifi = w;
        const dot = el('wsc-dot');
        if (dot) dot.className = 'wsc-dot ' + (w.connected ? 'on' : 'off');
        setText('wsc-ssid', w.connected ? w.ssid || '—' : 'Not connected');
        setText(
            'wsc-detail',
            w.connected
                ? (w.ip || '—') +
                      ' · ' +
                      (w.rssi || '—') +
                      ' dBm' +
                      (w.channel ? ' · Ch' + w.channel : '')
                : 'Searching…'
        );
        const ic = el('rssi-ic'),
            rv = el('rssi-v');
        if (rv) rv.textContent = w.rssi ? w.rssi + ' dBm' : '— dBm';
        if (ic) {
            let s = 0;
            if (w.rssi >= -55) s = 4;
            else if (w.rssi >= -65) s = 3;
            else if (w.rssi >= -75) s = 2;
            else if (w.rssi) s = 1;
            ic.className = 'rssi-ic' + (s ? ' s' + s : '');
        }
        const hdot = el('wdot'),
            hlbl = el('wifi-label');
        if (hdot) hdot.className = 'wdot ' + (w.connected ? 'up' : 'down');
        if (hlbl) hlbl.textContent = w.connected ? w.ssid || 'Connected' : 'Offline';
        renderProfiles(w.profiles || []);
    }

    /* ── Power / battery ── */
    if (d.power) {
        const p = d.power;
        const pct = p.battery_pct || 0;
        const bar = el('batt-fill');
        if (bar) {
            bar.style.width = Math.min(100, pct) + '%';
            bar.className = 'batt-fill' + (pct < 20 ? ' low' : pct < 50 ? ' mid' : '');
        }
        setText('batt-txt', pct + '%');
        setText('batt-v', p.voltage ? p.voltage.toFixed(2) + 'V' : '—V');
        setText('batt-ma', p.current ? p.current + 'mA' : '—mA');
        setText('pwr-src', p.source || '—');
        setText('batt-eta', p.eta_hours ? p.eta_hours.toFixed(1) + 'h' : '—h');
        if (p.sleep_mode != null) setValueIfIdle('sl-mode', p.sleep_mode);
        if (p.sleep_timeout != null) setValueIfIdle('sl-timeout', String(p.sleep_timeout));
        if (p.batt_alert_pct != null) setValueIfIdle('batt-alert', String(p.batt_alert_pct));
        if (p.ps_dim != null) setCheckedIfIdle('ps-dim', p.ps_dim);
        if (p.ps_wifi != null) setCheckedIfIdle('ps-wifi', p.ps_wifi);
    }

    /* ── Settings (only apply when inputs aren't actively focused
        AND no user edit is pending for that group)              ── */
    if (d.settings) {
        const s = d.settings;

        /* Display group — skip entirely if user is mid-edit */
        if (!_db.disp) {
            if (s.color && !isFocused('dp-color')) {
                S.color = s.color;
                setValueIfIdle('dp-color', s.color);
            }
            if (s.brightness != null && !isFocused('dp-brt')) {
                S.brightness = s.brightness;
                updateBrightnessUi(s.brightness);
            }
            if (s.display_mode && !isFocused('dp-mode')) {
                S.displayMode = s.display_mode;
                setValueIfIdle('dp-mode', s.display_mode);
            }
            if (s.display_value != null && !isFocused('dp-value')) {
                S.displayValue = s.display_value;
                setValueIfIdle('dp-value', String(s.display_value));
            }
            if (s.display_text != null && !isFocused('dp-text')) {
                S.displayText = s.display_text;
                setValueIfIdle('dp-text', s.display_text);
            }
            if (s.anim_colon != null && !isFocused('dp-blink')) {
                S.blink = s.anim_colon;
                setCheckedIfIdle('dp-blink', s.anim_colon);
            }
            if (s.anim_scroll != null && !isFocused('dp-scroll')) {
                S.scroll = s.anim_scroll;
                setCheckedIfIdle('dp-scroll', s.anim_scroll);
            }
            if (s.anim_pulse != null && !isFocused('dp-pulse')) {
                S.pulse = s.anim_pulse;
                setCheckedIfIdle('dp-pulse', s.anim_pulse);
            }
            if (s.anim_transition != null && !isFocused('dp-trans')) {
                S.transition = s.anim_transition;
                setCheckedIfIdle('dp-trans', s.anim_transition);
            }
            if (s.reaction_effect) setValueIfIdle('dp-react', s.reaction_effect);
            markActiveSwatch();
            syncDisplayModeInputs();
        }

        /* Clock group — skip entirely if user is mid-edit */
        if (!_db.clock) {
            if (s.time_format) applyFmtUi(s.time_format);
            if (s.timezone) setValueIfIdle('tz-sel', s.timezone);
            if (s.ntp_server) setValueIfIdle('ntp-srv', s.ntp_server);
            if (s.notif_type) setValueIfIdle('notif-type', s.notif_type);
            if (s.notif_sound) setValueIfIdle('notif-sound', s.notif_sound);
            if (s.alarm1_time) setValueIfIdle('al1-t', s.alarm1_time);
            if (s.alarm2_time) setValueIfIdle('al2-t', s.alarm2_time);
            if (s.alarm1_en != null) setCheckedIfIdle('al1-en', s.alarm1_en);
            if (s.alarm2_en != null) setCheckedIfIdle('al2-en', s.alarm2_en);
        }

        /* Power group — skip entirely if user is mid-edit */
        if (!_db.power) {
            if (s.ps_dim != null) setCheckedIfIdle('ps-dim', s.ps_dim);
            if (s.ps_wifi != null) setCheckedIfIdle('ps-wifi', s.ps_wifi);
            if (s.sleep_mode) setValueIfIdle('sl-mode', s.sleep_mode);
            if (s.sleep_timeout != null) setValueIfIdle('sl-timeout', String(s.sleep_timeout));
            if (s.batt_alert_pct != null) setValueIfIdle('batt-alert', String(s.batt_alert_pct));
        }
    }

    /* ── Incremental log entries — device tracks the seq cursor itself and
        only ever sends entries newer than what it last broadcast, so the
        client just appends whatever arrives. ──────────────────────── */
    if (d.logs?.entries?.length) {
        d.logs.entries.forEach(addLogEntry);
        setText('log-ts', 'Updated ' + new Date().toLocaleTimeString());
    }

    updateDispInfo();
    updateConnectionBadge();
}

function clearWsWatchdog() {
    if (S.wsWatchdog) {
        clearTimeout(S.wsWatchdog);
        S.wsWatchdog = null;
    }
}
/* Rearmed on every inbound message. If it ever fires, the socket has gone
   quiet without telling the browser (e.g. a half-open TCP connection) —
   force a close so the normal onclose → reconnect path takes over instead
   of the UI sitting on stale data indefinitely. */
function armWsWatchdog() {
    clearWsWatchdog();
    S.wsWatchdog = setTimeout(function () {
        console.warn('WS watchdog: no data for ' + WS_WATCHDOG_MS + 'ms — forcing reconnect');
        if (S.ws) {
            try {
                S.ws.close();
            } catch (_) {}
        }
    }, WS_WATCHDOG_MS);
}
function scheduleReconnect() {
    setTimeout(initWebSocket, S.wsRetryMs);
    S.wsRetryMs = Math.min(S.wsRetryMs * 2, WS_RETRY_MAX_MS);
}

function initWebSocket() {
    /* Close any previous socket */
    if (S.ws) {
        try {
            S.ws.close();
        } catch (_) {}
        S.ws = null;
    }
    clearWsWatchdog();

    const proto = location.protocol === 'https:' ? 'wss:' : 'ws:';
    const url = proto + '//' + location.host + '/ws';
    let ws;
    try {
        ws = new WebSocket(url);
    } catch (e) {
        console.warn('WebSocket init error:', e);
        scheduleReconnect();
        return;
    }

    S.ws = ws;

    ws.onopen = function () {
        S.wsConnected = true;
        S.wsRetryMs = WS_RETRY_MIN_MS; /* reset backoff on a successful connect */
        markConnectionSeen();
        armWsWatchdog();
        console.info('WS connected →', url);
    };

    ws.onmessage = function (evt) {
        markConnectionSeen();
        armWsWatchdog();
        try {
            handleWsMessage(JSON.parse(evt.data));
        } catch (e) {
            console.warn('WS parse error:', e);
        }
    };

    ws.onerror = function (e) {
        console.warn('WS error:', e);
    };

    ws.onclose = function () {
        S.wsConnected = false;
        S.ws = null;
        clearWsWatchdog();
        markConnectionLost();
        console.info('WS closed — retrying in', S.wsRetryMs, 'ms');
        scheduleReconnect();
    };
}

/* ═════ INIT ═════════════════════════════ */
function init() {
    initTheme();
    initSidebarState();
    setupDnD();
    updateConnectionBadge();
    window.addEventListener('resize', function () {
        resizeAll();
    });
    initNavState();
    resizeAll();
    requestRender();
    initWebSocket();
    S.timers.conn = setInterval(updateConnectionBadge, 250);
}
document.addEventListener('DOMContentLoaded', init);
