/* Copyright 2013-present Barefoot Networks, Inc.
 * Copyright 2024 Marvell Technology, Inc.
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
 * P4Runtime (gRPC) runner for the PNA NIC target.
 *
 * Modelled on simple_switch_grpc's switch_runner.cpp, with the pieces that do
 * not apply to PNA removed:
 *  - no CPU port / packet-in / packet-out (PNA has no CPU port concept),
 *  - no gNMI dataplane-interface service,
 *  - no sysrepo,
 *  - no drop-port / multi-queue constructor parameters.
 *
 * What is kept identical to simple_switch_grpc:
 *  - the PI gRPC P4Runtime server (PIGrpcServer*), so a single P4Runtime
 *    controller can drive this NIC exactly as it drives the switches,
 *  - capture of bmv2 notifications (LEA digests / AGE idle-timeout / SWP
 *    config swap) and forwarding to PI as P4Runtime StreamMessages,
 *  - optional Thrift runtime server (the "standard" service plus the
 *    target-specific "pna_nic" service: port classification and expire-time
 *    profiles, which have no P4Runtime equivalent).
 */

#include <bm/bm_sim/_assert.h>
#include <bm/bm_sim/logger.h>
#include <bm/bm_sim/options_parse.h>
#include <bm/bm_sim/transport.h>
#include <bm/PI/pi.h>
#include <PI/frontends/proto/logging.h>
#include <PI/proto/pi_server.h>
#include <PI/p4info/digests.h>
#include <PI/pi.h>
#include <PI/target/pi_learn_imp.h>

#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <typeinfo>

#include "pna_nic.h"
#include "switch_runner.h"
// Link anchor (see simple_switch_link_anchor below).
#include "simple_switch.h"

#ifdef WITH_THRIFT
#include <bm/PnaNic.h>
#include <bm/bm_runtime/bm_runtime.h>
namespace pnic_runtime {
using ::std::shared_ptr;
shared_ptr<PnaNicIf> get_handler(bm::pna::PnaNic *nic);
}  // namespace pnic_runtime
#endif

namespace pnic_grpc {

// bmv2's PI bridge (PI/src/pi_clone_imp.cpp) dynamic_casts the registered
// switch to SimpleSwitch, which references SimpleSwitch's typeinfo and its
// mirroring_add_session/mirroring_delete_session member functions. Those live
// in simple_switch.o, a member of the libsimpleswitch convenience library
// that gets flattened into this target's static archive; the linker scans the
// archive before libbmpi.so and would never extract that member, leaving the
// dynamic_cast's symbols undefined at link time. Taking the typeid here
// forces the extraction. This does not change behaviour: the dynamic_cast in
// pi_clone_imp.cpp still fails for PnaNic (it is not a SimpleSwitch), so the
// P4Runtime CloneSession RPC correctly returns NOT_IMPLEMENTED.
// (Non-const pointer on purpose: a namespace-scope const would have internal
// linkage and could be discarded by the optimiser, defeating the anchor.)
const void *simple_switch_link_anchor = &typeid(SimpleSwitch);

// P4Runtime supports sending notifications to the client which are
// "traditionally" sent using nanomsg PUBSUB messages: table entry idle time
// notifications & learning notifications. This target "captures" these
// notifications by providing a custom TransportIface implementation to the
// Switch base class (identical to simple_switch_grpc).
class NotificationsCapture : public bm::TransportIface {
 public:
  explicit NotificationsCapture(bm::SwitchWContexts *sw)
      : sw(sw) { }

 private:
  static constexpr size_t hdr_size = 32u;

  using Lock = std::lock_guard<std::mutex>;

  struct LEA_hdr_t {
    char sub_topic[4];
    uint64_t switch_id;
    uint32_t cxt_id;
    int list_id;
    uint64_t buffer_id;
    unsigned int num_samples;
  } __attribute__((packed));

  struct AGE_hdr_t {
    char sub_topic[4];
    uint64_t switch_id;
    uint32_t cxt_id;
    uint64_t buffer_id;
    int table_id;
    unsigned int num_entries;
  } __attribute__((packed));

