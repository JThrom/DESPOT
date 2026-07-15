// CrowPanel Advanced 9-inch ESP32-P4 HMI display backend.
//
// Implements the shared waveshare_display.hpp interface for the Elecrow
// CrowPanel Advanced 9" P4 board (1024x600 IPS MIPI-DSI + capacitive touch,
// ESP32-C6 WiFi-6 co-processor).
//
// Values below are taken from Elecrow's factory ESP-IDF source for this
// board (github.com/Elecrow-RD/CrowPanel-Advanced-9inch-ESP32-P4-HMI-AI-Display-1024x600-IPS-Touch-Screen,
// peripheral/bsp_illuminate + main/main.c). The CrowPanel does NOT use a
// CH422G IO-expander; the GT911 touch and backlight are driven directly by
// ESP32-P4 GPIOs. Touch wiring (GT911 @ I2C SDA45/SCL46, addr 0x5D/0x14,
// INT42/RST40) is documented here for the touch driver in ui_status_menu.cpp.

#include "waveshare_display.hpp"

#include "esp_heap_caps.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "driver/gpio.h"
#include "driver/ledc.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstdlib>

static constexpr int W = 1024;
static constexpr int H = 600;
static constexpr int DRAW_BUF_LINES = 24;
static constexpr int DSI_BUS_ID = 0;
static constexpr int DSI_LANES = 2;              // Elecrow factory: 2 data lanes
static constexpr int DSI_BITRATE_MBPS = 900;     // Elecrow factory: 900 Mbps/lane
static constexpr int DPI_CLK_MHZ = 51;           // Elecrow factory: 51 MHz DPI clock
static constexpr int LCD_HSYNC = 70;             // Elecrow factory: hsync pulse width
static constexpr int LCD_HBP = 160;              // Elecrow factory: hsync back porch
static constexpr int LCD_HFP = 160;              // Elecrow factory: hsync front porch
static constexpr int LCD_VSYNC = 10;             // Elecrow factory: vsync pulse width
static constexpr int LCD_VBP = 23;               // Elecrow factory: vsync back porch
static constexpr int LCD_VFP = 12;               // Elecrow factory: vsync front porch
static constexpr int DSI_PHY_LDO_CHAN = 3;       // Elecrow factory: DSI PHY LDO channel 3
static constexpr int DSI_PHY_LDO_MV = 2500;      // Elecrow factory: DSI PHY LDO 2500 mV
static constexpr int PANEL_LDO_CHAN = 4;         // Elecrow factory: panel/periph rail LDO channel 4
static constexpr int PANEL_LDO_MV = 3300;        // Elecrow factory: panel/periph rail 3300 mV
static constexpr gpio_num_t LCD_BACKLIGHT_GPIO = GPIO_NUM_31; // Elecrow factory: backlight PWM on GPIO31
static constexpr int LCD_BACKLIGHT_ON_LEVEL = 1;             // Elecrow factory: active HIGH
static constexpr int LCD_RESET_GPIO = -1;                    // Elecrow factory: no dedicated panel reset line

static const char *TAG = "CROWPANEL_P4";

static void *s_buf1 = NULL;
static void *s_buf2 = NULL;
static uint16_t *s_rot_buf = NULL;
static esp_lcd_panel_handle_t s_panel = NULL;
static esp_lcd_dsi_bus_handle_t s_dsi_bus = NULL;
static esp_lcd_panel_io_handle_t s_dbi_io = NULL;
static esp_ldo_channel_handle_t s_dsi_ldo = NULL;
static esp_ldo_channel_handle_t s_panel_ldo = NULL;
static TaskHandle_t s_bl_task = NULL;
static bool s_bl_pwm_ready = false;
static int s_bl_percent = 25;
static uint32_t s_bl_duty = 0;

static constexpr ledc_mode_t BL_LEDC_MODE = LEDC_LOW_SPEED_MODE;
static constexpr ledc_channel_t BL_LEDC_CHANNEL = LEDC_CHANNEL_0;
static constexpr ledc_timer_t BL_LEDC_TIMER = LEDC_TIMER_0;
static constexpr ledc_timer_bit_t BL_LEDC_DUTY_RES = LEDC_TIMER_11_BIT; // Elecrow factory: 11-bit
static constexpr uint32_t BL_LEDC_MAX_DUTY = (1U << BL_LEDC_DUTY_RES) - 1U;
static constexpr uint32_t BL_LEDC_FREQ_HZ = 30000; // Elecrow factory: 30 kHz

