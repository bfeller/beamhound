// Builds Beamhound's index from a Flipper-IRDB checkout (used on the PC and on the device).
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/builder.hpp"

namespace fs = std::filesystem;
namespace bh {

namespace {

std::string upper(std::string s)
{
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string brandKey(const std::string& s)
{
    std::string k;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c))) k.push_back(static_cast<char>(std::toupper(c)));
    return k;
}

// Folder names sometimes wrap a brand in a device word or a company suffix ("TV SAMSUNG",
// "Dish Network", "InFocus Systems"). Returns the key of the bare brand when stripping those
// words leaves a brand that exists on its own, else "". Distinct companies that merely share a
// first word ("Dream Multimedia" vs "Dream Vision") are kept apart: only the listed words go.
// `bare` receives the stripped spelling ("Pinnacle" from "Pinnacle_Systems").
std::string aliasKey(const std::string& brand, const std::set<std::string>& keys, std::string& bare)
{
    static const std::set<std::string> lead = {"TV", "TVS", "VCR", "DVD"};
    static const std::set<std::string> trail = {"TV", "AUDIO", "SYSTEMS", "NETWORK", "TECHNOLOGIES", "ELECTRONICS",
                                                "INC", "CORP", "CORPORATION", "CO", "LTD", "GMBH"};
    std::vector<std::string> words, spelled;
    std::string w;
    for (char c : brand + " ") {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            w.push_back(c);
        } else if (!w.empty()) {
            spelled.push_back(w);
            words.push_back(upper(w));
            w.clear();
        }
    }
    size_t b = 0, e = words.size();
    while (b < e && lead.count(words[b])) ++b;
    while (e > b && trail.count(words[e - 1])) --e;
    if (b == e || (b == 0 && e == words.size())) return "";
    std::string k;
    bare.clear();
    for (size_t i = b; i < e; ++i) {
        k += words[i];
        bare += (bare.empty() ? "" : " ") + spelled[i];
    }
    return keys.count(k) && k != brandKey(brand) ? k : "";
}

std::string pretty(std::string s)
{
    std::replace(s.begin(), s.end(), '_', ' ');
    return s;
}

Group groupFromCurated(const std::string& dir)
{
    static const std::map<std::string, Group> m = {
        {"TVs", Group::TV}, {"Monitors", Group::TV}, {"Touchscreen_Displays", Group::TV},
        {"Digital_Signs", Group::TV}, {"Projectors", Group::Projector},
        {"Audio_and_Video_Receivers", Group::Receiver}, {"Head_Units", Group::Receiver},
        {"Car_Multimedia", Group::Receiver}, {"SoundBars", Group::Soundbar}, {"Speakers", Group::Soundbar},
        {"Cable_Boxes", Group::SetTop}, {"Streaming_Devices", Group::SetTop}, {"DVB-T", Group::SetTop},
        {"TV_Tuner", Group::SetTop}, {"Multimedia", Group::SetTop}, {"Converters", Group::SetTop},
        {"Blu-Ray", Group::Disc}, {"DVD_Players", Group::Disc}, {"CD_Players", Group::Disc},
        {"VCR", Group::Disc}, {"Laserdisc", Group::Disc}, {"MiniDisc", Group::Disc},
        {"Fans", Group::Climate}, {"Heaters", Group::Climate}, {"Air_Purifiers", Group::Climate},
        {"Humidifiers", Group::Climate}, {"Fireplaces", Group::Climate}, {"LED_Lighting", Group::Lights},
    };
    auto it = m.find(dir);
    return it == m.end() ? Group::Other : it->second;
}

