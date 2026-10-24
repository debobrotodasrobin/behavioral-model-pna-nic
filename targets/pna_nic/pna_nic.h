/* Copyright 2024 Marvell Technology, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Rupesh Chiluka (rchiluka@marvell.com)
 *
 */

#ifndef PNA_NIC_PNA_NIC_H_
#define PNA_NIC_PNA_NIC_H_

#include <bm/bm_sim/queue.h>
#include <bm/bm_sim/queueing.h>
#include <bm/bm_sim/packet.h>
#include <bm/bm_sim/switch.h>
#include <bm/bm_sim/event_logger.h>
#include <bm/bm_sim/simple_pre_lag.h>

#include <memory>
#include <chrono>
#include <thread>
#include <vector>
#include <functional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

using ts_res = std::chrono::microseconds;
using std::chrono::duration_cast;
using ticks = std::chrono::nanoseconds;

namespace bm {

namespace pna {

// Packet register used as the drop flag, set by the drop_packet() primitive and
// consumed by PnaNic::main_thread(). PNA defines no egress_spec field (unlike
// v1model / PSA), so the target cannot signal a drop by poisoning a metadata
// field and has to remember the forwarding decision itself.
// Register 0 is taken by PACKET_LENGTH_REG_IDX (see pna_nic.cpp);
// Packet::nb_registers is 4, so indices 2 and 3 stay available.
constexpr size_t PACKET_DROP_FLAG_REG_IDX = 1;

// Packet register used to carry the mirror session id selected by the
// mirror_packet() extern function to PnaNic::main_thread(), which is where the
// copy is actually made. The copy has to be taken after the deparser so that
// the mirrored frame is the outgoing frame.
//
// The session id is stored offset by one so that a register value of 0 means
// "no mirroring": PNA defines PNA_MIRROR_SESSION_TO_CPU == 0, so 0 is a legal
// session id and cannot double as the "unset" sentinel.
constexpr size_t PACKET_MIRROR_SESSION_REG_IDX = 2;

// MirrorSessionId_t is bit<16> in the PNA architecture header.
constexpr uint64_t MIRROR_SESSION_ID_MAX = 0xffffu;

// Packet register used to carry the recirculate() request to
// PnaNic::main_thread(), which is where the packet is actually looped back.
// Last of the four registers Packet::nb_registers allows (0 is
// PACKET_LENGTH_REG_IDX, 1 the drop flag, 2 the mirror session).
constexpr size_t PACKET_RECIRCULATE_FLAG_REG_IDX = 3;

// Digest<T>::pack() needs a fifth slot, but Packet::nb_registers is fixed at
// four. bmv2's own answer to this (see simple_switch/register_access.h) is to
// treat each 64-bit register as a bag of bitfields rather than as one value,
// so the digest goes into the mirror register's upper half: MirrorSessionId_t
// is bit<16>, which leaves bits 16-63 of that register unused.
//   register 2, bits 0-15  : mirror session id + 1 (0 means "not mirrored")
//   register 2, bits 32-47 : digest field-list id + 1 (0 means "no digest")
// The +1 keeps id 0 representable, since 0 is a valid field-list id.
constexpr size_t PACKET_MIRROR_AND_DIGEST_REG_IDX = PACKET_MIRROR_SESSION_REG_IDX;
constexpr uint64_t PACKET_MIRROR_SESSION_MASK = 0x000000000000ffffull;
constexpr uint64_t PACKET_DIGEST_FIELD_LIST_MASK = 0x0000ffff00000000ull;
constexpr int PACKET_DIGEST_FIELD_LIST_SHIFT = 32;

//! \return the digest field-list id requested for \p packet, or -1 for none.
inline int packet_digest_field_list(Packet *packet) {
  const uint64_t reg = packet->get_register(PACKET_MIRROR_AND_DIGEST_REG_IDX);
  const uint64_t stored = (reg & PACKET_DIGEST_FIELD_LIST_MASK) >>
                          PACKET_DIGEST_FIELD_LIST_SHIFT;
  return stored == 0 ? -1 : static_cast<int>(stored - 1);
}

//! Records a digest request without disturbing the mirror session beside it.
inline void packet_set_digest_field_list(Packet &packet, uint16_t field_list_id) {
  const uint64_t reg = packet.get_register(PACKET_MIRROR_AND_DIGEST_REG_IDX);
  packet.set_register(
      PACKET_MIRROR_AND_DIGEST_REG_IDX,
      (reg & ~PACKET_DIGEST_FIELD_LIST_MASK) |
      (static_cast<uint64_t>(field_list_id) + 1) << PACKET_DIGEST_FIELD_LIST_SHIFT);
}

//! \return the mirror session id requested for \p packet, or -1 for none.
inline int packet_mirror_session(Packet *packet) {
  const uint64_t stored =
      packet->get_register(PACKET_MIRROR_AND_DIGEST_REG_IDX) &
      PACKET_MIRROR_SESSION_MASK;
  return stored == 0 ? -1 : static_cast<int>(stored - 1);
}

//! Records a mirror session without disturbing the digest id beside it.
inline void packet_set_mirror_session(Packet &packet, uint16_t session_id) {
  const uint64_t reg = packet.get_register(PACKET_MIRROR_AND_DIGEST_REG_IDX);
  packet.set_register(PACKET_MIRROR_AND_DIGEST_REG_IDX,
                      (reg & ~PACKET_MIRROR_SESSION_MASK) |
                      (static_cast<uint64_t>(session_id) + 1));
}

inline void packet_clear_mirror_session(Packet &packet) {
  const uint64_t reg = packet.get_register(PACKET_MIRROR_AND_DIGEST_REG_IDX);
  packet.set_register(PACKET_MIRROR_AND_DIGEST_REG_IDX,
                      reg & ~PACKET_MIRROR_SESSION_MASK);
}

// PNA classifies every port of a NIC as either host-facing or network-facing,
// and a program reads that classification through the extern functions
// is_host_port() / is_net_port(). Nothing in bmv2 carries it for us: the
// PktInstanceType / PktDirection enums in PnaNic are declared but never used
// anywhere, and PNA has no ingress_port metadata carrying a port kind the way
// PSA does. The control plane therefore declares it per port, over thrift
// (PnaNic_server::set_port_class).
//
// A port that has never been configured is reported as a network port. That is
// the safe default for a NIC, whose ports face the network until a host is
// attached, and it keeps is_net_port() true for the unconfigured two-veth
// setups the tests use.
//
// This lives at namespace scope rather than as a PnaNic member because the two
// callers have different reach: the is_host_port / is_net_port primitives run
// on packet worker threads and hold no handle to the switch instance, while
// the thrift handler that writes the table does. One process hosts exactly one
// NIC device, so one table per process is correct.
bool port_class_set(port_t port, bool is_host);
bool port_class_is_host(port_t port);

class PnaNic : public Switch {
 public:

