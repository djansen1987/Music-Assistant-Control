#include "web_portal.h"
#include "app_log.h"
#include "config.h"
#include "led.h"
#include "ma.h"
#include <ArduinoJson.h>
#include <ESPAsyncWebServer.h>
#include <WiFi.h>
#include <atomic>
#include <mbedtls/base64.h>

namespace {

const char PAGE_TOP[] PROGMEM = R"rawliteral(<!DOCTYPE html>
<html lang="en">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>Music Assistant Control Setup</title>
<style>
body{background:#121212;color:#fff;font-family:'Helvetica Neue',Helvetica,Arial,sans-serif;display:flex;justify-content:center;padding:20px;margin:0;box-sizing:border-box}
*,*::before,*::after{box-sizing:border-box}
.container{background:#181818;padding:30px;border-radius:8px;width:100%;max-width:400px;box-shadow:0 4px 12px rgba(0,0,0,.5)}
h2{color:#18bcf2;text-align:center;margin-top:0}
p{color:#B3B3B3;line-height:1.5}
.section{margin-bottom:14px;padding-bottom:12px;border-bottom:1px solid #282828}
h3{margin:0 0 8px;font-size:17px}
h4{margin:14px 0 6px}
.hint{margin-bottom:10px!important}
.section:last-child{border-bottom:none;margin-bottom:0;padding-bottom:0}
label{display:block;margin-bottom:8px;color:#B3B3B3;font-size:14px}
input[type=text],input[type=password],select{width:100%;padding:10px;margin-bottom:10px;background:#282828;border:1px solid #3E3E3E;color:#fff;border-radius:4px;font-size:16px}
input:focus{outline:none;border-color:#18bcf2}
button,a.btn{display:block;width:100%;padding:14px;background:#18bcf2;color:#fff;border:none;border-radius:500px;font-size:16px;font-weight:bold;cursor:pointer;text-align:center;text-decoration:none}
button:hover,a.btn:hover{background:#4fcdf6}
.hint{font-size:12px;color:#B3B3B3;margin-bottom:15px;display:block}
code{color:#18bcf2;word-break:break-all}
.msg{margin-top:15px;font-size:14px;color:#F5A623;min-height:1em}
details summary{cursor:pointer;font-size:17px;font-weight:bold;margin-bottom:10px}
details:not([open]) summary{margin-bottom:0}
.row{display:flex;align-items:center;gap:10px;margin-bottom:15px;color:#B3B3B3;font-size:14px}
.row input{width:auto;margin:0}
button.small{width:auto;padding:8px 16px;font-size:14px;background:#282828}
#log{background:#0e0e0e;border:1px solid #282828;border-radius:4px;padding:10px;font:12px/1.4 monospace;max-height:300px;overflow:auto;white-space:pre-wrap;word-break:break-word;margin:0}
#log .W{color:#F5A623}
#log .E{color:#ff6b6b}
#log .D{color:#7f7f7f}
.pl{display:flex;align-items:center;gap:8px;padding:8px 0;border-bottom:1px solid #282828}
.pl span{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.pl button{width:auto;padding:6px 12px;font-size:13px;background:#282828}
#pl-results{max-height:320px;overflow-y:auto;margin-top:10px}
[hidden]{display:none!important}
.tabs{display:flex;gap:4px;margin-bottom:24px;background:#222;padding:4px;border-radius:500px}
.tabs button{flex:1;padding:8px 4px;font-size:14px;background:transparent;color:#B3B3B3}
.tabs button:hover{background:#2a2a2a}
.tabs button.active{background:#18bcf2;color:#fff}
</style>
</head>
<body>
<div class="container">
)rawliteral";

const char PAGE_BOTTOM[] PROGMEM = "</div>\n</body>\n</html>\n";

const char INDEX_BODY[] PROGMEM = R"rawliteral(
<h2>Device Setup</h2>

<nav class="tabs" id="tabs">
<button type="button" data-go="wifi">Wi-Fi</button>
<button type="button" data-go="server" class="sta-only" hidden>Music Assistant</button>
<button type="button" data-go="playlists" class="sta-only" hidden>Playlists</button>
<button type="button" data-go="debug">Debug</button>
<button type="button" data-go="security">Security</button>
</nav>

<div class="section" data-tab="wifi" hidden>
<h3>Add a network</h3>
<form action="/save-wifi" method="POST">
<label for="ssid">Network Name (SSID)</label>
<input type="text" id="ssid" name="ssid" required maxlength="32" autocapitalize="none" autocorrect="off">
<label for="password">Password</label>
<input type="password" id="password" name="password" maxlength="63">
<button type="submit">Save Wi-Fi &amp; Restart</button>
</form>
</div>

<div class="section" data-tab="wifi" hidden>
<h3>Known networks</h3>
<span class="hint">The device tries these in order of last use and skips networks without internet. Networks you add are remembered.</span>
<div id="wifi-list"></div>
<div class="msg" id="wifi-warn"></div>
<button type="button" class="small" id="wifi-reset" style="margin-top:12px">Forget all &amp; restart in setup mode</button>
<div class="msg" id="wifi-msg"></div>
</div>

<div class="section" data-tab="server" hidden>
<details id="srv" open>
<summary>Music Assistant Server <span id="srv-state" style="font-size:13px;font-weight:normal;color:#B3B3B3"></span></summary>
<span class="hint">Enter the address of your Music Assistant server, for example <code>192.168.1.10:8095</code> or <code>https://music.example.com</code>. Then sign in with your Music Assistant username and password, paste an access token (Music Assistant &gt; Settings &gt; Profile &gt; Long-lived access tokens), or sign in with Home Assistant.</span>
<form id="server-form">
<label for="server">Server address</label>
<input type="text" id="server" name="server" maxlength="150" required placeholder="192.168.1.10:8095" autocapitalize="none" autocorrect="off">
<label for="username">Username</label>
<input type="text" id="username" name="username" maxlength="64" autocapitalize="none" autocorrect="off" autocomplete="username">
<label for="mapass">Password</label>
<input type="password" id="mapass" name="password" maxlength="128" autocomplete="current-password">
<span class="hint">Or use an access token instead of a username and password:</span>
<label for="token">Access token</label>
<input type="password" id="token" name="token" maxlength="1024" autocapitalize="none" autocorrect="off" autocomplete="off">
<button type="submit">Connect</button>
<button type="button" id="ha-login" style="margin-top:8px">Sign in with Home Assistant</button>
<span class="hint">Home Assistant sign-in opens a Music Assistant page in this browser, so use the address of the server as your browser reaches it on your home network. A Home Assistant ingress or Nabu Casa link only serves the web page and cannot be used by this device.</span>
</form>
<div class="msg" id="msg"></div>
</details>
</div>

<div class="section" data-tab="server" hidden>
<h3>Player</h3>
<span class="hint">The Music Assistant player this display controls. You can also change it on the display with the list icon.</span>
<label for="player">Player</label>
<select id="player"></select>
<div class="msg" id="player-msg"></div>
</div>

<div class="section" data-tab="server" hidden>
<h3>Display</h3>
<span class="hint" id="display-msg">Changes are saved automatically.</span>
<label for="resolution">Album art resolution</label>
<select id="resolution">
<option value="80">80 (Fastest, blurry)</option>
<option value="256">256 (Recommended)</option>
<option value="512">512 (Slow download)</option>
</select>
<label for="scroll-repeats">Long titles</label>
<select id="scroll-repeats">
<option value="0">Off (shorten with ...)</option>
<option value="1">Scroll once</option>
<option value="2">Scroll twice</option>
<option value="3">Scroll 3 times</option>
<option value="99">Keep scrolling</option>
</select>
<label for="scroll-speed">Scroll speed</label>
<select id="scroll-speed">
<option value="20">Slow</option>
<option value="35">Normal</option>
<option value="55">Fast</option>
</select>
<label for="led">RGB LED brightness: <span id="led-val"></span>%</label>
<input type="range" id="led" min="0" max="100" step="5" style="width:100%;accent-color:#18bcf2;margin-bottom:12px">
<div class="row" style="margin-bottom:0"><label style="margin:0"><input type="checkbox" id="nerd"> Nerd mode: show technical track info (format, bitrate, genre, ...) in the track popup on the display</label></div>
</div>

<div class="section" data-tab="playlists" hidden>
<h3>Playlists</h3>
<span class="hint">Pinned playlists are shown first on the display. Pin playlists from your Music Assistant library to add more.</span>
<div id="pins"></div>

<h4>My playlists</h4>
<input type="text" id="pl-search" placeholder="Search my playlists" autocapitalize="none" autocorrect="off" style="margin-bottom:0">
<div class="row" style="margin:10px 0 0">
<button type="button" class="small" id="load-playlists">Refresh playlists</button>
<span id="pl-progress"></span>
</div>
<div id="pl-results"></div>
</div>

<div class="section" data-tab="security" hidden>
<h3>Web password</h3>
<span class="hint" id="pw-hint"></span>
<form id="pw-form">
<label for="pw1">New password (4-64 characters)</label>
<input type="password" id="pw1" minlength="4" maxlength="64" autocomplete="new-password" required>
<label for="pw2">Repeat password</label>
<input type="password" id="pw2" minlength="4" maxlength="64" autocomplete="new-password" required>
<button type="submit">Set password</button>
</form>
<button type="button" class="small" id="pw-clear" style="margin-top:12px" hidden>Remove password</button>
<div class="msg" id="pw-msg"></div>
</div>

<div class="section" data-tab="debug" hidden>
<h3>Debug Log</h3>
<div class="row">
<label style="margin:0"><input type="checkbox" id="debug"> Verbose logging</label>
<button type="button" class="small" id="copy-log">Copy</button>
<button type="button" class="small" id="clear-log">Clear</button>
</div>
<pre id="log"></pre>
</div>

<script>
const $=id=>document.getElementById(id);
const msg=t=>$('msg').textContent=t;

fetch('/status').then(r=>r.json()).then(s=>{
  $('resolution').value=s.art;
  $('scroll-repeats').value=s.scroll_repeats;
  const sp=[20,35,55].reduce((a,b)=>Math.abs(b-s.scroll_speed)<Math.abs(a-s.scroll_speed)?b:a);
  $('scroll-speed').value=sp;
  $('nerd').checked=!!s.nerd;
  $('led').value=s.led;
  $('led-val').textContent=s.led;
  $('server').value=s.server;
  $('token').placeholder=s.configured?'Saved. Leave empty to keep it.':'';
  $('pw-hint').textContent=s.protected
    ?'This device is password protected. Your browser asks for it; the username can be anything, for example admin. Forgot it? Hold the BOOT button on the board for 3 seconds.'
    :'No password is set: anyone on your network can change these settings.';
  $('pw-clear').hidden=!s.protected;
  $('pw1').required=true;
  if(!s.ap){
    document.querySelectorAll('.sta-only').forEach(e=>e.hidden=false);
    if(s.configured){
      msg('Connected to Music Assistant.');
      $('srv').open=false;
      $('srv-state').textContent='- connected to '+s.server;
    }
  }
  const allowed=[...document.querySelectorAll('#tabs button:not([hidden])')].map(b=>b.dataset.go);
  const wanted=location.hash.slice(1);
  showTab(allowed.includes(wanted)?wanted:(s.ap?'wifi':(s.configured?'playlists':'server')));
});

function showTab(name){
  document.querySelectorAll('[data-tab]').forEach(e=>e.hidden=e.dataset.tab!==name);
  document.querySelectorAll('#tabs button').forEach(b=>b.classList.toggle('active',b.dataset.go===name));
  history.replaceState(null,'','#'+name);
  clearInterval(logTimer);
  if(name==='debug'){loadLog();logTimer=setInterval(loadLog,3000);}
  if(name==='playlists')ensurePlaylists();
  if(name==='server')loadPlayers();
  if(name==='wifi')loadWifi();
}
document.querySelectorAll('#tabs button').forEach(b=>b.addEventListener('click',()=>showTab(b.dataset.go)));

async function poll(){
  const s=await (await fetch('/status')).json();
  if(s.busy){setTimeout(poll,1000);return;}
  if(s.ha_url){msg('Opening Home Assistant sign-in...');location.href=s.ha_url;return;}
  if(s.error){msg('Error: '+s.error);return;}
  msg('Connected to Music Assistant. The player is ready.');
  $('token').value='';
  $('mapass').value='';
  loadPlayers();
}

$('ha-login').addEventListener('click',async()=>{
  msg('Contacting Music Assistant...');
  const r=await fetch('/ha-start',{method:'POST',body:new URLSearchParams({server:$('server').value})});
  if(!r.ok){msg(await r.text());return;}
  poll();
});

if(location.search.includes('ha=1'))poll();

$('server-form').addEventListener('submit',async e=>{
  e.preventDefault();
  msg('Connecting...');
  const r=await fetch('/save-server',{method:'POST',body:new URLSearchParams(new FormData(e.target))});
  if(!r.ok){msg(await r.text());return;}
  poll();
});

async function loadPlayers(){
  const box=$('player-msg');
  box.textContent='Loading players...';
  try{
    const j=await pollWeb('/api/players?');
    const sel=$('player');
    sel.textContent='';
    const none=document.createElement('option');
    none.value='';
    none.textContent='Select a player...';
    sel.appendChild(none);
    j.items.forEach(p=>{
      const o=document.createElement('option');
      o.value=p.id;
      o.textContent=p.name;
      o.selected=p.active;
      sel.appendChild(o);
    });
    box.textContent=j.items.length?'':'No players found.';
  }catch(e){
    box.textContent='Error: '+e.message;
  }
}
$('player').addEventListener('change',async e=>{
  const o=e.target.selectedOptions[0];
  if(!o||!o.value)return;
  const r=await postForm('/select-player',{id:o.value,name:o.textContent});
  $('player-msg').textContent=r.ok?'Saved.':await r.text();
});

const dmsg=t=>$('display-msg').textContent=t;
$('resolution').addEventListener('change',async()=>{
  const r=await postForm('/save-art',{resolution:$('resolution').value});
  dmsg(r.ok?'Saved. The new size is used from the next poll.':await r.text());
});

const sleep=ms=>new Promise(r=>setTimeout(r,ms));
const saveScroll=async()=>{
  const r=await postForm('/save-scroll',{repeats:$('scroll-repeats').value,speed:$('scroll-speed').value});
  dmsg(r.ok?'Saved. Scrolling applies from the next track.':await r.text());
};
$('scroll-repeats').addEventListener('change',saveScroll);
$('scroll-speed').addEventListener('change',saveScroll);
$('nerd').addEventListener('change',async e=>{
  const r=await postForm('/save-nerd',{enabled:e.target.checked?'1':'0'});
  dmsg(r.ok?'Saved. Reopen the track popup on the display.':await r.text());
});
let ledTimer=null;
$('led').addEventListener('input',e=>{
  $('led-val').textContent=e.target.value;
  clearTimeout(ledTimer);
  ledTimer=setTimeout(()=>postForm('/save-led',{value:e.target.value}),120);
});
$('led').addEventListener('change',e=>{
  clearTimeout(ledTimer);
  postForm('/save-led',{value:e.target.value,save:'1'});
});
$('pw-form').addEventListener('submit',async e=>{
  e.preventDefault();
  const box=$('pw-msg');
  if($('pw1').value!==$('pw2').value){box.textContent='The passwords do not match.';return;}
  const r=await postForm('/save-password',{password:$('pw1').value});
  if(!r.ok){box.textContent=await r.text();return;}
  box.textContent='Password set. Your browser will now ask for it.';
  setTimeout(()=>location.reload(),1200);
});
$('pw-clear').addEventListener('click',async()=>{
  if(!confirm('Remove the web password? Anyone on your network can then change the settings.'))return;
  await postForm('/clear-password',{});
  setTimeout(()=>location.reload(),600);
});
$('wifi-reset').addEventListener('click',async()=>{
  if(!confirm('Forget all saved networks and restart in setup mode?'))return;
  await postForm('/wifi-reset',{});
  $('wifi-msg').textContent='Restarting. Connect to the MusicAssistant-Control-Setup network and open http://192.168.4.1';
});
async function loadWifi(){
  const j=await (await fetch('/api/wifi')).json();
  const box=$('wifi-list');
  box.textContent='';
  if(!j.networks.length)box.textContent='No saved networks.';
  j.networks.forEach(n=>{
    const isCurrent=n.ssid===j.current;
    const row=makeRow(n.ssid+(isCurrent?(j.internet?' (connected)':' (connected, no internet)'):''));
    row.appendChild(makeButton(isCurrent?'Forget & disconnect':'Remove',async()=>{
      if(!confirm((isCurrent?'Forget and disconnect from ':'Remove ')+n.ssid+'?'))return;
      await postForm('/wifi-remove',{ssid:n.ssid});
      if(isCurrent)$('wifi-msg').textContent='Restarting. The device joins another saved network or starts the MusicAssistant-Control'-Setup network.';
      else loadWifi();
    }));
    box.appendChild(row);
  });
  $('wifi-warn').textContent=(j.current&&!j.internet)?'Connected to '+j.current+' but there is no internet access. Forget it to move on to another network.':'';
}
let pinned=[];
let allPlaylists=[];
let playlistsLoaded=false;
let loadingPlaylists=false;
const CACHE_KEY='ma-playlists-v1';
let logTimer=null;

function makeButton(text,onclick,disabled){
  const b=document.createElement('button');
  b.type='button';
  b.textContent=text;
  b.disabled=!!disabled;
  b.addEventListener('click',onclick);
  return b;
}
function makeRow(name){
  const row=document.createElement('div');
  row.className='pl';
  const label=document.createElement('span');
  label.textContent=name;
  row.appendChild(label);
  return row;
}
async function postForm(url,data){
  return fetch(url,{method:'POST',body:new URLSearchParams(data)});
}
function renderPins(){
  const box=$('pins');
  box.textContent='';
  if(!pinned.length){box.textContent='No pinned playlists yet.';return;}
  pinned.forEach((p,i)=>{
    const row=makeRow(p.name);
    if(i>0)row.appendChild(makeButton('Up',async()=>{await postForm('/pin-up',{id:p.id});loadPins();}));
    row.appendChild(makeButton('Rename',async()=>{
      const name=prompt('Name for this playlist',p.name);
      if(name&&name.trim()){await postForm('/pin-rename',{id:p.id,name:name.trim()});loadPins();}
    }));
    row.appendChild(makeButton('Remove',async()=>{await postForm('/pin-remove',{id:p.id});loadPins();}));
    box.appendChild(row);
  });
}
function renderResults(){
  const box=$('pl-results');
  box.textContent='';
  const q=$('pl-search').value.trim().toLowerCase();
  const ids=new Set(pinned.map(p=>p.id));
  allPlaylists.filter(p=>!q||p.name.toLowerCase().includes(q)||(p.owner||'').toLowerCase().includes(q)).slice(0,60).forEach(p=>{
    const row=makeRow(p.owner?p.name+' - '+p.owner:p.name);
    const isPinned=ids.has(p.id);
    row.appendChild(makeButton(isPinned?'Pinned':'Pin',async()=>{
      const r=await postForm('/pin-add',{id:p.id,name:p.name});
      if(!r.ok)$('pl-progress').textContent=await r.text();
      loadPins();
    },isPinned));
    box.appendChild(row);
  });
}
async function loadPins(){
  const j=await (await fetch('/api/pins')).json();
  pinned=j.pins;
  renderPins();
  renderResults();
}
async function pollWeb(url){
  for(let tries=0;tries<40;tries++){
    const j=await (await fetch(url+(tries===0?'&fresh=1':''))).json();
    if(j.status==='ready')return j;
    if(j.status==='error')throw new Error(j.error);
    await sleep(700);
  }
  throw new Error('Timed out waiting for Music Assistant');
}
const fetchPage=offset=>pollWeb('/api/playlists?offset='+offset);

async function loadPlaylists(){
  if(loadingPlaylists)return;
  loadingPlaylists=true;
  const btn=$('load-playlists');
  const progress=$('pl-progress');
  btn.disabled=true;
  allPlaylists=[];
  let complete=false;
  try{
    let offset=0,total=1;
    while(offset<total){
      const page=await fetchPage(offset);
      allPlaylists.push(...page.items);
      total=page.total;
      progress.textContent='Loaded '+allPlaylists.length;
      renderResults();
      if(page.next<=offset)break;
      offset=page.next;
    }
    complete=true;
    progress.textContent=allPlaylists.length+' playlists loaded.';
  }catch(e){
    progress.textContent='Error: '+e.message;
  }
  if(complete){
    playlistsLoaded=true;
    try{localStorage.setItem(CACHE_KEY,JSON.stringify({time:Date.now(),items:allPlaylists}));}catch(e){}
  }
  btn.disabled=false;
  loadingPlaylists=false;
}
async function ensurePlaylists(){
  if(playlistsLoaded||loadingPlaylists)return;
  try{
    const c=JSON.parse(localStorage.getItem(CACHE_KEY));
    if(c&&Array.isArray(c.items)){
      allPlaylists=c.items;
      playlistsLoaded=true;
      $('pl-progress').textContent=allPlaylists.length+' playlists (cached '+new Date(c.time).toLocaleString()+')';
      renderResults();
      return;
    }
  }catch(e){}
  await loadPlaylists();
}
$('load-playlists').addEventListener('click',loadPlaylists);
$('pl-search').addEventListener('input',renderResults);
loadPins();

async function loadLog(){
  const j=await (await fetch('/log')).json();
  $('debug').checked=j.debug;
  logText=j.entries.map(e=>e.t+'s ['+e.l+'] '+e.m).join('\n');
  const box=$('log');
  box.textContent='';
  if(!j.entries.length){box.textContent='No entries.';return;}
  for(const e of j.entries){
    const line=document.createElement('div');
    line.className=e.l;
    line.textContent=e.t+'s ['+e.l+'] '+e.m;
    box.appendChild(line);
  }
}
let logText='';
$('copy-log').addEventListener('click',async()=>{
  let ok=false;
  // The clipboard API is unavailable on plain http, so fall back to execCommand.
  if(navigator.clipboard&&window.isSecureContext){
    try{await navigator.clipboard.writeText(logText);ok=true;}catch(e){}
  }
  if(!ok){
    const ta=document.createElement('textarea');
    ta.value=logText;
    ta.style.position='fixed';
    ta.style.opacity='0';
    document.body.appendChild(ta);
    ta.select();
    ok=document.execCommand('copy');
    document.body.removeChild(ta);
  }
  const b=$('copy-log');
  b.textContent=ok?'Copied':'Copy failed';
  setTimeout(()=>b.textContent='Copy',1500);
});
$('debug').addEventListener('change',async e=>{
  await fetch('/debug',{method:'POST',body:new URLSearchParams({enabled:e.target.checked?'1':'0'})});
});
$('clear-log').addEventListener('click',async()=>{await fetch('/log-clear',{method:'POST'});loadLog();});
</script>
)rawliteral";

AsyncWebServer server(80);

struct PagePart {
    const char *data;
    size_t length;
};

// Streams the page straight from flash so the 13 KB document is never copied into heap.
size_t indexChunk(uint8_t *buffer, size_t maxLen, size_t index) {
    static const PagePart parts[] = {
        {PAGE_TOP, sizeof(PAGE_TOP) - 1},
        {INDEX_BODY, sizeof(INDEX_BODY) - 1},
        {PAGE_BOTTOM, sizeof(PAGE_BOTTOM) - 1},
    };
    size_t pos = index;
    size_t written = 0;
    for (const PagePart &part : parts) {
        if (pos >= part.length) {
            pos -= part.length;
            continue;
        }
        size_t n = min(maxLen - written, part.length - pos);
        memcpy(buffer + written, part.data + pos, n);
        written += n;
        pos = 0;
        if (written == maxLen) break;
    }
    return written;
}

String pendingServer;
String pendingToken;
String pendingUser;
String pendingPass;
String pendingReturn;
String haServer;
String haUrl;
String lastError;
std::atomic<bool> hasPending{false};
std::atomic<bool> busy{false};
std::atomic<uint32_t> restartAt{0};

String page(const char *body) {
    String out;
    out.reserve(strlen_P(PAGE_TOP) + strlen_P(body) + 64);
    out += FPSTR(PAGE_TOP);
    out += FPSTR(body);
    out += FPSTR(PAGE_BOTTOM);
    return out;
}

bool isSafeUri(const String &s, size_t maxLen) {
    if (s.isEmpty() || s.length() > maxLen) return false;
    for (size_t i = 0; i < s.length(); i++) {
        if ((unsigned char)s[i] < 0x20) return false;
    }
    return true;
}

void handleStatus(AsyncWebServerRequest *request) {
    JsonDocument doc;
    doc["ap"] = WiFi.getMode() == WIFI_AP;
    doc["busy"] = busy.load();
    doc["configured"] = ma_configured();
    doc["art"] = g_config.artSize;
    doc["scroll_repeats"] = g_config.scrollRepeats;
    doc["scroll_speed"] = g_config.scrollSpeed;
    doc["nerd"] = g_config.nerdMode;
    doc["led"] = g_config.ledBrightness;
    doc["server"] = g_config.maHost.length()
        ? String(g_config.maTls ? "https://" : "") + g_config.maHost + ((g_config.maTls ? g_config.maPort != 443 : g_config.maPort != 80) ? ":" + String(g_config.maPort) : String()) + g_config.maPath
        : String();
    doc["ha_url"] = haUrl;
    haUrl = "";
    doc["protected"] = config_web_password_set();
    doc["ssid"] = WiFi.status() == WL_CONNECTED ? WiFi.SSID() : String();
    doc["error"] = lastError;
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void handleWifiReset(AsyncWebServerRequest *request) {
    config_clear_networks();
    request->send(200, "text/plain", "OK");
    restartAt = millis() + 1500;
}

void handleApiWifi(AsyncWebServerRequest *request) {
    JsonDocument doc;
    bool connected = WiFi.status() == WL_CONNECTED;
    doc["current"] = connected ? WiFi.SSID() : String();
    doc["internet"] = g_internetOk;
    JsonArray networks = doc["networks"].to<JsonArray>();
    for (const WifiNet &net : config_get_networks()) {
        JsonObject o = networks.add<JsonObject>();
        o["ssid"] = net.ssid;
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

void handleWifiRemove(AsyncWebServerRequest *request) {
    if (!request->hasParam("ssid", true)) {
        request->send(400, "text/plain", "SSID is missing.");
        return;
    }
    String ssid = request->getParam("ssid", true)->value();
    bool wasCurrent = WiFi.status() == WL_CONNECTED && WiFi.SSID() == ssid;
    config_remove_network(ssid);
    request->send(200, "text/plain", "OK");
    if (wasCurrent) restartAt = millis() + 1500;
}

void handleSaveWifi(AsyncWebServerRequest *request) {
    if (!request->hasParam("ssid", true) || request->getParam("ssid", true)->value().isEmpty()) {
        request->send(400, "text/plain", "SSID is missing.");
        return;
    }
    config_add_network(request->getParam("ssid", true)->value(), request->hasParam("password", true) ? request->getParam("password", true)->value() : "");

    static const char body[] PROGMEM =
        "<h2>Wi-Fi saved</h2>"
        "<p>The device is restarting. Reconnect your phone or computer to your own network, "
        "then open the new address shown on the device display to finish the Music Assistant setup.</p>";
    request->send(200, "text/html", page(body));
    restartAt = millis() + 1500;
}

void handleLog(AsyncWebServerRequest *request) {
    request->send(200, "application/json", log_to_json());
}

void handleDebug(AsyncWebServerRequest *request) {
    bool on = request->hasParam("enabled", true) && request->getParam("enabled", true)->value() == "1";
    log_set_debug(on);
    app_log(LL_INFO, "Verbose logging %s", on ? "enabled" : "disabled");
    request->send(200, "text/plain", "OK");
}

void handleLogClear(AsyncWebServerRequest *request) {
    log_clear();
    request->send(200, "text/plain", "OK");
}

void handlePins(AsyncWebServerRequest *request) {
    JsonDocument doc;
    JsonArray pins = doc["pins"].to<JsonArray>();
    for (const Pin &pin : config_get_pins()) {
        JsonObject o = pins.add<JsonObject>();
        o["id"] = pin.id;
        o["name"] = pin.name;
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

bool pinIdParam(AsyncWebServerRequest *request, String &id) {
    if (!request->hasParam("id", true)) return false;
    id = request->getParam("id", true)->value();
    return isSafeUri(id, 120);
}

void handlePinAdd(AsyncWebServerRequest *request) {
    String id;
    if (!pinIdParam(request, id) || !request->hasParam("name", true)) {
        request->send(400, "text/plain", "Invalid playlist.");
        return;
    }
    String name = request->getParam("name", true)->value();
    name.trim();
    if (name.length() > 80) name = name.substring(0, 80);
    if (!config_add_pin(id, name)) {
        request->send(409, "text/plain", "Pin limit reached (" + String(MAX_PINS) + "). Remove one first.");
        return;
    }
    request->send(200, "text/plain", "OK");
}

void handlePinRemove(AsyncWebServerRequest *request) {
    String id;
    if (!pinIdParam(request, id)) {
        request->send(400, "text/plain", "Invalid playlist.");
        return;
    }
    config_remove_pin(id);
    request->send(200, "text/plain", "OK");
}

void handlePinRename(AsyncWebServerRequest *request) {
    String id;
    if (!pinIdParam(request, id) || !request->hasParam("name", true)) {
        request->send(400, "text/plain", "Invalid playlist.");
        return;
    }
    String name = request->getParam("name", true)->value();
    name.trim();
    if (name.isEmpty()) {
        request->send(400, "text/plain", "Name cannot be empty.");
        return;
    }
    if (name.length() > 80) name = name.substring(0, 80);
    config_rename_pin(id, name);
    request->send(200, "text/plain", "OK");
}

void handlePinUp(AsyncWebServerRequest *request) {
    String id;
    if (!pinIdParam(request, id)) {
        request->send(400, "text/plain", "Invalid playlist.");
        return;
    }
    config_move_pin_up(id);
    request->send(200, "text/plain", "OK");
}

void respondWebList(AsyncWebServerRequest *request, ListKind kind, uint32_t offset) {
    JsonDocument doc;
    if (!ma_configured()) {
        doc["status"] = "error";
        doc["error"] = "Music Assistant is not connected.";
    } else {
        String key = String(offset);
        bool fresh = request->hasParam("fresh");

        ListData data;
        ma_get_list(kind, data);
        bool same = data.key == key;
        if (same && data.status == LIST_LOADING) {
            doc["status"] = "loading";
        } else if (same && !fresh && (data.status == LIST_READY || data.status == LIST_ERROR)) {
            if (data.status == LIST_READY) {
                doc["status"] = "ready";
                doc["next"] = data.next;
                doc["total"] = data.total;
                JsonArray items = doc["items"].to<JsonArray>();
                for (const ListItem &item : data.items) {
                    JsonObject o = items.add<JsonObject>();
                    o["id"] = item.id;
                    o["name"] = item.name;
                    o["owner"] = item.owner;
                    o["active"] = item.active;
                }
            } else {
                doc["status"] = "error";
                doc["error"] = data.error;
            }
        } else {
            ma_request_list(kind, offset);
            doc["status"] = "loading";
        }
    }
    String out;
    serializeJson(doc, out);
    request->send(200, "application/json", out);
}

uint32_t offsetParam(AsyncWebServerRequest *request, long maxValue) {
    return request->hasParam("offset") ? (uint32_t)constrain(request->getParam("offset")->value().toInt(), 0, maxValue) : 0;
}

void handleWebPlaylists(AsyncWebServerRequest *request) {
    respondWebList(request, LIST_WEB_PLAYLISTS, offsetParam(request, 100000));
}

void handleWebPlayers(AsyncWebServerRequest *request) {
    respondWebList(request, LIST_WEB_PLAYERS, 0);
}


void handleSelectPlayer(AsyncWebServerRequest *request) {
    if (!request->hasParam("id", true) || !isSafeUri(request->getParam("id", true)->value(), 120)) {
        request->send(400, "text/plain", "Invalid player.");
        return;
    }
    String name = request->hasParam("name", true) ? request->getParam("name", true)->value() : "";
    if (name.length() > 80) name = name.substring(0, 80);
    ma_select(LIST_WEB_PLAYERS, request->getParam("id", true)->value(), name);
    request->send(200, "text/plain", "OK");
}

void handleSaveLed(AsyncWebServerRequest *request) {
    if (!request->hasParam("value", true)) {
        request->send(400, "text/plain", "Missing value.");
        return;
    }
    int value = constrain(request->getParam("value", true)->value().toInt(), 0, 100);
    g_config.ledBrightness = value;
    led_set_brightness(value);
    if (request->hasParam("save", true)) config_save();
    request->send(200, "text/plain", "OK");
}

void handleSavePassword(AsyncWebServerRequest *request) {
    String password = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";
    if (password.length() < 4 || password.length() > 64) {
        request->send(400, "text/plain", "The password must be 4-64 characters.");
        return;
    }
    config_set_web_password(password);
    request->send(200, "text/plain", "OK");
}

void handleClearPassword(AsyncWebServerRequest *request) {
    config_clear_web_password();
    request->send(200, "text/plain", "OK");
}

uint8_t authFailures = 0;
uint32_t authWindowStart = 0;
uint32_t authLockedUntil = 0;

// HTTP Basic auth: any username, the stored password is checked.
void requirePassword(AsyncWebServerRequest *request, ArMiddlewareNext next) {
    if (!config_web_password_set()) {
        next();
        return;
    }
    if ((int32_t)(millis() - authLockedUntil) < 0) {
        request->send(429, "text/plain", "Too many attempts. Try again in a few seconds.");
        return;
    }

    bool hasHeader = request->hasHeader("Authorization");
    if (hasHeader) {
        String header = request->header("Authorization");
        if (header.startsWith("Basic ")) {
            String encoded = header.substring(6);
            unsigned char decoded[128];
            size_t length = 0;
            if (encoded.length() < 160 &&
                mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &length, (const unsigned char *)encoded.c_str(), encoded.length()) == 0) {
                decoded[length] = 0;
                const char *colon = strchr((const char *)decoded, ':');
                if (colon && config_check_web_password(String(colon + 1))) {
                    authFailures = 0;
                    next();
                    return;
                }
            }
        }
        // A browser fires several requests at once, so only a burst of failures counts as guessing.
        uint32_t now = millis();
        if (authFailures == 0 || now - authWindowStart > 10000) {
            authFailures = 0;
            authWindowStart = now;
        }
        if (++authFailures > 12) {
            authFailures = 0;
            authLockedUntil = now + 30000;
        }
    }
    request->requestAuthentication(AsyncAuthType::AUTH_BASIC, "Music Assistant Player");
}

void handleSaveArt(AsyncWebServerRequest *request) {
    int art = request->hasParam("resolution", true) ? request->getParam("resolution", true)->value().toInt() : 0;
    if (art != 80 && art != 256 && art != 512) {
        request->send(400, "text/plain", "Invalid resolution.");
        return;
    }
    g_config.artSize = art;
    config_save();
    request->send(200, "text/plain", "OK");
}

void handleSaveNerd(AsyncWebServerRequest *request) {
    g_config.nerdMode = request->hasParam("enabled", true) && request->getParam("enabled", true)->value() == "1";
    config_save();
    request->send(200, "text/plain", "OK");
}

void handleSaveScroll(AsyncWebServerRequest *request) {
    int repeats = request->hasParam("repeats", true) ? request->getParam("repeats", true)->value().toInt() : -1;
    int speed = request->hasParam("speed", true) ? request->getParam("speed", true)->value().toInt() : 0;
    if (!((repeats >= 0 && repeats <= 5) || repeats == 99) || speed < 15 || speed > 80) {
        request->send(400, "text/plain", "Invalid scroll settings.");
        return;
    }
    g_config.scrollRepeats = repeats;
    g_config.scrollSpeed = speed;
    config_save();
    request->send(200, "text/plain", "OK");
}

void handleSaveServer(AsyncWebServerRequest *request) {
    if (WiFi.status() != WL_CONNECTED) {
        request->send(409, "text/plain", "Connect the device to Wi-Fi first.");
        return;
    }
    if (busy) {
        request->send(409, "text/plain", "Setup already in progress.");
        return;
    }
    auto param = [&](const char *n) {
        String v = request->hasParam(n, true) ? request->getParam(n, true)->value() : "";
        v.trim();
        return v;
    };
    String srv = param("server");
    String token = param("token");
    String user = param("username");
    String pass = request->hasParam("password", true) ? request->getParam("password", true)->value() : "";
    if (srv.isEmpty() || srv.length() > 100 || (token.isEmpty() && (user.isEmpty() || pass.isEmpty())) ||
        token.length() > 1024 || user.length() > 64 || pass.length() > 128) {
        request->send(400, "text/plain", "Enter the server and a token, or a username and password.");
        return;
    }
    pendingServer = srv;
    pendingToken = token;
    pendingUser = user;
    pendingPass = pass;
    pendingReturn = "";
    lastError = "";
    busy = true;
    hasPending = true;
    request->send(200, "text/plain", "OK");
}

void handleHaStart(AsyncWebServerRequest *request) {
    if (WiFi.status() != WL_CONNECTED) {
        request->send(409, "text/plain", "Connect the device to Wi-Fi first.");
        return;
    }
    String srv = request->hasParam("server", true) ? request->getParam("server", true)->value() : "";
    srv.trim();
    if (busy || srv.isEmpty() || srv.length() > 150) {
        request->send(400, "text/plain", busy ? "Setup already in progress." : "Enter the server address first.");
        return;
    }
    haServer = srv;
    pendingServer = srv;
    pendingToken = "";
    pendingUser = "";
    pendingPass = "";
    pendingReturn = "http://" + WiFi.localIP().toString() + "/ma-callback";
    haUrl = "";
    lastError = "";
    busy = true;
    hasPending = true;
    request->send(200, "text/plain", "OK");
}

void handleMaCallback(AsyncWebServerRequest *request) {
    String code = request->hasParam("code") ? request->getParam("code")->value() : "";
    bool valid = !code.isEmpty() && code.length() <= 1024 && !haServer.isEmpty();
    for (size_t i = 0; valid && i < code.length(); i++) {
        char c = code[i];
        valid = isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.';
    }
    if (!valid || busy) {
        request->send(400, "text/plain", "Invalid or expired sign-in. Start the Home Assistant sign-in again from the device page.");
        return;
    }
    pendingServer = haServer;
    pendingToken = code;
    pendingUser = "";
    pendingPass = "";
    pendingReturn = "";
    lastError = "";
    busy = true;
    hasPending = true;
    request->redirect("/?ha=1#server");
}

} // namespace

void web_portal_begin() {
    server.addMiddleware(requirePassword);
    server.on("/", HTTP_GET, [](AsyncWebServerRequest *request) {
        request->send(request->beginChunkedResponse("text/html", indexChunk));
    });
    server.on("/status", HTTP_GET, handleStatus);
    server.on("/save-wifi", HTTP_POST, handleSaveWifi);
    server.on("/wifi-reset", HTTP_POST, handleWifiReset);
    server.on("/wifi-remove", HTTP_POST, handleWifiRemove);
    server.on("/api/wifi", HTTP_GET, handleApiWifi);
    server.on("/save-art", HTTP_POST, handleSaveArt);
    server.on("/save-scroll", HTTP_POST, handleSaveScroll);
    server.on("/save-password", HTTP_POST, handleSavePassword);
    server.on("/clear-password", HTTP_POST, handleClearPassword);
    server.on("/save-led", HTTP_POST, handleSaveLed);
    server.on("/api/pins", HTTP_GET, handlePins);
    server.on("/api/playlists", HTTP_GET, handleWebPlaylists);
    server.on("/api/players", HTTP_GET, handleWebPlayers);
    server.on("/save-nerd", HTTP_POST, handleSaveNerd);
    server.on("/select-player", HTTP_POST, handleSelectPlayer);
    server.on("/pin-add", HTTP_POST, handlePinAdd);
    server.on("/pin-remove", HTTP_POST, handlePinRemove);
    server.on("/pin-up", HTTP_POST, handlePinUp);
    server.on("/pin-rename", HTTP_POST, handlePinRename);
    server.on("/log", HTTP_GET, handleLog);
    server.on("/debug", HTTP_POST, handleDebug);
    server.on("/log-clear", HTTP_POST, handleLogClear);
    server.on("/save-server", HTTP_POST, handleSaveServer);
    server.on("/ha-start", HTTP_POST, handleHaStart);
    server.on("/ma-callback", HTTP_GET, handleMaCallback);
    server.begin();
}

void web_portal_loop() {
    uint32_t t = restartAt;
    if (t && (int32_t)(millis() - t) >= 0) ESP.restart();
}

bool web_portal_take_setup(MaSetupRequest &req) {
    if (!hasPending.exchange(false)) return false;
    req.server = pendingServer;
    req.token = pendingToken;
    req.username = pendingUser;
    req.password = pendingPass;
    req.returnUrl = pendingReturn;
    return true;
}

void web_portal_set_ha_url(const String &url) {
    haUrl = url;
    lastError = "";
    busy = false;
}

void web_portal_set_result(bool ok, const String &error) {
    lastError = ok ? "" : error;
    busy = false;
}