// Converted entries only carry free-form path names; infer the device type from keywords.
Group groupFromWords(const std::string& path)
{
    const std::string u = upper(path);
    auto has = [&](std::initializer_list<const char*> ws) {
        for (const char* w : ws)
            if (u.find(w) != std::string::npos) return true;
        return false;
    };
    if (has({"PROJECTOR", "BEAMER"})) return Group::Projector;
    if (has({"SOUNDBAR", "SOUND_BAR", "SPEAKER", "SUBWOOFER"})) return Group::Soundbar;
    if (has({"RECEIVER", "AMPLIFIER", "AMP", "AVR", "TUNER", "PROCESSOR", "PREAMP", "HIFI", "STEREO"}))
        return Group::Receiver;
    if (has({"DVD", "BLU", "BD_", "CD_", "CD-", "VCR", "VIDEO_RECORDER", "LASERDISC", "MINIDISC", "MD_", "TAPE",
             "DECK"}))
        return Group::Disc;
    if (has({"CABLE", "SAT", "STB", "SET_TOP", "SETTOP", "DVR", "PVR", "TIVO", "ROKU", "APPLE_TV", "FIRE", "BOX",
             "MEDIA_PLAYER", "DECODER"}))
        return Group::SetTop;
    if (has({"FAN", "HEATER", "PURIFIER", "HUMIDIFIER", "FIREPLACE"})) return Group::Climate;
    if (has({"LIGHT", "LED", "LAMP", "BULB"})) return Group::Lights;
    if (has({"TV", "LCD", "PLASMA", "TELEVISION", "MONITOR", "DISPLAY", "OLED"})) return Group::TV;
    return Group::Unknown;
}

bool isAirConditioner(const std::string& rel)
{
    const std::string u = upper(rel);
    return u.rfind("ACS/", 0) == 0 || u.find("AIR_CON") != std::string::npos ||
           u.find("AIRCON") != std::string::npos || u.find("/AC/") != std::string::npos ||
           u.find("CLIMATE") != std::string::npos;
}

struct Entry {
    std::string rel;
    Group group;
    std::string brand;
    std::string model;
    std::string brandKey;  // after folding aliases
};

bool describePath(const std::string& rel, Entry& e)
{
    std::vector<std::string> parts;
    for (const auto& p : fs::path(rel)) parts.push_back(p.string());
    if (parts.size() < 2) return false;
    const std::string stem = fs::path(rel).stem().string();
    e.rel = rel;
    if (parts[0] == "_Converted_") {
        // _Converted_/<Source>/<Letter>/<Brand>/[...]/<file>.ir
        if (parts.size() < 5) return false;
        e.brand = parts[3];
        std::string model;
        for (size_t i = 4; i + 1 < parts.size(); ++i) model += parts[i] + " ";
        model += stem;
        e.model = pretty(model);
        e.group = groupFromWords(rel);
        return true;
    }
    if (parts[0] == "Universal_TV_Remotes" || parts[0] == "ACs") return false;
    e.group = groupFromCurated(parts[0]);
    if (parts.size() >= 3) {
        e.brand = parts[1];
        std::string model;
        for (size_t i = 2; i + 1 < parts.size(); ++i) model += parts[i] + " ";
        e.model = pretty(model + stem);
    } else {
        e.brand = "Unknown";
        e.model = pretty(stem);
    }
    if (e.group == Group::Other) {
        const Group g = groupFromWords(rel);
        if (g != Group::Unknown) e.group = g;
    }
    return true;
}

}  // namespace

