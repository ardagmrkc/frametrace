# FrameTrace · design notes

How the frame consumer is built and why. The README shows what was measured; this document explains the
mechanisms behind the numbers. Requirement IDs (DL-04, BUT-02, …) refer to [requirements.md](requirements.md).

## 1. Problem and constraints

- One producer, one consumer, one 12-byte message type. The load is fixed (§2), and the consumer is the only part
  that changes.
- **Every message is event data.** Losing one is a failure. A message that is late is still delivered, and the
  lateness is counted.
- **Deadline:** a message's service must start at most 20 ms after it was generated (DL-04).
- **Determinism:** no heap, no waits on the data path (every queue and port call uses 0 ticks), and a frame whose
  work is bounded by design.
- **Platform:**
  - STM32F407VG at 168 MHz, FreeRTOS V11 with a 1 kHz tick and static allocation only.
  - IAR EWARM 9.70.
  - SEGGER SystemView over RTT for traces.
  - TIM2 as a free-running 1 µs timestamp counter.

## 2. Load contract and sizing

| Phase | Attempts | Spacing |
|---|---|---|
| Normal | 1–10 | 100 ms after each |
| Burst | 11–34 | back to back |
| Normal | 35–44 | 100 ms after each |

The message is `{sequence, generated_us, run}`, 12 bytes (`_Static_assert` in `queue_exp.h`).

The number of handles in use at once equals the number of messages accepted but not yet served:

- **(a)** Before the burst the system is empty. Two normal attempts are 100 ms apart, which is more than one trip
  (at most 20 ms).
- **(b)** The burst brings 24 messages before any trip can end, so at most 24 handles are in use.
- **(c)** After the burst, one message arrives per 100 ms while at least (100 / 20) × couriers are served, so the
  backlog only shrinks.

The port therefore holds 24 + 8 margin = 32 slots. All three conditions are `_Static_assert`s in `arda.h`:
change the load, the trip length or the capacity so that one of them fails, and the build stops. A trip must also be
a whole number of frames (BUT-06), otherwise it would be rounded up silently.

## 3. Why the queue-based consumers lose

All three baseline variants use a FreeRTOS queue and a consumer at priority 1 that serves each message with a 20 ms
`vTaskDelay`. The producer has priority 2.

- **A (capacity 4, no wait).** The producer never blocks, so the consumer cannot run during the burst. Four
  messages fit; 15–34 fail at once.
- **B (capacity 16).** Sixteen fit and 27–34 fail. The survivors queue behind 20 ms services and wait up to 300 ms.
- **C (capacity 4, 20 ms send timeout).** The producer blocks on the full queue for 20 ticks. The consumer's 20 ms
  service also ends after 20 ticks, from the same tick:
  - Both tasks become Ready on the same SysTick (`xTaskIncrementTick`, two *Task Ready* events).
  - The higher-priority producer runs first and finds the queue still full; `xTaskCheckForTimeOut` returns
    `errQUEUE_FULL`.
  - The consumer frees a slot microseconds later, and the next attempt fits.

  Every second burst message is lost: 16, 18, …, 34. The trace shows the race in every one of the ten cases.

The common cause is that the queue is the consumer's only buffer and the producer decides what happens when it is
full.

## 4. The frame consumer

### 4.1 Frame task

`arda_frame_task` (task name `Arda` in the traces) runs at priority 5. It is the only task at that priority, so only
interrupts can delay it (BUT-05). It wakes once per 1 ms frame (§7) and always runs the same four steps:

| Step | Function | Work |
|---|---|---|
| 1 | `complete_trips` | Trips whose length has elapsed end: `DONE` for each message, handles returned to the port in position order. |
| 2 | `intake` | `port_take` with 0 ticks until the message port is empty; handles go to the inbox ring. |
| 3 | `dispatch` | Free couriers take waiting messages (§4.3); deadline check (§4.4). |
| 4 | `write_status` | Backlog, oldest waiting message's age and deadline count are written to the status port (§5). |

