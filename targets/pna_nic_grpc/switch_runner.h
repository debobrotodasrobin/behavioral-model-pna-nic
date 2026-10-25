/* Copyright 2013-present Barefoot Networks, Inc.
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
 * P4Runtime (gRPC) runner for the PNA NIC target, modelled on
 * simple_switch_grpc's SimpleSwitchGrpcRunner.
 */

#ifndef PNA_NIC_GRPC_SWITCH_RUNNER_H_
#define PNA_NIC_GRPC_SWITCH_RUNNER_H_

#include <memory>
#include <string>

namespace bm {

class OptionsParser;

namespace pna {
class PnaNic;
}  // namespace pna

}  // namespace bm

namespace pnic_grpc {

struct SSLOptions {
  std::string pem_root_certs;
  std::string pem_private_key;
  std::string pem_cert_chain;
  bool with_client_auth;
};

class PnaNicGrpcRunner {
 public:
  // singleton, as in simple_switch_grpc: the PI gRPC server API uses static
  // state, so at most one runner can exist per process.
  static PnaNicGrpcRunner &get_instance(
      bool enable_swap = false,
      std::string grpc_server_addr = "0.0.0.0:9559",
      std::shared_ptr<SSLOptions> ssl_options = nullptr) {
    static PnaNicGrpcRunner instance(
        enable_swap, grpc_server_addr, ssl_options);
    return instance;
  }

  int init_and_start(const bm::OptionsParser &parser);
  void wait();
  void shutdown();

  ~PnaNicGrpcRunner();

 private:
  PnaNicGrpcRunner(bool enable_swap,
                   std::string grpc_server_addr,
                   std::shared_ptr<SSLOptions> ssl_options);

  std::unique_ptr<bm::pna::PnaNic> pna_nic;
  std::string grpc_server_addr;
  std::shared_ptr<SSLOptions> ssl_options;
};

}  // namespace pnic_grpc

#endif  // PNA_NIC_GRPC_SWITCH_RUNNER_H_
