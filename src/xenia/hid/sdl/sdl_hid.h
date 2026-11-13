/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_HID_SDL_SDL_HID_H_
#define XENIA_HID_SDL_SDL_HID_H_

#include <memory>

#include "xenia/hid/input_system.h"

namespace xe {
namespace hid {
namespace sdl {

std::unique_ptr<InputDriver> Create(xe::ui::Window* window,
                                    size_t window_z_order);

// Basically360: reads the controllers now, in the UI thread (for a copy whose
// game is paused and so doesn't ask for them). Nothing if SDL isn't set up.
void PumpEvents();

}  // namespace sdl
}  // namespace hid
}  // namespace xe

#endif  // XENIA_HID_SDL_SDL_HID_H_
