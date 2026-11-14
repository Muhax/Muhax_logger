#pragma once
#include <pgmspace.h>
#include "page_common.h"

// Admin website (served on the company network, HTTP Basic login).
static const char ADMIN_HTML_HEAD[] PROGMEM =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>MUHAX Logger admin</title>" PAGE_CSS "</head><body><div class=wrap>"
    "<h1 id=ttl>MUHAX Logger</h1><div class=tabs id=tabs></div><div id=main></div></div><script>";

static const char ADMIN_JS[] PROGMEM = R"JS(
const $=s=>document.querySelector(s);
const esc=s=>String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
async function api(p,b){
  try{const r=await fetch(p,b===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
  let j={};try{j=await r.json()}catch(e){}
  if(!r.ok&&!j.error)j.error='HTTP '+r.status;return j;}catch(e){return{error:'Connection lost: '+e.message}}
}
function parseCsv(t){const rows=[];let r=[],c='',q=false;
  for(let i=0;i<t.length;i++){const ch=t[i];
    if(q){if(ch=='"'){if(t[i+1]=='"'){c+='"';i++}else q=false}else c+=ch}
    else if(ch=='"')q=true;else if(ch==','){r.push(c);c=''}
    else if(ch=='\n'){r.push(c);rows.push(r);r=[];c=''}else if(ch!='\r')c+=ch}
  if(c||r.length){r.push(c);rows.push(r)}return rows}
const tabs=[['dash','Dashboard'],['workers','Workers'],['unknown','Unregistered cards'],['records','Records'],['export','Export'],['settings','Settings'],['system','System']];
let tab='dash',ST=null,WK=[],S=null,edit=null,flash='',timer=null;
const ago=s=>s==null||s>=4e9?'never':s<90?s+' s ago':s<5400?Math.round(s/60)+' min ago':Math.round(s/3600)+' h ago';
const note=()=>flash?`<p class="${flash[0]=='!'?'bad':'ok'}">${esc(flash.replace(/^!/,''))}</p>`:'';
function setFlash(t){flash=t;draw()}
function drawTabs(){$('#tabs').innerHTML=tabs.map(t=>`<button class="${t[0]==tab?'on':''}" onclick="go('${t[0]}')">${t[1]}</button>`).join('')}
async function go(t){tab=t;flash='';edit=null;clearInterval(timer);drawTabs();$('#main').innerHTML='<p class=mu>Loading...</p>';
  if(t=='dash'){await loadStatus();timer=setInterval(async()=>{if(tab=='dash'){await loadStatus();draw()}},5000)}
  if(t=='workers'||t=='records'||t=='export')WK=(await api('/api/workers')).workers||[];
  if(t=='unknown'){WK=(await api('/api/workers')).workers||[];ST=await api('/api/unknown')}
  if(t=='settings'||t=='system'){S=await api('/api/settings');await loadStatus()}
  if(t=='records'||t=='export'){ST=await api('/api/dates')}
  draw()}
async function loadStatus(){const s=await api('/api/status');if(!s.error)ST=s;else flash='!'+s.error}
function draw(){drawTabs();$('#main').innerHTML=({dash:vDash,workers:vWorkers,unknown:vUnknown,records:vRecords,export:vExport,settings:vSettings,system:vSystem})[tab]()}

function vDash(){if(!ST||ST.error)return note()||'<p class=bad>No data</p>';
  $('#ttl').textContent='MUHAX Logger'+(ST.company?' - '+ST.company:'');
  const g=ST.github;const gs=['Not configured','Up to date','Pending / failing'][g.state];
  return `<div class=card><h2>${esc(ST.date)} &middot; ${esc(ST.time)}</h2>
  <p>Inside now: <b>${ST.inside.length}</b> &middot; Taps today: <b>${ST.tapsToday}</b></p>
  <p>${ST.inside.map(n=>`<span class="pill in">${esc(n)}</span>`).join(' ')||'<span class=mu>Nobody</span>'}</p></div>
  <div class=card><h2>Status</h2><table>
  <tr><td>Time</td><td class="${ST.timeTrusted?'ok':'bad'}">${ST.timeSynced?(ST.timeTrusted?'Trusted':'Untrusted (sync overdue)'):'Not synced'} &middot; last sync ${ago(ST.syncAgo)}</td></tr>
  <tr><td>SD card</td><td class="${ST.sd?'ok':'bad'}">${ST.sd?'OK':'MISSING'}${ST.queued?` &middot; ${ST.queued} taps waiting in flash`:''}${ST.pending?` &middot; ${ST.pending} taps waiting for time`:''}</td></tr>
  <tr><td>GitHub</td><td class="${g.state==1?'ok':g.state==2?'warn':'mu'}">${gs}${g.state?' &middot; last OK '+ago(g.okAgo):''}${g.pendingDays?` &middot; ${g.pendingDays} file(s) waiting`:''}${g.error?`<br><span class=bad>${esc(g.error)}</span>`:''}</td></tr>
  <tr><td>NFC reader</td><td class="${ST.nfc?'ok':'bad'}">${ST.nfc?'OK':'Not responding'}</td></tr>
  <tr><td>Device</td><td>${esc(ST.deviceId)} &middot; fw ${esc(ST.fw)} &middot; up ${Math.round(ST.uptime/60)} min &middot; free RAM ${Math.round(ST.heap/1024)} KB</td></tr></table></div>`}

function workerForm(){const w=edit||{};const isNew=!w.orig;
  return `<div class=card><h2>${isNew?'Add worker':'Edit worker '+esc(w.orig)}</h2>
  <label>Employee ID<input id=f_id value="${esc(w.id||'')}"></label>
  <label>Name<input id=f_name value="${esc(w.name||'')}"></label>
  <label>Card ID (hex)<input id=f_uid value="${esc(w.uid||'')}" style="text-transform:uppercase"></label>
  <button onclick="tapNow()">${edit&&edit.tapping?'Tap card...':'Tap card now'}</button>
  <label><input type=checkbox id=f_act ${w.active===false?'':'checked'}>Active</label>
  <button class=p onclick="saveW()">Save</button><button onclick="edit=null;draw()">Cancel</button></div>`}
function vWorkers(){return note()+(edit?workerForm():`<button class=p onclick="edit={orig:'',active:true};draw()">+ Add worker</button>
  <a class=btn href="/workers.csv">Export CSV</a><button onclick="impW()">Import CSV</button>`)+
  `<div class="card scroll"><table><tr><th>ID</th><th>Name</th><th>Card</th><th>State</th><th></th></tr>
  ${WK.map(w=>`<tr><td>${esc(w.id)}</td><td>${esc(w.name)}</td><td class=mu>${esc(w.uid)}</td>
  <td>${w.active?(w.in?'<span class="pill in">inside</span>':'<span class=pill>out</span>'):'<span class="pill bad">disabled</span>'}</td>
  <td style="white-space:nowrap"><button onclick="editW('${esc(w.id)}')">Edit</button>
  <button onclick="togW('${esc(w.id)}')">${w.active?'Disable':'Enable'}</button>
  <button class=d onclick="delW('${esc(w.id)}')">Delete</button></td></tr>`).join('')||'<tr><td colspan=5 class=mu>No workers yet</td></tr>'}</table></div>`}
function editW(id){const w=WK.find(x=>x.id==id);edit={orig:w.id,id:w.id,name:w.name,uid:w.uid,active:w.active};flash='';draw()}
async function saveW(){const b={origId:edit.orig,id:$('#f_id').value.trim(),name:$('#f_name').value.trim(),uid:$('#f_uid').value,active:$('#f_act').checked};
  const r=await api('/api/worker',b);if(r.error){flash='!'+r.error;draw();return}
  edit=null;flash='Saved';WK=(await api('/api/workers')).workers||[];draw()}
async function togW(id){const w=WK.find(x=>x.id==id);const r=await api('/api/worker',{origId:id,id:w.id,name:w.name,uid:w.uid,active:!w.active});
  flash=r.error?'!'+r.error:'Saved';WK=(await api('/api/workers')).workers||[];draw()}
async function delW(id){if(!confirm('Delete worker '+id+'?\n\nOld records stay in the logs. Use Disable if you may need this worker again.'))return;
  const r=await api('/api/worker/delete',{id});flash=r.error?'!'+r.error:'Deleted';WK=(await api('/api/workers')).workers||[];draw()}
async function tapNow(){if(edit)edit.tapping=true;
  edit.id=$('#f_id').value;edit.name=$('#f_name').value;edit.active=$('#f_act').checked;draw();
  const s=await api('/api/lasttap');let got=false;
  for(let k=0;k<25&&!got&&edit;k++){await sleep(600);const r=await api('/api/lasttap');if(r.uid&&r.seq!==s.seq){edit.uid=r.uid;got=true}}
  if(edit){edit.tapping=false;if(!got)flash='!No card detected';draw()}}
function impW(){const i=document.createElement('input');i.type='file';i.accept='.csv,text/csv';
  i.onchange=async()=>{const t=await i.files[0].text();const r=await api('/api/workers/import',{csv:t});
    flash=r.error?'!'+r.error:`Imported ${r.added} new, ${r.updated} updated`;WK=(await api('/api/workers')).workers||[];draw()};i.click()}

function vUnknown(){const l=ST.cards||[];return note()+`<div class="card scroll"><p class=mu>Cards that were tapped but are not registered (cleared on restart).</p><table><tr><th>Card ID</th><th>Last seen</th><th>Taps</th><th></th></tr>
  ${l.map(c=>`<tr><td>${esc(c.uid)}</td><td>${esc(c.last)}</td><td>${c.count}</td><td><button class=p onclick="useCard('${esc(c.uid)}')">Make worker</button></td></tr>`).join('')||'<tr><td colspan=4 class=mu>None</td></tr>'}</table></div>`}
function useCard(u){edit={orig:'',uid:u,active:true};tab='workers';go2()}
async function go2(){const e=edit;await go('workers');edit=e;draw()}

function vRecords(){const d=ST.days||[];const opt=WK.map(w=>`<option value="${esc(w.id)}">${esc(w.id)} - ${esc(w.name)}</option>`).join('');
  return `<div class=card><div class=row><label>Date<select id=r_d>${d.slice().reverse().map(x=>`<option>${x}</option>`).join('')||'<option value="">no data</option>'}</select></label>
  <label>Worker<select id=r_w><option value="">All</option>${opt}</select></label></div>
  <button class=p onclick="loadRec()">Show</button></div><div id=rec></div>`}
async function loadRec(){const d=$('#r_d').value;if(!d)return;$('#rec').innerHTML='<p class=mu>Loading...</p>';
  const r=await fetch(`/records.csv?from=${d}&to=${d}&worker=${encodeURIComponent($('#r_w').value)}`);
  const rows=parseCsv(await r.text());rows.shift();
  $('#rec').innerHTML=`<div class="card scroll"><table><tr><th>Time</th><th>ID</th><th>Name</th><th>Action</th><th>Notes</th></tr>
  ${rows.filter(x=>x.length>5).map(x=>{const n=[];if(x[6]=='0')n.push('time not trusted');if(x[7]=='1')n.push('time corrected');if(x[9])n.push(x[9].replace(/_/g,' ').toLowerCase());
  return `<tr><td>${esc(x[0].slice(11))}</td><td>${esc(x[3])}</td><td>${esc(x[4])}</td><td class="${x[5]=='IN'?'ok':'bad'}">${esc(x[5])}</td><td class=warn>${esc(n.join(', '))}</td></tr>`}).join('')||'<tr><td colspan=5 class=mu>No records</td></tr>'}</table></div>`}

function vExport(){const d=ST.days||[];const opt=WK.map(w=>`<option value="${esc(w.id)}">${esc(w.id)} - ${esc(w.name)}</option>`).join('');
  const last=d.length?d[d.length-1]:'',first=d.length?d[Math.max(0,d.length-31)]:'';
  return `<div class=card><h2>Export CSV</h2><div class=row><label>From<input type=date id=e_f value="${first}"></label><label>To<input type=date id=e_t value="${last}"></label>
  <label>Worker<select id=e_w><option value="">All</option>${opt}</select></label></div>
  <button class=p onclick="doExp()">Download CSV</button><p class=mu>UTF-8 with BOM: opens correctly in Excel, including Arabic names.</p></div>`}
function doExp(){const f=$('#e_f').value,t=$('#e_t').value;if(!f||!t){alert('Pick both dates');return}
  location.href=`/records.csv?from=${f}&to=${t}&worker=${encodeURIComponent($('#e_w').value)}&bom=1&dl=1`}

function vSettings(){const s=S;if(!s||s.error)return note()||'<p class=bad>Cannot load</p>';
  const tz=(()=>{let o='';for(let m=-720;m<=840;m+=30){const a=Math.abs(m),l='UTC'+(m<0?'-':'+')+Math.floor(a/60)+(a%60?':'+(a%60):'');o+=`<option value=${m} ${m===s.tz?'selected':''}>${l}</option>`}return o})();
  return note()+`<div class=card><h2>Settings</h2>
  <label>Company name<input id=s_co value="${esc(s.company)}"></label>
  <label>WiFi name<input id=s_ss value="${esc(s.wifi.ssid)}"></label><label>WiFi password (empty = keep)<input id=s_wp type=password autocomplete=new-password></label>
  <label>Time zone<select id=s_tz>${tz}</select></label><label>Time re-sync (hours)<input id=s_sh type=number min=1 max=24 value="${s.syncHours}"></label>
  <label>GitHub owner<input id=s_go value="${esc(s.gh.owner)}"></label><label>GitHub repo<input id=s_gr value="${esc(s.gh.repo)}"></label>
  <label>Branch<input id=s_gb value="${esc(s.gh.branch)}"></label>
  <label>GitHub token ${s.gh.tokenSet?'(saved, never shown; type to replace)':'(not set)'}<input id=s_gt type=password autocomplete=new-password></label>
  <button onclick="ghTest()">Test GitHub</button> <span id=gh_res></span>
  <label>Ignore same card for (seconds)<input id=s_du type=number min=1 max=120 value="${s.opts.dup}"></label>
  <label>Upload delay after a tap (seconds)<input id=s_ud type=number min=10 max=3600 value="${s.opts.uploadDelay}"></label>
  <label><input type=checkbox id=s_led ${s.opts.led?'checked':''}>LED feedback</label>
  <label><input type=checkbox id=s_dim ${s.opts.dim?'checked':''}>Dim screen when idle</label>
  <button class=p onclick="saveS()">Save settings</button></div>
  <div class=card><h2>Admin login</h2><label>Username<input id=p_u value="${esc(s.admin.user)}"></label>
  <label>Current password<input id=p_o type=password autocomplete=current-password></label>
  <label>New password (8+)<input id=p_n type=password autocomplete=new-password></label>
  <button onclick="savePw()">Change login</button></div>`}
async function ghTest(){$('#gh_res').textContent='Testing...';
  const r=await api('/api/ghtest',{owner:$('#s_go').value,repo:$('#s_gr').value,branch:$('#s_gb').value,token:$('#s_gt').value});
  $('#gh_res').innerHTML=`<span class="${r.ok?'ok':'bad'}">${esc(r.message||r.error)}</span>`}
async function saveS(){const b={company:$('#s_co').value,wifi:{ssid:$('#s_ss').value,pass:$('#s_wp').value},tz:Number($('#s_tz').value),syncHours:Number($('#s_sh').value),
  gh:{owner:$('#s_go').value,repo:$('#s_gr').value,branch:$('#s_gb').value,token:$('#s_gt').value},
  opts:{dup:Number($('#s_du').value),uploadDelay:Number($('#s_ud').value),led:$('#s_led').checked,dim:$('#s_dim').checked}};
  const r=await api('/api/settings',b);if(r.error){flash='!'+r.error;draw();return}
  if(r.restart){document.body.innerHTML='<div class=wrap><div class=card><h2>Restarting...</h2><p>WiFi settings changed. Check the device screen for its new address.</p></div></div>';return}
  S=await api('/api/settings');flash='Saved';draw()}
async function savePw(){const r=await api('/api/password',{user:$('#p_u').value,old:$('#p_o').value,pass:$('#p_n').value});
  if(r.error){flash='!'+r.error;draw();return}
  flash='Login changed. Reload the page to sign in again.';draw()}

function vSystem(){return note()+`<div class=card><h2>System</h2>
  <p><button class=p onclick="act('/api/sync-github','Upload started')">Sync to GitHub now</button>
  <button onclick="act('/api/sync-time','Time sync requested')">Sync time now</button>
  <button onclick="phone()">Set time from my phone</button></p>
  <p><button onclick="if(confirm('Restart the device?'))act('/api/restart','Restarting...')">Restart</button>
  <button onclick="if(confirm('Switch to setup mode? The Muhax_logger WiFi network will start; workers and records are kept.'))act('/api/setup-mode','Setup mode started')">Return to setup mode</button></p></div>
  <div class=card><h2>Factory reset</h2><p class=mu>Erases WiFi, GitHub, admin login and all settings. Records and workers on the SD card are kept.</p>
  <button class=d onclick="fr()">Factory reset</button></div>`}
async function act(p,m){const r=await api(p,{});flash=r.error?'!'+r.error:m;draw()}
async function phone(){const r=await api('/api/settime',{epoch:Math.floor(Date.now()/1000)});flash=r.error?'!'+r.error:'Time set from this device';draw()}
async function fr(){const t=prompt('Type RESET to erase all settings');if(t!=='RESET')return;const r=await api('/api/factory-reset',{confirm:'RESET'});
  document.body.innerHTML='<div class=wrap><div class=card><h2>Reset done</h2><p>The device restarts in setup mode.</p></div></div>'}
go('dash');
)JS";

static const char ADMIN_HTML_TAIL[] PROGMEM = "</script></body></html>";
