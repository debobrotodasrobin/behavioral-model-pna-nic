# pna_nic: Full PNA Feature Support

This branch implements all missing PNA features in the `pna_nic` target (bmv2), enabling it to support the complete PNA specification.

**Companion repo**: [p4c-pna-bmv2](https://github.com/debobrotodasrobin/p4c-pna-bmv2) (compiler-side changes)

## Summary

| Metric | Value |
|---|---|
| Files changed | 14 |
| Lines added | +734 |
| Lines removed | -11 |
| Net | +723 |
| Impact on simple_switch | **Zero** (colour hook defaults to identity) |
| Impact on psa_switch | **Zero** (execute_color is additive, never called unless used) |

## Features implemented

### 1. Core primitives (Tier 1)

| Primitive | Description |
|---|---|
| `drop_packet()` | Sets the drop flag in packet register 1 |
| `send_to_port(port)` | Transmits the packet out the given port |
| `register_read(dest, reg, idx)` | Reads a register cell |
| `register_write(reg, idx, val)` | Writes a register cell |
| `modify_field_rng_uniform(...)` | Random number generation (used by `Random<T>.read()`) |

**Files**: `targets/pna_nic/primitives.cpp`

### 2. mirror_packet (Tier 2a)

Copies the packet to a mirror session port. Uses bit-packed packet register 2 (bits 0-15, offset by 1) to carry the session ID through the pipeline.

**Files**: `targets/pna_nic/primitives.cpp`, `targets/pna_nic/pna_nic.{h,cpp}`

### 3. recirculate (Tier 2b)

Re-injects the packet into the ingress pipeline. Uses packet register 3 as the recirculate flag. The packet is re-parsed from scratch on the second pass.

**Files**: `targets/pna_nic/primitives.cpp`, `targets/pna_nic/pna_nic.{h,cpp}`

### 4. Digest / generate_digest (Tier 3b)

`Digest<T>.pack()` lowers to `generate_digest(field_list_id)`. The digest is sent via nanomsg notifications. The learn list goes in the JSON `learn_lists` section.

**Files**: `targets/pna_nic/primitives.cpp`, `targets/pna_nic/pna_nic.{h,cpp}`

### 5. Counter extern

`Counter<W,I>` with `count()`, `read()`, `reset()` methods. Uses `ExternType::init()` hook (runs after `init_counter_arrays()`) to resolve the counter array by name.

**Files**: `targets/pna_nic/externs/pna_counter.{h,cpp}`

### 6. Port classification (is_host_port / is_net_port)

Namespace-scope `port_class_set()` / `port_class_is_host()` with a mutex-guarded `unordered_set<port_t>`. Default: unconfigured port = network. Exposed via thrift RPCs `set_port_class(port, is_host)` and `get_port_class(port)`.

**Files**: `targets/pna_nic/pna_nic.{h,cpp}`, `targets/pna_nic/thrift/pna_nic.thrift`, `targets/pna_nic/thrift/src/PnaNic_server.cpp`

### 7. Meter execute_color()

`Meter.execute(index, color)` now works correctly. `PNA_Meter::execute_color()` translates the PNA colour enum (RED=0, GREEN=1, YELLOW=2) to bmv2's (GREEN=0, YELLOW=1, RED=2) before calling `MeterArray::execute_meter(pkt, idx, pre_color)`.

**Files**: `targets/pna_nic/externs/pna_meter.{h,cpp}`, `targets/psa_switch/externs/psa_meter.{h,cpp}`

### 8. DirectMeter colour translation (core bmv2)

bmv2's direct meter writes its colour straight into `result_target` with no target hook. PSA/PNA use a different colour numbering than bmv2. Added `set_direct_meter_color_translation()` hook in core bmv2 (`match_tables.h/cpp`), applied at `match_tables.cpp:117`. Default is identity (v1model unaffected). `pna_nic/main.cpp` registers the PNA mapping.

**Files**: `include/bm/bm_sim/match_tables.h`, `src/bm_sim/match_tables.cpp`, `targets/pna_nic/main.cpp`

### 9. DirectCounter

No target change needed. bmv2 auto-increments `EntryMeta::counter` on every table hit (`match_units.cpp:795-801`). The p4c fix (emitting `with_counters: true`) is sufficient.

### 10. IdleTimeout ageing externs (add_entry / restart_expire_timer / set_entry_expire_time / allocate_flow_id)

The four PNA IdleTimeout data-path externs plus the `AUTO_DELETE` idle-timeout
table behaviour that DASH conntrack relies on.

Core (`bm_sim`):
- `Packet` tracks the table applying an action (`entry_table`) and, for
  idle-timeout tables, the lookup key captured during lookup (`lookup_key`), so
  a data-path `add_entry()` re-inserts exactly the key that just missed even if
  the action mutated the key's header fields first.
- `MatchTableAbstract` gains an `IdleTimeoutMode` (`NO_TIMEOUT` /
  `NOTIFY_CONTROL` / `AUTO_DELETE`), `add_entry_from_key()` and
  `delete_entry_handle()` virtuals, and `build_key()`.
- `MatchKeyBuilder::key_to_match_params()` rebuilds match-key params from a raw
  key (EXACT / VALID / LPM / TERNARY).
- `AgeingMonitor::do_sweep()` deletes expired entries outright in `AUTO_DELETE`
  mode instead of notifying the control plane.
- `P4Objects` reads the `pna_idle_timeout` table property and resolves short
  action names for data-path `add_entry()`.

Target (`externs/pna_idle_timeout.{h,cpp}`): the four primitives plus an
expire-time-profile map. `add_entry` / `restart_expire_timer` /
`set_entry_expire_time` defer their table mutation to a post-pipeline flush
(`flush_pending_add_entry()`, called from `PnaNic::main_thread` after the
pipeline) because `apply_action()` holds the table's read lock for the whole
action and `boost::shared_mutex` is non-recursive — a synchronous write lock
there self-deadlocks. Profile 0 ("expire now") arms a 1 ms timeout (bmv2 treats
`timeout_ms == 0` as "ageing disarmed").

## Packet register layout

| Register | Purpose |
|---|---|
| 0 | Packet length |
| 1 | Drop flag |
| 2 | Mirror session (bits 0-15, +1) \| Digest field-list id (bits 32-47, +1) |
| 3 | Recirculate flag |

Register 2 is bit-packed (bmv2 convention). ALL reads/writes go through masked accessors in `pna_nic.h`.

## Thrift RPCs added

| RPC | Description |
|---|---|
| `set_port_class(port, is_host)` | Configure port as host or network |
| `get_port_class(port)` | Query port classification |
| `set_expire_time_profile(profile_id, timeout_ms)` | Configure an expire-time profile |
| `clear_expire_time_profile(profile_id)` | Remove an expire-time profile |
| `get_expire_time_profile(profile_id)` | Query a profile's timeout |

## Build

```bash
cd behavioral-model
./autogen.sh && ./configure --with-pna-nic && make -j$(nproc) && sudo make install
```

## Testing

13 feature suites (incl. `ft_ageing.sh`) + Mininet NIC+fabric demo (6/6) +
compiler A/B (PNA corpus, identical compilable set) + DASH conntrack
(`-DPNA_CONNTRACK`) compiles and loads in `pna_nic`.

## Spec-conformance audit (2026-10-25)

Every extern, table property, type and metadata field in the authoritative PNA
spec (`p4lang/pna`) was checked against this target, the p4c PNA backend, and
the `bmv2/pna.p4` header. Outcomes:

- **NOTIFY_CONTROL idle timeout** is now runtime-verified (`ft_notify.sh`): an
  expired entry survives (not deleted) and an ageing notification fires on the
  nanomsg socket. Previously only AUTO_DELETE was exercised.
- The three "no-if-in-action" wrappers (`add_entry_if`,
  `set_entry_expire_time_if`, `update_expire_info`) were **removed** from
  `bmv2/pna.p4` (p4c repo). They were declared but never functional (they crash
  p4c) and are pure sugar for a guarded base-extern call; this target supports
  `if`-in-action, so the guarded form is the supported path.
- `add_on_miss` and `pna_empty_group_action` table properties are now accepted
  by the compiler (p4c repo). `add_on_miss` is emitted in the table JSON; the
  `add_entry` primitive already enforces the spec's miss-only semantics.
  `pna_empty_group_action` is an accepted no-op (bmv2's indirect-table apply path
  has no empty-group hook; the property is optional and not in canonical pna.p4).
- Type widths and metadata struct fields match the canonical spec exactly; the
  extern surface is identical except for the target-specific `recirculate`.

14 feature suites now pass (the 13 above plus `ft_notify.sh`).
