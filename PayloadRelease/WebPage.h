// Control page served at "/". Talks to the JSON API in PayloadRelease.ino.
#pragma once

const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Payload Release</title>
<style>
:root{--bg:#111418;--card:#1b2027;--fg:#e8eaed;--mut:#8b949e;--ok:#2ea043;--warn:#d29922;--bad:#da3633;--acc:#388bfd}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:16px system-ui,sans-serif}
main{max-width:520px;margin:0 auto;padding:16px}
h1{font-size:20px;margin:4px 0 12px;display:flex;justify-content:space-between;align-items:center}
.dot{font-size:13px;color:var(--mut)}.dot::before{content:"";display:inline-block;width:9px;height:9px;border-radius:50%;background:var(--bad);margin-right:6px}
.dot.on::before{background:var(--ok)}
.card{background:var(--card);border-radius:12px;padding:16px;margin-bottom:12px}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.pill{padding:6px 12px;border-radius:99px;font-weight:600;font-size:14px;background:#30363d}
.armed{background:var(--warn);color:#000}.open{background:var(--acc)}
button{font:inherit;border:0;border-radius:10px;padding:12px 16px;background:#30363d;color:var(--fg);cursor:pointer;flex:1}
button:active{transform:scale(.98)}button:disabled{opacity:.35;cursor:not-allowed}
#drop{width:100%;padding:28px;font-size:24px;font-weight:700;background:var(--bad);margin-top:12px}
#arm.on{background:var(--warn);color:#000}
label{display:block;font-size:13px;color:var(--mut);margin:10px 0 4px}
input{font:inherit;width:100%;padding:10px;border-radius:8px;border:1px solid #30363d;background:var(--bg);color:var(--fg)}
input[type=range]{padding:0}
dl{display:grid;grid-template-columns:auto 1fr;gap:4px 12px;margin:12px 0 0;font-size:14px}dt{color:var(--mut)}dd{margin:0}
details summary{cursor:pointer;font-weight:600}
#msg{min-height:20px;font-size:14px;color:var(--bad);margin-top:8px}
</style></head><body><main>
<h1>Payload Release <span id="conn" class="dot">offline</span></h1>

<div class="card">
  <div class="row"><span id="armState" class="pill">DISARMED</span><span id="latch" class="pill">CLOSED</span></div>
  <button id="drop" disabled>DROP</button>
  <div class="row" style="margin-top:12px">
    <button id="arm">Arm</button><button id="open">Open</button><button id="close">Close</button>
  </div>
  <div id="msg"></div>
  <dl><dt>Servo</dt><dd id="pos">-</dd><dt>Drops</dt><dd id="drops">-</dd><dt>Uptime</dt><dd id="up">-</dd></dl>
</div>

<div class="card"><details><summary>Calibration</summary>
  <label>Jog servo: <b id="jogv">1500</b> us</label>
  <input id="jog" type="range" min="500" max="2500" step="10" value="1500">
  <div class="row" style="margin-top:8px"><button id="useClosed">Use as closed</button><button id="useOpen">Use as open</button></div>
  <label>Closed position (us)</label><input id="closedUs" type="number" min="500" max="2500">
  <label>Open position (us)</label><input id="openUs" type="number" min="500" max="2500">
  <label>Auto-close after drop (ms, 0 = off)</label><input id="autoCloseMs" type="number" min="0">
  <div class="row" style="margin-top:12px"><button id="apply">Apply</button><button id="save">Save to EEPROM</button><button id="defaults">Defaults</button></div>
</details></div>

<div class="card"><details><summary>Settings</summary>
  <label>Access key (only if WEB_KEY is set in the sketch)</label>
  <input id="key" type="password" autocomplete="off">
</details></div>

<script>
const $=id=>document.getElementById(id);
let key='';try{key=localStorage.getItem('key')||''}catch(e){}
$('key').value=key;
$('key').oninput=e=>{key=e.target.value;try{localStorage.setItem('key',key)}catch(e){}};
let s={},editing=false;
function msg(t){$('msg').textContent=t||''}
function render(d){
  s=d;
  $('armState').textContent=d.armed?'ARMED':'DISARMED';$('armState').className='pill'+(d.armed?' armed':'');
  $('latch').textContent=d.open?'OPEN':'CLOSED';$('latch').className='pill'+(d.open?' open':'');
  $('arm').textContent=d.armed?'Disarm':'Arm';$('arm').className=d.armed?'on':'';
  $('drop').disabled=!d.armed;
  $('pos').textContent=d.pos+' us';$('drops').textContent=d.drops;
  const u=d.uptimeS;$('up').textContent=Math.floor(u/3600)+'h '+Math.floor(u/60)%60+'m '+u%60+'s';
  if(!editing){$('closedUs').value=d.closedUs;$('openUs').value=d.openUs;$('autoCloseMs').value=d.autoCloseMs}
}
async function api(path,post=true){
  try{
    const r=await fetch(path,{method:post?'POST':'GET',headers:post?{'X-Key':key}:{}});
    const d=await r.json();
    $('conn').className='dot on';$('conn').textContent='online';
    render(d);msg(d.error?'Error: '+d.error:'');
    return r.ok;
  }catch(e){$('conn').className='dot';$('conn').textContent='offline';return false}
}
const poll=()=>api('/api/status',false);
$('arm').onclick=()=>api(s.armed?'/api/disarm':'/api/arm');
$('drop').onclick=()=>{if(confirm('Release the payload now?'))api('/api/drop')};
$('open').onclick=()=>api('/api/open');
$('close').onclick=()=>api('/api/close');
let jogT;$('jog').oninput=e=>{$('jogv').textContent=e.target.value;clearTimeout(jogT);jogT=setTimeout(()=>api('/api/jog?us='+e.target.value),80)};
$('useClosed').onclick=()=>{$('closedUs').value=$('jog').value;editing=true};
$('useOpen').onclick=()=>{$('openUs').value=$('jog').value;editing=true};
['closedUs','openUs','autoCloseMs'].forEach(id=>$(id).oninput=()=>editing=true);
$('apply').onclick=async()=>{if(await api('/api/set?closed='+$('closedUs').value+'&open='+$('openUs').value+'&autoclose='+$('autoCloseMs').value))editing=false};
$('save').onclick=async()=>{if(await api('/api/set?closed='+$('closedUs').value+'&open='+$('openUs').value+'&autoclose='+$('autoCloseMs').value)&&await api('/api/save')){editing=false;msg('Saved.')}};
$('defaults').onclick=()=>{editing=false;api('/api/defaults')};
poll();setInterval(poll,1000);
</script></main></body></html>)HTML";
