/**
 * @file      notes_path.cpp
 * @brief     Implementation of notes_path.h — pure logic, no Arduino deps.
 */
#include "notes_path.h"

#include <cstdio>
#include <cstring>

static const char *lstrip_slash(const char *p)
{
    if (p && *p == '/') return p + 1;
    return p ? p : "";
}

bool notes_crypto_path_is_protected(const char *path)
{
    if (!path) return false;
    const char *name = lstrip_slash(path);

    /* Journal index lives at the FFat root and stays there because it's
     * bookkeeping, not a user note. */
    if (strcmp(name, "journal_idx.bin") == 0) return true;

    /* User notes live one level deep under "notes/". Anything deeper is
     * outside this app's scope. */
    const size_t plen = strlen(hal::NOTES_PREFIX);
    if (strncmp(name, hal::NOTES_PREFIX, plen) == 0) {
        const char *leaf = name + plen;
        if (strchr(leaf, '/') != nullptr) return false;
        size_t n = strlen(leaf);
        return n > 4 && strcmp(leaf + n - 4, ".txt") == 0;
    }

    return false;
}

bool content_has_salted_magic(const char *buf, size_t len)
{
    return buf && len >= 8 && memcmp(buf, "Salted__", 8) == 0;
}

bool notes_path_is_tasks_file(const char *path)
{
    return path && strcmp(lstrip_slash(path), "tasks.txt") == 0;
}

void normalize_path(const char *in, char *out, size_t cap)
{
    if (cap == 0) return;
    if (!in || !in[0]) { out[0] = '\0'; return; }
    if (in[0] == '/') snprintf(out, cap, "%s", in);
    else snprintf(out, cap, "/%s", in);
}

bool sandbox_path(const char *in, char *out, size_t cap)
{
    if (cap == 0) return false;
    out[0] = '\0';
    if (!in) return false;

    size_t len = 0;          /* bytes written so far, excluding the NUL */
    const char *p = in;

    while (*p) {
        while (*p == '/') p++;
        if (!*p) break;

        const char *start = p;
        while (*p && *p != '/') {
            const unsigned char c = (unsigned char)*p;
            if (c < 0x20 || c == 0x7f || c == '\\') {
                out[0] = '\0';
                return false;
            }
            p++;
        }
        const size_t clen = (size_t)(p - start);

        if (clen == 1 && start[0] == '.') continue;                 /* "." */
        if (clen == 2 && start[0] == '.' && start[1] == '.') {      /* ".." */
            out[0] = '\0';
            return false;
        }
        if (clen > 64) {
            out[0] = '\0';
            return false;
        }

        /* Room for '/', the component, and the NUL. Never truncate. */
        if (len + 1 + clen + 1 > cap) {
            out[0] = '\0';
            return false;
        }
        out[len++] = '/';
        memcpy(out + len, start, clen);
        len += clen;
        out[len] = '\0';
    }

    if (len == 0) {
        if (cap < 2) return false;
        out[0] = '/';
        out[1] = '\0';
    }
    return true;
}
