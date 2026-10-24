// JSON-RPC method dispatch and the Shelly Gen2 payload builders.
// Shapes follow the Shelly Gen2 API docs and the payloads of the Tasmota (Pro 2PM) and
// sigelly_emu (Pro 3EM) emulators that are known to pair with Sigenergy mySigen.
#include "shelly_emu.h"
#ifdef USE_ESP32

#include <cstring>
#include <ctime>
#include <esp_heap_caps.h>

#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/hal.h"
#include "esphome/core/log.h"
#ifdef USE_WIFI
#include "esphome/components/wifi/wifi_component.h"
#endif

namespace esphome::shelly_emu {

static const char *const TAG = "shelly_emu.rpc";

static double r1(double v) { return std::round(v * 10.0) / 10.0; }
static double r2(double v) { return std::round(v * 100.0) / 100.0; }
static double r3(double v) { return std::round(v * 1000.0) / 1000.0; }

static void set_float_or_null(JsonObject o, const char *key, float v, double (*round)(double)) {
  if (std::isnan(v)) {
    o[key] = nullptr;
  } else {
    o[key] = round(v);
  }
}

static bool time_synced(time_t &now) {
  now = ::time(nullptr);
  return now > 1600000000;
}

static uint32_t minute_ts() {
  time_t now;
  if (!time_synced(now))
    return 0;
  return static_cast<uint32_t>(now - (now % 60));
}

static std::string ipv4_string() {
  auto ips = network::get_ip_addresses();
  for (const auto &ip : ips) {
    if (ip.is_set() && ip.is_ip4()) {
      char buf[network::IP_ADDRESS_BUFFER_SIZE];
      ip.str_to(buf);
      return buf;
    }
  }
  return "0.0.0.0";
}

int ShellyEmu::param_id_(JsonVariantConst params) {
  JsonVariantConst id = params["id"];
  if (id.is<int>())
    return id.as<int>();
  if (id.is<const char *>()) {
    const char *s = id.as<const char *>();
    if (s != nullptr && isdigit(static_cast<unsigned char>(s[0])))
      return atoi(s);
  }
  return -1;
}

// ---------------------------------------------------------------------------
// Switch / Input components
// ---------------------------------------------------------------------------

void ShellyEmu::build_switch_status_(JsonObject o, uint8_t index, const SwitchState &s) {
  o["id"] = this->switch_component_id(index);
  o["source"] = "init";
  if (this->output_addon_) {
    // Pro Output Add-on relay: no power metering, just the output state.
    o["output"] = s.output;
    JsonObject t = o["temperature"].to<JsonObject>();
    t["tC"] = std::isnan(s.temp) ? 0.0 : r1(s.temp);
    t["tF"] = std::isnan(s.temp) ? 32.0 : r1(s.temp * 9.0 / 5.0 + 32.0);
    return;
  }
  o["tag"] = nullptr;
  o["output"] = s.output;
  o["apower"] = r1(s.apower);
  o["voltage"] = std::isnan(s.voltage) ? 0.0 : r1(s.voltage);
  o["freq"] = std::isnan(s.freq) ? 0.0 : r1(s.freq);
  o["current"] = r3(s.current);
  o["pf"] = r2(s.pf);
  JsonObject ae = o["aenergy"].to<JsonObject>();
  ae["total"] = r3(s.aenergy);
  JsonArray bm = ae["by_minute"].to<JsonArray>();
  bm.add(0.0);
  bm.add(0.0);
  bm.add(0.0);
  ae["minute_ts"] = minute_ts();
  JsonObject re = o["ret_aenergy"].to<JsonObject>();
  re["total"] = r3(s.ret_aenergy);
  JsonArray rbm = re["by_minute"].to<JsonArray>();
  rbm.add(0.0);
  rbm.add(0.0);
  rbm.add(0.0);
  re["minute_ts"] = minute_ts();
  JsonObject counts = o["counts"].to<JsonObject>();
  counts["on_time"] = 0;
  counts["on_time_rst_ts"] = 0;
  counts["switch_on"] = 0;
  counts["switch_on_rst_ts"] = 0;
  counts["on_above_thr"] = 0;
  counts["on_above_thr_rst_ts"] = 0;
  JsonObject t = o["temperature"].to<JsonObject>();
  if (std::isnan(s.temp)) {
    t["tC"] = 0.0;
    t["tF"] = 32.0;
  } else {
    t["tC"] = r1(s.temp);
    t["tF"] = r1(s.temp * 9.0 / 5.0 + 32.0);
  }
}

void ShellyEmu::build_switch_config_(JsonObject o, uint8_t index) {
  o["id"] = this->switch_component_id(index);
  const char *name = index < this->switch_count_ ? this->channels_[index].name : "";
  if (name != nullptr && name[0] != '\0') {
    o["name"] = name;
  } else {
    o["name"] = nullptr;
  }
  if (this->output_addon_) {
    o["initial_state"] = "restore_last";
    o["auto_on"] = false;
    o["auto_on_delay"] = 60.0;
    o["auto_off"] = false;
    o["auto_off_delay"] = 60.0;
    return;
  }
  o["in_mode"] = "detached";
  o["in_locked"] = false;
  o["initial_state"] = "restore_last";
  o["auto_on"] = false;
  o["auto_on_delay"] = 60.0;
  o["auto_off"] = false;
  o["auto_off_delay"] = 60.0;
  o["power_limit"] = 4480;
  o["voltage_limit"] = 280;
  o["undervoltage_limit"] = 0;
  o["autorecover_voltage_errors"] = false;
  o["current_limit"] = 16.0;
  o["reverse"] = false;
  JsonObject counts = o["counts"].to<JsonObject>();
  counts["enable"] = true;
  counts["power_thr"] = 100;
}

void ShellyEmu::build_input_status_(JsonObject o, uint8_t id, const SwitchState &s) {
  o["id"] = id;
  o["state"] = s.input;
}

void ShellyEmu::build_input_config_(JsonObject o, uint8_t id) {
  o["id"] = id;
  o["name"] = nullptr;
  o["type"] = "switch";
  o["enable"] = true;
  o["invert"] = false;
}

// ---------------------------------------------------------------------------
// EM / EMData / Temperature (Pro 3EM)
// ---------------------------------------------------------------------------

void ShellyEmu::build_em_status_(JsonObject o, const EmState &em) {
  static const char PHASES[EM_PHASES] = {'a', 'b', 'c'};
  o["id"] = 0;
  double total_current = 0, total_act = 0, total_aprt = 0;
  for (uint8_t p = 0; p < EM_PHASES; p++) {
    const auto &e = em.phase[p];
    char key[16];
    snprintf(key, sizeof(key), "%c_current", PHASES[p]);
    o[std::string(key)] = r3(e.current);
    snprintf(key, sizeof(key), "%c_voltage", PHASES[p]);
    o[std::string(key)] = r1(e.voltage);
    snprintf(key, sizeof(key), "%c_act_power", PHASES[p]);
    o[std::string(key)] = r1(e.act_power);
    snprintf(key, sizeof(key), "%c_aprt_power", PHASES[p]);
    o[std::string(key)] = r1(e.aprt_power);
    snprintf(key, sizeof(key), "%c_pf", PHASES[p]);
    o[std::string(key)] = r2(e.pf);
    snprintf(key, sizeof(key), "%c_freq", PHASES[p]);
    o[std::string(key)] = r1(e.freq);
    total_current += e.current;
    total_act += e.act_power;
    total_aprt += e.aprt_power;
  }
  set_float_or_null(o, "n_current", em.n_current, r3);
  o["total_current"] = r3(total_current);
  o["total_act_power"] = r3(total_act);
  o["total_aprt_power"] = r3(total_aprt);
  o["user_calibrated_phase"].to<JsonArray>();
}

void ShellyEmu::build_em_config_(JsonObject o) {
  o["id"] = 0;
  o["name"] = nullptr;
  o["blink_mode_selector"] = "active_energy";
  o["phase_selector"] = "all";
  o["monitor_phase_sequence"] = false;
  o["ct_type"] = "120A";
  o["reverse"].to<JsonObject>();
  JsonObject alarms = o["alarms"].to<JsonObject>();
  for (const char *phase : {"a", "b", "c"}) {
    JsonObject ph = alarms[phase].to<JsonObject>();
    for (const char *kind : {"voltage", "current", "power"}) {
      JsonArray arr = ph[kind].to<JsonArray>();
      arr.add(nullptr);
      arr.add(nullptr);
    }
  }
}

void ShellyEmu::build_emdata_status_(JsonObject o, const EmState &em) {
  o["id"] = 0;
  double total = 0, total_ret = 0;
  o["a_total_act_energy"] = r2(em.phase[0].energy);
  o["a_total_act_ret_energy"] = r2(em.phase[0].ret_energy);
  o["b_total_act_energy"] = r2(em.phase[1].energy);
  o["b_total_act_ret_energy"] = r2(em.phase[1].ret_energy);
  o["c_total_act_energy"] = r2(em.phase[2].energy);
  o["c_total_act_ret_energy"] = r2(em.phase[2].ret_energy);
  for (const auto &e : em.phase) {
    total += e.energy;
    total_ret += e.ret_energy;
  }
  o["total_act"] = r2(total);
  o["total_act_ret"] = r2(total_ret);
}

void ShellyEmu::build_temperature_status_(JsonObject o, const EmState &em) {
  o["id"] = 0;
  const float t = std::isnan(em.temp) ? 45.0f : em.temp;
  o["tC"] = r1(t);
  o["tF"] = r1(t * 9.0 / 5.0 + 32.0);
}

void ShellyEmu::build_temperature_config_(JsonObject o) {
  o["id"] = 0;
  o["name"] = nullptr;
  o["report_thr_C"] = 5.0;
  o["offset_C"] = 0.0;
}

// ---------------------------------------------------------------------------
// Sys / Wifi / Eth / BLE / Cloud / Mqtt / WS
// ---------------------------------------------------------------------------

void ShellyEmu::build_sys_status_(JsonObject o) {
  o["mac"] = this->mac_;
  o["restart_required"] = false;
  time_t now;
  if (time_synced(now)) {
    struct tm lt{};
    localtime_r(&now, &lt);
    char tbuf[8];
    strftime(tbuf, sizeof(tbuf), "%H:%M", &lt);
    o["time"] = std::string(tbuf);
    o["unixtime"] = static_cast<uint32_t>(now);
    o["last_sync_ts"] = static_cast<uint32_t>(now);
    struct tm gm{};
    gmtime_r(&now, &gm);
    gm.tm_isdst = -1;
    o["utc_offset"] = static_cast<int32_t>(now - mktime(&gm));
  } else {
    o["time"] = nullptr;
    o["unixtime"] = nullptr;
    o["last_sync_ts"] = nullptr;
    o["utc_offset"] = 0;
  }
  o["uptime"] = millis() / 1000;
  o["ram_size"] = static_cast<uint32_t>(heap_caps_get_total_size(MALLOC_CAP_INTERNAL));
  o["ram_free"] = static_cast<uint32_t>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
  o["ram_min_free"] = static_cast<uint32_t>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
  o["fs_size"] = 458752;
  o["fs_free"] = 212992;
  o["cfg_rev"] = this->cfg_rev_;
  o["kvs_rev"] = 0;
  o["schedule_rev"] = 0;
  o["webhook_rev"] = 0;
  o["btrelay_rev"] = 0;
  if (!this->is_meter())
    o["bthc_rev"] = 0;
  o["available_updates"].to<JsonObject>();
  o["reset_reason"] = 3;
}

void ShellyEmu::build_sys_config_(JsonObject o) {
  JsonObject dev = o["device"].to<JsonObject>();
  if (this->device_name_.empty()) {
    dev["name"] = nullptr;
  } else {
    dev["name"] = this->device_name_;
  }
  dev["mac"] = this->mac_;
  dev["fw_id"] = this->fw_id_;
  dev["discoverable"] = true;
  dev["eco_mode"] = false;
  if (this->is_meter()) {
    dev["profile"] = "triphase";
  } else if (strcmp(this->app_, "Pro2PM") == 0 || strcmp(this->app_, "Plus2PM") == 0) {
    dev["profile"] = "switch";
  }
  if (this->has_output_addon()) {
    dev["addon_type"] = "prooutput";
  } else {
    dev["addon_type"] = nullptr;
  }
  dev["sys_btn_toggle"] = false;
  dev["tls_check_cert_validity_time"] = true;
  dev["enhanced_security"] = false;
  JsonObject loc = o["location"].to<JsonObject>();
  loc["tz"] = this->tz_;
  loc["lat"] = 0.0;
  loc["lon"] = 0.0;
  JsonObject dbg = o["debug"].to<JsonObject>();
  dbg["level"] = 2;
  dbg["file_level"] = nullptr;
  dbg["mqtt"]["enable"] = false;
  dbg["websocket"]["enable"] = false;
  dbg["file_log"]["enable"] = false;
  dbg["udp"]["addr"] = nullptr;
  o["ui_data"]["device_revision"] = "1-22";
  JsonObject udp = o["rpc_udp"].to<JsonObject>();
  udp["dst_addr"] = nullptr;
  udp["listen_port"] = nullptr;
  o["sntp"]["server"] = "time.google.com";
  o["cfg_rev"] = this->cfg_rev_;
}

void ShellyEmu::build_wifi_status_(JsonObject o) {
#ifdef USE_WIFI
  auto *w = wifi::global_wifi_component;
  const bool connected = w != nullptr && w->is_connected();
  o["sta_ip"] = ipv4_string();
  o["status"] = connected ? "got ip" : "disconnected";
  if (connected) {
    char ssid[wifi::SSID_BUFFER_SIZE];
    o["ssid"] = std::string(w->wifi_ssid_to(ssid));
    o["channel"] = w->get_wifi_channel();
    o["rssi"] = w->wifi_rssi();
    auto bssid = w->wifi_bssid();
    char bbuf[18];
    snprintf(bbuf, sizeof(bbuf), "%02x:%02x:%02x:%02x:%02x:%02x", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4],
             bssid[5]);
    o["bssid"] = std::string(bbuf);
  } else {
    o["ssid"] = nullptr;
    o["rssi"] = 0;
  }
  o["sta_ip6"].to<JsonArray>();
#else
  o["sta_ip"] = "0.0.0.0";
  o["status"] = "disconnected";
  o["ssid"] = nullptr;
  o["sta_ip6"] = nullptr;
#endif
}

