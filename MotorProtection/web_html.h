#pragma once

#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=1024">
<title>MPS-505 Motor Protection</title>
<style>
:root{
  --bg:#020617;--surface:#0F172A;--card:#1E293B;--muted:#1A1E2F;
  --fg:#F8FAFC;--dim:#94A3B8;--border:#334155;
  --ok:#22C55E;--warn:#F59E0B;--bad:#EF4444;--info:#38BDF8;
  --pad:16px;--r:10px;
}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--fg);
  font:14px/1.5 ui-sans-serif,system-ui,Segoe UI,sans-serif}
body{min-width:1024px}
a{color:var(--info);text-decoration:none}
a:hover{text-decoration:underline}
header{display:flex;align-items:center;gap:24px;padding:12px 24px;
  background:var(--surface);border-bottom:1px solid var(--border);position:sticky;top:0;z-index:10}
.brand{font:600 16px/1 ui-monospace,Consolas,monospace;letter-spacing:.08em}
nav a{color:var(--dim);margin-right:16px;padding:8px 4px;border-bottom:2px solid transparent}
nav a.on{color:var(--fg);border-color:var(--ok)}
.meta{margin-left:auto;color:var(--dim);font:12px/1.4 ui-monospace,Consolas,monospace}
main{padding:24px;max-width:1480px;margin:0 auto}
h1{font-size:20px;font-weight:600;margin:0 0 16px}
.banner{background:#1c1917;border:1px solid #78350f;color:#FCD34D;padding:12px 16px;border-radius:var(--r);margin-bottom:16px}
.err{background:#450a0a;border:1px solid #7f1d1d;color:#FECACA;padding:12px 16px;border-radius:var(--r);margin-bottom:16px}
.okmsg{background:#052e16;border:1px solid #166534;color:#BBF7D0;padding:12px 16px;border-radius:var(--r);margin-bottom:16px}
table{width:100%;border-collapse:collapse;background:var(--surface);border:1px solid var(--border);border-radius:var(--r);overflow:hidden}
th,td{padding:10px 12px;text-align:left;border-bottom:1px solid var(--border);font-variant-numeric:tabular-nums;vertical-align:middle}
th{color:var(--dim);font-size:12px;font-weight:600;text-transform:uppercase;letter-spacing:.04em;background:var(--muted)}
tr:last-child td{border-bottom:none}
tr:hover td{background:rgba(30,41,59,.55)}
.badge{display:inline-block;padding:2px 8px;border-radius:999px;font-size:12px;font-weight:600}
.s-Stopped{background:#1e293b;color:#94a3b8}
.s-Running{background:#052e16;color:#4ade80}
.s-Fault{background:#450a0a;color:#f87171}
.s-Cooling{background:#451a03;color:#fbbf24}
.fault-tag{display:block;font-size:11px;color:var(--bad);margin-top:2px}
button,.btn{cursor:pointer;border:1px solid var(--border);background:var(--card);color:var(--fg);
  padding:8px 14px;border-radius:6px;font:500 13px/1.2 inherit;min-height:36px}
button:hover,.btn:hover{border-color:#64748b}
button:disabled{opacity:.4;cursor:not-allowed}
.btn-ok{background:#14532d;border-color:#166534}
.btn-bad{background:#7f1d1d;border-color:#991b1b}
.btn-warn{background:#78350f;border-color:#92400e}
.row-actions{display:flex;gap:8px;flex-wrap:nowrap}
form{display:grid;grid-template-columns:240px 1fr;gap:12px 16px;max-width:760px;background:var(--surface);padding:20px;border:1px solid var(--border);border-radius:var(--r)}
label{color:var(--dim);align-self:center}
input,select{background:var(--bg);color:var(--fg);border:1px solid var(--border);border-radius:6px;padding:8px 10px;font:14px inherit;min-height:36px;width:100%}
input:focus,select:focus,button:focus{outline:2px solid var(--ok);outline-offset:1px}
input:disabled,select:disabled{opacity:.45}
.steps{grid-column:1/-1;display:grid;gap:8px}
.step{display:grid;grid-template-columns:80px 1fr 1fr;gap:8px;align-items:center}
.form-actions{grid-column:1/-1;display:flex;gap:8px;margin-top:8px}
.empty{color:var(--dim);padding:32px;text-align:center}
.bar{height:8px;background:#0f172a;border-radius:99px;overflow:hidden;min-width:80px;display:inline-block;vertical-align:middle}
.bar>i{display:block;height:100%;background:var(--ok)}
.bar.warn>i{background:var(--warn)}
.bar.hot>i{background:var(--bad)}
.page{display:none}
.page.on{display:block}
.help{color:var(--dim);font-size:12px;grid-column:2}
.dash-field{color:var(--dim);font-size:11px}
.mono{font-family:ui-monospace,Consolas,monospace}
#e-pick{max-width:360px;margin-left:8px}
</style>
</head>
<body>
<header>
  <div class="brand">MPS-505</div>
  <nav>
    <a href="#dash" data-p="dash" class="on">Dashboard</a>
    <a href="#add" data-p="add">Add motor</a>
    <a href="#edit" data-p="edit">Edit</a>
    <a href="#log" data-p="log">Log</a>
    <a href="#sec" data-p="sec">Security</a>
  </nav>
  <div class="meta" id="meta">connecting…</div>
  <a href="/logout" style="margin-left:16px;color:var(--dim)">Log out</a>
</header>
<main>
  <div id="flash"></div>
  <section class="page on" id="p-dash">
    <h1>Motors</h1>
    <div id="dash-empty" class="empty">No motors configured. Use Add motor.</div>
    <table id="dash-tbl" hidden>
      <thead><tr>
        <th>Ch</th><th>Name</th><th>Status</th><th>Uptime</th><th>Faults</th>
        <th>RMS (A)</th><th>Voltage (V)</th><th>Thermal</th><th></th>
      </tr></thead>
      <tbody id="dash-body"></tbody>
    </table>
    <p style="margin-top:16px"><button id="btn-cal">Calibrate zeros</button>
      <span class="help" style="display:inline;margin-left:12px">Current and DC voltage. All motors must be Stopped or Fault. Relays stay off so voltage taps sit at 0 V.</span></p>
  </section>
  <section class="page" id="p-add">
    <h1>Add motor</h1>
    <form id="f-add">
      <label for="a-ph">Phase count</label>
      <select id="a-ph" name="phases"><option value="1">1 — single phase</option><option value="3">3 — three phase</option></select>
      <label>Allocated channels</label>
      <div id="a-ch" class="help">select phase count</div>
      <label for="a-name">Name</label>
      <input id="a-name" name="name" maxlength="23" required>
      <label for="a-in">Operating current In (A)</label>
      <input id="a-in" name="in" type="number" step="0.01" min="0.01" required>
      <label for="a-ac">Supply</label>
      <select id="a-ac" name="ac"><option value="1">AC</option><option value="0">DC</option></select>
      <label for="a-hz" class="ac-only">Mains frequency</label>
      <select id="a-hz" name="hz" class="ac-only"><option value="50">50 Hz</option><option value="60">60 Hz</option></select>
      <label for="a-vac" class="ac-only">Rated AC voltage (V)</label>
      <input id="a-vac" name="vac" type="number" step="0.1" min="0.1" max="1000" class="ac-only" placeholder="e.g. 230">
      <span class="help ac-only">Static nameplate value. Not sensed, not used for trips.</span>
      <label for="a-uv" class="dc-only">Undervoltage trip (V)</label>
      <input id="a-uv" name="uv" type="number" step="0.1" min="0" max="55" value="0" class="dc-only">
      <span class="help dc-only">0 disables. Live DC, tap downstream of the relay.</span>
      <label for="a-ov" class="dc-only">Overvoltage trip (V)</label>
      <input id="a-ov" name="ov" type="number" step="0.1" min="0" max="55" value="0" class="dc-only">
      <span class="help dc-only">0 disables. Must be greater than UV if both are set.</span>
      <label for="a-stall">Stall current (A)</label>
      <input id="a-stall" name="stall" type="number" step="0.01" min="0.01" required>
      <label for="a-cool">Cooling time (s)</label>
      <input id="a-cool" name="cool" type="number" step="0.1" min="0.1" required>
      <label for="a-auto">Auto-restart</label>
      <select id="a-auto" name="auto"><option value="0">Off</option><option value="1">On</option></select>
      <label for="a-pol">Relay polarity</label>
      <select id="a-pol" name="pol"><option value="1">Active-HIGH (default)</option><option value="0">Active-LOW</option></select>
      <label for="a-n">Protection steps N</label>
      <input id="a-n" name="n" type="number" min="1" max="8" value="3" required>
      <div class="steps" id="a-steps"></div>
      <div class="form-actions"><button class="btn-ok" type="submit">Save motor</button></div>
    </form>
  </section>
  <section class="page" id="p-edit">
    <h1>Edit / delete</h1>
    <p id="edit-empty" class="empty">Select a motor from the dashboard Edit action, or pick below.</p>
    <p><label for="e-pick">Motor</label>
      <select id="e-pick"></select></p>
    <form id="f-edit" hidden>
      <label>Channels</label><div id="e-ch" class="help"></div>
      <label for="e-name">Name</label>
      <input id="e-name" maxlength="23" required>
      <label for="e-in">Operating current In (A)</label>
      <input id="e-in" type="number" step="0.01" min="0.01" required>
      <label for="e-ac">Supply</label>
      <select id="e-ac"><option value="1">AC</option><option value="0">DC</option></select>
      <label for="e-hz" class="e-ac-only">Mains frequency</label>
      <select id="e-hz" class="e-ac-only"><option value="50">50 Hz</option><option value="60">60 Hz</option></select>
      <label for="e-vac" class="e-ac-only">Rated AC voltage (V)</label>
      <input id="e-vac" type="number" step="0.1" min="0.1" max="1000" class="e-ac-only">
      <span class="help e-ac-only">Static nameplate value. Not sensed, not used for trips.</span>
      <label for="e-uv" class="e-dc-only">Undervoltage trip (V)</label>
      <input id="e-uv" type="number" step="0.1" min="0" max="55" class="e-dc-only">
      <span class="help e-dc-only">0 disables.</span>
      <label for="e-ov" class="e-dc-only">Overvoltage trip (V)</label>
      <input id="e-ov" type="number" step="0.1" min="0" max="55" class="e-dc-only">
      <span class="help e-dc-only">0 disables. Must be greater than UV if both are set.</span>
      <label for="e-stall">Stall current (A)</label>
      <input id="e-stall" type="number" step="0.01" min="0.01" required>
      <label for="e-cool">Cooling time (s)</label>
      <input id="e-cool" type="number" step="0.1" min="0.1" required>
      <label for="e-auto">Auto-restart</label>
      <select id="e-auto"><option value="0">Off</option><option value="1">On</option></select>
      <label for="e-pol">Relay polarity</label>
      <select id="e-pol"><option value="1">Active-HIGH</option><option value="0">Active-LOW</option></select>
      <label for="e-n">Protection steps N</label>
      <input id="e-n" type="number" min="1" max="8" required>
      <div class="steps" id="e-steps"></div>
      <div class="form-actions">
        <button class="btn-ok" type="submit">Save changes</button>
        <button class="btn-bad" type="button" id="e-del">Delete motor</button>
      </div>
    </form>
  </section>
  <section class="page" id="p-log">
    <h1>Fault log</h1>
    <div id="log-unavail" class="banner" hidden>Fault log unavailable — no SD card. Protection and dashboard still run.</div>
    <div id="log-wrap">
      <table>
        <thead><tr><th>Uptime (ms)</th><th>Motor</th><th>Type</th><th>Current (A)</th><th>Voltage (V)</th></tr></thead>
        <tbody id="log-body"></tbody>
      </table>
      <p class="form-actions" style="margin-top:16px">
        <a class="btn" href="/api/log/export">Export CSV</a>
        <button class="btn-bad" id="log-clear">Clear log</button>
      </p>
    </div>
  </section>
  <section class="page" id="p-sec">
    <h1>Security</h1>
    <form id="f-sec">
      <label for="s-user">Dashboard username</label>
      <input id="s-user" required maxlength="31">
      <label for="s-pass">Dashboard password</label>
      <input id="s-pass" type="password" required maxlength="31">
      <div class="form-actions"><button class="btn-ok" type="submit">Save credentials</button></div>
    </form>
  </section>
</main>
<script>
const ST=["Stopped","Running","Fault","Cooling"];
let DATA={motors:[],sd_ok:0,ads_ok:[0,0],heap:0,calibrated:0,free_ch:0};
function $(id){return document.getElementById(id)}
function flash(msg,kind){
  const el=$("flash");
  if(!msg){el.innerHTML="";return}
  el.innerHTML='<div class="'+(kind||"err")+'">'+esc(msg)+"</div>";
}
function esc(s){return String(s).replace(/[&<>"]/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;","\"":"&quot;"}[c]))}
function show(p){
  document.querySelectorAll(".page").forEach(x=>x.classList.remove("on"));
  document.querySelectorAll("nav a").forEach(x=>x.classList.toggle("on",x.dataset.p===p));
  $("p-"+p).classList.add("on");
  if(p==="log") loadLog();
  if(p==="add"){refreshAlloc();syncSupply("a");}
  if(p==="edit") fillEditPick();
}
window.addEventListener("hashchange",()=>{
  const p=(location.hash||"#dash").slice(1);
  show(["dash","add","edit","log","sec"].includes(p)?p:"dash");
});
function fmtU(ms){
  ms=ms||0;const s=Math.floor(ms/1000);
  const h=Math.floor(s/3600),m=Math.floor((s%3600)/60),ss=s%60;
  return h?h+"h "+m+"m":(m?m+"m "+ss+"s":ss+"s");
}
function rmsCell(m){
  return (m.rms||[]).map(v=>Number(v).toFixed(2)).join(" / ");
}
function voltCell(m){
  if(m.ac) return "—";
  return (m.volts||[]).map(v=>Number(v).toFixed(1)).join(" / ");
}
function thBar(pct){
  pct=Math.max(0,Number(pct)||0);
  const cls=pct>=90?"hot":pct>=60?"warn":"";
  const w=Math.min(100,pct);
  return '<div class="bar '+cls+'"><i style="width:'+w+'%"></i></div> '+pct.toFixed(0)+"%";
}
function adsLabel(){
  const a=DATA.ads_ok||[0,0];
  if(a[0]&&a[1]) return "ADS ok";
  if(!a[0]&&!a[1]) return "ADS none";
  return "ADS "+(a[0]?"0x48":"miss")+"/"+(a[1]?"0x49":"miss");
}
function renderDash(){
  $("meta").textContent="heap "+DATA.heap+"  |  SD "+(DATA.sd_ok?"ok":"none")+"  |  "+adsLabel()+"  |  free ch "+DATA.free_ch+"  |  cal "+(DATA.calibrated?"yes":"no");
  const used=DATA.motors.filter(m=>m.used);
  $("dash-empty").hidden=used.length>0;
  $("dash-tbl").hidden=used.length===0;
  const tb=$("dash-body");tb.innerHTML="";
  used.forEach(m=>{
    const st=ST[m.status]||"?";
    const ads=(DATA.ads_ok||[0,0]);
    const adsOk=m.ac||(m.channels||[]).every(c=>ads[c<4?0:1]);
    const canStart=m.status===0 && adsOk && (m.ac || m.vcal);
    const startWhy=canStart?"":(!m.ac&&!adsOk)?" title=\"ADS1115 missing for this motor\"":(!m.ac&&!m.vcal)?" title=\"Calibrate DC voltage zeros first\"":"";
    const canStop=m.status===1;
    const canReset=m.status===2||m.status===3;
    const fault=m.last_fault&&(m.status===2||m.status===3)?'<span class="fault-tag">'+esc(m.last_fault)+"</span>":"";
    const tr=document.createElement("tr");
    tr.innerHTML="<td class='mono'>"+esc((m.channels||[]).join(", "))+"</td><td>"+esc(m.name)+
      "<div class='dash-field'>"+(m.ac?("AC "+Number(m.vac||0).toFixed(0)+" V "+(m.hz||"")+" Hz"):"DC")+"</div></td>"+
      "<td><span class='badge s-"+st+"'>"+st+"</span>"+fault+"</td>"+
      "<td>"+fmtU(m.uptime_ms)+"</td><td>"+m.fault_count+"</td>"+
      "<td class='mono'>"+rmsCell(m)+"</td><td class='mono'>"+voltCell(m)+"</td><td>"+thBar(m.thermal_pct)+"</td>"+
      "<td class='row-actions'>"+
      "<button class='btn-ok' data-a='start' data-i='"+m.idx+"' "+(canStart?"":"disabled")+startWhy+">Start</button>"+
      "<button data-a='stop' data-i='"+m.idx+"' "+(canStop?"":"disabled")+">Stop</button>"+
      "<button class='btn-warn' data-a='reset' data-i='"+m.idx+"' "+(canReset?"":"disabled")+">Reset</button>"+
      "<button data-a='edit' data-i='"+m.idx+"'>Edit</button></td>";
    tb.appendChild(tr);
  });
}
$("dash-body").addEventListener("click",ev=>{
  const b=ev.target.closest("button");if(!b)return;
  const i=b.dataset.i,a=b.dataset.a;
  if(a==="edit"){location.hash="#edit";$("e-pick").value=i;fillEditForm();return;}
  post("/api/"+a,"idx="+i).then(r=>{
    if(!r.ok) flash(r.err||"command failed");
    else flash("");
    poll();
  });
});
$("btn-cal").onclick=()=>post("/api/calibrate","").then(r=>flash(r.ok?"Calibrated current and DC voltage zeros.":(r.err||"calibrate failed"), r.ok?"okmsg":"err"));
function stepFields(n,el,pref,vals){
  n=Math.max(1,Math.min(8,n|0));el.innerHTML="";
  for(let i=0;i<n;i++){
    const k=vals&&vals[i]?vals[i].k:"";
    const t=vals&&vals[i]?vals[i].t:"";
    const d=document.createElement("div");d.className="step";
    d.innerHTML="<span>Step "+(i+1)+"</span>"+
      "<input name='"+pref+"k"+i+"' type='number' step='0.01' min='0.01' placeholder='k × In' value='"+k+"' required>"+
      "<input name='"+pref+"t"+i+"' type='number' step='0.01' min='0.01' placeholder='trip time (s)' value='"+t+"' required>";
    el.appendChild(d);
  }
}
$("a-n").addEventListener("input",()=>stepFields(+$("a-n").value,$("a-steps"),"a"));
$("e-n").addEventListener("input",()=>stepFields(+$("e-n").value,$("e-steps"),"e"));
stepFields(3,$("a-steps"),"a");
function setShown(nodes,on){
  nodes.forEach(el=>{
    const lab=el.previousElementSibling;
    el.hidden=!on;
    if(el.tagName==="INPUT"||el.tagName==="SELECT") el.disabled=!on;
    if(lab&&lab.tagName==="LABEL") lab.hidden=!on;
  });
}
function syncSupply(which){
  const ac=$(which+"-ac").value==="1";
  if(which==="a"){
    setShown([...document.querySelectorAll("#f-add .ac-only")],ac);
    setShown([...document.querySelectorAll("#f-add .dc-only")],!ac);
    $("a-vac").required=ac;
  }else{
    setShown([...document.querySelectorAll("#f-edit .e-ac-only")],ac);
    setShown([...document.querySelectorAll("#f-edit .e-dc-only")],!ac);
    $("e-vac").required=ac;
  }
}
function refreshAlloc(){
  const n=$("a-ph").value;
  fetch("/api/alloc?n="+n,{credentials:"same-origin"}).then(guard).then(r=>r.json()).then(j=>{
    $("a-ch").textContent=j.ok?("CH "+j.channels.join(", ")):(j.err||"cannot allocate");
  });
}
$("a-ph").onchange=refreshAlloc;
$("a-ac").onchange=()=>syncSupply("a");
$("e-ac").onchange=()=>syncSupply("e");
$("f-add").onsubmit=ev=>{
  ev.preventDefault();
  const n=+$("a-n").value;
  const ac=$("a-ac").value;
  let body="phases="+$("a-ph").value+"&name="+encodeURIComponent($("a-name").value)+
    "&in="+$("a-in").value+"&ac="+ac+"&hz="+$("a-hz").value+
    "&vac="+(ac==="1"?$("a-vac").value:"0")+
    "&uv="+(ac==="0"?$("a-uv").value:"0")+
    "&ov="+(ac==="0"?$("a-ov").value:"0")+
    "&stall="+$("a-stall").value+"&cool="+$("a-cool").value+"&auto="+$("a-auto").value+
    "&pol="+$("a-pol").value+"&n="+n;
  for(let i=0;i<n;i++){
    body+="&k"+i+"="+document.querySelector("[name=ak"+i+"]").value;
    body+="&t"+i+"="+document.querySelector("[name=at"+i+"]").value;
  }
  post("/api/motor",body).then(j=>{
    if(j.ok){flash("Motor added.","okmsg");location.hash="#dash";poll();}
    else flash(j.err||"add failed");
  });
};
function fillEditPick(){
  const sel=$("e-pick");const cur=sel.value;
  sel.innerHTML=DATA.motors.filter(m=>m.used).map(m=>"<option value='"+m.idx+"'>"+esc(m.name)+"</option>").join("");
  if(cur) sel.value=cur;
  if(sel.value) fillEditForm();
}
$("e-pick").onchange=fillEditForm;
function fillEditForm(){
  const i=+$("e-pick").value;
  const m=DATA.motors.find(x=>x.idx===i);
  if(!m){$("f-edit").hidden=true;return}
  $("f-edit").hidden=false;
  $("e-ch").textContent="CH "+(m.channels||[]).join(", ")+"  ("+m.phases+"-phase)";
  $("e-name").value=m.name;$("e-in").value=m.in;$("e-ac").value=m.ac?1:0;
  $("e-hz").value=m.hz||50;$("e-stall").value=m.stall;$("e-cool").value=m.cool;
  $("e-auto").value=m.auto?1:0;$("e-pol").value=m.pol?1:0;$("e-n").value=m.steps.length||1;
  $("e-vac").value=m.vac||"";
  $("e-uv").value=m.uv||0;
  $("e-ov").value=m.ov||0;
  stepFields(m.steps.length,$("e-steps"),"e",m.steps);
  syncSupply("e");
}
$("f-edit").onsubmit=ev=>{
  ev.preventDefault();
  const i=$("e-pick").value;const n=+$("e-n").value;
  const ac=$("e-ac").value;
  let body="idx="+i+"&name="+encodeURIComponent($("e-name").value)+
    "&in="+$("e-in").value+"&ac="+ac+"&hz="+$("e-hz").value+
    "&vac="+(ac==="1"?$("e-vac").value:"0")+
    "&uv="+(ac==="0"?$("e-uv").value:"0")+
    "&ov="+(ac==="0"?$("e-ov").value:"0")+
    "&stall="+$("e-stall").value+"&cool="+$("e-cool").value+"&auto="+$("e-auto").value+
    "&pol="+$("e-pol").value+"&n="+n;
  for(let k=0;k<n;k++){
    body+="&k"+k+"="+document.querySelector("[name=ek"+k+"]").value;
    body+="&t"+k+"="+document.querySelector("[name=et"+k+"]").value;
  }
  post("/api/motor/edit",body).then(j=>{
    if(j.ok){flash("Saved.","okmsg");poll();}else flash(j.err||"save failed");
  });
};
$("e-del").onclick=()=>{
  if(!confirm("Delete this motor?"))return;
  post("/api/motor/del","idx="+$("e-pick").value).then(j=>{
    if(j.ok){flash("Deleted.","okmsg");location.hash="#dash";poll();}
    else flash(j.err||"delete failed");
  });
};
function loadLog(){
  fetch("/api/log",{credentials:"same-origin"}).then(guard).then(r=>r.json()).then(j=>{
    $("log-unavail").hidden=!!j.sd_ok;
    $("log-wrap").hidden=!j.sd_ok;
    const tb=$("log-body");tb.innerHTML="";
    (j.rows||[]).forEach(row=>{
      const tr=document.createElement("tr");
      tr.innerHTML="<td>"+esc(row.uptime_ms)+"</td><td>"+esc(row.motor)+"</td><td>"+esc(row.type)+"</td><td>"+esc(row.current_A)+"</td><td>"+esc(row.voltage_V||"")+"</td>";
      tb.appendChild(tr);
    });
    if(!(j.rows||[]).length && j.sd_ok){
      tb.innerHTML="<tr><td colspan='5' class='empty'>No fault entries.</td></tr>";
    }
  });
}
$("log-clear").onclick=()=>{
  if(!confirm("Clear fault log?"))return;
  post("/api/log/clear","").then(()=>loadLog());
};
$("f-sec").onsubmit=ev=>{
  ev.preventDefault();
  post("/api/auth","user="+encodeURIComponent($("s-user").value)+"&pass="+encodeURIComponent($("s-pass").value))
    .then(j=>{
      if(j.ok){flash("Credentials saved. Sign in again.","okmsg");setTimeout(()=>{location.href="/login";},400);}
      else flash(j.err||"failed");
    });
};
function guard(r){
  if(r.status===401){location.href="/login";throw 0;}
  return r;
}
function post(url,body){
  return fetch(url,{method:"POST",headers:{"Content-Type":"application/x-www-form-urlencoded"},body,credentials:"same-origin"})
    .then(guard).then(r=>r.json()).catch(()=>({ok:false,err:"request failed"}));
}
function refreshEditPickOnly(){
  const sel=$("e-pick");const cur=sel.value;
  sel.innerHTML=DATA.motors.filter(m=>m.used).map(m=>"<option value='"+m.idx+"'>"+esc(m.name)+"</option>").join("");
  if(cur) sel.value=cur;
}
function poll(){
  fetch("/api/status",{credentials:"same-origin"}).then(guard).then(r=>r.json()).then(j=>{
    DATA=j;renderDash();
    if($("p-edit").classList.contains("on")) refreshEditPickOnly();
  }).catch(()=>{});
}
poll();setInterval(poll,1000);
syncSupply("a");
show((location.hash||"#dash").slice(1)||"dash");
</script>
</body>
</html>
)HTML";

static const char LOGIN_HTML[] PROGMEM = R"HTML(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=1024">
<title>MPS-505 Sign in</title>
<style>
:root{
  --bg:#020617;--surface:#0F172A;--card:#1E293B;
  --fg:#F8FAFC;--dim:#94A3B8;--border:#334155;--ok:#22C55E;--bad:#EF4444;
}
*{box-sizing:border-box}
html,body{margin:0;height:100%;background:var(--bg);color:var(--fg);
  font:14px/1.5 ui-sans-serif,system-ui,Segoe UI,sans-serif}
body{display:flex;align-items:center;justify-content:center;min-width:1024px}
.card{width:420px;background:var(--surface);border:1px solid var(--border);border-radius:10px;padding:32px}
.brand{font:600 16px/1 ui-monospace,Consolas,monospace;letter-spacing:.08em;margin-bottom:8px}
h1{font-size:20px;font-weight:600;margin:0 0 8px}
p.sub{color:var(--dim);margin:0 0 24px}
label{display:block;color:var(--dim);margin:0 0 6px}
input{width:100%;background:var(--bg);color:var(--fg);border:1px solid var(--border);
  border-radius:6px;padding:10px 12px;font:14px inherit;min-height:40px;margin-bottom:16px}
input:focus{outline:2px solid var(--ok);outline-offset:1px}
button{cursor:pointer;width:100%;min-height:40px;border:1px solid #166534;background:#14532d;
  color:var(--fg);border-radius:6px;font:500 14px inherit}
button:hover{border-color:#22c55e}
.err{background:#450a0a;border:1px solid #7f1d1d;color:#FECACA;padding:10px 12px;border-radius:8px;margin-bottom:16px}
.hint{color:var(--dim);font-size:12px;margin-top:16px}
</style>
</head>
<body>
<div class="card">
  <div class="brand">MPS-505</div>
  <h1>Sign in</h1>
  <p class="sub">Motor protection dashboard</p>
  <div class="err" id="err" hidden>Invalid username or password.</div>
  <form id="f" method="post" action="/login">
    <label for="user">Username</label>
    <input id="user" name="user" autocomplete="username" required maxlength="31" autofocus>
    <label for="pass">Password</label>
    <input id="pass" name="pass" type="password" autocomplete="current-password" required maxlength="31">
    <button type="submit">Sign in</button>
  </form>
  <p class="hint">Default credentials are printed on Serial at boot.</p>
</div>
<script>
if(location.search.indexOf("bad")>=0) document.getElementById("err").hidden=false;
</script>
</body>
</html>
)HTML";
