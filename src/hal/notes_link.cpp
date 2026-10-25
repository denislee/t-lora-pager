/**
 * @file      notes_link.cpp
 * @brief     notes_link_manifest() + notes_link_sync() — see notes_link.h.
 *
 * notes_link_manifest() stats and CRC-32s every *.txt under /notes on the
 * file-service volumes. notes_link_sync() runs one notes.sync job per manifest
 * batch, subscribing to evt.notes.sync for progress and waiting for the job's
 * done event. Both are blocking; the caller runs them on a worker task/thread.
 */
#include "notes_link.h"

#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <set>

#include "link.h"
#include "link/fs_volume.h"
#include "link/link_json.h"
#include "notes_path.h"

// Map a failed hw_link_call() to a HalError plus a UI-ready message.
static HalError link_fail(const HwLinkResult &r, std::string &err)
{
    if (r.code == "disconnected") {
        err = hal_error_string(HalError::LinkOffline);
        return HalError::LinkOffline;
    }
    err = "phone: " + r.code + ": " + r.msg;
    return HalError::InternalError;
}

// Collect the errors reported by one event: a JSON array of strings held in
// `raw`. Decodes the common escapes; other escapes are kept verbatim (slash
// included). Malformed input simply yields what it can.
static bool parse_string_array(const std::string &raw, std::vector<std::string> &out)
{
    size_t i = 0;
    const size_t n = raw.size();
    while (i < n) {
        while (i < n && raw[i] != '"') ++i;  // find the next opening quote
        if (i >= n) break;
        ++i;

        std::string cur;
        while (i < n && raw[i] != '"') {
            const char c = raw[i];
            if (c == '\\' && i + 1 < n) {
                const char e = raw[i + 1];
                i += 2;
                switch (e) {
                    case '"': cur += '"'; break;
                    case '\\': cur += '\\'; break;
                    case '/': cur += '/'; break;
                    case 'n': cur += '\n'; break;
                    case 't': cur += '\t'; break;
                    case 'r': cur += '\r'; break;
                    case 'b': cur += '\b'; break;
                    case 'f': cur += '\f'; break;
                    default:  // keep an unsupported escape literal, backslash and all
                        cur += '\\';
                        cur += e;
                        break;
                }
            } else {
                cur += c;
                ++i;
            }
        }
        if (i < n && raw[i] == '"') ++i;  // skip the closing quote
        out.push_back(std::move(cur));
    }
    return true;
}

// Append every *.txt entry (not a directory, name ending in ".txt", and not in
// `skip`) under /notes on volume `v` to `out`, CRC-32 over its raw bytes (from
// the vol_file_crc() cache when the note is unchanged). When
// the listing itself fails: `required` turns that into a hard error, otherwise
// an SD without a /notes folder is simply empty.
static bool scan(pagerlink::Vol v, const char *volname, const std::set<std::string> *skip,
                 bool required, std::vector<NoteEntry> &out, std::string &err)
{
    std::vector<pagerlink::VolEntry> entries;
    if (!pagerlink::vol_list(v, hal::NOTES_DIR, entries)) {
        // No /notes folder at all is an empty volume, not an error.
        pagerlink::VolEntry dir;
        if (required && pagerlink::vol_stat(v, hal::NOTES_DIR, dir)) {
            err = "cannot list notes";
            return false;
        }
        return true;
    }

    for (const pagerlink::VolEntry &e : entries) {
        if (e.dir) continue;
        const std::string &name = e.name;
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".txt") != 0) continue;
        if (skip != nullptr && skip->count(name) != 0) continue;

        const std::string path = std::string(hal::NOTES_DIR) + "/" + name;

        pagerlink::VolEntry st;
        if (!pagerlink::vol_stat(v, path, st)) {
            err = "cannot stat " + name;
            return false;
        }

        // Shared with fs.read: an unchanged note is not re-read on every sync,
        // and the phone's fs.read of it then answers from the same cache.
        uint32_t crc = 0;
        if (!pagerlink::vol_file_crc(v, path, st.size, st.mtime, crc)) {
            err = "cannot read " + name;
            return false;
        }

        NoteEntry ne;
        ne.name = name;
        ne.size = st.size;
        ne.mtime = st.mtime;
        ne.crc32 = crc;
        ne.vol = volname;
        out.push_back(std::move(ne));
    }
    return true;
}

