#include "platform.hpp"
#include <algorithm>
#include <cstring>
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_codec_dev_defaults.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace embed {
namespace {
constexpr unsigned history = 8;
struct Stream {
    App *owner;
    int16_t frames[3][480];
    unsigned head, count, underruns;
    uint32_t cursor;
    bool reading, playing;
};
Stream *streams;
constexpr unsigned stream_count = 4;
int16_t mic[history][160];
uint32_t captured;
SemaphoreHandle_t mutex;
i2s_chan_handle_t tx, rx;
bool can_play, can_record;
struct Guard {
    Guard() { xSemaphoreTake(mutex, portMAX_DELAY); }
    ~Guard() { xSemaphoreGive(mutex); }
};
Stream *stream(App &app) {
    Stream *empty = nullptr;
    for (unsigned i = 0; i < stream_count; ++i) {
        if (streams[i].owner == &app)
            return &streams[i];
        if (!streams[i].owner)
            empty = &streams[i];
    }
    if (empty)
        empty->owner = &app;
    return empty;
}
esp_err_t init_channel(bool input) {
    i2s_chan_config_t channel = I2S_CHANNEL_DEFAULT_CONFIG(input ? I2S_NUM_1 : I2S_NUM_0, I2S_ROLE_MASTER);
    auto err = i2s_new_channel(&channel, input ? nullptr : &tx, input ? &rx : nullptr);
    if (err)
        return err;
    i2s_std_config_t cfg{};
    cfg.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(input ? 16000U : 48000U);
    cfg.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
    cfg.gpio_cfg.mclk = input ? GPIO_NUM_48 : GPIO_NUM_NC;
    cfg.gpio_cfg.bclk = input ? GPIO_NUM_47 : GPIO_NUM_7;
    cfg.gpio_cfg.ws = input ? GPIO_NUM_21 : GPIO_NUM_5;
    cfg.gpio_cfg.din = input ? GPIO_NUM_14 : GPIO_NUM_NC;
    cfg.gpio_cfg.dout = input ? GPIO_NUM_NC : GPIO_NUM_6;
    auto handle = input ? rx : tx;
    err = i2s_channel_init_std_mode(handle, &cfg);
    return err ? err : i2s_channel_enable(handle);
}
esp_err_t init_mic() {
    i2c_master_bus_config_t bus{};
    bus.i2c_port = I2C_NUM_0;
    bus.sda_io_num = GPIO_NUM_18;
    bus.scl_io_num = GPIO_NUM_8;
    bus.clk_source = I2C_CLK_SRC_DEFAULT;
    bus.glitch_ignore_cnt = 7;
    bus.flags.enable_internal_pullup = true;
    i2c_master_bus_handle_t master;
    auto err = i2c_new_master_bus(&bus, &master);
    if (err)
        return err;
    audio_codec_i2c_cfg_t ctrl{};
    ctrl.addr = ES7210_CODEC_DEFAULT_ADDR;
    ctrl.bus_handle = master;
    es7210_codec_cfg_t config{};
    config.ctrl_if = audio_codec_new_i2c_ctrl(&ctrl);
    config.mic_selected = ES7210_SEL_MIC3 | ES7210_SEL_MIC4;
    if (!config.ctrl_if)
        return ESP_FAIL;
    auto adc = es7210_codec_new(&config);
    if (!adc)
        return ESP_FAIL;
    esp_codec_dev_sample_info_t fs{};
    fs.bits_per_sample = 16;
    fs.sample_rate = 16000;
    fs.channel = 2;
    if (adc->set_fs(adc, &fs) || adc->enable(adc, true) || adc->set_mic_gain(adc, 36))
        return ESP_FAIL;
    return ESP_OK;
}
void record_task(void *) {
    int16_t stereo[320];
    while (true) {
        size_t n = 0;
        if (i2s_channel_read(rx, stereo, sizeof(stereo), &n, 100) || n != sizeof(stereo))
            continue;
        Guard lock;
        for (unsigned i = 0; i < 160; ++i)
            mic[captured % history][i] = stereo[2 * i];
        ++captured;
    }
}
void output_task(void *) {
    int32_t mixed[480];
    int16_t stereo[960];
    for (;;) {
        memset(mixed, 0, sizeof(mixed));
        {
            Guard lock;
            for (unsigned i = 0; i < stream_count; ++i) {
                auto &s = streams[i];
                if (!s.count) {
                    if (s.playing)
                        ++s.underruns;
                    continue;
                }
                auto &f = s.frames[s.head];
                for (unsigned j = 0; j < 480; ++j)
                    mixed[j] += f[j];
                s.head = (s.head + 1) % 3;
                --s.count;
            }
        }
        int level = volume();
        for (unsigned j = 0; j < 480; ++j)
            stereo[2 * j] = stereo[2 * j + 1] = std::clamp<int32_t>(mixed[j] * level / 100, -32768, 32767);
        size_t n;
        if (i2s_channel_write(tx, stereo, sizeof(stereo), &n, 100) != ESP_OK)
            vTaskDelay(1);
    }
}
} // namespace
void audio_init() {
    streams = static_cast<Stream *>(heap_caps_calloc(stream_count, sizeof(Stream), MALLOC_CAP_SPIRAM));
    mutex = xSemaphoreCreateMutex();
    if (!streams || !mutex)
        return;
    if (!ESP_ERROR_CHECK_WITHOUT_ABORT(init_channel(false)))
        can_play = xTaskCreate(output_task, "pcm_out", 7168, nullptr, 7, nullptr) == pdPASS;
    if (!ESP_ERROR_CHECK_WITHOUT_ABORT(init_channel(true)) && !ESP_ERROR_CHECK_WITHOUT_ABORT(init_mic()))
        can_record = xTaskCreate(record_task, "pcm_in", 4096, nullptr, 7, nullptr) == pdPASS;
}
bool play(App &app, const int16_t *frame, unsigned gain) {
    if (!can_play || !frame)
        return false;
    Guard lock;
    auto s = stream(app);
    if (!s || s->count == 3)
        return false;
    auto &dest = s->frames[(s->head + s->count) % 3];
    gain = std::min(gain, 100U);
    for (unsigned i = 0; i < 480; ++i)
        dest[i] = int32_t(frame[i]) * int(gain) / 100;
    ++s->count;
    s->playing = true;
    return true;
}
bool capture(App &app, int16_t *frame, uint32_t &lost) {
    lost = 0;
    if (!can_record || !frame)
        return false;
    Guard lock;
    auto s = stream(app);
    if (!s)
        return false;
    if (!s->reading) {
        s->reading = true;
        s->cursor = captured;
    }
    auto available = captured - s->cursor;
    if (!available)
        return false;
    if (available > history) {
        lost = available - history;
        s->cursor += lost;
    }
    memcpy(frame, mic[s->cursor++ % history], sizeof(mic[0]));
    return true;
}
void audio_release(App &app) {
    if (!streams || !mutex)
        return;
    Guard lock;
    for (unsigned i = 0; i < stream_count; ++i)
        if (streams[i].owner == &app)
            memset(&streams[i], 0, sizeof(Stream));
}
} // namespace embed
