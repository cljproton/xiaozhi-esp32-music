#include "config_api_handler.h"

#include "application.h"
#include "board.h"
#include "display.h"
#include "web_auth.h"
#include "standby/standby_storage.h"
#include "weather/weather_service.h"
#include "power_save_timer.h"
#include "settings.h"
#include "cjson_utils.h"
#include "display/lvgl_display/lvgl_image.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_http_client.h>
#include <esp_crt_bundle.h>
#include <esp_log.h>
#include <cstring>
#include <string>
#include <algorithm>
#include <vector>

#include "display/lvgl_display/jpg/jpeg_to_image.h"

#define TAG "ConfigApi"

namespace {

// HTML template for config page
const char* kConfigHtml = R"HTML(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>配置 - 小智</title>
<style>
:root{--bg:#070b14;--card:#111827;--line:#26334a;--fg:#e6edf7;--dim:#94a3b8;--acc:#22d3ee;--warn:#f88}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:-apple-system,BlinkMacSystemFont,"Segoe UI",Roboto,"PingFang SC",sans-serif;background:var(--bg);color:var(--fg);min-height:100vh;padding:20px 14px}
.app{max-width:640px;margin:0 auto}
header{display:flex;align-items:center;justify-content:space-between;margin-bottom:20px}
h1{font-size:1.5rem}
h2{font-size:1.1rem;margin-bottom:6px}
p{color:var(--dim);font-size:14px;line-height:1.7;margin-bottom:18px}
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:20px;margin-bottom:16px}
label{display:block;font-size:13px;color:var(--dim);margin-bottom:5px}
input,select{width:100%;background:var(--bg);border:1px solid var(--line);color:var(--fg);border-radius:8px;padding:9px;font-size:14px;margin-bottom:10px}
button{background:var(--acc);color:#06202a;border:0;border-radius:8px;padding:9px 16px;font-size:14px;font-weight:600;cursor:pointer}
button:disabled{opacity:0.4;cursor:default}
button.danger{background:#ef4444;color:#fff}
.row{display:flex;align-items:center;gap:8px}
.check{display:flex;align-items:center;gap:8px;font-size:14px;color:var(--fg)}
.check input{width:18px;height:18px;accent-color:var(--acc)}
#msg{color:#f88;font-size:13px;min-height:18px;margin-top:8px}
.ok{color:#4ade80}
.hint{color:var(--dim);font-size:12px;line-height:1.6;margin-top:4px}
.preview{margin-top:12px;text-align:center}
.preview img{max-width:100%;max-height:200px;border-radius:8px;border:1px solid var(--line)}
.hidden{display:none}
.note{background:rgba(34,211,238,0.1);border:1px solid rgba(34,211,238,0.3);border-radius:8px;padding:10px;margin-top:10px;font-size:13px;color:#9dc0ff}
.top-nav{display:flex;gap:10px;margin-bottom:16px}
.top-nav a{color:var(--dim);text-decoration:none;font-size:14px}
.top-nav a:hover{color:var(--acc)}
</style>
</head>
<body>
<div class="app">
  <div class="top-nav"><a href="/">🏠 首页</a><a href="/music">🎵 音乐</a><a href="/radio">📻 电台</a></div>
  <header><h1>设备配置</h1></header>

  <div class="card">
    <h2>修改密码</h2>
    <label for="old">当前密码</label><input id="old" type="password" autocomplete="current-password">
    <label for="new">新密码</label><input id="new" type="password" autocomplete="new-password">
    <button id="btn-pwd" type="button">修改密码</button>
    <div id="msg-pwd"></div>
  </div>

  <div class="card">
    <h2>睡眠模式</h2>
    <div class="check"><input type="checkbox" id="sleep-enable"><label for="sleep-enable">启用睡眠模式</label></div>
    <div id="sleep-options" class="hidden">
      <label for="sleep-off-at">每天关机时间 (HH:MM，留空不定时关机)</label>
      <input id="sleep-off-at" type="time" step="60">
      <label for="sleep-off-min">空闲多少分钟后关机 (0=不关机)</label>
      <input id="sleep-off-min" type="number" min="0" max="1440" step="1" placeholder="例如 60">
      <div class="hint">• 到点关机：每天固定时刻断电（充电时也会关机）<br>• 空闲关机：待机无操作 N 分钟后断电，有交互重新计时</div>
    </div>
    <button id="btn-sleep" type="button" style="margin-top:10px">保存睡眠设置</button>
    <div id="msg-sleep"></div>
  </div>

  <div class="card">
    <h2>待机背景图</h2>
    <div class="note">上传文件：支持 PNG/WebP/JPEG 等任意格式，自动裁剪 292×240 并压缩至 ≤16 KB。<br>网址图片：仅支持 Baseline JPEG（设备端硬件解码限制）。</div>
    <div class="tabs" style="margin-bottom:10px">
      <label><input type="radio" name="bg-source" value="file" checked> 📁 上传文件</label>
      <label><input type="radio" name="bg-source" value="url"> 🔗 网址图片</label>
    </div>
    <div id="bg-file-section">
      <label for="bg-file">选择图片</label>
      <input id="bg-file" type="file" accept="image/*">
    </div>
    <div id="bg-url-section" class="hidden">
      <label for="bg-url">图片网址 (https://...)</label>
      <input id="bg-url" type="url" placeholder="https://example.com/image.jpg">
      <div class="hint" id="bg-url-hint"></div>
    </div>
    <div class="preview" id="bg-preview"></div>
    <div class="hint" id="bg-hint"></div>
    <div class="row" style="margin-top:10px">
      <button id="btn-bg-save" type="button" class="hidden" style="flex:1">保存</button>
      <button id="btn-bg-del" type="button" class="danger" style="flex:1">删除背景图</button>
    </div>
    <div id="msg-bg"></div>
  </div>

  <div class="card">
    <h2>待机天气</h2>
    <label for="weather-city">城市名称（留空=按 IP 自动定位）</label>
    <input id="weather-city" type="text" placeholder="例如：北京、Shanghai、Tokyo">
    <button id="btn-weather" type="button">保存城市</button>
    <div id="msg-weather"></div>
  </div>
</div>
<script>
const $=id=>document.getElementById(id);
const T={pwd:{ok:'密码已修改',fail:'修改失败，请检查当前密码'},sleep:{ok:'睡眠设置已保存',fail:'保存失败'},bg:{ok:'背景图已更新',fail:'上传失败',del:'已删除',url_ok:'网址图片已应用',url_fail:'网址图片应用失败'},weather:{ok:'城市已保存',fail:'保存失败'}};

async function api(path, opt={}) {
  const r=await fetch(path,{...opt,headers:{'Content-Type':'application/json',...(opt.headers||{})}});
  if(r.status===401) location.replace('/login?next='+encodeURIComponent(location.pathname));
  return r.json().catch(()=>({}));
}

function showMsg(id, text, ok) { const el=$(id); el.textContent=text; el.className=ok?'ok':''; }

// Password
$('btn-pwd').addEventListener('click',async()=>{
  const d=await api('/api/auth',{method:'POST',body:JSON.stringify({action:'change',old_password:$('old').value,new_password:$('new').value})});
  showMsg('msg-pwd',d.status==='ok'?T.pwd.ok:T.pwd.fail,d.status==='ok');
  $('old').value='';$('new').value='';
});

// Sleep mode
const se=$('sleep-enable'), so=$('sleep-options');
se.addEventListener('change',()=>so.classList.toggle('hidden',!se.checked));
async function loadSleep(){
  const d=await api('/api/config');
  if(d.sleep_mode!==undefined){ se.checked=d.sleep_mode; so.classList.toggle('hidden',!d.sleep_mode); }
  if(d.sleep_off_at!==undefined&&d.sleep_off_at>=0){
    const h=Math.floor(d.sleep_off_at/60).toString().padStart(2,'0');
    const m=(d.sleep_off_at%60).toString().padStart(2,'0');
    $('sleep-off-at').value=h+':'+m;
  }
  if(d.sleep_off_min!==undefined && d.sleep_off_min>=0) $('sleep-off-min').value=d.sleep_off_min;
}
$('btn-sleep').addEventListener('click',async()=>{
  const body={sleep_mode:se.checked};
  if(se.checked){
    const t=$('sleep-off-at').value;
    if(t){ const [h,m]=t.split(':').map(Number); body.sleep_off_at=h*60+m; }
    else body.sleep_off_at=-1;
    body.sleep_off_min=parseInt($('sleep-off-min').value)||0;
  }
  const d=await api('/api/config',{method:'POST',body:JSON.stringify(body)});
  showMsg('msg-sleep',d.status==='ok'?T.sleep.ok:T.sleep.fail,d.status==='ok');
});
loadSleep();

// Background image
const preview=$('bg-preview'), hint=$('bg-hint');
let pendingBgB64 = null;  // 暂存待保存的图片

function renderPreview(jpeg_b64, url){
  preview.innerHTML=jpeg_b64?'<img src="data:image/jpeg;base64,'+jpeg_b64+'">':(url?'<img src="'+url+'">':'');
  hint.textContent=jpeg_b64?'已保存到设备':(url?'使用网址图片（开机将缓存到设备）':'无背景图');
}
async function loadBg(){
  const d=await api('/api/config/background');
  if(d.jpeg_b64) renderPreview(d.jpeg_b64);
  else if(d.url) renderPreview('',d.url);
  else renderPreview('');
  // 隐藏保存按钮（有已保存图片时）
  $('btn-bg-save').classList.add('hidden');
  // 初始同步 radio 选中状态（仅首次加载时，不覆盖用户手动选择）
  if (!window.bgModeInitialized) {
    const hasFile = !!d.jpeg_b64;
    const hasUrl = !!d.url;
    if (hasFile) $('input[name=bg-source][value=file]').checked = true;
    else if (hasUrl) $('input[name=bg-source][value=url]').checked = true;
    setBgMode(hasUrl ? 'url' : 'file');
    window.bgModeInitialized = true;
  }
}
function showPendingPreview(b64){
  preview.innerHTML='<img src="data:image/jpeg;base64,'+b64+'">';
  hint.textContent='预览图（点击保存写入设备）';
  $('btn-bg-save').classList.remove('hidden');
}
function setBgMode(mode){
  // mode: 'file' | 'url'
  const fileSection = $('bg-file-section');
  const urlSection = $('bg-url-section');
  if (mode === 'file'){
    fileSection.classList.remove('hidden');
    urlSection.classList.add('hidden');
    $('bg-url').value = '';
    $('bg-url-hint').textContent = '';
  }else{
    fileSection.classList.add('hidden');
    urlSection.classList.remove('hidden');
    $('bg-file').value = '';
  }
  pendingBgB64 = null;
  $('btn-bg-save').classList.add('hidden');
  // 不再调用 loadBg()，避免循环覆盖用户选择
}
async function processImageToJpeg(img){
  return new Promise((resolve, reject) => {
    const canvas=document.createElement('canvas');
    const W=292, H=240;
    canvas.width=W; canvas.height=H;
    const ctx=canvas.getContext('2d');
    // Fill white background first (handles transparency)
    ctx.fillStyle = '#FFFFFF';
    ctx.fillRect(0, 0, W, H);
    const scale=Math.max(W/img.width, H/img.height);
    const dw=img.width*scale, dh=img.height*scale;
    ctx.drawImage(img, (W-dw)/2, (H-dh)/2, dw, dh);
    // More aggressive quality steps for complex images
    const steps=[0.7,0.6,0.5,0.42,0.35,0.28,0.2,0.15,0.1];
    let tried=0;
    function tryStep(){
      if(tried>=steps.length){ reject(new Error('图片太复杂，无法压缩至16KB')); return; }
      canvas.toBlob(blob=>{
        if(blob && blob.size<=16384){
          const fr=new FileReader();
          fr.onload=()=>resolve(fr.result.split(',')[1]);
          fr.readAsDataURL(blob);
        }else{
          tried++; tryStep();
        }
      },'image/jpeg',steps[tried]);
    }
    tryStep();
  });
}
// 文件上传
$('bg-file').addEventListener('change',async e=>{
  const f=e.target.files[0]; if(!f) return;
  const img=new Image();
  img.onload=async()=>{
    try{
      showMsg('msg-bg','处理中…',true);
      const b64=await processImageToJpeg(img);
      pendingBgB64 = b64;
      showPendingPreview(b64);
      showMsg('msg-bg','预览已生成，点击"保存"写入设备',true);
    }catch(err){
      showMsg('msg-bg',err.message,false);
    }
  };
  img.src=URL.createObjectURL(f);
});
// URL 输入失焦 -> 预览
$('bg-url').addEventListener('blur',async e=>{
  const url=$('bg-url').value.trim(); if(!url) return;
  if(!/^https?:\/\//i.test(url)){
    $('bg-url-hint').textContent = '请输入 http/https 网址';
    return;
  }
  $('bg-url-hint').textContent = '下载中…';
  async function fetchAndProcess(){
    // Try CORS first
    try{
      const resp=await fetch(url,{mode:'cors'});
      if(resp.ok){
        const blob=await resp.blob();
        if(/^image\//.test(blob.type)){
          const img=new Image();
          await new Promise((res,rej)=>{ img.onload=res; img.onerror=rej; img.src=URL.createObjectURL(blob); });
          return img;
        }
      }
    }catch(e){ /* CORS failed, fall back to server proxy */ }
    // Fallback: server-side fetch
    $('bg-url-hint').textContent = '跨域受限，尝试服务端代理…';
    const d=await api('/api/config/background_fetch',{method:'POST',body:JSON.stringify({url})});
    if(d.status==='ok' && d.jpeg_b64){
      return d.jpeg_b64;  // base64 string, not Image
    }
    throw new Error(d.error || '服务端获取失败');
  }
  try{
    const result=await fetchAndProcess();
    if(typeof result === 'string'){  // base64 from server
      pendingBgB64 = result;
      showPendingPreview(result);
      showMsg('msg-bg','预览已生成（服务端代理），点击"保存"写入设备',true);
    }else{  // Image from CORS
      showMsg('msg-bg','处理中…',true);
      const b64=await processImageToJpeg(result);
      pendingBgB64 = b64;
      showPendingPreview(b64);
      showMsg('msg-bg','预览已生成，点击"保存"写入设备',true);
    }
    $('bg-url-hint').textContent = '';
  }catch(err){
    showMsg('msg-bg',err.message,false);
    $('bg-url-hint').textContent = '';
  }
});
// Radio 切换
document.querySelectorAll('input[name=bg-source]').forEach(r=>{
  r.addEventListener('change',()=>setBgMode(r.value));
});
// 统一保存
$('btn-bg-save').addEventListener('click',async()=>{
  if(!pendingBgB64) return;
  const d=await api('/api/config/background',{method:'POST',body:JSON.stringify({jpeg_b64:pendingBgB64})});
  const msg = d.status==='ok' ? T.bg.ok : (d.error || T.bg.fail);
  showMsg('msg-bg', msg, d.status==='ok');
  if(d.status==='ok'){
    pendingBgB64 = null;
    loadBg();
  }
});
$('btn-bg-del').addEventListener('click',async()=>{
  const d=await api('/api/config/background',{method:'DELETE'});
  showMsg('msg-bg',d.status==='ok'?T.bg.del:T.bg.fail,d.status==='ok');
  if(d.status==='ok'){ loadBg(); }
});
loadBg();

// Weather city
async function loadWeather(){
  const d=await api('/api/config');
  if(d.weather_city!==undefined) $('weather-city').value=d.weather_city||'';
}
$('btn-weather').addEventListener('click',async()=>{
  const d=await api('/api/config',{method:'POST',body:JSON.stringify({weather_city:$('weather-city').value.trim()})});
  showMsg('msg-weather',d.status==='ok'?T.weather.ok:T.weather.fail,d.status==='ok');
});
loadWeather();
</script>
</body>
</html>)HTML";

// Base64 decode helper
static size_t b64_decoded_len(const char* s) {
    size_t len = strlen(s);
    while (len > 0 && s[len-1] == '=') len--;
    return len * 3 / 4;
}

static int b64_decode_char(char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

static bool base64_decode(const char* in, uint8_t* out, size_t out_max, size_t& out_len) {
    size_t in_len = strlen(in);
    // Skip trailing padding chars
    while (in_len > 0 && in[in_len - 1] == '=') in_len--;
    out_len = 0;
    for (size_t i = 0; i + 3 < in_len; i += 4) {
        int a = b64_decode_char(in[i]);
        int b = b64_decode_char(in[i+1]);
        int c = b64_decode_char(in[i+2]);
        int d = b64_decode_char(in[i+3]);
        if (a < 0 || b < 0 || c < 0 || d < 0) return false;
        if (out_len + 3 > out_max) return false;
        out[out_len++] = (a << 2) | (b >> 4);
        out[out_len++] = ((b & 0xF) << 4) | (c >> 2);
        out[out_len++] = ((c & 0x3) << 6) | d;
    }
    return true;
}

esp_err_t SendJson(httpd_req_t* req, cJSON* root, const char* status = "200 OK") {
    CJsonStringUniquePtr printed(cJSON_PrintUnformatted(root));
    cJSON_Delete(root);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    const char* body = printed ? printed.get() : "{}";
    return httpd_resp_send(req, body, strlen(body));
}

esp_err_t SendError(httpd_req_t* req, const char* status, const char* message) {
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "error");
    cJSON_AddStringToObject(root, "error", message);
    return SendJson(req, root, status);
}

std::string ReadBody(httpd_req_t* req) {
    int remaining = req->content_len;
    if (remaining <= 0 || remaining > 65536) return "";
    std::string body(static_cast<size_t>(remaining), '\0');
    int received = 0;
    while (remaining > 0) {
        int n = httpd_req_recv(req, &body[static_cast<size_t>(received)], remaining);
        if (n <= 0) return "";
        received += n;
        remaining -= n;
    }
    return body;
}

// GET /config - config page
esp_err_t HandleConfigPage(httpd_req_t* req) {
    if (!web_auth::IsAuthorized(req)) {
        web_auth::RedirectToLogin(req, "/config");
        return ESP_OK;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, kConfigHtml, strlen(kConfigHtml));
}

// GET /api/config - read all config
esp_err_t HandleConfigGet(httpd_req_t* req) {
    if (!web_auth::IsAuthorized(req)) return SendError(req, "401 Unauthorized", "unauthorized");

    Settings settings("wifi", false);
    bool sleep_mode = settings.GetBool("sleep_mode", true);
    int sleep_off_min = settings.GetInt("sleep_off_min", 0);  // 0 = disabled (default)
    int sleep_off_at = settings.GetInt("sleep_off_at", -1);  // minutes from midnight, -1=disabled

    Settings wns("weather", false);
    std::string city = wns.GetString("city", "");

    size_t free_kb = standby::GetNvsFreeBytes() / 1024;

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddBoolToObject(root, "sleep_mode", sleep_mode);
    cJSON_AddNumberToObject(root, "sleep_off_min", sleep_off_min);
    cJSON_AddNumberToObject(root, "sleep_off_at", sleep_off_at);
    cJSON_AddStringToObject(root, "weather_city", city.c_str());
    cJSON_AddNumberToObject(root, "nvs_free_kb", static_cast<int>(free_kb));
    cJSON_AddBoolToObject(root, "has_bg_image", standby::HasBackgroundImage());
    cJSON_AddBoolToObject(root, "has_bg_url", standby::HasBackgroundUrl());
    if (standby::HasBackgroundUrl()) {
        cJSON_AddStringToObject(root, "bg_url", standby::GetBackgroundUrl().c_str());
    }
    return SendJson(req, root);
}

// POST /api/config - write config
esp_err_t HandleConfigPost(httpd_req_t* req) {
    if (!web_auth::IsAuthorized(req)) return SendError(req, "401 Unauthorized", "unauthorized");

    cJSON* root = cJSON_Parse(ReadBody(req).c_str());
    if (!root) return SendError(req, "400 Bad Request", "invalid JSON");

    // sleep_mode
    cJSON* item = cJSON_GetObjectItem(root, "sleep_mode");
    if (cJSON_IsBool(item)) {
        Settings settings("wifi", true);
        settings.SetBool("sleep_mode", cJSON_IsTrue(item));
    }

    // sleep_off_at (minutes from midnight, -1 = disabled)
    item = cJSON_GetObjectItem(root, "sleep_off_at");
    if (cJSON_IsNumber(item)) {
        int val = item->valueint;
        if (val >= -1 && val <= 1439) {
            Settings settings("wifi", true);
            settings.SetInt("sleep_off_at", val);
        }
    }

    // sleep_off_min (idle minutes, 0 = disabled)
    item = cJSON_GetObjectItem(root, "sleep_off_min");
    if (cJSON_IsNumber(item)) {
        int val = item->valueint;
        if (val >= 0 && val <= 1440) {
            Settings settings("wifi", true);
            settings.SetInt("sleep_off_min", val);
        }
    }

    // weather_city
    item = cJSON_GetObjectItem(root, "weather_city");
    if (cJSON_IsString(item)) {
        weather::SetCity(item->valuestring ? item->valuestring : "");
    }

    cJSON_Delete(root);
    cJSON* ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    return SendJson(req, ok);
}

// GET /api/config/background - preview
esp_err_t HandleBgGet(httpd_req_t* req) {
    if (!web_auth::IsAuthorized(req)) return SendError(req, "401 Unauthorized", "unauthorized");

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    if (standby::HasBackgroundImage()) {
        std::string jpeg;
        standby::LoadBackgroundImage(jpeg);
        // base64 encode
        static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded;
        encoded.reserve((jpeg.size() + 2) / 3 * 4);
        for (size_t i = 0; i < jpeg.size(); i += 3) {
            uint32_t v = jpeg[i] << 16;
            if (i + 1 < jpeg.size()) v |= jpeg[i+1] << 8;
            if (i + 2 < jpeg.size()) v |= jpeg[i+2];
            encoded.push_back(b64[(v >> 18) & 0x3F]);
            encoded.push_back(b64[(v >> 12) & 0x3F]);
            encoded.push_back(i + 1 < jpeg.size() ? b64[(v >> 6) & 0x3F] : '=');
            encoded.push_back(i + 2 < jpeg.size() ? b64[v & 0x3F] : '=');
        }
        cJSON_AddStringToObject(root, "jpeg_b64", encoded.c_str());
    } else if (standby::HasBackgroundUrl()) {
        cJSON_AddStringToObject(root, "url", standby::GetBackgroundUrl().c_str());
    }
    return SendJson(req, root);
}

// POST /api/config/background - upload JPEG (base64)
esp_err_t HandleBgPost(httpd_req_t* req) {
    if (!web_auth::IsAuthorized(req)) return SendError(req, "401 Unauthorized", "unauthorized");

    cJSON* root = cJSON_Parse(ReadBody(req).c_str());
    if (!root) return SendError(req, "400 Bad Request", "invalid JSON");

    cJSON* item = cJSON_GetObjectItem(root, "jpeg_b64");
    if (!cJSON_IsString(item) || !item->valuestring) {
        cJSON_Delete(root);
        return SendError(req, "400 Bad Request", "missing jpeg_b64");
    }

    const char* b64 = item->valuestring;
    size_t max_len = b64_decoded_len(b64);
    if (max_len == 0 || max_len > 16384) {
        cJSON_Delete(root);
        return SendError(req, "400 Bad Request", "image too large (>16KB)");
    }

    std::vector<uint8_t> buf(max_len);
    size_t dec_len = 0;
    if (!base64_decode(b64, buf.data(), max_len, dec_len)) {
        cJSON_Delete(root);
        return SendError(req, "400 Bad Request", "invalid base64");
    }
    buf.resize(dec_len);

    // Verify it's a valid JPEG by attempting to decode
    // (jpeg_to_image_fit will fail if not baseline JPEG)
    uint8_t* out = nullptr;
    size_t out_len = 0, w = 0, h = 0, stride = 0;
    esp_err_t err = jpeg_to_image_fit(buf.data(), dec_len, 292, 240, &out, &out_len, &w, &h, &stride);
    if (out) heap_caps_free(out);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return SendError(req, "400 Bad Request", "invalid JPEG (must be baseline JPEG)");
    }

    // Save
    if (!standby::SaveBackgroundImage(buf.data(), dec_len)) {
        cJSON_Delete(root);
        return SendError(req, "500 Internal Error", "storage full: clean up other data");
    }
    // Clear URL when uploading local image
    standby::ClearBackgroundUrl();

    // 立即在主任务刷新待机画面，让用户当场看到效果
    std::string jpeg_data(reinterpret_cast<const char*>(buf.data()), buf.size());
    Application::GetInstance().Schedule([jpeg_data]() {
        auto display = Board::GetInstance().GetDisplay();
        if (display == nullptr) return;
        uint8_t* out = nullptr;
        size_t out_len = 0, w = 0, h = 0, stride = 0;
        if (jpeg_to_image_fit(reinterpret_cast<const uint8_t*>(jpeg_data.data()), jpeg_data.size(),
                              292, 240, &out, &out_len, &w, &h, &stride) == ESP_OK) {
            display->SetStandbyImage(std::make_unique<LvglAllocatedImage>(
                out, out_len, w, h, stride, LV_COLOR_FORMAT_RGB565));
            display->SetStandby(true);
        }
    });

    cJSON_Delete(root);
    cJSON* ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    return SendJson(req, ok);
}

// POST /api/config/background_url - set URL
esp_err_t HandleBgUrlPost(httpd_req_t* req) {
    if (!web_auth::IsAuthorized(req)) return SendError(req, "401 Unauthorized", "unauthorized");

    cJSON* root = cJSON_Parse(ReadBody(req).c_str());
    if (!root) return SendError(req, "400 Bad Request", "invalid JSON");

    cJSON* item = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(item) || !item->valuestring) {
        cJSON_Delete(root);
        return SendError(req, "400 Bad Request", "missing url");
    }
    std::string url = item->valuestring;
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        cJSON_Delete(root);
        return SendError(req, "400 Bad Request", "URL must be http(s)");
    }

    // Save URL (device will download and cache on next boot/save)
    if (!standby::SetBackgroundUrl(url)) {
        cJSON_Delete(root);
        return SendError(req, "500 Internal Error", "failed to save URL");
    }
    // Clear local image when using URL
    standby::ClearBackgroundImage();

    cJSON_Delete(root);
    cJSON* ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    return SendJson(req, ok);
}

// DELETE /api/config/background - clear
esp_err_t HandleBgDelete(httpd_req_t* req) {
    if (!web_auth::IsAuthorized(req)) return SendError(req, "401 Unauthorized", "unauthorized");

    standby::ClearBackgroundImage();
    standby::ClearBackgroundUrl();

    Application::GetInstance().Schedule([]() {
        auto display = Board::GetInstance().GetDisplay();
        if (display != nullptr) display->SetStandbyImage(nullptr);
    });

    cJSON* ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    return SendJson(req, ok);
}

// POST /api/config/background_fetch - fetch image from URL (server-side, bypass CORS)
esp_err_t HandleBgFetchPost(httpd_req_t* req) {
    if (!web_auth::IsAuthorized(req)) return SendError(req, "401 Unauthorized", "unauthorized");

    cJSON* root = cJSON_Parse(ReadBody(req).c_str());
    if (!root) return SendError(req, "400 Bad Request", "invalid JSON");

    cJSON* item = cJSON_GetObjectItem(root, "url");
    if (!cJSON_IsString(item) || !item->valuestring) {
        cJSON_Delete(root);
        return SendError(req, "400 Bad Request", "missing url");
    }
    std::string url = item->valuestring;
    if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) {
        cJSON_Delete(root);
        return SendError(req, "400 Bad Request", "URL must be http(s)");
    }

    // Fetch image from URL
    esp_http_client_config_t config = {};
    config.url = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 15000;
    config.method = HTTP_METHOD_GET;
    config.buffer_size = 8192;
    config.buffer_size_tx = 1024;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        cJSON_Delete(root);
        return SendError(req, "500 Internal Error", "http client init failed");
    }

