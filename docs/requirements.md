# FrameTrace · Requirements

[Türkçe](gereksinimler.md) · [Design notes](design.md) · [README](../README.md)

| Version | Date | Note |
|---|---|---|
| 0.1 | 2026-10-13 | First draft. Decisions: DL-04 ≤ 20 ms; the RAM budget is split into application and measurement infrastructure. |
| 0.2 | 2026-10-13 | Port layer (queuing + sampling) and health monitor (section 6). KAY-04 implemented; service-start stamp fixed (DL-05). |
| 0.4 | 2026-10-13 | Step 4: second courier = second link (section 7). |
| 0.5 | 2026-10-13 | Step 5: batched service (KAN-04). DL-05 and KAN-03 rewritten for batches (DL-05b, KAN-03b). |
| 0.6 | 2026-10-13 | Step 6: the frame comes from TIM2, 500 µs after the tick edge (BUT-07). |
| 0.7 | 2026-10-13 | Memory layout (RAM-05): stacks and the frame task's state in CCM. |
| 0.8 | 2026-10-13 | RAM accounting: the trace-off build measured. |
| 1.0 | 2026-10-14 | Record 14 (steps 1–6) evaluated from its trace; statuses updated. |

This document defines the requirements of the frame consumer (task `Arda` in the traces). The steps build on each
other:
1. A 1 ms frame task with 1-byte handles, 0-tick waits and compile-time sizing.
2. Service = the 15-byte transmission model.
3. The port layer and the deadline counter.
4. A second link.
5. Batched service.
6. A TIM2-phased frame.

On-board records exist for step 1 (08), steps 1–2 (09) and steps 1–6 (14); steps 3–5 were verified in the host
simulation. The queue-based baseline consumers (A/B/C, records 01–07) are outside the scope of this document. They
are used for comparison only and are not changed (GEN-01).

Every requirement carries a measurable value, a rationale and a verification method. A row missing one of these is
not a requirement. When a requirement changes, a new row is added; the old row stays and is marked as changed.

## 1. Scope and assumptions

- Board: STM32F407VG, 168 MHz. FreeRTOS V11, 1 kHz tick (1 tick = 1 ms), static allocation only.
- Timestamps from TIM2 at 1 µs resolution.
- The UART (230 400 baud) carries only commands and the after-run dump; it is silent while measuring.
- The service is a **model**: the time to transmit the 12-byte message in a 15-byte binary frame. Nothing is sent
  during a run; the second link and the DMA batches are models as well.
- Measurements are single runs with tracing on. Record 14's dump was not saved; its values come from decoding the
  SystemView trace (`analysis/systemview/14Arda_yogun_2_v2.SVDat`).

## 2. Definitions

| Term | Definition | Field |
|---|---|---|
| Generation time | Timestamp the producer takes right before the send call | `t_gen_us` |
| Intake | From generation until the frame task takes the handle from the message port | `intake_max_us` (counter) |
| Service start (g2r) | From generation until the message's service starts, inbox wait included. For message *k* of a trip: start of the trip's frame + *k* × 651 µs | `generation_to_receive_us` |
| Service | From service start until the frame in which the trip ends | `service_us` |
| Burst | Attempts 11–34 of the burst load: 24 attempts with no wait between them | `phase = art_arda` |
| Frame | One wake-up of the frame task (TIM2, 1 ms) | `frames` |
| Frame work | A frame's work from wake-up to the end of its last step | `frame_work_max_us` |

**Verification methods:**
- **A** analysis (calculation, compile-time check);
- **T** test (host test, real-kernel simulation, fault injection);
- **M** measurement on the board (dump or trace);
- **I** inspection (code reading).

**Status:** ✅ met · ❌ not met · ⏳ not yet verified

## 3. General (GEN)

