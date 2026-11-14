/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/app/split_screen.h"

#include <algorithm>
#include <bitset>
#include <cctype>
#include <cstdlib>
#include <cstring>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/memory.h"
#include "xenia/base/platform.h"
#include "xenia/emulator.h"
#include "xenia/gpu/graphics_system.h"
#include "xenia/hid/controller_instances.h"
#include "xenia/hid/input_system.h"
#include "xenia/memory.h"
#include "xenia/ui/presenter.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

#if XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
#include "xenia/ui/window_win.h"

#include <intrin.h>
#include <tlhelp32.h>
#include <map>
#include <vector>
#endif

DECLARE_bool(split_screen);
DECLARE_int32(controller_instance);

DEFINE_string(
    split_screen_strip_crop, "0,0,100,48.889",
    "Basically360, split_screen: the part of the picture a copy shown as a "
    "wide strip (2 players, or player 1 of 3) shows, as x,y,width,height in "
    "percent of the picture. The default fits Garden Warfare with its split "
    "screen patch (the view drawn in the top 1280x352).",
    "Display");
DEFINE_string(
    split_screen_strip_address, "",
    "Basically360, split_screen: guest address (hex) where Xenia writes, while "
    "this copy is shown as a wide strip, how many rows of the 1280x720 picture "
    "the game should draw its view in (the strip's shape; 0 otherwise), with "
    "float aspect ratio and field of view multipliers at +0x10 and +0x14, for "
    "a game patch (Garden Warfare: 0x89958400). The copy then shows those "
    "rows. Empty: none (split_screen_strip_crop is shown).",
    "Display");
DEFINE_string(
    split_screen_join_address, "",
    "Basically360, split_screen: guest address (hex) of a 32-bit flag player "
    "1's game (patched) sets while other players may join, or \"server\": "
    "the program serving the game sets it in the table the copies share "
    "(Garden Warfare Server: while player 1's match is on). Empty: players "
    "may join any time.",
    "Display");
DEFINE_int32(
    split_screen_pause_after, 75,
    "Basically360, split_screen: seconds after starting that a copy still "
    "waiting for its player is slowed to 2 frames a second at idle priority "
    "(little CPU or GPU use until the player joins). 0: never.",
    "Display");
DEFINE_string(
    split_screen_join_script, "",
    "Basically360, split_screen: buttons Xenia presses for a player who has "
    "just joined, to take them into player 1's match: button names (a b x y "
    "start back up down left right lb rb, or several joined with +) pressed "
    "in turn and numbers of seconds to wait, separated by commas, for example "
    "\"3,start,20,a,4,a\". Empty: none.",
    "Display");

DEFINE_bool(
    split_screen_prejoin, false,
    "Basically360, split_screen: while player 1 plays alone, the next "
    "player's copy (the lowest numbered one waiting) goes into player 1's "
    "match ahead of time, hidden (with "
    "split_screen_join_script, "
    "once player 1's match lets players join), drawn with the shape it gets "
    "when its player joins, and waits at the character select screen; a "
    "player who joins is then there at once, like on the Xbox One. The "
    "waiting player is in player 1's match all along.",
    "Display");
DEFINE_int32(
    split_screen_prejoin_after, 40,
    "Basically360, split_screen_prejoin: seconds after starting before a "
    "waiting copy may go into player 1's match (its title screen up).",
    "Display");
DEFINE_int32(
    split_screen_prejoin_settle, 75,
    "Basically360, split_screen_prejoin: seconds the game patch must say a "
    "match is drawn (it does while the level still loads) before a waiting "
    "copy counts as at the character select screen and is slowed.",
    "Display");
DEFINE_int32(
    split_screen_prejoin_fps, 15,
    "Basically360, split_screen_prejoin: frames a second a copy waiting at "
    "the character select screen runs at until its player joins (0: full "
    "speed).",
    "Display");

DEFINE_int32(
    split_screen_strip_view_top, 8,
    "Basically360, split_screen: rows of its picture a game (patched) leaves "
    "above its view in a strip (Garden Warfare: 8, its 1280x704 view placed 8 "
    "rows down); the game is told that many rows more and the strip shows from "
    "there.",
    "Display");

DEFINE_string(
    split_screen_cpu_mask, "",
    "Basically360, split_screen: logical processors this copy runs on, as a "
    "hex mask (bit 0: processor 0), e.g. 0xFFFF. Empty: all.",
    "Display");

DEFINE_bool(
    split_screen_auto_cores, true,
    "Basically360, split_screen: on a processor with fast and slow cores, the "
    "copies on screen share out the fast cores between them (each its own) "
    "and the waiting copies get the slow ones. Not with split_screen_cpu_mask.",
    "Display");

DEFINE_string(
    split_screen_debug_address, "",
    "Basically360, split_screen: guest address (hex) of 64 bytes a debugging "
    "patch fills; logged every 2 seconds when they change. Empty: none.",
    "Display");

namespace xe {
namespace app {

namespace {

constexpr size_t kCount = hid::ControllerInstanceTable::kCount;

uint32_t ParseAddress(const std::string& text) {
  if (text.empty()) {
    return 0;
  }
  return uint32_t(std::strtoul(text.c_str(), nullptr, 16));
}

// The host memory behind a 32-bit guest flag, if its page is committed. The
// protection the heap records isn't checked: a patch's data in a title's image
// (where the game itself reads and writes the flag) may be recorded as no
// access while it's in fact read-write.
// The host memory behind a 32-bit guest flag, if it's there and writable. The
// guest heap's page table isn't asked: a title image's pages may be recorded
// there as free or no access while the game reads and writes them; the host
// memory behind them is what counts.
uint8_t* GuestFlag(Memory* memory, uint32_t address,
                   std::string* why = nullptr) {
  if (!memory || !address) {
    if (why) {
      *why = "no memory or address";
    }
    return nullptr;
  }
  uint8_t* host = memory->TranslateVirtual<uint8_t*>(address & ~uint32_t(3));
#if XE_PLATFORM_WIN32
  MEMORY_BASIC_INFORMATION info = {};
  if (!VirtualQuery(host, &info, sizeof(info)) || info.State != MEM_COMMIT ||
      !(info.Protect & (PAGE_READWRITE | PAGE_EXECUTE_READWRITE)) ||
      (info.Protect & PAGE_GUARD)) {
    if (why) {
      *why = fmt::format("host memory state {:X} protect {:X}", info.State,
                         info.Protect);
    }
    return nullptr;
  }
  if (why) {
    *why = "ok";
  }
  return host;
#else
  if (why) {
    *why = "not on this platform";
  }
  return nullptr;
#endif
}

#if XE_PLATFORM_WIN32
bool ProcessRunning(uint32_t pid) {
  if (!pid) {
    return false;
  }
  if (pid == GetCurrentProcessId()) {
    return true;
  }
  HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
  if (!process) {
    return GetLastError() == ERROR_ACCESS_DENIED;
  }
  const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
  CloseHandle(process);
  return running;
}

class TableLock {
 public:
  explicit TableLock(void* mutex) : mutex_(mutex) {
    if (mutex_) {
      const DWORD wait = WaitForSingleObject(mutex_, 200);
      locked_ = wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED;
    }
  }
  ~TableLock() {
    if (locked_) {
      ReleaseMutex(mutex_);
    }
  }
  bool locked() const { return locked_; }

