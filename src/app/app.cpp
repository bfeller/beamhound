#include "app/app.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace bh {
namespace {

constexpr int kW = 320;
constexpr int kH = 170;
constexpr int kTitleH = 20;
constexpr int kFooterH = 15;

// Palette (matches the launcher icon).
const lv_color_t kBg = lv_color_hex(0x111214);
const lv_color_t kCard = lv_color_hex(0x1E2024);
const lv_color_t kCardHi = lv_color_hex(0x3A2A10);
const lv_color_t kText = lv_color_hex(0xEDEDED);
const lv_color_t kDim = lv_color_hex(0x8A8F98);
const lv_color_t kAmber = lv_color_hex(0xF5A623);
const lv_color_t kPink = lv_color_hex(0xFF3D7F);
const lv_color_t kCyan = lv_color_hex(0x29C5F6);
const lv_color_t kGreen = lv_color_hex(0x4CD964);

lv_obj_t* box(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t bg, int radius = 0)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_size(o, w, h);
    lv_obj_set_style_bg_color(o, bg, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

lv_obj_t* text(lv_obj_t* parent, const std::string& s, const lv_font_t* font, lv_color_t color, int x, int y,
               int w = -1, lv_text_align_t align = LV_TEXT_ALIGN_LEFT,
               lv_label_long_mode_t mode = LV_LABEL_LONG_MODE_DOTS)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, s.c_str());
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_style_text_align(l, align, 0);
    if (w > 0) {
        lv_obj_set_width(l, w);
        lv_label_set_long_mode(l, mode);
    }
    lv_obj_set_pos(l, x, y);
    return l;
}

std::string shortGroup(Group g)
{
    switch (g) {
    case Group::TV: return "TV";
    case Group::Projector: return "Projector";
    case Group::Receiver: return "Receiver";
    case Group::Soundbar: return "Soundbar";
    case Group::SetTop: return "Box";
    case Group::Disc: return "Player";
    case Group::Climate: return "Fan";
    case Group::Lights: return "Lights";
    default: return "Device";
    }
}

std::string upperLabel(Btn b)
{
    std::string s = btnLabel(b);
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

// Remote screen layout: 4 rows x 5 tiles, D-pad in the right three columns.
struct Tile {
    Btn btn;
    const char* label;
    const char* key;
};
constexpr Tile kTiles[20] = {
    {Btn::Power, LV_SYMBOL_POWER " Power", "P"}, {Btn::Input, "Input", "T"},  {Btn::Menu, "Menu", "U"},
    {Btn::Home, LV_SYMBOL_HOME " Home", "H"},    {Btn::Mute, LV_SYMBOL_MUTE " Mute", "M"},
    {Btn::VolUp, "Vol +", "Q"},                   {Btn::ChUp, "Ch +", "W"},    {Btn::Back, "Back", LV_SYMBOL_BACKSPACE},
    {Btn::Up, LV_SYMBOL_UP, "I"},                 {Btn::Info, "Info", "N"},
    {Btn::VolDown, "Vol -", "A"},                 {Btn::ChDown, "Ch -", "S"},  {Btn::Left, LV_SYMBOL_LEFT, "J"},
    {Btn::Ok, "OK", LV_SYMBOL_NEW_LINE},          {Btn::Right, LV_SYMBOL_RIGHT, "L"},
    {Btn::PlayPause, LV_SYMBOL_PLAY " Play", "Spc"}, {Btn::Stop, LV_SYMBOL_STOP " Stop", "X"},
    {Btn::Exit, "Exit", "E"},                     {Btn::Down, LV_SYMBOL_DOWN, "K"},
    {Btn::None, LV_SYMBOL_LIST " All", "B"},
};

}  // namespace

App::App(std::string dataDir, std::string profileDir) : dataDir_(std::move(dataDir)), profileDir_(std::move(profileDir))
{
}

