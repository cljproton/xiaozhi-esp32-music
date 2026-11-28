# 网络电台

这篇文档说明 Xiaozhi ESP32 固件的网络电台功能：网页管理、AI 语音选台、HLS/ICY 播放和断流恢复。音乐播放器见 [WebDAV 音乐播放器](music-player_zh.md)。

## 适用范围

- 网络电台与音乐播放器共用同一套网页登录和鉴权。电台页地址是 `/radio`，首页 `/` 有入口。
- 电台代码由 `CONFIG_USE_RADIO` 控制；`nologo/xingzhi-cube-2.0tft-wifi` 板卡配置已开启。
- 播放需要 ESP-IDF 的 `esp_hls_stream` 组件，音源可以是 m3u8 播放列表、直连 HTTP 流或 ICY 流。

## 主要文件

| 文件 | 作用 |
| --- | --- |
| `main/radio/radio_source_esp_adf.cc` | 基于 `esp_audio_simple_player` + `esp_hls_io` 的播放器、断流重连、预缓冲 |
| `main/radio/station_manager.cc` | 电台列表，保存在 NVS |
| `main/radio/radio_api_handler.cc` | 电台网页路由与设备接口 |
| `main/radio/radio_index.html` | 电台管理页 |
| `main/radio/radio_factory.cc` | 创建电台源实例 |
| `main/mcp_server.cc` | AI 的 `radio.*` 工具 |

## 网页管理

打开 `http://<设备IP>/radio`，未登录时会跳到 `/login`。页面功能：

- 按关键词、分类、国家、语言搜索 radio-browser.info；需要先在「API 镜像」里填入所在地区的镜像地址。
- 试听只播放不保存；点「添加」才写入设备。
- 「我的电台」支持播放、改 URL、删除、上移/下移/置顶，以及每台单独的增益（50%–300%）。列表序号从 1 开始，第一台是 AI 的默认台。
- 音量滑块控制设备输出音量，与音乐页、AI 工具共用；每电台的增益是独立的一层。

设备接口（都需要 Cookie 鉴权）：

| 方法与路径 | 说明 |
| --- | --- |
| `GET /api/stations` | 列出电台与上限（80） |
| `POST /api/stations` | 新增电台 |
| `PUT /api/stations` | 修改电台 URL |
| `DELETE /api/stations?index=N` | 删除电台 |
| `POST /api/stations/gain` | 设置单台增益 |
| `POST /api/stations/order` | 调整顺序 |
| `POST /api/preview` | 试听一个 URL，不保存 |
| `POST /api/stop` | 停止电台 |
| `POST /api/volume` | 设置设备输出音量 |
| `GET /api/status` | 播放状态、电台名、设备音量 |
| `GET/PUT /api/radio/config` | API 镜像列表 |

## AI 工具

| 工具 | 说明 |
| --- | --- |
| `radio.play` | 按台名模糊匹配并播放 |
| `radio.play_default` | 播放列表第一台 |
| `radio.next` / `radio.prev` | 切到相邻电台 |
| `radio.stop` | 停止播放 |
| `radio.volume` | 设置设备输出音量（0–100） |
| `radio.web_guide` | 返回电台配置页地址与已保存数量 |
| `radio.stations.list` | 列出已保存电台 |
| `radio.stations.get` | 查看某个电台详情 |
| `radio.stations.delete` | 按序号删除，需要 `confirm=true` |

删除是破坏性操作：先列列表，向用户确认后再传 `confirm=true`。

## 播放与恢复

- 启动时先缓冲 4 秒 PCM 再推给输出，HLS 使用 2MB PSRAM 环形缓冲吸收网络抖动。
- HLS 每个分片都会新建 HTTP 请求，这是正常行为，不会被误判为断线。
- 播放出现 `STOPPED` / `FINISHED` / `ERROR`，或直连流发生 HTTP 重连时，会重建播放器并按指数退避重试：1、2、4、8、16 秒封顶。每次成功进入 `RUNNING` 后重试计数清零。
- 连续失败 10 次后停止电台并恢复麦克风，避免无效 URL 导致无限重连。
- 30 秒缓冲看门狗触发时重连当前流，不再直接停台。
- 电台播放期间保持 codec 输出常开，避免省电逻辑关闭输出后重配 I2S 造成卡顿。
- 音乐与电台互斥：开始播放其中一路会先停掉另一路。boot 键在电台播放时按一下停台并恢复麦克风。

## 限制

- 播放期间不支持语音暂停：麦克风会收到扬声器声音，这块板子的 AFE 没有回声消除。用 boot 键或网页/AI 的停止工具。
- 电台源自身的分片间隔如果超过预缓冲，仍可能有短暂空档。
- 固件不直接支持 FTP 音源。要播放 FTP 上的音乐，用 OpenList 挂载后通过 WebDAV 接入，见 [WebDAV 音乐播放器](music-player_zh.md)。
- 电台列表最多 80 台，保存在 NVS 的 `radio_stations` 命名空间。