 private:
  void* mutex_;
  bool locked_ = false;
};
#endif  // XE_PLATFORM_WIN32

}  // namespace

bool SplitScreen::Placement::operator==(const Placement& other) const {
  return visible == other.visible && strip == other.strip &&
         !std::memcmp(rect, other.rect, sizeof(rect));
}

bool SplitScreen::Enabled() {
#if XE_PLATFORM_WIN32
  return cvars::split_screen && cvars::controller_instance >= 1 &&
         cvars::controller_instance <= int32_t(kCount);
#else
  return false;
#endif
}

SplitScreen::SplitScreen(Emulator* emulator,
                         ui::WindowedAppContext& app_context,
                         ui::Window* window)
    : emulator_(emulator),
      app_context_(app_context),
      window_(window),
      me_(size_t(cvars::controller_instance - 1)),
      alive_(std::make_shared<std::atomic<bool>>(true)) {
#if XE_PLATFORM_WIN32
  table_mutex_ = CreateMutexW(nullptr, FALSE, XE_CONTROLLER_INSTANCE_LOCK_NAME);
  HANDLE mapping =
      CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                         DWORD(sizeof(hid::ControllerInstanceTable)),
                         XE_CONTROLLER_INSTANCE_TABLE_NAME);
  if (mapping) {
    table_ = MapViewOfFile(mapping, FILE_MAP_ALL_ACCESS, 0, 0,
                           sizeof(hid::ControllerInstanceTable));
    if (table_) {
      table_mapping_ = mapping;
      TableLock lock(table_mutex_);
      auto& table = *static_cast<hid::ControllerInstanceTable*>(table_);
      if (table.magic != hid::ControllerInstanceTable::kMagic) {
        std::memset(&table, 0, sizeof(table));
        table.magic = hid::ControllerInstanceTable::kMagic;
      }
    } else {
      CloseHandle(mapping);
    }
  }
  if (!table_) {
    XELOGE("split_screen: could not open the table shared by the copies ({})",
           GetLastError());
  }
#endif  // XE_PLATFORM_WIN32
  script_ = ParseScript(cvars::split_screen_join_script);
}

SplitScreen::~SplitScreen() {
  alive_->store(false);
  if (thread_.joinable()) {
    {
      std::lock_guard<std::mutex> lock(thread_mutex_);
      thread_stop_ = true;
    }
    thread_cv_.notify_all();
    thread_.join();
  }
#if XE_PLATFORM_WIN32
  if (table_) {
    UnmapViewOfFile(table_);
    table_ = nullptr;
  }
  if (table_mapping_) {
    CloseHandle(table_mapping_);
    table_mapping_ = nullptr;
  }
  if (table_mutex_) {
    CloseHandle(table_mutex_);
    table_mutex_ = nullptr;
  }
#endif  // XE_PLATFORM_WIN32
}

void SplitScreen::ApplyInitialPlacement() {
  Placement placement;
  placement.visible = me_ == 0;
  placement.rect[2] = 100.0f;
  placement.rect[3] = 100.0f;
  ApplyPlacement(placement);
}

void SplitScreen::Start() {
  start_time_ = std::chrono::steady_clock::now();
  std::shared_ptr<std::atomic<bool>> alive = alive_;
  thread_ = std::thread([this, alive]() {
    std::unique_lock<std::mutex> lock(thread_mutex_);
    while (!thread_stop_) {
      thread_cv_.wait_for(lock, std::chrono::milliseconds(100));
      if (thread_stop_) {
        break;
      }
      app_context_.CallInUIThread([this, alive]() {
        if (alive->load()) {
          Tick();
        }
      });
    }
  });
  XELOGI("split_screen: this copy is player {}'s", me_ + 1);
#if XE_PLATFORM_WIN32
  {
    int regs[4] = {};
    char brand[49] = {};
    __cpuid(regs, 0x80000000);
    if (uint32_t(regs[0]) >= 0x80000004) {
      for (int i = 0; i < 3; ++i) {
        __cpuid(reinterpret_cast<int*>(brand + i * 16), 0x80000002 + i);
      }
    }
    SetFastCores(me_ == 0);
    XELOGI("split_screen: processor {}, {} logical processors",
           std::string(brand), GetActiveProcessorCount(ALL_PROCESSOR_GROUPS));
    if (!cvars::split_screen_cpu_mask.empty()) {
      const uint64_t mask =
          std::strtoull(cvars::split_screen_cpu_mask.c_str(), nullptr, 16);
      if (mask &&
          SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(mask))) {
        XELOGI("split_screen: runs on processors mask {:X}", mask);
      } else {
        XELOGE("split_screen: could not set processors mask {} ({})",
               cvars::split_screen_cpu_mask, GetLastError());
      }
    }
  }
#endif  // XE_PLATFORM_WIN32
}

