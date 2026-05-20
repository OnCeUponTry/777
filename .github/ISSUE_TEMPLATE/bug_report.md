---
name: Bug report
about: Something broken in the compiler, runtime, or self-host chain
title: '[bug] '
labels: bug
---

## Describe the bug

A clear description of what went wrong.

## Steps to reproduce

```bash
# Exact commands you ran:
./bootstrap/gen0_compilador_777.elf < my_input.777m > out.elf
chmod +x out.elf
./out.elf
```

If you can paste the `.777m` source that triggers the bug, please do.

## Expected behavior

What you expected to happen.

## Actual behavior

What actually happened. Include exit codes, stdout/stderr if any.

## Environment

- OS: (e.g. Ubuntu 24.04, NixOS 25.05)
- Architecture: (must be x86_64; ARM/RISC-V not supported yet)
- Output of `./run_tests.sh` (does the baseline 60/60 pass?):

```
(paste output)
```

## Additional context

Anything else relevant: compiler version, custom modifications, etc.
