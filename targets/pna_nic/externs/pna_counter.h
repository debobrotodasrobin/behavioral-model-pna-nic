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


#ifndef PNA_NIC_PNA_COUNTER_H_
#define PNA_NIC_PNA_COUNTER_H_

#include <cassert>

#include <bm/bm_sim/counters.h>
#include <bm/bm_sim/extern.h>
// init() resolves this counter's array by name through P4Objects, so the type
// has to be complete here (extern.h only forward-declares it).
#include <bm/bm_sim/P4Objects.h>
#include <bm/bm_sim/logger.h>

namespace bm {

namespace pna {

class PNA_Counter : public bm::ExternType {
 public:
  static constexpr p4object_id_t spec_id = 0xffffffff;

  BM_EXTERN_ATTRIBUTES {
    BM_EXTERN_ATTRIBUTE_ADD(n_counters);
    BM_EXTERN_ATTRIBUTE_ADD(type);
  }
  
  void init() override {
    // Adopt the counter array that init_counter_arrays() built from the
    // program's counter_arrays JSON section, instead of keeping private
    // storage of our own.
    //
    // This matters because that JSON array is the one the control plane reads:
    // Context::read_counters() resolves the name the controller asked for via
    // P4Objects::get_counter_array_rt(). An extern-owned array - however
    // correctly incremented by count() - is invisible to it, so every counter
    // read reports zero. Owning a duplicate was the original behaviour here
    // (the array was even named "<name>.$impl", which nothing ever looked up).
    //
    // init() runs after init_counter_arrays(), and ExternType documents name
    // resolution in init() as the intended use of get_p4objects(); see the
    // comment on _set_p4objects() in bm_sim/extern.h.
    _counter = get_p4objects().get_counter_array_rt(get_name());
    if (_counter == nullptr) {
      BMLOG_ERROR("Counter '{}' is not present in the config's counter_arrays;"
                  " count() will be a no-op", get_name());
      return;
    }
    // The P4 declaration and the JSON section should agree on the size; if the
    // JSON is smaller, count() would index past the end.
    if (n_counters.get<size_t>() > _counter->size()) {
      BMLOG_ERROR("Counter '{}' declared with {} entries but config allocated"
                  " {}; count() will ignore larger indices",
                  get_name(), n_counters.get<size_t>(), _counter->size());
    }
  }

  void count(const Data &index);

  Counter &get_counter(size_t idx);

  const Counter &get_counter(size_t idx) const;

  Counter::CounterErrorCode reset_counters();

  size_t size() const {
    return _counter != nullptr ? _counter->size() : 0u;
  };

 private:
  Data n_counters;
  Data type;
  // Owned by P4Objects, which outlives the extern instance; see init().
  CounterArray *_counter{nullptr};
};

}  // namespace bm::pna

}  // namespace bm

#endif
