#include "WebUIRenderer.h"
#include "handler_registry.h"
#include "handlers/web_auth.h"
#include <esp_http_server.h>
#include <string>

namespace {

const char* kIndexHtmlTemplate = R"HTML(<!DOCTYPE html>
<html lang="{{LANG}}">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width, initial-scale=1.0">
<title>{{APP_TITLE}}</title>
<style>
{{STYLE_CSS}}
</style>
</head>
<body>
<div class="app">
  <header>
    <h1 id="app-title" data-i18n="app_title">小智音乐</h1>
    <div class="header-actions">
      <button id="btn-lang" class="lang-btn" type="button">EN</button>
      <span class="badge" id="badge">待机</span>
    </div>
  </header>

  <nav class="top-nav">
    <a href="/" data-i18n="nav_home">首页</a>
    <a href="/radio" data-i18n="nav_radio">电台</a>
  </nav>

  <section class="card source-card">
    <div class="row">
      <select id="source"><option value="">暂无音源</option></select>
      <button id="btn-reload" type="button" data-i18n="source_refresh">刷新</button>
      <button id="btn-edit-source" type="button" data-i18n="source_edit">编辑</button>
    </div>
    <div class="row">
      <label class="check"><input type="checkbox" id="shuffle"> <span data-i18n="shuffle">随机播放</span></label>
      <button id="btn-play-all" class="primary" type="button" data-i18n="play_all">播放全部</button>
    </div>
    <details id="src-details">
      <summary id="src-summary" data-i18n="source_mgmt">管理音源</summary>
      <p class="hint" id="src-hint" data-i18n="source_hint"></p>
      <form id="source-form">
        <input type="hidden" id="source-edit-index" value="-1">
        <label>基础地址<input id="base-url" type="url" placeholder="http://192.168.1.70:15244" required></label>
        <label>目录路径<input id="path" type="text" value="/dav" required></label>
        <label>用户名<input id="username" type="text" autocomplete="off" required></label>
        <label>密码<input id="password" type="password" autocomplete="off" required></label>
        <div class="row">
          <button type="submit" class="primary" id="src-add" data-i18n="source_add">添加</button>
          <button type="button" id="src-cancel" style="display:none" data-i18n="source_cancel">取消编辑</button>
          <button type="button" id="src-del" data-i18n="source_del">删除当前音源</button>
        </div>
      </form>
    </details>
  </section>

  <section class="card list-card">
    <div id="tracks"><p class="empty" id="src-empty">正在加载…</p></div>
    <div class="pager">
      <button id="btn-page-prev" type="button" data-i18n="page_prev">上一页</button>
      <span id="page-info">-</span>
      <button id="btn-page-next" type="button" data-i18n="page_next">下一页</button>
    </div>
  </section>

  <div class="player">
    <div class="np-title" id="np-title" data-i18n="not_playing">未在播放</div>
    <div class="row controls">
      <button id="btn-prev" type="button" data-i18n="prev">上一首</button>
      <button id="btn-stop" type="button" data-i18n="stop">停止</button>
      <button id="btn-toggle" class="primary" type="button" data-i18n="play">播放</button>
      <button id="btn-next" type="button" data-i18n="next">下一首</button>
    </div>
    <div class="row progress">
      <span id="progress-cur">0:00</span>
      <input id="progress" type="range" min="0" max="3600" step="1">
      <span id="progress-rem">--:--</span>
    </div>
    <div class="row volume">
      <span id="vol-label" data-i18n="volume">音量</span>
      <button id="btn-vol-down" type="button">-</button>
      <span id="volume-value">70</span>
      <button id="btn-vol-up" type="button">+</button>
    </div>
  </div>
</div>
<script>
{{APP_JS}}
</script>
</body>
</html>)HTML";


