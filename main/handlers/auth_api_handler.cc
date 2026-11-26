#include "auth_api_handler.h"

#include "web_auth.h"
#include "cjson_utils.h"

#include <cJSON.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <cstring>
#include <string>

namespace {

const char* TAG = "AuthApi";

const char* kLoginHtml = R"HTML(<!DOCTYPE html>
<html lang="zh">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>登录 - 小智</title>
<style>
:root{--bg:#0f1115;--card:#181b21;--line:#262a33;--fg:#e8eaee;--dim:#8b919c;--acc:#4f8cff}
*{box-sizing:border-box;margin:0;padding:0}
body{font-family:system-ui,-apple-system,"PingFang SC","Microsoft YaHei",sans-serif;background:var(--bg);color:var(--fg);min-height:100vh;display:flex;align-items:center;justify-content:center;padding:16px}
.card{background:var(--card);border:1px solid var(--line);border-radius:12px;padding:24px;width:100%;max-width:380px}
h1{font-size:20px;margin-bottom:8px}
p{color:var(--dim);font-size:13px;line-height:1.6;margin-bottom:16px}
input{width:100%;background:var(--bg);border:1px solid var(--line);color:var(--fg);border-radius:8px;padding:10px;font-size:15px;margin-bottom:12px}
button{width:100%;background:var(--acc);color:#fff;border:0;border-radius:8px;padding:10px;font-size:15px;cursor:pointer}
#msg{color:#f88;font-size:13px;min-height:18px;margin-top:10px}
#hint{background:rgba(79,140,255,.1);border:1px solid rgba(79,140,255,.3);border-radius:8px;padding:8px 10px;margin-bottom:14px;font-size:13px;color:#9dc0ff}
.lang-btn{background:transparent;border:1px solid var(--line);color:var(--dim);border-radius:999px;padding:4px 10px;margin-bottom:14px;font-size:13px;cursor:pointer;width:auto}
</style>
</head>
<body>
<div class="card">
  <h1 id="t-title">小智设备管理</h1>
  <p id="t-hint-text">请输入网页端登录密码。</p>
  <div class="hint" id="default-hint" style="display:none"></div>
  <input id="password" type="password" placeholder="密码" autocomplete="current-password">
  <button id="submit" type="button">登录</button>
  <button id="lang" class="lang-btn" type="button">中</button>
  <div id="msg"></div>
</div>
<script>
const $ = (id) => document.getElementById(id);
const LANG_KEY='lang';
let lang=localStorage.getItem(LANG_KEY)==='en'?'en':'zh';
const I18N={zh:{title:'小智设备管理',hint:'请输入网页端登录密码。',defaultHint:'默认密码：xiaozhi',password:'密码',login:'登录',failed:'登录失败',network:'网络错误'},en:{title:'XiaoZhi Device',hint:'Enter the web interface password.',defaultHint:'Default password: xiaozhi',password:'Password',login:'Sign in',failed:'Login failed',network:'Network error'}};
const T=k=>I18N[lang][k];
function applyLang(){document.documentElement.lang=lang;document.title=T('title');$('t-title').textContent=T('title');$('t-hint-text').textContent=T('hint');$('default-hint').textContent=T('defaultHint');$('password').placeholder=T('password');$('submit').textContent=T('login');$('lang').textContent=lang==='zh'?'中':'EN'}
$('lang').addEventListener('click',()=>{lang=lang==='zh'?'en':'zh';localStorage.setItem(LANG_KEY,lang);applyLang()});
window.addEventListener('storage',e=>{if(e.key===LANG_KEY){lang=e.newValue==='en'?'en':'zh';applyLang()}});
applyLang();
function nextPath(){
  const n = new URLSearchParams(location.search).get('next') || '/';
  return n.startsWith('/') && !n.startsWith('//') ? n : '/';
}
fetch('/api/auth').then(r=>r.json()).then(d=>{
  if(d.is_default) $('default-hint').style.display='block';
}).catch(()=>{});
async function doLogin(){
  $('msg').textContent='';
  try{
    const r = await fetch('/api/auth',{method:'POST',headers:{'Content-Type':'application/json'},
      body:JSON.stringify({action:'login',password:$('password').value})});
    const d = await r.json().catch(()=>({}));
    if(r.ok && d.status==='ok'){ location.replace(nextPath()); return; }
    $('msg').textContent = d.error || T('failed');
  }catch(e){ $('msg').textContent=T('network'); }
}
$('submit').addEventListener('click',doLogin);
$('password').addEventListener('keydown',e=>{if(e.key==='Enter')doLogin();});
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
    if (remaining <= 0 || remaining > 512) return "";
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

esp_err_t HandleLoginPage(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, kLoginHtml, strlen(kLoginHtml));
}

esp_err_t HandleAuth(httpd_req_t* req) {
    if (req->method == HTTP_GET) {
        cJSON* root = cJSON_CreateObject();
        cJSON_AddBoolToObject(root, "authenticated", web_auth::IsAuthorized(req));
        cJSON_AddBoolToObject(root, "is_default", web_auth::IsDefaultPassword());
        return SendJson(req, root);
    }
    if (req->method != HTTP_POST) {
        return SendError(req, "405 Method Not Allowed", "method not allowed");
    }
    cJSON* root = cJSON_Parse(ReadBody(req).c_str());
    if (root == nullptr) {
        return SendError(req, "400 Bad Request", "invalid JSON");
    }
    cJSON* action_item = cJSON_GetObjectItem(root, "action");
    std::string action = cJSON_IsString(action_item) ? action_item->valuestring : "login";
    std::string password;
    cJSON* item = cJSON_GetObjectItem(root, "password");
    if (cJSON_IsString(item) && item->valuestring != nullptr) password = item->valuestring;

    if (action == "login") {
        if (!web_auth::CheckPassword(password)) {
            cJSON_Delete(root);
            return SendError(req, "401 Unauthorized", "invalid password");
        }
        web_auth::SetCookie(req, password);
        cJSON_Delete(root);
        cJSON* ok = cJSON_CreateObject();
        cJSON_AddStringToObject(ok, "status", "ok");
        return SendJson(req, ok);
    }
    if (action == "change") {
        if (!web_auth::IsAuthorized(req)) {
            cJSON_Delete(root);
            return SendError(req, "401 Unauthorized", "unauthorized");
        }
        std::string old_password;
        std::string new_password;
        item = cJSON_GetObjectItem(root, "old_password");
        if (cJSON_IsString(item) && item->valuestring != nullptr) old_password = item->valuestring;
        item = cJSON_GetObjectItem(root, "new_password");
        if (cJSON_IsString(item) && item->valuestring != nullptr) new_password = item->valuestring;
        cJSON_Delete(root);
        if (!web_auth::ChangePassword(old_password, new_password)) {
            return SendError(req, "400 Bad Request", "invalid old password or new password");
        }
        web_auth::SetCookie(req, new_password);
        cJSON* ok = cJSON_CreateObject();
        cJSON_AddStringToObject(ok, "status", "ok");
        return SendJson(req, ok);
    }
    cJSON_Delete(root);
    return SendError(req, "400 Bad Request", "unknown action");
}

}  // namespace

void AuthApiHandler::RegisterHandlers(HandlerRegistry& registry) {
    registry.Register({"/login", HTTP_GET, HandleLoginPage, nullptr}, "Login page");
    registry.Register({"/api/auth", HTTP_GET, HandleAuth, nullptr}, "Auth status");
    registry.Register({"/api/auth", HTTP_POST, HandleAuth, nullptr}, "Login / change password");
}
