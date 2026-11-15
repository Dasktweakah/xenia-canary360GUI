/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Basically360 LAN Live: copies of Xenia on one home network (or on one PC)  *
 * show each other as Xbox LIVE friends, so a game's "join a friend" works    *
 * between them: who's playing what, the sessions they host (XSESSION_INFO),  *
 * the data a host's QoS listener gives (XNetQosListen), and the host's       *
 * session contexts - all shared over UDP, no Xbox LIVE involved.             *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XAM_LAN_LIVE_H_
#define XENIA_KERNEL_XAM_LAN_LIVE_H_

#include <array>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace xe {
namespace kernel {
namespace xam {
namespace lan_live {

using XnKid = std::array<uint8_t, 8>;
using XnKey = std::array<uint8_t, 16>;
using XnAddr = std::array<uint8_t, 36>;

// A profile signed in on another copy, as a friend.
struct Friend {
  uint64_t xuid = 0;
  std::string gamertag;
  uint32_t title_id = 0;
  XnKid session_id = {};  // the session it hosts or is in (zero: none)
  bool joinable = false;
};

// A session hosted by another copy (or by this one).
struct Session {
  XnKid id = {};
  XnKey key = {};
  XnAddr host = {};
  uint32_t flags = 0;
  uint32_t max_public = 0, max_private = 0;
  uint32_t filled_public = 0, filled_private = 0;
  uint64_t host_xuid = 0;
  std::vector<std::pair<uint32_t, uint32_t>> contexts;  // (id, value)
};

// Whether LAN Live is on (--lan_live, with --xnet_online).
bool Enabled();

// This copy's IPv4 address as XNADDRs give it (host byte order): xnet_address,
// or the PC's own address on the home network.
uint32_t OwnAddress();

// An XNADDR (36 bytes, as a guest sees it) for the copy at address: ina and
// inaOnline the address, port 1000, a MAC address made from the address (so
// each copy's differs).
void FillXnAddr(uint8_t* out, uint32_t address);

// Profiles signed in on other copies playing title_id (0: any).
std::vector<Friend> Friends(uint32_t title_id);

// A session by id, hosted here or by another copy.
bool FindSession(const XnKid& id, Session* out);

// The data a host gives for its session's QoS (XNetQosListen).
bool QosData(const XnKid& id, std::vector<uint8_t>* out);

// This copy hosts a session (from XSessionCreate): session_handle is the
// title's session object, for XSessionModify and XSessionDelete.
void HostSession(uint32_t session_handle, const Session& session);
void ModifySession(uint32_t session_handle, uint32_t flags, uint32_t max_public,
                   uint32_t max_private);
void EndSession(uint32_t session_handle);

// XNetQosListen for a session hosted here: its data (kept while the listener
// is off), whether it answers (ENABLE / DISABLE), and RELEASE.
void SetQosData(const XnKid& id, const std::vector<uint8_t>& data);
void SetQosListening(const XnKid& id, bool listening);
void ClearQos(const XnKid& id);

}  // namespace lan_live
}  // namespace xam
}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XAM_LAN_LIVE_H_
