#pragma once
#include <pgmspace.h>
#include "page_common.h"

// First-time setup wizard (served only on the Muhax_logger setup network).
static const char SETUP_HTML_HEAD[] PROGMEM =
    "<!doctype html><html><head><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<title>MUHAX Logger setup</title>" PAGE_CSS "</head><body><div class=wrap>"
    "<h1>MUHAX Logger &middot; setup</h1><div id=prog class=steps></div><div id=main class=card>Loading...</div><div id=nav></div></div><script>";

static const char SETUP_JS[] PROGMEM = R"JS(
const $=s=>document.querySelector(s);
const esc=s=>String(s==null?'':s).replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const sleep=ms=>new Promise(r=>setTimeout(r,ms));
let D={admin:{user:'admin',pass:'',pass2:''},company:'',wifi:{ssid:'',pass:''},tz:180,syncHours:2,
  gh:{owner:'',repo:'',branch:'main',token:''},opts:{dup:5,led:true,dim:true,uploadDelay:90},workers:[]};
let cur={configured:false,gh:{tokenSet:false}};
let step=0,msg={},nets=null,busy=false;
const names=['Admin','Company','WiFi','Time','GitHub','Workers','Options','Save'];
async function api(p,b){
  try{const r=await fetch(p,b===undefined?{}:{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(b)});
  let j={};try{j=await r.json()}catch(e){}
  if(!r.ok&&!j.error)j.error='HTTP '+r.status;return j;}catch(e){return{error:'Connection lost: '+e.message}}
}
function getp(p){return p.split('.').reduce((a,k)=>a[k],D)}
function setp(p,v){const k=p.split('.');let o=D;for(let i=0;i<k.length-1;i++)o=o[k[i]];o[k[k.length-1]]=v}
document.addEventListener('input',e=>{const t=e.target;if(!t.dataset.p)return;
  setp(t.dataset.p,t.type==='checkbox'?t.checked:(t.dataset.n?Number(t.value):t.value))});
const inp=(l,p,o={})=>`<label>${l}<input data-p="${p}" type="${o.type||'text'}" value="${esc(getp(p))}" ${o.n?'data-n=1':''} ${o.ph?`placeholder="${esc(o.ph)}"`:''} autocomplete=off autocapitalize=off></label>`;
const chk=(l,p)=>`<label><input type=checkbox data-p="${p}" ${getp(p)?'checked':''}>${l}</label>`;
const note=(k)=>msg[k]?`<p class="${msg[k].ok?'ok':'bad'}">${esc(msg[k].t)}</p>`:'';
function tzOpts(){let o='';for(let m=-720;m<=840;m+=30){const a=Math.abs(m),l='UTC'+(m<0?'-':'+')+Math.floor(a/60)+(a%60?':'+(a%60):'');
  o+=`<option value=${m} ${m===D.tz?'selected':''}>${l}</option>`}return o}
