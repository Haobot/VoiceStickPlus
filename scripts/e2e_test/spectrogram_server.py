"""VoiceStick 调试音频频谱分析页：本地小服务 + 浏览器交互式频谱/波形调试。

用法：
    python scripts/e2e_test/spectrogram_server.py [--port 8765] [--dir PATH] [--no-browser]

- 自动读取 %APPDATA%\\VoiceStick\\config.toml 的 debug_audio_dir（--dir 可覆盖）
- 浏览器打开 http://127.0.0.1:8765 ，左侧选会话，右侧频谱图 + 波形图联动
- 交互：滚轮缩放时间轴、拖动平移、悬停读数（时间/频率/dB）、点击或空格播放

纯标准库实现，无第三方依赖。页面（HTML/JS）内嵌在本文件中，单文件可分
发。音频解码与 STFT 全部在浏览器内完成（Web Audio + JS FFT）。
"""

import argparse
import http.server
import json
import os
import re
import threading
import webbrowser
from pathlib import Path

PAGE = r"""<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="utf-8">
<title>VoiceStick 频谱调试</title>
<style>
  :root { --bg:#14161a; --panel:#1d2026; --fg:#cfd3da; --dim:#7d838d; --accent:#4da3ff; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--fg); font:13px/1.4 "Segoe UI",sans-serif; height:100vh; display:flex; flex-direction:column; }
  #app { display:flex; flex:1; min-height:0; }
  aside { width:280px; min-width:280px; background:var(--panel); border-right:1px solid #2b2f36; display:flex; flex-direction:column; }
  aside header { padding:10px 12px; display:flex; justify-content:space-between; align-items:center; border-bottom:1px solid #2b2f36; }
  aside header b { font-size:14px; }
  #refresh { background:#2b3038; color:var(--fg); border:1px solid #3a404a; border-radius:4px; padding:3px 10px; cursor:pointer; }
  #refresh:hover { border-color:var(--accent); }
  #sessions { list-style:none; margin:0; padding:0; overflow-y:auto; flex:1; }
  #sessions li { padding:7px 12px; cursor:pointer; border-bottom:1px solid #23272e; }
  #sessions li:hover { background:#262b33; }
  #sessions li.active { background:#24405e; }
  #sessions .name { display:block; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  #sessions .meta { color:var(--dim); font-size:11px; }
  main { flex:1; display:flex; flex-direction:column; min-width:0; }
  #toolbar { padding:8px 12px; background:var(--panel); border-bottom:1px solid #2b2f36; display:flex; gap:14px; align-items:center; flex-wrap:wrap; }
  #toolbar .fname { font-weight:600; }
  #toolbar label { color:var(--dim); }
  #toolbar select, #toolbar button { background:#2b3038; color:var(--fg); border:1px solid #3a404a; border-radius:4px; padding:3px 8px; cursor:pointer; }
  #play { min-width:56px; }
  #canvases { flex:1; display:flex; flex-direction:column; min-height:0; position:relative; }
  #spec { flex:3; width:100%; min-height:0; cursor:crosshair; }
  #wave { flex:1; width:100%; min-height:0; cursor:crosshair; border-top:1px solid #2b2f36; }
  #status { padding:5px 12px; background:var(--panel); border-top:1px solid #2b2f36; color:var(--dim); font-variant-numeric:tabular-nums; }
  #err { color:#ff7b72; padding:4px 12px; display:none; }
  canvas { display:block; }
</style>
</head>
<body>
<div id="app">
  <aside>
    <header><b>调试会话</b><button id="refresh">刷新</button></header>
    <ul id="sessions"></ul>
  </aside>
  <main>
    <div id="toolbar">
      <span class="fname" id="fname">（选择左侧会话）</span>
      <span id="dur"></span>
      <button id="play">播放</button>
      <button id="zoomreset">重置缩放</button>
      <label>频宽 <select id="frange">
        <option value="2500">0–2.5k</option>
        <option value="8000" selected>0–8k</option>
        <option value="0">全频</option>
      </select></label>
      <label>对比度 <input type="range" id="contrast" min="40" max="120" value="90" style="vertical-align:middle"></label>
    </div>
    <div id="err"></div>
    <div id="canvases">
      <canvas id="spec"></canvas>
      <canvas id="wave"></canvas>
    </div>
    <div id="status">—</div>
  </main>
</div>
<audio id="player" preload="auto"></audio>
<script>
"use strict";
// ================= 状态 =================
const S = {
  name: null,          // 当前文件名
  pcm: null,           // Float32Array 单声道
  sampleRate: 0,
  duration: 0,
  spec: null,          // Float32Array frames*bins，dB 值（相对最大值）
  frames: 0, bins: 0, hop: 512, win: 2048,
  viewStart: 0, viewLen: 0,   // 视口（帧）
  fmax: 8000,          // 显示频率上限（0=奈奎斯特）
  dynRange: 90,        // 动态范围 dB
  playing: false,
  hover: null,         // {canvas,x,y}
};
const $ = id => document.getElementById(id);
const specCv = $("spec"), waveCv = $("wave");
const player = $("player");

// ================= FFT（radix-2 迭代） =================
function makeFFT(n) {
  const rev = new Uint32Array(n);
  for (let i = 0; i < n; i++) {
    rev[i] = (rev[i >> 1] >> 1) | ((i & 1) ? n >> 1 : 0);
  }
  const cosT = new Float32Array(n / 2), sinT = new Float32Array(n / 2);
  for (let i = 0; i < n / 2; i++) {
    cosT[i] = Math.cos(-2 * Math.PI * i / n);
    sinT[i] = Math.sin(-2 * Math.PI * i / n);
  }
  return { n, rev, cosT, sinT };
}
function fftRadix2(tbl, re, im) {
  const n = tbl.n, rev = tbl.rev, cosT = tbl.cosT, sinT = tbl.sinT;
  for (let i = 0; i < n; i++) {
    if (i < rev[i]) {
      const tr = re[i], ti = im[i];
      re[i] = re[rev[i]]; im[i] = im[rev[i]];
      re[rev[i]] = tr; im[rev[i]] = ti;
    }
  }
  for (let size = 2; size <= n; size <<= 1) {
    const half = size >> 1, step = n / size;
    for (let i = 0; i < n; i += size) {
      for (let j = i, k = 0; j < i + half; j++, k += step) {
        const l = j + half;
        const xr = re[l] * cosT[k] - im[l] * sinT[k];
        const xi = re[l] * sinT[k] + im[l] * cosT[k];
        re[l] = re[j] - xr; im[l] = im[j] - xi;
        re[j] += xr; im[j] += xi;
      }
    }
  }
}

// ================= magma 色图（控制点插值 256 级） =================
const MAGMA_STOPS = [
  [0.00, [0, 0, 4]], [0.15, [28, 16, 68]], [0.35, [79, 18, 123]],
  [0.55, [182, 54, 121]], [0.75, [251, 137, 97]], [0.90, [254, 194, 135]],
  [1.00, [252, 253, 245]],
];
const LUT = new Uint8Array(256 * 3);
for (let i = 0; i < 256; i++) {
  const t = i / 255;
  let a = MAGMA_STOPS[0], b = MAGMA_STOPS[MAGMA_STOPS.length - 1];
  for (let s = 0; s < MAGMA_STOPS.length - 1; s++) {
    if (t >= MAGMA_STOPS[s][0] && t <= MAGMA_STOPS[s + 1][0]) { a = MAGMA_STOPS[s]; b = MAGMA_STOPS[s + 1]; break; }
  }
  const f = (t - a[0]) / Math.max(1e-9, b[0] - a[0]);
  for (let c = 0; c < 3; c++) LUT[i * 3 + c] = Math.round(a[1][c] + (b[1][c] - a[1][c]) * f);
}

// ================= 会话列表 =================
async function loadSessions(keepSelection) {
  const res = await fetch("/api/sessions", { cache: "no-store" });
  const list = await res.json();
  const ul = $("sessions");
  ul.innerHTML = "";
  for (const it of list) {
    const li = document.createElement("li");
    li.dataset.name = it.name;
    const kb = (it.size / 1024).toFixed(1);
    li.innerHTML = `<span class="name">${it.name}</span><span class="meta">${it.mtime} · ${kb} KB</span>`;
    li.onclick = () => selectSession(it.name);
    if (keepSelection && it.name === S.name) li.classList.add("active");
    ul.appendChild(li);
  }
}
$("refresh").onclick = () => loadSessions(true);

// ================= 加载与 STFT =================
async function selectSession(name) {
  document.querySelectorAll("#sessions li").forEach(li =>
    li.classList.toggle("active", li.dataset.name === name));
  $("err").style.display = "none";
  $("fname").textContent = name;
  stopPlayback();
  try {
    const buf = await (await fetch("/api/audio?name=" + encodeURIComponent(name))).arrayBuffer();
    player.src = URL.createObjectURL(new Blob([buf], { type: "audio/ogg" }));
    const ac = new (window.AudioContext || window.webkitAudioContext)();
    const audio = await ac.decodeAudioData(buf);
    ac.close();
    S.name = name;
    S.pcm = audio.getChannelData(0);
    S.sampleRate = audio.sampleRate;
    S.duration = audio.duration;
    $("dur").textContent = S.duration.toFixed(2) + " s · " + (S.sampleRate / 1000) + " kHz";
    await computeSpectrogram();
    S.viewStart = 0; S.viewLen = S.frames;
    drawAll();
  } catch (e) {
    $("err").textContent = "加载失败: " + e.message;
    $("err").style.display = "block";
  }
}

async function computeSpectrogram() {
  const { win, hop } = S;
  const pcm = S.pcm;
  const frames = Math.max(1, Math.floor((pcm.length - win) / hop) + 1);
  const bins = win / 2;
  const tbl = makeFFT(win);
  const hann = new Float32Array(win);
  for (let i = 0; i < win; i++) hann[i] = 0.5 * (1 - Math.cos(2 * Math.PI * i / win));
  const re = new Float32Array(win), im = new Float32Array(win);
  const spec = new Float32Array(frames * bins);
  let maxDb = -Infinity;
  for (let f = 0; f < frames; f++) {
    const off = f * hop;
    for (let i = 0; i < win; i++) { re[i] = (off + i < pcm.length ? pcm[off + i] : 0) * hann[i]; im[i] = 0; }
    fftRadix2(tbl, re, im);
    for (let b = 0; b < bins; b++) {
      const mag = Math.sqrt(re[b] * re[b] + im[b] * im[b]) / win;
      const db = 20 * Math.log10(mag + 1e-9);
      spec[f * bins + b] = db;
      if (db > maxDb) maxDb = db;
    }
    if (f % 60 === 59) { $("status").textContent = `计算频谱… ${f + 1}/${frames}`; await new Promise(r => setTimeout(r)); }
  }
  // 归一化：最大值 = 0dB
  for (let i = 0; i < spec.length; i++) spec[i] -= maxDb;
  S.spec = spec; S.frames = frames; S.bins = bins;
  $("status").textContent = "—";
}

// ================= 渲染 =================
function fitCanvas(cv) {
  const r = cv.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(r.width * dpr)), h = Math.max(1, Math.round(r.height * dpr));
  if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
  return { w, h, dpr };
}
function fmaxHz() { return S.fmax > 0 ? Math.min(S.fmax, S.sampleRate / 2) : S.sampleRate / 2; }

function drawSpec() {
  if (!S.spec) { const c = specCv.getContext("2d"); const { w, h } = fitCanvas(specCv); c.clearRect(0, 0, w, h); return; }
  const { w, h } = fitCanvas(specCv);
  const ctx = specCv.getContext("2d");
  const frames = S.frames, bins = S.bins;
  const hzPerBin = S.sampleRate / 2 / bins;
  const binTop = Math.min(bins, Math.ceil(fmaxHz() / hzPerBin));
  // 视口参数
  const v0 = Math.floor(S.viewStart), vlen = Math.max(1, Math.ceil(S.viewLen));
  // 离屏渲染当前视口
  const off = document.createElement("canvas");
  off.width = vlen; off.height = binTop;
  const octx = off.getContext("2d");
  const img = octx.createImageData(vlen, binTop);
  const dr = S.dynRange;
  for (let x = 0; x < vlen; x++) {
    const f = Math.min(frames - 1, v0 + x);
    for (let y = 0; y < binTop; y++) {
      const b = binTop - 1 - y;  // 低频在下
      const db = S.spec[f * bins + b];
      let t = (db + dr) / dr;
      t = t < 0 ? 0 : t > 1 ? 1 : t;
      const li = (t * 255) | 0, p = (y * vlen + x) * 4;
      img.data[p] = LUT[li * 3]; img.data[p + 1] = LUT[li * 3 + 1]; img.data[p + 2] = LUT[li * 3 + 2]; img.data[p + 3] = 255;
    }
  }
  octx.putImageData(img, 0, 0);
  ctx.imageSmoothingEnabled = true;
  ctx.clearRect(0, 0, w, h);
  ctx.drawImage(off, 0, 0, w, h);
  drawScale(ctx, w, h, hzPerBin, binTop);
  drawPlayCursor(ctx, specCv, w, h);
  drawHover(ctx, specCv, w, h, hzPerBin, binTop);
}

function drawScale(ctx, w, h, hzPerBin, binTop) {
  const dpr = window.devicePixelRatio || 1;
  ctx.save();
  ctx.font = `${10 * dpr}px monospace`;
  ctx.fillStyle = "rgba(255,255,255,0.65)";
  ctx.strokeStyle = "rgba(255,255,255,0.18)";
  ctx.lineWidth = dpr;
  // 频率刻度（每 1kHz）
  const fTop = binTop * hzPerBin;
  for (let f = 1000; f < fTop; f += 1000) {
    const y = h - (f / fTop) * h;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
    ctx.fillText((f / 1000) + "k", 4 * dpr, y - 2 * dpr);
  }
  // 时间刻度
  const secPerFrame = S.hop / S.sampleRate;
  const span = S.viewLen * secPerFrame;
  const step = pickStep(span / 6);
  const t0 = S.viewStart * secPerFrame;
  for (let t = Math.ceil(t0 / step) * step; t < t0 + span; t += step) {
    const x = ((t - t0) / span) * w;
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
    ctx.fillText(t.toFixed(step < 1 ? 2 : 1) + "s", x + 3 * dpr, h - 4 * dpr);
  }
  ctx.restore();
}
function pickStep(target) {
  const steps = [0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 30, 60];
  for (const s of steps) if (s >= target) return s;
  return 60;
}

function drawWave() {
  const { w, h } = fitCanvas(waveCv);
  const ctx = waveCv.getContext("2d");
  ctx.clearRect(0, 0, w, h);
  if (!S.pcm) return;
  const secPerFrame = S.hop / S.sampleRate;
  const t0 = S.viewStart * secPerFrame, span = S.viewLen * secPerFrame;
  const s0 = Math.max(0, Math.floor(t0 * S.sampleRate));
  const s1 = Math.min(S.pcm.length, Math.ceil((t0 + span) * S.sampleRate));
  const mid = h / 2;
  ctx.strokeStyle = "#4da3ff";
  ctx.lineWidth = Math.max(1, (window.devicePixelRatio || 1));
  ctx.beginPath();
  const per = Math.max(1, (s1 - s0) / w);
  for (let x = 0; x < w; x++) {
    const a = Math.floor(s0 + x * per), b = Math.min(s1, Math.floor(a + per) + 1);
    let mn = 1, mx = -1;
    for (let i = a; i < b; i++) { const v = S.pcm[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
    ctx.moveTo(x + 0.5, mid - mx * mid * 0.92);
    ctx.lineTo(x + 0.5, mid - mn * mid * 0.92 + 1);
  }
  ctx.stroke();
  // 0 线
  ctx.strokeStyle = "rgba(255,255,255,0.18)";
  ctx.beginPath(); ctx.moveTo(0, mid); ctx.lineTo(w, mid); ctx.stroke();
  drawPlayCursor(ctx, waveCv, w, h);
}

function xToTime(x, w) {
  const secPerFrame = S.hop / S.sampleRate;
  return (S.viewStart + (x / w) * S.viewLen) * secPerFrame;
}
function timeToX(t, w) {
  const secPerFrame = S.hop / S.sampleRate;
  return ((t / secPerFrame) - S.viewStart) / S.viewLen * w;
}

function drawPlayCursor(ctx, cv, w, h) {
  if (!S.playing && player.paused) return;
  const x = timeToX(player.currentTime, w);
  if (x < 0 || x > w) return;
  ctx.save();
  ctx.strokeStyle = "rgba(255,80,80,0.9)";
  ctx.lineWidth = Math.max(1, window.devicePixelRatio || 1);
  ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
  ctx.restore();
}

function drawHover(ctx, cv, w, h, hzPerBin, binTop) {
  if (!S.hover || S.hover.canvas !== cv || !S.spec) return;
  const { x, y } = S.hover;
  ctx.save();
  ctx.strokeStyle = "rgba(255,255,255,0.35)";
  ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
  ctx.restore();
  // 读数
  const t = xToTime(x, w);
  const fTop = binTop * hzPerBin;
  const freq = (1 - y / h) * fTop;
  const frame = Math.min(S.frames - 1, Math.floor(S.viewStart + (x / w) * S.viewLen));
  const bin = Math.min(S.bins - 1, Math.max(0, Math.floor((1 - y / h) * binTop)));
  const db = S.spec[frame * S.bins + bin];
  $("status").textContent = `t = ${t.toFixed(3)} s   f = ${freq.toFixed(0)} Hz   电平 = ${db.toFixed(1)} dB(相对)`;
}

function drawAll() { drawSpec(); drawWave(); }

// ================= 交互：缩放/平移/悬停 =================
function clampView() {
  S.viewLen = Math.min(S.frames, Math.max(8, S.viewLen));
  S.viewStart = Math.min(S.frames - S.viewLen, Math.max(0, S.viewStart));
}
function bindCanvas(cv) {
  cv.addEventListener("wheel", e => {
    if (!S.spec) return;
    e.preventDefault();
    const r = cv.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const mx = (e.clientX - r.left) * dpr, w = cv.width;
    const anchor = S.viewStart + (mx / w) * S.viewLen;
    const factor = e.deltaY > 0 ? 1.25 : 0.8;
    S.viewLen *= factor;
    S.viewStart = anchor - (mx / w) * S.viewLen;
    clampView();
    drawAll();
  }, { passive: false });
  let drag = null;
  cv.addEventListener("mousedown", e => {
    drag = { x: e.clientX, vs: S.viewStart, moved: false };
  });
  window.addEventListener("mousemove", e => {
    if (drag && S.spec) {
      const dx = e.clientX - drag.x;
      if (Math.abs(dx) > 3) drag.moved = true;
      const dpr = window.devicePixelRatio || 1;
      S.viewStart = drag.vs - dx * dpr / cv.width * S.viewLen;
      clampView();
      drawAll();
    }
    const r = cv.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    if (e.clientX >= r.left && e.clientX <= r.right && e.clientY >= r.top && e.clientY <= r.bottom) {
      S.hover = { canvas: cv, x: (e.clientX - r.left) * dpr, y: (e.clientY - r.top) * dpr };
      drawSpec();
    }
  });
  window.addEventListener("mouseup", e => {
    if (drag && !drag.moved && S.pcm) {
      // 点击定位播放
      const r = cv.getBoundingClientRect();
      const t = xToTime((e.clientX - r.left) * (window.devicePixelRatio || 1), cv.width);
      player.currentTime = Math.min(S.duration, Math.max(0, t));
      drawAll();
    }
    drag = null;
  });
  cv.addEventListener("mouseleave", () => { S.hover = null; $("status").textContent = "—"; drawSpec(); });
}
bindCanvas(specCv);
bindCanvas(waveCv);

$("frange").onchange = e => { S.fmax = parseInt(e.target.value, 10); drawSpec(); };
$("contrast").oninput = e => { S.dynRange = parseInt(e.target.value, 10); drawSpec(); };
$("zoomreset").onclick = () => { S.viewStart = 0; S.viewLen = S.frames; drawAll(); };

// ================= 播放 =================
function stopPlayback() { player.pause(); S.playing = false; $("play").textContent = "播放"; }
$("play").onclick = () => {
  if (!S.pcm) return;
  if (player.paused) { player.play(); } else { stopPlayback(); }
};
player.addEventListener("play", () => { S.playing = true; $("play").textContent = "暂停"; });
player.addEventListener("pause", () => { S.playing = false; $("play").textContent = "播放"; });
player.addEventListener("ended", () => { S.playing = false; $("play").textContent = "播放"; drawAll(); });
(function raf() {
  if (S.playing) {
    // 播放位置跑出视口时跟随
    if (S.spec) {
      const secPerFrame = S.hop / S.sampleRate;
      const f = player.currentTime / secPerFrame;
      if (f < S.viewStart || f > S.viewStart + S.viewLen) {
        S.viewStart = Math.max(0, f - S.viewLen * 0.1);
        clampView();
      }
    }
    drawAll();
  }
  requestAnimationFrame(raf);
})();
window.addEventListener("keydown", e => {
  if (e.code === "Space" && S.pcm && document.activeElement.tagName !== "INPUT") {
    e.preventDefault();
    $("play").click();
  }
});
window.addEventListener("resize", drawAll);

loadSessions(false);
</script>
</body>
</html>
"""


