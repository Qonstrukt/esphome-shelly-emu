#include "shelly_emu.h"
#ifdef USE_ESP32

#include <cctype>
#include <cstring>

#include <mdns.h>
#include "lwip/udp.h"

#include "esphome/components/network/util.h"
#include "esphome/core/application.h"
#include "esphome/core/log.h"

#include "shelly_http.h"
#ifdef USE_SHELLY_EMU_BLE
#include "shelly_ble.h"
#endif
#ifdef USE_SHELLY_EMU_MODBUS
#include "shelly_modbus.h"
#endif

namespace esphome::shelly_emu {

static const char *const TAG = "shelly_emu";

static float sensor_value(sensor::Sensor *s, float fallback) {
  if (s == nullptr || !s->has_state() || std::isnan(s->state))
    return fallback;
  return s->state;
}

void ShellyEmu::add_switch_channel(switch_::Switch *sw, const char *name) {
  if (this->switch_count_ >= MAX_SWITCHES)
    return;
  auto &ch = this->channels_[this->switch_count_++];
  ch.sw = sw;
  ch.name = name != nullptr ? name : "";
}

void ShellyEmu::build_identity_() {
  uint8_t mac[6] = {0};
  bool have_mac = false;
  if (this->mac_override_ != nullptr && strlen(this->mac_override_) == 12) {
    have_mac = true;
    for (int i = 0; i < 6; i++) {
      char byte[3] = {this->mac_override_[2 * i], this->mac_override_[2 * i + 1], 0};
      mac[i] = static_cast<uint8_t>(strtoul(byte, nullptr, 16));
    }
  }
  if (!have_mac)
    get_mac_address_raw(mac);
  char buf[13];
  snprintf(buf, sizeof(buf), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  this->mac_ = buf;
  this->mac_lower_ = this->mac_;
  for (auto &c : this->mac_lower_)
    c = static_cast<char>(tolower(c));
  std::string prefix_lower = this->host_prefix_;
  for (auto &c : prefix_lower)
    c = static_cast<char>(tolower(c));
  this->device_id_ = prefix_lower + "-" + this->mac_lower_;
  this->hostname_ = std::string(this->host_prefix_) + "-" + this->mac_;
}

void ShellyEmu::setup() {
  this->build_identity_();

  this->http_ = new ShellyHttpHandler(this);  // NOLINT(cppcoreguidelines-owning-memory)
  this->base_->init();
  this->base_->add_handler_without_auth(this->http_);

#ifdef USE_SHELLY_EMU_BLE
  this->ble_ = new ShellyBleRpc(this, this->ble_server_);  // NOLINT(cppcoreguidelines-owning-memory)
#endif
#ifdef USE_SHELLY_EMU_MODBUS
  this->modbus_ = new ShellyModbusServer(this, this->modbus_port_);  // NOLINT(cppcoreguidelines-owning-memory)
  this->modbus_->setup();
#endif

  this->last_integration_ms_ = millis();
  this->update_snapshot_();
}

void ShellyEmu::loop() {
  const uint32_t now = millis();
  this->process_commands_();
  if (now - this->last_integration_ms_ >= 1000)
    this->integrate_energy_();
  if (now - this->last_snapshot_ms_ >= 200) {
    this->last_snapshot_ms_ = now;
    this->update_snapshot_();
  }
  if (now - this->last_mdns_ms_ >= 5000) {
    this->last_mdns_ms_ = now;
    this->mdns_refresh_();
  }
#ifdef USE_SHELLY_EMU_BLE
  if (this->ble_ != nullptr)
    this->ble_->loop();
#endif
#ifdef USE_SHELLY_EMU_MODBUS
  if (this->modbus_ != nullptr)
    this->modbus_->loop();
#endif
}

void ShellyEmu::dump_config() {
  ESP_LOGCONFIG(TAG,
                "Shelly emulator:\n"
                "  Model: %s (app %s, fw %s)\n"
                "  Device id: %s\n"
                "  Hostname: %s.local\n"
                "  HTTP port: %u",
                this->model_code_, this->app_, this->fw_version_, this->device_id_.c_str(), this->hostname_.c_str(),
                this->base_->get_port());
  for (uint8_t i = 0; i < this->switch_count_; i++) {
    const auto &ch = this->channels_[i];
    ESP_LOGCONFIG(TAG, "  switch:%u '%s' power=%s energy=%s", i, ch.name, YESNO(ch.power != nullptr),
                  ch.energy != nullptr ? "sensor" : "integrated");
  }
  if (this->is_meter()) {
    for (uint8_t p = 0; p < EM_PHASES; p++) {
      const auto &ph = this->em_phases_[p];
      ESP_LOGCONFIG(TAG, "  em phase %c power=%s voltage=%s energy=%s", 'a' + p, YESNO(ph.power != nullptr),
                    YESNO(ph.voltage != nullptr), ph.energy != nullptr ? "sensor" : "integrated");
    }
  }
#ifdef USE_SHELLY_EMU_BLE
  ESP_LOGCONFIG(TAG, "  BLE RPC: enabled (provision state over BLE: %s)", this->ble_provision_state_);
#endif
#ifdef USE_SHELLY_EMU_MODBUS
  ESP_LOGCONFIG(TAG, "  Modbus TCP: port %u", this->modbus_port_);
#endif
}

void ShellyEmu::integrate_energy_() {
  const uint32_t now = millis();
  const double dt_h = (now - this->last_integration_ms_) / 3600000.0;
  this->last_integration_ms_ = now;
  for (uint8_t i = 0; i < this->switch_count_; i++) {
    auto &ch = this->channels_[i];
    if (ch.energy != nullptr)
      continue;
    const float p = sensor_value(ch.power, 0.0f);
    if (p > 0)
      ch.integrated_wh += p * dt_h;
  }
  if (!this->is_meter())
    return;
  for (auto &ph : this->em_phases_) {
    const float p = sensor_value(ph.power, 0.0f);
    if (ph.energy == nullptr && p > 0)
      ph.integrated_wh += p * dt_h;
    if (ph.energy_returned == nullptr && p < 0)
      ph.integrated_ret_wh += -p * dt_h;
  }
}

void ShellyEmu::update_snapshot_() {
  Snapshot snap;
  const uint32_t now = millis();

  for (uint8_t i = 0; i < this->switch_count_; i++) {
    auto &ch = this->channels_[i];
    auto &s = snap.sw[i];
    bool state = ch.sw != nullptr && ch.sw->state;
    if (ch.optimistic) {
      if (static_cast<int32_t>(now - ch.optimistic_until) >= 0 || state == ch.optimistic_state) {
        ch.optimistic = false;
      } else {
        state = ch.optimistic_state;
      }
    }
    s.output = state;
    s.input = ch.input != nullptr && ch.input->state;
    s.apower = sensor_value(ch.power, 0.0f);
    s.voltage = sensor_value(ch.voltage, NAN);
    s.freq = sensor_value(ch.frequency, NAN);
    s.temp = sensor_value(ch.temperature, NAN);
    s.current = sensor_value(ch.current, NAN);
    if (std::isnan(s.current))
      s.current = (!std::isnan(s.voltage) && s.voltage > 0) ? std::fabs(s.apower) / s.voltage : 0.0f;
    s.pf = sensor_value(ch.power_factor, NAN);
    if (std::isnan(s.pf))
      s.pf = s.apower != 0 ? 1.0f : 0.0f;
    s.aenergy = ch.energy != nullptr ? sensor_value(ch.energy, 0.0f) * ch.energy_scale : ch.integrated_wh;
    s.ret_aenergy = ch.energy_returned != nullptr ? sensor_value(ch.energy_returned, 0.0f) * ch.energy_scale : 0.0;
  }

  if (this->is_meter()) {
    for (uint8_t p = 0; p < EM_PHASES; p++) {
      auto &ph = this->em_phases_[p];
      auto &e = snap.em.phase[p];
      e.act_power = sensor_value(ph.power, 0.0f);
      e.voltage = sensor_value(ph.voltage, 230.0f);
      e.freq = sensor_value(ph.frequency, 50.0f);
      e.pf = sensor_value(ph.power_factor, NAN);
      if (std::isnan(e.pf))
        e.pf = 1.0f;
      e.aprt_power = sensor_value(ph.apparent_power, NAN);
      if (std::isnan(e.aprt_power))
        e.aprt_power = std::fabs(e.pf) > 0.01f ? std::fabs(e.act_power) / std::fabs(e.pf) : std::fabs(e.act_power);
      e.current = sensor_value(ph.current, NAN);
      if (std::isnan(e.current))
        e.current = e.voltage > 0 ? e.aprt_power / e.voltage : 0.0f;
      e.energy = ph.energy != nullptr ? sensor_value(ph.energy, 0.0f) * this->em_energy_scale_ : ph.integrated_wh;
      e.ret_energy = ph.energy_returned != nullptr ? sensor_value(ph.energy_returned, 0.0f) * this->em_energy_scale_
                                                   : ph.integrated_ret_wh;
    }
    snap.em.n_current = sensor_value(this->em_neutral_current_, NAN);
    snap.em.temp = sensor_value(this->em_temperature_, NAN);
  }

  {
    // Commands that are queued but not yet applied still count as the new output state.
    LockGuard lock(this->command_mutex_);
    for (const auto &cmd : this->commands_) {
      if (cmd.id < MAX_SWITCHES)
        snap.sw[cmd.id].output = cmd.on;
    }
  }
  {
    LockGuard lock(this->snapshot_mutex_);
    this->snapshot_ = snap;
  }
}

void ShellyEmu::process_commands_() {
  std::vector<Command> cmds;
  {
    LockGuard lock(this->command_mutex_);
    if (this->commands_.empty())
      return;
    cmds.swap(this->commands_);
  }
  for (const auto &cmd : cmds) {
    if (cmd.id >= this->switch_count_)
      continue;
    auto &ch = this->channels_[cmd.id];
    ch.optimistic = true;
    ch.optimistic_state = cmd.on;
    ch.optimistic_until = millis() + 5000;
    ESP_LOGI(TAG, "switch:%u -> %s", cmd.id, ONOFF(cmd.on));
    if (ch.sw != nullptr) {
      if (cmd.on) {
        ch.sw->turn_on();
      } else {
        ch.sw->turn_off();
      }
    }
  }
}

Snapshot ShellyEmu::get_snapshot() {
  LockGuard lock(this->snapshot_mutex_);
  return this->snapshot_;
}

bool ShellyEmu::request_switch(uint8_t id, bool on, bool &was_on) {
  if (id >= this->switch_count_)
    return false;
  {
    LockGuard lock(this->snapshot_mutex_);
    was_on = this->snapshot_.sw[id].output;
    this->snapshot_.sw[id].output = on;
  }
  {
    LockGuard lock(this->command_mutex_);
    this->commands_.push_back({id, on});
  }
  return true;
}

// ---------------------------------------------------------------------------
// mDNS: a Shelly is found by its _shelly._tcp service, advertised under its own
// hostname (ShellyPro2PM-AABBCCDDEEFF.local) and instance name (the device id).
// ESPHome's mdns component owns the responder; we add a delegated host to it.
// ---------------------------------------------------------------------------

static constexpr size_t MDNS_MAX_ADDRESSES = 6;
static mdns_ip_addr_t s_mdns_addresses[MDNS_MAX_ADDRESSES];  // NOLINT

void ShellyEmu::mdns_refresh_() {
  if (!network::is_connected()) {
    this->mdns_remove_();
    return;
  }
  auto ips = network::get_ip_addresses();
  size_t count = 0;
  uint32_t signature = 2166136261u;  // FNV-1a over the address set
  for (const auto &ip : ips) {
    if (!ip.is_set() || count >= MDNS_MAX_ADDRESSES)
      continue;
    ip_addr_t addr = ip;
    auto &node = s_mdns_addresses[count];
    memset(&node, 0, sizeof(node));
#if LWIP_IPV6
    if (IP_IS_V6(&addr)) {
      node.addr.type = ESP_IPADDR_TYPE_V6;
      memcpy(node.addr.u_addr.ip6.addr, ip_2_ip6(&addr)->addr, sizeof(node.addr.u_addr.ip6.addr));
    } else
#endif
    {
      node.addr.type = ESP_IPADDR_TYPE_V4;
      node.addr.u_addr.ip4.addr = ip_2_ip4(&addr)->addr;
    }
    const auto *bytes = reinterpret_cast<const uint8_t *>(&node.addr);
    for (size_t i = 0; i < sizeof(node.addr); i++)
      signature = (signature ^ bytes[i]) * 16777619u;
    count++;
  }
  if (count == 0)
    return;
  for (size_t i = 0; i < count; i++)
    s_mdns_addresses[i].next = (i + 1 < count) ? &s_mdns_addresses[i + 1] : nullptr;
  if (this->mdns_services_added_ && this->mdns_signature_ == signature)
    return;

  esp_err_t err = mdns_hostname_exists(this->hostname_.c_str())
                      ? mdns_delegate_hostname_set_address(this->hostname_.c_str(), s_mdns_addresses)
                      : mdns_delegate_hostname_add(this->hostname_.c_str(), s_mdns_addresses);
  if (err != ESP_OK) {
    ESP_LOGW(TAG, "mDNS hostname registration failed: %s", esp_err_to_name(err));
    return;
  }
  if (!this->mdns_services_added_) {
    mdns_txt_item_t shelly_txt[] = {{"gen", "2"}, {"app", this->app_}, {"ver", this->fw_version_}};
    mdns_txt_item_t http_txt[] = {{"gen", "2"}};
    const uint16_t port = this->base_->get_port();
    esp_err_t e1 = mdns_service_add_for_host(this->device_id_.c_str(), "_shelly", "_tcp", this->hostname_.c_str(),
                                             port, shelly_txt, 3);
    esp_err_t e2 = mdns_service_add_for_host(this->device_id_.c_str(), "_http", "_tcp", this->hostname_.c_str(), port,
                                             http_txt, 1);
    if (e1 != ESP_OK || e2 != ESP_OK) {
      ESP_LOGW(TAG, "mDNS service registration failed: %s / %s", esp_err_to_name(e1), esp_err_to_name(e2));
      return;
    }
    this->mdns_services_added_ = true;
    ESP_LOGI(TAG, "mDNS: %s.local advertising _shelly._tcp as %s", this->hostname_.c_str(),
             this->device_id_.c_str());
  }
  // RFC 6762 requires IP TTL 255 on every mDNS packet. The responder only sets it for multicast;
  // unicast replies to QU questions would carry the default TTL and Shelly controllers drop those.
  for (struct udp_pcb *pcb = udp_pcbs; pcb != nullptr; pcb = pcb->next) {
    if (pcb->local_port == 5353)
      pcb->ttl = 255;
  }
  this->mdns_signature_ = signature;
}

void ShellyEmu::mdns_remove_() {
  if (!this->mdns_services_added_)
    return;
  mdns_service_remove_for_host(this->device_id_.c_str(), "_shelly", "_tcp", this->hostname_.c_str());
  mdns_service_remove_for_host(this->device_id_.c_str(), "_http", "_tcp", this->hostname_.c_str());
  mdns_delegate_hostname_remove(this->hostname_.c_str());
  this->mdns_services_added_ = false;
  this->mdns_signature_ = 0;
}

#ifdef USE_SHELLY_EMU_BLE
void ShellyEmu::gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                    esp_ble_gatts_cb_param_t *param) {
  if (this->ble_ != nullptr)
    this->ble_->gatts_event_handler(event, gatts_if, param);
}
void ShellyEmu::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  if (this->ble_ != nullptr)
    this->ble_->gap_event_handler(event, param);
}
#endif

}  // namespace esphome::shelly_emu

#endif  // USE_ESP32
