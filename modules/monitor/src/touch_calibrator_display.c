#include "touch_calibrator_display.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "hardware/flash.h"
#include "hardware/sync.h"
#include "lvgl/lvgl.h"
#include "pico/stdlib.h"
#include "scp/can_bus.h"
#include "scp/module_runtime.h"
#include "scp/protocol.h"

extern char __flash_binary_end;
extern const uint32_t monitor_jar_splash_rgb565_width;
extern const uint32_t monitor_jar_splash_rgb565_height;
extern const uint16_t monitor_jar_splash_rgb565[];

/* AliExpress module specs: 3.5" 480x320 SPI TFT (ILI9488) + ADS7843/XPT2046 touch. */
#define TOUCH_CALIBRATOR_DISP_HOR_RES 320
#define TOUCH_CALIBRATOR_DISP_VER_RES 480
#define TOUCH_CALIBRATOR_DRAW_BUF_LINES 40

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

#define MONITOR_MAX_CONNECTION_ROWS 16U
#define MONITOR_EVENT_MAX_LINES 14U
#define MONITOR_EVENT_LINE_CHARS 96U
#define MONITOR_EVENT_TEXT_CHARS ((MONITOR_EVENT_MAX_LINES * MONITOR_EVENT_LINE_CHARS) + MONITOR_EVENT_MAX_LINES + 1U)
#define MONITOR_TAB_COUNT 3U

#define MONITOR_HEARTBEAT_WINDOW 0x80U
#define MONITOR_HEARTBEAT_TIMEOUT_MS (SCP_HEARTBEAT_PERIOD * 3U)
#define MONITOR_ENCODER_DEBOUNCE_MS 8U

#define TOUCH_DEFAULT_RAW_MIN 200U
#define TOUCH_DEFAULT_RAW_MAX 3800U

#define TOUCH_CAL_FLASH_MAGIC 0x434C4254u /* "TBLC" */
#define TOUCH_CAL_FLASH_VERSION 4u
#define TOUCH_CAL_FLASH_OFFSET (PICO_FLASH_SIZE_BYTES - FLASH_SECTOR_SIZE)

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

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t length;
    float ax;
    float bx;
    float cx;
    float ay;
    float by;
    float cy;
    uint32_t crc32;
} touch_calibration_flash_record_t;

_Static_assert(sizeof(touch_calibration_flash_record_t) <= FLASH_PAGE_SIZE,
               "Calibration record must fit in one flash page");

typedef struct {
    bool used;
    bool heartbeat_seen;
    uint8_t module_id;
    uint32_t last_heartbeat_ms;
    lv_obj_t *row;
    lv_obj_t *icon;
    lv_obj_t *text;
} monitor_connection_row_t;

typedef enum {
    MONITOR_MODE_UI = 0,
    MONITOR_MODE_CALIBRATION = 1
} monitor_mode_t;

static monitor_mode_t s_mode;

static lv_obj_t *s_root_tabs;
static lv_obj_t *s_connections_list;
static lv_obj_t *s_connections_empty_label;
static lv_obj_t *s_event_log_label;
static lv_obj_t *s_settings_status_label;
static lv_obj_t *s_brightness_value_label;

static lv_obj_t *s_calibration_root;
static lv_obj_t *s_instruction_label;
static lv_obj_t *s_status_label;
static lv_obj_t *s_marker;

static monitor_connection_row_t s_connection_rows[MONITOR_MAX_CONNECTION_ROWS];
static char s_event_lines[MONITOR_EVENT_MAX_LINES][MONITOR_EVENT_LINE_CHARS];
static char s_event_log_text[MONITOR_EVENT_TEXT_CHARS];
static uint8_t s_event_line_count;

static touch_calibration_point_t s_points[TOUCH_CALIBRATION_POINT_COUNT];
static touch_calibration_transform_t s_transform;
static uint8_t s_current_point;

static bool s_touch_capture_active;
static uint32_t s_capture_sum_x;
static uint32_t s_capture_sum_y;
static uint16_t s_capture_samples;
static lv_point_t s_last_touch_point;

static bool s_encoder_available;
static uint8_t s_encoder_a_pin;
static uint8_t s_encoder_b_pin;
static uint8_t s_encoder_button_pin;
static uint8_t s_encoder_prev_ab_state;
static int8_t s_encoder_transition_accumulator;
static int16_t s_encoder_step_delta;
static bool s_encoder_button_raw_pressed;
static bool s_encoder_button_debounced_pressed;
static uint32_t s_encoder_button_last_edge_ms;
static bool s_encoder_button_click_pending;

static inline void ili9488_write(uint8_t mode, uint8_t value) {
    touch_calibrator_display_spi_set_lcd_cd(mode == ILI9488_DATA_MODE);
    touch_calibrator_display_spi_write_byte(value);
}

static inline void ili9488_write_array(uint8_t mode, const uint8_t *data, size_t len) {
    touch_calibrator_display_spi_set_lcd_cd(mode == ILI9488_DATA_MODE);
    touch_calibrator_display_spi_write_array(data, len);
}

