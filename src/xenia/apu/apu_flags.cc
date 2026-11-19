/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/apu/apu_flags.h"

#include <atomic>
#include <cstring>

DEFINE_bool(mute, false, "Mutes all audio output.", "APU")

    namespace xe {
  namespace apu {
  static std::atomic<float> output_gain_{1.0f};
  void SetOutputGain(float gain) {
    output_gain_.store(gain < 0.0f ? 0.0f : (gain > 1.0f ? 1.0f : gain));
  }
  float OutputGain() { return output_gain_.load(); }
  void ApplyOutputGain(float* samples, size_t count) {
    const float gain = OutputGain();
    if (gain >= 0.999f) {
      return;
    }
    if (gain <= 0.0f) {
      std::memset(samples, 0, count * sizeof(float));
      return;
    }
    for (size_t i = 0; i < count; ++i) {
      samples[i] *= gain;
    }
  }
  }  // namespace apu
}  // namespace xe
