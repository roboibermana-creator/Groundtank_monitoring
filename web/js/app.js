(function () {
  "use strict";

  // ---------- Plant constants ----------
  var CAPACITY_L = 20000;
  var TANK_H_CM = 400;
  var SENSOR_OFFSET_CM = 15; // jarak sensor ke muka air saat 100% (sesuai batas 15 cm di firmware)
  var SP = { HH: 90, H: 70, L: 30, LL: 10 };
  var FLOAT_TRIP = 97;  // LSH-01 float switch
  var TICK_MS = 1000;
  var HIST_LEN = 120;

  var NS = "http://www.w3.org/2000/svg";
  function $(id) { return document.getElementById(id); }
  function svgEl(tag, attrs, text) {
    var e = document.createElementNS(NS, tag);
    for (var k in attrs) e.setAttribute(k, attrs[k]);
    if (text != null) e.textContent = text;
    return e;
  }
  function pad(n) { return (n < 10 ? "0" : "") + n; }
  function hhmmss(d) { return pad(d.getHours()) + ":" + pad(d.getMinutes()) + ":" + pad(d.getSeconds()); }

  // ---------- Navigation ----------
  var screens = ["start", "dashboard", "pfd", "pid"];
  var titles = { start: "HOME", dashboard: "DASHBOARD", pfd: "PROCESS FLOW DIAGRAM", pid: "P&ID" };
  function goTo(name) {
    if (screens.indexOf(name) === -1) name = "start";
    screens.forEach(function (s) { $("screen-" + s).hidden = s !== name; });
    document.querySelectorAll(".navbtn").forEach(function (b) {
      b.setAttribute("aria-current", b.dataset.go === name ? "true" : "false");
    });
    $("topbarSub").textContent = titles[name];
    // riwayat URL & auto-scroll bersifat kosmetik saja; sebagian webview/preview
    // (mis. Live Preview VS Code) tidak menyediakan History API penuh, jadi jangan
    // sampai kegagalannya menghentikan seluruh aplikasi.
    try {
      if (window.history && history.replaceState && location.hash.slice(1) !== name) {
        history.replaceState(null, "", "#" + name);
      }
    } catch (e) { /* diabaikan: preview tanpa History API */ }
    try { window.scrollTo(0, 0); } catch (e) { /* diabaikan */ }
  }
  document.querySelectorAll("[data-go]").forEach(function (el) {
    el.addEventListener("click", function () { goTo(el.dataset.go); });
  });
  var startHash = "start";
  try { startHash = (location.hash || "#start").slice(1); } catch (e) { /* diabaikan */ }
  goTo(startHash);

  // ---------- Clock ----------
  function updClock() { $("clock").textContent = hhmmss(new Date()); }
  updClock(); setInterval(updClock, 1000);

  // ---------- Indikator koneksi ----------
  // Internet: langsung dari browser (navigator.onLine + event online/offline), jadi akurat real-time.
  // ESP32: dicek lewat Supabase, bukan dengan menghubungi ESP32 langsung. ESP32 mengirim
  // pembacaan sensor ke tabel Supabase (lihat supabase/schema.sql); dashboard membaca baris
  // TERAKHIR dari tabel itu. Kalau baris terakhir masih baru (di bawah staleAfterSec), berarti
  // ESP32 masih aktif mengirim -> "Terhubung". Kalau sudah lama / tidak ada baris sama sekali,
  // statusnya jujur menunjukkan "Tidak terhubung", bukan disimulasikan seolah tersambung.
  var CFG = window.SUPABASE_CONFIG || {};
  var ESP32_CHECK_MS = 5000;
  var supaClient = null;
  var liveMode = false; // true = dashboard dipasok data asli dari Supabase, simulator nonaktif

  if (CFG.url && CFG.anonKey && window.supabase && window.supabase.createClient) {
    try { supaClient = window.supabase.createClient(CFG.url, CFG.anonKey); }
    catch (e) { supaClient = null; }
  }

  function setLed(id, cls) { var el = $(id); if (el) el.className = "led " + cls; }

  function fmtAgo(sec) {
    if (sec < 60) return Math.round(sec) + " dtk lalu";
    if (sec < 3600) return Math.round(sec / 60) + " mnt lalu";
    return Math.round(sec / 3600) + " jam lalu";
  }

  var esp32State = "unset"; // unset | checking | online | offline | error
  var esp32Detail = "";

  function renderEsp32() {
    var label, ledCls;
    if (esp32State === "unset") { label = "Belum diatur"; ledCls = "led-off"; }
    else if (esp32State === "checking") { label = "Memeriksa…"; ledCls = "led-warn"; }
    else if (esp32State === "online") { label = "Terhubung"; ledCls = "led-ok"; }
    else if (esp32State === "error") { label = "Gagal terhubung ke Supabase"; ledCls = "led-crit"; }
    else { label = "Tidak terhubung"; ledCls = "led-crit"; }
    ["esp32Text", "esp32TextB"].forEach(function (id) { var el = $(id); if (el) el.textContent = label; });
    ["ledEsp32", "ledEsp32b"].forEach(function (id) { setLed(id, ledCls); });
    var note = $("esp32Note");
    if (note) {
      if (esp32State === "unset") note.textContent = "Belum diatur — isi URL & API key Supabase di js/supabase-config.js, dashboard masih pakai data simulasi.";
      else if (esp32State === "online") note.textContent = "Data terakhir dari tabel " + CFG.table + ": " + esp32Detail + ".";
      else if (esp32State === "error") note.textContent = "Tidak bisa membaca tabel " + CFG.table + " di Supabase. Cek URL/API key di supabase-config.js dan pastikan supabase/schema.sql sudah dijalankan.";
      else note.textContent = "Belum ada data baru dari ESP32" + (esp32Detail ? " (terakhir " + esp32Detail + ")" : "") + ". Dashboard sementara memakai data simulasi.";
    }
    var docNo = $("simDocNo");
    if (docNo) docNo.textContent = liveMode ? "DATA ASLI DARI SUPABASE" : "DATA DUMMY · BELUM TERHUBUNG SUPABASE";
    var simPanel = $("simPanel");
    if (simPanel) simPanel.classList.toggle("live", liveMode);
  }

  function applyLiveReading(row) {
    var L = row.level_percent != null ? Number(row.level_percent) :
      Math.max(0, Math.min(100, 100 - ((Number(row.distance_cm) - SENSOR_OFFSET_CM) / TANK_H_CM) * 100));
    state.level = Math.max(0, Math.min(100, L));
    state.filling = !!row.pump_filling;
    render();
    sample();
  }

  function checkEsp32() {
    if (!supaClient) { esp32State = "unset"; liveMode = false; renderEsp32(); return; }
    esp32State = "checking"; renderEsp32();
    supaClient.from(CFG.table).select("*").order("created_at", { ascending: false }).limit(1)
      .then(function (res) {
        if (res.error) { esp32State = "error"; liveMode = false; renderEsp32(); return; }
        var rows = res.data || [];
        if (!rows.length) { esp32State = "offline"; esp32Detail = ""; liveMode = false; renderEsp32(); return; }
        var row = rows[0];
        var ageSec = (Date.now() - new Date(row.created_at).getTime()) / 1000;
        esp32Detail = fmtAgo(ageSec);
        if (ageSec <= (CFG.staleAfterSec || 20)) {
          esp32State = "online"; liveMode = true;
          applyLiveReading(row);
        } else {
          esp32State = "offline"; liveMode = false;
        }
        renderEsp32();
      })
      .catch(function () { esp32State = "error"; liveMode = false; renderEsp32(); });
  }
  checkEsp32();
  setInterval(checkEsp32, ESP32_CHECK_MS);

  function renderInternet() {
    var online = navigator.onLine !== false; // browser lama tanpa dukungan dianggap online
    var label = online ? "Online" : "Offline";
    var ledCls = online ? "led-ok" : "led-crit";
    ["internetText", "internetTextB"].forEach(function (id) { var el = $(id); if (el) el.textContent = label; });
    ["ledInternet", "ledInternetB"].forEach(function (id) { setLed(id, ledCls); });
  }
  window.addEventListener("online", renderInternet);
  window.addEventListener("offline", renderInternet);
  renderInternet();

  // ---------- Alarm lamp logic ----------
  // HH saat pengisian  -> merah kedip
  // penuh saat idle    -> hijau
  // H / L              -> kuning
  // LL (kosong)        -> merah kedip
  function evaluate(level, filling) {
    if (level >= SP.HH) {
      if (filling) return { code: "HH", sev: "crit", blink: true, text: "HIGH-HIGH", desc: "Tangki hampir penuh saat pengisian. Pompa akan berhenti." };
      return { code: "FULL", sev: "ok", blink: false, text: "PENUH", desc: "Tangki penuh, pompa idle. Kondisi normal." };
    }
    if (level >= SP.H) return { code: "H", sev: "warn", blink: false, text: "HIGH", desc: "Level mendekati penuh." };
    if (level <= SP.LL) return { code: "LL", sev: "crit", blink: true, text: "LOW-LOW", desc: "Tangki kosong. Segera lakukan pengisian." };
    if (level <= SP.L) return { code: "L", sev: "warn", blink: false, text: "LOW", desc: "Level mendekati kosong." };
    return { code: "N", sev: "ok", blink: false, text: "NORMAL", desc: "Level dalam rentang operasi." };
  }

  var ALARM_DEF = {
    HH: { tag: "LAHH-01", prio: 1, msg: "Level HIGH-HIGH saat pengisian" },
    H:  { tag: "LAH-01",  prio: 2, msg: "Level HIGH" },
    L:  { tag: "LAL-01",  prio: 2, msg: "Level LOW" },
    LL: { tag: "LALL-01", prio: 1, msg: "Level LOW-LOW, tangki kosong" }
  };

  // ---------- Build static SVG parts ----------
  // Tank faceplate: inner 22 (100%) .. 278 (0%)
  var FP_TOP = 22, FP_BOT = 278;
  function fpY(p) { return FP_BOT - (FP_BOT - FP_TOP) * p / 100; }
  (function buildFaceplate() {
    var g = $("fpScale");
    g.setAttribute("class", "fp-scale");
    for (var p = 0; p <= 100; p += 10) {
      var y = fpY(p), major = p % 20 === 0;
      g.appendChild(svgEl("line", { x1: major ? 46 : 52, y1: y, x2: 58, y2: y }));
      if (major) g.appendChild(svgEl("text", { x: 28, y: y + 3.5 }, String(p)));
    }
    g.appendChild(svgEl("line", { x1: 58, y1: fpY(0), x2: 58, y2: fpY(100) }));
    var sp = $("fpSp");
    [["HH", SP.HH, "var(--red)"], ["H", SP.H, "var(--amber)"], ["L", SP.L, "var(--amber)"], ["LL", SP.LL, "var(--red)"]].forEach(function (d) {
      var y = fpY(d[1]);
      sp.appendChild(svgEl("line", { x1: 62, y1: y, x2: 94, y2: y, stroke: d[2], "class": "sp-line" }));
      sp.appendChild(svgEl("path", { d: "M99 " + y + " l6 -4 v8 z", fill: d[2] }));
      sp.appendChild(svgEl("text", { x: 108, y: y + 3.5, fill: d[2], "class": "sp-txt" }, d[0] + " " + d[1]));
    });
  })();

  // P&ID tank: inner 112 (100%) .. 388 (0%)
  var PID_TOP = 112, PID_BOT = 388;
  function pidY(p) { return PID_BOT - (PID_BOT - PID_TOP) * p / 100; }
  (function buildPidSp() {
    var sp = $("pidSp");
    [["HH", SP.HH, "var(--red)"], ["H", SP.H, "var(--amber)"], ["L", SP.L, "var(--amber)"], ["LL", SP.LL, "var(--red)"]].forEach(function (d) {
      var y = pidY(d[1]);
      sp.appendChild(svgEl("line", { x1: 562, y1: y, x2: 738, y2: y, stroke: d[2], "class": "sp-line" }));
      sp.appendChild(svgEl("text", { x: 566, y: y - 3, fill: d[2], "class": "sp-txt" }, d[0]));
    });
  })();

  // ---------- Trend ----------
  var TR = { x0: 44, x1: 596, y0: 14, y1: 190 };
  function trY(v) { return TR.y1 - (TR.y1 - TR.y0) * v / 100; }
  function drawTrend(hist) {
    var s = $("trend");
    var w = Math.max(320, Math.round(s.getBoundingClientRect().width) || 640);
    s.setAttribute("viewBox", "0 0 " + w + " 220");
    TR.x1 = w - 40;
    while (s.firstChild) s.removeChild(s.firstChild);
    for (var v = 0; v <= 100; v += 25) {
      s.appendChild(svgEl("line", { x1: TR.x0, y1: trY(v), x2: TR.x1, y2: trY(v), "class": "grid" }));
      s.appendChild(svgEl("text", { x: TR.x0 - 8, y: trY(v) + 3.5, "text-anchor": "end", "class": "ax" }, v + "%"));
    }
    var mins = [-120, -90, -60, -30, 0];
    mins.forEach(function (sec) {
      var x = TR.x1 + (TR.x1 - TR.x0) * sec / 120;
      s.appendChild(svgEl("line", { x1: x, y1: TR.y0, x2: x, y2: TR.y1, "class": "grid" }));
      s.appendChild(svgEl("text", { x: x, y: TR.y1 + 16, "text-anchor": "middle", "class": "ax" }, sec === 0 ? "sekarang" : sec + " s"));
    });
    [["sp-hh", SP.HH, "HH", "var(--red)"], ["sp-h", SP.H, "H", "var(--amber)"], ["sp-l", SP.L, "L", "var(--amber)"], ["sp-ll", SP.LL, "LL", "var(--red)"]].forEach(function (d) {
      s.appendChild(svgEl("line", { x1: TR.x0, y1: trY(d[1]), x2: TR.x1, y2: trY(d[1]), "class": d[0] }));
      s.appendChild(svgEl("text", { x: TR.x1 + 6, y: trY(d[1]) + 3.5, fill: d[3], "class": "sp-lbl" }, d[2]));
    });
    var n = hist.length;
    if (n < 2) return;
    var step = (TR.x1 - TR.x0) / (HIST_LEN - 1);
    var startX = TR.x1 - (n - 1) * step;
    var pts = hist.map(function (v, i) { return (startX + i * step).toFixed(1) + "," + trY(v).toFixed(1); });
    s.appendChild(svgEl("polygon", { points: startX + "," + TR.y1 + " " + pts.join(" ") + " " + TR.x1 + "," + TR.y1, "class": "pv-area" }));
    s.appendChild(svgEl("polyline", { points: pts.join(" "), "class": "pv" }));
    s.appendChild(svgEl("circle", { cx: TR.x1, cy: trY(hist[n - 1]), r: 3.5, "class": "now" }));
  }

  // ---------- Alarm summary ----------
  var alarms = [];
  var activeAlarm = null;

  function statusText(a) {
    if (a.active) return a.acked ? ["AKTIF · ACK", "ack"] : ["AKTIF · UNACK", "unack"];
    return a.acked ? ["NORMAL", "rtn"] : ["NORMAL · UNACK", "unack"];
  }
  function renderAlarms() {
    var body = $("alarmRows");
    if (!alarms.length) {
      body.innerHTML = '<tr><td colspan="5" class="empty">Belum ada kejadian alarm.</td></tr>';
    } else {
      body.innerHTML = alarms.map(function (a) {
        var st = statusText(a);
        return '<tr class="' + (st[1] === "unack" ? "unack" : "") + '">' +
          '<td class="mono">' + a.time + '</td>' +
          '<td class="mono">' + a.tag + '</td>' +
          '<td><span class="pri p' + a.prio + '">' + a.prio + '</span></td>' +
          '<td class="msg">' + a.msg + '</td>' +
          '<td><span class="st ' + st[1] + '">' + st[0] + '</span></td></tr>';
      }).join("");
    }
    var active = alarms.filter(function (a) { return a.active; });
    var tb = $("tbAlarm");
    $("tbAlarmCount").textContent = active.length;
    tb.classList.toggle("active", active.some(function (a) { return a.prio === 1; }));
    tb.classList.toggle("warn", active.length > 0 && !active.some(function (a) { return a.prio === 1; }));

    var banner = $("alarmBanner");
    banner.classList.remove("crit", "warn");
    if (activeAlarm) {
      banner.classList.add(activeAlarm.prio === 1 ? "crit" : "warn");
      $("abPri").textContent = "PRIO " + activeAlarm.prio;
      $("abMsg").textContent = activeAlarm.tag + " — " + activeAlarm.msg;
      $("abTime").textContent = activeAlarm.time;
    } else {
      $("abPri").textContent = "OK";
      $("abMsg").textContent = "Tidak ada alarm aktif";
      $("abTime").textContent = "";
    }
  }
  function onStateChange(code) {
    if (activeAlarm) { activeAlarm.active = false; activeAlarm = null; }
    var def = ALARM_DEF[code];
    if (def) {
      activeAlarm = { time: hhmmss(new Date()), tag: def.tag, prio: def.prio, msg: def.msg, active: true, acked: false };
      alarms.unshift(activeAlarm);
      if (alarms.length > 12) alarms.pop();
    }
    renderAlarms();
  }
  $("btnAck").addEventListener("click", function () {
    alarms.forEach(function (a) { a.acked = true; });
    renderAlarms();
  });

  // ---------- Toast notifications ----------
  var TOAST_MS = { crit: 9000, warn: 6000, ok: 4500 };
  var toastStack = $("toastStack");
  function showToast(sev, tag, title, msg) {
    if (!toastStack) return;
    var el = document.createElement("div");
    el.className = "toast " + sev;
    el.innerHTML =
      '<span class="toast-dot"></span>' +
      '<div class="toast-body">' +
        '<div class="toast-title"><b></b><span></span></div>' +
        '<div class="toast-msg"></div>' +
      '</div>' +
      '<button class="toast-close" type="button" aria-label="Tutup notifikasi">&times;</button>';
    el.querySelector(".toast-title b").textContent = title;
    el.querySelector(".toast-title span").textContent = tag;
    el.querySelector(".toast-msg").textContent = msg;
    var timer;
    function dismiss() {
      clearTimeout(timer);
      if (el.classList.contains("leaving")) return;
      el.classList.add("leaving");
      el.addEventListener("animationend", function () { el.remove(); }, { once: true });
      setTimeout(function () { el.remove(); }, 250); // fallback jika reduced-motion
    }
    el.querySelector(".toast-close").addEventListener("click", dismiss);
    el.addEventListener("mouseenter", function () { clearTimeout(timer); });
    el.addEventListener("mouseleave", function () { timer = setTimeout(dismiss, 2000); });
    toastStack.appendChild(el);
    while (toastStack.children.length > 4) toastStack.removeChild(toastStack.firstChild);
    timer = setTimeout(dismiss, TOAST_MS[sev] || 6000);
  }

  // ---------- Notifikasi sistem (Web Notification API) ----------
  // Beda dari toast: ini muncul di level OS, jadi tetap kelihatan walau
  // tab/browser sedang tidak dibuka (asal browser & OS mengizinkan).
  var btnNotif = $("btnNotif"), notifNote = $("notifNote");
  var NOTIF_ICON = { crit: "icons/clean/lamp_alarm.png", warn: "icons/clean/lamp_warning.png", ok: "icons/clean/lamp_normal.png" };
  var notifSupported = "Notification" in window;

  function updateNotifBtn() {
    if (!btnNotif) return;
    if (!notifSupported) {
      btnNotif.textContent = "TIDAK DIDUKUNG BROWSER";
      btnNotif.className = "notif-btn unsupported";
      btnNotif.disabled = true;
      return;
    }
    var perm = Notification.permission;
    if (perm === "granted") {
      btnNotif.textContent = "NOTIFIKASI AKTIF";
      btnNotif.className = "notif-btn granted";
      notifNote.textContent = "Alarm HH/H/L/LL akan muncul sebagai notifikasi sistem walau tab ini tidak sedang dibuka.";
    } else if (perm === "denied") {
      btnNotif.textContent = "NOTIFIKASI DIBLOKIR";
      btnNotif.className = "notif-btn denied";
      notifNote.textContent = "Diblokir di pengaturan browser. Aktifkan lewat ikon gembok di address bar untuk mengizinkan lagi.";
    } else {
      btnNotif.textContent = "IZINKAN NOTIFIKASI";
      btnNotif.className = "notif-btn";
      notifNote.textContent = "Muncul walau tab/browser ini sedang tidak dibuka.";
    }
  }
  if (btnNotif) {
    updateNotifBtn();
    btnNotif.addEventListener("click", function () {
      if (!notifSupported || Notification.permission !== "default") return;
      Notification.requestPermission().then(updateNotifBtn).catch(function () {});
    });
  }

  function notifySystem(sev, tag, title, msg) {
    if (!notifSupported || Notification.permission !== "granted") return;
    // Kalau tab ini sedang aktif dilihat, toast di dalam web sudah cukup;
    // notifikasi sistem diprioritaskan untuk saat operator sedang tidak menatap layar.
    if (!document.hidden && document.hasFocus()) return;
    try {
      var n = new Notification("[" + tag + "] " + title, {
        body: msg,
        tag: "gt-scada-" + tag, // notifikasi tag sama akan menimpa yang lama, tidak menumpuk
        renotify: true,
        icon: NOTIF_ICON[sev] || NOTIF_ICON.ok,
        silent: sev === "ok"
      });
      n.onclick = function () { window.focus(); n.close(); };
    } catch (e) { /* beberapa browser menolak Notification() langsung, diabaikan */ }
  }

  // ---------- Icon helpers ----------
  var ICON = "icons/clean/";
  var LAMP = { ok: "lamp_normal.png", warn: "lamp_warning.png", crit: "lamp_alarm.png", off: "lamp_off.png" };
  ["tank_0", "tank_1", "tank_2", "tank_3", "tank_4", "lamp_normal", "lamp_warning", "lamp_alarm", "lamp_off", "bell_on", "bell_off", "pump_on", "pump_off"]
    .forEach(function (n) { new Image().src = ICON + n + ".png"; });
  function setSrc(img, src) { if (img.getAttribute("src") !== src) img.setAttribute("src", src); }

  var lampSev = "ok", lampBlink = false, blinkPhase = true;
  var reduceMotion = window.matchMedia && window.matchMedia("(prefers-reduced-motion: reduce)").matches;
  function updateLamp() {
    var show = lampBlink && !blinkPhase && !reduceMotion ? "off" : lampSev;
    setSrc($("lampImg"), ICON + LAMP[show]);
  }
  setInterval(function () { blinkPhase = !blinkPhase; if (lampBlink) updateLamp(); }, 450);

  // ---------- Process state ----------
  // filling  : mode pengisian dipilih operator
  // silenced : operator menekan MATIKAN ALARM untuk kejadian alarm saat ini
  var state = { level: 48, filling: false, silenced: false };
  var levelHistory = [];
  var lastCode = null;

  var simLevel = $("simLevel"), simLevelVal = $("simLevelVal"), simAuto = $("simAuto");
  var btnIdle = $("btnIdle"), btnFill = $("btnFill"), btnSilence = $("btnSilence");

  var firstRender = true;
  function render() {
    var L = Math.max(0, Math.min(100, state.level));
    var ev = evaluate(L, state.filling);
    if (ev.code !== lastCode) {
      state.silenced = false; // alarm baru -> buzzer aktif lagi
      onStateChange(ev.code);
      lastCode = ev.code;
      if (!firstRender) {
        var toastTag = (ALARM_DEF[ev.code] && ALARM_DEF[ev.code].tag) || "LT-01";
        showToast(ev.sev, toastTag, ev.text, ev.desc);
        notifySystem(ev.sev, toastTag, ev.text, ev.desc);
      }
    }
    firstRender = false;
    var floatTrip = L >= FLOAT_TRIP;
    var flow = state.filling ? 118 + Math.round(Math.sin(Date.now() / 1700) * 6) : 0;
    var vol = Math.round(CAPACITY_L * L / 100);
    var dist = SENSOR_OFFSET_CM + TANK_H_CM * (1 - L / 100);
    var buzzerOn = ev.blink && !state.silenced;

    // faceplate
    var y = fpY(L);
    $("fpWater").setAttribute("y", y);
    $("fpWater").setAttribute("height", FP_BOT - y);
    $("fpPointer").setAttribute("d", "M44 " + y + " l-8 -5 v10 z");
    $("rLevel").textContent = L.toFixed(1);
    $("rDist").textContent = dist.toFixed(1);
    var rs = $("rState");
    rs.textContent = ev.text;
    rs.className = "r-v r-state " + ev.sev;

    // tank illustration & status lamp
    var tankIdx = L < 12 ? 0 : L < 35 ? 1 : L < 60 ? 2 : L < 85 ? 3 : 4;
    setSrc($("tankImg"), ICON + "tank_" + tankIdx + ".png");
    $("pfdTank").setAttribute("href", ICON + "tank_" + tankIdx + ".png");
    lampSev = ev.sev; lampBlink = ev.blink;
    updateLamp();
    var si = $("siState");
    si.textContent = ev.text;
    si.className = "si-state " + ev.sev;
    $("siDesc").textContent = ev.desc;
    var rowIdx = { HH: 0, H: 1, N: 2, FULL: 2, L: 3, LL: 4 }[ev.code];
    document.querySelectorAll(".sp-tbl tr").forEach(function (tr, i) { tr.classList.toggle("cur", i === rowIdx); });

    // operator panel
    var buz = $("buzImg"), bs = $("buzState");
    setSrc(buz, ICON + (buzzerOn ? "bell_on.png" : "bell_off.png"));
    buz.classList.toggle("sounding", buzzerOn);
    buz.classList.toggle("idle", !ev.blink);
    if (buzzerOn) {
      bs.textContent = "BUZZER BUNYI"; bs.className = "ps-state crit";
      $("buzSub").textContent = "Alarm " + ev.text + " aktif di panel M-01.";
    } else if (ev.blink) {
      bs.textContent = "ALARM DIMATIKAN"; bs.className = "ps-state warn";
      $("buzSub").textContent = "Buzzer diam, lampu tetap menyala sampai kondisi normal.";
    } else {
      bs.textContent = "BUZZER DIAM"; bs.className = "ps-state";
      $("buzSub").textContent = "Tidak ada alarm kritis.";
    }
    btnSilence.disabled = !buzzerOn;
    btnIdle.setAttribute("aria-pressed", state.filling ? "false" : "true");
    btnFill.setAttribute("aria-pressed", state.filling ? "true" : "false");
    $("kvFlow").textContent = flow + " L/min";
    $("kLevel").textContent = L.toFixed(1);
    $("kHeight").textContent = (TANK_H_CM * L / 100 / 100).toFixed(2);
    $("kVol").textContent = vol.toLocaleString("id-ID");
    $("kFlow").textContent = flow;
    $("kUpd").textContent = hhmmss(new Date());
    var pfdPump = $("pfdPump");
    if (pfdPump) pfdPump.setAttribute("href", ICON + (state.filling ? "pump_on.png" : "pump_off.png"));
    $("kvLsh").textContent = floatTrip ? "TRIP (penuh)" : "NORMAL";

    // P&ID live
    var py = pidY(L);
    $("pidWater").setAttribute("y", py);
    $("pidWater").setAttribute("height", PID_BOT - py);
    $("pidLevelTxt").textContent = L.toFixed(1) + " %";
    $("pidPump").classList.toggle("running", state.filling);
    $("pidPumpTxt").textContent = state.filling ? "PENGISIAN" : "IDLE";
    $("pidFlowTxt").textContent = flow + " L/min";
    $("pidLsh").classList.toggle("alarm", floatTrip);
    $("pidBuzzer").classList.toggle("alarm-blink", buzzerOn);

    // sim widgets
    simLevel.value = Math.round(L);
    simLevelVal.textContent = Math.round(L) + " %";
  }

  function sample() {
    levelHistory.push(state.level);
    if (levelHistory.length > HIST_LEN) levelHistory.shift();
    drawTrend(levelHistory);
  }

  // ---------- Controls ----------
  btnIdle.addEventListener("click", function () { state.filling = false; render(); });
  btnFill.addEventListener("click", function () { state.filling = true; render(); });
  btnSilence.addEventListener("click", function () {
    state.silenced = true;
    alarms.forEach(function (a) { a.acked = true; });
    renderAlarms();
    render();
    showToast("ok", "XA-01", "ALARM DIMATIKAN", "Buzzer disenyapkan oleh operator. Lampu tetap aktif sampai kondisi normal.");
  });

  simLevel.addEventListener("input", function () {
    simAuto.checked = false;
    state.level = parseFloat(simLevel.value);
    render();
  });

  // ---------- Simulation tick ----------
  // "Siklus otomatis" mensimulasikan operator yang menekan IDLE/PENGISIAN
  // bergantian di batas atas & bawah, supaya data dummy terus bergerak
  // selama tidak ada koneksi ESP32 nyata.
  function tick() {
    // Saat liveMode aktif, level/mode pengisian dipasok oleh checkEsp32() dari
    // data Supabase, jadi loop simulasi dummy di sini dilewati.
    if (!liveMode && simAuto.checked) {
      if (state.filling && state.level >= 92) state.filling = false;
      if (!state.filling && state.level <= 8) state.filling = true;
      state.level += state.filling ? 1.1 : -0.55;
      state.level = Math.max(0, Math.min(100, state.level));
    }
    if (!liveMode) { render(); sample(); }
  }

  for (var i = 0; i < 40; i++) levelHistory.push(48 + Math.sin(i / 7) * 1.5);
  render();
  drawTrend(levelHistory);
  setInterval(tick, TICK_MS);
  window.addEventListener("resize", function () { drawTrend(levelHistory); });
  document.querySelectorAll('[data-go="dashboard"]').forEach(function (el) {
    el.addEventListener("click", function () { drawTrend(levelHistory); });
  });
})();
