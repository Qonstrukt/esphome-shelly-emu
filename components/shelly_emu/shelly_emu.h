#pragma once

#include "esphome/core/defines.h"
#ifdef USE_ESP32

#include <array>
#include <cmath>
#include <string>
#include <vector>

#include "esphome/components/binary_sensor/binary_sensor.h"
#include "esphome/components/json/json_util.h"
#include "esphome/components/sensor/sensor.h"
#include "esphome/components/switch/switch.h"
#include "esphome/components/web_server_base/web_server_base.h"
#ifdef USE_TEXT_SENSOR
#include "esphome/components/text_sensor/text_sensor.h"
#endif
#include "esphome/core/component.h"
#include "esphome/core/helpers.h"

#ifdef USE_SHELLY_EMU_BLE
#include <esp_gap_ble_api.h>
#include <esp_gatts_api.h>
#include "esphome/components/esp32_ble_server/ble_server.h"
#endif

namespace esphome::shelly_emu {

static constexpr uint8_t MAX_SWITCHES = 4;
static constexpr uint8_t EM_PHASES = 3;

enum class DeviceKind : uint8_t { SWITCHES = 0, ENERGY_METER = 1 };

/// Result of an RPC call. code 0 means success.
struct RpcError {
  int code{0};
  std::string message;
  bool ok() const { return this->code == 0; }
  static RpcError method_not_found() { return {404, "Method not found"}; }
  static RpcError invalid_argument(const char *what) {
    return {-105, std::string("Invalid argument '") + what + "'"};
  }
};

struct SwitchState {
  bool output{false};
  bool input{false};
  float apower{0};
  float voltage{NAN};
  float current{0};
  float freq{NAN};
  float pf{0};
  float temp{NAN};
  double aenergy{0};      // Wh
  double ret_aenergy{0};  // Wh
};

struct EmPhaseState {
  float voltage{0};
  float current{0};
  float act_power{0};
  float aprt_power{0};
  float pf{1};
  float freq{50};
  double energy{0};      // Wh
  double ret_energy{0};  // Wh
};

struct EmState {
  std::array<EmPhaseState, EM_PHASES> phase{};
  float n_current{NAN};
  float temp{NAN};
};

/// Copy of all live values, safe to read from any task.
struct Snapshot {
  std::array<SwitchState, MAX_SWITCHES> sw{};
  EmState em{};
};

struct SwitchChannel {
  switch_::Switch *sw{nullptr};
  const char *name{""};
  sensor::Sensor *power{nullptr};
  sensor::Sensor *voltage{nullptr};
  sensor::Sensor *current{nullptr};
  sensor::Sensor *energy{nullptr};
  sensor::Sensor *energy_returned{nullptr};
  sensor::Sensor *frequency{nullptr};
  sensor::Sensor *power_factor{nullptr};
  sensor::Sensor *temperature{nullptr};
  binary_sensor::BinarySensor *input{nullptr};
  float energy_scale{1000.0f};
  double integrated_wh{0};
  bool optimistic{false};
  bool optimistic_state{false};
  uint32_t optimistic_until{0};
};

struct EmPhaseSensors {
  sensor::Sensor *voltage{nullptr};
  sensor::Sensor *current{nullptr};
  sensor::Sensor *power{nullptr};
  sensor::Sensor *apparent_power{nullptr};
  sensor::Sensor *power_factor{nullptr};
  sensor::Sensor *frequency{nullptr};
  sensor::Sensor *energy{nullptr};
  sensor::Sensor *energy_returned{nullptr};
  double integrated_wh{0};
  double integrated_ret_wh{0};
};

class ShellyHttpHandler;
class ShellyBleRpc;
class ShellyModbusServer;

class ShellyEmu : public Component {
 public:
  // ---- configuration (called from generated code) ----
  void set_web_server_base(web_server_base::WebServerBase *base) { this->base_ = base; }
  void set_model(const char *code, const char *app, const char *host_prefix, DeviceKind kind, bool is_pro) {
    this->model_code_ = code;
    this->app_ = app;
    this->host_prefix_ = host_prefix;
    this->kind_ = kind;
    this->is_pro_ = is_pro;
  }
  void set_firmware(const char *version, const char *fw_id) {
    this->fw_version_ = version;
    this->fw_id_ = fw_id;
  }
  void set_timezone(const char *tz) { this->tz_ = tz; }
  void set_device_name(const char *name) { this->device_name_ = name; }
  void set_mac_override(const char *mac) { this->mac_override_ = mac; }