void App::init()
{
    indexOk_ = ix_.load(dataDir_ + "/index.bin", indexError_);
    std::string e1, e2, e3;
    const bool rxOk = rx_.open(e1);
    const bool txOk = tx_.open(e2);
    kb_.open(e3);
    if (!rxOk) hwError_ = "IR receiver: " + e1;
    else if (!txOk) hwError_ = "IR transmitter: " + e2;
    loadProfiles();

    lv_obj_t* scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, kBg, 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    root_ = box(scr, 0, 0, kW, kH, kBg);
    anim_ = lv_timer_create(
        [](lv_timer_t* t) {
            auto* self = static_cast<App*>(lv_timer_get_user_data(t));
            self->animPhase_ = (self->animPhase_ + 1) % 4;
            for (size_t i = 0; i < self->arcs_.size(); ++i) {
                const bool on = static_cast<int>(i) < self->animPhase_;
                lv_obj_set_style_arc_opa(self->arcs_[i], on ? LV_OPA_COVER : LV_OPA_20, LV_PART_INDICATOR);
            }
        },
        300, this);
    show(Screen::Home);
}

void App::tick()
{
    for (KeyEvent e = kb_.poll(); e.key != Key::None; e = kb_.poll()) onKey(e);
    std::vector<uint32_t> burst;
    if (rx_.poll(burst)) onIr(burst);
    if (statusLabel_ && statusUntil_ && nowMs() > statusUntil_) {
        statusUntil_ = 0;
        lv_label_set_text(statusLabel_, "");
    }
}

void App::flash(const std::string& msg)
{
    status_ = msg;
    statusUntil_ = nowMs() + 2500;
    if (statusLabel_) lv_label_set_text(statusLabel_, msg.c_str());
}

void App::show(Screen s)
{
    screen_ = s;
    render();
}

void App::render()
{
    lv_obj_clean(root_);
    arcs_.clear();
    statusLabel_ = nullptr;
    std::fill(std::begin(tiles_), std::end(tiles_), nullptr);
    switch (screen_) {
    case Screen::Home: buildHome(); break;
    case Screen::Listen: buildListen(); break;
    case Screen::Question: buildQuestion(); break;
    case Screen::Result: buildResult(); break;
    case Screen::Remote: buildRemote(); break;
    case Screen::AllButtons: buildAllButtons(); break;
    case Screen::Message: buildMessage(); break;
    case Screen::ConfirmDelete: buildConfirmDelete(); break;
    }
}

// ------------------------------------------------------------------ widgets
lv_obj_t* App::titleBar(const std::string& title, const std::string& right)
{
    lv_obj_t* bar = box(root_, 0, 0, kW, kTitleH, kCard);
    text(bar, title, &lv_font_montserrat_14, kAmber, 6, 2, 200);
    if (!right.empty()) text(bar, right, &lv_font_montserrat_10, kDim, 200, 5, 114, LV_TEXT_ALIGN_RIGHT);
    return bar;
}

void App::footer(const std::string& hint)
{
    lv_obj_t* bar = box(root_, 0, kH - kFooterH, kW, kFooterH, kCard);
    text(bar, hint, &lv_font_montserrat_10, kDim, 4, 1, kW - 8, LV_TEXT_ALIGN_CENTER);
}

void App::list(const std::vector<std::string>& items, int top, int height, bool numbered)
{
    constexpr int rowH = 19;
    const int visible = std::max(1, height / rowH);
    const int count = static_cast<int>(items.size());
    int first = std::clamp(sel_ - visible / 2, 0, std::max(0, count - visible));
    for (int i = first; i < count && i < first + visible; ++i) {
        const bool hi = i == sel_;
        lv_obj_t* row = box(root_, 4, top + (i - first) * rowH, kW - 8, rowH - 2, hi ? kCardHi : kBg, 4);
        if (hi) {
            lv_obj_set_style_border_color(row, kAmber, 0);
            lv_obj_set_style_border_width(row, 1, 0);
        }
        std::string label = items[i];
        if (numbered && i < 9) label = std::to_string(i + 1) + "  " + label;
        text(row, label, &lv_font_montserrat_12, hi ? kText : kDim, 6, 2, kW - 24);
    }
    if (count > visible) {
        const int barH = std::max(10, height * visible / count);
        const int barY = top + (height - barH) * first / std::max(1, count - visible);
        box(root_, kW - 3, barY, 2, barH, kDim, 1);
    }
}

