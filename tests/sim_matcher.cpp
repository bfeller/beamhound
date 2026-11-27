// End-to-end matcher simulation. Two variants, both must be 100% correct:
//   with remote: a virtual user holds a real database remote and answers every question truthfully.
//   no remote:   the user only knows the brand (and maybe the category), steps through the brand's
//                power codes until the device reacts, then answers test-send questions.
// Measures question count and final correctness.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <random>
#include <string>

#include "core/matcher.hpp"

using namespace bh;

static std::vector<uint32_t> distort(const std::vector<uint32_t>& in, std::mt19937& rng)
{
    std::uniform_int_distribution<int> j(-70, 70);
    std::vector<uint32_t> out;
    for (size_t i = 0; i < in.size(); ++i)
        out.push_back(static_cast<uint32_t>(std::max(50, static_cast<int>(in[i]) + j(rng) + (i % 2 ? -90 : 90))));
    return out;
}

// Does `pick` control the device that `truth` controls? Every button both have must share a key.
static bool compatible(const Index& ix, uint32_t truth, uint32_t pick, int& shared)
{
    std::map<int, std::vector<uint64_t>> a, b;
    for (const Sig* s = ix.remoteSigsBegin(truth); s != ix.remoteSigsEnd(truth); ++s) a[(int)s->btn].push_back(s->key);
    for (const Sig* s = ix.remoteSigsBegin(pick); s != ix.remoteSigsEnd(pick); ++s) b[(int)s->btn].push_back(s->key);
    shared = 0;
    for (auto& [btn, ka] : a) {
        auto it = b.find(btn);
        if (it == b.end()) continue;
        ++shared;
        bool any = false;
        for (auto k : ka) any |= std::find(it->second.begin(), it->second.end(), k) != it->second.end();
        if (!any) return false;
    }
    return true;
}

// Answer the matcher's questions truthfully as the owner of `truth` until it is done.
static int askQuestions(const Index& ix, Matcher& m, uint32_t truth, const std::vector<IrButton>& buttons,
                        std::mt19937& rng, std::map<std::string, int>& kinds, int maxQ)
{
    int q = 0;
    while (!m.done() && q < maxQ) {
        Question qu = m.next();
        ++q;
        switch (qu.kind) {
        case Question::Kind::PressButton: {
            kinds["press"]++;
            const IrButton* hb = nullptr;
            for (auto& b : buttons) if (classifyButton(b.name) == qu.btn && buttonKey(b)) { hb = &b; break; }
            std::string msg;
            if (hb) m.answerPress(qu.btn, distort(buttonTimings(*hb).timings, rng), msg);
            else m.answerNoSuchButton(qu.btn);
            break;
        }
        case Question::Kind::Category: {
            kinds["category"]++;
            Group g = ix.remotes[truth].group;
            if (g == Group::Unknown) m.answerCategory(static_cast<Group>(qu.values.front()));  // user guesses
            else m.answerCategory(g);
            break;
        }
        case Question::Kind::Brand: {
            kinds["brand"]++;
            uint32_t br = ix.remotes[truth].brand;
            if (std::find(qu.values.begin(), qu.values.end(), br) != qu.values.end()) m.answerBrand(br);
            else m.answerBrandOther(qu.values);
            break;
        }
        case Question::Kind::TestSend: {
            kinds["test"]++;
            auto sent = ix.keysFor(qu.testRemote, qu.btn);
            auto mine = ix.keysFor(truth, qu.btn);
            bool reacted = std::any_of(sent.begin(), sent.end(), [&](uint64_t k) {
                return std::find(mine.begin(), mine.end(), k) != mine.end(); });
            m.answerTest(qu.testRemote, qu.btn, reacted);
            break;
        }
        case Question::Kind::PickRemote:
            kinds["pick"]++;
            {   // The pick screen lets the user test each option; they choose one that works.
                uint32_t choice = qu.values.front();
                for (uint32_t r : qu.values) { int sh; if (compatible(ix, truth, r, sh)) { choice = r; break; } }
                m.answerPick(choice);
            }
            break;
        default: break;
        }
    }
    return q;
}

// Does the device (controlled by `truth`, switched ON as the screen asks) react to power code `c`?
static bool reactsToPower(const Index& ix, uint32_t truth, const PowerCode& c)
{
    // A device that is on reacts to toggle Power and Power Off; Power On entries are only offered
    // for remotes without either, and the screen then asks for the device to be off.
    auto accepts = [&](Btn b) {
        return c.btn == Btn::PowerOn ? b == Btn::PowerOn : (b == Btn::Power || b == Btn::PowerOff);
    };
    if (c.key) {
        for (const Sig* s = ix.remoteSigsBegin(truth); s != ix.remoteSigsEnd(truth); ++s)
            if (s->key == c.key && accepts(s->btn)) return true;
        return false;
    }
    const RawSig& ref = ix.rawSigs[c.raw];
    for (const auto& rs : ix.rawSigs) {
        if (rs.remote != truth || !accepts(rs.btn)) continue;
        std::vector<uint32_t> t(ix.rawData.begin() + rs.offset, ix.rawData.begin() + rs.offset + rs.len);
        if (rawSimilarity(ix.rawData.data() + ref.offset, ref.len, t) >= 0.85) return true;
    }
    return false;
}

