/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Basically360 bridge: the console's own screens (message boxes, the        *
 * achievements list) shown by the Basically360 dashboard when it's running. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/b360_bridge.h"

#include "xenia/base/logging.h"
#include "xenia/base/platform.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#endif

namespace xe {
namespace kernel {
namespace xam {
namespace b360 {

#if XE_PLATFORM_WIN32

static const wchar_t kPipeName[] = L"\\\\.\\pipe\\Basically360-guide";

bool Available() {
  // TRUE: an instance is free now. FALSE with ERROR_SEM_TIMEOUT: it's there
  // but busy (another screen is up) - still the dashboard's to answer.
  if (WaitNamedPipeW(kPipeName, 1)) {
    return true;
  }
  return GetLastError() == ERROR_SEM_TIMEOUT;
}

static std::string Clean(const std::string& s) {
  std::string out = s;
  for (auto& c : out) {
    if (c == '\t' || c == '\r') {
      c = ' ';
    } else if (c == '\n') {
      c = '\x1f';  // a line break the dashboard puts back
    }
  }
  return out;
}

bool Ask(const std::vector<std::string>& fields, std::string* reply) {
  std::string request;
  for (size_t i = 0; i < fields.size(); ++i) {
    if (i) {
      request += '\t';
    }
    request += Clean(fields[i]);
  }
  HANDLE pipe = INVALID_HANDLE_VALUE;
  for (int tries = 0; tries < 50; ++tries) {
    pipe = CreateFileW(kPipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                       OPEN_EXISTING, 0, nullptr);
    if (pipe != INVALID_HANDLE_VALUE || GetLastError() != ERROR_PIPE_BUSY) {
      break;
    }
    WaitNamedPipeW(kPipeName, 200);
  }
  if (pipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD mode = PIPE_READMODE_MESSAGE;
  SetNamedPipeHandleState(pipe, &mode, nullptr, nullptr);
  DWORD written = 0;
  bool ok = WriteFile(pipe, request.data(), DWORD(request.size()), &written,
                      nullptr) &&
            written == request.size();
  std::string answer;
  if (ok) {
    char buffer[4096];
    DWORD read = 0;
    for (;;) {
      if (ReadFile(pipe, buffer, sizeof(buffer), &read, nullptr)) {
        answer.append(buffer, read);
        break;
      }
      if (GetLastError() != ERROR_MORE_DATA) {
        ok = false;
        break;
      }
      answer.append(buffer, read);
    }
  }
  CloseHandle(pipe);
  if (!ok) {
    XELOGW("Basically360: no answer to {}",
           fields.empty() ? std::string() : fields[0]);
    return false;
  }
  if (reply) {
    *reply = answer;
  }
  return true;
}

#else

bool Available() { return false; }

bool Ask(const std::vector<std::string>& fields, std::string* reply) {
  (void)fields;
  (void)reply;
  return false;
}

#endif  // XE_PLATFORM_WIN32

}  // namespace b360
}  // namespace xam
}  // namespace kernel
}  // namespace xe
