"""VoiceStick 音频实验台：本地小服务 + 浏览器可视化音频质量评估与双音源 A/B 对比。

用法：
    python scripts/e2e_test/audio_lab.py [--port 8790] [--dir PATH] [--no-browser]

- 自动读取 config.toml 的 debug_audio_dir（Windows %APPDATA%\\VoiceStick，macOS
  ~/Library/Application Support/VoiceStick；--dir 可覆盖），与 spectrogram_server.py
  同一目录约定；也直接吃 atvv_capture.py 的 .wav 产物。
- 浏览器打开 http://127.0.0.1:8790 ：
  * 左侧会话列表（含侧车来源徽标 VS-/VG-/RC-、seq 丢帧、现场识别文本）；
    每行 A/B 两个选键，A+B 都选即进入对比模式。
  * 单会话：频谱图 + 波形 + 三层指标红绿灯（传输/信号/后处理判据）。
  * A/B 对比：双轨共享时间轴（包络互相关自动对齐 + ±10ms 手动微调）、指标
    对照表（Δ 列）、三种试听：只听 A / 只听 B / 左右分耳同步（左耳=A 右耳=B），
    空格键即时切换，等响度（衰减对齐）与循环播放开关。
- 评价标准（阈值出处）见 Doc/Ref/audio-quality-metrics.md；红绿灯阈值锚定
  2026-10-16 网关音频取证与 RNNoise 判据的实测数据。

纯标准库实现；音频解码/STFT/指标全部在浏览器内完成（Web Audio + JS FFT），
推荐 Chrome/Edge（Safari 对 ogg/opus 解码支持不稳）。单文件可分发。
"""

from __future__ import annotations

