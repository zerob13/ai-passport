// DimOS Now Playing page. Metadata and cover art come from Android MediaSession.
#include "app_music.h"

#include "app_pager.h"
#include "sync_ble.h"
#include "ui_font_cjk_16.h"
#include "ui_pixel.h"
#include "esp_timer.h"
#include "lvgl.h"

#include <stdint.h>

static lv_obj_t *s_scr;
static lv_obj_t *s_bat;
static lv_obj_t *s_source;
static lv_obj_t *s_title;
static lv_obj_t *s_artist;
static lv_obj_t *s_album;
static lv_obj_t *s_state;
static lv_obj_t *s_art;
static lv_obj_t *s_no_art;
static lv_obj_t *s_progress_fill;
static lv_obj_t *s_elapsed;
static lv_obj_t *s_duration;
static lv_obj_t *s_foot;
static lv_timer_t *s_timer;
static lv_image_dsc_t s_art_dsc;
static bool s_art_visible;
static uint32_t s_position_base;
static uint32_t s_duration_ms;
static int64_t s_position_epoch_us;
static bool s_playing;

static lv_obj_t *box(lv_obj_t *parent, int x, int y, int w, int h,
                     uint32_t color, int border, int radius)
{
    lv_obj_t *obj = lv_obj_create(parent);
    lv_obj_remove_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj, x, y);
    lv_obj_set_size(obj, w, h);
    lv_obj_set_style_bg_color(obj, lv_color_hex(color), 0);
    lv_obj_set_style_border_color(obj, lv_color_hex(UI_INK), 0);
    lv_obj_set_style_border_width(obj, border, 0);
    lv_obj_set_style_radius(obj, radius, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    return obj;
}

static void format_time(char out[10], uint32_t milliseconds)
{
    uint32_t seconds = milliseconds / 1000;
    if (seconds >= 3600) {
        lv_snprintf(out, 10, "%u:%02u:%02u", (unsigned)(seconds / 3600),
                    (unsigned)((seconds / 60) % 60), (unsigned)(seconds % 60));
    } else {
        lv_snprintf(out, 10, "%02u:%02u", (unsigned)(seconds / 60),
                    (unsigned)(seconds % 60));
    }
}

static uint32_t current_position(void)
{
    uint32_t position = s_position_base;
    if (s_playing && s_position_epoch_us > 0) {
        int64_t elapsed = esp_timer_get_time() - s_position_epoch_us;
        if (elapsed > 0) position += (uint32_t)(elapsed / 1000);
    }
    if (s_duration_ms > 0 && position > s_duration_ms) position = s_duration_ms;
    return position;
}

static void progress_render(void)
{
    if (!s_scr) return;
    uint32_t position = current_position();
    int width = s_duration_ms > 0
                    ? (int)(((uint64_t)position * 202u) / s_duration_ms)
                    : 0;
    lv_obj_set_width(s_progress_fill, width);

    char elapsed[10];
    char duration[10];
    format_time(elapsed, position);
    format_time(duration, s_duration_ms);
    lv_label_set_text(s_elapsed, elapsed);
    lv_label_set_text(s_duration, duration);
}

static void timer_tick(lv_timer_t *timer)
{
    (void)timer;
    if (s_playing) progress_render();
}

