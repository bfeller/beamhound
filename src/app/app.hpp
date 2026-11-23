// Beamhound application: screens, navigation and state.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "core/index.hpp"
#include "core/matcher.hpp"
#include "lvgl.h"
#include "platform/hw.hpp"

namespace bh {

struct Profile {
    std::string name;
    std::string path;  // .ir file relative to <data>/irdb
};

class App {
public:
    App(std::string dataDir, std::string profileDir);
    void init();     // load data, open hardware, build first screen
    void tick();     // poll keyboard/IR; call regularly from the main loop
    bool quit() const { return quit_; }

    // Desktop/demo hooks.
    IrReceiver& rx() { return rx_; }
    Keyboard& kb() { return kb_; }
    const Index& index() const { return ix_; }
    const Matcher& matcher() const { return matcher_; }
    std::string dataDir() const { return dataDir_; }

private:
    enum class Screen { Home, Listen, Question, Result, Remote, AllButtons, Message, ConfirmDelete };

    void show(Screen s);
    void render();
    void onKey(const KeyEvent& e);
    void onIr(const std::vector<uint32_t>& burst);

    // screen builders
    void buildHome();
    void buildListen();
    void buildQuestion();
    void buildResult();
    void buildRemote();
    void buildAllButtons();
    void buildMessage();
    void buildConfirmDelete();

    // helpers
    lv_obj_t* titleBar(const std::string& title, const std::string& right);
    void footer(const std::string& hint);
    void list(const std::vector<std::string>& items, int top, int height, bool numbered);
    void listNav(const KeyEvent& e, int count);
    void nextQuestion();
    void finishMatch();
    void openProfile(const Profile& p);
    bool sendButton(Btn b);
    bool sendIndex(size_t idx);
    bool sendTest();
    void loadProfiles();
    void saveProfiles();
    void flash(const std::string& msg);

    std::string dataDir_, profileDir_;
    Index ix_;
    std::string indexError_;
    bool indexOk_ = false;
    Matcher matcher_{ix_};
    IrReceiver rx_;
    IrTransmitter tx_;
    Keyboard kb_;
    std::string hwError_;

    Screen screen_ = Screen::Home;
    Screen messageReturn_ = Screen::Home;
    bool quit_ = false;
    int sel_ = 0;
    std::string status_;
    int64_t statusUntil_ = 0;
    Question q_;
    bool testSent_ = false;
    std::string msgTitle_, msgBody_;

    std::vector<Profile> profiles_;
    Profile current_;
    std::vector<IrButton> buttons_;
    uint32_t resultRemote_ = 0;
    bool toggle_ = false;

    lv_obj_t* root_ = nullptr;
    lv_obj_t* statusLabel_ = nullptr;
    lv_obj_t* tiles_[20] = {};
    lv_timer_t* anim_ = nullptr;
    int animPhase_ = 0;
    std::vector<lv_obj_t*> arcs_;
};

}  // namespace bh
