# bambu-relay

ESP32 WiFi bridge: connects to OZU eduroam (WPA2-Enterprise/PEAP) as a STA and rebroadcasts as a private WPA2 AP so the Bambu Lab P2S printer (which has no WPA2-Enterprise support) can reach the network.

## Why

OZU eduroam requires WPA2-Enterprise (PEAP/MSCHAPv2). The Bambu P2S only does WPA2-PSK. This relay sits in between.

## Hardware

- ESP32 (any variant with PSRAM is fine, basic ESP32 works too)
- USB power — once flashed and provisioned, just needs 5V

## Toolchain

ESP-IDF v5.4.1 at `/opt/espressif/sdk`, tools at `/opt/espressif/env`.

```bash
source /opt/espressif/sdk/export.sh   # activate environment
```

Alias `get_idf` does the same if configured in `.zshrc`.

**Important:** use Python 3.12 venv, not 3.14 — `idf.py monitor` freezes on 3.14.

## First flash

```bash
cd /opt/espressif/projects/bambu-relay
idf.py -p /dev/cu.usbserial-0001 erase-flash flash monitor
```

`erase-flash` wipes NVS so provisioning starts clean. Only needed on first flash or if you want to reset credentials.

## Provisioning credentials

On first boot with no credentials stored, the device drops to a REPL prompt:

```
relay> provision <identity> <username> <password>
```

For OZU eduroam:
```
relay> provision efe.atcali@ozu.edu.tr efe.atcali@ozu.edu.tr <password>
```

The device saves credentials to NVS (survives power cycles) and restarts into relay mode.

## Subsequent flashes (preserving credentials)

```bash
idf.py -p /dev/cu.usbserial-0001 flash
```

No `erase-flash` — NVS partition is left untouched.

## Network topology

```
[Bambu P2S / iPhone]
    192.168.4.x
         |
    [ESP32 AP]  BambuBridge / WPA2 / bambu2024!
         |
    [ESP32 STA] 10.200.x.x (eduroam DHCP)
         |
    [eduroam] → internet
```

lwIP NAPT handles routing. A UDP DNS forwarder on port 53 proxies queries to 8.8.8.8 (DHCP DNS option alone wasn't reliable).

Tested throughput: ~5 Mbps through eduroam. Sufficient for Bambu cloud control and print monitoring.

## Security

- MAC whitelist enforced on AP connect (`WIFI_EVENT_AP_STACONNECTED` → `esp_wifi_deauth_sta()`)
- WPA2-PSK on the AP (password required to join)
- EAP credentials stored in NVS, not in the binary
- Whitelist:
  - `3c:1a:cc:dc:e7:bc` — Bambu P2S
  - `f8:2a:e2:05:80:36` — Efe iPhone

To add a MAC: edit `ALLOWED_MACS` in `relay_main.c` and reflash (no erase-flash needed).

## Feature flags (top of relay_main.c)

| Flag | Effect |
|---|---|
| `USE_ENTERPRISE` | Connect upstream via WPA2-Enterprise (eduroam). Comment out to use plain PSK hotspot defined by `STA_SSID`/`STA_PASSWORD`. |
| `USE_MAC_FILTER` | Enforce MAC whitelist on AP. Comment out to allow any client. |

## Serial console commands

Monitor via `idf.py -p /dev/cu.usbserial-0001 monitor`. Exit with `Ctrl+]`.

| Command | Description |
|---|---|
| `status` | STA connection, IP, RSSI, connected clients |
| `monitor on/off` | Print ~bandwidth estimate every second |
| `dnslog on/off` | Log all DNS queries (useful for debugging) |
| `ssid show/hide` | Toggle AP SSID broadcast at runtime |
| `reconnect` | Disconnect and reconnect upstream WiFi |
| `restart` | Reboot the ESP32 |
| `provision <id> <user> <pass>` | Store new EAP credentials in NVS and restart |
| `clear_creds` | Wipe stored credentials (next boot drops to REPL) |
| `whoami` | Show stored identity/username |

## Camera

Remote camera (Bambu Handy over cellular) does **not** work — Bambu uses IOTC P2P for video which can't punch through double NAT (eduroam + ESP32).

**LAN camera works:** connect your phone to BambuBridge, enable LAN mode in Bambu Handy. Phone and printer are on the same 192.168.4.x subnet so RTSP streams directly on port 6000.

Remote camera would require a reverse tunnel (frp/VPS) or a separate camera (e.g. Raspberry Pi camera module with its own outbound stream).

## Known issues

- `idf.py monitor` can freeze if Python 3.14 is active — switch to 3.12 venv
- Kill stray monitor processes: `pkill -9 -f "idf.py|idf_monitor|esp_idf_monitor"`
- Monitor exits with `Ctrl+]`, not `Ctrl+C`
- Changing AP config at runtime (e.g. `ssid show`) causes a brief AP restart — connected clients will reconnect
- iCloud Private Relay on iPhone can break internet through the relay — disable it for BambuBridge in Settings → Wi-Fi → ⓘ → Private Relay → Off