const char* kHomeHtmlTemplate = R"HTML(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>小智首页</title>
<style>
:root{--bg:#070b14;--card:#111827;--line:#26334a;--fg:#e6edf7;--dim:#94a3b8;--acc:#22d3ee}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"PingFang SC",sans-serif;background:var(--bg);color:var(--fg);min-height:100vh;padding:20px 14px}
.app{max-width:640px;margin:0 auto}
header{display:flex;justify-content:space-between;align-items:center;margin-bottom:20px}
h1{font-size:1.5rem}
h2{font-size:1.1rem;margin-bottom:6px}
p{color:var(--dim);font-size:14px;line-height:1.7;margin-bottom:18px}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(220px,1fr));gap:14px;margin-bottom:20px}
.card{display:block;text-decoration:none;color:var(--fg);background:var(--card);border:1px solid var(--line);border-radius:14px;padding:20px;transition:border-color .2s,transform .2s}
.card:hover{border-color:var(--acc);transform:translateY(-2px)}
.card .icon{font-size:28px;margin-bottom:10px}
.card small{color:var(--dim);font-size:13px}
.panel{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:18px;margin-bottom:16px}
label{display:block;font-size:13px;color:var(--dim);margin-bottom:5px}
input{width:100%;background:var(--bg);border:1px solid var(--line);color:var(--fg);border-radius:8px;padding:9px;font-size:14px;margin-bottom:10px}
button{background:var(--acc);color:#06202a;border:0;border-radius:8px;padding:9px 16px;font-size:14px;font-weight:600;cursor:pointer}
#msg{color:#f88;font-size:13px;min-height:18px;margin-top:8px}
</style>
</head>
<body>
<div class="app">
  <header><h1 id="welcome">欢迎使用小智</h1></header>
  <p id="intro">这台设备现在既是一台 WebDAV 音乐播放器，也是一台网络电台。选择一个入口开始使用。</p>
  <div class="cards">
    <a class="card" href="/music"><div class="icon">🎵</div><h2 id="t-music">音乐播放器</h2><small id="d-music">浏览 WebDAV 音源、播放曲目、管理播放模式</small></a>
    <a class="card" href="/radio"><div class="icon">📻</div><h2 id="t-radio">网络电台</h2><small id="d-radio">搜索、收藏和播放网络电台</small></a>
  </div>
  <div class="panel">
    <h2 id="t-pw">修改登录密码</h2>
    <label for="old" id="l-old">当前密码</label><input id="old" type="password" autocomplete="current-password">
    <label for="new" id="l-new">新密码</label><input id="new" type="password" autocomplete="new-password">
    <button id="change" type="button">修改密码</button>
    <div id="msg"></div>
  </div>
</div>
<script>
const I18N={zh:{welcome:'欢迎使用小智',intro:'这台设备现在既是一台 WebDAV 音乐播放器，也是一台网络电台。选择一个入口开始使用。',music:'音乐播放器',dmusic:'浏览 WebDAV 音源、播放曲目、管理播放模式',radio:'网络电台',dradio:'搜索、收藏和播放网络电台',pw:'修改登录密码',old:'当前密码',nw:'新密码',change:'修改密码',ok:'密码已修改',fail:'修改失败，请检查当前密码'},en:{welcome:'Welcome to XiaoZhi',intro:'This device is both a WebDAV music player and an internet radio. Pick an entry point to get started.',music:'Music Player',dmusic:'Browse WebDAV sources, play tracks and manage playback mode',radio:'Internet Radio',dradio:'Search, save and play radio stations',pw:'Change Password',old:'Current password',nw:'New password',change:'Change password',ok:'Password changed',fail:'Failed to change password'}};
const lang=(navigator.language||'').toLowerCase().startsWith('zh')?'zh':'en';const T=I18N[lang];
for(const [id,key] of [['welcome','welcome'],['intro','intro'],['t-music','music'],['d-music','dmusic'],['t-radio','radio'],['d-radio','dradio'],['t-pw','pw'],['l-old','old'],['l-new','nw'],['change','change']]){const el=document.getElementById(id);if(el)el.textContent=T[key];}
const $=id=>document.getElementById(id);
fetch('/api/auth').then(r=>r.json()).then(d=>{if(!d.authenticated)location.replace('/login?next=/');}).catch(()=>location.replace('/login?next=/'));
$('change').addEventListener('click',async()=>{
  $('msg').textContent='';
  try{
    const r=await fetch('/api/auth',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action:'change',old_password:$('old').value,new_password:$('new').value})});
    if(r.ok)$('msg').textContent=T.ok;else $('msg').textContent=T.fail;
    $('old').value='';$('new').value='';
  }catch(e){$('msg').textContent=T.fail;}
});
</script>
</body>
</html>)HTML";

