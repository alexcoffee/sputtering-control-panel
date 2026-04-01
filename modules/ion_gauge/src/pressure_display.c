#include "pressure_display.h"

#include "lvgl/lvgl.h"
#include "pressure_display_spi.h"
#include "display/GC9A01.h"

#define PRESSURE_DISP_HOR_RES 240
#define PRESSURE_DISP_VER_RES 240

static lv_obj_t *s_meter;
static lv_meter_indicator_t *s_needle;
static lv_obj_t *s_value_label;
static lv_obj_t *s_unit_label;

void pressure_display_init(const pressure_display_spi_pins_t *pins) {
    pressure_display_spi_init(pins);
    lv_init();
    (void)GC9A01_init();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t buf_1[PRESSURE_DISP_HOR_RES * 10];
    lv_disp_draw_buf_init(&draw_buf, buf_1, NULL, PRESSURE_DISP_HOR_RES * 10);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = PRESSURE_DISP_HOR_RES;
    disp_drv.ver_res = PRESSURE_DISP_VER_RES;
    disp_drv.sw_rotate = 0;
    disp_drv.rotated = LV_DISP_ROT_NONE;
    disp_drv.flush_cb = GC9A01_flush;
    disp_drv.draw_buf = &draw_buf;
    lv_disp_drv_register(&disp_drv);

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_white(), LV_PART_MAIN);

    s_meter = lv_meter_create(lv_scr_act());
    lv_obj_center(s_meter);
    lv_obj_set_size(s_meter, PRESSURE_DISP_HOR_RES, PRESSURE_DISP_VER_RES);
    lv_obj_set_style_bg_color(s_meter, lv_color_white(), LV_PART_MAIN);

    lv_meter_scale_t *scale = lv_meter_add_scale(s_meter);
    lv_meter_set_scale_range(s_meter, scale, 0, 1000, 270, 135);
    lv_meter_set_scale_ticks(s_meter, scale, 11, 2, 10, lv_palette_main(LV_PALETTE_GREY));
    lv_meter_set_scale_major_ticks(s_meter, scale, 1, 3, 15, lv_color_black(), 10);

    lv_meter_indicator_t *arc = lv_meter_add_arc(s_meter, scale, 18, lv_palette_main(LV_PALETTE_GREEN), 18);
    lv_meter_set_indicator_start_value(s_meter, arc, 0);
    lv_meter_set_indicator_end_value(s_meter, arc, 750);

    arc = lv_meter_add_arc(s_meter, scale, 18, lv_palette_darken(LV_PALETTE_GREEN, 3), 18);
    lv_meter_set_indicator_start_value(s_meter, arc, 750);
    lv_meter_set_indicator_end_value(s_meter, arc, 1000);

    s_needle = lv_meter_add_needle_line(s_meter, scale, 10, lv_palette_main(LV_PALETTE_DEEP_PURPLE), 20);

    lv_obj_t *center_circle = lv_obj_create(s_meter);
    lv_obj_set_size(center_circle, 95, 95);
    lv_obj_center(center_circle);
    lv_obj_clear_flag(center_circle, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(center_circle, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(center_circle, lv_color_white(), 0);
    lv_obj_set_style_border_width(center_circle, 0, 0);

    s_value_label = lv_label_create(lv_scr_act());
    lv_obj_set_width(s_value_label, 200);
    lv_obj_center(s_value_label);
    lv_obj_set_style_text_font(s_value_label, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_align(s_value_label, LV_TEXT_ALIGN_CENTER, 0);

    s_unit_label = lv_label_create(lv_scr_act());
    lv_obj_set_width(s_unit_label, 200);
    lv_obj_align(s_unit_label, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_style_text_font(s_unit_label, &lv_font_montserrat_32, 0);
    lv_obj_set_style_text_align(s_unit_label, LV_TEXT_ALIGN_CENTER, 0);

    pressure_display_render_unplugged();
}

void pressure_display_tick(uint32_t elapsed_ms) {
    lv_tick_inc(elapsed_ms);
}

void pressure_display_task_handler(void) {
    (void)lv_timer_handler();
}

void pressure_display_render(float torr_value, float voltage, pressure_display_unit_t unit) {
    if (s_meter == NULL || s_needle == NULL || s_value_label == NULL || s_unit_label == NULL) {
        return;
    }

    float gauge_value = torr_value;
    if (unit == PRESSURE_DISPLAY_UNIT_VOLTAGE) {
        gauge_value = voltage * 100.0f;
    }

    if (gauge_value < 0.0f) {
        gauge_value = 0.0f;
    }
    if (gauge_value > 1000.0f) {
        gauge_value = 1000.0f;
    }

    lv_meter_set_indicator_value(s_meter, s_needle, (int32_t)gauge_value);

    char value_buf[16];
    char unit_buf[8];
    pressure_format_reading(value_buf,
                            (int)sizeof(value_buf),
                            unit_buf,
                            (int)sizeof(unit_buf),
                            torr_value,
                            voltage,
                            unit);
    lv_label_set_text(s_value_label, value_buf);
    lv_label_set_text(s_unit_label, unit_buf);

    if (torr_value > 0.01f) {
        lv_obj_set_style_bg_color(lv_scr_act(), lv_palette_main(LV_PALETTE_RED), LV_PART_MAIN);
    } else {
        lv_obj_set_style_bg_color(lv_scr_act(), lv_color_white(), LV_PART_MAIN);
    }
}

void pressure_display_render_unplugged(void) {
    if (s_meter == NULL || s_needle == NULL || s_value_label == NULL || s_unit_label == NULL) {
        return;
    }

    lv_meter_set_indicator_value(s_meter, s_needle, 0);
    lv_label_set_text(s_value_label, "--");
    lv_label_set_text(s_unit_label, "plug in");
    lv_obj_set_style_bg_color(lv_scr_act(), lv_palette_lighten(LV_PALETTE_GREY, 2), LV_PART_MAIN);
}
