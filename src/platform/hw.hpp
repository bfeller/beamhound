// Hardware access: LIRC IR receive/transmit and the evdev keyboard.
// On non-Linux/desktop builds these are inert stubs (see BH_DESKTOP).
#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/ir.hpp"

namespace bh {

class IrReceiver {
public:
    ~IrReceiver();
    bool open(std::string& error);
    // Non-blocking. Returns true and fills `burst` when a complete signal burst has arrived.
    bool poll(std::vector<uint32_t>& burst);
    // Discard anything pending and ignore input for `ms` (e.g. our own transmission's echo).
    void mute(int ms);
    bool ok() const { return fd_ >= 0; }
    std::string device;

    // Desktop/demo builds: queue a fake capture.
    void inject(const std::vector<uint32_t>& burst) { injected_ = burst; }

private:
    int fd_ = -1;
    std::vector<uint32_t> cur_;
    bool started_ = false;
    int64_t mutedUntilMs_ = 0;
    std::vector<uint32_t> injected_;
};

class IrTransmitter {
public:
    ~IrTransmitter();
    bool open(std::string& error);
    bool send(const IrRaw& raw, std::string& error);
    bool ok() const { return fd_ >= 0; }
    std::string device;

private:
    int fd_ = -1;
    uint32_t features_ = 0;
};

enum class Key : uint16_t {
    None = 0, Esc, LongEsc, Enter, Backspace, Space, Tab, Up, Down, Left, Right,
    Mute, VolUp, VolDown, PlayPause, Rewind, FastForward, BrightDown, BrightUp, Home, Char,
};

struct KeyEvent {
    Key key = Key::None;
    char ch = 0;  // for Key::Char: lowercase letter or digit
};

class Keyboard {
public:
    ~Keyboard();
    bool open(std::string& error);
    // Non-blocking; returns Key::None when nothing is pending.
    KeyEvent poll();
    void inject(KeyEvent e) { injected_.push_back(e); }

private:
    int fd_ = -1;
    int64_t escDownMs_ = -1;
    bool escLongFired_ = false;
    std::vector<KeyEvent> injected_;
};

int64_t nowMs();

}  // namespace bh
