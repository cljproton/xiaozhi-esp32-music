# WebDAV Music Player

This document covers the remote music playback features added to the Xiaozhi ESP32 firmware: WebDAV sources, the web player, AI control tools, and the playback session shared by both.

## Scope

- The full WebUI is wired to the `nologo/xingzhi-cube-2.0tft-wifi` board and listens on port 80 once the device is online.
- The `self.music.*` tools are registered in `mcp_server.cc`. Any board that includes the music code can use them once a source and network access are available.
- The `electron-bot` and `otto-robot` WebSocket control servers also expose `POST /api/music/play` for a single temporary source. They do not include the full WebUI.

## Architecture

A play request from the browser or AI enters `PlaybackScheduler`, which calls the playback session in `Application`. `NotifyPlayer` starts a GMF pipeline, fetches audio from WebDAV over HTTP, decodes it to PCM, and sends it to `AudioService` for output. Both control paths share one session, so a pause in one is reflected in the other.

| File | Purpose |
| --- | --- |
| `main/handlers/music_api_handler.cc` | WebUI HTTP API |
| `main/handlers/system_api_handler.cc` | WebUI login |
| `main/web_ui/WebUIRenderer.cc` | WebUI markup, styles, and script |
| `main/protocols/ws_control_server.cc` | HTTP and WebSocket server |
| `main/mcp_server.cc` | AI tools and WebDAV directory parsing |
| `main/playback_scheduler.cc` / `.h` | Command queue |
| `main/application.cc` / `.h` | Session, pause snapshot, TTS interruption |
| `main/notify/notify_player.cc` / `.h` | GMF playback, HTTP fetching, byte positioning |

`PlaybackScheduler` defines priorities `SYSTEM(0) > TTS(10) > MUSIC(20) > NOTIFICATION(30)`. `TTS_PLAY` and `TTS_STOP` are reserved commands; TTS audio still uses the original protocol audio channel.

## Sources

Sources are stored in NVS under the `music` namespace. The `sources` key holds a JSON array; each entry has this shape:

```json
{
  "type": "webdav",
  "base_url": "http://<host>:<port>",
  "path": "/dav",
  "username": "<username>",
  "password": "<password>"
}
```

`base_url` stops at `http://host:port` and must not contain `/dav`. `path` is `/dav` or one of its subdirectories. Use the credentials required by your WebDAV server.

OpenList is the recommended source gateway. It mounts FTP, SFTP, SMB, object storage and other drives, then exposes them through WebDAV; the web source form shows the same hint. If FTP is mounted at `/ftp` in OpenList, set `path` to `/dav/ftp` to play from that FTP directory.

Sources are added, edited, and deleted only in the web UI. The AI tool `self.music.sources` is read-only. On read, a `path` without a leading slash is corrected automatically.

## WebUI

Open `http://<device-ip>/` in a browser and log in with the default token `xiaozhi`. The page switches between Chinese and English.

The page provides source editing, source selection, a 20-track-per-page list, playback controls, shuffle, volume, progress seeking, title search, and direct play from search results. It polls `/api/music/status` once per second; a pause is reflected in the toggle button. The HTML, CSS, and JavaScript are generated in `WebUIRenderer.cc`, so no extra filesystem assets are needed.

### HTTP API

Every endpoint except `/api/music/login` requires `Authorization: Bearer <token>`; the default token is `xiaozhi`.

| Method and path | Body or query | Purpose |
| --- | --- | --- |
| `POST /api/music/login` | `{"token":"xiaozhi"}` | Log in |
| `GET /api/music/sources` | none | List sources and `selected_index` |
| `POST /api/music/sources` | Source array | Save source configuration |
| `GET /api/music/tracks` | `?source=0&page=1` | Paginated tracks, 20 per page |
| `POST /api/music/source` | `{"index":0,"start":0}` | Play a whole source and save it as selected |
| `POST /api/music/track` | `{"index":3}` | Play a track in the current list |
| `POST /api/music/next` / `prev` | none | Next / previous track |
| `POST /api/music/pause` / `resume` / `stop` | none | Playback control |
| `POST /api/music/seek` | `{"time_ms":30000}` | Seek to a position |
| `POST /api/music/mode` | `{"mode":"shuffle"}` | Set mode and persist it |
| `POST /api/music/selected_source` | `{"index":0}` | Save the selected source |
| `POST /api/music/volume` | `{"volume":50}` | Output volume, 0–100 |
| `GET /api/music/status` | none | Current playback state |
| `POST /api/music/search` | `{"keyword":"...","source":0}` | Search tracks |
| `POST /api/music/play_track` | `{"source":0,"index":3}` | Play a track from a given source |
| `POST /api/music/play` | `{"source":{...}}` | Play one track from a temporary source |
| `POST /api/music/list` | `{"source":{...}}` | List raw WebDAV entries |

`POST /api/music/source` still accepts an optional `mode` field for older clients. When present, it is written to NVS first; when absent, the saved mode is used.

## AI tools