A courier that returns in step 1 can leave again in step 3 of the same frame. The frame's work is measured from
the frame start to the end of step 4 (`frame_work_max_us`, BUT-02).

Rules the frame task keeps (`arda.h`, K1–K7):

- **K1–K4:** slot, handle and state rules. They are implemented by the port layer (§5).
- **K5:** the inbox has the port's capacity, so every handle taken from the port fits. The port is the only capacity
  limit.
- **K6:** every counter has exactly one writer, and at the end of a run every handle is back in the free queue
  (`pool_leak` = 0).
- **K7:** deadline accounting (§4.4).

### 4.2 Service model

A trip models the transmission of a message as a 15-byte binary frame: sync, type, 12-byte message and CRC. At
230400 8N1 one byte is 10 bits, so a frame is 15 × 10 / 230400 s = 651 µs (`ARDA_TX_US`). Nothing is transmitted
during a run; the UART carries only commands and the dump after the run.

The earlier model, a fixed 20 ms service identical to the baseline consumer's, is kept in the code as the upper
bound used by the sizing rules (§2).

### 4.3 Couriers, links and batches

- **A courier is a link.** One UART carries one frame at a time, so every courier models its own line. The F407 has
  five UARTs free besides the console, so `ARDA_COURIERS` ≤ 5 is a `_Static_assert`. Two links are used for capacity,
  not redundancy.
- **Batches.** A free courier takes up to `ARDA_BATCH_MAX` = 3 waiting messages and never waits for its batch to fill.
  The messages leave back to back on its line, which models DMA.
  - A trip of *n* messages lasts ⌈*n* × 651 µs / 1 ms⌉ frames: 1 message → 1 frame, 2 or 3 messages → 2 frames.
  - Message *k* (*k* = 0, 1, …) of a trip starts transmitting *k* × 651 µs after the trip start. That is its service
    start, and the deadline is checked against it.
  - All messages of a trip are delivered in the frame where the trip ends.
- **Striping.** When several couriers are free in the same frame, round *k* gives one message to each of them:
  11 → link 0, 12 → link 1, 13 → link 0, … Messages at the same position start at the same moment on different links,
  so service starts follow the sequence number (KAN-03b). A receiver merging the two links needs at most one message
  of buffering.
- **Delivery order.** `complete_trips` delivers the couriers that finish in the same frame position by position: the
  first message of every courier, then the second, and so on.

Why batching helps without changing the load: with a 1 ms frame, a single 651 µs message leaves 349 µs of every frame
unused. Three messages (1 953 µs) fit in two frames, which raises throughput from 1 to 1.5 messages per ms per link.

### 4.4 Deadline accounting (K7)

In `dispatch`, every message whose service start is more than 20 ms after its generation increments
`deadline_miss` and is reported to the health monitor as `HM_DEADLINE_MISS`. The message is still served: dropping
it would turn lateness into loss (KAY-04).

The service start is the frame's start time plus *k* × 651 µs, and the frame start is taken once, before any work.
An earlier version stamped the trip start at dispatch time, after intake had already run. In the burst frame that
made the first message's service look 142 µs short (records 08 and 09).

## 5. Port layer (`port_layer.h`, `port_config.c`)

A small counterpart of ARINC 653 ports. Tasks exchange data only through ports. Every port is declared in one table
with its kind, message size, capacity, refresh time and statically allocated storage.

| Port | Kind | Writer → reader | Content |
|---|---|---|---|
| `PORT_ARDA_MSG` | queuing | Producer → frame task | `qexp_message_t`, 32 slots |
| `PORT_ARDA_STATUS` | sampling | frame task → Producer | `arda_status_t`, refresh 2 frames |

Rules (P1–P6):

- **P1, static memory.** Slot and handle counts come from the table.
- **P2, no waiting.** No port call blocks (0 ticks); full or empty is a return value.
- **P3, one writer and one reader per port.** They are bound with `port_bind` at start-up. Any other caller is a
  design error, and the health monitor halts.