static inline int clamp_percent(int percent) {
    if (percent < 0) return 0;
    if (percent > 100) return 100;
    return percent;
}

static uint32_t percent_to_duty(int percent) {
    // Active-HIGH backlight: higher percent -> higher duty (0% -> off).
    const int p = clamp_percent(percent);
    return (uint32_t)((p * (int)BL_LEDC_MAX_DUTY + 50) / 100);
}

static void apply_backlight_percent(int percent) {
    s_bl_percent = clamp_percent(percent);
    s_bl_duty = percent_to_duty(s_bl_percent);

    if (s_bl_pwm_ready) {
        ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, s_bl_duty);
        ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
        return;
    }

    const int level = (s_bl_percent > 0) ? LCD_BACKLIGHT_ON_LEVEL : (LCD_BACKLIGHT_ON_LEVEL ? 0 : 1);
    gpio_set_level(LCD_BACKLIGHT_GPIO, level);
}

static void backlight_keepalive_task(void *arg) {
    (void)arg;
    while (true) {
        if (s_bl_pwm_ready) {
            ledc_set_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL, s_bl_duty);
            ledc_update_duty(BL_LEDC_MODE, BL_LEDC_CHANNEL);
        } else {
            gpio_set_direction(LCD_BACKLIGHT_GPIO, GPIO_MODE_OUTPUT);
            const int level = (s_bl_percent > 0) ? LCD_BACKLIGHT_ON_LEVEL : (LCD_BACKLIGHT_ON_LEVEL ? 0 : 1);
            gpio_set_level(LCD_BACKLIGHT_GPIO, level);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
}

static void free_draw_buffers(void) {
    if (s_buf1) free(s_buf1);
    if (s_buf2) free(s_buf2);
    if (s_rot_buf) free(s_rot_buf);
    s_buf1 = NULL;
    s_buf2 = NULL;
    s_rot_buf = NULL;
}

static bool p4_mipi_flush_done_cb(esp_lcd_panel_handle_t panel,
                                  esp_lcd_dpi_panel_event_data_t *edata,
                                  void *user_ctx) {
    (void)panel;
    (void)edata;
    lv_display_flush_ready((lv_display_t *)user_ctx);
    return false;
}

static void p4_mipi_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    esp_lcd_panel_handle_t panel = (esp_lcd_panel_handle_t)lv_display_get_user_data(disp);
    if (!panel) {
        lv_display_flush_ready(disp);
        return;
    }

    // Native orientation (no software rotation): draw the LVGL area directly.
    esp_err_t ret = esp_lcd_panel_draw_bitmap(panel,
                                              area->x1,
                                              area->y1,
                                              area->x2 + 1,
                                              area->y2 + 1,
                                              px_map);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "panel draw bitmap failed: %s", esp_err_to_name(ret));
        lv_display_flush_ready(disp);
    }
}