| ID | Requirement | Rationale | Verification | Status |
|---|---|---|---|---|
| GEN-01 | The baseline's (A/B/C) code, load and measurement definitions must not change. | The comparison's reference stays fixed. | I, T (A/B/C loss sets unchanged in the simulation) | ✅ |
| GEN-02 | The frame consumer must use the same load recipe and the same 12-byte message as the baseline. | The comparison stays single-factor. | I | ✅ |
| GEN-03 | Each step must change one factor and have its own measurement. | Every result can be traced to one change. | I | ✅ board: 08 → 09 changes only the service model; steps 3–5 in simulation |
| GEN-04 | Every measurement file must state the firmware version it was taken with. | The software behind a measurement is readable from the file itself. | I | ❌ |

## 4. Load contract (YUK)

| ID | Requirement | Value | Rationale | Verification | Status |
|---|---|---|---|---|---|
| YUK-01 | Send attempts per run | 44 | Load contract. | A (`QEXP_ATTEMPTS`), M (`attempted`) | ✅ 08, 09, 14: 44 |
| YUK-02 | Most messages in the burst | 24 | The port capacity is derived from it (YUK-05). | A (`arda.h` check b), M | ✅ 24 messages; within 323 µs in 09, ~670 µs in 14 |
| YUK-03 | Gap between normal attempts | ≥ 100 ms | The system is empty before the burst. | A (`arda.h` check a) | ✅ |
| YUK-04 | Message size | 12 bytes | Fixed format: sequence, generation time, run number. | A (`queue_exp.h` compile-time check) | ✅ |
| YUK-05 | After the burst, service must outpace arrivals: at least 2 messages served for every message arriving per 100 ms. | > 1 | The backlog must only shrink, or the port fills over time. | A (`arda.h` check c) | ✅ |
| YUK-06 | Behaviour outside the contract must be defined: excess messages counted as `pool_empty`, the send returns failure, the system does not hang, the run ends and the checks turn red. | — | In a real system the load comes from outside and can exceed the contract. | T, M (stress test with 32- and 33-message bursts) | ⏳ |

## 5. Loss policy (KAY)

Messages are **event data**, the counterpart of an ARINC 653 queuing port: every one matters, order is kept, and an
old message is never dropped.

| ID | Requirement | Value | Rationale | Verification | Status |
|---|---|---|---|---|---|
| KAY-01 | Messages lost within the load contract (at send or in service) | 0 | Event data cannot be recovered. | T, M (check "no loss at all"; trace: SEND = RECEIVE = DONE) | ✅ 08: 0, 09: 0, 14: 0 (44/44/44) |
| KAY-02 | Every loss that does occur must appear in a counter and in the check list; no silent loss. | — | An error not caught where it happens cannot be found later. | T (fault injection: corrupted handle → 4 checks red) | ✅ simulation |
| KAY-03 | Every queue and port operation on the data path must wait 0 ticks. A full queue is a design error: prevented at compile time, halted by `configASSERT`, or counted. | 0 ticks | Using a wait as a control tool creates tick races (baseline C). | I, M (`send_call` 7–8 µs) | ✅ |
| KAY-04 | A message past its age limit must not be dropped; it is reported as a deadline miss. | — | Must not conflict with KAY-01. | T, M (`deadline_miss` = number of records with g2r > 20 ms) | ✅ simulation (counter = records); 14: no wait above 20 ms in the trace |
| KAY-05 | The dump request is posted with 0 wait; if there is no room it is retried for at most 90 ticks, then `configASSERT`. | 90 ticks | Twice the worst-case drain time of the UART queue (45 ms). | A (`queue_exp.c`), M (`dump_retry`) | ✅ 08, 09: 0 |

## 6. Port layer (PRT)

Tasks exchange data only through ports (`port_layer.h`, table in `port_config.c`).
- Messages go through a **queuing port** (`PORT_ARDA_MSG`).
- The frame task's latest status (backlog, oldest message's age, deadline count) goes through a **sampling port**
  (`PORT_ARDA_STATUS`). An unread update is skipped, which is not an error; a value older than two frames is stale.
