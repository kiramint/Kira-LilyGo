#include "platform.hpp"
using namespace embed;
namespace {
int16_t pcm[160];
uint16_t pixels[160 * 48];
void scope(App &app) {
    uint32_t lost;
    if (!capture(app, pcm, lost) || !app.root)
        return;
    for (auto &p : pixels)
        p = 0x0841;
    for (int x = 0; x < 160; ++x)
        pixels[(24 + pcm[x] / 1400) * 160 + x] = 0x07ff;
    present(app, 20, 60, 160, 48, pixels, 160);
}
void show(App &app) { lv_label_set_text(lv_label_create(app.root), "Microphone / 16 kHz"); }
} // namespace
extern "C" int app_main(int, char **argv) {
    auto &app = *reinterpret_cast<App *>(argv[0]);
    app.foreground = show;
    app.tick = scope;
    return 0;
}
