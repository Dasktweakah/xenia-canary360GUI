/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xlivebase_app.h"

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/xenumerator.h"

DECLARE_bool(xnet_online);

namespace xe {
namespace kernel {
namespace xam {
namespace apps {

XLiveBaseApp::XLiveBaseApp(KernelState* kernel_state)
    : App(kernel_state, 0xFC) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

X_HRESULT XLiveBaseApp::DispatchMessageSync(uint32_t message,
                                            uint32_t buffer_ptr,
                                            uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x0005008C: {
      // Called on startup of blades dashboard v1888 to v2858
      XELOGD("XLiveBaseUnk5008C, unimplemented");
      return X_E_FAIL;
    }
    case 0x00050094: {
      // Called on startup of blades dashboard v4532 to v4552
      XELOGD("XLiveBaseUnk50094, unimplemented");
      return X_E_FAIL;
    }
    case 0x00058003: {
      /* Notes:
         - Called on startup of dashboard (netplay build)
         - used by other internet funtions to check if online (e.g.
         XamGetLiveHiveValueA)
         - Return is Saved elsewhere and used here
      */
      XELOGD("XLiveBaseLogonGetHR, implemented in netplay");
      if (cvars::xnet_online) {
        // Basically360: the title's own servers are reachable (xnet_online).
        return 0x001510F0;  // X_ONLINE_S_LOGON_CONNECTION_ESTABLISHED
      }
      return 0x001510F1;  // X_ONLINE_S_LOGON_DISCONNECTED
    }
    case 0x00058004: {
      /* Notes:
         - Called on startup, seems to just return a bool in the buffer.
         - It is Saved elsewhere and used here
      */
      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetLogonId({:08X})", buffer_ptr);
      xe::store_and_swap<uint32_t>(buffer, 1);  // ?
      return X_E_SUCCESS;
    }
    case 0x00058006: {
      // Buffer only set when online
      assert_true(!buffer_length || buffer_length == 4);
      XELOGD("XLiveBaseGetNatType({:08X})", buffer_ptr);
      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x00058007: {
      // Occurs if title calls XOnlineGetServiceInfo, expects dwServiceId
      // and pServiceInfo. pServiceInfo should contain pointer to
      // XONLINE_SERVICE_INFO structure.
      XELOGD("XLiveBaseOnlineGetServiceInfo({:08X}, {:08X})", buffer_ptr,
             buffer_length);
      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x00058020: {
      // XFriendsCreateEnumerator(user_index, starting_index,
      // friends_to_return, *buffer_size, *enum_handle). The XDK's wrapper
      // marshals its arguments into buffer_length: 16-byte entries of
      // {be u32 size, u32 pad, be u64 address of the argument} with the entry
      // count at +0x200; the first three addresses point at copies of the
      // values, the last two are the caller's out pointers (seen in Plants vs.
      // Zombies Garden Warfare, 0x882E4CA8, whose EA network library retries
      // forever while this fails). Answer with an empty friends list: a valid
      // enumerator that XamEnumerate reports as ERROR_NO_MORE_FILES.
      if (!buffer_length) {
        return X_E_INVALIDARG;
      }
      auto args = memory_->TranslateVirtual<uint8_t*>(buffer_length);
      uint32_t arg_count = xe::load_and_swap<uint32_t>(args + 0x200);
      if (arg_count < 5) {
        XELOGW("XFriendsCreateEnumerator: {} marshalled arguments", arg_count);
        return X_E_INVALIDARG;
      }
      auto arg_address = [&](uint32_t i) {
        return uint32_t(xe::load_and_swap<uint64_t>(args + i * 16 + 8));
      };
      auto arg_value = [&](uint32_t i) {
        uint32_t address = arg_address(i);
        return address ? xe::load_and_swap<uint32_t>(
                             memory_->TranslateVirtual(address))
                       : 0u;
      };
      const uint32_t user_index = arg_value(0);
      const uint32_t starting_index = arg_value(1);
      uint32_t friends_to_return = std::max(arg_value(2), 1u);
      const uint32_t buffer_size_ptr = arg_address(3);
      const uint32_t handle_ptr = arg_address(4);
      // XONLINE_FRIEND: XUID, gamertag[16], state, session id, title id, user
      // time, invite id, invite time, rich presence length, wchar[64].
      constexpr uint32_t kFriendSize = 0xC4;
      auto e = make_object<XStaticUntypedEnumerator>(
          kernel_state_, friends_to_return, kFriendSize);
      auto result = e->Initialize(user_index, 0xFC, 0x58021, 0x58022, 0);
      if (XFAILED(result)) {
        return X_E_FAIL;
      }
      if (buffer_size_ptr) {
        xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(buffer_size_ptr),
                                     kFriendSize * friends_to_return);
      }
      if (handle_ptr) {
        xe::store_and_swap<uint32_t>(memory_->TranslateVirtual(handle_ptr),
                                     e->handle());
      }
      XELOGD(
          "XFriendsCreateEnumerator(user {}, start {}, count {}) -> empty "
          "list, handle {:08X}",
          user_index, starting_index, friends_to_return, e->handle());
      return X_E_SUCCESS;
    }
    case 0x00058023: {
      XELOGD(
          "CXLiveMessaging::XMessageGameInviteGetAcceptedInfo({:08X}, {:08X}) "
          "unimplemented",
          buffer_ptr, buffer_length);
      return X_E_FAIL;
    }
    case 0x00058037: {
      XELOGD("XPresenceInitialize({:08X}, {:08X})", buffer_ptr, buffer_length);
      return X_E_SUCCESS;
    }
    case 0x00058046: {
      // Required to be successful for 4D530910 to detect signed-in profile
      // Doesn't seem to set anything in the given buffer, probably only takes
      // input
      XELOGD("XLiveBaseUnk58046({:08X}, {:08X}) unimplemented", buffer_ptr,
             buffer_length);
      return X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XLIVEBASE message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
