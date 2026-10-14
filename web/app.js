/* WatchTower web console. Everything except uploads goes over one WebSocket. */
'use strict';

const $ = (id) => document.getElementById(id);
const COLORS = ['#ffffff', '#00c8ff', '#00ff99', '#ffdd57', '#ff8c00', '#ff3860', '#b48eff'];
const RETRY_MIN = 1000,
    RETRY_MAX = 8000;
const SILENT_MS = 5000; // no message for this long: the connection is dead
const HIDDEN_MS = 30000; // a hidden page lets its connection go, freeing it for others
const EDIT_HOLD_MS = 1500; // a field edited this recently is not overwritten by the device
const LOG_MAX = 500;

const S = {
    ws: null,
    retry: RETRY_MIN,
    timers: {},
    display: null,
    wifi: null,
    scan: { scanning: false, networks: [], done: false },
    settings: {},
    pending: {},
    edited: {},
    logs: [],
    logLevel: '',
    minLeds: 58,
    maxLeds: 100,
};

/* ---- Helpers ---- */
function text(id, v) {
    const e = $(id);
    if (e && e.textContent !== String(v)) e.textContent = v;
}
function esc(s) {
    return String(s).replace(/[&<>"']/g, (c) => '&#' + c.charCodeAt(0) + ';');
}
function toast(msg, err) {
    const t = document.createElement('div');
    t.className = 'toast' + (err ? ' err' : '');
    t.textContent = msg;
    $('toasts').appendChild(t);
    setTimeout(() => t.remove(), err ? 4000 : 2400);
}
function bytes(b) {
    if (b == null) return '–';
    return b < 1048576 ? (b / 1024).toFixed(0) + ' KB' : (b / 1048576).toFixed(1) + ' MB';
}
function uptime(s) {
    const d = Math.floor(s / 86400),
        h = Math.floor((s % 86400) / 3600),
        m = Math.floor((s % 3600) / 60);
    return d ? d + 'd ' + h + 'h' : h ? h + 'h ' + m + 'm' : m + 'm ' + (s % 60) + 's';
}
function meter(id, pct) {
    pct = Math.max(0, Math.min(100, Math.round(pct)));
    $(id).style.width = pct + '%';
    text(id + '-v', pct + '%');
}
function bars(rssi) {
    const s = rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 : 1;
    return '<span class="bars s' + s + '"><i></i><i></i><i></i><i></i></span>';
}

/* ---- Commands ---- */
function send(cmd, data) {
    if (!S.ws || S.ws.readyState !== WebSocket.OPEN) {
        toast('Not connected to the device', true);
        return false;
    }
    S.ws.send(JSON.stringify(Object.assign({ cmd: cmd }, data)));
    return true;
}
/* Settings are sent one field at a time, so two people changing different
   things do not undo each other. Changes in quick succession (a slider
   being dragged) are combined into one message. */
function setSetting(key, value, delay) {
    S.pending[key] = value;
    S.edited[key] = Date.now();
    S.settings[key] = value;
    clearTimeout(S.timers.settings);
    S.timers.settings = setTimeout(
        () => {
            const data = S.pending;
            S.pending = {};
            send('settings', data);
        },
        delay == null ? 120 : delay
    );
}
function wifi(op, data, okMsg) {
    if (send('wifi', Object.assign({ op: op }, data)) && okMsg) toast(okMsg);
}

/* ---- Display preview ---- */
// Segment bit order a-g, matching the firmware.
const SEGS = [
    [0.5, 0, 'h'],
    [1, 0.25, 'v'],
    [1, 0.75, 'v'],
    [0.5, 1, 'h'],
    [0, 0.75, 'v'],
    [0, 0.25, 'v'],
    [0.5, 0.5, 'h'],
];
function rgb(c, a) {
    return 'rgba(' + c[0] + ',' + c[1] + ',' + c[2] + ',' + (a == null ? 1 : a) + ')';
}
function drawDisplay() {
    S.timers.frame = 0;
    const cv = $('disp');
    if (!cv.offsetWidth) return;
    const dpr = window.devicePixelRatio || 1;
    const W = Math.round(cv.offsetWidth * dpr),
        H = Math.round(cv.offsetHeight * dpr);
    if (cv.width !== W || cv.height !== H) {
        cv.width = W;
        cv.height = H;
    }
    const ctx = cv.getContext('2d');
    ctx.clearRect(0, 0, W, H);

    const d = S.display || { digits: [0, 0, 0, 0], colon: false, color_on: [80, 80, 80] };
    const on = d.color_on || [255, 255, 255];
    const alpha = 0.35 + 0.65 * ((d.brightness == null ? 100 : d.brightness) / 100);
    const ghost = 'rgba(255,255,255,0.05)';

    // Four digits and a colon, scaled to fit the canvas.
    const dh = H * 0.86,
        dw = dh * 0.56,
        gap = dw * 0.28,
        cw = dw * 0.45;
    const total = dw * 4 + gap * 2 + cw;
    const scale = Math.min(1, W / (total * 1.06));
    const DH = dh * scale,
        DW = dw * scale,
        G = gap * scale,
        CW = cw * scale;
    const t = DW * 0.16; // segment thickness
    let x = (W - (DW * 4 + G * 2 + CW)) / 2;
    const y = (H - DH) / 2;

    for (let i = 0; i < 4; i++) {
        const mask = (d.digits && d.digits[i]) || 0;
        for (let s = 0; s < 7; s++) {
            const lit = (mask >> s) & 1;
            const own = d.digit_colors && d.digit_colors[i] && d.digit_colors[i][s];
            ctx.fillStyle = own
                ? own[0] + own[1] + own[2] > 24
                    ? rgb(own)
                    : ghost
                : lit
                  ? rgb(on, alpha)
                  : ghost;
            const [fx, fy, dir] = SEGS[s];
            const cx = x + fx * (DW - t) + t / 2,
                cy = y + fy * (DH - t) + t / 2;
            const len = dir === 'h' ? DW - t * 1.4 : DH / 2 - t * 1.1;
            segment(ctx, cx, cy, len, t, dir === 'v');
        }
        x += DW + (i === 1 ? 0 : G);
        if (i === 1) {
            ctx.fillStyle = d.colon ? rgb(d.colon_color || on, d.colon_color ? 1 : alpha) : ghost;
            for (const fy of [0.32, 0.68]) {
                ctx.beginPath();
                ctx.arc(x + CW / 2, y + DH * fy, t * 0.55, 0, Math.PI * 2);
                ctx.fill();
            }
            x += CW;
        }
    }
}
// A hexagonal segment centred on (cx, cy).
function segment(ctx, cx, cy, len, t, vertical) {
    const h = len / 2,
        w = t / 2;
    ctx.save();
    ctx.translate(cx, cy);
    if (vertical) ctx.rotate(Math.PI / 2);
    ctx.beginPath();
    ctx.moveTo(-h, 0);
    ctx.lineTo(-h + w, -w);
    ctx.lineTo(h - w, -w);
    ctx.lineTo(h, 0);
    ctx.lineTo(h - w, w);
    ctx.lineTo(-h + w, w);
    ctx.closePath();
    ctx.fill();
    ctx.restore();
}
function redraw() {
    if (!S.timers.frame) S.timers.frame = requestAnimationFrame(drawDisplay);
}

/* ---- Showing device state ---- */
// Write a value into a field unless the user is editing it.
function field(id, key, value) {
    const e = $(id);
    if (!e || value == null) return;
    if (document.activeElement === e || Date.now() - (S.edited[key] || 0) < EDIT_HOLD_MS) return;
    if (e.type === 'checkbox') e.checked = !!value;
    else if (String(e.value) !== String(value)) e.value = value;
}
function segValue(id, value) {
    document
        .querySelectorAll('#' + id + ' button')
        .forEach((b) => b.classList.toggle('on', b.dataset.v === String(value)));
}
function showSettings() {
    const s = S.settings;
    segValue('mode', s.display_mode);
    $('row-number').hidden = s.display_mode !== 'number';
    $('row-text').hidden = s.display_mode !== 'text';
    field('value', 'display_value', s.display_value);
    field('text', 'display_text', s.display_text);
    field('blink', 'anim_colon', s.anim_colon);
    field('brt', 'brightness', s.brightness);
    text('brt-v', (s.brightness == null ? '–' : s.brightness) + '%');
    field('fx-en', 'bg_effect_en', s.bg_effect_en);
    field('fx', 'anim_effect', s.anim_effect);
    $('fx').disabled = !s.bg_effect_en;
    field('leds', 'led_count', s.led_count);
    document
        .querySelectorAll('#swatches button')
        .forEach((b) => b.classList.toggle('on', b.dataset.c === (s.color || '').toLowerCase()));
    segValue('fmt', s.time_format);
    if (s.timezone && !$('tz').querySelector('option[value="' + CSS.escape(s.timezone) + '"]')) {
        $('tz').add(new Option(s.timezone, s.timezone));
    }
    field('tz', 'timezone', s.timezone);
    field('ntp', 'ntp_server', s.ntp_server);
}
function showWifi() {
    const w = S.wifi;
    text('n-ssid', w.connected ? w.ssid : 'Not connected');
    text('n-ip', w.connected ? w.ip : '–');
    text('n-rssi', w.connected ? w.rssi + ' dBm' : '–');
    text('n-addr', w.hostname ? w.hostname + '.local' : '–');
    text(
        'n-ap',
        w.ap_active ? w.ap_ssid + ' · ' + w.ap_ip + ' · ' + w.ap_clients + ' joined' : 'Off'
    );
    text('hdr-name', w.hostname || 'WatchTower');
    field('host', 'hostname', w.hostname);

    const list = w.profiles || [];
    $('saved').innerHTML = list.length
        ? list
              .map((p, i) => {
                  const cur = w.connected && w.ssid === p.ssid;
                  return (
                      '<li><span class="name">' +
                      esc(p.ssid) +
                      '</span>' +
                      (cur
                          ? '<span class="tag">Connected</span>'
                          : '<button class="btn small ghost" data-connect="' +
                            i +
                            '">Connect</button>') +
                      '<button class="btn small ghost" data-forget="' +
                      i +
                      '">Forget</button></li>'
                  );
              })
              .join('')
        : '<li class="empty">None yet. The device can always be reached on its own access point.</li>';
    showScan();
}
function showScan() {
    const btn = $('scan');
    btn.disabled = S.scan.scanning;
    btn.textContent = S.scan.scanning ? 'Scanning…' : 'Scan';
    const nets = S.scan.networks;
    if (!nets.length) {
        $('scan-list').innerHTML =
            '<li class="empty">' +
            (S.scan.scanning
                ? 'Looking for networks…'
                : S.scan.done
                  ? 'No networks found.'
                  : 'Press Scan to look for networks.') +
            '</li>';
        return;
    }
    const w = S.wifi || {};
    $('scan-list').innerHTML = nets
        .map((n, i) => {
            const cur = w.connected && w.ssid === n.ssid;
            return (
                '<li>' +
                bars(n.rssi) +
                '<span class="name">' +
                esc(n.ssid) +
                '<br><span class="sub">' +
                (n.secure ? 'Secured' : 'Open') +
                ' · ' +
                n.rssi +
                ' dBm</span></span>' +
                (cur
                    ? '<span class="tag">Connected</span>'
                    : '<button class="btn small ghost" data-pick="' + i + '">Join</button>') +
                '</li>'
            );
        })
        .join('');
}
function addLog(line) {
    // Lines look like "14-10-2026 08:41:02.123 [WARN] message".
    const m = /\[(E|W)[A-Z]*\]/.exec(line);
    const lvl = m ? m[1] : 'I';
    S.logs.push({ line: line, lvl: lvl });
    if (S.logs.length > LOG_MAX) S.logs.shift();
    if (!S.logLevel || S.logLevel === lvl) {
        const box = $('log');
        const atEnd = box.scrollHeight - box.scrollTop - box.clientHeight < 40;
        const div = document.createElement('div');
        div.className = lvl;
        div.textContent = line;
        box.appendChild(div);
        while (box.childNodes.length > LOG_MAX) box.firstChild.remove();
        if (atEnd) box.scrollTop = box.scrollHeight;
    }
}
function showLogs() {
    $('log').innerHTML = '';
    const keep = S.logs;
    S.logs = [];
    keep.forEach((l) => addLog(l.line));
}

function onMessage(d) {
    if (d.ack) {
        if (d.ack === 'err') toast(d.msg || 'The device rejected the request', true);
        return;
    }
    switch (d.type) {
        case 'hello':
            return;
        case 'info':
            text('s-fw', d.app_version || '–');
            text('s-date', d.build_date || '–');
            text('s-idf', d.idf_version || '–');
            text(
                's-chip',
                (d.chip_model || '–') +
                    (d.cpu_cores ? ' · ' + d.cpu_cores + ' cores · ' + d.cpu_freq_mhz + ' MHz' : '')
            );
            text('s-flash', bytes(d.flash_size));
            text('s-slot', d.ota_slot || '–');
            text('s-reset', d.reset_reason || '–');
            if (d.min_leds) S.minLeds = d.min_leds;
            if (d.max_leds) S.maxLeds = d.max_leds;
            $('leds').min = S.minLeds;
            $('leds').max = S.maxLeds;
            return;
        case 'scan':
            S.scan = { scanning: !!d.scanning, networks: d.networks || [], done: !d.scanning };
            clearTimeout(S.timers.scan);
            showScan();
            return;
        case 'disp':
            if (d.display && d.display.available) {
                S.display = d.display;
                redraw();
            }
            return;
        case 'full':
            break;
        default:
            return;
    }

    if (d.display && d.display.available) {
        S.display = d.display;
        redraw();
    }
    text('s-up', uptime(d.uptime_s || 0));
    text('s-temp', d.temperature ? d.temperature.toFixed(1) + ' °C' : '–');
    text('s-heap', bytes(d.free_heap));
    text('s-minheap', bytes(d.min_free_heap));
    if (d.total_heap) meter('s-ram', 100 * (1 - d.free_heap / d.total_heap));

    // wifi and settings are null when unchanged since the last frame.
    if (d.settings) {
        for (const k in d.settings) {
            if (!(k in S.pending)) S.settings[k] = d.settings[k];
        }
        showSettings();
    }
    if (d.wifi) {
        S.wifi = d.wifi;
        showWifi();
    }
    if (d.logs && d.logs.entries)
        d.logs.entries.forEach((e) => addLog(typeof e === 'string' ? e : e.msg || ''));
}

/* ---- Connection ---- */
function setConn(state) {
    const c = $('conn');
    c.className = 'conn ' + state;
    c.title =
        state === 'up' ? 'Connected' : state === 'down' ? 'Disconnected, retrying' : 'Connecting';
}
function connect() {
    if (S.ws) {
        S.ws.onclose = S.ws.onmessage = null;
        S.ws.close();
    }
    clearTimeout(S.timers.retry);
    const ws = new WebSocket(
        (location.protocol === 'https:' ? 'wss://' : 'ws://') + location.host + '/ws'
    );
    S.ws = ws;
    const alive = () => {
        clearTimeout(S.timers.silent);
        S.timers.silent = setTimeout(() => ws.close(), SILENT_MS);
    };
    ws.onopen = () => {
        S.retry = RETRY_MIN;
        setConn('up');
        alive();
    };
    ws.onmessage = (e) => {
        alive();
        try {
            onMessage(JSON.parse(e.data));
        } catch (err) {
            console.warn(err);
        }
    };
    ws.onclose = () => {
        clearTimeout(S.timers.silent);
        S.ws = null;
        setConn('down');
        if (document.hidden) return; // reconnects when the page is shown again
        S.timers.retry = setTimeout(connect, S.retry);
        S.retry = Math.min(S.retry * 2, RETRY_MAX);
    };
}
function onVisibility() {
    clearTimeout(S.timers.hidden);
    if (document.hidden) {
        S.timers.hidden = setTimeout(() => S.ws && S.ws.close(), HIDDEN_MS);
    } else if (!S.ws) {
        S.retry = RETRY_MIN;
        connect();
    }
}

/* ---- Uploads ---- */
function upload(file) {
    if (!file) return;
    if (!file.name.endsWith('.bin')) {
        toast('Choose a .bin firmware file', true);
        return;
    }
    if (!confirm('Install ' + file.name + '? The device restarts afterwards.')) return;
    $('up').hidden = false;
    text('up-name', file.name);
    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/ota');
    xhr.upload.onprogress = (e) =>
        e.lengthComputable && meter('up-bar', (100 * e.loaded) / e.total);
    xhr.onload = () => {
        let msg = '';
        try {
            msg = JSON.parse(xhr.responseText).error || '';
        } catch (e) {}
        if (xhr.status === 200) toast('Installed. Restarting…');
        else toast('Upload failed' + (msg ? ': ' + msg : ''), true);
    };
    xhr.onerror = () => toast('Upload failed', true);
    const fd = new FormData();
    fd.append('file', file);
    xhr.send(fd);
}

/* ---- Wiring ---- */
function showTab(name) {
    if (!$('tab-' + name)) name = 'display';
    document
        .querySelectorAll('.tab')
        .forEach((t) => t.classList.toggle('active', t.id === 'tab-' + name));
    document
        .querySelectorAll('#tabs button')
        .forEach((b) => b.classList.toggle('active', b.dataset.tab === name));
    if (location.hash !== '#' + name) history.replaceState(null, '', '#' + name);
    if (name === 'display') redraw();
}
function setTheme(t) {
    if (t) document.documentElement.dataset.theme = t;
    try {
        localStorage.setItem('wt-theme', t);
    } catch (e) {}
}
function on(id, ev, fn) {
    $(id).addEventListener(ev, fn);
}
function onSeg(id, fn) {
    on(id, 'click', (e) => e.target.dataset.v != null && fn(e.target.dataset.v));
}

function init() {
    try {
        const t = localStorage.getItem('wt-theme');
        if (t) document.documentElement.dataset.theme = t;
    } catch (e) {}
    showTab(location.hash.slice(1));
    window.addEventListener('hashchange', () => showTab(location.hash.slice(1)));

    on('tabs', 'click', (e) => {
        const b = e.target.closest('button');
        if (b) showTab(b.dataset.tab);
    });
    on('theme', 'click', () => {
        const dark = getComputedStyle(document.documentElement).colorScheme === 'dark';
        setTheme(dark ? 'light' : 'dark');
    });

    // Display
    $('swatches').innerHTML =
        COLORS.map(
            (c) =>
                '<button data-c="' +
                c +
                '" style="background:' +
                c +
                '" aria-label="' +
                c +
                '"></button>'
        ).join('') + '<label title="Custom colour"><input id="color" type="color"></label>';
    on('swatches', 'click', (e) => {
        if (e.target.dataset.c) {
            setSetting('color', e.target.dataset.c);
            showSettings();
        }
    });
    on('color', 'input', (e) => setSetting('color', e.target.value, 200));
    onSeg('mode', (v) => {
        setSetting('display_mode', v, 0);
        showSettings();
    });
    on(
        'value',
        'input',
        (e) =>
            e.target.value !== '' &&
            setSetting('display_value', Math.max(0, Math.min(9999, +e.target.value)), 400)
    );
    on('text', 'input', (e) => {
        const v = e.target.value
            .toUpperCase()
            .replace(/[^A-Z0-9 _-]/g, '')
            .slice(0, 4);
        e.target.value = v;
        setSetting('display_text', v, 400);
    });
    on('blink', 'change', (e) => setSetting('anim_colon', e.target.checked, 0));
    on('brt', 'input', (e) => {
        setSetting('brightness', +e.target.value);
        text('brt-v', e.target.value + '%');
    });
    on('fx-en', 'change', (e) => {
        setSetting('bg_effect_en', e.target.checked, 0);
        showSettings();
    });
    on('fx', 'change', (e) => setSetting('anim_effect', e.target.value, 0));
    on('leds', 'change', (e) => {
        const n = Math.max(S.minLeds, Math.min(S.maxLeds, +e.target.value || S.minLeds));
        e.target.value = n;
        setSetting('led_count', n, 0);
    });

    // Clock
    onSeg('fmt', (v) => {
        setSetting('time_format', +v, 0);
        showSettings();
    });
    on('tz', 'change', (e) => setSetting('timezone', e.target.value, 0));
    on('ntp', 'change', (e) =>
        setSetting('ntp_server', e.target.value.trim() || 'pool.ntp.org', 0)
    );
    on(
        'ntp-sync',
        'click',
        () =>
            send('ntp_sync', { server: $('ntp').value.trim() || 'pool.ntp.org' }) &&
            toast('Time sync requested')
    );

    // Network
    on('scan', 'click', () => {
        if (!send('wifi', { op: 'scan' })) return;
        S.scan = { scanning: true, networks: S.scan.networks, done: false };
        showScan();
        clearTimeout(S.timers.scan);
        S.timers.scan = setTimeout(() => {
            S.scan.scanning = false;
            showScan();
        }, 15000);
    });
    on('scan-list', 'click', (e) => {
        const n = S.scan.networks[e.target.dataset.pick];
        if (!n) return;
        const saved = ((S.wifi && S.wifi.profiles) || []).findIndex((p) => p.ssid === n.ssid);
        if (saved >= 0) wifi('connect', { index: saved }, 'Connecting to ' + n.ssid + '…');
        else if (!n.secure) wifi('add', { ssid: n.ssid, password: '' }, n.ssid + ' saved');
        else {
            $('add-form').hidden = false;
            $('add-ssid').value = n.ssid;
            $('add-pass').value = '';
            $('add-pass').focus();
        }
    });
    on('saved', 'click', (e) => {
        const ds = e.target.dataset,
            w = S.wifi;
        if (ds.connect) wifi('connect', { index: +ds.connect }, 'Connecting…');
        if (ds.forget) {
            const p = w.profiles[+ds.forget];
            const cur = w.connected && w.ssid === p.ssid;
            if (
                !confirm(
                    'Forget ' +
                        p.ssid +
                        '?' +
                        (cur
                            ? ' The device disconnects from it; it stays reachable on its own access point.'
                            : '')
                )
            )
                return;
            wifi('del', { index: +ds.forget }, 'Network forgotten');
        }
    });
    on('add-show', 'click', () => {
        $('add-form').hidden = false;
        $('add-ssid').focus();
    });
    on('add-cancel', 'click', () => {
        $('add-form').hidden = true;
    });
    on('add-form', 'submit', (e) => {
        e.preventDefault();
        const ssid = $('add-ssid').value.trim(),
            pass = $('add-pass').value;
        if (!ssid) return toast('Enter the network name', true);
        if (pass && (pass.length < 8 || pass.length > 63))
            return toast('The password must be 8 to 63 characters', true);
        wifi('add', { ssid: ssid, password: pass }, ssid + ' saved');
        $('add-form').hidden = true;
        $('add-form').reset();
    });
    on('host-save', 'click', () => {
        const name = $('host').value.trim().toLowerCase();
        if (!/^[a-z0-9]([a-z0-9-]{0,29}[a-z0-9])?$/.test(name))
            return toast('Use 1 to 31 characters: a-z, 0-9 and -', true);
        S.edited.hostname = Date.now();
        wifi('hostname', { name: name }, 'Now at ' + name + '.local');
    });
    on('ap-save', 'click', () => {
        const pass = $('ap-pass').value;
        if (pass.length < 8 || pass.length > 63)
            return toast('The password must be 8 to 63 characters', true);
        if (!confirm('Change the access point password? Devices on it will have to rejoin.'))
            return;
        wifi('ap_password', { password: pass }, 'Access point password changed');
        $('ap-pass').value = '';
    });

    // System
    on('file', 'change', (e) => {
        upload(e.target.files[0]);
        e.target.value = '';
    });
    const drop = $('drop');
    drop.addEventListener('dragover', (e) => {
        e.preventDefault();
        drop.classList.add('over');
    });
    drop.addEventListener('dragleave', () => drop.classList.remove('over'));
    drop.addEventListener('drop', (e) => {
        e.preventDefault();
        drop.classList.remove('over');
        upload(e.dataTransfer.files[0]);
    });
    on(
        'reboot',
        'click',
        () => confirm('Restart the device?') && send('reboot', {}) && toast('Restarting…')
    );

    // Logs
    onSeg('lvl', (v) => {
        S.logLevel = v;
        segValue('lvl', v);
        showLogs();
    });
    on('log-clr', 'click', () => {
        S.logs = [];
        $('log').innerHTML = '';
    });
    on('log-dl', 'click', () => {
        const a = document.createElement('a');
        a.href = URL.createObjectURL(
            new Blob([S.logs.map((l) => l.line).join('\n')], { type: 'text/plain' })
        );
        a.download = 'watchtower-log.txt';
        a.click();
    });

    window.addEventListener('resize', redraw);
    document.addEventListener('visibilitychange', onVisibility);
    window.addEventListener('online', () => !S.ws && connect());
    setConn('');
    redraw();
    connect();
}
document.addEventListener('DOMContentLoaded', init);