// No original remote: the user picks the brand, maybe the category, then presses Space through
// the power codes until the device reacts, then answers test-send questions.
static bool noRemoteSim(const Index& ix, int n, std::mt19937& rng)
{
    // Every remote with a usable power signal (a decodable one, or a raw frame of 8+ timings).
    std::vector<uint32_t> pool;
    std::vector<bool> rawOk(ix.remotes.size(), false);
    for (const auto& rs : ix.rawSigs)
        if (rs.len >= 8) rawOk[rs.remote] = true;
    int unusable = 0;
    for (uint32_t r = 0; r < ix.remotes.size(); ++r) {
        bool usable = rawOk[r];
        for (const Sig* s = ix.remoteSigsBegin(r); s != ix.remoteSigsEnd(r) && !usable; ++s) usable = isPowerClass(s->btn);
        if (usable) pool.push_back(r);
        else ++unusable;
    }
    std::shuffle(pool.begin(), pool.end(), rng);

    int ok = 0, wrong = 0, notFound = 0, total = 0, maxQ = 0, maxPress = 0;
    long questions = 0, presses = 0;
    std::map<int, int> pressHist, qHist;
    std::map<std::string, int> kinds;
    const std::vector<IrButton> noButtons;
    for (int t = 0; t < n && t < (int)pool.size(); ++t) {
        const uint32_t truth = pool[t];
        ++total;
        Matcher m(ix);
        m.startBrand(ix.remotes[truth].brand);
        const Question cq = m.categoryQuestion();
        if (cq.kind == Question::Kind::Category) {
            const Group g = ix.remotes[truth].group;
            if (std::find(cq.values.begin(), cq.values.end(), static_cast<uint32_t>(g)) != cq.values.end())
                m.answerCategory(g);
            else
                m.skip(cq);  // "Not sure"
        }
        const auto codes = m.powerCodes();
        int press = 0;
        const PowerCode* hit = nullptr;
        for (const auto& c : codes) {
            ++press;
            if (reactsToPower(ix, truth, c)) { hit = &c; break; }
        }
        if (!hit) {
            ++wrong;
            ++notFound;
            if (std::getenv("SIM_DEBUG"))
                std::printf("NOCODE %s (%zu codes)\n", ix.str(ix.remotes[truth].path).c_str(), codes.size());
            continue;
        }
        m.answerPowerCode(*hit);
        const int q = askQuestions(ix, m, truth, noButtons, rng, kinds, 25);
        int shared;
        const bool good = compatible(ix, truth, m.bestRemote(), shared);
        (good ? ok : wrong)++;
        if (!good && std::getenv("SIM_DEBUG"))
            std::printf("WRONG-NR truth=%s | pick=%s | q=%d left=%zu codesets=%zu done=%d\n",
                        ix.str(ix.remotes[truth].path).c_str(), ix.str(ix.remotes[m.bestRemote()].path).c_str(), q,
                        m.remoteCount(), m.codesetCount(), (int)m.done());
        presses += press;
        questions += q;
        maxPress = std::max(maxPress, press);
        maxQ = std::max(maxQ, q);
        pressHist[press <= 1 ? 1 : press <= 3 ? 3 : press <= 10 ? 10 : press <= 25 ? 25 : 999]++;
        qHist[q]++;
    }
    std::printf("no remote: simulated %d users: %d correct (%.1f%%), %d wrong (%d power code not found); "
                "avg %.2f presses (max %d), avg %.2f questions (max %d)\n",
                total, ok, 100.0 * ok / total, wrong, notFound, (double)presses / std::max(1, total - notFound),
                maxPress, (double)questions / std::max(1, total - notFound), maxQ);
    std::printf("  Space presses until the device reacted: ");
    for (auto& [k, v] : pressHist) std::printf("<=%d:%d  ", k, v);
    std::printf("\n  questions histogram: "); for (auto& [k, v] : qHist) std::printf("%d:%d  ", k, v);
    std::printf("\n  question kinds: "); for (auto& [k, v] : kinds) std::printf("%s:%d  ", k.c_str(), v);
    std::printf("\n  remotes without a usable power signal (not searchable): %d\n", unusable);
    return ok == total;
}