static esp_err_t p4_mipi_try_init(lv_display_t **out_disp) {
    // CrowPanel: no CH422G IO-expander. Bring up the panel/peripheral rail
    // (LDO channel 4 @ 3300 mV) and the DSI PHY rail (channel 3 @ 2500 mV).
    esp_ldo_channel_config_t panel_ldo_cfg = {};
    panel_ldo_cfg.chan_id = PANEL_LDO_CHAN;
    panel_ldo_cfg.voltage_mv = PANEL_LDO_MV;
    esp_err_t ret = esp_ldo_acquire_channel(&panel_ldo_cfg, &s_panel_ldo);
    if (ret != ESP_OK) {
        // Non-fatal: the rail may already be powered by hardware default.
        ESP_LOGW(TAG, "panel rail LDO acquire failed: %s", esp_err_to_name(ret));
        s_panel_ldo = NULL;
    }

    esp_ldo_channel_config_t ldo_cfg = {};
    ldo_cfg.chan_id = DSI_PHY_LDO_CHAN;
    ldo_cfg.voltage_mv = DSI_PHY_LDO_MV;
    ret = esp_ldo_acquire_channel(&ldo_cfg, &s_dsi_ldo);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "DSI PHY LDO acquire failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_dsi_bus_config_t bus_cfg = {};
    bus_cfg.bus_id = DSI_BUS_ID;
    bus_cfg.num_data_lanes = DSI_LANES;
    bus_cfg.lane_bit_rate_mbps = DSI_BITRATE_MBPS;
    ret = esp_lcd_new_dsi_bus(&bus_cfg, &s_dsi_bus);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "new dsi bus failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG, "DSI bus initialized: id=%d lanes=%d bitrate=%dMbps", DSI_BUS_ID, DSI_LANES, DSI_BITRATE_MBPS);

    esp_lcd_dbi_io_config_t dbi_cfg = {};
    dbi_cfg.virtual_channel = 0;
    dbi_cfg.lcd_cmd_bits = 8;
    dbi_cfg.lcd_param_bits = 8;
    ret = esp_lcd_new_panel_io_dbi(s_dsi_bus, &dbi_cfg, &s_dbi_io);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "new panel io dbi failed: %s", esp_err_to_name(ret));
        return ret;
    }

    esp_lcd_dpi_panel_config_t dpi_cfg = {};
    dpi_cfg.virtual_channel = 0;
    dpi_cfg.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_cfg.dpi_clock_freq_mhz = DPI_CLK_MHZ;
    dpi_cfg.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_cfg.video_timing.h_size = W;
    dpi_cfg.video_timing.v_size = H;
    dpi_cfg.video_timing.hsync_back_porch = LCD_HBP;
    dpi_cfg.video_timing.hsync_pulse_width = LCD_HSYNC;
    dpi_cfg.video_timing.hsync_front_porch = LCD_HFP;
    dpi_cfg.video_timing.vsync_back_porch = LCD_VBP;
    dpi_cfg.video_timing.vsync_pulse_width = LCD_VSYNC;
    dpi_cfg.video_timing.vsync_front_porch = LCD_VFP;

    // Elecrow factory: same EK79007 driver IC as the 7" panel.
    ek79007_vendor_config_t vendor_cfg = {};
    vendor_cfg.mipi_config.dsi_bus = s_dsi_bus;
    vendor_cfg.mipi_config.dpi_config = &dpi_cfg;
    vendor_cfg.mipi_config.lane_num = DSI_LANES;

    esp_lcd_panel_dev_config_t panel_cfg = {};
    panel_cfg.reset_gpio_num = (gpio_num_t)LCD_RESET_GPIO;
    panel_cfg.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_cfg.bits_per_pixel = 16;
    panel_cfg.vendor_config = &vendor_cfg;

    ret = esp_lcd_new_panel_ek79007(s_dbi_io, &panel_cfg, &s_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "new panel failed: %s", esp_err_to_name(ret));
        return ret;
    }

    if (LCD_RESET_GPIO >= 0) {
        gpio_set_direction((gpio_num_t)LCD_RESET_GPIO, GPIO_MODE_OUTPUT);
        gpio_set_level((gpio_num_t)LCD_RESET_GPIO, 1);
    }

    ret = esp_lcd_panel_init(s_panel);
    if (ret != ESP_OK) return ret;

    gpio_config_t bl_cfg = {};
    bl_cfg.pin_bit_mask = 1ULL << LCD_BACKLIGHT_GPIO;
    bl_cfg.mode = GPIO_MODE_OUTPUT;
    ret = gpio_config(&bl_cfg);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "backlight gpio config failed: %s", esp_err_to_name(ret));
    } else {
        ledc_timer_config_t timer_cfg = {};
        timer_cfg.speed_mode = BL_LEDC_MODE;
        timer_cfg.timer_num = BL_LEDC_TIMER;
        timer_cfg.duty_resolution = BL_LEDC_DUTY_RES;
        timer_cfg.freq_hz = BL_LEDC_FREQ_HZ;
        timer_cfg.clk_cfg = LEDC_AUTO_CLK;

        esp_err_t ledc_ret = ledc_timer_config(&timer_cfg);
        if (ledc_ret == ESP_OK) {
            ledc_channel_config_t ch_cfg = {};
            ch_cfg.gpio_num = LCD_BACKLIGHT_GPIO;
            ch_cfg.speed_mode = BL_LEDC_MODE;
            ch_cfg.channel = BL_LEDC_CHANNEL;
            ch_cfg.intr_type = LEDC_INTR_DISABLE;
            ch_cfg.timer_sel = BL_LEDC_TIMER;
            ch_cfg.duty = percent_to_duty(s_bl_percent);
            ch_cfg.hpoint = 0;
            ledc_ret = ledc_channel_config(&ch_cfg);
        }

        if (ledc_ret == ESP_OK) {
            s_bl_pwm_ready = true;
            apply_backlight_percent(s_bl_percent);
            ESP_LOGI(TAG, "backlight PWM enabled on GPIO %d", (int)LCD_BACKLIGHT_GPIO);
        } else {
            s_bl_pwm_ready = false;
            ESP_LOGW(TAG, "backlight PWM setup failed: %s", esp_err_to_name(ledc_ret));
            apply_backlight_percent(s_bl_percent);
        }

        if (!s_bl_task) {
            BaseType_t ok = xTaskCreate(backlight_keepalive_task,
                                        "bl_keepalive",
                                        2048,
                                        NULL,
                                        2,
                                        &s_bl_task);
            if (ok != pdPASS) {
                ESP_LOGW(TAG, "backlight keepalive task create failed");
                s_bl_task = NULL;
            }
        }
    }

    const size_t draw_buf_bytes = W * DRAW_BUF_LINES * sizeof(uint16_t);
    s_buf1 = heap_caps_malloc(draw_buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_buf2 = heap_caps_malloc(draw_buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf1 || !s_buf2) {
        ESP_LOGE(TAG, "MIPI draw buffer alloc failed");
        return ESP_ERR_NO_MEM;
    }

    lv_display_t *disp = lv_display_create(W, H);
    if (!disp) {
        ESP_LOGE(TAG, "LVGL display create failed");
        return ESP_FAIL;
    }

    lv_display_set_user_data(disp, s_panel);
    lv_display_set_flush_cb(disp, p4_mipi_flush_cb);
    lv_display_set_buffers(disp, s_buf1, s_buf2, draw_buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);

    // CrowPanel Advanced 9": native panel orientation, no software rotation.

    esp_lcd_dpi_panel_event_callbacks_t cbs = {};
    cbs.on_color_trans_done = p4_mipi_flush_done_cb;
    ret = esp_lcd_dpi_panel_register_event_callbacks(s_panel, &cbs, disp);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "panel callback register failed: %s", esp_err_to_name(ret));
        return ret;
    }

    *out_disp = disp;
    ESP_LOGI(TAG, "MIPI DSI display initialized (CrowPanel Advanced 9\", 1024x600)");
    return ESP_OK;
}

