// Beamhound core: IR protocol model, decoder and encoder.
//
// Address/command values follow the conventions of the Flipper Zero .ir file
// format so that codes decoded from live captures compare directly with the
// "parsed" entries in Flipper-IRDB. Implemented from the protocol
// specifications; no Flipper firmware code is included.
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bh {

enum class Proto : uint8_t {
    Unknown = 0,
    NEC,
    NECext,
    NEC42,
    NEC42ext,
    Samsung32,
    RC5,
    RC5X,
    RC6,
    SIRC,
    SIRC15,
    SIRC20,
    Kaseikyo,
    RCA,
    Pioneer,
};

// Protocol families: codes in the same family with the same canonical value
// are the same signal on the wire (e.g. NEC addr 0x04 == NECext addr 0xFB04).
enum class Family : uint8_t { None = 0, NEC = 1, NEC42 = 2, Samsung = 3, RC5 = 4, RC6 = 5, SIRC = 6, Kaseikyo = 7, RCA = 8 };

std::string_view protoName(Proto p);
Proto protoFromName(std::string_view name);
Family protoFamily(Proto p);
uint32_t protoCarrierHz(Proto p);

struct IrCode {
    Proto proto = Proto::Unknown;
    uint32_t address = 0;
    uint32_t command = 0;
    bool operator==(const IrCode& o) const { return proto == o.proto && address == o.address && command == o.command; }
};

// Raw signal: alternating mark/space durations in microseconds, starting with a mark.
struct IrRaw {
    uint32_t carrierHz = 38000;
    std::vector<uint32_t> timings;
};

// 64-bit key identifying a signal independent of how it is written down.
// Top byte = family, low bits = canonical frame value. 0 = invalid.
uint64_t canonicalKey(const IrCode& code);
std::string describe(const IrCode& code);

// Find and decode the first recognisable frame in a raw capture.
std::optional<IrCode> decode(const std::vector<uint32_t>& timings);

// Encode a code as transmit-ready timings (includes protocol-required repeats).
IrRaw encode(const IrCode& code);

// Toggle bit state for RC5/RC6 transmissions (flip between presses).
void setToggle(bool on);

}  // namespace bh
