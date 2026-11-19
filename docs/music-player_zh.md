# WebDAV 音乐播放器

这篇文档说明 Xiaozhi ESP32 固件里新增的远程音乐播放功能：WebDAV 音源、网页播放器、AI 语音控制工具，以及它们共用的播放会话。

## 适用范围

- 完整 WebUI 目前接入 `nologo/xingzhi-cube-2.0tft-wifi` 开发板，设备启动并联网后监听 80 端口。
- AI 的 `self.music.*` 工具由 `mcp_server.cc` 注册；编译进固件的板卡均可使用，前提是已经配置音源并有网络。
- `electron-bot` 和 `otto-robot` 自带的 WebSocket 控制服务还额外提供 `POST /api/music/play` 单曲接口，没有完整 WebUI。

## 组件与数据流

网页或 AI 发出播放指令后，命令进入 `PlaybackScheduler`，再调用 `Application` 的播放会话；`NotifyPlayer` 启动 GMF 流水线，通过 HTTP IO 从 WebDAV 拉取音频，解码成 PCM 后交给 `AudioService` 从扬声器输出。网页和 AI 共用同一个播放会话，所以一方暂停，另一方看到的状态也会同步。

主要文件：

| 文件 | 作用 |
| --- | --- |
| `main/handlers/music_api_handler.cc` | WebUI 的 HTTP API |
| `main/handlers/system_api_handler.cc` | WebUI 登录 |
| `main/web_ui/WebUIRenderer.cc` | WebUI 页面、样式和脚本 |
| `main/protocols/ws_control_server.cc` | HTTP/WebSocket 服务 |
| `main/mcp_server.cc` | AI 音乐工具与 WebDAV 目录解析 |
| `main/playback_scheduler.cc` / `.h` | 播放命令队列 |
| `main/application.cc` / `.h` | 播放会话、暂停快照、TTS 打断 |
| `main/notify/notify_player.cc` / `.h` | GMF 播放、HTTP 拉取、字节定位 |

`PlaybackScheduler` 定义了 `SYSTEM(0) > TTS(10) > MUSIC(20) > NOTIFICATION(30)` 的优先级。当前 `TTS_PLAY` / `TTS_STOP` 只是预留命令，TTS 音频仍走原有的协议音频通道。

## 配置音源

音源保存在 NVS 的 `music` 命名空间里，`sources` 是一个 JSON 数组。单个音源格式如下：

```json
{
  "type": "webdav",
  "base_url": "http://<host>:<port>",
  "path": "/dav",
  "username": "<username>",
  "password": "<password>"
}
```

`base_url` 只写到 `http://主机:端口`，不要带 `/dav`；`path` 写 `/dav` 或其下的子目录。账号和密码按 WebDAV 服务器实际配置填写。

推荐用 OpenList 统一管理音源。它能把 FTP、SFTP、SMB、对象存储等协议挂载为网盘，再统一通过 WebDAV 暴露给设备；网页的音源管理表单里也有同样的提示。例如 FTP 挂载到 OpenList 的 `/ftp` 后，`path` 填 `/dav/ftp`，设备即可从该 FTP 目录播放音乐。

音源的增删改只在网页端进行，AI 的 `self.music.sources` 是只读的。`base_url` 与 `path` 在读取时会被规范化：`path` 缺少开头的 `/` 会自动补上。

## WebUI

在浏览器打开 `http://<设备IP>/`，登录令牌默认是 `xiaozhi`。页面提供中英文切换。

页面功能包括：

- 音源列表与增删改；切换音源时会把序号写入 NVS。
- 曲目列表，每页 20 首，可翻页；标题从 WebDAV 的 PROPFIND 结果解析。
- 播放、暂停、继续、停止、上一首、下一首、音量调节。
- 顺序 / 随机模式切换。
- 拖动进度条跳转，暂停态和播放态都支持。
- 按标题搜索，并直接播放搜索结果中的某一首。
- 每秒轮询 `/api/music/status` 同步状态；暂停状态会反映到按钮文字。