  struct SWP_hdr_t {
    char sub_topic[4];
    uint64_t switch_id;
    uint32_t cxt_id;
    int status;
    char _padding[12];
  } __attribute__((packed));

  static_assert(sizeof(LEA_hdr_t) == hdr_size,
                "Invalid size for notification header");
  static_assert(sizeof(AGE_hdr_t) == hdr_size,
                "Invalid size for notification header");
  static_assert(sizeof(SWP_hdr_t) == hdr_size,
                "Invalid size for notification header");

  int send_generic(const std::string &msg) const {
    if (msg.size() < hdr_size) return 1;
    // all notification headers have size 32 bytes, padded at the end if needed
    std::aligned_storage<hdr_size>::type storage;
    std::memcpy(&storage, msg.data(), sizeof(storage));
    const char *data = msg.data() + hdr_size;
    Lock lock(mutex);
    if (!memcmp("SWP|", msg.data(), 4)) {
      handle_SWP(reinterpret_cast<const SWP_hdr_t *>(&storage));
    } else if (!memcmp("LEA|", msg.data(), 4)) {
      handle_LEA(reinterpret_cast<const LEA_hdr_t *>(&storage),
                 data, msg.size() - hdr_size);
    } else if (!memcmp("AGE|", msg.data(), 4)) {
      handle_AGE(reinterpret_cast<const AGE_hdr_t *>(&storage),
                 data, msg.size() - hdr_size);
    }
    return 0;
  }

  // we use Swap notifications to ensure that learning & ageing notifications
  // are not sent to PI / P4Runtime during the config swap process, which is a
  // requirement of the p4lang P4Runtime implementation.
  void handle_SWP(const SWP_hdr_t *hdr) const {
    if (hdr->cxt_id != 0) {
      return;
    }
    enum SwapStatus {
      NEW_CONFIG_LOADED = 0,
      SWAP_REQUESTED = 1,
      SWAP_COMPLETED = 2,
      SWAP_CANCELLED = 3
    };
    if (static_cast<SwapStatus>(hdr->status) == NEW_CONFIG_LOADED) {
      ongoing_swap = true;
    } else if (static_cast<SwapStatus>(hdr->status) == SWAP_COMPLETED ||
               static_cast<SwapStatus>(hdr->status) == SWAP_CANCELLED) {
      ongoing_swap = false;
    }
  }

  void handle_LEA(const LEA_hdr_t *hdr, const char *data, size_t size) const {
    // do not send notifications to PI if there is an ongoing swap; this is a
    // requirement of the p4lang P4Runtime implementation.
    if (ongoing_swap) {
      BMLOG_TRACE(
          "Ignoring LEA notification because of ongoing dataplane swap");
      return;
    }
    const auto *learn_engine = sw->get_learn_engine(0);
    std::string list_name;
    if (learn_engine->list_get_name_from_id(hdr->list_id, &list_name) !=
        bm::LearnEngineIface::LearnErrorCode::SUCCESS) {
      bm::Logger::get()->error(
          "Ignoring LEA notification with unknown learn list id {}",
          hdr->list_id);
      return;
    }
    auto *p4info = pi_get_device_p4info(hdr->switch_id);
    if (p4info == nullptr) {
      bm::Logger::get()->error(
          "Ignoring LEA notification for device {} which has no p4info",
          hdr->switch_id);
      return;
    }
    pi_p4_id_t pi_id = pi_p4info_digest_id_from_name(p4info, list_name.c_str());
    if (pi_id == PI_INVALID_ID) {
      bm::Logger::get()->error(
          "Ignoring LEA notification whose name '{}' cannot be found in p4info",
          list_name);
      return;
    }
    size_t data_size = pi_p4info_digest_data_size(p4info, pi_id);
    if (data_size != size / hdr->num_samples) {
      bm::Logger::get()->error(
          "Dropping LEA notification with name '{}' because of unexpected "
          "digest size", list_name);
      return;
    }
    // Arguably this part of the code should be in PI/src/pi_learn_imp.cpp,
    // along with the pi_learn_msg_done implementation (which releases the
    // memory allocated here).
    pi_learn_msg_t *pi_msg = new pi_learn_msg_t;
    pi_msg->dev_tgt.dev_id = hdr->switch_id;
    pi_msg->dev_tgt.dev_pipe_mask = hdr->cxt_id;
    pi_msg->learn_id = pi_id;
    pi_msg->msg_id = hdr->buffer_id;
    pi_msg->num_entries = hdr->num_samples;
    pi_msg->entry_size = data_size;
    pi_msg->entries = new char[size];
    std::memcpy(pi_msg->entries, data, size);
    pi_learn_new_msg(pi_msg);
  }

