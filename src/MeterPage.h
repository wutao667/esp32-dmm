#pragma once

constexpr char kMeterPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>CoreS3 测量记录</title>
<style>
  :root {color-scheme:dark; font:15px system-ui,sans-serif; background:#0b111a; color:#e7eef8}
  * {box-sizing:border-box}
  body {margin:0}
  main {max-width:1080px; padding:28px 20px; margin:auto}
  header {display:flex; flex-wrap:wrap; align-items:baseline; justify-content:space-between; gap:8px}
  h1 {font-size:clamp(24px,5vw,34px); margin:0}
  h2 {font-size:16px; margin:0}
  .muted {color:#a5b5c9}
  nav {display:flex; flex-wrap:wrap; gap:8px; align-items:center; margin:20px 0 12px}
  button, select {font:inherit; color:inherit; background:#182638; border:1px solid #3e526a;
    border-radius:7px; padding:8px 12px; min-height:40px}
  button:not(:disabled) {cursor:pointer}
  button:disabled {opacity:.45}
  .stats {display:grid; grid-template-columns:repeat(auto-fit,minmax(255px,1fr)); gap:12px;
    margin-top:12px}
  .stats article {background:#121e2d; border:1px solid #2c3e53; border-radius:12px; padding:14px}
  .stats dl {display:grid; grid-template-columns:1fr 1fr; gap:8px 14px; margin:10px 0 0}
  .stats dt {font-size:13px; color:#a5b5c9}
  .stats dd {margin:4px 0 0; font-size:18px; font-variant-numeric:tabular-nums}
  .mean-current {color:#70e3b7}
  .mean-voltage {color:#7dcaff}
  .chart {background:#121e2d; border:1px solid #2c3e53; border-radius:12px;
    margin-top:12px; padding:14px}
  .chart header {gap:4px}
  canvas {width:100%; height:200px; display:block; margin-top:8px}
  #detail {min-height:1.5em; font-variant-numeric:tabular-nums}
  #status {min-height:1.4em}
  .error {color:#ffb596}
</style>
<main>
  <header><h1>CoreS3 测量记录</h1><span class="muted" id="count"></span></header>
  <nav aria-label="记录范围">
    <label for="limit">显示</label>
    <select id="limit"><option value="300">300 条</option><option value="900" selected>900 条</option>
      <option value="3600">3,600 条</option></select>
    <button id="prev" disabled>上一段</button><button id="next" disabled>下一段</button>
    <button id="latest">回到最新</button>
    <button id="exportPage" disabled>导出当前页 CSV</button>
    <button id="exportAll" disabled>导出全部历史 CSV</button>
  </nav>
  <div id="status" class="muted" role="status">正在读取 Flash…</div>
  <div id="exportStatus" class="muted" role="status"></div>
  <section class="stats" aria-label="测量统计">
    <article><header><h2>最新值</h2><time id="liveUpdated" class="muted"></time></header>
      <dl><div><dt>电流</dt><dd class="mean-current" id="liveCurrent">—</dd></div>
        <div><dt>电压</dt><dd class="mean-voltage" id="liveVoltage">—</dd></div></dl></article>
    <article><header><h2>历史平均</h2><span id="allSamples" class="muted"></span></header>
      <dl><div><dt>平均电流</dt><dd class="mean-current" id="allCurrent">—</dd></div>
        <div><dt>平均电压</dt><dd class="mean-voltage" id="allVoltage">—</dd></div></dl></article>
    <article><header><h2>当前页平均</h2><span id="pageSamples" class="muted"></span></header>
      <dl><div><dt>平均电流</dt><dd class="mean-current" id="pageCurrent">—</dd></div>
        <div><dt>平均电压</dt><dd class="mean-voltage" id="pageVoltage">—</dd></div></dl></article>
  </section>
  <section class="chart"><header><h2>电流 · µA</h2></header>
    <canvas id="currentChart" aria-label="平均电流曲线"></canvas></section>
  <section class="chart"><header><h2>电压 · V</h2></header>
    <canvas id="voltageChart" aria-label="平均电压曲线"></canvas></section>
  <p id="detail" class="muted">移动指针或轻触曲线查看单条记录。</p>
</main>
<script>
const $ = id => document.getElementById(id);
const timeFormat = new Intl.DateTimeFormat(undefined, {
  month:'2-digit', day:'2-digit', hour:'2-digit', minute:'2-digit', second:'2-digit', hour12:false
});
const refreshTimeFormat = new Intl.DateTimeFormat(undefined, {
  hour:'2-digit', minute:'2-digit', second:'2-digit', hourCycle:'h23'
});
const isUtc = t => t >= 1577836800;
const formatCurrent = value => Number.isFinite(value) ? value.toFixed(4) + ' µA' : '—';
const formatVoltage = value => Number.isFinite(value) ? value.toFixed(6) + ' V' : '—';
const timeText = t => isUtc(t) ? timeFormat.format(t * 1000) : '开机后 ' + t + ' 秒';
const plots = [[$('currentChart'), 'microamps', '#70e3b7'], [$('voltageChart'), 'volts', '#7dcaff']];
let rows = [], start = 0, total = 0, wanted = null, version = 0, pending = false;
let selected = -1;
let exporting = false;
const limit = () => Number($('limit').value);
const margin = {left:64, right:12, top:9, bottom:28};

function timeAxis() {
  return rows.length > 1 && rows.every((r,i) =>
    isUtc(r.t) && (!i || r.t > rows[i-1].t));
}
function position(row, index, useTime) {
  if (rows.length < 2) return 0;
  return useTime ? (row.t - rows[0].t) / (rows.at(-1).t - rows[0].t)
                 : index / (rows.length - 1);
}
function plot(canvas, field, color) {
  const w = canvas.clientWidth, h = canvas.clientHeight;
  const dpr = Math.min(devicePixelRatio || 1, 2);
  canvas.width = Math.round(w * dpr);
  canvas.height = Math.round(h * dpr);
  const ctx = canvas.getContext('2d');
  ctx.scale(dpr,dpr);
  ctx.font = '11px system-ui';
  if (!rows.length) {
    ctx.fillStyle = '#a5b5c9';
    ctx.fillText('暂无记录', margin.left, 24);
    return;
  }
  const values = rows.map(r => r[field]);
  const low = Math.min(...values), high = Math.max(...values);
  const pad = Math.max((high-low)*.08, field === 'volts' ? .0001 : .01);
  const min = low-pad, max = high+pad;
  const pw = w-margin.left-margin.right, ph = h-margin.top-margin.bottom;
  const useTime = timeAxis();
  const x = (r,i) => margin.left + position(r,i,useTime)*pw;
  const y = v => margin.top + (max-v)/(max-min)*ph;
  ctx.textAlign = 'right';
  for (let i=0; i<=4; i++) {
    const v = max - (max-min)*i/4, gy = y(v);
    ctx.strokeStyle = '#314257';
    ctx.beginPath(); ctx.moveTo(margin.left,gy); ctx.lineTo(w-margin.right,gy); ctx.stroke();
    ctx.fillStyle = '#a5b5c9';
    ctx.fillText(v.toFixed(field === 'volts' ? 4 : 2), margin.left-6,gy+4);
  }
  ctx.textAlign = 'left';
  ctx.fillText(timeText(rows[0].t),margin.left,h-8);
  if (rows.length > 1 && pw > 190) {
    ctx.textAlign = 'right';
    ctx.fillText(timeText(rows.at(-1).t),w-margin.right,h-8);
  }
  ctx.strokeStyle = color;
  ctx.fillStyle = color;
  ctx.lineWidth = 1.6;
  ctx.beginPath();
  rows.forEach((r,i) => {
    if (!i || r.t !== rows[i-1].t+1) ctx.moveTo(x(r,i),y(r[field]));
    else ctx.lineTo(x(r,i),y(r[field]));
  });
  ctx.stroke();
  rows.forEach((r,i) => {
    if (i && rows[i-1].t+1 === r.t) return;
    if (i+1 < rows.length && rows[i+1].t === r.t+1) return;
    ctx.beginPath(); ctx.arc(x(r,i),y(r[field]),2,0,2*Math.PI); ctx.fill();
  });
  if (selected >= 0 && selected < rows.length) {
    const r = rows[selected], sx = x(r,selected);
    ctx.strokeStyle = '#a5b5c9';
    ctx.beginPath(); ctx.moveTo(sx,margin.top); ctx.lineTo(sx,h-margin.bottom); ctx.stroke();
    ctx.beginPath(); ctx.arc(sx,y(r[field]),3,0,2*Math.PI); ctx.fill();
  }
}
function redraw() {
  plots.forEach(args => plot(...args));
}
function showPage(page) {
  $('count').textContent = '已存 ' + total.toLocaleString() + ' 条';
  $('status').className = 'muted';
  $('status').textContent = rows.length
    ? '第 ' + (start+1).toLocaleString() + '–' + (start+rows.length).toLocaleString() +
      ' 条 · ' + (wanted === null ? '自动更新' : '历史记录')
    : 'Flash 暂无记录';
  $('prev').disabled = start === 0;
  $('next').disabled = start+rows.length >= total;
  $('exportPage').disabled = exporting || rows.length === 0;
  $('exportAll').disabled = exporting || total === 0;
  $('allSamples').textContent = total.toLocaleString() + ' 条';
  $('pageSamples').textContent = rows.length.toLocaleString() + ' 条';
  $('allCurrent').textContent = formatCurrent(page.meanCurrent);
  $('allVoltage').textContent = formatVoltage(page.meanVoltage);
  $('pageCurrent').textContent = formatCurrent(rows.reduce((sum,r) => sum + r.microamps, 0) / rows.length);
  $('pageVoltage').textContent = formatVoltage(rows.reduce((sum,r) => sum + r.volts, 0) / rows.length);
  $('detail').textContent = '移动指针或轻触曲线查看单条记录。';
  selected = -1;
  redraw();
}
async function fetchPage(pageLimit, pageStart) {
  const query = new URLSearchParams({limit:pageLimit});
  if (pageStart !== null) query.set('start',pageStart);
  const response = await fetch('/api/log?' + query, {cache:'no-store',signal:AbortSignal.timeout(8000)});
  if (!response.ok) throw new Error('HTTP ' + response.status);
  const data = new DataView(await response.arrayBuffer());
  if (data.byteLength % 12) throw new Error('记录长度错误');
  const records = [];
  for (let p=0; p<data.byteLength; p+=12) {
    records.push({t:data.getUint32(p,true),volts:data.getFloat32(p+4,true),
      microamps:data.getFloat32(p+8,true)});
  }
  return {total:Number(response.headers.get('X-Log-Count')),
    start:Number(response.headers.get('X-Log-Start')),
    meanCurrent:parseFloat(response.headers.get('X-Log-Mean-Current')),
    meanVoltage:parseFloat(response.headers.get('X-Log-Mean-Voltage')),records};
}
async function load() {
  if (pending) return;
  pending = true;
  const requestVersion = version;
  try {
    const page = await fetchPage(limit(), wanted);
    if (requestVersion !== version) return;
    total = page.total;
    start = page.start;
    rows = page.records;
    showPage(page);
  } catch (error) {
    if (requestVersion === version) {
      $('status').className = 'error';
      $('status').textContent = '读取失败 · ' + error.message;
    }
  } finally {
    pending = false;
    if (requestVersion !== version) load();
  }
}
const csvHeader = '\uFEFF日期,时间,电流,电压\r\n';
const padTime = value => String(value).padStart(2,'0');
function csvRows(records) {
  return records.map(r => {
    let date = '', time;
    if (isUtc(r.t)) {
      const local = new Date(r.t*1000);
      date = [local.getFullYear(),padTime(local.getMonth()+1),padTime(local.getDate())].join('-');
      time = [local.getHours(),local.getMinutes(),local.getSeconds()].map(padTime).join(':');
    } else {
      const elapsed = [Math.floor(r.t/3600),Math.floor(r.t/60)%60,r.t%60].map(padTime).join(':');
      time = '开机后 ' + elapsed;
    }
    return [date,time,r.microamps.toPrecision(9),r.volts.toPrecision(9)].join(',');
  }).join('\r\n') + '\r\n';
}
function downloadCsv(parts, suffix) {
  const url = URL.createObjectURL(new Blob(parts,{type:'text/csv;charset=utf-8'}));
  const link = document.createElement('a');
  const stamp = new Date().toISOString().slice(0,19).replaceAll(':','-');
  link.href = url;
  link.download = 'cores3-' + suffix + '-' + stamp + '.csv';
  document.body.append(link);
  link.click();
  link.remove();
  setTimeout(() => URL.revokeObjectURL(url),10000);
}
function exportStatus(text, error = false) {
  $('exportStatus').className = error ? 'error' : 'muted';
  $('exportStatus').textContent = text;
}
$('exportPage').onclick = () => {
  if (!rows.length) return;
  downloadCsv([csvHeader,csvRows(rows)],'page');
  exportStatus('已生成当前页 CSV，共 ' + rows.length.toLocaleString() + ' 条。');
};
$('exportAll').onclick = async () => {
  if (exporting) return;
  exporting = true;
  $('exportAll').disabled = true;
  $('exportPage').disabled = true;
  exportStatus('正在读取全部历史…');
  const parts = [csvHeader];
  try {
    const first = await fetchPage(3600,0);
    const target = first.total;
    let exported = 0;
    while (exported < target) {
      const page = exported ? await fetchPage(Math.min(3600,target-exported),exported) : first;
      if (page.total < target || page.start !== exported || !page.records.length) {
        throw new Error('历史记录已变化，请重试');
      }
      const batch = page.records.slice(0,target-exported);
      parts.push(csvRows(batch));
      exported += batch.length;
      exportStatus('正在导出 ' + exported.toLocaleString() + ' / ' + target.toLocaleString() + ' 条…');
    }
    downloadCsv(parts,'history');
    exportStatus('已生成全部历史 CSV，共 ' + target.toLocaleString() + ' 条。');
  } catch (error) {
    exportStatus('导出失败 · ' + error.message,true);
  } finally {
    exporting = false;
    $('exportAll').disabled = total === 0;
    $('exportPage').disabled = rows.length === 0;
  }
};
function navigate(index) {
  wanted = index;
  ++version;
  load();
}
$('prev').onclick = () => navigate(Math.max(0,start-limit()));
$('next').onclick = () => navigate(start+limit());
$('latest').onclick = () => navigate(null);
$('limit').onchange = () => navigate(wanted === null ? null : start);
plots.forEach(([canvas]) => canvas.addEventListener('pointermove', e => {
  if (!rows.length) return;
  const rect = canvas.getBoundingClientRect();
  const target = (e.clientX-rect.left-margin.left)/(rect.width-margin.left-margin.right);
  const useTime = timeAxis();
  let best = Infinity;
  rows.forEach((r,i) => {
    const distance = Math.abs(position(r,i,useTime)-target);
    if (distance < best) { selected = i; best = distance; }
  });
  const r = rows[selected];
  $('detail').textContent = timeText(r.t)+' · '+r.microamps.toFixed(4)+' µA · '+r.volts.toFixed(6)+' V';
  redraw();
}));
addEventListener('resize',redraw);
setInterval(() => { if (wanted === null && !exporting) load(); },4000);
async function updateLive() {
  let latest = null;
  try {
    const response = await fetch('/api/live',{cache:'no-store',signal:AbortSignal.timeout(3000)});
    if (response.ok) latest = await response.json();
  } catch {
    // Clear the live values when the device is unreachable.
  }
  $('liveCurrent').textContent = formatCurrent(latest?.microamps);
  $('liveVoltage').textContent = formatVoltage(latest?.volts);
  if (Number.isFinite(latest?.microamps) && Number.isFinite(latest?.volts)) {
    const now = new Date();
    const updated = $('liveUpdated');
    updated.dateTime = now.toISOString();
    updated.title = timeFormat.format(now);
    updated.textContent = '更新于 ' + refreshTimeFormat.format(now);
  }
  setTimeout(updateLive,1000);
}
load();
updateLive();
</script>
</html>)HTML";
