#include "core/irfile.hpp"

#include <cctype>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unordered_map>

namespace bh {
namespace {

std::string_view trim(std::string_view s)
{
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

// "07 00 00 00" -> 0x00000007 (little-endian byte list)
uint32_t parseLeBytes(std::string_view v)
{
    uint32_t out = 0;
    int idx = 0;
    std::string tok;
    std::istringstream in{std::string(v)};
    while (in >> tok && idx < 4) {
        out |= static_cast<uint32_t>(std::strtoul(tok.c_str(), nullptr, 16) & 0xFF) << (8 * idx);
        ++idx;
    }
    return out;
}

void finish(std::vector<IrButton>& out, IrButton& cur, bool& have)
{
    if (have && !cur.name.empty()) {
        const bool ok = cur.parsed ? cur.code.proto != Proto::Unknown : cur.raw.timings.size() >= 3;
        if (ok) out.push_back(std::move(cur));
    }
    cur = IrButton{};
    have = false;
}

std::string normalizeName(std::string_view in)
{
    std::string s;
    for (size_t i = 0; i < in.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(in[i]);
        if (std::isalnum(ch)) {
            s.push_back(static_cast<char>(std::toupper(ch)));
        } else if (ch == '+') {
            s += "_PLUS_";
        } else if (ch == '-') {
            const bool atEnd = i + 1 >= in.size() || !std::isalnum(static_cast<unsigned char>(in[i + 1]));
            s += atEnd ? "_MINUS_" : "_";
        } else {
            s.push_back('_');
        }
    }
    std::string out;
    for (char c : s)
        if (!(c == '_' && (out.empty() || out.back() == '_'))) out.push_back(c);
    while (!out.empty() && out.back() == '_') out.pop_back();
    for (const char* p : {"TV_", "KEY_", "BTN_", "BUTTON_", "IR_"}) {
        const std::string pre = p;
        if (out.size() > pre.size() && out.compare(0, pre.size(), pre) == 0) out = out.substr(pre.size());
    }
    return out;
}

const std::unordered_map<std::string, Btn>& nameTable()
{
    static const std::unordered_map<std::string, Btn> table = [] {
        std::unordered_map<std::string, Btn> t;
        auto add = [&](Btn b, std::initializer_list<const char*> names) {
            for (const char* n : names) t.emplace(n, b);
        };
        add(Btn::Power, {"POWER", "PWR", "POWER_TOGGLE", "POWER_ON_OFF", "POWER_ONOFF", "ON_OFF", "ONOFF",
                         "STANDBY", "POWER_STANDBY", "TOGGLE_POWER", "POWER_ON_STANDBY", "POWER_KEY",
                         "POWERTOGGLE", "POWER_ON_OFF_TOGGLE", "STANDBY_ON", "ON_STANDBY"});
        add(Btn::PowerOn, {"POWER_ON", "PWR_ON", "ON", "POWERON", "DISCRETE_ON", "POWER_ON_ASSERT"});
        add(Btn::PowerOff, {"POWER_OFF", "PWR_OFF", "OFF", "POWEROFF", "DISCRETE_OFF", "POWER_OFF_ASSERT"});
        add(Btn::VolUp, {"VOL_UP", "VOLUME_UP", "VOL_PLUS", "VOLUME_PLUS", "VOLUP", "V_PLUS", "VOLUMEUP"});
        add(Btn::VolDown, {"VOL_DN", "VOL_DOWN", "VOLUME_DOWN", "VOL_MINUS", "VOLUME_MINUS", "VOLDOWN", "VOLDN",
                           "V_MINUS", "VOLUMEDOWN", "VOL_DWN", "VOLUME_DN"});
        add(Btn::Mute, {"MUTE", "MUTING", "AUDIO_MUTE", "MUTE_TOGGLE", "MUTE_ON_OFF"});
        add(Btn::ChUp, {"CH_UP", "CH_PLUS", "CHANNEL_UP", "CHANNEL_PLUS", "CHUP", "CH_NEXT", "CHANNEL_NEXT",
                        "P_PLUS", "PROG_PLUS", "PROG_UP", "PROGRAM_UP", "PROGRAM_PLUS"});
        add(Btn::ChDown, {"CH_DN", "CH_DOWN", "CH_MINUS", "CHANNEL_DOWN", "CHANNEL_MINUS", "CHDN", "CHDOWN",
                          "CH_PREV", "CHANNEL_PREV", "P_MINUS", "PROG_MINUS", "PROG_DOWN", "PROGRAM_DOWN",
                          "PROGRAM_MINUS"});
        add(Btn::Input, {"INPUT", "SOURCE", "SRC", "AV", "TV_AV", "INPUT_SELECT", "SELECT_INPUT", "INPUT_SOURCE",
                         "AV_TV", "TV_VIDEO", "INPUTS", "SOURCES", "INPUT_SEL", "SOURCE_SELECT"});
        add(Btn::Menu, {"MENU", "SETUP", "SETTINGS", "MAIN_MENU"});
        add(Btn::Ok, {"OK", "ENTER", "SELECT", "CONFIRM", "OK_ENTER", "CENTER"});
        add(Btn::Up, {"UP", "CURSOR_UP", "ARROW_UP", "NAV_UP", "UP_ARROW", "DPAD_UP"});
        add(Btn::Down, {"DOWN", "CURSOR_DOWN", "ARROW_DOWN", "NAV_DOWN", "DOWN_ARROW", "DPAD_DOWN", "DN"});
        add(Btn::Left, {"LEFT", "CURSOR_LEFT", "ARROW_LEFT", "NAV_LEFT", "LEFT_ARROW", "DPAD_LEFT"});
        add(Btn::Right, {"RIGHT", "CURSOR_RIGHT", "ARROW_RIGHT", "NAV_RIGHT", "RIGHT_ARROW", "DPAD_RIGHT"});
        add(Btn::Back, {"BACK", "RETURN", "RTN"});
        add(Btn::Home, {"HOME", "SMART_HUB"});
        add(Btn::Guide, {"GUIDE", "EPG"});
        add(Btn::Info, {"INFO", "DISPLAY", "OSD"});
        add(Btn::Exit, {"EXIT"});
        add(Btn::Play, {"PLAY"});
        add(Btn::Pause, {"PAUSE"});
        add(Btn::PlayPause, {"PLAY_PAUSE", "PLAYPAUSE", "PLAY_PAUSE_TOGGLE"});
        add(Btn::Stop, {"STOP"});
        add(Btn::Rewind, {"REW", "REWIND", "RWD", "FAST_REWIND", "FR"});
        add(Btn::FastForward, {"FF", "FWD", "FAST_FORWARD", "FFWD"});
        add(Btn::Next, {"NEXT", "SKIP_FWD", "SKIP_NEXT", "SKIP_PLUS", "NEXT_TRACK", "TRACK_PLUS"});
        add(Btn::Prev, {"PREV", "PREVIOUS", "SKIP_BACK", "SKIP_PREV", "SKIP_MINUS", "PREV_TRACK", "TRACK_MINUS"});
        add(Btn::Speed, {"SPEED", "FAN_SPEED", "SPEED_UP", "WIND_SPEED"});
        add(Btn::Oscillate, {"OSCILLATE", "OSC", "SWING", "ROTATE", "OSCILLATION"});
        add(Btn::Timer, {"TIMER"});
        add(Btn::Brightness_Up, {"BRIGHTNESS_UP", "BRIGHT_UP", "BRIGHTER", "BRIGHTNESS_PLUS"});
        add(Btn::Brightness_Down, {"BRIGHTNESS_DOWN", "BRIGHT_DOWN", "DIMMER", "BRIGHTNESS_MINUS", "DIM"});
        const char* digits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
        for (int d = 0; d < 10; ++d) {
            const Btn b = static_cast<Btn>(static_cast<int>(Btn::Num0) + d);
            const std::string ds = digits[d];
            for (const char* pre : {"", "NUM_", "NUM", "DIGIT_", "NUMBER_", "N"}) t.emplace(std::string(pre) + ds, b);
        }
        return t;
    }();
    return table;
}

}  // namespace

std::vector<IrButton> parseIrFile(std::string_view text)
{
    std::vector<IrButton> out;
    IrButton cur;
    bool have = false;
    size_t pos = 0;
    while (pos <= text.size()) {
        const size_t eol = text.find('\n', pos);
        std::string_view line = trim(text.substr(pos, eol == std::string_view::npos ? std::string_view::npos : eol - pos));
        pos = eol == std::string_view::npos ? text.size() + 1 : eol + 1;
        if (line.empty() || line.front() == '#') {
            if (!line.empty()) finish(out, cur, have);
            continue;
        }
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) continue;
        const std::string_view key = trim(line.substr(0, colon));
        const std::string_view val = trim(line.substr(colon + 1));
        if (key == "name") {
            finish(out, cur, have);
            cur.name = std::string(val);
            have = true;
        } else if (!have) {
            continue;
        } else if (key == "type") {
            cur.parsed = (val == "parsed");
        } else if (key == "protocol") {
            cur.code.proto = protoFromName(val);
        } else if (key == "address") {
            cur.code.address = parseLeBytes(val);
        } else if (key == "command") {
            cur.code.command = parseLeBytes(val);
        } else if (key == "frequency") {
            cur.raw.carrierHz = static_cast<uint32_t>(std::strtoul(std::string(val).c_str(), nullptr, 10));
        } else if (key == "data") {
            std::istringstream in{std::string(val)};
            long v;
            while (in >> v)
                if (v > 0) cur.raw.timings.push_back(static_cast<uint32_t>(v));
        }
    }
    finish(out, cur, have);
    return out;
}

std::vector<IrButton> loadIrFile(const std::string& path)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    return parseIrFile(ss.str());
}

