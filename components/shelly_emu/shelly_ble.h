#pragma once

#include "esphome/core/defines.h"
#if defined(USE_ESP32) && defined(USE_SHELLY_EMU_BLE)

#include <span>
#include <string>
#include "esphome/components/esp32_ble_server/ble_server.h"
#include "shelly_emu.h"

namespace esphome::shelly_emu {

/// Shelly Gen2 "RPC over GATT" transport (Mongoose OS BLE RPC service).
///
/// Client -> device: 4-byte big-endian frame length written to TX control, then the JSON
/// frame written to the data characteristic in MTU-sized chunks.
/// Device -> client: 4-byte big-endian response length in RX control (read / notify), then the
/// client reads the data characteristic repeatedly, each read returning the next chunk.
class ShellyBleRpc {
 public:
  ShellyBleRpc(ShellyEmu *emu, esp32_ble_server::BLEServer *server) : emu_(emu), server_(server) {}

  void loop();
  void gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if, esp_ble_gatts_cb_param_t *param);
  void gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param);

 protected:
  void create_service_();
  void on_tx_ctl_write_(std::span<const uint8_t> data);
  void on_data_write_(std::span<const uint8_t> data);
  void on_data_read_();
  void finish_request_();
  void set_rx_len_(uint32_t len);
  void apply_advertising_();
  void reset_();

  ShellyEmu *emu_;
  esp32_ble_server::BLEServer *server_;
  esp32_ble_server::BLEService *service_{nullptr};
  esp32_ble_server::BLECharacteristic *data_{nullptr};
  esp32_ble_server::BLECharacteristic *tx_ctl_{nullptr};
  esp32_ble_server::BLECharacteristic *rx_ctl_{nullptr};

  bool started_{false};
  bool advertising_requested_{false};
  bool name_set_{false};
  uint16_t mtu_{23};

  uint32_t expected_len_{0};
  std::string rx_buf_;
  std::string tx_buf_;
  size_t tx_cursor_{0};
};

}  // namespace esphome::shelly_emu

#endif
