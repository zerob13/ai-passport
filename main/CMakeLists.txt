idf_component_register(
    SRCS "main.c"
         "ui_pixel.c"
         "ui_pixel_math.c"
         "pager_core.c"
         "app_chime.c"
         "adpcm_ima.c"
         "sync_proto.c"
         "sync_ble.c"
         "app_pager.c"
         "app_record.c"
         "app_schedule.c"
         "app_todo.c"
         "app_music.c"
         "ui_font_cjk_16.c"
    INCLUDE_DIRS "."
    REQUIRES bsp bt nvs_flash esp_timer
)
