#include "shelly_modbus.h"
#if defined(USE_ESP32) && defined(USE_SHELLY_EMU_MODBUS)

#include <cerrno>
#include <cstring>
#include <ctime>
#include "esphome/core/hal.h"
#include "esphome/core/log.h"

namespace esphome::shelly_emu {

static const char *const TAG = "shelly_emu.modbus";

static constexpr size_t MAX_CLIENTS = 4;
static constexpr uint32_t CLIENT_TIMEOUT_MS = 120000;
static constexpr uint8_t FC_READ_COILS = 0x01;
static constexpr uint8_t FC_READ_HOLDING = 0x03;
static constexpr uint8_t FC_READ_INPUT = 0x04;
static constexpr uint8_t FC_WRITE_COIL = 0x05;
static constexpr uint8_t FC_WRITE_COILS = 0x0F;
// Pro Output Add-on relay (switch:100): coil 100, plus the generic Switch input-register block.
static constexpr uint16_t ADDON_COIL = 100;
static constexpr uint16_t ADDON_SWITCH_REG = 3000;  // docs 33000
static constexpr uint8_t EX_ILLEGAL_FUNCTION = 0x01;
static constexpr uint8_t EX_ILLEGAL_ADDRESS = 0x02;

// Shelly serves 32-bit values word-swapped (CDAB): low word first, then high word.
static void put_float(std::map<uint16_t, uint16_t> &regs, uint16_t addr, float value) {
  uint32_t bits;
  memcpy(&bits, &value, sizeof(bits));
  regs[addr] = static_cast<uint16_t>(bits & 0xFFFF);
  regs[addr + 1] = static_cast<uint16_t>(bits >> 16);
}

static void put_u32(std::map<uint16_t, uint16_t> &regs, uint16_t addr, uint32_t value) {
  regs[addr] = static_cast<uint16_t>(value & 0xFFFF);
  regs[addr + 1] = static_cast<uint16_t>(value >> 16);
}

static void put_ascii(std::map<uint16_t, uint16_t> &regs, uint16_t addr, const char *text, size_t n_regs) {
  const size_t len = strlen(text);
  for (size_t i = 0; i < n_regs; i++) {
    const uint8_t lo = 2 * i < len ? text[2 * i] : 0;
    const uint8_t hi = 2 * i + 1 < len ? text[2 * i + 1] : 0;
    regs[addr + i] = static_cast<uint16_t>((hi << 8) | lo);
  }
}

void ShellyModbusServer::build_register_map_(std::map<uint16_t, uint16_t> &regs) {
  const Snapshot snap = this->emu_->get_snapshot();
  const EmState &em = snap.em;

  // Identity block at 0: MAC (6 regs) + model (8 regs), as captured from a real Pro 3EM.
  put_ascii(regs, 0, this->emu_->get_mac().c_str(), 6);
  put_ascii(regs, 6, this->emu_->get_model_code(), 8);

  const time_t now = ::time(nullptr);
  const auto ts = static_cast<uint32_t>(now > 1600000000 ? now : 0);

  float total_current = 0, total_act = 0, total_aprt = 0;
  for (const auto &p : em.phase) {
    total_current += p.current;
    total_act += p.act_power;
    total_aprt += p.aprt_power;
  }

  // EM (docs: 31000+, on the wire 1000+)
  put_u32(regs, 1000, ts);
  for (uint16_t a = 1002; a <= 1006; a++)
    regs[a] = 0;
  put_float(regs, 1007, std::isnan(em.n_current) ? 0.0f : em.n_current);
  regs[1009] = 0;
  regs[1010] = 0;
  put_float(regs, 1011, total_current);
  put_float(regs, 1013, total_act);
  put_float(regs, 1015, total_aprt);
  for (uint8_t i = 0; i < EM_PHASES; i++) {
    const uint16_t base = 1020 + 20 * i;
    const auto &p = em.phase[i];
    put_float(regs, base + 0, p.voltage);
    put_float(regs, base + 2, p.current);
    put_float(regs, base + 4, p.act_power);
    put_float(regs, base + 6, p.aprt_power);
    put_float(regs, base + 8, p.pf);
    regs[base + 10] = 0;
    regs[base + 11] = 0;
    regs[base + 12] = 0;
    put_float(regs, base + 13, p.freq);
  }

  if (this->emu_->has_output_addon()) {
    regs[ADDON_SWITCH_REG] = snap.sw[0].output ? 1 : 0;
  }

  // EMData (docs: 31160+, on the wire 1160+)
  double total_e = 0, total_ret = 0;
  for (const auto &p : em.phase) {
    total_e += p.energy;
    total_ret += p.ret_energy;
  }
  put_u32(regs, 1160, ts);
  put_float(regs, 1162, static_cast<float>(total_e));
  put_float(regs, 1164, static_cast<float>(total_ret));
  for (uint8_t i = 0; i < EM_PHASES; i++) {
    const uint16_t base = 1170 + 20 * i;
    const auto &p = em.phase[i];
    put_float(regs, base + 0, static_cast<float>(p.energy));
    put_float(regs, base + 2, static_cast<float>(p.energy));
    put_float(regs, base + 4, static_cast<float>(p.ret_energy));
    put_float(regs, base + 6, static_cast<float>(p.ret_energy));
    put_float(regs, base + 8, 0.0f);
    put_float(regs, base + 10, 0.0f);
    put_float(regs, base + 12, static_cast<float>(p.energy));
    put_float(regs, base + 14, static_cast<float>(p.ret_energy));
  }
}

void ShellyModbusServer::setup() {
  this->listen_ = socket::socket_ip_loop_monitored(SOCK_STREAM, 0);
  if (this->listen_ == nullptr) {
    ESP_LOGE(TAG, "Could not create listening socket");
    return;
  }
  int enable = 1;
  this->listen_->setsockopt(SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(int));
  this->listen_->setblocking(false);
  struct sockaddr_storage server{};
  socklen_t sl = socket::set_sockaddr_any(reinterpret_cast<struct sockaddr *>(&server), sizeof(server), this->port_);
  if (sl == 0 || this->listen_->bind(reinterpret_cast<struct sockaddr *>(&server), sl) != 0) {
    ESP_LOGE(TAG, "bind(%u) failed: errno %d", this->port_, errno);
    this->listen_ = nullptr;
    return;
  }
  if (this->listen_->listen(4) != 0) {
    ESP_LOGE(TAG, "listen() failed: errno %d", errno);
    this->listen_ = nullptr;
    return;
  }
  ESP_LOGI(TAG, "Modbus TCP listening on port %u", this->port_);
}

void ShellyModbusServer::accept_() {
  struct sockaddr_storage source{};
  socklen_t len = sizeof(source);
  auto sock = this->listen_->accept_loop_monitored(reinterpret_cast<struct sockaddr *>(&source), &len);
  if (sock == nullptr)
    return;
  sock->setblocking(false);
  if (this->clients_.size() >= MAX_CLIENTS) {
    ESP_LOGW(TAG, "Too many clients, dropping oldest");
    this->clients_.erase(this->clients_.begin());
  }
  char addr[socket::SOCKADDR_STR_LEN];
  sock->getpeername_to(addr);
  ESP_LOGD(TAG, "Client connected: %s", addr);
  Client client;
  client.sock = std::move(sock);
  client.last_activity = millis();
  this->clients_.push_back(std::move(client));
}

void ShellyModbusServer::loop() {
  if (this->listen_ == nullptr)
    return;
  this->accept_();
  const uint32_t now = millis();
  for (auto it = this->clients_.begin(); it != this->clients_.end();) {
    bool keep = this->service_client_(*it);
    if (keep && now - it->last_activity > CLIENT_TIMEOUT_MS) {
      ESP_LOGD(TAG, "Client idle, closing");
      keep = false;
    }
    if (keep) {
      ++it;
    } else {
      it = this->clients_.erase(it);
    }
  }
}

bool ShellyModbusServer::service_client_(Client &client) {
  uint8_t buf[260];
  while (true) {
    const ssize_t n = client.sock->read(buf, sizeof(buf));
    if (n == 0)
      return false;  // closed by peer
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK)
        break;
      return false;
    }
    client.last_activity = millis();
    client.buf.insert(client.buf.end(), buf, buf + n);
    if (client.buf.size() > 1024) {
      ESP_LOGW(TAG, "Garbage from client, closing");
      return false;
    }
    // MBAP header: transaction id (2), protocol id (2), length (2), unit id (1)
    while (client.buf.size() >= 7) {
      const size_t length = (static_cast<size_t>(client.buf[4]) << 8) | client.buf[5];
      if (length < 2 || length > 260)
        return false;
      if (client.buf.size() < 6 + length)
        break;
      this->handle_frame_(client, client.buf.data(), 6 + length);
      client.buf.erase(client.buf.begin(), client.buf.begin() + 6 + length);
    }
  }
  return true;
}