function view(){
  switch(step){
  case 0:return`<h2>1. Admin account</h2><p class=mu>Used to log in to the device's website later.</p>
    ${inp('Admin username','admin.user')}
    ${inp(cur.configured?'New password (leave empty to keep)':'Password (8+ characters)','admin.pass',{type:'password'})}
    ${inp('Repeat password','admin.pass2',{type:'password'})}`;
  case 1:return`<h2>2. Company</h2>${inp('Company name','company')}`;
  case 2:return`<h2>3. Company WiFi</h2><p class=mu>Must be a 2.4 GHz network.</p>
    <button onclick="scan()">${busy==='scan'?'Scanning...':'Scan networks'}</button>
    ${nets?`<div class=scroll>${nets.map(n=>`<div><button onclick="pick(${JSON.stringify(n.ssid).replace(/"/g,'&quot;')})">${esc(n.ssid)} <span class=mu>${n.rssi} dBm${n.open?'':' &#128274;'}</span></button></div>`).join('')||'<p class=mu>No networks found</p>'}</div>`:''}
    ${inp('WiFi name','wifi.ssid')}${inp(cur.configured?'WiFi password (leave empty to keep)':'WiFi password','wifi.pass',{type:'password'})}
    <button onclick="testWifi()" ${busy?'disabled':''}>${busy==='wifi'?'Testing...':'Test connection'}</button>${note('wifi')}`;
  case 3:return`<h2>4. Time</h2><label>Time zone<select onchange="D.tz=Number(this.value)">${tzOpts()}</select></label>
    ${inp('Re-sync from the internet every (hours)','syncHours',{n:1,type:'number'})}
    <button onclick="phoneTime()">Use my phone's time now</button>${note('time')}
    <p class=mu>The device has no battery clock; it gets the time from the internet at every start.</p>`;
  case 4:return`<h2>5. GitHub (online copy)</h2><p class=mu>A <b>private</b> repo and a fine-grained token limited to that repo with <i>Contents: read and write</i>. Leave empty to skip.</p>
    ${inp('Owner (user or organisation)','gh.owner')}${inp('Repository name','gh.repo')}${inp('Branch','gh.branch')}
    ${inp(cur.gh.tokenSet?'Access token (saved; type to replace)':'Access token','gh.token',{type:'password'})}
    <button onclick="testGh()" ${busy?'disabled':''}>${busy==='gh'?'Testing...':'Test GitHub'}</button>${note('gh')}`;
  case 5:return`<h2>6. Workers</h2><p class=mu>Optional now; you can also do it later in the admin site.</p>
    <div class=scroll><table><tr><th>ID</th><th>Name</th><th>Card ID</th><th></th></tr>
    ${D.workers.map((w,i)=>`<tr><td><input data-p="workers.${i}.id" value="${esc(w.id)}" size=6></td>
      <td><input data-p="workers.${i}.name" value="${esc(w.name)}"></td>
      <td><input data-p="workers.${i}.uid" id=u${i} value="${esc(w.uid)}" size=12 style="text-transform:uppercase"></td>
      <td style="white-space:nowrap"><button onclick="tapFor(${i})">${msg.tap===i?'Tap card...':'Tap card now'}</button><button class=d onclick="delW(${i})">&times;</button></td></tr>`).join('')}</table></div>
    <button onclick="addW()">+ Add worker</button>${note('w')}`;
  case 6:return`<h2>7. Options</h2>${inp('Ignore the same card for (seconds)','opts.dup',{n:1,type:'number'})}
    ${chk('LED feedback (green = IN, red = OUT)','opts.led')}${chk('Dim the screen when idle','opts.dim')}`;
  default:return`<h2>8. Review &amp; save</h2><div class=mu>
    <p>Admin: <b>${esc(D.admin.user)}</b><br>Company: <b>${esc(D.company)||'-'}</b><br>WiFi: <b>${esc(D.wifi.ssid)||'-'}</b><br>
    GitHub: <b>${D.gh.owner&&D.gh.repo?esc(D.gh.owner+'/'+D.gh.repo):'not set'}</b><br>Workers: <b>${D.workers.filter(w=>w.id&&w.uid).length}</b></p></div>
    <button class=p onclick="save()" ${busy?'disabled':''}>${busy==='save'?'Saving...':'Save and restart'}</button>${note('save')}`;
}}
function render(){
  $('#prog').innerHTML=names.map((n,i)=>`<i class="${i<=step?'on':''}" title="${n}"></i>`).join('');
  $('#main').innerHTML=view();
  $('#nav').innerHTML=(step>0&&!msg.done?'<button onclick="go(-1)">Back</button>':'')+(step<names.length-1?'<button class=p onclick="go(1)">Next</button>':'');
}
function check(){
  if(step===0){const a=D.admin;
    if(a.user.trim().length<3)return'Username needs 3+ characters';
    if(!cur.configured||a.pass){if(a.pass.length<8)return'Password needs 8+ characters';if(a.pass!==a.pass2)return'Passwords do not match'}}
  if(step===2&&!D.wifi.ssid.trim())return'Enter the company WiFi name';
  if(step===3&&(D.syncHours<1||D.syncHours>24))return'Re-sync must be 1 to 24 hours';
  if(step===5){const ids=new Set(),uids=new Set();for(const w of D.workers){if(!w.id&&!w.uid&&!w.name)continue;
    if(!/^[A-Za-z0-9._-]{1,16}$/.test(w.id))return'Worker ID "'+w.id+'" must be 1-16 letters, digits or . _ -';
    const u=(w.uid||'').toUpperCase().replace(/[^0-9A-F]/g,'');if(u.length<8||u.length%2)return'Card ID for '+w.id+' must be 8+ hex characters (use Tap card now)';
    if(ids.has(w.id))return'Duplicate ID '+w.id;if(uids.has(u))return'Duplicate card for '+w.id;ids.add(w.id);uids.add(u)}}
  return''}
