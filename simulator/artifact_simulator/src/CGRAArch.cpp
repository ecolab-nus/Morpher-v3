#include "ArtifactSim/CGRAArch.h"

#include <fstream>
#include <stdexcept>
#include <string>

#include "morpher/arch/json.hpp"

using json = nlohmann::json;

namespace ArtifactSim {

static std::set<std::string> opsFromJson(const json &opsObj) {
    std::set<std::string> result;
    for (auto it = opsObj.begin(); it != opsObj.end(); ++it) {
        result.insert(it.key());
    }
    return result;
}

CGRAArch CGRAArch::parse(const std::string &jsonPath) {
    std::ifstream f(jsonPath);
    if (!f.is_open()) {
        throw std::runtime_error("Cannot open arch JSON: " + jsonPath);
    }
    json root;
    f >> root;

    const json &arch = root.at("ARCH");

    // Collect ops and latency per FU type.
    // In the JSON, OPS values are the latency (1 for FU, 2 for FU_MEM).
    std::set<std::string> fuOps, fuMemOps;
    int fuMemLatency = 2;
    if (arch.contains("FU") && arch["FU"].contains("OPS")) {
        fuOps = opsFromJson(arch["FU"]["OPS"]);
    }
    if (arch.contains("FU_MEM") && arch["FU_MEM"].contains("OPS")) {
        fuMemOps = opsFromJson(arch["FU_MEM"]["OPS"]);
        // Use the value of any op as the latency (they are all the same within one FU type).
        const json &opsObj = arch["FU_MEM"]["OPS"];
        if (!opsObj.empty()) {
            fuMemLatency = opsObj.begin().value().get<int>();
        }
    }

    // Find the GRID pattern inside CGRA.SUBMODS.
    const json &cgraSubmods = arch.at("CGRA").at("SUBMODS");
    const json *gridEntry = nullptr;
    for (const auto &entry : cgraSubmods) {
        if (entry.value("PATTERN", std::string{}) == "GRID") {
            gridEntry = &entry;
            break;
        }
    }
    if (!gridEntry) {
        throw std::runtime_error("No GRID pattern found in CGRA.SUBMODS in " + jsonPath);
    }

    CGRAArch result;
    result.xdim = (*gridEntry)["DIMS"]["X"].get<int>();
    result.ydim = (*gridEntry)["DIMS"]["Y"].get<int>();

    // Default all PEs to non-mem.
    result.pe.assign(result.ydim, std::vector<PEConfig>(result.xdim));
    for (int y = 0; y < result.ydim; ++y) {
        for (int x = 0; x < result.xdim; ++x) {
            result.pe[y][x].hasMem = false;
            result.pe[y][x].memLatency = 1;
            result.pe[y][x].supportedOps = fuOps;
        }
    }

    // Override per-tile config from the MODS list.
    if (gridEntry->contains("MODS")) {
        for (const auto &mod : (*gridEntry)["MODS"]) {
            int x = mod["X"].get<int>();
            int y = mod["Y"].get<int>();
            std::string modType = mod.value("MOD", std::string("PE"));
            if (x < 0 || x >= result.xdim || y < 0 || y >= result.ydim) {
                continue;
            }
            bool isMem = (modType == "PE_MEM");
            result.pe[y][x].hasMem = isMem;
            result.pe[y][x].memLatency = isMem ? fuMemLatency : 1;
            result.pe[y][x].supportedOps = isMem ? fuMemOps : fuOps;
        }
    }

    // SPM: count SPM_B*_WRAPPER entries; read SIZE from SPM submodule if present.
    int numBanks = 0;
    int bankSize = 0;
    for (auto it = arch.begin(); it != arch.end(); ++it) {
        const std::string &key = it.key();
        if (key.rfind("SPM_B", 0) == 0 && key.find("_WRAPPER") != std::string::npos) {
            ++numBanks;
        }
    }
    if (arch.contains("SPM")) {
        const json &spm = arch["SPM"];
        if (spm.contains("MEM") && spm["MEM"].contains("SIZE")) {
            bankSize = spm["MEM"]["SIZE"].get<int>();
        }
    }
    result.spm.numBanks = numBanks;
    result.spm.bankSize = bankSize;

    return result;
}

const PEConfig &CGRAArch::peConfig(int x, int y) const {
    return pe.at(y).at(x);
}

} // namespace ArtifactSim
