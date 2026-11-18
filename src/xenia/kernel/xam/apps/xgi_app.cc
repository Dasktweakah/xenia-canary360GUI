/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2021 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/kernel/xam/apps/xgi_app.h"
#include "xenia/kernel/xsession.h"

#include <algorithm>
#include <cstring>

#include <random>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/xam/lan_live.h"
#include "xenia/kernel/xam/xam_state.h"

DECLARE_bool(xnet_online);

namespace xe {
namespace kernel {
namespace xam {
namespace apps {
/*
 * Most of the structs below were found in the Source SDK, provided as stubs.
 * Specifically, they can be found in the Source 2007 SDK and the Alien Swarm
 * Source SDK. Both are available on Steam for free. A GitHub mirror of the
 * Alien Swarm SDK can be found here:
 * https://github.com/NicolasDe/AlienSwarm/blob/master/src/common/xbox/xboxstubs.h
 */

struct X_USER_ACHIEVEMENT {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> achievement_id;
};
static_assert_size(X_USER_ACHIEVEMENT, 0x8);

struct XGI_WRITEACHIEVEMENT {
  xe::be<uint32_t> num_achievements;
  xe::be<uint32_t> achievements_ptr;  // X_USER_ACHIEVEMENT*
};
static_assert_size(XGI_WRITEACHIEVEMENT, 0x8);

struct X_USER_AVATAR_ASSET {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> award_id;
};
static_assert_size(X_USER_AVATAR_ASSET, 0x8);

struct XGI_AWARD_AVATAR_ASSETS {
  xe::be<uint32_t> num_assets;
  xe::be<uint32_t> assets_ptr;  // X_USER_AVATAR_ASSET*
};
static_assert_size(XGI_AWARD_AVATAR_ASSETS, 0x8);

struct XGI_XUSER_GET_PROPERTY {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;  // If xuid is 0 then user_index is used.
  xe::be<uint32_t>
      property_size_ptr;  // Normally filled with sizeof(XUSER_PROPERTY), with
                          // exception of binary and wstring type.
  xe::be<uint32_t> context_address;
  xe::be<uint32_t> property_address;
};
static_assert_size(XGI_XUSER_GET_PROPERTY, 0x20);

struct XGI_XUSER_SET_CONTEXT {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;
  XUSER_CONTEXT context;
};
static_assert_size(XGI_XUSER_SET_CONTEXT, 0x18);

struct XGI_XUSER_SET_PROPERTY {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> unused;
  xe::be<uint64_t> xuid;
  xe::be<uint32_t> property_id;
  xe::be<uint32_t> data_size;
  xe::be<uint32_t> data_address;
};
static_assert_size(XGI_XUSER_SET_PROPERTY, 0x20);

struct XUSER_STATS_VIEW {
  xe::be<uint32_t> ViewId;
  xe::be<uint32_t> TotalViewRows;
  xe::be<uint32_t> NumRows;
  xe::be<uint32_t> pRows;
};

struct XUSER_STATS_COLUMN {
  xe::be<uint16_t> ColumnId;
  X_USER_DATA Value;
};

struct XUSER_STATS_RESET {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> view_id;
};

struct XUSER_ANID {
  xe::be<uint32_t> user_index;
  xe::be<uint32_t> cchAnIdBuffer;
  xe::be<uint32_t> pszAnIdBuffer;
  xe::be<uint32_t> value_const;  // 1
};

XgiApp::XgiApp(KernelState* kernel_state) : App(kernel_state, 0xFB) {}

// http://mb.mirage.org/bugzilla/xliveless/main.c

// Basically360: sessions (lan_live) as an XSESSION_SEARCHRESULT_HEADER (count,
// results) and its XSESSION_SEARCHRESULTs (0x5C) - each its XSESSION_INFO,
// open and filled slots, then (after all the results) the host's number
// properties (XUSER_PROPERTY, 0x18) and contexts (XUSER_CONTEXT, 8). As many
// as fit in results_size; the count is returned.
uint32_t XgiApp::WriteSearchResults(
    uint32_t results_ptr, uint32_t results_size,
    const std::vector<lan_live::Session>& found) {
  constexpr uint32_t kResultSize = 0x5C;
  constexpr uint32_t kPropertySize = 0x18;
  uint8_t* out = memory_->TranslateVirtual(results_ptr);
  std::memset(out, 0, results_size);
  uint32_t fits = 0;
  size_t extra = 0;
  for (const auto& session : found) {
    const size_t more =
        session.properties.size() * kPropertySize + session.contexts.size() * 8;
    if (8 + (fits + 1) * kResultSize + extra + more > results_size) {
      break;
    }
    extra += more;
    ++fits;
  }
  xe::store_and_swap<uint32_t>(out + 0x0, fits);
  xe::store_and_swap<uint32_t>(out + 0x4, fits ? results_ptr + 8 : 0);
  uint32_t extra_at = 8 + fits * kResultSize;
  for (uint32_t i = 0; i < fits; ++i) {
    const auto& session = found[i];
    uint8_t* r = out + 8 + i * kResultSize;
    std::memcpy(r + 0x00, session.id.data(), 8);
    std::memcpy(r + 0x08, session.host.data(), 36);
    std::memcpy(r + 0x2C, session.key.data(), 16);
    const uint32_t filled_public = std::max<uint32_t>(session.filled_public, 1);
    xe::store_and_swap<uint32_t>(r + 0x3C,
                                 session.max_public > filled_public
                                     ? session.max_public - filled_public
                                     : 0);
    xe::store_and_swap<uint32_t>(
        r + 0x40, session.max_private > session.filled_private
                      ? session.max_private - session.filled_private
                      : 0);
    xe::store_and_swap<uint32_t>(r + 0x44, filled_public);
    xe::store_and_swap<uint32_t>(r + 0x48, session.filled_private);
    xe::store_and_swap<uint32_t>(r + 0x4C, uint32_t(session.properties.size()));
    xe::store_and_swap<uint32_t>(r + 0x50, uint32_t(session.contexts.size()));
    xe::store_and_swap<uint32_t>(
        r + 0x54, session.properties.empty() ? 0 : results_ptr + extra_at);
    for (const auto& [property_id, value] : session.properties) {
      uint8_t* p = out + extra_at;
      xe::store_and_swap<uint32_t>(p + 0x0, property_id);
      p[0x8] = 1;  // XUSER_DATA_TYPE_INT32
      xe::store_and_swap<uint32_t>(p + 0x10, value);
      extra_at += kPropertySize;
    }
    xe::store_and_swap<uint32_t>(
        r + 0x58, session.contexts.empty() ? 0 : results_ptr + extra_at);
    for (const auto& [context_id, value] : session.contexts) {
      xe::store_and_swap<uint32_t>(out + extra_at, context_id);
      xe::store_and_swap<uint32_t>(out + extra_at + 4, value);
      extra_at += 8;
    }
  }
  return fits;
}

X_HRESULT XgiApp::DispatchMessageSync(uint32_t message, uint32_t buffer_ptr,
                                      uint32_t buffer_length) {
  // NOTE: buffer_length may be zero or valid.
  auto buffer = memory_->TranslateVirtual(buffer_ptr);
  switch (message) {
    case 0x000B0006: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_XUSER_SET_CONTEXT));
      const XGI_XUSER_SET_CONTEXT* xgi_context =
          reinterpret_cast<const XGI_XUSER_SET_CONTEXT*>(buffer);

