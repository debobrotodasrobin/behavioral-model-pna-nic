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

#ifndef PNA_NIC_PNA_METER_H_
#define PNA_NIC_PNA_METER_H_

#include <bm/bm_sim/extern.h>
#include <bm/bm_sim/meters.h>

namespace bm {

namespace pna {

class PNA_Meter : public bm::ExternType {
 public:
  static constexpr p4object_id_t spec_id = 0xfffffffe;

  BM_EXTERN_ATTRIBUTES {
    BM_EXTERN_ATTRIBUTE_ADD(n_meters);
    BM_EXTERN_ATTRIBUTE_ADD(type);
    BM_EXTERN_ATTRIBUTE_ADD(is_direct);
    BM_EXTERN_ATTRIBUTE_ADD(rate_count);
  }

  void init() override;

  void execute(const Data &index, Data &value);

  // Colour-aware meter update, for the two-argument PNA form
  // execute(index, color). The existing execute() above always meters as if
  // the packet had never been coloured (pre-colour GREEN, i.e. colour blind);
  // RFC 2698 colour-aware metering needs the packet's existing colour to be
  // taken into account, and there was no way to supply one.
  //
  // pre_color arrives in PNA's enumeration (RED=0, GREEN=1, YELLOW=2) and is
  // translated to bmv2's (GREEN=0, YELLOW=1, RED=2) before the meter call.
  void execute_color(const Data &pre_color, const Data &index, Data &value);

  Meter &get_meter(size_t idx);

  const Meter &get_meter(size_t idx) const;

  Meter::MeterErrorCode set_rates(const std::vector<Meter::rate_config_t> &configs);

  size_t size() const { return _meter->size(); };

 private:
  Data n_meters;
  std::string type;
  Data is_direct;
  Data rate_count;
  Data color;
  std::unique_ptr<MeterArray> _meter;
};

}  // namespace bm::pna

}  // namespace bm
#endif