const char* kStyleCss = R"CSS(
* { box-sizing: border-box; margin: 0; padding: 0; }
body { font-family: -apple-system, BlinkMacSystemFont, "Segoe UI", Roboto, "Helvetica Neue", sans-serif;
       background: #070b14; color: #e6edf7; line-height: 1.5; min-height: 100vh;
       background-image:
         radial-gradient(560px 320px at 15% -10%, rgba(34, 211, 238, 0.16), transparent 65%),
         radial-gradient(640px 360px at 90% -20%, rgba(167, 139, 250, 0.14), transparent 65%),
         radial-gradient(520px 300px at 50% 110%, rgba(59, 130, 246, 0.10), transparent 70%);
       background-attachment: fixed; }
.app { max-width: 560px; margin: 0 auto; padding: 20px 16px 230px; }
header { display: flex; align-items: center; justify-content: space-between; margin-bottom: 16px; }
header h1 { font-size: 1.35rem; font-weight: 700; letter-spacing: 0.5px;
           background: linear-gradient(90deg, #67e8f9, #a78bfa); -webkit-background-clip: text;
           background-clip: text; color: transparent; }
.header-actions { display: flex; align-items: center; gap: 8px; }
.top-nav { display: flex; gap: 14px; margin-bottom: 14px; }
.top-nav a { color: #94a3b8; text-decoration: none; font-size: 0.85rem; padding: 4px 8px;
             border-radius: 8px; border: 1px solid rgba(148, 163, 184, 0.18); background: rgba(15, 23, 42, 0.5); }
.top-nav a:hover { color: #67e8f9; border-color: rgba(34, 211, 238, 0.5); }
.badge { font-size: 0.72rem; color: #94a3b8; border: 1px solid rgba(148, 163, 184, 0.35); border-radius: 999px;
        padding: 3px 10px; background: rgba(15, 23, 42, 0.6); backdrop-filter: blur(6px); }
.badge.on { color: #22d3ee; border-color: rgba(34, 211, 238, 0.55); box-shadow: 0 0 12px rgba(34, 211, 238, 0.25); }
.lang-btn { min-width: 40px; border-radius: 999px; border: 1px solid rgba(148, 163, 184, 0.4);
           background: rgba(15, 23, 42, 0.7); color: #e6edf7; font-size: 0.8rem; }
.lang-btn:active { background: rgba(34, 211, 238, 0.2); }
.card { background: rgba(15, 23, 42, 0.66); border: 1px solid rgba(148, 163, 184, 0.18); border-radius: 14px;
       padding: 12px; margin-bottom: 12px; box-shadow: 0 10px 30px rgba(2, 6, 23, 0.55),
                  inset 0 1px 0 rgba(255, 255, 255, 0.04); backdrop-filter: blur(12px); }
.row { display: flex; align-items: center; gap: 8px; }
.row + .row { margin-top: 10px; }
select, input[type=text], input[type=url], input[type=password], button { font-size: 0.9rem; font-family: inherit;
       border-radius: 10px; border: 1px solid rgba(148, 163, 184, 0.28); background: rgba(2, 6, 23, 0.55);
       color: #e6edf7; padding: 9px 12px; }
select { flex: 1; min-width: 0; }
button { cursor: pointer; white-space: nowrap; }
button:disabled { opacity: 0.4; cursor: default; }
button.primary { background: linear-gradient(135deg, #06b6d4, #8b5cf6); border: none; color: #fff; font-weight: 600;
       box-shadow: 0 0 16px rgba(34, 211, 238, 0.35); }
button.primary:active { filter: brightness(0.85); }
button:not(.primary) { background: rgba(30, 41, 59, 0.8); color: #e2e8f0; }
.check { display: flex; align-items: center; gap: 6px; font-size: 0.9rem; color: #cbd5e1; }
.check input { width: 18px; height: 18px; accent-color: #22d3ee; }
.list-card { padding: 4px 4px 8px; }
#tracks { min-height: 60px; }
.empty { color: #64748b; font-size: 0.9rem; padding: 16px; text-align: center; }
.hint { color: #94a3b8; font-size: 0.8rem; line-height: 1.65; margin: 4px 0 12px; }
.track { display: flex; align-items: center; gap: 8px; padding: 9px 10px; border-radius: 9px; cursor: pointer;
        border: 1px solid transparent; }
.track:hover { background: rgba(30, 41, 59, 0.7); }
.track.active { background: rgba(34, 211, 238, 0.10); border-color: rgba(34, 211, 238, 0.35); font-weight: 600; }
.track .idx { width: 2.4em; text-align: right; color: #64748b; font-size: 0.75rem; flex: none; }
.track.active .idx { color: #22d3ee; }
.track .name { flex: 1; min-width: 0; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
              font-size: 0.92rem; color: #e2e8f0; }
.pager { display: flex; align-items: center; justify-content: space-between; gap: 8px; padding: 8px 8px 0;
        border-top: 1px solid rgba(148, 163, 184, 0.15); }
.pager span { font-size: 0.8rem; color: #64748b; }
.pager button { padding: 6px 12px; font-size: 0.85rem; }
summary { cursor: pointer; font-size: 0.9rem; color: #cbd5e1; padding: 4px; }
form { margin-top: 10px; }
form label { display: block; font-size: 0.82rem; color: #94a3b8; margin-bottom: 10px; }
form label input { display: block; width: 100%; margin-top: 4px; }
form .row button { flex: 1; }
.player { position: fixed; left: 0; right: 0; bottom: 0; background: rgba(7, 11, 20, 0.82); backdrop-filter: blur(14px);
        border-top: 1px solid rgba(148, 163, 184, 0.2); padding: 12px 16px calc(12px + env(safe-area-inset-bottom));
        box-shadow: 0 -8px 30px rgba(2, 6, 23, 0.6); }
.player > * { max-width: 528px; margin-left: auto; margin-right: auto; }
.np-title { font-size: 0.95rem; font-weight: 600; overflow: hidden; text-overflow: ellipsis; white-space: nowrap;
           color: #f1f5f9; margin-bottom: 10px; }
.controls button { flex: 1; }
.controls button.primary { flex: 1.3; }
.progress { font-size: 0.78rem; color: #94a3b8; display: flex; align-items: center; gap: 8px; }
.progress input { flex: 1; min-width: 0; height: 4px; -webkit-appearance: none; appearance: none;
       background: rgba(148, 163, 184, 0.25); border-radius: 2px; outline: none; }
.progress input::-webkit-slider-thumb { -webkit-appearance: none; appearance: none; width: 14px; height: 14px;
       border-radius: 50%; background: #22d3ee; box-shadow: 0 0 10px rgba(34, 211, 238, 0.8); }
.progress input::-moz-range-thumb { width: 14px; height: 14px; border: none; border-radius: 50%;
       background: #22d3ee; box-shadow: 0 0 10px rgba(34, 211, 238, 0.8); }
.progress span { flex: none; font-variant-numeric: tabular-nums; }
.volume { font-size: 0.78rem; color: #94a3b8; display: flex; align-items: center; gap: 8px; }
.volume button { flex: none; padding: 6px 12px; font-size: 0.8rem; }
.volume #volume-value { min-width: 2ch; text-align: center; font-variant-numeric: tabular-nums; color: #e6edf7; }
@media (max-width: 480px) { .app { padding-bottom: 250px; } }
)CSS";

const char* kAppJs = R"JS(
const LANG_KEY = 'lang';
const $ = (id) => document.getElementById(id);
let sources = [];
let sourceIndex = 0;
let page = 1;
let pages = 1;
let playing = { active: false };
let volume = 70;
let pollTimer = null;
let lang = localStorage.getItem(LANG_KEY) === 'en' ? 'en' : 'zh';

const I18N = {
  zh: {
    app_title: '小智音乐', status_idle: '待机', status_playing: '播放中', no_source: '暂无音源',
    loading: '正在加载…', source_mgmt: '管理音源', source_hint: '推荐用 OpenList 统一管理音源：它支持 FTP、SFTP、SMB、WebDAV、对象存储等多种网盘，并统一通过 WebDAV 提供给设备。基础地址填 OpenList 的 http 地址，目录路径填 /dav/<挂载目录>，例如 /dav/ftp/music。目录路径可直接填中文，系统自动编码。', source_refresh: '刷新', source_edit: '编辑',
    source_add: '添加', source_save: '保存', source_cancel: '取消编辑', source_del: '删除当前音源',
    source_none: '暂无音源', shuffle: '随机播放', play_all: '播放全部', prev: '上一首', stop: '停止',
    play: '播放', pause: '暂停', next: '下一首', not_playing: '未在播放', unknown_track: '未知曲目',
    no_tracks: '这个音源没有音频文件', page_prev: '上一页', page_next: '下一页', volume: '音量',
    token_prompt: '请输入访问令牌', token_invalid: '令牌错误', delete_confirm: '删除当前音源？',
    track_count: '首', truncated: '（仅显示前 500 首）',
    nav_home: '🏠 首页', nav_radio: '📻 电台'
  },
  en: {
    app_title: 'XiaoZhi Music', status_idle: 'Idle', status_playing: 'Playing', no_source: 'No sources',
    loading: 'Loading…', source_mgmt: 'Manage Sources', source_hint: 'Use OpenList to manage sources: it supports FTP, SFTP, SMB, WebDAV, object storage and other drives, and exposes them all through WebDAV. Set Base URL to the OpenList http address and Path to /dav/<mount path>, for example /dav/ftp/music. Path supports Chinese characters directly; auto-encoded.', source_refresh: 'Refresh', source_edit: 'Edit',
    source_add: 'Add', source_save: 'Save', source_cancel: 'Cancel edit', source_del: 'Delete this source',
    source_none: 'No source', shuffle: 'Shuffle', play_all: 'Play all', prev: 'Previous', stop: 'Stop',
    play: 'Play', pause: 'Pause', next: 'Next', not_playing: 'Not playing', unknown_track: 'Unknown track',
    no_tracks: 'No audio files in this source', page_prev: 'Prev', page_next: 'Next', volume: 'Volume',
    token_prompt: 'Enter access token', token_invalid: 'Invalid token', delete_confirm: 'Delete current source?',
    track_count: 'track', truncated: '(first 500 shown)',
    nav_home: '🏠 Home', nav_radio: '📻 Radio'
  }
};
const tr = (key) => (I18N[lang] && I18N[lang][key]) || I18N.en[key] || key;

function setLang() {
  lang = lang === 'zh' ? 'en' : 'zh';
  document.documentElement.lang = lang;
  localStorage.setItem(LANG_KEY, lang);
  $('btn-lang').textContent = lang === 'zh' ? 'EN' : '中';
  document.querySelectorAll('[data-i18n]').forEach((el) => {
    const v = (I18N[lang] || I18N.en)[el.getAttribute('data-i18n')];
    if (v) el.textContent = v;
  });
  $('app-title').textContent = tr('app_title');
  renderStatus(playing);
}

$('btn-lang').addEventListener('click', () => setLang());

const esc = (s) => String(s == null ? '' : s).replace(/[&<>"']/g,
  (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));

async function api(path, options = {}) {
  const headers = { 'Content-Type': 'application/json' };
  const res = await fetch(path, Object.assign({}, options, { headers }));
  if (res.status === 401) {
    location.replace('/login?next=' + encodeURIComponent(location.pathname));
    throw new Error('unauthorized');
  }
  return res.json();
}

const mode = () => ($('shuffle').checked ? 'shuffle' : 'sequential');

function markActive() {
  document.querySelectorAll('#tracks .track').forEach((el) => {
    const idx = parseInt(el.dataset.index, 10);
    const on = playing.active && playing.source === sourceIndex && playing.track_index === idx;
    el.classList.toggle('active', on);
    el.querySelector('.idx').textContent = on ? '▶' : String(idx + 1);
  });
}

function fmtTime(ms) {
  const t = Math.max(0, Math.floor(ms || 0));
  const s = Math.floor(t / 1000);
  const h = Math.floor(s / 3600);
  const m = Math.floor((s % 3600) / 60);
  const sec = String(s % 60).padStart(2, '0');
  if (h > 0) return h + ':' + String(m).padStart(2, '0') + ':' + sec;
  return String(m) + ':' + sec;
}

function renderStatus(st) {
  playing = st || { active: false };
  const badge = $('badge');
  if (!playing.active) {
    $('np-title').textContent = tr('not_playing');
    $('btn-toggle').textContent = tr('play');
    badge.textContent = tr('status_idle');
    badge.classList.remove('on');
    $('progress-cur').textContent = '0:00';
    $('progress-rem').textContent = '--:--';
  } else {
    $('np-title').textContent = playing.title || tr('unknown_track');
    $('btn-toggle').textContent = playing.paused ? tr('play') : tr('pause');
    badge.textContent = tr('status_playing');
    badge.classList.add('on');
    if (!playing.single) $('shuffle').checked = playing.mode === 'shuffle';
    const dur_s = (playing.duration_ms || 0) / 1000;
    const dur_ms = playing.duration_ms || 0;
    const pos_ms = playing.position_ms || 0;
    if (dur_s > 0) $('progress').max = Math.round(dur_s);
    $('progress').value = Math.min(parseFloat($('progress').max) || 3600, pos_ms / 1000);
    if (dur_ms > 0) {
      const disp_pos = Math.min(pos_ms, dur_ms);
      $('progress-cur').textContent = fmtTime(disp_pos);
      $('progress-rem').textContent = '-' + fmtTime(dur_ms - disp_pos);
    } else {
      $('progress-cur').textContent = '0:00';
      $('progress-rem').textContent = '--:--';
    }
    if (typeof playing.volume === 'number') $('volume-value').textContent = playing.volume;
  }
  markActive();
}

async function pollStatus() {
  try {
    renderStatus(await api('/api/music/status'));
  } catch (e) { /* 忽略单次失败 */ }
}

function startPoll() {
  if (!pollTimer) pollTimer = setInterval(pollStatus, 1000);
}

function stopPoll() {
  clearInterval(pollTimer);
  pollTimer = null;
}

async function loadTracks() {
  if (!sources.length) {
    $('tracks').innerHTML = '<p class="empty">' + tr('no_source') + '</p>';
    $('page-info').textContent = '-';
    $('btn-page-prev').disabled = true;
    $('btn-page-next').disabled = true;
    return;
  }
  const res = await api('/api/music/tracks?source=' + sourceIndex + '&page=' + page);
  if (res.status !== 'ok') {
    $('tracks').innerHTML = '<p class="empty">' + esc(res.error || tr('loading')) + '</p>';
    return;
  }
  pages = res.pages || 1;
  page = res.page || 1;
  $('page-info').textContent = page + '/' + pages + ' · ' + (res.total || 0) + ' ' + tr('track_count') +
    (res.truncated ? ' · ' + tr('truncated') : '');
  $('btn-page-prev').disabled = page <= 1;
  $('btn-page-next').disabled = page >= pages;
  $('tracks').innerHTML = (res.tracks || []).map((t) =>
    '<div class="track" data-index="' + t.index + '">' +
      '<span class="idx">' + (t.index + 1) + '</span>' +
      '<span class="name">' + esc(t.title) + '</span>' +
    '</div>').join('') || '<p class="empty">' + tr('no_tracks') + '</p>';
  markActive();
}

async function loadSources() {
  const res = await api('/api/music/sources');
  sources = res.sources || [];
  sourceIndex = Number.isInteger(res.selected_index) ? res.selected_index : 0;
  const sel = $('source');
  if (!sources.length) {
    sel.innerHTML = '<option value="">' + tr('source_none') + '</option>';
    sourceIndex = 0;
    $('src-details').open = true;
  } else {
    if (sourceIndex >= sources.length) sourceIndex = 0;
    sel.innerHTML = sources.map((s, i) =>
      '<option value="' + i + '">' + esc((s.base_url || '') + (s.path || '')) + '</option>').join('');
    sel.value = String(sourceIndex);
  }
  await loadTracks();
}

function loadSourceForEdit(idx) {
  if (idx >= 0 && idx < sources.length) {
    setSourceFormMode(true, idx);
  }
}

async function playAll(start) {
  if (!sources.length) return;
  await api('/api/music/source', {
    method: 'POST',
    body: JSON.stringify({ index: sourceIndex, start: start || 0 })
  });
  await pollStatus();
}

async function playTrack(index) {
  if (playing.active && playing.source === sourceIndex && !playing.single) {
    await api('/api/music/track', { method: 'POST', body: JSON.stringify({ index }) });
  } else {
    await playAll(index);
  }
  await pollStatus();
}

$('tracks').addEventListener('click', (e) => {
  const row = e.target.closest('.track');
  if (row) playTrack(parseInt(row.dataset.index, 10));
});

$('btn-play-all').addEventListener('click', () => playAll(0));
$('btn-toggle').addEventListener('click', async () => {
  if (playing.active && !playing.paused) {
    await api('/api/music/pause', { method: 'POST' });
  } else if (playing.paused) {
    await api('/api/music/resume', { method: 'POST' });
  } else {
    await playAll(0);
  }
  await pollStatus();
});
$('progress').addEventListener('change', async (e) => {
  const time_ms = parseInt(e.target.value, 10) * 1000;
  await api('/api/music/seek', { method: 'POST', body: JSON.stringify({ time_ms }) });
  await pollStatus();
});
$('btn-stop').addEventListener('click', async () => {
  if (!playing.active) return;
  await api('/api/music/stop', { method: 'POST' });
  await pollStatus();
});
$('btn-next').addEventListener('click', async () => {
  if (!playing.active) return;
  await api('/api/music/next', { method: 'POST' });
  await pollStatus();
});
$('btn-prev').addEventListener('click', async () => {
  if (!playing.active) return;
  await api('/api/music/prev', { method: 'POST' });
  await pollStatus();
});
$('shuffle').addEventListener('change', async () => {
  await api('/api/music/mode', { method: 'POST', body: JSON.stringify({ mode: mode() }) });
  await pollStatus();
});
async function setVolume(v) {
  volume = Math.max(0, Math.min(100, v));
  await api('/api/music/volume', { method: 'POST', body: JSON.stringify({ volume }) });
  $('volume-value').textContent = volume;
}
$('btn-vol-up').addEventListener('click', () => setVolume(volume + 10));
$('btn-vol-down').addEventListener('click', () => setVolume(volume - 10));
$('btn-page-prev').addEventListener('click', () => {
  if (page > 1) { page--; loadTracks(); }
});
$('btn-page-next').addEventListener('click', () => {
  if (page < pages) { page++; loadTracks(); }
});
$('btn-reload').addEventListener('click', () => { page = 1; loadSources(); });
$('btn-edit-source').addEventListener('click', () => {
  loadSourceForEdit(sourceIndex);
  $('src-details').open = true;
});
$('source').addEventListener('change', async () => {
  sourceIndex = parseInt($('source').value, 10) || 0;
  await api('/api/music/selected_source', {
    method: 'POST', body: JSON.stringify({ index: sourceIndex })
  }).catch(() => {});
  page = 1;
  loadTracks();
});

function setSourceFormMode(editing, idx) {
  const form = $('source-form');
  const submitBtn = $('src-add');
  const cancelBtn = $('src-cancel');
  const editIndex = $('source-edit-index');

  if (editing && idx >= 0 && idx < sources.length) {
    const s = sources[idx];
    editIndex.value = String(idx);
    $('base-url').value = s.base_url || '';
    $('path').value = s.path || '/dav';
    $('username').value = s.username || '';
    $('password').value = s.password || '';
    submitBtn.textContent = tr('source_save');
    cancelBtn.removeAttribute('style');
  } else {
    editIndex.value = '-1';
    form.reset();
    $('path').value = '/dav';
    submitBtn.textContent = tr('source_add');
    cancelBtn.style.display = 'none';
  }
}

$('source-form').addEventListener('submit', async (e) => {
  e.preventDefault();
  const res = await api('/api/music/sources');
  const list = res.sources || [];
  const editIdx = parseInt($('source-edit-index').value, 10);
  const source = {
    type: 'webdav',
    base_url: $('base-url').value.trim(),
    path: $('path').value.trim() || '/',
    username: $('username').value.trim(),
    password: $('password').value
  };
  if (editIdx >= 0 && editIdx < list.length) {
    list[editIdx] = source;
  } else {
    list.push(source);
  }
  await api('/api/music/sources', { method: 'POST', body: JSON.stringify(list) });
  setSourceFormMode(false, -1);
  if (editIdx < 0) sourceIndex = list.length - 1;
  await loadSources();
});

$('src-cancel').addEventListener('click', () => {
  setSourceFormMode(false, -1);
});

$('src-del').addEventListener('click', async () => {
  if (!sources.length) return;
  if (!confirm(tr('delete_confirm'))) return;
  const res = await api('/api/music/sources');
  const list = res.sources || [];
  list.splice(sourceIndex, 1);
  await api('/api/music/sources', { method: 'POST', body: JSON.stringify(list) });
  sourceIndex = 0;
  page = 1;
  await loadSources();
});

document.addEventListener('visibilitychange', () => {
  if (document.hidden) {
    stopPoll();
  } else {
    pollStatus();
    startPoll();
  }
});

(async () => {
  await loadSources();
  await pollStatus();
  startPoll();
})();

)JS";

}  // namespace

WebUIRenderer::WebUIRenderer(const WebUIProvider& provider) : provider_(provider), lang_("zh-CN") {}

void WebUIRenderer::RegisterRoutes(HandlerRegistry& registry) {
    registry.Register({
        .uri = "/",
        .method = HTTP_GET,
        .handler = HomeHandler,
        .user_ctx = const_cast<WebUIRenderer*>(this),
        .is_websocket = false
    }, "WebUI home page");
    registry.Register({
        .uri = "/music",
        .method = HTTP_GET,
        .handler = IndexHandler,
        .user_ctx = const_cast<WebUIRenderer*>(this),
        .is_websocket = false
    }, "WebUI music page");
}

std::string WebUIRenderer::GenerateHomeHtml() const {
    return kHomeHtmlTemplate;
}

esp_err_t WebUIRenderer::HomeHandler(httpd_req_t* req) {
    auto* self = static_cast<WebUIRenderer*>(req->user_ctx);
    if (!self) return ESP_FAIL;
    if (!web_auth::IsAuthorized(req)) {
        web_auth::RedirectToLogin(req, "/");
        return ESP_OK;
    }
    std::string html = self->GenerateHomeHtml();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html.c_str(), html.size());
}

std::string WebUIRenderer::GenerateHtml() const {
    std::string html = kIndexHtmlTemplate;

    auto replace = [&html](const std::string& placeholder, const std::string& value) {
        size_t pos = 0;
        while ((pos = html.find(placeholder, pos)) != std::string::npos) {
            html.replace(pos, placeholder.length(), value);
            pos += value.length();
        }
    };

    replace("{{APP_TITLE}}", "小智音乐");
    replace("{{STYLE_CSS}}", kStyleCss);
    replace("{{APP_JS}}", kAppJs);
    replace("{{LANG}}", lang_);

    std::string extraCss = provider_.GetThemeCSS();
    if (!extraCss.empty()) {
        size_t pos = html.find("</style>");
        if (pos != std::string::npos) {
            html.insert(pos, extraCss);
        }
    }

    return html;
}

esp_err_t WebUIRenderer::IndexHandler(httpd_req_t* req) {
    auto* self = static_cast<WebUIRenderer*>(req->user_ctx);
    if (!self) return ESP_FAIL;
    if (!web_auth::IsAuthorized(req)) {
        web_auth::RedirectToLogin(req, "/music");
        return ESP_OK;
    }

    std::string html = self->GenerateHtml();
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, html.c_str(), html.size());
}
