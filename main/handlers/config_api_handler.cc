#include "config_api_handler.h"

#include "web_auth.h"
#include "settings.h"
#include "cjson_utils.h"
#include "network/network_config.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
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
  <div class="top-nav"><a href="/" data-i18n="nav_home">🏠 首页</a><a href="/music" data-i18n="nav_music">🎵 音乐</a><a href="/book" data-i18n="nav_book">📖 电子书</a><a href="/radio" data-i18n="nav_radio">📻 电台</a></div>
  <header><h1 data-i18n="title">设备配置</h1></header>

  <div class="card">
    <h2 data-i18n="lang_title">界面语言</h2>
    <label for="lang-select" data-i18n="lang_label">语言 / Language</label>
    <select id="lang-select"><option value="zh">中文</option><option value="en">English</option></select>
    <div class="hint" data-i18n="lang_hint">切换后所有页面立即使用所选语言。</div>
  </div>

  <div class="card">
    <h2 data-i18n="net_title">静态 IP（下次连接网络后生效）</h2>
    <div class="check"><input type="checkbox" id="net-enable"><label for="net-enable" data-i18n="net_enable">使用静态 IP（关闭则自动获取 DHCP）</label></div>
    <div class="row" style="margin-top:10px"><input id="net-ip" placeholder="192.168.1.150"><input id="net-mask" placeholder="255.255.255.0"></div>
    <div class="row"><input id="net-gw" placeholder="192.168.1.1"><input id="net-dns1" placeholder="DNS1 8.8.8.8"></div>
    <input id="net-dns2" placeholder="DNS2（可选）" data-i18n-ph="dns2_ph">
    <button id="btn-net" type="button" data-i18n="save_net">保存网络设置</button>
    <div class="hint" data-i18n="net_hint">留空的 DNS 不覆盖原值；配置在下次 Wi-Fi 连接或重启后生效。</div>
    <div id="msg-net"></div>
  </div>

  <div class="card">
    <h2 data-i18n="pwd_title">修改密码</h2>
    <label for="old" data-i18n="old_pwd">当前密码</label><input id="old" type="password" autocomplete="current-password">
    <label for="new" data-i18n="new_pwd">新密码</label><input id="new" type="password" autocomplete="new-password">
    <button id="btn-pwd" type="button" data-i18n="change_pwd">修改密码</button>
    <div id="msg-pwd"></div>
  </div>

  <div class="card">
    <h2 data-i18n="sleep_title">睡眠模式</h2>
    <div class="check"><input type="checkbox" id="sleep-enable"><label for="sleep-enable" data-i18n="sleep_enable">启用睡眠模式</label></div>
    <div id="sleep-options" class="hidden">
      <label for="sleep-off-at" data-i18n="sleep_off_at">每天关机时间 (HH:MM，留空不定时关机)</label>
      <input id="sleep-off-at" type="time" step="60">
      <label for="sleep-off-min" data-i18n="sleep_off_min">空闲多少分钟后关机 (0=不关机)</label>
      <input id="sleep-off-min" type="number" min="0" max="1440" step="1" placeholder="例如 60" data-i18n-ph="min_ph">
      <div class="hint" data-i18n="sleep_hint">• 到点关机：每天固定时刻断电<br>• 空闲关机：待机无操作 N 分钟后断电，有交互重新计时<br>• 注意：电池供电时可真正断电；USB 供电时只发出关机信号，设备会继续运行</div>
    </div>
    <button id="btn-sleep" type="button" style="margin-top:10px" data-i18n="save_sleep">保存睡眠设置</button>
    <div id="msg-sleep"></div>
  </div>
