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
#include "scp/module_ids.h"
#include "scp/module_runtime.h"
#include "scp/protocol.h"

extern char __flash_binary_end;
extern char __end__;
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
#define ILI9488_INVOFF 0x20
#define ILI9488_INVON 0x21
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
#define MONITOR_EVENT_MAX_LINES 42U
#define MONITOR_EVENT_LINE_CHARS 96U
#define MONITOR_EVENT_TEXT_CHARS ((MONITOR_EVENT_MAX_LINES * MONITOR_EVENT_LINE_CHARS) + MONITOR_EVENT_MAX_LINES + 1U)
#define MONITOR_TAB_COUNT 3U
#define MONITOR_TAB_CONNECTIONS_INDEX 0U
#define MONITOR_TAB_SETTINGS_INDEX 2U
#define MONITOR_SETTINGS_FOCUSABLE_COUNT 5U
#define MONITOR_ENCODER_SLIDER_STEP 1
#define MONITOR_ENCODER_BUTTON_FALLBACK_PIN 17U

#define MONITOR_HEARTBEAT_WINDOW 0x80U
#define MONITOR_HEARTBEAT_TIMEOUT_MS (SCP_HEARTBEAT_PERIOD * 3U)
#define MONITOR_ENCODER_DEBOUNCE_MS 8U
#define MONITOR_ENCODER_TRANSITIONS_PER_STEP 2

#define TOUCH_DEFAULT_RAW_MIN 200U
#define TOUCH_DEFAULT_RAW_MAX 3800U

#define MONITOR_STARTUP_DIAG_PATTERN 0
#define MONITOR_STARTUP_DIAG_STEP_MS 1500U
#define MONITOR_LCD_USE_BGR_ORDER 0
#define MONITOR_LCD_USE_CUSTOM_TUNING 0
/*
 * Some ILI9488-compatible panels appear to interpret inversion polarity opposite
 * to datasheet naming. For this panel variant, INVON renders expected colors.
 */
#define MONITOR_LCD_NORMAL_INVERSION_CMD ILI9488_INVON

#define MONITOR_UI_BG_COLOR 0xF4F7FC
#define MONITOR_UI_SURFACE_COLOR 0xFFFFFF
#define MONITOR_UI_SURFACE_ALT_COLOR 0xF7FAFE
#define MONITOR_UI_BORDER_COLOR 0xCCD8E6
#define MONITOR_UI_TITLE_COLOR 0x225EA8
#define MONITOR_UI_TEXT_COLOR 0x1E2A36
#define MONITOR_UI_TEXT_MUTED_COLOR 0x5A6F86

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

typedef struct {
    uint8_t module_id;
    const char *friendly_name;
} monitor_module_name_t;

static monitor_mode_t s_mode;

static lv_obj_t *s_root_tabs;
static lv_obj_t *s_connections_list;
static lv_obj_t *s_connections_empty_label;
static lv_obj_t *s_event_log_label;
static lv_obj_t *s_settings_status_label;
static lv_obj_t *s_brightness_value_label;
static lv_obj_t *s_unit_status_label;
static lv_obj_t *s_start_calibration_btn;
static lv_obj_t *s_brightness_slider;
static lv_obj_t *s_unit_torr_btn;
static lv_obj_t *s_unit_bar_btn;
static lv_obj_t *s_unit_voltage_btn;

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
static bool s_encoder_button_fallback_available;
static uint8_t s_encoder_button_fallback_pin;
static uint8_t s_encoder_prev_ab_state;
static int8_t s_encoder_transition_accumulator;
static int16_t s_encoder_step_delta;
static bool s_encoder_button_raw_pressed;
static bool s_encoder_button_debounced_pressed;
static uint32_t s_encoder_button_last_edge_ms;
static bool s_encoder_button_click_pending;
static bool s_encoder_button_active_low;
static bool s_encoder_tab_entered;
static bool s_encoder_slider_adjust_mode;
static uint8_t s_settings_focus_index;
static uint8_t s_selected_pressure_unit;
static bool s_pressure_unit_command_pending;

static const monitor_module_name_t s_module_names[] = {
    {SCP_MODULE_ID_ION_GAUGE, "Ion Gauge"},
    {SCP_MODULE_ID_PIRANI, "Pirani Gauge"},
    {SCP_MODULE_ID_ROUGHING_PUMP, "Roughing Pump"},
    {SCP_MODULE_ID_TURBO_PUMP, "Turbo Pump"},
    {SCP_MODULE_ID_MONITOR, "Monitor"},
    {SCP_MODULE_ID_USB_CAN_BRIDGE, "USB-CAN Bridge"},
};

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
static uint8_t monitor_encoder_get_active_tab_index(void);
static bool monitor_encoder_settings_tab_active(void);
static lv_obj_t *monitor_settings_get_focusable(uint8_t index);
static void monitor_settings_set_focus_style(lv_obj_t *obj, bool focused);
static void monitor_settings_clear_focus(void);
static void monitor_settings_set_focus_index(uint8_t index);
static void monitor_settings_step_focus(int8_t direction);
static void monitor_settings_activate_focused(void);
static void monitor_settings_adjust_brightness(int16_t steps);
static void monitor_encoder_exit_tab_content_mode(void);

static bool ili9488_draw_splash_from_flash(void);

static void wait_for_splash_dismiss(uint32_t max_wait_ms);
static void update_pressure_unit_controls(void);
static void request_pressure_unit_change(uint8_t unit);
static void on_pressure_unit_button_clicked(lv_event_t *event);

static const char *friendly_module_name(uint8_t module_id);
static bool is_connection_row_online(const monitor_connection_row_t *entry, uint32_t now_ms);
static void clear_connection_row(monitor_connection_row_t *entry);

