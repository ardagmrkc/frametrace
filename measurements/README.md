# Measurement data

Every run on the board produces four files. They are written either by the ground station (`interface/index.html`,
*save run*) or by `analysis/queue_experiment.py`. The board keeps all results in RAM during the run and sends them
over UART after it. The files are that dump, decoded.

## Records in this directory

| Record | Consumer | Load | Trace | Files | Result |
|---|---|---|---|---|---|
| 01 | A: capacity 4, no wait | normal | on | `kayit01_A_normal_iz_acik*` | 44/44 |
| 02 | B: capacity 16, no wait | normal | on | `kayit02_B_normal_iz_acik*` | 44/44 |
| 03 | C: capacity 4, 20 ms wait | normal | on | `kayit03_C_normal_iz_acik*` | 44/44 |
| 04 | A | burst | on | `kayit04_A_yogun_iz_acik*` | 24/44, 15–34 lost |
| 05 | B | burst | on | `kayit05_B_yogun_iz_acik*` | 36/44, 27–34 lost |
| 06 | C | burst | on | `kayit06_C_yogun_iz_acik*` | 34/44, 16, 18, …, 34 lost |
| 08 | Frame consumer, step 1 (20 ms service) | burst | on | `kayit08_Arda_yogun_iz_acik*` | 44/44, worst wait 460.67 ms |
| 09 | Frame consumer, steps 1–2 (transmission-time service) | burst | on | `kayit09_Arda2_yogun_iz_acik*` | 44/44, worst wait 23.67 ms |
| 14 | Frame consumer, steps 1–6 | burst | on | trace only: `analysis/systemview/14Arda_yogun_2_v2.SVDat` | 44/44, worst wait 6.86 ms (from the trace) |

- **Record 07** (baseline A with tracing compiled out) is defined in the ground station but is not stored here.
- **Records 10–13** were never taken; steps 3–5 were verified in the host simulation.
- **Record 14's dump was not saved.** Its numbers come from decoding its SystemView trace (see "Traces" below).

File names keep the Turkish terms used by the tools: `kayit` = record, `yogun` = burst load, `iz_acik` = trace on,
`iz_kapali` = trace off. In file names the frame consumer appears as `Arda` (step 1) and `Arda2` (step 2 onwards),
the names its variants carry in the ground station.

## Files of one record

### `kayitNN_<variant>_<load>_iz_<acik|kapali>.csv`: one row per send attempt

| Column | Meaning |
|---|---|
| `kayit`, `tekrar` | Record number and repetition. |
| `board_run` | The board's run counter since reset. |
| `variant`, `load`, `trace` | Consumer, load (`normal` / `yogun`), whether tracing was compiled in. |
| `seq` | Attempt number, 1–44. |
| `phase` | `normal` or `art_arda` (attempts 11–34 of the burst load). |
| `send_result` | `accepted` or `failed`. |
| `t_gen_us` | Generation time, µs since the first attempt of the run (TIM2). |
| `send_call_us` | Duration of the send call. For a failed send in variant C this is the blocked time. |
| `generation_to_receive_us` | g2r: from generation to the start of the message's service. Empty for a failed send. |
| `service_us` | From service start to service end. Empty for a failed send. |
| `queue_depth_after_send` | Messages waiting in the queue / message port right after the send. |

### `…_counters.csv`: the run's counters, as `name,value`

Every run:

| Counter | Meaning |
|---|---|
| `attempted`, `accepted`, `failed`, `consumed` | Send attempts, accepted sends, failed sends, messages whose service finished. |
| `queue_max` | Highest queue depth seen after a successful send. |
| `rx_idle_timeout` | Baseline consumer only: the receive wait timed out before the producer finished. Must be 0. |
| `order_error` | A message out of order or from another run. Must be 0. |
| `run_us` | From the first generation to the end of the last service. |
| `stack_hwm_producer`, `stack_hwm_consumer` | Least free stack since reset, in words. The consumer entry is the frame task for frame-consumer runs. |

Frame consumer only, grouped as in [`firmware/Core/Inc/protocol.h`](../firmware/Core/Inc/protocol.h):

| Group | Counters |
|---|---|
| Frame timing | `frames`, `frame_miss`, `frame_dt_min_us`, `frame_dt_max_us`, `frame_work_max_us`, `frame_phase_us`, `frame_phase_min_us`, `frame_phase_max_us` |
| Intake | `intake_max_us`, `inbox_max` |
| Message port and handles | `pool_empty` (port full), `pool_alloc_error`, `pool_free_error`, `pool_leak` (must all be 0) |
| Deadline | `deadline_miss`: services that started more than 20 ms after generation. They are served, never dropped. |
| Status port (sampling) | `status_writes`, `status_reads`, `status_stale`, `status_missed`, `status_age_max_us`, `status_backlog_max` |
| Configuration | `couriers`, `courier_busy_max`, `batch`, `batch_max` |
| Dump | `dump_retry` |

Records 08 and 09 were taken with firmware from before the port layer, so the deadline, status-port, courier, batch
and phase counters are absent there. The analysis treats a missing counter as "not measured", never as 0.

### `…_meta.json`

Run definition (`kosu`), serial port and the board's confirmation line (`seri`), source of the data (`yazilim`), the
SystemView recording if one was taken (`systemview`), every consistency check with its result (`kontroller`) and the
summary (`ozet`).

The ground station cannot read which firmware commit is flashed on the board, so `yazilim` names only the tool.
Requirement GEN-04 is open for this reason.

### `…_raw.txt`

Every byte exchanged with the board, with timestamps: `>` for bytes sent to the board (the command frame), `<` for
lines received (`QRUN`, `QMSG`, `QCNT`, `QEND`). Every summary can be recomputed from this file.

## Summaries

- `queue_summary.csv`: one row per stored record, written by `python analysis/queue_experiment.py --ozet` from the
  `*_meta.json` files.
- `queue_summary_tarayici.csv`: the same table as exported by the ground station (`tarayici` = browser).
- `../analysis/karsilastirma.md` and `../analysis/plots/`: single-factor comparisons and plots of the baseline runs,
  written by `python analysis/analyze.py`.

## Traces

SystemView recordings are in [`../analysis/systemview/`](../analysis/systemview/). Open them with
*File → Load Recording*.

| File | Content |
|---|---|
| `A_normal`, `A_yogun`, `B_normal`, `B_yogun`, `C_normal`, `C_yogun` | Baseline runs (the README's C0–C7 come from `C_yogun`). |
| `Arda_yogun_2` | Frame consumer before step 6 (frame driven by the tick). |
| `14Arda_yogun_2`, `14Arda_yogun_2_v2` | Two recordings of record 14 (steps 1–6). The README's F1–F10 and every record-14 number come from `_v2`. |
| `eski/`, `../systemview_test/` | Traces from the earlier PressTrace measurement chain (button and telemetry scenarios), kept for reference. |

Record 14's numbers come from decoding the trace:
- **Frame phase and period:** from the SysTick and TIM2 interrupt entries.
- **Message timing:** each message's generation (`SEND_BEGIN`), intake (`RECEIVE`) and delivery (`DONE`) events.
- **Service starts:** reconstructed from the frame starts, each message's position in its trip and the trip length.
- **Dropped events:** the recording contains no OVERFLOW packet, so no event was lost.
