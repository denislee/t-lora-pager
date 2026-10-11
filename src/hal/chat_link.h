/**
 * @file      chat_link.h
 * @brief     Chat turn over the PagerLink companion (voice memo + chat.ask).
 *
 * Runs one chat turn on the phone, which holds the Groq/Whisper keys and the
 * conversation history (HUB_REMOVAL_PLAN.md H2.4): optionally upload a voice
 * memo as a bulk DATA transfer, then ask the phone to transcribe and answer.
 * This is the only header the chat UI includes for that feature.
 *
 * Blocking: chat_link_ask() drives hw_link_call()/hw_link_send_bytes(), which
 * wait on the phone's answer — see the threading note in link.h.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

#include "result.h"

// Voice memo cap: 15 s of the 16 kHz/16-bit/mono WAV hw_rec_start() writes
// (32000 B/s) + 44-byte header.
constexpr uint32_t CHAT_AUDIO_MAX_MS = 15000;
constexpr size_t CHAT_AUDIO_MAX_BYTES = 44 + (CHAT_AUDIO_MAX_MS / 1000) * 32000;

// chat.ask waits for STT + LLM on the phone; named like fs.write.commit's 60 s.
constexpr uint32_t CHAT_ASK_TIMEOUT_MS = 60000;

// Result of one turn: the phone's transcription of the audio (empty when the
// turn was text-only) and its reply.
struct ChatReply {
    std::string transcript;
    std::string reply;
};

// Blocking: run on a worker task/thread, never the LVGL thread. On failure
// returns the HalError and fills err with a UI-ready message.
HalResult<ChatReply> chat_link_ask(const std::string &text, const std::string &audio_path,
                                   std::string &err);