bool buildIndex(const std::string& rootDir, Index& ix, BuildReport& rep,
                const std::function<void(size_t, size_t)>& progress, const std::string& copyTo)
{
    const fs::path root = rootDir;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
        rep.error = "database folder not found: " + rootDir;
        return false;
    }
    ix = Index{};
    std::unordered_map<std::string, uint32_t> strId;
    auto intern = [&](const std::string& s) {
        auto [it, ins] = strId.emplace(s, static_cast<uint32_t>(ix.strings.size()));
        if (ins) ix.strings.push_back(s);
        return it->second;
    };
    intern("");

    // Brand spelling: prefer a mixed-case spelling from the curated folders.
    std::map<std::string, std::map<std::string, int>> spellings;
    std::vector<Entry> entries;
    std::vector<std::string> files;
    for (auto& f : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
        if (!f.is_regular_file() || f.path().extension() != ".ir") continue;
        files.push_back(fs::relative(f.path(), root).generic_string());
    }
    std::sort(files.begin(), files.end());

    long skippedAc = 0, skippedNoPower = 0, skippedUniversal = 0, skippedPath = 0;
    std::map<std::vector<std::pair<int, uint64_t>>, uint32_t> codesets;
    std::map<Group, int> groupCounts;
    long powerDecoded = 0, powerRaw = 0;

    size_t done = 0;
    for (const auto& rel : files) {
        if (progress && (++done % 50 == 0 || done == files.size())) progress(done, files.size());
        if (isAirConditioner(rel)) { ++skippedAc; continue; }
        Entry e;
        if (!describePath(rel, e)) { ++skippedPath; continue; }
        const auto buttons = loadIrFile((root / rel).string());

        std::vector<std::pair<int, uint64_t>> set;
        std::vector<std::pair<Btn, const IrButton*>> rawPower;
        int powerCount = 0;
        for (const auto& b : buttons) {
            const Btn cls = classifyButton(b.name);
            if (cls == Btn::None) continue;
            const uint64_t key = buttonKey(b);
            if (isPowerClass(cls)) ++powerCount;
            if (key) set.emplace_back(static_cast<int>(cls), key);
            // Keep raw-recorded power signals for fuzzy matching even when they decode:
            // a marginal live capture may fail to decode where the shape still matches.
            if (isPowerClass(cls) && !b.parsed) rawPower.emplace_back(cls, &b);
        }
        // Brute-force / universal lists contain dozens of power codes; they would match everything.
        if (powerCount > 6) { ++skippedUniversal; continue; }
        const bool hasPowerKey = std::any_of(set.begin(), set.end(),
                                             [](auto& p) { return isPowerClass(static_cast<Btn>(p.first)); });
        if (!hasPowerKey && rawPower.empty()) { ++skippedNoPower; continue; }
        if (hasPowerKey) ++powerDecoded; else ++powerRaw;

        std::sort(set.begin(), set.end());
        set.erase(std::unique(set.begin(), set.end()), set.end());
        auto [csIt, csNew] = codesets.emplace(set, static_cast<uint32_t>(codesets.size()));
        (void)csNew;

        // Distinctive button names hint at the device type when the path doesn't.
        if (e.group == Group::Unknown) {
            int disc = 0, recv = 0;
            for (const auto& b : buttons) {
                const std::string n = upper(b.name);
                if (n.find("EJECT") != std::string::npos || n.find("OPEN") != std::string::npos ||
                    n.find("DISC") != std::string::npos || n.find("CHAPTER") != std::string::npos)
                    ++disc;
                if (n == "PHONO" || n == "TUNER" || n == "TAPE" || n == "AUX" || n == "FM" || n == "AM" ||
                    n.find("SURROUND") != std::string::npos || n.find("BASS") != std::string::npos)
                    ++recv;
            }
            if (disc >= 2) e.group = Group::Disc;
            else if (recv >= 2) e.group = Group::Receiver;
        }

        const uint32_t rid = static_cast<uint32_t>(ix.remotes.size());
        Remote r;
        r.group = e.group;
        r.model = intern(e.model);
        r.path = intern(rel);
        r.codeset = csIt->second;
        ix.remotes.push_back(r);
        entries.push_back(e);
        ++groupCounts[e.group];

        for (auto& [cls, key] : set) ix.sigs.push_back({key, rid, static_cast<Btn>(cls)});
        for (auto& [cls, b] : rawPower) {
            // First frame only (up to the first long gap), capped.
            RawSig rs{rid, cls, static_cast<uint32_t>(ix.rawData.size()), 0};
            for (uint32_t t : b->raw.timings) {
                if (rs.len >= 512) break;
                ix.rawData.push_back(static_cast<uint16_t>(std::min<uint32_t>(t, 65535)));
                ++rs.len;
                if (t > 20000 && rs.len > 8) break;
            }
            ix.rawSigs.push_back(rs);
        }

        if (!copyTo.empty()) {
            const fs::path dst = fs::path(copyTo) / rel;
            fs::create_directories(dst.parent_path());
            fs::copy_file(root / rel, dst, fs::copy_options::overwrite_existing);
        }
    }

    // Fold brand aliases, then resolve display names.
    {
        std::set<std::string> keys;
        for (const auto& e : entries) keys.insert(brandKey(e.brand));
        std::set<std::string> merged;
        for (auto& e : entries) {
            std::string bare;
            const std::string a = aliasKey(e.brand, keys, bare);
            e.brandKey = a.empty() ? brandKey(e.brand) : a;
            if (!a.empty()) merged.insert(e.brand);
            // An alias votes with its bare spelling ("Pinnacle"), never as "TV SAMSUNG".
            spellings[e.brandKey][a.empty() ? e.brand : bare] += (e.rel.rfind("_Converted_", 0) == 0) ? 1 : 100;
        }
        rep.brandsMerged = static_cast<int>(merged.size());
    }
    for (size_t i = 0; i < ix.remotes.size(); ++i) {
        const auto& sp = spellings[entries[i].brandKey];
        std::string best;
        int bestScore = -1;
        for (auto& [s, n] : sp) {
            const bool mixed = std::any_of(s.begin(), s.end(), [](char c) { return std::islower(static_cast<unsigned char>(c)); });
            const int score = n * 2 + (mixed ? 1 : 0);
            if (score > bestScore) { bestScore = score; best = s; }
        }
        if (best.empty()) best = entries[i].brand;  // alias of an alias: keep its own spelling
        ix.remotes[i].brand = intern(pretty(best));
    }

    // Propagate categories: an unknown remote that shares a power code and at least one other
    // code with known-category remotes of the same brand takes their majority category.
    {
        std::unordered_map<uint64_t, std::vector<uint32_t>> byKey;
        std::vector<std::vector<std::pair<Btn, uint64_t>>> keysOf(ix.remotes.size());
        for (const auto& s : ix.sigs) {
            byKey[s.key].push_back(s.remote);
            keysOf[s.remote].emplace_back(s.btn, s.key);
        }
        int propagated = 0;
        for (uint32_t r = 0; r < ix.remotes.size(); ++r) {
            if (ix.remotes[r].group != Group::Unknown) continue;
            std::map<uint32_t, int> shared;  // other remote -> shared key count (power required)
            for (auto& [btn, key] : keysOf[r]) {
                if (!isPowerClass(btn)) continue;
                for (uint32_t o : byKey[key])
                    if (o != r && ix.remotes[o].brand == ix.remotes[r].brand && ix.remotes[o].group != Group::Unknown)
                        shared[o] = 0;
            }
            if (shared.empty()) continue;
            for (auto& [btn, key] : keysOf[r])
                for (uint32_t o : byKey[key])
                    if (shared.count(o)) ++shared[o];
            std::map<Group, int> votes;
            for (auto& [o, n] : shared)
                if (n >= 2) ++votes[ix.remotes[o].group];
            if (votes.empty()) continue;
            auto best = std::max_element(votes.begin(), votes.end(), [](auto& a, auto& b) { return a.second < b.second; });
            --groupCounts[Group::Unknown];
            ++groupCounts[best->first];
            ix.remotes[r].group = best->first;
            ++propagated;
        }
        rep.propagated = propagated;
    }

    ix.codesetCount = static_cast<uint32_t>(codesets.size());
    ix.builderRev = kBuilderRev;
    std::sort(ix.sigs.begin(), ix.sigs.end(),
              [](const Sig& a, const Sig& b) { return a.key != b.key ? a.key < b.key : a.remote < b.remote; });
    ix.buildDerived();
    rep.files = files.size();
    rep.powerDecoded = powerDecoded;
    rep.powerRaw = powerRaw;
    rep.skippedAc = skippedAc;
    rep.skippedUniversal = skippedUniversal;
    rep.skippedNoPower = skippedNoPower;
    rep.skippedPath = skippedPath;
    for (auto& [g, n] : groupCounts) rep.groups[g] = n;
    if (ix.remotes.empty()) {
        rep.error = "no usable remotes found in " + rootDir;
        return false;
    }
    return true;
}

}  // namespace bh
