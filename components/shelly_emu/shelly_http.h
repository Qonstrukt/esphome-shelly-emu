#pragma once

#include "esphome/core/defines.h"
#ifdef USE_ESP32

#include <string>
#include "esphome/components/web_server_base/web_server_base.h"
#include "shelly_emu.h"

namespace esphome::shelly_emu {

/// Serves the Shelly Gen2 HTTP API: GET /shelly, GET|POST /rpc, GET|POST /rpc/<Method>.
/// Runs on the HTTP server task; only touches ShellyEmu through its thread-safe RPC entry points.
class ShellyHttpHandler : public AsyncWebHandler {
 public:
  explicit ShellyHttpHandler(ShellyEmu *emu) : emu_(emu) {}

  bool canHandle(AsyncWebServerRequest *request) const override;
  void handleRequest(AsyncWebServerRequest *request) override;
  void handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) override;
  bool isRequestHandlerTrivial() const override { return false; }

 protected:
  void handle_bare_(AsyncWebServerRequest *request, const char *method, JsonVariantConst params);
  ShellyEmu *emu_;
  std::string body_;
};

}  // namespace esphome::shelly_emu

#endif  // USE_ESP32