  void handle_AGE(const AGE_hdr_t *hdr, const char *data, size_t size) const {
    (void) size;
    // do not send notifications to PI if there is an ongoing swap; this is a
    // requirement of the p4lang P4Runtime implementation.
    if (ongoing_swap) {
      BMLOG_TRACE(
          "Ignoring AGE notification because of ongoing dataplane swap");
      return;
    }

    const auto *ageing_monitor = sw->get_ageing_monitor(0);
    auto table_name = ageing_monitor->get_table_name_from_id(hdr->table_id);
    if (table_name == "") {
      bm::Logger::get()->error(
          "Ignoring AGE notification with unknown table id {}", hdr->table_id);
      return;
    }
    auto *p4info = pi_get_device_p4info(hdr->switch_id);
    if (p4info == nullptr) {
      bm::Logger::get()->error(
          "Ignoring AGE notification for device {} which has no p4info",
          hdr->switch_id);
      return;
    }
    pi_p4_id_t pi_id = pi_p4info_table_id_from_name(p4info, table_name.c_str());
    if (pi_id == PI_INVALID_ID) {
      bm::Logger::get()->error(
          "Ignoring AGE notification for table whose name '{}' "
          "cannot be found in p4info", table_name);
      return;
    }

    auto *handles = reinterpret_cast<const uint32_t *>(data);
    for (unsigned int i = 0; i < hdr->num_entries; i++) {
      bm::pi::table_idle_timeout_notify(
          hdr->switch_id, pi_id, static_cast<pi_entry_handle_t>(handles[i]));
    }
  }

  int open_() override {
    return 0;
  }

  int send_(const std::string &msg) const override {
    return send_generic(msg);
  }

  int send_(const char *msg, int len) const override {
    return send_generic(std::string(msg, len));
  }

  int send_msgs_(
      const std::initializer_list<std::string> &msgs) const override {
    std::string buf;
    for (const auto &msg : msgs) buf.append(msg);
    return send_generic(buf);
  }

  int send_msgs_(const std::initializer_list<MsgBuf> &msgs) const override {
    std::string buf;
    for (const auto &msg : msgs) buf.append(msg.buf, msg.len);
    return send_generic(buf);
  }

