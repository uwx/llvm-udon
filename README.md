# UDON backend

`llvm-udon` translates an LLVM module into VRChat Udon assembly (`.uasm`) plus a
heap image, which the Udon assembler turns into a program a `UdonBehaviour` can
run. The point is to reach VRChat from an existing frontend — Rust, C, Zig —
instead of writing UdonSharp by hand.

Three things ship:

* **`llvm-udon`** — the backend and driver: `.ll` in, `.uasm` + `.udonheap.json`
  out.
* **`tools/Udon.VM`** — a headless harness that assembles and executes that
  output against stub externs, so the whole pipeline is testable without a
  Unity editor.
* **`Assets/Udon/Editor`** (in the Unity project) — an importer that turns the
  same output into a `SerializedUdonProgramAsset`.

## Layout

An out-of-tree project, discovered by `llvm/projects/CMakeLists.txt` the same
way `llvm-cbe` and `llvm-msil` are. It is not a member of
`LLVM_TARGETS_TO_BUILD`, so nothing is registered in LLVM's `Targets.def` and
`llc -march=udon` does not exist; `llvm-udon` is the only entry point. A
text-only writer gains nothing from the `llc` path, and Udon's calling
convention is not something a machine-code emitter could express anyway.

```
lib/Target/UDON/
  UDONTargetMachine.{h,cpp}   data layout and the pass pipeline
  UDONWriter.{h,cpp}          module driver, slots, constants, file layout
  UDONWriterSelect.cpp        instruction selection
  UDONWriterMemory.cpp        byte memory and the shadow stack
  UDONWriterCalls.cpp         calling convention, caller-save, re-entry
  UDONWriterIntrinsics.cpp    intrinsic expansion
  UDONWriterRuntime.cpp       program slots, entry prologue, halt block
  UDONCallGraph.cpp           SCCs and the re-entrancy classification
  UDONExterns.def             generated extern table
  runtime/udon_rt.ll          hand-written runtime helpers
tools/llvm-udon/              the driver
tools/Udon.VM/                the headless harness
tools/Udon.Heap/              heap-image library, shared with the importer
tools/Udon.ExternProbe/       probes the real SDK wrapper for the extern table
scripts/gen-externs.py        regenerates UDONExterns.def and test/externs.json
test/                         lit suite
```

## Building

```
cmake -S . -B build -G Ninja \
      -DLLVM_DIR=<llvm-build>/lib/cmake/llvm
cmake --build build -j 8
```

The build type has to match LLVM's or linking fails with `LNK2038`, so it is
read from LLVM's config rather than defaulted. On Windows the compiler is found
only from a developer prompt:

```
"C:\Program Files\Microsoft Visual Studio\...\vcvars64.bat"
cmake -S . -B build -G Ninja -DLLVM_DIR=...
cmake --build build -j 8
```

The harness is a separate .NET build, and the driver copies `udon_rt.ll` beside
itself so a build tree is self-contained:

```
dotnet build tools/Udon.VM -c Debug
```

## Usage

```
rustc --target wasm32-unknown-unknown --emit=llvm-ir -O -C panic=abort \
      prog.rs -o prog.ll
llvm-udon prog.ll -o prog.uasm
```

That writes two files: `prog.uasm` and `prog.udonheap.json`. Both are needed —
the heap image carries every constant the assembler's literal syntax cannot
express (`i64`, `bool`, `f32`/`f64` as raw bits, and the globals image).

Running it headless:

```
dotnet run --project tools/Udon.VM -- run prog.uasm \
    --heap-json prog.udonheap.json --entry _start
```

```
ret _start = 9
```

In Unity, put the pair in `Assets/` and run **Tools > Udon > Import UASM
Programs**, which writes `Assets/SerializedUdonPrograms/<name>.asset` for a
`UdonBehaviour` to point at.

A construct the backend cannot express is one line on stderr and exit 1:

```
llvm-udon: error: dynamic alloca is not supported: f
```

The message names the symbol or type that caused it. There is no crash banner,
stack dump or dialog: the driver installs a fatal-error handler, so a rejection
is a diagnostic rather than an abort. (A genuine compiler bug still aborts,
which is the distinction worth keeping.)

### Why wasm32

The frontend has to target a 32-bit-pointer, little-endian layout, because a
pointer here is a byte offset into a 32-bit address space. `wasm32` is the
convenient way to get that from Rust without a custom target; the backend
checks the module's data layout and rejects anything else by name rather than
silently miscompiling.