WebUI 的静态页面、CSS 和 JS 都由 `WebUIRenderer.cc` 生成，不需要额外的文件系统资源。

### HTTP API

除 `/api/music/login` 外，所有接口都要求 `Authorization: Bearer <token>`，默认 token 为 `xiaozhi`。

| 方法与路径 | 请求体或参数 | 说明 |
| --- | --- | --- |
| `POST /api/music/login` | `{"token":"xiaozhi"}` | 登录 |
| `GET /api/music/sources` | 无 | 返回音源数组和 `selected_index` |
| `POST /api/music/sources` | 音源 JSON 数组 | 保存音源配置 |
| `GET /api/music/tracks` | `?source=0&page=1` | 曲目分页，每页 20 首 |
| `POST /api/music/source` | `{"index":0,"start":0}` | 播放整个音源，保存选中音源 |
| `POST /api/music/track` | `{"index":3}` | 播放当前列表中的某首 |
| `POST /api/music/next` / `prev` | 无 | 上一首 / 下一首 |
| `POST /api/music/pause` / `resume` / `stop` | 无 | 播放控制 |
| `POST /api/music/seek` | `{"time_ms":30000}` | 跳转到指定位置 |
| `POST /api/music/mode` | `{"mode":"shuffle"}` | 设置模式并写入 NVS |
| `POST /api/music/selected_source` | `{"index":0}` | 保存选中的音源 |
| `POST /api/music/volume` | `{"volume":50}` | 设置输出音量，范围 0–100 |
| `GET /api/music/status` | 无 | 当前播放状态 |
| `POST /api/music/search` | `{"keyword":"周杰伦","source":0}` | 搜索曲目 |
| `POST /api/music/play_track` | `{"source":0,"index":3}` | 播放指定音源的指定曲目 |
| `POST /api/music/play` | `{"source":{...}}` | 用临时音源 JSON 播放单曲 |
| `POST /api/music/list` | `{"source":{...}}` | 列出 WebDAV 原始目录条目 |

`POST /api/music/source` 兼容旧请求中的 `mode` 字段：传了会先写入 NVS；不传则使用 NVS 里保存的模式。

## AI 工具

| 工具 | 参数 | 说明 |
| --- | --- | --- |
| `self.music.sources` | 无 | 列出已配置音源，只读 |
| `self.music.play` | `source` | 用临时音源 JSON 播放单曲 |
| `self.music.play_all` | `source?`、`random?` | 播放全部曲目；不传 `source` 时使用 NVS 保存的音源，`random:true` 切换到随机模式并持久化 |
| `self.music.play_source` | `index`、`random?` | 按序号播放整个音源，同时保存选中音源 |
| `self.music.count` | `source` | 统计音源里的音频文件数 |
| `self.music.search` | `keyword`、`source?` | 搜索曲目，返回 `index` 和 `title` |
| `self.music.play_track` | `source?`、`index` | 播放搜索结果中的某一首 |
| `self.music.next` / `prev` | 无 | 上一首 / 下一首，不改变播放模式 |
| `self.music.pause` / `resume` / `stop` | 无 | 播放控制 |
| `self.music.seek` | `time_ms` | 跳转到指定位置 |
| `self.music.volume` | `volume` | 设置音量，范围 0–100 |
| `self.music.mode` | `mode` | `shuffle` 或 `sequential`，写入 NVS |
| `self.music.status` | 无 | 查询当前播放状态 |

音源序号从 0 开始。搜索命中多首时，应把列表读给用户并追问要播放哪一首，再用 `self.music.play_track` 播放。单个音源最多解析 500 首曲目，超出的部分会被截断，状态里的 `truncated` 为 `true`。

## 模式与音源记忆

NVS 的 `music` 命名空间里保存两个额外字段：

- `play_mode`：`0` 顺序，`1` 随机。通过 `/api/music/mode` 或 `self.music.mode` 修改后立即写入；固件启动时读回。
- `selected_source`：当前选中的音源序号。网页切换音源或 AI 调用 `self.music.play_source` 时写入。

