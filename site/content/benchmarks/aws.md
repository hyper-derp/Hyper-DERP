---
title: "AWS c7i Results"
description: >-
  AWS c7i benchmark results for cross-cloud validation.
weight: 3
draft: true
---

## Setup

- **Platform**: AWS c7i instances (Ice Lake Xeon)
- **Configs**: 8 vCPU, 16 vCPU
- **TLS mode**: kTLS for Hyper-DERP, Go TLS for Tailscale

## Purpose

Cross-cloud validation ensures the performance advantage
isn't specific to GCP's c4-highcpu microarchitecture or
network stack. AWS c7i uses a different hypervisor and
NIC.

<!-- TODO: add throughput table from AWS report -->
<!-- TODO: add plots from static/img/ -->
