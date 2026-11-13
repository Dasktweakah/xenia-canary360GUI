/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_SDL_SDL_INPUT_DRIVER_H_
#define XENIA_HID_SDL_SDL_INPUT_DRIVER_H_

#include <array>
#include <atomic>
#include <chrono>
#include <mutex>
#include <optional>
#include <string>

#include "SDL.h"
#include "third_party/rapidcsv/src/rapidcsv.h"
#include "xenia/hid/controller_instances.h"
#include "xenia/hid/input_driver.h"

#define HID_SDL_USER_COUNT 4
#define HID_SDL_THUMB_THRES 0x4E00
#define HID_SDL_TRIGG_THRES 0x1F
#define HID_SDL_REPEAT_DELAY 400
#define HID_SDL_REPEAT_RATE 100

namespace xe {
namespace hid {
namespace sdl {

class SDLInputDriver final : public InputDriver {
 public:
  explicit SDLInputDriver(xe::ui::Window* window, size_t window_z_order);
  ~SDLInputDriver() override;

  X_STATUS Setup() override;

  void LoadGameControllerDB();

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t flags,
                           X_INPUT_CAPABILITIES* out_caps) override;
  X_RESULT GetState(uint32_t user_index, X_INPUT_STATE* out_state) override;
  X_RESULT SetState(uint32_t user_index, X_INPUT_VIBRATION* vibration) override;
  X_RESULT GetKeystroke(uint32_t user_index, uint32_t flags,
                        X_INPUT_KEYSTROKE* out_keystroke) override;
  virtual InputType GetInputType() const override;

 private:
  struct ControllerState {
    SDL_GameController* sdl;
    X_INPUT_CAPABILITIES caps;
    X_INPUT_STATE state;
    bool state_changed;
    // Reported as a guitar kind.
    bool guitar;
    // The whammy bar arrives as the left trigger.
    bool whammy_on_trigger;
    // The whammy has moved, so the stick holds the device's value.
    bool whammy_seen;
    // A button (other than Guide) has been pressed since it was connected.
    bool used;
    // Names the device the same way in every copy of Xenia on this PC (see
    // controller_instance).
    std::string device_key;
  };

  enum class RepeatState {
    Idle,       // no buttons pressed or repeating has ended
    Waiting,    // a button is held and the delay is awaited
    Repeating,  // actively repeating at a rate
  };
  struct KeystrokeState {
    uint64_t buttons;
    RepeatState repeat_state;
    // the button number that was pressed last:
    uint8_t repeat_butt_idx;
    // the last time (ms) a down (and/or repeat) event for that button was send:
    uint32_t repeat_time;
  };

  void HandleEvent(const SDL_Event& event);
  void OnControllerDeviceAdded(const SDL_Event& event);
  void OnControllerDeviceRemoved(const SDL_Event& event);
  void OnControllerDeviceAxisMotion(const SDL_Event& event);
  void OnControllerDeviceButtonChanged(const SDL_Event& event);
  // A controller's first button press: it moves to the lowest port that no
  // other used controller holds (swapping with an unused one), so ports go in
  // the order the players pick their controllers up. Returns its port.
  size_t ClaimPortOnFirstUse(size_t user_index);

  // controller_instance: several copies of Xenia, one per player, share the
  // controllers through a table every copy on this PC opens. A copy takes the
  // first controller pressed that no copy holds, when no lower-numbered copy
  // is still waiting for one, and shows only that controller, as player 1.
  using InstanceTable = ControllerInstanceTable;
  static bool InstanceMode();
  void OpenInstanceTable();
  void OfferJoin(const std::string& device_key);
  void TakeAssignedController();
  void CloseInstanceTable();
  // Takes the device for this copy if the table allows: nobody else holds it,
  // and (unless it is this copy's own controller coming back) this is the
  // lowest-numbered running copy without a controller.
  bool ClaimForInstance(const std::string& device_key, bool returning);
  void ReleaseInstanceClaim();
  void TakeInstanceController(size_t port);
  std::string DeviceKey(SDL_GameController* controller) const;

  inline uint64_t AnalogToKeyfield(const X_INPUT_GAMEPAD& gamepad) const;
  std::optional<size_t> GetControllerIndexFromInstanceID(
      SDL_JoystickID instance_id);
  ControllerState* GetControllerState(uint32_t user_index);
  bool TestSDLVersion() const;
  void UpdateXCapabilities(ControllerState& state, size_t user_index);
  void QueueControllerUpdate();
  // The kind controller_subtypes asks for in this slot, if it asks for one.
  static std::optional<uint8_t> ForcedSubtypeForSlot(size_t user_index);
  static bool IsGuitarSubtype(uint8_t sub_type);
  // True when guitar_whammy_on_stick routes this slot's trigger to the stick.
  bool WhammyOnStick(size_t user_index) const;

  bool sdl_events_initialized_;
  bool sdl_gamecontroller_initialized_;
  int sdl_events_unflushed_;
  std::atomic<bool> sdl_pumpevents_queued_;
  std::array<ControllerState, HID_SDL_USER_COUNT> controllers_;
  std::array<KeystrokeState, HID_SDL_USER_COUNT> keystroke_states_;
  // Ports whose used controller went away, kept for it to come back to.
  std::array<bool, HID_SDL_USER_COUNT> port_reserved_;
  // controller_instance: the port of this copy's controller, the device it
  // had last (to take back when it reconnects), and the shared table.
  std::optional<size_t> instance_port_;
  std::string instance_last_key_;
  std::chrono::steady_clock::time_point instance_assigned_check_;
  InstanceTable* instance_table_ = nullptr;
  void* instance_mapping_ = nullptr;
  void* instance_mutex_ = nullptr;
};

}  // namespace sdl
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_SDL_SDL_INPUT_DRIVER_H_
