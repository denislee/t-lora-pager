/**
 * @file      tg_favs.h
 * @brief     Telegram favourites: the one codec of their NVS format.
 *
 * Ids in `favs` (comma-separated), titles in `fav_titles` as
 * `<id>\x1F<title>\x1E` records, separators a chat title won't contain (the
 * registry refuses control characters in one). The list keeps the order it was
 * stored in. The title pref is best-effort: a missing record leaves the title
 * empty. An id of 0 or an unparsable one is not a chat, and is dropped.
 *
 * The encode/decode pair is pure, so test/test_tg_favs links it without NVS.
 */
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace apps {

using TgFavs = std::vector<std::pair<long long, std::string>>;

inline TgFavs tg_favs_decode(const std::string &ids, const std::string &titles)
{
    TgFavs out;
    size_t i = 0;
    while (i < ids.size()) {
        size_t j = ids.find(',', i);
        if (j == std::string::npos) j = ids.size();
        const long long id = std::strtoll(ids.substr(i, j - i).c_str(), nullptr, 10);
        if (id != 0) out.emplace_back(id, std::string());
        i = j + 1;
    }
    i = 0;
    while (i < titles.size()) {
        size_t end = titles.find('\x1E', i);
        if (end == std::string::npos) end = titles.size();
        const std::string rec = titles.substr(i, end - i);
        const size_t sep = rec.find('\x1F');
        if (sep != std::string::npos) {
            const long long id = std::strtoll(rec.substr(0, sep).c_str(), nullptr, 10);
            for (auto &f : out) {
                if (f.first == id) f.second = rec.substr(sep + 1);
            }
        }
        i = end + 1;
    }
    return out;
}

inline void tg_favs_encode(const TgFavs &favs, std::string &ids, std::string &titles)
{
    ids.clear();
    titles.clear();
    for (const auto &f : favs) {
        char b[24];
        std::snprintf(b, sizeof(b), "%lld", f.first);
        if (!ids.empty()) ids.push_back(',');
        ids += b;
        if (f.second.empty()) continue;
        titles += b;
        titles.push_back('\x1F');
        titles += f.second;
        titles.push_back('\x1E');
    }
}

// The stored list, in NVS namespace `tgbridge`. Any thread (ui_telegram.cpp).
TgFavs tg_favs_load();
void tg_favs_save(const TgFavs &favs);

}  // namespace apps