      XELOGD("XGIUserSetContext({:08X}, ID: {:08X}, Value: {:08X})",
             xgi_context->user_index.get(),
             xgi_context->context.context_id.get(),
             xgi_context->context.value.get());

      UserProfile* user = nullptr;
      if (xgi_context->xuid != 0) {
        user = kernel_state_->xam_state()->GetUserProfile(xgi_context->xuid);
      } else {
        user =
            kernel_state_->xam_state()->GetUserProfile(xgi_context->user_index);
      }

      if (user) {
        kernel_state_->xam_state()->user_tracker()->UpdateContext(
            user->xuid(), xgi_context->context.context_id,
            xgi_context->context.value);
        // Basically360: a session this copy hosts for the profile takes it.
        lan_live::HostAttribute(user->xuid(), xgi_context->context.context_id,
                                xgi_context->context.value, true);
      }
      return X_E_SUCCESS;
    }
    case 0x000B0007: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_XUSER_SET_PROPERTY));
      const XGI_XUSER_SET_PROPERTY* xgi_property =
          reinterpret_cast<const XGI_XUSER_SET_PROPERTY*>(buffer);

      XELOGD("XGIUserSetPropertyEx({:08X}, {:08X}, {}, {:08X})",
             xgi_property->user_index.get(), xgi_property->property_id.get(),
             xgi_property->data_size.get(), xgi_property->data_address.get());

