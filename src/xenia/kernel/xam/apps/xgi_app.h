/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_APPS_XGI_APP_H_
#define XENIA_KERNEL_XAM_APPS_XGI_APP_H_

#include <vector>

#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/app_manager.h"
#include "xenia/kernel/xam/lan_live.h"

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

class XgiApp : public App {
 public:
  explicit XgiApp(KernelState* kernel_state);

  X_HRESULT DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                uint32_t buffer_length) override;

 private:
  uint32_t WriteSearchResults(uint32_t results_ptr, uint32_t results_size,
                              const std::vector<lan_live::Session>& found);
};

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_APPS_XGI_APP_H_
