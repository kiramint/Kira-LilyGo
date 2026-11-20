#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include "esp_err.h"
#include "lvgl.h"

namespace embed {
enum class Input { left, right, click, home };
struct App {
    lv_obj_t *root; // Valid only in foreground callbacks on the System UI task.
    lv_group_t *focus;
    void (*foreground)(App &);
    void (*background)(App &);
    void (*tick)(App &); // Cooperative worker, runs in foreground and background.
    void (*input)(App &, Input);
    bool (*stop)(App &); // Worker context. Join direct tasks, close files and sockets.
};
// ELF entry receives its App as argv[0] and installs the callbacks above.
bool present(App &, int x, int y, int w, int h, const uint16_t *pixels, int stride);
bool lamp(App &, uint8_t r, uint8_t g, uint8_t b);
esp_err_t claim_pin(App &, int gpio);
bool play(App &, const int16_t *frame, unsigned gain = 100); // 480 samples
bool capture(App &, int16_t *frame, uint32_t &lost);         // 160 samples
void audio_release(App &);
void audio_init();
void board_init();
void storage_init();
esp_err_t mount_card(bool repair = false);
esp_err_t format_card();
bool mounted();
int battery_mv();
int brightness();
int volume();
void set_level(bool backlight, int percent, bool persist);
bool read_input(Input &);
void release_pins();
void write_lamp(uint8_t r, uint8_t g, uint8_t b);
void network_init();
void ui_start();
} // namespace embed
