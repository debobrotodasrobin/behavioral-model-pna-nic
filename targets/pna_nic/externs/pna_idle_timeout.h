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
 * PNA IdleTimeout support.
 *
 * The PNA architecture defines four extern *functions* (pna.p4, not methods
 * on an object) for table-entry ageing:
 *
 *   extern AddEntryErrorStatus_t add_entry<T>(string action_name,
 *                            in T action_params,
 *                            in ExpireTimeProfileId_t expire_time_profile_id);
 *   extern FlowId_t allocate_flow_id();
 *   extern void set_entry_expire_time(in ExpireTimeProfileId_t profile_id);
 *   extern void restart_expire_timer();
 *
 * Because they are free functions, p4c lowers each to a plain data-path
 * primitive (see externs/pna_idle_timeout.cpp), exactly like is_host_port() -
 * there is no extern instance to register.
 *
 * bmv2 already owns the timer machinery (per-entry EntryMeta::ts + timeout_ms,
 * swept by the ageing monitor). What it lacked was a way for the data path to
 * touch those timers, or to insert a table entry from inside an action. The
 * primitives supply both, locating the relevant table/entry through the
 * per-packet tracking added to MatchTableAbstract::apply_action.
 *
 * The expire-time-profile map (profile id -> milliseconds) is a property of
 * the switch, shared by every pipeline, and is configured by the control plane
 * through the pna_nic thrift service (set_expire_time_profile). PNA reserves
 * profile id 0 to mean "expire now" (DASH's EXPIRE_TIME_PROFILE_NOW); it is
 * special-cased in the primitives and does not need a map entry.
 */

#ifndef PNA_NIC_PNA_IDLE_TIMEOUT_H_
#define PNA_NIC_PNA_IDLE_TIMEOUT_H_

#include <cstdint>

namespace bm {

namespace pna {

//! PNA convention: expire-time profile id 0 means "expire the entry now".
constexpr uint8_t PNA_EXPIRE_TIME_PROFILE_NOW = 0;

//! Maps an expire-time profile id to a duration in milliseconds, as
//! configured by the control plane. Thread-safe; read by the data-path
//! primitives, written by the thrift server.
//!
//! A profile id that has never been set has no entry: set_entry_expire_time
//! then leaves the matched entry's timer untouched, and add_entry inserts with
//! ageing disarmed. Profile id 0 is never stored here (see
//! PNA_EXPIRE_TIME_PROFILE_NOW).
void set_expire_time_profile(uint8_t profile_id, uint32_t timeout_ms);

//! Removes a profile mapping. Returns false if it was not present.
bool clear_expire_time_profile(uint8_t profile_id);

//! Reads a configured profile duration. Returns false if unset.
bool get_expire_time_profile(uint8_t profile_id, uint32_t *timeout_ms);

//! Drains the thread-local queue of data-path add_entry() requests recorded
//! during the current packet's pipeline. The target calls this after the
//! pipeline returns (all table locks released), because inserting during the
//! action would self-deadlock on the table lock. No-op if nothing was queued.
void flush_pending_add_entry();

}  // namespace pna

}  // namespace bm

#endif  // PNA_NIC_PNA_IDLE_TIMEOUT_H_
