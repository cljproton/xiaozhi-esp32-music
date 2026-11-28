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
.book-item .when{font-size:11px;color:var(--dim);flex:none}
.pager{display:flex;align-items:center;justify-content:space-between;gap:8px;padding:8px 0}
.pager span{font-size:.8rem;color:var(--dim)}
.pager button{padding:6px 12px;font-size:.85rem}
.player{position:fixed;left:0;right:0;bottom:0;background:rgba(7,11,20,.9);backdrop-filter:blur(14px);border-top:1px solid var(--line);padding:12px 16px calc(12px + env(safe-area-inset-bottom));box-shadow:0 -8px 30px rgba(0,0,0,.5)}
.player>.inner{max-width:560px;margin:0 auto}
.np-title{font-size:.95rem;font-weight:600;overflow:hidden;text-overflow:ellipsis;white-space:nowrap;margin-bottom:8px}
.progress{font-size:.78rem;color:var(--dim);display:flex;align-items:center;gap:8px}
.progress input{flex:1;height:4px;accent-color:var(--acc)}
.volume{font-size:.78rem;color:var(--dim);display:flex;align-items:center;gap:8px;margin-top:8px}
.volume input{flex:1;min-width:60px;accent-color:var(--acc)}
#volume-value{min-width:3ch;text-align:center;color:var(--fg);font-variant-numeric:tabular-nums}
.hidden{display:none}
</style>
</head>
<body>
<div class="app">
  <div class="top-nav"><a href="/" data-i18n="nav_home">🏠 首页</a><a href="/music" data-i18n="nav_music">🎵 音乐</a><a href="/radio" data-i18n="nav_radio">📻 电台</a><a href="/config" data-i18n="nav_config">⚙️ 配置</a></div>
  <h1 data-i18n="title">📖 电子书</h1>

  <div class="card hidden" id="recent-card">
    <h2 data-i18n="recent_title">最近播放</h2>
    <div id="recent-book"></div>
  </div>

  <div class="card">
    <h2 data-i18n="books_title">WebDAV 书目</h2>
    <label data-i18n="book_url">WebDAV 目录 URL</label>
    <input id="book-url" placeholder="http://host:port/dav/books">
    <div class="row"><input id="book-user" placeholder="用户名" data-i18n-ph="user_ph"><input id="book-pass" type="password" placeholder="密码" data-i18n-ph="pass_ph"></div>
    <div class="hint" data-i18n="book_hint">仅支持 TXT（UTF-8 / GBK）。</div>
    <div class="row" style="margin-top:10px"><button id="btn-save-source" data-i18n="save_source">保存书源</button></div>
    <div id="book-list"></div>
    <div class="pager"><button id="book-prev" data-i18n="prev_page">上一页</button><span id="book-page">-</span><button id="book-next" data-i18n="next_page">下一页</button></div>
    <div class="msg" id="book-msg"></div>
  </div>

  <div class="card">
    <h2 data-i18n="tts_title">TTS 设置</h2>
    <label data-i18n="tts_provider">方案</label>
    <select id="tts-provider"><option value="edge">Edge TTS</option><option value="http">HTTP</option></select>
    <label data-i18n="tts_voice">音色</label>
    <select id="tts-voice"></select>
    <label><span data-i18n="tts_speed">语速（%）</span> <span id="speed-val">0</span></label>
    <input id="tts-speed" type="range" min="-50" max="50" step="5" value="0">
    <div id="http-cfg" class="hidden">
      <label data-i18n="tts_url">HTTP TTS URL</label><input id="tts-url" placeholder="http://host:port/v1/audio/speech">
      <label data-i18n="api_key">API Key（可选）</label><input id="tts-key" type="password" placeholder="Bearer token">
    </div>
    <div class="row" style="margin-top:10px"><button id="btn-tts-save" data-i18n="save_tts">保存 TTS 设置</button></div>
    <div class="msg" id="tts-msg"></div>
    <label style="margin-top:14px" data-i18n="test_voice">测试语音</label>
    <input id="tts-test-text" value="你好，这是语音测试">
    <button id="btn-tts-test" data-i18n="try_voice">试听效果</button>
    <div class="msg" id="tts-test-msg"></div>
  </div>
</div>

