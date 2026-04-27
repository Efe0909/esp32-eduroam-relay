# esp32-eduroam-relay

An ESP32 firmware that bridges a WPA2-Enterprise (eduroam) network to a private WPA2 AP. Useful for connecting devices that don't support 802.1X — like 3D printers, lab equipment, or IoT devices — to a university or enterprise network without touching network infrastructure.

```
[your device]  ──WPA2-PSK──►  [ESP32 AP]
                                    │  NAT + DNS proxy
                              [ESP32 STA]  ──WPA2-Enterprise──►  [eduroam / internet]
```

Tested with a Bambu Lab P2S printer on OZU eduroam (PEAP/MSCHAPv2), achieving ~5 Mbps throughput.

---

## Features

- Connects upstream via **WPA2-Enterprise PEAP/MSCHAPv2** (eduroam)
- Rebroadcasts as a private **WPA2-PSK AP**
- **lwIP NAPT** for full routing — clients get real internet, not just local access
- **UDP DNS proxy** — more reliable than DHCP DNS option
- **MAC whitelist** — deauthenticates unlisted devices on connect
- **NVS credential storage** — EAP credentials stored on-device, not in the binary
- **Serial REPL console** — provision credentials, check status, debug DNS, toggle SSID broadcast at runtime
- Plain WPA2-PSK fallback mode for testing without eduroam

---

## Requirements

### Hardware

- Any ESP32 development board (ESP32, ESP32-S2, ESP32-S3, ESP32-C3, etc.)
- USB cable for flashing; USB power brick for deployment

### Software

- [ESP-IDF v5.4.1](https://docs.espressif.com/projects/esp-idf/en/v5.4.1/esp32/get-started/index.html)
- Python 3.12 (the ESP-IDF Python venv must use 3.12 — `idf.py monitor` freezes on Python 3.14)

---

## Installing ESP-IDF

```bash
# Clone ESP-IDF v5.4.1
git clone --recursive --branch v5.4.1 https://github.com/espressif/esp-idf.git ~/esp/esp-idf

# Install toolchain
cd ~/esp/esp-idf
./install.sh esp32        # replace esp32 with your chip variant if needed

# Activate environment (run this in every new shell)
source ~/esp/esp-idf/export.sh
```

Add to your shell profile for convenience:
```bash
alias get_idf='source ~/esp/esp-idf/export.sh'
```

---

## Setup

### 1. Clone this repo

```bash
git clone https://github.com/your-username/esp32-eduroam-relay.git
cd esp32-eduroam-relay
```

### 2. Configure

Edit `main/relay_main.c`:

**AP settings** (the network your devices connect to):
```c
#define AP_SSID     "BambuBridge"       // your AP name
#define AP_PASSWORD "YOUR_AP_PASSWORD"  // at least 8 characters
#define AP_HIDDEN   1                   // 1 = hidden SSID, 0 = visible
```

**MAC whitelist** (if `USE_MAC_FILTER` is enabled):
```c
static const uint8_t ALLOWED_MACS[][6] = {
    { 0xAA, 0xBB, 0xCC, 0xDD, 0xEE, 0x01 },  /* your device */
};
```
To find a device's MAC: connect it (it will be blocked), then run `status` in the serial console — the blocked MAC is logged.

**For plain WPA2 testing** (no eduroam), comment out `#define USE_ENTERPRISE` and fill in:
```c
#define STA_SSID     "YOUR_HOTSPOT_SSID"
#define STA_PASSWORD "YOUR_HOTSPOT_PASSWORD"
```

### 3. Build

```bash
source ~/esp/esp-idf/export.sh
idf.py build
```

---

## Flashing

Find your serial port:
```bash
ls /dev/cu.usbserial-*   # macOS
ls /dev/ttyUSB*          # Linux
```

**First flash** (erases NVS so you start provisioning fresh):
```bash
idf.py -p /dev/cu.usbserial-0001 erase-flash flash monitor
```

**Subsequent flashes** (preserves stored credentials):
```bash
idf.py -p /dev/cu.usbserial-0001 flash monitor
```

Exit the monitor with `Ctrl+]`.

---

## Provisioning EAP Credentials

On first boot with no credentials, the device drops to a serial REPL:

```
W (320) relay: Not provisioned. Connect via serial and run:
W (320) relay:   provision <identity> <username> <password>
relay>
```

Run:
```
relay> provision user@university.edu user@university.edu yourpassword
```

- **identity**: outer EAP identity (sometimes `anonymous@university.edu` — check with your IT)
- **username**: inner EAP identity (usually your full university email)
- **password**: your network password

Credentials are saved to NVS and the device restarts into relay mode. After this, it runs headlessly — just plug into any USB power source.

---

## Serial Console

Connect via `idf.py -p <port> monitor` or any serial terminal at 115200 baud. Exit with `Ctrl+]`.

| Command | Description |
|---|---|
| `status` | STA connection, IP, RSSI, connected AP clients |
| `ssid <show\|hide>` | Toggle AP SSID broadcast at runtime (no restart needed) |
| `monitor <on\|off>` | Print estimated bandwidth every second |
| `dnslog <on\|off>` | Log all DNS queries from AP clients — useful for debugging |
| `reconnect` | Disconnect and reconnect upstream WiFi |
| `restart` | Reboot the ESP32 |
| `provision <id> <user> <pass>` | Store new EAP credentials in NVS and restart |
| `clear_creds` | Wipe stored credentials (next boot drops to REPL) |
| `whoami` | Show stored identity/username |
| `help` | List all commands |

---

## Network Layout

```
AP clients (192.168.4.x)
    192.168.4.1  — ESP32 gateway / DHCP server / DNS proxy
    192.168.4.2+ — connected devices

ESP32 STA — DHCP address from upstream (e.g. 10.x.x.x on eduroam)

DNS: all port-53 UDP queries from AP clients → forwarded to 8.8.8.8
NAT: lwIP NAPT masquerades all AP traffic through the STA IP
```

---

## Known Limitations

**Remote camera / P2P video** won't work through this relay. Services like Bambu Lab's camera stream use IOTC P2P (Tutk), which needs to punch inbound through NAT. With eduroam NAT + ESP32 NAT (double NAT), the punch-through fails. Local LAN streaming works fine if your phone is also on the relay AP.

**iCloud Private Relay** on iOS can break internet access through the relay. Disable it for this network: Settings → Wi-Fi → ⓘ → Private Relay → Off.

**Throughput** is limited by the ESP32's single-core WiFi stack handling both STA and AP simultaneously. Expect 3–6 Mbps in practice.

---

## Feature Flags

At the top of `relay_main.c`:

| Flag | Default | Effect |
|---|---|---|
| `USE_ENTERPRISE` | on | Connect upstream via WPA2-Enterprise. Comment out for plain PSK. |
| `USE_MAC_FILTER` | on | Enforce MAC whitelist on the AP. Comment out to allow all clients. |

---

## Troubleshooting

**`idf.py monitor` freezes** — Python 3.14 bug. Use Python 3.12 for the IDF venv.

**Kill a stray monitor process:**
```bash
pkill -9 -f "idf.py|idf_monitor|esp_idf_monitor"
```

**Printer connects but has no internet** — run `dnslog on` and watch the console while the device tries to connect. If DNS queries appear but traffic doesn't flow, eduroam may be blocking the required ports.

**Device blocked on join** — run `status` to see the blocked MAC, add it to `ALLOWED_MACS`, and reflash.

**Want to reset credentials** — run `clear_creds` in the console, then `restart`. The device drops to the provisioning REPL on next boot.

---

## License

MIT
