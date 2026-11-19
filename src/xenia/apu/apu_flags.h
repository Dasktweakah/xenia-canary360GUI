/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_APU_APU_FLAGS_H_
#define XENIA_APU_APU_FLAGS_H_

#include <cstddef>

#include "xenia/base/cvar.h"
DECLARE_bool(mute)

    namespace xe {
  namespace apu {
  // Basically360: a gain on everything this copy plays (1: as it is, 0:
  // silent) - split screen silences a copy while it waits hidden for its
  // player and plays players 2-4 a little quieter (split_screen_volume).
  void SetOutputGain(float gain);
  float OutputGain();
  // Scales `count` float samples by OutputGain() (nothing to do at 1).
  void ApplyOutputGain(float* samples, size_t count);
  }  // namespace apu
}  // namespace xe

#endif  // XENIA_APU_APU_FLAGS_H_