void SplitScreen::SetFastCores(bool fast) {
#if XE_PLATFORM_WIN32
  // Windows 11 on hybrid processors moves the threads of programs in the
  // background (not focused) to the slower cores ("EcoQoS"): a copy on screen
  // asks never to be slowed so, focused or not; a waiting copy asks to be, to
  // leave the fast cores to the players.
  PROCESS_POWER_THROTTLING_STATE state = {};
  state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
  state.ControlMask = PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  state.StateMask = fast ? 0 : PROCESS_POWER_THROTTLING_EXECUTION_SPEED;
  if (fast_cores_ != int(fast) &&
      SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling, &state,
                            sizeof(state))) {
    fast_cores_ = int(fast);
    XELOGI("split_screen: this copy {}",
           fast ? "keeps the fast cores" : "may be put on slow cores");
  }
  // Windows 11 also stops honouring the timer resolution Xenia asks for
  // (0.5 ms) for programs it counts as in the background: their short
  // sleeps (vertical blanks, the frame limiter) then last a whole 15.6 ms
  // tick, and an unfocused copy drops to 16 frames a second. Every copy asks
  // for its timer resolution to be honoured.
  if (!timer_resolution_kept_) {
#ifndef PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION
#define PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION 0x4
#endif
    PROCESS_POWER_THROTTLING_STATE timer_state = {};
    timer_state.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    timer_state.ControlMask = PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    timer_state.StateMask = 0;
    if (SetProcessInformation(GetCurrentProcess(), ProcessPowerThrottling,
                              &timer_state, sizeof(timer_state))) {
      timer_resolution_kept_ = true;
      XELOGI("split_screen: timer resolution kept in the background");
    } else {
      timer_resolution_kept_ = true;
      XELOGW("split_screen: could not keep the timer resolution ({})",
             GetLastError());
    }
  }
#endif  // XE_PLATFORM_WIN32
}

void SplitScreen::UpdateCores(bool on_screen, size_t position, size_t count) {
#if XE_PLATFORM_WIN32
  if (!cvars::split_screen_auto_cores ||
      !cvars::split_screen_cpu_mask.empty()) {
    return;
  }
  if (!cores_known_) {
    cores_known_ = true;
    ULONG length = 0;
    GetSystemCpuSetInformation(nullptr, 0, &length, GetCurrentProcess(), 0);
    std::vector<uint8_t> buffer(length);
    if (!length ||
        !GetSystemCpuSetInformation(
            reinterpret_cast<PSYSTEM_CPU_SET_INFORMATION>(buffer.data()),
            length, &length, GetCurrentProcess(), 0)) {
      XELOGW("split_screen: could not read the processor's cores ({})",
             GetLastError());
      return;
    }
    // Physical cores (by group and core index) with their logical processors
    // and efficiency class (higher: faster).
    std::map<uint32_t, std::pair<uint64_t, uint8_t>> cores;
    uint8_t fastest = 0;
    for (size_t offset = 0; offset < length;) {
      const auto& info = *reinterpret_cast<const SYSTEM_CPU_SET_INFORMATION*>(
          buffer.data() + offset);
      if (!info.Size) {
        break;
      }
      offset += info.Size;
      if (info.Type != CpuSetInformation || info.CpuSet.Group != 0 ||
          info.CpuSet.LogicalProcessorIndex >= 64) {
        continue;
      }
      auto& core = cores[info.CpuSet.CoreIndex];
      core.first |= uint64_t(1) << info.CpuSet.LogicalProcessorIndex;
      core.second = info.CpuSet.EfficiencyClass;
      fastest = std::max(fastest, uint8_t(info.CpuSet.EfficiencyClass));
    }
    for (const auto& core : cores) {
      all_cores_mask_ |= core.second.first;
      (core.second.second == fastest ? fast_core_masks_ : slow_core_masks_)
          .push_back(core.second.first);
    }
    XELOGI(
        "split_screen: {} fast and {} slow cores, {} logical processors "
        "(mask {:X})",
        fast_core_masks_.size(), slow_core_masks_.size(),
        std::bitset<64>(all_cores_mask_).count(), all_cores_mask_);
  }
  if (slow_core_masks_.empty() || fast_core_masks_.empty()) {
    // All the cores are alike: Windows shares them out well enough.
    return;
  }
  // Cores [begin, end) of n even shares, the share at index i.
  auto share = [](const std::vector<uint64_t>& masks, size_t i, size_t n) {
    uint64_t mask = 0;
    for (size_t c = masks.size() * i / n; c < masks.size() * (i + 1) / n; ++c) {
      mask |= masks[c];
    }
    return mask;
  };
  uint64_t mask;
  if (!on_screen) {
    mask = share(slow_core_masks_, 0, 1);
  } else if (count <= 1) {
    mask = all_cores_mask_;
  } else {
    // Each copy on screen its own fast cores (Windows otherwise favours the
    // focused window's threads on them), with 3 or 4 also its own slow ones.
    mask = share(fast_core_masks_, position, count);
    if (count >= 3 || !mask) {
      mask |= share(slow_core_masks_, position, count);
    }
  }
  // With 3 or 4 on screen a copy's own fast cores are few: its busiest
  // threads (the game's main thread and job thread) are kept on them, as
  // Windows would put an unfocused window's threads on the slow ones.
  const uint64_t pin_mask =
      on_screen && count >= 3 ? share(fast_core_masks_, position, count) : 0;
  // GPU scheduling: Windows favours the focused window's GPU work, and the
  // game waits for some of its GPU results (its render jobs are switched on
  // by the GPU), so an unfocused copy waits longer for them each frame. The
  // copies on screen all ask for high GPU priority, the waiting ones low.
  const int gpu_priority = !on_screen   ? 1 /* below normal */
                           : count >= 2 ? 4 /* high */
                                        : 2 /* normal */;
  if (gpu_priority != gpu_priority_) {
    using SetPriorityFn = LONG(APIENTRY*)(HANDLE, int);
    static const auto set_gpu_priority = reinterpret_cast<SetPriorityFn>(
        GetProcAddress(LoadLibraryW(L"gdi32.dll"),
                       "D3DKMTSetProcessSchedulingPriorityClass"));
    const LONG status =
        set_gpu_priority ? set_gpu_priority(GetCurrentProcess(), gpu_priority)
                         : -1;
    gpu_priority_ = gpu_priority;
    if (status == 0) {
      XELOGI("split_screen: GPU priority {}", gpu_priority == 4 ? "high"
                                              : gpu_priority == 2
                                                  ? "normal"
                                                  : "below normal");
    } else {
      XELOGW("split_screen: could not set the GPU priority ({:X})",
             uint32_t(status));
    }
  }
  if (!mask || (mask == cores_mask_ && pin_mask == pin_mask_)) {
    return;
  }
  pin_mask_ = pin_mask;
  if (SetProcessAffinityMask(GetCurrentProcess(), DWORD_PTR(mask))) {
    cores_mask_ = mask;
    PinBusyThreads();
    XELOGI("split_screen: runs on processors mask {:X} ({})", mask,
           !on_screen   ? "waiting: the slow cores"
           : count <= 1 ? "alone: all"
                        : "its share of the fast cores");
  } else {
    XELOGE("split_screen: could not set processors mask {:X} ({})", mask,
           GetLastError());
  }
#endif  // XE_PLATFORM_WIN32
}