IrRaw buttonTimings(const IrButton& b) { return b.parsed ? encode(b.code) : b.raw; }

uint64_t buttonKey(const IrButton& b)
{
    if (b.parsed) return canonicalKey(b.code);
    if (auto c = decode(b.raw.timings)) return canonicalKey(*c);
    return 0;
}

Btn classifyButton(std::string_view name)
{
    const auto& t = nameTable();
    const auto it = t.find(normalizeName(name));
    return it == t.end() ? Btn::None : it->second;
}

bool isPowerClass(Btn b) { return b == Btn::Power || b == Btn::PowerOn || b == Btn::PowerOff; }

const char* btnLabel(Btn b)
{
    switch (b) {
    case Btn::Power: return "Power";
    case Btn::PowerOn: return "Power on";
    case Btn::PowerOff: return "Power off";
    case Btn::VolUp: return "Vol +";
    case Btn::VolDown: return "Vol -";
    case Btn::Mute: return "Mute";
    case Btn::ChUp: return "Ch +";
    case Btn::ChDown: return "Ch -";
    case Btn::Input: return "Input";
    case Btn::Menu: return "Menu";
    case Btn::Ok: return "OK";
    case Btn::Up: return "Up";
    case Btn::Down: return "Down";
    case Btn::Left: return "Left";
    case Btn::Right: return "Right";
    case Btn::Back: return "Back";
    case Btn::Home: return "Home";
    case Btn::Guide: return "Guide";
    case Btn::Info: return "Info";
    case Btn::Exit: return "Exit";
    case Btn::Play: return "Play";
    case Btn::Pause: return "Pause";
    case Btn::PlayPause: return "Play/Pause";
    case Btn::Stop: return "Stop";
    case Btn::Rewind: return "Rewind";
    case Btn::FastForward: return "Fast fwd";
    case Btn::Next: return "Next";
    case Btn::Prev: return "Prev";
    case Btn::Speed: return "Speed";
    case Btn::Oscillate: return "Oscillate";
    case Btn::Timer: return "Timer";
    case Btn::Brightness_Up: return "Bright +";
    case Btn::Brightness_Down: return "Bright -";
    default: break;
    }
    if (b >= Btn::Num0 && b <= Btn::Num9) {
        static const char* d[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
        return d[static_cast<int>(b) - static_cast<int>(Btn::Num0)];
    }
    return "?";
}

}  // namespace bh