  void add_switch_channel(switch_::Switch *sw, const char *name);
  /// Pro 3EM with the Shelly Pro Output Add-on: channel 0 is exposed as switch:100.
  void set_output_addon(bool enabled) { this->output_addon_ = enabled; }
  void set_switch_power(uint8_t ch, sensor::Sensor *s) { this->channels_[ch].power = s; }
  void set_switch_voltage(uint8_t ch, sensor::Sensor *s) { this->channels_[ch].voltage = s; }
  void set_switch_current(uint8_t ch, sensor::Sensor *s) { this->channels_[ch].current = s; }
  void set_switch_energy(uint8_t ch, sensor::Sensor *s) { this->channels_[ch].energy = s; }
  void set_switch_energy_returned(uint8_t ch, sensor::Sensor *s) { this->channels_[ch].energy_returned = s; }
  void set_switch_frequency(uint8_t ch, sensor::Sensor *s) { this->channels_[ch].frequency = s; }
  void set_switch_power_factor(uint8_t ch, sensor::Sensor *s) { this->channels_[ch].power_factor = s; }
  void set_switch_temperature(uint8_t ch, sensor::Sensor *s) { this->channels_[ch].temperature = s; }
  void set_switch_input(uint8_t ch, binary_sensor::BinarySensor *b) { this->channels_[ch].input = b; }
  void set_switch_energy_scale(uint8_t ch, float scale) { this->channels_[ch].energy_scale = scale; }

  void set_em_voltage(uint8_t p, sensor::Sensor *s) { this->em_phases_[p].voltage = s; }
  void set_em_current(uint8_t p, sensor::Sensor *s) { this->em_phases_[p].current = s; }
  void set_em_power(uint8_t p, sensor::Sensor *s) { this->em_phases_[p].power = s; }
  void set_em_apparent_power(uint8_t p, sensor::Sensor *s) { this->em_phases_[p].apparent_power = s; }
  void set_em_power_factor(uint8_t p, sensor::Sensor *s) { this->em_phases_[p].power_factor = s; }
  void set_em_frequency(uint8_t p, sensor::Sensor *s) { this->em_phases_[p].frequency = s; }
  void set_em_energy(uint8_t p, sensor::Sensor *s) { this->em_phases_[p].energy = s; }
  void set_em_energy_returned(uint8_t p, sensor::Sensor *s) { this->em_phases_[p].energy_returned = s; }
  void set_em_neutral_current(sensor::Sensor *s) { this->em_neutral_current_ = s; }
  void set_em_temperature(sensor::Sensor *s) { this->em_temperature_ = s; }
  void set_em_energy_scale(float scale) { this->em_energy_scale_ = scale; }

#ifdef USE_TEXT_SENSOR
  void set_version_text_sensor(text_sensor::TextSensor *s) { this->version_text_sensor_ = s; }
  void set_device_id_text_sensor(text_sensor::TextSensor *s) { this->device_id_text_sensor_ = s; }
#endif
#ifdef USE_SHELLY_EMU_BLE
  void set_ble_server(esp32_ble_server::BLEServer *server) { this->ble_server_ = server; }
  void set_ble_provision_state(const char *state) { this->ble_provision_state_ = state; }
  void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);
#endif
#ifdef USE_SHELLY_EMU_MODBUS
  void set_modbus_port(uint16_t port) { this->modbus_port_ = port; }
#endif

  // ---- Component ----
  void setup() override;
  void loop() override;
  void dump_config() override;
  float get_setup_priority() const override { return setup_priority::LATE; }

  // ---- identity ----
  const std::string &get_device_id() const { return this->device_id_; }
  const std::string &get_hostname() const { return this->hostname_; }
  const std::string &get_mac() const { return this->mac_; }
  const char *get_app() const { return this->app_; }
  const char *get_model_code() const { return this->model_code_; }
  const char *get_fw_version() const { return this->fw_version_; }
  const char *get_fw_id() const { return this->fw_id_; }
  DeviceKind get_kind() const { return this->kind_; }
  bool is_meter() const { return this->kind_ == DeviceKind::ENERGY_METER; }
  uint8_t get_switch_count() const { return this->switch_count_; }
  bool has_output_addon() const { return this->output_addon_ && this->switch_count_ > 0; }
  /// Shelly component id of channel `index` (0..n for switch devices, 100 for the add-on relay).
  int switch_component_id(uint8_t index) const { return this->output_addon_ ? 100 + index : index; }
  /// Channel index for a Shelly switch id, or -1 if the id does not exist.
  int switch_index_for_id(int id) const;
  const char *get_ble_provision_state() const {
#ifdef USE_SHELLY_EMU_BLE
    return this->ble_provision_state_;
#else
    return "complete";
#endif
  }

  // ---- RPC (safe to call from any task) ----
  /// Dispatch one method. `result` receives the result value (object, or null for void methods).
  RpcError rpc_call(const char *method, JsonVariantConst params, JsonDocument &result, bool via_ble = false);
  /// Handle a framed request {"id","src","method","params"} and return the serialized response envelope.
  std::string rpc_envelope(JsonVariantConst request, bool via_ble = false);
  void build_device_info(JsonObject o, bool via_ble);
  Snapshot get_snapshot();
  /// Queue an output change for the main loop. Returns false for an unknown channel id.
  bool request_switch(uint8_t id, bool on, bool &was_on);

