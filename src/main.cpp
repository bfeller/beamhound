// Beamhound entry point.
//   Device build: draws to the Cardputer LCD framebuffer, reads the keyboard and IR.
//   Desktop build (BH_DESKTOP): headless; `beamhound --demo <dir>` runs a scripted
//   walkthrough against the real index and writes a PPM screenshot per step.
#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <thread>

#include "app/app.hpp"
#include "lvgl.h"

using namespace bh;

static volatile std::sig_atomic_t g_stop = 0;
static uint32_t tickMs() { return static_cast<uint32_t>(nowMs()); }

static std::string dataDir()
{
    if (const char* d = std::getenv("BEAMHOUND_DATA")) return d;
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.local/share/beamhound";
}

static std::string profileDir()
{
    const char* home = std::getenv("HOME");
    return std::string(home ? home : "/tmp") + "/.local/share/beamhound";
}

#if !defined(BH_DESKTOP)

// The Cardputer LCD is a small SPI panel; find its framebuffer rather than assuming fb0.
static std::string findFramebuffer()
{
    if (const char* e = std::getenv("LV_LINUX_FBDEV_DEVICE")) return e;
    std::ifstream f("/proc/fb");
    int n;
    std::string name;
    while (f >> n >> name)
        if (name.find("st7789") != std::string::npos || name.find("mipi") != std::string::npos ||
            name.find("panel") != std::string::npos)
            return "/dev/fb" + std::to_string(n);
    return "/dev/fb0";
}

int main()
{
    std::signal(SIGINT, [](int) { g_stop = 1; });
    std::signal(SIGTERM, [](int) { g_stop = 1; });
    lv_init();
    lv_tick_set_cb(tickMs);
    lv_display_t* disp = lv_linux_fbdev_create();
    const std::string fb = findFramebuffer();
    if (lv_linux_fbdev_set_file(disp, fb.c_str()) != LV_RESULT_OK) {
        std::fprintf(stderr, "beamhound: cannot open framebuffer %s\n", fb.c_str());
        return 1;
    }
    App app(dataDir(), profileDir());
    app.init();
    while (!g_stop && !app.quit()) {
        const uint32_t wait = lv_timer_handler();
        app.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(std::min<uint32_t>(wait, 10)));
    }
    // Leave a clean black screen for the launcher to repaint.
    lv_obj_clean(lv_screen_active());
    lv_obj_set_style_bg_color(lv_screen_active(), lv_color_black(), 0);
    lv_refr_now(disp);
    return 0;
}

#else  // BH_DESKTOP

static uint16_t g_fb[320 * 170];

static void flushCb(lv_display_t* disp, const lv_area_t* a, uint8_t* px)
{
    const uint16_t* src = reinterpret_cast<const uint16_t*>(px);
    for (int y = a->y1; y <= a->y2; ++y)
        for (int x = a->x1; x <= a->x2; ++x) g_fb[y * 320 + x] = *src++;
    lv_display_flush_ready(disp);
}

static void shot(lv_display_t* disp, const std::string& path)
{
    lv_obj_invalidate(lv_screen_active());
    lv_refr_now(disp);
    std::ofstream f(path, std::ios::binary);
    f << "P6\n320 170\n255\n";
    for (uint16_t p : g_fb) {
        const unsigned char rgb[3] = {static_cast<unsigned char>(((p >> 11) & 0x1F) * 255 / 31),
                                      static_cast<unsigned char>(((p >> 5) & 0x3F) * 255 / 63),
                                      static_cast<unsigned char>((p & 0x1F) * 255 / 31)};
        f.write(reinterpret_cast<const char*>(rgb), 3);
    }
    std::printf("wrote %s\n", path.c_str());
}

