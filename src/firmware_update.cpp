#include "firmware_update.hpp"
#include <esp_ota_ops.h>

// Arduino's weak C hook otherwise accepts pending images during initArduino().
extern "C" bool verifyRollbackLater(){return true;}

namespace gateway {
void checkFirmwareProbation(uint64_t now_ms,bool services_ready,bool ethernet_link,bool has_ip){
  const auto* running=esp_ota_get_running_partition();esp_ota_img_states_t state;
  if(!running||esp_ota_get_state_partition(running,&state)!=ESP_OK||state!=ESP_OTA_IMG_PENDING_VERIFY)return;
  if(now_ms>=kFirmwareProbationMs){esp_ota_mark_app_invalid_rollback_and_reboot();return;}
  if(services_ready&&ethernet_link&&has_ip)esp_ota_mark_app_valid_cancel_rollback();
}
}
