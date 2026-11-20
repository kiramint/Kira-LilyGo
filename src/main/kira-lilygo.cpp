#include "platform.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#include "esp_flash.h"
#include "driver/gpio.h"
#include "nvs_flash.h"

extern "C" void app_main(void) {
    gpio_set_direction(GPIO_NUM_46, GPIO_MODE_OUTPUT);
    gpio_set_level(GPIO_NUM_46, 1);
    ESP_ERROR_CHECK(nvs_flash_init());
    uint32_t flash{};
    esp_flash_get_size(nullptr, &flash);
    ESP_LOGI("boot", "Flash %lu, PSRAM %u, internal free %u / largest %u", (unsigned long)flash,
             (unsigned)esp_psram_get_size(),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    embed::board_init();
    embed::storage_init();
    embed::audio_init();
    embed::network_init();
    embed::ui_start();
}