void ShellyEmu::build_wifi_config_(JsonObject o) {
  JsonObject ap = o["ap"].to<JsonObject>();
  ap["ssid"] = this->hostname_;
  ap["is_open"] = true;
  ap["enable"] = false;
  ap["range_extender"]["enable"] = false;
  JsonObject sta = o["sta"].to<JsonObject>();
#ifdef USE_WIFI
  {
    char ssid[wifi::SSID_BUFFER_SIZE];
    auto *w = wifi::global_wifi_component;
    if (w != nullptr && w->is_connected()) {
      sta["ssid"] = std::string(w->wifi_ssid_to(ssid));
    } else {
      sta["ssid"] = nullptr;
    }
    sta["is_open"] = false;
    sta["enable"] = true;
  }
#else
  sta["ssid"] = nullptr;
  sta["is_open"] = true;
  sta["enable"] = false;
#endif
  sta["ipv4mode"] = "dhcp";
  sta["ip"] = nullptr;
  sta["netmask"] = nullptr;
  sta["gw"] = nullptr;
  sta["nameserver"] = nullptr;
  JsonObject sta1 = o["sta1"].to<JsonObject>();
  sta1["ssid"] = nullptr;
  sta1["is_open"] = true;
  sta1["enable"] = false;
  sta1["ipv4mode"] = "dhcp";
  sta1["ip"] = nullptr;
  sta1["netmask"] = nullptr;
  sta1["gw"] = nullptr;
  sta1["nameserver"] = nullptr;
  JsonObject roam = o["roam"].to<JsonObject>();
  roam["rssi_thr"] = -80;
  roam["interval"] = 60;
}

