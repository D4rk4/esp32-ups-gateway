#include <Arduino.h>
#include <ETH.h>
#include <WiFi.h>
#include <esp_system.h>
#include <esp_ota_ops.h>
#include <memory>
#include "config_store.hpp"
#include "ups_service.hpp"
#include "snmp_service.hpp"
#include "web_service.hpp"
#include "event_bus.hpp"
#include "management_service.hpp"
#include "firmware_update.hpp"

namespace {
gateway::ConfigStore configStore;
gateway::Config activeConfig;
gateway::EventBus events;
std::unique_ptr<gateway::UpsService> upsService;
std::unique_ptr<gateway::SnmpService> snmpService;
std::unique_ptr<gateway::ManagementService> managementService;
std::unique_ptr<gateway::WebService> webService;
bool running=false,configReady=false;
String consoleLine;
bool consoleOverflow=false;

IPAddress ipv4(const std::string& text){IPAddress address;address.fromString(text.c_str());return address;}
void fatal(const char* message){Serial.printf("FATAL: %s\n",message);}
void consoleCommand(const String& command){
  if(command=="help"){
    Serial.println("Commands: status | reset-password | reboot | help");
    Serial.println("reset-password preserves all other settings; the new password applies after reboot.");
  }else if(command=="status"){
    const auto state=upsService->snapshot();
    Serial.printf("Firmware %s; driver %s; UPS %s; successful polls %lu; errors %lu; heap %u\n",FIRMWARE_VERSION,activeConfig.driver.c_str(),state.connected?"connected":"disconnected",static_cast<unsigned long>(state.successful_polls),static_cast<unsigned long>(state.errors),ESP.getFreeHeap());
    Serial.print("Ethernet IP: ");Serial.println(ETH.localIP());
  }else if(command=="reset-password"){
    std::string password,error;
    if(webService->resetPasswordLocal(password,error)){
      Serial.print("NEW WEB PASSWORD (applies after reboot): ");Serial.println(password.c_str());
      Serial.println("Type reboot when ready. Other configuration is preserved.");
    }else{Serial.print("Password reset failed: ");Serial.println(error.c_str());}
  }else if(command=="reboot"){
    if(upsService->controlStatus().last_state=="queued"){Serial.println("Command still queued; wait before reboot.");return;}
    std::string error;if(!events.flushPending(error))Serial.printf("Warning: persistent log flush failed: %s\n",error.c_str());
    Serial.println("Restarting gateway...");Serial.flush();ESP.restart();
  }else if(command.length())Serial.println("Unknown command; type help.");
}
}

void setup(){
  Serial.begin(115200);delay(200);
  Serial.printf("\nESP32 UPS Gateway %s / ESP32-POE WROOM 16MB + MOD-RS232\n",FIRMWARE_VERSION);
  const auto* runningPartition=esp_ota_get_running_partition();esp_ota_img_states_t imageState;
  if(runningPartition&&esp_ota_get_state_partition(runningPartition,&imageState)==ESP_OK)
    Serial.printf("Running partition: %s; OTA image state: %lu\n",runningPartition->label,static_cast<unsigned long>(imageState));
  else Serial.printf("Running partition: %s; OTA image state: unavailable\n",runningPartition?runningPartition->label:"unknown");
  if(ESP.getFlashChipSize()<16u*1024u*1024u){fatal("This image needs physical 16MB flash; check the board before writing appcfg.");return;}
  const bool valid=configStore.begin();configReady=valid;activeConfig=configStore.saved();
  if(!valid){Serial.print("Configuration is read-only: ");Serial.println(configStore.error().c_str());}
  if(configStore.generatedCredentials()){
    Serial.println(valid?"FIRST BOOT CREDENTIALS — keep privately":"TEMPORARY RAM CREDENTIALS — existing config was NOT erased");
    Serial.print("Web user: ");Serial.println(activeConfig.web_user.c_str());
    Serial.print("Web password: ");Serial.println(activeConfig.web_password.c_str());
    Serial.print("SNMP read community: ");Serial.println(activeConfig.snmp_community.c_str());
  }
  if(!events.begin()){fatal("Cannot allocate event queues");return;}
  WiFi.onEvent([](WiFiEvent_t event){if(event==ARDUINO_EVENT_ETH_START)ETH.setHostname(activeConfig.hostname.c_str());});
  // Official Olimex ESP32-POE WROOM pinout; WROVER is a different target.
  if(!ETH.begin(0,12,23,18,ETH_PHY_LAN8720,ETH_CLOCK_GPIO17_OUT)){fatal("Ethernet initialization failed");return;}
  if(!activeConfig.dhcp&&!ETH.config(ipv4(activeConfig.ip),ipv4(activeConfig.gateway),ipv4(activeConfig.netmask),ipv4(activeConfig.dns))){fatal("Static Ethernet configuration failed");return;}
  Serial.print("Ethernet MAC: ");Serial.println(ETH.macAddress());
  const uint32_t bootId=esp_random();
  upsService.reset(new gateway::UpsService(activeConfig,events));
  snmpService.reset(new gateway::SnmpService(activeConfig,*upsService,events,bootId));
  managementService.reset(new gateway::ManagementService(activeConfig,*upsService,events));
  webService.reset(new gateway::WebService(activeConfig,configStore,*upsService,*snmpService,events,*managementService,bootId));
  if(!upsService->begin()){fatal("UPS service initialization failed");return;}
  if(!snmpService->begin()){fatal("SNMP service initialization failed");return;}
  if(!managementService->begin()){fatal("Management service initialization failed");return;}
  if(!webService->begin()){fatal("HTTP service initialization failed");return;}
  events.publish(gateway::EventType::Startup);
  Serial.println("Ethernet: waiting for link/address. UART2 TX4 RX36. Type help for local recovery commands.");
  running=true;
}

void loop(){
  gateway::checkFirmwareProbation(gateway::uptimeMs(),running&&configReady,running&&ETH.linkUp(),running&&static_cast<uint32_t>(ETH.localIP())!=0);
  events.tick();
  if(!running){delay(100);return;}
  webService->tick();
  managementService->tick();
  static IPAddress previous;
  const auto current=ETH.localIP();
  if(current!=previous){previous=current;Serial.print("Web: http://");Serial.println(current);}
  for(unsigned count=0;Serial.available()&&count<64;++count){
    const char ch=char(Serial.read());
    if(ch=='\r'||ch=='\n'){
      if(consoleOverflow)Serial.println("Console line too long");else{consoleLine.trim();consoleCommand(consoleLine);}
      consoleLine="";consoleOverflow=false;
    }else if(consoleLine.length()<96&&!consoleOverflow)consoleLine+=ch;
    else consoleOverflow=true;
  }
  delay(10);
}
