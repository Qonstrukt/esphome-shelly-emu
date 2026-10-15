# Design notes: emulating a Shelly Gen2 device for Sigenergy

This file records what was found about how the Sigenergy stack (SigenStor
gateway + mySigen app) talks to Shelly devices, and the design decisions
derived from it. Sources are listed at the bottom.

## 1. How Sigenergy integrates Shelly (findings)

* **Transport is the LAN, not Bluetooth.** The SigenStor gateway and the
  mySigen app discover Shelly Gen2 devices with **mDNS** (`_shelly._tcp`,
  TXT `gen=2`, `app=<App>`, `ver=<version>`) and then use the **Shelly Gen2
  JSON-RPC API over HTTP** (`GET /shelly`, `GET /rpc/<Method>?...`,
  `POST /rpc`). The documented add-device flow in mySigen offers
  "WLAN Network" (device already on Wi-Fi, the common case) and "Bluetooth".
* **Bluetooth is only a provisioning front door.** Shelly's and Sigenergy's
  own guides say the Bluetooth option is for a factory-fresh Shelly that is
  not on Wi-Fi yet: the phone connects to the Shelly over BLE RPC, pushes
  the Wi-Fi credentials (`Wifi.SetConfig`), and after that everything runs
  over the LAN. So BLE is optional for an emulator that is already on Wi-Fi.
  It is still implemented here (opt-in) so the "Bluetooth" path in the app
  can be used if the WLAN scan is flaky.
* **Energy meters are polled over Modbus TCP.** For a Shelly Pro 3EM the
  SigenStor reads live power over **Modbus TCP port 502** (input registers,
  FC04, any unit id; mySigen uses 7) after enrollment. HTTP is only used
  for discovery/identity. (Source: sigelly_emu, a working Pro 3EM emulator.)
* **Switches / plugs are controlled over HTTP RPC.** Tasmota's Shelly
  emulation, written explicitly for mySigen, implements only HTTP polling
  plus `Switch.Set` / `Switch.Toggle` and works. No WebSocket, no MQTT,
  no notifications are needed.
* **Firmware version is checked.** uni-meter users saw "This shelly model
  requires a newer software version" with `fw_id` 1.4.4. Working emulators
  report version `2.0.0` with a 2026 build id. This project defaults to the
  same strings (configurable).
