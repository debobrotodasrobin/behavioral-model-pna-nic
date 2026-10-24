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

#include <bm/config.h>
#include <bm/PnaNic.h>

#include <thrift/protocol/TBinaryProtocol.h>
#include <thrift/server/TSimpleServer.h>
#include <thrift/transport/TServerSocket.h>
#include <thrift/transport/TBufferTransports.h>

namespace thrift_provider = apache::thrift;

#include <bm/bm_sim/switch.h>
#include <bm/bm_sim/logger.h>
#include <bm/thrift/stdcxx.h>

#include "pna_nic.h"

using namespace bm::pna;

namespace pnic_runtime {

class PnaNicHandler : virtual public PnaNicIf {
 public:
  explicit PnaNicHandler(PnaNic *nic)
    : nic_(nic) { }

  int64_t get_time_elapsed_us() {
    bm::Logger::get()->trace("get_time_elapsed_us");
    // cast from unsigned to signed
    return static_cast<int64_t>(nic_->get_time_elapsed_us());
  }

  int64_t get_time_since_epoch_us() {
    bm::Logger::get()->trace("get_time_since_epoch_us");
    // cast from unsigned to signed
    return static_cast<int64_t>(nic_->get_time_since_epoch_us());
  }

  // Mirror session control plane backing the mirror_packet() extern function.
  // Returns 1 on success, 0 on failure.
  int32_t mirroring_session_add(const int32_t session_id,
                                const int32_t egress_port) {
    bm::Logger::get()->trace("mirroring_session_add");
    PnaNic::MirroringSessionConfig config;
    config.egress_port = static_cast<bm::port_t>(egress_port);
    return nic_->mirroring_session_add(static_cast<int>(session_id), config) ? 1 : 0;
  }

  int32_t mirroring_session_delete(const int32_t session_id) {
    bm::Logger::get()->trace("mirroring_session_delete");
    return nic_->mirroring_session_delete(static_cast<int>(session_id)) ? 1 : 0;
  }

  // Returns the configured egress port, or -1 when the session is unknown.
  int32_t mirroring_session_get_egress_port(const int32_t session_id) {
    bm::Logger::get()->trace("mirroring_session_get_egress_port");
    PnaNic::MirroringSessionConfig config;
    if (!nic_->mirroring_session_get(static_cast<int>(session_id), &config))
      return -1;
    return static_cast<int32_t>(config.egress_port);
  }

  // Port classification for the is_host_port() / is_net_port() externs. See
  // the declaration of port_class_set() in pna_nic.h for the default applied to
  // a port that has never been configured.
  void set_port_class(const int32_t port, const bool is_host) {
    bm::Logger::get()->trace("set_port_class");
    port_class_set(static_cast<bm::port_t>(port), is_host);
  }

  int32_t get_port_class(const int32_t port) {
    bm::Logger::get()->trace("get_port_class");
    return port_class_is_host(static_cast<bm::port_t>(port)) ? 1 : 0;
  }

 private:
  PnaNic *nic_;
};

stdcxx::shared_ptr<PnaNicIf> get_handler(PnaNic *nic) {
  return stdcxx::shared_ptr<PnaNicHandler>(new PnaNicHandler(nic));
}

}  // namespace pnic_runtime
