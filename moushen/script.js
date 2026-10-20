/* Lumen — 90s product film · 20 scenes · 3D camera rig + real code examples. No dependencies. */
(function () {
  "use strict";

  var DURATION = 90000;

  /* ---------- syntax highlighting ---------- */

  var KW = {
    let: 1, fn: 1, struct: 1, enum: 1, namespace: 1, ret: 1, return: 1,
    if: 1, else: 1, while: 1, for: 1, echo: 1, out: 1, outln: 1, import: 1,
    ref: 1, strict: 1, static: 1, private: 1, is: 1, type: 1, new: 1,
    break: 1, continue: 1, true: 1, false: 1, int: 1, program: 1, var: 1,
    begin: 1, end: 1, to: 1, do: 1, main: 1
  };
  var TYP = {
    i64: 1, i32: 1, i16: 1, i8: 1, u64: 1, f32: 1, f64: 1, str: 1, bool: 1,
    char: 1, void: 1, auto: 1, Type: 1, Null: 1, Integer: 1
  };

  function esc(s) {
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  }

  function hl(src) {
    var re = /(#[^\n]*)|(\/\/[^\n]*)|("(?:\\.|[^"\\\n])*")|(\b\d+(?:\.\d+)?\b)|([A-Za-z_][A-Za-z0-9_]*)|([^\n])/g;
    var out = "", last = 0, m;
    while ((m = re.exec(src)) !== null) {
      out += esc(src.slice(last, m.index));
      var whole = m[0];
      var dir = m[1], cm = m[2], str = m[3], num = m[4], word = m[5], punct = m[6];
      var after = src[m.index + whole.length];
      if (dir || cm) out += '<span class="c-co">' + esc(dir || cm) + "</span>";
      else if (str) out += '<span class="c-st">' + esc(str) + "</span>";
      else if (num) out += '<span class="c-nu">' + esc(num) + "</span>";
      else if (word) {
        if (KW[word]) out += '<span class="c-kw">' + word + "</span>";
        else if (TYP[word]) out += '<span class="c-ty">' + word + "</span>";
        else if (/^[A-Z]/.test(word)) out += '<span class="c-ty">' + word + "</span>";
        else if (after === "(") out += '<span class="c-fn">' + word + "</span>";
        else out += esc(word);
      } else if (punct) out += '<span class="c-pu">' + esc(punct) + "</span>";
      last = m.index + whole.length;
    }
    out += esc(src.slice(last));
    return out;
  }

  /* ---------- real code examples (ex1…ex6) ---------- */

  var EX_SRC = {
    ex1: 'let x = 5;\nlet name = "Аня";\nlet pi = 3.14;\n\noutln x;\noutln name;',
    ex2: 'strict let age: i64 = 15;\nstrict let price: f64 = 9.99;\n\nlet total = age + 1;\noutln total;',
    ex3: 'fn add(a: i64, b: i64) -> i64 {\n    ret a + b;\n}\n\nlet sum = add(2, 3);\noutln sum;',
    ex4: 'struct Point {\n    x: i64;\n    y: i64;\n}\n\nlet p = Point { x: 3, y: 4 };\noutln p.x;',
    ex5: 'for (let i = 1; i <= 5; i = i + 1;) {\n    if (i % 2 == 0) {\n        outln i;\n    }\n}',
    ex6: 'let base = 10;\nref let alias = base;\nalias = 20;\n\noutln base;'
  };

  function lineDelay(i) { return (0.45 + i * 0.24).toFixed(2) + "s"; }

  function buildExample(sc) {
    var pre = sc.querySelector(".ex-code");
    if (!pre || pre.dataset.built) return;
    pre.dataset.built = "1";
    var lines = (EX_SRC[pre.dataset.src] || "").split("\n");
    var out = "";
    for (var i = 0; i < lines.length; i++) {
      var last = i === lines.length - 1;
      var body = lines[i].length ? hl(lines[i]) : "\u200b";
      out += '<span class="cline" style="--ld:' + lineDelay(i) + '">' +
        '<span class="ln">' + (i + 1) + "</span>" +
        '<span class="lc' + (last ? " caret" : "") + '">' + body + "</span></span>";
    }
    pre.innerHTML = out;
  }

  /* ---------- sound (synthesized, no assets) ---------- */

  var audio = (function () {
    var ctx = null, master = null, noiseBuf = null, muted = false, drone = null;

    function init() {
      if (ctx) return;
      var AC = window.AudioContext || window.webkitAudioContext;
      if (!AC) return;
      ctx = new AC();
      master = ctx.createGain();
      master.gain.value = muted ? 0 : 0.9;
      master.connect(ctx.destination);
      var len = Math.floor(ctx.sampleRate * 1.2);
      noiseBuf = ctx.createBuffer(1, len, ctx.sampleRate);
      var d = noiseBuf.getChannelData(0);
      for (var i = 0; i < len; i++) d[i] = Math.random() * 2 - 1;
    }

    function resume() { init(); if (ctx && ctx.state === "suspended") ctx.resume(); }
    function now() { return ctx ? ctx.currentTime : 0; }

    function tone(freq, dur, gain, type, when) {
      if (!ctx) return;
      var t = (when || now());
      var o = ctx.createOscillator(), g = ctx.createGain();
      o.type = type || "sine";
      o.frequency.setValueAtTime(freq, t);
      g.gain.setValueAtTime(0, t);
      g.gain.linearRampToValueAtTime(gain, t + Math.min(0.012, dur * 0.3));
      g.gain.exponentialRampToValueAtTime(0.0001, t + dur);
      o.connect(g); g.connect(master);
      o.start(t); o.stop(t + dur + 0.02);
    }

    function noise(dur, gain, f0, f1, q) {
      if (!ctx || !noiseBuf) return;
      var t = now();
      var s = ctx.createBufferSource(); s.buffer = noiseBuf;
      var bp = ctx.createBiquadFilter(); bp.type = "bandpass"; bp.Q.value = q || 0.8;
      bp.frequency.setValueAtTime(f0, t);
      bp.frequency.exponentialRampToValueAtTime(f1, t + dur);
      var g = ctx.createGain();
      g.gain.setValueAtTime(0.0001, t);
      g.gain.exponentialRampToValueAtTime(gain, t + dur * 0.25);
      g.gain.exponentialRampToValueAtTime(0.0001, t + dur);
      s.connect(bp); bp.connect(g); g.connect(master);
      s.start(t); s.stop(t + dur + 0.02);
    }

    function startDrone() {
      if (!ctx || drone) return;
      var g = ctx.createGain();
      g.gain.value = 0.0001;
      g.gain.exponentialRampToValueAtTime(0.04, ctx.currentTime + 2.5);
      g.connect(master);
      var o1 = ctx.createOscillator(); o1.type = "sine"; o1.frequency.value = 55;
      var o2 = ctx.createOscillator(); o2.type = "sine"; o2.frequency.value = 82.41;
      var lfo = ctx.createOscillator(); lfo.type = "sine"; lfo.frequency.value = 0.07;
      var lg = ctx.createGain(); lg.gain.value = 6;
      lfo.connect(lg); lg.connect(o2.frequency);
      o1.connect(g); o2.connect(g);
      o1.start(); o2.start(); lfo.start();
      drone = { o1: o1, o2: o2, lfo: lfo, g: g };
    }

    return {
      start: function () { init(); resume(); startDrone(); },
      mute: function (m) { muted = m; if (master) master.gain.setTargetAtTime(m ? 0 : 0.9, now(), 0.05); },
      isMuted: function () { return muted; },
      tick: function (f) { tone(f || 1180, 0.05, 0.045, "triangle"); },
      click: function () { noise(0.03, 0.045, 1800 + Math.random() * 900, 1200, 1.4); },
      whoosh: function () { noise(0.45, 0.08, 260, 2600, 0.6); },
      chime: function () { var t = now(); tone(659.25, 0.5, 0.06, "sine", t); tone(987.77, 0.55, 0.045, "sine", t + 0.06); }
    };
  })();

  /* ---------- dom ---------- */

  var scenes = Array.prototype.slice.call(document.querySelectorAll(".scene"));
  var railSec = document.getElementById("railSec");
  var railIdx = document.getElementById("railIdx");
  var railName = document.getElementById("railName");
  var progressFill = document.getElementById("progressFill");
  var timecode = document.getElementById("timecode");
  var beam = document.querySelector(".beam");
  var aurora = document.querySelector(".aurora");
  var gridBg = document.getElementById("gridBg");
  var gate = document.getElementById("gate");
  var startBtn = document.getElementById("startBtn");
  var chapterDots = document.getElementById("chapter") ? document.getElementById("chapter").children : [];

  function fireCues(sc, local) {
    var key = sc.dataset.key;
    if (!key) return;
    var list = CUES[key];
    if (!list) return;
    for (var k = 0; k < list.length; k++) {
      var c = list[k], id = key + "@" + c.at;
      if (!fired[id] && local >= c.at) { fired[id] = 1; c.fn(); }
    }
  }

  function exCues(srcKey) {
    var n = (EX_SRC[srcKey] || "").split("\n").length;
    var out = [];
    for (var i = 0; i < n; i++) {
      out.push({ at: Math.round((0.45 + i * 0.24) * 1000), fn: (function () { return function () { audio.tick(1250); }; })() });
    }
    return out;
  }

  var CUES = {
    race: [{ at: 800, fn: function () { audio.tick(430); } }, { at: 2400, fn: function () { audio.chime(); } }],
    ex1: exCues("ex1"), ex2: exCues("ex2"), ex3: exCues("ex3"),
    ex4: exCues("ex4"), ex5: exCues("ex5"), ex6: exCues("ex6"),
    err: [{ at: 950, fn: function () { audio.tick(1500); } }, { at: 1300, fn: function () { audio.tick(1500); } }],
    run: [
      { at: 350, fn: function () { audio.tick(1300); } },
      { at: 1400, fn: function () { audio.tick(1300); } },
      { at: 2100, fn: function () { audio.tick(1300); } },
      { at: 3400, fn: function () { audio.chime(); } }
    ],
    final: [{ at: 600, fn: function () { audio.chime(); } }]
  };

  /* ---------- camera (3D keyframes per scene) ---------- */

  var CAMS = {
    dollyIn: { z: [0, 230] },
    dollyOut: { z: [230, 0] },
    truckL: { x: [0, -170], z: [110, 110] },
    truckR: { x: [0, 170], z: [110, 110] },
    arcRight: { x: [0, 120], ry: [0, 5.5], z: [110, 110] },
    arcLeft: { x: [0, -120], ry: [0, -5.5], z: [110, 110] },
    pedUp: { y: [0, -140], z: [90, 90] },
    pedDown: { y: [0, 140], z: [90, 90] },
    crane: { y: [150, -150], z: [0, 130] },
    pushRoll: { z: [0, 230], rz: [0, -1.8] }
  };

  function easeIO(k) { return k < 0.5 ? 2 * k * k : 1 - Math.pow(-2 * k + 2, 2) / 2; }
  function lerp(a, e) { return a[0] + (a[1] - a[0]) * e; }
  function prop(c, e, name) { return c[name] ? lerp(c[name], e) : 0; }

  function applyCam(sc, local, dur) {
    var c = CAMS[sc.dataset.cam] || { z: [0, 90] };
    var k = dur > 0 ? Math.min(1, Math.max(0, local / dur)) : 1;
    var e = easeIO(k);
    var tsec = local / 1000;
    var nx = Math.sin(tsec * 0.7) * 1.6 + Math.sin(tsec * 1.9) * 0.7;
    var ny = Math.cos(tsec * 0.6) * 1.4 + Math.sin(tsec * 1.3) * 0.5;
    var x = prop(c, e, "x") + nx;
    var y = prop(c, e, "y") + ny;
    var z = prop(c, e, "z");
    var ry = prop(c, e, "ry");
    var rz = prop(c, e, "rz");

    var inner = sc.querySelector(".scene-inner");
    if (inner) {
      inner.style.transform = "translate3d(" + x.toFixed(2) + "px," + y.toFixed(2) + "px," + z.toFixed(1) + "px) " +
        "rotateY(" + ry.toFixed(3) + "deg) rotateZ(" + rz.toFixed(3) + "deg)";
    }
    var deco = sc.querySelector(".deco");
    if (deco) {
      deco.style.transform = "translate3d(" + (x * 0.4).toFixed(2) + "px," + (y * 0.4).toFixed(2) + "px,0)";
    }
  }

  /* ---------- counters ---------- */

  function startCounters(scene) {
    var els = scene.querySelectorAll("[data-count]");
    Array.prototype.forEach.call(els, function (el) {
      if (el.dataset.done) return;
      el.dataset.done = "1";
      var to = parseFloat(el.dataset.count);
      var dec = parseInt(el.dataset.dec || "0", 10);
      var suf = el.dataset.suffix || "";
      var t0 = null, dur = 1300;
      function step(ts) {
        if (t0 === null) t0 = ts;
        var k = Math.min(1, (ts - t0) / dur);
        var e = 1 - Math.pow(1 - k, 3);
        el.textContent = (to * e).toFixed(dec) + suf;
        if (k < 1) requestAnimationFrame(step);
        else el.textContent = to.toFixed(dec) + suf;
      }
      requestAnimationFrame(step);
    });
  }

  /* ---------- director ---------- */

  var t = 0, running = false, lastTs = 0, curIdx = -1, fired = {}, frozen = false;

  function timeStr(ms) {
    var s = Math.floor(ms / 1000);
    return Math.floor(s / 60) + ":" + ("0" + (s % 60)).slice(-2);
  }

  function activeIndex(time) {
    for (var i = 0; i < scenes.length; i++) {
      var s = parseInt(scenes[i].dataset.start, 10);
      var e = parseInt(scenes[i].dataset.end, 10);
      if (time >= s && time < e) return i;
    }
    return scenes.length - 1;
  }

  function setScene(i) {
    if (i === curIdx) return;
    curIdx = i;
    for (var k = 0; k < scenes.length; k++) scenes[k].classList.toggle("active", k === i);
    var sc = scenes[i];
    railSec.textContent = sc.dataset.sec || "";
    railIdx.textContent = sc.dataset.idx || "—";
    railIdx.classList.remove("flip"); void railIdx.offsetWidth; railIdx.classList.add("flip");
    railName.textContent = sc.dataset.label || "";
    beam.classList.remove("fire"); void beam.offsetWidth; beam.classList.add("fire");
    var n = parseInt(sc.dataset.idx, 10);
    for (var d = 0; d < chapterDots.length; d++) chapterDots[d].classList.toggle("on", !isNaN(n) && d < n);
    buildExample(sc);
    startCounters(sc);
  }

  function render(time) {
    var i = activeIndex(time);
    setScene(i);
    var sc = scenes[i];
    var local = time - parseInt(sc.dataset.start, 10);
    var dur = parseInt(sc.dataset.end, 10) - parseInt(sc.dataset.start, 10);
    applyCam(sc, local, dur);
    fireCues(sc, local);

    var p = Math.min(1, time / DURATION);
    if (gridBg) gridBg.style.transform = "translate3d(" + ((p * 6 - 3)).toFixed(2) + "px," + ((p * 4 - 2)).toFixed(2) + "px,0)";
    if (aurora) aurora.style.backgroundPosition = (p * 60).toFixed(1) + "% " + (p * 40).toFixed(1) + "%";

    progressFill.style.width = (p * 100).toFixed(2) + "%";
    timecode.textContent = timeStr(time) + " / " + timeStr(DURATION);
  }

  function frame(ts) {
    if (!running) return;
    if (!lastTs) lastTs = ts;
    var dt = ts - lastTs; lastTs = ts;
    if (dt > 120) dt = 120;
    t += dt;
    render(t);
    if (t >= DURATION) { running = false; render(DURATION); return; }
    requestAnimationFrame(frame);
  }

  function start() { if (running) return; lastTs = 0; running = true; requestAnimationFrame(frame); }

  function restart() {
    t = 0; running = false; lastTs = 0; curIdx = -1; fired = {};
    Array.prototype.forEach.call(document.querySelectorAll("[data-count]"), function (el) {
      delete el.dataset.done; el.textContent = "0";
    });
    render(0);
    start();
  }

  /* ---------- url seek (deterministic frames for review) ---------- */

  var params = new URLSearchParams(window.location.search);
  var seek = params.get("t");

  function boot() {
    if (seek !== null) {
      frozen = true;
      gate.classList.add("hide");
      t = Math.max(0, Math.min(DURATION, (parseFloat(seek) * 1000) || 0));
      render(t);
      if (params.get("play") === "1") { frozen = false; start(); }
      return;
    }
    render(0);
  }

  /* ---------- input ---------- */

  function enter() { audio.start(); gate.classList.add("hide"); restart(); }

  startBtn.addEventListener("click", enter);
  document.addEventListener("keydown", function (e) {
    if (!gate.classList.contains("hide")) {
      if (e.key === " " || e.key === "Enter") { e.preventDefault(); enter(); }
      return;
    }
    if (e.key === " " || e.key === "r" || e.key === "R") {
      e.preventDefault();
      if (frozen) { frozen = false; gate.classList.add("hide"); }
      restart();
    } else if (e.key === "f" || e.key === "F") {
      if (!document.fullscreenElement) { if (document.documentElement.requestFullscreen) document.documentElement.requestFullscreen(); }
      else if (document.exitFullscreen) document.exitFullscreen();
    } else if (e.key === "m" || e.key === "M") {
      audio.mute(!audio.isMuted());
    }
  });

  boot();
})();
