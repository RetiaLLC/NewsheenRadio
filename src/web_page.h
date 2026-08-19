// The whole control surface as one PROGMEM page. It talks to the firmware over
// a small JSON API rather than having the device render HTML, which keeps the
// C++ side to data and avoids rebuilding strings on every request.

#pragma once
#include <Arduino.h>

static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html>
<html><head><meta charset=utf-8><meta name=viewport content="width=device-width,initial-scale=1">
<title>Newsheen Radio</title><style>
:root{--bg:#141018;--card:#1e1826;--line:#33283d;--fg:#f0e6f5;--dim:#a595b0;--accent:#7c4dff;--accent2:#c9a7e0}
*{box-sizing:border-box}
body{background:var(--bg);color:var(--fg);font:16px/1.5 system-ui,-apple-system,sans-serif;margin:0;padding:clamp(14px,2.2vw,28px);max-width:1280px;margin-inline:auto}
h1{font-size:1.3rem;margin:0 0 2px}
.sub{color:var(--dim);font-size:.85rem;margin:0 0 16px}
nav{display:flex;gap:6px;margin-bottom:16px;flex-wrap:wrap}
nav button{flex:1;min-width:80px;background:var(--card);border:1px solid var(--line);color:var(--dim);border-radius:10px;padding:9px 6px;font:inherit;font-size:.85rem;cursor:pointer}
nav button.on{background:var(--accent);border-color:var(--accent);color:#fff}
section{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:16px;margin-bottom:14px}
h2{font-size:.75rem;text-transform:uppercase;letter-spacing:.08em;color:var(--accent2);margin:0 0 12px}
button,input[type=submit]{background:var(--accent);color:#fff;border:0;border-radius:8px;padding:9px 14px;font:inherit;cursor:pointer}
button.g{background:var(--line)}
button.sm{padding:5px 10px;font-size:.8rem}
input[type=text],input[type=password],input[type=file],select{background:var(--bg);color:var(--fg);border:1px solid #4a3a58;border-radius:8px;padding:9px;font:inherit;width:100%;margin-bottom:8px}
input[type=range]{width:100%}
ul{list-style:none;padding:0;margin:0}
li{display:flex;align-items:center;gap:8px;padding:8px 0;border-top:1px solid #2b2235}
li:first-child{border-top:0}
li .n{flex:1;min-width:0}
li .n b{display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;font-weight:600}
li .n s{display:block;color:var(--dim);font-size:.75rem;text-decoration:none}
.now{background:linear-gradient(135deg,#2a1f38,#1e1826);border-color:#4a3a58}
.now b{font-size:1.05rem;display:block;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.now s{color:var(--accent2);text-decoration:none;font-size:.85rem}
.row{display:flex;gap:8px;align-items:center;flex-wrap:wrap}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(104px,1fr));gap:6px}
.grid button{background:var(--line);font-size:.8rem;padding:9px 4px}
.grid button.on{background:var(--accent)}
.dot{width:8px;height:8px;border-radius:50%;background:#666;flex:0 0 auto}
.dot.ok{background:#4ade80}.dot.no{background:#f87171}
label{font-size:.8rem;color:var(--dim);display:block;margin:10px 0 4px}
code{background:var(--bg);padding:2px 5px;border-radius:4px;font-size:.85em;word-break:break-all}
a{color:var(--accent2)}
.hint{color:var(--dim);font-size:.8rem;margin:8px 0 0}
.zb{width:38px;height:38px;padding:0;font-size:1.25rem;line-height:1;border-radius:10px;
    background:rgba(30,24,38,.86);border:1px solid var(--line);color:var(--fg);
    display:flex;align-items:center;justify-content:center;cursor:pointer}
.zb:hover{background:var(--accent);border-color:var(--accent)}
.wrap{display:grid;gap:16px;grid-template-columns:1fr;align-items:start}
[hidden]{display:none!important}
.side,.main{min-width:0}
@media(min-width:880px){
 .wrap{grid-template-columns:minmax(290px,350px) 1fr;gap:20px}
 .side{position:sticky;top:clamp(14px,2.2vw,28px)}
 nav{flex-direction:column}
 nav button{flex:none;text-align:left;padding:11px 14px;font-size:.9rem}
 h1{font-size:1.6rem}
 .now b{font-size:1.2rem}
}
@media(min-width:1180px){
 /* Multi-column, not grid: cards have very different heights and a grid leaves
    a ragged hole under every short one. Columns flow and stay tight.
    Note the :not([hidden]) — a bare rule here overrides [hidden]{display:none}
    and every tab renders at once. */
 .main>div:not([hidden]){columns:2;column-gap:16px}
 .main section{break-inside:avoid;margin:0 0 16px}
}
</style></head><body>
<h1>Newsheen Radio</h1><p class=sub id=sub>connecting…</p>

<div class=wrap>
<div class=side>
<section class=now><b id=npTitle>—</b><s id=npSub></s>
<div class=row style="margin-top:12px">
<button onclick="api('/api/stop')">Stop</button>
<button class=g onclick="api('/api/sing')">Song</button>
<button class=g onclick="api('/api/say?t=Hello!+I+am+new+sheen.')">Greeting</button>
</div>
<label>Volume <span id=volv></span></label>
<input type=range min=0 max=100 id=vol onchange="api('/api/vol?v='+this.value)">
</section>

<nav>
<button id=tRadio class=on onclick="tab('Radio')">Radio</button>
<button id=tGlobe onclick="tab('Globe')">Globe</button>
<button id=tFx onclick="tab('Fx')">Effects</button>
<button id=tFiles onclick="tab('Files')">Files</button>
<button id=tWifi onclick="tab('Wifi')">Wi-Fi</button>
</nav>
</div>
<div class=main>

<div id=pRadio>
 <section><h2>Find a station</h2>
  <input type=text id=q placeholder="Name, genre or country — e.g. jazz, Iceland, BBC" onkeydown="if(event.key=='Enter')search()">
  <div class=row><button onclick="search()">Search</button><span class=hint id=shint></span></div>
  <ul id=results></ul>
 </section>
 <section><h2>Favourites</h2><ul id=favs><li><span class=n>None yet.</span></li></ul></section>
 <section><h2>Paste a stream URL</h2>
  <input type=text id=url placeholder="http://… or https://…  (.mp3 / .aac)">
  <button onclick="tune(document.getElementById('url').value,'Direct URL')">Tune</button>
  <p class=hint>Also works from radio.garden: drag this to your bookmarks bar, then click it while a station is playing there —
  <a id=bm href="#">Send to Newsheen</a>. Your browser resolves the station (it passes Cloudflare; the puck can't), then hands the puck the stream.</p>
 </section>
</div>

<div id=pGlobe hidden>
 <section><h2>The world</h2>
  <div style="position:relative">
   <canvas id=gl width=760 height=760 style="width:100%;display:block;border-radius:50%;touch-action:none;cursor:grab"></canvas>
   <div style="position:absolute;right:10px;top:10px;display:flex;flex-direction:column;gap:6px">
    <button class=zb onclick="gSetZoom(gZoom*1.45)" title="Zoom in">+</button>
    <button class=zb onclick="gSetZoom(gZoom/1.45)" title="Zoom out">&minus;</button>
    <button class=zb onclick="gSetZoom(1)" title="Reset" style="font-size:.8rem">&#9678;</button>
   </div>
   <div id=gname style="position:absolute;left:50%;bottom:6px;transform:translateX(-50%);background:rgba(10,7,16,.82);border:1px solid var(--line);border-radius:999px;padding:5px 14px;font-size:.82rem;white-space:nowrap;max-width:92%;overflow:hidden;text-overflow:ellipsis;opacity:0;transition:opacity .18s"></div>
  </div>
  <div class=row style="margin-top:12px">
   <select id=gc onchange="globeCountry()" style="flex:1;min-width:150px;margin:0"><option value="">Browse a country…</option></select>
   <input type=text id=gq placeholder="or search…" onkeydown="if(event.key=='Enter')globeSearch()" style="flex:1;min-width:120px;margin:0">
   <button onclick="globeSearch()">Find</button>
  </div>
  <p class=hint id=ghint>Drag to spin, scroll or pinch to zoom, tap a light to tune.</p>
 </section>
</div>

<div id=pFx hidden>
 <section><h2>Visualizers</h2>
  <p class=hint style="margin:0 0 10px">Audio-reactive, designed for the silicone topper: continuous light fields rather than per-pixel patterns, because diffusion erases individual pixels.</p>
  <div class=grid id=vizlist></div></section>
 <section><h2>Effect</h2><div class=grid id=fxlist></div></section>
 <section><h2>Colour &amp; motion</h2>
  <label>Brightness</label><input type=range min=1 max=255 id=bri onchange="fxset()">
  <label>Speed</label><input type=range min=0 max=255 id=spd onchange="fxset()">
  <label>Colour</label>
  <div class=grid id=swatches></div>
 </section>
</div>

<div id=pFiles hidden>
 <section><h2>On the puck</h2><ul id=files></ul></section>
 <section><h2>Upload an MP3</h2>
  <form method=POST action=/upload enctype=multipart/form-data>
  <input type=file name=f accept=.mp3><input type=submit value=Upload></form>
  <p class=hint>Uploading writes one file. Reflashing the whole filesystem is the slow way.</p>
 </section>
</div>

<div id=pWifi hidden>
 <section><h2>Home network</h2>
  <p class=hint style="margin:0 0 10px">Streaming needs internet. The <b>Newsheen-Audio</b> access point stays up either way, so you can always get back in here.</p>
  <input type=text id=ssid placeholder="Network name"><input type=password id=pass placeholder="Password">
  <div class=row><button onclick="joinWifi()">Save &amp; connect</button><button class=g onclick="scan()">Scan</button></div>
  <p class=hint>One radio, two jobs: joining your network moves this access point to that network's channel, so <b>expect to be dropped for a few seconds</b>. Your phone reconnects on its own. Once the header shows an address, you can go back to your normal Wi-Fi and use <code>newsheen.local</code>.</p>
  <p class=hint>2.4 GHz only — a 5 GHz network will not appear in the scan.</p>
  <ul id=nets></ul>
 </section>
</div>

</div>
</div>

<script>
let S={};
const $=id=>document.getElementById(id);
const api=(u)=>fetch(u).then(r=>r.ok?r.text():'').then(()=>setTimeout(poll,250));

function tab(n){
 for(const t of ['Radio','Globe','Fx','Files','Wifi']){$('p'+t).hidden=(t!=n);$('t'+t).className=(t==n?'on':'')}
 if(n=='Files')files(); if(n=='Fx')fx(); if(n=='Globe')globeInit();
}
function esc(s){return (s||'').replace(/[<>&"]/g,c=>({'<':'&lt;','>':'&gt;','&':'&amp;','"':'&quot;'}[c]))}

function poll(){
 fetch('/api/status').then(r=>r.json()).then(s=>{
  S=s;
  $('npTitle').textContent=s.title||'—';
  $('npSub').textContent=s.station||'';
  $('vol').value=Math.round(s.vol*100); $('volv').textContent=Math.round(s.vol*100)+'%';
  const w=s.wifi;
  $('sub').innerHTML=(w.sta?`<span class="dot ok"></span> ${esc(w.ssid)} · ${w.ip} · ${w.rssi} dBm`
                            :`<span class="dot no"></span> no internet — set up Wi-Fi to stream`)
                     +` &nbsp;·&nbsp; AP ${w.apip}`;
  $('bm').href="javascript:(async()=>{const m=location.pathname.match(/listen\\/[^/]+\\/([^/?#]+)/);"
   +"if(!m){alert('Open a station page on radio.garden first.');return}"
   +"const r=await fetch('/api/ara/content/secure/listen/'+m[1]+'/channel.mp3',{redirect:'follow'});"
   +"window.open('http://"+w.ip+"/tune?n='+encodeURIComponent(document.title)+'&u='+encodeURIComponent(r.url),'_blank')})()";
 }).catch(()=>{});
}

function render(list,el,fav){
 el.innerHTML = list.length?'':'<li><span class=n>Nothing here.</span></li>';
 list.forEach(s=>{
  const li=document.createElement('li');
  li.innerHTML=`<span class=n><b>${esc(s.name)}</b><s>${esc(s.codec)} ${s.bitrate||'?'}k${s.country?' · '+esc(s.country):''}</s></span>`;
  const p=document.createElement('button');p.className='sm';p.textContent='Play';
  p.onclick=()=>tune(s.url,s.name);
  li.appendChild(p);
  const f=document.createElement('button');f.className='sm g';f.textContent=fav?'✕':'★';
  f.onclick=()=>{fetch((fav?'/api/fav/del?u=':'/api/fav/add?n='+encodeURIComponent(s.name)
    +'&c='+encodeURIComponent(s.codec)+'&b='+(s.bitrate||0)+'&u=')+encodeURIComponent(s.url)).then(favs)};
  li.appendChild(f);
  el.appendChild(li);
 });
}
function search(){
 const q=$('q').value.trim(); if(!q)return;
 $('shint').textContent='searching…';
 fetch('/api/search?q='+encodeURIComponent(q)).then(r=>r.json()).then(d=>{
  $('shint').textContent=d.length?'':'nothing found';
  render(d,$('results'),false);
 }).catch(e=>$('shint').textContent='search failed');
}
function favs(){fetch('/api/fav').then(r=>r.json()).then(d=>render(d,$('favs'),true))}
function tune(u,n){if(!u)return;api('/api/tune?n='+encodeURIComponent(n||'')+'&u='+encodeURIComponent(u))}

function fx(){
 fetch('/api/fx').then(r=>r.json()).then(d=>{
  $('bri').value=d.bri; $('spd').value=d.speed;
  const g=$('fxlist'), v=$('vizlist'); g.innerHTML=''; v.innerHTML='';
  const split=d.classic||d.names.length;
  d.names.forEach((nm,i)=>{
   const b=document.createElement('button'); b.textContent=nm;
   if(i==d.effect)b.className='on';
   b.onclick=()=>{fetch('/api/fx?effect='+i).then(fx)};
   (i<split?g:v).appendChild(b);
  });
  const sw=$('swatches'); sw.innerHTML='';
  [[255,120,40,'Warm'],[255,255,255,'White'],[255,0,0,'Red'],[0,255,60,'Green'],
   [40,80,255,'Blue'],[255,0,160,'Pink'],[255,200,0,'Amber'],[130,0,255,'Violet']].forEach(c=>{
   const b=document.createElement('button'); b.textContent=c[3];
   b.style.color=`rgb(${c[0]},${c[1]},${c[2]})`;
   b.onclick=()=>fetch(`/api/fx?r=${c[0]}&g=${c[1]}&b=${c[2]}`).then(fx);
   sw.appendChild(b);
  });
 });
}
function fxset(){fetch(`/api/fx?bri=${$('bri').value}&speed=${$('spd').value}`)}

function files(){
 fetch('/api/files').then(r=>r.json()).then(d=>{
  const el=$('files'); el.innerHTML=d.length?'':'<li><span class=n>No MP3s uploaded.</span></li>';
  d.forEach(f=>{
   const li=document.createElement('li');
   li.innerHTML=`<span class=n><b>${esc(f.name)}</b><s>${Math.round(f.size/1024)} KB</s></span>`;
   const p=document.createElement('button');p.className='sm';p.textContent='Play';
   p.onclick=()=>api('/api/play?f='+encodeURIComponent(f.name));li.appendChild(p);
   const x=document.createElement('button');x.className='sm g';x.textContent='✕';
   x.onclick=()=>{if(confirm('Delete '+f.name+'?'))fetch('/api/rm?f='+encodeURIComponent(f.name)).then(files)};
   li.appendChild(x);
   el.appendChild(li);
  });
 });
}
function scan(){
 $('nets').innerHTML='<li><span class=n>scanning…</span></li>';
 fetch('/api/scan').then(r=>r.json()).then(d=>{
  const el=$('nets'); el.innerHTML='';
  d.forEach(n=>{
   const li=document.createElement('li');
   li.innerHTML=`<span class=n><b>${esc(n.ssid)}</b><s>${n.rssi} dBm${n.lock?' · locked':''}</s></span>`;
   const b=document.createElement('button');b.className='sm';b.textContent='Use';
   b.onclick=()=>{$('ssid').value=n.ssid;$('pass').focus()};
   li.appendChild(b); el.appendChild(li);
  });
 });
}
function joinWifi(){
 fetch('/api/wifi?ssid='+encodeURIComponent($('ssid').value)+'&pass='+encodeURIComponent($('pass').value))
  .then(()=>{$('sub').textContent='joining…'; setTimeout(poll,6000)});
}

// --- Globe -------------------------------------------------------------
// Orthographic canvas globe. Natural Earth 110m land (public domain), simplified
// to 46 polygons / 1243 points. Points on the far side are pushed out to the limb
// rather than dropped, so coastlines stay closed shapes at the edge instead of
// tearing open — the thing that made the first attempt look broken.
const LAND=[[1069,769,1141,758,1094,741,1232,729,1232,737,1269,735,1312,707,1322,718,1398,714,1391,724,1404,728,1495,722,1529,708,1590,708,1609,694,1678,695,1695,686,1708,690,1700,696,1704,701,1757,698,1800,689,1800,649,1774,646,1792,623,1736,616,1703,598,1689,605,1635,598,1620,582,1631,576,1621,548,1603,543,1600,532,1585,529,1567,510,1554,553,1559,567,1636,611,1644,625,1601,605,1593,617,1567,614,1542,597,1550,591,1512,587,1513,595,1497,596,1422,590,1351,547,1381,537,1399,541,1413,522,1400,484,1382,463,1348,434,1322,432,1275,397,1294,367,1290,350,1264,343,1261,367,1268,368,1247,381,1253,395,1210,389,1221,404,1216,409,1180,392,1175,387,1189,374,1223,374,1191,349,1219,316,1212,306,1220,298,1216,282,1186,245,1158,227,1107,214,1104,203,1085,217,1058,197,1093,134,1092,116,1051,86,1050,99,1001,134,992,92,1033,48,1042,12,1013,27,1000,64,985,83,983,77,987,114,971,169,953,157,941,160,943,182,914,227,905,228,902,218,869,215,865,201,803,159,798,103,775,79,735,159,726,213,704,208,663,254,574,257,569,269,547,264,515,278,501,301,479,299,508,247,510,260,515,258,517,240,540,241,563,264,568,242,598,223,578,202,576,189,552,172,486,140,434,126,426,167,391,212,384,236,346,280,349,295,339,276,324,298,355,231,368,220,374,186,433,123,427,117,446,104,511,120,510,106,477,42,402,-25,392,-46,388,-64,404,-107,407,-146,394,-167,347,-197,356,-237,325,-257,322,-287,282,-327,257,-339,196,-348,183,-341,182,-316,152,-270,142,-221,117,-180,117,-157,136,-107,119,-50,88,-11,94,37,85,47,59,42,43,62,-19,47,-90,48,-124,72,-166,121,-176,147,-161,181,-169,218,-144,262,-95,299,-93,325,-59,357,-21,351,14,366,95,373,111,369,103,337,190,302,201,322,215,328,289,308,309,315,337,309,360,346,361,366,276,366,261,394,292,412,335,420,383,409,417,419,366,452,391,472,349,462,363,451,338,443,324,453,333,460,307,465,276,425,288,410,263,401,249,409,237,406,239,399,226,402,240,376,231,379,231,364,224,364,194,402,195,417,131,457,123,453,125,440,184,401,168,404,170,389,161,379,154,400,88,443,65,431,31,430,30,418,8,410,1,387,-21,366,-53,359,-65,369,-89,368,-93,430,-13,440,-11,460,-45,486,-16,486,-19,497,13,501,47,530,81,535,88,540,81,555,85,571,105,577,109,564,96,554,109,540,196,544,212,551,215,574,241,570,244,583,233,591,291,600,228,598,213,607,215,631,254,651,239,660,221,657,213,644,178,627,171,613,187,600,168,587,158,561,129,553,103,594,83,583,56,585,49,619,105,644,147,678,245,710,281,711,312,704,300,701,311,695,365,690,410,674,411,667,383,660,331,666,348,659,349,644,370,638,365,647,371,651,395,645,404,647,397,655,420,664,439,660,445,667,434,685,462,682,468,676,455,670,463,666,537,688,544,688,534,682,588,688,599,682,610,689,600,695,605,698,685,680,691,686,669,694,666,710,692,728,725,727,718,714,727,703,725,690,736,684,712,663,724,661,750,677,749,689,736,696,744,706,731,714,748,721,746,728,756,723,752,713,763,711,759,718,775,722,815,717,805,736,868,739,860,744,871,751,1007,764,1043,777,1069,769],[-905,695,-905,684,-892,692,-873,672,-855,687,-855,698,-826,696,-812,691,-819,681,-812,676,-833,664,-857,665,-873,647,-931,620,-946,589,-932,587,-923,570,-822,551,-821,532,-799,512,-786,525,-798,546,-765,565,-785,588,-773,598,-781,623,-738,624,-695,610,-692,589,-676,582,-645,603,-614,569,-618,563,-573,546,-557,532,-556,521,-600,502,-664,502,-711,468,-650,492,-641,487,-651,480,-644,462,-615,458,-605,470,-598,459,-653,435,-661,444,-644,452,-671,451,-706,430,-699,416,-737,409,-719,409,-739,407,-749,389,-755,395,-750,384,-759,372,-763,391,-763,380,-769,382,-757,355,-813,314,-800,268,-803,252,-817,258,-841,300,-891,303,-892,292,-901,291,-938,297,-965,283,-973,273,-978,224,-962,193,-944,181,-920,187,-907,192,-902,210,-870,215,-889,158,-849,160,-834,152,-838,111,-814,87,-795,96,-768,86,-749,110,-717,124,-711,121,-719,114,-717,90,-714,109,-699,121,-681,105,-648,100,-618,107,-623,99,-571,59,-539,57,-513,42,-499,17,-503,0,-486,-2,-485,-12,-478,-5,-449,-15,-445,-26,-399,-28,-356,-51,-347,-73,-351,-90,-386,-130,-392,-178,-409,-219,-476,-248,-488,-286,-538,-344,-562,-348,-584,-339,-567,-369,-592,-387,-623,-388,-627,-410,-651,-410,-649,-420,-634,-425,-651,-435,-655,-450,-672,-455,-675,-463,-656,-472,-659,-481,-691,-507,-681,-523,-708,-529,-710,-538,-749,-522,-756,-486,-741,-469,-756,-466,-743,-441,-732,-444,-727,-423,-743,-432,-732,-392,-735,-371,-714,-324,-701,-197,-714,-173,-760,-146,-797,-71,-812,-61,-814,-47,-797,-26,-809,-22,-809,-10,-771,38,-781,83,-795,89,-808,72,-856,99,-874,133,-912,139,-946,162,-965,156,-1035,182,-1054,199,-1060,227,-1122,289,-1131,311,-1147,318,-1146,301,-1094,233,-1100,228,-1121,247,-1123,260,-1150,277,-1141,285,-1173,330,-1206,346,-1244,403,-1239,455,-1246,481,-1231,480,-1225,471,-1228,490,-1274,508,-1278,523,-1291,527,-1340,581,-1471,608,-1517,591,-1506,612,-1540,593,-1532,588,-1542,581,-1584,559,-1647,544,-1577,575,-1570,589,-1619,586,-1618,596,-1638,598,-1661,615,-1645,631,-1607,637,-1615,644,-1607,647,-1649,644,-1681,656,-1644,665,-1616,661,-1667,683,-1565,713,-1365,689,-1281,704,-1257,694,-1244,701,-1242,694,-1214,698,-1139,684,-1153,679,-1088,673,-1077,678,-1088,683,-1081,686,-1061,688,-1014,676,-984,677,-976,685,-961,682,-961,672,-942,690,-964,700,-963,711,-952,719,-915,701,-924,697,-905,695],[-586,-641,-620,-648,-626,-654,-621,-661,-656,-679,-618,-707,-608,-737,-706,-766,-772,-767,-736,-779,-779,-783,-780,-791,-582,-832,-497,-817,-428,-820,-285,-803,-296,-792,-356,-794,-357,-783,-175,-751,-157,-745,-164,-738,-154,-731,-103,-712,-74,-717,-68,-709,-2,-716,77,-698,108,-708,134,-699,270,-704,319,-696,338,-685,386,-697,545,-658,614,-679,688,-679,696,-692,678,-703,690,-706,679,-718,698,-722,738,-698,776,-694,827,-672,867,-671,879,-662,896,-671,957,-673,997,-672,1028,-655,1061,-669,1136,-658,1198,-672,1347,-662,1350,-653,1374,-669,1454,-669,1488,-683,1542,-685,1615,-705,1712,-717,1692,-736,1660,-743,1635,-762,1647,-781,1670,-787,1617,-791,1597,-809,1694,-838,1800,-847,1800,-900,-1800,-900,-1800,-847,-1790,-841,-1699,-838,-1619,-851,-1485,-856,-1431,-850,-1535,-836,-1528,-820,-1568,-811,-1506,-813,-1464,-803,-1553,-790,-1580,-780,-1583,-768,-1513,-774,-1461,-764,-1462,-753,-1352,-743,-1210,-745,-1139,-737,-1123,-747,-1075,-751,-1001,-748,-1025,-741,-1036,-726,-963,-736,-900,-733,-892,-725,-814,-738,-803,-731,-748,-738,-673,-724,-685,-697,-674,-681,-677,-673,-630,-646,-572,-635,-586,-641],[-271,835,-208,827,-319,822,-220,817,-231,811,-157,819,-122,812,-200,801,-177,801,-197,787,-196,776,-184,769,-216,766,-198,761,-196,752,-206,751,-193,743,-235,733,-223,721,-247,723,-217,706,-255,714,-252,707,-263,702,-223,701,-398,654,-428,626,-424,619,-433,601,-482,608,-516,636,-539,671,-508,699,-546,696,-543,708,-513,705,-558,716,-547,725,-585,755,-685,760,-714,770,-667,773,-733,780,-657,793,-680,801,-622,813,-626,817,-503,824,-445,816,-467,826,-386,835,-271,835],[1435,-137,1453,-149,1463,-189,1488,-203,1531,-260,1530,-309,1500,-374,1463,-390,1450,-379,1436,-388,1406,-380,1395,-361,1381,-356,1382,-343,1368,-352,1378,-329,1359,-348,1342,-326,1313,-315,1261,-322,1236,-338,1198,-339,1180,-350,1150,-342,1156,-316,1133,-261,1142,-263,1133,-243,1141,-217,1142,-225,1167,-207,1208,-196,1230,-164,1238,-170,1235,-166,1256,-142,1270,-138,1296,-149,1306,-125,1325,-121,1318,-112,1364,-118,1369,-123,1355,-150,1402,-177,1412,-163,1425,-106,1435,-137],[-865,731,-857,725,-823,737,-806,727,-807,720,-778,727,-741,713,-722,715,-669,691,-688,687,-618,668,-639,650,-680,662,-646,633,-650,626,-687,637,-661,619,-748,646,-777,642,-785,645,-779,653,-739,654,-729,677,-789,701,-849,699,-898,712,-894,731,-858,738,-865,731],[1341,-11,1344,-27,1354,-33,1383,-17,1445,-38,1476,-60,1471,-73,1506,-105,1479,-101,1447,-76,1426,-93,1391,-81,1376,-84,1386,-73,1379,-53,1336,-35,1329,-41,1319,-28,1337,-22,1305,-9,1323,-3,1341,-11],[-685,831,-618,826,-676,815,-654,815,-711,798,-769,793,-753,785,-797,772,-778,767,-805,761,-894,764,-877,771,-882,779,-849,775,-879,783,-850,793,-869,802,-818,804,-876,805,-915,818,-793,831,-685,831],[1409,371,1402,351,1372,346,1357,334,1350,346,1309,338,1320,331,1313,314,1302,314,1304,323,1294,333,1326,354,1356,355,1367,373,1373,368,1394,382,1398,405,1413,413,1418,391,1409,371],[-1141,731,-1146,726,-1099,729,-1081,716,-1076,720,-1084,730,-1065,730,-1044,709,-1010,695,-1027,695,-1024,687,-1133,685,-1173,699,-1124,703,-1179,705,-1184,709,-1161,713,-1194,715,-1178,727,-1141,731],[1252,14,1236,2,1201,2,1209,-14,1233,-6,1215,-19,1231,-53,1222,-52,1227,-44,1214,-45,1209,-26,1203,-29,1204,-55,1193,-53,1195,-34,1187,-28,1200,5,1208,13,1252,14],[-30,586,-40,575,-19,576,-31,559,16,527,14,512,-52,499,-57,501,-34,514,-52,519,-42,523,-45,535,-29,539,-48,547,-50,557,-55,553,-61,567,-50,586,-30,586],[-561,506,-568,498,-534,492,-537,485,-530,486,-526,475,-530,466,-541,468,-542,477,-554,468,-562,476,-592,476,-573,507,-554,515,-561,506],[1178,18,1190,9,1178,7,1161,-40,1102,-29,1090,-4,1096,20,1111,18,1113,27,1130,31,1167,69,1191,54,1173,32,1178,18],[1263,84,1265,71,1262,62,1258,72,1253,67,1254,55,1242,61,1236,78,1219,71,1234,86,1254,89,1254,97,1263,84],[500,-135,503,-157,496,-157,471,-249,454,-256,440,-249,432,-220,443,-200,444,-162,477,-145,491,-120,500,-135],[-145,664,-147,658,-136,651,-186,635,-227,639,-217,644,-239,648,-222,653,-243,656,-221,664,-205,657,-145,664],[-1750,665,-1718,669,-1699,659,-1725,654,-1729,642,-1783,653,-1786,661,-1798,658,-1800,649,-1800,689,-1749,672,-1750,665],[1213,185,1222,184,1225,170,1217,143,1239,137,1240,125,1206,138,1209,145,1200,149,1198,163,1213,185],[1439,441,1453,443,1455,432,1431,420,1416,426,1410,415,1399,415,1398,425,1413,433,1419,455,1439,441],[1436,507,1446,489,1431,493,1425,478,1435,461,1427,467,1420,459,1415,519,1422,542,1436,507],[-1003,738,-973,737,-980,729,-965,725,-967,716,-993,713,-1025,725,-1004,727,-1015,733,-1003,738],[-1082,762,-1057,754,-1122,744,-1138,747,-1117,751,-1177,752,-1154,764,-1090,754,-1105,764,-1082,762],[1746,-361,1767,-378,1785,-377,1752,-416,1749,-399,1738,-395,1747,-373,1726,-345,1746,-361],[-796,227,-741,202,-777,198,-770,204,-787,216,-818,226,-849,219,-822,231,-796,227],[575,707,536,707,514,720,556,750,662,768,688,765,584,743,554,723,575,707],[-684,-709,-687,-721,-710,-725,-750,-716,-720,-711,-717,-695,-702,-688,-684,-709],[1058,-58,1025,-42,952,54,974,52,1038,1,1034,-7,1061,-30,1058,-58],[-725,198,-683,186,-706,184,-714,176,-744,183,-723,186,-734,196,-725,198],[-1204,714,-1230,709,-1259,718,-1239,736,-1249,742,-1155,734,-1192,725,-1204,714],[-451,-780,-439,-784,-433,-800,-504,-810,-541,-806,-486,-780,-451,-780],[-677,-538,-650,-547,-692,-555,-746,-528,-711,-540,-693,-525,-677,-538],[1730,-409,1742,-417,1706,-459,1693,-466,1666,-462,1670,-451,1730,-409],[-67,522,-99,518,-91,528,-96,538,-67,551,-56,545,-67,522],[-946,771,-891,756,-811,757,-798,749,-897,745,-971,767,-946,771],[1454,-407,1482,-408,1479,-432,1460,-435,1447,-411,1454,-407],[1086,-67,1107,-64,1157,-83,1053,-68,1060,-59,1086,-67],[1519,-54,1502,-63,1483,-57,1508,-54,1521,-41,1519,-54],[1255,121,1257,110,1250,113,1248,101,1242,125,1255,121],[-1327,540,-1317,541,-1320,529,-1311,521,-1330,534,-1327,540],[-932,727,-954,720,-960,734,-945,741,-905,738,-932,727],[1450,755,1443,748,1389,746,1369,752,1388,761,1450,755],[-985,767,-977,762,-981,750,-1025,755,-1025,763,-985,767],[-1000,783,-996,779,-1051,783,-1042,786,-1054,793,-1000,783],[999,788,949,790,911,803,959,812,1001,797,999,788],[-870,796,-858,793,-908,782,-967,801,-924,812,-870,796]];
let gRot=18,gTilt=14,gDrag=false,gPX=0,gPY=0,gVel=0.10,gLastMove=0,gStations=[],gRaf=0,gSel=null,gT=0;
let gZoom=1,gPinch=0;
const STARS=(()=>{let a=[],s=12345;const r=()=>(s=(s*1103515245+12345)&0x7fffffff)/0x7fffffff;
 for(let i=0;i<90;i++)a.push([r(),r(),r()*0.7+0.3]);return a;})();

function gproj(lon,lat,cx,cy,r){
 const l=(lon-gRot)*Math.PI/180,p=lat*Math.PI/180,t=gTilt*Math.PI/180;
 const cp=Math.cos(p),sp=Math.sin(p),cl=Math.cos(l),sl=Math.sin(l),ct=Math.cos(t),st=Math.sin(t);
 const z=st*sp+ct*cp*cl;
 let x=cp*sl, y=-(ct*sp-st*cp*cl);
 return {x:cx+r*x, y:cy+r*y, z:z};
}
// Far-side points ride the limb so polygons stay closed.
function gclamp(p,cx,cy,r){
 if(p.z>0) return p;
 const dx=p.x-cx,dy=p.y-cy,d=Math.hypot(dx,dy)||1;
 return {x:cx+dx/d*r, y:cy+dy/d*r, z:p.z};
}

// Global so the on-canvas +/- buttons can drive it, not just wheel and pinch:
// a touchpad pinch is awkward and a mouse wheel is not obvious, so the buttons
// are the discoverable path.
function gSetZoom(z){
 gZoom=Math.max(1,Math.min(6,z)); gLastMove=Date.now();
 const h=document.getElementById('ghint');
 if(h) h.textContent = gZoom>1.02
  ? 'zoom '+gZoom.toFixed(1)+'\u00d7 \u2014 drag to spin, \u25c9 to reset'
  : 'drag to spin, +/\u2212 or scroll to zoom, tap a light to tune';
 if(!gRaf) globeDraw();
}
function globeDraw(){
 const c=document.getElementById('gl'); if(!c||document.getElementById('pGlobe').hidden){gRaf=0;return;}
 const x=c.getContext('2d'),W=c.width,H=c.height,cx=W/2,cy=H/2,r=W*0.415*gZoom;
 gT++;
 x.clearRect(0,0,W,H);

 // starfield
 for(const[sx,sy,sa] of STARS){
  x.globalAlpha=sa*0.5; x.fillStyle='#cbb8e0';
  x.fillRect(sx*W, sy*H, 1.4, 1.4);
 }
 x.globalAlpha=1;

 // atmosphere — only meaningful when the whole sphere is in frame
 if(gZoom<1.35){
  const halo=x.createRadialGradient(cx,cy,r*0.92,cx,cy,r*1.16);
  halo.addColorStop(0,'rgba(124,77,255,'+(0.34*(1.35-gZoom)/0.35)+')'); halo.addColorStop(1,'rgba(124,77,255,0)');
  x.beginPath(); x.arc(cx,cy,r*1.16,0,7); x.fillStyle=halo; x.fill();
 }

 // ocean, lit from upper-left
 const oc=x.createRadialGradient(cx-r*0.36,cy-r*0.42,r*0.05,cx,cy,r);
 oc.addColorStop(0,'#3a2c58'); oc.addColorStop(0.55,'#241b3a'); oc.addColorStop(1,'#120c1e');
 x.beginPath(); x.arc(cx,cy,r,0,7); x.fillStyle=oc; x.fill();

 x.save(); x.beginPath(); x.arc(cx,cy,r,0,7); x.clip();

 // graticule
 x.strokeStyle='rgba(201,167,224,0.10)'; x.lineWidth=1;
 const gstep=gZoom>2.2?10:20;
 for(let la=-80;la<=80;la+=gstep){ x.beginPath(); let st=false;
  for(let lo=-180;lo<=180;lo+=3){const p=gproj(lo,la,cx,cy,r);
   if(p.z>0){st?x.lineTo(p.x,p.y):x.moveTo(p.x,p.y);st=true;}else st=false;}
  x.stroke();}
 for(let lo=-180;lo<180;lo+=gstep){ x.beginPath(); let st=false;
  for(let la=-90;la<=90;la+=3){const p=gproj(lo,la,cx,cy,r);
   if(p.z>0){st?x.lineTo(p.x,p.y):x.moveTo(p.x,p.y);st=true;}else st=false;}
  x.stroke();}

 // land
 x.fillStyle='rgba(150,110,235,0.42)'; x.strokeStyle='rgba(214,190,255,0.5)'; x.lineWidth=1.1;
 for(const poly of LAND){
  let any=false; x.beginPath();
  for(let i=0;i<poly.length;i+=2){
   const raw=gproj(poly[i]/10,poly[i+1]/10,cx,cy,r);
   if(raw.z>0) any=true;
   const p=gclamp(raw,cx,cy,r);
   i?x.lineTo(p.x,p.y):x.moveTo(p.x,p.y);
  }
  if(!any) continue;
  x.closePath(); x.fill(); x.stroke();
 }

 // limb shading for sphericity
 const sh=x.createRadialGradient(cx-r*0.3,cy-r*0.35,r*0.15,cx,cy,r);
 sh.addColorStop(0,'rgba(0,0,0,0)'); sh.addColorStop(0.72,'rgba(0,0,0,0.12)'); sh.addColorStop(1,'rgba(0,0,0,0.62)');
 x.beginPath(); x.arc(cx,cy,r,0,7); x.fillStyle=sh; x.fill();
 x.restore();

 // stations
 for(const st of gStations){
  const p=gproj(st.lon,st.lat,cx,cy,r);
  st._x=p.x; st._y=p.y; st._v=p.z>0.02;
  if(!st._v) continue;
  const on=(S.station&&S.station===st.name);
  const fade=Math.min(1,p.z*2.6);
  if(on){
   const pr=1+0.35*Math.sin(gT/12);
   x.beginPath(); x.arc(p.x,p.y,9*pr*Math.min(1.9,0.72+gZoom*0.34),0,7);
   x.fillStyle='rgba(74,222,128,0.22)'; x.fill();
  }
  x.save(); x.shadowBlur=on?16:9; x.shadowColor=on?'#4ade80':'#ffc35a';
  const msz=(on?5.5:3.4)*Math.min(1.9,0.72+gZoom*0.34);
  x.beginPath(); x.arc(p.x,p.y,msz,0,7);
  x.globalAlpha=fade; x.fillStyle=on?'#4ade80':'#ffca6b'; x.fill();
  x.restore(); x.globalAlpha=1;
  if(gZoom>=2.6 && gStations.length<220){
   x.globalAlpha=Math.min(1,(gZoom-2.6)/0.8)*fade;
   x.font='500 15px system-ui,sans-serif'; x.textAlign='center';
   x.fillStyle='rgba(10,7,16,0.75)';
   const w=x.measureText(st.name).width;
   x.fillRect(p.x-w/2-5, p.y+9, w+10, 19);
   x.fillStyle=on?'#4ade80':'#e8dcf5';
   x.fillText(st.name, p.x, p.y+23);
   x.globalAlpha=1;
  }
 }
 x.beginPath(); x.arc(cx,cy,r,0,7);
 x.strokeStyle='rgba(201,167,224,0.35)'; x.lineWidth=1.4; x.stroke();

 if(!gDrag){
  const idleV=gZoom>1.4?0.0:0.10;
  if(Date.now()-gLastMove>1600 && Math.abs(gVel)<0.10) gVel += (idleV-gVel)*0.02;
  gRot=(gRot+gVel+360)%360; gVel*=0.985;
  if(gZoom<=1.4 && Math.abs(gVel)<0.02) gVel=0.02;
 }
 gRaf=requestAnimationFrame(globeDraw);
}

function globeInit(){
 const c=document.getElementById('gl'); if(!c) return;
 if(!c._wired){
  c._wired=1;
  const pos=e=>{const b=c.getBoundingClientRect();const t=e.touches?e.touches[0]:e;
   return {x:(t.clientX-b.left)*c.width/b.width, y:(t.clientY-b.top)*c.height/b.height};};
  let moved=0;
  const down=e=>{gDrag=true;moved=0;const p=pos(e);gPX=p.x;gPY=p.y;c.style.cursor='grabbing';};
  const move=e=>{if(!gDrag)return;const p=pos(e);const dx=p.x-gPX;
   gRot=(gRot-dx*0.30/gZoom+360)%360; gTilt=Math.max(-78,Math.min(78,gTilt+(p.y-gPY)*0.22/gZoom));
   gVel=-dx*0.030/gZoom; moved+=Math.abs(dx)+Math.abs(p.y-gPY); gPX=p.x;gPY=p.y;
   gLastMove=Date.now(); if(e.cancelable)e.preventDefault();};
  const up=e=>{
   if(gDrag&&moved<7){
    const t=e.changedTouches?{touches:e.changedTouches}:e; const p=pos(t);
    let best=null,bd=1e9;
    for(const st of gStations){if(!st._v)continue;
     const d=Math.hypot(st._x-p.x,st._y-p.y); if(d<bd){bd=d;best=st;}}
    if(best&&bd<30){gSel=best; tune(best.url,best.name); showName(best.name+' · '+(best.country||''));}
   }
   gDrag=false;c.style.cursor='grab';gLastMove=Date.now();
  };
  c.addEventListener('wheel',e=>{gSetZoom(gZoom*Math.exp(-e.deltaY*0.0016)); e.preventDefault();},{passive:false});
  c.addEventListener('dblclick',()=>gSetZoom(1));
  c.addEventListener('touchstart',e=>{
   if(e.touches.length===2){
    gPinch=Math.hypot(e.touches[0].clientX-e.touches[1].clientX,
                      e.touches[0].clientY-e.touches[1].clientY); gDrag=false;}
  },{passive:true});
  c.addEventListener('touchmove',e=>{
   if(e.touches.length===2&&gPinch){
    const d=Math.hypot(e.touches[0].clientX-e.touches[1].clientX,
                       e.touches[0].clientY-e.touches[1].clientY);
    gSetZoom(gZoom*(d/gPinch)); gPinch=d; if(e.cancelable)e.preventDefault();}
  },{passive:false});
  c.addEventListener('touchend',e=>{ if(e.touches.length<2) gPinch=0; },{passive:true});
  let lastTap=0;
  c.addEventListener('touchend',()=>{const n=Date.now(); if(n-lastTap<320) gSetZoom(1); lastTap=n;},{passive:true});
  c.addEventListener('mousedown',down); c.addEventListener('mousemove',move);
  window.addEventListener('mouseup',up);
  c.addEventListener('touchstart',down,{passive:true});
  c.addEventListener('touchmove',move,{passive:false});
  c.addEventListener('touchend',up);
 }
 if(!document.getElementById('gc').dataset.filled) loadCountries();
 if(!gStations.length) globeSample();
 if(!gRaf) globeDraw();
}
// The catalogue lives on the device, so browsing needs no directory API at all.
function loadCountries(){
 const sel=document.getElementById('gc');
 fetch('/api/countries').then(r=>r.json()).then(d=>{
  if(!d.length) return;
  sel.dataset.filled=1;
  d.sort((a,b)=>b.n-a.n);
  for(const c of d){
   const o=document.createElement('option');
   o.value=c.off+':'+c.n; o.textContent=c.name+' ('+c.n+')';
   sel.appendChild(o);
  }
 }).catch(()=>{});
}
// First view: a spread of the whole catalogue, so the globe is never blank.
function globeSample(){
 const h=document.getElementById('ghint'); h.textContent='loading the world…';
 fetch('/api/catalogue?sample=320').then(r=>r.json()).then(d=>{
  gStations=d.filter(s=>s.lat!=null&&s.lon!=null);
  h.textContent=gStations.length+' stations worldwide — pick a country for more, or zoom in';
  if(!gRaf) globeDraw();
 }).catch(()=>{h.textContent='could not load the catalogue';});
}
function globeCountry(){
 const v=document.getElementById('gc').value; if(!v) return;
 const [off,n]=v.split(':');
 const h=document.getElementById('ghint'); h.textContent='loading…';
 fetch('/api/catalogue?off='+off+'&n='+Math.min(300,n)).then(r=>r.json()).then(d=>{
  gStations=d.filter(s=>s.lat!=null&&s.lon!=null);
  if(gStations.length){
   const la=gStations.reduce((a,s)=>a+s.lat,0)/gStations.length;
   const lo=gStations.reduce((a,s)=>a+s.lon,0)/gStations.length;
   gRot=lo; gTilt=Math.max(-70,Math.min(70,la)); gVel=0; gLastMove=Date.now();
  }
  h.textContent=gStations.length+' stations — scroll or pinch to zoom in';
  if(!gRaf) globeDraw();
 }).catch(()=>{h.textContent='could not load that country';});
}
function showName(t){const e=document.getElementById('gname');e.textContent=t;e.style.opacity=1;
 clearTimeout(e._t); e._t=setTimeout(()=>e.style.opacity=0,3200);}
function globeSearch(quiet){
 const q=(document.getElementById('gq').value||'').trim()||'radio';
 const h=document.getElementById('ghint'); h.textContent='searching…';
 fetch('/api/search?q='+encodeURIComponent(q)).then(r=>r.json()).then(d=>{
  gStations=d.filter(s=>s.lat!=null&&s.lon!=null&&(s.lat||s.lon));
  h.textContent = gStations.length
    ? gStations.length+' of '+d.length+' placed — drag to spin, tap a light to tune'
    : 'none of those '+d.length+' stations carry coordinates';
  if(!gRaf) globeDraw();
 }).catch(()=>{h.textContent='search failed';});
}

poll(); favs(); setInterval(poll,3000);
</script></body></html>)HTML";