## Options

| Option | Effect |
|---|---|
| `-udon-memory-bytes=N` | End of the globals image, i.e. the byte offset where the shadow stack begins. Default `1 << 20` (1 MiB). Must be a positive multiple of 16. |
| `-udon-shadow-stack-bytes=N` | Bytes of `alloca` shadow stack above the globals image. Default `1 << 16` (64 KiB). Must be a positive multiple of 16. |
| `-udon-stack-depth=N` | Entries per caller-save array. Default `1024`. |
| `-udon-heap-slots=N` | Heap slot count. Default: computed from the module. A value below what the module needs is an error, not a truncation. |
| `-udon-runtime-none` | Do not link `runtime/udon_rt.ll`. Bit counting and a non-zero `memset` then fail to resolve. |
| `-udon-manifest=<path>` | Write the used-extern manifest. |
| `-udon-export-labels` | Export every internal label with a `__` prefix, so the lit suite can cross-check the computed addresses. Test only. |
| `-o <path>` | Output `.uasm`. The heap image is written beside it with the `.uasm` replaced by `.udonheap.json`. |

## How types map

| IR | Udon slot type |
|----|----------------|
| `i1` | `SystemBoolean` |
| `i8`, `i16`, `i32`, `ptr` | `SystemInt32` |
| `i64` | `SystemInt64` |
| `float`, `double` | `SystemSingle`, `SystemDouble` |
| `<2/3/4 x float>` | `UnityEngineVector2/3/4` |

`i8` and `i16` live in `%SystemInt32` slots masked to their width. A
`%SystemByte` slot is never produced: `SystemConvert.__ToInt32__SystemByte`
aborts inside the VM.

`i128`, `half`, `fp128` and `x86_fp80` are rejected by name. Rust's `core`
formatting pulls in `i128`, so anything reaching `fmt` fails this way — that is
intended, not worked around.

## Memory

`__mem` is one `%SystemByteArray`, and a pointer is a byte offset into it, so
every LLVM offset is used verbatim and no shift is ever applied. The layout is
stated once:

