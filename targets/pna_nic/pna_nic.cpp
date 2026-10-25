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

#include <bm/bm_sim/parser.h>
#include <bm/bm_sim/tables.h>
#include <bm/bm_sim/logger.h>

#include <unistd.h>

#include <iostream>
#include <fstream>
#include <string>

#include <bm/bm_sim/learning.h>
#include "pna_nic.h"
#include "externs/pna_idle_timeout.h"

extern int import_primitives();
extern int import_counters();
extern int import_meters();
extern int import_random();
extern int import_internet_checksum();
extern int import_hash();

namespace bm {

namespace pna {

packet_id_t PnaNic::packet_id = 0;


PnaNic::PnaNic(bool enable_swap)
  : Switch(enable_swap),
    input_buffer(1024),
    output_buffer(128),
    // cannot use std::bind because of a clang bug
    // https://stackoverflow.com/questions/32030141/is-this-incorrect-use-of-stdbind-or-a-compiler-bug
    my_transmit_fn([this](port_t port_num, packet_id_t pkt_id,
                          const char *buffer, int len) {
        _BM_UNUSED(pkt_id);
        this->transmit_fn(port_num, buffer, len);
    }),
    start(clock::now())
    {
  add_required_field("pna_main_parser_input_metadata", "recirculated");
  add_required_field("pna_main_parser_input_metadata", "input_port");

  add_required_field("pna_main_input_metadata", "recirculated");
  add_required_field("pna_main_input_metadata", "timestamp");
  add_required_field("pna_main_input_metadata", "parser_error");
  add_required_field("pna_main_input_metadata", "class_of_service");
  add_required_field("pna_main_input_metadata", "input_port");

  add_required_field("pna_main_output_metadata", "class_of_service");

  force_arith_header("pna_main_parser_input_metadata");
  force_arith_header("pna_main_input_metadata");
  force_arith_header("pna_main_output_metadata");

  import_primitives();
  import_counters();
  import_meters();
  import_random();
  import_internet_checksum();
  import_hash();
}

#define PACKET_LENGTH_REG_IDX 0

int
PnaNic::receive_(port_t port_num, const char *buffer, int len) {
  // we limit the packet buffer to original size + 512 bytes, which means we
  // cannot add more than 512 bytes of header data to the packet, which should
  // be more than enough
  
  auto packet = new_packet_ptr(port_num, packet_id++, len,
                               bm::PacketBuffer(len + 512, buffer, len));

  BMELOG(packet_in, *packet);
  auto *phv = packet->get_phv();

  // many current p4 programs assume this
  // from pna spec - PNA does not mandate initialization of user-defined
  // metadata to known values as given as input to the parser
  phv->reset_metadata();
  
  phv->get_field("pna_main_parser_input_metadata.recirculated").set(0);
  phv->get_field("pna_main_parser_input_metadata.input_port").set(port_num);

  // using packet register 0 to store length, this register will be updated for
  // each add_header / remove_header primitive call
  packet->set_register(PACKET_LENGTH_REG_IDX, len);

  // clear the drop flag; the packet is forwarded unless drop_packet() sets it
  // again during the main control pass
  packet->set_register(PACKET_DROP_FLAG_REG_IDX, 0);

  // No mirroring and no digest unless the main control pass asks for them.
  // Both live in register 2, and a packet fresh off the wire has neither, so
  // clearing the whole register (rather than one field of it) is what we want.
  packet->set_register(PACKET_MIRROR_AND_DIGEST_REG_IDX, 0);

  input_buffer.push_front(std::move(packet));
  return 0;
}

void
PnaNic::start_and_return_() {
  threads_.push_back(std::thread(&PnaNic::main_thread, this));
  threads_.push_back(std::thread(&PnaNic::transmit_thread, this));
}

PnaNic::~PnaNic() {
  input_buffer.push_front(nullptr);
  output_buffer.push_front(nullptr);
  for (auto& thread_ : threads_) {
    thread_.join();
  }
}

void
PnaNic::reset_target_state_() {
  bm::Logger::get()->debug("Resetting pna_nic target-specific state");
  get_component<McSimplePreLAG>()->reset_state();
}

uint64_t
PnaNic::get_time_elapsed_us() const {
  return get_ts().count();
}

uint64_t
PnaNic::get_time_since_epoch_us() const {
  auto tp = clock::now();
  return duration_cast<ts_res>(tp.time_since_epoch()).count();
}

void
PnaNic::set_transmit_fn(TransmitFn fn) {
  my_transmit_fn = std::move(fn);
}

// See the declaration in pna_nic.h for why this table is namespace scope and
// why an unconfigured port counts as a network port.
namespace {

std::mutex port_class_mutex;
std::unordered_set<port_t> host_ports;

}  // namespace

bool
port_class_set(port_t port, bool is_host) {
  std::lock_guard<std::mutex> lock(port_class_mutex);
  if (is_host) {
    host_ports.insert(port);
  } else {
    host_ports.erase(port);
  }
  return true;
}

bool
port_class_is_host(port_t port) {
  std::lock_guard<std::mutex> lock(port_class_mutex);
  return host_ports.find(port) != host_ports.end();
}

bool
PnaNic::mirroring_session_add(int session_id,
                              const MirroringSessionConfig &config) {
  if (session_id < 0 ||
      static_cast<uint64_t>(session_id) > MIRROR_SESSION_ID_MAX) {
    bm::Logger::get()->error(
        "mirror session id {} out of range (0..{}); session not added",
        session_id, MIRROR_SESSION_ID_MAX);
    return false;
  }
  std::lock_guard<std::mutex> lock(mirror_sessions_mutex);
  mirror_sessions[session_id] = config;
  return true;
}

bool
PnaNic::mirroring_session_delete(int session_id) {
  std::lock_guard<std::mutex> lock(mirror_sessions_mutex);
  return mirror_sessions.erase(session_id) == 1;
}

bool
PnaNic::mirroring_session_get(int session_id,
                              MirroringSessionConfig *config) const {
  std::lock_guard<std::mutex> lock(mirror_sessions_mutex);
  auto it = mirror_sessions.find(session_id);
  if (it == mirror_sessions.end()) return false;
  if (config != nullptr) *config = it->second;
  return true;
}

void
PnaNic::transmit_thread() {
  while (1) {
    std::unique_ptr<Packet> packet;
    output_buffer.pop_back(&packet);

    if (packet == nullptr) break;
    BMELOG(packet_out, *packet);
    BMLOG_DEBUG_PKT(*packet, "Transmitting packet of size {} out of port {}",
                    packet->get_data_size(), packet->get_egress_port());

    my_transmit_fn(packet->get_egress_port(), packet->get_packet_id(),
                   packet->data(), packet->get_data_size());
  }
}

ts_res
PnaNic::get_ts() const {
  return duration_cast<ts_res>(clock::now() - start);
}

void
PnaNic::main_thread() {
  PHV *phv;

  while (1) {
    std::unique_ptr<Packet> packet;
    input_buffer.pop_back(&packet);
    if (packet == nullptr) break;

    phv = packet->get_phv();
    auto input_port =
        phv->get_field("pna_main_parser_input_metadata.input_port").
            get_uint();
    BMLOG_DEBUG_PKT(*packet, "Processing packet received on port {}",
                    input_port);
                    
    phv->get_field("pna_main_input_metadata.timestamp").set(
        get_ts().count());

    Parser *parser = this->get_parser("main_parser");
    parser->parse(packet.get());

    // pass relevant values from main parser
    phv->get_field("pna_main_input_metadata.recirculated").set(
        phv->get_field("pna_main_parser_input_metadata.recirculated"));
    phv->get_field("pna_main_input_metadata.parser_error").set(
        packet->get_error_code().get());
    phv->get_field("pna_main_input_metadata.class_of_service").set(0);
    phv->get_field("pna_main_input_metadata.input_port").set(
        phv->get_field("pna_main_parser_input_metadata.input_port"));
    
    Pipeline *main_mau = this->get_pipeline("main_control");
    main_mau->apply(packet.get());
    packet->reset_exit();

    // A data-path add_entry() cannot insert while apply_action() holds the
    // table's lock, so the primitive queued the request instead; perform the
    // inserts now that the pipeline has returned and released every lock. This
    // runs before the drop / recirculate handling so an entry learned by a
    // packet that is subsequently dropped or recirculated is still installed,
    // matching the PNA semantics that add_entry() is a side effect independent
    // of the packet's fate.
    bm::pna::flush_pending_add_entry();

    // Digest<T>::pack() leaves the field list it wants reported in the mirror
    // register. Emitting it here, before the drop and recirculate handling,
    // matches what pack() promises: the digest reaches the control plane
    // whatever becomes of the packet. It also has to precede recirculate(),
    // which resets the PHV, because a digest is a set of PHV fields.
    const int digest_field_list_id = packet_digest_field_list(packet.get());
    if (digest_field_list_id >= 0) {
      BMLOG_DEBUG_PKT(*packet, "Generating digest for field list {}",
                      digest_field_list_id);
      get_learn_engine()->learn(digest_field_list_id, *packet);
    }

    // drop_packet() was called: per the PNA specification the packet is
    // dropped once the main control has finished executing, so it is neither
    // deparsed nor transmitted.
    if (packet->get_register(PACKET_DROP_FLAG_REG_IDX) != 0) {
      BMLOG_DEBUG_PKT(*packet, "Dropping packet");
      continue;
    }

    Deparser *deparser = this->get_deparser("main_deparser");
    deparser->deparse(packet.get());

    // recirculate() asks for a second pass through the main parser rather than
    // a trip out to the wire. Handling it here, after the deparser, is what
    // makes that well defined: deparse() has just prepended the headers to the
    // buffer, so data() now points at the complete frame and the parser will
    // read the same bytes a peer NIC would have seen. This is the same ordering
    // psa_switch uses when it loops a packet back after its egress deparser.
    if (packet->get_register(PACKET_RECIRCULATE_FLAG_REG_IDX) != 0) {
      // clear it before the packet goes back round, or the second pass would
      // recirculate forever
      packet->set_register(PACKET_RECIRCULATE_FLAG_REG_IDX, 0);
      // the frame carries no record of which port it came in on, and the PNA
      // model reports that port unchanged across a recirculation, so it is
      // carried across the PHV reset by hand
      const uint64_t recirc_input_port =
          phv->get_field("pna_main_parser_input_metadata.input_port").get_uint();
      BMLOG_DEBUG_PKT(*packet, "Recirculating packet (input port {})",
                      recirc_input_port);
      phv->reset();
      phv->reset_header_stacks();
      phv->reset_metadata();
      phv->get_field("pna_main_parser_input_metadata.recirculated").set(1);
      phv->get_field("pna_main_parser_input_metadata.input_port").
          set(recirc_input_port);
      input_buffer.push_front(std::move(packet));
      continue;
    }

    // mirror_packet() records a session here; the copy is made after the
    // deparser so that the mirrored frame is exactly the frame that leaves the
    // NIC (cloning before the deparser would require re-parsing the copy, the
    // way simple_switch does it for its ingress clones).
    const int session_id = packet_mirror_session(packet.get());
    if (session_id >= 0) {
      // clear just this field so that the clone does not mirror itself, and so
      // that the digest id sharing the register survives
      packet_clear_mirror_session(*packet);
      MirroringSessionConfig config;
      if (mirroring_session_get(session_id, &config)) {
        BMLOG_DEBUG_PKT(*packet, "Mirroring packet to port {} (session {})",
                        config.egress_port, session_id);
        std::unique_ptr<Packet> packet_copy = packet->clone_with_phv_ptr();
        packet_copy->set_egress_port(config.egress_port);
        output_buffer.push_front(std::move(packet_copy));
      } else {
        BMLOG_DEBUG_PKT(*packet,
                        "mirror_packet() selected unconfigured session {};"
                        " packet not mirrored", session_id);
      }
    }

    output_buffer.push_front(std::move(packet));
  }
}

}  // namespace bm::pna

}  // namespace bm
