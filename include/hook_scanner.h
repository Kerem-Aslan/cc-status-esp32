// Tiny streaming JSON scanner that pulls the needed fields out of a Claude Code hook body.
//
// Why not a full JSON library? A PostToolUse body contains the tool's entire output
// (e.g. the whole file that was read) and can be hundreds of KB. Buffering that on an
// ESP32 is risky. This scanner processes the body chunk by chunk, keeps only a few
// TOP-LEVEL string fields, and uses about 230 bytes.
//
// Nested objects and escaped quotes inside strings (\") are tracked correctly, so a
// "hook_event_name" that appears inside tool_input is never mistaken for the top-level field.
//
// No Arduino dependency, so it can also be compiled and tested on a PC (see test/host).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>

struct HookBodyScanner {
  static constexpr size_t KEY_MAX = 24;  // Longest key we look for: "notification_type" (17)
  static constexpr size_t VAL_MAX = 48;  // session_id is a UUID (36 characters)

  // --- Outputs (empty string if not found) ---
  char hookEventName[VAL_MAX];
  char notificationType[VAL_MAX];
  char toolName[VAL_MAX];   // Tool being run for PreToolUse/PostToolUse (long MCP names are cut)
  char sessionId[VAL_MAX];  // Not used yet; ready for multi-session support.

  // Clears everything. The struct is POD, so malloc + reset() is safe.
  void reset() { memset(this, 0, sizeof(*this)); }

  void feed(const uint8_t* data, size_t len) {
    for (size_t i = 0; i < len; ++i) feedByte(static_cast<char>(data[i]));
  }

 private:
  int16_t depth_;       // 1 = inside the top-level object
  bool inString_;
  bool escaped_;
  bool readingKey_;
  bool expectValue_;    // Saw ':' at the top level, waiting for a value
  bool keyTooLong_;
  uint8_t keyLen_;
  uint8_t targetLen_;
  char key_[KEY_MAX];
  char* target_;        // Output field currently being filled (nullptr if none)

  void feedByte(char c) {
    if (inString_) {
      if (escaped_) {             // \" \\ \n etc.: take the character as-is.
        escaped_ = false;         // The fields we want are plain ASCII, that's enough.
        store(c);
      } else if (c == '\\') {
        escaped_ = true;
      } else if (c == '"') {
        inString_ = false;
        endString();
      } else {
        store(c);
      }
      return;
    }

    switch (c) {
      case '"':
        inString_ = true;
        escaped_ = false;
        readingKey_ = false;
        target_ = nullptr;
        if (depth_ == 1) {
          if (!expectValue_) {    // A top-level key starts
            readingKey_ = true;
            keyLen_ = 0;
            keyTooLong_ = false;
          } else {                // A top-level string value starts
            target_ = pickTarget();
            targetLen_ = 0;
          }
        }
        break;
      case ':':
        if (depth_ == 1) expectValue_ = true;
        break;
      case ',':
        if (depth_ == 1) expectValue_ = false;
        break;
      case '{':
      case '[':
        ++depth_;
        break;
      case '}':
      case ']':
        --depth_;
        break;
      default:
        break;  // Numbers, true/false/null and whitespace don't matter to us
    }
  }

  void store(char c) {
    if (readingKey_) {
      if (keyLen_ < KEY_MAX - 1) key_[keyLen_++] = c;
      else keyTooLong_ = true;
    } else if (target_ != nullptr && targetLen_ < VAL_MAX - 1) {
      target_[targetLen_++] = c;
    }
  }

  void endString() {
    if (readingKey_) {
      key_[keyLen_] = '\0';
      readingKey_ = false;
    } else if (target_ != nullptr) {
      target_[targetLen_] = '\0';
      target_ = nullptr;
    }
  }

  char* pickTarget() {
    if (keyTooLong_) return nullptr;
    if (strcmp(key_, "hook_event_name") == 0) return hookEventName;
    if (strcmp(key_, "notification_type") == 0) return notificationType;
    if (strcmp(key_, "tool_name") == 0) return toolName;
    if (strcmp(key_, "session_id") == 0) return sessionId;
    return nullptr;
  }
};
