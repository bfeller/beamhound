// Compact remote database index (built on the PC from Flipper-IRDB, loaded on device).
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "core/irfile.hpp"

namespace bh {

enum class Group : uint8_t {
    Unknown = 0, TV, Projector, Receiver, Soundbar, SetTop, Disc, Climate, Lights, Other, Count
};
const char* groupLabel(Group g);

struct Remote {
    Group group = Group::Unknown;
    uint32_t brand = 0;   // string id
    uint32_t model = 0;   // string id
    uint32_t path = 0;    // string id: .ir file relative to the data dir
    uint32_t codeset = 0; // remotes with identical signal sets share a codeset
};

struct Sig {
    uint64_t key;     // canonicalKey of a decodable button
    uint32_t remote;
    Btn btn;
};

// Undecodable power signals, kept as raw timings for fuzzy matching.
struct RawSig {
    uint32_t remote;
    Btn btn;
    uint32_t offset;  // into rawData
    uint16_t len;
};

struct Index {
    std::vector<std::string> strings;
    std::vector<Remote> remotes;
    std::vector<Sig> sigs;         // sorted by key
    std::vector<RawSig> rawSigs;
    std::vector<uint16_t> rawData; // microseconds, capped at 65535
    uint32_t codesetCount = 0;

    // Built on load: per-remote slices of sigs (sorted by remote).
    std::vector<Sig> byRemote;
    std::vector<uint32_t> remoteStart;  // size remotes+1

    const std::string& str(uint32_t id) const { return strings[id]; }
    bool save(const std::string& path) const;
    bool load(const std::string& path, std::string& error);
    void buildDerived();

    // All signals of a remote (decodable only).
    const Sig* remoteSigsBegin(uint32_t r) const { return byRemote.data() + remoteStart[r]; }
    const Sig* remoteSigsEnd(uint32_t r) const { return byRemote.data() + remoteStart[r + 1]; }
    // Keys for a remote's button class (0..n).
    std::vector<uint64_t> keysFor(uint32_t r, Btn b) const;
};

// Raw timing similarity in [0,1]; 1 = identical shape.
double rawSimilarity(const uint16_t* ref, size_t refLen, const std::vector<uint32_t>& capture);

}  // namespace bh