  mutable std::mutex mutex{};
  mutable bool ongoing_swap{false};
  bm::SwitchWContexts *sw;
};

namespace {

// PNA numbers the meter colour enumeration RED=0, GREEN=1, YELLOW=2, inverted
// relative to bmv2's internal GREEN=0, YELLOW=1, RED=2. This is the same
// mapping PNA_Meter::execute applies at the indirect-meter boundary; a direct
// meter has no such boundary (the core writes its colour straight into
// result_target), so the mapping is registered here instead. Keep it in sync
// with the switch in externs/pna_meter.cpp (and with targets/pna_nic/main.cpp).
bm::Meter::color_t pna_direct_meter_color(bm::Meter::color_t c) {
  switch (c) {
    case 0: return 1;  // bmv2 GREEN  -> PNA GREEN=1
    case 1: return 2;  // bmv2 YELLOW -> PNA YELLOW=2
    case 2: return 0;  // bmv2 RED    -> PNA RED=0
    default:
      bm::Logger::get()->warn(
          "pna_nic_grpc: direct-meter colour {} out of range (0..2); using GREEN", c);
      return 1;  // PNA GREEN
  }
}

}  // namespace

PnaNicGrpcRunner::PnaNicGrpcRunner(
    bool enable_swap,
    std::string grpc_server_addr,
    std::shared_ptr<SSLOptions> ssl_options)
    : pna_nic(new bm::pna::PnaNic(enable_swap)),
      grpc_server_addr(grpc_server_addr),
      ssl_options(ssl_options) {
  // Register before any config load or packet processing so every direct
  // meter's colour is written in PNA's encoding.
  bm::set_direct_meter_color_translation(&pna_direct_meter_color);
  PIGrpcServerInit();
}

int
PnaNicGrpcRunner::init_and_start(const bm::OptionsParser &parser) {
  auto my_transport = std::make_shared<NotificationsCapture>(
      pna_nic.get());
  int status = pna_nic->init_from_options_parser(
      parser, std::move(my_transport), nullptr);
  if (status != 0) return status;

  if (parser.option_was_provided("notifications-addr")) {
    bm::Logger::get()->warn(
        "You provided --notifications-addr but this target captures all "
        "notifications and does not generate nanomsg messages");
  }

  // PNA has no CPU port, so P4Runtime packet-in / packet-out is not
  // supported; cpu_port=0 disables it in the PI bridge.
  bm::pi::register_switch(pna_nic.get(), 0);

  {
    using pi::fe::proto::LogWriterIface;
    using pi::fe::proto::LoggerConfig;
    class P4RuntimeLogger : public LogWriterIface {
      void write(Severity severity, const char *msg) override {
        auto severity_map = [&severity]() {
          namespace spdL = spdlog::level;
          switch (severity) {
            case Severity::TRACE : return spdL::trace;
            case Severity::DEBUG: return spdL::debug;
            case Severity::INFO: return spdL::info;
            case Severity::WARN: return spdL::warn;
            case Severity::ERROR: return spdL::err;
            case Severity::CRITICAL: return spdL::critical;
          }
          return spdL::off;
        };
        bm::Logger::get()->log(severity_map(), "[P4Runtime] {}", msg);
      }
    };
    LoggerConfig::set_writer(std::make_shared<P4RuntimeLogger>());
  }

  PIGrpcServerSSLOptions_t pi_ssl_options;
  if (ssl_options != nullptr) {
    pi_ssl_options.pem_root_certs = ssl_options->pem_root_certs.c_str();
    pi_ssl_options.pem_private_key = ssl_options->pem_private_key.c_str();
    pi_ssl_options.pem_cert_chain = ssl_options->pem_cert_chain.c_str();
    if (ssl_options->with_client_auth) {
      pi_ssl_options.client_auth =
          PI_GRPC_SSL_REQUEST_AND_REQUIRE_CLIENT_CERTIFICATE_AND_VERIFY;
    } else {
      pi_ssl_options.client_auth =
          PI_GRPC_SSL_DONT_REQUEST_CLIENT_CERTIFICATE;
    }
  }
  PIGrpcServerRunV2(grpc_server_addr.c_str(),
                    nullptr,
                    (ssl_options != nullptr) ? &pi_ssl_options : nullptr);

#ifdef WITH_THRIFT
  // The Thrift server stays available for the target-specific "pna_nic"
  // service (port classification, expire-time profiles), which has no
  // P4Runtime equivalent. The "standard" match-action service is also
  // exposed, but a P4Runtime controller should use gRPC instead.
  int thrift_port = pna_nic->get_runtime_port();
  bm_runtime::start_server(pna_nic.get(), thrift_port);
  using ::pnic_runtime::PnaNicIf;
  using ::pnic_runtime::PnaNicProcessor;
  bm_runtime::add_service<PnaNicIf, PnaNicProcessor>(
      "pna_nic", pnic_runtime::get_handler(pna_nic.get()));
#endif

  pna_nic->start_and_return();

  return 0;
}

void
PnaNicGrpcRunner::wait() {
  PIGrpcServerWait();
}

void
PnaNicGrpcRunner::shutdown() {
  PIGrpcServerShutdown();
}

PnaNicGrpcRunner::~PnaNicGrpcRunner() {
  PIGrpcServerCleanup();
}

}  // namespace pnic_grpc
