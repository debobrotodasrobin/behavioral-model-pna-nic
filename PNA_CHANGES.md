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

## Build

```bash
cd behavioral-model
./autogen.sh && ./configure --with-pna-nic && make -j$(nproc) && sudo make install
```

## Testing

12 feature suites + Mininet NIC+fabric demo (6/6) + compiler A/B (287 programs, 0 semantic diffs).
