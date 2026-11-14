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
    "Basically360, split_screen: a copy waiting for its player goes into "
    "player 1's match ahead of time, hidden (with split_screen_join_script, "
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
      TickPrejoin(join_open_now, now);
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
    SetSlowed(cvars::split_screen_prejoin_fps, false);
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