void App::listNav(const KeyEvent& e, int count)
{
    if (count <= 0) return;
    const bool up = e.key == Key::Up || (e.key == Key::Char && (e.ch == 'w' || e.ch == 'i'));
    const bool down = e.key == Key::Down || (e.key == Key::Char && (e.ch == 's' || e.ch == 'k'));
    if (up) sel_ = (sel_ + count - 1) % count;
    if (down) sel_ = (sel_ + 1) % count;
    if (up || down) render();
}

// ------------------------------------------------------------------ screens
void App::buildHome()
{
    titleBar("Beamhound", hwError_.empty() ? "IR ready" : "IR error");
    std::vector<std::string> items{LV_SYMBOL_EYE_OPEN "  Sniff out a remote"};
    for (const auto& p : profiles_) items.push_back(LV_SYMBOL_SETTINGS "  " + p.name);
    sel_ = std::clamp(sel_, 0, static_cast<int>(items.size()) - 1);
    list(items, kTitleH + 4, 110, false);
    if (!indexOk_) {
        text(root_, "Database missing: " + indexError_, &lv_font_montserrat_10, kPink, 6, 134, kW - 12);
    } else if (!hwError_.empty()) {
        text(root_, hwError_, &lv_font_montserrat_10, kPink, 6, 134, kW - 12);
    } else if (profiles_.empty()) {
        text(root_, "Saved remotes will appear here.", &lv_font_montserrat_10, kDim, 6, 134, kW - 12);
    }
    footer(profiles_.empty() ? "Enter select  " LV_SYMBOL_UP LV_SYMBOL_DOWN " move  Esc hold: exit"
                             : "Enter select  D delete  " LV_SYMBOL_UP LV_SYMBOL_DOWN " move  Esc hold: exit");
}

void App::buildListen()
{
    titleBar("Sniffing...", "");
    // Beam arcs on the left, echoing the icon.
    const lv_color_t colors[3] = {kPink, kPink, kCyan};
    for (int i = 0; i < 3; ++i) {
        lv_obj_t* a = lv_arc_create(root_);
        const int r = 18 + i * 14;
        lv_obj_set_size(a, r * 2, r * 2);
        lv_obj_set_pos(a, 40 - r, 80 - r);
        lv_arc_set_bg_angles(a, 0, 360);
        lv_arc_set_angles(a, 315, 45);
        lv_obj_remove_style(a, nullptr, LV_PART_KNOB);
        lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
        lv_obj_set_style_arc_width(a, 6, LV_PART_INDICATOR);
        lv_obj_set_style_arc_color(a, colors[i], LV_PART_INDICATOR);
        lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
        lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
        arcs_.push_back(a);
    }
    text(root_, "Press", &lv_font_montserrat_14, kDim, 110, 40);
    text(root_, LV_SYMBOL_POWER " POWER", &lv_font_montserrat_24, kAmber, 110, 58);
    text(root_, "on your remote, pointed at\nthe Cardputer's IR window.", &lv_font_montserrat_12, kDim, 110, 92, 205,
         LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_MODE_WRAP);
    statusLabel_ = text(root_, status_, &lv_font_montserrat_10, kPink, 6, 136, kW - 12);
    footer("Esc back");
}