| Tool | Arguments | Purpose |
| --- | --- | --- |
| `self.music.sources` | none | List configured sources, read-only |
| `self.music.play` | `source` | Play one track from a temporary source |
| `self.music.play_all` | `source?`, `random?` | Play all tracks. Without `source`, use the saved source. `random:true` switches to shuffle and persists it |
| `self.music.play_source` | `index`, `random?` | Play a whole source by index and save it as selected |
| `self.music.count` | `source` | Count audio files in a source |
| `self.music.search` | `keyword`, `source?` | Search by title, returning `index` and `title` |
| `self.music.play_track` | `source?`, `index` | Play a search result |
| `self.music.next` / `prev` | none | Next / previous track; mode is unchanged |
| `self.music.pause` / `resume` / `stop` | none | Playback control |
| `self.music.seek` | `time_ms` | Seek to a position |
| `self.music.volume` | `volume` | Output volume, 0–100 |
| `self.music.mode` | `mode` | `shuffle` or `sequential`, written to NVS |
| `self.music.status` | none | Current playback state |

Source indexes start at 0. When a search returns several matches, read the list to the user, ask which one to play, then call `self.music.play_track`. Each source yields at most 500 tracks; the excess is dropped and `truncated` becomes `true`.

## Saved mode and source

Two more keys live in the `music` NVS namespace:

- `play_mode`: `0` for sequential, `1` for shuffle. It is written by `/api/music/mode` or `self.music.mode` and loaded at boot.
- `selected_source`: the current source index. It is written when the web UI changes the source or AI calls `self.music.play_source`.

"Play all" falls back to `selected_source` when no source is supplied, and uses `play_mode` for the order. Next and previous only move the cursor; they do not change either key.

The pause snapshot lives only in PSRAM and is not written to NVS, so it does not survive a reboot.

## Pause and seeking

GMF `pause` / `resume` is unreliable on this HTTP audio path: the stream can abort while paused. The current implementation instead:

1. Reads the real HTTP IO byte position, file size, duration, and play position from GMF.
2. Writes those numbers into a single PSRAM snapshot, then stops GMF and the HTTP IO.
3. Restarts with a `Range` request at the saved byte position on resume.
4. Updates the same snapshot for seeking, using one resume path for both paused and playing states.

The snapshot stores numbers only. URLs and credentials stay in the in-memory playlist and are looked up by index, so there is no string-length limit. `Application::GetMusicStatus()` falls back to the snapshot duration while paused, which keeps the web progress slider valid.

The GMF task stack is placed in PSRAM, along with the HTTP IO 24 KB stack and 2 MB ring buffer. This avoids task creation failures when internal SRAM is tight. WebDAV PROPFIND retries once when the connection fails.

## Interaction with AI speech

Before playback starts, the firmware calls `AbortSpeaking()`, clears the queued TTS audio, and moves the device back to Idle so music can start immediately. The AI's later TTS reply shares the output channel with the music.

Voice pause during playback is not supported: this board's AFE has no acoustic echo cancellation, so the microphone picks up the speaker. Lowering the volume makes the wake word detectable, but "wake word + command" recognition is not reliable enough, so that path is disabled. Use the boot button or the web UI instead. One boot-button press pauses; a second press stops playback.

## Logging

Serial output includes AI conversation traffic and tool calls:

- `PROTO`: `RX` / `TX` messages; `hello`, `goodbye`, `ping`, and `pong` are filtered out.
- `MCP`: `Tool call: <name>`, result, and elapsed time.
- `PlaybackScheduler`: command execution.
- `NotifyPlayer`: GMF state and byte positions.

`sdkconfig.defaults` enables `CONFIG_LOG_MAXIMUM_LEVEL_DEBUG=y`; runtime levels are set with `esp_log_level_set()`.

## Build and flash

Load the ESP-IDF environment, then select a board variant with the build script:

```sh
source $IDF_PATH/export.sh
python3 scripts/build.py --list-boards
python3 scripts/build.py nologo/xingzhi-cube-2.0tft-wifi
```

Flash with esptool. Offsets are bootloader `0x0`, partition table `0x8000`, ota_data `0x15000`, app `0x20000`, and assets `0x800000`:

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

Use `serial.tools.miniterm` to capture serial logs:

```sh
script -qec "python3 -m serial.tools.miniterm <serial-port> 115200 --raw" /tmp/serial.log
```

`sdkconfig.defaults` also raises `CONFIG_LWIP_MAX_SOCKETS` to 16. With the default of 10, WebUI, the AI protocol, and WebDAV together can exhaust sockets and produce `connect_failed (native=23)`.

## Known limitations

- Voice pause during playback is not available, for the reason above.
- The pause snapshot does not survive a power cycle.
- Variable-bitrate MP3 files can seek with a small position error or brief silence when resuming away from a frame boundary.
- If a saved source index becomes invalid because sources were deleted, "play all" reports that the index is out of range; select a source again.
- Each source is capped at 500 tracks and the track list is fixed at 20 items per page.