void ShellyEmu::build_wifi_scan_(JsonObject o) {
  JsonArray results = o["results"].to<JsonArray>();
#ifdef USE_WIFI
  auto *w = wifi::global_wifi_component;
  if (w != nullptr && w->is_connected()) {
    JsonObject ap = results.add<JsonObject>();
    char ssid[wifi::SSID_BUFFER_SIZE];
    ap["ssid"] = std::string(w->wifi_ssid_to(ssid));
    auto bssid = w->wifi_bssid();
    char bbuf[18];
    snprintf(bbuf, sizeof(bbuf), "%02x:%02x:%02x:%02x:%02x:%02x", bssid[0], bssid[1], bssid[2], bssid[3], bssid[4],
             bssid[5]);
    ap["bssid"] = std::string(bbuf);
    ap["auth"] = 3;
    ap["channel"] = w->get_wifi_channel();
    ap["rssi"] = w->wifi_rssi();
  }
#endif
}

void ShellyEmu::build_eth_status_(JsonObject o) {
#ifdef USE_ETHERNET
  o["ip"] = ipv4_string();
#else
  o["ip"] = nullptr;
#endif
  o["ip6"] = nullptr;
}

void ShellyEmu::build_eth_config_(JsonObject o) {
  o["enable"] = true;
  o["server_mode"] = false;
  o["ipv4mode"] = "dhcp";
  o["ip"] = nullptr;
  o["netmask"] = nullptr;
  o["gw"] = nullptr;
  o["nameserver"] = nullptr;
}

