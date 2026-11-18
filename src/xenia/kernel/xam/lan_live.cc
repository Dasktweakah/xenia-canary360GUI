/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Basically360 LAN Live - see lan_live.h.                                    *
 *                                                                            *
 * Each copy says, about once a second, over UDP (port lan_live_port) to the  *
 * home network's broadcast address and to any lan_live_peers:                *
 *   XLAN 1                                                                   *
 *   id <this copy's random id>                                               *
 *   title <title id>                                                         *
 *   user <xuid> <gamertag>                (each profile signed in here)      *
 *   session <id> <key> <host XNADDR> <flags> <max public> <max private>      *
 *           <filled public> <filled private> <host xuid>                     *
 *   ctx <session id> <context id> <value> (the host's contexts)              *
 *   prop <session id> <property id> <value> (its number properties)         *
 *   qos <session id> <data>               (XNetQosListen's data)             *
 * (numbers and bytes in hex). What the others say is kept for 6 seconds.     *
 ******************************************************************************
 */

#include "xenia/kernel/xam/lan_live.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/platform.h"
#include "xenia/kernel/kernel_state.h"
#include "xenia/kernel/util/shim_utils.h"
#include "xenia/kernel/xam/xam_state.h"
#include "xenia/kernel/xsocket.h"

#ifdef XE_PLATFORM_WIN32
#include "xenia/base/platform_win.h"
// NOTE: must be included after windows.h.
#include <winsock2.h>  // NOLINT(build/include_order)
#include <ws2tcpip.h>  // NOLINT(build/include_order)
using lan_socket_t = SOCKET;
static const lan_socket_t kNoSocket = INVALID_SOCKET;
static void CloseLanSocket(lan_socket_t s) { closesocket(s); }
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
using lan_socket_t = int;
static const lan_socket_t kNoSocket = -1;
static void CloseLanSocket(lan_socket_t s) { close(s); }
#endif

DECLARE_bool(xnet_online);

DEFINE_bool(lan_live, false,
            "Basically360: copies of Xenia on this home network (or on this "
            "PC) are each other's Xbox LIVE friends, so a game can join a "
            "friend's game between them (Minecraft: Xbox 360 Edition's Join "
            "tab). Needs xnet_online and xam_signin_live; xnet_address should "
            "be this PC's address on the home network (or a different "
            "127.0.0.x per copy on one PC, with lan_live_peers).",
            "Live");
DEFINE_int32(lan_live_port, 36071,
             "Basically360: the UDP port LAN Live copies talk on.", "Live");
DEFINE_string(lan_live_peers, "",
              "Basically360: addresses LAN Live also talks to directly, "
              "separated by commas - for copies on this PC (127.0.0.1, "
              "127.0.0.2...) or a network that blocks broadcasts.",
              "Live");

namespace xe {
namespace kernel {
namespace xam {
namespace lan_live {

namespace {

struct Peer {
  uint32_t title_id = 0;
  std::vector<std::pair<uint64_t, std::string>> users;
  std::vector<Session> sessions;
  std::map<XnKid, std::vector<uint8_t>> qos;
  std::chrono::steady_clock::time_point seen;
};

struct HostedSession {
  Session session;
};

std::mutex mutex_;
std::once_flag started_;
uint64_t instance_id_ = 0;
std::map<uint64_t, Peer> peers_;            // by instance id
std::map<uint32_t, HostedSession> hosted_;  // by session handle
struct QosListener {
  std::vector<uint8_t> data;
  bool listening = false;
};
std::map<XnKid, QosListener> qos_;  // ours (XNetQosListen), by session id

std::string Hex(const uint8_t* p, size_t n) {
  static const char digits[] = "0123456789ABCDEF";
  std::string s;
  s.reserve(n * 2);
  for (size_t i = 0; i < n; ++i) {
    s += digits[p[i] >> 4];
    s += digits[p[i] & 15];
  }
  return s;
}

bool Unhex(const std::string& s, uint8_t* out, size_t n) {
  if (s.size() != n * 2) {
    return false;
  }
  auto v = [](char c) -> int {
    if (c >= '0' && c <= '9') {
      return c - '0';
    }
    if (c >= 'A' && c <= 'F') {
      return c - 'A' + 10;
    }
    if (c >= 'a' && c <= 'f') {
      return c - 'a' + 10;
    }
    return -1;
  };
  for (size_t i = 0; i < n; ++i) {
    int hi = v(s[i * 2]), lo = v(s[i * 2 + 1]);
    if (hi < 0 || lo < 0) {
      return false;
    }
    out[i] = uint8_t(hi << 4 | lo);
  }
  return true;
}

std::vector<uint8_t> UnhexVector(const std::string& s) {
  std::vector<uint8_t> out(s.size() / 2);
  if (!Unhex(s, out.data(), out.size())) {
    out.clear();
  }
  return out;
}

uint32_t ParseIPv4(const std::string& text) {
  in_addr a = {};
  if (inet_pton(AF_INET, text.c_str(), &a) != 1) {
    return 0;
  }
  return ntohl(a.s_addr);
}

// The message about this copy.
std::string Describe() {
  std::ostringstream m;
  m << "XLAN 1\n";
  m << "id " << std::hex << instance_id_ << "\n";
  m << "title " << std::hex << kernel_state()->title_id() << "\n";
  auto xam = kernel_state()->xam_state();
  for (uint32_t i = 0; i < 4; ++i) {
    if (auto user = xam->GetUserProfile(i)) {
      m << "user " << std::hex << user->xuid() << " " << user->name() << "\n";
    }
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [handle, hosted] : hosted_) {
    const Session& s = hosted.session;
    m << "session " << Hex(s.id.data(), 8) << " " << Hex(s.key.data(), 16)
      << " " << Hex(s.host.data(), 36) << " " << std::hex << s.flags << " "
      << s.max_public << " " << s.max_private << " " << s.filled_public << " "
      << s.filled_private << " " << s.host_xuid << "\n";
    for (const auto& [id, value] : s.contexts) {
      m << "ctx " << Hex(s.id.data(), 8) << " " << std::hex << id << " "
        << value << "\n";
    }
    for (const auto& [id, value] : s.properties) {
      m << "prop " << Hex(s.id.data(), 8) << " " << std::hex << id << " "
        << value << "\n";
    }
  }
  for (const auto& [id, listener] : qos_) {
    if (listener.listening) {
      m << "qos " << Hex(id.data(), 8) << " "
        << Hex(listener.data.data(), listener.data.size()) << "\n";
    }
  }
  return m.str();
}

// Receive: a message from another copy (false: not one).
bool Receive(const std::string& message) {
  std::istringstream in(message);
  std::string line;
  if (!std::getline(in, line) || line != "XLAN 1") {
    return false;
  }
  uint64_t id = 0;
  Peer peer;
  std::map<std::string, size_t> session_index;
  while (std::getline(in, line)) {
    std::istringstream f(line);
    std::string key;
    f >> key;
    if (key == "id") {
      f >> std::hex >> id;
    } else if (key == "title") {
      f >> std::hex >> peer.title_id;
    } else if (key == "user") {
      uint64_t xuid = 0;
      std::string tag;
      f >> std::hex >> xuid;
      std::getline(f >> std::ws, tag);
      peer.users.emplace_back(xuid, tag);
    } else if (key == "session") {
      std::string sid, skey, shost;
      Session s;
      f >> sid >> skey >> shost >> std::hex >> s.flags >> s.max_public >>
          s.max_private >> s.filled_public >> s.filled_private >> s.host_xuid;
      if (Unhex(sid, s.id.data(), 8) && Unhex(skey, s.key.data(), 16) &&
          Unhex(shost, s.host.data(), 36)) {
        session_index[sid] = peer.sessions.size();
        peer.sessions.push_back(s);
      }
    } else if (key == "ctx") {
      std::string sid;
      uint32_t cid = 0, value = 0;
      f >> sid >> std::hex >> cid >> value;
      auto it = session_index.find(sid);
      if (it != session_index.end()) {
        peer.sessions[it->second].contexts.emplace_back(cid, value);
      }
    } else if (key == "prop") {
      std::string sid;
      uint32_t pid = 0, value = 0;
      f >> sid >> std::hex >> pid >> value;
      auto it = session_index.find(sid);
      if (it != session_index.end()) {
        peer.sessions[it->second].properties.emplace_back(pid, value);
      }
    } else if (key == "qos") {
      std::string sid, data;
      f >> sid >> data;
      XnKid kid;
      if (Unhex(sid, kid.data(), 8)) {
        peer.qos[kid] = UnhexVector(data);
      }
    }
  }
  if (!id || id == instance_id_) {
    return false;  // (our own, heard back)
  }
  peer.seen = std::chrono::steady_clock::now();
  std::lock_guard<std::mutex> lock(mutex_);
  bool is_new = !peers_.count(id);
  peers_[id] = std::move(peer);
  if (is_new) {
    const Peer& p = peers_[id];
    XELOGI("LAN Live: found a copy ({} profile(s), title {:08X})",
           p.users.size(), p.title_id);
  }
  return true;
}

void Run() {
#ifdef XE_PLATFORM_WIN32
  WSADATA wsa;
  WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
  const uint16_t port = uint16_t(cvars::lan_live_port);
  const uint32_t own = OwnAddress();
  const bool loopback = (own >> 24) == 127;
  lan_socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  if (s == kNoSocket) {
    XELOGE("LAN Live: no UDP socket");
    return;
  }
  int one = 1;
  setsockopt(s, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one),
             sizeof(one));
  setsockopt(s, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one),
             sizeof(one));
  sockaddr_in local = {};
  local.sin_family = AF_INET;
  local.sin_port = htons(port);
  // Copies on one PC each have their own 127.0.0.x; on a network, any address
  // (broadcasts arrive there).
  local.sin_addr.s_addr = htonl(loopback ? own : INADDR_ANY);
  if (bind(s, reinterpret_cast<sockaddr*>(&local), sizeof(local)) != 0) {
    XELOGE("LAN Live: can't listen on UDP port {}", port);
    CloseLanSocket(s);
    return;
  }
  std::vector<uint32_t> targets;
  if (!loopback) {
    // The home network's broadcast addresses: everywhere, and this PC's
    // network (a /24, as home routers give) in case it has more than one.
    targets.push_back(0xFFFFFFFF);
    targets.push_back((own & 0xFFFFFF00) | 0xFF);
  }
  {
    std::string list = cvars::lan_live_peers;
    size_t start = 0;
    while (start <= list.size()) {
      size_t end = list.find(',', start);
      if (end == std::string::npos) {
        end = list.size();
      }
      std::string item = list.substr(start, end - start);
      item.erase(0, item.find_first_not_of(" \t"));
      item.erase(item.find_last_not_of(" \t") + 1);
      if (uint32_t a = ParseIPv4(item)) {
        if (a != own) {
          targets.push_back(a);
        }
      }
      start = end + 1;
    }
  }
  XELOGI("LAN Live: on UDP port {} ({} target(s), this copy {:08X})", port,
         targets.size(), own);
  auto next_send = std::chrono::steady_clock::now();
  std::vector<char> buffer(65536);
  while (true) {
    auto now = std::chrono::steady_clock::now();
    if (now >= next_send) {
      next_send = now + std::chrono::milliseconds(1000);
      std::string message = Describe();
      for (uint32_t target : targets) {
        sockaddr_in to = {};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = htonl(target);
        sendto(s, message.data(), int(message.size()), 0,
               reinterpret_cast<sockaddr*>(&to), sizeof(to));
      }
      std::lock_guard<std::mutex> lock(mutex_);
      for (auto it = peers_.begin(); it != peers_.end();) {
        if (now - it->second.seen > std::chrono::seconds(6)) {
          XELOGI("LAN Live: a copy has gone");
          it = peers_.erase(it);
        } else {
          ++it;
        }
      }
    }
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(s, &read_set);
    timeval timeout = {0, 200000};
    if (select(int(s) + 1, &read_set, nullptr, nullptr, &timeout) > 0) {
      sockaddr_in from = {};
      socklen_t from_len = sizeof(from);
      int n = recvfrom(s, buffer.data(), int(buffer.size()), 0,
                       reinterpret_cast<sockaddr*>(&from), &from_len);
      if (n > 0 && Receive(std::string(buffer.data(), size_t(n)))) {
        // A copy that found this one (one that started later, or a friend
        // over Tailscale) hears back from it from now on.
        const uint32_t sender = ntohl(from.sin_addr.s_addr);
        if (sender && sender != own &&
            std::find(targets.begin(), targets.end(), sender) ==
                targets.end()) {
          targets.push_back(sender);
          XELOGI("LAN Live: talking to {:08X} too", sender);
        }
      }
    }
  }
}

void Start() {
  std::call_once(started_, []() {
    std::random_device random;
    instance_id_ = (uint64_t(random()) << 32) | random();
    if (!instance_id_) {
      instance_id_ = 1;
    }
    std::thread(Run).detach();
  });
}

}  // namespace

bool Enabled() {
  if (!cvars::lan_live || !cvars::xnet_online) {
    return false;
  }
  Start();
  return true;
}

uint32_t OwnAddress() {
  if (uint32_t own = XSocket::InstanceAddress()) {
    return own;
  }
  static const uint32_t detected = []() -> uint32_t {
    // The address this PC would send from (no packet is sent).
    lan_socket_t s = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s == kNoSocket) {
      return 0x7F000001;
    }
    sockaddr_in to = {};
    to.sin_family = AF_INET;
    to.sin_port = htons(53);
    to.sin_addr.s_addr = htonl(0x08080808);
    uint32_t address = 0x7F000001;
    if (connect(s, reinterpret_cast<sockaddr*>(&to), sizeof(to)) == 0) {
      sockaddr_in local = {};
      socklen_t len = sizeof(local);
      if (getsockname(s, reinterpret_cast<sockaddr*>(&local), &len) == 0 &&
          local.sin_addr.s_addr) {
        address = ntohl(local.sin_addr.s_addr);
      }
    }
    CloseLanSocket(s);
    return address;
  }();
  return detected;
}

void FillXnAddr(uint8_t* out, uint32_t address) {
  std::memset(out, 0, 36);
  for (int i = 0; i < 4; ++i) {
    out[i] = out[4 + i] = uint8_t(address >> (24 - 8 * i));
  }
  out[8] = 1000 >> 8;
  out[9] = 1000 & 0xFF;
  // A locally administered MAC address: 02:58:xx:xx:xx:xx (the address).
  out[10] = 0x02;
  out[11] = 0x58;
  for (int i = 0; i < 4; ++i) {
    out[12 + i] = uint8_t(address >> (24 - 8 * i));
  }
}

std::vector<Friend> Friends(uint32_t title_id) {
  std::vector<Friend> out;
  if (!Enabled()) {
    return out;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [id, peer] : peers_) {
    if (title_id && peer.title_id != title_id) {
      continue;
    }
    for (const auto& [xuid, tag] : peer.users) {
      Friend f;
      f.xuid = xuid;
      f.gamertag = tag;
      f.title_id = peer.title_id;
      // In the session its copy hosts (its own, if it hosts more than one).
      for (const auto& s : peer.sessions) {
        if (!f.joinable || s.host_xuid == xuid) {
          f.session_id = s.id;
          f.joinable = true;
        }
      }
      out.push_back(f);
    }
  }
  return out;
}

bool FindSession(const XnKid& id, Session* out) {
  if (!Enabled()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [handle, hosted] : hosted_) {
    if (hosted.session.id == id) {
      *out = hosted.session;
      return true;
    }
  }
  for (const auto& [pid, peer] : peers_) {
    for (const auto& s : peer.sessions) {
      if (s.id == id) {
        *out = s;
        return true;
      }
    }
  }
  return false;
}

bool QosData(const XnKid& id, std::vector<uint8_t>* out) {
  if (!Enabled()) {
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto own = qos_.find(id);
  if (own != qos_.end() && own->second.listening) {
    *out = own->second.data;
    return true;
  }
  for (const auto& [pid, peer] : peers_) {
    auto it = peer.qos.find(id);
    if (it != peer.qos.end()) {
      *out = it->second;
      return true;
    }
  }
  return false;
}

std::vector<Session> Search(
    const std::vector<std::pair<uint32_t, uint32_t>>& contexts,
    const std::vector<std::pair<uint32_t, uint32_t>>& properties,
    uint32_t max_results) {
  std::vector<Session> out;
  if (!Enabled()) {
    return out;
  }
  const uint32_t title_id = kernel_state()->title_id();
  auto differs = [](const std::vector<std::pair<uint32_t, uint32_t>>& have,
                    uint32_t id, uint32_t value) {
    for (const auto& [hid, hvalue] : have) {
      if (hid == id) {
        return hvalue != value;
      }
    }
    return false;  // (not set by the host: matches)
  };
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& [pid, peer] : peers_) {
    if (peer.title_id != title_id) {
      continue;
    }
    for (const auto& session : peer.sessions) {
      const uint32_t filled = std::max<uint32_t>(session.filled_public, 1);
      if (session.max_public <= filled) {
        continue;  // (full, or private only)
      }
      bool match = true;
      for (const auto& [id, value] : contexts) {
        match = match && !differs(session.contexts, id, value);
      }
      for (const auto& [id, value] : properties) {
        // Title-defined ones (system properties - skill, gamer zone... -
        // are ranges in a query, not equal values).
        if (!(id & 0x8000)) {
          match = match && !differs(session.properties, id, value);
        }
      }
      if (match) {
        out.push_back(session);
      }
    }
  }
  std::stable_sort(out.begin(), out.end(),
                   [](const Session& a, const Session& b) {
                     return a.filled_public + a.filled_private >
                            b.filled_public + b.filled_private;
                   });
  if (out.size() > max_results) {
    out.resize(max_results);
  }
  return out;
}

void HostAttribute(uint64_t xuid, uint32_t id, uint32_t value, bool context) {
  if (!cvars::lan_live || !xuid) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  for (auto& [handle, hosted] : hosted_) {
    if (hosted.session.host_xuid != xuid) {
      continue;
    }
    auto& list = context ? hosted.session.contexts : hosted.session.properties;
    bool found = false;
    for (auto& [hid, hvalue] : list) {
      if (hid == id) {
        hvalue = value;
        found = true;
      }
    }
    if (!found) {
      list.emplace_back(id, value);
    }
  }
}

void HostSession(uint32_t session_handle, const Session& session) {
  if (!Enabled()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  hosted_[session_handle].session = session;
  XELOGI(
      "LAN Live: hosting session {} for the other copies (flags {:08X}, {} "
      "public / {} private, {} context(s), {} propert(ies))",
      Hex(session.id.data(), 8), session.flags, session.max_public,
      session.max_private, session.contexts.size(), session.properties.size());
  for (const auto& [id, value] : session.contexts) {
    XELOGI("LAN Live:   context {:08X} = {}", id, value);
  }
  for (const auto& [id, value] : session.properties) {
    XELOGI("LAN Live:   property {:08X} = {}", id, value);
  }
}

void ModifySession(uint32_t session_handle, uint32_t flags, uint32_t max_public,
                   uint32_t max_private) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = hosted_.find(session_handle);
  if (it == hosted_.end()) {
    return;
  }
  it->second.session.flags = flags;
  it->second.session.max_public = max_public;
  it->second.session.max_private = max_private;
}

void EndSession(uint32_t session_handle) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = hosted_.find(session_handle);
  if (it == hosted_.end()) {
    return;
  }
  XELOGI("LAN Live: session {} ended", Hex(it->second.session.id.data(), 8));
  qos_.erase(it->second.session.id);
  hosted_.erase(it);
}

void SetQosData(const XnKid& id, const std::vector<uint8_t>& data) {
  if (!Enabled()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  qos_[id].data = data;
}

void SetQosListening(const XnKid& id, bool listening) {
  if (!Enabled()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  qos_[id].listening = listening;
}

void ClearQos(const XnKid& id) {
  std::lock_guard<std::mutex> lock(mutex_);
  qos_.erase(id);
}

}  // namespace lan_live
}  // namespace xam
}  // namespace kernel
}  // namespace xe
