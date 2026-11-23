// Core tests: protocol round trips, button classification, file parsing.
// Optional: pass a Flipper-IRDB checkout path to run the database consistency check.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <random>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "core/ir.hpp"
#include "core/irfile.hpp"

using namespace bh;

static int g_fail = 0;
#define CHECK(cond, ...)                                    \
    do {                                                    \
        if (!(cond)) {                                      \
            ++g_fail;                                       \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                       \
            std::printf("\n");                              \
        }                                                   \
    } while (0)

// Simulate a real receiver: marks stretched, spaces shrunk, plus jitter.
static std::vector<uint32_t> distort(const std::vector<uint32_t>& in, std::mt19937& rng)
{
    std::uniform_int_distribution<int> jitter(-60, 60);
    std::vector<uint32_t> out;
    for (size_t i = 0; i < in.size(); ++i) {
        int v = static_cast<int>(in[i]) + jitter(rng) + ((i % 2 == 0) ? 80 : -80);
        out.push_back(static_cast<uint32_t>(std::max(v, 50)));
    }
    return out;
}

static void testRoundTrips()
{
    struct Case { Proto p; uint32_t amask, cmask; };
    const Case cases[] = {
        {Proto::NEC, 0xFF, 0xFF},         {Proto::NECext, 0xFFFF, 0xFFFF}, {Proto::NEC42, 0x1FFF, 0xFF},
        {Proto::Samsung32, 0xFF, 0xFF},   {Proto::RC5, 0x1F, 0x3F},        {Proto::RC5X, 0x1F, 0x3F},
        {Proto::RC6, 0xFF, 0xFF},         {Proto::SIRC, 0x1F, 0x7F},       {Proto::SIRC15, 0xFF, 0x7F},
        {Proto::SIRC20, 0x1FFF, 0x7F},    {Proto::Kaseikyo, 0x03FFFFFF, 0x3FF},
        {Proto::RCA, 0xF, 0xFF},          {Proto::Pioneer, 0xFF, 0xFF},
    };
    std::mt19937 rng(42);
    for (const auto& c : cases) {
        int ok = 0, total = 0;
        for (int i = 0; i < 300; ++i) {
            IrCode code{c.p, static_cast<uint32_t>(rng() & c.amask), static_cast<uint32_t>(rng() & c.cmask)};
            if (c.p == Proto::NECext) {
                // avoid values that are valid plain NEC (decoder reports those as NEC)
                if ((code.address & 0xFF) == (~(code.address >> 8) & 0xFF)) code.address ^= 0x100;
            }
            if (c.p == Proto::NEC42 || c.p == Proto::Kaseikyo) code.address &= c.amask;
            setToggle(i & 1);
            const IrRaw raw = encode(code);
            for (int pass = 0; pass < 2; ++pass) {
                const auto timings = pass == 0 ? raw.timings : distort(raw.timings, rng);
                const auto got = decode(timings);
                ++total;
                const bool match = got && canonicalKey(*got) == canonicalKey(code);
                if (match) ++ok;
                else if (ok + 3 > total) {
                    CHECK(false, "%s round trip: want %s got %s (pass %d)", std::string(protoName(c.p)).c_str(),
                          describe(code).c_str(), got ? describe(*got).c_str() : "none", pass);
                }
            }
        }
        std::printf("  %-10s %d/%d\n", std::string(protoName(c.p)).c_str(), ok, total);
        CHECK(ok == total, "%s: %d/%d round trips", std::string(protoName(c.p)).c_str(), ok, total);
    }
}

static void testEquivalence()
{
    // NEC addr 0x04 cmd 0x08 is the same frame as NECext addr 0xFB04 cmd 0xF708.
    CHECK(canonicalKey({Proto::NEC, 0x04, 0x08}) == canonicalKey({Proto::NECext, 0xFB04, 0xF708}), "NEC/NECext");
    CHECK(canonicalKey({Proto::NEC, 0x04, 0x08}) == canonicalKey({Proto::Pioneer, 0x04, 0x08}), "NEC/Pioneer");
    CHECK(canonicalKey({Proto::RC5, 1, 12}) != canonicalKey({Proto::RC5X, 1, 12}), "RC5/RC5X differ");
    CHECK(canonicalKey({Proto::SIRC, 1, 21}) != canonicalKey({Proto::SIRC15, 1, 21}), "SIRC widths differ");
}

