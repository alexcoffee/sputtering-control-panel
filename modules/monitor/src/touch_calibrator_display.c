#include "touch_calibrator_display.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "lvgl/lvgl.h"
#include "pico/stdlib.h"

/* AliExpress module specs: 3.5" 480x320 SPI TFT (ILI9488) + XPT2046 resistive touch. */
#define TOUCH_CALIBRATOR_DISP_HOR_RES 480
#define TOUCH_CALIBRATOR_DISP_VER_RES 320

#define ILI9488_CMD_MODE 0
#define ILI9488_DATA_MODE 1

#define ILI9488_SWRESET 0x01
#define ILI9488_SLPOUT 0x11
#define ILI9488_DISPON 0x29
#define ILI9488_CASET 0x2A
#define ILI9488_PASET 0x2B
#define ILI9488_RAMWR 0x2C
#define ILI9488_MADCTL 0x36
#define ILI9488_PIXFMT 0x3A

#define ILI9488_MADCTL_MY 0x80
#define ILI9488_MADCTL_MX 0x40
#define ILI9488_MADCTL_MV 0x20
#define ILI9488_MADCTL_BGR 0x08

#define TOUCH_CALIBRATION_POINT_COUNT 5U
#define TOUCH_MIN_CAPTURE_SAMPLES 3U

typedef struct {
    lv_point_t screen;
    uint16_t raw_x;
    uint16_t raw_y;
} touch_calibration_point_t;

typedef struct {
    float ax;
    float bx;
    float cx;
    float ay;
    float by;
    float cy;
    bool valid;
} touch_calibration_transform_t;

static lv_obj_t *s_instruction_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_marker;
static lv_obj_t *s_cursor;
static lv_obj_t *s_driver_test_label;

static touch_calibration_point_t s_points[TOUCH_CALIBRATION_POINT_COUNT];
static touch_calibration_transform_t s_transform;
static uint8_t s_current_point;

static bool s_touch_capture_active;
static uint32_t s_capture_sum_x;
static uint32_t s_capture_sum_y;
static uint16_t s_capture_samples;

static inline void ili9488_write(uint8_t mode, uint8_t value) {
    touch_calibrator_display_spi_set_lcd_cd(mode == ILI9488_DATA_MODE);
    touch_calibrator_display_spi_write_byte(value);
}

static inline void ili9488_write_array(uint8_t mode, const uint8_t *data, size_t len) {
    touch_calibrator_display_spi_set_lcd_cd(mode == ILI9488_DATA_MODE);
    touch_calibrator_display_spi_write_array(data, len);
}

static void ili9488_set_window(int32_t x1, int32_t y1, int32_t x2, int32_t y2);

static void ili9488_fill_color565(uint16_t color) {
    static uint8_t row_buf[TOUCH_CALIBRATOR_DISP_HOR_RES * 3];
    const uint8_t r = (uint8_t)((((color >> 11) & 0x1FU) * 255U) / 31U);
    const uint8_t g = (uint8_t)((((color >> 5) & 0x3FU) * 255U) / 63U);
    const uint8_t b = (uint8_t)(((color & 0x1FU) * 255U) / 31U);

    for (size_t i = 0; i < TOUCH_CALIBRATOR_DISP_HOR_RES; ++i) {
        row_buf[(i * 3U)] = r;
        row_buf[(i * 3U) + 1U] = g;
        row_buf[(i * 3U) + 2U] = b;
    }

    touch_calibrator_display_spi_set_lcd_cs(0);
    ili9488_set_window(0, 0, TOUCH_CALIBRATOR_DISP_HOR_RES - 1, TOUCH_CALIBRATOR_DISP_VER_RES - 1);
    for (size_t y = 0; y < TOUCH_CALIBRATOR_DISP_VER_RES; ++y) {
        ili9488_write_array(ILI9488_DATA_MODE, row_buf, sizeof(row_buf));
    }
    touch_calibrator_display_spi_set_lcd_cs(1);
}

static void ili9488_boot_test_pattern(void) {
    /* Raw SPI display test independent of LVGL rendering. */
    ili9488_fill_color565(0xF800U); /* Red   */
    sleep_ms(250);
    ili9488_fill_color565(0x07E0U); /* Green */
    sleep_ms(250);
    ili9488_fill_color565(0x001FU); /* Blue  */
    sleep_ms(250);
}

