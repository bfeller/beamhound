#include "core/ir.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace bh {
namespace {

struct ProtoInfo {
    Proto proto;
    const char* name;
    Family family;
    uint32_t carrier;
};

constexpr std::array<ProtoInfo, 14> kProtos{{
    {Proto::NEC, "NEC", Family::NEC, 38000},
    {Proto::NECext, "NECext", Family::NEC, 38000},
    {Proto::NEC42, "NEC42", Family::NEC42, 38000},
    {Proto::NEC42ext, "NEC42ext", Family::NEC42, 38000},
    {Proto::Samsung32, "Samsung32", Family::Samsung, 38000},
    {Proto::RC5, "RC5", Family::RC5, 36000},
    {Proto::RC5X, "RC5X", Family::RC5, 36000},
    {Proto::RC6, "RC6", Family::RC6, 36000},
    {Proto::SIRC, "SIRC", Family::SIRC, 40000},
    {Proto::SIRC15, "SIRC15", Family::SIRC, 40000},
    {Proto::SIRC20, "SIRC20", Family::SIRC, 40000},
    {Proto::Kaseikyo, "Kaseikyo", Family::Kaseikyo, 38000},
    {Proto::RCA, "RCA", Family::RCA, 38000},
    {Proto::Pioneer, "Pioneer", Family::NEC, 40000},
}};

const ProtoInfo* info(Proto p)
{
    for (const auto& i : kProtos)
        if (i.proto == p) return &i;
    return nullptr;
}

bool g_toggle = false;

// Generous tolerance: real demodulators stretch marks and shrink spaces.
bool near(uint32_t d, uint32_t target, double rel, uint32_t absMin)
{
    const double tol = std::max(target * rel, static_cast<double>(absMin));
    return std::fabs(static_cast<double>(d) - target) <= tol;
}

// ---------------------------------------------------------------- pulse distance
struct PdSpec {
    uint32_t pm, ps;   // preamble mark/space
    uint32_t bm;       // bit mark
    uint32_t b0s, b1s; // bit 0/1 space
};

constexpr PdSpec kNec{9000, 4500, 560, 560, 1690};
constexpr PdSpec kSamsung{4500, 4500, 550, 550, 1650};
constexpr PdSpec kKaseikyo{3456, 1728, 432, 432, 1296};
constexpr PdSpec kRca{4000, 4000, 500, 1000, 2000};
constexpr PdSpec kPioneer{8500, 4225, 500, 500, 1500};

// Bits are stored LSB first in received order.
bool pdDecode(const std::vector<uint32_t>& t, size_t i, const PdSpec& s, uint64_t& bits, int& n)
{
    if (i + 1 >= t.size()) return false;
    if (!near(t[i], s.pm, 0.2, 300) || !near(t[i + 1], s.ps, 0.25, 300)) return false;
    const uint32_t thr = (s.b0s + s.b1s) / 2;
    const uint32_t gap = std::max<uint32_t>(s.b1s * 2, 3000);
    bits = 0;
    n = 0;
    size_t j = i + 2;
    while (j < t.size()) {
        // Accept any plausible mark: receivers stretch marks a lot; bit values come from spaces.
        if (t[j] < s.bm * 2 / 5 || t[j] > s.bm * 11 / 5 + 100) return false;
        if (j + 1 >= t.size()) break;  // final (stop) mark at end of capture
        const uint32_t sp = t[j + 1];
        if (sp >= gap) break;          // stop mark followed by inter-frame gap
        if (n >= 64) return false;
        if (sp > static_cast<uint32_t>(s.b1s * 1.6)) return false;
        if (sp > thr) bits |= (1ull << n);
        ++n;
        j += 2;
    }
    return n > 0;
}

std::vector<uint32_t> pdEncode(const PdSpec& s, uint64_t bits, int n)
{
    std::vector<uint32_t> out{s.pm, s.ps};
    for (int k = 0; k < n; ++k) {
        out.push_back(s.bm);
        out.push_back((bits >> k) & 1 ? s.b1s : s.b0s);
    }
    out.push_back(s.bm);
    return out;
}

uint8_t byteAt(uint64_t bits, int idx) { return static_cast<uint8_t>(bits >> (8 * idx)); }

std::optional<IrCode> tryNec(const std::vector<uint32_t>& t, size_t i)
{
    uint64_t b;
    int n;
    if (!pdDecode(t, i, kNec, b, n)) return std::nullopt;
    if (n == 32) {
        const uint8_t a = byteAt(b, 0), ai = byteAt(b, 1), c = byteAt(b, 2), ci = byteAt(b, 3);
        if (a == static_cast<uint8_t>(~ai) && c == static_cast<uint8_t>(~ci)) return IrCode{Proto::NEC, a, c};
        return IrCode{Proto::NECext, static_cast<uint32_t>(b & 0xFFFF), static_cast<uint32_t>((b >> 16) & 0xFFFF)};
    }
    if (n == 42) {
        const uint32_t a = b & 0x1FFF, ai = (b >> 13) & 0x1FFF;
        const uint32_t c = (b >> 26) & 0xFF, ci = (b >> 34) & 0xFF;
        if (a == (~ai & 0x1FFF) && c == (~ci & 0xFF)) return IrCode{Proto::NEC42, a, c};
        return IrCode{Proto::NEC42ext, a | (ai << 13), c | (ci << 8)};
    }
    return std::nullopt;
}

std::optional<IrCode> trySamsung(const std::vector<uint32_t>& t, size_t i)
{
    uint64_t b;
    int n;
    if (!pdDecode(t, i, kSamsung, b, n) || n != 32) return std::nullopt;
    const uint8_t a1 = byteAt(b, 0), a2 = byteAt(b, 1), c = byteAt(b, 2), ci = byteAt(b, 3);
    if (a1 != a2 || c != static_cast<uint8_t>(~ci)) return std::nullopt;
    return IrCode{Proto::Samsung32, a1, c};
}

std::optional<IrCode> tryKaseikyo(const std::vector<uint32_t>& t, size_t i)
{
    uint64_t b;
    int n;
    if (!pdDecode(t, i, kKaseikyo, b, n) || n != 48) return std::nullopt;
    uint8_t d[6];
    for (int k = 0; k < 6; ++k) d[k] = byteAt(b, k);
    const uint16_t vendor = static_cast<uint16_t>(d[0] | (d[1] << 8));
    const uint8_t vparity = d[2] & 0x0F;
    const uint8_t genre1 = d[2] >> 4, genre2 = d[3] & 0x0F;
    const uint16_t data = static_cast<uint16_t>((d[3] >> 4) | ((d[4] & 0x3F) << 4));
    const uint8_t id = d[4] >> 6;
    uint8_t vcheck = d[0] ^ d[1];
    vcheck = (vcheck & 0x0F) ^ (vcheck >> 4);
    if (vparity != vcheck || d[5] != static_cast<uint8_t>(d[2] ^ d[3] ^ d[4])) return std::nullopt;
    const uint32_t addr = (static_cast<uint32_t>(id) << 24) | (static_cast<uint32_t>(vendor) << 8) |
                          (static_cast<uint32_t>(genre1) << 4) | genre2;
    return IrCode{Proto::Kaseikyo, addr, data};
}

std::optional<IrCode> tryRca(const std::vector<uint32_t>& t, size_t i)
{
    uint64_t b;
    int n;
    if (!pdDecode(t, i, kRca, b, n) || n != 24) return std::nullopt;
    const uint32_t a = b & 0xF, c = (b >> 4) & 0xFF, ai = (b >> 12) & 0xF, ci = (b >> 16) & 0xFF;
    if (a != (~ai & 0xF) || c != (~ci & 0xFF)) return std::nullopt;
    return IrCode{Proto::RCA, a, c};
}

std::optional<IrCode> tryPioneer(const std::vector<uint32_t>& t, size_t i)
{
    uint64_t b;
    int n;
    if (!pdDecode(t, i, kPioneer, b, n) || (n != 32 && n != 33)) return std::nullopt;
    const uint8_t a = byteAt(b, 0), ai = byteAt(b, 1), c = byteAt(b, 2), ci = byteAt(b, 3);
    if (ai != static_cast<uint8_t>(~a) || ci != static_cast<uint8_t>(~c)) return std::nullopt;
    return IrCode{Proto::Pioneer, a, c};
}

// ---------------------------------------------------------------- SIRC (pulse width)
std::optional<IrCode> trySirc(const std::vector<uint32_t>& t, size_t i)
{
    if (i + 1 >= t.size()) return std::nullopt;
    if (!near(t[i], 2400, 0.2, 300) || !near(t[i + 1], 600, 0.5, 200)) return std::nullopt;
    uint32_t bits = 0;
    int n = 0;
    for (size_t j = i + 2; j < t.size(); j += 2) {
        // Pulse width: threshold halfway between the 600us and 1200us marks.
        if (t[j] < 250 || t[j] > 1900) return std::nullopt;
        const int bit = t[j] > 900 ? 1 : 0;
        if (n >= 20) return std::nullopt;
        bits |= static_cast<uint32_t>(bit) << n;
        ++n;
        if (j + 1 >= t.size() || t[j + 1] > 1500) break;
    }
    switch (n) {
    case 12: return IrCode{Proto::SIRC, (bits >> 7) & 0x1F, bits & 0x7F};
    case 15: return IrCode{Proto::SIRC15, (bits >> 7) & 0xFF, bits & 0x7F};
    case 20: return IrCode{Proto::SIRC20, (bits >> 7) & 0x1FFF, bits & 0x7F};
    default: return std::nullopt;
    }
}

// ---------------------------------------------------------------- Manchester helpers
// Expand durations (alternating mark/space starting with a mark) into unit levels.
// Stops at the first space longer than maxUnits; fails on irregular durations.
bool expandUnits(const std::vector<uint32_t>& t, size_t i, uint32_t unit, int maxUnits, std::vector<uint8_t>& out)
{
    for (size_t k = i; k < t.size(); ++k) {
        const bool mark = ((k - i) % 2) == 0;
        const double u = static_cast<double>(t[k]) / unit;
        const int units = static_cast<int>(std::lround(u));
        if (units < 1 || std::fabs(u - units) > 0.4) {
            if (!mark && units > maxUnits) return true;  // trailing gap
            return false;
        }
        if (units > maxUnits) return !mark;
        for (int x = 0; x < units; ++x) out.push_back(mark ? 1 : 0);
        if (out.size() > 64) return false;
    }
    return true;
}

std::vector<uint32_t> unitsToTimings(const std::vector<uint8_t>& levels, uint32_t unit)
{
    std::vector<uint32_t> out;
    size_t k = 0;
    while (k < levels.size() && levels[k] == 0) ++k;  // drop leading low
    while (k < levels.size()) {
        const uint8_t lv = levels[k];
        uint32_t run = 0;
        while (k < levels.size() && levels[k] == lv) {
            ++run;
            ++k;
        }
        out.push_back(run * unit);
    }
    if (!out.empty() && (out.size() % 2) == 0) out.pop_back();  // end on a mark
    return out;
}

constexpr uint32_t kRc5Unit = 889;
constexpr uint32_t kRc6Unit = 444;

std::optional<IrCode> tryRc5Biased(const std::vector<uint32_t>& t, size_t i)
{
    if (!near(t[i], kRc5Unit, 0.4, 200) && !near(t[i], 2 * kRc5Unit, 0.25, 250)) return std::nullopt;
    std::vector<uint8_t> h{0};  // the leading low half of start bit 1 is invisible
    if (!expandUnits(t, i, kRc5Unit, 2, h)) return std::nullopt;
    if (h.size() == 27) h.push_back(0);
    if (h.size() != 28) return std::nullopt;
    uint32_t bits[14];
    for (int b = 0; b < 14; ++b) {
        const uint8_t h0 = h[2 * b], h1 = h[2 * b + 1];
        if (h0 == 0 && h1 == 1)
            bits[b] = 1;
        else if (h0 == 1 && h1 == 0)
            bits[b] = 0;
        else
            return std::nullopt;
    }
    if (bits[0] != 1) return std::nullopt;
    uint32_t addr = 0, cmd = 0;
    for (int b = 3; b < 8; ++b) addr = (addr << 1) | bits[b];
    for (int b = 8; b < 14; ++b) cmd = (cmd << 1) | bits[b];
    return IrCode{bits[1] ? Proto::RC5 : Proto::RC5X, addr, cmd};
}

std::optional<IrCode> tryRc6Biased(const std::vector<uint32_t>& t, size_t i)
{
    if (i + 1 >= t.size()) return std::nullopt;
    if (!near(t[i], 2666, 0.2, 300) || !near(t[i + 1], 889, 0.35, 200)) return std::nullopt;
    std::vector<uint8_t> u;
    if (!expandUnits(t, i + 2, kRc6Unit, 3, u)) return std::nullopt;
    constexpr size_t kTotal = 2 * 4 + 4 + 2 * 16;
    while (u.size() < kTotal && u.size() >= kTotal - 2) u.push_back(0);
    if (u.size() != kTotal) return std::nullopt;
    auto bitAt = [&](size_t pos, int& out) {
        if (u[pos] == 1 && u[pos + 1] == 0) out = 1;
        else if (u[pos] == 0 && u[pos + 1] == 1) out = 0;
        else return false;
        return true;
    };
    int s, m;
    if (!bitAt(0, s) || s != 1) return std::nullopt;
    for (size_t p = 2; p < 8; p += 2)
        if (!bitAt(p, m) || m != 0) return std::nullopt;  // mode 0 only
    const bool trOk = (u[8] == u[9]) && (u[10] == u[11]) && (u[8] != u[10]);
    if (!trOk) return std::nullopt;
    uint32_t addr = 0, cmd = 0;
    for (int b = 0; b < 16; ++b) {
        int v;
        if (!bitAt(12 + 2 * b, v)) return std::nullopt;
        if (b < 8) addr = (addr << 1) | v;
        else cmd = (cmd << 1) | v;
    }
    return IrCode{Proto::RC6, addr, cmd};
}


// Receivers lengthen marks and shorten spaces by a roughly constant amount, which eats
// into Manchester's short time units. Retry with a few bias corrections.
template <typename Fn>
std::optional<IrCode> withBias(const std::vector<uint32_t>& t, size_t i, Fn fn)
{
    if (auto r = fn(t, i)) return r;
    const size_t end = std::min(t.size(), i + 64);
    for (int bias : {90, 170, -90}) {
        std::vector<uint32_t> adj(t.begin() + static_cast<long>(i), t.begin() + static_cast<long>(end));
        for (size_t k = 0; k < adj.size(); ++k) {
            const int v = static_cast<int>(adj[k]) + ((k % 2 == 0) ? -bias : bias);
            adj[k] = static_cast<uint32_t>(std::max(v, 1));
        }
        if (auto r = fn(adj, 0)) return r;
    }
    return std::nullopt;
}

std::optional<IrCode> tryRc5(const std::vector<uint32_t>& t, size_t i) { return withBias(t, i, tryRc5Biased); }
std::optional<IrCode> tryRc6(const std::vector<uint32_t>& t, size_t i) { return withBias(t, i, tryRc6Biased); }

void appendFrame(std::vector<uint32_t>& out, const std::vector<uint32_t>& frame, uint32_t gap)
{
    if (!out.empty()) out.push_back(gap);
    out.insert(out.end(), frame.begin(), frame.end());
}

uint32_t total(const std::vector<uint32_t>& v)
{
    uint32_t s = 0;
    for (auto x : v) s += x;
    return s;
}

}  // namespace

