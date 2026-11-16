# The UASM format, as measured

Everything here was measured against VRChat SDK 3.10.6 on Unity 2022.3.22f1 by
assembling a program and reading `program.ByteCode.Length` and
`EntryPoints.GetAddressFromSymbol`. Nothing is quoted from documentation.

## Instruction sizes

| Instruction | Bytes |
|---|---|
| `NOP`, `POP`, `COPY` | 4 |
| `PUSH`, `JUMP`, `JUMP_IF_FALSE`, `JUMP_INDIRECT`, `EXTERN` | 8 |

A `JUMP_IF_FALSE` and a `JUMP_INDIRECT` each additionally need their own
preceding `PUSH` (8), because the target travels in a heap slot.

The writer lays out its own functions from these numbers, so every code address
it emits — a return address, `<fn>_exit`, `__fnaddr_<i>` — is this arithmetic.
A call/return program whose label addresses were computed as `0x1C` and `0x40`
from these sizes matched `GetAddressFromSymbol` exactly and executed correctly.
The lit suite runs with `-udon-export-labels` and asserts every computed address
against the assembler, so the arithmetic is checked for every function rather
than just the entry points.

## What an operand may be

`PUSH` takes a **data symbol only**. A code label is not a valid operand:

```
AssemblyException: Undefined data identifier '_x'. Identifiers must be defined
before use.
```

Data-block initializers likewise reject identifiers. Every constant and every
code address therefore travels through a slot, which is why the writer emits
`__const_*` and `__exitaddr_*` and `__fnaddr_*` slots instead of trying to
inline anything.

`JUMP` and `JUMP_IF_FALSE` accept either a code label or `0x%08X`. `COPY` is
`PUSH <src>; PUSH <dst>; COPY`, copying src → dst.

`JUMP_IF_FALSE` pops one heap address whose heap type **must be
`SystemBoolean`**. An `Int32` condition throws
`Cannot retrieve heap variable of type 'Int32' as type 'Boolean'`, so a
non-`i1` condition goes through `SystemConvert.__ToBoolean__`.

Halt: `JUMP_INDIRECT` through a `%SystemUInt32` slot holding any value
`>= 0xFFFFFFFC` ends `Interpret()` with return code 0. A null/uninitialized slot
throws. `0xFFFFFFFF` is the sentinel UdonSharp itself uses.

Identifier charset is **`[A-Za-z_][A-Za-z0-9_]*`**. `.`, `$`, `-`, a leading
digit and a space all fail with `ParseException`, which is why every LLVM name is
mangled.

## Literals

| Udon type | Literal in `.data_start` |
|---|---|
| `SystemInt32` | yes (`5`, `-7`, `0x00000005`) |
| `SystemUInt32` | yes (`0xFFFFFFFF`) |
| `SystemSingle` | yes (`1.5`) |
| `SystemDouble` | yes (`1.5`) |
| `SystemString` | yes (`"text"`) |
| `SystemInt64`, `SystemBoolean`, `SystemChar`, `SystemInt16`, `SystemByte` | **no** — `AssemblyException: Type 'X' must be initialized to null or a this reference.` |
| any reference/struct type | `null` only (`%UnityEngineVector3, null` → `(0,0,0)`) |
| `%VRCUdonCommonInterfacesIUdonEventReceiver, this` | the behaviour's own reference |

Everything else is written by the host after assembly with
`IUdonHeap.SetHeapVariable(uint, object, Type)`.

### Two literal traps

**`-2147483648` cannot be written in decimal.** The scanner lexes the digits as
a positive `Int32` and only then applies the sign, so the literal throws
`OverflowException: Value was either too large or too small for an Int32`. Hex
has no such problem — `0x80000000` assembles — so the one value decimal cannot
express is written in hex.

**A `null` value-type slot reads as its default.** A `%SystemBoolean, null` slot
reads `false` (verified: `IsHeapVariableInitialized` is true, the value is
`False`), which is why the init guard needs no host-written initial value.

## Extern calls

For a signature `Type.__Method__[P1_P2_]__Ret`:

1. push the instance address (instance methods only),
2. push each argument address in order,
3. push the destination address when `Ret != SystemVoid`,
4. `EXTERN, "<signature>"`.

The address count the VM pops is the **arity** from
`IUdonWrapper.GetExternFunctionParameterCount`, which equals
`params + (ret != void ? 1 : 0)` and comes from node-registry metadata, not from
parsing the string. Measured:

| Signature | Arity |
|---|---|
| `SystemInt32.__op_Addition__…__SystemInt32` | 3 |
| `SystemInt32.__ToString__SystemString` | 2 |
| `UnityEngineDebug.__Log__SystemObject__SystemVoid` | 1 |
| `SystemInt32Array.__ctor__SystemInt32__SystemInt32Array` | 2 |
| `SystemObject.__ctor____SystemObject` | 1 |

Generic modules throw `External module 'SystemCollectionsGenericListT' is not
supported.`, so the recorded number is authoritative and must be captured, never
computed.

Argument typing is **strict**: an `Int32` heap slot pushed into a `UInt32`
parameter throws, and into an `Int64` parameter throws. Only a value-type slot
into a `SystemObject` parameter is accepted (boxing), which is what makes
`UnityEngineDebug.__Log__` usable for `int` (verified, printed `42`).

## The registry over-reports

`GetNodeDefinitions()` returns 35,738 definitions / 35,627 unique `fullName`s,
33,291 of which contain `__`. Membership is **not** proof that the extern works:

`SystemInt64.__op_Remainder` and `SystemUInt64.__op_Remainder` are absent from
the registry, and both `GetExternFunctionParameterCount` and
`GetExternFunctionDelegate` throw `Function '…' is not implemented yet`; an
`EXTERN` to it aborts the VM with that message.

Also confirmed absent: `SystemUInt32.__op_Remainder`,
`SystemInt32.__op_UnsignedRightShift`, `SystemString.__get_Chars`,
`SystemMathF.*`, `System.Numerics.*`.

The rule this implies, and the reason `scripts/gen-externs.py` exists: **the
table is generated from `GetExternFunctionDelegate` succeeding, not from
registry membership**, and `udon-vm verify` asserts that every signature the
writer emits resolves.
