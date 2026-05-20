# 777

**An adaptive, self-hosting programming language with a fully auditable bootstrap chain — from 357 bytes of hand-typeable hex0 to a working compiler that compiles itself byte-perfectly.**

```
hex0 (357 bytes, hand-typed)
   ↓ stage0-posix (audited public tools)
M2-Planet (C-subset compiler, ~64 KB)
   ↓ compiles compilador_777.c (production compiler in C subset)
compilador_777.elf
   ↓ compiles self-host/compilador_777.777m (compiler written in 777 itself)
gen0_compilador_777.elf  ←  fixed point: compiles itself byte-perfect
```

Verify the entire chain end-to-end on your own machine. No `cargo install`, no `apt-get install gcc`, no `curl ... | sh`. Just bytes you can read with your eyes and `bash`.

---

## Why 777

Three properties define the language:

### 1. Auditable from 357 bytes upward

The trust chain starts at `hex0`, a tiny human-readable hex format you can verify by hand. From there, [stage0-posix](https://github.com/oriansj/stage0-posix) bootstraps a C-subset compiler (M2-Planet) in a sequence of micro-steps anyone can audit. M2-Planet then compiles `compilador_777.c` (a few thousand lines of C-subset source) into a working ELF. That ELF compiles `self-host/compilador_777.777m` (the compiler rewritten in 777 itself) into the binary you actually use. At no point is there a multi-megabyte opaque blob you must trust.

If you don't trust the binary shipped in this repo, you can regenerate it from `hex0` and verify byte-for-byte equality with what's distributed. That's the entire point.

### 2. Adaptive — the language compiles itself

The compiler is written in 777. You modify it in 777. You rebuild it with itself. There is no external toolchain that has to keep up with the language; it keeps up with itself. Add a new opcode, a new syntactic construct, a new optimization — and the next generation of the compiler is built by the previous one. The fixed point `gen2 == gen3` proves convergence: the compiler reaches a self-consistent form and stays there.

This is what "adaptive" means here. The language is not frozen against an external authority. It evolves through self-application.

### 3. Zero opaque runtime dependencies

No libc, no LLVM, no Rust toolchain, no GCC. The compiled programs talk directly to Linux syscalls. The compiler does the same. The only external dependency for a full from-scratch rebuild is `stage0-posix` (also fully auditable), and even that is only needed if you don't trust the shipped binary.

If your software supply chain matters to you — for security research, for long-term archival, for educational integrity, for distrust of pre-compiled toolchains — 777 gives you a complete chain you can verify, modify, and own.

---

## What 777 is useful for today

Real programs you can write in 777 right now (verified by the 60 tests + 8 demos in `examples/`):

- **CLI tools and filters**: parsers, log analyzers, format converters, hex viewers, line counters.
- **TCP servers and clients**: HTTP/1.0, IRC, custom protocols. Socket / Bind / Listen / Accept / Connect are all wired.
- **Multi-process supervisors**: mini-shells, init systems, daemon managers. Fork / Execve / Wait4 / Pipe / Dup2 are operational.
- **File processors**: memory-mapped tools via Mmap_fd / Munmap, classic Open/Read/Write/Close, ELF parsers.
- **TUI text-mode programs**: calculators, menus, REPLs (`examples/demo_*.777m`).
- **TUI fullscreen ncurses-like**: editors, pagers, panel managers (Ioctl + ANSI escapes; demos pending).
- **Direct framebuffer drawing**: `/dev/fb0` via Open + Ioctl + Mmap.
- **Event-driven daemons**: Poll + Nanosleep + GetTimeOfDay for timer-driven work.
- **The 777 compiler itself**: 3,900 lines of `.777m` that compiles to a 62 KB ELF that compiles itself byte-perfectly.

### Not yet supported (with planned versions)

- Direct function recursion (`F` calling itself): planned for `v0.3`. Call chains `A → B → C` already work.
- Floating point: planned for `v0.5`.
- Threading: planned for `v0.8`.
- C FFI: deferred (not on near roadmap).
- Module / import system: planned for `v0.4`.

See [CHANGELOG.md](CHANGELOG.md) for the full versioning plan.

---

## 30-second demo

```bash
git clone https://github.com/OnCeUponTry/777.git && cd 777

# Run all 60 tests using the shipped self-host fixed-point compiler:
./run_tests.sh
# → PASS 60 / FAIL 0

# Write and run your first 777 program:
cat > hello.777m <<'EOF'
D 0 0 msg Text "Hello, world!\n"
D 0 0 len Integer 14
R 0 0 FdWrite [1, msg, len] _
R 0 0 ExitTop [0]
EOF

./bootstrap/gen0_compilador_777.elf < hello.777m > hello.elf
chmod +x hello.elf
./hello.elf
# → Hello, world!
```

## Verify self-host in 4 commands

```bash
# Compile the compiler-in-777 with the included compiler:
./bootstrap/gen0_compilador_777.elf < self-host/compilador_777.777m > gen1.elf
chmod +x gen1.elf

# Use gen1 to compile itself:
./gen1.elf < self-host/compilador_777.777m > gen2.elf
chmod +x gen2.elf

# Fixed-point reached: gen2 byte-equal to the shipped gen0:
cmp gen2.elf bootstrap/gen0_compilador_777.elf
# → (no output = identical)

# And idempotent: gen2 compiled with itself = same gen2:
./gen2.elf < self-host/compilador_777.777m > gen3.elf
cmp gen2.elf gen3.elf
# → (identical = self-host fixed point)
```

This is the strongest possible self-hosting evidence: the compiler written in 777 compiles itself to a byte-identical binary, repeatedly.

---

## Language at a glance

777 is small, stack-based, and Forth-flavored. Every program is a sequence of three line types:

```
# Declarations (data):
D <row> <col> name Integer 42
D <row> <col> hello Text "Hello\n"

# Reductions (operations):
R <row> <col> Add [a, b] result
R <row> <col> FdWrite [1, hello, 6] _
R <row> <col> ExitTop [result]

# Functions (Sub-A params + Sub-B return):
F <row> <col> square [n]
R 0 0 Mul [n, n] sq
R 0 0 Ret [sq]
R 0 0 EndF

R 0 0 Call [square, 6] r
R 0 0 ExitTop [r]   # exits with 36
```

### 52 opcodes in five families

| Family | Opcodes |
|---|---|
| Arithmetic | Add, Sub, Mul, Div, Mod, AddR12 |
| Bitwise | BitAnd, BitOr, BitXor, Shl, Shr |
| Comparison | Eq, Lt, Gt |
| Control flow | Halt, Jmp, Jz, Call, Ret, ExitTop, Label (parser-level) |
| Memory | Alloc, LoadByte, StoreByte, LoadI64, StoreI64, LoadImm32, LoadImm64, LoadVar, StoreVar, LoadVar16, StoreVar16 |
| Syscalls | FdRead, FdWrite, Open, Close, Ioctl, Nanosleep, Poll, Fork, Execve, Wait4, Pipe, Dup2, Socket, Connect, Bind, Listen, Accept, MmapFd, Munmap, GetTimeOfDay, RtSigAction |

### Scope and references

- **Sub-A**: function parameters via auto-pop from the value stack.
- **Sub-B**: function return value via `Ret [val]` (also `Ret []` for void).
- **Sub-C**: variables declared inside `F` are scope-local by default. Access top-level globals from inside `F` with the `__name` prefix.
- **Sub-E**: named labels (`Label [name]`) with forward references resolved by the parser via a fixup table.
- **Sub-G**: `EndF` explicitly closes a function body, allowing multiple `Ret` statements inside.
- **Lote 4**: variable slots up to 65,535 (compiler auto-promotes single-byte slot opcodes to 16-bit when needed).

See [examples/](examples/) for working programs covering every opcode family.

---

## Build from source (full auditable chain)

The repo ships `bootstrap/gen0_compilador_777.elf` (62 KB) for immediate use. If you don't trust that binary and want to regenerate it from 357 bytes of `hex0`:

```bash
# 1. Get stage0-posix and bootstrap it to working M2-Planet:
git clone https://github.com/oriansj/stage0-posix
cd stage0-posix
bash kaem.amd64   # ~5-10 minutes, fully auditable chain hex0 → M2-Planet
export STAGE0_POSIX=$(pwd)

# 2. Build compilador_777.c with M2-Planet:
cd /path/to/777/bootstrap
./build.sh compilador_777.c compilador_777.elf

# 3. Use the C-built compiler to produce the self-host fixed point:
./compilador_777.elf < ../self-host/compilador_777.777m > /tmp/local_gen1.elf
chmod +x /tmp/local_gen1.elf
/tmp/local_gen1.elf < ../self-host/compilador_777.777m > /tmp/local_gen2.elf
chmod +x /tmp/local_gen2.elf

# 4. Verify your locally-built binary matches the shipped fixed-point:
cmp /tmp/local_gen2.elf gen0_compilador_777.elf
# → identical = full chain verified end to end
```

The build is **deterministic**: identical source → byte-identical ELF. No timestamps, no random UUIDs, no embedded paths.

---

## Project status

This is **alpha software** (`v0.7.0-alpha`). The language is internally consistent and the self-host loop is closed and verified, but the surface area is small.

| Capability | State |
|---|---|
| Bootstrap chain `hex0 → ELF` | ✅ Working and verifiable end-to-end |
| Source-level self-host (compiler in 777) | ✅ 3,900 LOC `.777m` |
| Byte-perfect self-host fixed point | ✅ `gen2 == gen3` verified |
| 60 tests via host C compiler | ✅ 60/60 |
| 60 tests via self-host compiler | ✅ 60/60 |
| Zero Rust / GCC / LLVM in the production chain | ✅ Verified |
| Direct function recursion | ❌ Planned `v0.3` |
| Floating point | ❌ Planned `v0.5` |
| Production-ready stable | ❌ Alpha |

Honest scope: **useful today** for small CLI tools, network utilities, supervisors, educational projects, security research, and as a reproducible auditable bootstrap testbed. **Not yet useful** for numerical computation, multithreaded software, or large applications requiring recursion or floats.

---

## Getting help

- Read the [CHANGELOG.md](CHANGELOG.md) for what's in and what's coming.
- File an issue using one of the templates in [`.github/ISSUE_TEMPLATE/`](.github/ISSUE_TEMPLATE/).
- For broader bootstrap / auditable-builds discussion, the [Bootstrappable Builds](https://bootstrappable.org/) community is excellent.

---

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md).

The bootstrap chain is sacred. Any change that breaks `cmp gen2.elf gen3.elf` or drops the 60/60 test pass rate is a regression.

---

## License

Apache License 2.0 with LLVM Exception. See [LICENSE](LICENSE) and [NOTICE](NOTICE).

**Plain English summary**:

- You can use, modify, and distribute 777 freely, including in commercial and proprietary products.
- You must keep the copyright notice and the NOTICE file with any redistribution.
- Forks may not call themselves "777" or otherwise imply endorsement by the original authors (Apache 2.0 §6).
- The LLVM Exception means **programs you compile with 777 inherit no licensing obligation** from the compiler. You may license your `.777m` code however you want (proprietary, MIT, GPL, anything).

---

## References

- [stage0-posix](https://github.com/oriansj/stage0-posix) — the 357-byte seed of the bootstrap chain
- [M2-Planet](https://github.com/oriansj/m2-planet) — the minimal C-subset compiler used in stage 2
- [Bootstrappable Builds](https://bootstrappable.org/) — the broader project this aligns with
- [GNU Mes](https://www.gnu.org/software/mes/) — a complementary auditable bootstrap
- [Reflections on Trusting Trust](https://www.cs.cmu.edu/~rdriley/487/papers/Thompson_1984_ReflectionsonTrustingTrust.pdf) — Ken Thompson's classic paper on the problem 777 attempts to solve