int main(int argc, char** argv)
{
    if (argc < 2) { std::fprintf(stderr, "usage: sim <index.bin> [n]\n"); return 2; }
    Index ix;
    std::string err;
    if (!ix.load(argv[1], err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 1; }
    const int n = argc > 2 ? std::atoi(argv[2]) : 400;
    std::mt19937 rng(7);
    std::vector<uint32_t> pool;
    for (uint32_t r = 0; r < ix.remotes.size(); ++r)
        for (const Sig* s = ix.remoteSigsBegin(r); s != ix.remoteSigsEnd(r); ++s)
            if (s->btn == Btn::Power) { pool.push_back(r); break; }
    std::shuffle(pool.begin(), pool.end(), rng);

    int ok = 0, wrong = 0, total = 0, maxQ = 0;
    std::map<int, int> qHist, startCands;
    std::map<std::string, int> kinds;
    long questions = 0;
    for (int t = 0; t < n && t < (int)pool.size(); ++t) {
        const uint32_t truth = pool[t];
        const auto power = ix.keysFor(truth, Btn::Power);
        // Find the power code's protocol by re-reading the file is overkill: re-encode via the index key.
        // Instead, load the file to get the actual signal.
        auto buttons = loadIrFile("build/data/irdb/" + ix.str(ix.remotes[truth].path));
        const IrButton* pb = nullptr;
        for (auto& b : buttons) if (classifyButton(b.name) == Btn::Power && buttonKey(b)) { pb = &b; break; }
        if (!pb) continue;
        Matcher m(ix);
        {
            auto tim = distort(buttonTimings(*pb).timings, rng);
            if (m.start(tim) != Matcher::StartResult::Matched) {
                ++wrong; ++total;
                if (std::getenv("SIM_DEBUG")) {
                    auto clean = decode(buttonTimings(*pb).timings);
                    std::printf("NOSTART %s btn=%s parsed=%d file=%s clean=%s distorted=%s\n", ix.str(ix.remotes[truth].path).c_str(),
                        pb->name.c_str(), (int)pb->parsed, pb->parsed ? describe(pb->code).c_str() : "raw",
                        clean ? describe(*clean).c_str() : "none", m.heard() ? describe(*m.heard()).c_str() : "none");
                }
                continue;
            }
        }
        int start = (int)m.codesetCount();
        startCands[start <= 1 ? 1 : start <= 5 ? 5 : start <= 20 ? 20 : start <= 100 ? 100 : 999]++;
        const int q = askQuestions(ix, m, truth, buttons, rng, kinds, 15);
        int shared;
        const bool good = compatible(ix, truth, m.bestRemote(), shared);
        (good ? ok : wrong)++;
        if (!good && std::getenv("SIM_DEBUG")) {
            uint32_t pick = m.bestRemote();
            std::printf("WRONG truth=%s | pick=%s | q=%d start=%d left=%zu codesets=%zu done=%d\n",
                        ix.str(ix.remotes[truth].path).c_str(), ix.str(ix.remotes[pick].path).c_str(), q, start,
                        m.remoteCount(), m.codesetCount(), (int)m.done());
            std::map<int, std::vector<uint64_t>> a, b;
            for (const Sig* s = ix.remoteSigsBegin(truth); s != ix.remoteSigsEnd(truth); ++s) a[(int)s->btn].push_back(s->key);
            for (const Sig* s = ix.remoteSigsBegin(pick); s != ix.remoteSigsEnd(pick); ++s) b[(int)s->btn].push_back(s->key);
            for (auto& [btn, ka] : a) if (b.count(btn)) {
                bool any=false; for (auto k: ka) any |= std::find(b[btn].begin(), b[btn].end(), k) != b[btn].end();
                if (!any) std::printf("   conflict on %s: truth %llx pick %llx\n", btnLabel((Btn)btn), (unsigned long long)ka[0], (unsigned long long)b[btn][0]);
            }
        }
        ++total;
        questions += q;
        maxQ = std::max(maxQ, q);
        qHist[q]++;
    }
    std::printf("with remote: simulated %d users: %d correct (%.1f%%), %d wrong; avg %.2f questions, max %d\n", total, ok,
                100.0 * ok / total, wrong, (double)questions / total, maxQ);
    std::printf("  code sets after POWER: "); for (auto& [k, v] : startCands) std::printf("<=%d:%d  ", k, v); std::printf("\n");
    std::printf("  questions histogram: "); for (auto& [k, v] : qHist) std::printf("%d:%d  ", k, v); std::printf("\n");
    std::printf("  question kinds: "); for (auto& [k, v] : kinds) std::printf("%s:%d  ", k.c_str(), v); std::printf("\n");

    const bool nrOk = noRemoteSim(ix, n, rng);
    const bool allOk = ok == total && nrOk;
    return allOk ? 0 : 1;
}
