/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Basically360 bridge: the console's own screens (message boxes, the        *
 * achievements list) shown by the Basically360 dashboard when it's running. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_B360_BRIDGE_H_
#define XENIA_KERNEL_XAM_B360_BRIDGE_H_

#include <string>
#include <vector>

namespace xe {
namespace kernel {
namespace xam {
namespace b360 {

// Whether the Basically360 dashboard is running and listening (its pipe,
// \\.\pipe\Basically360-guide, is there).
bool Available();

// Sends one request (fields joined by tabs) and waits for the dashboard's
// answer - as long as the screen it shows is up. False if the dashboard isn't
// there or the pipe broke: the caller falls back to Xenia's own screen or a
// default answer.
bool Ask(const std::vector<std::string>& fields, std::string* reply);

}  // namespace b360
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_B360_BRIDGE_H_