function go(d){if(d>0){const e=check();if(e){alert(e);return}}step=Math.max(0,Math.min(names.length-1,step+d));render();scrollTo(0,0)}
async function scan(){busy='scan';render();const r=await api('/api/scan');nets=r.networks||[];busy=false;render()}
function pick(s){D.wifi.ssid=s;render()}
async function testWifi(){busy='wifi';msg.wifi=null;render();
  const r=await api('/api/wifitest',{ssid:D.wifi.ssid,pass:D.wifi.pass});
  msg.wifi={ok:!!r.ok,t:r.ok?'Connected. Device IP: '+r.ip:(r.error||'Failed')};busy=false;render()}
async function testGh(){busy='gh';msg.gh=null;render();
  const r=await api('/api/ghtest',{owner:D.gh.owner,repo:D.gh.repo,branch:D.gh.branch,token:D.gh.token});
  msg.gh={ok:!!r.ok,t:r.message||r.error||'Failed'};busy=false;render()}
async function phoneTime(){const r=await api('/api/settime',{epoch:Math.floor(Date.now()/1000)});
  msg.time={ok:!!r.ok,t:r.ok?'Time set from this phone':(r.error||'Failed')};render()}
function addW(){D.workers.push({id:'',name:'',uid:''});render()}
function delW(i){D.workers.splice(i,1);render()}
async function tapFor(i){msg.tap=i;msg.w=null;render();
  const s=await api('/api/lasttap');const seq=s.seq;let got=false;
  for(let k=0;k<25&&!got;k++){await sleep(600);const r=await api('/api/lasttap');
    if(r.uid&&r.seq!==seq){D.workers[i].uid=r.uid;got=true}}
  msg.tap=-1;if(!got)msg.w={ok:false,t:'No card detected. Hold it on the reader and try again.'};render()}
async function save(){const e=check();if(e){alert(e);return}
  busy='save';msg.save=null;render();
  const p=JSON.parse(JSON.stringify(D));delete p.admin.pass2;
  p.workers=p.workers.filter(w=>w.id||w.uid||w.name);
  const r=await api('/api/setup',p);busy=false;
  if(r.ok){msg.done=true;$('#main').innerHTML='<h2 class=ok>Saved!</h2><p>The device is restarting and joining the company WiFi. Reconnect your phone to your normal WiFi, then read the device address from its screen.</p>';$('#nav').innerHTML='';$('#prog').innerHTML=''}
  else{msg.save={ok:false,t:r.error||'Failed'};render()}}
(async()=>{const c=await api('/api/current');
  if(c&&!c.error){cur=c;D.admin.user=c.admin.user||'admin';D.company=c.company||'';D.wifi.ssid=c.wifi.ssid||'';D.tz=c.tz;D.syncHours=c.syncHours;
    D.gh.owner=c.gh.owner||'';D.gh.repo=c.gh.repo||'';D.gh.branch=c.gh.branch||'main';D.opts=Object.assign(D.opts,c.opts||{});
    D.workers=(c.workers||[]).map(w=>({id:w.id,name:w.name,uid:w.uid}))}
  else{D.tz=-new Date().getTimezoneOffset()}
  render()})();
)JS";

static const char SETUP_HTML_TAIL[] PROGMEM = "</script></body></html>";