void App::buildQuestion()
{
    char right[64];
    std::snprintf(right, sizeof right, "%zu code sets left", matcher_.codesetCount());
    titleBar("Narrowing down", right);
    const int top = kTitleH + 4;
    std::string hint = "B best guess  " LV_SYMBOL_BACKSPACE " undo  Esc cancel";
    switch (q_.kind) {
    case Question::Kind::PressButton:
        text(root_, "Now press", &lv_font_montserrat_14, kDim, 10, top + 8);
        text(root_, upperLabel(q_.btn), &lv_font_montserrat_24, kAmber, 10, top + 28, kW - 20);
        text(root_, "on your remote. Its code tells similar remotes apart.", &lv_font_montserrat_12, kDim, 10,
             top + 62, kW - 20, LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_MODE_WRAP);
        hint = "N my remote has no " + std::string(btnLabel(q_.btn)) + "  B best guess  Esc cancel";
        break;
    case Question::Kind::Category: {
        text(root_, "What kind of device is it?", &lv_font_montserrat_14, kText, 8, top);
        auto items = q_.options;
        items.push_back("Not sure");
        list(items, top + 20, 108, true);
        break;
    }
    case Question::Kind::Brand: {
        text(root_, "Which brand is it?", &lv_font_montserrat_14, kText, 8, top);
        auto items = q_.options;
        items.push_back("Other / not listed");
        items.push_back("Not sure");
        list(items, top + 20, 108, true);
        break;
    }
    case Question::Kind::TestSend:
        if (!testSent_) {
            text(root_, "Point me at the device", &lv_font_montserrat_16, kText, 10, top + 6);
            text(root_, "then press Enter to send", &lv_font_montserrat_12, kDim, 10, top + 32);
            text(root_, upperLabel(q_.btn), &lv_font_montserrat_24, kCyan, 10, top + 52, kW - 20);
            hint = "Enter send  S skip  B best guess  Esc cancel";
        } else {
            text(root_, "Did the device react", &lv_font_montserrat_16, kText, 10, top + 6);
            text(root_, "to " + upperLabel(q_.btn) + "?", &lv_font_montserrat_16, kText, 10, top + 28, kW - 20);
            text(root_, "Y  yes      N  no      Enter  send again", &lv_font_montserrat_12, kAmber, 10, top + 62);
            hint = "S not sure  " LV_SYMBOL_BACKSPACE " undo  Esc cancel";
        }
        break;
    case Question::Kind::PickRemote:
        text(root_, "Pick one to try (T = test power)", &lv_font_montserrat_14, kText, 8, top);
        list(q_.options, top + 20, 108, true);
        hint = "Enter choose  T test  Esc cancel";
        break;
    default: break;
    }
    statusLabel_ = text(root_, status_, &lv_font_montserrat_10, kPink, 6, kH - kFooterH - 14, kW - 12);
    footer(hint);
}

void App::buildResult()
{
    titleBar("Found it!", "");
    const Remote& r = ix_.remotes[resultRemote_];
    const std::string brand = ix_.str(r.brand);
    text(root_, brand + " " + shortGroup(r.group), &lv_font_montserrat_24, kAmber, 10, 30, kW - 20);
    if (ix_.str(r.model) != brand)
        text(root_, "Codes from: " + ix_.str(r.model), &lv_font_montserrat_12, kDim, 10, 64, kW - 20);
    char info[96];
    const size_t n = matcher_.remoteCount();
    if (n > 1) std::snprintf(info, sizeof info, "%zu remotes in the database share these codes.", n);
    else std::snprintf(info, sizeof info, "Matched one remote in the database.");
    text(root_, info, &lv_font_montserrat_12, kDim, 10, 82, kW - 20);
    if (matcher_.fuzzy())
        text(root_, "Matched by signal shape (approximate).", &lv_font_montserrat_10, kPink, 10, 102, kW - 20);
    text(root_, "Enter  save & open remote      T  test power", &lv_font_montserrat_12, kText, 10, 122, kW - 20);
    statusLabel_ = text(root_, status_, &lv_font_montserrat_10, kPink, 6, kH - kFooterH - 14, kW - 12);
    footer(LV_SYMBOL_BACKSPACE " keep narrowing   Esc cancel");
}

