/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2013 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/cpu/ppc/ppc_frontend.h"

#include <cstdlib>
#include <string>

#include "xenia/base/atomic.h"
#include "xenia/base/logging.h"
#include "xenia/base/mutex.h"
#include "xenia/cpu/cpu_flags.h"
#include "xenia/cpu/ppc/ppc_context.h"
#include "xenia/cpu/ppc/ppc_emit.h"
#include "xenia/cpu/ppc/ppc_opcode_info.h"
#include "xenia/cpu/ppc/ppc_translator.h"
#include "xenia/cpu/processor.h"

namespace xe {
namespace cpu {
namespace ppc {

void InitializeIfNeeded();
void CleanupOnShutdown();

void InitializeIfNeeded() {
  static bool has_initialized = false;
  if (has_initialized) {
    return;
  }
  has_initialized = true;

  RegisterEmitCategoryAltivec();
  RegisterEmitCategoryALU();
  RegisterEmitCategoryControl();
  RegisterEmitCategoryFPU();
  RegisterEmitCategoryMemory();

  atexit(CleanupOnShutdown);
}

void CleanupOnShutdown() {}

PPCFrontend::PPCFrontend(Processor* processor) : processor_(processor) {
  InitializeIfNeeded();
}

PPCFrontend::~PPCFrontend() {
  // Force cleanup now before we deinit.
  translator_pool_.Reset();
}

Memory* PPCFrontend::memory() const { return processor_->memory(); }

// Checks the state of the global lock and sets scratch to the current MSR
// value.
void CheckGlobalLock(PPCContext* ppc_context, void* arg0, void* arg1) {
  auto global_mutex = reinterpret_cast<global_mutex_type*>(arg0);
  auto global_lock_count = reinterpret_cast<int32_t*>(arg1);
  std::lock_guard<global_mutex_type> lock(*global_mutex);
  ppc_context->scratch = *global_lock_count ? 0 : 0x8000;
}

// Enters the global lock. Safe to recursion.
void EnterGlobalLock(PPCContext* ppc_context, void* arg0, void* arg1) {
  auto global_mutex = reinterpret_cast<global_mutex_type*>(arg0);
  auto global_lock_count = reinterpret_cast<int32_t*>(arg1);
  global_mutex->lock();
  xe::atomic_inc(global_lock_count);
}

// Leaves the global lock. Safe to recursion.
void LeaveGlobalLock(PPCContext* ppc_context, void* arg0, void* arg1) {
  auto global_mutex = reinterpret_cast<global_mutex_type*>(arg0);
  auto global_lock_count = reinterpret_cast<int32_t*>(arg1);
  auto new_lock_count = xe::atomic_dec(global_lock_count);
  assert_true(new_lock_count >= 0);
  global_mutex->unlock();
}

void SyscallHandler(PPCContext* ppc_context, void* arg0, void* arg1) {
  uint64_t syscall_number = ppc_context->r[0];
  switch (syscall_number) {
    default:
      assert_unhandled_case(syscall_number);
      XELOGE("Unhandled syscall {}!", syscall_number);
      break;
#pragma warning(suppress : 4065)
  }
}

// Basically360: --trace_guest_addresses.
static std::string TracePointText(Memory* memory, uint32_t value) {
  // Show the text a register points to, if it points to readable guest memory
  // holding printable characters.
  if (value < 0x40000000 || !memory) {
    return {};
  }
  auto heap = memory->LookupHeap(value);
  uint32_t protect = 0;
  if (!heap || !heap->QueryProtect(value, &protect) ||
      !(protect & kMemoryProtectRead)) {
    return {};
  }
  const char* text = memory->TranslateVirtual<const char*>(value);
  std::string out;
  uint32_t page_end = (value | 0xFFF) + 1;
  for (uint32_t i = 0; i < 80 && value + i < page_end; ++i) {
    char c = text[i];
    if (c == 0) {
      break;
    }
    if (c < 0x20 || c > 0x7E) {
      return {};
    }
    out.push_back(c);
  }
  return out.size() >= 2 ? " \"" + out + "\"" : std::string();
}

static void TracePoint(PPCContext* ppc_context, void* arg0, void* arg1) {
  uint32_t address = uint32_t(reinterpret_cast<uintptr_t>(arg0));
  uint32_t r[6];
  std::string text;
  for (int i = 0; i < 6; ++i) {
    r[i] = uint32_t(ppc_context->r[3 + i]);
    std::string s = TracePointText(static_cast<Memory*>(arg1), r[i]);
    if (!s.empty()) {
      text += fmt::format(" r{}={}", 3 + i, s);
    }
  }
  XELOGI(
      "trace {:08X} thread {:X}: r3={:08X} r4={:08X} r5={:08X} r6={:08X} "
      "r7={:08X} r8={:08X} r9={:08X} r10={:08X} r11={:08X} r31={:08X} "
      "ctr={:08X} lr={:08X}{}",
      address, ppc_context->thread_id, r[0], r[1], r[2], r[3], r[4], r[5],
      uint32_t(ppc_context->r[9]), uint32_t(ppc_context->r[10]),
      uint32_t(ppc_context->r[11]), uint32_t(ppc_context->r[31]),
      uint32_t(ppc_context->ctr), uint32_t(ppc_context->lr), text);
}

bool PPCFrontend::Initialize() {
  void* arg0 = reinterpret_cast<void*>(&xe::global_critical_region::mutex());
  void* arg1 = reinterpret_cast<void*>(&builtins_.global_lock_count);
  builtins_.check_global_lock =
      processor_->DefineBuiltin("CheckGlobalLock", CheckGlobalLock, arg0, arg1);
  builtins_.enter_global_lock =
      processor_->DefineBuiltin("EnterGlobalLock", EnterGlobalLock, arg0, arg1);
  builtins_.leave_global_lock =
      processor_->DefineBuiltin("LeaveGlobalLock", LeaveGlobalLock, arg0, arg1);
  builtins_.syscall_handler = processor_->DefineBuiltin(
      "SyscallHandler", SyscallHandler, nullptr, nullptr);
  {
    std::string list = cvars::trace_guest_addresses;
    for (char& c : list) {
      if (c == ',' || c == ';') {
        c = ' ';
      }
    }
    size_t pos = 0;
    while (pos < list.size()) {
      size_t end = list.find(' ', pos);
      if (end == std::string::npos) {
        end = list.size();
      }
      std::string item = list.substr(pos, end - pos);
      pos = end + 1;
      if (item.empty()) {
        continue;
      }
      uint32_t address = uint32_t(std::strtoul(item.c_str(), nullptr, 16));
      if (!address || builtins_.trace_points.count(address)) {
        continue;
      }
      builtins_.trace_points[address] = processor_->DefineBuiltin(
          fmt::format("TracePoint_{:08X}", address), TracePoint,
          reinterpret_cast<void*>(uintptr_t(address)), processor_->memory());
    }
    if (!builtins_.trace_points.empty()) {
      XELOGI("Tracing {} guest addresses", builtins_.trace_points.size());
    }
  }
  return true;
}

bool PPCFrontend::DeclareFunction(GuestFunction* function) {
  // chrispy: make sure we aren't declaring a function that is actually padding
  // data, this will mess up PPCScanner and is hard to debug wow, this halo
  // reach actually has branches into 0 opcodes, look into further
  // xenia_assert(*reinterpret_cast<const uint32_t*>(
  //                  this->memory()->TranslateVirtual(function->address())) !=
  //                  0);
  //  Could scan or something here.
  //  Could also check to see if it's a well-known function type and classify
  //  for later.
  //  Could also kick off a precompiler, since we know it's likely the function
  //  will be demanded soon.
  return true;
}

bool PPCFrontend::DefineFunction(GuestFunction* function,
                                 uint32_t debug_info_flags) {
  auto translator = translator_pool_.Allocate(this);
  bool result = translator->Translate(function, debug_info_flags);
  translator->Reset();
  translator_pool_.Release(translator);
  return result;
}

}  // namespace ppc
}  // namespace cpu
}  // namespace xe
