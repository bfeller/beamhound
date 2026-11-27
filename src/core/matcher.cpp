#include "core/matcher.hpp"

#include <algorithm>
#include <functional>
#include <map>
#include <unordered_map>

namespace bh {
namespace {

// Buttons we may ask the user to press, in order of how common they are on remotes.
constexpr Btn kPressOrder[] = {Btn::VolUp, Btn::Mute, Btn::ChUp, Btn::VolDown, Btn::ChDown, Btn::Input,
                               Btn::Menu,  Btn::Ok,   Btn::Num1, Btn::Up,      Btn::Home,   Btn::Back,
                               Btn::Info,  Btn::Guide, Btn::PlayPause, Btn::Play, Btn::Stop, Btn::Speed,
                               Btn::Oscillate, Btn::Brightness_Up, Btn::Timer};
// Buttons with an effect the user can see when we transmit them.
constexpr Btn kTestOrder[] = {Btn::Mute, Btn::VolUp, Btn::Input, Btn::Menu, Btn::Speed, Btn::Power};

constexpr double kEffortPress = 1.0;
constexpr double kEffortCategory = 1.15;
constexpr double kEffortBrand = 1.3;
constexpr double kEffortTest = 1.6;

int askKey(Question::Kind k, Btn b) { return (static_cast<int>(k) << 8) | static_cast<int>(b); }

bool isConverted(const std::string& path) { return path.rfind("_Converted_", 0) == 0; }

}  // namespace

uint32_t Matcher::richness(uint32_t r) const
{
    const auto n = static_cast<uint32_t>(ix_.remoteSigsEnd(r) - ix_.remoteSigsBegin(r));
    return n * 2 + (isConverted(ix_.str(ix_.remotes[r].path)) ? 0 : 10);
}

void Matcher::push() { history_.push_back({cand_, asked_, tested_}); }

void Matcher::setCandidates(std::vector<uint32_t> next)
{
    std::sort(next.begin(), next.end());
    next.erase(std::unique(next.begin(), next.end()), next.end());
    cand_ = std::move(next);
}

bool Matcher::undo()
{
    if (history_.empty()) return false;
    cand_ = std::move(history_.back().cand);
    asked_ = std::move(history_.back().asked);
    tested_ = std::move(history_.back().tested);
    history_.pop_back();
    return true;
}

void Matcher::reset()
{
    cand_.clear();
    asked_.clear();
    tested_.clear();
    history_.clear();
    heard_.reset();
    fuzzy_ = false;
    noRemote_ = false;
}

Matcher::StartResult Matcher::start(const std::vector<uint32_t>& timings)
{
    reset();
    heard_ = decode(timings);
    std::vector<uint32_t> found;
    if (heard_) {
        const uint64_t key = canonicalKey(*heard_);
        auto lo = std::lower_bound(ix_.sigs.begin(), ix_.sigs.end(), key,
                                   [](const Sig& s, uint64_t k) { return s.key < k; });
        for (auto it = lo; it != ix_.sigs.end() && it->key == key; ++it)
            if (isPowerClass(it->btn)) found.push_back(it->remote);
    }
    if (found.empty()) {
        // Remotes stored only as raw timings: compare signal shapes.
        for (const auto& rs : ix_.rawSigs)
            if (rawSimilarity(ix_.rawData.data() + rs.offset, rs.len, timings) >= 0.85) found.push_back(rs.remote);
        fuzzy_ = !found.empty();
    }
    setCandidates(std::move(found));
    if (!cand_.empty()) return StartResult::Matched;
    return heard_ ? StartResult::NoMatch : StartResult::NotDecoded;
}

size_t Matcher::codesetCount() const
{
    std::vector<uint32_t> cs;
    for (uint32_t r : cand_) cs.push_back(ix_.remotes[r].codeset);
    std::sort(cs.begin(), cs.end());
    return static_cast<size_t>(std::unique(cs.begin(), cs.end()) - cs.begin());
}

// Representative (richest) remote and weight (remote count) of each code set.
static std::vector<std::pair<uint32_t, uint32_t>> codesetReps(const Index& ix, const std::vector<uint32_t>& cand,
                                                               const std::function<uint32_t(uint32_t)>& rich)
{
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> m;  // codeset -> (rep, weight)
    for (uint32_t r : cand) {
        auto [it, ins] = m.emplace(ix.remotes[r].codeset, std::make_pair(r, 0u));
        ++it->second.second;
        if (!ins && rich(r) > rich(it->second.first)) it->second.first = r;
    }
    std::vector<std::pair<uint32_t, uint32_t>> out;
    for (auto& [cs, rw] : m) out.push_back(rw);
    std::sort(out.begin(), out.end(), [](auto& a, auto& b) { return a.second != b.second ? a.second > b.second : a.first < b.first; });
    return out;
}

bool Matcher::done() const
{
    if (cand_.empty()) return true;
    auto reps = codesetReps(ix_, cand_, [this](uint32_t r) { return richness(r); });
    if (reps.size() <= 1) return true;
    // Compatible when, for every button, some key is shared by every code set that has the button.
    std::map<int, std::map<uint64_t, int>> keyCount;
    std::map<int, int> haveCount;
    for (auto& [rep, w] : reps) {
        std::map<int, std::vector<uint64_t>> perBtn;
        for (const Sig* s = ix_.remoteSigsBegin(rep); s != ix_.remoteSigsEnd(rep); ++s)
            perBtn[static_cast<int>(s->btn)].push_back(s->key);
        for (auto& [b, keys] : perBtn) {
            ++haveCount[b];
            std::sort(keys.begin(), keys.end());
            keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
            for (uint64_t k : keys) ++keyCount[b][k];
        }
    }
    for (auto& [b, n] : haveCount) {
        bool common = false;
        for (auto& [k, c] : keyCount[b])
            if (c == n) common = true;
        if (!common) return false;
    }
    return true;
}

Question Matcher::next() const
{
    Question best;
    if (done()) return best;
    auto reps = codesetReps(ix_, cand_, [this](uint32_t r) { return richness(r); });
    double W = 0;
    for (auto& [rep, w] : reps) W += w;
    double bestScore = 0.97;  // must remove at least ~3% (effort-weighted) to be worth asking

    // --- press a button on your remote (only when the user has it)
    for (Btn b : kPressOrder) {
        if (noRemote_) break;
        if (asked_.count(askKey(Question::Kind::PressButton, b))) continue;
        std::map<uint64_t, double> bucket;
        double none = 0;
        for (auto& [rep, w] : reps) {
            auto keys = ix_.keysFor(rep, b);
            if (keys.empty()) none += w;
            for (uint64_t k : keys) bucket[k] += w;
        }
        const double has = W - none;
        if (bucket.size() < 2 || has < 0.5 * W) continue;
        double exp = 0;
        for (auto& [k, w] : bucket) exp += (w / has) * (w + none);
        const double score = exp / W * kEffortPress;
        if (score < bestScore) {
            bestScore = score;
            best = Question{Question::Kind::PressButton, b, 0, {}, {}};
        }
    }

    // --- device category
    if (!asked_.count(askKey(Question::Kind::Category, Btn::None))) {
        std::map<Group, double> bucket;
        double unknown = 0;
        for (uint32_t r : cand_) {
            const Group g = ix_.remotes[r].group;
            if (g == Group::Unknown) unknown += 1;
            else bucket[g] += 1;
        }
        const double total = static_cast<double>(cand_.size()), known = total - unknown;
        if (bucket.size() >= 2) {
            double exp = 0;
            for (auto& [g, w] : bucket) exp += (w / known) * (w + unknown);
            const double score = exp / total * kEffortCategory;
            if (score < bestScore) {
                bestScore = score;
                best = categoryQuestion();
            }
        }
    }

    // --- brand
    if (!asked_.count(askKey(Question::Kind::Brand, Btn::None))) {
        std::map<uint32_t, double> bucket;
        for (uint32_t r : cand_) bucket[ix_.remotes[r].brand] += 1;
        if (bucket.size() >= 2) {
            const double total = static_cast<double>(cand_.size());
            double exp = 0;
            for (auto& [b, w] : bucket) exp += (w / total) * w;
            const double score = exp / total * kEffortBrand;
            if (score < bestScore) {
                bestScore = score;
                best = Question{Question::Kind::Brand, Btn::None, 0, {}, {}};
                std::vector<std::pair<double, uint32_t>> order;
                for (auto& [b, w] : bucket) order.emplace_back(w, b);
                std::sort(order.begin(), order.end(), [&](auto& a, auto& b) {
                    return a.first != b.first ? a.first > b.first : ix_.str(a.second) < ix_.str(b.second);
                });
                for (size_t i = 0; i < order.size() && i < 9; ++i) {
                    best.options.push_back(ix_.str(order[i].second));
                    best.values.push_back(order[i].second);
                }
            }
        }
    }

    // --- transmit a test signal and ask whether the device reacted
    for (Btn b : kTestOrder) {
        std::map<uint64_t, std::pair<double, uint32_t>> bucket;  // key -> (weight, a remote with it)
        for (auto& [rep, w] : reps)
            for (uint64_t k : ix_.keysFor(rep, b)) {
                auto& e = bucket[k];
                if (e.first == 0) e.second = rep;
                e.first += w;
            }
        for (auto& [k, e] : bucket) {
            if (tested_.count(k) || e.first >= W) continue;
            const double p = e.first / W;
            const double exp = p * e.first + (1 - p) * (W - e.first);
            const double score = exp / W * kEffortTest;
            if (score < bestScore) {
                bestScore = score;
                best = Question{Question::Kind::TestSend, b, e.second, {}, {}};
            }
        }
    }

    // Nothing cheap left: test-send any button on which the remaining code sets still disagree.
    if (best.kind == Question::Kind::None) {
        double fallback = 1.0;
        for (int bi = 1; bi < static_cast<int>(Btn::Count); ++bi) {
            const Btn b = static_cast<Btn>(bi);
            std::map<uint64_t, std::pair<double, uint32_t>> bucket;
            for (auto& [rep, w] : reps)
                for (uint64_t k : ix_.keysFor(rep, b)) {
                    auto& e = bucket[k];
                    if (e.first == 0) e.second = rep;
                    e.first += w;
                }
            if (bucket.size() < 2) continue;
            for (auto& [k, e] : bucket) {
                if (tested_.count(k) || e.first >= W) continue;
                const double p = e.first / W;
                const double exp = (p * e.first + (1 - p) * (W - e.first)) / W;
                if (exp < fallback) {
                    fallback = exp;
                    best = Question{Question::Kind::TestSend, b, e.second, {}, {}};
                }
            }
        }
    }

    if (best.kind == Question::Kind::None) {
        best.kind = Question::Kind::PickRemote;
        for (uint32_t r : rankedRemotes(8)) {
            best.options.push_back(ix_.str(ix_.remotes[r].brand) + " " + ix_.str(ix_.remotes[r].model));
            best.values.push_back(r);
        }
    }
    return best;
}

bool Matcher::answerPress(Btn b, const std::vector<uint32_t>& timings, std::string& message)
{
    const auto code = decode(timings);
    if (!code) {
        message = "Couldn't read that signal. Try again, holding the remote closer.";
        return false;
    }
    const uint64_t key = canonicalKey(*code);
    std::vector<uint32_t> keep, lacking;
    for (uint32_t r : cand_) {
        auto keys = ix_.keysFor(r, b);
        if (keys.empty()) lacking.push_back(r);
        else if (std::find(keys.begin(), keys.end(), key) != keys.end()) keep.push_back(r);
    }
    push();
    asked_.insert(askKey(Question::Kind::PressButton, b));
    if (!keep.empty()) {
        message = "";
        setCandidates(keep);
        return true;
    }
    if (!lacking.empty() && lacking.size() < cand_.size()) {
        message = "No match for that code; keeping remotes that don't list this button.";
        setCandidates(lacking);
        return true;
    }
    history_.pop_back();
    message = "That code didn't match any candidate, so I'll skip this one.";
    return false;
}

void Matcher::answerNoSuchButton(Btn b) { asked_.insert(askKey(Question::Kind::PressButton, b)); }

void Matcher::skip(const Question& q)
{
    if (q.kind == Question::Kind::TestSend) {
        for (uint64_t k : ix_.keysFor(q.testRemote, q.btn)) tested_.insert(k);
        return;
    }
    asked_.insert(askKey(q.kind, q.kind == Question::Kind::PressButton ? q.btn : Btn::None));
}

void Matcher::answerCategory(Group g)
{
    std::vector<uint32_t> keep;
    for (uint32_t r : cand_)
        if (ix_.remotes[r].group == g || ix_.remotes[r].group == Group::Unknown) keep.push_back(r);
    push();
    asked_.insert(askKey(Question::Kind::Category, Btn::None));
    if (!keep.empty()) setCandidates(keep);
}

void Matcher::answerBrand(uint32_t brand)
{
    std::vector<uint32_t> keep;
    for (uint32_t r : cand_)
        if (ix_.remotes[r].brand == brand) keep.push_back(r);
    push();
    asked_.insert(askKey(Question::Kind::Brand, Btn::None));
    if (!keep.empty()) setCandidates(keep);
}

void Matcher::answerBrandOther(const std::vector<uint32_t>& listed)
{
    std::vector<uint32_t> keep;
    for (uint32_t r : cand_)
        if (std::find(listed.begin(), listed.end(), ix_.remotes[r].brand) == listed.end()) keep.push_back(r);
    push();
    asked_.insert(askKey(Question::Kind::Brand, Btn::None));
    if (!keep.empty()) setCandidates(keep);
}

void Matcher::answerTest(uint32_t remote, Btn b, bool reacted)
{
    const auto sent = ix_.keysFor(remote, b);
    std::vector<uint32_t> keep;
    for (uint32_t r : cand_) {
        const auto keys = ix_.keysFor(r, b);
        const bool shares = std::any_of(keys.begin(), keys.end(), [&](uint64_t k) {
            return std::find(sent.begin(), sent.end(), k) != sent.end();
        });
        if (shares == reacted) keep.push_back(r);
    }
    push();
    for (uint64_t k : sent) tested_.insert(k);
    if (!keep.empty()) setCandidates(keep);
}

void Matcher::answerPick(uint32_t remote)
{
    std::vector<uint32_t> keep;
    for (uint32_t r : cand_)
        if (ix_.remotes[r].codeset == ix_.remotes[remote].codeset) keep.push_back(r);
    push();
    setCandidates(keep.empty() ? std::vector<uint32_t>{remote} : keep);
}

// ------------------------------------------------------------------ no original remote

void Matcher::startBrand(uint32_t brand)
{
    reset();
    noRemote_ = true;
    std::vector<uint32_t> c;
    for (uint32_t r = 0; r < ix_.remotes.size(); ++r)
        if (ix_.remotes[r].brand == brand) c.push_back(r);
    setCandidates(std::move(c));
}

Question Matcher::categoryQuestion() const
{
    Question q;
    std::map<Group, double> bucket;
    for (uint32_t r : cand_)
        if (ix_.remotes[r].group != Group::Unknown) bucket[ix_.remotes[r].group] += 1;
    if (bucket.size() < 2) return q;
    q.kind = Question::Kind::Category;
    std::vector<std::pair<double, Group>> order;
    for (auto& [g, w] : bucket) order.emplace_back(w, g);
    std::sort(order.rbegin(), order.rend());
    for (auto& [w, g] : order) {
        q.options.push_back(groupLabel(g));
        q.values.push_back(static_cast<uint32_t>(g));
    }
    return q;
}

namespace {
constexpr double kRawSame = 0.85;  // same threshold as matching a live capture
constexpr uint16_t kRawMinLen = 8;  // shorter "signals" are recording junk, not a sendable frame

std::vector<uint32_t> rawTimings(const Index& ix, const RawSig& rs)
{
    return std::vector<uint32_t>(ix.rawData.begin() + rs.offset, ix.rawData.begin() + rs.offset + rs.len);
}
}  // namespace

std::vector<PowerCode> Matcher::powerCodes() const
{
    std::vector<PowerCode> out;
    std::vector<int> tier;  // 0 = toggle / off, 1 = power-on only
    std::unordered_map<uint64_t, size_t> byKey, byOnKey;
    std::set<uint32_t> covered;

    // Raw power signals of the candidates, grouped by remote.
    std::unordered_map<uint32_t, std::vector<size_t>> rawOf;
    for (size_t i = 0; i < ix_.rawSigs.size(); ++i)
        if (std::binary_search(cand_.begin(), cand_.end(), ix_.rawSigs[i].remote))
            rawOf[ix_.rawSigs[i].remote].push_back(i);

    auto addKeys = [&](uint32_t r, bool onOnly, int t) {
        std::set<size_t> counted;
        for (const Sig* s = ix_.remoteSigsBegin(r); s != ix_.remoteSigsEnd(r); ++s) {
            const bool want = onOnly ? s->btn == Btn::PowerOn : (s->btn == Btn::Power || s->btn == Btn::PowerOff);
            if (!want) continue;
            // Power On is its own pass (device switched off), so it never merges with a toggle code.
            auto [it, ins] = (onOnly ? byOnKey : byKey).emplace(s->key, out.size());
            if (ins) {
                out.push_back({s->btn, s->key, -1, r, 0});
                tier.push_back(t);
            }
            if (counted.insert(it->second).second) ++out[it->second].weight;
        }
        return !counted.empty();
    };
    auto addRaw = [&](uint32_t r, bool onOnly, int t) {
        auto it = rawOf.find(r);
        if (it == rawOf.end()) return false;
        std::set<size_t> counted;
        for (size_t ri : it->second) {
            const RawSig& rs = ix_.rawSigs[ri];
            const bool want = onOnly ? rs.btn == Btn::PowerOn : (rs.btn == Btn::Power || rs.btn == Btn::PowerOff);
            if (!want || rs.len < kRawMinLen) continue;
            const auto shape = rawTimings(ix_, rs);
            size_t hit = out.size();
            for (size_t e = 0; e < out.size() && hit == out.size(); ++e)
                if (out[e].key == 0 && tier[e] == t && rawSimilarity(ix_.rawData.data() + ix_.rawSigs[out[e].raw].offset,
                                                     ix_.rawSigs[out[e].raw].len, shape) >= kRawSame)
                    hit = e;
            if (hit == out.size()) {
                out.push_back({rs.btn, 0, static_cast<int>(ri), r, 0});
                tier.push_back(t);
            }
            if (counted.insert(hit).second) ++out[hit].weight;
        }
        return !counted.empty();
    };

    for (uint32_t r : cand_)
        if (addKeys(r, false, 0)) covered.insert(r);
    for (uint32_t r : cand_)
        if (!covered.count(r) && addRaw(r, false, 0)) covered.insert(r);
    for (uint32_t r : cand_) {
        if (covered.count(r)) continue;
        const bool keyed = addKeys(r, true, 1);
        const bool raw = addRaw(r, true, 1);  // both: a remote may store several power-on signals
        if (keyed || raw) covered.insert(r);
    }

    std::vector<size_t> order(out.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
        if (tier[a] != tier[b]) return tier[a] < tier[b];
        return out[a].weight > out[b].weight;
    });
    std::vector<PowerCode> sorted;
    for (size_t i : order) sorted.push_back(out[i]);
    return sorted;
}

