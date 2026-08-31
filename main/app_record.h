// main/app_record.h —— 录音页:麦克风 → IMA ADPCM → BLE 实时上传,手机保存。
#pragma once

#include "bsp_button.h"

#ifdef __cplusplus
extern "C" {
#endif

void app_record_enter(void);
void app_record_exit(void);
void app_record_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void app_record_refresh(void);

#ifdef __cplusplus
}
#endif