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


#include "pna_counter.h"

#include <bm/bm_sim/logger.h>

namespace bm {

namespace pna {

void
PNA_Counter::count(const Data &index) {
  if (_counter == nullptr) return;  // see init()
  const size_t idx = index.get<size_t>();
  // an out-of-range index would assert inside CounterArray; report and skip
  // instead, so one bad data-plane index cannot take the whole NIC down
  if (idx >= _counter->size()) {
    BMLOG_ERROR_PKT(get_packet(),
                    "Counter '{}' index {} out of range (size {}); not counted",
                    get_name(), idx, _counter->size());
    return;
  }
  _counter->get_counter(idx).increment_counter(get_packet());
}

Counter &
PNA_Counter::get_counter(size_t idx) {
  assert(_counter != nullptr && "counter not registered in config");
  return _counter->get_counter(idx);
}

const Counter &
PNA_Counter::get_counter(size_t idx) const {
  assert(_counter != nullptr && "counter not registered in config");
  return _counter->get_counter(idx);
}

Counter::CounterErrorCode
PNA_Counter::reset_counters(){
  if (_counter == nullptr) return Counter::CounterErrorCode::SUCCESS;
  return _counter->reset_counters();
}

BM_REGISTER_EXTERN_W_NAME(Counter, PNA_Counter);
BM_REGISTER_EXTERN_W_NAME_METHOD(Counter, PNA_Counter, count, const Data &);

}  // namespace bm::pna

}  // namespace bm

int import_counters(){
  return 0;
}
