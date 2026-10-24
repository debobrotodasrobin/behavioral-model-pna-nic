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

#include <bm/bm_sim/actions.h>
#include <bm/bm_sim/calculations.h>
#include <bm/bm_sim/core/primitives.h>
#include <bm/bm_sim/counters.h>
#include <bm/bm_sim/meters.h>
#include <bm/bm_sim/packet.h>
#include <bm/bm_sim/phv.h>
#include <bm/bm_sim/logger.h>
#include <bm/bm_sim/stateful.h>

#include "pna_nic.h"

#include <random>
#include <thread>

namespace bm {

namespace pna {

class add_header : public ActionPrimitive<Header &> {
  void operator ()(Header &hdr) {
    // TODO(antonin): reset header to 0?
    if (!hdr.is_valid()) {
      hdr.reset();
      hdr.mark_valid();
      // updated the length packet register (register 0)
      auto &packet = get_packet();
      packet.set_register(0, packet.get_register(0) + hdr.get_nbytes_packet());
    }
  }
};

REGISTER_PRIMITIVE(add_header);

class add_header_fast : public ActionPrimitive<Header &> {
  void operator ()(Header &hdr) {
    hdr.mark_valid();
  }
};

REGISTER_PRIMITIVE(add_header_fast);

class remove_header : public ActionPrimitive<Header &> {
  void operator ()(Header &hdr) {
    if (hdr.is_valid()) {
      // updated the length packet register (register 0)
      auto &packet = get_packet();
      packet.set_register(0, packet.get_register(0) - hdr.get_nbytes_packet());
      hdr.mark_invalid();
    }
  }
};

REGISTER_PRIMITIVE(remove_header);

// extern function: send_to_port
class send_to_port : public ActionPrimitive<const Data &> {
  void operator ()(const Data &dest_port) {
    auto &packet = get_packet();
    packet.set_egress_port(dest_port.get<uint32_t>());
    // send_to_port() is a forwarding function like drop_packet(), so it
    // supersedes any earlier drop_packet() call (see drop_packet below).
    packet.set_register(PACKET_DROP_FLAG_REG_IDX, 0);
  }
};

REGISTER_PRIMITIVE(send_to_port);

// extern function: drop_packet
//
// The PNA architecture header documents drop_packet() and send_to_port()
// together as "forwarding functions": they all set the destination of the
// packet, calling one of them overwrites the effect of any earlier call, and
// only the last one executed takes effect. send_to_port() records the egress
// port on the packet; drop_packet() has no port to record, so it sets the
// target-private drop flag instead and PnaNic::main_thread() discards the
// packet once the main control has finished, as the specification requires.
class drop_packet : public ActionPrimitive<> {
  void operator ()() {
    auto &packet = get_packet();
    packet.set_egress_port(0);
    packet.set_register(PACKET_DROP_FLAG_REG_IDX, 1);
  }
};

REGISTER_PRIMITIVE(drop_packet);

// extern function: mirror_packet
//
// Unlike drop_packet() and send_to_port(), mirror_packet() is NOT one of the
// PNA "forwarding functions": it does not set the destination of the original
// packet and therefore does not interact with the drop flag. It asks the target
// to also emit a copy. The copy itself is made by PnaNic::main_thread() once
// the deparser has run; all this primitive does is remember which session was
// selected.
//
// The id is stored offset by one so that 0 keeps meaning "no mirroring", since
// PNA_MIRROR_SESSION_TO_CPU is a valid session with id 0.
//
// The mirror_slot_id selects which headers make up the mirrored frame on real
// hardware. bmv2 has no per-slot header selection model, so the whole deparsed
// packet is mirrored and the slot id is only logged.
class mirror_packet : public ActionPrimitive<const Data &, const Data &> {
  void operator ()(const Data &mirror_slot_id, const Data &mirror_session_id) {
    const uint64_t session = mirror_session_id.get<uint64_t>();
    if (session > MIRROR_SESSION_ID_MAX) {
      BMLOG_ERROR_PKT(get_packet(),
                      "mirror_packet() session {} out of range (0..{});"
                      " packet not mirrored",
                      session, MIRROR_SESSION_ID_MAX);
      return;
    }
    BMLOG_TRACE_PKT(get_packet(),
                    "mirror_packet() slot {} session {}",
                    mirror_slot_id.get<uint64_t>(), session);
    // masked write: register 2 also carries the digest field-list id, so a
    // plain set_register() here would erase a digest requested earlier
    packet_set_mirror_session(get_packet(), static_cast<uint16_t>(session));
  }
};

REGISTER_PRIMITIVE(mirror_packet);

// extern method: Digest<T>::pack()
//
// The p4c portable backend lowers digest.pack() to generate_digest(), passing
// the id of the field list the digest carries (p4c has already turned the
// packed struct into a field list while compiling). All this primitive does is
// remember that id; the control-plane round trip itself belongs in
// PnaNic::main_thread(), which owns the learn engine.
//
// The write is masked because register 2 is shared with the mirror session.
class generate_digest : public ActionPrimitive<const Data &> {
  void operator ()(const Data &field_list_id) {
    const uint64_t id = field_list_id.get<uint64_t>();
    if (id > 0xffffu) {
      BMLOG_ERROR_PKT(get_packet(),
                      "generate_digest() field list {} out of range; ignored",
                      id);
      return;
    }
    packet_set_digest_field_list(get_packet(), static_cast<uint16_t>(id));
  }
};

REGISTER_PRIMITIVE(generate_digest);

// extern function: recirculate
//
// Like drop_packet() and send_to_port(), recirculate() is one of the PNA
// forwarding functions: it decides where the packet goes, and the last such
// call executed wins. Its "destination" is the NIC's own main parser, so
// instead of naming a port it asks for a second pass through the pipeline.
//
// All this primitive can do is leave a mark; the loop itself needs the deparser
// to have run first (the parser on the next pass must see the finished frame),
// so PnaNic::main_thread() acts on the flag after the deparser, in the same
// place where it acts on the drop flag.
//
// Clearing the flag is done by whoever consumes it, which is also what makes
// "last forwarding call wins" work: send_to_port() clears the drop flag for
// the same reason, and recirculate() has to overwrite a destination a previous
// forwarding call may already have set.
class recirculate : public ActionPrimitive<> {
  void operator ()() {
    auto &packet = get_packet();
    // a recirculated packet is not transmitted, so any destination chosen
    // earlier is void, exactly as drop_packet() voids it
    packet.set_egress_port(0);
    packet.set_register(PACKET_DROP_FLAG_REG_IDX, 0);
    packet.set_register(PACKET_RECIRCULATE_FLAG_REG_IDX, 1);
  }
};

REGISTER_PRIMITIVE(recirculate);

class modify_field_rng_uniform
  : public ActionPrimitive<Data &, const Data &, const Data &> {
  void operator ()(Data &f, const Data &b, const Data &e) {
    // TODO(antonin): a little hacky, fix later if there is a need using GMP
    // random fns
    using engine = std::default_random_engine;
    using hash = std::hash<std::thread::id>;
    static thread_local engine generator(hash()(std::this_thread::get_id()));
    using distrib64 = std::uniform_int_distribution<uint64_t>;
    distrib64 distribution(b.get_uint64(), e.get_uint64());
    f.set(distribution(generator));
  }
};

// Random<T>::read() is translated by the p4c bmv2-pna backend
// (backends/bmv2/portable_common/portable.cpp, ExternConverter_Random) into the
// legacy "modify_field_rng_uniform" primitive rather than an "_Random_read"
// extern-method primitive, so this is the name that has to be registered.
REGISTER_PRIMITIVE(modify_field_rng_uniform);

class register_read
  : public ActionPrimitive<Field &, const RegisterArray &, const Data &> {
  void operator ()(Field &dst, const RegisterArray &src, const Data &idx) {
    auto i = idx.get_uint();
#ifndef NDEBUG
    if (i >= src.size()) {
        BMLOG_ERROR_PKT(get_packet(),
                        "Attempted to read register '{}' with size {}"
                        " at out-of-bounds index {}."
                        "  Dest field was not updated.",
                        src.get_name(), src.size(), i);
        return;
    }
#endif  // NDEBUG
    dst.set(src[i]);
    BMLOG_TRACE_PKT(get_packet(),
                    "Read register '{}' at index {} read value {}",
                    src.get_name(), i, src[i]);
  }
};

REGISTER_PRIMITIVE(register_read);

class register_write
  : public ActionPrimitive<RegisterArray &, const Data &, const Data &> {
  void operator ()(RegisterArray &dst, const Data &idx, const Data &src) {
    auto i = idx.get_uint();
#ifndef NDEBUG
    if (i >= dst.size()) {
        BMLOG_ERROR_PKT(get_packet(),
                        "Attempted to write register '{}' with size {}"
                        " at out-of-bounds index {}."
                        "  No register array elements were updated.",
                        dst.get_name(), dst.size(), i);
        return;
    }
#endif  // NDEBUG
    dst[i].set(src);
    BMLOG_TRACE_PKT(get_packet(),
                    "Wrote register '{}' at index {} with value {}",
                    dst.get_name(), i, dst[i]);
  }
};

REGISTER_PRIMITIVE(register_write);

// extern function: is_host_port / is_net_port
//
// PNA's port classification test. The p4c portable backend lowers a call used
// as an expression into a temporary plus this primitive, passing the temporary
// as the first parameter, so the destination arrives first and the port second.
//
// The two functions are complements and read the same table, which the control
// plane fills in through PnaNic_server::set_port_class; see the declaration of
// port_class_is_host() in pna_nic.h for the default when a port is unconfigured.
class is_host_port : public ActionPrimitive<Data &, const Data &> {
  void operator ()(Data &dest, const Data &port) {
    const bool host =
        port_class_is_host(static_cast<port_t>(port.get<uint64_t>()));
    dest.set(host ? 1 : 0);
  }
};

class is_net_port : public ActionPrimitive<Data &, const Data &> {
  void operator ()(Data &dest, const Data &port) {
    const bool host =
        port_class_is_host(static_cast<port_t>(port.get<uint64_t>()));
    dest.set(host ? 0 : 1);
  }
};

REGISTER_PRIMITIVE(is_host_port);
REGISTER_PRIMITIVE(is_net_port);

}  // namespace bm::pna

}  // namespace pna

// dummy function, which ensures that this unit is not discarded by the linker
// it is being called by the constructor of PnaNic
// the previous alternative was to have all the primitives in a header file (the
// primitives could also be placed in pna_nic.cpp directly), but I need
// this dummy function if I want to keep the primitives in their own file
int import_primitives() {
  return 0;
}
