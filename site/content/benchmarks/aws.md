---
title: "AWS Benchmarks"
description: "AWS c7i cross-cloud validation results."
weight: 2
---

## Platform

AWS c7i instances, Intel Xeon Platinum 8488C. Tested at 8 and
16 vCPU to validate that GCP results are not platform-specific.

## Cross-Cloud Consistency

| vCPU | GCP HD/TS | AWS HD/TS |
|-----:|----------:|----------:|
| 8 | 2.0x | 2.0-2.2x |
| 16 | 1.6x | 1.3x |

The advantage is architectural, not platform-specific. Both
Intel platforms show the same performance characteristics.

## Significance

Cross-cloud testing confirms that the throughput advantage
comes from the io_uring + kTLS + shard-per-core architecture,
not from any GCP-specific optimization or hardware feature.

## Full Report

The detailed AWS report is in the
[HD-bench-data](https://github.com/hyper-derp/HD-bench-data)
repository.
