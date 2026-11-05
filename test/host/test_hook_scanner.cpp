// Host-side test for HookBodyScanner. No ESP32 needed.
//   g++ -std=c++17 -Wall -Wextra -I include test/host/test_hook_scanner.cpp -o scanner_test && ./scanner_test
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include "hook_scanner.h"

static int failures = 0;

#define CHECK_STR(actual, expected)                                                    \
  do {                                                                                 \
    if (std::strcmp((actual), (expected)) != 0) {                                      \
      std::printf("  FAIL %s:%d  %s = \"%s\", expected \"%s\"\n", __FILE__, __LINE__, \
                  #actual, (actual), (expected));                                      \
      ++failures;                                                                      \
    }                                                                                  \
  } while (0)

// Feeds the body in chunks of the given size (mimics data arriving from the network).
static HookBodyScanner scan(const std::string& body, size_t chunk) {
  HookBodyScanner sc;
  sc.reset();
  for (size_t i = 0; i < body.size(); i += chunk) {
    const size_t n = std::min(chunk, body.size() - i);
    sc.feed(reinterpret_cast<const uint8_t*>(body.data() + i), n);
  }
  return sc;
}

static void run(const char* name, const std::string& body, const char* ev, const char* nt,
                const char* sid) {
  std::printf("%s\n", name);
  for (size_t chunk : {size_t(1), size_t(3), size_t(7), size_t(64), body.size() + 1}) {
    HookBodyScanner sc = scan(body, chunk);
    CHECK_STR(sc.hookEventName, ev);
    CHECK_STR(sc.notificationType, nt);
    CHECK_STR(sc.sessionId, sid);
  }
}

int main() {
  run("Stop (simple)",
      R"({"session_id":"abc-123","transcript_path":"C:\\Users\\x\\t.jsonl","cwd":"D:\\p",)"
      R"("permission_mode":"default","hook_event_name":"Stop","stop_hook_active":false,)"
      R"("last_assistant_message":"Done."})",
      "Stop", "", "abc-123");

  run("Notification permission_prompt (with whitespace)",
      "{\n  \"session_id\" : \"s1\",\n  \"hook_event_name\" :\t\"Notification\",\n"
      "  \"message\": \"Claude needs your permission\",\n"
      "  \"notification_type\": \"permission_prompt\"\n}",
      "Notification", "permission_prompt", "s1");

  // The top level must win even if tool_input / tool_response contain the same keys
  run("PostToolUse, nested decoy keys",
      R"({"session_id":"real","hook_event_name":"PostToolUse","tool_name":"Read",)"
      R"("tool_input":{"hook_event_name":"Stop","session_id":"fake","arr":[{"a":"}"}]},)"
      R"("tool_response":"{\"hook_event_name\":\"StopFailure\"} \\\" ] } {"})",
      "PostToolUse", "", "real");

  {
    std::printf("PreToolUse AskUserQuestion, tool_name is read\n");
    HookBodyScanner sc = scan(
        R"({"session_id":"q","hook_event_name":"PreToolUse","tool_name":"AskUserQuestion",)"
        R"("tool_input":{"questions":[{"question":"tool_name?","header":"x"}]}})",
        5);
    CHECK_STR(sc.hookEventName, "PreToolUse");
    CHECK_STR(sc.toolName, "AskUserQuestion");
  }

  run("Different key order, hook_event_name last",
      R"({"effort":{"level":"high"},"n":12.5,"b":true,"x":null,"error":"rate_limit",)"
      R"("hook_event_name":"StopFailure"})",
      "StopFailure", "", "");

  run("Very long key and value are cut off, no overflow",
      R"({"this_key_is_definitely_longer_than_the_buffer_hook_event_name":"X",)"
      R"("hook_event_name":"AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"})",
      "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", "", "");

  run("Empty body", "", "", "", "");
  run("No hook_event_name", R"({"foo":"bar"})", "", "", "");

  // Large body: ~600 KB tool_response (similar to a real PostToolUse)
  {
    std::string big = R"({"session_id":"big","hook_event_name":"PostToolUse","tool_response":")";
    for (int i = 0; i < 20000; ++i) big += R"(line \"quoted\" {not json} [x],\n)";
    big += R"("})";
    std::printf("Large body (%lu bytes)\n", (unsigned long)big.size());
    HookBodyScanner sc = scan(big, 1436);  // typical TCP segment
    CHECK_STR(sc.hookEventName, "PostToolUse");
    CHECK_STR(sc.sessionId, "big");
  }

  std::printf("\nsizeof(HookBodyScanner) = %lu bytes\n", (unsigned long)sizeof(HookBodyScanner));
  if (failures) std::printf("\n%d FAILED\n", failures);
  else std::printf("\nALL TESTS PASSED\n");
  return failures ? 1 : 0;
}