void App::buildRemote()
{
    lv_obj_t* bar = titleBar(current_.name, "");
    statusLabel_ = text(bar, status_, &lv_font_montserrat_10, kGreen, 150, 5, 164, LV_TEXT_ALIGN_RIGHT);
    constexpr int cols = 5, tw = 62, th = 30, gap = 2, top = kTitleH + 3;
    for (int i = 0; i < 20; ++i) {
        const Tile& t = kTiles[i];
        bool have = t.btn == Btn::None;
        for (const auto& b : buttons_)
            if (classifyButton(b.name) == t.btn ||
                (t.btn == Btn::Power && classifyButton(b.name) == Btn::PowerOn) ||
                (t.btn == Btn::PlayPause && classifyButton(b.name) == Btn::Play))
                have = true;
        const int x = 2 + (i % cols) * (tw + gap), y = top + (i / cols) * (th + gap);
        lv_obj_t* tile = box(root_, x, y, tw, th, have ? kCard : kBg, 5);
        lv_obj_set_style_border_color(tile, have ? lv_color_hex(0x2C2F35) : lv_color_hex(0x1A1C20), 0);
        lv_obj_set_style_border_width(tile, 1, 0);
        text(tile, t.label, &lv_font_montserrat_12, have ? kText : lv_color_hex(0x40444B), 0, 2, tw,
             LV_TEXT_ALIGN_CENTER);
        text(tile, t.key, &lv_font_montserrat_10, have ? kAmber : lv_color_hex(0x40444B), 0, 16, tw,
             LV_TEXT_ALIGN_CENTER);
        tiles_[i] = tile;
    }
    footer("B all buttons  0-9 digits  Z/C rew/ffwd  O off  Esc home");
}

void App::buildAllButtons()
{
    lv_obj_t* bar = titleBar(current_.name, "");
    statusLabel_ = text(bar, status_.empty() ? std::to_string(buttons_.size()) + " buttons" : status_,
                        &lv_font_montserrat_10, kGreen, 150, 5, 164, LV_TEXT_ALIGN_RIGHT);
    std::vector<std::string> items;
    for (const auto& b : buttons_) items.push_back(b.name);
    if (items.empty()) items.push_back("(no buttons)");
    sel_ = std::clamp(sel_, 0, static_cast<int>(items.size()) - 1);
    list(items, kTitleH + 3, 130, false);
    footer("Enter send  " LV_SYMBOL_UP LV_SYMBOL_DOWN " move  Esc back");
}

void App::buildMessage()
{
    titleBar(msgTitle_, "");
    text(root_, msgBody_, &lv_font_montserrat_12, kText, 10, 30, kW - 20, LV_TEXT_ALIGN_LEFT,
         LV_LABEL_LONG_MODE_WRAP);
    footer("Enter try again   Esc home");
}

void App::buildConfirmDelete()
{
    titleBar("Delete remote?", "");
    const int idx = sel_ - 1;
    const std::string name = idx >= 0 && idx < static_cast<int>(profiles_.size()) ? profiles_[idx].name : "";
    text(root_, "Remove \"" + name + "\" from saved remotes?", &lv_font_montserrat_14, kText, 10, 40, kW - 20,
         LV_TEXT_ALIGN_LEFT, LV_LABEL_LONG_MODE_WRAP);
    text(root_, "Y  delete      N  keep", &lv_font_montserrat_14, kAmber, 10, 100);
    footer("");
}

// ------------------------------------------------------------------ flow
void App::nextQuestion()
{
    if (matcher_.done() || matcher_.remoteCount() <= 1) {
        finishMatch();
        return;
    }
    q_ = matcher_.next();
    testSent_ = false;
    sel_ = 0;
    show(Screen::Question);
}

void App::finishMatch()
{
    resultRemote_ = matcher_.bestRemote();
    status_.clear();
    show(Screen::Result);
}

void App::onIr(const std::vector<uint32_t>& burst)
{
    if (screen_ == Screen::Listen) {
        if (!indexOk_) return;
        switch (matcher_.start(burst)) {
        case Matcher::StartResult::Matched:
            status_.clear();
            nextQuestion();
            break;
        case Matcher::StartResult::NotDecoded:
            flash("Couldn't read that. Try again, a little closer.");
            break;
        case Matcher::StartResult::NoMatch:
            msgTitle_ = "Not in my database";
            msgBody_ = "I heard " + describe(*matcher_.heard()) +
                       " but no remote in the database uses it for power.\n\nMake sure you pressed POWER, then try "
                       "again.";
            messageReturn_ = Screen::Listen;
            show(Screen::Message);
            break;
        }
        return;
    }
    if (screen_ == Screen::Question && q_.kind == Question::Kind::PressButton) {
        std::string msg;
        matcher_.answerPress(q_.btn, burst, msg);
        status_ = msg;
        if (!msg.empty() && msg.rfind("Couldn't", 0) == 0) {
            flash(msg);
            return;
        }
        nextQuestion();
    }
}