static void ili9488_set_window(int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    uint8_t data[4];

    ili9488_write(ILI9488_CMD_MODE, ILI9488_CASET);
    data[0] = (uint8_t)((x1 >> 8) & 0xFF);
    data[1] = (uint8_t)(x1 & 0xFF);
    data[2] = (uint8_t)((x2 >> 8) & 0xFF);
    data[3] = (uint8_t)(x2 & 0xFF);
    ili9488_write_array(ILI9488_DATA_MODE, data, sizeof(data));

    ili9488_write(ILI9488_CMD_MODE, ILI9488_PASET);
    data[0] = (uint8_t)((y1 >> 8) & 0xFF);
    data[1] = (uint8_t)(y1 & 0xFF);
    data[2] = (uint8_t)((y2 >> 8) & 0xFF);
    data[3] = (uint8_t)(y2 & 0xFF);
    ili9488_write_array(ILI9488_DATA_MODE, data, sizeof(data));

    ili9488_write(ILI9488_CMD_MODE, ILI9488_RAMWR);
}

static void ili9488_init_panel(void) {
    static const uint8_t gamma_pos[] = {0x00, 0x04, 0x0E, 0x08, 0x17, 0x0A, 0x40, 0x79, 0x4D, 0x07, 0x0E, 0x0A, 0x1A, 0x1D, 0x0F};
    static const uint8_t gamma_neg[] = {0x00, 0x1B, 0x1F, 0x02, 0x10, 0x05, 0x32, 0x34, 0x43, 0x02, 0x0A, 0x09, 0x32, 0x36, 0x0F};
    static const uint8_t power_c0[] = {0x17, 0x15};
    static const uint8_t power_c5[] = {0x00, 0x12, 0x80};
    static const uint8_t power_f7[] = {0xA9, 0x51, 0x2C, 0x82};
    static const uint8_t disp_fn[] = {0x02, 0x02};

    touch_calibrator_display_spi_set_lcd_cs(1);
    touch_calibrator_display_spi_set_lcd_reset(0);
    sleep_ms(20);
    touch_calibrator_display_spi_set_lcd_reset(1);
    sleep_ms(120);
    touch_calibrator_display_spi_set_lcd_cs(0);

    ili9488_write(ILI9488_CMD_MODE, ILI9488_SWRESET);
    sleep_ms(120);

    ili9488_write(ILI9488_CMD_MODE, 0xE0);
    ili9488_write_array(ILI9488_DATA_MODE, gamma_pos, sizeof(gamma_pos));

    ili9488_write(ILI9488_CMD_MODE, 0xE1);
    ili9488_write_array(ILI9488_DATA_MODE, gamma_neg, sizeof(gamma_neg));

    ili9488_write(ILI9488_CMD_MODE, 0xC0);
    ili9488_write_array(ILI9488_DATA_MODE, power_c0, sizeof(power_c0));

    ili9488_write(ILI9488_CMD_MODE, 0xC1);
    ili9488_write(ILI9488_DATA_MODE, 0x41);

    ili9488_write(ILI9488_CMD_MODE, 0xC5);
    ili9488_write_array(ILI9488_DATA_MODE, power_c5, sizeof(power_c5));

    ili9488_write(ILI9488_CMD_MODE, ILI9488_MADCTL);
    ili9488_write(ILI9488_DATA_MODE, ILI9488_MADCTL_MV | ILI9488_MADCTL_MX | ILI9488_MADCTL_MY | ILI9488_MADCTL_BGR);

    ili9488_write(ILI9488_CMD_MODE, ILI9488_PIXFMT);
    ili9488_write(ILI9488_DATA_MODE, 0x66);

    ili9488_write(ILI9488_CMD_MODE, 0xB0);
    ili9488_write(ILI9488_DATA_MODE, 0x00);

    ili9488_write(ILI9488_CMD_MODE, 0xB1);
    ili9488_write(ILI9488_DATA_MODE, 0xA0);

    ili9488_write(ILI9488_CMD_MODE, 0xB4);
    ili9488_write(ILI9488_DATA_MODE, 0x02);

    ili9488_write(ILI9488_CMD_MODE, 0xB6);
    ili9488_write_array(ILI9488_DATA_MODE, disp_fn, sizeof(disp_fn));

    ili9488_write(ILI9488_CMD_MODE, 0xF7);
    ili9488_write_array(ILI9488_DATA_MODE, power_f7, sizeof(power_f7));

    ili9488_write(ILI9488_CMD_MODE, ILI9488_SLPOUT);
    sleep_ms(120);

    ili9488_write(ILI9488_CMD_MODE, ILI9488_DISPON);
    sleep_ms(20);

    touch_calibrator_display_spi_set_lcd_cs(1);
}

