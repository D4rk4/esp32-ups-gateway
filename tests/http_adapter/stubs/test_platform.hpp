#pragma once
// Host-only platform boundary. Production HTTP and auth/config code stay real.
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>
using esp_err_t=int;
constexpr esp_err_t ESP_OK=0,ESP_FAIL=-1;
using SemaphoreHandle_t=void*;using QueueHandle_t=void*;using TaskHandle_t=void*;
constexpr unsigned portMAX_DELAY=~0u;
inline SemaphoreHandle_t xSemaphoreCreateMutex(){return reinterpret_cast<void*>(1);}
inline int xSemaphoreTake(SemaphoreHandle_t,unsigned){return 1;}
inline int xSemaphoreGive(SemaphoreHandle_t){return 1;}
using httpd_handle_t=void*;
enum httpd_method_t {HTTP_GET,HTTP_POST};
struct httpd_req_t {
  const char* uri{nullptr};httpd_method_t method{HTTP_GET};size_t content_len{0};void* user_ctx{nullptr};
  std::map<std::string,std::string> headers,response_headers;
  std::vector<std::pair<const char*,const char*>> pending_headers;
  std::string body,response,status{"200 OK"},type;
  size_t offset{0};bool sent_headers{false};
};
using HttpHandler=esp_err_t(*)(httpd_req_t*);
struct httpd_uri_t {const char* uri;httpd_method_t method;HttpHandler handler;void* user_ctx;};
struct httpd_config_t {
  size_t stack_size{0},max_uri_handlers{8},max_open_sockets{0};
  unsigned recv_wait_timeout{0},send_wait_timeout{0};bool lru_purge_enable{false};
  bool (*uri_match_fn)(const char*,const char*,size_t){nullptr};
};
#define HTTPD_DEFAULT_CONFIG() httpd_config_t{}
esp_err_t httpd_start(httpd_handle_t*,const httpd_config_t*);
esp_err_t httpd_stop(httpd_handle_t);
esp_err_t httpd_register_uri_handler(httpd_handle_t,const httpd_uri_t*);
size_t httpd_req_get_hdr_value_len(httpd_req_t*,const char*);
esp_err_t httpd_req_get_hdr_value_str(httpd_req_t*,const char*,char*,size_t);
int httpd_req_to_sockfd(httpd_req_t*);
int httpd_req_recv(httpd_req_t*,char*,size_t);
esp_err_t httpd_resp_set_status(httpd_req_t*,const char*);
esp_err_t httpd_resp_set_type(httpd_req_t*,const char*);
esp_err_t httpd_resp_set_hdr(httpd_req_t*,const char*,const char*);
esp_err_t httpd_resp_send(httpd_req_t*,const char*,size_t);
esp_err_t httpd_resp_send_chunk(httpd_req_t*,const char*,size_t);
void esp_fill_random(void*,size_t);
uint32_t millis();
struct FakeESP {
  void restart()const;
  uint32_t getFreeHeap()const{return 160000;}
  uint32_t getMinFreeHeap()const{return 100000;}
  uint32_t getFlashChipSize()const{return 16*1024*1024;}
  const char* getChipModel()const{return "ESP32";}
  uint32_t getChipRevision()const{return 3;}
  uint32_t getCpuFreqMHz()const{return 240;}
};
inline FakeESP ESP;
struct FakeSerial {template<typename... Args> void printf(const char*,Args...)const{}};
inline FakeSerial Serial;
struct FakeIPAddress {std::string toString()const{return "192.0.2.10";}};
struct FakeETH {
  FakeIPAddress localIP()const{return {};}
  std::string macAddress()const{return "02:00:00:00:00:01";}
  bool linkUp()const{return true;}
};
inline FakeETH ETH;