// ---------------------------------------------------------------- public API
std::string_view protoName(Proto p)
{
    const auto* i = info(p);
    return i ? i->name : "Unknown";
}

Proto protoFromName(std::string_view name)
{
    for (const auto& i : kProtos)
        if (name == i.name) return i.proto;
    return Proto::Unknown;
}

Family protoFamily(Proto p)
{
    const auto* i = info(p);
    return i ? i->family : Family::None;
}

uint32_t protoCarrierHz(Proto p)
{
    const auto* i = info(p);
    return i ? i->carrier : 38000;
}

uint64_t canonicalKey(const IrCode& c)
{
    uint64_t v = 0;
    const uint32_t a = c.address, m = c.command;
    switch (c.proto) {
    case Proto::NEC:
    case Proto::Pioneer:
        v = (a & 0xFF) | ((~a & 0xFF) << 8) | ((m & 0xFFull) << 16) | ((~m & 0xFFull) << 24);
        break;
    case Proto::NECext: v = (a & 0xFFFF) | ((m & 0xFFFFull) << 16); break;
    case Proto::NEC42:
        v = (a & 0x1FFF) | ((~a & 0x1FFFull) << 13) | ((m & 0xFFull) << 26) | ((~m & 0xFFull) << 34);
        break;
    case Proto::NEC42ext: v = (a & 0x3FFFFFF) | ((m & 0xFFFFull) << 26); break;
    case Proto::Samsung32: v = (a & 0xFF) | ((a & 0xFFull) << 8) | ((m & 0xFFull) << 16) | ((~m & 0xFFull) << 24); break;
    case Proto::RC5: v = ((a & 0x1F) << 7) | (m & 0x3F); break;
    case Proto::RC5X: v = ((a & 0x1F) << 7) | (m & 0x3F) | 0x40; break;
    case Proto::RC6: v = ((a & 0xFF) << 8) | (m & 0xFF); break;
    case Proto::SIRC: v = (12ull << 32) | ((a & 0x1F) << 7) | (m & 0x7F); break;
    case Proto::SIRC15: v = (15ull << 32) | ((a & 0xFF) << 7) | (m & 0x7F); break;
    case Proto::SIRC20: v = (20ull << 32) | ((a & 0x1FFF) << 7) | (m & 0x7F); break;
    case Proto::Kaseikyo: v = (static_cast<uint64_t>(a & 0x03FFFFFF) << 10) | (m & 0x3FF); break;
    case Proto::RCA: v = ((a & 0xF) << 8) | (m & 0xFF); break;
    default: return 0;
    }
    return (static_cast<uint64_t>(protoFamily(c.proto)) << 56) | (v & 0x00FFFFFFFFFFFFFFull);
}