- Errors are reported to the health monitor (`health.h`), whose reactions are a table: unauthorised access halts,
  everything else is counted.

| ID | Requirement | Value | Rationale | Verification | Status |
|---|---|---|---|---|---|
| PRT-01 | Every port has exactly one writer and one reader; access from any other task halts the system. | — | Data ownership can be read from configuration, not code. | I (`port_bind`, `check_access`, `HM_PORT_ACCESS`) | ✅ |
| PRT-02 | Queuing port: sent = received, overflow 0, handles not returned 0 | 0 | KAY-01 at port level. | M (`pool_*`), T (simulation: port 44/44) | ✅ simulation; 14: 44 sends, 44 takes, 44 deliveries in the trace |
| PRT-03 | Sampling port: the value read is at most two frames old; stale reads 0 | ≤ 2 ms, 0 | Status data is meaningful only while fresh. | M (`status_age_max_us`, `status_stale`) | ✅ simulation (max 136 µs, 0) · board ⏳ |
| PRT-04 | Port calls never wait; the sampling-port copy is in a critical section and at most 32 bytes. | 0 ticks, ≤ 32 B | The critical section stays short and constant. | A (`port_config.c` `_Static_assert`), I | ✅ status message 16 B |

## 7. Links (KAN)

A courier models a line: a trip is the transmission of messages on a line, and one UART carries one frame at a time.
The two links add **capacity**, not redundancy.

| ID | Requirement | Value | Rationale | Verification | Status |
|---|---|---|---|---|---|
| KAN-01 | Every courier models its own line; the number of couriers must not exceed the free UARTs. | ≤ 5 (USART1/3/6, UART4/5) | Counting several couriers on one line would make the model unreal. | A (`arda.h` `_Static_assert`) | ✅ |
| KAN-02 | The dump carries the number of couriers and the most couriers on the road at once; the record must match the board's courier count. | 2 / 2 | A run taken with the wrong firmware is detected. | M (`couriers`, `courier_busy_max`) | ✅ simulation; 14: both links deliver in every heavy frame of the trace |
| KAN-03 | Services end in the order they started. | — | The receiver merges the links by sequence number. | I | Changed in 0.5 to KAN-03b (a whole batch is delivered at once) |
| KAN-03b | Service starts follow the sequence number (equal times allowed). | 0 inversions | With the start order kept, the receiver needs at most one message of buffering to merge. | I (striping), M (check "service starts follow the sequence number") | ✅ simulation (0 inversions); 14: deliveries in sequence order in the trace |
| KAN-04 | Batched trip: at most 3 messages per trip, back to back on one line, each in its own 15-byte frame; the courier does not wait for the batch to fill. | ≤ 3 | Use the end of the frame lost to the 1 ms rounding without changing the message format or the load (3 messages 1 953 µs, 2 frames). | A (`ARDA_BATCH_MAX`), M (`batch`, `batch_max`) | ✅ simulation (3 / 3); 14: 6 deliveries every 2 ms (2 links × 3) in the trace |

## 8. Timing (DL)