bool App::sendTest()
{
    const auto keys = ix_.keysFor(q_.testRemote, q_.btn);
    const auto buttons = loadIrFile(dataDir_ + "/irdb/" + ix_.str(ix_.remotes[q_.testRemote].path));
    for (const auto& b : buttons) {
        if (classifyButton(b.name) != q_.btn) continue;
        const uint64_t k = buttonKey(b);
        if (std::find(keys.begin(), keys.end(), k) == keys.end()) continue;
        std::string err;
        rx_.mute(500);
        if (!tx_.send(buttonTimings(b), err)) {
            flash(err);
            return false;
        }
        return true;
    }
    flash("Couldn't find that signal to send.");
    return false;
}

bool App::sendIndex(size_t idx)
{
    if (idx >= buttons_.size()) return false;
    toggle_ = !toggle_;
    setToggle(toggle_);
    std::string err;
    rx_.mute(500);
    if (!tx_.send(buttonTimings(buttons_[idx]), err)) {
        flash(err);
        return false;
    }
    flash("Sent " + buttons_[idx].name);
    return true;
}

bool App::sendButton(Btn b)
{
    for (size_t i = 0; i < buttons_.size(); ++i)
        if (classifyButton(buttons_[i].name) == b) return sendIndex(i);
    // Fallbacks for remotes that only have the discrete variant.
    if (b == Btn::Power) return sendButton(Btn::PowerOn);
    if (b == Btn::PlayPause) return sendButton(Btn::Play);
    flash(std::string("This remote has no ") + btnLabel(b));
    return false;
}

void App::openProfile(const Profile& p)
{
    current_ = p;
    buttons_ = loadIrFile(dataDir_ + "/irdb/" + p.path);
    status_.clear();
    sel_ = 0;
    show(Screen::Remote);
}

