#pragma once

constexpr char kMeterPage[] PROGMEM = R"HTML(<!doctype html>
<html lang="zh-CN">
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>ESP32 DMM电流计</title>
<style>
  :root {color-scheme:dark; font:15px system-ui,sans-serif; background:#0b111a; color:#e7eef8}
  * {box-sizing:border-box}
  body {margin:0}
  main {max-width:1080px; padding:28px 20px; margin:auto}
  header {display:flex; flex-wrap:wrap; align-items:baseline; justify-content:space-between; gap:8px}
  h1 {font-size:clamp(24px,5vw,34px); margin:0}
  h2 {font-size:16px; margin:0}
  .muted {color:#a5b5c9}
  .header-log {margin-inline-start:auto; text-align:right; font-variant-numeric:tabular-nums}
  nav {display:flex; flex-wrap:wrap; gap:8px; align-items:center; margin:20px 0 12px}
  button, select {font:inherit; color:inherit; background:#182638; border:1px solid #3e526a;
    border-radius:7px; padding:8px 12px; min-height:40px}
  button:not(:disabled) {cursor:pointer}
  button:disabled {opacity:.45}
  .danger {color:#ffc1b4; border-color:#a65f59}
  dialog {width:min(420px,calc(100vw - 32px)); background:#182638; color:#e7eef8;
    border:1px solid #3e526a; border-radius:12px; padding:20px}
  dialog::backdrop {background:#0009}
  dialog form {display:flex; justify-content:flex-end; gap:8px; margin-top:18px}
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
  footer {display:flex; flex-wrap:wrap; align-items:center; justify-content:flex-end;
    gap:10px; margin-top:16px; font-variant-numeric:tabular-nums}
  #rangeButton {margin-right:auto}
  .firmware-controls {display:flex; align-items:center; gap:10px}
  #rangeOptions label {display:flex; align-items:center; gap:10px; padding:8px 0}
  #rangeOptions input {width:18px; height:18px; margin:0}
  #otaFile,#otaProgress {width:100%; margin:8px 0; font:inherit}
</style>
<main>
  <header><h1>ESP32 DMM电流计</h1><span class="muted" id="deviceId"></span>
    <div class="muted header-log">
      <div id="count"></div>
      <div id="testDuration"></div>
    </div>
  </header>
  <nav aria-label="记录范围">
    <label for="limit">显示</label>
    <select id="limit"><option value="300">300 条</option><option value="900" selected>900 条</option>
      <option value="3600">3,600 条</option></select>
    <button id="prev" disabled>上一段</button><button id="next" disabled>下一段</button>
    <button id="latest">回到最新</button>
    <button id="exportPage" disabled>导出当前页 CSV</button>
    <button id="exportAll" disabled>导出全部历史 CSV</button>
    <button id="clearHistory" class="danger" disabled>清空历史</button>
  </nav>
  <dialog id="clearDialog" aria-labelledby="clearTitle">
    <h2 id="clearTitle">确认清空历史？</h2>
    <p>此操作会删除 CoreS3 上已保存的全部测量记录。清空后设备会继续记录。</p>
    <form method="dialog">
      <button value="cancel" autofocus>取消</button>
      <button value="clear" class="danger">确认清空</button>
    </form>
  </dialog>
  <dialog id="otaDialog" aria-labelledby="otaTitle">
    <h2 id="otaTitle">固件升级</h2>
    <p id="otaModel" class="muted"></p>
    <p>升级后将重新开始记录，Wi-Fi 配置会保留。</p>
    <label for="otaFile">选择对应机型的固件 .bin</label>
    <input id="otaFile" type="file" accept=".bin,application/octet-stream">
    <progress id="otaProgress" max="100" value="0" hidden></progress>
    <p id="otaStatus" class="muted" role="status"></p>
    <form method="dialog">
      <button id="otaClose" value="cancel" autofocus>关闭</button>
      <button id="otaArm" type="button" disabled>准备升级</button>
      <button id="otaUpload" type="button" disabled>上传并升级</button>
    </form>
  </dialog>
  <dialog id="rangeDialog" aria-labelledby="rangeTitle">
    <h2 id="rangeTitle">选择电流档位</h2>
    <p>选择与实际分流电阻一致的档位。</p>
    <fieldset id="rangeOptions">
      <legend>量程与分流电阻</legend>
      <label><input type="radio" name="range" value="8mA">8 mA · 10 Ω</label>
      <label><input type="radio" name="range" value="1A">1 A · 80 mΩ</label>
      <label><input type="radio" name="range" value="10A">10 A · 5 mΩ</label>
    </fieldset>
    <p id="rangeStatus" class="error" role="status"></p>
    <form method="dialog">
      <button id="rangeCancel" value="cancel" autofocus>取消</button>
      <button id="rangeConfirm" type="button">确认</button>
    </form>
  </dialog>
  <div id="status" class="muted" role="status">正在读取 Flash…</div>
  <div id="exportStatus" class="muted" role="status"></div>
  <div id="clearStatus" class="muted" role="status"></div>
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
  <section class="chart"><header><h2 id="currentChartTitle">电流 · mA</h2></header>
    <canvas id="currentChart" aria-label="平均电流曲线"></canvas></section>
  <section class="chart"><header><h2>电压 · V</h2></header>
    <canvas id="voltageChart" aria-label="平均电压曲线"></canvas></section>
  <p id="detail" class="muted">移动指针或轻触曲线查看单条记录。</p>
  <footer><button id="rangeButton" disabled>当前档位 —</button>
    <div class="firmware-controls"><span id="firmwareVersion" class="muted">固件版本 —</span>
      <button id="openOta">固件升级</button></div></footer>
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
const ranges = {
  '8mA':{label:'8 mA · 10 Ω',unit:'µA',scale:1},
  '1A':{label:'1 A · 80 mΩ',unit:'mA',scale:.001},
  '10A':{label:'10 A · 5 mΩ',unit:'mA',scale:.001}
};
let rangeId = '1A', rangeRevision = 0, rangeSaving = false;
let historyMeanCurrent = NaN, pageMeanCurrent = NaN;
const currentRange = () => ranges[rangeId];
const formatCurrent = value => Number.isFinite(value)
  ? (value * currentRange().scale).toFixed(4) + ' ' + currentRange().unit : '—';
const formatVoltage = value => Number.isFinite(value) ? value.toFixed(6) + ' V' : '—';
const timeText = t => isUtc(t) ? timeFormat.format(t * 1000) : '开机后 ' + t + ' 秒';
const plots = [[$('currentChart'), 'microamps', '#70e3b7'], [$('voltageChart'), 'volts', '#7dcaff']];
let rows = [], start = 0, total = 0, wanted = null, version = 0, pending = false;
let selected = -1;
let exporting = false;
let clearing = false;
let otaPoll = null, otaUploading = false, otaToken = '';
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
  const current = field === 'microamps';
  const scale = current ? currentRange().scale : 1;
  const values = rows.map(r => r[field] * scale);
  const low = Math.min(...values), high = Math.max(...values);
  const pad = Math.max((high-low)*.08, current ? .01 * scale : .0001);
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
    ctx.fillText(v.toFixed(current && scale === 1 ? 2 : 4), margin.left-6,gy+4);
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
    if (!i || r.t !== rows[i-1].t+1) ctx.moveTo(x(r,i),y(values[i]));
    else ctx.lineTo(x(r,i),y(values[i]));
  });
  ctx.stroke();
  rows.forEach((r,i) => {
    if (i && rows[i-1].t+1 === r.t) return;
    if (i+1 < rows.length && rows[i+1].t === r.t+1) return;
    ctx.beginPath(); ctx.arc(x(r,i),y(values[i]),2,0,2*Math.PI); ctx.fill();
  });
  if (selected >= 0 && selected < rows.length) {
    const r = rows[selected], sx = x(r,selected);
    ctx.strokeStyle = '#a5b5c9';
    ctx.beginPath(); ctx.moveTo(sx,margin.top); ctx.lineTo(sx,h-margin.bottom); ctx.stroke();
    ctx.beginPath(); ctx.arc(sx,y(values[selected]),3,0,2*Math.PI); ctx.fill();
  }
}
function redraw() {
  plots.forEach(args => plot(...args));
}
function updateCurrentStats() {
  $('allCurrent').textContent = formatCurrent(historyMeanCurrent);
  $('pageCurrent').textContent = formatCurrent(pageMeanCurrent);
}
function applyRange(id) {
  if (!Object.hasOwn(ranges,id)) return;
  $('rangeButton').disabled = false;
  $('rangeButton').textContent = '当前档位 · ' + ranges[id].label;
  if (id === rangeId) return;
  rangeId = id;
  $('currentChartTitle').textContent = '电流 · ' + currentRange().unit;
  $('liveCurrent').textContent = '—';
  $('detail').textContent = '移动指针或轻触曲线查看单条记录。';
  selected = -1;
  updateCurrentStats();
  redraw();
}
function showPage(page) {
  $('count').textContent = `已存${total.toLocaleString()}条，空间占用${page.usagePercent}%`;
  $('testDuration').textContent = `累积测试时长：${(total / 3600).toFixed(1)} 小时`;
  $('status').className = 'muted';
  $('status').textContent = rows.length
    ? '第 ' + (start+1).toLocaleString() + '–' + (start+rows.length).toLocaleString() +
      ' 条 · ' + (wanted === null ? '自动更新' : '历史记录')
    : 'Flash 暂无记录';
  $('prev').disabled = start === 0;
  $('next').disabled = start+rows.length >= total;
  $('exportPage').disabled = exporting || rows.length === 0;
  $('exportAll').disabled = exporting || total === 0;
  $('clearHistory').disabled = clearing || exporting || total === 0;
  $('allSamples').textContent = total.toLocaleString() + ' 条';
  $('pageSamples').textContent = rows.length.toLocaleString() + ' 条';
  historyMeanCurrent = page.meanCurrent;
  $('allVoltage').textContent = formatVoltage(page.meanVoltage);
  pageMeanCurrent = rows.reduce((sum,r) => sum + r.microamps, 0) / rows.length;
  updateCurrentStats();
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
    usagePercent:Number(response.headers.get('X-Log-Usage-Percent')),
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
function csvRows(records, scale) {
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
    return [date,time,(r.microamps * scale).toPrecision(9),r.volts.toPrecision(9)].join(',');
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
  const range = currentRange();
  downloadCsv([csvHeader,csvRows(rows,range.scale)],'page-'+range.unit);
  exportStatus('已生成当前页 CSV，共 ' + rows.length.toLocaleString() + ' 条。电流单位 ' + range.unit + '。');
};
$('exportAll').onclick = async () => {
  if (exporting) return;
  exporting = true;
  $('exportAll').disabled = true;
  $('exportPage').disabled = true;
  $('clearHistory').disabled = true;
  exportStatus('正在读取全部历史…');
  const parts = [csvHeader];
  const range = currentRange();
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
      parts.push(csvRows(batch,range.scale));
      exported += batch.length;
      exportStatus('正在导出 ' + exported.toLocaleString() + ' / ' + target.toLocaleString() + ' 条…');
    }
    downloadCsv(parts,'history-'+range.unit);
    exportStatus('已生成全部历史 CSV，共 ' + target.toLocaleString() + ' 条。电流单位 ' + range.unit + '。');
  } catch (error) {
    exportStatus('导出失败 · ' + error.message,true);
  } finally {
    exporting = false;
    $('exportAll').disabled = total === 0;
    $('exportPage').disabled = rows.length === 0;
    $('clearHistory').disabled = clearing || total === 0;
  }
};
$('rangeButton').onclick = () => {
  $('rangeOptions').querySelector('input[value="' + rangeId + '"]').checked = true;
  $('rangeStatus').textContent = '';
  $('rangeDialog').showModal();
};
$('rangeDialog').addEventListener('cancel',event => { if (rangeSaving) event.preventDefault(); });
$('rangeConfirm').onclick = async () => {
  if (rangeSaving) return;
  const selectedRange = $('rangeOptions').querySelector('input:checked');
  if (!selectedRange) return;
  rangeSaving = true;
  $('rangeConfirm').disabled = $('rangeCancel').disabled = true;
  $('rangeStatus').textContent = '';
  try {
    const response = await fetch('/api/range',{
      method:'POST',headers:{'X-Meter-Action':'set-range'},
      body:new URLSearchParams({range:selectedRange.value}),signal:AbortSignal.timeout(5000)
    });
    if (!response.ok) throw new Error('HTTP ' + response.status);
    ++rangeRevision;
    applyRange(await response.text());
    $('rangeDialog').close();
  } catch (error) {
    $('rangeStatus').textContent = '无法保存档位 · ' + error.message;
  } finally {
    rangeSaving = false;
    $('rangeConfirm').disabled = $('rangeCancel').disabled = false;
  }
};
$('clearHistory').onclick = () => {
  const dialog = $('clearDialog');
  dialog.returnValue = 'cancel';
  dialog.showModal();
};
$('clearDialog').addEventListener('close',async () => {
  if ($('clearDialog').returnValue !== 'clear' || clearing || exporting) return;
  clearing = true;
  $('clearHistory').disabled = true;
  const message = $('clearStatus');
  message.className = 'muted';
  message.textContent = '正在清空历史…';
  try {
    const response = await fetch('/api/log/clear',{
      method:'POST', headers:{'X-Meter-Action':'clear-history'}, signal:AbortSignal.timeout(6000)
    });
    if (response.status !== 204) throw new Error('HTTP ' + response.status);
    rows = [];
    total = start = 0;
    showPage({meanCurrent:NaN,meanVoltage:NaN,usagePercent:0});
    message.textContent = '历史已清空，设备正在记录新数据。';
    navigate(null);
  } catch (error) {
    message.className = 'error';
    message.textContent = '未能确认清空结果 · ' + error.message;
  } finally {
    clearing = false;
    $('clearHistory').disabled = exporting || total === 0;
  }
});
const otaHeaders = {'X-Meter-Action':'firmware-update'};
let otaConfirmButton = '设备按键';
async function readOta() {
  const response = await fetch('/api/ota',{cache:'no-store',signal:AbortSignal.timeout(3500)});
  if (!response.ok) throw new Error('HTTP ' + response.status);
  return response.json();
}
function otaMessage(text, error = false) {
  $('otaStatus').textContent = text;
  $('otaStatus').className = error ? 'error' : 'muted';
}
$('openOta').onclick = async () => {
  $('otaArm').disabled = true;
  $('otaUpload').disabled = true;
  $('otaProgress').hidden = true;
  $('otaModel').textContent = '';
  otaToken = '';
  $('otaDialog').showModal();
  try {
    const info = await readOta();
    otaConfirmButton = info.model === 'sticks3' ? '左侧 BOOT 键' : '设备按键';
    $('otaModel').textContent = info.model + ' · 当前版本 ' + info.version;
    $('otaArm').disabled = !info.available;
    otaMessage(info.available ? '选好固件后，点击准备升级。' : '此设备没有可用的 OTA 分区。');
  } catch (error) {
    otaMessage('无法读取升级状态 · ' + error.message,true);
  }
};
$('otaArm').onclick = async () => {
  if (!$('otaFile').files.length) {
    otaMessage('请先选择固件文件。',true);
    return;
  }
  $('otaArm').disabled = true;
  try {
    const response = await fetch('/api/ota/arm',{method:'POST',headers:otaHeaders});
    if (!response.ok) throw new Error('HTTP ' + response.status);
    otaToken = await response.text();
    otaMessage('请在 60 秒内短按' + otaConfirmButton + '确认。');
    const deadline = Date.now() + 60000;
    const poll = async () => {
      if (!$('otaDialog').open) return;
      try {
        if ((await readOta()).armed) {
          $('otaUpload').disabled = false;
          otaMessage('设备已确认。点击上传并升级。');
          return;
        }
      } catch {
        // Retry until the button confirmation window expires.
      }
      if (Date.now() < deadline) otaPoll = setTimeout(poll,1000);
      else {
        $('otaArm').disabled = false;
        otaMessage('确认超时，请重新准备升级。',true);
      }
    };
    otaPoll = setTimeout(poll,500);
  } catch (error) {
    $('otaArm').disabled = false;
    otaMessage('无法准备升级 · ' + error.message,true);
  }
};
$('otaDialog').addEventListener('close',() => clearTimeout(otaPoll));
$('otaDialog').addEventListener('cancel',e => { if (otaUploading) e.preventDefault(); });
$('otaUpload').onclick = () => {
  const file = $('otaFile').files[0];
  if (!file || !otaToken || otaUploading) return;
  const form = new FormData();
  form.append('firmware',file);
  const request = new XMLHttpRequest();
  request.open('POST','/api/ota/upload');
  request.setRequestHeader('X-Meter-Action',otaHeaders['X-Meter-Action']);
  request.setRequestHeader('X-Meter-Ota-Token',otaToken);
  request.timeout = 180000;
  otaUploading = true;
  $('otaUpload').disabled = true;
  $('otaClose').disabled = true;
  const progress = $('otaProgress');
  progress.value = 0;
  progress.hidden = false;
  otaMessage('正在上传固件…');
  request.upload.onprogress = event => {
    if (event.lengthComputable) progress.value = event.loaded * 100 / event.total;
  };
  const finish = (ok, text) => {
    otaUploading = false;
    $('otaClose').disabled = false;
    $('otaArm').disabled = ok;
    otaMessage(text,!ok);
    if (ok) {
      progress.value = 100;
      setTimeout(() => location.reload(),8000);
    }
  };
  request.onload = () => finish(request.status === 200,request.status === 200
    ? '固件已校验，设备正在重启。'
    : '升级失败 · ' + (request.responseText || 'HTTP ' + request.status));
  request.onerror = request.ontimeout = () => finish(false,'上传中断，请检查设备状态后重试。');
  request.send(form);
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
  $('detail').textContent = timeText(r.t)+' · '+formatCurrent(r.microamps)+' · '+r.volts.toFixed(6)+' V';
  redraw();
}));
addEventListener('resize',redraw);
setInterval(() => { if (wanted === null && !exporting && !clearing) load(); },4000);
async function updateLive() {
  let latest = null;
  const revision = rangeRevision;
  try {
    const response = await fetch('/api/live',{cache:'no-store',signal:AbortSignal.timeout(3000)});
    if (response.ok) {
      const id = response.headers.get('X-Meter-Id');
      if (id) $('deviceId').textContent = '序列号：#' + id;
      const firmware = response.headers.get('X-Meter-Version');
      if (firmware) $('firmwareVersion').textContent = '固件版本 ' + firmware;
      const range = response.headers.get('X-Meter-Range');
      latest = await response.json();
      if (revision === rangeRevision) applyRange(range);
    }
  } catch {
    // Clear the live values when the device is unreachable.
  }
  if (revision !== rangeRevision) {
    setTimeout(updateLive,1000);
    return;
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
