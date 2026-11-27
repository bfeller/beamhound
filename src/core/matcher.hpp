// Interactive remote identification: narrows database candidates with as few
// (and as easy) questions as possible.
#pragma once

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "core/index.hpp"

namespace bh {

struct Question {
    enum class Kind { None, PressButton, Category, Brand, TestSend, PickRemote };
    Kind kind = Kind::None;
    Btn btn = Btn::None;            // PressButton / TestSend
    uint32_t testRemote = 0;        // TestSend: send btn from this remote
    std::vector<std::string> options;
    std::vector<uint32_t> values;   // Category: Group ids; Brand: brand string ids; PickRemote: remote ids
};

// One distinct power signal to try in code search (no original remote).
struct PowerCode {
    Btn btn = Btn::Power;   // Power, PowerOff or PowerOn
    uint64_t key = 0;       // canonical key; 0 for a raw-only signal
    int raw = -1;           // index into Index::rawSigs when key == 0
    uint32_t remote = 0;    // a remote whose file holds this signal
    uint32_t weight = 0;    // candidate remotes using it (more = tried earlier)
};

class Matcher {
public:
    explicit Matcher(const Index& ix) : ix_(ix) {}

    enum class StartResult { Matched, NotDecoded, NoMatch };
    // Begin with a captured power-button signal.
    StartResult start(const std::vector<uint32_t>& timings);

    // Begin without the original remote: every remote of `brand` is a candidate.
    // In this mode next() never asks the user to press a button on their remote.
    void startBrand(uint32_t brand);
    bool noRemote() const { return noRemote_; }
    // Category question for the current candidates (Kind::None when they don't span 2+ categories).
    Question categoryQuestion() const;
    // Distinct power signals of the candidates, most common first. Toggle Power and Power Off
    // come first (the user is told to start with the device on), then a second pass of
    // Power On codes (btn == PowerOn, device off) for remotes that have nothing else.
    std::vector<PowerCode> powerCodes() const;
    // The device reacted to `c`: keep the remotes that use that signal for power.
    void answerPowerCode(const PowerCode& c);

    std::optional<IrCode> heard() const { return heard_; }
    bool fuzzy() const { return fuzzy_; }
    size_t remoteCount() const { return cand_.size(); }
    size_t codesetCount() const;
    int questionsAsked() const { return static_cast<int>(history_.size()); }

    // True when all remaining candidates behave the same on every known button.
    bool done() const;
    Question next() const;

    // Answers. Each returns false when the answer could not narrow anything
    // (candidates are then left unchanged and the question is not repeated).
    bool answerPress(Btn b, const std::vector<uint32_t>& timings, std::string& message);
    void answerNoSuchButton(Btn b);
    void answerCategory(Group g);
    void answerBrand(uint32_t brand);
    void answerBrandOther(const std::vector<uint32_t>& listed);
    void answerTest(uint32_t remote, Btn b, bool reacted);
    void answerPick(uint32_t remote);
    // "Not sure": never ask this question again, without narrowing.
    void skip(const Question& q);
    bool undo();

    // Best guess right now: richest remote of the most likely code set.
    uint32_t bestRemote() const;
    // Representative remotes, one per code set, most likely first.
    std::vector<uint32_t> rankedRemotes(size_t max) const;
    const std::vector<uint32_t>& candidates() const { return cand_; }

private:
    struct State {
        std::vector<uint32_t> cand;
        std::set<int> asked;
        std::set<uint64_t> tested;
    };
    void push();
    void reset();
    void setCandidates(std::vector<uint32_t> next);
    uint32_t richness(uint32_t remote) const;

    const Index& ix_;
    std::vector<uint32_t> cand_;
    std::set<int> asked_;       // encoded (kind<<8 | btn) of questions not to repeat
    std::set<uint64_t> tested_; // keys already test-sent
    std::vector<State> history_;
    std::optional<IrCode> heard_;
    bool fuzzy_ = false;
    bool noRemote_ = false;
};

}  // namespace bh
