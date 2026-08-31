// main/app_todo.h —— 任务 Todo 页:上下选择,OK 勾选完成,回传手机。
#pragma once

#include "bsp_button.h"

#ifdef __cplusplus
extern "C" {
#endif

void app_todo_enter(void);
void app_todo_exit(void);
void app_todo_key(bsp_btn_t btn, bsp_btn_ev_t ev);
void app_todo_refresh(void);

#ifdef __cplusplus
}
#endif