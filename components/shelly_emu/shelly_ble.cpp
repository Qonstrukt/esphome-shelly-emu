#include "shelly_ble.h"
#if defined(USE_ESP32) && defined(USE_SHELLY_EMU_BLE)

#include <algorithm>
#include <cstring>
#include <esp_gap_ble_api.h>
#include "esphome/components/esp32_ble/ble.h"
#include "esphome/core/log.h"

namespace esphome::shelly_emu {

static const char *const TAG = "shelly_emu.ble";

// Mongoose OS RPC-over-GATT UUIDs, as used by every Shelly Gen2+ device.
static const char *const SERVICE_UUID = "5f6d4f53-5f52-5043-5f53-56435f49445f";  // _mOS_RPC_SVC_ID_
static const char *const DATA_UUID = "5f6d4f53-5f52-5043-5f64-6174615f5f5f";     // _mOS_RPC_data___
static const char *const TX_CTL_UUID = "5f6d4f53-5f52-5043-5f74-785f63746c5f";   // _mOS_RPC_tx_ctl_
static const char *const RX_CTL_UUID = "5f6d4f53-5f52-5043-5f72-785f63746c5f";   // _mOS_RPC_rx_ctl_

static constexpr size_t MAX_REQUEST = 4096;

using esp32_ble::ESPBTUUID;
using esp32_ble_server::BLECharacteristic;

void ShellyBleRpc::reset_() {
  this->expected_len_ = 0;
  this->rx_buf_.clear();
  this->tx_buf_.clear();
  this->tx_cursor_ = 0;
  this->mtu_ = 23;
}

void ShellyBleRpc::create_service_() {
  this->service_ = this->server_->create_service(ESPBTUUID::from_raw(SERVICE_UUID), true, 12);
  this->data_ = this->service_->create_characteristic(
      ESPBTUUID::from_raw(DATA_UUID),
      BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  this->data_->on_write([this](std::span<const uint8_t> data, uint16_t) { this->on_data_write_(data); });
  this->data_->on_read([this](uint16_t) { this->on_data_read_(); });

  this->tx_ctl_ = this->service_->create_characteristic(
      ESPBTUUID::from_raw(TX_CTL_UUID), BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR);
  this->tx_ctl_->on_write([this](std::span<const uint8_t> data, uint16_t) { this->on_tx_ctl_write_(data); });

  this->rx_ctl_ = this->service_->create_characteristic(
      ESPBTUUID::from_raw(RX_CTL_UUID), BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY);
  this->set_rx_len_(0);
}

void ShellyBleRpc::loop() {
  if (!this->server_->is_running())
    return;
  if (this->service_ == nullptr) {
    this->create_service_();
    return;
  }
  if (!this->started_) {
    if (this->service_->is_created()) {
      this->service_->start();
      this->started_ = true;
    }
    return;
  }
  if (!this->service_->is_running())
    return;
  if (!this->name_set_ && esp32_ble::global_ble->is_active()) {
    // Shelly devices advertise and name themselves after their hostname.
    esp_ble_gap_set_device_name(this->emu_->get_hostname().c_str());
    this->name_set_ = true;
  }
  if (!this->advertising_requested_) {
    this->advertising_requested_ = true;
    esp32_ble::global_ble->advertising_start();
    ESP_LOGI(TAG, "BLE RPC service started, advertising as %s", this->emu_->get_hostname().c_str());
  }
}

void ShellyBleRpc::apply_advertising_() {
  // Advertisement: flags + complete list of 128-bit service UUIDs (21 bytes).
  // Scan response: complete local name (the Shelly hostname, up to 29 chars).
  const esp_bt_uuid_t uuid = ESPBTUUID::from_raw(SERVICE_UUID).get_uuid();
  uint8_t adv[31];
  size_t n = 0;
  adv[n++] = 2;
  adv[n++] = ESP_BLE_AD_TYPE_FLAG;
  adv[n++] = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT;
  adv[n++] = 17;
  adv[n++] = ESP_BLE_AD_TYPE_128SRV_CMPL;
  memcpy(&adv[n], uuid.uuid.uuid128, 16);
  n += 16;

  uint8_t rsp[31];
  size_t m = 0;
  const std::string &name = this->emu_->get_hostname();
  const size_t name_len = std::min<size_t>(name.size(), 29);
  rsp[m++] = static_cast<uint8_t>(name_len + 1);
  rsp[m++] = ESP_BLE_AD_TYPE_NAME_CMPL;
  memcpy(&rsp[m], name.data(), name_len);
  m += name_len;

  esp_err_t e1 = esp_ble_gap_config_adv_data_raw(adv, n);
  esp_err_t e2 = esp_ble_gap_config_scan_rsp_data_raw(rsp, m);
  if (e1 != ESP_OK || e2 != ESP_OK)
    ESP_LOGW(TAG, "Raw advertising setup failed: %s / %s", esp_err_to_name(e1), esp_err_to_name(e2));
}

void ShellyBleRpc::gap_event_handler(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param) {
  if (event == ESP_GAP_BLE_ADV_START_COMPLETE_EVT && this->started_) {
    // ESPHome just (re)started advertising with its own payload; replace it with the Shelly one.
    this->apply_advertising_();
  }
}

void ShellyBleRpc::gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                       esp_ble_gatts_cb_param_t *param) {
  switch (event) {
    case ESP_GATTS_CONNECT_EVT:
      ESP_LOGD(TAG, "Client connected");
      this->reset_();
      break;
    case ESP_GATTS_DISCONNECT_EVT:
      ESP_LOGD(TAG, "Client disconnected");
      this->reset_();
      break;
    case ESP_GATTS_MTU_EVT:
      this->mtu_ = param->mtu.mtu;
      ESP_LOGD(TAG, "MTU %u", this->mtu_);
      break;
    default:
      break;
  }
}

void ShellyBleRpc::set_rx_len_(uint32_t len) {
  this->rx_ctl_->set_value(std::vector<uint8_t>{static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16),
                                                static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)});
}