void SplitScreen::PinBusyThreads() {
#if XE_PLATFORM_WIN32
  // Busy threads on pin_mask_ (when set), the others back on the process'
  // processors.
  std::vector<uint32_t> pinned;
  for (uint32_t id : busy_threads_) {
    if (!pin_mask_) {
      break;
    }
    HANDLE thread = OpenThread(
        THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
        FALSE, id);
    if (thread) {
      if (SetThreadAffinityMask(thread, DWORD_PTR(pin_mask_))) {
        pinned.push_back(id);
      }
      CloseHandle(thread);
    }
  }
  for (uint32_t id : pinned_threads_) {
    if (std::find(pinned.begin(), pinned.end(), id) != pinned.end()) {
      continue;
    }
    HANDLE thread = OpenThread(
        THREAD_SET_LIMITED_INFORMATION | THREAD_QUERY_LIMITED_INFORMATION,
        FALSE, id);
    if (thread) {
      SetThreadAffinityMask(
          thread, DWORD_PTR(cores_mask_ ? cores_mask_ : all_cores_mask_));
      CloseHandle(thread);
    }
  }
  if (pinned != pinned_threads_ && !pinned.empty()) {
    XELOGI("split_screen: {} busy threads kept on processors mask {:X}",
           pinned.size(), pin_mask_);
  }
  pinned_threads_ = std::move(pinned);
#endif  // XE_PLATFORM_WIN32
}

void SplitScreen::LogLoad(double seconds) {
#if XE_PLATFORM_WIN32
  // The copy's processor use, and its busiest threads (% of one processor).
  FILETIME created, exited, kernel, user;
  auto ticks = [](const FILETIME& t) {
    return (uint64_t(t.dwHighDateTime) << 32) | t.dwLowDateTime;
  };
  if (!GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel,
                       &user)) {
    return;
  }
  const uint64_t process_total = ticks(kernel) + ticks(user);
  std::vector<std::pair<double, std::string>> busy;
  std::vector<uint32_t> busy_now;
  HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
  if (snapshot != INVALID_HANDLE_VALUE) {
    THREADENTRY32 entry;
    entry.dwSize = sizeof(entry);
    const DWORD pid = GetCurrentProcessId();
    std::map<uint32_t, uint64_t> now_times;
    for (BOOL more = Thread32First(snapshot, &entry); more;
         more = Thread32Next(snapshot, &entry)) {
      if (entry.th32OwnerProcessID != pid) {
        continue;
      }
      HANDLE thread = OpenThread(THREAD_QUERY_LIMITED_INFORMATION, FALSE,
                                 entry.th32ThreadID);
      if (!thread) {
        continue;
      }
      if (GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
        const uint64_t total = ticks(kernel) + ticks(user);
        now_times[entry.th32ThreadID] = total;
        auto last = thread_times_.find(entry.th32ThreadID);
        if (last != thread_times_.end() && seconds > 0.0) {
          const double percent =
              double(total - last->second) / (seconds * 1.0e7) * 100.0;
          std::string name;
          PWSTR description = nullptr;
          if (SUCCEEDED(GetThreadDescription(thread, &description)) &&
              description) {
            for (const wchar_t* c = description; *c; ++c) {
              name.push_back(*c < 128 ? char(*c) : '?');
            }
            LocalFree(description);
          }
          if (name.empty()) {
            name = fmt::format("{}", entry.th32ThreadID);
          }
          busy.emplace_back(percent, name);
          if (percent >= 40.0) {
            busy_now.push_back(entry.th32ThreadID);
          }
        }
      }
      CloseHandle(thread);
    }
    CloseHandle(snapshot);
    thread_times_ = std::move(now_times);
    if (seconds > 0.0) {
      busy_threads_ = std::move(busy_now);
      PinBusyThreads();
    }
  }
  std::sort(busy.begin(), busy.end(),
            [](const auto& a, const auto& b) { return a.first > b.first; });
  std::string top;
  for (size_t i = 0; i < busy.size() && i < 6; ++i) {
    top += fmt::format("{}{} {:.0f}%", i ? ", " : "", busy[i].second,
                       busy[i].first);
  }
  const double cores =
      last_process_time_ && seconds > 0.0
          ? double(process_total - last_process_time_) / (seconds * 1.0e7)
          : 0.0;
  last_process_time_ = process_total;
  // Clocks of the fast and slow cores (Windows' figures) and the power
  // source: a laptop's cores slow down when many are busy or on battery.
  std::string clocks;
  struct ProcessorPowerInformation {
    ULONG number;
    ULONG max_mhz;
    ULONG current_mhz;
    ULONG mhz_limit;
    ULONG max_idle_state;
    ULONG current_idle_state;
  };
  using CallNtPowerInformationFn =
      LONG(WINAPI*)(int, PVOID, ULONG, PVOID, ULONG);
  static const auto call_nt_power_information =
      reinterpret_cast<CallNtPowerInformationFn>(GetProcAddress(
          LoadLibraryW(L"powrprof.dll"), "CallNtPowerInformation"));
  const DWORD processor_count = GetActiveProcessorCount(0);
  if (call_nt_power_information && processor_count && processor_count <= 64) {
    std::vector<ProcessorPowerInformation> power(processor_count);
    if (call_nt_power_information(
            11 /* ProcessorInformation */, nullptr, 0, power.data(),
            ULONG(power.size() * sizeof(ProcessorPowerInformation))) == 0) {
      uint64_t fast_mask = 0;
      for (uint64_t mask : fast_core_masks_) {
        fast_mask |= mask;
      }
      double sums[2][3] = {};
      for (const ProcessorPowerInformation& info : power) {
        const int fast =
            info.number < 64 && ((fast_mask >> info.number) & 1) ? 1 : 0;
        sums[fast][0] += info.current_mhz;
        sums[fast][1] += info.mhz_limit;
        sums[fast][2] += 1.0;
      }
      for (int fast = 1; fast >= 0; --fast) {
        if (sums[fast][2] > 0.0) {
          clocks += fmt::format(
              "{}{} cores {:.0f} MHz (limit {:.0f})",
              clocks.empty() ? "" : ", ", fast ? "fast" : "slow",
              sums[fast][0] / sums[fast][2], sums[fast][1] / sums[fast][2]);
        }
      }
    }
  }
  SYSTEM_POWER_STATUS power_status;
  if (GetSystemPowerStatus(&power_status)) {
    clocks += power_status.ACLineStatus == 1   ? ", plugged in"
              : power_status.ACLineStatus == 0 ? ", on battery"
                                               : "";
  }
  XELOGI("split_screen: load {:.1f} processors; busiest threads: {}; {}", cores,
         top, clocks);
