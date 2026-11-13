/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APP_SPLIT_SCREEN_H_
#define XENIA_APP_SPLIT_SCREEN_H_

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace xe {
class Emulator;
namespace ui {
class Window;
class WindowedAppContext;
}  // namespace ui
namespace app {

// Basically360: live split screen (--split_screen with --controller_instance).
// One copy of Xenia per player, started together. Copy 1 fills the screen; the
// others wait hidden (and paused) until their player joins, then every copy
// moves its window to its part of the screen.
class SplitScreen {
 public:
  static bool Enabled();

  SplitScreen(Emulator* emulator, ui::WindowedAppContext& app_context,
              ui::Window* window);
  ~SplitScreen();

  // Before the window is opened: copy 1 full screen, the others hidden.
  void ApplyInitialPlacement();
  // After the window is opened.
  void Start();
  // Test key (F8): the next copy waiting joins without a controller.
  void RequestTestJoin();

 private:
  struct Placement {
    bool visible = false;
    bool strip = false;
    float rect[4] = {};
    bool operator==(const Placement& other) const;
  };
  struct ScriptStep {
    uint16_t buttons = 0;  // pressed for a moment, or
    float wait_seconds = 0.0f;
  };

  void Tick();
  void ApplyPlacement(const Placement& placement);
  void WriteGuestStrip(bool strip);
  bool ReadGuestJoinOpen();
  void RunScript();
  static std::vector<ScriptStep> ParseScript(const std::string& text);

  Emulator* emulator_;
  ui::WindowedAppContext& app_context_;
  ui::Window* window_;
  size_t me_;

  void* table_mapping_ = nullptr;
  void* table_mutex_ = nullptr;
  void* table_ = nullptr;

  std::shared_ptr<std::atomic<bool>> alive_;
  std::thread thread_;
  std::mutex thread_mutex_;
  std::condition_variable thread_cv_;
  bool thread_stop_ = false;

  bool placement_applied_ = false;
  Placement placement_;
  std::chrono::steady_clock::time_point start_time_;
  bool paused_by_us_ = false;
  unsigned long normal_priority_ = 0;
  bool joined_seen_ = false;
  bool join_open_logged_ = false;
  bool join_wait_logged_ = false;
  bool join_read_failure_logged_ = false;

  std::vector<ScriptStep> script_;
  size_t script_step_ = 0;
  std::chrono::steady_clock::time_point script_step_time_;
  bool script_button_down_ = false;
};

}  // namespace app
}  // namespace xe

#endif  // XENIA_APP_SPLIT_SCREEN_H_