| ID | Requirement | Value | Rationale | Verification | Status |
|---|---|---|---|---|---|
| DL-01 | Intake, worst case | ≤ 1.1 ms | One frame period plus a margin. | M (`intake_max_us`; RECEIVE latency in the trace) | ✅ 08: 990 µs, 09: 983 µs, 14: 697 µs |
| DL-02 | Service start (g2r) under normal load, worst case | ≤ 1.1 ms | A message should not wait under normal load. | M | ✅ 09: 988 µs, 14: ≈ 0.48 ms · ❌ 08 (because of DL-03) |
| DL-03 | The burst must not leak into the following normal messages: service start of the first normal message after the burst (35) | ≤ 1.1 ms | A load peak that delays other traffic spreads failures. | M | ✅ 09: 988 µs, 14: ≈ 0.48 ms · ❌ 08: 381 ms |
| DL-04 | Worst service start (g2r) in the burst | **≤ 20 ms** | One service period of a 50 Hz receiver. | M (`g2r_max_us`; trace) | ✅ 14: 6.86 ms · ❌ 09: 23.7 ms · ❌ 08: 460.7 ms |
| DL-05 | Service time must match the model: 20 ms for the fixed model, one frame (1 ms) for the transmission model. | ± 50 µs | The delay budget depends on it. | M (`service_us`) | 08, 09: ✅ for 43 messages; the first burst message (11) 142 µs short (stamp artefact, fixed in 0.2). Changed in 0.5 to DL-05b. |
| DL-05b | Service time must match the model: message *k* of an *n*-message trip is served for ⌈*n* × 651 / 1000⌉ ms − *k* × 651 µs (n = 1: 1 ms; n = 3: 2.0 / 1.35 / 0.70 ms). | ± 50 µs | In a batch the service time depends on the position. | M (`service_us`) | ✅ simulation (0.67–2.04 ms, Windows timing) · board ⏳ (the trace does not stamp the service start; needs a dump) |
| DL-06 | Measured delays must match the prediction written before the step. | difference ≤ 5 % | A missed prediction means the system is not understood. | A + M | ✅ 08: predicted 463, measured 460.7 ms · ✅ 09: 23.9 / 23.7 ms · ❌ 14: 7.6 / 6.86 ms (−10 %); the prediction assumed the burst ends before the first frame, and the burst is now split by the frame |

## 9. Frame budget (BUT)

| ID | Requirement | Value | Rationale | Verification | Status |
|---|---|---|---|---|---|
| BUT-01 | Interval between two frame starts | 1 ms ± 10 µs | Timing known in advance. | M (`frame_dt_min/max_us`; trace) | ✅ 08, 09: 998–1 002 µs · 14: 999.90–1 000.18 µs |
| BUT-02 | Work of one frame, worst case | ≤ 300 µs (30 %) | Leave room for interrupts and added work. | M (`frame_work_max_us`; trace) | ✅ 09: 143 µs · 14: 292.4 µs (SystemView bar), close to the limit |
| BUT-03 | Missed frames | 0 | A missed frame means the timing assumption failed. | M (`frame_miss`; trace) | ✅ 08, 09: 0 · 14: no interval above 1.0002 ms in the trace |
| BUT-04 | Most messages handled in one frame | ≤ K | Bound the frame's work independently of the load. K follows from the measured per-message cost (~13 µs with tracing). | A, M | ⏳ |
| BUT-05 | The frame task is alone at the highest priority (P5). | P5 | Only interrupts can delay the frame. | I (`main.h`) | ✅ |
| BUT-06 | A trip lasts a whole number of frame periods. | — | Otherwise the trip is rounded up to the next frame and the service grows unnoticed. | A (`arda.c` compile-time check) | ✅ |
| BUT-07 | The frame starts half a period after the tick edge on which the producer wakes: the producer and the consumer are not locked to the same edge. | 500 µs, deviation ≤ 50 µs | On a shared edge the higher-priority frame runs first and a message waits almost a full period (988 µs). Half a period halves that wait and removes the race on the edge. | A (`frame_timer.c`), M (`frame_phase_min/max_us`; trace) | ✅ 14: 498.9–500.1 µs over 2 051 frames (SysTick → TIM2 interrupt) |

## 10. Memory (RAM)

The application and the measurement tool are budgeted separately. SystemView's buffer is the measurement tool's
memory and disappears from the trace-off build. The two regions are reported separately (`mem_layout.h`,
`stm32f407vg_flash.icf`):

- **CCM RAM** (0x1000 0000, 64 KB): CPU-only data bus; no DMA, no code. Only CPU data goes here.
- **Main SRAM** (0x2000 0000, 128 KB): anything a DMA or the debugger may read, and everything else.