std::string describe(const IrCode& c)
{
    char buf[64];
    std::snprintf(buf, sizeof buf, "%s A:%X C:%X", std::string(protoName(c.proto)).c_str(), c.address, c.command);
    return buf;
}

std::optional<IrCode> decode(const std::vector<uint32_t>& t)
{
    using Fn = std::optional<IrCode> (*)(const std::vector<uint32_t>&, size_t);
    static constexpr Fn kDecoders[] = {tryNec, trySamsung, tryRc6, trySirc, tryKaseikyo, tryRca, tryPioneer, tryRc5};
    for (size_t i = 0; i < t.size(); i += 2)
        for (Fn fn : kDecoders)
            if (auto r = fn(t, i)) return r;
    return std::nullopt;
}

void setToggle(bool on) { g_toggle = on; }

IrRaw encode(const IrCode& c)
{
    IrRaw raw;
    raw.carrierHz = protoCarrierHz(c.proto);
    auto& out = raw.timings;
    const uint32_t a = c.address, m = c.command;
    switch (c.proto) {
    case Proto::NEC:
    case Proto::NECext: {
        const uint64_t data = canonicalKey(c) & 0xFFFFFFFFull;
        auto frame = pdEncode(kNec, data, 32);
        appendFrame(out, frame, 0);
        appendFrame(out, {9000, 2250, 560}, 108000 - total(frame));  // one repeat code
        break;
    }
    case Proto::NEC42:
    case Proto::NEC42ext: appendFrame(out, pdEncode(kNec, canonicalKey(c) & 0x3FFFFFFFFFFull, 42), 0); break;
    case Proto::Samsung32: appendFrame(out, pdEncode(kSamsung, canonicalKey(c) & 0xFFFFFFFFull, 32), 0); break;
    case Proto::Pioneer: {
        auto frame = pdEncode(kPioneer, canonicalKey(c) & 0xFFFFFFFFull, 32);
        appendFrame(out, frame, 0);
        appendFrame(out, frame, 26000);
        break;
    }
    case Proto::RCA: {
        const uint64_t data = (a & 0xF) | ((m & 0xFFull) << 4) | ((~a & 0xFull) << 12) | ((~m & 0xFFull) << 16);
        appendFrame(out, pdEncode(kRca, data, 24), 0);
        break;
    }
    case Proto::Kaseikyo: {
        uint8_t d[6];
        const uint16_t vendor = static_cast<uint16_t>((a >> 8) & 0xFFFF);
        d[0] = vendor & 0xFF;
        d[1] = vendor >> 8;
        uint8_t vp = d[0] ^ d[1];
        vp = (vp & 0x0F) ^ (vp >> 4);
        d[2] = static_cast<uint8_t>((vp & 0x0F) | (((a >> 4) & 0x0F) << 4));
        d[3] = static_cast<uint8_t>((a & 0x0F) | ((m & 0x0F) << 4));
        d[4] = static_cast<uint8_t>(((m >> 4) & 0x3F) | (((a >> 24) & 0x03) << 6));
        d[5] = d[2] ^ d[3] ^ d[4];
        uint64_t data = 0;
        for (int k = 0; k < 6; ++k) data |= static_cast<uint64_t>(d[k]) << (8 * k);
        appendFrame(out, pdEncode(kKaseikyo, data, 48), 0);
        break;
    }
    case Proto::SIRC:
    case Proto::SIRC15:
    case Proto::SIRC20: {
        const int n = c.proto == Proto::SIRC ? 12 : c.proto == Proto::SIRC15 ? 15 : 20;
        const uint32_t data = (m & 0x7F) | (a << 7);
        std::vector<uint32_t> frame{2400, 600};
        for (int k = 0; k < n; ++k) {
            frame.push_back((data >> k) & 1 ? 1200 : 600);
            frame.push_back(600);
        }
        frame.pop_back();
        const uint32_t gap = 45000 - total(frame);
        for (int r = 0; r < 3; ++r) appendFrame(out, frame, gap);
        break;
    }
    case Proto::RC5:
    case Proto::RC5X: {
        uint32_t bits[14];
        bits[0] = 1;
        bits[1] = c.proto == Proto::RC5 ? 1 : 0;
        bits[2] = g_toggle ? 1 : 0;
        for (int b = 0; b < 5; ++b) bits[3 + b] = (a >> (4 - b)) & 1;
        for (int b = 0; b < 6; ++b) bits[8 + b] = (m >> (5 - b)) & 1;
        std::vector<uint8_t> h;
        for (uint32_t b : bits) {
            h.push_back(b ? 0 : 1);
            h.push_back(b ? 1 : 0);
        }
        appendFrame(out, unitsToTimings(h, kRc5Unit), 0);
        break;
    }
    case Proto::RC6: {
        std::vector<uint8_t> u;
        auto put = [&](int bit) {
            u.push_back(bit ? 1 : 0);
            u.push_back(bit ? 0 : 1);
        };
        put(1);
        for (int k = 0; k < 3; ++k) put(0);
        const uint8_t tr = g_toggle ? 1 : 0;
        u.insert(u.end(), {tr, tr, static_cast<uint8_t>(!tr), static_cast<uint8_t>(!tr)});
        for (int b = 7; b >= 0; --b) put((a >> b) & 1);
        for (int b = 7; b >= 0; --b) put((m >> b) & 1);
        std::vector<uint32_t> frame{2666, 889};
        auto body = unitsToTimings(u, kRc6Unit);
        frame.insert(frame.end(), body.begin(), body.end());
        appendFrame(out, frame, 0);
        break;
    }
    default: break;
    }
    return raw;
}

}  // namespace bh