void App::onKey(const KeyEvent& e)
{
    if (e.key == Key::LongEsc) {
        quit_ = true;
        return;
    }
    switch (screen_) {
    case Screen::Home: {
        const int count = 1 + static_cast<int>(profiles_.size());
        if (e.key == Key::Esc) return;  // hold Esc to exit
        if (e.key == Key::Char && e.ch >= '1' && e.ch <= '9' && e.ch - '1' < count) {
            sel_ = e.ch - '1';
            render();
        }
        if (e.key == Key::Enter) {
            if (sel_ == 0) {
                status_.clear();
                show(Screen::Listen);
            } else {
                openProfile(profiles_[sel_ - 1]);
            }
            return;
        }
        if (e.key == Key::Char && e.ch == 'd' && sel_ > 0) {
            show(Screen::ConfirmDelete);
            return;
        }
        listNav(e, count);
        return;
    }
    case Screen::ConfirmDelete:
        if (e.key == Key::Char && e.ch == 'y' && sel_ > 0) {
            profiles_.erase(profiles_.begin() + (sel_ - 1));
            saveProfiles();
            sel_ = 0;
        }
        if ((e.key == Key::Char && (e.ch == 'y' || e.ch == 'n')) || e.key == Key::Esc) show(Screen::Home);
        return;
    case Screen::Listen:
        if (e.key == Key::Esc) show(Screen::Home);
        return;
    case Screen::Message:
        if (e.key == Key::Enter) {
            status_.clear();
            show(messageReturn_);
        }
        if (e.key == Key::Esc) show(Screen::Home);
        return;
    case Screen::Question: {
        if (e.key == Key::Esc) {
            show(Screen::Home);
            return;
        }
        if (e.key == Key::Backspace) {
            if (matcher_.undo()) {
                status_ = "Undone.";
                nextQuestion();
            } else {
                status_.clear();
                show(Screen::Listen);
            }
            return;
        }
        if (e.key == Key::Char && e.ch == 'b') {
            finishMatch();
            return;
        }
        switch (q_.kind) {
        case Question::Kind::PressButton:
            if (e.key == Key::Char && e.ch == 'n') {
                matcher_.answerNoSuchButton(q_.btn);
                status_.clear();
                nextQuestion();
            }
            return;
        case Question::Kind::Category:
        case Question::Kind::Brand: {
            const bool brand = q_.kind == Question::Kind::Brand;
            const int n = static_cast<int>(q_.options.size());
            const int count = n + (brand ? 2 : 1);
            int pick = -1;
            if (e.key == Key::Enter) pick = sel_;
            if (e.key == Key::Char && e.ch >= '1' && e.ch <= '9' && e.ch - '1' < count) pick = e.ch - '1';
            if (pick < 0) {
                listNav(e, count);
                return;
            }
            status_.clear();
            if (pick < n) {
                if (brand) matcher_.answerBrand(q_.values[pick]);
                else matcher_.answerCategory(static_cast<Group>(q_.values[pick]));
            } else if (brand && pick == n) {
                matcher_.answerBrandOther(q_.values);
            } else {
                matcher_.skip(q_);
            }
            nextQuestion();
            return;
        }
        case Question::Kind::TestSend:
            if (e.key == Key::Enter) {
                if (sendTest()) {
                    testSent_ = true;
                    render();
                }
                return;
            }
            if (e.key == Key::Char && e.ch == 's') {
                matcher_.skip(q_);
                nextQuestion();
                return;
            }
            if (testSent_ && e.key == Key::Char && (e.ch == 'y' || e.ch == 'n')) {
                matcher_.answerTest(q_.testRemote, q_.btn, e.ch == 'y');
                status_.clear();
                nextQuestion();
            }
            return;
        case Question::Kind::PickRemote: {
            const int count = static_cast<int>(q_.values.size());
            if (e.key == Key::Char && e.ch == 't' && sel_ < count) {
                const auto buttons = loadIrFile(dataDir_ + "/irdb/" + ix_.str(ix_.remotes[q_.values[sel_]].path));
                for (const auto& b : buttons)
                    if (isPowerClass(classifyButton(b.name))) {
                        std::string err;
                        rx_.mute(500);
                        tx_.send(buttonTimings(b), err);
                        flash(err.empty() ? "Sent power" : err);
                        break;
                    }
                return;
            }
            int pick = -1;
            if (e.key == Key::Enter) pick = sel_;
            if (e.key == Key::Char && e.ch >= '1' && e.ch <= '9' && e.ch - '1' < count) pick = e.ch - '1';
            if (pick >= 0) {
                matcher_.answerPick(q_.values[pick]);
                finishMatch();
                return;
            }
            listNav(e, count);
            return;
        }
        default: return;
        }
    }
    case Screen::Result: {
        if (e.key == Key::Esc) {
            show(Screen::Home);
            return;
        }
        if (e.key == Key::Backspace) {
            nextQuestion();
            if (screen_ == Screen::Result) flash("Nothing left to ask; this is the best match.");
            return;
        }
        if (e.key == Key::Char && e.ch == 't') {
            const auto buttons = loadIrFile(dataDir_ + "/irdb/" + ix_.str(ix_.remotes[resultRemote_].path));
            for (const auto& b : buttons)
                if (isPowerClass(classifyButton(b.name))) {
                    std::string err;
                    rx_.mute(500);
                    tx_.send(buttonTimings(b), err);
                    flash(err.empty() ? "Sent " + b.name : err);
                    break;
                }
            return;
        }
        if (e.key == Key::Enter) {
            const Remote& r = ix_.remotes[resultRemote_];
            Profile p{ix_.str(r.brand) + " " + shortGroup(r.group), ix_.str(r.path)};
            // Keep names unique ("Sony TV 2").
            std::string base = p.name;
            for (int n = 2; std::any_of(profiles_.begin(), profiles_.end(),
                                        [&](const Profile& x) { return x.name == p.name && x.path != p.path; });
                 ++n)
                p.name = base + " " + std::to_string(n);
            if (std::none_of(profiles_.begin(), profiles_.end(), [&](const Profile& x) { return x.path == p.path; })) {
                profiles_.push_back(p);
                saveProfiles();
            }
            openProfile(p);
        }
        return;
    }
    case Screen::Remote: {
        if (e.key == Key::Esc) {
            sel_ = 0;
            show(Screen::Home);
            return;
        }
        Btn b = Btn::None;
        switch (e.key) {
        case Key::Enter: b = Btn::Ok; break;
        case Key::Backspace: b = Btn::Back; break;
        case Key::Space: b = Btn::PlayPause; break;
        case Key::Tab: b = Btn::Input; break;
        case Key::Up: b = Btn::Up; break;
        case Key::Down: b = Btn::Down; break;
        case Key::Left: b = Btn::Left; break;
        case Key::Right: b = Btn::Right; break;
        case Key::Mute: b = Btn::Mute; break;
        case Key::VolUp: b = Btn::VolUp; break;
        case Key::VolDown: b = Btn::VolDown; break;
        case Key::PlayPause: b = Btn::PlayPause; break;
        case Key::Home: b = Btn::Home; break;
        case Key::Char:
            if (e.ch >= '0' && e.ch <= '9') {
                b = static_cast<Btn>(static_cast<int>(Btn::Num0) + (e.ch - '0'));
                break;
            }
            switch (e.ch) {
            case 'p': b = Btn::Power; break;
            case 'o': b = Btn::PowerOff; break;
            case 't': b = Btn::Input; break;
            case 'u': b = Btn::Menu; break;
            case 'h': b = Btn::Home; break;
            case 'm': b = Btn::Mute; break;
            case 'q': b = Btn::VolUp; break;
            case 'a': b = Btn::VolDown; break;
            case 'w': b = Btn::ChUp; break;
            case 's': b = Btn::ChDown; break;
            case 'i': b = Btn::Up; break;
            case 'k': b = Btn::Down; break;
            case 'j': b = Btn::Left; break;
            case 'l': b = Btn::Right; break;
            case 'n': b = Btn::Info; break;
            case 'g': b = Btn::Guide; break;
            case 'e': b = Btn::Exit; break;
            case 'x': b = Btn::Stop; break;
            case 'z': b = Btn::Rewind; break;
            case 'c': b = Btn::FastForward; break;
            case 'b':
                sel_ = 0;
                status_.clear();
                show(Screen::AllButtons);
                return;
            default: break;
            }
            break;
        default: break;
        }
        if (b == Btn::None) return;
        sendButton(b);
        for (int i = 0; i < 20; ++i)
            if (tiles_[i] && kTiles[i].btn == b) {
                lv_obj_set_style_border_color(tiles_[i], kAmber, 0);
                lv_obj_set_style_border_width(tiles_[i], 2, 0);
            }
        return;
    }
    case Screen::AllButtons: {
        if (e.key == Key::Esc) {
            status_.clear();
            show(Screen::Remote);
            return;
        }
        if (e.key == Key::Enter) {
            sendIndex(static_cast<size_t>(sel_));
            return;
        }
        listNav(e, static_cast<int>(buttons_.size()));
        return;
    }
    }
}

// ------------------------------------------------------------------ profiles
void App::loadProfiles()
{
    profiles_.clear();
    std::ifstream f(profileDir_ + "/profiles.tsv");
    std::string line;
    while (std::getline(f, line)) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) continue;
        profiles_.push_back({line.substr(0, tab), line.substr(tab + 1)});
    }
}

void App::saveProfiles()
{
    std::error_code ec;
    std::filesystem::create_directories(profileDir_, ec);
    const std::string tmp = profileDir_ + "/profiles.tsv.tmp";
    {
        std::ofstream f(tmp);
        for (const auto& p : profiles_) f << p.name << '\t' << p.path << '\n';
    }
    std::filesystem::rename(tmp, profileDir_ + "/profiles.tsv", ec);
}

}  // namespace bh