      UserProfile* user = nullptr;
      if (xgi_property->xuid != 0) {
        user = kernel_state_->xam_state()->GetUserProfile(xgi_property->xuid);
      } else {
        user = kernel_state_->xam_state()->GetUserProfile(
            xgi_property->user_index);
      }

      if (user) {
        Property property(
            xgi_property->property_id,
            Property::get_valid_data_size(xgi_property->property_id,
                                          xgi_property->data_size),
            memory_->TranslateVirtual<uint8_t*>(xgi_property->data_address));

        kernel_state_->xam_state()->user_tracker()->AddProperty(user->xuid(),
                                                                &property);
        // Basically360: a 32-bit number property goes with a session this
        // copy hosts for the profile (matchmaking).
        if ((xgi_property->property_id >> 28) == 1 &&
            xgi_property->data_address) {
          lan_live::HostAttribute(
              user->xuid(), xgi_property->property_id,
              xe::load_and_swap<uint32_t>(
                  memory_->TranslateVirtual(xgi_property->data_address)),
              false);
        }
      }
      return X_E_SUCCESS;
    }
    case 0x000B0008: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(X_USER_ACHIEVEMENT));

      const XGI_WRITEACHIEVEMENT* write_achievements =
          reinterpret_cast<const XGI_WRITEACHIEVEMENT*>(buffer);

      const X_USER_ACHIEVEMENT* achievements =
          memory_->TranslateVirtual<X_USER_ACHIEVEMENT*>(
              write_achievements->achievements_ptr);

      XELOGD("XGIUserWriteAchievements({:08X}, {:08X})",
             write_achievements->num_achievements.get(),
             write_achievements->achievements_ptr.get());

      for (uint32_t i = 0; i < write_achievements->num_achievements; i++) {
        const X_USER_ACHIEVEMENT& achievement = achievements[i];

        kernel_state_->achievement_manager()->EarnAchievement(
            achievement.user_index, kernel_state_->title_id(),
            achievement.achievement_id);
      }

      return X_E_SUCCESS;
    }
    case 0x000B0010: {
      XELOGD("XSessionCreate({:08X}, {:08X}), implemented in netplay",
             buffer_ptr, buffer_length);
      assert_true(!buffer_length || buffer_length == 28);
      // Sequence:
      // - XamSessionCreateHandle
      // - XamSessionRefObjByHandle
      // - [this]
      // - CloseHandle
      uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t flags = xe::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t num_slots_public = xe::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t num_slots_private = xe::load_and_swap<uint32_t>(buffer + 0xC);
      uint32_t user_xuid = xe::load_and_swap<uint32_t>(buffer + 0x10);
      uint32_t session_info_ptr = xe::load_and_swap<uint32_t>(buffer + 0x14);
      uint32_t nonce_ptr = xe::load_and_swap<uint32_t>(buffer + 0x18);

      XELOGD(
          "XGISessionCreateImpl({:08X}, {:08X}, {}, {}, {:08X}, {:08X}, "
          "{:08X})",
          session_ptr, flags, num_slots_public, num_slots_private, user_xuid,
          session_info_ptr, nonce_ptr);

      // 584107FB expects offline session creation using flags 0 to succeed
      // while offline.
      // 58410889 expects stats session creation failure while offline.
      //
      // Allow offline session creation, but do not allow Xbox Live featured
      // session creation.

      if (IsXboxLiveSession(static_cast<SessionFlags>(flags))) {
        if (!cvars::xnet_online) {
          return 0x80155209;  // X_ONLINE_E_SESSION_NOT_LOGGED_ON
        }
        // Basically360 (xnet_online): a title's own online service (for
        // example EA Blaze, served locally) hosts the game, so an Xbox Live
        // session is created here as on a console: a host gets a new
        // XSESSION_INFO (an online-peer session id, its own XNADDR - as
        // XNetGetTitleXnAddr gives it - and a key) and nonce. A joining
        // console passes the host's XSESSION_INFO in and gets it back as is.
        std::random_device random;
        auto fill_random = [&random](uint8_t* out, size_t length) {
          for (size_t i = 0; i < length; ++i) {
            out[i] = static_cast<uint8_t>(random());
          }
        };
        if ((flags & SessionFlags::HOST) && session_info_ptr) {
          uint8_t* info = memory_->TranslateVirtual<uint8_t*>(session_info_ptr);
          // XNKID sessionID (8)
          fill_random(info, 8);
          info[0] = 0x80 | (info[0] & 0x0F);  // XNET_XNKID_ONLINE_PEER
          // XNADDR hostAddress (36): ina, inaOnline, wPortOnline, abEnet,
          // abOnline
          uint8_t* host = info + 8;
          if (lan_live::Enabled()) {
            // LAN Live: this copy, as the other copies reach it.
            lan_live::FillXnAddr(host, lan_live::OwnAddress());
          } else {
            std::memset(host, 0, 36);
            host[0] = 127;
            host[3] = 1;
            host[4] = 127;
            host[7] = 1;
            host[8] = 1000 >> 8;
            host[9] = 1000 & 0xFF;
            std::memset(host + 10, 0xCC, 6);
          }
          // XNKEY keyExchangeKey (16)
          fill_random(info + 44, 16);
          if (lan_live::Enabled()) {
            // Shared with the other copies, which see the host's profiles
            // as friends in it (Minecraft's Join tab).
            lan_live::Session hosted;
            std::memcpy(hosted.id.data(), info, 8);
            std::memcpy(hosted.host.data(), info + 8, 36);
            std::memcpy(hosted.key.data(), info + 44, 16);
            hosted.flags = flags;
            hosted.max_public = num_slots_public;
            hosted.max_private = num_slots_private;
            auto xam = kernel_state_->xam_state();
            UserProfile* user =
                user_xuid < 4 ? xam->GetUserProfile(user_xuid) : nullptr;
            for (uint32_t i = 0; !user && i < 4; ++i) {
              user = xam->GetUserProfile(i);
            }
            hosted.host_xuid = user ? user->xuid() : 0;
            if (user) {
              // The host's contexts and number properties - what other
              // copies' matchmaking searches look at.
              auto tracker = xam->user_tracker();
              for (const auto& key : tracker->GetUserContextIds(user->xuid())) {
                if (auto value =
                        tracker->GetUserContext(user->xuid(), key.value)) {
                  hosted.contexts.emplace_back(key.value, *value);
                }
              }
              for (const auto& key :
                   tracker->GetUserPropertyIds(user->xuid())) {
                const Property* property =
                    tracker->GetProperty(user->xuid(), key.value);
                if (!property || (key.value >> 28) != 1) {
                  continue;
                }
                const auto data = property->get_host_data();
                if (const auto* v = std::get_if<int32_t>(&data)) {
                  hosted.properties.emplace_back(key.value, uint32_t(*v));
                } else if (const auto* u = std::get_if<uint32_t>(&data)) {
                  hosted.properties.emplace_back(key.value, *u);
                }
              }
            }
            lan_live::HostSession(session_ptr, hosted);
          }
        }
        if ((flags & SessionFlags::HOST) && nonce_ptr) {
          fill_random(memory_->TranslateVirtual<uint8_t*>(nonce_ptr), 8);
        }
        XELOGI("XSessionCreate: Xbox Live session (flags {:08X}) {}", flags,
               (flags & SessionFlags::HOST) ? "hosted" : "joined");
      }

      return X_E_SUCCESS;
    }
    case 0x000B0011: {
      XELOGD("XGISessionDelete({:08X}, {:08X}), implemented in netplay",
             buffer_ptr, buffer_length);
      if (buffer_length >= 4) {
        lan_live::EndSession(xe::load_and_swap<uint32_t>(buffer + 0x0));
      }
      return X_STATUS_SUCCESS;
    }
    case 0x000B0012: {
      assert_true(buffer_length == 0x14);
      uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
      uint32_t user_count = xe::load_and_swap<uint32_t>(buffer + 0x4);
      uint32_t unk_0 = xe::load_and_swap<uint32_t>(buffer + 0x8);
      uint32_t user_index_array = xe::load_and_swap<uint32_t>(buffer + 0xC);
      uint32_t private_slots_array = xe::load_and_swap<uint32_t>(buffer + 0x10);

      assert_zero(unk_0);
      XELOGD("XGISessionJoinLocal({:08X}, {}, {}, {:08X}, {:08X})", session_ptr,
             user_count, unk_0, user_index_array, private_slots_array);
      return X_E_SUCCESS;
    }
    case 0x000B0018: {
      // XSessionModify(session, flags, public slots, private slots).
      if (buffer_length >= 0x10) {
        const uint32_t session_ptr = xe::load_and_swap<uint32_t>(buffer + 0x0);
        const uint32_t flags = xe::load_and_swap<uint32_t>(buffer + 0x4);
        const uint32_t max_public = xe::load_and_swap<uint32_t>(buffer + 0x8);
        const uint32_t max_private = xe::load_and_swap<uint32_t>(buffer + 0xC);
        XELOGI("XSessionModify({:08X}, flags {:08X}, {} public, {} private)",
               session_ptr, flags, max_public, max_private);
        lan_live::ModifySession(session_ptr, flags, max_public, max_private);
      }
      return X_E_SUCCESS;
    }
    case 0x000B001B:
    case 0x000B0060: {
      // XSessionSearchByID(user, session id, results) and
      // XSessionSearchByIds(user, count, ids, results): Basically360
      // (lan_live) finds the sessions hosted by the other copies on this home
      // network; nothing else is found.
      if (buffer_length < 0x14) {
        return X_E_INVALIDARG;
      }
      std::vector<lan_live::XnKid> ids;
      if (message == 0x000B001B) {
        lan_live::XnKid kid;
        std::memcpy(kid.data(), buffer + 0x4, 8);
        ids.push_back(kid);
      } else {
        const uint32_t count = xe::load_and_swap<uint32_t>(buffer + 0x4);
        const uint32_t ids_ptr = xe::load_and_swap<uint32_t>(buffer + 0x8);
        for (uint32_t i = 0; ids_ptr && i < std::min(count, 100u); ++i) {
          lan_live::XnKid kid;
          std::memcpy(kid.data(), memory_->TranslateVirtual(ids_ptr + i * 8),
                      8);
          ids.push_back(kid);
        }
      }
      uint32_t results_size = xe::load_and_swap<uint32_t>(buffer + 0xC);
      if (results_size >= 0x10000000) {  // a pointer to it
        results_size = xe::load_and_swap<uint32_t>(
            memory_->TranslateVirtual(results_size));
      }
      const uint32_t results_ptr = xe::load_and_swap<uint32_t>(buffer + 0x10);
      if (!results_ptr || results_size < 8) {
        return X_E_INVALIDARG;
      }
      std::vector<lan_live::Session> found;
      for (const auto& kid : ids) {
        lan_live::Session session;
        if (lan_live::FindSession(kid, &session)) {
          found.push_back(session);
        }
      }
      const uint32_t fits =
          WriteSearchResults(results_ptr, results_size, found);
      XELOGI("XSessionSearchByIds({} id(s)) -> {} session(s) found", ids.size(),
             fits);
      return X_E_SUCCESS;
    }
    case 0x000B001C: {
      // XSessionSearch(procedure, user, max results, property count, context
      // count, properties, contexts, results size, results): matchmaking.
      // Basically360 (lan_live): the sessions the other copies host for this
      // title that fit (lan_live::Search); without LAN Live, nothing.
      {
        std::string dump;
        for (uint32_t i = 0; i < std::min<uint32_t>(buffer_length, 0x40); ++i) {
          dump += fmt::format("{:02X}", buffer[i]);
        }
        XELOGI("XSessionSearch buffer ({} bytes): {}", buffer_length, dump);
      }
      if (!lan_live::Enabled() || buffer_length < 0x20) {
        XELOGI("XSessionSearch({:08X}, {:08X}) -> no sessions", buffer_ptr,
               buffer_length);
        return X_E_FAIL;
      }
      const uint32_t procedure = xe::load_and_swap<uint32_t>(buffer + 0x0);
      const uint32_t max_results = xe::load_and_swap<uint32_t>(buffer + 0x8);
      const uint32_t num_props = xe::load_and_swap<uint16_t>(buffer + 0xC);
      const uint32_t num_ctx = xe::load_and_swap<uint16_t>(buffer + 0xE);
      const uint32_t props_ptr = xe::load_and_swap<uint32_t>(buffer + 0x10);
      const uint32_t ctx_ptr = xe::load_and_swap<uint32_t>(buffer + 0x14);
      const uint32_t size_field = xe::load_and_swap<uint32_t>(buffer + 0x18);
      const uint32_t results_ptr = xe::load_and_swap<uint32_t>(buffer + 0x1C);
      std::vector<std::pair<uint32_t, uint32_t>> contexts, properties;
      for (uint32_t i = 0; ctx_ptr && i < std::min(num_ctx, 64u); ++i) {
        const uint8_t* c = memory_->TranslateVirtual(ctx_ptr + i * 8);
        contexts.emplace_back(xe::load_and_swap<uint32_t>(c),
                              xe::load_and_swap<uint32_t>(c + 4));
      }
      for (uint32_t i = 0; props_ptr && i < std::min(num_props, 64u); ++i) {
        const uint8_t* p = memory_->TranslateVirtual(props_ptr + i * 0x18);
        const uint32_t id = xe::load_and_swap<uint32_t>(p);
        if (p[0x8] == 1) {  // XUSER_DATA_TYPE_INT32
          properties.emplace_back(id, xe::load_and_swap<uint32_t>(p + 0x10));
        }
      }
      for (const auto& [id, value] : contexts) {
        XELOGI("XSessionSearch:   context {:08X} = {}", id, value);
      }
      for (const auto& [id, value] : properties) {
        XELOGI("XSessionSearch:   property {:08X} = {}", id, value);
      }
      // The results' size: the value, or (as XSessionSearch takes it) a
      // pointer to it.
      uint32_t results_size = size_field;
      uint32_t* size_out = nullptr;
      if (size_field >= 0x10000000) {
        size_out = memory_->TranslateVirtual<uint32_t*>(size_field);
        results_size = xe::byte_swap(*size_out);
      }
      const auto found =
          lan_live::Search(contexts, properties,
                           std::max<uint32_t>(1, std::min(max_results, 50u)));
      if (!results_ptr || results_size < 8) {
        // Asking how big a buffer the results need.
        uint32_t need = 8;
        for (const auto& session : found) {
          need += 0x5C + uint32_t(session.properties.size()) * 0x18 +
                  uint32_t(session.contexts.size()) * 8;
        }
        if (size_out) {
          *size_out = xe::byte_swap(
              std::max(need, 8u + 0x5C * std::max(1u, max_results)));
        }
        XELOGI("XSessionSearch(procedure {}) -> needs {} bytes", procedure,
               need);
        return X_E_INSUFFICIENT_BUFFER;
      }
      const uint32_t fits =
          WriteSearchResults(results_ptr, results_size, found);
      XELOGI(
          "XSessionSearch(procedure {}, up to {}, {} context(s), {} "
          "propert(ies)) -> {} session(s) found",
          procedure, max_results, contexts.size(), properties.size(), fits);
      return X_E_SUCCESS;
    }
    case 0x000B0014: {
      // Gets 584107FB in game.
      // get high score table?
      XELOGD("XSessionStart({:08X}), implemented in netplay", buffer_ptr);
      return X_STATUS_SUCCESS;
    }
    case 0x000B0015: {
      // send high scores?
      XELOGD("XSessionEnd({:08X}, {:08X}), implemented in netplay", buffer_ptr,
             buffer_length);
      return X_STATUS_SUCCESS;
    }
    case 0x000B0021: {
      XELOGD("XUserReadStats");

      struct XUserReadStats {
        xe::be<uint32_t> titleId;
        xe::be<uint32_t> xuids_count;
        xe::be<uint32_t> xuids_guest_address;
        xe::be<uint32_t> specs_count;
        xe::be<uint32_t> specs_guest_address;
        xe::be<uint32_t> results_size;
        xe::be<uint32_t> results_guest_address;
      }* data = reinterpret_cast<XUserReadStats*>(buffer);

      return 0x80151802;  // X_ONLINE_E_LOGON_NOT_LOGGED_ON
    }
    case 0x000B0036: {
      // Called after opening xbox live arcade and clicking on xbox live v5759
      // to 5787 and called after clicking xbox live in the game library from
      // v6683 to v6717
      // Does not get sent a buffer
      XELOGD("XInvalidateGamerTileCache, unimplemented");
      return X_E_FAIL;
    }
    case 0x000B003D: {
      // Used in 5451082A, 5553081E
      // XUserGetCachedANID
      XELOGI("XUserGetANID({:08X}, {:08X}), implemented in netplay", buffer_ptr,
             buffer_length);
      return X_E_FAIL;
    }
    case 0x000B0041: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_XUSER_GET_PROPERTY));
      const XGI_XUSER_GET_PROPERTY* xgi_property =
          reinterpret_cast<const XGI_XUSER_GET_PROPERTY*>(buffer);

      UserProfile* user = nullptr;
      if (xgi_property->xuid != 0) {
        user = kernel_state_->xam_state()->GetUserProfile(xgi_property->xuid);
      } else {
        user = kernel_state_->xam_state()->GetUserProfile(
            xgi_property->user_index);
      }

      if (!user) {
        XELOGD(
            "XGIUserGetProperty - Invalid user provided: Index: {:08X} XUID: "
            "{:16X}",
            xgi_property->user_index.get(), xgi_property->xuid.get());
        return X_E_NOTFOUND;
      }

      // Process context
      if (xgi_property->context_address) {
        XUSER_CONTEXT* context = memory_->TranslateVirtual<XUSER_CONTEXT*>(
            xgi_property->context_address);

        XELOGD("XGIUserGetProperty - Context requested: {:08X} XUID: {:16X}",
               context->context_id.get(), user->xuid());

        auto context_value =
            kernel_state_->xam_state()->user_tracker()->GetUserContext(
                user->xuid(), context->context_id);

        if (!context_value) {
          return X_E_INVALIDARG;
        }

        context->value = context_value.value();
        return X_E_SUCCESS;
      }

      if (!xgi_property->property_size_ptr || !xgi_property->property_address) {
        return X_E_INVALIDARG;
      }

      // Process property
      XUSER_PROPERTY* property = memory_->TranslateVirtual<XUSER_PROPERTY*>(
          xgi_property->property_address);

      XELOGD("XGIUserGetProperty - Property requested: {:08X} XUID: {:16X}",
             property->property_id.get(), user->xuid());

      return kernel_state_->xam_state()->user_tracker()->GetProperty(
          user->xuid(),
          memory_->TranslateVirtual<uint32_t*>(xgi_property->property_size_ptr),
          property);
    }
    case 0x000B0071: {
      assert_true(!buffer_length ||
                  buffer_length == sizeof(XGI_AWARD_AVATAR_ASSETS));
      const XGI_AWARD_AVATAR_ASSETS* award_avatar_assets =
          reinterpret_cast<const XGI_AWARD_AVATAR_ASSETS*>(buffer);

      XELOGD("XUserAwardAvatarAssets({:08X}, {:08X})",
             award_avatar_assets->num_assets.get(),
             award_avatar_assets->assets_ptr.get());

      const X_USER_AVATAR_ASSET* avatar_assets =
          memory_->TranslateVirtual<X_USER_AVATAR_ASSET*>(
              award_avatar_assets->assets_ptr);

      for (uint32_t i = 0; i < award_avatar_assets->num_assets; i++) {
        const X_USER_AVATAR_ASSET& avatar_asset = avatar_assets[i];

        const auto user =
            kernel_state_->xam_state()->GetUserProfile(avatar_asset.user_index);

        if (user) {
          XELOGI("Player: {} Unlocked Avatar Award Asset ID: {}", user->name(),
                 avatar_asset.award_id.get());
          // Basically360: kept in the title's GPD, for the Guide's Awards.
          kernel_state_->xam_state()->user_tracker()->AwardAvatarAsset(
              user->xuid(), avatar_asset.award_id);
        }
      }

      return X_E_SUCCESS;
    }
  }
  XELOGE(
      "Unimplemented XGI message app={:08X}, msg={:08X}, arg1={:08X}, "
      "arg2={:08X}",
      app_id(), message, buffer_ptr, buffer_length);
  return X_E_FAIL;
}

}  // namespace apps
}  // namespace xam
}  // namespace kernel
}  // namespace xe