void ShellyEmu::build_ble_status_(JsonObject o) {
  // Shelly Gen2 derives its public BLE address from the Wi-Fi MAC + 2.
  unsigned int b[6];
  sscanf(this->mac_.c_str(), "%2x%2x%2x%2x%2x%2x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]);
  b[5] = (b[5] + 2) & 0xFF;
  char addr[24];
  snprintf(addr, sizeof(addr), "%02x:%02x:%02x:%02x:%02x:%02x,p", b[0], b[1], b[2], b[3], b[4], b[5]);
  o["addr"] = std::string(addr);
  o["flags"].to<JsonArray>();
}

void ShellyEmu::build_ble_config_(JsonObject o) {
#ifdef USE_SHELLY_EMU_BLE
  o["rpc"]["enable"] = true;
#else
  o["rpc"]["enable"] = false;
#endif
}

void ShellyEmu::build_cloud_status_(JsonObject o) { o["connected"] = false; }
void ShellyEmu::build_cloud_config_(JsonObject o) {
  o["enable"] = false;
  o["server"] = "iot.shelly.cloud:6012/jrpc";
}
void ShellyEmu::build_mqtt_status_(JsonObject o) { o["connected"] = false; }
void ShellyEmu::build_mqtt_config_(JsonObject o) {
  o["enable"] = false;
  o["server"] = nullptr;
  o["client_id"] = this->device_id_;
  o["user"] = nullptr;
  o["ssl_ca"] = nullptr;
  o["topic_prefix"] = this->device_id_;
  o["rpc_ntf"] = true;
  o["status_ntf"] = false;
  o["use_client_cert"] = false;
  o["enable_rpc"] = true;
  o["enable_control"] = true;
}
void ShellyEmu::build_ws_status_(JsonObject o) { o["connected"] = false; }
void ShellyEmu::build_ws_config_(JsonObject o) {
  o["enable"] = false;
  o["server"] = nullptr;
  o["ssl_ca"] = "ca.pem";
}
void ShellyEmu::build_knx_config_(JsonObject o) {
  o["enable"] = false;
  o["ia"] = "15.15.255";
  o["routing"]["addr"] = "224.0.23.12:3671";
}

// ---------------------------------------------------------------------------
// Device info, component enumeration
// ---------------------------------------------------------------------------

void ShellyEmu::build_device_info(JsonObject o, bool via_ble) {
  if (this->device_name_.empty()) {
    o["name"] = nullptr;
  } else {
    o["name"] = this->device_name_;
  }
  o["id"] = this->device_id_;
  o["mac"] = this->mac_;
  o["slot"] = 0;
  o["model"] = this->model_code_;
  o["gen"] = 2;
  o["fw_id"] = this->fw_id_;
  o["ver"] = this->fw_version_;
  o["app"] = this->app_;
  o["auth_en"] = false;
  o["auth_domain"] = nullptr;
  if (this->is_meter()) {
    o["profile"] = "triphase";
  } else if (strcmp(this->app_, "Pro2PM") == 0 || strcmp(this->app_, "Plus2PM") == 0) {
    o["profile"] = "switch";
  }
  o["provision"] = via_ble ? this->get_ble_provision_state() : "complete";
  o["enhanced_security"] = false;
}

void ShellyEmu::component_keys_(std::vector<std::string> &keys) {
  const bool has_inputs = strcmp(this->app_, "PlusPlugS") != 0;
  keys.emplace_back("ble");
  keys.emplace_back("bthome");
  keys.emplace_back("cloud");
  if (this->is_meter()) {
    keys.emplace_back("em:0");
    keys.emplace_back("emdata:0");
  }
  if (this->is_pro_)
    keys.emplace_back("eth");
  if (!this->is_meter() && has_inputs) {
    for (uint8_t i = 0; i < this->switch_count_; i++)
      keys.emplace_back("input:" + std::to_string(i));
  }
  if (this->is_pro_ && !this->is_meter())
    keys.emplace_back("knx");
  if (this->is_meter())
    keys.emplace_back("modbus");
  keys.emplace_back("mqtt");
  for (uint8_t i = 0; i < this->switch_count_; i++)
    keys.emplace_back("switch:" + std::to_string(this->switch_component_id(i)));
  keys.emplace_back("sys");
  if (this->is_meter())
    keys.emplace_back("temperature:0");
  keys.emplace_back("wifi");
  keys.emplace_back("ws");
}

bool ShellyEmu::component_status_(const std::string &key, JsonObject o, const Snapshot &snap) {
  if (key == "ble") {
    this->build_ble_status_(o);
  } else if (key == "bthome" || key == "knx" || key == "modbus") {
    // empty object
  } else if (key == "cloud") {
    this->build_cloud_status_(o);
  } else if (key == "em:0") {
    this->build_em_status_(o, snap.em);
  } else if (key == "emdata:0") {
    this->build_emdata_status_(o, snap.em);
  } else if (key == "eth") {
    this->build_eth_status_(o);
  } else if (key.rfind("input:", 0) == 0) {
    const int id = atoi(key.c_str() + 6);
    this->build_input_status_(o, id, snap.sw[id]);
  } else if (key == "mqtt") {
    this->build_mqtt_status_(o);
  } else if (key.rfind("switch:", 0) == 0) {
    const int index = this->switch_index_for_id(atoi(key.c_str() + 7));
    if (index < 0)
      return false;
    this->build_switch_status_(o, index, snap.sw[index]);
  } else if (key == "sys") {
    this->build_sys_status_(o);
  } else if (key == "temperature:0") {
    this->build_temperature_status_(o, snap.em);
  } else if (key == "wifi") {
    this->build_wifi_status_(o);
  } else if (key == "ws") {
    this->build_ws_status_(o);
  } else {
    return false;
  }
  return true;
}

bool ShellyEmu::component_config_(const std::string &key, JsonObject o) {
  if (key == "ble") {
    this->build_ble_config_(o);
  } else if (key == "bthome" || key == "emdata:0") {
    // empty object
  } else if (key == "cloud") {
    this->build_cloud_config_(o);
  } else if (key == "em:0") {
    this->build_em_config_(o);
  } else if (key == "eth") {
    this->build_eth_config_(o);
  } else if (key.rfind("input:", 0) == 0) {
    this->build_input_config_(o, atoi(key.c_str() + 6));
  } else if (key == "knx") {
    this->build_knx_config_(o);
  } else if (key == "modbus") {
    o["enable"] = true;
  } else if (key == "mqtt") {
    this->build_mqtt_config_(o);
  } else if (key.rfind("switch:", 0) == 0) {
    const int index = this->switch_index_for_id(atoi(key.c_str() + 7));
    if (index < 0)
      return false;
    this->build_switch_config_(o, index);
  } else if (key == "sys") {
    this->build_sys_config_(o);
  } else if (key == "temperature:0") {
    this->build_temperature_config_(o);
  } else if (key == "wifi") {
    this->build_wifi_config_(o);
  } else if (key == "ws") {
    this->build_ws_config_(o);
  } else {
    return false;
  }
  return true;
}

void ShellyEmu::build_full_status_(JsonObject o, const Snapshot &snap) {
  std::vector<std::string> keys;
  this->component_keys_(keys);
  for (const auto &key : keys)
    this->component_status_(key, o[key].to<JsonObject>(), snap);
}

void ShellyEmu::build_full_config_(JsonObject o) {
  std::vector<std::string> keys;
  this->component_keys_(keys);
  for (const auto &key : keys)
    this->component_config_(key, o[key].to<JsonObject>());
}

void ShellyEmu::build_components_(JsonObject o, JsonVariantConst params, const Snapshot &snap) {
  bool want_status = true, want_config = true;
  JsonArrayConst include = params["include"];
  if (!include.isNull()) {
    want_status = false;
    want_config = false;
    for (JsonVariantConst v : include) {
      const char *s = v.as<const char *>();
      if (s == nullptr)
        continue;
      if (strcmp(s, "status") == 0)
        want_status = true;
      if (strcmp(s, "config") == 0)
        want_config = true;
    }
  }
  const bool dynamic_only = params["dynamic_only"] | false;
  int offset = params["offset"] | 0;
  if (offset < 0)
    offset = 0;

  std::vector<std::string> keys;
  if (!dynamic_only)
    this->component_keys_(keys);
  JsonArray comps = o["components"].to<JsonArray>();
  for (size_t i = offset; i < keys.size(); i++) {
    JsonObject c = comps.add<JsonObject>();
    c["key"] = keys[i];
    if (want_status)
      this->component_status_(keys[i], c["status"].to<JsonObject>(), snap);
    if (want_config)
      this->component_config_(keys[i], c["config"].to<JsonObject>());
  }
  o["cfg_rev"] = this->cfg_rev_;
  o["offset"] = offset;
  o["total"] = static_cast<uint32_t>(keys.size());
}

void ShellyEmu::build_list_methods_(JsonObject o) {
  JsonArray m = o["methods"].to<JsonArray>();
  static const char *const COMMON[] = {
      "Shelly.GetDeviceInfo", "Shelly.GetStatus", "Shelly.GetConfig", "Shelly.GetComponents", "Shelly.ListMethods",
      "Shelly.CheckForUpdate", "Shelly.Reboot",   "RPC.Ping",         "Sys.GetStatus",        "Sys.GetConfig",
      "Sys.SetConfig",        "Wifi.GetStatus",   "Wifi.GetConfig",   "Wifi.SetConfig",       "Wifi.Scan",
      "BLE.GetStatus",        "BLE.GetConfig",    "BLE.SetConfig",    "Cloud.GetStatus",      "Cloud.GetConfig",
      "Cloud.SetConfig",      "Mqtt.GetStatus",   "Mqtt.GetConfig",   "WS.GetStatus",         "WS.GetConfig",
      "Schedule.List",        "Webhook.List",     "Script.List",      "KVS.GetMany",
  };
  for (const char *name : COMMON)
    m.add(name);
  if (this->is_pro_) {
    m.add("Eth.GetStatus");
    m.add("Eth.GetConfig");
  }
  if (this->is_meter()) {
    for (const char *name : {"EM.GetStatus", "EM.GetConfig", "EMData.GetStatus", "EMData.GetConfig",
                             "Temperature.GetStatus", "Temperature.GetConfig", "Modbus.GetStatus", "Modbus.GetConfig"})
      m.add(name);
    if (this->has_output_addon()) {
      for (const char *name : {"Switch.GetStatus", "Switch.GetConfig", "Switch.SetConfig", "Switch.Set", "Switch.Toggle",
                               "ProOutputAddon.GetPeripherals", "ProOutputAddon.AddPeripheral",
                               "ProOutputAddon.RemovePeripheral"})
        m.add(name);
    }
  } else {
    for (const char *name : {"Switch.GetStatus", "Switch.GetConfig", "Switch.SetConfig", "Switch.Set", "Switch.Toggle",
                             "Switch.ResetCounters", "Input.GetStatus", "Input.GetConfig", "Input.SetConfig"})
      m.add(name);
    if (strcmp(this->app_, "Pro2PM") == 0 || strcmp(this->app_, "Plus2PM") == 0)
      m.add("Shelly.ListProfiles");
  }
}

void ShellyEmu::build_list_profiles_(JsonObject o) {
  JsonObject profiles = o["profiles"].to<JsonObject>();
  JsonArray cover = profiles["cover"]["components"].to<JsonArray>();
  JsonObject c0 = cover.add<JsonObject>();
  c0["type"] = "input";
  c0["count"] = 2;
  JsonObject c1 = cover.add<JsonObject>();
  c1["type"] = "cover";
  c1["count"] = 1;
  JsonArray sw = profiles["switch"]["components"].to<JsonArray>();
  JsonObject s0 = sw.add<JsonObject>();
  s0["type"] = "input";
  s0["count"] = 2;
  JsonObject s1 = sw.add<JsonObject>();
  s1["type"] = "switch";
  s1["count"] = 2;
}

// ---------------------------------------------------------------------------
// Dispatcher
// ---------------------------------------------------------------------------

RpcError ShellyEmu::rpc_call(const char *method, JsonVariantConst params, JsonDocument &result, bool via_ble) {
  const Snapshot snap = this->get_snapshot();
  JsonObject o = result.to<JsonObject>();
  auto is = [method](const char *m) { return strcasecmp(method, m) == 0; };
  auto starts = [method](const char *p) { return strncasecmp(method, p, strlen(p)) == 0; };
  auto ack = [&o]() { o["restart_required"] = false; };

  if (via_ble)
    ESP_LOGD(TAG, "BLE %s", method);

  // ---- Shelly.* ----
  if (is("Shelly.GetDeviceInfo")) {
    this->build_device_info(o, via_ble);
  } else if (is("Shelly.GetStatus")) {
    this->build_full_status_(o, snap);
  } else if (is("Shelly.GetConfig")) {
    this->build_full_config_(o);
  } else if (is("Shelly.GetComponents")) {
    this->build_components_(o, params, snap);
  } else if (is("Shelly.ListMethods")) {
    this->build_list_methods_(o);
  } else if (is("Shelly.ListProfiles")) {
    if (this->is_meter() || (strcmp(this->app_, "Pro2PM") != 0 && strcmp(this->app_, "Plus2PM") != 0))
      return RpcError::method_not_found();
    this->build_list_profiles_(o);
  } else if (is("Shelly.CheckForUpdate")) {
    // empty object: no update available
  } else if (is("Shelly.Reboot") || is("Shelly.Update") || is("Shelly.SetAuth") || is("Shelly.DetectLocation")) {
    result.clear();  // null result
  } else if (is("RPC.Ping")) {
    // empty object
  }
  // ---- Sys / Wifi / Eth / BLE / Cloud / Mqtt / WS ----
  else if (is("Sys.GetStatus")) {
    this->build_sys_status_(o);
  } else if (is("Sys.GetConfig")) {
    this->build_sys_config_(o);
  } else if (is("Sys.SetConfig")) {
    JsonVariantConst name = params["config"]["device"]["name"];
    if (name.is<const char *>()) {
      this->device_name_ = name.as<const char *>();
      this->cfg_rev_++;
    }
    ack();
  } else if (is("Wifi.GetStatus")) {
    this->build_wifi_status_(o);
  } else if (is("Wifi.GetConfig")) {
    this->build_wifi_config_(o);
  } else if (is("Wifi.SetConfig")) {
    // Credentials are managed by ESPHome; acknowledge so provisioning flows complete.
    ESP_LOGI(TAG, "Wifi.SetConfig received (ignored, Wi-Fi is managed by ESPHome)");
    ack();
  } else if (is("Wifi.Scan")) {
    this->build_wifi_scan_(o);
  } else if (is("Wifi.ListAPClients")) {
    o["ap_clients"].to<JsonArray>();
  } else if (is("Eth.GetStatus")) {
    if (!this->is_pro_)
      return RpcError::method_not_found();
    this->build_eth_status_(o);
  } else if (is("Eth.GetConfig")) {
    if (!this->is_pro_)
      return RpcError::method_not_found();
    this->build_eth_config_(o);
  } else if (is("BLE.GetStatus")) {
    this->build_ble_status_(o);
  } else if (is("BLE.GetConfig")) {
    this->build_ble_config_(o);
  } else if (is("BLE.SetConfig") || is("Cloud.SetConfig") || is("Mqtt.SetConfig") || is("WS.SetConfig") ||
             is("Eth.SetConfig") || is("Modbus.SetConfig") || is("EM.SetConfig") || is("EMData.SetConfig") ||
             is("Temperature.SetConfig")) {
    ack();
  } else if (is("Cloud.GetStatus")) {
    this->build_cloud_status_(o);
  } else if (is("Cloud.GetConfig")) {
    this->build_cloud_config_(o);
  } else if (is("Mqtt.GetStatus")) {
    this->build_mqtt_status_(o);
  } else if (is("Mqtt.GetConfig")) {
    this->build_mqtt_config_(o);
  } else if (is("WS.GetStatus")) {
    this->build_ws_status_(o);
  } else if (is("WS.GetConfig")) {
    this->build_ws_config_(o);
  }
  // ---- harmless extras some apps call ----
  else if (is("Schedule.List")) {
    o["jobs"].to<JsonArray>();
    o["rev"] = 0;
  } else if (is("Webhook.List")) {
    o["hooks"].to<JsonArray>();
    o["rev"] = 0;
  } else if (is("Script.List")) {
    o["scripts"].to<JsonArray>();
  } else if (is("KVS.GetMany") || is("KVS.List")) {
    o["items"].to<JsonObject>();
    o["offset"] = 0;
    o["total"] = 0;
    o["rev"] = 0;
  }
  // ---- Switch / Input ----
  else if (is("ProOutputAddon.GetPeripherals")) {
    if (!this->has_output_addon())
      return RpcError::method_not_found();
    JsonObject dout = o["digital_out"].to<JsonObject>();
    for (uint8_t i = 0; i < this->switch_count_; i++)
      dout["switch:" + std::to_string(this->switch_component_id(i))].to<JsonObject>();
  } else if (is("ProOutputAddon.AddPeripheral") || is("ProOutputAddon.RemovePeripheral")) {
    if (!this->has_output_addon())
      return RpcError::method_not_found();
    return {-108, "Peripheral configuration is fixed on this device"};
  } else if (starts("Switch.") || starts("Input.")) {
    if (this->is_meter() && (!this->has_output_addon() || starts("Input.")))
      return RpcError::method_not_found();
    const int index = this->switch_index_for_id(param_id_(params));
    if (index < 0)
      return RpcError::invalid_argument("id");
    const auto cid = static_cast<uint8_t>(index);
    if (is("Switch.GetStatus")) {
      this->build_switch_status_(o, cid, snap.sw[cid]);
    } else if (is("Switch.GetConfig")) {
      this->build_switch_config_(o, cid);
    } else if (is("Switch.SetConfig") || is("Input.SetConfig")) {
      ack();
    } else if (is("Switch.ResetCounters")) {
      o["aenergy"]["total"] = r3(snap.sw[cid].aenergy);
    } else if (is("Switch.Set")) {
      JsonVariantConst on = params["on"];
      bool value;
      if (on.is<bool>()) {
        value = on.as<bool>();
      } else if (on.is<int>()) {
        value = on.as<int>() != 0;
      } else {
        return RpcError::invalid_argument("on");
      }
      bool was_on = false;
      this->request_switch(cid, value, was_on);
      o["was_on"] = was_on;
    } else if (is("Switch.Toggle")) {
      bool was_on = snap.sw[cid].output;
      this->request_switch(cid, !was_on, was_on);
      o["was_on"] = was_on;
    } else if (is("Input.GetStatus")) {
      if (strcmp(this->app_, "PlusPlugS") == 0)
        return RpcError::invalid_argument("id");
      this->build_input_status_(o, cid, snap.sw[cid]);
    } else if (is("Input.GetConfig")) {
      if (strcmp(this->app_, "PlusPlugS") == 0)
        return RpcError::invalid_argument("id");
      this->build_input_config_(o, cid);
    } else {
      return RpcError::method_not_found();
    }
  }
  // ---- EM / EMData / Temperature / Modbus (Pro 3EM) ----
  else if (starts("EM.") || starts("EMData.") || starts("Temperature.") || starts("Modbus.")) {
    if (!this->is_meter())
      return RpcError::method_not_found();
    const int id = param_id_(params);
    if (id > 0)
      return RpcError::invalid_argument("id");
    if (is("EM.GetStatus")) {
      this->build_em_status_(o, snap.em);
    } else if (is("EM.GetConfig")) {
      this->build_em_config_(o);
    } else if (is("EM.GetCTTypes")) {
      JsonArray t = o["supported"].to<JsonArray>();
      t.add("120A");
      t.add("400A");
    } else if (is("EMData.GetStatus")) {
      this->build_emdata_status_(o, snap.em);
    } else if (is("EMData.GetConfig")) {
      // empty object
    } else if (is("Temperature.GetStatus")) {
      this->build_temperature_status_(o, snap.em);
    } else if (is("Temperature.GetConfig")) {
      this->build_temperature_config_(o);
    } else if (is("Modbus.GetStatus")) {
      // empty object
    } else if (is("Modbus.GetConfig")) {
      o["enable"] = true;
    } else {
      return RpcError::method_not_found();
    }
  } else {
    ESP_LOGW(TAG, "Unknown RPC method: %s", method);
    return RpcError::method_not_found();
  }
  return {};
}

std::string ShellyEmu::rpc_envelope(JsonVariantConst request, bool via_ble) {
  JsonDocument env;
  JsonDocument result;
  const char *method = request["method"].as<const char *>();
  RpcError err;
  if (method == nullptr || method[0] == '\0') {
    err = {-32600, "Invalid Request"};
  } else {
    err = this->rpc_call(method, request["params"], result, via_ble);
  }
  env["id"] = request["id"];
  env["src"] = this->device_id_;
  const char *dst = request["src"].as<const char *>();
  env["dst"] = dst != nullptr ? dst : "user_1";
  if (err.ok()) {
    env["result"] = result.as<JsonVariantConst>();
  } else {
    JsonObject e = env["error"].to<JsonObject>();
    e["code"] = err.code;
    e["message"] = err.message;
  }
  std::string out;
  serializeJson(env, out);
  return out;
}

}  // namespace esphome::shelly_emu

#endif  // USE_ESP32