static void p4_headless_flush_cb(lv_display_t *disp, const lv_area_t *area, uint8_t *px_map) {
    (void)area;
    (void)px_map;
    lv_display_flush_ready(disp);
}

esp_err_t waveshare_display_init(lv_display_t **out_disp) {
    if (!out_disp) return ESP_ERR_INVALID_ARG;

    esp_err_t ret = p4_mipi_try_init(out_disp);
    if (ret == ESP_OK) {
        return ESP_OK;
    }
    ESP_LOGW(TAG, "MIPI init failed (%s), falling back to headless backend", esp_err_to_name(ret));
    free_draw_buffers();

    const size_t draw_buf_bytes = W * DRAW_BUF_LINES * sizeof(uint16_t);
    s_buf1 = heap_caps_malloc(draw_buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_buf2 = heap_caps_malloc(draw_buf_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_buf1 || !s_buf2) {
        free_draw_buffers();
        ESP_LOGE(TAG, "P4 LVGL headless buffers alloc failed");
        return ESP_ERR_NO_MEM;
    }

    lv_display_t *disp = lv_display_create(W, H);
    if (!disp) {
        free_draw_buffers();
        return ESP_FAIL;
    }

    lv_display_set_flush_cb(disp, p4_headless_flush_cb);
    lv_display_set_buffers(disp, s_buf1, s_buf2, draw_buf_bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
    *out_disp = disp;

    ESP_LOGW(TAG, "Using temporary headless 1024x600 LVGL display backend (no MIPI output yet)");
    return ESP_OK;
}

int waveshare_display_width(void) {
    return W;
}

int waveshare_display_height(void) {
    return H;
}

bool waveshare_display_brightness_supported(void) {
    return true;
}

int waveshare_display_get_brightness(void) {
    return s_bl_percent;
}

esp_err_t waveshare_display_set_brightness(int percent) {
    s_bl_percent = clamp_percent(percent);
    apply_backlight_percent(s_bl_percent);
    return ESP_OK;
}
