// End-to-end matcher simulation: a virtual user holds a real database remote and
// answers every question truthfully. Measures question count and final correctness.
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
        int q = 0;
        while (!m.done() && q < 15) {
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
    std::printf("simulated %d users: %d correct (%.1f%%), %d wrong; avg %.2f questions, max %d\n", total, ok,
                100.0 * ok / total, wrong, (double)questions / total, maxQ);
    std::printf("code sets after POWER: "); for (auto& [k, v] : startCands) std::printf("<=%d:%d  ", k, v); std::printf("\n");
    std::printf("questions histogram: "); for (auto& [k, v] : qHist) std::printf("%d:%d  ", k, v); std::printf("\n");
    std::printf("question kinds: "); for (auto& [k, v] : kinds) std::printf("%s:%d  ", k.c_str(), v); std::printf("\n");
    return 0;
}
