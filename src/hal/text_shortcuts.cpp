/**
 * @file      text_shortcuts.cpp
 * @brief     iOS-style typing shortcuts for the notes editor (pure logic).
 */
#include "text_shortcuts.h"

namespace hal {

bool double_space_period(const char *before, size_t len)
{
    if (!before || len < 2) return false;
    if (before[len - 1] != ' ') return false;

    const unsigned char c = (unsigned char)before[len - 2];
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')) {
        return true;
    }
    // Any byte >= 0x80 ends a word here: the cursor sits on a character
    // boundary, so it is the tail byte of a multi-byte UTF-8 letter.
    if (c >= 0x80) return true;
    return c == ')' || c == '"' || c == '\'';
}

} // namespace hal
