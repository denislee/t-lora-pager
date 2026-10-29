/**
 * @file      text_shortcuts.h
 * @brief     iOS-style typing shortcuts for the notes editor, as pure logic.
 *
 * Pure C++ (no Arduino / LVGL) so the rules are exercised by host-side unit
 * tests — test/test_text_shortcuts drives them under pio test -e native_test.
 * The editor feeds the text left of the cursor and only decides what to do
 * with the answer (ui_text_editor.cpp).
 */
#pragma once

#include <cstddef>

namespace hal {

// `before` is the text left of the cursor (len bytes, UTF-8). Returns true
// when a typed space should turn the previous space into ". " (the iOS "."
// shortcut): len >= 2, before[len-1] == ' ', and
// c = (unsigned char)before[len-2] is an ASCII letter or digit, or c >= 0x80
// (the tail byte of a UTF-8 letter such as an accented char), or one of
// ')' '"' '\''. Otherwise false (null `before` -> false).
bool double_space_period(const char *before, size_t len);

} // namespace hal
