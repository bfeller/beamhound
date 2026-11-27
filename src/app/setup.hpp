// First-run database setup: download Flipper-IRDB, extract it and build the index,
// all on a background thread so the UI stays responsive.
#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace bh {

// Pinned snapshot of https://github.com/Lucaslhm/Flipper-IRDB (the matching data the app was tested with).
constexpr const char* kIrdbCommit = "d126fb1b6f1e114c52b4a8c19839ea65e3a9c24d";

class DbSetup {
public:
    enum class Phase { Idle, Downloading, Extracting, Indexing, Done, Failed };

    ~DbSetup();
    void start(const std::string& dataDir);
    // Rebuild index.bin from the database already in <dataDir>/irdb (after an app update whose
    // builder makes a different index). Phase goes straight to Indexing; the old index stays in
    // place until the new one is written.
    void reindex(const std::string& dataDir);
    bool reindexing() const { return reindexing_; }
    Phase phase() const { return phase_.load(); }
    // Downloaded bytes while Downloading; files indexed / total while Indexing.
    size_t done() const { return done_.load(); }
    size_t total() const { return total_.load(); }
    std::string error() const;

private:
    void run(std::string dataDir);
    void runReindex(std::string dataDir);
    void fail(const std::string& msg);

    std::thread worker_;
    std::atomic<Phase> phase_{Phase::Idle};
    std::atomic<size_t> done_{0}, total_{0};
    bool reindexing_ = false;
    mutable std::mutex mu_;
    std::string error_;
};

}  // namespace bh
