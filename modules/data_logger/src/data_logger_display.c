#include "data_logger_display.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "can2040.h"
#include "lvgl/lvgl.h"
#include "pico/stdlib.h"

#define DATA_LOGGER_DISP_HOR_RES 480
#define DATA_LOGGER_DISP_VER_RES 320

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

#define DATA_LOGGER_MAX_LINES 10
#define DATA_LOGGER_LINE_CHARS 80
#define DATA_LOGGER_TEXT_CHARS ((DATA_LOGGER_MAX_LINES * DATA_LOGGER_LINE_CHARS) + DATA_LOGGER_MAX_LINES + 1)

static lv_obj_t *s_status_label;
static lv_obj_t *s_log_label;

static char s_log_lines[DATA_LOGGER_MAX_LINES][DATA_LOGGER_LINE_CHARS];
static char s_log_text[DATA_LOGGER_TEXT_CHARS];
static uint32_t s_frame_count;

static inline void ili9488_write(uint8_t mode, uint8_t value) {
    data_logger_display_spi_set_cd(mode == ILI9488_DATA_MODE);
    data_logger_display_spi_write_byte(value);
}

static inline void ili9488_write_array(uint8_t mode, const uint8_t *data, size_t len) {
    data_logger_display_spi_set_cd(mode == ILI9488_DATA_MODE);
    data_logger_display_spi_write_array(data, len);
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

    data_logger_display_spi_set_cs(1);
    data_logger_display_spi_set_reset(0);
    sleep_ms(20);
    data_logger_display_spi_set_reset(1);
    sleep_ms(120);
    data_logger_display_spi_set_cs(0);

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
    ili9488_write(ILI9488_DATA_MODE, ILI9488_MADCTL_MV | ILI9488_MADCTL_MY | ILI9488_MADCTL_BGR);

    ili9488_write(ILI9488_CMD_MODE, ILI9488_PIXFMT);
    ili9488_write(ILI9488_DATA_MODE, 0x55);

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

    data_logger_display_spi_set_cs(1);
}

static void ili9488_flush(lv_disp_drv_t *disp_drv, const lv_area_t *area, lv_color_t *color_p) {
    if (area->x2 < 0 || area->y2 < 0
        || area->x1 > (DATA_LOGGER_DISP_HOR_RES - 1)
        || area->y1 > (DATA_LOGGER_DISP_VER_RES - 1)) {
        lv_disp_flush_ready(disp_drv);
        return;
    }

    const int32_t act_x1 = area->x1 < 0 ? 0 : area->x1;
    const int32_t act_y1 = area->y1 < 0 ? 0 : area->y1;
    const int32_t act_x2 = area->x2 > (DATA_LOGGER_DISP_HOR_RES - 1) ? (DATA_LOGGER_DISP_HOR_RES - 1) : area->x2;
    const int32_t act_y2 = area->y2 > (DATA_LOGGER_DISP_VER_RES - 1) ? (DATA_LOGGER_DISP_VER_RES - 1) : area->y2;

    const int32_t src_width = (area->x2 - area->x1) + 1;
    const int32_t skip_x = act_x1 - area->x1;
    const int32_t skip_y = act_y1 - area->y1;
    const int32_t row_bytes = ((act_x2 - act_x1) + 1) * 2;
    const lv_color_t *row_ptr = color_p + (skip_y * src_width) + skip_x;

    data_logger_display_spi_set_cs(0);
    ili9488_set_window(act_x1, act_y1, act_x2, act_y2);
    for (int32_t y = act_y1; y <= act_y2; ++y) {
        ili9488_write_array(ILI9488_DATA_MODE, (const uint8_t *)row_ptr, (size_t)row_bytes);
        row_ptr += src_width;
    }
    data_logger_display_spi_set_cs(1);

    lv_disp_flush_ready(disp_drv);
}

static void data_logger_display_refresh_log(void) {
    size_t offset = 0;

    s_log_text[0] = '\0';
    for (size_t i = 0; i < DATA_LOGGER_MAX_LINES; ++i) {
        const char *line = s_log_lines[i];
        if (line[0] == '\0') {
            continue;
        }

        if (offset >= (sizeof(s_log_text) - 1)) {
            break;
        }

        int written = snprintf(&s_log_text[offset], sizeof(s_log_text) - offset, "%s\n", line);
        if (written < 0) {
            break;
        }
        if ((size_t)written >= (sizeof(s_log_text) - offset)) {
            offset = sizeof(s_log_text) - 1;
            break;
        }
        offset += (size_t)written;
    }

    if (offset == 0) {
        (void)snprintf(s_log_text, sizeof(s_log_text), "Waiting for CAN traffic...");
    }

    lv_label_set_text(s_log_label, s_log_text);
}

