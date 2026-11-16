# The UdonVM lifecycle, as measured

These are the facts the harness and the entry/exit design rest on. All were
measured this way: build a program, assemble it, then drive `UdonVM` directly
and read `Interpret()`'s return, `GetProgramCounter()`, and the heap.

`UdonBehaviour.RunProgram(uint)` is the reference implementation
(`Runtime/Udon/UdonBehaviour.cs`, ~1136-1196); the harness replicates it rather
than inventing its own calling discipline.

## A normal halt returns 0

`Interpret()` exits its loop with `0` once the program counter runs past the
bytecode, and a `JUMP_INDIRECT` to any address past the end does exactly that —
which is why the `0xFFFFFFFF` sentinel halts. Measured: `Interpret -> 0`,
`_halted == false`, pc left at `0xFFFFFFFF`.

`RunProgram` treats a non-zero result as a VM error and disables the behaviour,
so **`0` is the only acceptable outcome**.

**Only the halt sentinel is a high address.** `RunProgram`'s
`if (originalAddress < 0xFFFFFFFC) SetProgramCounter(originalAddress)` rule
applies to the *saved program counter* — the value observed on entry to an
event — not to jump targets. Ordinary code addresses (return addresses,
`<fn>_exit`, `__fnaddr_<i>`) are real bytecode offsets far below `0xFFFFFFFC`
and must stay that way: they are `JUMP_INDIRECT` targets that have to land
inside the bytecode.

## `_halted` is not set by a halt

The IL shows it is set only on error paths (stack underflow, an invalid extern
operand, a thrown exception), and at the top of `Interpret` it short-circuits to
`1`. A `PUSH`/`COPY` mismatch that leaves the stack short therefore produces
`result=1` and a sticky `_halted` — that is what a first, malformed probe
measured. Correct bytecode never sets it.

## One `UdonVM` serves every event

`RunProgram` saves `GetProgramCounter()`, sets the entry point, calls
`Interpret()`, and in `finally` restores the pc **only if
`originalAddress < 0xFFFFFFFC`**.

Measured on one instance: four sequential events each returned `0` with
`_halted == false` and a shared heap counter reaching `4`; a nested event from
an extern also worked with no `_halted` clearing at all. So the harness needs
**no** fresh VM per invocation, **no** `_halted` reflection, and **no**
workaround.

## Nested `Interpret()` needs the pc save/restore

The pc lives in a single `_programCounter` field, so a nested call that omits
the restore leaves the outer invocation resuming at the inner call's final pc.
Measured: without restore the outer invocation never ran its remaining
instructions; with

```
savedPc = GetProgramCounter();
SetProgramCounter(target);
Interpret();
if (savedPc < 0xFFFFFFFC) SetProgramCounter(savedPc);
```

the nested case ran correctly (log order `C-ran`, `A-after-B`, `C-ran`, `A-end`).

## A null `JUMP_INDIRECT` target falls through

The IL advances the pc by 8 when the target slot holds null. Measured: it throws
`An exception occurred in an UdonVM` only because the fall-through landed
outside the bytecode. A `%SystemUInt32` slot left `null` is therefore *not* a
safe halt; the `0xFFFFFFFF` literal is.

## A fresh VM shares the heap

A fresh `UdonVM` over the same `IUdonProgram` **shares the heap**, so heap state
(`__depth`, `__sp`, `__mem`) survives a new instance. Isolation after an
exception therefore requires a fresh **`IUdonProgram`** — re-assemble and
re-apply the heap image — not merely a fresh VM.

That is why the harness's exception path discards the program, and why
`test/exception-isolation.ll` discriminates: `_throws` abandons 12288 bytes of a
16 KiB region and then raises, so `_start`'s 8 KiB frame only fits if the heap
was reset.

## `%<Type>, this` assembles

`__this: %VRCUdonCommonInterfacesIUdonEventReceiver, this` in `.data_start`
assembles into a live `VRC.Udon.Common.UdonGameObjectComponentHeapReference`,
which is what UdonSharp emits for `this` (`Value.GetDeclarationStr`). Verified by
assembling it and pushing it into a two-argument extern.

## Why the entry exit block zeroes `__halt_reason`

`Interpret()` returns 0 for both a halt and a normal return, so the return code
cannot tell them apart, and the entry's return slot would hold a stale value
after a halt.

The harness brackets every invocation with `__halt_reason`: it writes 0 before
the event and reads it after, printing `HALT <entry> <reason>` instead of the
`ret` line when it is non-zero. The exit block zeroes it on a normal return so a
later event cannot inherit a previous halt's reason.

That zeroing is what makes the reason attributable under nesting. Measured: `A`
re-enters `B`, `B` halts with reason `2`, `A` then returns normally, and the
outer event's `__halt_reason` is still `2` — so without the zero, `A` would be
misreported as `HALT _A 2` despite having completed. Zeroing cannot mask a real
halt, because a halted activation never reaches its exit block.

## Depth accounting

`__depth` counts live entry activations. Every entry stub tests `__depth == 0`
**before** incrementing — incrementing first would make a fresh event read `1`,
the test false, and the reset would never run — and both the exit block and the
halt block decrement by one.

Decrementing by one, **not zeroing**, is what makes a leaked frame bounded
without breaking nesting. A halt ends exactly one `Interpret()`, which is
exactly one entry activation. Consider event A (depth 1) re-entering B (depth
2), where B halts: the halt takes depth to 1, A resumes correctly, and A's frame
stays live. If `__halt` zeroed the depth, A would continue with `__depth == 0`,
so a later re-entry C would see `== 0`, reset `__sp` to the region top
*underneath A's live frame*, allocate over it, and A's pops would return
garbage.

`__halt` also deliberately does **not** reset `__sp` or the `__sptr_*` cursors:
a halt abandons the current activation without running its epilogue, so that
frame leaks, and resetting `__sp` would put the pointer above a resuming nested
caller's live frame.

Verified end to end: `PUSH __exitaddr; PUSH _ret; COPY; JUMP_INDIRECT _ret`,
then `_exit` decrementing `__depth` and halting — three events on one `UdonVM`
through the `RunProgram` sequence each returned `0` with `__depth` back to `0`.

## The `Assemble(uasm, N)` off-by-slack

`UdonEditorManager.Instance.Assemble(uasm)` caps the heap at 512 slots:

```
Address: '512' is larger than the UdonVM's heap size of '512'
```

`UdonSharp.Compiler.Udon.CompilerUdonInterface.Assemble(string, uint)` takes an
explicit size (internal, reached by reflection; it lives in `UdonSharp.Editor`,
not in an assembly called `VRC.Udon.Compiler`).

`N` is **not** an exact minimum. For a program with 264 symbols, `Assemble(uasm,
264)` fails with `Address: '264' is larger than the UdonVM's heap size of
'264'`, and 265 through 291 fail the same way; 292 is the first capacity that
succeeds. The assembler assigns addresses past the symbol count while resolving
exports, so the call needs headroom. The importer therefore passes
`N + N/4 + 64`.