static void ili9488_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    if (area->x2 < 0 || area->y2 < 0
        || area->x1 > (TOUCH_CALIBRATOR_DISP_HOR_RES - 1)
        || area->y1 > (TOUCH_CALIBRATOR_DISP_VER_RES - 1)) {
        lv_disp_flush_ready(disp_drv);
        return;
    }

    const int32_t act_x1 = area->x1 < 0 ? 0 : area->x1;
    const int32_t act_y1 = area->y1 < 0 ? 0 : area->y1;
    const int32_t act_x2 = area->x2 > (TOUCH_CALIBRATOR_DISP_HOR_RES - 1) ? (TOUCH_CALIBRATOR_DISP_HOR_RES - 1) : area->x2;
    const int32_t act_y2 = area->y2 > (TOUCH_CALIBRATOR_DISP_VER_RES - 1) ? (TOUCH_CALIBRATOR_DISP_VER_RES - 1) : area->y2;

    const int32_t src_width = (area->x2 - area->x1) + 1;
    const int32_t skip_x = act_x1 - area->x1;
    const int32_t skip_y = act_y1 - area->y1;
    const int32_t act_width = (act_x2 - act_x1) + 1;
    const lv_color_t *row_ptr = color_p + (skip_y * src_width) + skip_x;
    static uint8_t line_buf[TOUCH_CALIBRATOR_DISP_HOR_RES * 3];

    touch_calibrator_display_spi_set_lcd_cs(0);
    ili9488_set_window(act_x1, act_y1, act_x2, act_y2);
    for (int32_t y = act_y1; y <= act_y2; ++y) {
        for (int32_t x = 0; x < act_width; ++x) {
            lv_color32_t c32;
            c32.full = lv_color_to32(row_ptr[x]);
            line_buf[(size_t)x * 3U] = c32.ch.red;
            line_buf[((size_t)x * 3U) + 1U] = c32.ch.green;
            line_buf[((size_t)x * 3U) + 2U] = c32.ch.blue;
        }
        ili9488_write_array(ILI9488_DATA_MODE, line_buf, (size_t)act_width * 3U);
        row_ptr += src_width;
    }
    touch_calibrator_display_spi_set_lcd_cs(1);

    lv_disp_flush_ready(disp_drv);
}

static bool solve_3x3(float a[3][3], float b[3], float out[3]) {
    for (uint8_t col = 0; col < 3U; ++col) {
        uint8_t pivot = col;
        float pivot_abs = a[pivot][col] >= 0.0f ? a[pivot][col] : -a[pivot][col];

        for (uint8_t row = (uint8_t)(col + 1U); row < 3U; ++row) {
            const float row_abs = a[row][col] >= 0.0f ? a[row][col] : -a[row][col];
            if (row_abs > pivot_abs) {
                pivot = row;
                pivot_abs = row_abs;
            }
        }

        if (pivot_abs < 1e-6f) {
            return false;
        }

        if (pivot != col) {
            for (uint8_t k = 0; k < 3U; ++k) {
                const float tmp = a[col][k];
                a[col][k] = a[pivot][k];
                a[pivot][k] = tmp;
            }
            const float b_tmp = b[col];
            b[col] = b[pivot];
            b[pivot] = b_tmp;
        }

        const float diag = a[col][col];
        for (uint8_t k = col; k < 3U; ++k) {
            a[col][k] /= diag;
        }
        b[col] /= diag;

        for (uint8_t row = 0; row < 3U; ++row) {
            if (row == col) {
                continue;
            }
            const float factor = a[row][col];
            if (factor == 0.0f) {
                continue;
            }
            for (uint8_t k = col; k < 3U; ++k) {
                a[row][k] -= factor * a[col][k];
            }
            b[row] -= factor * b[col];
        }
    }

    out[0] = b[0];
    out[1] = b[1];
    out[2] = b[2];
    return true;
}