static void pump(App& app, int ms = 60)
{
    const int64_t end = nowMs() + ms;
    while (nowMs() < end) {
        lv_timer_handler();
        app.tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

int main(int argc, char** argv)
{
    if (argc >= 3 && std::strcmp(argv[1], "--setup-test") == 0) {
        // First-run flow against an empty BEAMHOUND_DATA: press Enter, snapshot each phase.
        const std::string out = argv[2];
        lv_init();
        lv_tick_set_cb(tickMs);
        lv_display_t* disp = lv_display_create(320, 170);
        static uint16_t buf[320 * 170];
        lv_display_set_buffers(disp, buf, nullptr, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
        lv_display_set_flush_cb(disp, flushCb);
        App app(dataDir(), profileDir());
        app.init();
        pump(app);
        shot(disp, out + "/setup-0-idle.ppm");
        app.kb().inject({Key::Enter, 0});
        int n = 1;
        std::string last;
        for (int i = 0; i < 6000 && !app.index().remotes.size(); ++i) {
            pump(app, 100);
            if (i % 20 == 0) shot(disp, out + "/setup-" + std::to_string(n++) + ".ppm");
        }
        pump(app, 200);
        shot(disp, out + "/setup-done.ppm");
        std::printf("remotes loaded: %zu\n", app.index().remotes.size());
        return app.index().remotes.empty() ? 1 : 0;
    }
    if (argc < 3 || std::strcmp(argv[1], "--demo") != 0) {
        std::fprintf(stderr, "usage: %s --demo <outdir> [remote-path-substring] | --setup-test <outdir>\n", argv[0]);
        return 2;
    }
    const std::string out = argv[2];
    const std::string want = argc > 3 ? argv[3] : "TVs/Samsung";
    lv_init();
    lv_tick_set_cb(tickMs);
    lv_display_t* disp = lv_display_create(320, 170);
    static uint16_t buf[320 * 170];
    lv_display_set_buffers(disp, buf, nullptr, sizeof buf, LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp, flushCb);

    App app(dataDir(), profileDir());
    app.init();
    pump(app);
    int step = 0;
    auto snap = [&](const char* name) {
        char p[256];
        std::snprintf(p, sizeof p, "%s/%02d-%s.ppm", out.c_str(), ++step, name);
        shot(disp, p);
    };
    snap("home");
    app.kb().inject({Key::Enter, 0});
    pump(app);
    snap("listen");

    // Pick the demo "user's" remote and press its power button.
    const Index& ix = app.index();
    std::vector<IrButton> truth;
    uint32_t truthId = 0;
    for (uint32_t r = 0; r < ix.remotes.size() && truth.empty(); ++r) {
        if (ix.str(ix.remotes[r].path).find(want) == std::string::npos) continue;
        truthId = r;
        truth = loadIrFile(app.dataDir() + "/irdb/" + ix.str(ix.remotes[r].path));
        std::printf("demo remote: %s\n", ix.str(ix.remotes[r].path).c_str());
    }
    auto press = [&](Btn b) {
        for (const auto& btn : truth)
            if (classifyButton(btn.name) == b && buttonKey(btn)) {
                app.rx().inject(buttonTimings(btn).timings);
                return true;
            }
        return false;
    };
    press(Btn::Power);
    pump(app);
    for (int i = 0; i < 8 && app.matcher().remoteCount() > 0 && !app.matcher().done(); ++i) {
        const Question q = app.matcher().next();
        snap("question");
        switch (q.kind) {
        case Question::Kind::PressButton:
            if (!press(q.btn)) app.kb().inject({Key::Char, 'n'});
            break;
        case Question::Kind::Category:
        case Question::Kind::Brand:
        case Question::Kind::PickRemote:
            app.kb().inject({Key::Char, '1'});
            break;
        case Question::Kind::TestSend:
            app.kb().inject({Key::Enter, 0});
            pump(app);
            snap("test-sent");
            app.kb().inject({Key::Char, 'y'});
            break;
        default: break;
        }
        pump(app);
    }
    snap("result");
    app.kb().inject({Key::Enter, 0});
    pump(app);
    snap("remote");
    app.kb().inject({Key::Char, 'q'});
    pump(app);
    snap("remote-sent");
    app.kb().inject({Key::Char, 'b'});
    pump(app);
    snap("all-buttons");
    app.kb().inject({Key::Esc, 0});
    app.kb().inject({Key::Esc, 0});
    pump(app);
    snap("home-saved");

    // Second walkthrough: the same device found without its remote (brand code search).
    app.kb().inject({Key::Down, 0});
    pump(app);
    snap("home-nr");
    app.kb().inject({Key::Enter, 0});
    pump(app);
    snap("brand-search");
    const std::string brandName = ix.str(ix.remotes[truthId].brand);
    for (size_t i = 0; i < brandName.size() && i < 4; ++i) {
        const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(brandName[i])));
        app.kb().inject(c == ' ' ? KeyEvent{Key::Space, 0} : KeyEvent{Key::Char, c});
    }
    pump(app);
    snap("brand-typed");
    app.kb().inject({Key::Enter, 0});
    pump(app);
    const Question cq = app.matcher().categoryQuestion();
    if (cq.kind == Question::Kind::Category) {
        snap("nr-category");
        const auto& v = cq.values;
        const auto it = std::find(v.begin(), v.end(), static_cast<uint32_t>(ix.remotes[truthId].group));
        const int idx = it == v.end() ? static_cast<int>(v.size()) : static_cast<int>(it - v.begin());  // "Not sure"
        app.kb().inject({Key::Char, static_cast<char>('1' + std::min(idx, 8))});
        pump(app);
    }
    snap("code-search");
    auto reacts = [&](const PowerCode& c) {
        for (const Sig* s = ix.remoteSigsBegin(truthId); s != ix.remoteSigsEnd(truthId); ++s)
            if (c.key && s->key == c.key && (s->btn == Btn::Power || s->btn == Btn::PowerOff)) return true;
        return false;
    };
    const auto codes = app.matcher().powerCodes();
    size_t hit = 0;
    while (hit + 1 < codes.size() && !reacts(codes[hit])) ++hit;
    for (size_t i = 0; i <= hit; ++i) {
        app.kb().inject({Key::Space, 0});
        pump(app);
        if (i == 0) snap("code-sent");
    }
    snap("code-hit");
    app.kb().inject({Key::Enter, 0});
    pump(app);
    snap("code-confirm");
    app.kb().inject({Key::Enter, 0});
    pump(app);
    snap("code-confirm-sent");
    app.kb().inject({Key::Char, 'y'});
    pump(app);
    for (int i = 0; i < 8 && !app.matcher().done(); ++i) {
        const Question q = app.matcher().next();
        snap("nr-question");
        if (q.kind == Question::Kind::TestSend) {
            const auto sent = ix.keysFor(q.testRemote, q.btn), mine = ix.keysFor(truthId, q.btn);
            const bool yes = std::any_of(sent.begin(), sent.end(), [&](uint64_t k) {
                return std::find(mine.begin(), mine.end(), k) != mine.end();
            });
            app.kb().inject({Key::Enter, 0});
            app.kb().inject({Key::Char, yes ? 'y' : 'n'});
        } else {
            app.kb().inject({Key::Char, '1'});
        }
        pump(app);
    }
    snap("nr-result");
    return 0;
}

#endif
