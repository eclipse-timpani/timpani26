# Running the FCW example (L1 TT + L2 CBS) with TIMPANI

Step-by-step flow for running `mock_radar_producer` (L1 TT) and `fcw_consumer` (L2 CBS) under
timpani-o / timpani-n, plus the commands to inject every anomaly.

Replace `<path-to-timpani>` with the location of your TIMPANI checkout:

```bash
export TIMPANI_ROOT=<path-to-timpani>
```

Terminals used below: **T1** consumer, **T2** producer, **T3** timpani-o, **T4** timpani-n,
**T5** pullpiri mock.

---

## 1. Prerequisites

- Build the example: `cd $TIMPANI_ROOT/examples/someip && mkdir -p build && cd build && cmake .. && make -j$(nproc)`
- Build timpani-o and timpani-n (`$TIMPANI_ROOT/timpani-o/build`, `$TIMPANI_ROOT/timpani-n/build`).
- Python virtual environment with the dependencies of `scripts/mock_pullpiri.py` in
  `$TIMPANI_ROOT/scripts/.venv`.
- Run the two applications from a shell that is inside the isolated-CPU cgroup, otherwise they cannot
  use the isolated CPUs (see section 5.3 of `README.md`).

---

## 2. Workload definition

`$TIMPANI_ROOT/scripts/mock_pullpiri.yaml` is what the pullpiri mock sends:

```yaml
# SPDX-FileCopyrightText: Copyright 2026 LG Electronics Inc.
# SPDX-License-Identifier: MIT

workloads:
  - workload_id: fcw_producer_tt
    temporal_class: TEMPORAL_PERIODIC
    tasks:
      - name: tt_task
        priority: 20
        policy: FIFO
        cpu_affinity: 0x1
        period: 20000
        runtime: 2000
        deadline: 20000
        release_time: 0
        max_dmiss: 2

  - workload_id: verify_wl_cbs
    temporal_class: TEMPORAL_SPORADIC
    tasks:
      - name: cbs_task
        priority: 18
        policy: FIFO
        cpu_affinity: 0x1
        period: 20000
        runtime: 2000
        deadline: 5000
        release_time: 0
        max_dmiss: 2
```

- `name` must equal the thread name of the task: `tt_task` is the producer's publishing thread,
  `cbs_task` is the consumer's worker thread (maximum 15 characters).
- `max_dmiss` is the number of deadline misses tolerated before the fault is forwarded to timpani-o.
- The CPU is chosen by timpani-o from `available_cpus` (next section); `policy` and `cpu_affinity`
  are not used for placement.

---

## 3. Node configuration (timpani-o)

`$TIMPANI_ROOT/timpani-o/examples/node_configurations.yaml` lists the CPUs timpani-o may use on
each node:

```yaml
# TTRT Global Scheduler - Node Configuration File
# Hardware description of each node

nodes:
  <node-id>:                      # must match the node id of timpani-n (hostname, or the -n option)
    available_cpus: [2]
    max_memory_mb: 4096
    architecture: "aarch64"
    location: "front_sensor_unit"
    description: "Autonomous driving perception and sensor fusion node"
```

`available_cpus` must list the **isolated** CPUs, for example `[2, 3]`.
`[2]` is used here on purpose so that both tasks are scheduled on the same CPU for testing.

---

## 4. Execution order

Start the applications first: the task threads must already exist when timpani-n applies the table.

**T1 - consumer**
```bash
cd $TIMPANI_ROOT/examples/someip/build
LD_LIBRARY_PATH=. ./radar_based_fcw/fcw_consumer
```

**T2 - producer**
```bash
cd $TIMPANI_ROOT/examples/someip/build
LD_LIBRARY_PATH=. ./radar_based_fcw/mock_radar_producer
```

Both applications now wait: the producer for the futex wake-up from timpani-n, the consumer for events.

**T3 - timpani-o**
```bash
cd $TIMPANI_ROOT/timpani-o/build
sudo ./timpani-o -c ../examples/node_configurations.yaml
```

**T4 - timpani-n**
```bash
cd $TIMPANI_ROOT/timpani-n/build
sudo ./timpani-n            # add -g to record the scheduling slices (<node-id>.gpdata)
```

**T5 - pullpiri mock (last)**
```bash
cd $TIMPANI_ROOT/scripts
source .venv/bin/activate
python mock_pullpiri.py --config mock_pullpiri.yaml
```

Expected timpani-n output after T5:
```
[main] Registered task in BPF: tt_task pid=<tid> type=TT ...
[main] Applied SCHED_EXT to tt_task pid=<tid>
[main] Applied SCHED_EXT to CBS cbs_task pid=<tid>
[main] Registered CBS task in BPF: cbs_task ... budget=2000us period=20000us
```
The producer then runs every 20 ms. Frames are published once the target is closer than 15 m, and the
consumer logs `Processed Frame ...`.

---

## 5. Simulating anomalies

Restart T1 and T2 for each case. A restarted application has new thread ids, so also **restart T4
(timpani-n)**: when it reconnects, timpani-o replays the stored table and the new threads are
registered. Run the commands from `$TIMPANI_ROOT/examples/someip/build` with
`export LD_LIBRARY_PATH=.`.

| Case | T1 (consumer) | T2 (producer) | Evidence |
|---|---|---|---|
| Normal | `./radar_based_fcw/fcw_consumer` | `./radar_based_fcw/mock_radar_producer` | no anomalies |
| MIT violation | normal | `./radar_based_fcw/mock_radar_producer --burst-ms 5` | consumer log: `MIT VIOLATION ... < mit=18000us` |
| Wakeup latency | `./radar_based_fcw/fcw_consumer --wakeup-delay-ms 5` | normal | consumer log: `WAKEUP/E2E LATENCY EXCEEDED` (limit 3 ms) |
| Budget exceeded | `./radar_based_fcw/fcw_consumer --overrun-ms 10` | normal | `FAULT_BUDGET_EXCEED` in the timpani-n log |
| TT deadline miss | normal | `./radar_based_fcw/mock_radar_producer --tt-overrun-ms 25` | `Dmiss` lines for `tt_task`, fault to timpani-o |
| CBS deadline miss | `./radar_based_fcw/fcw_consumer --overrun-ms 10` | normal | `Dmiss` lines for `cbs_task` |

Notes:
- `--burst-ms` replaces the timpani-n wake-up with a local timer, so use it only for the MIT case.
- Budget exceeded and the CBS deadline miss occur together: an overrun that is throttled also takes
  longer than the 5 ms deadline.
- The deadline-miss fault is forwarded to timpani-o only after `max_dmiss` (2) is exceeded; expect a
  few `Dmiss` counts for `tt_task` right after start-up.
- The MIT and wakeup-latency anomalies are detected by `libtimpani_rt` and appear only in the
  consumer log; timpani-n does not read them yet.
