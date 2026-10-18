// On-Air sign internals: slideshow navigation + interactive demos.
// Firmware behaviour mirrored here is taken from onair-led-sign-firmware.ino.
(() => {
  'use strict';
  const $ = (s, r = document) => r.querySelector(s);
  const $$ = (s, r = document) => [...r.querySelectorAll(s)];

  // ---------------------------------------------------------------- firmware maths
  // breathingTick(): integer triangle wave between min and max over period.
  function breathPct(t, period, min, max) {
    const half = Math.floor(period / 2);
    const elapsed = t % period;
    const span = max > min ? max - min : 0;
    if (half === 0) return max;
    if (elapsed < half) return min + Math.floor((span * elapsed) / half);
    return max - Math.floor((span * (elapsed - half)) / half);
  }
  // statusLedTick(): breathing -> two 120 ms flashes per 1500 ms cycle.
  function statusLed(mode, t) {
    if (mode === 1) return true;
    if (mode === 0) return false;
    const c = t % 1500;
    return c < 120 || (c >= 240 && c < 360);
  }
  const setLevel = (el, lvl) => el && el.style.setProperty('--level', lvl.toFixed(3));

  // ---------------------------------------------------------------- navigation
  const slides = $$('.slide');
  const counter = $('#counter');
  const chapter = $('#chapter');
  const fill = $('#progress-fill');
  let cur = -1;

  function go(i, push = true) {
    i = Math.max(0, Math.min(slides.length - 1, i));
    if (i === cur) return;
    cur = i;
    slides.forEach((s, k) => {
      s.classList.toggle('active', k === i);
      s.classList.toggle('before', k < i);
    });
    const s = slides[i];
    counter.textContent = `${i + 1} / ${slides.length}`;
    chapter.textContent = `${s.dataset.chapter} · ${s.dataset.title}`;
    fill.style.width = `${((i + 1) / slides.length) * 100}%`;
    document.title = `${s.dataset.title} · On-Air Sign Internals`;
    if (push && location.hash !== `#/${s.id}`) history.replaceState(null, '', `#/${s.id}`);
    s.scrollTop = 0;
    $$('#overview-list a').forEach((a, k) => a.classList.toggle('cur', k === i));
    if (s.id === 'output-modes') resizeGraph();
  }
  const fromHash = () => {
    const id = location.hash.replace(/^#\/?/, '');
    const k = slides.findIndex(s => s.id === id);
    return k < 0 ? 0 : k;
  };
  window.addEventListener('hashchange', () => go(fromHash(), false));
  $('#btn-prev').onclick = () => go(cur - 1);
  $('#btn-next').onclick = () => go(cur + 1);

  const overview = $('#overview');
  const ovList = $('#overview-list');
  slides.forEach((s, k) => {
    const li = document.createElement('li');
    li.innerHTML = `<a href="#/${s.id}"><small>${k + 1} · ${s.dataset.chapter}</small>${s.dataset.title}</a>`;
    li.firstChild.onclick = () => toggleOverview(false);
    ovList.appendChild(li);
  });
  function toggleOverview(show = overview.hidden) {
    overview.hidden = !show;
    if (show) ($('a.cur', ovList) || $('a', ovList)).focus();
  }
  $('#btn-overview').onclick = () => toggleOverview(true);
  $('#btn-close-ov').onclick = () => toggleOverview(false);

  document.addEventListener('keydown', e => {
    if (e.altKey || e.ctrlKey || e.metaKey) return;
    const tag = e.target.tagName;
    if (tag === 'INPUT' || tag === 'SELECT' || tag === 'TEXTAREA') return;
    if (e.key === 'Escape') return toggleOverview(false);
    if (e.key === 'o' || e.key === 'O') return toggleOverview();
    if (!overview.hidden) return;
    const onButton = tag === 'BUTTON';
    if (e.key === 'ArrowRight' || e.key === 'PageDown' || (e.key === ' ' && !onButton)) { e.preventDefault(); go(cur + 1); }
    else if (e.key === 'ArrowLeft' || e.key === 'PageUp') { e.preventDefault(); go(cur - 1); }
    else if (e.key === 'Home') go(0);
    else if (e.key === 'End') go(slides.length - 1);
  });

  let touch = null;
  const deck = $('#deck');
  deck.addEventListener('touchstart', e => {
    if (e.target.closest('input, select, button, canvas, .part, .flashmap')) { touch = null; return; }
    touch = { x: e.touches[0].clientX, y: e.touches[0].clientY };
  }, { passive: true });
  deck.addEventListener('touchend', e => {
    if (!touch) return;
    const dx = e.changedTouches[0].clientX - touch.x;
    const dy = e.changedTouches[0].clientY - touch.y;
    if (Math.abs(dx) > 60 && Math.abs(dx) > Math.abs(dy) * 1.5) go(cur + (dx < 0 ? 1 : -1));
    touch = null;
  });

  // ---------------------------------------------------------------- hero sign
  const hero = $('#hero-sign');
  let heroMode = 1;
  hero.onclick = () => { heroMode = (heroMode + 1) % 3; };

  // ---------------------------------------------------------------- architecture
  const ARCH = {
    ext: ['Chrome extension', 'Lives in a separate repo (<code>onair-meeting-trigger</code>). It decides <em>whether</em> you are in a meeting. It calls the sign on the LAN first (1.5 s timeout), falls back to the cloud bridge, and once a minute re-reads the real state and corrects any drift.'],
    dev: ['ESP32-C6 sign', 'This firmware. It switches the MOSFET, serves the setup portal, the HTTP API and OTA, and, if provisioned, keeps one outbound MQTT/TLS connection to AWS so it can receive commands without being reachable from the internet.'],
    lam: ['Cloud bridge', '<code>scripts/cloud-bridge/lambda_function.py</code> behind an API Gateway HTTP API. <b>POST</b> publishes <code>{"mode":N}</code> to <code>onair/&lt;thing&gt;/cmd</code>; <b>GET</b> reads the Device Shadow. Shared bearer token plus a Thing allowlist.'],
    iot: ['AWS IoT Core', 'Managed MQTT broker with per-device X.509 identities. It routes <code>cmd</code> messages to the device and keeps a <b>Device Shadow</b>: the last state the device reported, readable even while the device is offline.']
  };
  $$('.arch-node').forEach(b => b.onclick = () => {
    $$('.arch-node').forEach(x => x.classList.toggle('active', x === b));
    const [t, d] = ARCH[b.dataset.k];
    $('#arch-info').innerHTML = `<h3>${t}</h3><p>${d}</p>`;
  });

  // ---------------------------------------------------------------- control-path flow
  const SCEN = {
    lan: { sign: 0, steps: [
      { p: 'p-lan', n: 'n-ext', t: 'The extension sees a meeting tab and tries the device first: <code>GET /api/set?state=1</code> with a 1.5 s timeout.' },
      { n: 'n-dev', s: 1, t: '<code>setOutputMode(1)</code> drives the MOSFET gate HIGH. The sign lights.' },
      { p: 'p-lan', r: 1, n: 'n-ext', t: 'The device replies <code>{"ok":true,"state":true}</code>.' },
      { p: 'p-iot-dev', r: 1, n: 'n-iot', t: 'The same call to <code>setOutputMode()</code> publishes to <code>onair/&lt;thing&gt;/state</code> and to the shadow.' },
      { p: 'p-iot-sh', n: 'n-sh', t: 'The shadow now says <code>reported.mode = 1</code>, ready for the cloud check later.' }
    ]},
    cloud: { sign: 0, steps: [
      { p: 'p-lan', fail: 1, n: 'n-ext', t: 'You\'re away from home: <code>/api/set</code> gets no answer within 1.5 s.' },
      { p: 'p-ext-lam', n: 'n-lam', t: 'Fallback: <code>POST {"thing":"onair-office","mode":1}</code> with <code>Authorization: Bearer …</code>.' },
      { p: 'p-lam-iot', n: 'n-iot', t: 'Lambda checks the token and allowlist, then calls <code>iot.publish("onair/onair-office/cmd", {"mode":1})</code>.' },
      { p: 'p-iot-dev', n: 'n-dev', s: 1, t: 'IoT Core delivers the message over the device\'s <em>outbound</em> MQTT connection, so no port forwarding is needed. <code>onMqttMessage()</code> → <code>setOutputMode(1)</code>.' },
      { p: 'p-iot-dev', r: 1, n: 'n-iot', t: 'The device reports the change on the state topic and the shadow.' },
      { p: 'p-iot-sh', n: 'n-sh', t: 'Shadow updated.' }
    ]},
    verify: { sign: 0, steps: [
      { n: 'n-dev', t: 'A meeting is running but the sign is dark, say after a power blip (it always boots OFF).' },
      { p: 'p-lan', n: 'n-dev', t: 'The extension\'s once-a-minute check: <code>GET /api/status</code>.' },
      { p: 'p-lan', r: 1, n: 'n-ext', t: 'The reply says <code>"output_mode":"off"</code>. That doesn\'t match the meeting.' },
      { p: 'p-lan', n: 'n-dev', s: 1, t: 'The extension sends ON again. Off the LAN, the same check reads the shadow via <code>GET ?thing=…</code> on the bridge.' }
    ]},
    reconnect: { sign: 0, steps: [
      { n: 'n-dev', t: 'Power returns. <code>setup()</code> drives the output LOW right away; Wi-Fi connects.' },
      { p: 'p-iot-dev', r: 1, n: 'n-iot', t: 'NTP sync, then mutual TLS to port 8883 and a subscription to <code>onair/&lt;thing&gt;/cmd</code>.' },
      { p: 'p-iot-sh', n: 'n-sh', t: 'On every (re)connect the device publishes its current state, so the shadow learns it is OFF.' },
      { p: 'p-ext-lam', n: 'n-lam', t: 'At the next check the extension (here off-LAN) asks the bridge: <code>GET ?thing=onair-office</code>.' },
      { p: 'p-lam-sh', n: 'n-sh', t: '<code>get_thing_shadow()</code> → <code>parse_reported_mode()</code> = 0. That doesn\'t match the meeting, so the extension POSTs mode 1.' },
      { p: 'p-lam-iot', n: 'n-iot', t: 'Publish to <code>cmd</code>…' },
      { p: 'p-iot-dev', n: 'n-dev', s: 1, t: '…and the sign is back ON within a minute of the outage, with no one touching it.' }
    ]}
  };
  let scen = 'lan', step = -1, animToken = 0;
  const packet = $('#packet');
  const flowX = $('#flow-x');
  const flowSign = $('#flow-sign');
  const reduced = matchMedia('(prefers-reduced-motion: reduce)').matches;

  function animatePacket(path, reverse, failHalf) {
    const token = ++animToken;
    const len = path.getTotalLength();
    const dur = reduced ? 1 : 900;
    const stop = failHalf ? 0.5 : 1;
    return new Promise(res => {
      const t0 = performance.now();
      const frame = now => {
        if (token !== animToken) return res(false);
        const k = Math.min(1, (now - t0) / dur);
        const e = (k < .5 ? 2 * k * k : 1 - Math.pow(-2 * k + 2, 2) / 2) * stop;
        const pt = path.getPointAtLength(len * (reverse ? 1 - e : e));
        packet.setAttribute('cx', pt.x); packet.setAttribute('cy', pt.y);
        if (k < 1) requestAnimationFrame(frame); else res(true);
      };
      requestAnimationFrame(frame);
    });
  }
  function renderFlow(animate) {
    const sc = SCEN[scen];
    const ol = $('#flow-steps');
    ol.innerHTML = sc.steps.map((s, k) => `<li class="${k === step ? 'cur' : ''}">${s.t}</li>`).join('');
    let sign = sc.sign;
    for (let k = 0; k <= step; k++) if (sc.steps[k].s !== undefined) sign = sc.steps[k].s;
    $$('.fp').forEach(p => p.classList.remove('hot', 'done'));
    $$('.fn').forEach(n => n.classList.remove('hot'));
    for (let k = 0; k < step; k++) if (sc.steps[k].p) $('#' + sc.steps[k].p).classList.add('done');
    flowX.setAttribute('opacity', 0);
    packet.setAttribute('cx', -50);
    $('#flow-prev').disabled = step < 0;
    $('#flow-next').disabled = step >= sc.steps.length - 1;
    const s = sc.steps[step];
    if (!s) { animToken++; setLevel(flowSign, sign); return Promise.resolve(); }
    const finish = () => {
      if (s.n) $('#' + s.n).classList.add('hot');
      if (s.fail) flowX.setAttribute('opacity', 1);
      setLevel(flowSign, sign);
    };
    if (!s.p) { animToken++; finish(); return Promise.resolve(); }
    const path = $('#' + s.p);
    path.classList.add('hot');
    if (!animate) { animToken++; finish(); return Promise.resolve(); }
    return animatePacket(path, s.r, s.fail).then(ok => { if (ok) finish(); });
  }
  $$('#flow-scenarios .chip').forEach(b => b.onclick = () => {
    $$('#flow-scenarios .chip').forEach(x => x.classList.toggle('active', x === b));
    scen = b.dataset.s; step = -1; renderFlow(false);
  });
  $('#flow-next').onclick = () => { if (step < SCEN[scen].steps.length - 1) { step++; renderFlow(true); } };
  $('#flow-prev').onclick = () => { if (step >= 0) { step--; renderFlow(false); } };
  $('#flow-play').onclick = async () => {
    step = -1; renderFlow(false);
    const mine = scen;
    while (scen === mine && step < SCEN[scen].steps.length - 1) {
      step++;
      await renderFlow(true);
      await new Promise(r => setTimeout(r, reduced ? 800 : 500));
      if (scen !== mine) return;
    }
  };
  renderFlow(false);

  // ---------------------------------------------------------------- hardware
  const HW = {
    sign: ['LED sign', 'A 5 V USB neon/LED sign. Its + lead goes straight to the 5 V rail. Its − lead goes to the MOSFET drain, so the MOSFET decides whether current can reach ground.'],
    fet: ['FGP30N06L MOSFET', 'A <b>logic-level</b> N-channel MOSFET: 3.3 V on the gate is enough to switch it fully on. Low-side switching keeps the gate drive referenced to the shared ground. Add a flyback diode if the load is inductive.'],
    rg: ['Series gate resistor', '100–220 Ω between the GPIO and the gate limits the inrush current into the gate capacitance and damps ringing on the PWM edges.'],
    rpd: ['10 kΩ pull-down: required', 'From power-on until <code>setup()</code> configures the pin (ROM bootloader plus up to ~1.7 s waiting for USB serial), the GPIO is not driven. Without this resistor the gate floats and the sign can flicker or glow when plugged in.'],
    mcu: ['ESP32-C6 board', 'XIAO ESP32-C6 or Beetle ESP32-C6, powered from the same 5 V. Output defaults to <b>GPIO18</b> (XIAO D10) and can be changed at runtime with <code>POST /api/pin</code>. GPIO9 is the BOOT button (input pull-up, used for resets).']
  };
  $$('.part').forEach(p => {
    const show = () => {
      $$('.part').forEach(x => x.classList.toggle('active', x === p));
      const [t, d] = HW[p.dataset.p];
      $('#hw-info').innerHTML = `<h3>${t}</h3><p>${d}</p>`;
    };
    p.addEventListener('mouseenter', show);
    p.addEventListener('click', show);
  });

  // ---------------------------------------------------------------- loop()
  const LOOP = [
    '<b>Deferred reboot.</b> HTTP handlers call <code>scheduleReboot()</code> instead of restarting straight away, so the response (often a redirect) reaches the browser before the chip goes down.',
    '<b>Serial commands.</b> <code>GET-STICKER</code>, <code>GET-NET</code>, <code>RESET-CFG/ALL/MFG</code> over USB-CDC at 115200 baud. Used by <code>print-sticker.py</code> on the bench.',
    '<b>Captive portal.</b> Answers DNS queries with 192.168.4.1 and reboots after 10 minutes if nobody completed setup.',
    '<b>Breathing.</b> Recomputes the PWM duty from <code>millis()</code> on every pass (no timers), giving a smooth triangle wave.',
    '<b>BOOT button.</b> Measures how long BOOT has been held, drives the blink feedback, and fires the 5 s / 15 s resets. 60 ms release debounce.',
    '<b>Status LED.</b> Mirrors the output: off, solid, or a double flash for breathing. Skipped in portal mode, during reset feedback, or when the LED <em>is</em> the output.',
    '<b>Wi-Fi supervisor.</b> Re-issues <code>WiFi.begin()</code> every 10 s while down; reboots after 15 minutes offline.',
    '<b>AWS IoT.</b> Pumps MQTT keep-alive and incoming messages; reconnects with a 5 s backoff. No-op when not provisioned.',
    '<b>Yield.</b> 5 ms sleep lets the Wi-Fi stack and the async web server run.'
  ];
  const loopItems = $$('#loop-list li');
  loopItems.forEach(li => li.onclick = () => {
    loopItems.forEach(x => x.classList.toggle('active', x === li));
    $('#loop-info').innerHTML = LOOP[+li.dataset.i];
  });

  // ---------------------------------------------------------------- power-up walkthrough
  const BOOT = {
    power: { t: 'Power applied', s: 0, x: 'The ROM bootloader runs first. Until <code>setup()</code> configures the output pin, the GPIO floats and the 10 kΩ pull-down holds the MOSFET off, so the sign stays dark.', n: [['Continue', 'setup']] },
    setup: { t: 'setup()', s: 0, x: 'Waits up to 1.5 s for USB serial, opens the <code>cfg</code> and <code>mfg</code> NVS namespaces, builds the setup-AP SSID and password, drives the output LOW and registers all HTTP routes.', c: 'pinMode(PIN_OUT, OUTPUT);\noutputWrite(false);        // sign OFF, always\nsetupHttpHandlers();       // routes only, server not started\ncheckBootTimeFactoryReset();', n: [['BOOT not held', 'wifi'], ['BOOT held at power-up', 'bootheld']] },
    bootheld: { t: 'checkBootTimeFactoryReset()', x: 'Boot pauses while BOOT is down. Release before 5 s to continue booting, between 5 and 15 s for a factory reset, or keep holding to 15 s for a deep reset. Try it on the BOOT slide.', n: [['Released before 5 s', 'wifi'], ['Released at 5–15 s', 'resetcfg'], ['Held to 15 s', 'resetdeep']] },
    resetcfg: { t: 'Factory reset', x: 'The <code>cfg</code> namespace is cleared and the chip restarts. With no saved Wi-Fi it opens the setup portal. The sticker password in <code>mfg</code> survives.', n: [['Reboot', 'power']] },
    resetdeep: { t: 'Deep factory reset', x: 'The whole NVS partition is erased: <code>cfg</code>, <code>mfg</code> and <code>aws_iot</code> (including the device\'s private key). The next boot generates a new AP password, so the old sticker no longer works and <code>GET-STICKER</code> is unlocked again.', n: [['Reboot', 'power']] },
    wifi: { t: 'Saved Wi-Fi?', x: '<code>connectWifiWithSaved()</code> reads <code>ssid</code>/<code>pass</code> from <code>cfg</code> and waits up to 12 s, still watching the BOOT button.', c: 'if (ssid.length() == 0) return false;\nWiFi.begin(ssid, pass);\nwhile (millis() - start < 12000) { … handleResetLongPress(); }', n: [['Joins within 12 s', 'connected'], ['Saved, but no join in 12 s', 'portal'], ['Nothing saved', 'portal']] },
    portal: { t: 'Setup portal', x: 'AP+STA access point <code>C6-SETUP-&lt;MAC12&gt;</code>, wildcard DNS to 192.168.4.1, background network scan, 10-minute timeout. OTA is disabled here.', c: 'WiFi.mode(WIFI_AP_STA);\nWiFi.softAP(apSsid, apPassStr, 1, 0, 4);\ndnsServer.start(53, "*", apIP);', n: [['User submits the form', 'saved'], ['10 minutes, nobody comes', 'ptimeout']] },
    saved: { t: 'Configuration saved', x: 'Wi-Fi, hostname, pin, polarity and auth are written to <code>cfg</code>, then the chip reboots to come up in connected mode.', n: [['Reboot', 'power']] },
    ptimeout: { t: 'Portal timeout', x: 'Reboots and tries the saved network again. This helps when the router was simply slower than the sign to come back after a power cut.', n: [['Reboot', 'power']] },
    connected: { t: 'startConnectedServices()', s: 0, x: 'Stops the AP, hardens Wi-Fi (auto-reconnect, no modem sleep), loads the pin and breathing settings, <b>forces the output OFF</b>, creates the API token on first join, sets <code>stk_lock</code>, starts mDNS and the web server.', c: 'WiFi.setAutoReconnect(true);\nWiFi.setSleep(false);\nsetOutputMode(MODE_OFF);   // always come up dark\nmfg.putBool("stk_lock", true);\nserver.begin();\nawsIotSetup();', n: [['AWS provisioned', 'aws'], ['Not provisioned (LAN only)', 'running']] },
    aws: { t: 'awsIotSetup()', x: 'Loads endpoint, cert and key from the <code>aws_iot</code> namespace, syncs time over NTP, opens mutual TLS on port 8883, subscribes to <code>onair/&lt;thing&gt;/cmd</code> and publishes the current state (OFF) to the state topic and the shadow.', n: [['Continue', 'running']] },
    running: { t: 'Running: loop()', x: 'The non-blocking ticks run every ~5 ms. The sign stays OFF until a command arrives, at once if a meeting is in progress.', n: [['Command: ON', 'on'], ['Wi-Fi drops', 'drop'], ['Power cut', 'power']] },
    on: { t: 'Sign ON', s: 1, x: '<code>setOutputMode(1)</code> drives the gate HIGH and reports the new state everywhere.', n: [['Command: OFF', 'running0'], ['Wi-Fi drops', 'drop'], ['Power cut', 'power']] },
    running0: { t: 'Sign OFF again', s: 0, x: 'Meeting over. <code>setOutputMode(0)</code>, state reported.', n: [['Command: ON', 'on'], ['Power cut', 'power']] },
    drop: { t: 'Wi-Fi lost', x: '<code>wifiSupervisorTick()</code> re-issues <code>WiFi.begin()</code> every 10 s. The output <em>keeps</em> its current state. MQTT reconnects (5 s backoff) once the link is back.', n: [['Link returns', 'running'], ['Still down after 15 min', 'offreboot']] },
    offreboot: { t: 'Offline reboot', x: '<code>ESP.restart()</code> after 15 minutes offline clears any stuck Wi-Fi/lwIP state. The sign comes back OFF.', n: [['Reboot', 'power']] }
  };
  let trail = [], bootSign = 0;
  function bootGo(id, via) {
    const node = BOOT[id];
    if (node.s !== undefined) bootSign = node.s;
    trail.push({ id, via });
    if (trail.length > 10) trail = trail.slice(-10);
    $('#boot-trail').innerHTML = trail.map((s, k) =>
      `<li class="${k === trail.length - 1 ? 'cur' : ''}">${s.via ? `<span class="ch">${s.via}</span>` : ''}${BOOT[s.id].t}</li>`).join('');
    $('#boot-title').textContent = node.t;
    $('#boot-text').innerHTML = node.x;
    $('#boot-code').textContent = node.c || '';
    setLevel($('#boot-sign'), bootSign);
    const ch = $('#boot-choices');
    ch.innerHTML = '';
    node.n.forEach(([label, to], k) => {
      const b = document.createElement('button');
      b.className = 'btn' + (k === 0 ? ' primary' : '');
      b.textContent = label;
      b.onclick = () => bootGo(to, label);
      ch.appendChild(b);
    });
    const r = document.createElement('button');
    r.className = 'btn ghost'; r.textContent = '↺ Restart';
    r.onclick = () => { trail = []; bootGo('power'); };
    ch.appendChild(r);
  }
  bootGo('power');

  // ---------------------------------------------------------------- AP password generator
  const WORDS = ['mint','plum','sky','river','stone','laser','panda','tiger','maple','cider','pearl','ember',
    'ocean','olive','sun','cloud','storm','delta','orbit','pixel','cobalt','coral','saffron','koi',
    'luna','nova','terra','flora','canyon','ridge','breeze','zephyr','aurora','comet','neon','iris',
    'eagle','fox','otter','koala','gecko','sparrow','falcon','whale','mango','cocoa','honey','latte'];
  const rnd = n => crypto.getRandomValues(new Uint32Array(1))[0] % n;
  function generate() {
    const A = 'abcdefghijklmnopqrstuvwxyz0123456789';
    const tail = Array.from({ length: 4 }, () => A[rnd(36)]).join('');
    const pass = `${WORDS[rnd(48)]}-${WORDS[rnd(48)]}-${WORDS[rnd(48)]}-${tail}`;
    const mac = Array.from({ length: 6 }, () => rnd(256).toString(16).padStart(2, '0')).join('').toUpperCase();
    const ssid = `C6-SETUP-${mac}`;
    $('#gen-ssid').textContent = ssid;
    $('#gen-pass').textContent = pass;
    $('#gen-qr').textContent = `WIFI:T:WPA;S:${ssid};P:${pass};H:false;;`;
  }
  $('#gen-btn').onclick = generate;
  generate();

  // ---------------------------------------------------------------- output mode simulator
  const sim = { mode: 0, period: 3000, min: 5, max: 100, t0: performance.now() };
  const simSign = $('#sim-sign'), simLed = $('#sim-led');
  const canvas = $('#sim-graph'), ctx = canvas.getContext('2d');
  function resizeGraph() {
    const dpr = window.devicePixelRatio || 1;
    const w = canvas.clientWidth || 600;
    canvas.width = w * dpr; canvas.height = 160 * dpr;
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  }
  window.addEventListener('resize', resizeGraph);
  function simLevel(t) {
    if (sim.mode === 1) return 100;
    if (sim.mode === 0) return 0;
    return breathPct(t, sim.period, sim.min, sim.max);
  }
  function updateCmds(err) {
    const names = ['off', 'on', 'breathing'];
    let http = `/api/mode?mode=${names[sim.mode]}`;
    if (sim.mode === 2) http += `&period_ms=${sim.period}&min_pct=${sim.min}&max_pct=${sim.max}`;
    $('#sim-http').textContent = err ? `HTTP 400: ${err}` : `GET ${http}`;
    $('#sim-http').style.color = err ? 'var(--red)' : '';
    $('#sim-mqtt').textContent = `onair/<thing>/cmd  {"mode":${sim.mode}}`;
  }
  $$('#sim-modes button').forEach(b => b.onclick = () => {
    $$('#sim-modes button').forEach(x => x.classList.toggle('active', x === b));
    sim.mode = +b.dataset.m;
    sim.t0 = performance.now(); // setOutputMode resets breathPhaseStart
    updateCmds();
  });
  const sP = $('#s-period'), sMin = $('#s-min'), sMax = $('#s-max');
  function onSlider() {
    $('#o-period').textContent = sP.value;
    $('#o-min').textContent = sMin.value;
    $('#o-max').textContent = sMax.value;
    const min = +sMin.value, max = +sMax.value;
    if (max <= min) { updateCmds('max_pct must be greater than min_pct. The device keeps its previous values.'); return; }
    sim.period = +sP.value; sim.min = min; sim.max = max;
    updateCmds();
  }
  [sP, sMin, sMax].forEach(s => s.addEventListener('input', onSlider));
  updateCmds();

  function drawGraph(t) {
    const w = canvas.clientWidth, h = 160;
    if (!w) return;
    const win = sim.mode === 2 ? sim.period * 2 : 3000;
    ctx.clearRect(0, 0, w, h);
    const top = 12, bottom = h - 34;
    const y = pct => bottom - (pct / 100) * (bottom - top);
    ctx.strokeStyle = '#2e333d'; ctx.lineWidth = 1;
    [0, 50, 100].forEach(p => { ctx.beginPath(); ctx.moveTo(0, y(p)); ctx.lineTo(w, y(p)); ctx.stroke(); });
    ctx.fillStyle = '#9aa0ab'; ctx.font = '11px ui-monospace, monospace';
    ctx.fillText('100%', 4, y(100) + 11); ctx.fillText('0%', 4, y(0) - 3);
    // level trace across a window ending "now"
    const start = Math.max(0, t - win);
    ctx.strokeStyle = '#ff3b3b'; ctx.lineWidth = 2.5; ctx.beginPath();
    for (let x = 0; x <= w; x += 2) {
      const tt = start + (x / w) * win;
      const v = tt > t ? null : simLevel(tt);
      if (v === null) break;
      x === 0 ? ctx.moveTo(x, y(v)) : ctx.lineTo(x, y(v));
    }
    ctx.stroke();
    // status LED track
    const ly = h - 18;
    ctx.fillStyle = '#9aa0ab'; ctx.fillText('status LED', 4, ly - 4);
    for (let x = 0; x <= w; x += 2) {
      const tt = start + (x / w) * win;
      if (tt > t) break;
      ctx.fillStyle = statusLed(sim.mode, tt) ? '#7dff9b' : '#20262b';
      ctx.fillRect(x, ly, 2, 8);
    }
    const nowX = Math.min(w, ((t - start) / win) * w);
    ctx.strokeStyle = '#ffb547'; ctx.lineWidth = 1; ctx.beginPath(); ctx.moveTo(nowX, 0); ctx.lineTo(nowX, h); ctx.stroke();
  }

  // ---------------------------------------------------------------- BOOT button simulator
  const bb = { pressed: false, start: 0, done: false, ledOn: false, lastToggle: 0 };
  const bbBtn = $('#bb-btn'), bbLed = $('#bb-led'), bbFill = $('#bb-fill'), bbStatus = $('#bb-status');
  const ns = { cfg: $('#ns-cfg'), mfg: $('#ns-mfg'), aws: $('#ns-aws') };
  const speed = () => ($('#bb-fast').checked ? 3 : 1);
  const nsSet = (cfg, mfg, aws) => {
    [['cfg', cfg], ['mfg', mfg], ['aws', aws]].forEach(([k, c]) => { ns[k].className = 'ns' + (c ? ' ' + c : ''); });
  };
  function bbDown(e) {
    if (bb.pressed) return;
    if (e && e.pointerId !== undefined) bbBtn.setPointerCapture(e.pointerId);
    Object.assign(bb, { pressed: true, start: performance.now(), done: false, ledOn: false, lastToggle: 0 });
    bbBtn.classList.add('pressed');
    nsSet('', '', '');
    bbStatus.textContent = 'Held… slow blink. Release now and nothing happens.';
  }
  function bbUp() {
    if (!bb.pressed) return;
    bb.pressed = false;
    bbBtn.classList.remove('pressed');
    if (bb.done) return;
    const held = (performance.now() - bb.start) * speed();
    bbLed.classList.remove('on');
    if (held >= 5000) {
      bbStatus.textContent = `Released after ${(held / 1000).toFixed(1)} s → factory reset: cfg cleared, restart. Sticker and AWS identity kept.`;
      nsSet('wiped', 'kept', 'kept');
    } else {
      bbStatus.textContent = `Released after ${(held / 1000).toFixed(1)} s: below 5 s, ignored. Output mode restored.`;
      bbFill.style.width = '0';
    }
  }
  bbBtn.addEventListener('pointerdown', bbDown);
  bbBtn.addEventListener('pointerup', bbUp);
  bbBtn.addEventListener('pointercancel', bbUp);
  bbBtn.addEventListener('keydown', e => { if ((e.key === ' ' || e.key === 'Enter') && !e.repeat) { e.preventDefault(); bbDown(); } });
  bbBtn.addEventListener('keyup', e => { if (e.key === ' ' || e.key === 'Enter') { e.preventDefault(); bbUp(); } });
  function bbTick(now) {
    if (!bb.pressed || bb.done) return;
    const held = (now - bb.start) * speed();
    bbFill.style.width = `${Math.min(100, (held / 20000) * 100)}%`;
    bbFill.className = 'tl-fill' + (held >= 15000 ? ' danger' : held >= 5000 ? ' warn' : '');
    if (held >= 15000) {
      bb.done = true;
      bbLed.classList.add('on');
      bbStatus.textContent = '15 s reached → deep reset fires at once: all NVS erased, including the AWS key. New AP password on next boot.';
      nsSet('wiped', 'wiped', 'wiped');
      return;
    }
    const interval = held < 5000 ? 500 : 150;
    if (held - bb.lastToggle >= interval) { bb.lastToggle = held; bb.ledOn = !bb.ledOn; bbLed.classList.toggle('on', bb.ledOn); }
    if (held >= 5000) {
      ns.cfg.className = 'ns armed';
      bbStatus.textContent = 'Fast blink: release now to clear cfg, or keep holding for a deep reset.';
    }
  }

  // ---------------------------------------------------------------- Wi-Fi supervisor
  const sOff = $('#s-off');
  function wifiUpdate() {
    const m = +sOff.value;
    $('#o-off').textContent = m.toFixed(2).replace(/\.?0+$/, '') || '0';
    $('#wifi-fill').style.width = `${(m / 16) * 100}%`;
    $('#wifi-fill').style.background = m >= 15 ? 'var(--red)' : 'var(--amber)';
    let txt;
    if (m === 0) txt = 'Connected. The supervisor clears the offline timer and returns immediately.';
    else if (m < 15) txt = `Down for ${m} min: about <b>${Math.floor((m * 60) / 10)}</b> forced reconnects so far (one every 10 s). The sign keeps its current state; MQTT waits until Wi-Fi is back, then reconnects with a 5 s backoff.`;
    else txt = '<b>15 minutes reached → <code>ESP.restart()</code>.</b> After the reboot the sign is OFF, Wi-Fi joins again, the device reports its state, and the extension corrects it within a minute.';
    $('#wifi-text').innerHTML = txt;
  }
  sOff.addEventListener('input', wifiUpdate);
  wifiUpdate();

  // ---------------------------------------------------------------- API builder
  const EP = {
    status: ['GET', '/api/status', '{"ok":true,"mode":"sta","ip":"192.168.1.42","ssid":"home","hostname":"xiao-esp32-c6",\n "out_pin":18,"led_active_high":true,"output_mode":"off","br_period_ms":3000,\n "br_min_pct":5,"br_max_pct":100,"state":false,"rssi":-55}'],
    on: ['GET', '/api/set?state=1', '{"ok":true,"state":true}'],
    off: ['GET', '/api/set?state=0', '{"ok":true,"state":false}'],
    breath: ['GET', '/api/mode?mode=breathing&period_ms=3000&min_pct=5&max_pct=100', '{"ok":true,"output_mode":"breathing","br_period_ms":3000,"br_min_pct":5,"br_max_pct":100}'],
    config: ['GET', '/api/config', '{"ok":true,"ssid":"home","hostname":"xiao-esp32-c6","out":18,"ledah":true,\n "output_mode":"off","br_period_ms":3000,"br_min_pct":5,"br_max_pct":100}'],
    aws: ['GET', '/api/aws/status', '{"enabled":true,"provisioned":true,"connected":true,"last_rc":0,\n "thing":"onair-office","endpoint":"xxxx-ats.iot.eu-west-1.amazonaws.com"}'],
    reboot: ['POST', '/api/reboot', '{"ok":true,"rebooting":true}']
  };
  function apiUpdate() {
    const [method, path, resp] = EP[$('#api-ep').value];
    const auth = $('#api-auth').value;
    const host = 'http://xiao-esp32-c6.local';
    let url = host + path, flags = method === 'POST' ? '-X POST ' : '';
    if (auth === 'hdr') flags += '-H "X-API-Token: $TOKEN" ';
    if (auth === 'bearer') flags += '-H "Authorization: Bearer $TOKEN" ';
    if (auth === 'basic') flags += '-u admin:esp32c6 ';
    if (auth === 'query') url += (path.includes('?') ? '&' : '?') + 'token=$TOKEN';
    $('#api-curl').textContent = `curl ${flags}"${url}"`;
    $('#api-resp').textContent = auth === 'none' ? 'HTTP/1.1 401 Unauthorized\nWWW-Authenticate: Basic realm="Login Required"' : resp;
  }
  $('#api-ep').onchange = $('#api-auth').onchange = apiUpdate;
  apiUpdate();

  // ---------------------------------------------------------------- provisioning tabs + claim stepper
  $$('#prov-tabs .chip').forEach(b => b.onclick = () => {
    $$('#prov-tabs .chip').forEach(x => x.classList.toggle('active', x === b));
    $$('#provisioning .tab-body').forEach(t => { t.hidden = t.dataset.t !== b.dataset.t; });
  });
  let claimStep = 0;
  const claimItems = $$('#claim-steps li');
  const claimRender = () => claimItems.forEach((li, k) => li.classList.toggle('cur', k === claimStep));
  $$('#provisioning [data-step]').forEach(b => b.onclick = () => {
    claimStep = Math.max(0, Math.min(claimItems.length - 1, claimStep + +b.dataset.step));
    claimRender();
  });
  claimRender();

  // ---------------------------------------------------------------- flash map
  $$('.fm').forEach(f => {
    const show = () => { $$('.fm').forEach(x => x.classList.toggle('active', x === f)); $('#fm-info').textContent = f.dataset.d; };
    f.addEventListener('mouseenter', show);
    f.addEventListener('click', show);
  });

  // ---------------------------------------------------------------- secret-scan tester (mirrors build.yml)
  const BAD_HOST = /[a-z0-9]+-ats\.iot\.[a-z0-9-]+\.amazonaws\.com/;
  const PEM = /^MII[A-Za-z0-9+/=]{50,}$/;
  const CA1 = 'MIIDQTCCAimgAwIBAgITBmyfz5m/jAo54vB4ikPmljZbyjANBgkqhkiG9w0BAQsF';
  const scanIn = $('#scan-in');
  function scan() {
    const s = scanIn.value.trim();
    let out;
    if (BAD_HOST.test(s)) out = '<b style="color:var(--red)">✗ Real ATS endpoint.</b> The build fails: an account endpoint must never be baked into the binary.';
    else if (s === CA1) out = '<b style="color:var(--lan)">✓ Amazon Root CA 1.</b> The one PEM body that is allowed (and required) in the binary.';
    else if (PEM.test(s)) out = '<b style="color:var(--red)">✗ PEM body.</b> Counted as a second certificate or key; the build fails if there is more than one.';
    else out = '<b style="color:var(--lan)">✓ Ignored.</b> No base64 body and no real endpoint: placeholders and library marker strings pass.';
    $('#scan-out').innerHTML = out;
  }
  scanIn.addEventListener('input', scan);
  $$('[data-scan]').forEach(b => b.onclick = () => { scanIn.value = b.dataset.scan; scan(); });
  scan();

  // ---------------------------------------------------------------- shared animation loop
  function frame(now) {
    const id = slides[cur] && slides[cur].id;
    if (id === 'title') {
      const lvl = heroMode === 2 ? breathPct(now, 3000, 5, 100) / 100 : heroMode;
      setLevel(hero, lvl);
    } else if (id === 'output-modes') {
      const t = now - sim.t0;
      const pct = simLevel(t);
      setLevel(simSign, pct / 100);
      simLed.classList.toggle('on', statusLed(sim.mode, t));
      $('#sim-pct').textContent = pct;
      $('#sim-duty').textContent = Math.floor((pct * 1023) / 100);
      drawGraph(t);
    } else if (id === 'boot-button') {
      bbTick(now);
    }
    requestAnimationFrame(frame);
  }

  go(fromHash(), false);
  requestAnimationFrame(frame);
})();
