/**
 * @file      fs_util.h
 * @brief     Whole-file read/write helpers shared by hal/storage.cpp,
 *            hal/storage_bulk.cpp and hal/notes_crypto.cpp.
 *
 * HAL internal — not a public API. Hardware only (Arduino `fs::FS`). None of
 * these take a lock: SD callers hold `core::ScopedSpiLock`/`MaybeSpiLock`
 * around the call exactly as they did around the open/read/write they replace.
 * No decryption or encryption happens here — the bytes are the on-disk bytes.
 */
#pragma once

#ifdef ARDUINO
#include <FS.h>
#include <cstddef>
#include <cstdint>

namespace hal {

/* Read the whole of an already-open file into `out` (std::string or
 * std::vector<uint8_t>; resized to the file size) and close it. */
template <typename Buf>
inline void fs_read_all(File &f, Buf &out)
{
    size_t n = f.size();
    out.resize(n);
    if (n) f.read(reinterpret_cast<uint8_t *>(&out[0]), n);
    f.close();
}

/* Open `path` on `fs` and read it whole into `out`. False when the file can't
 * be opened (`out` is left untouched). */
template <typename Buf>
inline bool fs_read_all(fs::FS &fs, const char *path, Buf &out)
{
    File f = fs.open(path, FILE_READ);
    if (!f) return false;
    fs_read_all(f, out);
    return true;
}

/* Create/truncate `path` on `fs` and write `len` bytes. True only when the
 * file opened and every byte landed (len == 0 → an empty file, true). */
inline bool fs_write_all(fs::FS &fs, const char *path, const uint8_t *buf, size_t len)
{
    File f = fs.open(path, FILE_WRITE);
    if (!f) return false;
    size_t w = len ? f.write(buf, len) : 0;
    f.close();
    return w == len;
}

} // namespace hal
#endif // ARDUINO