- **P4, one place per handle.** A queuing-port handle is in exactly one place at a time and follows
  FREE → QUEUED → TAKEN → FREE. An invalid handle is never used to access a slot; it is reported.
- **P5, every error is reported.** The health monitor decides the reaction (§6).
- **P6, one writer per counter.** Send-side counters are written by the writer, receive-side counters by the reader.

**Queuing port.**
- Messages stay in static slots, and two FreeRTOS queues carry 1-byte handles: one for free slots, one for ready
  slots in FIFO order.
- `port_send` takes a free handle, copies the message into its slot and posts the handle.
- `port_take` gives the oldest handle, and the reader reads the message in place (`port_slot`, no copy).
- `port_release` returns the handle exactly once.
- Both queues have the port's capacity, so if a handle was obtained, posting it cannot fail.

**Sampling port.**
- One slot. The writer overwrites it, and the reader gets the latest value with its age, its validity
  (age ≤ refresh time) and the number of updates it missed.
- Both copies run inside a short critical section; the message size is limited to 32 bytes by a `_Static_assert`.
- A missed update is normal for status data. A stale read means the writer has stopped, and it is reported.

**Why messages use a queuing port and status a sampling port.** Each message must be delivered, in order, exactly
once. The frame task's status only has to be fresh.

In the simulation the producer read the status 44 times while the frame task wrote it about 2 050 times. The burst
there is sent within one frame, so the backlog the producer saw never exceeded 0: the whole burst had been sent
before the frame task updated its status once. On the board (record 14) the frame interrupts the burst, so the
producer's last six reads do see a backlog, but only as it was at the end of the last frame.

Either way, a sampling port shows a snapshot of the last state and cannot carry events.

## 6. Health monitor (`health.h`)

Events and their reactions are a table:

| Event | Reported by | Reaction |
|---|---|---|
| `HM_PORT_OVERFLOW` | writer: no free slot | count |
| `HM_PORT_ALLOC_ERROR` | writer: an invalid handle from the free queue | count |
| `HM_PORT_FREE_ERROR` | reader: an invalid handle, or a handle in an unexpected state | count |
| `HM_PORT_ACCESS` | any port call from an unbound task | halt (`configASSERT`) |
| `HM_STALE_READ` | sampling-port reader | count |
| `HM_DEADLINE_MISS` | frame task, late service start | count |

Counts are incremented inside a critical section, so several tasks can report. The dump carries the counters, and
the analysis checks them against the per-message records. For example, `deadline_miss` must equal the number of
waits above 20 ms.

## 7. Frame timer (`frame_timer.c`)

Until step 6 the frame task used `xTaskDelayUntil` on the tick. The producer also wakes on the tick, from its
`vTaskDelay`. On the shared edge the frame task (priority 5) runs first, finds nothing, and the producer's message
then waits almost a full frame: 988 µs in record 09. The phase of the two tasks was an accident of scheduling, not a
design parameter.

Now the frame comes from TIM2's compare channel 1:

- TIM2 is already the 1 µs free-running counter used for every timestamp, so frames and measurements share one
  clock. SysTick (HCLK) and TIM2 (APB1 × 2) come from the same PLL, so the phase does not drift.
- `frame_timer_start`, called at run start inside a critical section:
  - computes the last tick edge as `TIM2->CNT − (SysTick->LOAD − SysTick->VAL) / 168`;
  - sets the first compare to edge + `ARDA_FRAME_PHASE_US` (500 µs), or one frame later if that is less than 20 µs
    away;
  - enables the compare interrupt.
- `frame_timer_isr` (TIM2_IRQHandler, NVIC priority 5):
  - clears the flag, advances `CCR1` by 1 000 µs and notifies the frame task on notification index 1. Index 0 is
    used for the run-start notification.
  - If the interrupt itself ran late, it notifies once for every frame that has passed; the task counts those as
    missed frames (`frame_miss`) and still advances its trips by the real frame count.