static bool compute_transform(void) {
    float ata[3][3] = {{0.0f}};
    float atx[3] = {0.0f};
    float aty[3] = {0.0f};

    for (size_t i = 0; i < TOUCH_CALIBRATION_POINT_COUNT; ++i) {
        const float u = (float)s_points[i].raw_x;
        const float v = (float)s_points[i].raw_y;
        const float x = (float)s_points[i].screen.x;
        const float y = (float)s_points[i].screen.y;

        ata[0][0] += u * u;
        ata[0][1] += u * v;
        ata[0][2] += u;
        ata[1][1] += v * v;
        ata[1][2] += v;
        ata[2][2] += 1.0f;

        atx[0] += u * x;
        atx[1] += v * x;
        atx[2] += x;

        aty[0] += u * y;
        aty[1] += v * y;
        aty[2] += y;
    }

    ata[1][0] = ata[0][1];
    ata[2][0] = ata[0][2];
    ata[2][1] = ata[1][2];

    float lhs_x[3][3];
    float lhs_y[3][3];
    memcpy(lhs_x, ata, sizeof(lhs_x));
    memcpy(lhs_y, ata, sizeof(lhs_y));

    float coeff_x[3];
    float coeff_y[3];
    if (!solve_3x3(lhs_x, atx, coeff_x) || !solve_3x3(lhs_y, aty, coeff_y)) {
        return false;
    }

    s_transform.ax = coeff_x[0];
    s_transform.bx = coeff_x[1];
    s_transform.cx = coeff_x[2];
    s_transform.ay = coeff_y[0];
    s_transform.by = coeff_y[1];
    s_transform.cy = coeff_y[2];
    s_transform.valid = true;
    return true;
}

static void raw_to_screen(uint16_t raw_x, uint16_t raw_y, lv_point_t *out_point) {
    if (out_point == NULL) {
        return;
    }

    const float mapped_x = (s_transform.ax * (float)raw_x) + (s_transform.bx * (float)raw_y) + s_transform.cx;
    const float mapped_y = (s_transform.ay * (float)raw_x) + (s_transform.by * (float)raw_y) + s_transform.cy;

    float clamped_x = mapped_x;
    float clamped_y = mapped_y;
    if (clamped_x < 0.0f) {
        clamped_x = 0.0f;
    }
    if (clamped_x > (float)(TOUCH_CALIBRATOR_DISP_HOR_RES - 1)) {
        clamped_x = (float)(TOUCH_CALIBRATOR_DISP_HOR_RES - 1);
    }
    if (clamped_y < 0.0f) {
        clamped_y = 0.0f;
    }
    if (clamped_y > (float)(TOUCH_CALIBRATOR_DISP_VER_RES - 1)) {
        clamped_y = (float)(TOUCH_CALIBRATOR_DISP_VER_RES - 1);
    }

    out_point->x = (lv_coord_t)(clamped_x + 0.5f);
    out_point->y = (lv_coord_t)(clamped_y + 0.5f);
}

static void move_marker_to_point(uint8_t point_index) {
    if (s_marker == NULL || point_index >= TOUCH_CALIBRATION_POINT_COUNT) {
        return;
    }

    const lv_point_t target = s_points[point_index].screen;
    const lv_coord_t marker_w = lv_obj_get_width(s_marker);
    const lv_coord_t marker_h = lv_obj_get_height(s_marker);
    lv_obj_set_pos(s_marker, target.x - (marker_w / 2), target.y - (marker_h / 2));
}

static void show_calibration_prompt(void) {
    if (s_instruction_label == NULL) {
        return;
    }

    char instruction[96];
    (void)snprintf(instruction,
                   sizeof(instruction),
                   "Tap marker %u/%u, hold briefly, then release.",
                   (unsigned int)(s_current_point + 1U),
                   (unsigned int)TOUCH_CALIBRATION_POINT_COUNT);
    lv_label_set_text(s_instruction_label, instruction);
}

static void start_capture(void) {
    s_touch_capture_active = true;
    s_capture_sum_x = 0U;
    s_capture_sum_y = 0U;
    s_capture_samples = 0U;
}

