// Index builder: turns a Flipper-IRDB checkout into a Beamhound Index.
#pragma once

#include <functional>
#include <map>
#include <string>

#include "core/index.hpp"

namespace bh {

// Revision of the builder's output. Bump it whenever a change to the builder alters the index
// (brands, categories, signals): installed apps then rebuild their index from the database
// already on the device. 1 = original, 2 = brand aliases folded ("TV SAMSUNG" -> Samsung),
// 3 = placeholder brands ("Unknown Brand", "BrandUnknown", "No Brand", "(Generic)") -> Unknown.
constexpr uint32_t kBuilderRev = 3;

struct BuildReport {
    size_t files = 0;
    long powerDecoded = 0, powerRaw = 0;
    long skippedAc = 0, skippedUniversal = 0, skippedNoPower = 0, skippedPath = 0;
    int propagated = 0;
    int brandsMerged = 0;  // brand spellings folded into another brand ("TV SAMSUNG" -> Samsung)
    std::map<Group, int> groups;
    std::string error;
};

// Scans `rootDir` for .ir files. `progress(done, total)` is called periodically.
// When `copyTo` is set, the used .ir files are copied there (keeping relative paths).
bool buildIndex(const std::string& rootDir, Index& ix, BuildReport& rep,
                const std::function<void(size_t, size_t)>& progress = {}, const std::string& copyTo = "");

}  // namespace bh