- `frame_timer_wait` blocks on index 1. If no frame arrives within two periods, the clock has stopped, and
  `configASSERT` halts.
- `frame_timer_stop` disables the interrupt at the end of the run.

At every frame start the frame task measures the elapsed time since the tick edge from the SysTick counter and keeps
the minimum and maximum (`frame_phase_min/max_us`).

In the host simulation (the FreeRTOS Windows port) there is no sub-tick clock, so the simulator's frame timer is
tick-based with phase 0. The phase effect is measurable only on the board. Record 14's trace shows the tick-to-TIM2
interval as 498.9–500.1 µs over 2 051 frames, and frames 999.90–1000.18 µs apart.

## 8. Memory layout (`mem_layout.h`, `stm32f407vg_flash.icf`)

| Region | What lives there | Why |
|---|---|---|
| **CCM RAM**, 0x1000 0000, 64 KB | main stack (at the very base), all task stacks and TCBs, the frame task's inbox, couriers and counters | CPU-only data bus: no DMA contention. A main-stack overflow runs into the reserved range below 0x1000 0000 and faults instead of corrupting data. |
| **Main SRAM**, 0x2000 0000, 128 KB | port message slots, queues, UART buffers, SystemView/RTT buffer, everything else | Anything a DMA or the debugger may read has to be here. In a real link the DMA would read the message slots. |

- `CCM_DATA` places a zero-initialised static object in `.ccmram`, and the linker zero-fills it at start-up. The
  macro is empty in the host simulation.
- Defining `CSTACK_OVERFLOW_TEST` builds an image that recurses on the main stack until it faults; it should end in
  `HardFault_Handler`.

RAM in use, from the linker map:

| Build | Main SRAM | CCM | Total |
|---|---|---|---|
| Tracing on | 25 375 B (19.4 %) | 12 420 B (19.0 %) | 37 795 B |
| Tracing off | 7 286 B (5.6 %) | 12 420 B (19.0 %) | 19 706 B |

SystemView's buffer is 18 085 B of the traced build. Task stacks have 3–10× headroom over the IAR stack analysis.

## 9. Timing budget (record 14, tracing on)

| Item | Value | Note |
|---|---|---|
| Frame period | 999.90–1000.18 µs | BUT-01: 1 ms ± 10 µs |
| Phase, tick → TIM2 interrupt | 498.9–500.1 µs | BUT-07 |
| TIM2 interrupt | 8.9 µs | notify + context-switch request, traced |
| Empty frame | 26.5–27.0 µs | about 2.7 % of the CPU, every frame |
| Frame that takes or delivers one message | 42.5–44.9 µs | ≈ 13–18 µs per message, traced |
| Burst frame: 18 intakes + 6 trip starts | 292.4 µs | BUT-02 budget is 300 µs |
| Heavy frame: 6 deliveries + 6 trip starts | 140.2–140.3 µs | every 2 ms during the drain |
| Producer, one attempt | ~27 µs | status read + `port_send` + trace events |

The burst frame shows the open item: the work of a frame grows with the number of messages it takes. At ~13 µs per
message, a frame that took all 32 port slots would need ~450 µs. BUT-04 (a per-frame message cap) closes this gap.
The cap can be derived from these numbers.

## 10. Run control and the dump

- **Command.** The ground station sends a 5-byte frame: `AA 55 'Q' arg checksum`.
  - Bits 0–2 of `arg` select the consumer: 0 A, 1 B, 2 C, 4 the frame consumer; 3 is rejected.
  - Bit 4 selects the burst load.
  - TelemetryTask handles the command and starts the run, or answers `QBUSY` while a run or its dump is in progress.
- **Run states.** `IDLE → RUNNING → DUMPING → IDLE`. The consumer of the run reports the end. UartTxTask, the only
  owner of the UART, dumps the records and marks the board idle before the last line.