| ID | Requirement | Value | Rationale | Verification | Status |
|---|---|---|---|---|---|
| RAM-01a | Application RAM (trace-off build) | ≤ 32 KB | Leave room to grow. | A (map file of the trace-off build) | ✅ 19 706 B (main SRAM 7 286 + CCM 12 420) |
| RAM-01b | Measurement infrastructure RAM (SystemView + RTT) | ≤ 20 KB, 0 dropped events | The tool must neither distort nor miss. | A (map), M (OVERFLOW packets in the trace) | ✅ 18 085 B; 0 dropped events in the traces of runs 06 and 14 |
| RAM-02 | Frame consumer RAM including stack (port layer, health monitor, frame timer) | ≤ 4 KB | Keep the consumer small. | A (map) | ✅ 2 568 B |
| RAM-03 | Every task stack is at least 1.25 × the largest use found by static analysis, with the rationale written down. | ≥ 1.25 × | Stack sizes chosen from evidence, not guesses. | A (IAR stack analysis), M (`stack_hwm_*`) | ⏳ (3–10× headroom today; table below) |
| RAM-04 | No dynamic memory. | — | Memory use known at build time. | I (`configSUPPORT_DYNAMIC_ALLOCATION 0`, no `malloc` in the map) | ✅ |
| RAM-05 | Data a DMA can reach lives in main SRAM, CPU-only data in CCM. The main stack sits at the base of CCM, so an overflow writes to a reserved address and faults. CCM variables are zeroed at start-up. | — | No silent overflow; no DMA data where a DMA cannot reach. | A (map: CSTACK 0x1000 0000–0x1000 0800, `.ccmram` kind "zero"), T (`CSTACK_OVERFLOW_TEST` → HardFault) | Map ✅ · overflow test on the board ⏳ |

**By region** (Debug, map file):

| Build | Main SRAM (128 KB) | CCM (64 KB) | Total (192 KB) |
|---|---|---|---|
| Tracing on | 25 375 B used · 80.6 % free | 12 420 B used · 81.0 % free | 37 795 B used · 80.8 % free |
| Tracing off | 7 286 B used · 94.4 % free | 12 420 B used · 81.0 % free | 19 706 B used · 90.0 % free |

Main SRAM with tracing on (bytes):
- SystemView + RTT: 18 085;
- button and telemetry buffers inherited from the measurement chain: 2 432;
- baseline and records: 1 532;
- UART TX queue: 1 200;
- FreeRTOS kernel (Idle stack 512 included): 861;
- port layer, health monitor, frame timer and the rest of the frame task: 788;
- other: 477.

CCM (bytes): main stack 2 048 (at the base), 6 task stacks 9 728, 6 TCBs 480, frame-task state 164.

**Task stacks: allocated vs. needed.** Values are bytes, from the IAR stack analysis (worst call chain). Trace-on
values are upper bounds because of SystemView's recursion. The analysis excludes the frame pushed on task switches
and exception entry (up to ~200 B with the FPU).

| Task | Allocated | Needed, trace off | Needed, trace on |
|---|---|---|---|
| UartTx | 2 048 | 736 | ≤ 1 048 |
| Telemetry | 1 536 | 232 | ≤ 576 |
| Producer | 1 536 | 152 | ≤ 504 |
| Consumer | 1 536 | 152 | ≤ 504 |
| Button | 1 536 | 136 | ≤ 488 |
| Frame task (Arda) | 1 536 | 160 | ≤ 512 |
| Interrupts (main stack) | 2 048 | max 288, all nested 512 | max 600, all nested 2 136 |

## 11. Traceability