void ShellyModbusServer::handle_frame_(Client &client, const uint8_t *frame, size_t len) {
  const uint8_t unit = frame[6];
  const uint8_t fc = frame[7];
  std::vector<uint8_t> pdu;
  const bool addon = this->emu_->has_output_addon();

  auto exception = [&pdu, fc](uint8_t code) { pdu = {static_cast<uint8_t>(fc | 0x80), code}; };

  if (len < 12) {
    exception(EX_ILLEGAL_FUNCTION);
  } else if (fc == FC_READ_INPUT || fc == FC_READ_HOLDING) {
    const uint16_t address = (static_cast<uint16_t>(frame[8]) << 8) | frame[9];
    const uint16_t count = (static_cast<uint16_t>(frame[10]) << 8) | frame[11];
    if (count < 1 || count > 125 || fc == FC_READ_HOLDING) {
      // A real Pro 3EM answers holding-register reads with "illegal data address".
      exception(EX_ILLEGAL_ADDRESS);
    } else {
      std::map<uint16_t, uint16_t> regs;
      this->build_register_map_(regs);
      pdu.reserve(2 + 2 * count);
      pdu.push_back(fc);
      pdu.push_back(static_cast<uint8_t>(count * 2));
      for (uint16_t i = 0; i < count; i++) {
        auto it = regs.find(address + i);
        const uint16_t v = it != regs.end() ? it->second : 0;
        pdu.push_back(static_cast<uint8_t>(v >> 8));
        pdu.push_back(static_cast<uint8_t>(v & 0xFF));
      }
      ESP_LOGV(TAG, "unit %u FC%02X addr %u count %u", unit, fc, address, count);
    }
  } else if (fc == FC_READ_COILS && addon) {
    const uint16_t address = (static_cast<uint16_t>(frame[8]) << 8) | frame[9];
    const uint16_t count = (static_cast<uint16_t>(frame[10]) << 8) | frame[11];
    if (count < 1 || count > 2000 || address > ADDON_COIL || address + count <= ADDON_COIL) {
      exception(EX_ILLEGAL_ADDRESS);
    } else {
      const Snapshot snap = this->emu_->get_snapshot();
      const uint8_t n_bytes = (count + 7) / 8;
      pdu.assign(2 + n_bytes, 0);
      pdu[0] = fc;
      pdu[1] = n_bytes;
      if (snap.sw[0].output) {
        const uint16_t bit = ADDON_COIL - address;
        pdu[2 + bit / 8] |= static_cast<uint8_t>(1 << (bit % 8));
      }
      ESP_LOGV(TAG, "unit %u FC01 addr %u count %u -> %s", unit, address, count, ONOFF(snap.sw[0].output));
    }
  } else if (fc == FC_WRITE_COIL && addon) {
    const uint16_t address = (static_cast<uint16_t>(frame[8]) << 8) | frame[9];
    const uint16_t value = (static_cast<uint16_t>(frame[10]) << 8) | frame[11];
    if (address != ADDON_COIL) {
      exception(EX_ILLEGAL_ADDRESS);
    } else if (value != 0xFF00 && value != 0x0000) {
      exception(0x03);  // illegal data value
    } else {
      bool was_on = false;
      this->emu_->request_switch(0, value == 0xFF00, was_on);
      ESP_LOGI(TAG, "Modbus coil %u <- %s", address, ONOFF(value == 0xFF00));
      pdu.assign(frame + 7, frame + 12);  // echo request
    }
  } else if (fc == FC_WRITE_COILS && addon) {
    const uint16_t address = (static_cast<uint16_t>(frame[8]) << 8) | frame[9];
    const uint16_t count = (static_cast<uint16_t>(frame[10]) << 8) | frame[11];
    if (address != ADDON_COIL || count != 1 || len < 14) {
      exception(EX_ILLEGAL_ADDRESS);
    } else {
      bool was_on = false;
      const bool on = (frame[13] & 0x01) != 0;
      this->emu_->request_switch(0, on, was_on);
      ESP_LOGI(TAG, "Modbus coils %u <- %s", address, ONOFF(on));
      pdu.assign(frame + 7, frame + 12);
    }
  } else {
    exception(EX_ILLEGAL_FUNCTION);
  }

  std::vector<uint8_t> resp;
  resp.reserve(7 + pdu.size());
  resp.push_back(frame[0]);
  resp.push_back(frame[1]);
  resp.push_back(0);
  resp.push_back(0);
  const uint16_t rlen = pdu.size() + 1;
  resp.push_back(static_cast<uint8_t>(rlen >> 8));
  resp.push_back(static_cast<uint8_t>(rlen & 0xFF));
  resp.push_back(unit);
  resp.insert(resp.end(), pdu.begin(), pdu.end());

  size_t sent = 0;
  while (sent < resp.size()) {
    const ssize_t n = client.sock->write(resp.data() + sent, resp.size() - sent);
    if (n < 0) {
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        delay(1);
        continue;
      }
      return;
    }
    sent += n;
  }
}

}  // namespace esphome::shelly_emu

#endif
