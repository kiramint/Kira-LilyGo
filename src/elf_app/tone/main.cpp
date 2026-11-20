#include "platform.hpp"
using namespace embed;
namespace {
unsigned phase;
bool enabled = true;
void sound(App &app) {
    if (!enabled)
        return;
    int16_t pcm[480];
    for (auto &s : pcm) {
        phase = (phase + 440) % 48000;
        s = phase < 24000 ? 1000 : -1000;
    }
    play(app, pcm);
}
void show(App &app) { lv_label_set_text(lv_label_create(app.root), "440 Hz tone\nClick to mute / resume"); }
void input(App &, Input key) {
    if (key == Input::click)
        enabled = !enabled;
}
} // namespace
extern "C" int app_main(int, char **argv) {
    auto &app = *reinterpret_cast<App *>(argv[0]);
    app.foreground = show;
    app.tick = sound;
    app.input = input;
    return 0;
}