static void finish_calibration(void) {
    if (compute_transform()) {
        char text[160];
        (void)snprintf(text,
                       sizeof(text),
                       "Calibration complete. Touch to test.\nraw->px: x=%.6f*u + %.6f*v + %.2f\ny=%.6f*u + %.6f*v + %.2f",
                       (double)s_transform.ax,
                       (double)s_transform.bx,
                       (double)s_transform.cx,
                       (double)s_transform.ay,
                       (double)s_transform.by,
                       (double)s_transform.cy);
        lv_label_set_text(s_instruction_label, text);
        lv_obj_add_flag(s_marker, LV_OBJ_FLAG_HIDDEN);
        printf("touch calibration complete\n");
        printf("x = %.6f * raw_x + %.6f * raw_y + %.2f\n",
               (double)s_transform.ax,
               (double)s_transform.bx,
               (double)s_transform.cx);
        printf("y = %.6f * raw_x + %.6f * raw_y + %.2f\n",
               (double)s_transform.ay,
               (double)s_transform.by,
               (double)s_transform.cy);
        return;
    }

    s_transform.valid = false;
    s_current_point = 0U;
    show_calibration_prompt();
    move_marker_to_point(s_current_point);
    lv_label_set_text(s_status_label, "Calibration solve failed. Restarting...");
}

static void update_calibration(void) {
    uint16_t raw_x;
    uint16_t raw_y;
    const bool touched = touch_calibrator_display_spi_touch_read_raw(&raw_x, &raw_y);

    if (touched) {
        if (s_driver_test_label != NULL) {
            lv_obj_add_flag(s_driver_test_label, LV_OBJ_FLAG_HIDDEN);
        }

        if (!s_touch_capture_active) {
            start_capture();
        }

        s_capture_sum_x += raw_x;
        s_capture_sum_y += raw_y;
        s_capture_samples++;

        char status[64];
        (void)snprintf(status,
                       sizeof(status),
                       "capturing raw=(%u,%u), samples=%u",
                       (unsigned int)raw_x,
                       (unsigned int)raw_y,
                       (unsigned int)s_capture_samples);
        lv_label_set_text(s_status_label, status);
        return;
    }

    if (!s_touch_capture_active) {
        return;
    }

    s_touch_capture_active = false;
    if (s_capture_samples < TOUCH_MIN_CAPTURE_SAMPLES) {
        lv_label_set_text(s_status_label, "Capture too short. Try the same point again.");
        return;
    }

    s_points[s_current_point].raw_x = (uint16_t)(s_capture_sum_x / s_capture_samples);
    s_points[s_current_point].raw_y = (uint16_t)(s_capture_sum_y / s_capture_samples);

    char status[64];
    (void)snprintf(status,
                   sizeof(status),
                   "P%u raw=(%u,%u) saved",
                   (unsigned int)(s_current_point + 1U),
                   (unsigned int)s_points[s_current_point].raw_x,
                   (unsigned int)s_points[s_current_point].raw_y);
    lv_label_set_text(s_status_label, status);

    s_current_point++;
    if (s_current_point < TOUCH_CALIBRATION_POINT_COUNT) {
        show_calibration_prompt();
        move_marker_to_point(s_current_point);
        return;
    }

    finish_calibration();
}

