# AeroCore — flight booking driven by a 5-level MLFQ scheduler

Every booking, cancellation, check-in and refund is a *process*. Requests are queued, a simulated CPU
runs them through a 5-queue MLFQ, and each request is **committed to the database only when the
scheduler finishes it** — so scheduling order decides who wins a contested seat.

```
make            # builds ./aerocore
make test       # scheduler maths, aging, preemption, booking rules, real torn-write crash recovery
./aerocore      # then: demo, run, compare
```

## Scheduler

| Queue | Algorithm | CPU allotment before demotion | Notes |
|-------|-----------|-------------------------------|-------|
| Q1 (highest) | Round Robin, quantum 2 | 4 ticks | urgent requests are served first |
| Q2 | SJF (non-preemptive) | 4 | shortest total burst first |
| Q3 | Priority (non-preemptive) | 6 | 1 = most important |
| Q4 | SRTF (preemptive) | 8 | shortest remaining time, re-picked every tick |
| Q5 (lowest) | FCFS | — | |

* **Entry / demotion:** every request enters Q1. Using up a queue's allotment demotes it one level.
* **Preemption:** a ready process in a higher queue always preempts a lower queue (also SRTF inside Q4).
* **Aging:** a request that waits `age` ticks (default 25, `set age <n>`, 0 = off) without CPU moves
  up one queue and its priority number drops by one (min 1).
* **Priority:** urgent = 1; otherwise check-in 2, book 3, cancel 4, refund 5.
* **Burst (service time):** book 6, cancel 3, check-in 2, refund 8 ticks; override with `b=<n>`.
* **Metrics:** avg waiting / turnaround / response time, throughput, CPU utilisation.
* `compare` replays the *same* requests through MLFQ, FCFS, SJF and RR using the same simulator engine
  (a 1-queue configuration), so the numbers are directly comparable.

Each `run` is one scheduling epoch starting at t=0; arrivals are ticks within that epoch
(default: one request per tick, or `@<tick>`).

## Commands

```
flights | seats <flight> | pax | pax add <phone> <name...>
book <pax> <flight> [seat]        no seat = interactive seat map + prompt
cancel|checkin|refund <pnr>
   options: urgent   @<arrival tick>   b=<burst ticks>
pending | run | compare | demo
pnr <pnr> | history <pax> | txns
trace off|on|live | set quantum <n> | set age <n>
crash [k]      power loss during journal write k+1 of the next run
```

`demo` loads passengers, some existing bookings and a 13-request batch (two long jobs, an urgent
request, seat races, a cancel-vs-check-in race). `run` shows the commit order, per-request timings,
the Gantt chart, every promotion/demotion/preemption, and the metrics; `trace on` prints each tick with
the contents of all five queues and remaining burst times (`trace live` paces it).

## Booking rules

* Seat map is 10 rows × A–D per flight. A seat is taken while a booking is `BOOKED` or `CHECKED_IN`;
  cancelling releases it automatically (occupancy is derived from bookings, never stored separately).
* Duplicate prevention: a seat can't be double-booked, and a passenger can't hold two live bookings on
  one flight. Checked at submit time *and* again when the scheduler commits the request.
* PNR: 6 characters, unique. Status flow `BOOKED → CHECKED_IN`, `BOOKED → CANCELLED → REFUNDED`.
  Refund returns 80% of the fare. No cancelling after check-in.
* Passenger profiles, ticket history (`history`), and a transaction log (`txns`).

## Persistence and crash recovery

State lives in `aerocore.journal` (override with `AEROCORE_DB`): an append-only text file, one record per
line, each ending in an FNV-1a checksum and fsync'd. A booking change and its transaction are one line,
so they are atomic. At startup the journal is replayed; a line without a trailing newline or with a bad
checksum (a torn write) is discarded and the file is truncated back to the last good record.

Try it: `demo`, `crash 2`, `run` → the process dies mid-write after two commits. Restart and the
committed bookings are back, the torn one is gone, and the startup banner reports the discarded record.

## Files

`sched.c` MLFQ engine, baselines, Gantt, metrics · `airline.c` flights/passengers/bookings + journal ·
`main.c` CLI and demo · `test.c` self-check · `aerocore.h` shared types.

## Known limits

Fixed capacities (64 requests per batch, 256 passengers, 1024 bookings). The journal is never compacted.
Context switches cost zero ticks. No concurrent writers.