  using TransmitFn = std::function<void(port_t, packet_id_t,
                                        const char *, int)>;

  // Where a mirror session sends its copies. PNA mirrors to a port; a NIC has
  // no egress pipeline, no tunnel encapsulation and no PRE, so a port is all a
  // session needs.
  struct MirroringSessionConfig {
    port_t egress_port;
  };


 private:
  using clock = std::chrono::high_resolution_clock;

 public:
  // by default, swapping is off
  explicit PnaNic(bool enable_swap = false);

  ~PnaNic();

  int receive_(port_t port_num, const char *buffer, int len) override;

  void start_and_return_() override;

  // Mirror session control plane, exposed over thrift by PnaNicHandler.
  // Returns true when the session was added / removed, or found respectively.
  bool mirroring_session_add(int session_id,
                             const MirroringSessionConfig &config);
  bool mirroring_session_delete(int session_id);
  bool mirroring_session_get(int session_id,
                             MirroringSessionConfig *config) const;

  void reset_target_state_() override;

  // returns the number of microseconds elapsed since the nic started
  uint64_t get_time_elapsed_us() const;

  // returns the number of microseconds elasped since the clock's epoch
  uint64_t get_time_since_epoch_us() const;

  // returns the packet id of most recently received packet. Not thread-safe.
  static packet_id_t get_packet_id() {
    return (packet_id-1);
  }

  void set_transmit_fn(TransmitFn fn);

 private:
  static packet_id_t packet_id;

  enum PktInstanceType {
    FROM_NET_PORT,
    FROM_NET_LOOPEDBACK,
    FROM_NET_RECIRCULATED,
    FROM_HOST,
    FROM_HOST_LOOPEDBACK,
    FROM_HOST_RECIRCULATED,
  };

  enum PktDirection {
    NET_TO_HOST,
    HOST_TO_NET,
  };

 private:
  void main_thread();
  void transmit_thread();

  ts_res get_ts() const;

 private:
  std::vector<std::thread> threads_;
  Queue<std::unique_ptr<Packet> > input_buffer;
  Queue<std::unique_ptr<Packet> > output_buffer;
  TransmitFn my_transmit_fn;

  // Mirror session table. Guarded by mirror_sessions_mutex because it is
  // written by the thrift control plane thread and read by main_thread().
  mutable std::mutex mirror_sessions_mutex;
  std::unordered_map<int, MirroringSessionConfig> mirror_sessions;
  clock::time_point start;
};

}  // namespace bm::pna

}  // namespace bm

#endif  // PNA_NIC_PNA_NIC_H_