    std::vector<uint8_t> data;
    esp_err_t err = esp_http_client_open(client, 0);
    if (err == ESP_OK) {
        int64_t content_length = esp_http_client_fetch_headers(client);
        char buf[1024];
        int read;
        while ((read = esp_http_client_read(client, buf, sizeof(buf))) > 0) {
            data.insert(data.end(), buf, buf + read);
        }
    }
    esp_http_client_cleanup(client);

    if (data.empty()) {
        cJSON_Delete(root);
        return SendError(req, "500 Internal Error", "failed to fetch image");
    }

    // Verify it's a valid JPEG
    uint8_t* out = nullptr;
    size_t out_len = 0, w = 0, h = 0, stride = 0;
    err = jpeg_to_image_fit(data.data(), data.size(), 292, 240, &out, &out_len, &w, &h, &stride);
    if (out) heap_caps_free(out);
    if (err != ESP_OK) {
        cJSON_Delete(root);
        return SendError(req, "400 Bad Request", "invalid JPEG (must be baseline JPEG)");
    }

    // Convert to base64
    static const char* b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string encoded;
    encoded.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3) {
        uint32_t v = data[i] << 16;
        if (i + 1 < data.size()) v |= data[i+1] << 8;
        if (i + 2 < data.size()) v |= data[i+2];
        encoded.push_back(b64[(v >> 18) & 0x3F]);
        encoded.push_back(b64[(v >> 12) & 0x3F]);
        encoded.push_back(i + 1 < data.size() ? b64[(v >> 6) & 0x3F] : '=');
        encoded.push_back(i + 2 < data.size() ? b64[v & 0x3F] : '=');
    }

    cJSON_Delete(root);
    cJSON* ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
    cJSON_AddStringToObject(ok, "jpeg_b64", encoded.c_str());
    return SendJson(req, ok);
}

}  // namespace

