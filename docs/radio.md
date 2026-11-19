# Internet Radio

This document covers the internet radio feature: web management, AI voice control, HLS/ICY playback and stream recovery. For the music player, see [WebDAV Music Player](music-player.md).

## Scope

- Radio and music share the same web login and authentication. The radio page is `/radio`, linked from the home page `/`.
- Radio code is gated by `CONFIG_USE_RADIO`; the `nologo/xingzhi-cube-2.0tft-wifi` board config enables it.
- Playback needs the `esp_hls_stream` component. Sources can be m3u8 playlists, direct HTTP streams or ICY streams.

## Main files

| File | Purpose |
| --- | --- |
| `main/radio/radio_source_esp_adf.cc` | Player based on `esp_audio_simple_player` + `esp_hls_io`, reconnection and prebuffering |
| `main/radio/station_manager.cc` | Station list stored in NVS |
| `main/radio/radio_api_handler.cc` | Radio web routes and device API |
| `main/radio/radio_index.html` | Station management page |
| `main/radio/radio_factory.cc` | Radio source factory |
| `main/mcp_server.cc` | AI `radio.*` tools |

## Web management

Open `http://<device-ip>/radio`. Without a cookie the page redirects to `/login`. The page provides:

- Search radio-browser.info by keyword, tag, country and language. Configure a regional API mirror under API Mirrors first.
- Preview plays a station without saving it; Add stores it on the device.
- My Stations supports play, edit URL, delete, move up/down/top and per-station gain (50%–300%). Indexes start at 1; the first station is the AI default.
- The volume slider controls device output volume, shared with the music page and AI tools. Per-station gain is a separate layer.

Device API (all require cookie authentication):

| Method and path | Purpose |
| --- | --- |
| `GET /api/stations` | List stations and the limit (80) |
| `POST /api/stations` | Add a station |
| `PUT /api/stations` | Change a station URL |
| `DELETE /api/stations?index=N` | Delete a station |
| `POST /api/stations/gain` | Set per-station gain |
| `POST /api/stations/order` | Reorder stations |
| `POST /api/preview` | Preview a URL without saving |
| `POST /api/stop` | Stop radio |
| `POST /api/volume` | Set device output volume |
| `GET /api/status` | Playback state, station name, device volume |
| `GET/PUT /api/config` | API mirror list |

## AI tools

| Tool | Purpose |
| --- | --- |
| `radio.play` | Play a saved station by fuzzy name match |
| `radio.play_default` | Play the first saved station |
| `radio.next` / `radio.prev` | Switch to an adjacent station |
| `radio.stop` | Stop playback |
| `radio.volume` | Set device output volume (0–100) |
| `radio.web_guide` | Return the station page URL and saved count |
| `radio.stations.list` | List saved stations |
| `radio.stations.get` | Show one station's details |
| `radio.stations.delete` | Delete by index; requires `confirm=true` |

Deletion is destructive: list the stations, ask the user to confirm, then pass `confirm=true`.

## Playback and recovery

- The player buffers 4 seconds of PCM before output. HLS uses a 2MB PSRAM ring buffer to absorb network jitter.
- Each HLS segment opens a new HTTP request. This is normal and is not treated as a disconnect.
- `STOPPED` / `FINISHED` / `ERROR` events, or an HTTP reconnect on a direct stream, rebuild the player and retry with exponential backoff: 1, 2, 4, 8, 16 seconds. Reaching `RUNNING` resets the retry count.
- After 10 consecutive failures the radio stops and the microphone is restored, so an invalid URL cannot retry forever.
- A 30-second buffering watchdog reconnects the current stream instead of stopping the radio.
- Codec output stays enabled during radio playback. Otherwise the power-saving code would disable it and reinitialize I2S on the next chunk, causing an audible glitch.
- Music and radio are mutually exclusive: starting one stops the other. The boot button stops the radio and restores the microphone.

## Limitations

- Voice pause during playback is not available: the microphone picks up the speaker and this board's AFE has no acoustic echo cancellation. Use the boot button, the web page or the AI stop tool.
- If the station's own segment spacing exceeds the prebuffer, short gaps can still occur.
- The firmware does not play FTP directly. Use OpenList to mount FTP and expose it through WebDAV; see [WebDAV Music Player](music-player.md).
- Up to 80 stations are stored in NVS under the `radio_stations` namespace.
