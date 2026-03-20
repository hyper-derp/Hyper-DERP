---
title: "Bare-Metal Profiling"
description: "Haswell microarchitecture profiling results."
weight: 3
---

## Platform

Bare-metal Haswell server with hardware performance counters.
Used for microarchitectural profiling (cache behavior, branch
prediction, IPC) rather than throughput comparison.

## Purpose

Bare-metal profiling complements the cloud benchmarks by
providing hardware-level insight into where CPU cycles are
spent. Key findings:

- The forwarding hot path is cache-friendly due to
  data-oriented struct layout.
- SPSC ring transfers avoid cache-line bouncing between
  workers.
- kTLS eliminates userspace crypto from the profile entirely.

## Full Report

The detailed Haswell profiling report is in the
[HD-bench-data](https://github.com/hyper-derp/HD-bench-data)
repository.
