// Build a Beamhound index on the PC (for development and the desktop preview).
// Usage: build_index <irdb-root> <out-dir>
// Writes <out-dir>/index.bin and copies the used .ir files to <out-dir>/irdb/.
#include <cstdio>
#include <filesystem>

#include "core/builder.hpp"

int main(int argc, char** argv)
{
    if (argc != 3) {
        std::fprintf(stderr, "usage: %s <irdb-root> <out-dir>\n", argv[0]);
        return 2;
    }
    const std::string out = argv[2];
    std::filesystem::create_directories(out + "/irdb");
    bh::Index ix;
    bh::BuildReport rep;
    if (!bh::buildIndex(argv[1], ix, rep, {}, out + "/irdb") || !ix.save(out + "/index.bin")) {
        std::fprintf(stderr, "build failed: %s\n", rep.error.c_str());
        return 1;
    }
    std::printf("categories inferred from shared codes: %d\n", rep.propagated);
    std::printf("brand spellings merged into another brand: %d\n", rep.brandsMerged);
    std::printf("remotes: %zu  codesets: %u  signals: %zu  raw power sigs: %zu  strings: %zu\n", ix.remotes.size(),
                ix.codesetCount, ix.sigs.size(), ix.rawSigs.size(), ix.strings.size());
    std::printf("power decodable: %ld  power raw-only: %ld\n", rep.powerDecoded, rep.powerRaw);
    std::printf("skipped: ACs %ld, universal lists %ld, no power button %ld, bad path %ld\n", rep.skippedAc,
                rep.skippedUniversal, rep.skippedNoPower, rep.skippedPath);
    for (auto& [g, n] : rep.groups) std::printf("  %-26s %d\n", bh::groupLabel(g), n);
    return 0;
}
