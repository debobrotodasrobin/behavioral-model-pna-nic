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
 * pna_nic_grpc: the PNA NIC target (pna_nic) with a P4Runtime gRPC control
 * plane, so a single P4Runtime controller can drive PNA NICs and v1model
 * switches (simple_switch_grpc) uniformly.
 */

#include <bm/bm_sim/options_parse.h>
#include <bm/bm_sim/target_parser.h>
#include <bm/bm_grpc/pem.h>

#include <exception>
#include <iostream>
#include <memory>
#include <string>

#include "switch_runner.h"

int
main(int argc, char* argv[]) {
  bm::TargetParserBasicWithDynModules pna_nic_parser;
  pna_nic_parser.add_flag_option(
      "disable-swap",
      "Disable JSON swapping at runtime; this is not recommended when using "
      "P4Runtime!");
  pna_nic_parser.add_string_option(
      "grpc-server-addr",
      "Bind gRPC server to given address [default is 0.0.0.0:9559]");
  pna_nic_parser.add_flag_option(
      "grpc-server-ssl",
      "Enable SSL/TLS for gRPC server");
  pna_nic_parser.add_string_option(
      "grpc-server-cacert",
      "Path to pem file holding CA certificate to verify peer against");
  pna_nic_parser.add_string_option(
      "grpc-server-cert",
      "Path to pem file holding server certificate");
  pna_nic_parser.add_string_option(
      "grpc-server-key",
      "Path to pem file holding server key");
  pna_nic_parser.add_flag_option(
      "grpc-server-with-client-auth",
      "Require client to have a valid certificate for mutual authentication");

  bm::OptionsParser parser;
  parser.parse(argc, argv, &pna_nic_parser);

  bool disable_swap_flag = false;
  {
    auto rc = pna_nic_parser.get_flag_option(
        "disable-swap", &disable_swap_flag);
    if (rc != bm::TargetParserBasic::ReturnCode::SUCCESS) std::exit(1);
  }

  std::string grpc_server_addr;
  {
    auto rc = pna_nic_parser.get_string_option(
        "grpc-server-addr", &grpc_server_addr);
    if (rc == bm::TargetParserBasic::ReturnCode::OPTION_NOT_PROVIDED)
      grpc_server_addr = "0.0.0.0:9559";
    else if (rc != bm::TargetParserBasic::ReturnCode::SUCCESS)
      std::exit(1);
  }

  bool grpc_server_ssl = false;
  {
    auto rc = pna_nic_parser.get_flag_option(
        "grpc-server-ssl", &grpc_server_ssl);
    if (rc != bm::TargetParserBasic::ReturnCode::SUCCESS) std::exit(1);
  }

  std::string grpc_server_cacert;
  {
    auto rc = pna_nic_parser.get_string_option(
        "grpc-server-cacert", &grpc_server_cacert);
    if (rc == bm::TargetParserBasic::ReturnCode::OPTION_NOT_PROVIDED)
      grpc_server_cacert = "";
    else if (rc != bm::TargetParserBasic::ReturnCode::SUCCESS)
      std::exit(1);
  }

  std::string grpc_server_cert;
  {
    auto rc = pna_nic_parser.get_string_option(
        "grpc-server-cert", &grpc_server_cert);
    if (rc == bm::TargetParserBasic::ReturnCode::OPTION_NOT_PROVIDED)
      grpc_server_cert = "";
    else if (rc != bm::TargetParserBasic::ReturnCode::SUCCESS)
      std::exit(1);
  }

  std::string grpc_server_key;
  {
    auto rc = pna_nic_parser.get_string_option(
        "grpc-server-key", &grpc_server_key);
    if (rc == bm::TargetParserBasic::ReturnCode::OPTION_NOT_PROVIDED)
      grpc_server_key = "";
    else if (rc != bm::TargetParserBasic::ReturnCode::SUCCESS)
      std::exit(1);
  }

  bool grpc_server_with_client_auth = false;
  {
    auto rc = pna_nic_parser.get_flag_option(
        "grpc-server-with-client-auth", &grpc_server_with_client_auth);
    if (rc != bm::TargetParserBasic::ReturnCode::SUCCESS) std::exit(1);
  }

  if (grpc_server_ssl && grpc_server_cert == "") {
    std::cerr << "When enabling SSL/TLS for gRPC server, "
        << "--grpc-server-cert is required\n";
    std::exit(1);
  }
  if (grpc_server_ssl && grpc_server_key == "") {
    std::cerr << "When enabling SSL/TLS for gRPC server, "
        << "--grpc-server-key is required\n";
    std::exit(1);
  }

  auto ssl_options = std::make_shared<pnic_grpc::SSLOptions>();
  try {
    if (grpc_server_ssl) {
      if (grpc_server_cacert != "") {
        ssl_options->pem_root_certs = bm::read_pem_file(grpc_server_cacert);
      }
      if (grpc_server_cert != "") {
        ssl_options->pem_cert_chain = bm::read_pem_file(grpc_server_cert);
      }
      if (grpc_server_key != "") {
        ssl_options->pem_private_key = bm::read_pem_file(grpc_server_key);
      }
      ssl_options->with_client_auth = grpc_server_with_client_auth;
    }
  } catch (const bm::read_pem_exception &e) {
    std::cerr << e.msg();
    std::exit(1);
  }

  auto &runner = pnic_grpc::PnaNicGrpcRunner::get_instance(
      !disable_swap_flag,
      grpc_server_addr,
      grpc_server_ssl ? ssl_options : nullptr);
  int status = runner.init_and_start(parser);
  if (status != 0) std::exit(status);

  runner.wait();
  return 0;
}
