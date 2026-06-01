# Dynamic Connectivity Benchmark

This directory contains small benchmark traces and helper scripts for testing dynamic connectivity indexes in the Kuzu/LadybugDB prototype.

## Trace Format

Each non-comment line has either three or four whitespace-separated fields:

```text
OP u v
OP u v expected_connected