| Bytes | Region |
|---|---|
| `[0, 16)` | reserved: bitcast scratch, and the reason no object has address 0 (Rust's `None` niche) |
| `[16, -udon-memory-bytes)` | globals image |
| `[-udon-memory-bytes, +-udon-shadow-stack-bytes)` | `alloca` shadow stack, growing down |
| the trailing 8 bytes | padding for sub-word window reads |

The 8 pad bytes are what collapse the alignment problem: a `load i8` at the last
usable byte still reads a full 32-bit window, so there is one code path instead
of an alignment case analysis.

* `load i32` → `SystemBitConverter.__ToInt32__(__mem, p)`. Works at any
  alignment; `align` is ignored, not rejected.
* `load i8`/`i16` → the same window read, then mask, then sign-extend.
* `store i32`/`i64`/`float`/`double` → `__GetBytes__` into a scratch slot, then
  `SystemByteArray.__CopyTo__`. **This allocates one small array per store**;
  the whitelist has no span overload. Four cheaper-looking alternatives were
  measured and all fail, so the cost is not a shortcut:

  | Alternative | Result |
  |---|---|
  | `Convert.__ToByte__SystemInt32` + `SystemByteArray.__Set__` | `Convert` aborts in the VM, even in range |
  | `SystemArray.__SetValue__(object, i32)` | runtime type check rejects the box into a `byte[]` |
  | `Int32Array.__Set__(i, v)` (word array) | works, no allocation, but `BitConverter.To*` rejects an `Int32Array`, so every load would need manual shifts |
  | `BitConverter.__ToInt32` on a word array | `EXCEPTION` — it requires `SystemByteArray` |

  `wasm2usharp` makes the same choice for the same reason
  (`BitConverter.GetBytes($p1).CopyTo(memory, idx)`). Switching to the word
  model is possible — the plan's contingency — but it is a different memory
  model, not a local optimisation: unaligned sub-word access becomes a
  `p >> 2` plus a `(p & 3) * 8` shift with both straddle cases, and the globals
  image would need a byte array anyway for `__CopyTo__`.
* `store i8`/`i16` → one read-modify-write on the window: `(w & 0xFFFFFF00) |
  (v & 0xFF)`. **No allocation**, because it stores a whole window rather than
  a byte.

An `alloca` that survives `mem2reg` lives in the shadow stack: `__sp` is a
`%SystemInt32` starting at the region top, a function's frame is the sum of its
allocas rounded up to 16, and the epilogue adds it back. Every frame has a fixed
size, so no save slot is needed. A dynamic alloca is rejected by name. A frame
that would cross the region bottom halts with `__halt_reason = 2` rather than
letting a `BitConverter` read throw a bare bounds exception.

## Calls

A direct call copies its arguments into the callee's private `<fn>_arg_<i>`
slots, copies its return-label address into `<callee>_ret`, and jumps to the
body. The return label is a block of the caller, so control resumes there.

A function that may be **re-entered** while running needs more: the nested
activation executes the same code and writes the same slots, so the caller
spills what is live across the call onto typed stacks (`__stack_i32`,
`__stack_i64`, `__stack_f32`, `__stack_f64`, `__stack_obj`) and restores after.
Re-entrant means: in a recursive SCC, address-taken, or able to reach an extern
that can synchronously run Udon code. That last set is an **allowlist** — the
safe families are enumerable and everything else is suspect — which covers
`SendCustomEvent`, `SetProgramVariable` (a field-change callback can fire an
event back), `Instantiate` (runs `OnEnable`), `Pickup.Drop` (fires `OnDrop`),
and ownership changes without having to name them.

The graph is built from the IR, never from `norecurse`: hand-written and freshly
emitted IR carry no function attributes, so an attribute-based answer would be
wrong exactly where it matters.

Function pointers are indices into `__fn_table`, never raw code addresses.
Index 0 is the null entry, so a zeroed pointer reaches a halt that names itself.

## Externs

Every operation is an extern call on typed heap slots, because Udon has no
registers and no arithmetic of its own. The table is **generated**, and the
generator exists because the node registry over-reports:

```
SystemInt64.__op_Remainder
```

is a registered node definition whose delegate request throws `not implemented
yet`. Membership is therefore not proof that an extern works, and the table is
built from `GetExternFunctionDelegate` succeeding instead:

```
python scripts/gen-externs.py --check \
    --nodedefs <dump of GetNodeDefinitions()> \
    --unity-managed <Unity>/Editor/Data/Managed/UnityEngine
```

Exit codes: `0` clean, `1` checked and drifted, `2` couldn't check. CI treats
`2` as "check skipped", never as "checked and clean"; the checked-in
`UDONExterns.def` is the source of truth between regenerations.

The probe (`tools/Udon.ExternProbe`) constructs the SDK's own
`VRC.Udon.Wrapper.UdonWrapper` in plain .NET — its modules take only
`IUdonComponentGetter` and `IUdonSecurityFilter`, both pure dispatch
interfaces — so the table can be regenerated without a Unity editor. It
resolves Unity's managed assemblies from `--unity-managed` so the
`UnityEngine*` modules construct too. Currently 1,921 of 1,926 probed
signatures resolve.

`udon-vm verify` is the matching runtime check: it fails the process naming any
signature the program uses that the table lacks, which turns the
registry-over-reports problem into a build failure rather than a surprise.

## Entry points

A function named `udon_event_<name>` or carrying `@"udon.event"="<name>"` is
exported. The name goes through a **table lookup, not a prefix rule**, because
the built-in events are camelCase after the underscore:

`_start`, `_update`, `_lateUpdate`, `_fixedUpdate`, `_postLateUpdate`,
`_onEnable`, `_onDisable`, `_interact`, `_onPickup`, `_onDrop`,
`_onPickupUseDown`, `_onPickupUseUp`, `_onPlayerJoined`, `_onPlayerLeft`,
`_onOwnershipTransferred`, `_onDeserialization`, `_onPreSerialization`

A name in that table gets the `_` prefix; anything else is a custom event and is
exported verbatim. `main` maps to `_start`. Two functions claiming one name is a
diagnostic, and an entry point with parameters is rejected.

Each entry stub runs, in order: a depth check that resets the shadow stack and
the caller-save cursors when `__depth == 0`, `__depth += 1`, the one-shot init
guard, then the body. The exit block zeroes `__halt_reason`, publishes the
result as `__retval_<export>`, decrements `__depth`, and jumps to the `0xFFFFFFFF`
sentinel.

The test-only `__udon_send_custom_event(ptr)` is the only way source code can
re-enter the VM. The name deliberately avoids the `llvm.` prefix — LLVM reserves
it for intrinsics, and an unknown `llvm.*` name can be rejected by the verifier
or mis-handled by passes.

## The harness

```
udon-vm run <program.uasm> [--heap-json <json>] [--entry <name>]...
                           [--stubs <json>] [--trace] [--repeat N]
udon-vm assemble <program.uasm> --heap-slots N
udon-vm arity <signature>
udon-vm verify <program.uasm>
```

`--entry` is repeatable and ordered; all of them run on one `UdonVM` over one
`IUdonProgram`, so heap state carries between them exactly as it does between
events in VRChat. `--repeat N` repeats the whole list.

Output is one line per observable event:

| Form | Meaning |
|---|---|
| `ret <entry> = <value>` | the invocation returned normally |
| `HALT <entry> <reason>` | it halted; `1` caller-save overflow, `2` shadow-stack overflow, `3` null function pointer |
| `LOG: <text>` | one `UnityEngineDebug.__Log__` call, printed in call order |
| `EXCEPTION <entry> <Type>: <msg>` | it threw; the harness then discards the `IUdonProgram` |

Exit codes: `0` when every invocation returned or threw, `1` on a non-zero
`Interpret()` result (a VM error, which VRChat also treats as fatal) or a
harness failure. An exception is data, not a failure, so a fixture can assert it
with an `OUT:` line.

Three measured facts the harness depends on, all confirmed against the SDK's own
dispatch code:

* A normal halt returns **0**; `RunProgram` treats non-zero as fatal.
* **One `UdonVM` serves every event** — no fresh VM, no `_halted` reflection.
* A fresh `UdonVM` over the same `IUdonProgram` **shares the heap**, so
  exception isolation requires a fresh `IUdonProgram`, not merely a fresh VM.

## Tests

```
python -m pip install lit
lit build/test
```

21 fixtures, each asserting exact values with `FileCheck`:

| Fixture | Covers |
|---|---|
| `i32` | arithmetic, shifts, compares, select, phi, diamond |
| `i32-edge` | `udiv 0xFFFFFFFF, 1 → -1`, `urem` wrap, unsigned compare |
| `intrinsics` | `umin`/`umax`/`smin`/`smax`, `abs`, `ctlz`/`cttz`/`ctpop`, `bswap`, `fshl` |
| `memory` | aligned and unaligned `i32`/`i64`/`float`/`double`, `memcpy`, non-zero `memset`, the pad-byte window |
| `subword` | `u8` round trip, `0x80 → -128`, `i16` at an odd offset with intact neighbours |
| `alloca` | local array, `sret` struct return, frames that do not alias |
| `alloca-overflow` | a frame larger than the region → `HALT _start 2` |
| `calls` | two-level chain with an argument and a return value |
| `recursion` | `factorial(5) = 120`, which needs the caller-save discipline |
| `indirect`, `indirect-nested` | calls through a pointer; the transfer-slot regression |
| `i64` | `trunc`, `srem`, both `udiv`/`urem` branches, `lshr` by 0/1/4, `sext i1` |
| `float` | arithmetic, all 14 `fcmp` predicates with NaN, saturating `fptosi`/`fptoui` |
| `bitcast` | `i32↔f32`, `i64↔f64` through the scratch bytes, `-0.0` preserved |
| `vector` | `<3 x float>` add through `UnityEngineVector3` |
| `entry`, `entry-internal` | two entry points, one-shot init, an internal call to an entry point |
| `entry-recover` | `ret`/`HALT 2`/`ret` across three invocations, and the nested shape |
| `exception-isolation` | a throwing event does not poison the next |

The end-to-end check is a real frontend:

```
rustc --target wasm32-unknown-unknown --emit=llvm-ir -O -C panic=abort \
      test/programs/sum.rs -o sum.ll
llvm-udon sum.ll -o sum.uasm
udon-vm run sum.uasm --heap-json sum.udonheap.json --entry _start
```

prints `ret _start = 9` (`3+1+4+1`). `sum.rs` is `#![no_std]` with a raw-pointer
loop, so it emits zero externs and no panic path.

The largest acceptance program is libsm64 -- a Super Mario 64 port written for a
hosted target and never adjusted for this one. It is far past what a fixture can
cover: 58 translation units, 838 KB of IR, structs, unions, function-pointer
tables, variadic calls, a heap, and every intrinsic shape a C frontend emits.

```
scripts/build-libsm64.sh <libsm64-src> test/Inputs/libsm64-driver.c
```

prints `ret _start = 50536`. That is a checksum the driver computes after
`sm64_global_init` has loaded Mario's textures and animations into a 4 MiB
heap buffer, and after `sm64_mario_create` has run the whole spawn path: 505 for
the byte sum of the texture buffer, 36 for the bytes that were actually
written. `-1` there means no collision data was found, which is the correct
answer with no level loaded -- so the driver asserts that value rather than
ticking a Mario that has no floor.

The generated program is 173 MB of UASM over 489 000 slots, and the real
`VRC.Udon.UAssembly` assembler accepts all of it in about 8 seconds
(`udon-vm assemble <uasm> --heap-slots <n>`). It also assembles and runs inside
the Unity editor, through `Assets/Udon/Editor/UdonProgramImporter.cs`, and
returns the same checksum in ClientSim.

The on-screen end-to-end is `test/Inputs/mario-playground.c`: a real game loop
in miniature. It compiles a floor and walls as `SM64Surface` triangles, spawns
Mario, and exports the bridge a Unity host reads and writes:

```
udon_export_ud_input     float[8]      stick + buttons (host -> C)
udon_export_ud_state     float[16]     position, face angle, velocity (C -> host)
udon_export_ud_geo_count uint32        triangles used (C -> host)
udon_export_ud_geo       float[33*1024] per-triangle pos/normal/color/uv (C -> host)
```

Build it with `build/libsm64/playground-build.sh` (the same 58 translation
units plus the driver), then `udon-vm run playground.uasm --entry _start
--entry report` proves input in and state + a 752-triangle mesh out:
`ret report = 2500752` is the triangle count and Mario's position after 30
forward ticks. The script compiles at `-Os` (override with `UDON_OPT`): versus
`-O2` that cuts the emitted program from 174 MB / 3.16M instructions to
143 MB / 2.65M instructions -- a 16% smaller interpreter loop -- with the same
results. `-Oz` does not work: it exposes an unresolved `_stat64i32` from the
CRT surface libsm64's audio loader reaches. In the editor, `Assets/Udon/Runtime/MarioPlayground.cs` drives
the same program: it writes `ud_input`, sends the `tick` event at 30 Hz, reads
`ud_state` and the geometry buffer back out of `__mem`, and renders Mario with
a vertex-color shader -- the scene is
`Assets/Scenes/LlvmUdonPlayground.unity`.

The bridge is `-udon-export-memory` plus the `udon_export_` prefix. The writer
exports `__mem` itself and, for every `udon_export_<name>` global, a
`%SystemInt32` slot holding the global's byte offset; the host reads and writes
through `__mem` at that offset. No ROM is bundled: the texture atlas is left
zeroed and a minimal one-animation table is assembled at the offset the
animation loader reads, so the animated-part geometry stays on real data
instead of indexing `values[-1]`.

The driver also reserves `udon_export_ud_rom` (8 MiB, the size libsm64's
offsets assume) and takes a ROM through it: the Unity host copies the user's
`.z64` into `__mem` at that window before `_start` and the driver passes the
pointer to `sm64_global_init`, so a real ROM lights the textures and animations
up. The ROM itself is never distributed -- the repo ships the window and the
byte-exact copy, not the data.

## Known limitations

* **Objects and user-callable externs are out of scope.** Nothing lets source
  name an Udon extern and there is no object model, so the backend can compute
  but not yet touch the world. The ABI leaves room: `__stack_obj` and the
  per-callee argument slots take an `i32` handle.
* **Stores allocate.** One small array per scalar store, as described above.
* **Colouring is per-function only.** Cross-function slot sharing needs
  interprocedural liveness.
* **A non-zero `memset` doubles with `memcpy`**, so it is O(n) in `Array.Copy`
  calls rather than one fill.
* **`u64 as f32` above 2⁶³ rounds twice** (through `double`), so it may differ
  from LLVM by at most 1 ulp. Accepted rather than special-cased.
* **Client array-size limits are unverified.** ClientSim does not enforce them;
  a Build & Test run is what gates the 1 MiB default. If it refuses the array,
  drop `-udon-memory-bytes` to `1 << 16` and `-udon-shadow-stack-bytes` to
  `1 << 13`.

## Notes

Longer-form notes on the measurements this design rests on live beside the code
they describe; the ones that shaped it most are the UASM instruction sizes, the
extern-arity rule, and the `UdonVM` lifecycle. Each is recorded in the comment at
the point it matters rather than in a separate document, so the code and its
justification cannot drift apart.
