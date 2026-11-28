Rugxulo supplied these three BASIC programs on 27 November 2026 to reproduce
DOS16 string-space exhaustion and a NULL-assignment warning in GW-BASIC 2026.
The source files and LICENSE.PD are preserved byte for byte from the attachment.
See LICENSE.PD for the public-domain dedication.

tests/run_dos_strings.py runs them against a small, generated assembly-text
fixture. It checks LOAD/RUN, LOAD/MERGE/RUN, command-line startup, and both ways
of setting CLEAR ,12288. If the larger pool cannot fit after LOAD on DOS16,
the test requires an Out of memory error at CLEAR, then removes that line and
runs the converter using the retained pool. A separate allocation-failure test
in test_runtime_types.py checks that failed resizing preserves the pool.
All converted output files are compared byte for byte
with independently specified expected text. No assembler is invoked.

The original report used PSR Invaders 1.1's INVADERS.ASM. That separately licensed
file is not included or downloaded by CI. Full-input acceptance testing should
use its MD5 c045418cc6d059b9cdf5082cd4951ed6 and these output MD5s supplied by
Rugxulo:
  TIDY.ASM    c7d04a498b303a2413765edbddc4aa48
  HEXNUM.OUT  bebb081efb00514bfb0512ff1f69e70d
  INV.A86     2b4ac03187060380585972f9cfd125bd
  INV.INC     c5231b409bd91f9f664da0d9ac95b693
