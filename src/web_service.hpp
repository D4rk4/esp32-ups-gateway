#pragma once
#include "config_store.hpp"
#include "ups_service.hpp"
#include "snmp_service.hpp"
#include "event_bus.hpp"
#include "management_service.hpp"
#include "auth_core.hpp"
#include <esp_http_server.h>
#include <ArduinoJson.h>
#include <atomic>
namespace gateway {
class WebService {
 public:
  WebService(const Config&,ConfigStore&,UpsService&,SnmpService&,EventBus&,ManagementService&,uint32_t boot_id);
  bool begin();
  void tick();
  bool resetPasswordLocal(std::string& password,std::string& error);
 private:
  static esp_err_t dispatch(httpd_req_t* request);
  static bool matchUri(const char* registered,const char* uri,size_t length);
  esp_err_t handle(httpd_req_t* request);
  auth::Access authorizeRequest(httpd_req_t* request,const auth::Route& route,auth::Session& session);
  bool readBody(httpd_req_t* request,size_t maximum,std::string& body);
  esp_err_t json(httpd_req_t* request,const std::string& body,const char* status="200 OK");
  esp_err_t json(httpd_req_t* request,const JsonDocument& doc,const char* status="200 OK");
  esp_err_t error(httpd_req_t* request,const char* status,const std::string& message);
  esp_err_t status(httpd_req_t* request);
  esp_err_t sessionResponse(httpd_req_t* request,const auth::Session* session);
  esp_err_t eventsResponse(httpd_req_t* request,const auth::RequestTarget& target,bool csv);
  esp_err_t capabilities(httpd_req_t* request);
  esp_err_t about(httpd_req_t* request);
  const Config& active_;ConfigStore& store_;UpsService& ups_;SnmpService& snmp_;EventBus& events_;ManagementService& management_;
  uint32_t boot_id_;auth::SessionStore sessions_;
  httpd_handle_t server_{nullptr};SemaphoreHandle_t config_mutex_{nullptr},auth_mutex_{nullptr};
  std::atomic<bool> needs_reboot_{false};
  std::atomic<uint64_t> reboot_at_{0};
};
}