 protected:
  friend class ShellyModbusServer;

  // builders (shelly_rpc.cpp)
  void build_switch_status_(JsonObject o, uint8_t index, const SwitchState &s);
  void build_switch_config_(JsonObject o, uint8_t index);
  void build_input_status_(JsonObject o, uint8_t id, const SwitchState &s);
  void build_input_config_(JsonObject o, uint8_t id);
  void build_em_status_(JsonObject o, const EmState &em);
  void build_em_config_(JsonObject o);
  void build_emdata_status_(JsonObject o, const EmState &em);
  void build_temperature_status_(JsonObject o, const EmState &em);
  void build_temperature_config_(JsonObject o);
  void build_sys_status_(JsonObject o);
  void build_sys_config_(JsonObject o);
  void build_wifi_status_(JsonObject o);
  void build_wifi_config_(JsonObject o);
  void build_wifi_scan_(JsonObject o);
  void build_eth_status_(JsonObject o);
  void build_eth_config_(JsonObject o);
  void build_ble_status_(JsonObject o);
  void build_ble_config_(JsonObject o);
  void build_cloud_status_(JsonObject o);
  void build_cloud_config_(JsonObject o);
  void build_mqtt_status_(JsonObject o);
  void build_mqtt_config_(JsonObject o);
  void build_ws_status_(JsonObject o);
  void build_ws_config_(JsonObject o);
  void build_knx_config_(JsonObject o);
  void build_full_status_(JsonObject o, const Snapshot &snap);
  void build_full_config_(JsonObject o);
  void build_components_(JsonObject o, JsonVariantConst params, const Snapshot &snap);
  void build_list_methods_(JsonObject o);
  void build_list_profiles_(JsonObject o);
  void component_keys_(std::vector<std::string> &keys);
  bool component_status_(const std::string &key, JsonObject o, const Snapshot &snap);
  bool component_config_(const std::string &key, JsonObject o);
  static int param_id_(JsonVariantConst params);

  // core (shelly_emu.cpp)
  void build_identity_();
  void update_snapshot_();
  void integrate_energy_();
  void process_commands_();
  void mdns_refresh_();
  void mdns_remove_();

  web_server_base::WebServerBase *base_{nullptr};
  ShellyHttpHandler *http_{nullptr};

  const char *model_code_{""};
  const char *app_{""};
  const char *host_prefix_{"Shelly"};
  DeviceKind kind_{DeviceKind::SWITCHES};
  bool is_pro_{true};
  const char *fw_version_{"2.0.0"};
  const char *fw_id_{"20260710-101218/2.0.0-g87fbfa4"};
  const char *tz_{"UTC"};
  const char *mac_override_{nullptr};
  std::string device_name_;
  std::string mac_;        // AABBCCDDEEFF
  std::string mac_lower_;  // aabbccddeeff
  std::string device_id_;  // shellypro2pm-aabbccddeeff
  std::string hostname_;   // ShellyPro2PM-AABBCCDDEEFF

  std::array<SwitchChannel, MAX_SWITCHES> channels_{};
  uint8_t switch_count_{0};
  bool output_addon_{false};
  std::array<EmPhaseSensors, EM_PHASES> em_phases_{};
  sensor::Sensor *em_neutral_current_{nullptr};
  sensor::Sensor *em_temperature_{nullptr};
  float em_energy_scale_{1000.0f};

  Snapshot snapshot_{};
  Mutex snapshot_mutex_;
  struct Command {
    uint8_t id;
    bool on;
  };
  std::vector<Command> commands_;
  Mutex command_mutex_;

  uint32_t last_integration_ms_{0};
  uint32_t last_snapshot_ms_{0};
  uint32_t last_mdns_ms_{0};
  uint32_t mdns_signature_{0};
  bool mdns_services_added_{false};
  uint32_t cfg_rev_{1};

#ifdef USE_TEXT_SENSOR
  text_sensor::TextSensor *version_text_sensor_{nullptr};
  text_sensor::TextSensor *device_id_text_sensor_{nullptr};
#endif
#ifdef USE_SHELLY_EMU_BLE
  esp32_ble_server::BLEServer *ble_server_{nullptr};
  ShellyBleRpc *ble_{nullptr};
  const char *ble_provision_state_{"pending"};
#endif
#ifdef USE_SHELLY_EMU_MODBUS
  ShellyModbusServer *modbus_{nullptr};
  uint16_t modbus_port_{502};
#endif
};

}  // namespace esphome::shelly_emu

#endif  // USE_ESP32