* **`Wifi.GetStatus` must exist.** Sigenergy calls it and logs an error if
  the method is unknown (uni-meter issue #268).
* **`auth_en` must be `false`.** Sigen does not do digest auth.
* **mDNS replies must carry TTL 255.** The Tasmota author found that Shelly
  controllers discard unicast mDNS replies with a lower IP TTL and patches
  the lwIP UDP PCB TTL for port 5353. This project does the same.
* **One IP = one Shelly.** Sigen identifies a device by its IP (port 80 and
  502 are assumed). A single ESP32 therefore emulates one Shelly *device*,
  but that device can have several channels (Pro 2PM = 2, Pro 4PM = 4).
  Shelly's Sigen guide confirms mySigen supports multi-channel devices:
  the first channel is added when pairing, the others are added afterwards
  from the Smart Load section.
* Multi-channel devices need `Shelly.GetComponents`, `Shelly.ListProfiles`
  and per-channel `Switch.*` / `Input.*` methods (all implemented by
  Tasmota; mirrored here).

## 2. Shelly Gen2 identity

| YAML `model` | `model` code     | `app`     | hostname / id prefix | channels |
|--------------|------------------|-----------|----------------------|----------|
| `pro1pm`     | SPSW-201PE16EU   | Pro1PM    | ShellyPro1PM         | 1 switch |
| `pro2pm`     | SPSW-202PE16EU   | Pro2PM    | ShellyPro2PM         | 2 switch |
| `pro4pm`     | SPSW-004PE16EU   | Pro4PM    | ShellyPro4PM         | 4 switch |
| `plus1pm`    | SNSW-001P16EU    | Plus1PM   | ShellyPlus1PM        | 1 switch |
| `plus2pm`    | SNSW-102P16EU    | Plus2PM   | ShellyPlus2PM        | 2 switch |
| `plusplugs`  | SNPL-00112EU     | PlusPlugS | ShellyPlusPlugS      | 1 switch |
| `pro3em`     | SPEM-003CEBEU    | Pro3EM    | ShellyPro3EM         | 3-phase meter |

* `id` = lowercase prefix + `-` + lowercase MAC (`shellypro2pm-aabbccddeeff`)
* mDNS hostname = prefix + `-` + uppercase MAC (`ShellyPro2PM-AABBCCDDEEFF.local`)
* mDNS instance name = `id`, services `_shelly._tcp` (TXT gen/app/ver) and
  `_http._tcp` (TXT gen) on port 80.
* HTTP responses carry `Server: ShellyHTTP/1.0.0`.

Verified 2026-10-15 with a real SigenStor / mySigen: `pro4pm` (four channels
fed from Home Assistant plugs) is discovered via the WLAN scan, pairs, reports
power and energy, and accepts switch commands. The `pro2pm` identity is the one
Tasmota uses and `pro3em` the one sigelly_emu uses; the remaining codes come
from the Shelly API docs and device captures and are not yet verified.

## 3. Architecture

```
Home Assistant  <== native API ==>  ESPHome node (ESP32)
   switch.*  ------ homeassistant switch platform ------>  shelly_emu channel output
   sensor.*  ------ homeassistant sensor platform  ------>  apower / voltage / energy ...
                                                               |
                 +---------------------------------------------+
                 |  shelly_emu component
                 |   * identity (model, id, mac, fw)
                 |   * thread-safe state snapshot
                 |   * JSON-RPC dispatcher (shared by all transports)
                 |   * command queue (Switch.Set -> HA service call on main loop)
                 |
                 +-- HTTP transport  (web_server_base, port 80)  <-- SigenStor / mySigen
                 +-- mDNS            (_shelly._tcp / _http._tcp)  <-- discovery
                 +-- Modbus TCP      (port 502, pro3em only)      <-- SigenStor meter polling
                 +-- BLE RPC         (mOS GATT service, optional) <-- mySigen "Bluetooth" add path
```

Why ESPHome: the Home Assistant bridge is free (native API, state
subscriptions, service calls), and the node still shows up as a normal
ESPHome device. Everything Shelly-specific is one external component.

### Threading

ESP-IDF's HTTP server runs handlers on its own task. ESPHome is not
thread-safe, so:

* The main loop copies sensor/switch states into a `Snapshot` under a mutex.
* RPC handlers (HTTP task) only read a copy of that snapshot.
* `Switch.Set` / `Switch.Toggle` push a command into a mutex-protected
  queue; the main loop calls `turn_on()` / `turn_off()` on the ESPHome switch
  (which for the `homeassistant` platform becomes a HA service call).
  The snapshot is updated optimistically for a few seconds so an immediate
  `Switch.GetStatus` already shows the new state.
* BLE RPC and Modbus run on the main loop (ESPHome event queue / polled
  sockets) and use the same snapshot and queue for uniformity.

### BLE RPC transport (mOS protocol)

Shelly Gen2 devices expose the Mongoose OS RPC-over-GATT service:

| Role        | UUID                                   | properties   |
|-------------|----------------------------------------|--------------|
| service     | `5f6d4f53-5f52-5043-5f53-56435f49445f` |              |
| data        | `5f6d4f53-5f52-5043-5f64-6174615f5f5f` | read, write  |
| tx control  | `5f6d4f53-5f52-5043-5f74-785f63746c5f` | write        |
| rx control  | `5f6d4f53-5f52-5043-5f72-785f63746c5f` | read, notify |

Client -> device: write 4-byte big-endian frame length to *tx control*, then
write the JSON frame to *data* in MTU-sized chunks. Device -> client: the
device sets *rx control* to the 4-byte big-endian response length (and
notifies), the client then reads *data* repeatedly, each read returning the
next chunk, until it has the whole frame.

Advertising: flags + the 128-bit service UUID in the advertisement, complete
local name (`ShellyPro2PM-AABBCCDDEEFF`) in the scan response. Clients find
Shelly devices by the `Shelly` name prefix and/or the service UUID.

Since Shelly firmware 2.0.0 BLE RPC requires bonding except while the device
is in provisioning state. The emulator accepts unpaired connections and
reports `provision: pending` over BLE by default so the app treats it as a
fresh device; `Wifi.SetConfig` is acknowledged and ignored (the node is
already on Wi-Fi via ESPHome).

### Modbus TCP (pro3em)

Register layout follows the Shelly docs (addresses 31000+ for EM, 31160+
for EMData). On the wire they are input registers at `address - 30000`.
Floats are IEEE-754 single with the two 16-bit words swapped (CDAB), same
as a real Pro 3EM. FC03 (holding) returns *illegal data address* like the
real device; only FC04 is served. Any unit id is accepted.

## 4. What is deliberately not implemented

* Shelly Cloud, MQTT, outbound WebSocket, scripts, schedules, webhooks, KVS,
  OTA via RPC, digest authentication. Methods return "Method not found".
* Writing configuration: `*.SetConfig` is acknowledged (`restart_required:
  false`) but ignored, except `Sys.SetConfig` `device.name`, which is kept
  in RAM.
* Running several *distinct* Shelly devices on one ESP32. Sigen addresses a
  device by IP, so this would need multiple IPs. Use multi-channel models
  (Pro 4PM) instead, or one ESP32 per emulated device.

## 5. Sources

* Shelly KB, "For Sigen owners" and "Sigenergy x Shelly":
  https://kb.shelly.cloud/knowledge-base/for-sigen-owners,
  https://kb.shelly.cloud/knowledge-base/kbuca-sigenergy-sigen
* Shelly KB, "Communicating with Shelly devices via Bluetooth":
  https://kb.shelly.cloud/knowledge-base/kbsa-communicating-with-shelly-devices-via-bluetoo
* Shelly Gen2 API docs (Shelly, Switch, Sys, Wifi, EM, EMData, Modbus, BLE):
  https://shelly-api-docs.shelly.cloud/gen2/
* Tasmota Shelly Gen2 emulation (Pro 2PM, built for mySigen):
  https://tasmota.github.io/docs/Shelly-Emulation/ and
  `tasmota/tasmota_xdrv_driver/xdrv_89_esp32_shelly_emulation.ino`
* sigelly_emu, Shelly Pro 3EM emulator for Sigenstor (Modbus TCP finding):
  https://github.com/wgentine/sigelly_emu
* uni-meter issue 268 (firmware version rejection, `WiFi.GetStatus`):
  https://github.com/sdeigm/uni-meter/issues/268
* esp32-shelly-ble-rpc (mOS BLE RPC client, framing details):
  https://github.com/matthias-bs/esp32-shelly-ble-rpc
* Whirlpool forum thread on Sigenergy with Shelly:
  https://forums.whirlpool.net.au/archive/98888676
