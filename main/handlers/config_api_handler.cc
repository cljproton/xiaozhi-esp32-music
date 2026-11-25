#include "config_api_handler.h"

#include "web_auth.h"
#include "settings.h"
#include "cjson_utils.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <cstring>
#include <string>

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
.card{background:var(--card);border:1px solid var(--line);border-radius:14px;padding:20px;margin-bottom:16px}
label{display:block;font-size:13px;color:var(--dim);margin-bottom:5px}
input,select{width:100%;background:var(--bg);border:1px solid var(--line);color:var(--fg);border-radius:8px;padding:9px;font-size:14px;margin-bottom:10px}
button{background:var(--acc);color:#06202a;border:0;border-radius:8px;padding:9px 16px;font-size:14px;font-weight:600;cursor:pointer}
button:disabled{opacity:0.4;cursor:default}
.check{display:flex;align-items:center;gap:8px;font-size:14px;color:var(--fg)}
.check input{width:18px;height:18px;accent-color:var(--acc)}
#msg{color:#f88;font-size:13px;min-height:18px;margin-top:8px}
.ok{color:#4ade80}
.hint{color:var(--dim);font-size:12px;line-height:1.6;margin-top:4px}
.hidden{display:none}
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
</div>
<script>
const $=id=>document.getElementById(id);
const T={pwd:{ok:'密码已修改',fail:'修改失败，请检查当前密码'},sleep:{ok:'睡眠设置已保存',fail:'保存失败'}};

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
</script>
</body>
</html>)HTML";

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

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddBoolToObject(root, "sleep_mode", sleep_mode);
    cJSON_AddNumberToObject(root, "sleep_off_min", sleep_off_min);
    cJSON_AddNumberToObject(root, "sleep_off_at", sleep_off_at);
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

    cJSON_Delete(root);
    cJSON* ok = cJSON_CreateObject();
    cJSON_AddStringToObject(ok, "status", "ok");
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
}

}  // namespace config_api