def find_debug_dir(cli_dir: str | None) -> Path:
    """确定调试音频目录：--dir 优先，其次 config.toml 的 debug_audio_dir，兜底 %APPDATA%\\VoiceStick。"""
    if cli_dir:
        return Path(cli_dir)
    appdata = os.environ.get("APPDATA", "")
    cfg = Path(appdata) / "VoiceStick" / "config.toml"
    if cfg.is_file():
        m = re.search(r'debug_audio_dir\s*=\s*"([^"]+)"', cfg.read_text(encoding="utf-8"))
        if m:
            return Path(m.group(1).replace("\\\\", "\\"))
    return Path(appdata) / "VoiceStick"


def make_handler(audio_dir: Path):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, fmt, *args):  # 静音常规访问日志
            pass

        def _send_json(self, obj, status=200):
            body = json.dumps(obj, ensure_ascii=False).encode("utf-8")
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Cache-Control", "no-store")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            from urllib.parse import urlparse, parse_qs
            u = urlparse(self.path)
            if u.path == "/" or u.path == "/index.html":
                body = PAGE.encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Cache-Control", "no-store")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)
            elif u.path == "/api/sessions":
                self._handle_sessions()
            elif u.path == "/api/audio":
                name = parse_qs(u.query).get("name", [""])[0]
                self._handle_audio(name)
            else:
                self.send_error(404)

        def _handle_sessions(self):
            items = []
            try:
                for p in audio_dir.glob("*.ogg"):
                    st = p.stat()
                    m = re.search(r"session-(\d+)", p.name)
                    items.append({
                        "name": p.name,
                        "size": st.st_size,
                        "mtime": __import__("datetime").datetime.fromtimestamp(
                            st.st_mtime).strftime("%m-%d %H:%M:%S"),
                        "session": int(m.group(1)) if m else 0,
                    })
            except OSError as e:
                self._send_json({"error": str(e)}, 500)
                return
            items.sort(key=lambda x: x["name"], reverse=True)
            self._send_json(items)

        def _handle_audio(self, name: str):
            # 防路径穿越：只允许目录内的 .ogg 文件
            if not name or "/" in name or "\\" in name or ".." in name or not name.endswith(".ogg"):
                self.send_error(400)
                return
            path = audio_dir / name
            if not path.is_file():
                self.send_error(404)
                return
            data = path.read_bytes()
            # 支持单段 Range（<audio> 拖动定位会发 Range 请求）
            range_header = self.headers.get("Range")
            if range_header:
                m = re.match(r"bytes=(\d+)-(\d*)", range_header)
                if m:
                    start = int(m.group(1))
                    end = int(m.group(2)) if m.group(2) else len(data) - 1
                    end = min(end, len(data) - 1)
                    if start <= end:
                        chunk = data[start:end + 1]
                        self.send_response(206)
                        self.send_header("Content-Type", "audio/ogg")
                        self.send_header("Accept-Ranges", "bytes")
                        self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
                        self.send_header("Content-Length", str(len(chunk)))
                        self.end_headers()
                        self.wfile.write(chunk)
                        return
            self.send_response(200)
            self.send_header("Content-Type", "audio/ogg")
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    return Handler


def main():
    ap = argparse.ArgumentParser(description="VoiceStick 调试音频频谱分析页")
    ap.add_argument("--port", type=int, default=8765)
    ap.add_argument("--dir", default=None, help="调试音频目录（默认读 config.toml 的 debug_audio_dir）")
    ap.add_argument("--no-browser", action="store_true", help="不自动打开浏览器")
    args = ap.parse_args()

    audio_dir = find_debug_dir(args.dir)
    if not audio_dir.is_dir():
        print(f"错误：目录不存在 {audio_dir}")
        return 1

    server = http.server.ThreadingHTTPServer(("127.0.0.1", args.port), make_handler(audio_dir))
    url = f"http://127.0.0.1:{args.port}/"
    print(f"音频目录: {audio_dir}")
    print(f"频谱调试页: {url}  (Ctrl+C 停止)")
    if not args.no_browser:
        threading.Timer(0.5, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
