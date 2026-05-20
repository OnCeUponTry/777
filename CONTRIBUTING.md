# Contributing to 777

Thanks for your interest. This is an alpha-stage language with a small surface, so contributions are very welcome — but please read this short doc first to keep things sane.

## Ground rules

1. **The bootstrap chain is sacred**: `hex0 → M2-Planet → compilador_777.c → ELF → self-host fixed-point` must remain auditable and byte-perfectly reproducible. Any change that breaks `cmp gen2.elf gen0_compilador_777.elf` is a regression.
2. **Zero new runtime dependencies**. No libc, no LLVM, no Rust toolchain, no curl. Linux syscalls only.
3. **Tests must stay green**. `./run_tests.sh` reports 60/60 today; any PR that drops this is rejected until fixed.

## Workflow

```
# 1. Fork and clone
git clone <your-fork-url>
cd 777

# 2. Make changes in your branch
git checkout -b your-feature-name

# 3. Run tests locally
./run_tests.sh

# 4. Verify self-host circular still works
./bootstrap/gen0_compilador_777.elf < self-host/compilador_777.777m > /tmp/gen1.elf
chmod +x /tmp/gen1.elf
/tmp/gen1.elf < self-host/compilador_777.777m > /tmp/gen2.elf
chmod +x /tmp/gen2.elf
cmp /tmp/gen2.elf bootstrap/gen0_compilador_777.elf
# (no output = OK)

# 5. Commit with a clear message
git commit -m "feat: short description of what you did"

# 6. Push and open a PR
```

## What's a good first PR

Easier (open issue first to discuss):

- Add a new `.777m` test program in `examples/`. Update `test_manifest.txt`.
- Document an existing opcode that isn't covered in README.
- Fix a typo in source comments.
- Add a missing edge-case test (e.g., negative integers, large strings, syscall error paths).

Harder (please open issue first):

- Adding a new opcode (requires updating `arch_x86.c` handler bytes + `compilador_777.c` parser + `self-host/compilador_777.777m` `arch_init` + tests).
- Touching the self-host source (`self-host/compilador_777.777m`). Subtle bugs (especially inverted `Jz` patterns) have bitten us before; see the commit log for examples.
- Changing the calling convention or function ABI.

## Coding conventions

- **C subset**: must compile with M2-Planet. No `malloc`, no local arrays, no forward declarations, no `char` literals > 127, no struct fields beyond simple types. See existing code in `bootstrap/compilador_777.c` for patterns.
- **`.777m` source**: lines start with `D` / `R` / `F` / `#` (comment) or `7` (header). Function bodies must close with `R 0 0 EndF`.
- **No emojis in code or comments**. ASCII only in source files; markdown like this can have whatever Markdown supports.

## Code of Conduct

See [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md).

## License

By contributing, you agree your changes are licensed under Apache License 2.0 with LLVM Exception, the same as the rest of the project.
