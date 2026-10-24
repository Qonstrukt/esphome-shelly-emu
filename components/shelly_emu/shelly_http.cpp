#include "shelly_http.h"
#ifdef USE_ESP32

#include <cstring>
#include <utility>
#include <vector>

#include "esphome/core/log.h"

namespace esphome::shelly_emu {

static const char *const TAG = "shelly_emu.http";
static constexpr size_t MAX_BODY = 8192;

static std::string url_decode(const std::string &in) {
  std::string out;
  out.reserve(in.size());
  for (size_t i = 0; i < in.size(); i++) {
    const char c = in[i];
    if (c == '+') {
      out.push_back(' ');
    } else if (c == '%' && i + 2 < in.size() && isxdigit(static_cast<unsigned char>(in[i + 1])) &&
               isxdigit(static_cast<unsigned char>(in[i + 2]))) {
      char hex[3] = {in[i + 1], in[i + 2], 0};
      out.push_back(static_cast<char>(strtoul(hex, nullptr, 16)));
      i += 2;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

using QueryList = std::vector<std::pair<std::string, std::string>>;

static QueryList parse_query(const char *uri) {
  QueryList out;
  const char *q = strchr(uri, '?');
  if (q == nullptr)
    return out;
  std::string query(q + 1);
  size_t start = 0;
  while (start <= query.size()) {
    size_t end = query.find('&', start);
    if (end == std::string::npos)
      end = query.size();
    std::string pair = query.substr(start, end - start);
    if (!pair.empty()) {
      size_t eq = pair.find('=');
      if (eq == std::string::npos) {
        out.emplace_back(url_decode(pair), "");
      } else {
        out.emplace_back(url_decode(pair.substr(0, eq)), url_decode(pair.substr(eq + 1)));
      }
    }
    start = end + 1;
  }
  return out;
}

static const std::string *query_get(const QueryList &q, const char *key) {
  for (const auto &kv : q) {
    if (kv.first == key)
      return &kv.second;
  }
  return nullptr;
}

/// Shelly HTTP GET semantics: every query parameter is an RPC parameter, values are JSON
/// when they parse as JSON (numbers, booleans, objects) and strings otherwise.
static void coerce_params(const QueryList &q, JsonDocument &params, const char *skip_key) {
  JsonObject o = params.to<JsonObject>();
  for (const auto &kv : q) {
    if (skip_key != nullptr && kv.first == skip_key)
      continue;
    JsonDocument tmp;
    if (deserializeJson(tmp, kv.second) == DeserializationError::Ok && !tmp.is<const char *>()) {
      o[kv.first] = tmp.as<JsonVariantConst>();
    } else {
      o[kv.first] = kv.second;
    }
  }
}

static void send_json(AsyncWebServerRequest *request, int code, const std::string &body) {
  auto *response = request->beginResponse(code, "application/json", body);
  response->addHeader("Server", "ShellyHTTP/1.0.0");
  response->addHeader("Connection", "close");
  request->send(response);
  // A real Shelly closes the connection after every response. Do the same so idle controller
  // connections cannot pile up in the ESP-IDF HTTP server's small socket pool.
  httpd_req_t *raw = *request;
  httpd_sess_trigger_close(raw->handle, httpd_req_to_sockfd(raw));
}

static std::string error_json(const RpcError &err) {
  JsonDocument doc;
  doc["code"] = err.code;
  doc["message"] = err.message;
  std::string out;
  serializeJson(doc, out);
  return out;
}

static std::string request_url(AsyncWebServerRequest *request) {
  char buf[AsyncWebServerRequest::URL_BUF_SIZE];
  StringRef url = request->url_to(buf);
  return std::string(url.c_str(), url.size());
}

bool ShellyHttpHandler::canHandle(AsyncWebServerRequest *request) const {
  const std::string url = request_url(request);
  const bool ours = url == "/shelly" || url == "/rpc" || url.rfind("/rpc/", 0) == 0;
  const bool method_ok = request->method() == HTTP_GET || request->method() == HTTP_POST;
  if (!ours || !method_ok) {
    // Not a Shelly endpoint we serve: log it so unexpected controller requests become visible.
    const httpd_req_t *raw = *request;
    ESP_LOGD(TAG, "Unhandled HTTP request: method %d %s", static_cast<int>(request->method()), raw->uri);
  }
  return ours && method_ok;
}

void ShellyHttpHandler::handleBody(AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index,
                                   size_t total) {
  if (index == 0)
    this->body_.clear();
  if (total > MAX_BODY)
    return;
  this->body_.append(reinterpret_cast<const char *>(data), len);
}

void ShellyHttpHandler::handle_bare_(AsyncWebServerRequest *request, const char *method, JsonVariantConst params) {
  JsonDocument result;
  RpcError err = this->emu_->rpc_call(method, params, result, false);
  if (!err.ok()) {
    send_json(request, err.code == 404 ? 404 : 500, error_json(err));
    return;
  }
  std::string out;
  serializeJson(result, out);
  send_json(request, 200, out);
}

void ShellyHttpHandler::handleRequest(AsyncWebServerRequest *request) {
  const std::string url = request_url(request);
  const httpd_req_t *raw = *request;
  const QueryList query = parse_query(raw->uri);
  std::string body = std::move(this->body_);
  this->body_.clear();
  const bool is_post = request->method() == HTTP_POST;

  if (url == "/shelly") {
    JsonDocument doc;
    this->emu_->build_device_info(doc.to<JsonObject>(), false);
    std::string out;
    serializeJson(doc, out);
    send_json(request, 200, out);
    return;
  }

  if (url == "/rpc") {
    if (is_post && !body.empty()) {
      JsonDocument req;
      if (deserializeJson(req, body) != DeserializationError::Ok) {
        send_json(request, 400, error_json({-32700, "Invalid JSON"}));
        return;
      }
      if (req.is<JsonArrayConst>()) {
        std::string out = "[";
        bool first = true;
        for (JsonVariantConst item : req.as<JsonArrayConst>()) {
          if (!first)
            out += ",";
          first = false;
          out += this->emu_->rpc_envelope(item, false);
        }
        out += "]";
        send_json(request, 200, out);
        return;
      }
      send_json(request, 200, this->emu_->rpc_envelope(req.as<JsonVariantConst>(), false));
      return;
    }
    // GET /rpc?method=Switch.Set&id=0&on=true  (bare result)
    const std::string *method = query_get(query, "method");
    if (method == nullptr || method->empty()) {
      send_json(request, 400, error_json({-32600, "Missing 'method'"}));
      return;
    }
    JsonDocument params;
    const std::string *params_json = query_get(query, "params");
    if (params_json != nullptr) {
      deserializeJson(params, *params_json);
    } else {
      coerce_params(query, params, "method");
    }
    this->handle_bare_(request, method->c_str(), params.as<JsonVariantConst>());
    return;
  }

  // /rpc/<Method>: params from the JSON body (POST) or from the query string (GET)
  const std::string method = url.substr(5);
  if (method.empty()) {
    send_json(request, 404, error_json(RpcError::method_not_found()));
    return;
  }
  JsonDocument params;
  if (is_post && !body.empty()) {
    if (deserializeJson(params, body) != DeserializationError::Ok) {
      send_json(request, 400, error_json({-32700, "Invalid JSON"}));
      return;
    }
  } else {
    coerce_params(query, params, nullptr);
  }
  this->handle_bare_(request, method.c_str(), params.as<JsonVariantConst>());
}

}  // namespace esphome::shelly_emu

#endif  // USE_ESP32