static void testClassify()
{
    struct { const char* n; Btn b; } cases[] = {
        {"POWER", Btn::Power}, {"Power", Btn::Power}, {"TV-POWER", Btn::Power}, {"Power_off", Btn::PowerOff},
        {"POWER_ON/OFF", Btn::Power}, {"Vol_up", Btn::VolUp}, {"VOL+", Btn::VolUp}, {"Vol_dn", Btn::VolDown},
        {"VOL-", Btn::VolDown}, {"CH+", Btn::ChUp}, {"Ch_prev", Btn::ChDown}, {"MUTE", Btn::Mute},
        {"Source", Btn::Input}, {"KEY_5", Btn::Num5}, {"5", Btn::Num5}, {"ZONE_2_OFF", Btn::None},
        {"SUBTITLE_ON/OFF", Btn::None}, {"Cursor_Up", Btn::Up}, {"Play/Pause", Btn::PlayPause},
    };
    for (auto& c : cases) CHECK(classifyButton(c.n) == c.b, "classify %s -> %d", c.n, (int)classifyButton(c.n));
}

static void testParse()
{
    const char* text =
        "Filetype: IR signals file\nVersion: 1\n#\nname: Power\ntype: parsed\nprotocol: NECext\n"
        "address: 04 FB 00 00\ncommand: 08 F7 00 00\n#\nname: Vol_up\ntype: raw\nfrequency: 38000\n"
        "duty_cycle: 0.33\ndata: 9000 4500 560 560 560\n";
    auto b = parseIrFile(text);
    CHECK(b.size() == 2, "parsed %zu buttons", b.size());
    if (b.size() == 2) {
        CHECK(b[0].parsed && b[0].code.proto == Proto::NECext && b[0].code.address == 0xFB04, "parsed fields");
        CHECK(!b[1].parsed && b[1].raw.timings.size() == 5, "raw fields");
        CHECK(buttonKey(b[0]) == canonicalKey({Proto::NEC, 4, 8}), "key equivalence via file");
    }
}

// For each protocol family: of the raw DB signals we decode, how many land on a key that
// also appears as a parsed entry somewhere in the DB? A wrong bit layout gives ~0%.
static void dbConsistency(const std::string& root)
{
    std::unordered_set<uint64_t> parsedKeys;
    struct Stat { int decoded = 0, overlap = 0; };
    std::map<std::string, Stat> stats;
    std::vector<std::pair<std::string, uint64_t>> rawDecoded;
    long files = 0, raw = 0, rawOk = 0, parsed = 0;
    for (auto& e : std::filesystem::recursive_directory_iterator(root)) {
        if (!e.is_regular_file() || e.path().extension() != ".ir") continue;
        ++files;
        for (auto& b : loadIrFile(e.path().string())) {
            if (b.parsed) {
                ++parsed;
                parsedKeys.insert(canonicalKey(b.code));
            } else {
                ++raw;
                if (auto c = decode(b.raw.timings)) {
                    ++rawOk;
                    rawDecoded.emplace_back(std::string(protoName(c->proto)), canonicalKey(*c));
                }
            }
        }
    }
    for (auto& [p, k] : rawDecoded) {
        auto& s = stats[p];
        ++s.decoded;
        if (parsedKeys.count(k)) ++s.overlap;
    }
    std::printf("\nDB: %ld files, %ld parsed, %ld raw (%ld decoded = %.1f%%)\n", files, parsed, raw, rawOk,
                raw ? 100.0 * rawOk / raw : 0.0);
    std::printf("  %-10s %8s %8s %6s\n", "protocol", "decoded", "overlap", "pct");
    for (auto& [p, s] : stats)
        std::printf("  %-10s %8d %8d %5.1f%%\n", p.c_str(), s.decoded, s.overlap, 100.0 * s.overlap / s.decoded);
}

int main(int argc, char** argv)
{
    std::printf("round trips (clean + distorted):\n");
    testRoundTrips();
    testEquivalence();
    testClassify();
    testParse();
    if (argc > 1) dbConsistency(argv[1]);
    std::printf(g_fail ? "\n%d FAILURES\n" : "\nALL TESTS PASSED\n", g_fail);
    return g_fail ? 1 : 0;
}