static void data_logger_display_push_line(const char *line) {
    if (line == NULL) {
        return;
    }

    memmove(s_log_lines[1], s_log_lines[0], (DATA_LOGGER_MAX_LINES - 1U) * DATA_LOGGER_LINE_CHARS);
    (void)snprintf(s_log_lines[0], DATA_LOGGER_LINE_CHARS, "%s", line);
    data_logger_display_refresh_log();
}

void data_logger_display_init(const data_logger_display_spi_pins_t *pins) {
    if (pins == NULL) {
        return;
    }

    memset(s_log_lines, 0, sizeof(s_log_lines));
    memset(s_log_text, 0, sizeof(s_log_text));
    s_frame_count = 0;

    data_logger_display_spi_init(pins);
    lv_init();
    ili9488_init_panel();

    static lv_disp_draw_buf_t draw_buf;
    static lv_color_t draw_buf_1[DATA_LOGGER_DISP_HOR_RES * 20];
    lv_disp_draw_buf_init(&draw_buf, draw_buf_1, NULL, DATA_LOGGER_DISP_HOR_RES * 20);

    static lv_disp_drv_t disp_drv;
    lv_disp_drv_init(&disp_drv);
    disp_drv.hor_res = DATA_LOGGER_DISP_HOR_RES;
    disp_drv.ver_res = DATA_LOGGER_DISP_VER_RES;
    disp_drv.flush_cb = ili9488_flush;
    disp_drv.draw_buf = &draw_buf;
    (void)lv_disp_drv_register(&disp_drv);

    lv_obj_set_style_bg_color(lv_scr_act(), lv_color_hex(0x0B1020), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(lv_scr_act(), LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *title_label = lv_label_create(lv_scr_act());
    lv_label_set_text(title_label, "CAN Data Logger");
    lv_obj_align(title_label, LV_ALIGN_TOP_LEFT, 10, 8);
    lv_obj_set_style_text_color(title_label, lv_color_hex(0x66E3FF), 0);

    s_status_label = lv_label_create(lv_scr_act());
    lv_label_set_text(s_status_label, "Frames: 0");
    lv_obj_align(s_status_label, LV_ALIGN_TOP_RIGHT, -10, 8);
    lv_obj_set_style_text_color(s_status_label, lv_color_hex(0xDDE6F5), 0);

    s_log_label = lv_label_create(lv_scr_act());
    lv_obj_set_size(s_log_label, DATA_LOGGER_DISP_HOR_RES - 20, DATA_LOGGER_DISP_VER_RES - 50);
    lv_obj_align(s_log_label, LV_ALIGN_TOP_LEFT, 10, 40);
    lv_label_set_long_mode(s_log_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_log_label, lv_color_hex(0xE3F2FD), 0);

    data_logger_display_refresh_log();
}

void data_logger_display_tick(uint32_t elapsed_ms) {
    lv_tick_inc(elapsed_ms);
}

void data_logger_display_task_handler(void) {
    (void)lv_timer_handler();
}

void data_logger_display_append_can_event(const struct can2040_msg *msg, uint32_t uptime_ms) {
    if (msg == NULL || s_status_label == NULL || s_log_label == NULL) {
        return;
    }

    const uint8_t dlc = msg->dlc > 8U ? 8U : (uint8_t)msg->dlc;
    char line[DATA_LOGGER_LINE_CHARS];
    int offset = snprintf(line,
                          sizeof(line),
                          "%8lums ID:%08lX DLC:%u",
                          (unsigned long)uptime_ms,
                          (unsigned long)msg->id,
                          dlc);

    if (offset < 0) {
        return;
    }

    for (uint8_t i = 0; i < dlc; ++i) {
        if ((size_t)offset >= sizeof(line)) {
            break;
        }
        int written = snprintf(&line[offset], sizeof(line) - (size_t)offset, " %02X", msg->data[i]);
        if (written < 0) {
            break;
        }
        offset += written;
    }

    s_frame_count++;
    char status[48];
    (void)snprintf(status, sizeof(status), "Frames: %lu", (unsigned long)s_frame_count);
    lv_label_set_text(s_status_label, status);

    data_logger_display_push_line(line);
}