#endif  // XE_PLATFORM_WIN32
}

void SplitScreen::RequestTestJoin() {
#if XE_PLATFORM_WIN32
  if (!table_) {
    return;
  }
  TableLock lock(table_mutex_);
  if (!lock.locked()) {
    return;
  }
  auto& table = *static_cast<hid::ControllerInstanceTable*>(table_);
  for (size_t i = 1; i < kCount; ++i) {
    if (ProcessRunning(table.pid[i]) && !table.joined[i]) {
      table.join_requested[i] = 1;
      XELOGI("split_screen: test key, player {} joins", i + 1);
      return;
    }
  }
#endif  // XE_PLATFORM_WIN32
}

void SplitScreen::Tick() {
#if XE_PLATFORM_WIN32
  if (!table_) {
    return;
  }
  uint32_t pid[kCount];
  uint32_t joined[kCount];
  bool join_open_now = false;
  {
    TableLock lock(table_mutex_);
    if (!lock.locked()) {
      return;
    }
    auto& table = *static_cast<hid::ControllerInstanceTable*>(table_);
    // "server": the Garden Warfare Server sets join_open (in the table) while
    // a match is on.
    if (me_ == 0 && cvars::split_screen_join_address != "server") {
      const uint32_t join_open = ParseAddress(cvars::split_screen_join_address)
                                     ? ReadGuestJoinOpen()
                                     : 1;
      if (join_open != table.join_open || !join_open_logged_) {
        XELOGI("split_screen: other players {} join now",
               join_open ? "can" : "can't");
        join_open_logged_ = true;
      }
      table.join_open = join_open;
    }
    if (table.join_requested[me_]) {
      if (table.joined[me_]) {
        table.join_requested[me_] = 0;
      } else if (table.join_open) {
        table.join_requested[me_] = 0;
        table.joined[me_] = 1;
        XELOGI("split_screen: player {} asked to join", me_ + 1);
      } else if (!join_wait_logged_) {
        XELOGI("split_screen: player {} waits for player 1's match to join",
               me_ + 1);
        join_wait_logged_ = true;
      }
    }
    std::memcpy(pid, table.pid, sizeof(pid));
    std::memcpy(joined, table.joined, sizeof(joined));
    join_open_now = table.join_open != 0;
  }

  // The copies on screen, in player order: player 1's while it runs, the
  // others' once their player has joined.
  size_t active[kCount];
  size_t active_count = 0;
  size_t my_position = SIZE_MAX;
  for (size_t i = 0; i < kCount; ++i) {
    const bool running = i == me_ || ProcessRunning(pid[i]);
    if (!running || (i != 0 && !joined[i])) {
      continue;
    }
    if (i == me_) {
      my_position = active_count;
    }
    active[active_count++] = i;
  }

  Placement placement;
  if (my_position != SIZE_MAX) {
    placement.visible = true;
    float* r = placement.rect;
    switch (active_count) {
      case 1:
        r[2] = 100.0f;
        r[3] = 100.0f;
        break;
      case 2:
        placement.strip = true;
        r[1] = my_position ? 50.0f : 0.0f;
        r[2] = 100.0f;
        r[3] = 50.0f;
        break;
      case 3:
        if (!my_position) {
          placement.strip = true;
          r[2] = 100.0f;
        } else {
          r[0] = my_position == 1 ? 0.0f : 50.0f;
          r[1] = 50.0f;
          r[2] = 50.0f;
        }
        r[3] = 50.0f;
        break;
      default:
        r[0] = (my_position & 1) ? 50.0f : 0.0f;
        r[1] = (my_position & 2) ? 50.0f : 0.0f;
        r[2] = 50.0f;
        r[3] = 50.0f;
        break;
    }
  }
  UpdateCores(placement.visible, my_position, active_count);
  if (!placement_applied_ || !(placement == placement_)) {
    if (placement.visible && !placement_.visible && placement_applied_ &&
        me_ != 0) {
      // This player has just joined.
      XELOGI("split_screen: player {} joined ({} on screen){}", me_ + 1,
             active_count,
             prejoin_ready_     ? ", already at the character select screen"
             : prejoin_started_ ? ", on the way into the match"
                                : "");
      if (!prejoin_started_) {
        script_step_ = 0;
        script_step_time_ = std::chrono::steady_clock::now();
        script_button_down_ = false;
      }
      joined_seen_ = true;
      SetFastCores(true);
    }
    ApplyPlacement(placement);
  }
  // A strip: the game draws its view in the top rows of its 1280x720 picture,
  // as many as give the strip's shape (a multiple of 16), shown filling it.
  // A copy going into player 1's match ahead of time (hidden) is drawn with
  // the shape it gets when its player joins (the next one on screen), so its
  // screens are made with that shape and nothing changes when it appears.
  uint32_t rows = 0;
  if (ParseAddress(cvars::split_screen_strip_address)) {
    if (placement.visible) {
      if (placement.strip) {
        rows = StripRows(placement.rect);
      }
    } else if (prejoin_started_ && active_count + 1 == 2) {
      const float next_rect[4] = {0.0f, 50.0f, 100.0f, 50.0f};
      rows = StripRows(next_rect);
    }
  }
  // The game draws its view a few rows down its picture (Garden Warfare: 8,
  // its 1280x704 view placed 8 rows down) and no further than the rows it is
  // told: it is told that many rows more, and the strip shows from there, with
  // exactly the strip's shape (no black line on top, nothing undrawn below).
  const uint32_t top =
      rows ? uint32_t(std::min(std::max(cvars::split_screen_strip_view_top, 0),
                               int32_t(64)))
           : 0;
  const uint32_t game_rows =
      rows ? std::min((rows + top + 4) & ~uint32_t(7), uint32_t(704)) : 0;
  if (game_rows != strip_rows_ || top != strip_top_) {
    strip_rows_ = game_rows;
    strip_top_ = top;
    if (game_rows) {
      ui::Presenter::SetCropOverride(0.0f, float(top) * 100.0f / 720.0f, 100.0f,
                                     float(game_rows - top) * 100.0f / 720.0f);
    }
  }
  WriteGuestStrip(game_rows);

  // Diagnostics every 10 seconds: the flags as this copy sees them.
  const auto now = std::chrono::steady_clock::now();
  if (now - last_diagnostic_ >= std::chrono::seconds(10)) {
    last_diagnostic_ = now;
    std::string why_strip, why_join;
    const uint8_t* strip_host =
        GuestFlag(emulator_->memory(),
                  ParseAddress(cvars::split_screen_strip_address), &why_strip);
    const uint8_t* join_host =
        GuestFlag(emulator_->memory(),
                  ParseAddress(cvars::split_screen_join_address), &why_join);
    const uint64_t swaps = gpu::GraphicsSystem::swap_count.load();
    const double seconds =
        std::chrono::duration<double>(now - last_swap_time_).count();
    const double fps =
        last_swap_time_.time_since_epoch().count() && seconds > 0.0
            ? double(swaps - last_swap_count_) / seconds
            : 0.0;
    last_swap_count_ = swaps;
    last_swap_time_ = now;
    XELOGI("split_screen: {:.1f} frames a second", fps);
    LogLoad(seconds);
    XELOGI(
        "split_screen: title open {}, strip {} ({}), join flag {} ({}), "
        "players on screen {}",
        emulator_->is_title_open(),
        strip_host ? int64_t(xe::load_and_swap<uint32_t>(strip_host)) : -1,
        why_strip,
        join_host ? int64_t(xe::load_and_swap<uint32_t>(join_host)) : -1,
        why_join, active_count);
  }

  // Debugging: 64 bytes a probe patch fills, logged when they change.
  if (const uint32_t debug_address =
          ParseAddress(cvars::split_screen_debug_address)) {
    if (now - last_debug_ >= std::chrono::seconds(2)) {
      last_debug_ = now;
      if (const uint8_t* host = GuestFlag(emulator_->memory(), debug_address)) {
        std::array<uint32_t, 16> words;
        for (size_t i = 0; i < words.size(); ++i) {
          words[i] = xe::load_and_swap<uint32_t>(host + 4 * i);
        }
        if (words != last_debug_words_) {
          last_debug_words_ = words;
          std::string text;
          for (size_t i = 0; i < words.size(); ++i) {
            float f;
            std::memcpy(&f, &words[i], 4);
            text += fmt::format("{}{:08X}({:g})", i ? " " : "", words[i], f);
          }
          XELOGI("split_screen debug {:08X}: {}", debug_address, text);
        }
      }
    }
  }

  // A copy waiting for its player: after a while, slow it to 2 frames a
  // second at idle priority. (Not suspended: suspending the game's threads
  // can leave a lock taken that the UI thread then waits for forever.)
  if (!placement.visible) {
    if (cvars::split_screen_prejoin && me_ != 0) {
      // Only the next player to join goes in ahead of time (each player
      // waiting in player 1's match costs player 1's game frames): the copies
      // of lower numbered players have all joined, or aren't running.
      // Only while player 1 plays alone: every player in player 1's match
      // costs both screens frames (two players: about 16 and 23 frames a
      // second; a third one waiting: 12 and 17), so players 3 and 4 join the
      // usual way.
      bool next_in_line = active_count == 1;
      for (size_t i = 1; i < me_; ++i) {
        if (ProcessRunning(pid[i]) && !joined[i]) {
          next_in_line = false;
        }
      }
      TickPrejoin(join_open_now && (next_in_line || prejoin_started_), now);
      if (prejoin_started_) {
        return;
      }
    }
    const auto waited = std::chrono::steady_clock::now() - start_time_;
    if (!paused_by_us_ && cvars::split_screen_pause_after > 0 &&
        waited >= std::chrono::seconds(cvars::split_screen_pause_after) &&
        emulator_->is_title_open()) {
      XELOGI("split_screen: slowed down until player {} joins", me_ + 1);
      gpu::GraphicsSystem::slow_vblank_hz.store(2);
      normal_priority_ = GetPriorityClass(GetCurrentProcess());
      SetPriorityClass(GetCurrentProcess(), IDLE_PRIORITY_CLASS);
      paused_by_us_ = true;
    }
  } else {
    if (paused_by_us_) {
      gpu::GraphicsSystem::slow_vblank_hz.store(0);
      if (normal_priority_) {
        SetPriorityClass(GetCurrentProcess(), normal_priority_);
      }
      paused_by_us_ = false;
    }
    if (joined_seen_) {
      RunScript();
      // Player 1's match has ended (no joining for a while, past its score
      // screen): this player's copy starts again, to wait for player 1's next
      // match (player 1 then fills the screen again). The game drops the
      // other players when its host leaves the match anyway.
      if (cvars::split_screen_prejoin && me_ != 0) {
        if (join_open_now) {
          join_closed_seen_ = false;
        } else if (!join_closed_seen_) {
          join_closed_seen_ = true;
          join_closed_time_ = now;
        } else if (now - join_closed_time_ >= std::chrono::seconds(15)) {
          XELOGI(
              "split_screen: player 1's match has ended; player {}'s copy "
              "starts again to wait for the next one",
              me_ + 1);
          Relaunch();
        }
      }
    }
  }
#endif  // XE_PLATFORM_WIN32
}