static void ili9488_set_window(int32_t x1, int32_t y1, int32_t x2, int32_t y2);
static void refresh_connection_rows(uint32_t now_ms);
static void start_calibration_session(void);
static bool load_saved_calibration(void);
static bool save_calibration_to_flash(void);
static void monitor_encoder_init(const touch_calibrator_display_spi_pins_t *pins, uint32_t now_ms);
static void monitor_encoder_sample(uint32_t now_ms);
static void monitor_encoder_apply_tab_navigation(void);
static bool ili9488_draw_splash_from_flash(void);
static void wait_for_splash_dismiss(uint32_t max_wait_ms);

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
    ili9488_fill_color565(0xF800U);
    sleep_ms(10);
    ili9488_fill_color565(0x07E0U);
    sleep_ms(10);
    ili9488_fill_color565(0x001FU);
    sleep_ms(10);
}

static bool ili9488_draw_splash_from_flash(void) {
    if (monitor_jar_splash_rgb565_width != TOUCH_CALIBRATOR_DISP_HOR_RES
        || monitor_jar_splash_rgb565_height != TOUCH_CALIBRATOR_DISP_VER_RES) {
        return false;
    }

    static uint8_t row_buf[TOUCH_CALIBRATOR_DISP_HOR_RES * 3];

    touch_calibrator_display_spi_set_lcd_cs(0);
    ili9488_set_window(0, 0, TOUCH_CALIBRATOR_DISP_HOR_RES - 1, TOUCH_CALIBRATOR_DISP_VER_RES - 1);
    for (size_t y = 0; y < TOUCH_CALIBRATOR_DISP_VER_RES; ++y) {
        const uint16_t *row = &monitor_jar_splash_rgb565[y * TOUCH_CALIBRATOR_DISP_HOR_RES];
        for (size_t x = 0; x < TOUCH_CALIBRATOR_DISP_HOR_RES; ++x) {
            const uint16_t c = row[x];
            row_buf[x * 3U] = (uint8_t)((((c >> 11) & 0x1FU) * 255U) / 31U);
            row_buf[(x * 3U) + 1U] = (uint8_t)((((c >> 5) & 0x3FU) * 255U) / 63U);
            row_buf[(x * 3U) + 2U] = (uint8_t)(((c & 0x1FU) * 255U) / 31U);
        }
        ili9488_write_array(ILI9488_DATA_MODE, row_buf, sizeof(row_buf));
    }
    touch_calibrator_display_spi_set_lcd_cs(1);
    return true;
}

static void wait_for_splash_dismiss(uint32_t max_wait_ms) {
    const uint32_t start_ms = to_ms_since_boot(get_absolute_time());
    uint16_t raw_x = 0U;
    uint16_t raw_y = 0U;

    while ((uint32_t)(to_ms_since_boot(get_absolute_time()) - start_ms) < max_wait_ms) {
        if (touch_calibrator_display_spi_touch_read_raw(&raw_x, &raw_y)) {
            const uint32_t release_start_ms = to_ms_since_boot(get_absolute_time());
            while (touch_calibrator_display_spi_touch_read_raw(&raw_x, &raw_y)) {
                if ((uint32_t)(to_ms_since_boot(get_absolute_time()) - release_start_ms) >= 300U) {
                    break;
                }
                sleep_ms(10);
            }
            return;
        }
        sleep_ms(15);
    }
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
    /* Rotate panel output 180 degrees from the previous orientation (portrait, upside-down). */
    ili9488_write(ILI9488_CMD_MODE, ILI9488_MADCTL);
    ili9488_write(ILI9488_DATA_MODE, ILI9488_MADCTL_MX | ILI9488_MADCTL_BGR);
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

static void raw_to_screen_calibrated(uint16_t raw_x, uint16_t raw_y, lv_point_t *out_point) {
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

static lv_coord_t map_axis_linear(uint16_t raw, uint16_t raw_min, uint16_t raw_max, lv_coord_t screen_max) {
    if (raw <= raw_min) {
        return 0;
    }
    if (raw >= raw_max) {
        return screen_max;
    }

    const uint32_t span_raw = (uint32_t)raw_max - (uint32_t)raw_min;
    const uint32_t normalized = (uint32_t)raw - (uint32_t)raw_min;
    const uint32_t mapped = (normalized * (uint32_t)screen_max) / span_raw;
    return (lv_coord_t)mapped;
}

static void raw_to_screen_default(uint16_t raw_x, uint16_t raw_y, lv_point_t *out_point) {
    if (out_point == NULL) {
        return;
    }

    /* Panel is rotated 180 degrees from prior orientation: swap axes and flip X. */
    const lv_coord_t mapped_x = map_axis_linear(raw_y,
                                                TOUCH_DEFAULT_RAW_MIN,
                                                TOUCH_DEFAULT_RAW_MAX,
                                                TOUCH_CALIBRATOR_DISP_HOR_RES - 1);
    const lv_coord_t mapped_y = map_axis_linear(raw_x,
                                                TOUCH_DEFAULT_RAW_MIN,
                                                TOUCH_DEFAULT_RAW_MAX,
                                                TOUCH_CALIBRATOR_DISP_VER_RES - 1);

    out_point->x = (TOUCH_CALIBRATOR_DISP_HOR_RES - 1) - mapped_x;
    out_point->y = mapped_y;
}

static void map_raw_to_screen(uint16_t raw_x, uint16_t raw_y, lv_point_t *out_point) {
    if (s_transform.valid) {
        raw_to_screen_calibrated(raw_x, raw_y, out_point);
        return;
    }
    raw_to_screen_default(raw_x, raw_y, out_point);
}

static uint32_t crc32_compute(const uint8_t *data, size_t length) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < length; ++i) {
        crc ^= (uint32_t)data[i];
        for (uint8_t bit = 0; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1U) ^ (0xEDB88320u & mask);
        }
    }
    return ~crc;
}

