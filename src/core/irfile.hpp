// Flipper .ir file parsing and button-name classification.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "core/ir.hpp"

namespace bh {

struct IrButton {
    std::string name;
    bool parsed = false;
    IrCode code;  // valid when parsed (or when a raw signal decoded)
    IrRaw raw;    // valid when !parsed
};

// Parse a Flipper "IR signals file". Malformed entries are skipped.
std::vector<IrButton> parseIrFile(std::string_view text);
std::vector<IrButton> loadIrFile(const std::string& path);

// The best transmit-ready representation of a button.
IrRaw buttonTimings(const IrButton& b);
// Canonical key for matching: from the parsed code, or by decoding raw timings. 0 if unknown.
uint64_t buttonKey(const IrButton& b);

// Standard buttons used for matching and for the remote-control screen.
enum class Btn : uint8_t {
    None = 0,
    Power, PowerOn, PowerOff,
    VolUp, VolDown, Mute,
    ChUp, ChDown,
    Input, Menu, Ok, Up, Down, Left, Right, Back, Home, Guide, Info, Exit,
    Play, Pause, PlayPause, Stop, Rewind, FastForward, Next, Prev,
    Num0, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
    Speed, Oscillate, Timer, Brightness_Up, Brightness_Down,
    Count
};

Btn classifyButton(std::string_view name);
const char* btnLabel(Btn b);  // short human label, e.g. "Vol +"
bool isPowerClass(Btn b);

}  // namespace bh
