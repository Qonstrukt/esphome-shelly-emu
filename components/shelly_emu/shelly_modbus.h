#pragma once

#include "esphome/core/defines.h"
#if defined(USE_ESP32) && defined(USE_SHELLY_EMU_MODBUS)

#include <map>
#include <memory>
#include <vector>
#include "esphome/components/socket/socket.h"
#include "shelly_emu.h"

namespace esphome::shelly_emu {

/// Minimal Modbus TCP server exposing the Shelly Pro 3EM EM/EMData input registers.
/// Sigenergy polls the meter this way after enrollment (FC04, any unit id).
class ShellyModbusServer {
 public:
  ShellyModbusServer(ShellyEmu *emu, uint16_t port) : emu_(emu), port_(port) {}
  void setup();
  void loop();

 protected:
  struct Client {
    std::unique_ptr<socket::Socket> sock;
    std::vector<uint8_t> buf;
    uint32_t last_activity{0};
  };

  void accept_();
  bool service_client_(Client &client);
  void handle_frame_(Client &client, const uint8_t *frame, size_t len);
  void build_register_map_(std::map<uint16_t, uint16_t> &regs);

  ShellyEmu *emu_;
  uint16_t port_;
  std::unique_ptr<socket::ListenSocket> listen_;
  std::vector<Client> clients_;
};

}  // namespace esphome::shelly_emu

#endif