“播放全部音乐”在未指定音源时会读取 `selected_source`，模式则读取 `play_mode`。上一首和下一首只移动播放游标，不会修改这两个字段。

暂停快照只放在 PSRAM，不写入 NVS，断电后不保留。

## 暂停与进度跳转

GMF 的 `pause` / `resume` 在这条 HTTP 音频链路上不稳定，暂停时会拉流中断。当前实现改为：

1. 从 GMF 读取真实的 HTTP IO 字节位置、文件大小、时长和播放位置；
2. 把这些数字写进一份单例 PSRAM 快照，然后停止 GMF 和 HTTP IO；
3. 恢复时按快照里的字节位置重新发起带 `Range` 的请求；
4. 进度跳转更新同一份快照，暂停态和播放态走同一条恢复路径。

快照只存数字，不存 URL 和凭证；曲目信息从内存中的播放列表按序号反查，因此没有字符串长度限制。`Application::GetMusicStatus()` 在暂停时会用快照里的时长兜底，网页进度条不会丢失最大值。

GMF 的任务栈放在 PSRAM，HTTP IO 的 24 KB 栈和 2 MB 环形缓冲区也使用 PSRAM，避免内部 SRAM 紧张时音频任务创建失败。WebDAV 的 PROPFIND 在连接失败时会重试一次。

## 与 AI 语音的关系

开始播放音乐前，固件会调用 `AbortSpeaking()` 并清空本地 TTS 队列，然后把设备状态切回 Idle，使音乐可以立即开始。AI 随后回复的 TTS 与音乐共用输出通道。

播放期间不支持语音打断暂停：这块板子的 AFE 没有回声消除，麦克风会收到扬声器里的音乐。降低音量后唤醒词可以识别，但「唤醒词 + 指令」的识别不稳定，因此没有启用这条路径。暂停请用 boot 键或网页；boot 键在播放中按一下暂停，再按一下停止。

## 日志

串口会打印 AI 对话收发和工具调用：

- `PROTO`：`RX` / `TX` 消息；`hello`、`goodbye`、`ping`、`pong` 被过滤。
- `MCP`：`Tool call: <name>`、成功或失败及耗时。
- `PlaybackScheduler`：命令执行信息。
- `NotifyPlayer`：GMF 状态和字节位置。

`sdkconfig.defaults` 里开启了 `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y`，运行时用 `esp_log_level_set()` 调整各模块的级别。

## 构建与烧录

先加载 ESP-IDF 环境，再用构建脚本选择单板变体：

```sh
source $IDF_PATH/export.sh
python3 scripts/build.py --list-boards
python3 scripts/build.py nologo/xingzhi-cube-2.0tft-wifi
```

烧录使用 esptool，偏移依次为 bootloader `0x0`、分区表 `0x8000`、ota_data `0x15000`、应用 `0x20000`、资源 `0x800000`：

```sh
python3 -m esptool --chip esp32s3 -p <serial-port> -b 921600 \
    --before default-reset --after hard-reset \
    write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
    0x0 build/bootloader/bootloader.bin \
    0x8000 build/partition_table/partition-table.bin \
    0x15000 build/ota_data_initial.bin \
    0x20000 build/xiaozhi.bin \
    0x800000 build/generated_assets.bin
```

抓串口日志建议使用 `serial.tools.miniterm`：

```sh
script -qec "python3 -m serial.tools.miniterm <serial-port> 115200 --raw" /tmp/serial.log
```

`sdkconfig.defaults` 还把 `CONFIG_LWIP_MAX_SOCKETS` 提高到 16。默认 10 个 socket 在 WebUI、AI 协议和 WebDAV 同时工作时会出现 `connect_failed (native=23)`。

## 已知限制

- 播放中的语音暂停不可用，原因见上文。
- 暂停快照不跨重启保存。
- 可变码率 MP3 从非帧边界恢复时，可能有轻微位置偏差或开头短暂静音。
- NVS 里保存的音源序号如果因为删除音源而失效，“播放全部音乐”会报序号超出范围，需要重新选择音源。
- 曲目上限 500 首，列表每页固定 20 首。
