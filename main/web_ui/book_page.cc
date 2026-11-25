#include "book_page.h"

const char* const kBookPageHtml = R"HTML(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>电子书 - 小智</title>
<style>
:root{--bg:#070b14;--card:#111827;--line:#26334a;--fg:#e6edf7;--dim:#94a3b8;--acc:#22d3ee}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI","PingFang SC",sans-serif;background:var(--bg);color:var(--fg);min-height:100vh;padding:16px 14px 250px}
.app{max-width:560px;margin:0 auto}
.top-nav{display:flex;gap:12px;margin-bottom:16px}
.top-nav a{color:var(--dim);text-decoration:none;font-size:14px}
h1{font-size:1.4rem;margin-bottom:16px}
h2{font-size:1.1rem;margin-bottom:12px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:18px;margin-bottom:16px}
label{display:block;font-size:13px;color:var(--dim);margin-bottom:4px}
input,select{width:100%;background:var(--bg);border:1px solid var(--line);color:var(--fg);border-radius:8px;padding:9px;font-size:14px;margin-bottom:10px}
button{background:var(--acc);color:#06202a;border:0;border-radius:8px;padding:9px 14px;font-size:14px;font-weight:600;cursor:pointer}
button:disabled{opacity:.4}
.row{display:flex;gap:8px;align-items:center}
.row>*{flex:1}
.hint{font-size:12px;color:var(--dim);line-height:1.6}
.msg{font-size:12px;margin-top:8px;min-height:16px;color:#94a3b8}
.msg.ok{color:#4ade80}
.msg.err{color:#f88}
.msg.info{color:#94a3b8}
.book-item{display:flex;align-items:center;gap:8px;padding:10px;border-bottom:1px solid var(--line);cursor:pointer}
.book-item:hover{background:rgba(34,211,238,.08)}
.book-item .name{flex:1;overflow:hidden;text-overflow:ellipsis;white-space:nowrap}
.book-item .prog{font-size:11px;color:var(--dim);flex:none}
.pager{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:8px 0}
.pager span{font-size:.8rem;color:var(--dim)}
.pager button{padding:6px 12px;font-size:.85rem}
.player{position:fixed;left:0;right:0;bottom:0;background:rgba(7,11,20,.9);backdrop-filter:blur(14px);border-top:1px solid var(--line);padding:12px 16px calc(12px + env(safe-area-inset-bottom));box-shadow:0 -8px 30px rgba(0,0,0,.5)}
.player>.inner{max-width:560px;margin:0 auto}
.np-title{font-size:.95rem;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;margin-bottom:8px}
.progress{font-size:.78rem;color:var(--dim);display:flex;align-items:center;gap:8px}
.progress input{flex:1;height:4px;accent-color:var(--acc)}
.hidden{display:none}
</style>
</head>
<body>
<div class="app">
  <div class="top-nav"><a href="/">🏠 首页</a><a href="/music">🎵 音乐</a><a href="/radio">📻 电台</a><a href="/config">⚙️ 配置</a></div>
  <h1>📖 电子书</h1>

  <div class="card">
    <h2>WebDAV 书目</h2>
    <label>WebDAV 目录 URL（会列出其中的 TXT/EPUB）</label>
    <input id="book-url" placeholder="http://host:port/dav/books">
    <div class="row"><input id="book-user" placeholder="用户名"><input id="book-pass" type="password" placeholder="密码"></div>
    <div class="hint">支持 TXT（UTF-8）与 EPUB；PDF/MOBI 不支持。</div>
    <div class="row" style="margin-top:10px"><button id="btn-save-source">保存书源</button></div>
    <div id="book-list"></div>
    <div class="pager"><button id="book-prev">上一页</button><span id="book-page">-</span><button id="book-next">下一页</button></div>
    <div class="msg" id="book-msg"></div>
  </div>

  <div class="card">
    <h2>TTS 设置</h2>
    <label>方案</label>
    <select id="tts-provider"><option value="edge">Edge TTS（在线，音色自然）</option><option value="http">自定义 HTTP（OpenAI 兼容）</option></select>
    <label>音色</label>
    <select id="tts-voice"></select>
    <label>语速（%）<span id="speed-val">0</span></label>
    <input id="tts-speed" type="range" min="-50" max="50" step="5" value="0">
    <div id="http-cfg" class="hidden">
      <label>HTTP TTS URL</label><input id="tts-url" placeholder="http://host:port/v1/audio/speech">
      <label>API Key（可选）</label><input id="tts-key" type="password" placeholder="Bearer token">
    </div>
    <div class="row" style="margin-top:10px"><button id="btn-tts-save">保存 TTS 设置</button></div>
    <div class="msg" id="tts-msg"></div>
    <label style="margin-top:14px">测试语音</label>
    <input id="tts-test-text" value="你好，这是语音测试">
    <button id="btn-tts-test">试听效果</button>
    <div class="msg" id="tts-test-msg"></div>
  </div>
</div>

<div class="player"><div class="inner">
  <div class="np-title" id="np-title">未播放</div>
  <div class="progress"><span id="pg-cur">0</span><input id="pg" type="range" min="0" max="1" step="1" disabled><span id="pg-total">0</span></div>
  <div class="row" style="margin-top:8px">
    <button id="btn-prev">上一章</button><button id="btn-toggle">播放</button>
    <button id="btn-next">下一章</button><button id="btn-stop">停止</button>
  </div>
</div></div>

<script>
const $=id=>document.getElementById(id);
async function api(path,opt={}){const r=await fetch(path,{...opt,headers:{'Content-Type':'application/json'}});if(r.status===401)location.replace('/login?next=/book');return r.json().catch(()=>({}))}
const esc=s=>String(s||'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const setMsg=(id,text,kind)=>{const el=$(id);el.textContent=text;el.className='msg'+(kind?' '+kind:'')};
let voices=[];
async function loadTts(){const d=await api('/api/ebook/tts-config');$('tts-provider').value=d.provider||'edge';voices=d.voices||[];$('tts-voice').innerHTML=voices.map(v=>'<option>'+v+'</option>').join('');$('tts-voice').value=d.voice||voices[0]||'';$('tts-speed').value=d.speed||0;$('speed-val').textContent=d.speed||0;$('tts-url').value=d.url||'';$('tts-key').value=d.api_key||'';if(d.source_url){$('book-url').value=d.source_url}$('book-user').value=d.source_user||'';$('book-pass').value=d.source_pass||'';toggleHttp()}
function toggleHttp(){$('http-cfg').classList.toggle('hidden',$('tts-provider').value!=='http')}
$('tts-provider').addEventListener('change',toggleHttp);
$('tts-speed').addEventListener('input',()=>$('speed-val').textContent=$('tts-speed').value);
$('btn-tts-save').addEventListener('click',async()=>{setMsg('tts-msg','保存中…','info');const d=await api('/api/ebook/tts-config',{method:'POST',body:JSON.stringify({provider:$('tts-provider').value,voice:$('tts-voice').value,speed:parseInt($('tts-speed').value),url:$('tts-url').value,api_key:$('tts-key').value,source_url:$('book-url').value.trim(),source_user:$('book-user').value,source_pass:$('book-pass').value})});setMsg('tts-msg',d.status==='ok'?'已保存':'保存失败',d.status==='ok'?'ok':'err');setTimeout(()=>setMsg('tts-msg',''),3000)});
async function loadStatus(){const d=await api('/api/ebook/status');$('np-title').textContent=d.active?(d.book||'播放中')+(d.total?(' · 第'+(d.paragraph+1)+'/'+d.total+'段'):''):'未播放';$('pg-cur').textContent=d.paragraph||0;$('pg-total').textContent=d.total||0;$('pg').max=d.total||1;$('pg').value=d.paragraph||0;$('btn-toggle').textContent=d.paused?'继续':(d.active?'暂停':'播放')}
let allBooks=[];let bookPage=1;const PAGE_SIZE=20;
function renderBooks(){const pages=Math.max(1,Math.ceil(allBooks.length/PAGE_SIZE));if(bookPage>pages)bookPage=pages;const start=(bookPage-1)*PAGE_SIZE;const list=allBooks.slice(start,start+PAGE_SIZE);$('book-list').innerHTML=list.map(b=>'<div class="book-item" data-url="'+esc(b.url)+'"><span class="name">📖 '+esc(b.name)+'</span>'+(b.progress>0?'<span class="prog">已读 '+b.progress+' 段</span>':'')+'<button>播放</button></div>').join('');$('book-list').querySelectorAll('.book-item').forEach(el=>{el.addEventListener('click',()=>playBook(el.dataset.url));el.querySelector('button').addEventListener('click',e=>{e.stopPropagation();playBook(el.dataset.url)})});$('book-page').textContent=bookPage+'/'+pages+' · '+allBooks.length+' 本';$('book-prev').disabled=bookPage<=1;$('book-next').disabled=bookPage>=pages}
$('book-prev').addEventListener('click',()=>{if(bookPage>1){bookPage--;renderBooks()}});
$('book-next').addEventListener('click',()=>{bookPage++;renderBooks()});
function playBook(url){setMsg('book-msg','加载中…','info');api('/api/ebook/play',{method:'POST',body:JSON.stringify({url:url,username:$('book-user').value,password:$('book-pass').value})}).then(d=>{setMsg('book-msg',d.status==='ok'?'已开始播放':'加载失败',d.status==='ok'?'ok':'err');loadStatus();if(d.status==='ok')setTimeout(()=>setMsg('book-msg',''),3000)})}
async function loadBooks(){const u=$('book-url').value.trim();if(!u)return;setMsg('book-msg','加载中…','info');const d=await api('/api/ebook/list',{method:'POST',body:JSON.stringify({url:u,username:$('book-user').value,password:$('book-pass').value})});if(d.status!=='ok'){setMsg('book-msg',d.error||'加载失败','err');return}allBooks=d.books||[];bookPage=1;renderBooks();setMsg('book-msg',allBooks.length?('找到 '+allBooks.length+' 本书'):'未找到 TXT/EPUB','info')}
$('book-url').addEventListener('change',loadBooks);
$('btn-save-source').addEventListener('click',async()=>{setMsg('book-msg','保存中…','info');const d=await api('/api/ebook/tts-config',{method:'POST',body:JSON.stringify({source_url:$('book-url').value.trim(),source_user:$('book-user').value,source_pass:$('book-pass').value})});setMsg('book-msg',d.status==='ok'?'书源已保存':'保存失败',d.status==='ok'?'ok':'err');if(d.status==='ok')loadBooks()});
$('btn-tts-test').addEventListener('click',async()=>{setMsg('tts-test-msg','合成中…','info');const d=await api('/api/ebook/tts-test',{method:'POST',body:JSON.stringify({text:$('tts-test-text').value,voice:$('tts-voice').value,speed:parseInt($('tts-speed').value)})});if(d.status!=='ok'){setMsg('tts-test-msg',d.error||'测试失败','err');return}setMsg('tts-test-msg','试听中…','info');setTimeout(()=>setMsg('tts-test-msg',''),4000)});
$('btn-toggle').addEventListener('click',async()=>{const d=await api('/api/ebook/status');await api(d.active?(d.paused?'/api/ebook/resume':'/api/ebook/pause'):'/api/ebook/stop',{method:'POST'});loadStatus()});
$('btn-stop').addEventListener('click',async()=>{await api('/api/ebook/stop',{method:'POST'});loadStatus()});
$('btn-next').addEventListener('click',async()=>{await api('/api/ebook/next',{method:'POST'});loadStatus()});
setInterval(loadStatus,2000);loadTts().then(loadBooks);loadStatus();
</script>
</body>
</html>)HTML";
