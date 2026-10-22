/**
 * @file      notes_migrate.cpp
 * @brief     See notes_migrate.h.
 */
#include "notes_migrate.h"

#include <cstring>
#include <unordered_map>

namespace nc_migrate {

std::vector<NameCopies> group_by_name(const std::vector<std::string> &ffat,
                                      const std::vector<std::string> &sd)
{
    std::vector<NameCopies> out;
    std::unordered_map<std::string, size_t> idx;
    out.reserve(ffat.size() + sd.size());
    for (const auto &p : ffat) {
        auto it = idx.find(p);
        if (it != idx.end()) continue;
        idx.emplace(p, out.size());
        NameCopies n;
        n.path = p;
        n.ffat = true;
        out.push_back(std::move(n));
    }
    for (const auto &p : sd) {
        auto it = idx.find(p);
        if (it != idx.end()) {
            out[it->second].sd = true;
            continue;
        }
        idx.emplace(p, out.size());
        NameCopies n;
        n.path = p;
        n.sd = true;
        out.push_back(std::move(n));
    }
    return out;
}

CopyPlan plan_copies(bool ffat_ok, bool sd_ok, bool same)
{
    if (ffat_ok && sd_ok) return same ? CopyPlan::Shared : CopyPlan::Separate;
    if (ffat_ok) return CopyPlan::FfatOnly;
    if (sd_ok) return CopyPlan::SdOnly;
    return CopyPlan::None;
}

Recovery pending_recovery(bool pending_present, bool pending_verifies)
{
    if (!pending_present) return Recovery::None;
    return pending_verifies ? Recovery::RollForward : Recovery::RollBack;
}

std::string staged_path(const std::string &path)
{
    return path + ".ncnew";
}

bool bytes_equal(const uint8_t *a, size_t an, const uint8_t *b, size_t bn)
{
    if (an != bn) return false;
    if (an == 0) return true;
    if (!a || !b) return false;
    return memcmp(a, b, an) == 0;
}

} // namespace nc_migrate