</div>
<script>
const $=id=>document.getElementById(id);
const LANG_KEY='lang';
let lang=localStorage.getItem(LANG_KEY)==='en'?'en':'zh';
const I18N={
zh:{nav_home:'🏠 首页',nav_music:'🎵 音乐',nav_book:'📖 电子书',nav_radio:'📻 电台',title:'设备配置',lang_title:'界面语言',lang_label:'语言 / Language',lang_hint:'切换后所有页面立即使用所选语言。',net_title:'静态 IP（下次连接网络后生效）',net_enable:'使用静态 IP（关闭则自动获取 DHCP）',save_net:'保存网络设置',net_hint:'留空的 DNS 不覆盖原值；配置在下次 Wi-Fi 连接或重启后生效。',pwd_title:'修改密码',old_pwd:'当前密码',new_pwd:'新密码',change_pwd:'修改密码',sleep_title:'睡眠模式',sleep_enable:'启用睡眠模式',sleep_off_at:'每天关机时间 (HH:MM，留空不定时关机)',sleep_off_min:'空闲多少分钟后关机 (0=不关机)',sleep_hint:'• 到点关机：每天固定时刻断电<br>• 空闲关机：待机无操作 N 分钟后断电，有交互重新计时<br>• 注意：电池供电时可真正断电；USB 供电时只发出关机信号，设备会继续运行',dns2_ph:'DNS2（可选）',min_ph:'例如 60',save_sleep:'保存睡眠设置',pwd_ok:'密码已修改',pwd_fail:'修改失败，请检查当前密码',sleep_ok:'睡眠设置已保存',sleep_fail:'保存失败',net_ok:'网络设置已保存，下次连接生效',net_fail:'保存失败'},
en:{nav_home:'🏠 Home',nav_music:'🎵 Music',nav_book:'📖 Books',nav_radio:'📻 Radio',title:'Settings',lang_title:'Language',lang_label:'Language',lang_hint:'All pages switch to the selected language immediately.',net_title:'Static IP (applies on next network connection)',net_enable:'Use static IP (off = DHCP)',save_net:'Save network settings',net_hint:'Empty DNS fields keep their previous values. Changes apply on the next Wi-Fi connection or reboot.',pwd_title:'Change password',old_pwd:'Current password',new_pwd:'New password',change_pwd:'Change password',sleep_title:'Sleep mode',sleep_enable:'Enable sleep mode',sleep_off_at:'Daily power-off time (HH:MM, empty = none)',sleep_off_min:'Power off after idle minutes (0 = off)',sleep_hint:'• Scheduled: power off daily at a fixed time<br>• Idle: power off after N minutes without activity<br>• Battery power really powers off; on USB the device only signals shutdown',dns2_ph:'DNS2 (optional)',min_ph:'e.g. 60',save_sleep:'Save sleep settings',pwd_ok:'Password changed',pwd_fail:'Failed: check your current password',sleep_ok:'Sleep settings saved',sleep_fail:'Save failed',net_ok:'Network settings saved; applies on next connection',net_fail:'Save failed'}};
const T=k=>I18N[lang][k]||I18N.zh[k]||k;
function applyLang(){document.documentElement.lang=lang;document.title=T('title');document.querySelectorAll('[data-i18n]').forEach(el=>{el.innerHTML=T(el.getAttribute('data-i18n'))});document.querySelectorAll('[data-i18n-ph]').forEach(el=>{el.placeholder=T(el.getAttribute('data-i18n-ph'))});$('lang-select').value=lang}
$('lang-select').addEventListener('change',e=>{lang=e.target.value;localStorage.setItem(LANG_KEY,lang);applyLang()});
window.addEventListener('storage',e=>{if(e.key===LANG_KEY){lang=e.newValue==='en'?'en':'zh';applyLang()}});
applyLang();

async function api(path, opt={}) {
  const r=await fetch(path,{...opt,headers:{'Content-Type':'application/json',...(opt.headers||{})}});
  if(r.status===401) location.replace('/login?next='+encodeURIComponent(location.pathname));
  return r.json().catch(()=>({}));
}

function showMsg(id, text, ok) { const el=$(id); el.textContent=text; el.className=ok?'ok':''; }

// Password
$('btn-pwd').addEventListener('click',async()=>{
  const d=await api('/api/auth',{method:'POST',body:JSON.stringify({action:'change',old_password:$('old').value,new_password:$('new').value})});
  showMsg('msg-pwd',d.status==='ok'?T('pwd_ok'):T('pwd_fail'),d.status==='ok');
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
  const n=d.network||{};$('net-enable').checked=!!n.static_enabled;$('net-ip').value=n.ip||'';$('net-mask').value=n.netmask||'';$('net-gw').value=n.gateway||'';$('net-dns1').value=n.dns1||'';$('net-dns2').value=n.dns2||'';
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
  showMsg('msg-sleep',d.status==='ok'?T('sleep_ok'):T('sleep_fail'),d.status==='ok');
});
$('btn-net').addEventListener('click',async()=>{
  const d=await api('/api/config',{method:'POST',body:JSON.stringify({network:{static_enabled:$('net-enable').checked,ip:$('net-ip').value.trim(),netmask:$('net-mask').value.trim(),gateway:$('net-gw').value.trim(),dns1:$('net-dns1').value.trim(),dns2:$('net-dns2').value.trim()}})});
  showMsg('msg-net',d.status==='ok'?T('net_ok'):T('net_fail'),d.status==='ok');
});
loadSleep();
</script>
</body>
</html>)HTML";

bool ValidIpv4(const std::string& s) {
    struct in_addr addr;
    return inet_pton(AF_INET, s.c_str(), &addr) == 1;
}

const char* JsonString(const cJSON* obj, const char* key) {
    auto* item = cJSON_GetObjectItem(obj, key);
    return cJSON_IsString(item) ? item->valuestring : "";
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

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddBoolToObject(root, "sleep_mode", sleep_mode);
    cJSON_AddNumberToObject(root, "sleep_off_min", sleep_off_min);
    cJSON_AddNumberToObject(root, "sleep_off_at", sleep_off_at);

    auto net = NetworkSettings::LoadStatic();
    cJSON* net_json = cJSON_AddObjectToObject(root, "network");
    cJSON_AddBoolToObject(net_json, "static_enabled", net.enabled);
    cJSON_AddStringToObject(net_json, "ip", net.ip.c_str());
    cJSON_AddStringToObject(net_json, "netmask", net.netmask.c_str());
    cJSON_AddStringToObject(net_json, "gateway", net.gateway.c_str());
    cJSON_AddStringToObject(net_json, "dns1", net.dns1.c_str());
    cJSON_AddStringToObject(net_json, "dns2", net.dns2.c_str());

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

    // Network (static IP, applied on next Wi-Fi connect)
    auto* net_item = cJSON_GetObjectItem(root, "network");
    if (cJSON_IsObject(net_item)) {
        Settings s("network", true);
        auto* enabled = cJSON_GetObjectItem(net_item, "static_enabled");
        if (cJSON_IsBool(enabled)) s.SetBool("static_enabled", cJSON_IsTrue(enabled));
        for (const char* key : {"ip", "netmask", "gateway", "dns1", "dns2"}) {
            std::string value = JsonString(net_item, key);
            if (value.empty() || ValidIpv4(value)) s.SetString(key, value);
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