- **Lines.** Every dump line is 64 bytes:
  - `QRUN,<run>,<variant>,<load>,<capacity>,<wait_ticks>,<service_ms>,<gap_ms>,<trace>`
  - `QMSG,<run>,<seq>,<ok|fail>,<t_us>,<send_us>,<g2r_us>,<svc_us>,<depth>`, one per attempt
  - `QCNT,<run>,<name>,<value>`, the counters below
  - `QEND,<run>,<records>`
- **Counters in every run:** `attempted`, `accepted`, `failed`, `consumed`, `queue_max`, `rx_idle_timeout`,
  `order_error`, `run_us`, `stack_hwm_producer`, `stack_hwm_consumer`.
- **Frame consumer only:**
  - Frame timing: `frames`, `frame_miss`, `frame_dt_min_us`, `frame_dt_max_us`, `frame_work_max_us`.
  - Intake and inbox: `intake_max_us`, `inbox_max`.
  - Port and handles: `pool_empty`, `pool_alloc_error`, `pool_free_error`, `pool_leak`, `dump_retry`.
  - Deadline: `deadline_miss`.
  - Status port: `status_writes`, `status_reads`, `status_stale`, `status_missed`, `status_age_max_us`,
    `status_backlog_max`.
  - Configuration, so that a run on the wrong image fails its checks: `couriers`, `courier_busy_max`, `batch`,
    `batch_max`, `frame_phase_us`, `frame_phase_min_us`, `frame_phase_max_us`.

## 11. Verification

- **Compile time:** the load-contract and sizing rules (§2), trip = whole frames, couriers ≤ free UARTs, port
  capacity < 255 handles, sampling message ≤ 32 B, frame phase inside the frame.
- **Host simulation** (`tests/host/sim/`). The real `queue_exp.c`, `arda.c`, `port_layer.c`, `port_config.c`,
  `health.c` and `protocol.c` run on the FreeRTOS Windows port.
  - A and B must lose exactly 15–34 and 27–34.
  - C may lose only inside the burst, and only after a full 20 ms block in the send call.

  The frame consumer must:
  - deliver 44/44;
  - send and receive 44 on the port, with no overflow and no handle error;
  - keep `deadline_miss` equal to the late records and to the health monitor's count;
  - write the status port once per frame and read it once per attempt;
  - keep both couriers busy in the burst and fill its batches;
  - start services in sequence order.
- **Protocol test** (`tests/host/test_queue_exp.c`). Every dump line is encoded within 64 bytes, and the counter names
  match the enum.
- **Fault injection.** A copy of the port layer that posts an invalid handle on the 20th send. The message is
  accepted but never served, and 4 checks turn red: service-side loss, handle error, handle not returned, and an
  accepted message without a wait.
- **Analysis checks** (`analysis/queue_experiment.py`, the ground station): run on every dump. They cover counter
  identities, loss on both sides, deadline count vs. records, status freshness, configuration vs. record, and service
  start order.

## 12. Known limitations and next steps

- **BUT-04, a per-frame message cap**, derived from the measured ~13 µs per message, so that the frame budget holds
  for any burst.
- **Stack sizes from evidence (RAM-03).** Shrink the stacks to 1.25 × (analysed + exception frame). Encode the real
  interrupt nesting depth as a `check that` rule in `stack_usage.suc`: IAR's sum of all interrupts (2 136 B) assumes
  nesting that cannot happen.
- **A receiver for two links:** merging by sequence number, and a "link down" health event. With one link left,
  capacity halves and deadline misses return.
- **MPU partitioning** of the frame task's data, the step from layout isolation to enforced isolation.
- **Firmware identity in every dump (GEN-04)** and a saved dump for a full steps 1–6 run with tracing on and off.
- **A load-contract stress test (YUK-06)** with 32- and 33-message bursts.
- **A TIM2 name in the SystemView system description**, so traces show "TIM2" instead of "ISR 44".
