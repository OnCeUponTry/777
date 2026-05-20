# Changelog

All notable changes to 777 will be documented here.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).
Versioning follows the "Ritmo del 7": `MAJOR.MINOR.PATCH+YYYYMMDD`. Every 7 patches bump minor; every 7 minors (49 patches) bump major. Releases ending in `.7` are visual/ceremonial.

## [0.7.0-alpha] - 2026-05-19

First public release. Self-host fixed point reached.

### Added

- Self-host CIRCULAR fixed point: `cmp gen2.elf gen3.elf` is byte-perfect.
- Compiler in `.777m` source (`self-host/compilador_777.777m`, ~3,900 LOC) that compiles itself byte-identically.
- 52 VM opcodes: arithmetic, bitwise, comparison, control flow, file I/O, sockets, processes (Fork/Execve/Wait4/Pipe/Dup2), memory mapping (Mmap_fd/Munmap), time (Nanosleep, Poll, GetTimeOfDay), signals (Rt_sigaction), ioctl.
- Sub-A (function parameters + Call args), Sub-B (Ret val + Call result + DROP slot), Sub-C (scope-local cells inside `F`), Sub-E (named labels with fixup tables), Sub-G (explicit `EndF` marker for multi-Ret support).
- Lote 4: dual-opcode 16-bit slot encoding (`OP_LOAD_VAR16`/`OP_STORE_VAR16`) with parser auto-promotion when slot ≥ 256.
- 60 functional tests in `examples/`, all green via host C compiler and via the self-host fixed-point binary.
- Apache 2.0 with LLVM Exception license.
- `bootstrap/gen0_compilador_777.elf` (62 KB) ready-to-use self-host fixed-point binary.

### Removed

- `compilador_777.elf` (M2-Planet build artifact, ~1 MB) is no longer shipped in the repo. It is regeneratable from `bootstrap/build.sh` using stage0-posix. The self-host fixed-point `gen0_compilador_777.elf` is sufficient for all usage.
- `v3_elf_source.777m` legacy snapshot (1,134 LOC, from the abandoned Rust-era bootstrap) removed. Replaced by the actual self-host source `self-host/compilador_777.777m`.

### Fixed

- **B10**: three inverted `Jz` patterns in `self-host/compilador_777.777m` that prevented the self-compiled compiler from working correctly:
  - `emit_load_arg_at` integer-literal path.
  - `func_resolve_fixups` patch-vs-skip logic.
  - `compile_line_F` row/col digit-skipping loops.
  These caused 20/60 tests to fail via self-compiled before; now 60/60.

### Known limitations (tracked for next versions)

- Direct F-recursion not supported (`v0.3` will introduce stack-frame calling convention).
- No floating point yet (`v0.5`).
- No threading (`v0.8`).
- No module/import system (`v0.4`).
- No C FFI.

[0.7.0-alpha]: https://github.com/OnCeUponTry/777/releases/tag/v0.7.0-alpha