static void update_test_cursor(void) {
    uint16_t raw_x;
    uint16_t raw_y;
    if (!touch_calibrator_display_spi_touch_read_raw(&raw_x, &raw_y)) {
        lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    lv_point_t point;
    raw_to_screen(raw_x, raw_y, &point);
    lv_obj_clear_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_pos(s_cursor, point.x - 4, point.y - 4);

    char status[80];
    (void)snprintf(status,
                   sizeof(status),
                   "raw=(%u,%u) mapped=(%d,%d)",
                   (unsigned int)raw_x,
                   (unsigned int)raw_y,
                   point.x,
                   point.y);
    lv_label_set_text(s_status_label, status);
}

void touch_calibrator_display_init(const touch_calibrator_display_spi_pins_t *pins) {
    if (pins == NULL) {
        return;
    }

    memset(&s_transform, 0, sizeof(s_transform));
    s_current_point = 0U;
    s_touch_capture_active = false;
    s_capture_sum_x = 0U;
    s_capture_sum_y = 0U;
    s_capture_samples = 0U;

    s_points[0].screen = (lv_point_t){30, 30};
    s_points[1].screen = (lv_point_t){TOUCH_CALIBRATOR_DISP_HOR_RES - 31, 30};
    s_points[2].screen = (lv_point_t){TOUCH_CALIBRATOR_DISP_HOR_RES - 31, TOUCH_CALIBRATOR_DISP_VER_RES - 31};
    s_points[3].screen = (lv_point_t){30, TOUCH_CALIBRATOR_DISP_VER_RES - 31};
    s_points[4].screen = (lv_point_t){TOUCH_CALIBRATOR_DISP_HOR_RES / 2, TOUCH_CALIBRATOR_DISP_VER_RES / 2};

    touch_calibrator_display_spi_init(pins);
    lv_init();
    ili9488_init_panel();
    ili9488_boot_test_pattern();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t draw_buf_1[TOUCH_CALIBRATOR_DISP_HOR_RES * 20];
    lv_disp_draw_buf_init(&draw_buf, draw_buf_1, NULL, TOUCH_CALIBRATOR_DISP_HOR_RES * 20);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = TOUCH_CALIBRATOR_DISP_HOR_RES;
    disp_drv.ver_res = TOUCH_CALIBRATOR_DISP_VER_RES;
    disp_drv.flush_cb = ili9488_flush;
    disp_drv.draw_buf = &draw_buf;
    (void)lv_disp_drv_register(&disp_drv);

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0A0E19), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title_label = lv_label_create(lv_scr_act());
    lv_label_set_text(title_label, "LCD Touch Calibration (ILI9488 + XPT2046)");
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x6AE4FF), 0);

    s_driver_test_label = lv_label_create(lv_scr_act());
    lv_obj_set_width(s_driver_test_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 40);
    lv_label_set_text(s_driver_test_label,
                      "LCD DRIVER TEST\n"
                      "If you can read this text, SPI display is working.");
    lv_label_set_long_mode(s_driver_test_label, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_driver_test_label, LV_ALIGN_TOP_LEFT, 20, 56);
    lv_obj_set_style_text_color(s_driver_test_label, lv_color_hex(0xFFE29A), 0);

    s_instruction_label = lv_label_create(lv_scr_act());
    lv_obj_set_width(s_instruction_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 20);
    lv_obj_align(s_instruction_label, LV_ALIGN_TOP_LEFT, 10, 130);
    lv_label_set_long_mode(s_instruction_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_instruction_label, lv_color_hex(0xE5EDF5), 0);

    s_status_label = lv_label_create(lv_scr_act());
    lv_obj_set_width(s_status_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 20);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_LEFT, 10, -8);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xA6C2D9), 0);

    s_marker = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_marker);
    lv_obj_set_size(s_marker, 26, 26);
    lv_obj_set_style_radius(s_marker, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_marker, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_marker, 2, 0);
    lv_obj_set_style_border_color(s_marker, lv_color_hex(0xFFB347), 0);
    lv_obj_set_style_outline_color(s_marker, lv_color_hex(0xFFB347), 0);
    lv_obj_set_style_outline_opa(s_marker, LV_OPA_30, 0);
    lv_obj_set_style_outline_width(s_marker, 4, 0);
    lv_obj_set_style_outline_pad(s_marker, 0, 0);

    lv_obj_t *marker_cross = lv_label_create(s_marker);
    lv_label_set_text(marker_cross, "+");
    lv_obj_set_style_text_color(marker_cross, lv_color_hex(0xFFB347), 0);
    lv_obj_center(marker_cross);

    s_cursor = lv_obj_create(lv_scr_act());
    lv_obj_remove_style_all(s_cursor);
    lv_obj_set_size(s_cursor, 8, 8);
    lv_obj_set_style_radius(s_cursor, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(s_cursor, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(s_cursor, lv_color_hex(0x59F07A), 0);
    lv_obj_add_flag(s_cursor, LV_OBJ_FLAG_HIDDEN);

    show_calibration_prompt();
    lv_label_set_text(s_status_label, "Tap the marker to capture raw touch values.");
    move_marker_to_point(s_current_point);
}

void touch_calibrator_display_tick(uint32_t elapsed_ms) {
    lv_tick_inc(elapsed_ms);
}

void touch_calibrator_display_task_handler(void) {
    if (!s_transform.valid) {
        update_calibration();
    } else {
        update_test_cursor();
    }
    (void)lv_timer_handler();
}
