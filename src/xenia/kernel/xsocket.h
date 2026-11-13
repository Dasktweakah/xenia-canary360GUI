/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2015 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_KERNEL_XSOCKET_H_
#define XENIA_KERNEL_XSOCKET_H_

#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <queue>
#include <thread>

#include "xenia/base/byte_order.h"
#include "xenia/kernel/xobject.h"

namespace xe {
namespace kernel {
enum class X_WSAError : uint32_t {
  X_WSA_INVALID_PARAMETER = 0x0057,
  X_WSAEFAULT = 0x271E,
  X_WSAEINVAL = 0x2726,
  X_WSAENOTSOCK = 0x2736,
  X_WSAEMSGSIZE = 0x2738,
};

struct XSOCKADDR {
  xe::be<uint16_t> address_family;
  char sa_data[14];
};
static_assert_size(XSOCKADDR, 0x10);

struct N_XSOCKADDR {
  N_XSOCKADDR() {}
  N_XSOCKADDR(const XSOCKADDR* other) { *this = *other; }
  N_XSOCKADDR& operator=(const XSOCKADDR& other) {
    address_family = other.address_family;
    std::memcpy(sa_data, other.sa_data, xe::countof(sa_data));
    return *this;
  }

  uint16_t address_family;
  char sa_data[14];
};

struct XSOCKADDR_IN {
  xe::be<uint16_t> sin_family;

  // Always big-endian!
  xe::be<uint16_t> sin_port;
  xe::be<uint32_t> sin_addr;
  // sin_zero is defined as __pad on Android, so prefixed here.
  char x_sin_zero[8];
};
static_assert_size(XSOCKADDR_IN, 0x10);

// Xenia native sockaddr_in
struct N_XSOCKADDR_IN {
  N_XSOCKADDR_IN() {}
  N_XSOCKADDR_IN(const XSOCKADDR_IN* other) { *this = *other; }
  N_XSOCKADDR_IN& operator=(const XSOCKADDR_IN& other) {
    sin_family = other.sin_family;
    sin_port = other.sin_port;
    sin_addr = other.sin_addr;
    std::memset(x_sin_zero, 0, sizeof(x_sin_zero));

    return *this;
  }

  uint16_t sin_family;
  xe::be<uint16_t> sin_port;
  xe::be<uint32_t> sin_addr;
  // sin_zero is defined as __pad on Android, so prefixed here.
  char x_sin_zero[8];
};

class XSocket : public XObject {
 public:
  static const XObject::Type kObjectType = XObject::Type::Socket;

  enum AddressFamily {
    AF_INET = 2,
  };

  enum Type {
    SOCK_STREAM = 1,
    SOCK_DGRAM = 2,
  };

  enum Protocol {
    XE_IPPROTO_TCP = 6,
    XE_IPPROTO_UDP = 17,

    // LIVE Voice and Data Protocol
    // https://blog.csdn.net/baozi3026/article/details/4277227
    // Format: [cbGameData][GameData(encrypted)][VoiceData(unencrypted)]
    XE_IPPROTO_VDP = 254,
  };

  XSocket(KernelState* kernel_state);
  ~XSocket();

  uint64_t native_handle() const { return native_handle_; }
  uint16_t bound_port() const { return bound_port_; }

  X_STATUS Initialize(AddressFamily af, Type type, Protocol proto);
  X_STATUS Close();

  X_STATUS GetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                     uint32_t* optlen);
  X_STATUS SetOption(uint32_t level, uint32_t optname, void* optval_ptr,
                     uint32_t optlen);
  X_STATUS IOControl(uint32_t cmd, uint8_t* arg_ptr);

  X_STATUS Connect(N_XSOCKADDR* name, int name_len);
  X_STATUS Bind(N_XSOCKADDR_IN* name, int name_len);
  X_STATUS Listen(int backlog);
  X_STATUS GetSockName(uint8_t* buf, int* buf_len);
  object_ref<XSocket> Accept(N_XSOCKADDR* name, int* name_len);
  int Shutdown(int how);

  int Recv(uint8_t* buf, uint32_t buf_len, uint32_t flags);
  int Send(const uint8_t* buf, uint32_t buf_len, uint32_t flags);

  int RecvFrom(uint8_t* buf, uint32_t buf_len, uint32_t flags,
               N_XSOCKADDR_IN* from, uint32_t* from_len);
  int SendTo(uint8_t* buf, uint32_t buf_len, uint32_t flags, N_XSOCKADDR_IN* to,
             uint32_t to_len);

  uint32_t GetLastWSAError() const;

  // Basically360: an overlapped receive (WSARecvFrom with an OVERLAPPED) that
  // found nothing waiting. A host thread waits for the socket's next packet
  // (up to max_len bytes) and hands it to done: the result recvfrom gave (the
  // length, or -1 with the WSA error), the data and where it came from.
  using RecvDone =
      std::function<void(int result, const uint8_t* data,
                         const N_XSOCKADDR_IN& from, uint32_t wsa_error)>;
  void RecvFromAsync(uint32_t max_len, RecvDone done);

  // Basically360: xnet_address as a number in host byte order, 0 when unset.
  static uint32_t InstanceAddress();

  struct packet {
    // These values are in network byte order.
    xe::be<uint16_t> src_port;
    xe::be<uint32_t> src_ip;

    uint16_t data_len;
    uint8_t data[1];
  };

  // Queue a packet into our internal buffer.
  bool QueuePacket(uint32_t src_ip, uint16_t src_port, const uint8_t* buf,
                   size_t len);

 private:
  XSocket(KernelState* kernel_state, uint64_t native_handle);
  void BindToInstanceAddress();
  uint64_t native_handle_ = -1;

  AddressFamily af_;    // Address family
  Type type_;           // Type (DGRAM/Stream/etc)
  Protocol proto_;      // Protocol (TCP/UDP/etc)
  bool secure_ = true;  // Secure socket (encryption enabled)

  bool bound_ = false;  // Explicitly bound to an IP address?
  uint16_t bound_port_ = 0;

  bool broadcast_socket_ = false;

  // RecvFromAsync's waiting receives and the thread that completes them.
  void AsyncRecvThread();
  void StopAsyncRecv();
  std::mutex async_mutex_;
  std::condition_variable async_cv_;
  std::deque<std::pair<uint32_t, RecvDone>> async_recvs_;
  std::thread async_thread_;
  bool async_stop_ = false;

  std::unique_ptr<xe::threading::Event> event_;
  std::mutex incoming_packet_mutex_;
  std::queue<uint8_t*> incoming_packets_;
};

}  // namespace kernel
}  // namespace xe

#endif  // XENIA_KERNEL_XSOCKET_H_