import argparse
import datetime
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
<title>VoiceStick 音频实验台</title>
<style>
  :root { --bg:#14161a; --panel:#1d2026; --fg:#cfd3da; --dim:#7d838d; --accent:#4da3ff;
          --good:#3fb950; --warn:#d29922; --bad:#f85149; }
  * { box-sizing: border-box; }
  body { margin:0; background:var(--bg); color:var(--fg); font:13px/1.4 "Segoe UI","PingFang SC",sans-serif; height:100vh; display:flex; flex-direction:column; }
  #app { display:flex; flex:1; min-height:0; }
  aside { width:300px; min-width:300px; background:var(--panel); border-right:1px solid #2b2f36; display:flex; flex-direction:column; }
  aside header { padding:10px 12px; display:flex; justify-content:space-between; align-items:center; border-bottom:1px solid #2b2f36; }
  aside header b { font-size:14px; }
  #refresh { background:#2b3038; color:var(--fg); border:1px solid #3a404a; border-radius:4px; padding:3px 10px; cursor:pointer; }
  #refresh:hover { border-color:var(--accent); }
  #sessions { list-style:none; margin:0; padding:0; overflow-y:auto; flex:1; }
  #sessions li { padding:6px 10px; border-bottom:1px solid #23272e; display:flex; gap:8px; align-items:center; }
  #sessions li:hover { background:#262b33; }
  #sessions li .info { flex:1; min-width:0; cursor:pointer; }
  #sessions .name { display:block; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; font-size:12px; }
  #sessions .meta { color:var(--dim); font-size:11px; display:flex; gap:8px; flex-wrap:wrap; }
  #sessions .text { color:#9aa3ad; font-size:11px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  .pick { width:24px; height:24px; border-radius:4px; border:1px solid #3a404a; background:#2b3038;
          color:var(--fg); cursor:pointer; font-weight:700; flex:none; }
  .pick.onA { background:#1f4d7a; border-color:var(--accent); }
  .pick.onB { background:#5a3a7a; border-color:#b085f5; }
  .badge { display:inline-block; padding:0 5px; border-radius:3px; font-size:10px; line-height:16px; }
  .badge.VS { background:#1e3a5f; color:#8ec2ff; }
  .badge.VG { background:#4a3a1e; color:#ffce7a; }
  .badge.RC { background:#3a1e2f; color:#ff9ad5; }
  .badge.raw { background:#333; color:#aaa; }
  .badge.bad { background:#4a1e1e; color:#ff9b9b; }
  main { flex:1; display:flex; flex-direction:column; min-width:0; }
  #toolbar { padding:7px 12px; background:var(--panel); border-bottom:1px solid #2b2f36; display:flex; gap:12px; align-items:center; flex-wrap:wrap; }
  #toolbar .group { display:flex; gap:6px; align-items:center; }
  #toolbar label { color:var(--dim); font-size:12px; }
  #toolbar button, #toolbar select { background:#2b3038; color:var(--fg); border:1px solid #3a404a; border-radius:4px; padding:3px 9px; cursor:pointer; }
  #toolbar button:hover { border-color:var(--accent); }
  #toolbar button.activeA { background:#1f4d7a; border-color:var(--accent); }
  #toolbar button.activeB { background:#5a3a7a; border-color:#b085f5; }
  #toolbar button.sel { background:#1f4d7a; border-color:var(--accent); }
  #play { min-width:60px; font-weight:700; }
  .fname { font-weight:600; max-width:340px; white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  #content { flex:1; display:flex; flex-direction:column; min-height:0; }
  .track { display:flex; flex-direction:column; min-height:0; }
  .trackA { flex:6; }
  .trackB { flex:6; border-top:2px solid #2b2f36; display:none; }
  .track .thead { display:flex; gap:10px; padding:3px 12px; font-size:12px; align-items:center; }
  .track .thead .tagA { color:var(--accent); font-weight:700; }
  .track .thead .tagB { color:#b085f5; font-weight:700; }
  .track .thead .tname { color:var(--dim); white-space:nowrap; overflow:hidden; text-overflow:ellipsis; }
  .tcanvas { display:flex; flex-direction:column; flex:1; min-height:0; }
  .tcanvas canvas.wave { flex:1; min-height:26px; cursor:crosshair; border-top:1px solid #23272e; }
  .tcanvas canvas.spec { flex:4; min-height:0; cursor:crosshair; }
  #metrics { max-height:34vh; overflow-y:auto; background:var(--panel); border-top:2px solid #2b2f36; padding:6px 12px 10px; }
  #metrics table { border-collapse:collapse; width:100%; }
  #metrics th, #metrics td { text-align:left; padding:2px 10px 2px 0; font-size:12px; white-space:nowrap; }
  #metrics th.sec { color:var(--dim); font-weight:600; padding-top:6px; }
  #metrics td.k { color:var(--dim); }
  #metrics td.v { font-variant-numeric:tabular-nums; }
  #metrics td.d { font-variant-numeric:tabular-nums; color:#9aa3ad; }
  .chip { display:inline-block; min-width:26px; text-align:center; border-radius:3px; padding:0 6px; }
  .chip.good { background:rgba(63,185,80,0.16); color:var(--good); }
  .chip.warn { background:rgba(210,153,34,0.16); color:var(--warn); }
  .chip.bad  { background:rgba(248,81,73,0.14);  color:var(--bad); }
  .chip.info { color:var(--fg); }
  #status { padding:4px 12px; background:var(--panel); border-top:1px solid #2b2f36; color:var(--dim); font-variant-numeric:tabular-nums; font-size:12px; }
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
      <span class="fname" id="fname">（左列表点行=单轨；A/B 键选两轨对比）</span>
      <div class="group">
        <button id="play">播放</button>
        <label><input type="checkbox" id="loop"> 循环</label>
      </div>
      <div class="group" id="listenGroup" style="display:none">
        <label>试听</label>
        <button id="listenA">只听 A</button>
        <button id="listenB">只听 B</button>
        <button id="listenAB">左右分耳</button>
        <label><input type="checkbox" id="match"> 等响度</label>
      </div>
      <div class="group" id="alignGroup" style="display:none">
        <label>对齐</label>
        <button id="alignAuto">自动</button>
        <button id="nudgeL">−10ms</button>
        <button id="nudgeR">+10ms</button>
        <span id="alignVal" class="v" style="font-variant-numeric:tabular-nums">0ms</span>
        <button id="alignZero">归零</button>
      </div>
      <div class="group">
        <label>频宽</label>
        <select id="frange">
          <option value="2500">0–2.5k</option>
          <option value="8000" selected>0–8k</option>
          <option value="0">全频</option>
        </select>
        <label>对比度</label>
        <input type="range" id="contrast" min="40" max="120" value="90" style="vertical-align:middle">
        <button id="zoomreset">重置缩放</button>
      </div>
    </div>
    <div id="err"></div>
    <div id="content">
      <div class="track trackA" id="boxA">
        <div class="thead"><span class="tagA">A</span><span class="tname" id="nameA">—</span></div>
        <div class="tcanvas">
          <canvas class="spec" id="specA"></canvas>
          <canvas class="wave" id="waveA"></canvas>
        </div>
      </div>
      <div class="track trackB" id="boxB">
        <div class="thead"><span class="tagB">B</span><span class="tname" id="nameB">—</span></div>
        <div class="tcanvas">
          <canvas class="spec" id="specB"></canvas>
          <canvas class="wave" id="waveB"></canvas>
        </div>
      </div>
      <div id="metrics"><span style="color:var(--dim)">选择会话后此处显示三层指标红绿灯。</span></div>
    </div>
    <div id="status">—</div>
  </main>
</div>
<script>
"use strict";
// ================= 全局状态 =================
const S = {
  tracks: { A: null, B: null },   // Track：{name,pcm,sampleRate,duration,spec,frames,bins,hop,win,sidecar,raw,buffer,metrics}
  view: { start: 0, span: 5 },    // 共享时间轴视口（秒；B 轨内容平移 offsetB）
  offsetB: 0,                     // B 相对 A 的起始偏移（秒）
  listen: "AB",                   // 'A' | 'B' | 'AB'（左右分耳）
  matchLevel: false,              // 等响度：把较响一路衰减到另一路（不放大防削波）
  loop: false,
  fmax: 8000, dynRange: 90,
  hover: null, sessions: [],
};
const $ = id => document.getElementById(id);
const canvases = { A: { spec: $("specA"), wave: $("waveA") }, B: { spec: $("specB"), wave: $("waveB") } };
const HOP = 512, WIN = 2048;

function totalDur() {
  let d = S.tracks.A ? S.tracks.A.duration : 0;
  if (S.tracks.B) d = Math.max(d, S.offsetB + S.tracks.B.duration);
  return d || 5;
}
// 轨道时间 <-> 共享时间轴（B 轨加 offsetB）
function axisToTrack(sec, key) { return key === "B" ? sec - S.offsetB : sec; }
function trackToAxis(sec, key) { return key === "B" ? sec + S.offsetB : sec; }

// ================= FFT（radix-2，与 spectrogram_server 同款） =================
function makeFFT(n) {
  const rev = new Uint32Array(n);
  for (let i = 0; i < n; i++) rev[i] = (rev[i >> 1] >> 1) | ((i & 1) ? n >> 1 : 0);
  const cosT = new Float32Array(n / 2), sinT = new Float32Array(n / 2);
  for (let i = 0; i < n / 2; i++) {
    cosT[i] = Math.cos(-2 * Math.PI * i / n); sinT[i] = Math.sin(-2 * Math.PI * i / n);
  }
  return { n, rev, cosT, sinT };
}
function fftRadix2(tbl, re, im) {
  const n = tbl.n, rev = tbl.rev, cosT = tbl.cosT, sinT = tbl.sinT;
  for (let i = 0; i < n; i++) if (i < rev[i]) {
    const tr = re[i], ti = im[i]; re[i] = re[rev[i]]; im[i] = im[rev[i]]; re[rev[i]] = tr; im[rev[i]] = ti;
  }
  for (let size = 2; size <= n; size <<= 1) {
    const half = size >> 1, step = n / size;
    for (let i = 0; i < n; i += size) for (let j = i, k = 0; j < i + half; j++, k += step) {
      const l = j + half;
      const xr = re[l] * cosT[k] - im[l] * sinT[k], xi = re[l] * sinT[k] + re[l] * cosT[k];
      re[l] = re[j] - xr; im[l] = im[j] - xi; re[j] += xr; im[j] += xi;
    }
  }
}

// ================= magma 色图 =================
const MAGMA_STOPS = [
  [0.00, [0, 0, 4]], [0.15, [28, 16, 68]], [0.35, [79, 18, 123]],
  [0.55, [182, 54, 121]], [0.75, [251, 137, 97]], [0.90, [254, 194, 135]],
  [1.00, [252, 253, 245]],
];
const LUT = new Uint8Array(256 * 3);
for (let i = 0; i < 256; i++) {
  const t = i / 255;
  let a = MAGMA_STOPS[0], b = MAGMA_STOPS[MAGMA_STOPS.length - 1];
  for (let s = 0; s < MAGMA_STOPS.length - 1; s++)
    if (t >= MAGMA_STOPS[s][0] && t <= MAGMA_STOPS[s + 1][0]) { a = MAGMA_STOPS[s]; b = MAGMA_STOPS[s + 1]; break; }
  const f = (t - a[0]) / Math.max(1e-9, b[0] - a[0]);
  for (let c = 0; c < 3; c++) LUT[i * 3 + c] = Math.round(a[1][c] + (b[1][c] - a[1][c]) * f);
}

// ================= 会话列表 =================
function srcBadge(it) {
  const sc = it.sidecar;
  let tag = null;
  if (sc && sc.source) tag = { stickMic: "VS", gatewayRelay: "VG", remoteDirect: "RC" }[sc.source];
  if (!tag) { // 无侧车按文件名前缀猜（历史/Windows 文件）
    const m = it.name.match(/-(VS|VG|RC)-/); tag = m ? m[1] : null;
  }
  return tag;
}
function srcName(it) {
  const map = { VS: "Stick 麦", VG: "网关中继", RC: "遥控器直连" };
  const t = srcBadge(it); return t ? map[t] : "未知";
}
async function loadSessions() {
  const res = await fetch("/api/sessions", { cache: "no-store" });
  S.sessions = await res.json();
  const ul = $("sessions");
  ul.innerHTML = "";
  for (const it of S.sessions) {
    const li = document.createElement("li");
    const kb = (it.size / 1024).toFixed(0);
    const tag = srcBadge(it);
    let badges = tag ? `<span class="badge ${tag}">${tag}</span>` : "";
    if (it.raw) badges += ` <span class="badge raw">增强前</span>`;
    if (it.sidecar && it.sidecar.droppedFrames > 0)
      badges += ` <span class="badge bad">丢帧 ${it.sidecar.droppedFrames}</span>`;
    const text = it.sidecar && it.sidecar.finalText ? `<span class="text">“${it.sidecar.finalText}”</span>` : "";
    li.innerHTML = `<div class="info">
        <span class="name">${it.name}</span>
        <span class="meta">${it.mtime} · ${kb}KB · ${srcName(it)}${badges ? " · " + badges : ""}</span>
        ${text}</div>
      <button class="pick pickA" title="设为 A 轨">A</button>
      <button class="pick pickB" title="设为 B 轨（进入对比）">B</button>`;
    li.querySelector(".info").onclick = () => setTrack("A", it.name, true);
    li.querySelector(".pickA").onclick = () => setTrack("A", it.name, false);
    li.querySelector(".pickB").onclick = () => setTrack("B", it.name, false);
    ul.appendChild(li);
  }
  refreshPickHighlight();
}
function refreshPickHighlight() {
  document.querySelectorAll("#sessions li").forEach(li => {
    const nameA = S.tracks.A && S.tracks.A.name, nameB = S.tracks.B && S.tracks.B.name;
    li.querySelector(".pickA").classList.toggle("onA", li.querySelector(".name").textContent === nameA);
    li.querySelector(".pickB").classList.toggle("onB", li.querySelector(".name").textContent === nameB);
  });
}
$("refresh").onclick = loadSessions;

// ================= 加载 + 分析 =================
let audioCtx = null;
function ensureCtx() {
  if (!audioCtx) audioCtx = new (window.AudioContext || window.webkitAudioContext)();
  if (audioCtx.state === "suspended") audioCtx.resume();
  return audioCtx;
}
async function setTrack(key, name, solo) {
  $("err").style.display = "none";
  if (solo) { // 单轨模式：清 B
    S.tracks.B = null; S.offsetB = 0;
    if (S.tracks.A && S.tracks.A.name === name) { updateLayout(); return; }
  }
  const it = S.sessions.find(x => x.name === name);
  try {
    $("status").textContent = "加载 " + name + " …";
    const buf = await (await fetch("/api/audio?name=" + encodeURIComponent(name))).arrayBuffer();
    ensureCtx();
    const audio = await audioCtx.decodeAudioData(buf);
    const track = {
      name, raw: !!(it && it.raw), sidecar: it && it.sidecar,
      buffer: audio, pcm: audio.getChannelData(0),
      sampleRate: audio.sampleRate, duration: audio.duration,
    };
    S.tracks[key] = track;
    Transport.stop();
    await analyze(track);
    // B 轨载入即自动对齐一次（可手动微调/归零）
    if (key === "B" && S.tracks.A) autoAlign();
    S.view.start = 0; S.view.span = totalDur();
    updateLayout(); drawAll(); renderMetrics();
    $("status").textContent = "—";
  } catch (e) {
    $("err").textContent = "加载失败: " + e.message + "（Safari 对 ogg/opus 解码不稳，建议 Chrome/Edge）";
    $("err").style.display = "block";
  }
  refreshPickHighlight();
}

// 指标分析：电平/噪声/VAD/频段占比/F0 + 频谱图（一次 STFT）
async function analyze(track) {
  const pcm = track.pcm, sr = track.sampleRate, n = pcm.length;
  // ---- 电平 ----
  let peak = 0, sumSq = 0, clip = 0, dc = 0;
  for (let i = 0; i < n; i++) {
    const v = pcm[i], a = v < 0 ? -v : v;
    if (a > peak) peak = a;
    if (a > 0.98) clip++;
    sumSq += v * v; dc += v;
  }
  const rmsDb = db10(sumSq / n), peakDb = db20(peak);
  // ---- VAD（20ms 帧 RMS）----
  const fr = Math.floor(sr * 0.02), nf = Math.max(1, Math.floor(n / fr));
  const frameRms = new Float32Array(nf);
  for (let f = 0; f < nf; f++) {
    let s = 0, c = 0;
    for (let i = f * fr; i < Math.min(n, (f + 1) * fr); i++) { s += pcm[i] * pcm[i]; c++; }
    frameRms[f] = Math.sqrt(s / Math.max(1, c));
  }
  const sorted = Float32Array.from(frameRms).sort();
  // 底噪 = 最低 15% 分位（最低 30% 区间的下中位，稳健）
  const q = Math.max(1, Math.floor(nf * 0.15));
  const noiseFloor = sorted[q];
  // 语音门限 = max(底噪+6dB, 绝对 -55dBFS)
  const thresh = Math.max(noiseFloor * 2, Math.pow(10, -55 / 20));
  let speechFrames = 0;
  const speechMask = new Uint8Array(nf);
  for (let f = 0; f < nf; f++) if (frameRms[f] > thresh) { speechMask[f] = 1; speechFrames++; }
  const speechRatio = speechFrames / nf;
  // 语音段 RMS：仅累计语音帧内样本（三遍：帧 RMS → 门限 → 样本累计）
  let speechSq = 0, speechN = 0;
  for (let f = 0; f < nf; f++) {
    if (!speechMask[f]) continue;
    for (let i = f * fr; i < Math.min(n, (f + 1) * fr); i++) { const v = pcm[i]; speechSq += v * v; speechN++; }
  }
  const speechRmsDb = speechN > 0 ? db10(speechSq / speechN) : rmsDb;
  // ---- STFT + 频段能量（语音帧）----
  const tbl = makeFFT(WIN), hann = new Float32Array(WIN);
  for (let i = 0; i < WIN; i++) hann[i] = 0.5 * (1 - Math.cos(2 * Math.PI * i / WIN));
  const frames = Math.max(1, Math.floor((n - WIN) / HOP) + 1), bins = WIN / 2;
  const spec = new Float32Array(frames * bins);
  const hzPerBin = sr / 2 / bins;
  const bands = [[0, 300], [300, 2000], [2000, 4000], [4000, 6500], [6500, 100000]];
  const bandE = new Float64Array(bands.length);
  let maxDb = -Infinity, specSpeechFrames = 0;
  const re = new Float32Array(WIN), im = new Float32Array(WIN);
  for (let f = 0; f < frames; f++) {
    const off = f * HOP;
    for (let i = 0; i < WIN; i++) { re[i] = (off + i < n ? pcm[off + i] : 0) * hann[i]; im[i] = 0; }
    fftRadix2(tbl, re, im);
    const isSpeech = speechMask[Math.min(nf - 1, Math.floor((off + WIN / 2) / fr))] === 1;
    for (let b = 0; b < bins; b++) {
      const mag = Math.sqrt(re[b] * re[b] + im[b] * im[b]) / WIN;
      const dbv = 20 * Math.log10(mag + 1e-9);
      spec[f * bins + b] = dbv;
      if (dbv > maxDb) maxDb = dbv;
      if (isSpeech) {
        const hz = b * hzPerBin, pw = mag * mag;
        for (let k = 0; k < bands.length; k++)
          if (hz >= bands[k][0] && hz < bands[k][1]) { bandE[k] += pw; break; }
      }
    }
    if (isSpeech) specSpeechFrames++;
    if (f % 120 === 119) { $("status").textContent = `分析中… 帧 ${f + 1}/${frames}`; await new Promise(r => setTimeout(r)); }
  }
  for (let i = 0; i < spec.length; i++) spec[i] -= maxDb;
  const bandTotal = bandE.reduce((a, b) => a + b, 0) || 1;
  const bandPct = Array.from(bandE, e => 100 * e / bandTotal);
  // ---- F0（语音帧自相关，70–400Hz）----
  const lagMin = Math.floor(sr / 400), lagMax = Math.min(Math.floor(sr / 70), 400);
  const f0s = [];
  const step = Math.max(1, Math.floor(nf / 300));
  for (let f = 0; f < nf && f0s.length < 300; f += step) {
    if (!speechMask[f]) continue;
    const off = f * fr, len = Math.min(fr * 3, n - off);
    if (len < lagMax + 8) continue;
    let e0 = 0;
    for (let i = 0; i < len; i++) e0 += pcm[off + i] * pcm[off + i];
    if (e0 < 1e-7) continue;
    let bestLag = 0, bestR = 0;
    for (let lag = lagMin; lag <= lagMax; lag++) {
      let s = 0, e1 = 0;
      for (let i = 0; i + lag < len; i++) { const v = pcm[off + i]; s += v * pcm[off + i + lag]; e1 += pcm[off + i + lag] * pcm[off + i + lag]; }
      const r = s / Math.sqrt(e0 * e1 + 1e-12);
      if (r > bestR) { bestR = r; bestLag = lag; }
    }
    if (bestR > 0.35 && bestLag > 0) f0s.push(sr / bestLag);
  }
  f0s.sort((a, b) => a - b);
  const f0Med = f0s.length ? f0s[Math.floor(f0s.length / 2)] : null;
  track.spec = spec; track.frames = frames; track.bins = bins; track.hop = HOP; track.win = WIN;
  track.metrics = {
    peakDb, rmsDb, clipPct: 100 * clip / n, dc,
    noiseDb: db20(noiseFloor), speechRmsDb, snrDb: speechRmsDb - db20(noiseFloor),
    speechSec: speechFrames * 0.02, speechRatio,
    bandPct, f0Med, f0N: f0s.length, specSpeechFrames,
  };
}
const db20 = v => 20 * Math.log10(v + 1e-12);
const db10 = v => 10 * Math.log10(v + 1e-12);

// ---- 包络互相关自动对齐（10ms 包络，±2s 搜索） ----
function envelope(track) {
  const pcm = track.pcm, sr = track.sampleRate, hopS = Math.floor(sr * 0.01);
  const m = Math.max(1, Math.floor(pcm.length / hopS)), env = new Float32Array(m);
  for (let i = 0; i < m; i++) {
    let mx = 0;
    for (let j = i * hopS; j < Math.min(pcm.length, (i + 1) * hopS); j++) {
      const a = Math.abs(pcm[j]); if (a > mx) mx = a;
    }
    env[i] = mx;
  }
  return env;
}
function autoAlign() {
  const A = S.tracks.A, B = S.tracks.B;
  if (!A || !B) return;
  const ea = envelope(A), eb = envelope(B);
  const maxLag = 200; // ±2s @100Hz
  let bestLag = 0, bestC = -1;
  for (let lag = -maxLag; lag <= maxLag; lag++) {
    let dot = 0, ea2 = 0, eb2 = 0, cnt = 0;
    for (let i = 0; i < ea.length; i++) {
      const j = i + lag;
      if (j < 0 || j >= eb.length) continue;
      dot += ea[i] * eb[j]; ea2 += ea[i] * ea[i]; eb2 += eb[j] * eb[j]; cnt++;
    }
    if (cnt < 30) continue;
    const c = dot / Math.sqrt(ea2 * eb2 + 1e-12);
    if (c > bestC) { bestC = c; bestLag = lag; }
  }
  // lag>0：B 晚于 A → offsetB 为正
  S.offsetB = bestLag * 0.01;
  updateAlignUI();
}
function updateAlignUI() {
  $("alignVal").textContent = Math.round(S.offsetB * 1000) + "ms";
  drawAll();
}
$("alignAuto").onclick = () => { autoAlign(); S.view.start = 0; S.view.span = totalDur(); drawAll(); };
$("nudgeL").onclick = () => { S.offsetB -= 0.01; updateAlignUI(); };
$("nudgeR").onclick = () => { S.offsetB += 0.01; updateAlignUI(); };
$("alignZero").onclick = () => { S.offsetB = 0; updateAlignUI(); };

// ================= 红绿灯阈值（锚定 Doc/Ref/audio-quality-metrics.md） =================
const G = "good", W = "warn", R = "bad";
const LIGHTS = {
  // 传输层（侧车；无侧车显示 —）
  coverage: v => v >= 0.95 ? G : v >= 0.8 ? W : R,          // 帧覆盖率（0.5~0.8=尾帧丢弃指纹）
  dropped: v => v === 0 ? G : v <= 2 ? W : R,               // seq 缺口实测
  spanRatio: v => (v >= 0.9 && v <= 1.6) ? G : W,           // span/录音（积压拖尾）
  // 电平
  peak: v => v > -1.0 ? R : v > -3.0 ? W : G,
  clip: v => v > 0.05 ? R : v > 0 ? W : G,                  // %
  speechRms: v => (v >= -32 && v <= -8) ? G : W,
  // 噪声
  snr: v => v >= 20 ? G : v >= 10 ? W : R,
  // 频段占比（2026-10-16 取证健康区间）
  low300: v => v <= 34 ? G : v <= 45 ? W : R,               // <300Hz（轰鸣/直流指纹）
  high4k: v => v >= 15 ? G : v >= 8 ? W : R,                // 4–6.5k（发闷指纹）
  // A/B Δ（后处理判据）
  dRms: v => Math.abs(v) <= 0.5 ? G : W,                    // 语音电平保持（RNNoise ≤0.3dB 判据放宽到 0.5）
  dNoise: v => v < -0.5 ? G : v <= 0.5 ? W : R,             // 底噪应下降
  f0ratio: v => (v >= 0.97 && v <= 1.05) ? G : W,           // 时间基漂移指纹
};
function chip(kind, text, title) {
  return `<span class="chip ${kind}" ${title ? `title="${title}"` : ""}>${text}</span>`;
}
function metricCell(kind, value, fmt) {
  if (value === null || value === undefined || Number.isNaN(value)) return `<td class="v">—</td>`;
  const light = LIGHTS[kind] ? LIGHTS[kind](value) : "info";
  return `<td class="v">${chip(light, fmt ? fmt(value) : value.toFixed(1))}</td>`;
}
function fmtPct(v) { return v.toFixed(1) + "%"; }
function fmtDb(v) { return v.toFixed(1) + " dB"; }

function renderMetrics() {
  const A = S.tracks.A, B = S.tracks.B;
  const el = $("metrics");
  if (!A) { el.innerHTML = `<span style="color:var(--dim)">选择会话后此处显示三层指标红绿灯。</span>`; return; }
  const cmp = !!B;
  const rows = [];
  const sec = t => `<tr><td class="k" colspan="${cmp ? 4 : 2}"><span class="chip info">${t}</span></td></tr>`;
  rows.push(`<tr><th class="sec"></th>${cmp ? '<th class="sec">A</th><th class="sec">B</th><th class="sec">Δ / 比值</th>' : '<th class="sec">值</th>'}</tr>`);

  // —— 传输完整性（侧车） ——
  rows.push(sec("传输完整性（会话侧车）"));
  const scOf = t => t.sidecar;
  const transRow = (label, kind, val, fmt) => {
    if (!cmp) {
      const sc = scOf(A);
      let cell;
      if (!sc) cell = `<td class="v">—（无侧车）</td>`;
      else {
        const v = val(sc);
        cell = v === null || v === undefined ? `<td class="v">—</td>` :
          `<td class="v">${chip(LIGHTS[kind](v), fmt(v))}</td>`;
      }
      return `<tr><td class="k">${label}</td>${cell}</tr>`;
    }
    const cell = t => {
      const sc = t.sidecar;
      if (!sc) return `<td class="v">—</td>`;
      const v = val(sc);
      if (v === null || v === undefined) return `<td class="v">—</td>`;
      return `<td class="v">${chip(LIGHTS[kind](v), fmt(v))}</td>`;
    };
    return `<tr><td class="k">${label}</td>${cell(A)}${cell(B)}<td class="d"></td></tr>`;
  };
  rows.push(transRow("帧覆盖率（frames×40ms÷时长）", "coverage",
    sc => sc.frames && sc.recordingSeconds ? (sc.frames * 0.04 / sc.recordingSeconds) : null, v => v.toFixed(2)));
  rows.push(transRow("seq 缺口丢帧（实测）", "dropped", sc => sc.droppedFrames, v => String(v)));
  rows.push(transRow("到达跨度 span/录音", "spanRatio",
    sc => sc.frames && sc.recordingSeconds ? sc.streamSpanSeconds / sc.recordingSeconds : null, v => v.toFixed(2)));

  // —— 信号质量 ——
  rows.push(sec("信号质量（浏览器计算）"));
  const sigRow = (label, kind, get, fmt) => {
    fmt = fmt || (v => v.toFixed(1));
    if (!cmp) return `<tr><td class="k">${label}</td>${metricCell(kind, get(A.metrics), fmt)}</tr>`;
    const a = get(A.metrics), b = get(B.metrics);
    return `<tr><td class="k">${label}</td>${metricCell(kind, a, fmt)}${metricCell(kind, b, fmt)}<td class="d"></td></tr>`;
  };
  rows.push(sigRow("峰值电平", "peak", m => m.peakDb, fmtDb));
  rows.push(sigRow("语音段 RMS", "speechRms", m => m.speechRmsDb, fmtDb));
  rows.push(sigRow("削波样本占比", "clip", m => m.clipPct, fmtPct));
  rows.push(sigRow("底噪（最低15%帧中位）", null, m => m.noiseDb, fmtDb));
  rows.push(sigRow("SNR 估计（语音-底噪）", "snr", m => m.snrDb, fmtDb));
  rows.push(sigRow("<300Hz 能量占比", "low300", m => m.bandPct[0], fmtPct));
  rows.push(sigRow("300–2k 占比", null, m => m.bandPct[1], fmtPct));
  rows.push(sigRow("2–4k 占比", null, m => m.bandPct[2], fmtPct));
  rows.push(sigRow("4–6.5k 占比（发闷指纹）", "high4k", m => m.bandPct[3], fmtPct));
  rows.push(sigRow(">6.5k 占比", null, m => m.bandPct[4], fmtPct));
  rows.push(sigRow("基频 F0 中位", null, m => m.f0Med, v => v.toFixed(1) + " Hz"));
  rows.push(sigRow("语音时长（VAD）", null, m => m.speechSec, v => v.toFixed(2) + " s"));

  // —— A/B 对照（后处理/双源判据） ——
  if (cmp) {
    rows.push(sec("A/B 对照判据"));
    const dRow = (label, kind, get, fmt, note) => {
      const a = get(A.metrics), b = get(B.metrics);
      if (a === null || b === null) return `<tr><td class="k">${label}</td><td class="v">—</td><td class="v">—</td><td class="d"></td></tr>`;
      const d = fmt(a, b);
      return `<tr><td class="k">${label}${note ? ` <span style="color:#565c66">${note}</span>` : ""}</td>
        <td class="v">${a.toFixed(1)}</td><td class="v">${b.toFixed(1)}</td>
        <td class="d">${chip(LIGHTS[kind](d), (d >= 0 ? "+" : "") + d.toFixed(2))}</td></tr>`;
    };
    rows.push(dRow("Δ 语音 RMS (dB)", "dRms", m => m.speechRmsDb, (a, b) => b - a, "RNNoise: |Δ|≤0.3"));
    rows.push(dRow("Δ 底噪 (dB)", "dNoise", m => m.noiseDb, (a, b) => b - a, "应为负（降噪）"));
    rows.push(dRow("Δ 4–6.5k (pt)", "dRms", m => m.bandPct[3], (a, b) => b - a, "音色保持"));
    const f0row = (() => {
      const fa = A.metrics.f0Med, fb = B.metrics.f0Med;
      if (!fa || !fb) return `<tr><td class="k">F0 比值（时间基）</td><td class="v">—</td><td class="v">—</td><td class="d"></td></tr>`;
      const r = fb / fa;
      return `<tr><td class="k">F0 比值（时间基指纹）</td><td class="v">${fa.toFixed(1)}</td><td class="v">${fb.toFixed(1)}</td>
        <td class="d">${chip(LIGHTS.f0ratio(r), r.toFixed(3))}</td></tr>`;
    })();
    rows.push(f0row);
  }

  // —— 现场识别 ——
  rows.push(sec("现场识别（ASR final，来自侧车）"));
  const txt = t => t.sidecar && t.sidecar.finalText
    ? `<td class="v" style="white-space:normal">${escapeHtml(t.sidecar.finalText)}</td>`
    : `<td class="v" style="color:#565c66">—（无文本/无侧车）</td>`;
  rows.push(cmp ? `<tr><td class="k">识别文本</td>${txt(A)}${txt(B)}<td class="d"></td></tr>`
                : `<tr><td class="k">识别文本</td>${txt(A)}</tr>`);
  rows.push(`<tr><td class="k">备注</td><td class="d" colspan="${cmp ? 3 : 1}" style="white-space:normal">
    阈值出处 Doc/Ref/audio-quality-metrics.md（2026-10-16 网关取证 + RNNoise 判据）。
    顺序复述配对含说话本身差异；<300Hz 或 4–6.5k 亮黄/红先看放置与环境，再查链路。</td></tr>`);
  el.innerHTML = `<table>${rows.join("")}</table>`;
}
function escapeHtml(s) {
  return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
}

// ================= 播放传输（Web Audio） =================
const Transport = {
  playing: false, startPos: 0, ctxStart: 0, nodes: [],
  get pos() {
    return this.playing ? this.startPos + (audioCtx.currentTime - this.ctxStart) : this.startPos;
  },
  duration() { return totalDur(); },
  play(from) {
    const A = S.tracks.A, B = S.tracks.B;
    if (!A) return;
    this.stop();
    const ctx = ensureCtx();
    const now = ctx.currentTime + 0.06;
    const master = ctx.createGain(); master.gain.value = 0.95; master.connect(ctx.destination);
    const parts = [];
    const mkSrc = (track, axisStart) => {
      // 本轨在共享时间轴上的起点 = axisStart；本次播放从 from 开始
      const into = from - axisStart;
      if (into >= track.duration) return null;   // 已放完
      const src = ctx.createBufferSource(); src.buffer = track.buffer;
      const gain = ctx.createGain();
      const pan = ctx.createStereoPanner ? ctx.createStereoPanner() : null;
      src.connect(gain);
      if (pan) { gain.connect(pan); pan.connect(master); } else gain.connect(master);
      const when = into < 0 ? now - into : now;   // into<0：延迟启动
      const offset = Math.max(0, into);
      src.start(when, offset);
      return { src, gain, pan, track };
    };
    const solo = !B;
    if (A) { const p = mkSrc(A, 0); if (p) parts.push(p); }
    if (B) { const p = mkSrc(B, S.offsetB); if (p) parts.push(p); }
    if (!parts.length) return;
    this.nodes = parts; this.masterNode = master;
    this.startPos = from; this.ctxStart = now; this.playing = true;
    this.applyListen(solo);
    $("play").textContent = "暂停";
  },
  applyListen(solo) {
    const A = S.tracks.A, B = S.tracks.B;
    solo = solo || !B;
    // 等响度：把较响一路衰减到另一路的语音电平（只衰减不放大，防削波）
    let gA = 1, gB = 1;
    if (!solo && S.matchLevel && A && B && A.metrics && B.metrics) {
      const d = A.metrics.speechRmsDb - B.metrics.speechRmsDb; // >0: A 更响
      const lin = Math.pow(10, -Math.abs(d) / 20);
      if (d > 0) gA = lin; else gB = lin;
    }
    for (const p of this.nodes) {
      if (!p.gain) continue;
      const isA = p.track === A;
      let g;
      if (solo) g = 1;
      else if (S.listen === "A") g = isA ? gA : 0;
      else if (S.listen === "B") g = isA ? 0 : gB;
      else g = isA ? gA : gB;                 // 左右分耳
      if (p.pan) p.pan.pan.value = solo ? 0 : (isA ? -1 : 1);
      p.gain.gain.setTargetAtTime(g, audioCtx.currentTime, 0.008);
    }
  },
  stop() {
    for (const p of this.nodes) { try { p.src.stop(); } catch (e) {} try { p.src.disconnect(); } catch (e) {} }
    if (this.masterNode) { try { this.masterNode.disconnect(); } catch (e) {} }
    this.nodes = []; this.masterNode = null;
    if (this.playing) this.startPos = this.pos; // 保持位置
    this.playing = false;
    $("play").textContent = "播放";
  },
  seek(sec) {
    const wasPlaying = this.playing;
    this.stop();
    this.startPos = Math.max(0, Math.min(this.duration(), sec));
    if (wasPlaying) this.play(this.startPos);
  },
};
$("play").onclick = () => {
  if (!S.tracks.A) return;
  if (Transport.playing) Transport.stop();
  else { if (Transport.pos >= Transport.duration() - 0.05) Transport.startPos = 0; Transport.play(Transport.pos); }
};
$("loop").onchange = e => { S.loop = e.target.checked; };
$("match").onchange = e => { S.matchLevel = e.target.checked; Transport.applyListen(); };
function setListen(m) {
  S.listen = m;
  for (const [id, v] of [["listenA", "A"], ["listenB", "B"], ["listenAB", "AB"]])
    $(id).classList.toggle("sel", v === m);
  Transport.applyListen();
}
$("listenA").onclick = () => setListen("A");
$("listenB").onclick = () => setListen("B");
$("listenAB").onclick = () => setListen("AB");
setListen("AB");

function updateLayout() {
  const cmp = !!S.tracks.B;
  $("boxB").style.display = cmp ? "flex" : "none";
  $("listenGroup").style.display = cmp ? "flex" : "none";
  $("alignGroup").style.display = cmp ? "flex" : "none";
  $("nameA").textContent = S.tracks.A ? S.tracks.A.name + (S.tracks.A.raw ? "（增强前）" : "") : "—";
  $("nameB").textContent = S.tracks.B ? S.tracks.B.name + (S.tracks.B.raw ? "（增强前）" : "") : "—";
  $("fname").textContent = cmp ? "A/B 对比模式（顺序复述配对含说话差异）" : (S.tracks.A ? S.tracks.A.name : "（左列表点行=单轨）");
  updateAlignUI();
}

// ================= 渲染（每轨 spec+wave，共享时间轴） =================
function fitCanvas(cv) {
  const r = cv.getBoundingClientRect();
  const dpr = window.devicePixelRatio || 1;
  const w = Math.max(1, Math.round(r.width * dpr)), h = Math.max(1, Math.round(r.height * dpr));
  if (cv.width !== w || cv.height !== h) { cv.width = w; cv.height = h; }
  return { w, h, dpr };
}
function fmaxHz(t) { return S.fmax > 0 ? Math.min(S.fmax, t.sampleRate / 2) : t.sampleRate / 2; }

function drawSpecFor(key) {
  const cv = canvases[key].spec, t = S.tracks[key];
  const { w, h } = fitCanvas(cv);
  const ctx = cv.getContext("2d");
  ctx.clearRect(0, 0, w, h);
  if (!t || !t.spec) return;
  const bins = t.bins, hzPerBin = t.sampleRate / 2 / bins;
  const binTop = Math.min(bins, Math.ceil(fmaxHz(t) / hzPerBin));
  const secPerFrame = t.hop / t.sampleRate;
  // 共享时间轴 → 本轨帧区间（B 轨平移 offsetB）
  const axis0 = trackToAxis(0, key);
  let startF = Math.max(0, Math.floor((S.view.start - axis0) / secPerFrame));
  let endF = Math.min(t.frames, Math.ceil((S.view.start + S.view.span - axis0) / secPerFrame));
  const vlen = Math.max(1, endF - startF);
  const off = document.createElement("canvas");
  off.width = vlen; off.height = binTop;
  const octx = off.getContext("2d");
  const img = octx.createImageData(vlen, binTop);
  const dr = S.dynRange;
  for (let x = 0; x < vlen; x++) {
    const f = Math.min(t.frames - 1, startF + x);
    for (let y = 0; y < binTop; y++) {
      const b = binTop - 1 - y;
      const dbv = t.spec[f * bins + b];
      let tv = (dbv + dr) / dr; tv = tv < 0 ? 0 : tv > 1 ? 1 : tv;
      const li = (tv * 255) | 0, p = (y * vlen + x) * 4;
      img.data[p] = LUT[li * 3]; img.data[p + 1] = LUT[li * 3 + 1]; img.data[p + 2] = LUT[li * 3 + 2]; img.data[p + 3] = 255;
    }
  }
  octx.putImageData(img, 0, 0);
  ctx.imageSmoothingEnabled = true;
  ctx.drawImage(off, 0, 0, w, h);
  drawScale(ctx, w, h, t);
  drawCursor(ctx, cv, w, h, key);
  drawHover(ctx, cv, w, h, key, hzPerBin, binTop);
}
function drawScale(ctx, w, h, t) {
  const dpr = window.devicePixelRatio || 1;
  ctx.save();
  ctx.font = `${10 * dpr}px monospace`;
  ctx.fillStyle = "rgba(255,255,255,0.65)";
  ctx.strokeStyle = "rgba(255,255,255,0.18)";
  ctx.lineWidth = dpr;
  const binTop = Math.min(t.bins, Math.ceil(fmaxHz(t) / (t.sampleRate / 2 / t.bins)));
  const fTop = binTop * (t.sampleRate / 2 / t.bins);
  for (let f = 1000; f < fTop; f += 1000) {
    const y = h - (f / fTop) * h;
    ctx.beginPath(); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
    ctx.fillText((f / 1000) + "k", 4 * dpr, y - 2 * dpr);
  }
  const step = pickStep(S.view.span / 6);
  for (let tt = Math.ceil(S.view.start / step) * step; tt < S.view.start + S.view.span; tt += step) {
    const x = ((tt - S.view.start) / S.view.span) * w;
    ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
    ctx.fillText(tt.toFixed(step < 1 ? 2 : 1) + "s", x + 3 * dpr, h - 4 * dpr);
  }
  ctx.restore();
}
function pickStep(target) {
  const steps = [0.05, 0.1, 0.2, 0.5, 1, 2, 5, 10, 30, 60];
  for (const s of steps) if (s >= target) return s;
  return 60;
}
function drawWaveFor(key) {
  const cv = canvases[key].wave, t = S.tracks[key];
  const { w, h } = fitCanvas(cv);
  const ctx = cv.getContext("2d");
  ctx.clearRect(0, 0, w, h);
  if (!t) return;
  const mid = h / 2;
  ctx.strokeStyle = key === "A" ? "#4da3ff" : "#b085f5";
  ctx.lineWidth = Math.max(1, window.devicePixelRatio || 1);
  const axis0 = trackToAxis(0, key);
  // 共享视口映射到本轨样本
  const s0 = Math.max(0, Math.floor((S.view.start - axis0) * t.sampleRate));
  const s1 = Math.min(t.pcm.length, Math.ceil((S.view.start + S.view.span - axis0) * t.sampleRate));
  ctx.beginPath();
  const per = Math.max(1, (s1 - s0) / w);
  for (let x = 0; x < w; x++) {
    const a = Math.floor(s0 + x * per), b = Math.min(s1, Math.floor(a + per) + 1);
    let mn = 1, mx = -1;
    for (let i = a; i < b; i++) { const v = t.pcm[i]; if (v < mn) mn = v; if (v > mx) mx = v; }
    const xx = axis0 + a / t.sampleRate; // 本轨样本 → 共享轴 x
    const px = ((xx - S.view.start) / S.view.span) * w;
    if (px < -1 || px > w + 1) continue;
    ctx.moveTo(px + 0.5, mid - mx * mid * 0.92);
    ctx.lineTo(px + 0.5, mid - mn * mid * 0.92 + 1);
  }
  ctx.stroke();
  ctx.strokeStyle = "rgba(255,255,255,0.18)";
  ctx.beginPath(); ctx.moveTo(0, mid); ctx.lineTo(w, mid); ctx.stroke();
  drawCursor(ctx, cv, w, h, key);
}
function drawCursor(ctx, cv, w, h, key) {
  if (!Transport.playing) return;
  const x = ((Transport.pos - S.view.start) / S.view.span) * w;
  if (x < 0 || x > w) return;
  ctx.save();
  ctx.strokeStyle = "rgba(255,80,80,0.9)";
  ctx.lineWidth = Math.max(1, window.devicePixelRatio || 1);
  ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.stroke();
  ctx.restore();
}
function drawHover(ctx, cv, w, h, key, hzPerBin, binTop) {
  if (!S.hover || S.hover.cv !== cv) return;
  const { x, y } = S.hover;
  ctx.save();
  ctx.strokeStyle = "rgba(255,255,255,0.35)";
  ctx.lineWidth = 1;
  ctx.beginPath(); ctx.moveTo(x, 0); ctx.lineTo(x, h); ctx.moveTo(0, y); ctx.lineTo(w, y); ctx.stroke();
  ctx.restore();
  const axisT = S.view.start + (x / w) * S.view.span;
  const trackT = axisToTrack(axisT, key);
  const fTop = binTop * hzPerBin;
  const freq = (1 - y / h) * fTop;
  const t = S.tracks[key];
  let dbTxt = "";
  if (t && t.spec) {
    const frame = Math.min(t.frames - 1, Math.max(0, Math.floor(trackT * t.sampleRate / t.hop)));
    const bin = Math.min(t.bins - 1, Math.max(0, Math.floor((1 - y / h) * binTop)));
    dbTxt = `  电平 ${t.spec[frame * t.bins + bin].toFixed(1)} dB(相对)`;
  }
  $("status").textContent = `[${key}] t=${trackT.toFixed(3)}s  f=${freq.toFixed(0)}Hz${dbTxt}`;
}
function drawAll() {
  drawSpecFor("A"); drawWaveFor("A");
  if (S.tracks.B) { drawSpecFor("B"); drawWaveFor("B"); }
}

// ================= 交互 =================
function clampView() {
  S.view.span = Math.min(totalDur(), Math.max(0.05, S.view.span));
  S.view.start = Math.min(Math.max(0, totalDur() - S.view.span), Math.max(-0.5, S.view.start));
}
function bindCanvas(key, cv) {
  cv.addEventListener("wheel", e => {
    if (!S.tracks.A) return;
    e.preventDefault();
    const r = cv.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    const mx = (e.clientX - r.left) * dpr, w = cv.width;
    const anchor = S.view.start + (mx / w) * S.view.span;
    S.view.span *= (e.deltaY > 0 ? 1.25 : 0.8);
    S.view.start = anchor - (mx / w) * S.view.span;
    clampView(); drawAll();
  }, { passive: false });
  let drag = null;
  cv.addEventListener("mousedown", e => { drag = { x: e.clientX, vs: S.view.start, moved: false }; });
  window.addEventListener("mousemove", e => {
    if (drag) {
      const dx = e.clientX - drag.x;
      if (Math.abs(dx) > 3) drag.moved = true;
      const dpr = window.devicePixelRatio || 1;
      S.view.start = drag.vs - dx * dpr / cv.width * S.view.span;
      clampView(); drawAll();
    }
    const r = cv.getBoundingClientRect();
    const dpr = window.devicePixelRatio || 1;
    if (e.clientX >= r.left && e.clientX <= r.right && e.clientY >= r.top && e.clientY <= r.bottom) {
      S.hover = { cv, x: (e.clientX - r.left) * dpr, y: (e.clientY - r.top) * dpr };
      drawAll();
    }
  });
  window.addEventListener("mouseup", e => {
    if (drag && !drag.moved && S.tracks.A) {
      const r = cv.getBoundingClientRect();
      const t = S.view.start + ((e.clientX - r.left) / r.width) * S.view.span;
      Transport.seek(t);
      drawAll();
    }
    drag = null;
  });
  cv.addEventListener("mouseleave", () => { if (S.hover && S.hover.cv === cv) { S.hover = null; $("status").textContent = "—"; drawAll(); } });
}
bindCanvas("A", canvases.A.spec); bindCanvas("A", canvases.A.wave);
bindCanvas("B", canvases.B.spec); bindCanvas("B", canvases.B.wave);
$("frange").onchange = e => { S.fmax = parseInt(e.target.value, 10); drawAll(); };
$("contrast").oninput = e => { S.dynRange = parseInt(e.target.value, 10); drawAll(); };
$("zoomreset").onclick = () => { S.view.start = 0; S.view.span = totalDur(); drawAll(); };

window.addEventListener("keydown", e => {
  if (e.code === "Space" && S.tracks.A && !/INPUT|SELECT/.test(document.activeElement.tagName)) {
    e.preventDefault();
    if (S.tracks.B) setListen(S.listen === "A" ? "B" : S.listen === "B" ? "AB" : "A");
    else $("play").click();
  }
});
window.addEventListener("resize", drawAll);

// rAF：播放跟随 + 循环
(function raf() {
  if (Transport.playing) {
    if (Transport.pos >= Transport.duration() - 0.02) {
      if (S.loop) Transport.play(0);
      else { Transport.stop(); Transport.startPos = 0; drawAll(); }
    } else {
      // 播放位置跑出视口时跟随
      const p = Transport.pos;
      if (p < S.view.start || p > S.view.start + S.view.span) {
        S.view.start = Math.max(0, p - S.view.span * 0.1);
        clampView();
      }
    }
    drawAll();
  }
  requestAnimationFrame(raf);
})();

loadSessions();
</script>
</body>
</html>
"""


def find_debug_dir(cli_dir: str | None) -> Path:
    """与 spectrogram_server.py 相同的目录解析约定。"""
    if cli_dir:
        return Path(cli_dir)
    appdata = os.environ.get("APPDATA", "")
    if appdata:  # Windows
        roots = [Path(appdata) / "VoiceStick"]
    else:        # macOS / Linux
        roots = [Path.home() / "Library" / "Application Support" / "VoiceStick"]
    for root in roots:
        cfg = root / "config.toml"
        if cfg.is_file():
            m = re.search(r'debug_audio_dir\s*=\s*"([^"]+)"', cfg.read_text(encoding="utf-8"))
            if m:
                return Path(m.group(1).replace("\\\\", "\\"))
        if (root / "DebugAudio").is_dir():
            return root / "DebugAudio"
    return roots[0]


def read_sidecar(audio_dir: Path, ogg_name: str, raw: bool) -> dict | None:
    """读会话侧车：raw 文件继承主文件（去 .raw）的侧车。"""
    base = ogg_name[:-4]  # 去 .ogg
    if raw:
        if not base.endswith(".raw"):
            return None
        base = base[:-4]
    sc = audio_dir / (base + ".json")
    if not sc.is_file():
        return None
    try:
        return json.loads(sc.read_text(encoding="utf-8"))
    except (OSError, ValueError):
        return None


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
            if u.path in ("/", "/index.html"):
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
                files = sorted(audio_dir.glob("*.ogg")) + sorted(audio_dir.glob("*.wav"))
                for p in files:
                    st = p.stat()
                    m = re.search(r"session[-_](\d+)", p.name)
                    item = {
                        "name": p.name,
                        "size": st.st_size,
                        "mtime": datetime.datetime.fromtimestamp(
                            st.st_mtime).strftime("%m-%d %H:%M:%S"),
                        "session": int(m.group(1)) if m else 0,
                        "raw": p.name.endswith(".raw.ogg") or p.name.endswith(".raw.wav"),
                    }
                    if p.suffix == ".ogg":
                        sc = read_sidecar(audio_dir, p.name, item["raw"])
                        if sc:
                            item["sidecar"] = sc
                    items.append(item)
            except OSError as e:
                self._send_json({"error": str(e)}, 500)
                return
            items.sort(key=lambda x: x["name"], reverse=True)
            self._send_json(items)

        def _handle_audio(self, name: str):
            # 防路径穿越：只允许目录内的 .ogg/.wav 文件（同 spectrogram_server）
            if (not name or "/" in name or "\\" in name or ".." in name
                    or not name.endswith((".ogg", ".wav"))):
                self.send_error(400)
                return
            path = audio_dir / name
            if not path.is_file():
                self.send_error(404)
                return
            data = path.read_bytes()
            ctype = "audio/ogg" if name.endswith(".ogg") else "audio/wav"
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
                        self.send_header("Content-Type", ctype)
                        self.send_header("Accept-Ranges", "bytes")
                        self.send_header("Content-Range", f"bytes {start}-{end}/{len(data)}")
                        self.send_header("Content-Length", str(len(chunk)))
                        self.end_headers()
                        self.wfile.write(chunk)
                        return
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Accept-Ranges", "bytes")
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

    return Handler


def main():
    ap = argparse.ArgumentParser(description="VoiceStick 音频实验台（质量红绿灯 + A/B 对比）")
    ap.add_argument("--port", type=int, default=8790)
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
    print(f"音频实验台: {url}  (Ctrl+C 停止)")
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
