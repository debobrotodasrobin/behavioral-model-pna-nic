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
 * Data-path primitives for the four PNA IdleTimeout extern functions. See
 * pna_idle_timeout.h for the feature overview and the expire-time-profile
 * contract.
 */

#include "pna_idle_timeout.h"

#include <bm/bm_sim/P4Objects.h>
#include <bm/bm_sim/actions.h>
#include <bm/bm_sim/logger.h>
#include <bm/bm_sim/match_tables.h>
#include <bm/bm_sim/packet.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace bm {

namespace pna {

namespace {

std::mutex g_profile_mutex{};
std::unordered_map<uint8_t, uint32_t> g_expire_time_profiles{};

// "Expire now" (profile 0) is realised by arming a 1 ms timeout with a freshly
// reset timestamp: the ageing sweep fires once now - ts >= timeout, so the
// entry is removed (AUTO_DELETE) or reported (NOTIFY_CONTROL) on the next
// sweep, i.e. within one sweep interval. bmv2 treats timeout_ms == 0 as
// "ageing disarmed", so 0 cannot be used to mean "expire immediately".
constexpr uint32_t PNA_EXPIRE_NOW_MS = 1;

}  // namespace

void
set_expire_time_profile(uint8_t profile_id, uint32_t timeout_ms) {
  std::lock_guard<std::mutex> lock(g_profile_mutex);
  g_expire_time_profiles[profile_id] = timeout_ms;
}

bool
clear_expire_time_profile(uint8_t profile_id) {
  std::lock_guard<std::mutex> lock(g_profile_mutex);
  return g_expire_time_profiles.erase(profile_id) > 0;
}

bool
get_expire_time_profile(uint8_t profile_id, uint32_t *timeout_ms) {
  std::lock_guard<std::mutex> lock(g_profile_mutex);
  auto it = g_expire_time_profiles.find(profile_id);
  if (it == g_expire_time_profiles.end()) return false;
  *timeout_ms = it->second;
  return true;
}

namespace {

// Resolves an expire-time profile id to a timeout in milliseconds.
//   - profile 0 ("expire now") -> PNA_EXPIRE_NOW_MS
//   - configured profile        -> its duration
//   - unconfigured profile      -> \p unset (returned as-is)
uint32_t
resolve_profile(uint8_t profile_id, uint32_t unset) {
  if (profile_id == PNA_EXPIRE_TIME_PROFILE_NOW) return PNA_EXPIRE_NOW_MS;
  uint32_t ms = 0;
  if (get_expire_time_profile(profile_id, &ms)) return ms;
  return unset;
}

// A data-path add_entry() cannot insert into its own table synchronously:
// MatchTableAbstract::apply_action() holds the table's shared lock for the whole
// duration of the action, and MatchTable::add_entry() needs that lock
// exclusively, so calling it from the action self-deadlocks. Instead the
// primitive records the request here and the target flushes it once the
// pipeline has returned and released the lock (see flush_pending_add_entry,
// called from PnaNic::main_thread right after the pipeline apply).
//
// The queue is thread_local because each packet is processed start-to-finish by
// a single worker thread, and the flush runs on that same thread before it
// picks up the next packet, so no locking is needed and a request can never
// bleed across threads.
struct PendingAdd {
  MatchTableAbstract *table;
  ByteContainer key;
  const ActionFn *action_fn;
  ActionData action_data;
  uint32_t timeout_ms;
};
thread_local std::vector<PendingAdd> g_pending_add;

// restart_expire_timer and set_entry_expire_time modify an existing entry's
// idle timer, which requires the table's write lock (set_entry_ttl) and, for
// restart, its read lock (get_entry_ttl). Neither can be taken from inside the
// action: apply_action() already holds the table's read lock for the whole
// action and boost::shared_mutex is non-recursive, so a write lock taken here
// self-deadlocks (and even the nested read lock is unsafe once a writer is
// queued). Like add_entry, the request is recorded here and applied by
// flush_pending_add_entry() once the pipeline has released the lock.
struct PendingTtl {
  MatchTableAbstract *table;
  entry_handle_t handle;
  // false: restart (re-arm with the entry's current timeout, read at flush
  // time). true: set an explicit timeout.
  bool has_timeout;
  uint32_t timeout_ms;
};
thread_local std::vector<PendingTtl> g_pending_ttl;

// The table whose action is running, or nullptr (with a log) if the packet
// never went through a table application. All four primitives need it.
MatchTableAbstract *
current_table(Packet &pkt, const char *who) {
  MatchTableAbstract *table = pkt.get_entry_table();
  if (table == nullptr) {
    BMLOG_ERROR_PKT(pkt, "{}(): no table is currently applying an action", who);
    return nullptr;
  }
  return table;
}

// The matched entry index within \p table, or Packet::INVALID_ENTRY_INDEX
// (with a log) if the table lookup was a miss. restart_expire_timer and
// set_entry_expire_time operate on the matched entry, so they need a hit.
size_t
matched_entry(Packet &pkt, MatchTableAbstract *table, const char *who) {
  const size_t idx = pkt.get_entry_index();
  if (idx == Packet::INVALID_ENTRY_INDEX) {
    BMLOG_ERROR_PKT(pkt, "{}(): table '{}' was a miss, no entry to time",
                    who, table->get_name());
    return Packet::INVALID_ENTRY_INDEX;
  }
  return idx;
}

// extern void restart_expire_timer()
//
// Resets the matched entry's idle timer without changing its timeout. Reads
// the current timeout and re-arms it, which is exactly what set_entry_ttl does
// (it sets the timeout and resets the timestamp together).
class restart_expire_timer : public ActionPrimitive<> {
  void operator ()() {
    auto &pkt = get_packet();
    MatchTableAbstract *table = current_table(pkt, "restart_expire_timer");
    if (table == nullptr) return;
    const size_t idx = matched_entry(pkt, table, "restart_expire_timer");
    if (idx == Packet::INVALID_ENTRY_INDEX) return;
    // Defer: reading the current timeout and re-arming both need the table
    // lock, which apply_action() holds. The flush re-reads the timeout (so a
    // set_entry_expire_time queued earlier in the same action is honoured) and
    // re-arms it, which resets the timestamp.
    g_pending_ttl.push_back(
        PendingTtl{table, static_cast<entry_handle_t>(idx), false, 0});
  }
};

REGISTER_PRIMITIVE(restart_expire_timer);

// extern void set_entry_expire_time(in ExpireTimeProfileId_t profile_id)
//
// Sets the matched entry's timeout to the profile's duration and restarts its
// timer (PNA: "It _also_ behaves as if restart_expire_timer() was called").
// Profile 0 means "expire now". An unconfigured, non-zero profile leaves the
// timer untouched.
class set_entry_expire_time : public ActionPrimitive<const Data &> {
  void operator ()(const Data &profile_id) {
    auto &pkt = get_packet();
    MatchTableAbstract *table = current_table(pkt, "set_entry_expire_time");
    if (table == nullptr) return;
    const size_t idx = matched_entry(pkt, table, "set_entry_expire_time");
    if (idx == Packet::INVALID_ENTRY_INDEX) return;

    const uint8_t id =
        static_cast<uint8_t>(profile_id.get<uint64_t>());
    // unset == UINT32_MAX sentinel: distinguishes "leave timer unchanged" from
    // a legitimate timeout of 0 (which would mean disarmed).
    const uint32_t timeout_ms = resolve_profile(id, UINT32_MAX);
    if (timeout_ms == UINT32_MAX) {
      BMLOG_DEBUG_PKT(pkt,
                      "set_entry_expire_time(): profile {} not configured; "
                      "leaving timer unchanged", id);
      return;
    }
    // Defer the write: set_entry_ttl needs the table's write lock, which
    // apply_action() holds for the duration of the action.
    g_pending_ttl.push_back(
        PendingTtl{table, static_cast<entry_handle_t>(idx), true, timeout_ms});
  }
};

REGISTER_PRIMITIVE(set_entry_expire_time);

// extern FlowId_t allocate_flow_id()
//
// bmv2 addresses entries by handle, not by flow id, so there is nothing for a
// flow id to alias. It is handed out as a monotonically increasing counter so
// a control plane can correlate digest notifications with the flows they
// describe. The value is written to the destination the p4c portable backend
// hoisted the call into (dest-first, like is_host_port).
class allocate_flow_id : public ActionPrimitive<Data &> {
  void operator ()(Data &dest) {
    static std::atomic<uint32_t> next_flow_id{1};
    dest.set(next_flow_id.fetch_add(1));
  }
};

REGISTER_PRIMITIVE(allocate_flow_id);

// extern AddEntryErrorStatus_t add_entry<T>(string action_name,
//                                           in T action_params,
//                                           in ExpireTimeProfileId_t profile)
//
// Re-inserts the key that just missed, bound to the named action and the
// supplied parameters, into the table whose miss action is running, and arms
// its idle timer from the profile. The key is the one captured during lookup
// (Packet::get_lookup_key), so it is correct even if the action modified the
// key's header fields first.
//
// The PNA signature returns AddEntryErrorStatus_t, but the portable backend
// only prepends a destination when that result is actually consumed; DASH (and
// every realistic caller) invokes add_entry() as a statement and discards it,
// so the primitive is emitted with just the three declared arguments and has no
// destination to write. A program that consumes the result would be lowered
// with a destination argument and would not match this primitive; bmv2 does not
// surface the success / not-done status, so that use is not supported.
class add_entry : public ActionPrimitive<const std::string &,
                                         const std::vector<Data>,
                                         const Data &> {
  void operator ()(const std::string &action_name,
                   const std::vector<Data> action_params,
                   const Data &profile_id) {
    auto &pkt = get_packet();
    MatchTableAbstract *table = current_table(pkt, "add_entry");
    if (table == nullptr) return;

    // add_entry is only meaningful on a miss (the key that just missed).
    if (pkt.get_entry_index() != Packet::INVALID_ENTRY_INDEX) {
      BMLOG_DEBUG_PKT(pkt, "add_entry(): table '{}' was a hit; nothing to add",
                      table->get_name());
      return;
    }

    P4Objects *p4objects = get_p4objects();
    // The P4 programmer writes the action's short name (e.g. "allow"); bmv2
    // stores it fully qualified (e.g. "MainControlImpl.allow"), so resolve by
    // short name within the table.
    const ActionFn *action_fn = p4objects->get_action_rt_by_short_name(
        table->get_name(), action_name);
    if (action_fn == nullptr) {
      BMLOG_ERROR_PKT(pkt, "add_entry(): action '{}' not found in table '{}'",
                      action_name, table->get_name());
      return;
    }

    ActionData action_data;
    for (const Data &d : action_params)
      action_data.push_back_action_data(d);

    const uint8_t id =
        static_cast<uint8_t>(profile_id.get<uint64_t>());
    // Unconfigured, non-zero profile -> insert with ageing disarmed (0).
    const uint32_t timeout_ms = resolve_profile(id, 0);

    // Defer the actual insert to after the pipeline releases the table lock
    // (see PendingAdd above). Capture everything now: the key is this table's
    // lookup key for the current packet, valid only during this action.
    g_pending_add.push_back(
        PendingAdd{table, pkt.get_lookup_key(), action_fn,
                   std::move(action_data), timeout_ms});
  }
};

REGISTER_PRIMITIVE(add_entry);

}  // namespace

// Drains the thread-local add_entry queue. Called by the target after the
// pipeline has run and released every table lock, so the inserts can take the
// table's write lock without deadlocking. Safe to call unconditionally; it is a
// no-op when no add_entry() ran during the pipeline.
void flush_pending_add_entry() {
  // Inserts first, then timer updates: a timer op always targets an entry that
  // already exists, so applying adds before ttl changes keeps that invariant
  // even if one packet both learns a new entry in one table and re-arms a
  // timer in another.
  for (auto &p : g_pending_add) {
    entry_handle_t handle = 0;
    const auto rc = p.table->add_entry_from_key(
        p.key, p.action_fn, std::move(p.action_data), &handle, p.timeout_ms);
    if (rc != MatchErrorCode::SUCCESS) {
      BMLOG_DEBUG("add_entry(): deferred insert into '{}' failed: {}",
                  p.table->get_name(), match_error_code_to_string(rc));
    }
  }
  g_pending_add.clear();

  for (auto &t : g_pending_ttl) {
    if (t.has_timeout) {
      const auto rc = t.table->set_entry_ttl(t.handle, t.timeout_ms);
      if (rc != MatchErrorCode::SUCCESS) {
        BMLOG_DEBUG("set_entry_expire_time(): deferred ttl on '{}' handle {} "
                    "failed: {}", t.table->get_name(), t.handle,
                    match_error_code_to_string(rc));
      }
      continue;
    }
    // restart: re-read the current timeout and re-arm it (resets timestamp).
    uint32_t timeout_ms = 0;
    uint32_t time_since_hit_ms = 0;
    if (!t.table->get_entry_ttl(t.handle, &timeout_ms, &time_since_hit_ms)) {
      BMLOG_DEBUG("restart_expire_timer(): no ageing or gone for '{}' handle {}",
                  t.table->get_name(), t.handle);
      continue;
    }
    t.table->set_entry_ttl(t.handle, timeout_ms);
  }
  g_pending_ttl.clear();
}

}  // namespace pna

}  // namespace bm