static void apply_calibration_record(const touch_calibration_flash_record_t *record) {
    if (record == NULL) {
        return;
    }

    s_transform.ax = record->ax;
    s_transform.bx = record->bx;
    s_transform.cx = record->cx;
    s_transform.ay = record->ay;
    s_transform.by = record->by;
    s_transform.cy = record->cy;
    s_transform.valid = true;
}

static bool load_saved_calibration(void) {
    const touch_calibration_flash_record_t *record =
            (const touch_calibration_flash_record_t *)(XIP_BASE + TOUCH_CAL_FLASH_OFFSET);

    if (record->magic != TOUCH_CAL_FLASH_MAGIC
        || record->version != TOUCH_CAL_FLASH_VERSION
        || record->length != sizeof(touch_calibration_flash_record_t)) {
        return false;
    }

    const uint32_t expected_crc = crc32_compute((const uint8_t *)record,
                                                offsetof(touch_calibration_flash_record_t, crc32));
    if (record->crc32 != expected_crc) {
        return false;
    }

    apply_calibration_record(record);
    return true;
}

static bool save_calibration_to_flash(void) {
    touch_calibration_flash_record_t record = {
        .magic = TOUCH_CAL_FLASH_MAGIC,
        .version = TOUCH_CAL_FLASH_VERSION,
        .length = sizeof(touch_calibration_flash_record_t),
        .ax = s_transform.ax,
        .bx = s_transform.bx,
        .cx = s_transform.cx,
        .ay = s_transform.ay,
        .by = s_transform.by,
        .cy = s_transform.cy,
        .crc32 = 0U,
    };
    record.crc32 = crc32_compute((const uint8_t *)&record,
                                 offsetof(touch_calibration_flash_record_t, crc32));

    uint8_t page_buf[FLASH_PAGE_SIZE];
    memset(page_buf, 0xFF, sizeof(page_buf));
    memcpy(page_buf, &record, sizeof(record));

    const uint32_t irq_state = save_and_disable_interrupts();
    flash_range_erase(TOUCH_CAL_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(TOUCH_CAL_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);
    restore_interrupts(irq_state);

    const touch_calibration_flash_record_t *stored =
            (const touch_calibration_flash_record_t *)(XIP_BASE + TOUCH_CAL_FLASH_OFFSET);
    const uint32_t stored_crc = crc32_compute((const uint8_t *)stored,
                                              offsetof(touch_calibration_flash_record_t, crc32));

    return stored->magic == TOUCH_CAL_FLASH_MAGIC
           && stored->version == TOUCH_CAL_FLASH_VERSION
           && stored->length == sizeof(touch_calibration_flash_record_t)
           && stored->crc32 == stored_crc;
}

static void monitor_encoder_set_tab_relative(int8_t direction) {
    if (s_root_tabs == NULL || direction == 0) {
        return;
    }

    const int32_t current = (int32_t)lv_tabview_get_tab_act(s_root_tabs);
    int32_t target = current + (direction > 0 ? 1 : -1);
    if (target < 0) {
        target = 0;
    }
    if (target >= (int32_t)MONITOR_TAB_COUNT) {
        target = (int32_t)(MONITOR_TAB_COUNT - 1U);
    }

    if (target != current) {
        lv_tabview_set_act(s_root_tabs, (uint32_t)target, LV_ANIM_OFF);
    }
}

static void monitor_encoder_init(const touch_calibrator_display_spi_pins_t *pins, uint32_t now_ms) {
    s_encoder_available = false;
    if (pins == NULL) {
        return;
    }

    if (pins->encoder_a_pin > SCP_PICO_GPIO_MAX
        || pins->encoder_b_pin > SCP_PICO_GPIO_MAX
        || pins->encoder_button_pin > SCP_PICO_GPIO_MAX) {
        return;
    }

    s_encoder_a_pin = pins->encoder_a_pin;
    s_encoder_b_pin = pins->encoder_b_pin;
    s_encoder_button_pin = pins->encoder_button_pin;

    gpio_init(s_encoder_a_pin);
    gpio_set_dir(s_encoder_a_pin, GPIO_IN);
    gpio_pull_up(s_encoder_a_pin);

    gpio_init(s_encoder_b_pin);
    gpio_set_dir(s_encoder_b_pin, GPIO_IN);
    gpio_pull_up(s_encoder_b_pin);

    gpio_init(s_encoder_button_pin);
    gpio_set_dir(s_encoder_button_pin, GPIO_IN);
    gpio_pull_up(s_encoder_button_pin);

    s_encoder_prev_ab_state = (uint8_t)(((gpio_get(s_encoder_a_pin) ? 1U : 0U) << 1U)
                                        | (gpio_get(s_encoder_b_pin) ? 1U : 0U));
    s_encoder_transition_accumulator = 0;
    s_encoder_step_delta = 0;
    s_encoder_button_raw_pressed = gpio_get(s_encoder_button_pin) == 0U;
    s_encoder_button_debounced_pressed = s_encoder_button_raw_pressed;
    s_encoder_button_last_edge_ms = now_ms;
    s_encoder_button_click_pending = false;
    s_encoder_available = true;
}

static void monitor_encoder_sample(uint32_t now_ms) {
    if (!s_encoder_available) {
        return;
    }

    static const int8_t transition_lut[16] = {
        0, -1, 1, 0,
        1, 0, 0, -1,
        -1, 0, 0, 1,
        0, 1, -1, 0,
    };

    const uint8_t curr_ab_state = (uint8_t)(((gpio_get(s_encoder_a_pin) ? 1U : 0U) << 1U)
                                            | (gpio_get(s_encoder_b_pin) ? 1U : 0U));
    if (curr_ab_state != s_encoder_prev_ab_state) {
        const int8_t transition = transition_lut[(s_encoder_prev_ab_state << 2U) | curr_ab_state];
        if (transition != 0) {
            s_encoder_transition_accumulator += transition;
            if (s_encoder_transition_accumulator >= 4) {
                s_encoder_transition_accumulator = 0;
                s_encoder_step_delta++;
            } else if (s_encoder_transition_accumulator <= -4) {
                s_encoder_transition_accumulator = 0;
                s_encoder_step_delta--;
            }
        }
        s_encoder_prev_ab_state = curr_ab_state;
    }

    const bool button_raw_pressed = gpio_get(s_encoder_button_pin) == 0U;
    if (button_raw_pressed != s_encoder_button_raw_pressed) {
        s_encoder_button_raw_pressed = button_raw_pressed;
        s_encoder_button_last_edge_ms = now_ms;
    }

    if ((uint32_t)(now_ms - s_encoder_button_last_edge_ms) < MONITOR_ENCODER_DEBOUNCE_MS) {
        return;
    }

    if (s_encoder_button_debounced_pressed == s_encoder_button_raw_pressed) {
        return;
    }

    const bool was_pressed = s_encoder_button_debounced_pressed;
    s_encoder_button_debounced_pressed = s_encoder_button_raw_pressed;
    if (was_pressed && !s_encoder_button_debounced_pressed) {
        s_encoder_button_click_pending = true;
    }
}

static void monitor_encoder_apply_tab_navigation(void) {
    if (s_mode != MONITOR_MODE_UI || s_root_tabs == NULL) {
        s_encoder_step_delta = 0;
        s_encoder_button_click_pending = false;
        return;
    }

    while (s_encoder_step_delta > 0) {
        monitor_encoder_set_tab_relative(1);
        s_encoder_step_delta--;
    }
    while (s_encoder_step_delta < 0) {
        monitor_encoder_set_tab_relative(-1);
        s_encoder_step_delta++;
    }

    if (s_encoder_button_click_pending) {
        monitor_encoder_set_tab_relative(1);
        s_encoder_button_click_pending = false;
    }
}

static int find_connection_row(uint8_t module_id) {
    for (size_t i = 0; i < MONITOR_MAX_CONNECTION_ROWS; ++i) {
        if (s_connection_rows[i].used && s_connection_rows[i].module_id == module_id) {
            return (int)i;
        }
    }
    return -1;
}

static void update_connection_row_visual(monitor_connection_row_t *entry, uint32_t now_ms) {
    if (entry == NULL || !entry->used || entry->icon == NULL || entry->text == NULL) {
        return;
    }

    const bool online = entry->heartbeat_seen
                        && ((now_ms - entry->last_heartbeat_ms) <= MONITOR_HEARTBEAT_TIMEOUT_MS);
    lv_label_set_text(entry->icon, online ? LV_SYMBOL_LOOP : LV_SYMBOL_STOP);
    lv_obj_set_style_text_color(entry->icon,
                                online ? lv_color_hex(0x5BE37D) : lv_color_hex(0x657080),
                                0);

    char text[48];
    if (!entry->heartbeat_seen) {
        (void)snprintf(text, sizeof(text), "Module %u  (seen, no heartbeat)", (unsigned int)entry->module_id);
    } else {
        (void)snprintf(text, sizeof(text), "Module %u  (%s)", (unsigned int)entry->module_id, online ? "online" : "offline");
    }
    lv_label_set_text(entry->text, text);
}

static int ensure_connection_row(uint8_t module_id) {
    const int existing = find_connection_row(module_id);
    if (existing >= 0) {
        return existing;
    }

    for (size_t i = 0; i < MONITOR_MAX_CONNECTION_ROWS; ++i) {
        if (s_connection_rows[i].used) {
            continue;
        }

        monitor_connection_row_t *entry = &s_connection_rows[i];
        memset(entry, 0, sizeof(*entry));
        entry->used = true;
        entry->module_id = module_id;

        if (s_connections_list != NULL) {
            entry->row = lv_obj_create(s_connections_list);
            lv_obj_set_size(entry->row, lv_pct(100), LV_SIZE_CONTENT);
            lv_obj_set_style_bg_color(entry->row, lv_color_hex(0x161D2B), 0);
            lv_obj_set_style_border_color(entry->row, lv_color_hex(0x2A3347), 0);
            lv_obj_set_style_border_width(entry->row, 1, 0);
            lv_obj_set_style_radius(entry->row, 8, 0);
            lv_obj_set_style_pad_all(entry->row, 8, 0);

            entry->icon = lv_label_create(entry->row);
            lv_obj_align(entry->icon, LV_ALIGN_LEFT_MID, 0, 0);

            entry->text = lv_label_create(entry->row);
            lv_obj_align(entry->text, LV_ALIGN_LEFT_MID, 26, 0);
            lv_obj_set_style_text_color(entry->text, lv_color_hex(0xDFE7F5), 0);
        }

        if (s_connections_empty_label != NULL) {
            lv_obj_add_flag(s_connections_empty_label, LV_OBJ_FLAG_HIDDEN);
        }
        return (int)i;
    }

    return -1;
}

static void refresh_connection_rows(uint32_t now_ms) {
    bool any = false;
    for (size_t i = 0; i < MONITOR_MAX_CONNECTION_ROWS; ++i) {
        if (!s_connection_rows[i].used) {
            continue;
        }
        any = true;
        update_connection_row_visual(&s_connection_rows[i], now_ms);
    }

    if (s_connections_empty_label != NULL) {
        if (any) {
            lv_obj_add_flag(s_connections_empty_label, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_clear_flag(s_connections_empty_label, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void refresh_event_log_label(void) {
    if (s_event_log_label == NULL) {
        return;
    }

    if (s_event_line_count == 0U) {
        lv_label_set_text(s_event_log_label, "No CAN events yet.\nHeartbeats are filtered out.");
        return;
    }

    s_event_log_text[0] = '\0';
    size_t offset = 0U;
    for (uint8_t i = 0U; i < s_event_line_count; ++i) {
        const size_t line_len = strnlen(s_event_lines[i], MONITOR_EVENT_LINE_CHARS);
        if ((offset + line_len + 2U) >= sizeof(s_event_log_text)) {
            break;
        }
        memcpy(&s_event_log_text[offset], s_event_lines[i], line_len);
        offset += line_len;
        s_event_log_text[offset++] = '\n';
    }
    if (offset > 0U) {
        s_event_log_text[offset - 1U] = '\0';
    } else {
        s_event_log_text[0] = '\0';
    }

    lv_label_set_text(s_event_log_label, s_event_log_text);
}

static void push_event_line(const char *line) {
    if (line == NULL || line[0] == '\0') {
        return;
    }

    if (MONITOR_EVENT_MAX_LINES > 1U) {
        memmove(s_event_lines[1], s_event_lines[0],
                (MONITOR_EVENT_MAX_LINES - 1U) * MONITOR_EVENT_LINE_CHARS);
    }
    (void)snprintf(s_event_lines[0], MONITOR_EVENT_LINE_CHARS, "%s", line);
    if (s_event_line_count < MONITOR_EVENT_MAX_LINES) {
        s_event_line_count++;
    }
    refresh_event_log_label();
}

static const char *event_name_from_code(uint8_t code) {
    switch (code) {
    case SCP_EVENT_SWITCH_CHANGED:
        return "switch_changed";
    case SCP_EVENT_CONNECTION_DETECTED:
        return "connection_detected";
    case SCP_EVENT_CONNECTION_LOST:
        return "connection_lost";
    default:
        return "event_unknown";
    }
}

static bool decode_module_id_from_msg_id(uint32_t msg_id, uint8_t *module_id_out) {
    if (msg_id >= SCP_MSG_HEARTBEAT_BASE && msg_id < (SCP_MSG_HEARTBEAT_BASE + MONITOR_HEARTBEAT_WINDOW)) {
        if (module_id_out != NULL) {
            *module_id_out = (uint8_t)(msg_id - SCP_MSG_HEARTBEAT_BASE);
        }
        return true;
    }

    if (msg_id >= SCP_MSG_EVENT_BASE && msg_id < (SCP_MSG_EVENT_BASE + MONITOR_HEARTBEAT_WINDOW)) {
        if (module_id_out != NULL) {
            *module_id_out = (uint8_t)(msg_id - SCP_MSG_EVENT_BASE);
        }
        return true;
    }

    if (msg_id >= SCP_MSG_FAULT_BASE && msg_id < SCP_MSG_HEARTBEAT_BASE) {
        if (module_id_out != NULL) {
            *module_id_out = (uint8_t)(msg_id - SCP_MSG_FAULT_BASE);
        }
        return true;
    }

    return false;
}

static bool is_heartbeat_msg_id(uint32_t msg_id) {
    return msg_id >= SCP_MSG_HEARTBEAT_BASE
           && msg_id < (SCP_MSG_HEARTBEAT_BASE + MONITOR_HEARTBEAT_WINDOW);
}

static bool is_event_msg_id(uint32_t msg_id) {
    return msg_id >= SCP_MSG_EVENT_BASE
           && msg_id < (SCP_MSG_EVENT_BASE + MONITOR_HEARTBEAT_WINDOW);
}

static void format_can_event_line(const struct can2040_msg *msg, uint32_t uptime_ms, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U || msg == NULL) {
        return;
    }

    uint8_t module_id = 0U;
    const bool has_module_id = decode_module_id_from_msg_id(msg->id, &module_id);

    if (is_event_msg_id(msg->id) && msg->dlc >= 3U) {
        const char *event_name = event_name_from_code(msg->data[2]);
        (void)snprintf(out,
                       out_len,
                       "[%8lu ms] M%u %s",
                       (unsigned long)uptime_ms,
                       (unsigned int)module_id,
                       event_name);
        return;
    }

    (void)snprintf(out,
                   out_len,
                   "[%8lu ms] %sID 0x%03lx DLC%u",
                   (unsigned long)uptime_ms,
                   has_module_id ? "M" : "",
                   (unsigned long)msg->id,
                   (unsigned int)msg->dlc);
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

static void end_calibration_session(const char *status_text) {
    s_mode = MONITOR_MODE_UI;
    if (s_root_tabs != NULL) {
        lv_obj_clear_flag(s_root_tabs, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_calibration_root != NULL) {
        lv_obj_add_flag(s_calibration_root, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_settings_status_label != NULL && status_text != NULL) {
        lv_label_set_text(s_settings_status_label, status_text);
    }
}

static void finish_calibration(void) {
    if (compute_transform()) {
        printf("touch calibration complete\n");
        printf("x = %.6f * raw_x + %.6f * raw_y + %.2f\n",
               (double)s_transform.ax,
               (double)s_transform.bx,
               (double)s_transform.cx);
        printf("y = %.6f * raw_x + %.6f * raw_y + %.2f\n",
               (double)s_transform.ay,
               (double)s_transform.by,
               (double)s_transform.cy);
        if (save_calibration_to_flash()) {
            end_calibration_session("Touch calibration: saved to flash");
        } else {
            printf("touch calibration save failed\n");
            end_calibration_session("Touch calibration active (save failed)");
        }
        return;
    }

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

static void start_calibration_session(void) {
    s_mode = MONITOR_MODE_CALIBRATION;
    s_current_point = 0U;
    s_touch_capture_active = false;
    s_capture_sum_x = 0U;
    s_capture_sum_y = 0U;
    s_capture_samples = 0U;

    if (s_root_tabs != NULL) {
        lv_obj_add_flag(s_root_tabs, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_calibration_root != NULL) {
        lv_obj_clear_flag(s_calibration_root, LV_OBJ_FLAG_HIDDEN);
    }

    show_calibration_prompt();
    lv_label_set_text(s_status_label, "Tap the marker to capture raw touch values.");
    lv_obj_clear_flag(s_marker, LV_OBJ_FLAG_HIDDEN);
    move_marker_to_point(s_current_point);
}

static void on_start_calibration_clicked(lv_event_t *event) {
    (void)event;
    start_calibration_session();
}

static void set_brightness_value_label(uint8_t brightness_percent) {
    if (s_brightness_value_label == NULL) {
        return;
    }

    char brightness_text[12];
    (void)snprintf(brightness_text, sizeof(brightness_text), "%u%%", (unsigned int)brightness_percent);
    lv_label_set_text(s_brightness_value_label, brightness_text);
}

static void on_brightness_slider_changed(lv_event_t *event) {
    lv_obj_t *slider = lv_event_get_target(event);
    if (slider == NULL) {
        return;
    }

    const int32_t value = lv_slider_get_value(slider);
    const uint8_t brightness_percent = value < 0 ? 0U : (uint8_t)value;
    touch_calibrator_display_spi_set_backlight_percent(brightness_percent);
    set_brightness_value_label(brightness_percent);
}

static void monitor_touch_read_cb(lv_indev_drv_t *indev_drv, lv_indev_data_t *data) {
    (void)indev_drv;

    if (s_mode == MONITOR_MODE_CALIBRATION) {
        data->state = LV_INDEV_STATE_REL;
        data->point = s_last_touch_point;
        data->continue_reading = false;
        return;
    }

    uint16_t raw_x;
    uint16_t raw_y;
    if (!touch_calibrator_display_spi_touch_read_raw(&raw_x, &raw_y)) {
        data->state = LV_INDEV_STATE_REL;
        data->point = s_last_touch_point;
        data->continue_reading = false;
        return;
    }

    map_raw_to_screen(raw_x, raw_y, &s_last_touch_point);
    data->point = s_last_touch_point;
    data->state = LV_INDEV_STATE_PR;
    data->continue_reading = false;
}

static void build_ui(void) {
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x090C14), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);

    s_root_tabs = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 38);
    lv_obj_set_size(s_root_tabs, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_root_tabs, lv_color_hex(0x0D111B), 0);
    lv_obj_set_style_bg_opa(s_root_tabs, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root_tabs, 0, 0);
    lv_obj_t *tab_content = lv_tabview_get_content(s_root_tabs);
    lv_obj_clear_flag(tab_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tab_content, LV_DIR_NONE);
    lv_obj_set_style_anim_time(tab_content, 0, 0);

    lv_obj_t *tab_connections = lv_tabview_add_tab(s_root_tabs, "Connections");
    lv_obj_t *tab_events = lv_tabview_add_tab(s_root_tabs, "Event Log");
    lv_obj_t *tab_settings = lv_tabview_add_tab(s_root_tabs, "Settings");
    lv_obj_clear_flag(tab_connections, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tab_connections, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(tab_connections, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(tab_events, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tab_events, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(tab_events, LV_SCROLLBAR_MODE_OFF);
    lv_obj_clear_flag(tab_settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tab_settings, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(tab_settings, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *conn_title = lv_label_create(tab_connections);
    lv_label_set_text(conn_title, "CAN Modules");
    lv_obj_align(conn_title, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_text_color(conn_title, lv_color_hex(0x9EC4FF), 0);

    s_connections_list = lv_obj_create(tab_connections);
    lv_obj_set_size(s_connections_list, lv_pct(100), lv_pct(100));
    lv_obj_align(s_connections_list, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_obj_set_style_bg_color(s_connections_list, lv_color_hex(0x101522), 0);
    lv_obj_set_style_border_color(s_connections_list, lv_color_hex(0x263248), 0);
    lv_obj_set_style_border_width(s_connections_list, 1, 0);
    lv_obj_set_style_pad_all(s_connections_list, 8, 0);
    lv_obj_set_layout(s_connections_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_connections_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_connections_list, 8, 0);

    s_connections_empty_label = lv_label_create(s_connections_list);
    lv_label_set_text(s_connections_empty_label, "No modules discovered yet.");
    lv_obj_set_style_text_color(s_connections_empty_label, lv_color_hex(0x96A3B8), 0);

    s_event_log_label = lv_label_create(tab_events);
    lv_obj_set_width(s_event_log_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 18);
    lv_obj_align(s_event_log_label, LV_ALIGN_TOP_LEFT, 8, 10);
    lv_label_set_long_mode(s_event_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_event_log_label, lv_color_hex(0xD8E2F0), 0);
    refresh_event_log_label();

    lv_obj_t *calibrate_btn = lv_btn_create(tab_settings);
    lv_obj_set_size(calibrate_btn, 250, 46);
    lv_obj_align(calibrate_btn, LV_ALIGN_TOP_LEFT, 8, 16);
    lv_obj_set_style_bg_color(calibrate_btn, lv_color_hex(0x275DB3), 0);
    lv_obj_set_style_bg_color(calibrate_btn, lv_color_hex(0x306FD1), LV_STATE_PRESSED);
    lv_obj_add_event_cb(calibrate_btn, on_start_calibration_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cal_btn_label = lv_label_create(calibrate_btn);
    lv_label_set_text(cal_btn_label, "Start Touch Calibration");
    lv_obj_center(cal_btn_label);

    s_settings_status_label = lv_label_create(tab_settings);
    lv_obj_set_width(s_settings_status_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 20);
    lv_obj_align(s_settings_status_label, LV_ALIGN_TOP_LEFT, 8, 76);
    lv_label_set_long_mode(s_settings_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_settings_status_label, lv_color_hex(0xB9C8DD), 0);
    lv_label_set_text(s_settings_status_label, "Touch calibration: default mapping");

    lv_obj_t *brightness_title_label = lv_label_create(tab_settings);
    lv_label_set_text(brightness_title_label, "Brightness");
    lv_obj_align(brightness_title_label, LV_ALIGN_TOP_LEFT, 8, 114);
    lv_obj_set_style_text_color(brightness_title_label, lv_color_hex(0x9DB0C8), 0);

    lv_obj_t *brightness_slider = lv_slider_create(tab_settings);
    lv_obj_set_size(brightness_slider, TOUCH_CALIBRATOR_DISP_HOR_RES - 66, 18);
    lv_obj_align(brightness_slider, LV_ALIGN_TOP_LEFT, 8, 134);
    lv_slider_set_range(brightness_slider, 5, 100);
    uint8_t brightness_percent = touch_calibrator_display_spi_get_backlight_percent();
    if (brightness_percent < 5U) {
        brightness_percent = 5U;
        touch_calibrator_display_spi_set_backlight_percent(brightness_percent);
    }
    lv_slider_set_value(brightness_slider, brightness_percent, LV_ANIM_OFF);
    lv_obj_add_event_cb(brightness_slider, on_brightness_slider_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_brightness_value_label = lv_label_create(tab_settings);
    lv_obj_align(s_brightness_value_label, LV_ALIGN_TOP_RIGHT, -8, 132);
    lv_obj_set_style_text_color(s_brightness_value_label, lv_color_hex(0xB9C8DD), 0);
    set_brightness_value_label(brightness_percent);

    lv_obj_t *flash_label = lv_label_create(tab_settings);
    lv_obj_set_width(flash_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 20);
    lv_obj_align(flash_label, LV_ALIGN_TOP_LEFT, 8, 160);
    lv_label_set_long_mode(flash_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(flash_label, lv_color_hex(0x9DB0C8), 0);
    const uint32_t flash_total_kib = PICO_FLASH_SIZE_BYTES / 1024U;
    const uint32_t flash_used_bytes = (uint32_t)((uintptr_t)&__flash_binary_end - XIP_BASE);
    const uint32_t flash_free_kib = (PICO_FLASH_SIZE_BYTES > flash_used_bytes)
                                    ? ((PICO_FLASH_SIZE_BYTES - flash_used_bytes) / 1024U)
                                    : 0U;
    char flash_text[96];
    (void)snprintf(flash_text,
                   sizeof(flash_text),
                   "Flash: %lu KiB total, ~%lu KiB free",
                   (unsigned long)flash_total_kib,
                   (unsigned long)flash_free_kib);
    lv_label_set_text(flash_label, flash_text);

    lv_obj_t *build_label = lv_label_create(tab_settings);
    lv_obj_set_width(build_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 20);
    lv_obj_align(build_label, LV_ALIGN_BOTTOM_LEFT, 8, -12);
    lv_label_set_long_mode(build_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(build_label, lv_color_hex(0x8493AA), 0);

    char build_text[96];
    (void)snprintf(build_text, sizeof(build_text), "Firmware build: %s %s", __DATE__, __TIME__);
    lv_label_set_text(build_label, build_text);
}

static void build_calibration_view(void) {
    s_calibration_root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_calibration_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_calibration_root, lv_color_hex(0x0A0E19), 0);
    lv_obj_set_style_bg_opa(s_calibration_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_calibration_root, 0, 0);
    lv_obj_add_flag(s_calibration_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title_label = lv_label_create(s_calibration_root);
    lv_label_set_text(title_label, "Touch Calibration");
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x6AE4FF), 0);

    s_instruction_label = lv_label_create(s_calibration_root);
    lv_obj_set_width(s_instruction_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 20);
    lv_obj_align(s_instruction_label, LV_ALIGN_TOP_LEFT, 10, 40);
    lv_label_set_long_mode(s_instruction_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_instruction_label, lv_color_hex(0xE5EDF5), 0);

    s_status_label = lv_label_create(s_calibration_root);
    lv_obj_set_width(s_status_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 20);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_LEFT, 10, -8);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xA6C2D9), 0);

    s_marker = lv_obj_create(s_calibration_root);
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
}

void touch_calibrator_display_init(const touch_calibrator_display_spi_pins_t *pins) {
    if (pins == NULL) {
        return;
    }

    memset(&s_transform, 0, sizeof(s_transform));
    memset(&s_connection_rows, 0, sizeof(s_connection_rows));
    memset(&s_event_lines, 0, sizeof(s_event_lines));
    s_event_line_count = 0U;
    s_mode = MONITOR_MODE_UI;
    s_last_touch_point = (lv_point_t){0, 0};
    s_encoder_available = false;
    s_encoder_step_delta = 0;
    s_encoder_button_click_pending = false;

    s_points[0].screen = (lv_point_t){30, 30};
    s_points[1].screen = (lv_point_t){TOUCH_CALIBRATOR_DISP_HOR_RES - 31, 30};
    s_points[2].screen = (lv_point_t){TOUCH_CALIBRATOR_DISP_HOR_RES - 31, TOUCH_CALIBRATOR_DISP_VER_RES - 31};
    s_points[3].screen = (lv_point_t){30, TOUCH_CALIBRATOR_DISP_VER_RES - 31};
    s_points[4].screen = (lv_point_t){TOUCH_CALIBRATOR_DISP_HOR_RES / 2, TOUCH_CALIBRATOR_DISP_VER_RES / 2};

    touch_calibrator_display_spi_init(pins);
    monitor_encoder_init(pins, to_ms_since_boot(get_absolute_time()));
    lv_init();
    ili9488_init_panel();
    if (ili9488_draw_splash_from_flash()) {
        wait_for_splash_dismiss(5000U);
    } else {
        ili9488_boot_test_pattern();
    }

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t draw_buf_1[TOUCH_CALIBRATOR_DISP_HOR_RES * TOUCH_CALIBRATOR_DRAW_BUF_LINES];
    lv_disp_draw_buf_init(&draw_buf,
                          draw_buf_1,
                          NULL,
                          TOUCH_CALIBRATOR_DISP_HOR_RES * TOUCH_CALIBRATOR_DRAW_BUF_LINES);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = TOUCH_CALIBRATOR_DISP_HOR_RES;
    disp_drv.ver_res = TOUCH_CALIBRATOR_DISP_VER_RES;
    disp_drv.flush_cb = ili9488_flush;
    disp_drv.draw_buf = &draw_buf;
    (void)lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = monitor_touch_read_cb;
    (void)lv_indev_drv_register(&indev_drv);

    build_ui();
    build_calibration_view();

    const bool calibration_loaded = load_saved_calibration();
    if (calibration_loaded) {
        printf("touch calibration loaded from flash\n");
        if (s_settings_status_label != NULL) {
            lv_label_set_text(s_settings_status_label, "Touch calibration: loaded from flash");
        }
    } else {
        if (s_settings_status_label != NULL) {
            lv_label_set_text(s_settings_status_label, "Touch calibration: required (new orientation)");
        }
        start_calibration_session();
    }
}

void touch_calibrator_display_tick(uint32_t elapsed_ms) {
    lv_tick_inc(elapsed_ms);
}

void touch_calibrator_display_task_handler(void) {
    const uint32_t now_ms = to_ms_since_boot(get_absolute_time());
    monitor_encoder_sample(now_ms);
    monitor_encoder_apply_tab_navigation();

    if (s_mode == MONITOR_MODE_CALIBRATION) {
        update_calibration();
    } else {
        refresh_connection_rows(now_ms);
    }

    (void)lv_timer_handler();
}

void touch_calibrator_display_handle_can_message(const struct can2040_msg *msg, uint32_t uptime_ms) {
    if (msg == NULL) {
        return;
    }

    uint8_t module_id = 0U;
    if (decode_module_id_from_msg_id(msg->id, &module_id)) {
        const int idx = ensure_connection_row(module_id);
        if (idx >= 0) {
            if (is_heartbeat_msg_id(msg->id)) {
                s_connection_rows[idx].heartbeat_seen = true;
                s_connection_rows[idx].last_heartbeat_ms = uptime_ms;
            }
        }
    }

    if (is_heartbeat_msg_id(msg->id)) {
        return;
    }

    char line[MONITOR_EVENT_LINE_CHARS];
    format_can_event_line(msg, uptime_ms, line, sizeof(line));
    push_event_line(line);
}