<div class="player"><div class="inner">
  <div class="np-title" id="np-title">未播放</div>
  <div class="progress"><span id="pg-cur">0.0%</span><input id="pg" type="range" min="0" max="1000" step="1"><span id="pg-total">100%</span></div>
  <div class="row" style="margin-top:8px">
    <button id="btn-prev" data-i18n="prev_chap">上一章</button><button id="btn-toggle" data-i18n="play">播放</button>
    <button id="btn-next" data-i18n="next_chap">下一章</button><button id="btn-stop" data-i18n="stop">停止</button><button id="btn-replay" data-i18n="replay">重播</button>
  </div>
  <div class="volume"><span id="vol-label" data-i18n="volume">音量</span><input id="vol" type="range" min="0" max="100" step="1" value="70" aria-label="volume"><span id="volume-value">70%</span></div>
</div></div>

<script>
const $=id=>document.getElementById(id);
async function api(path,opt={}){const r=await fetch(path,{...opt,headers:{'Content-Type':'application/json'}});if(r.status===401)location.replace('/login?next=/book');return r.json().catch(()=>({}))}
const esc=s=>String(s||'').replace(/[&<>"']/g,c=>({'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c]));
const setMsg=(id,text,kind)=>{const el=$(id);el.textContent=text;el.className='msg'+(kind?' '+kind:'')};
const LANG_KEY='lang';let lang=localStorage.getItem(LANG_KEY)==='en'?'en':'zh';
const I18N={zh:{title:'📖 电子书',doc_title:'电子书 - 小智',user_ph:'用户名',pass_ph:'密码',test_text:'你好，这是语音测试',books_unit:' 本',recent_title:'最近播放',continue_reading:'继续',last_played:'上次播放',nav_home:'🏠 首页',nav_music:'🎵 音乐',nav_radio:'📻 电台',nav_config:'⚙️ 配置',books_title:'WebDAV 书目',book_url:'WebDAV 目录 URL',book_hint:'仅支持 TXT（UTF-8 / GBK）。',save_source:'保存书源',prev_page:'上一页',next_page:'下一页',tts_title:'TTS 设置',tts_provider:'方案',tts_voice:'音色',tts_speed:'语速（%）',tts_url:'HTTP TTS URL',api_key:'API Key（可选）',save_tts:'保存 TTS 设置',test_voice:'测试语音',try_voice:'试听效果',prev_chap:'上一章',next_chap:'下一章',play:'播放',pause:'暂停',resume:'继续',stop:'停止',replay:'重播',volume:'音量',loading:'加载中…',started:'已开始播放',failed:'加载失败',found:'找到 {n} 本书',no_books:'未找到 TXT',saving:'保存中…',source_saved:'书源已保存',saved:'已保存',synth:'合成中…',trying:'试听中…',test_failed:'测试失败',playing:'播放中',not_playing:'未播放',read_failed:'书源读取失败',read:'已读'},en:{title:'📖 Books',doc_title:'Books - XiaoZhi',user_ph:'Username',pass_ph:'Password',test_text:'Hello, this is a voice test',books_unit:' books',recent_title:'Recently read',continue_reading:'Resume',last_played:'Last played',nav_home:'🏠 Home',nav_music:'🎵 Music',nav_radio:'📻 Radio',nav_config:'⚙️ Settings',books_title:'WebDAV books',book_url:'WebDAV directory URL',book_hint:'TXT only (UTF-8 / GBK).',save_source:'Save source',prev_page:'Prev',next_page:'Next',tts_title:'TTS settings',tts_provider:'Provider',tts_voice:'Voice',tts_speed:'Speed (%)',tts_url:'HTTP TTS URL',api_key:'API key (optional)',save_tts:'Save TTS settings',test_voice:'Test voice',try_voice:'Preview',prev_chap:'Prev',next_chap:'Next',play:'Play',pause:'Pause',resume:'Resume',stop:'Stop',replay:'Replay',volume:'Volume',loading:'Loading…',started:'Playback started',failed:'Load failed',found:'{n} books found',no_books:'No TXT files found',saving:'Saving…',source_saved:'Source saved',saved:'Saved',synth:'Synthesizing…',trying:'Playing preview…',test_failed:'Test failed',playing:'Playing',not_playing:'Not playing',read_failed:'Source read failed',read:'Read'}};
const tr=k=>I18N[lang][k]||I18N.zh[k]||k;
function applyLang(){document.documentElement.lang=lang;document.title=tr('doc_title');document.querySelectorAll('[data-i18n]').forEach(el=>{el.textContent=tr(el.getAttribute('data-i18n'))});document.querySelectorAll('[data-i18n-ph]').forEach(el=>{el.placeholder=tr(el.getAttribute('data-i18n-ph'))});$('tts-test-text').value=tr('test_text');loadStatus()}
window.addEventListener('storage',e=>{if(e.key===LANG_KEY){lang=e.newValue==='en'?'en':'zh';applyLang()}});
let voices=[];let recentKey='';const fmtTime=t=>{const d=new Date(t*1000),p=n=>String(n).padStart(2,'0');return d.getFullYear()+'-'+p(d.getMonth()+1)+'-'+p(d.getDate())+' '+p(d.getHours())+':'+p(d.getMinutes())};
async function loadTts(){const d=await api('/api/ebook/tts-config');$('tts-provider').value=d.provider||'edge';voices=d.voices||[];$('tts-voice').innerHTML=voices.map(v=>'<option>'+v+'</option>').join('');$('tts-voice').value=d.voice||voices[0]||'';$('tts-speed').value=d.speed||0;$('speed-val').textContent=d.speed||0;$('tts-url').value=d.url||'';$('tts-key').value='';if(d.source_url){$('book-url').value=d.source_url}$('book-user').value=d.source_user||'';$('book-pass').value=d.source_pass||'';toggleHttp()}
function toggleHttp(){$('http-cfg').classList.toggle('hidden',$('tts-provider').value!=='http')}
$('tts-provider').addEventListener('change',toggleHttp);
$('tts-speed').addEventListener('input',()=>$('speed-val').textContent=$('tts-speed').value);
$('btn-tts-save').addEventListener('click',async()=>{setMsg('tts-msg',tr('saving'),'info');const cfg={provider:$('tts-provider').value,voice:$('tts-voice').value,speed:parseInt($('tts-speed').value),url:$('tts-url').value,source_url:$('book-url').value.trim(),source_user:$('book-user').value,source_pass:$('book-pass').value};if($('tts-key').value)cfg.api_key=$('tts-key').value;const d=await api('/api/ebook/tts-config',{method:'POST',body:JSON.stringify(cfg)});setMsg('tts-msg',d.status==='ok'?tr('saved'):tr('failed'),d.status==='ok'?'ok':'err');setTimeout(()=>setMsg('tts-msg',''),3000)});
async function loadStatus(){const d=await api('/api/ebook/status');const pct=d.total?(d.consumed*100/d.total):(d.progress||0);const shown=Number(pct).toFixed(1);$('np-title').textContent=(d.error?tr('read_failed')+' · ':'')+(d.active?(d.book||tr('playing'))+' · '+shown+'%':tr('not_playing'));$('pg-cur').textContent=shown+'%';$('pg-total').textContent='100%';$('pg').max=1000;if(document.activeElement!==$('pg'))$('pg').value=Math.round(pct*10);$('btn-toggle').textContent=d.paused?tr('resume'):(d.active?tr('pause'):tr('play'));const recent=d.recent||[];const sig=recent.map(b=>b.url+'|'+b.time).join(';');if(sig!==recentKey){recentKey=sig;const items=recent.map(b=>{const item=document.createElement('div');item.className='book-item';const when=b.time>0?'<small class="when">'+esc(tr('last_played')+' '+fmtTime(b.time))+'</small>':'';item.innerHTML='<span class="name">📖 '+esc(b.name||b.url)+'</span>'+when+'<button>'+tr('continue_reading')+'</button>';item.addEventListener('click',()=>playBook(b.url));item.querySelector('button').addEventListener('click',e=>{e.stopPropagation();playBook(b.url)});return item});$('recent-book').replaceChildren(...items);$('recent-card').classList.toggle('hidden',!items.length)}}
let bookTotal=0;let bookPage=1;let bookPages=1;const PAGE_SIZE=20;
function renderBooks(list){$('book-list').innerHTML=list.map(b=>'<div class="book-item" data-url="'+esc(b.url)+'"><span class="name">📖 '+esc(b.name)+'</span>'+(b.progress>0?'<span class="prog">'+tr('read')+' '+b.progress+'%</span>':'')+'<button>'+tr('play')+'</button></div>').join('');$('book-list').querySelectorAll('.book-item').forEach(el=>{el.addEventListener('click',()=>playBook(el.dataset.url));el.querySelector('button').addEventListener('click',e=>{e.stopPropagation();playBook(el.dataset.url)})});$('book-page').textContent=bookPage+'/'+bookPages+' · '+bookTotal+tr('books_unit');$('book-prev').disabled=bookPage<=1;$('book-next').disabled=bookPage>=bookPages}
$('book-prev').addEventListener('click',()=>{if(bookPage>1)loadBooks(bookPage-1)});
$('book-next').addEventListener('click',()=>{if(bookPage<bookPages)loadBooks(bookPage+1)});
function playBook(url){setMsg('book-msg',tr('loading'),'info');api('/api/ebook/play',{method:'POST',body:JSON.stringify({url:url,username:$('book-user').value,password:$('book-pass').value})}).then(d=>{setMsg('book-msg',d.status==='ok'?tr('started'):tr('failed'),d.status==='ok'?'ok':'err');loadStatus();if(d.status==='ok')setTimeout(()=>setMsg('book-msg',''),3000)})}
async function loadBooks(p=1){const u=$('book-url').value.trim();if(!u)return;setMsg('book-msg',tr('loading'),'info');const d=await api('/api/ebook/list',{method:'POST',body:JSON.stringify({url:u,username:$('book-user').value,password:$('book-pass').value,page:p,page_size:PAGE_SIZE})});if(d.status!=='ok'){setMsg('book-msg',d.error||tr('failed'),'err');return}bookTotal=d.total||0;bookPages=d.pages||1;bookPage=d.page||1;renderBooks(d.books||[]);setMsg('book-msg',bookTotal?tr('found').replace('{n}',bookTotal):tr('no_books'),'info')}
$('book-url').addEventListener('change',loadBooks);
$('btn-save-source').addEventListener('click',async()=>{setMsg('book-msg',tr('saving'),'info');const d=await api('/api/ebook/tts-config',{method:'POST',body:JSON.stringify({source_url:$('book-url').value.trim(),source_user:$('book-user').value,source_pass:$('book-pass').value})});setMsg('book-msg',d.status==='ok'?tr('source_saved'):tr('failed'),d.status==='ok'?'ok':'err');if(d.status==='ok')loadBooks()});
$('btn-tts-test').addEventListener('click',async()=>{setMsg('tts-test-msg',tr('synth'),'info');const d=await api('/api/ebook/tts-test',{method:'POST',body:JSON.stringify({text:$('tts-test-text').value,voice:$('tts-voice').value,speed:parseInt($('tts-speed').value)})});if(d.status!=='ok'){setMsg('tts-test-msg',d.error||tr('test_failed'),'err');return}setMsg('tts-test-msg',tr('trying'),'info');setTimeout(()=>setMsg('tts-test-msg',''),4000)});
$('btn-toggle').addEventListener('click',async()=>{const d=await api('/api/ebook/status');await api(d.active?(d.paused?'/api/ebook/resume':'/api/ebook/pause'):'/api/ebook/stop',{method:'POST'});loadStatus()});
$('btn-stop').addEventListener('click',async()=>{await api('/api/ebook/stop',{method:'POST'});loadStatus()});
$('btn-replay').addEventListener('click',async()=>{await api('/api/ebook/replay',{method:'POST'});loadStatus()});
$('btn-next').addEventListener('click',async()=>{await api('/api/ebook/next',{method:'POST'});loadStatus()});
$('pg').addEventListener('change',async()=>{const pct=Math.round((parseInt($('pg').value,10)||0)/10);await api('/api/ebook/seek',{method:'POST',body:JSON.stringify({percent:pct})});loadStatus()});
const volEl=$('vol');volEl.addEventListener('input',()=>{$('volume-value').textContent=volEl.value+'%'});volEl.addEventListener('change',async()=>{try{await api('/api/music/volume',{method:'POST',body:JSON.stringify({volume:+volEl.value})})}catch(e){}});
async function loadVolume(){const d=await api('/api/music/status');if(typeof d.volume==='number'&&document.activeElement!==volEl){volEl.value=d.volume;$('volume-value').textContent=d.volume+'%'}}
setInterval(loadStatus,2000);setInterval(loadVolume,5000);applyLang();loadTts().then(()=>loadBooks());loadVolume();
</script>
</body>
</html>)HTML";
