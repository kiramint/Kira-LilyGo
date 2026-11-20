#include "platform.hpp"
#include <algorithm>
#include <atomic>
#include "button_gpio.h"
#include "iot_knob.h"
#include "driver/ledc.h"
#include "driver/sdspi_host.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lvgl_port.h"
#include "esp_vfs_fat.h"
#include "esp_log.h"
#include "nvs.h"

namespace embed {
namespace {
QueueHandle_t keys;
sdmmc_card_t *card;
uint64_t claimed;
nvs_handle_t prefs;
std::atomic<int> light{65}, gain{35};
adc_oneshot_unit_handle_t adc;
adc_cali_handle_t calibration;
void enqueue(void *, void *data) {
    Input event = static_cast<Input>(reinterpret_cast<uintptr_t>(data));
    xQueueSend(keys, &event, 0);
}
void inputs() {
    keys = xQueueCreate(32, sizeof(Input));
    assert(keys);
    knob_config_t k{};
    k.gpio_encoder_a = 2;
    k.gpio_encoder_b = 1;
    auto knob = iot_knob_create(&k);
    ESP_ERROR_CHECK(iot_knob_register_cb(knob, KNOB_LEFT, enqueue, (void *)Input::left));
    ESP_ERROR_CHECK(iot_knob_register_cb(knob, KNOB_RIGHT, enqueue, (void *)Input::right));
    button_config_t cfg{};
    cfg.long_press_time = 800;
    button_gpio_config_t pin{};
    pin.gpio_num = 0;
    pin.active_level = 0;
    button_handle_t btn{};
    ESP_ERROR_CHECK(iot_button_new_gpio_device(&cfg, &pin, &btn));
    ESP_ERROR_CHECK(iot_button_register_cb(btn, BUTTON_SINGLE_CLICK, nullptr, enqueue, (void *)Input::click));
    ESP_ERROR_CHECK(
        iot_button_register_cb(btn, BUTTON_LONG_PRESS_START, nullptr, enqueue, (void *)Input::home));
}
spi_bus_config_t bus(int mosi, int miso, int clk) {
    spi_bus_config_t b{};
    b.mosi_io_num = mosi;
    b.miso_io_num = miso;
    b.sclk_io_num = clk;
    b.quadwp_io_num = b.quadhd_io_num = -1;
    b.max_transfer_sz = 320 * 32 * 2;
    return b;
}
void display() {
    auto b = bus(11, -1, 12);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &b, SPI_DMA_CH_AUTO));
    esp_lcd_panel_io_spi_config_t cfg{};
    cfg.dc_gpio_num = GPIO_NUM_13;
    cfg.cs_gpio_num = GPIO_NUM_10;
    cfg.pclk_hz = 40000000;
    cfg.lcd_cmd_bits = cfg.lcd_param_bits = 8;
    cfg.trans_queue_depth = 6;
    esp_lcd_panel_io_handle_t io{};
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi(SPI2_HOST, &cfg, &io));
    esp_lcd_panel_dev_config_t dev{};
    dev.reset_gpio_num = GPIO_NUM_9;
    dev.bits_per_pixel = 16;
    dev.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    esp_lcd_panel_handle_t panel{};
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(io, &dev, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(panel, true));
    ESP_ERROR_CHECK(esp_lcd_panel_set_gap(panel, 0, 35));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    lvgl_port_cfg_t lv = ESP_LVGL_PORT_INIT_CONFIG();
    lv.task_stack = 12288;
    ESP_ERROR_CHECK(lvgl_port_init(&lv));
    lvgl_port_display_cfg_t d{};
    d.io_handle = io;
    d.panel_handle = panel;
    d.buffer_size = 320 * 24;
    d.double_buffer = true;
    d.hres = 320;
    d.vres = 170;
    d.rotation.swap_xy = true;
    d.rotation.mirror_y = true;
    d.flags.buff_dma = true;
    d.flags.swap_bytes = true;
    d.color_format = LV_COLOR_FORMAT_RGB565;
    ESP_ERROR_CHECK(lvgl_port_add_disp(&d) ? ESP_OK : ESP_ERR_NO_MEM);
}
void send_byte(uint8_t v) {
    for (int bit = 7; bit >= 0; --bit) {
        gpio_set_level(GPIO_NUM_42, (v >> bit) & 1);
        gpio_set_level(GPIO_NUM_45, 1);
        gpio_set_level(GPIO_NUM_45, 0);
    }
}
} // namespace
void write_lamp(uint8_t r, uint8_t g, uint8_t b) {
    for (int i = 0; i < 4; ++i)
        send_byte(0);
    for (int i = 0; i < 7; ++i) {
        send_byte(0xe4);
        send_byte(b);
        send_byte(g);
        send_byte(r);
    }
    send_byte(0xff);
}
esp_err_t claim_pin(App &app, int pin) {
    // UART0 owns the Grove pins when the default serial console is enabled.
    bool available = pin == 16 || pin == 17;
#if !CONFIG_ESP_CONSOLE_UART_DEFAULT
    available |= pin == 43 || pin == 44;
#endif
    if (!app.root || !available)
        return ESP_ERR_INVALID_STATE;
    claimed |= 1ULL << pin;
    return ESP_OK;
}
void release_pins() {
    for (int pin : {16, 17, 43, 44})
        if (claimed & (1ULL << pin))
            gpio_reset_pin((gpio_num_t)pin);
    claimed = 0;
    write_lamp(0, 0, 0);
}
bool read_input(Input &e) { return xQueueReceive(keys, &e, 0) == pdTRUE; }
int brightness() { return light.load(); }
int volume() { return gain.load(); }
void set_level(bool bl, int p, bool persist) {
    p = std::clamp(p, bl ? 1 : 0, 100);
    (bl ? light : gain).store(p);
    if (bl) {
        ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, p * 1023 / 100);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);
    }
    if (persist && prefs && nvs_set_u8(prefs, bl ? "light" : "volume", p) == ESP_OK)
        nvs_commit(prefs);
}
int battery_mv() {
    int raw, mv;
    if (!calibration || adc_oneshot_read(adc, ADC_CHANNEL_3, &raw) ||
        adc_cali_raw_to_voltage(calibration, raw, &mv))
        return -1;
    return mv * 2;
}
void board_init() {
    gpio_set_direction(GPIO_NUM_46, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_46, 1);
    gpio_set_direction(GPIO_NUM_42, GPIO_MODE_OUTPUT);
    gpio_set_direction(GPIO_NUM_45, GPIO_MODE_OUTPUT);
    write_lamp(0, 0, 0);
    ledc_timer_config_t timer{};
    timer.speed_mode = LEDC_LOW_SPEED_MODE;
    timer.duty_resolution = LEDC_TIMER_10_BIT;
    timer.freq_hz = 5000;
    timer.clk_cfg = LEDC_AUTO_CLK;
    ESP_ERROR_CHECK(ledc_timer_config(&timer));
    ledc_channel_config_t ch{};
    ch.gpio_num = 15;
    ch.speed_mode = LEDC_LOW_SPEED_MODE;
    ESP_ERROR_CHECK(ledc_channel_config(&ch));
    nvs_open("system", NVS_READWRITE, &prefs);
    uint8_t value;
    set_level(true, prefs && !nvs_get_u8(prefs, "light", &value) ? value : 65, false);
    set_level(false, prefs && !nvs_get_u8(prefs, "volume", &value) ? value : 35, false);
    adc_oneshot_unit_init_cfg_t a{};
    a.unit_id = ADC_UNIT_1;
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&a, &adc));
    adc_oneshot_chan_cfg_t c{};
    c.atten = ADC_ATTEN_DB_12;
    c.bitwidth = ADC_BITWIDTH_DEFAULT;
    ESP_ERROR_CHECK(adc_oneshot_config_channel(adc, ADC_CHANNEL_3, &c));
    adc_cali_curve_fitting_config_t cal{};
    cal.unit_id = a.unit_id;
    cal.chan = ADC_CHANNEL_3;
    cal.atten = c.atten;
    cal.bitwidth = c.bitwidth;
    ESP_ERROR_CHECK_WITHOUT_ABORT(adc_cali_create_scheme_curve_fitting(&cal, &calibration));
    inputs();
    display();
}
void storage_init() {
    auto b = bus(41, 38, 40);
    ESP_ERROR_CHECK(spi_bus_initialize(SPI3_HOST, &b, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK_WITHOUT_ABORT(mount_card());
}
bool mounted() { return card != nullptr; }
esp_err_t mount_card(bool repair) {
    if (card) {
        auto previous = card;
        card = nullptr;
        auto err = esp_vfs_fat_sdcard_unmount("/sdcard", previous);
        if (err != ESP_OK)
            return err;
    }
    sdmmc_host_t h = SDSPI_HOST_DEFAULT();
    h.slot = SPI3_HOST;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = SPI3_HOST;
    slot.gpio_cs = GPIO_NUM_39;
    esp_vfs_fat_mount_config_t m{};
    m.max_files = 12;
    m.disk_status_check_enable = true;
    m.format_if_mount_failed = repair;
    return esp_vfs_fat_sdspi_mount("/sdcard", &h, &slot, &m, &card);
}
esp_err_t format_card() {
    if (!card) {
        auto e = mount_card(true);
        if (e != ESP_OK)
            return e;
    }
    return esp_vfs_fat_sdcard_format("/sdcard", card);
}
} // namespace embed