bool notes_link_manifest(bool include_sd, std::vector<NoteEntry> &out, std::string &err)
{
    out.clear();
    err.clear();

    if (!scan(pagerlink::Vol::Int, "int", nullptr, true, out, err)) return false;

    if (include_sd && pagerlink::vol_available(pagerlink::Vol::Sd)) {
        std::set<std::string> internal;
        for (const NoteEntry &e : out) internal.insert(e.name);
        if (!scan(pagerlink::Vol::Sd, "sd", &internal, false, out, err)) return false;
    }
    return true;
}

// Shared with the event callback (which outlives any single wait): a
// shared_ptr so a late callback after unsubscribe is still harmless.
struct NotesSyncState {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> q;
};

// Unsubscribe on every exit path.
struct LinkSub {
    int tok;
    ~LinkSub() { hw_link_unsubscribe(tok); }
};

HalResult<NotesSyncSummary> notes_link_sync(const std::vector<NoteEntry> &files,
                                            const std::vector<std::string> &targets,
                                            const NotesLogFn &log, std::string &err)
{
    err.clear();

    if (!hw_link_connected()) {
        err = hal_error_string(HalError::LinkOffline);
        return HalError::LinkOffline;
    }

    if (files.empty()) {
        if (log) log("no notes to sync");
        return NotesSyncSummary{};
    }

    auto st = std::make_shared<NotesSyncState>();

    /* Subscribe before the first call so no progress event can slip past. */
    const int tok = hw_link_subscribe("evt.notes.sync", [st](const std::string &p) {
        {
            std::lock_guard<std::mutex> lk(st->m);
            st->q.push_back(p);
        }
        st->cv.notify_one();
    });
    LinkSub guard{tok};

    const std::vector<std::pair<size_t, size_t>> batches =
        notes_manifest_batches(targets, files);

    NotesSyncSummary summary;
    for (const std::pair<size_t, size_t> &b : batches) {
        const HwLinkResult r =
            hw_link_call("notes.sync", notes_sync_params(targets, files, b.first, b.second));
        if (!r.ok) return link_fail(r, err);

        pagerlink::JsonObject reply;
        int64_t job = 0;
        if (!reply.parse(r.json) || !reply.get_int("job", job)) {
            err = "phone: bad notes.sync reply";
            return HalError::InternalError;
        }

        for (;;) {
            std::string ev;
            bool got = false;
            {
                std::unique_lock<std::mutex> lk(st->m);
                // Wake every second so a dropped link fails fast; give up
                // after NOTES_SYNC_STALL_MS without an event.
                for (uint32_t waited = 0; waited < NOTES_SYNC_STALL_MS && !got; waited += 1000) {
                    got = st->cv.wait_for(lk, std::chrono::milliseconds(1000),
                                          [&st] { return !st->q.empty(); });
                    if (!got && !hw_link_connected()) break;
                }
                if (got) {
                    ev = std::move(st->q.front());
                    st->q.pop_front();
                }
            }
            if (!got) {
                if (!hw_link_connected()) {
                    err = hal_error_string(HalError::LinkOffline);
                    return HalError::LinkOffline;
                }
                err = "phone: notes sync stalled (no progress for 120 s)";
                return HalError::InternalError;
            }

            /* Parse and log without the lock held. */
            pagerlink::JsonObject eo;
            if (!eo.parse(ev)) continue;

            int64_t ejob = 0;
            if (!eo.get_int("job", ejob) || ejob != job) continue;

            int64_t uploaded = 0;
            int64_t already = 0;
            bool done = false;
            eo.get_int("uploaded", uploaded);
            eo.get_int("already", already);
            eo.get_bool("done", done);

            std::vector<std::string> names;
            std::string raw;
            if (eo.get_raw("errors", raw)) parse_string_array(raw, names);

            std::string line = "uploaded=" + std::to_string(uploaded) +
                               " already=" + std::to_string(already) +
                               " errors=" + std::to_string(names.size());
            if (!names.empty()) {
                line += " (";
                for (size_t k = 0; k < names.size(); ++k) {
                    if (k) line += ", ";
                    line += names[k];
                }
                line += ")";
            }
            if (log) log(line);

            if (done) {
                summary.uploaded += static_cast<int>(uploaded);
                summary.already += static_cast<int>(already);
                for (std::string &nm : names) summary.errors.push_back(std::move(nm));
                break;  // this job is complete, move to the next batch
            }
        }
    }

    return summary;
}

HalResult<NotesSyncSummary> notes_link_copy_to_phone(std::string &err)
{
    std::vector<NoteEntry> files;
    if (!notes_link_manifest(true, files, err)) return HalError::InternalError;
    return notes_link_sync(files, {"phone"}, nullptr, err);
}
