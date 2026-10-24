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

namespace cpp pnic_runtime
namespace py pnic_runtime

// Destination of a mirror session. PNA mirrors to a port; a NIC has no egress
// pipeline, no tunnel encapsulation and no PRE, so a port is all a session
// needs.
struct MirroringSessionConfig {
  1:optional i32 egress_port,
}

service PnaNic {

  // these methods are here as an experiment, prefer get_time_elapsed_us() when
  // possible
  i64 get_time_elapsed_us();
  i64 get_time_since_epoch_us();

  // Mirror session control plane for the mirror_packet() architecture extern.
  // mirroring_session_add / _delete return 1 on success and 0 on failure.
  // mirroring_session_get_egress_port returns the configured port, or -1 when
  // the session is not configured.
  i32 mirroring_session_add(1:i32 session_id, 2:i32 egress_port);
  i32 mirroring_session_delete(1:i32 session_id);
  i32 mirroring_session_get_egress_port(1:i32 session_id);

  // Port classification, backing the is_host_port() / is_net_port()
  // architecture externs. Every port of a PNA NIC is either host-facing or
  // network-facing, and bmv2 cannot infer which, so the control plane says so
  // here: set_port_class(port, true) marks a port host-facing, false marks it
  // network-facing. A port that has never been set is network-facing.
  // get_port_class returns 1 for a host port, 0 for a network port.
  void set_port_class(1:i32 port, 2:bool is_host);
  i32 get_port_class(1:i32 port);

}
