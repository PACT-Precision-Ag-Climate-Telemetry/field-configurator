// web_page.h - single-file mobile-first configurator UI (served from flash)
#pragma once
#include <pgmspace.h>

static const char WEB_PAGE[] PROGMEM = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#0f172a">
<title>PACT Configurator</title>
<style>
:root{--bg:#f4f6f8;--card:#fff;--ink:#0f172a;--mut:#64748b;--line:#e2e8f0;--acc:#0b7a5b;--acc2:#e6f4ef;--bad:#b42318;--badbg:#fde8e6;--warn:#b54708}
@media(prefers-color-scheme:dark){:root{--bg:#0b1220;--card:#131c2e;--ink:#e6edf7;--mut:#8b9ab3;--line:#243049;--acc:#34d399;--acc2:#12302b;--bad:#ff8a80;--badbg:#3a1a1a;--warn:#fdb022}}
*{box-sizing:border-box;-webkit-tap-highlight-color:transparent}
html{-webkit-text-size-adjust:100%}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.4 system-ui,-apple-system,Segoe UI,Roboto,sans-serif;padding-bottom:env(safe-area-inset-bottom)}
header{position:sticky;top:0;z-index:5;background:var(--card);border-bottom:1px solid var(--line);padding:10px 16px calc(8px);padding-top:max(10px,env(safe-area-inset-top))}
.top{display:flex;align-items:center;gap:10px}
h1{font-size:18px;margin:0;flex:1}
.dot{width:10px;height:10px;border-radius:50%;background:var(--mut)}
.dot.ok{background:var(--acc)}.dot.bad{background:var(--bad)}
.sub{font-size:13px;color:var(--mut);margin-top:2px;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
nav{display:flex;gap:8px;margin-top:10px}
nav button{flex:1}
main{max-width:720px;margin:0 auto;padding:12px 16px 90px}
button,input{font:inherit}
button{min-height:44px;padding:0 16px;border-radius:12px;border:1px solid var(--line);background:var(--card);color:var(--ink);cursor:pointer}
button.pri{background:var(--acc);border-color:var(--acc);color:#fff;font-weight:600}
@media(prefers-color-scheme:dark){button.pri{color:#04201a}}
button.on{background:var(--acc2);border-color:var(--acc);color:var(--acc);font-weight:600}
button:disabled{opacity:.5}
input[type=text],input[type=search]{width:100%;min-height:44px;padding:0 12px;border-radius:12px;border:1px solid var(--line);background:var(--bg);color:var(--ink);font-size:16px}
.bar{display:flex;gap:8px;margin:0 0 10px;flex-wrap:wrap}
.bar input{flex:1 1 100%}.bar button{flex:1;min-width:90px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:12px;margin-bottom:10px}
.row{display:flex;align-items:center;gap:8px}
.name{font-weight:600;font-size:14px;word-break:break-all;flex:1}
.tag{font-size:11px;padding:2px 8px;border-radius:99px;background:var(--acc2);color:var(--acc)}
.tag.ro{background:var(--line);color:var(--mut)}
.val{font:14px ui-monospace,Menlo,Consolas,monospace;margin:8px 0;white-space:pre-wrap;word-break:break-all;min-height:20px;color:var(--ink)}
.val.err{color:var(--bad)}.val.mut{color:var(--mut)}
.edit{display:flex;gap:8px;margin-top:6px}.edit input{flex:1;min-width:0}
.note{font-size:12px;color:var(--warn);margin-top:4px}
#toast{position:fixed;left:16px;right:16px;bottom:max(16px,env(safe-area-inset-bottom));max-width:688px;margin:auto;background:var(--ink);color:var(--bg);padding:12px 16px;border-radius:12px;opacity:0;transform:translateY(10px);transition:.2s;pointer-events:none;z-index:9}
#toast.show{opacity:1;transform:none}
#log{background:#0b1220;color:#d6e2f5;border-radius:14px;padding:12px;font:13px ui-monospace,Menlo,Consolas,monospace;height:50vh;overflow:auto;white-space:pre-wrap;word-break:break-all}
#ser{background:#0b1220;color:#d6e2f5;border-radius:14px;padding:12px;font:13px ui-monospace,Menlo,Consolas,monospace;height:60vh;overflow:auto;white-space:pre-wrap;word-break:break-all}
#log .me{color:#7dd3fc}#log .er{color:#ff8a80}
.hide{display:none}
.empty{text-align:center;color:var(--mut);padding:30px 10px}
</style></head><body>
<header>
 <div class="top"><h1>PACT Configurator</h1><span class="dot" id="dot"></span></div>
 <div class="sub" id="sub">Connecting to mainboard…</div>
 <nav><button id="tS" class="on">Settings</button><button id="tC">Console</button><button id="tL">Serial</button></nav>
</header>
<main>
 <section id="vS">
  <div class="bar">
   <input id="q" type="search" placeholder="Filter settings…" autocomplete="off">
   <button id="fAll" class="on">All</button><button id="fRw">Editable</button><button id="fRo">Sensors</button>
   <button id="rdAll" class="pri">Read editable</button>
  </div>
  <div id="list"><div class="empty">Loading…</div></div>
 </section>
 <section id="vC" class="hide">
  <div id="log"></div>
  <form class="edit" id="cf" style="margin-top:10px"><input id="ci" type="text" placeholder="get ID   |   set NAME VALUE   |   help" autocomplete="off" autocapitalize="off" spellcheck="false"><button class="pri">Send</button></form>
 </section>
 <section id="vL" class="hide">
  <div id="ser"></div>
  <div class="bar" style="margin-top:10px"><button id="sPause">Pause</button><button id="sClear">Clear</button><button id="sCopy">Copy</button></div>
 </section>
</main>
<div id="toast"></div>
<script>
const $=s=>document.querySelector(s);
const SLOW=/^(BATTERY_MV|MOISTURE_PERCENT|MOISTURE_VOLTS|MOISTURE_RAW)$/;
let items=[],filter='all',busy=false;

function toast(m){const t=$('#toast');t.textContent=m;t.classList.add('show');clearTimeout(toast.t);toast.t=setTimeout(()=>t.classList.remove('show'),2200)}

// Serialise requests: the mainboard console handles one command at a time.
let chain=Promise.resolve();
function cmd(c,t){
  const run=async()=>{
    busy=true;
    try{
      const r=await fetch('/api/cmd',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:'c='+encodeURIComponent(c)+'&t='+(t||6)});
      const j=await r.json();setLink(true);return j;
    }catch(e){setLink(false);return{ok:false,text:'Connection to configurator lost'}}
    finally{busy=false}
  };
  const p=chain.then(run);chain=p.catch(()=>{});return p;
}
function setLink(ok,txt){$('#dot').className='dot '+(ok?'ok':'bad');if(txt)$('#sub').textContent=txt}

// "OK NAME=value[\nmore lines]" -> value
function parseVal(name,text){
  const lines=text.split('\n'),p='OK '+name.toUpperCase()+'=';
  for(let i=0;i<lines.length;i++)if(lines[i].toUpperCase().startsWith(p))return[true,[lines[i].slice(p.length),...lines.slice(i+1)].join('\n')];
  return[false,text];
}

async function loadList(){
  const j=await cmd('list',8);
  const names=[],rw={},vals={};
  for(const l of j.text.split('\n')){
    const m=l.match(/^\s*([A-Z][A-Z0-9_]*)\s*=\s*(.*)$/i);
    if(!m||/^(OK|ERR)$/i.test(m[1]))continue;
    names.push(m[1]);vals[m[1]]=m[2].replace(/\s*\(ro\)\s*$/i,'');
    if(!/\(ro\)/i.test(l))rw[m[1]]=1;
  }
  if(!names.length){
    $('#list').innerHTML='<div class="empty">No reply from the mainboard.<br>Check the UART wiring and baud rate.<br><br><button class="pri" onclick="loadList()">Retry</button></div>';
    setLink(false,'Mainboard not responding');return;
  }
  items=names.map(n=>({n,rw:!!rw[n],v:vals[n]===undefined?null:vals[n],err:false,slow:SLOW.test(n)}));
  render();
  const id=await cmd('get ID',5);const [ok,v]=parseVal('ID',id.text);
  setLink(true,ok?'Mainboard ID: '+v.split('\n')[0]:'Mainboard connected');
}

function render(){
  const q=$('#q').value.trim().toLowerCase();
  const list=items.filter(i=>(filter==='all'||(filter==='rw'?i.rw:!i.rw))&&i.n.toLowerCase().includes(q));
  $('#list').innerHTML=list.length?'':'<div class="empty">No settings match.</div>';
  for(const i of list){
    const c=document.createElement('div');c.className='card';c.id='c_'+i.n;
    c.innerHTML=`<div class="row"><span class="name"></span><span class="tag ${i.rw?'':'ro'}">${i.rw?'editable':'read-only'}</span></div>
     <div class="val ${i.v==null?'mut':i.err?'err':''}"></div>
     ${i.slow?'<div class="note">Updates in the background – read twice, 5 s apart, for a fresh value.</div>':''}
     <div class="edit"><button class="rd">${i.slow?'Read':'Read'}</button></div>`;
    c.querySelector('.name').textContent=i.n;
    c.querySelector('.val').textContent=i.v==null?'—':i.v;
    c.querySelector('.rd').onclick=()=>read(i);
    if(i.rw){
      const e=c.querySelector('.edit');
      e.insertAdjacentHTML('afterbegin','<input type="text" autocapitalize="off" autocomplete="off" spellcheck="false" placeholder="New value">');
      e.insertAdjacentHTML('beforeend','<button class="pri sv">Save</button>');
      const inp=e.querySelector('input');if(i.v!=null&&!i.err)inp.value=i.v;
      const save=()=>write(i,inp.value.trim());
      e.querySelector('.sv').onclick=save;
      inp.onkeydown=ev=>{if(ev.key==='Enter')save()};
    }
    $('#list').appendChild(c);
  }
}

function paint(i){const c=$('#c_'+i.n);if(!c)return;const v=c.querySelector('.val');v.textContent=i.v==null?'—':i.v;v.className='val '+(i.v==null?'mut':i.err?'err':'');
  const inp=c.querySelector('input');if(inp&&!i.err&&i.v!=null&&document.activeElement!==inp)inp.value=i.v}

async function read(i){
  const c=$('#c_'+i.n),b=c&&c.querySelector('.rd');if(b){b.disabled=true;b.textContent='Reading…'}
  const j=await cmd('get '+i.n,i.slow?35:6);const [ok,v]=parseVal(i.n,j.text);
  i.v=ok?v:j.text;i.err=!ok;paint(i);
  if(b){b.disabled=false;b.textContent=i.slow?'Read':'Read'}
}
async function write(i,val){
  if(!val){toast('Enter a value first');return}
  const c=$('#c_'+i.n),b=c.querySelector('.sv');b.disabled=true;b.textContent='Saving…';
  const j=await cmd('set '+i.n+' '+val,8);const [ok,v]=parseVal(i.n,j.text);
  b.disabled=false;b.textContent='Save';
  if(ok){i.v=v;i.err=false;paint(i);toast(i.n+' saved')}else{toast(j.text.slice(0,80))}
}
async function readEditable(){
  const b=$('#rdAll');b.disabled=true;
  for(const i of items.filter(x=>x.rw)){b.textContent='Reading '+i.n.slice(0,14)+'…';await read(i)}
  b.disabled=false;b.textContent='Read editable';toast('Done');
}

// Tabs / filters
let cur='S';
function tab(s){cur=s;$('#vS').classList.toggle('hide',s!=='S');$('#vC').classList.toggle('hide',s!=='C');$('#vL').classList.toggle('hide',s!=='L');$('#tS').className=s==='S'?'on':'';$('#tC').className=s==='C'?'on':'';$('#tL').className=s==='L'?'on':''}
$('#tS').onclick=()=>tab('S');$('#tC').onclick=()=>{tab('C');$('#ci').focus()};$('#tL').onclick=()=>tab('L');

// Serial: live raw mainboard output, polled while the tab is open
let sNext=0,sPaused=false,sPolling=false;
async function serialPoll(){
  if(cur!=='L'||sPaused||busy||sPolling||document.hidden)return;
  sPolling=true;
  try{
    const r=await fetch('/api/serial?since='+sNext,{cache:'no-store'});const j=await r.json();setLink(true);
    sNext=j.next;
    if(j.text){
      const e=$('#ser'),stick=e.scrollTop+e.clientHeight>=e.scrollHeight-30;
      e.appendChild(document.createTextNode(j.text));
      while(e.childNodes.length>400)e.removeChild(e.firstChild);
      if(stick)e.scrollTop=e.scrollHeight;
    }
  }catch(x){setLink(false)}
  finally{sPolling=false}
}
setInterval(serialPoll,700);
$('#sPause').onclick=()=>{sPaused=!sPaused;$('#sPause').textContent=sPaused?'Resume':'Pause';$('#sPause').className=sPaused?'on':''};
$('#sClear').onclick=()=>{$('#ser').textContent=''};
$('#sCopy').onclick=async()=>{try{await navigator.clipboard.writeText($('#ser').textContent);toast('Copied')}catch(x){toast('Copy not available over http')}};
function setF(f){filter=f;$('#fAll').className=f==='all'?'on':'';$('#fRw').className=f==='rw'?'on':'';$('#fRo').className=f==='ro'?'on':'';render()}
$('#fAll').onclick=()=>setF('all');$('#fRw').onclick=()=>setF('rw');$('#fRo').onclick=()=>setF('ro');
$('#q').oninput=render;$('#rdAll').onclick=readEditable;

// Console
function log(t,c){const l=$('#log'),d=document.createElement('div');if(c)d.className=c;d.textContent=t;l.appendChild(d);l.scrollTop=l.scrollHeight}
$('#cf').onsubmit=async e=>{
  e.preventDefault();const c=$('#ci').value.trim();if(!c)return;$('#ci').value='';
  log('> '+c,'me');const j=await cmd(c,SLOW.test(c.split(' ')[1]||'')?35:8);log(j.text,j.ok?'':'er');
};
loadList();
</script></body></html>)HTML";