static void ili9488_fill_color565(uint16_t color) {
    static uint8_t row_buf[TOUCH_CALIBRATOR_DISP_HOR_RES * 3];
    const uint8_t r = (uint8_t) ((((color >> 11) & 0x1FU) * 255U) / 31U);
    const uint8_t g = (uint8_t) ((((color >> 5) & 0x3FU) * 255U) / 63U);
    const uint8_t b = (uint8_t) (((color & 0x1FU) * 255U) / 31U);

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

#if MONITOR_STARTUP_DIAG_PATTERN
static void ili9488_send_cmd(uint8_t cmd) {
    touch_calibrator_display_spi_set_lcd_cs(0);
    ili9488_write(ILI9488_CMD_MODE, cmd);
    touch_calibrator_display_spi_set_lcd_cs(1);
}

static void ili9488_startup_diagnostic_pattern(void) {
    static const uint16_t green_steps[] = {
        0x01E0U, /* ~25% green */
        0x03E0U, /* ~50% green */
        0x05E0U, /* ~75% green */
        0x07E0U, /* 100% green */
    };

    printf("monitor: startup green ramp diagnostic (%u ms/step)\n",
           (unsigned int) MONITOR_STARTUP_DIAG_STEP_MS);

    ili9488_send_cmd(MONITOR_LCD_NORMAL_INVERSION_CMD);
    for (size_t i = 0; i < (sizeof(green_steps) / sizeof(green_steps[0])); ++i) {
        ili9488_fill_color565(green_steps[i]);
        sleep_ms(MONITOR_STARTUP_DIAG_STEP_MS);
    }
}
#endif

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
            row_buf[x * 3U] = (uint8_t) ((((c >> 11) & 0x1FU) * 255U) / 31U);
            row_buf[(x * 3U) + 1U] = (uint8_t) ((((c >> 5) & 0x3FU) * 255U) / 63U);
            row_buf[(x * 3U) + 2U] = (uint8_t) (((c & 0x1FU) * 255U) / 31U);
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

    while ((uint32_t) (to_ms_since_boot(get_absolute_time()) - start_ms) < max_wait_ms) {
        if (touch_calibrator_display_spi_touch_read_raw(&raw_x, &raw_y)) {
            const uint32_t release_start_ms = to_ms_since_boot(get_absolute_time());
            while (touch_calibrator_display_spi_touch_read_raw(&raw_x, &raw_y)) {
                if ((uint32_t) (to_ms_since_boot(get_absolute_time()) - release_start_ms) >= 300U) {
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
    data[0] = (uint8_t) ((x1 >> 8) & 0xFF);
    data[1] = (uint8_t) (x1 & 0xFF);
    data[2] = (uint8_t) ((x2 >> 8) & 0xFF);
    data[3] = (uint8_t) (x2 & 0xFF);
    ili9488_write_array(ILI9488_DATA_MODE, data, sizeof(data));

    ili9488_write(ILI9488_CMD_MODE, ILI9488_PASET);
    data[0] = (uint8_t) ((y1 >> 8) & 0xFF);
    data[1] = (uint8_t) (y1 & 0xFF);
    data[2] = (uint8_t) ((y2 >> 8) & 0xFF);
    data[3] = (uint8_t) (y2 & 0xFF);
    ili9488_write_array(ILI9488_DATA_MODE, data, sizeof(data));

    ili9488_write(ILI9488_CMD_MODE, ILI9488_RAMWR);
}

static void ili9488_init_panel(void) {
#if MONITOR_LCD_USE_CUSTOM_TUNING
    static const uint8_t gamma_pos[] = {0x00, 0x04, 0x0E, 0x08, 0x17, 0x0A, 0x40, 0x79, 0x4D, 0x07, 0x0E, 0x0A, 0x1A, 0x1D, 0x0F};
    static const uint8_t gamma_neg[] = {0x00, 0x1B, 0x1F, 0x02, 0x10, 0x05, 0x32, 0x34, 0x43, 0x02, 0x0A, 0x09, 0x32, 0x36, 0x0F};
    static const uint8_t power_c0[] = {0x17, 0x15};
    static const uint8_t power_c5[] = {0x00, 0x12, 0x80};
    static const uint8_t power_f7[] = {0xA9, 0x51, 0x2C, 0x82};
    static const uint8_t disp_fn[] = {0x02, 0x02};
#endif

    touch_calibrator_display_spi_set_lcd_cs(1);
    touch_calibrator_display_spi_set_lcd_reset(0);
    sleep_ms(20);
    touch_calibrator_display_spi_set_lcd_reset(1);
    sleep_ms(120);
    touch_calibrator_display_spi_set_lcd_cs(0);

    ili9488_write(ILI9488_CMD_MODE, ILI9488_SWRESET);
    sleep_ms(120);

#if MONITOR_LCD_USE_CUSTOM_TUNING
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
#endif
    /* Rotate panel output 180 degrees from the previous orientation (portrait, upside-down). */
    ili9488_write(ILI9488_CMD_MODE, ILI9488_MADCTL);
#if MONITOR_LCD_USE_BGR_ORDER
    ili9488_write(ILI9488_DATA_MODE, ILI9488_MADCTL_MX | ILI9488_MADCTL_BGR);
#else
    ili9488_write(ILI9488_DATA_MODE, ILI9488_MADCTL_MX);
#endif
    ili9488_write(ILI9488_CMD_MODE, ILI9488_PIXFMT);
    ili9488_write(ILI9488_DATA_MODE, 0x66);
#if MONITOR_LCD_USE_CUSTOM_TUNING
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
#endif
    ili9488_write(ILI9488_CMD_MODE, ILI9488_SLPOUT);
    sleep_ms(120);
    /* Apply panel-variant "normal color" inversion command. */
    ili9488_write(ILI9488_CMD_MODE, MONITOR_LCD_NORMAL_INVERSION_CMD);
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
            line_buf[(size_t) x * 3U] = c32.ch.red;
            line_buf[((size_t) x * 3U) + 1U] = c32.ch.green;
            line_buf[((size_t) x * 3U) + 2U] = c32.ch.blue;
        }
        ili9488_write_array(ILI9488_DATA_MODE, line_buf, (size_t) act_width * 3U);
        row_ptr += src_width;
    }
    touch_calibrator_display_spi_set_lcd_cs(1);

    lv_disp_flush_ready(disp_drv);
}

static bool solve_3x3(float a[3][3], float b[3], float out[3]) {
    for (uint8_t col = 0; col < 3U; ++col) {
        uint8_t pivot = col;
        float pivot_abs = a[pivot][col] >= 0.0f ? a[pivot][col] : -a[pivot][col];

        for (uint8_t row = (uint8_t) (col + 1U); row < 3U; ++row) {
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
        const float u = (float) s_points[i].raw_x;
        const float v = (float) s_points[i].raw_y;
        const float x = (float) s_points[i].screen.x;
        const float y = (float) s_points[i].screen.y;

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

    const float mapped_x = (s_transform.ax * (float) raw_x) + (s_transform.bx * (float) raw_y) + s_transform.cx;
    const float mapped_y = (s_transform.ay * (float) raw_x) + (s_transform.by * (float) raw_y) + s_transform.cy;

    float clamped_x = mapped_x;
    float clamped_y = mapped_y;
    if (clamped_x < 0.0f) {
        clamped_x = 0.0f;
    }
    if (clamped_x > (float) (TOUCH_CALIBRATOR_DISP_HOR_RES - 1)) {
        clamped_x = (float) (TOUCH_CALIBRATOR_DISP_HOR_RES - 1);
    }
    if (clamped_y < 0.0f) {
        clamped_y = 0.0f;
    }
    if (clamped_y > (float) (TOUCH_CALIBRATOR_DISP_VER_RES - 1)) {
        clamped_y = (float) (TOUCH_CALIBRATOR_DISP_VER_RES - 1);
    }

    out_point->x = (lv_coord_t) (clamped_x + 0.5f);
    out_point->y = (lv_coord_t) (clamped_y + 0.5f);
}

static lv_coord_t map_axis_linear(uint16_t raw, uint16_t raw_min, uint16_t raw_max, lv_coord_t screen_max) {
    if (raw <= raw_min) {
        return 0;
    }
    if (raw >= raw_max) {
        return screen_max;
    }

    const uint32_t span_raw = (uint32_t) raw_max - (uint32_t) raw_min;
    const uint32_t normalized = (uint32_t) raw - (uint32_t) raw_min;
    const uint32_t mapped = (normalized * (uint32_t) screen_max) / span_raw;
    return (lv_coord_t) mapped;
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
        crc ^= (uint32_t) data[i];
        for (uint8_t bit = 0; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t) -(int32_t) (crc & 1u);
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
            (const touch_calibration_flash_record_t *) (XIP_BASE + TOUCH_CAL_FLASH_OFFSET);

    if (record->magic != TOUCH_CAL_FLASH_MAGIC
        || record->version != TOUCH_CAL_FLASH_VERSION
        || record->length != sizeof(touch_calibration_flash_record_t)) {
        return false;
    }

    const uint32_t expected_crc = crc32_compute((const uint8_t *) record,
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
    record.crc32 = crc32_compute((const uint8_t *) &record,
                                 offsetof(touch_calibration_flash_record_t, crc32));

    uint8_t page_buf[FLASH_PAGE_SIZE];
    memset(page_buf, 0xFF, sizeof(page_buf));
    memcpy(page_buf, &record, sizeof(record));

    const uint32_t irq_state = save_and_disable_interrupts();
    flash_range_erase(TOUCH_CAL_FLASH_OFFSET, FLASH_SECTOR_SIZE);
    flash_range_program(TOUCH_CAL_FLASH_OFFSET, page_buf, FLASH_PAGE_SIZE);
    restore_interrupts(irq_state);

    const touch_calibration_flash_record_t *stored =
            (const touch_calibration_flash_record_t *) (XIP_BASE + TOUCH_CAL_FLASH_OFFSET);
    const uint32_t stored_crc = crc32_compute((const uint8_t *) stored,
                                              offsetof(touch_calibration_flash_record_t, crc32));

    return stored->magic == TOUCH_CAL_FLASH_MAGIC
           && stored->version == TOUCH_CAL_FLASH_VERSION
           && stored->length == sizeof(touch_calibration_flash_record_t)
           && stored->crc32 == stored_crc;
}

static uint8_t monitor_encoder_get_active_tab_index(void) {
    if (s_root_tabs == NULL) {
        return MONITOR_TAB_CONNECTIONS_INDEX;
    }

    const uint32_t active = lv_tabview_get_tab_act(s_root_tabs);
    if (active >= MONITOR_TAB_COUNT) {
        return MONITOR_TAB_CONNECTIONS_INDEX;
    }
    return (uint8_t) active;
}

static bool monitor_encoder_settings_tab_active(void) {
    return monitor_encoder_get_active_tab_index() == MONITOR_TAB_SETTINGS_INDEX;
}

static lv_obj_t *monitor_settings_get_focusable(uint8_t index) {
    switch (index) {
    case 0U:
        return s_start_calibration_btn;
    case 1U:
        return s_brightness_slider;
    case 2U:
        return s_unit_torr_btn;
    case 3U:
        return s_unit_bar_btn;
    case 4U:
        return s_unit_voltage_btn;
    default:
        return NULL;
    }
}

static void monitor_settings_set_focus_style(lv_obj_t *obj, bool focused) {
    if (obj == NULL) {
        return;
    }

    if (focused) {
        lv_obj_add_state(obj, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(obj, 2, 0);
        lv_obj_set_style_outline_color(obj, lv_color_hex(0x275DB3), 0);
        lv_obj_set_style_outline_pad(obj, 2, 0);
    } else {
        lv_obj_clear_state(obj, LV_STATE_FOCUSED);
        lv_obj_set_style_outline_width(obj, 0, 0);
    }
}

static void monitor_settings_clear_focus(void) {
    for (uint8_t i = 0; i < MONITOR_SETTINGS_FOCUSABLE_COUNT; ++i) {
        monitor_settings_set_focus_style(monitor_settings_get_focusable(i), false);
    }
}

static void monitor_settings_set_focus_index(uint8_t index) {
    monitor_settings_clear_focus();

    for (uint8_t offset = 0; offset < MONITOR_SETTINGS_FOCUSABLE_COUNT; ++offset) {
        const uint8_t candidate =
                (uint8_t) ((index + offset) % MONITOR_SETTINGS_FOCUSABLE_COUNT);
        lv_obj_t *focusable = monitor_settings_get_focusable(candidate);
        if (focusable == NULL) {
            continue;
        }

        s_settings_focus_index = candidate;
        monitor_settings_set_focus_style(focusable, true);
        return;
    }
}

static void monitor_settings_step_focus(int8_t direction) {
    if (direction == 0) {
        return;
    }

    int16_t next = (int16_t) s_settings_focus_index + (direction > 0 ? 1 : -1);
    if (next < 0) {
        next = (int16_t) (MONITOR_SETTINGS_FOCUSABLE_COUNT - 1U);
    } else if (next >= (int16_t) MONITOR_SETTINGS_FOCUSABLE_COUNT) {
        next = 0;
    }

    monitor_settings_set_focus_index((uint8_t) next);
}

static void monitor_settings_adjust_brightness(int16_t steps) {
    if (s_brightness_slider == NULL || steps == 0) {
        return;
    }

    const int32_t min = lv_slider_get_min_value(s_brightness_slider);
    const int32_t max = lv_slider_get_max_value(s_brightness_slider);
    int32_t value = lv_slider_get_value(s_brightness_slider);
    value += steps * MONITOR_ENCODER_SLIDER_STEP;
    if (value < min) {
        value = min;
    } else if (value > max) {
        value = max;
    }

    if (value != lv_slider_get_value(s_brightness_slider)) {
        lv_slider_set_value(s_brightness_slider, value, LV_ANIM_OFF);
        (void) lv_event_send(s_brightness_slider, LV_EVENT_VALUE_CHANGED, NULL);
    }
}

static void monitor_settings_activate_focused(void) {
    lv_obj_t *focus_target = monitor_settings_get_focusable(s_settings_focus_index);
    if (focus_target == NULL) {
        monitor_settings_set_focus_index(0U);
        focus_target = monitor_settings_get_focusable(s_settings_focus_index);
    }
    if (focus_target == NULL) {
        return;
    }

    if (focus_target == s_brightness_slider) {
        s_encoder_slider_adjust_mode = !s_encoder_slider_adjust_mode;
        return;
    }

    (void) lv_event_send(focus_target, LV_EVENT_CLICKED, NULL);
}

static void monitor_encoder_exit_tab_content_mode(void) {
    s_encoder_tab_entered = false;
    s_encoder_slider_adjust_mode = false;
    monitor_settings_clear_focus();
}

static void monitor_encoder_set_tab_relative(int8_t direction) {
    if (s_root_tabs == NULL || direction == 0) {
        return;
    }

    const int32_t current = (int32_t) lv_tabview_get_tab_act(s_root_tabs);
    int32_t target = current + (direction > 0 ? 1 : -1);
    if (target < 0) {
        target = 0;
    }
    if (target >= (int32_t) MONITOR_TAB_COUNT) {
        target = (int32_t) (MONITOR_TAB_COUNT - 1U);
    }

    if (target != current) {
        lv_tabview_set_act(s_root_tabs, (uint32_t) target, LV_ANIM_OFF);
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
    s_encoder_button_fallback_available = false;
    s_encoder_button_fallback_pin = MONITOR_ENCODER_BUTTON_FALLBACK_PIN;

    gpio_init(s_encoder_a_pin);
    gpio_set_dir(s_encoder_a_pin, GPIO_IN);
    gpio_pull_up(s_encoder_a_pin);

    gpio_init(s_encoder_b_pin);
    gpio_set_dir(s_encoder_b_pin, GPIO_IN);
    gpio_pull_up(s_encoder_b_pin);

    gpio_init(s_encoder_button_pin);
    gpio_set_dir(s_encoder_button_pin, GPIO_IN);
    gpio_pull_up(s_encoder_button_pin);

    if (s_encoder_button_fallback_pin <= SCP_PICO_GPIO_MAX
        && s_encoder_button_fallback_pin != s_encoder_a_pin
        && s_encoder_button_fallback_pin != s_encoder_b_pin
        && s_encoder_button_fallback_pin != s_encoder_button_pin) {
        gpio_init(s_encoder_button_fallback_pin);
        gpio_set_dir(s_encoder_button_fallback_pin, GPIO_IN);
        gpio_pull_up(s_encoder_button_fallback_pin);
        s_encoder_button_fallback_available = true;
    }

    s_encoder_prev_ab_state = (uint8_t) (((gpio_get(s_encoder_a_pin) ? 1U : 0U) << 1U)
                                         | (gpio_get(s_encoder_b_pin) ? 1U : 0U));
    s_encoder_transition_accumulator = 0;
    s_encoder_step_delta = 0;
    s_encoder_button_active_low = gpio_get(s_encoder_button_pin) != 0U;
    s_encoder_button_raw_pressed = s_encoder_button_active_low
                                           ? (gpio_get(s_encoder_button_pin) == 0U)
                                           : (gpio_get(s_encoder_button_pin) != 0U);
    s_encoder_button_debounced_pressed = s_encoder_button_raw_pressed;
    s_encoder_button_last_edge_ms = now_ms;
    s_encoder_button_click_pending = false;
    s_encoder_tab_entered = false;
    s_encoder_slider_adjust_mode = false;
    s_settings_focus_index = 0U;
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

    bool button_raw_pressed = s_encoder_button_active_low
                                      ? (gpio_get(s_encoder_button_pin) == 0U)
                                      : (gpio_get(s_encoder_button_pin) != 0U);
    if (s_encoder_button_fallback_available) {
        const bool fallback_pressed = gpio_get(s_encoder_button_fallback_pin) == 0U;
        button_raw_pressed = button_raw_pressed || fallback_pressed;
    }
    if (button_raw_pressed != s_encoder_button_raw_pressed) {
        s_encoder_button_raw_pressed = button_raw_pressed;
        s_encoder_button_last_edge_ms = now_ms;
    }

    const bool button_in_debounce_window =
            ((uint32_t) (now_ms - s_encoder_button_last_edge_ms) < MONITOR_ENCODER_DEBOUNCE_MS);
    const bool button_assumed_pressed = s_encoder_button_raw_pressed || s_encoder_button_debounced_pressed;

    const uint8_t curr_ab_state = (uint8_t) (((gpio_get(s_encoder_a_pin) ? 1U : 0U) << 1U)
                                             | (gpio_get(s_encoder_b_pin) ? 1U : 0U));
    if (button_assumed_pressed || button_in_debounce_window) {
        /* Ignore rotary jitter while pressing/releasing the encoder button. */
        s_encoder_prev_ab_state = curr_ab_state;
        s_encoder_transition_accumulator = 0;
    } else if (curr_ab_state != s_encoder_prev_ab_state) {
        const int8_t transition = transition_lut[(s_encoder_prev_ab_state << 2U) | curr_ab_state];
        if (transition != 0) {
            s_encoder_transition_accumulator += transition;
            if (s_encoder_transition_accumulator >= MONITOR_ENCODER_TRANSITIONS_PER_STEP) {
                s_encoder_transition_accumulator = 0;
                s_encoder_step_delta++;
            } else if (s_encoder_transition_accumulator <= -MONITOR_ENCODER_TRANSITIONS_PER_STEP) {
                s_encoder_transition_accumulator = 0;
                s_encoder_step_delta--;
            }
        }
        s_encoder_prev_ab_state = curr_ab_state;
    }

    if (button_in_debounce_window) {
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
        monitor_encoder_exit_tab_content_mode();
        return;
    }

    if (!s_encoder_tab_entered) {
        while (s_encoder_step_delta > 0) {
            monitor_encoder_set_tab_relative(1);
            s_encoder_step_delta--;
        }
        while (s_encoder_step_delta < 0) {
            monitor_encoder_set_tab_relative(-1);
            s_encoder_step_delta++;
        }

        if (s_encoder_button_click_pending) {
            s_encoder_tab_entered = true;
            s_encoder_slider_adjust_mode = false;
            if (monitor_encoder_settings_tab_active()) {
                monitor_settings_set_focus_index(s_settings_focus_index);
            }
            s_encoder_button_click_pending = false;
        }
        return;
    }

    if (!monitor_encoder_settings_tab_active()) {
        s_encoder_slider_adjust_mode = false;
        monitor_settings_clear_focus();

        if (s_encoder_step_delta != 0) {
            s_encoder_tab_entered = false;
            while (s_encoder_step_delta > 0) {
                monitor_encoder_set_tab_relative(1);
                s_encoder_step_delta--;
            }
            while (s_encoder_step_delta < 0) {
                monitor_encoder_set_tab_relative(-1);
                s_encoder_step_delta++;
            }
        }

        if (s_encoder_button_click_pending) {
            s_encoder_tab_entered = false;
            s_encoder_button_click_pending = false;
        }
        return;
    }

    if (s_encoder_slider_adjust_mode) {
        while (s_encoder_step_delta > 0) {
            monitor_settings_adjust_brightness(1);
            s_encoder_step_delta--;
        }
        while (s_encoder_step_delta < 0) {
            monitor_settings_adjust_brightness(-1);
            s_encoder_step_delta++;
        }

        if (s_encoder_button_click_pending) {
            s_encoder_slider_adjust_mode = false;
            s_encoder_button_click_pending = false;
        }
        return;
    }

    while (s_encoder_step_delta > 0) {
        monitor_settings_step_focus(1);
        s_encoder_step_delta--;
    }
    while (s_encoder_step_delta < 0) {
        monitor_settings_step_focus(-1);
        s_encoder_step_delta++;
    }

    if (s_encoder_button_click_pending) {
        monitor_settings_activate_focused();
        s_encoder_button_click_pending = false;
    }
}

static int find_connection_row(uint8_t module_id) {
    for (size_t i = 0; i < MONITOR_MAX_CONNECTION_ROWS; ++i) {
        if (s_connection_rows[i].used && s_connection_rows[i].module_id == module_id) {
            return (int) i;
        }
    }
    return -1;
}

static const char *friendly_module_name(uint8_t module_id) {
    for (size_t i = 0; i < (sizeof(s_module_names) / sizeof(s_module_names[0])); ++i) {
        if (s_module_names[i].module_id == module_id) {
            return s_module_names[i].friendly_name;
        }
    }
    return NULL;
}

static bool is_connection_row_online(const monitor_connection_row_t *entry, uint32_t now_ms) {
    return entry != NULL
           && entry->used
           && entry->heartbeat_seen
           && ((now_ms - entry->last_heartbeat_ms) <= MONITOR_HEARTBEAT_TIMEOUT_MS);
}

static void clear_connection_row(monitor_connection_row_t *entry) {
    if (entry == NULL || !entry->used) {
        return;
    }

    if (entry->row != NULL) {
        lv_obj_del(entry->row);
    }
    memset(entry, 0, sizeof(*entry));
}


static lv_color_t online_status_dot_color(uint8_t module_id) {
    switch (module_id) {
    case SCP_MODULE_ID_PIRANI:
        return lv_color_hex(~0xBF745E); /* orange */
    case SCP_MODULE_ID_ROUGHING_PUMP:
        return lv_color_hex(~0x22C55E); /* green */
    default:
        return lv_color_hex(~0x5BE37D); /* default online green */
    }
}

static void update_connection_row_visual(monitor_connection_row_t *entry, uint32_t now_ms) {
    if (entry == NULL || !entry->used || entry->icon == NULL || entry->text == NULL) {
        return;
    }
    (void) now_ms;

    lv_obj_set_style_bg_opa(entry->icon, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(entry->icon,
                              online_status_dot_color(entry->module_id),
                              0);

    const char *name = friendly_module_name(entry->module_id);
    char text[80];
    if (name != NULL) {
        (void) snprintf(text, sizeof(text), "%s", name);
    } else {
        (void) snprintf(text, sizeof(text), "Module %u", (unsigned int) entry->module_id);
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
            lv_obj_set_style_bg_color(entry->row, lv_color_hex(MONITOR_UI_SURFACE_ALT_COLOR), 0);
            lv_obj_set_style_border_color(entry->row, lv_color_hex(MONITOR_UI_BORDER_COLOR), 0);
            lv_obj_set_style_border_width(entry->row, 1, 0);
            lv_obj_set_style_radius(entry->row, 8, 0);
            lv_obj_set_style_pad_all(entry->row, 8, 0);

            entry->icon = lv_obj_create(entry->row);
            lv_obj_set_size(entry->icon, 12, 12);
            lv_obj_set_style_radius(entry->icon, LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_border_width(entry->icon, 0, 0);
            lv_obj_set_style_pad_all(entry->icon, 0, 0);
            lv_obj_clear_flag(entry->icon, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_align(entry->icon, LV_ALIGN_LEFT_MID, 0, 0);

            entry->text = lv_label_create(entry->row);
            lv_obj_align(entry->text, LV_ALIGN_LEFT_MID, 26, 0);
            lv_obj_set_style_text_color(entry->text, lv_color_hex(MONITOR_UI_TEXT_COLOR), 0);
        }

        if (s_connections_empty_label != NULL) {
            lv_obj_add_flag(s_connections_empty_label, LV_OBJ_FLAG_HIDDEN);
        }
        return (int) i;
    }

    return -1;
}

static void refresh_connection_rows(uint32_t now_ms) {
    bool any = false;
    for (size_t i = 0; i < MONITOR_MAX_CONNECTION_ROWS; ++i) {
        if (!s_connection_rows[i].used) {
            continue;
        }
        if (!is_connection_row_online(&s_connection_rows[i], now_ms)) {
            clear_connection_row(&s_connection_rows[i]);
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
    (void) snprintf(s_event_lines[0], MONITOR_EVENT_LINE_CHARS, "%s", line);
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
        case SCP_EVENT_PRESSURE_READING:
            return "pressure_reading";
        case SCP_EVENT_CURRENT_READING:
            return "current_reading";
        default:
            return "event_unknown";
    }
}

static bool decode_module_id_from_msg_id(uint32_t msg_id, uint8_t *module_id_out) {
    if (msg_id >= SCP_MSG_HEARTBEAT_BASE && msg_id < (SCP_MSG_HEARTBEAT_BASE + MONITOR_HEARTBEAT_WINDOW)) {
        if (module_id_out != NULL) {
            *module_id_out = (uint8_t) (msg_id - SCP_MSG_HEARTBEAT_BASE);
        }
        return true;
    }

    if (msg_id >= SCP_MSG_EVENT_BASE && msg_id < (SCP_MSG_EVENT_BASE + MONITOR_HEARTBEAT_WINDOW)) {
        if (module_id_out != NULL) {
            *module_id_out = (uint8_t) (msg_id - SCP_MSG_EVENT_BASE);
        }
        return true;
    }

    if (msg_id >= SCP_MSG_FAULT_BASE && msg_id < SCP_MSG_HEARTBEAT_BASE) {
        if (module_id_out != NULL) {
            *module_id_out = (uint8_t) (msg_id - SCP_MSG_FAULT_BASE);
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

static const char *module_abbreviation(uint8_t module_id) {
    switch (module_id) {
    case SCP_MODULE_ID_ION_GAUGE:
        return "IG";
    case SCP_MODULE_ID_PIRANI:
        return "PG";
    case SCP_MODULE_ID_ROUGHING_PUMP:
        return "RP";
    case SCP_MODULE_ID_TURBO_PUMP:
        return "TP";
    case SCP_MODULE_ID_USB_CAN_BRIDGE:
        return "CB";
    default:
        return NULL;
    }
}

static void format_uptime_hh_mm_ss(uint32_t uptime_ms, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U) {
        return;
    }

    const uint32_t total_seconds = uptime_ms / 1000U;
    const uint32_t hours = total_seconds / 3600U;
    const uint32_t minutes = (total_seconds / 60U) % 60U;
    const uint32_t seconds = total_seconds % 60U;
    char hour_field[16];
    const char first_sep = (hours == 0U) ? ' ' : ':';

    if (hours == 0U) {
        (void) snprintf(hour_field, sizeof(hour_field), "  ");
    } else {
        (void) snprintf(hour_field, sizeof(hour_field), "%2lu", (unsigned long) hours);
    }

    (void) snprintf(out,
                    out_len,
                    "%s%c%2lu:%02lu",
                    hour_field,
                    first_sep,
                    (unsigned long) minutes,
                    (unsigned long) seconds);
}

static void format_can_event_line(const struct can2040_msg *msg, uint32_t uptime_ms, char *out, size_t out_len) {
    if (out == NULL || out_len == 0U || msg == NULL) {
        return;
    }

    char timestamp[16];
    format_uptime_hh_mm_ss(uptime_ms, timestamp, sizeof(timestamp));

    uint8_t module_id = 0U;
    const bool has_module_id = decode_module_id_from_msg_id(msg->id, &module_id);
    const char *module_code = has_module_id ? module_abbreviation(module_id) : NULL;

    if (is_event_msg_id(msg->id) && msg->dlc >= 3U) {
        const char *event_name = event_name_from_code(msg->data[2]);
        if (module_code != NULL) {
            (void) snprintf(out,
                            out_len,
                            "%s %s %s",
                            timestamp,
                            module_code,
                            event_name);
        } else {
            (void) snprintf(out,
                            out_len,
                            "%s M%u %s",
                            timestamp,
                            (unsigned int) module_id,
                            event_name);
        }
        return;
    }

    if (module_code != NULL) {
        (void) snprintf(out,
                        out_len,
                        "%s %s ID 0x%03lx DLC%u",
                        timestamp,
                        module_code,
                        (unsigned long) msg->id,
                        (unsigned int) msg->dlc);
    } else if (has_module_id) {
        (void) snprintf(out,
                        out_len,
                        "%s M%u ID 0x%03lx DLC%u",
                        timestamp,
                        (unsigned int) module_id,
                        (unsigned long) msg->id,
                        (unsigned int) msg->dlc);
    } else {
        (void) snprintf(out,
                        out_len,
                        "%s ID 0x%03lx DLC%u",
                        timestamp,
                        (unsigned long) msg->id,
                        (unsigned int) msg->dlc);
    }
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
    (void) snprintf(instruction,
                    sizeof(instruction),
                    "Tap marker %u/%u, hold briefly, then release.",
                    (unsigned int) (s_current_point + 1U),
                    (unsigned int) TOUCH_CALIBRATION_POINT_COUNT);
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
               (double) s_transform.ax,
               (double) s_transform.bx,
               (double) s_transform.cx);
        printf("y = %.6f * raw_x + %.6f * raw_y + %.2f\n",
               (double) s_transform.ay,
               (double) s_transform.by,
               (double) s_transform.cy);
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
        (void) snprintf(status,
                        sizeof(status),
                        "capturing raw=(%u,%u), samples=%u",
                        (unsigned int) raw_x,
                        (unsigned int) raw_y,
                        (unsigned int) s_capture_samples);
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

    s_points[s_current_point].raw_x = (uint16_t) (s_capture_sum_x / s_capture_samples);
    s_points[s_current_point].raw_y = (uint16_t) (s_capture_sum_y / s_capture_samples);

    char status[64];
    (void) snprintf(status,
                    sizeof(status),
                    "P%u raw=(%u,%u) saved",
                    (unsigned int) (s_current_point + 1U),
                    (unsigned int) s_points[s_current_point].raw_x,
                    (unsigned int) s_points[s_current_point].raw_y);
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
    (void) event;
    start_calibration_session();
}

static void set_brightness_value_label(uint8_t brightness_percent) {
    if (s_brightness_value_label == NULL) {
        return;
    }

    char brightness_text[12];
    (void) snprintf(brightness_text, sizeof(brightness_text), "%u%%", (unsigned int) brightness_percent);
    lv_label_set_text(s_brightness_value_label, brightness_text);
}

static void on_brightness_slider_changed(lv_event_t *event) {
    lv_obj_t *slider = lv_event_get_target(event);
    if (slider == NULL) {
        return;
    }

    const int32_t value = lv_slider_get_value(slider);
    const uint8_t brightness_percent = value < 0 ? 0U : (uint8_t) value;
    touch_calibrator_display_spi_set_backlight_percent(brightness_percent);
    set_brightness_value_label(brightness_percent);
}

static bool is_valid_pressure_unit(uint8_t unit) {
    return unit == SCP_DISPLAY_UNIT_TORR
           || unit == SCP_DISPLAY_UNIT_BAR
           || unit == SCP_DISPLAY_UNIT_VOLTAGE;
}

static const char *pressure_unit_name(uint8_t unit) {
    switch (unit) {
        case SCP_DISPLAY_UNIT_BAR:
            return "bar";
        case SCP_DISPLAY_UNIT_VOLTAGE:
            return "voltage";
        case SCP_DISPLAY_UNIT_TORR:
        default:
            return "torr";
    }
}

static void set_unit_button_selected_style(lv_obj_t *btn, bool selected) {
    if (btn == NULL) {
        return;
    }

    if (selected) {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x275DB3), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0x306FD1), LV_STATE_PRESSED);
        lv_obj_set_style_text_color(btn, lv_color_hex(0xFFFFFF), 0);
    } else {
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xE6EEF9), 0);
        lv_obj_set_style_bg_color(btn, lv_color_hex(0xD9E5F4), LV_STATE_PRESSED);
        lv_obj_set_style_text_color(btn, lv_color_hex(MONITOR_UI_TEXT_COLOR), 0);
    }
}

static void update_pressure_unit_controls(void) {
    set_unit_button_selected_style(s_unit_torr_btn, s_selected_pressure_unit == SCP_DISPLAY_UNIT_TORR);
    set_unit_button_selected_style(s_unit_bar_btn, s_selected_pressure_unit == SCP_DISPLAY_UNIT_BAR);
    set_unit_button_selected_style(s_unit_voltage_btn, s_selected_pressure_unit == SCP_DISPLAY_UNIT_VOLTAGE);

    if (s_unit_status_label != NULL) {
        char unit_status[80];
        (void) snprintf(unit_status,
                        sizeof(unit_status),
                        "Gauge units: %s%s",
                        pressure_unit_name(s_selected_pressure_unit),
                        s_pressure_unit_command_pending ? " (pending CAN send)" : "");
        lv_label_set_text(s_unit_status_label, unit_status);
    }
}

static void request_pressure_unit_change(uint8_t unit) {
    if (!is_valid_pressure_unit(unit)) {
        return;
    }

    if (unit != s_selected_pressure_unit) {
        s_selected_pressure_unit = unit;
    }
    s_pressure_unit_command_pending = true;
    update_pressure_unit_controls();
}

static void on_pressure_unit_button_clicked(lv_event_t *event) {
    const uintptr_t user_data_value = (uintptr_t) lv_event_get_user_data(event);
    request_pressure_unit_change((uint8_t) user_data_value);
}

static void monitor_touch_read_cb(lv_indev_drv_t *indev_drv, lv_indev_data_t *data) {
    (void) indev_drv;

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
    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(MONITOR_UI_BG_COLOR), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);

    s_root_tabs = lv_tabview_create(lv_scr_act(), LV_DIR_TOP, 38);
    lv_obj_set_size(s_root_tabs, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_root_tabs, lv_color_hex(MONITOR_UI_SURFACE_COLOR), 0);
    lv_obj_set_style_bg_opa(s_root_tabs, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_root_tabs, 1, 0);
    lv_obj_set_style_border_color(s_root_tabs, lv_color_hex(MONITOR_UI_BORDER_COLOR), 0);
    lv_obj_t *tab_content = lv_tabview_get_content(s_root_tabs);
    lv_obj_clear_flag(tab_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tab_content, LV_DIR_NONE);
    lv_obj_set_style_anim_time(tab_content, 0, 0);
    lv_obj_set_style_bg_color(tab_content, lv_color_hex(MONITOR_UI_SURFACE_COLOR), 0);

    lv_obj_t *tab_connections = lv_tabview_add_tab(s_root_tabs, "Connections");
    lv_obj_t *tab_events = lv_tabview_add_tab(s_root_tabs, "Event Log");
    lv_obj_t *tab_settings = lv_tabview_add_tab(s_root_tabs, "Settings");
    lv_obj_clear_flag(tab_connections, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tab_connections, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(tab_connections, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(tab_events, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(tab_events, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_pad_left(tab_events, 0, 0);
    lv_obj_set_style_pad_right(tab_events, 0, 0);
    lv_obj_clear_flag(tab_settings, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(tab_settings, LV_DIR_NONE);
    lv_obj_set_scrollbar_mode(tab_settings, LV_SCROLLBAR_MODE_OFF);

    lv_obj_t *conn_title = lv_label_create(tab_connections);
    lv_label_set_text(conn_title, "CAN Modules");
    lv_obj_align(conn_title, LV_ALIGN_TOP_LEFT, 8, 8);
    lv_obj_set_style_text_color(conn_title, lv_color_hex(MONITOR_UI_TITLE_COLOR), 0);

    s_connections_list = lv_obj_create(tab_connections);
    lv_obj_set_size(s_connections_list, lv_pct(100), lv_pct(100));
    lv_obj_align(s_connections_list, LV_ALIGN_TOP_LEFT, 0, 30);
    lv_obj_set_style_bg_color(s_connections_list, lv_color_hex(MONITOR_UI_SURFACE_COLOR), 0);
    lv_obj_set_style_border_color(s_connections_list, lv_color_hex(MONITOR_UI_BORDER_COLOR), 0);
    lv_obj_set_style_border_width(s_connections_list, 1, 0);
    lv_obj_set_style_pad_all(s_connections_list, 8, 0);
    lv_obj_set_layout(s_connections_list, LV_LAYOUT_FLEX);
    lv_obj_set_flex_flow(s_connections_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_connections_list, 8, 0);

    s_connections_empty_label = lv_label_create(s_connections_list);
    lv_label_set_text(s_connections_empty_label, "No modules discovered yet.");
    lv_obj_set_style_text_color(s_connections_empty_label, lv_color_hex(MONITOR_UI_TEXT_MUTED_COLOR), 0);

    s_event_log_label = lv_label_create(tab_events);
    lv_obj_set_width(s_event_log_label, lv_pct(100));
    lv_obj_align(s_event_log_label, LV_ALIGN_TOP_LEFT, 0, 10);
    lv_label_set_long_mode(s_event_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_event_log_label, lv_color_hex(MONITOR_UI_TEXT_COLOR), 0);
    lv_obj_set_style_text_font(s_event_log_label, &lv_font_unscii_8, 0);
    refresh_event_log_label();

    s_start_calibration_btn = lv_btn_create(tab_settings);
    lv_obj_set_size(s_start_calibration_btn, 250, 46);
    lv_obj_align(s_start_calibration_btn, LV_ALIGN_TOP_LEFT, 8, 16);
    lv_obj_set_style_bg_color(s_start_calibration_btn, lv_color_hex(0x275DB3), 0);
    lv_obj_set_style_bg_color(s_start_calibration_btn, lv_color_hex(0x306FD1), LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_start_calibration_btn, on_start_calibration_clicked, LV_EVENT_CLICKED, NULL);

    lv_obj_t *cal_btn_label = lv_label_create(s_start_calibration_btn);
    lv_label_set_text(cal_btn_label, "Start Touch Calibration");
    lv_obj_center(cal_btn_label);

    s_settings_status_label = lv_label_create(tab_settings);
    lv_obj_set_width(s_settings_status_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 20);
    lv_obj_align(s_settings_status_label, LV_ALIGN_TOP_LEFT, 8, 76);
    lv_label_set_long_mode(s_settings_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_settings_status_label, lv_color_hex(MONITOR_UI_TEXT_MUTED_COLOR), 0);
    lv_label_set_text(s_settings_status_label, "Touch calibration: default mapping");

    lv_obj_t *brightness_title_label = lv_label_create(tab_settings);
    lv_label_set_text(brightness_title_label, "Brightness");
    lv_obj_align(brightness_title_label, LV_ALIGN_TOP_LEFT, 8, 114);
    lv_obj_set_style_text_color(brightness_title_label, lv_color_hex(MONITOR_UI_TEXT_MUTED_COLOR), 0);

    s_brightness_slider = lv_slider_create(tab_settings);
    lv_obj_set_size(s_brightness_slider, TOUCH_CALIBRATOR_DISP_HOR_RES - 66, 18);
    lv_obj_align(s_brightness_slider, LV_ALIGN_TOP_LEFT, 8, 134);
    lv_slider_set_range(s_brightness_slider, 5, 100);
    uint8_t brightness_percent = touch_calibrator_display_spi_get_backlight_percent();
    if (brightness_percent < 5U) {
        brightness_percent = 5U;
        touch_calibrator_display_spi_set_backlight_percent(brightness_percent);
    }
    lv_slider_set_value(s_brightness_slider, brightness_percent, LV_ANIM_OFF);
    lv_obj_add_event_cb(s_brightness_slider, on_brightness_slider_changed, LV_EVENT_VALUE_CHANGED, NULL);

    s_brightness_value_label = lv_label_create(tab_settings);
    lv_obj_align(s_brightness_value_label, LV_ALIGN_TOP_RIGHT, -8, 132);
    lv_obj_set_style_text_color(s_brightness_value_label, lv_color_hex(MONITOR_UI_TEXT_MUTED_COLOR), 0);
    set_brightness_value_label(brightness_percent);

    lv_obj_t *unit_title_label = lv_label_create(tab_settings);
    lv_label_set_text(unit_title_label, "Gauge Display Units");
    lv_obj_align(unit_title_label, LV_ALIGN_TOP_LEFT, 8, 168);
    lv_obj_set_style_text_color(unit_title_label, lv_color_hex(MONITOR_UI_TEXT_MUTED_COLOR), 0);

    s_unit_torr_btn = lv_btn_create(tab_settings);
    lv_obj_set_size(s_unit_torr_btn, 94, 36);
    lv_obj_align(s_unit_torr_btn, LV_ALIGN_TOP_LEFT, 8, 190);
    lv_obj_add_event_cb(s_unit_torr_btn,
                        on_pressure_unit_button_clicked,
                        LV_EVENT_CLICKED,
                        (void *) (uintptr_t) SCP_DISPLAY_UNIT_TORR);
    lv_obj_t *unit_torr_label = lv_label_create(s_unit_torr_btn);
    lv_label_set_text(unit_torr_label, "Torr");
    lv_obj_center(unit_torr_label);

    s_unit_bar_btn = lv_btn_create(tab_settings);
    lv_obj_set_size(s_unit_bar_btn, 94, 36);
    lv_obj_align(s_unit_bar_btn, LV_ALIGN_TOP_LEFT, 109, 190);
    lv_obj_add_event_cb(s_unit_bar_btn,
                        on_pressure_unit_button_clicked,
                        LV_EVENT_CLICKED,
                        (void *) (uintptr_t) SCP_DISPLAY_UNIT_BAR);
    lv_obj_t *unit_bar_label = lv_label_create(s_unit_bar_btn);
    lv_label_set_text(unit_bar_label, "Bar");
    lv_obj_center(unit_bar_label);

    s_unit_voltage_btn = lv_btn_create(tab_settings);
    lv_obj_set_size(s_unit_voltage_btn, 94, 36);
    lv_obj_align(s_unit_voltage_btn, LV_ALIGN_TOP_LEFT, 210, 190);
    lv_obj_add_event_cb(s_unit_voltage_btn,
                        on_pressure_unit_button_clicked,
                        LV_EVENT_CLICKED,
                        (void *) (uintptr_t) SCP_DISPLAY_UNIT_VOLTAGE);
    lv_obj_t *unit_voltage_label = lv_label_create(s_unit_voltage_btn);
    lv_label_set_text(unit_voltage_label, "Voltage");
    lv_obj_center(unit_voltage_label);

    s_unit_status_label = lv_label_create(tab_settings);
    lv_obj_set_width(s_unit_status_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 20);
    lv_obj_align(s_unit_status_label, LV_ALIGN_TOP_LEFT, 8, 232);
    lv_label_set_long_mode(s_unit_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_unit_status_label, lv_color_hex(MONITOR_UI_TEXT_MUTED_COLOR), 0);
    update_pressure_unit_controls();
    monitor_settings_clear_focus();

    lv_obj_t *flash_label = lv_label_create(tab_settings);
    lv_obj_set_width(flash_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 20);
    lv_obj_align(flash_label, LV_ALIGN_BOTTOM_LEFT, 8, -56);
    lv_label_set_long_mode(flash_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(flash_label, lv_color_hex(MONITOR_UI_TEXT_MUTED_COLOR), 0);
    const uint32_t flash_total_kib = PICO_FLASH_SIZE_BYTES / 1024U;
    const uint32_t flash_used_bytes = (uint32_t) ((uintptr_t) &__flash_binary_end - XIP_BASE);
    const uint32_t flash_used_kib = flash_used_bytes / 1024U;
    const uint32_t flash_used_percent = (flash_total_kib > 0U)
                                            ? (uint32_t) ((((uint64_t) flash_used_kib * 100U) + (flash_total_kib / 2U)) /
                                                          flash_total_kib)
                                            : 0U;
    char flash_text[96];
    (void) snprintf(flash_text,
                    sizeof(flash_text),
                    "Flash: %luKB/%luKB (%lu%% used)",
                    (unsigned long) flash_used_kib,
                    (unsigned long) flash_total_kib,
                    (unsigned long) flash_used_percent);
    lv_label_set_text(flash_label, flash_text);

    lv_obj_t *ram_label = lv_label_create(tab_settings);
    lv_obj_set_width(ram_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 20);
    lv_obj_align(ram_label, LV_ALIGN_BOTTOM_LEFT, 8, -34);
    lv_label_set_long_mode(ram_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(ram_label, lv_color_hex(MONITOR_UI_TEXT_MUTED_COLOR), 0);
    const uint32_t ram_total_kib = (SRAM_END - SRAM_BASE) / 1024U;
    const uintptr_t ram_static_bytes = (uintptr_t) &__end__ - SRAM_BASE;
    const uint32_t ram_static_kib = (uint32_t) (ram_static_bytes / 1024U);
    const uint32_t ram_used_percent = (ram_total_kib > 0U)
                                          ? (uint32_t) ((((uint64_t) ram_static_kib * 100U) + (ram_total_kib / 2U)) /
                                                        ram_total_kib)
                                          : 0U;
    char ram_text[128];
    (void) snprintf(ram_text,
                    sizeof(ram_text),
                    "RAM: %luKB/%luKB (%lu%% used)",
                    (unsigned long) ram_static_kib,
                    (unsigned long) ram_total_kib,
                    (unsigned long) ram_used_percent);
    lv_label_set_text(ram_label, ram_text);

    lv_obj_t *build_label = lv_label_create(tab_settings);
    lv_obj_set_width(build_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 20);
    lv_obj_align(build_label, LV_ALIGN_BOTTOM_LEFT, 8, -12);
    lv_label_set_long_mode(build_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(build_label, lv_color_hex(MONITOR_UI_TEXT_MUTED_COLOR), 0);

    char build_text[96];
    (void) snprintf(build_text, sizeof(build_text), "Firmware build: %s %s", __DATE__, __TIME__);
    lv_label_set_text(build_label, build_text);
}

static void build_calibration_view(void) {
    s_calibration_root = lv_obj_create(lv_scr_act());
    lv_obj_set_size(s_calibration_root, lv_pct(100), lv_pct(100));
    lv_obj_set_style_bg_color(s_calibration_root, lv_color_hex(MONITOR_UI_BG_COLOR), 0);
    lv_obj_set_style_bg_opa(s_calibration_root, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_calibration_root, 0, 0);
    lv_obj_add_flag(s_calibration_root, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *title_label = lv_label_create(s_calibration_root);
    lv_label_set_text(title_label, "Touch Calibration");
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_set_style_text_color(title_label, lv_color_hex(MONITOR_UI_TITLE_COLOR), 0);

    s_instruction_label = lv_label_create(s_calibration_root);
    lv_obj_set_width(s_instruction_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 20);
    lv_obj_align(s_instruction_label, LV_ALIGN_TOP_LEFT, 10, 40);
    lv_label_set_long_mode(s_instruction_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_instruction_label, lv_color_hex(MONITOR_UI_TEXT_COLOR), 0);

    s_status_label = lv_label_create(s_calibration_root);
    lv_obj_set_width(s_status_label, TOUCH_CALIBRATOR_DISP_HOR_RES - 20);
    lv_obj_align(s_status_label, LV_ALIGN_BOTTOM_LEFT, 10, -8);
    lv_label_set_long_mode(s_status_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(MONITOR_UI_TEXT_MUTED_COLOR), 0);

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
    s_encoder_button_active_low = true;
    s_encoder_button_fallback_available = false;
    s_encoder_button_fallback_pin = MONITOR_ENCODER_BUTTON_FALLBACK_PIN;
    s_encoder_tab_entered = false;
    s_encoder_slider_adjust_mode = false;
    s_settings_focus_index = 0U;
    s_start_calibration_btn = NULL;
    s_brightness_slider = NULL;
    s_unit_torr_btn = NULL;
    s_unit_bar_btn = NULL;
    s_unit_voltage_btn = NULL;
    s_selected_pressure_unit = SCP_DISPLAY_UNIT_TORR;
    s_pressure_unit_command_pending = true;

    s_points[0].screen = (lv_point_t){30, 30};
    s_points[1].screen = (lv_point_t){TOUCH_CALIBRATOR_DISP_HOR_RES - 31, 30};
    s_points[2].screen = (lv_point_t){TOUCH_CALIBRATOR_DISP_HOR_RES - 31, TOUCH_CALIBRATOR_DISP_VER_RES - 31};
    s_points[3].screen = (lv_point_t){30, TOUCH_CALIBRATOR_DISP_VER_RES - 31};
    s_points[4].screen = (lv_point_t){TOUCH_CALIBRATOR_DISP_HOR_RES / 2, TOUCH_CALIBRATOR_DISP_VER_RES / 2};

    touch_calibrator_display_spi_init(pins);
    monitor_encoder_init(pins, to_ms_since_boot(get_absolute_time()));
    lv_init();
    ili9488_init_panel();
#if MONITOR_STARTUP_DIAG_PATTERN
    ili9488_startup_diagnostic_pattern();
#else
    if (ili9488_draw_splash_from_flash()) {
        wait_for_splash_dismiss(5000U);
    } else {
        ili9488_boot_test_pattern();
    }
#endif

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
    (void) lv_disp_drv_register(&disp_drv);

    static lv_indev_drv_t indev_drv;
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.read_cb = monitor_touch_read_cb;
    (void) lv_indev_drv_register(&indev_drv);

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

    (void) lv_timer_handler();
}

void touch_calibrator_display_handle_can_message(const struct can2040_msg *msg, uint32_t uptime_ms) {
    if (msg == NULL) {
        return;
    }

    if (is_heartbeat_msg_id(msg->id)) {
        uint8_t module_id = 0U;
        if (decode_module_id_from_msg_id(msg->id, &module_id)) {
            const int idx = ensure_connection_row(module_id);
            if (idx >= 0) {
                s_connection_rows[idx].heartbeat_seen = true;
                s_connection_rows[idx].last_heartbeat_ms = uptime_ms;
            }
        }
        return;
    }

    char line[MONITOR_EVENT_LINE_CHARS];
    format_can_event_line(msg, uptime_ms, line, sizeof(line));
    push_event_line(line);
}

bool touch_calibrator_display_take_pressure_unit_command(uint8_t *unit_out) {
    if (!s_pressure_unit_command_pending) {
        return false;
    }

    s_pressure_unit_command_pending = false;
    if (unit_out != NULL) {
        *unit_out = s_selected_pressure_unit;
    }
    update_pressure_unit_controls();
    return true;
}
