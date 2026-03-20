---
title: "GCP Plain TCP Results"
description: >-
  GCP c4-highcpu full sweep results without TLS overhead.
weight: 2
draft: true
---

## Setup

- **Platform**: GCP c4-highcpu instances
- **TLS mode**: plain TCP (no encryption) for both servers
- **Purpose**: isolate the relay forwarding overhead from
  TLS overhead

## Results

Plain TCP numbers show higher absolute throughput and
larger HD/TS ratios, since Hyper-DERP's io_uring data
plane has less overhead than Go's net poller even without
the kTLS advantage.

<!-- TODO: add throughput table from full sweep report -->
<!-- TODO: add plots from static/img/ -->

The difference between TCP and kTLS results quantifies
the cost of kernel TLS on each platform.