| ID | Implemented by | Verified by | Status |
|---|---|---|---|
| GEN-01 | — | Simulation: A/B/C loss sets | ✅ |
| GEN-04 | — | Version field in the measurement files | ❌ |
| YUK-02, YUK-03, YUK-05 | `arda.h` compile-time checks (a), (b), (c) | Build; build test with a broken load | ✅ |
| YUK-04 | `queue_exp.h` `_Static_assert(sizeof(qexp_message_t) == 12U)` | Build | ✅ |
| YUK-06 | `port_send` → `pool_empty` (port overflow) | Stress test | ⏳ |
| KAY-01 | `arda.h` rules K1–K7, `port_layer.h` rules P1–P6 | Check "no loss at all" (Python, ground station), simulation, trace | ✅ |
| KAY-02 | Counters `pool_*` (port + health monitor), `dump_retry`; `qexp_consumer_drop` | Fault injection: corrupted handle (20th send) → 4 checks red | ✅ simulation |
| KAY-03 | `port_send`, `port_take`, `port_release`, `port_read/write`: all 0 ticks; `configASSERT` | Inspection; `send_call` measurement | ✅ |
| KAY-04 | `arda.c` `dispatch` (K7) → `deadline_miss`, `HM_DEADLINE_MISS` | Check "deadline counter matches the records"; simulation | ✅ |
| KAY-05 | `qexp_finish_run`, `QEXP_DUMP_RETRY_TICKS` | Check "dump request posted without waiting" | ✅ |
| PRT-01…PRT-04 | `port_layer.c`, `port_config.c`, `health.c`; binding in `queue_exp_init` | Simulation; "status port …" checks | Section 6 |
| KAN-01…KAN-04 | `arda.h` (`ARDA_COURIERS`, `ARDA_BATCH_MAX`), `arda.c` `dispatch` (striping), `complete_trips` | Simulation; "couriers", "at most 3 per trip", "sequence order" checks; record 14 trace | Section 7 |
| DL-01…DL-05b | `arda.c` frame order (complete → intake → dispatch → status); service start = frame start + *k* × 651 µs | Records 08, 09 (dump), 14 (trace); ground-station tiles | Section 8 |
| BUT-01, BUT-03, BUT-07 | `frame_timer.c` (TIM2 CC1 interrupt, phase from SysTick), `arda.c` frame loop | `frame_dt_*`, `frame_miss`, `frame_phase_*`; record 14 trace | ✅ |
| BUT-02, BUT-04 | `arda.c` | `frame_work_max_us`; trace | BUT-02 ✅ · BUT-04 ⏳ |
| BUT-06 | `arda.c` `_Static_assert(ARDA_TRIP_TICKS_FIXED % ARDA_FRAME_TICKS == 0U)` | Build | ✅ |
| RAM-01a, RAM-01b, RAM-02 | — | Map file; OVERFLOW packets in the traces | Section 10 |
| RAM-04 | `FreeRTOSConfig.h` | Map file | ✅ |
| RAM-05 | `mem_layout.h` (`CCM_DATA`), `stm32f407vg_flash.icf` (`place at start of CCMRAM_region { block CSTACK }`), `main.c` (`CSTACK_OVERFLOW_TEST`) | Map file; overflow-test build | Map ✅ · board ⏳ |

## 12. Open items

| Topic | Note |
|---|---|
| Per-frame message cap (BUT-04) | From record 14's trace: ~13 µs per message with tracing. A frame that took all 32 slots would need ~450 µs. |
| Interrupt stack rule (RAM-03) | IAR's "total interrupt use" assumes every interrupt nests (2 136 B). EXTI0, USART2 and TIM2 share priority 5 and cannot interrupt each other; they only preempt SysTick/PendSV at priority 15, so ≤ 2 × 600 = 1 200 B. To be written as a `check that` rule in `stack_usage.suc`. The main stack is at the base of CCM, so an overflow faults (RAM-05). |
| A saved dump for record 14 | A steps 1–6 run with the board's own counters (deadline, phase, port, status, couriers, batches), and a trace-off run. |
| Merging two links at the receiver | Outside the model. A real system needs merging by sequence number and a "link down" health event; with one link left, capacity halves and deadline misses return. |
| Firmware identity (GEN-04) | In every dump and trace. |
| Stress test (YUK-06) | Bursts of 32 and 33 messages. |
| MPU | Isolate the frame task's data in hardware instead of by layout. |
| Buffers inherited from the measurement chain (2 432 B) | Left from the button and telemetry measurement; unused in this experiment. |