void app_music_enter(void)
{
    s_scr = ui_pixel_screen_create("MUSIC");
    s_bat = app_battery_create(s_scr);

    s_source = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_SUBTLE);
    lv_obj_set_size(s_source, 112, 20);
    lv_obj_set_pos(s_source, 118, 31);
    lv_obj_set_style_text_align(s_source, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_source, LV_LABEL_LONG_MODE_DOTS);

    lv_obj_t *panel = ui_pixel_panel_create(s_scr, 10, 62, 220, 190, UI_SURFACE);
    lv_obj_set_style_pad_all(panel, 0, 0);

    lv_obj_t *art_frame = box(panel, 8, 8, 100, 100, UI_SURFACE, 1, 4);
    s_no_art = ui_pixel_label(art_frame, "NO\nART", &lv_font_montserrat_14,
                              UI_SUBTLE);
    lv_obj_set_size(s_no_art, 98, 40);
    lv_obj_set_pos(s_no_art, 0, 28);
    lv_obj_set_style_text_align(s_no_art, LV_TEXT_ALIGN_CENTER, 0);

    s_art_dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    s_art_dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    s_art_dsc.header.flags = 0;
    s_art_dsc.header.w = SYNC_MEDIA_ART_W;
    s_art_dsc.header.h = SYNC_MEDIA_ART_H;
    s_art_dsc.header.stride = SYNC_MEDIA_ART_W * 2;
    s_art_dsc.data_size = SYNC_MEDIA_ART_BYTES;
    s_art_dsc.data = (const uint8_t *)app_store()->media.art_rgb565;
    s_art = lv_image_create(art_frame);
    lv_obj_set_pos(s_art, 1, 1);
    lv_obj_add_flag(s_art, LV_OBJ_FLAG_HIDDEN);

    s_title = ui_pixel_label(panel, "", &ui_font_cjk_16, UI_INK);
    lv_obj_set_pos(s_title, 116, 5);
    lv_obj_set_size(s_title, 94, 42);
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_MODE_DOTS);

    s_artist = ui_pixel_label(panel, "", &ui_font_cjk_16, UI_SUBTLE);
    lv_obj_set_pos(s_artist, 116, 50);
    lv_obj_set_size(s_artist, 94, 22);
    lv_label_set_long_mode(s_artist, LV_LABEL_LONG_MODE_DOTS);

    s_album = ui_pixel_label(panel, "", &ui_font_cjk_16, UI_SUBTLE);
    lv_obj_set_pos(s_album, 116, 75);
    lv_obj_set_size(s_album, 94, 22);
    lv_label_set_long_mode(s_album, LV_LABEL_LONG_MODE_DOTS);

    box(panel, 8, 123, 202, 4, UI_LINE, 0, 0);
    s_progress_fill = box(panel, 8, 123, 0, 4, UI_INK, 0, 0);
    s_elapsed = ui_pixel_label(panel, "00:00", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_pos(s_elapsed, 8, 132);
    s_duration = ui_pixel_label(panel, "00:00", &lv_font_montserrat_14, UI_SUBTLE);
    lv_obj_set_size(s_duration, 80, 20);
    lv_obj_set_pos(s_duration, 130, 132);
    lv_obj_set_style_text_align(s_duration, LV_TEXT_ALIGN_RIGHT, 0);

    s_state = ui_pixel_label(panel, "", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_size(s_state, 202, 22);
    lv_obj_set_pos(s_state, 8, 160);
    lv_obj_set_style_text_align(s_state, LV_TEXT_ALIGN_CENTER, 0);

    s_foot = ui_pixel_label(s_scr, "", &lv_font_montserrat_14, UI_INK);
    lv_obj_set_size(s_foot, 220, 20);
    lv_obj_set_pos(s_foot, 10, 263);
    lv_obj_set_style_text_align(s_foot, LV_TEXT_ALIGN_CENTER, 0);
    ui_pixel_nav_create(s_scr, 3);

    s_art_visible = false;
    s_timer = lv_timer_create(timer_tick, 250, NULL);
    app_music_refresh();
    lv_screen_load(s_scr);
}

void app_music_refresh(void)
{
    if (!s_scr) return;
    const sync_media_t *media = &app_store()->media;

    lv_label_set_text(s_foot,
                      sync_ble_is_connected() ? "LIVE FROM PHONE  |  2X BACK"
                                              : "PHONE OFFLINE");
    if (!media->active) {
        lv_label_set_text(s_source, "MEDIASESSION");
        lv_label_set_text(s_title, "NO MUSIC");
        lv_label_set_text(s_artist, "PLAY FROM PHONE");
        lv_label_set_text(s_album, "");
        lv_label_set_text(s_state, "WAITING");
        s_position_base = 0;
        s_duration_ms = 0;
        s_position_epoch_us = 0;
        s_playing = false;
    } else {
        lv_label_set_text(s_source, media->source_len ? media->source : "MEDIA");
        lv_label_set_text(s_title, media->title_len ? media->title : "UNKNOWN TRACK");
        lv_label_set_text(s_artist, media->artist_len ? media->artist : "UNKNOWN ARTIST");
        lv_label_set_text(s_album, media->album_len ? media->album : "");
        lv_label_set_text(s_state, media->playing ? "PLAYING" : "PAUSED");
        s_position_base = media->position_ms;
        s_duration_ms = media->duration_ms;
        s_position_epoch_us = esp_timer_get_time();
        s_playing = media->playing;
    }

    if (media->art_ready != s_art_visible) {
        s_art_visible = media->art_ready;
        if (s_art_visible) {
            lv_image_set_src(s_art, &s_art_dsc);
            lv_obj_remove_flag(s_art, LV_OBJ_FLAG_HIDDEN);
            lv_obj_add_flag(s_no_art, LV_OBJ_FLAG_HIDDEN);
            lv_obj_invalidate(s_art);
        } else {
            lv_obj_add_flag(s_art, LV_OBJ_FLAG_HIDDEN);
            lv_obj_remove_flag(s_no_art, LV_OBJ_FLAG_HIDDEN);
        }
    }
    progress_render();
}

void app_music_key(bsp_btn_t btn, bsp_btn_ev_t ev)
{
    (void)btn;
    (void)ev;
}

void app_music_exit(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_scr) {
        app_battery_unregister(s_bat);
        s_bat = NULL;
        lv_obj_delete(s_scr);
        s_scr = NULL;
    }
    s_source = s_title = s_artist = s_album = s_state = NULL;
    s_art = s_no_art = s_progress_fill = NULL;
    s_elapsed = s_duration = s_foot = NULL;
    s_art_visible = false;
}