uint32_t SplitScreen::StripRows(const float rect[4]) {
  // The part of the screen the copy fills (fullscreen: of its monitor).
  double width = 0.0, height = 0.0;
#if XE_PLATFORM_WIN32
  HWND hwnd = static_cast<ui::Win32Window*>(window_)->hwnd();
  HMONITOR monitor =
      hwnd ? MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY) : nullptr;
  MONITORINFO monitor_info;
  monitor_info.cbSize = sizeof(monitor_info);
  if (monitor && GetMonitorInfo(monitor, &monitor_info)) {
    width = double(monitor_info.rcMonitor.right - monitor_info.rcMonitor.left) *
            rect[2] / 100.0;
    height =
        double(monitor_info.rcMonitor.bottom - monitor_info.rcMonitor.top) *
        rect[3] / 100.0;
  }
#endif  // XE_PLATFORM_WIN32
  if (width < 1.0 || height < 1.0) {
    width = double(window_->GetActualPhysicalWidth());
    height = double(window_->GetActualPhysicalHeight());
  }
  if (width < 1.0 || height < 1.0) {
    return 0;
  }
  // The strip's shape in rows of the 1280-wide picture.
  uint32_t rows = uint32_t(1280.0 * height / width + 0.5);
  return std::min(std::max(rows, uint32_t(176)), uint32_t(640));
}

