# shelly-emu: Shelly Gen2 emulator for ESPHome

Turn an ESP32 running [ESPHome](https://esphome.io) into a **Shelly Gen2 device** on
your LAN, with all data and control coming from **Home Assistant**. Built so a
**Sigenergy SigenStor / mySigen** installation can manage any HA switch as a
"Shelly smart load", or read any HA power source as a "Shelly Pro 3EM" meter,
without buying Shelly hardware.

Emulated models:

| `model`     | Shelly device    | channels | Sigen use            |
|-------------|------------------|----------|----------------------|
| `pro1pm`    | Shelly Pro 1PM   | 1 switch | smart load           |
| `pro2pm`    | Shelly Pro 2PM   | 2 switch | smart load (tested identity, see below) |
| `pro4pm`    | Shelly Pro 4PM   | 4 switch | smart load           |
| `plus1pm`   | Shelly Plus 1PM  | 1 switch | smart load           |
| `plus2pm`   | Shelly Plus 2PM  | 2 switch | smart load           |
| `plusplugs` | Shelly Plus Plug S | 1 switch | smart load         |
| `pro3em`    | Shelly Pro 3EM   | 3-phase meter | energy meter (Modbus TCP) |

What the component provides:

* **HTTP JSON-RPC** (`/shelly`, `/rpc`, `/rpc/<Method>`) with the Shelly Gen2 payloads
  mySigen and the SigenStor gateway expect (`Shelly.*`, `Switch.*`, `Input.*`,
  `Sys.*`, `Wifi.*`, `EM.*`, `EMData.*`, ...).
* **mDNS** advertisement as `ShellyPro2PM-AABBCCDDEEFF.local` with `_shelly._tcp` and
  `_http._tcp`, the way controllers discover Shelly devices.
* **Modbus TCP** (port 502) with the Pro 3EM register map, which is how the
  SigenStor polls meters after enrollment.
* **BLE RPC** (optional): the Shelly GATT RPC service and advertisement, so the
  "Bluetooth" add path in mySigen can see the device like a factory-fresh Shelly.
* **Home Assistant bridge for free**: channel outputs are ESPHome `switch` entities
  (normally `platform: homeassistant`), power/energy values are ESPHome `sensor`
  entities. `Switch.Set` from Sigen becomes a `switch.turn_on/off` in HA.

Read [docs/DESIGN.md](docs/DESIGN.md) for the protocol findings behind these choices
(and the sources).

## Status

Freshly written and compiled, **not yet paired with a real Sigenergy system**. The HTTP
surface mirrors two emulators that are known to work with mySigen (Tasmota's Pro 2PM
emulation and sigelly_emu's Pro 3EM), so the `pro2pm` and `pro3em` identities are the
best bets for a first test. BLE is experimental. Please report what works.

## Quick start

1. Put the `components/` folder somewhere ESPHome can see it, or reference this repo
   with `external_components`.
2. Define the HA entities you want to expose as ESPHome `homeassistant` switches and
   sensors.
3. Add a `shelly_emu:` block. Minimal Pro 2PM:

```yaml
api:
  homeassistant_services: true   # lets the node call switch.turn_on/off in HA

external_components:
  - source: github://Qonstrukt/shelly-emu
    components: [shelly_emu]

switch:
  - platform: homeassistant
    id: ha_boiler
    entity_id: switch.boiler

sensor:
  - platform: homeassistant
    id: ha_boiler_power
    entity_id: sensor.boiler_power

shelly_emu:
  model: pro2pm
  name: "Shelly emu"
  switches:
    - switch: ha_boiler
      name: "Boiler"
      power: ha_boiler_power
```

4. In Home Assistant, open the ESPHome integration entry for the node and enable
   **"Allow the device to perform Home Assistant actions"**. Without it the node can
   read states but cannot switch anything.
5. Flash, check the log for `mDNS: ShellyPro2PM-....local advertising _shelly._tcp`,
   then run `python3 tools/validate.py http://<node-ip>`.
6. mySigen: **Add device → Smart Load → WLAN Network**, pick the device, give it a
   name. For multi-channel models only the first channel is added during pairing;
   add the others from the Smart Load section afterwards (that is how mySigen handles
   real Pro 2PM / 4PM units).

Complete examples: [examples/](examples/).

## Configuration

```yaml
shelly_emu:
  model: pro2pm              # required, see table above
  name: "Living room"        # Shelly device name (sys.device.name)
  mac: AABBCCDDEEFF          # optional, defaults to the Wi-Fi MAC; fixes the Shelly id
  model_code: SPSW-202PE16EU # optional override of the reported model code
  firmware_version: "2.0.0"  # reported "ver" (mySigen rejects old versions)
  fw_id: "20260710-101218/2.0.0-g87fbfa4"
  timezone: Europe/Amsterdam # only reported in Sys.GetConfig
  ble: false                 # enable the BLE RPC service + advertising (experimental)
  ble_provision_state: pending   # what GetDeviceInfo reports over BLE: pending|complete
  modbus: false              # Modbus TCP server (default true for pro3em)
  modbus_port: 502

  switches:                  # 1..N entries, N = channels of the model
    - switch: ha_boiler      # any ESPHome switch; its state is the Shelly output
      name: "Boiler"
      power: ha_boiler_power          # W   (optional, default 0)
      voltage: ha_boiler_voltage      # V   (optional)
      current: ha_boiler_current      # A   (optional, derived from P/V if absent)
      energy: ha_boiler_energy        # total energy (optional; integrated from power if absent)
      energy_returned: ...            # optional
      energy_unit: kWh                # kWh (default) or Wh, applies to energy sensors
      frequency: ...                  # Hz (optional)
      power_factor: ...               # optional
      temperature: ...                # °C (optional)
      input: some_binary_sensor       # optional, reported as input:N

  em:                        # pro3em only
    energy_unit: kWh
    neutral_current: ...     # optional
    temperature: ...         # optional
    phases:                  # 1..3 entries (a, b, c); missing phases report 0 W
      - power: p1_power_l1   # W, negative = export
        voltage: ...         # V (default 230)
        current: ...         # A (derived if absent)
        apparent_power: ...  # VA (derived if absent)
        power_factor: ...    # default 1
        frequency: ...       # default 50
        energy: ...          # import counter
        energy_returned: ... # export counter
```

Notes:

* One ESP32 emulates **one** Shelly device. Sigen identifies devices by IP address, so
  several independent Shellys would need several IPs. Use `pro4pm` for up to four
  independently controlled loads on one board, or one board per device.
* `ble: true` pulls in the ESP32 BLE stack. Keep such a node small (no web_server,
  no bluetooth_proxy) and prefer a board with PSRAM.
* The HTTP API is unauthenticated, as Sigen requires (`auth_en: false`). Put the node
  on a trusted LAN/VLAN.
* `*.SetConfig` calls are acknowledged but ignored, except `Sys.SetConfig` device name.

## Testing without a Sigen

```bash
python3 tools/validate.py http://<node-ip>            # HTTP RPC surface
python3 tools/validate.py http://<node-ip> --switch   # also toggles switch:0 off/on
python3 tools/modbus_smoke.py <node-ip>               # Modbus registers (pro3em)
```

`dns-sd -B _shelly._tcp` (macOS) or `avahi-browse -r _shelly._tcp` (Linux) shows the
mDNS advertisement.

## Project layout

```
components/shelly_emu/   ESPHome external component
  __init__.py            YAML schema and code generation
  shelly_emu.*           core: identity, HA snapshot, command queue, mDNS
  shelly_rpc.cpp         JSON-RPC dispatcher and Shelly payload builders
  shelly_http.*          HTTP transport (web_server_base / ESP-IDF httpd)
  shelly_ble.*           BLE RPC transport (optional)
  shelly_modbus.*        Modbus TCP server (optional / pro3em)
examples/                ready-to-adapt node configs
tests/                   compile-check configs
tools/                   validate.py, modbus_smoke.py
docs/DESIGN.md           protocol findings and architecture
```

## Credits

Protocol knowledge comes from the Shelly Gen2 API documentation, Tasmota's Shelly
emulation driver (Rene Hexel), sigelly_emu (W. Gentine), the uni-meter issue tracker
and the esp32-shelly-ble-rpc library (matthias-bs). See docs/DESIGN.md.

## License

MIT
