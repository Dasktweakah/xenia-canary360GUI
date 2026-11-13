/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_CONTROLLER_INSTANCES_H_
#define XENIA_HID_CONTROLLER_INSTANCES_H_

#include <cstddef>
#include <cstdint>

namespace xe {
namespace hid {

// Basically360: the table every copy of Xenia run with controller_instance
// (one copy per player) shares, in named shared memory (Windows).
struct ControllerInstanceTable {
  static constexpr uint32_t kMagic = 0x58434933;  // XCI3
  static constexpr size_t kCount = 4;
  static constexpr size_t kKeySize = 256;
  uint32_t magic;
  // The process running each player's copy, 0 if none.
  uint32_t pid[kCount];
  // Nonzero once the copy has taken a controller (for split_screen: the player
  // has joined), until it closes.
  uint32_t joined[kCount];
  // split_screen: nonzero while player 1's game lets other players join.
  uint32_t join_open;
  // A join asked for, per copy: by the split_screen test key (no controller),
  // or by a running copy that saw a button pressed on a controller nobody has
  // (assigned_key: the copy takes that controller once it runs again).
  uint32_t join_requested[kCount];
  char assigned_key[kCount][kKeySize];
  // The device each copy has taken (empty if none).
  char key[kCount][kKeySize];
};

#define XE_CONTROLLER_INSTANCE_TABLE_NAME L"Local\\XeniaControllerInstances3"
#define XE_CONTROLLER_INSTANCE_LOCK_NAME L"Local\\XeniaControllerInstances3Lock"

}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_CONTROLLER_INSTANCES_H_
