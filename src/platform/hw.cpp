#include "platform/hw.hpp"

#include <chrono>
#include <cstring>

#if !defined(BH_DESKTOP)
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <linux/lirc.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cerrno>
#endif

namespace bh {

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

#if !defined(BH_DESKTOP)
namespace {

// Find the LIRC device that has the wanted feature bit (receiver vs transmitter).
int openLirc(uint32_t wantFeature, int flags, std::string& path, uint32_t& features, std::string& error)
{
    for (int n = 0; n < 8; ++n) {
        const std::string p = "/dev/lirc" + std::to_string(n);
        const int fd = ::open(p.c_str(), flags | O_CLOEXEC);
        if (fd < 0) {
            if (errno == EACCES) error = "no permission for " + p + " (user must be in group 'video')";
            continue;
        }
        uint32_t f = 0;
        if (::ioctl(fd, LIRC_GET_FEATURES, &f) == 0 && (f & wantFeature)) {
            path = p;
            features = f;
            return fd;
        }
        ::close(fd);
    }
    if (error.empty()) error = "no suitable /dev/lirc device found";
    return -1;
}

}  // namespace

IrReceiver::~IrReceiver()
{
    if (fd_ >= 0) ::close(fd_);
}

bool IrReceiver::open(std::string& error)
{
    uint32_t features = 0;
    fd_ = openLirc(LIRC_CAN_REC_MODE2, O_RDONLY | O_NONBLOCK, device, features, error);
    if (fd_ < 0) return false;
    uint32_t mode = LIRC_MODE_MODE2;
    ::ioctl(fd_, LIRC_SET_REC_MODE, &mode);
    // A 25 ms silence ends a burst; the kernel reports it as a timeout sample.
    uint32_t timeout = 25000;
    if (features & LIRC_CAN_SET_REC_TIMEOUT) ::ioctl(fd_, LIRC_SET_REC_TIMEOUT, &timeout);
    uint32_t on = 1;
    ::ioctl(fd_, LIRC_SET_REC_TIMEOUT_REPORTS, &on);
    return true;
}

void IrReceiver::mute(int ms)
{
    mutedUntilMs_ = nowMs() + ms;
    cur_.clear();
    started_ = false;
}

bool IrReceiver::poll(std::vector<uint32_t>& burst)
{
    if (!injected_.empty()) {
        burst.swap(injected_);
        injected_.clear();
        return true;
    }
    if (fd_ < 0) return false;
    uint32_t buf[256];
    bool done = false;
    for (;;) {
        const ssize_t n = ::read(fd_, buf, sizeof buf);
        if (n <= 0) break;
        if (nowMs() < mutedUntilMs_) continue;  // drain and drop
        for (ssize_t i = 0; i < n / 4 && !done; ++i) {
            const uint32_t v = buf[i];
            const uint32_t val = LIRC_VALUE(v);
            const uint32_t m = LIRC_MODE2(v);
            if (m == LIRC_MODE2_PULSE) {
                if (started_ && cur_.size() % 2 == 1) cur_.back() += val;  // merge
                else cur_.push_back(val);
                started_ = true;
            } else if (m == LIRC_MODE2_SPACE) {
                if (!started_) continue;
                if (val > 40000) { done = true; break; }
                if (cur_.size() % 2 == 0) cur_.back() += val;
                else cur_.push_back(val);
            } else if (m == LIRC_MODE2_TIMEOUT) {
                if (started_) done = true;
            }
        }
        if (done) break;
    }
    if (!done) return false;
    if (cur_.size() % 2 == 0 && !cur_.empty()) cur_.pop_back();  // end on a mark
    const bool useful = cur_.size() >= 3;
    if (useful) burst = cur_;
    cur_.clear();
    started_ = false;
    // Ignore the rest of this key press (repeat frames) for a moment.
    mutedUntilMs_ = nowMs() + 250;
    return useful;
}

IrTransmitter::~IrTransmitter()
{
    if (fd_ >= 0) ::close(fd_);
}

bool IrTransmitter::open(std::string& error)
{
    fd_ = openLirc(LIRC_CAN_SEND_PULSE, O_WRONLY, device, features_, error);
    if (fd_ < 0) return false;
    uint32_t mode = LIRC_MODE_PULSE;
    ::ioctl(fd_, LIRC_SET_SEND_MODE, &mode);
    return true;
}

bool IrTransmitter::send(const IrRaw& raw, std::string& error)
{
    if (fd_ < 0) {
        error = "IR transmitter not available";
        return false;
    }
    if (raw.timings.empty()) {
        error = "nothing to send";
        return false;
    }
    std::vector<unsigned int> t(raw.timings.begin(), raw.timings.end());
    if (t.size() % 2 == 0) t.pop_back();  // LIRC requires an odd count (ends with a pulse)
    if (features_ & LIRC_CAN_SET_SEND_CARRIER) {
        uint32_t c = raw.carrierHz ? raw.carrierHz : 38000;
        ::ioctl(fd_, LIRC_SET_SEND_CARRIER, &c);
    }
    if (features_ & LIRC_CAN_SET_SEND_DUTY_CYCLE) {
        uint32_t d = 33;
        ::ioctl(fd_, LIRC_SET_SEND_DUTY_CYCLE, &d);
    }
    const ssize_t want = static_cast<ssize_t>(t.size() * sizeof(unsigned int));
    if (::write(fd_, t.data(), static_cast<size_t>(want)) != want) {
        error = std::string("IR send failed: ") + std::strerror(errno);
        return false;
    }
    return true;
}

Keyboard::~Keyboard()
{
    if (fd_ >= 0) ::close(fd_);
}

bool Keyboard::open(std::string& error)
{
    const char* paths[] = {"/dev/input/by-path/platform-3f804000.i2c-event", "/dev/input/cardputer-zero-internal",
                           "/dev/input/event3"};
    for (const char* p : paths) {
        fd_ = ::open(p, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        if (fd_ >= 0) return true;
    }
    error = "keyboard not found (user must be in group 'input')";
    return false;
}

KeyEvent Keyboard::poll()
{
    if (!injected_.empty()) {
        KeyEvent e = injected_.front();
        injected_.erase(injected_.begin());
        return e;
    }
    // Long-press Esc fires while still held.
    if (escDownMs_ >= 0 && !escLongFired_ && nowMs() - escDownMs_ >= 800) {
        escLongFired_ = true;
        return {Key::LongEsc, 0};
    }
    if (fd_ < 0) return {};
    input_event ev;
    while (::read(fd_, &ev, sizeof ev) == static_cast<ssize_t>(sizeof ev)) {
        if (ev.type != EV_KEY) continue;
        if (ev.code == KEY_ESC) {
            if (ev.value == 1) {
                escDownMs_ = nowMs();
                escLongFired_ = false;
            } else if (ev.value == 0) {
                const bool wasLong = escLongFired_;
                escDownMs_ = -1;
                if (!wasLong) return {Key::Esc, 0};
            }
            continue;
        }
        if (ev.value == 0) continue;  // act on press and auto-repeat
        static const char letters[] = "qwertyuiop\0\0\0\0asdfghjkl\0\0\0\0\0zxcvbnm";
        switch (ev.code) {
        case KEY_ENTER: return {Key::Enter, 0};
        case KEY_BACKSPACE: return {Key::Backspace, 0};
        case KEY_SPACE: return {Key::Space, 0};
        case KEY_TAB: return {Key::Tab, 0};
        case KEY_UP: return {Key::Up, 0};
        case KEY_DOWN: return {Key::Down, 0};
        case KEY_LEFT: return {Key::Left, 0};
        case KEY_RIGHT: return {Key::Right, 0};
        case KEY_MUTE: return {Key::Mute, 0};
        case KEY_VOLUMEUP: return {Key::VolUp, 0};
        case KEY_VOLUMEDOWN: return {Key::VolDown, 0};
        case KEY_PLAYPAUSE: return {Key::PlayPause, 0};
        // Fn-layer media keys printed on the Cardputer: Fn+W rewind, Fn+E fast-forward,
        // Fn+U/I brightness down/up.
        case KEY_PREVIOUSSONG: return {Key::Rewind, 0};
        case KEY_NEXTSONG: return {Key::FastForward, 0};
        case KEY_BRIGHTNESSDOWN: return {Key::BrightDown, 0};
        case KEY_BRIGHTNESSUP: return {Key::BrightUp, 0};
        case KEY_HOME: return {Key::Home, 0};
        default: break;
        }
        if (ev.code >= KEY_1 && ev.code <= KEY_9) return {Key::Char, static_cast<char>('1' + (ev.code - KEY_1))};
        if (ev.code == KEY_0) return {Key::Char, '0'};
        if (ev.code >= KEY_Q && ev.code <= KEY_M) {
            const char c = letters[ev.code - KEY_Q];
            if (c) return {Key::Char, c};
        }
    }
    return {};
}

#else  // BH_DESKTOP: inert hardware, inputs come from inject()

IrReceiver::~IrReceiver() = default;
bool IrReceiver::open(std::string&) { device = "(desktop)"; return true; }
void IrReceiver::mute(int) {}
bool IrReceiver::poll(std::vector<uint32_t>& burst)
{
    if (injected_.empty()) return false;
    burst.swap(injected_);
    injected_.clear();
    return true;
}
IrTransmitter::~IrTransmitter() = default;
bool IrTransmitter::open(std::string&) { device = "(desktop)"; fd_ = 0; return true; }
bool IrTransmitter::send(const IrRaw&, std::string&) { return true; }
Keyboard::~Keyboard() = default;
bool Keyboard::open(std::string&) { return true; }
KeyEvent Keyboard::poll()
{
    if (injected_.empty()) return {};
    KeyEvent e = injected_.front();
    injected_.erase(injected_.begin());
    return e;
}

#endif

}  // namespace bh