void Matcher::answerPowerCode(const PowerCode& c)
{
    std::vector<uint32_t> keep;
    if (c.key) {
        for (uint32_t r : cand_)
            for (const Sig* s = ix_.remoteSigsBegin(r); s != ix_.remoteSigsEnd(r); ++s)
                if (s->key == c.key && isPowerClass(s->btn)) {
                    keep.push_back(r);
                    break;
                }
    } else if (c.raw >= 0) {
        const RawSig& ref = ix_.rawSigs[c.raw];
        for (const auto& rs : ix_.rawSigs)
            if (std::binary_search(cand_.begin(), cand_.end(), rs.remote) &&
                rawSimilarity(ix_.rawData.data() + ref.offset, ref.len, rawTimings(ix_, rs)) >= kRawSame)
                keep.push_back(rs.remote);
    }
    push();
    // The power signal is settled: never test-send it again.
    if (c.key) tested_.insert(c.key);
    for (uint64_t k : ix_.keysFor(c.remote, c.btn)) tested_.insert(k);
    if (!keep.empty()) setCandidates(std::move(keep));
}

std::vector<uint32_t> Matcher::rankedRemotes(size_t max) const
{
    auto reps = codesetReps(ix_, cand_, [this](uint32_t r) { return richness(r); });
    std::vector<uint32_t> out;
    for (size_t i = 0; i < reps.size() && i < max; ++i) out.push_back(reps[i].first);
    return out;
}

uint32_t Matcher::bestRemote() const
{
    if (cand_.empty()) return 0;
    if (done()) {
        // All remaining code sets are compatible: take the one with the most buttons.
        auto all = rankedRemotes(cand_.size());
        return *std::max_element(all.begin(), all.end(),
                                 [this](uint32_t a, uint32_t b) { return richness(a) < richness(b); });
    }
    return rankedRemotes(1).front();
}

}  // namespace bh