void SplitScreen::SetSlowed(int32_t frames_per_second, bool idle) {
#if XE_PLATFORM_WIN32
  if (frames_per_second > 0) {
    gpu::GraphicsSystem::slow_vblank_hz.store(uint32_t(frames_per_second));
    if (!paused_by_us_) {
      normal_priority_ = GetPriorityClass(GetCurrentProcess());
    }
    SetPriorityClass(GetCurrentProcess(),
                     idle ? IDLE_PRIORITY_CLASS : BELOW_NORMAL_PRIORITY_CLASS);
    paused_by_us_ = true;
    SetFastCores(false);
  } else if (paused_by_us_) {
    gpu::GraphicsSystem::slow_vblank_hz.store(0);
    if (normal_priority_) {
      SetPriorityClass(GetCurrentProcess(), normal_priority_);
    }
    paused_by_us_ = false;
  }
#endif  // XE_PLATFORM_WIN32
}

bool SplitScreen::ReadGuestInMatch() {
  // The game patch's INMATCH word, 8 bytes after STRIP.
  const uint32_t strip = ParseAddress(cvars::split_screen_strip_address);
  if (!strip || !emulator_->is_title_open()) {
    return false;
  }
  const uint8_t* host = GuestFlag(emulator_->memory(), strip + 8);
  return host && xe::load_and_swap<uint32_t>(host) != 0;
}

void SplitScreen::TickPrejoin(bool join_open,
                              std::chrono::steady_clock::time_point now) {
  // Player 1's match has ended (no joining for a while) after this copy went
  // into it: start this copy again, to go into player 1's next match from the
  // title screen.
  if (prejoin_started_) {
    if (join_open) {
      join_closed_seen_ = false;
    } else if (!join_closed_seen_) {
      join_closed_seen_ = true;
      join_closed_time_ = now;
    } else if (now - join_closed_time_ >= std::chrono::seconds(5)) {
      XELOGI(
          "split_screen: player 1's match has ended; player {}'s copy starts "
          "again for the next one",
          me_ + 1);
      Relaunch();
      return;
    }
  }
  if (!prejoin_started_) {
    // Waiting at the title screen until player 1's match lets players join.
    if (join_open && emulator_->is_title_open() && !script_.empty() &&
        now - start_time_ >=
            std::chrono::seconds(
                std::max(cvars::split_screen_prejoin_after, int32_t(0)))) {
      SetSlowed(0, false);
      prejoin_started_ = true;
      prejoin_time_ = now;
      script_step_ = 0;
      script_step_time_ = now;
      script_button_down_ = false;
      XELOGI(
          "split_screen: player {}'s copy goes into player 1's match ahead "
          "of time (hidden)",
          me_ + 1);
    }
    return;
  }
  if (script_step_ < script_.size()) {
    RunScript();
    if (script_step_ >= script_.size()) {
      prejoin_time_ = now;
      XELOGI("split_screen: player {}'s copy has asked to join the match",
             me_ + 1);
    }
    return;
  }
  if (prejoin_ready_) {
    return;
  }
  // In the match (the game patch says the match is drawn) for a while, or long
  // enough after the script for the level to have loaded: at the character
  // select screen.
  if (ReadGuestInMatch()) {
    if (!in_match_seen_) {
      in_match_seen_ = true;
      in_match_time_ = now;
    }
  } else {
    in_match_seen_ = false;
  }
  if ((in_match_seen_ &&
       now - in_match_time_ >=
           std::chrono::seconds(
               std::max(cvars::split_screen_prejoin_settle, int32_t(0)))) ||
      now - prejoin_time_ >= std::chrono::seconds(180)) {
    prejoin_ready_ = true;
    XELOGI(
        "split_screen: player {}'s copy waits at the character select screen "
        "({}){}",
        me_ + 1, in_match_seen_ ? "in the match" : "by time",
        cvars::split_screen_prejoin_fps > 0
            ? fmt::format(", slowed to {} frames a second",
                          cvars::split_screen_prejoin_fps)
            : std::string());
    SetSlowed(cvars::split_screen_prejoin_fps, true);
  }
}

