#include "core/index.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace bh {
namespace {

constexpr char kMagic[4] = {'B', 'H', 'I', 'X'};
constexpr uint32_t kVersion = 1;

template <typename T>
void put(std::ofstream& f, const T& v)
{
    f.write(reinterpret_cast<const char*>(&v), sizeof v);
}

template <typename T>
bool get(const std::vector<char>& buf, size_t& pos, T& v)
{
    if (pos + sizeof v > buf.size()) return false;
    std::memcpy(&v, buf.data() + pos, sizeof v);
    pos += sizeof v;
    return true;
}

}  // namespace

const char* groupLabel(Group g)
{
    switch (g) {
    case Group::TV: return "TV";
    case Group::Projector: return "Projector";
    case Group::Receiver: return "AV receiver / amp";
    case Group::Soundbar: return "Soundbar / speaker";
    case Group::SetTop: return "Cable / streaming box";
    case Group::Disc: return "Disc player / VCR";
    case Group::Climate: return "Fan / heater / purifier";
    case Group::Lights: return "Lights";
    case Group::Other: return "Something else";
    default: return "Unknown";
    }
}

bool Index::save(const std::string& path) const
{
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    f.write(kMagic, 4);
    put(f, kVersion);
    put(f, static_cast<uint32_t>(strings.size()));
    put(f, static_cast<uint32_t>(remotes.size()));
    put(f, static_cast<uint32_t>(sigs.size()));
    put(f, static_cast<uint32_t>(rawSigs.size()));
    put(f, static_cast<uint32_t>(rawData.size()));
    put(f, codesetCount);
    for (const auto& s : strings) {
        put(f, static_cast<uint16_t>(s.size()));
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
    }
    for (const auto& r : remotes) {
        put(f, static_cast<uint8_t>(r.group));
        put(f, r.brand);
        put(f, r.model);
        put(f, r.path);
        put(f, r.codeset);
    }
    for (const auto& s : sigs) {
        put(f, s.key);
        put(f, s.remote);
        put(f, static_cast<uint8_t>(s.btn));
    }
    for (const auto& r : rawSigs) {
        put(f, r.remote);
        put(f, static_cast<uint8_t>(r.btn));
        put(f, r.offset);
        put(f, r.len);
    }
    f.write(reinterpret_cast<const char*>(rawData.data()), static_cast<std::streamsize>(rawData.size() * 2));
    return static_cast<bool>(f);
}

bool Index::load(const std::string& path, std::string& error)
{
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        error = "cannot open " + path;
        return false;
    }
    std::vector<char> buf((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    size_t pos = 4;
    if (buf.size() < 32 || std::memcmp(buf.data(), kMagic, 4) != 0) {
        error = "not a Beamhound index";
        return false;
    }
    uint32_t ver, ns, nr, nsig, nraw, nrawdata;
    get(buf, pos, ver);
    if (ver != kVersion) {
        error = "index version mismatch";
        return false;
    }
    get(buf, pos, ns);
    get(buf, pos, nr);
    get(buf, pos, nsig);
    get(buf, pos, nraw);
    get(buf, pos, nrawdata);
    get(buf, pos, codesetCount);
    strings.resize(ns);
    for (auto& s : strings) {
        uint16_t len;
        if (!get(buf, pos, len) || pos + len > buf.size()) goto bad;
        s.assign(buf.data() + pos, len);
        pos += len;
    }
    remotes.resize(nr);
    for (auto& r : remotes) {
        uint8_t g;
        if (!get(buf, pos, g) || !get(buf, pos, r.brand) || !get(buf, pos, r.model) || !get(buf, pos, r.path) ||
            !get(buf, pos, r.codeset))
            goto bad;
        r.group = static_cast<Group>(g);
    }
    sigs.resize(nsig);
    for (auto& s : sigs) {
        uint8_t b;
        if (!get(buf, pos, s.key) || !get(buf, pos, s.remote) || !get(buf, pos, b)) goto bad;
        s.btn = static_cast<Btn>(b);
    }
    rawSigs.resize(nraw);
    for (auto& r : rawSigs) {
        uint8_t b;
        if (!get(buf, pos, r.remote) || !get(buf, pos, b) || !get(buf, pos, r.offset) || !get(buf, pos, r.len))
            goto bad;
        r.btn = static_cast<Btn>(b);
    }
    if (pos + nrawdata * 2 > buf.size()) goto bad;
    rawData.resize(nrawdata);
    std::memcpy(rawData.data(), buf.data() + pos, nrawdata * 2);
    buildDerived();
    return true;
bad:
    error = "index file is truncated or corrupt";
    return false;
}

void Index::buildDerived()
{
    byRemote = sigs;
    std::stable_sort(byRemote.begin(), byRemote.end(), [](const Sig& a, const Sig& b) { return a.remote < b.remote; });
    remoteStart.assign(remotes.size() + 1, 0);
    size_t k = 0;
    for (uint32_t r = 0; r <= remotes.size(); ++r) {
        while (k < byRemote.size() && byRemote[k].remote < r) ++k;
        remoteStart[r] = static_cast<uint32_t>(k);
    }
}

std::vector<uint64_t> Index::keysFor(uint32_t r, Btn b) const
{
    std::vector<uint64_t> out;
    for (const Sig* s = remoteSigsBegin(r); s != remoteSigsEnd(r); ++s)
        if (s->btn == b && std::find(out.begin(), out.end(), s->key) == out.end()) out.push_back(s->key);
    return out;
}

double rawSimilarity(const uint16_t* ref, size_t refLen, const std::vector<uint32_t>& cap)
{
    if (refLen < 4 || cap.size() < 4) return 0.0;
    double best = 0.0;
    for (size_t off = 0; off + 4 <= cap.size() && off <= 8; off += 2) {
        const size_t n = std::min(refLen, cap.size() - off);
        if (n < refLen * 9 / 10) break;
        double err = 0.0;
        for (size_t i = 0; i < n; ++i) {
            // Treat any long space as "gap" so trailing silence lengths don't matter.
            const double a = std::min<double>(ref[i], 20000.0);
            const double b = std::min<double>(cap[off + i], 20000.0);
            err += std::min(1.0, std::fabs(a - b) / std::max({a, b, 300.0}));
        }
        err += static_cast<double>(refLen - n);  // missing tail counts as full error
        best = std::max(best, 1.0 - err / refLen);
    }
    return best;
}

}  // namespace bh
