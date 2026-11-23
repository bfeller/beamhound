#include "app/setup.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>

#include <chrono>
#include <filesystem>
#include <functional>
#include <vector>

#include "core/builder.hpp"

extern char** environ;

namespace bh {
namespace fs = std::filesystem;

namespace {

// Run a program (no shell) with output discarded; call `tick` every 200 ms while it runs.
int runProcess(const std::vector<std::string>& args, const std::function<void()>& tick)
{
    std::vector<char*> argv;
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    const int rc = posix_spawnp(&pid, argv[0], &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    if (rc != 0) return -1;
    int status = 0;
    for (;;) {
        const pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0) return -1;
        if (tick) tick();
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

}  // namespace

DbSetup::~DbSetup()
{
    if (worker_.joinable()) worker_.join();
}

std::string DbSetup::error() const
{
    std::lock_guard<std::mutex> lock(mu_);
    return error_;
}

void DbSetup::fail(const std::string& msg)
{
    {
        std::lock_guard<std::mutex> lock(mu_);
        error_ = msg;
    }
    phase_ = Phase::Failed;
}

void DbSetup::start(const std::string& dataDir)
{
    if (phase_ == Phase::Downloading || phase_ == Phase::Extracting || phase_ == Phase::Indexing) return;
    if (worker_.joinable()) worker_.join();
    {
        std::lock_guard<std::mutex> lock(mu_);
        error_.clear();
    }
    done_ = 0;
    total_ = 0;
    phase_ = Phase::Downloading;
    worker_ = std::thread(&DbSetup::run, this, dataDir);
}

void DbSetup::run(std::string dataDir)
{
    std::error_code ec;
    const fs::path base = dataDir, work = base / ".setup", tarball = work / "irdb.tar.gz",
                   staging = work / "irdb";
    fs::remove_all(work, ec);
    fs::create_directories(staging, ec);
    if (ec) return fail("Can't create " + work.string() + ": " + ec.message());

    // 1. Download the pinned snapshot.
    const std::string url = std::string("https://codeload.github.com/Lucaslhm/Flipper-IRDB/tar.gz/") + kIrdbCommit;
    int rc = runProcess({"curl", "-fsSL", "--retry", "2", "--connect-timeout", "20", "-o", tarball.string(), url},
                        [&] {
                            std::error_code e;
                            const auto sz = fs::file_size(tarball, e);
                            if (!e) done_ = static_cast<size_t>(sz);
                        });
    if (rc != 0)
        return fail(rc < 0 ? "curl is not installed." : "Download failed (curl error " + std::to_string(rc) +
                                                             "). Check the Wi-Fi connection and try again.");

    // 2. Extract only what the app uses: the .ir files plus the licence and readme.
    phase_ = Phase::Extracting;
    rc = runProcess({"tar", "-xzf", tarball.string(), "-C", staging.string(), "--strip-components=1", "--wildcards",
                     "*.ir", "*/LICENSE", "*/README.md"},
                    {});
    fs::remove(tarball, ec);
    if (rc != 0) return fail("Couldn't unpack the database (tar error " + std::to_string(rc) + ").");

    // 3. Build the index.
    phase_ = Phase::Indexing;
    Index ix;
    BuildReport rep;
    const bool ok = buildIndex(staging.string(), ix, rep, [&](size_t d, size_t t) {
        done_ = d;
        total_ = t;
    });
    if (!ok) return fail("Indexing failed: " + rep.error);
    if (!ix.save((work / "index.bin").string())) return fail("Couldn't write the index.");

    // 4. Swap into place.
    fs::remove_all(base / "irdb", ec);
    fs::rename(staging, base / "irdb", ec);
    if (ec) return fail("Couldn't install the database: " + ec.message());
    fs::rename(work / "index.bin", base / "index.bin", ec);
    if (ec) return fail("Couldn't install the index: " + ec.message());
    fs::remove_all(work, ec);
    phase_ = Phase::Done;
}

}  // namespace bh