void SplitScreen::Relaunch() {
#if XE_PLATFORM_WIN32
  if (relaunched_) {
    return;
  }
  relaunched_ = true;
  std::wstring command_line = GetCommandLineW();
  wchar_t directory[MAX_PATH] = {};
  GetCurrentDirectoryW(MAX_PATH, directory);
  STARTUPINFOW startup_info = {};
  startup_info.cb = sizeof(startup_info);
  PROCESS_INFORMATION process_info = {};
  if (!CreateProcessW(nullptr, command_line.data(), nullptr, nullptr, FALSE,
                      normal_priority_ ? normal_priority_ : 0, nullptr,
                      directory, &startup_info, &process_info)) {
    XELOGE("split_screen: could not start this copy again ({})",
           GetLastError());
    relaunched_ = false;
    return;
  }
  CloseHandle(process_info.hThread);
  CloseHandle(process_info.hProcess);
  // The new copy takes this player's place in the table; this one goes.
  TerminateProcess(GetCurrentProcess(), 0);
#endif  // XE_PLATFORM_WIN32
}

void SplitScreen::ApplyPlacement(const Placement& placement) {
  placement_ = placement;
  placement_applied_ = true;
#if XE_PLATFORM_WIN32
  static_cast<ui::Win32Window*>(window_)->SetSplitScreenPlacement(
      placement.visible, placement.rect);
#endif  // XE_PLATFORM_WIN32
  float crop[4] = {};
  strip_rows_ = 0;
  strip_top_ = 0;
  // With a strip address the crop follows the rows the game draws (Tick).
  if (placement.strip && !ParseAddress(cvars::split_screen_strip_address)) {
    const char* text = cvars::split_screen_strip_crop.c_str();
    for (size_t i = 0; i < 4; ++i) {
      char* end = nullptr;
      crop[i] = std::strtof(text, &end);
      if (end == text) {
        crop[2] = 0.0f;
        break;
      }
      while (*end == ' ' || *end == '%' || *end == ',') {
        ++end;
      }
      text = end;
    }
  }
  ui::Presenter::SetCropOverride(crop[0], crop[1], crop[2], crop[3]);
}

void SplitScreen::WriteGuestStrip(uint32_t rows) {
  if (!emulator_->is_title_open()) {
    return;
  }
  // STRIP (rows, 0: normal) at the address; the aspect ratio and field of
  // view multipliers for those rows at +0x10 and +0x14 (written first).
  uint8_t* host = GuestFlag(emulator_->memory(),
                            ParseAddress(cvars::split_screen_strip_address));
  if (!host || xe::load_and_swap<uint32_t>(host) == rows) {
    return;
  }
  if (rows) {
    xe::store_and_swap<float>(host + 0x10, 720.0f / float(rows));
    xe::store_and_swap<float>(host + 0x14, float(rows) / 720.0f);
  }
  xe::store_and_swap<uint32_t>(host, rows);
}

bool SplitScreen::ReadGuestJoinOpen() {
  if (!emulator_->is_title_open()) {
    return false;
  }
  const uint32_t address = ParseAddress(cvars::split_screen_join_address);
  const uint8_t* host = GuestFlag(emulator_->memory(), address);
  if (!host) {
    if (!join_read_failure_logged_) {
      XELOGW("split_screen: can't read split_screen_join_address {:08X} yet",
             address);
      join_read_failure_logged_ = true;
    }
    return false;
  }
  return xe::load_and_swap<uint32_t>(host) != 0;
}

void SplitScreen::RunScript() {
  hid::InputSystem* input = emulator_->input_system();
  if (!input || script_step_ >= script_.size()) {
    return;
  }
  const auto now = std::chrono::steady_clock::now();
  const float elapsed =
      std::chrono::duration<float>(now - script_step_time_).count();
  const ScriptStep& step = script_[script_step_];
  if (!step.buttons) {
    if (elapsed >= step.wait_seconds) {
      ++script_step_;
      script_step_time_ = now;
    }
    return;
  }
  // A button: held for 0.2 seconds, then 0.4 seconds before the next step.
  if (!script_button_down_) {
    input->SetScriptedButtons(step.buttons);
    script_button_down_ = true;
    script_step_time_ = now;
  } else if (elapsed >= 0.2f && elapsed < 0.6f) {
    input->SetScriptedButtons(0);
  } else if (elapsed >= 0.6f) {
    input->SetScriptedButtons(0);
    script_button_down_ = false;
    ++script_step_;
    script_step_time_ = now;
  }
}

std::vector<SplitScreen::ScriptStep> SplitScreen::ParseScript(
    const std::string& text) {
  static const std::pair<const char*, uint16_t> kButtons[] = {
      {"a", 0x1000},     {"b", 0x2000},     {"x", 0x4000},  {"y", 0x8000},
      {"start", 0x0010}, {"back", 0x0020},  {"up", 0x0001}, {"down", 0x0002},
      {"left", 0x0004},  {"right", 0x0008}, {"lb", 0x0100}, {"rb", 0x0200},
  };
  std::vector<ScriptStep> steps;
  size_t start = 0;
  while (start < text.size()) {
    size_t end = text.find(',', start);
    if (end == std::string::npos) {
      end = text.size();
    }
    std::string item;
    for (size_t i = start; i < end; ++i) {
      if (!std::isspace(uint8_t(text[i]))) {
        item.push_back(char(std::tolower(uint8_t(text[i]))));
      }
    }
    start = end + 1;
    if (item.empty()) {
      continue;
    }
    ScriptStep step;
    if (std::isdigit(uint8_t(item[0])) || item[0] == '.') {
      step.wait_seconds = std::strtof(item.c_str(), nullptr);
    } else {
      size_t name_start = 0;
      while (name_start <= item.size()) {
        size_t name_end = item.find('+', name_start);
        if (name_end == std::string::npos) {
          name_end = item.size();
        }
        const std::string name = item.substr(name_start, name_end - name_start);
        bool known = false;
        for (const auto& button : kButtons) {
          if (name == button.first) {
            step.buttons |= button.second;
            known = true;
          }
        }
        if (!known) {
          XELOGW("split_screen_join_script: unknown button \"{}\"", name);
        }
        name_start = name_end + 1;
      }
      if (!step.buttons) {
        continue;
      }
    }
    steps.push_back(step);
  }
  return steps;
}

}  // namespace app
}  // namespace xe
