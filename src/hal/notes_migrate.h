/**
 * @file      notes_migrate.h
 * @brief     Pure planning helpers for the bulk passphrase migrations in
 *            notes_crypto.cpp (P7.7). No filesystem, NVS or crypto here, so
 *            the native Unity tests can cover the decisions directly.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace nc_migrate {

/* One protected name and the volumes it exists on. hw_save_preferred_*
 * dual-writes the same bytes (same salt) to FFat and SD, so most names have
 * two copies. */
struct NameCopies {
    std::string path;
    bool ffat = false;
    bool sd = false;
};

/* Merge the FFat and SD lists of protected paths by exact name. Order: the
 * FFat list order, then SD-only names in SD list order. Duplicates within a
 * list collapse into one entry. */
std::vector<NameCopies> group_by_name(const std::vector<std::string> &ffat,
                                      const std::vector<std::string> &sd);

enum class CopyPlan {
    None,       /* no copy to process */
    FfatOnly,   /* only the FFat copy */
    SdOnly,     /* only the SD copy */
    Shared,     /* both copies, byte-identical: transform once, write to both */
    Separate,   /* both copies, different bytes: transform each */
};

/* `ffat_ok` / `sd_ok`: that copy is selected and was read; `same`: the two
 * reads are byte-identical (ignored unless both are ok). */
CopyPlan plan_copies(bool ffat_ok, bool sd_ok, bool same);

/* What an unlock must do with an interrupted change-passphrase (see
 * notes_crypto.cpp, "Change-passphrase ordering"). `pending_present`: the
 * pending verifier exists in NVS; `pending_verifies`: it decrypts under the
 * passphrase the canary just accepted. */
enum class Recovery {
    None,         /* nothing pending */
    RollForward,  /* the canary was rotated (commit happened): install the staged copies */
    RollBack,     /* the canary is still the old one: discard the staged copies */
};
Recovery pending_recovery(bool pending_present, bool pending_verifies);

/* Name of the staged copy of `path` written before the commit. */
std::string staged_path(const std::string &path);

/* Size and contents equal. */
bool bytes_equal(const uint8_t *a, size_t an, const uint8_t *b, size_t bn);

} // namespace nc_migrate