void ShellyBleRpc::on_tx_ctl_write_(std::span<const uint8_t> data) {
  if (data.size() < 4) {
    ESP_LOGW(TAG, "Short TX control write (%u bytes)", static_cast<unsigned>(data.size()));
    return;
  }
  this->expected_len_ = (static_cast<uint32_t>(data[0]) << 24) | (static_cast<uint32_t>(data[1]) << 16) |
                        (static_cast<uint32_t>(data[2]) << 8) | data[3];
  this->rx_buf_.clear();
  this->tx_buf_.clear();
  this->tx_cursor_ = 0;
  if (this->expected_len_ > MAX_REQUEST) {
    ESP_LOGW(TAG, "Request too large (%u bytes)", static_cast<unsigned>(this->expected_len_));
    this->expected_len_ = 0;
  }
}

void ShellyBleRpc::on_data_write_(std::span<const uint8_t> data) {
  if (this->expected_len_ == 0)
    return;
  this->rx_buf_.append(reinterpret_cast<const char *>(data.data()), data.size());
  if (this->rx_buf_.size() >= this->expected_len_)
    this->finish_request_();
}

void ShellyBleRpc::finish_request_() {
  this->rx_buf_.resize(this->expected_len_);
  ESP_LOGV(TAG, "Request: %s", this->rx_buf_.c_str());
  JsonDocument req;
  if (deserializeJson(req, this->rx_buf_) != DeserializationError::Ok) {
    this->tx_buf_ = "{\"id\":null,\"src\":\"" + this->emu_->get_device_id() +
                    "\",\"error\":{\"code\":-32700,\"message\":\"Invalid JSON\"}}";
  } else {
    this->tx_buf_ = this->emu_->rpc_envelope(req.as<JsonVariantConst>(), true);
  }
  this->rx_buf_.clear();
  this->expected_len_ = 0;
  this->tx_cursor_ = 0;
  ESP_LOGV(TAG, "Response (%u bytes): %s", static_cast<unsigned>(this->tx_buf_.size()), this->tx_buf_.c_str());
  this->set_rx_len_(this->tx_buf_.size());
  this->rx_ctl_->notify();
}

void ShellyBleRpc::on_data_read_() {
  // Each read returns the next chunk. Keep chunks below MTU-1 so the client never starts a
  // long-read (blob) sequence, which would confuse the sequential cursor.
  const size_t remaining = this->tx_buf_.size() - std::min(this->tx_cursor_, this->tx_buf_.size());
  const size_t max_chunk = this->mtu_ > 23 ? static_cast<size_t>(this->mtu_ - 3) : 20;
  const size_t chunk = std::min(remaining, max_chunk);
  std::vector<uint8_t> value(this->tx_buf_.begin() + this->tx_cursor_,
                             this->tx_buf_.begin() + this->tx_cursor_ + chunk);
  this->data_->set_value(std::move(value));
  this->tx_cursor_ += chunk;
  if (this->tx_cursor_ >= this->tx_buf_.size()) {
    this->tx_buf_.clear();
    this->tx_cursor_ = 0;
  }
}

}  // namespace esphome::shelly_emu

#endif