namespace config_api {

void RegisterHandlers(HandlerRegistry& registry) {
    registry.Register({
        .uri = "/config",
        .method = HTTP_GET,
        .handler = HandleConfigPage,
        .user_ctx = nullptr,
        .is_websocket = false
    }, "Config page");

    registry.Register({
        .uri = "/api/config",
        .method = HTTP_GET,
        .handler = HandleConfigGet,
        .user_ctx = nullptr,
        .is_websocket = false
    }, "Config get");

    registry.Register({
        .uri = "/api/config",
        .method = HTTP_POST,
        .handler = HandleConfigPost,
        .user_ctx = nullptr,
        .is_websocket = false
    }, "Config set");

    registry.Register({
        .uri = "/api/config/background",
        .method = HTTP_GET,
        .handler = HandleBgGet,
        .user_ctx = nullptr,
        .is_websocket = false
    }, "Background preview");

    registry.Register({
        .uri = "/api/config/background",
        .method = HTTP_POST,
        .handler = HandleBgPost,
        .user_ctx = nullptr,
        .is_websocket = false
    }, "Background upload");

    registry.Register({
        .uri = "/api/config/background_url",
        .method = HTTP_POST,
        .handler = HandleBgUrlPost,
        .user_ctx = nullptr,
        .is_websocket = false
    }, "Background URL");

    registry.Register({
        .uri = "/api/config/background_fetch",
        .method = HTTP_POST,
        .handler = HandleBgFetchPost,
        .user_ctx = nullptr,
        .is_websocket = false
    }, "Background fetch from URL");

    registry.Register({
        .uri = "/api/config/background",
        .method = HTTP_DELETE,
        .handler = HandleBgDelete,
        .user_ctx = nullptr,
        .is_websocket = false
    }, "Background delete");
}

}  // namespace config_api