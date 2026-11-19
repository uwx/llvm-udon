//===-- Externs.cs - signature registry and stub implementations ----------===//
//
// Arity is *declared* here, never parsed from the signature string: the VM pops
// that many addresses, and the SDK's own number comes from node-registry
// metadata. The declarations in test/externs.json are generated from the live
// registry (scripts/gen-externs.py); the table below is the hand-written
// fallback for the families the writer emits, so the harness works before the
// generator has run.
//
// A signature that is missing from both is a hard error naming it. That is the
// check that catches the registry-over-reports problem: SystemInt64's
// __op_Remainder resolves as a node definition and throws "not implemented yet"
// when a delegate is actually requested, so a lowering that reaches for one
// must fail loudly here rather than misbehave at runtime.
//
//===----------------------------------------------------------------------===//

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using VRC.Udon.Common.Delegates;
using VRC.Udon.Common.Interfaces;

namespace Udon.VM
{
    /// <summary>One resolvable extern: how many addresses it pops and what it does.</summary>
    public sealed class ExternEntry
    {
        public string Signature;
        public int Arity;
        public UdonExternDelegate Delegate;
    }

    /// <summary>
    /// The harness's extern table. Built from the hand-written families below,
    /// then overlaid with `test/externs.json` when it is present so the arities
    /// match what the live registry reported.
    /// </summary>
    public sealed class ExternRegistry
    {
        private readonly Dictionary<string, ExternEntry> _entries =
            new Dictionary<string, ExternEntry>(StringComparer.Ordinal);

        /// <summary>Signatures the writer emitted, so `verify` can report the missing ones.</summary>
        public IEnumerable<string> Signatures => _entries.Keys;

        public bool TryGet(string signature, out ExternEntry entry) =>
            _entries.TryGetValue(signature, out entry);

        public void Add(string signature, int arity, UdonExternDelegate fn)
        {
            _entries[signature] = new ExternEntry { Signature = signature, Arity = arity, Delegate = fn };
        }

        /// <summary>
        /// Every extern the backend's lowering can emit, with the arity measured
        /// from the live registry and the stub body that implements it.
        /// </summary>
        public static ExternRegistry CreateDefault(TextWriter log)
        {
            var r = new ExternRegistry();
            Stubs.RegisterAll(r, log);
            return r;
        }

        /// <summary>
        /// Overlay arities from `externs.json`. The JSON is authoritative where
        /// it disagrees, because it was measured; a signature present there but
        /// unimplemented is deliberately *not* registered, so reaching for it
        /// fails.
        /// </summary>
        public void LoadArityFile(string path)
        {
            if (!File.Exists(path)) return;
            string text = File.ReadAllText(path);
            var reader = new Udon.Heap.JsonReader(text);
            reader.Expect('{');
            bool first = true;
            while (!reader.TryConsume('}'))
            {
                if (!first) reader.Expect(',');
                first = false;
                string signature = reader.ReadString();
                reader.Expect(':');
                reader.Expect('{');
                int arity = -1;
                bool implemented = true;
                bool sawFields = false;
                while (!reader.TryConsume('}'))
                {
                    if (sawFields) reader.Expect(',');
                    sawFields = true;
                    string key = reader.ReadString();
                    reader.Expect(':');
                    switch (key)
                    {
                        case "arity": arity = (int)reader.ReadNumber(); break;
                        case "implemented": implemented = reader.ReadBool(); break;
                        default: reader.SkipValue(); break;
                    }
                }
                if (!implemented) _entries.Remove(signature);
                else if (arity >= 0 && _entries.TryGetValue(signature, out ExternEntry e)) e.Arity = arity;
            }
        }
    }

    /// <summary>
    /// Delegate bodies for the extern families the writer emits. Each reads and
    /// writes <see cref="IUdonHeap"/> at the addresses in the popped span, in
    /// the order the SDK wrapper does: instance first (if any), then arguments,
    /// then the destination.
    /// </summary>
    public static class Stubs
    {
        /// <summary>Hook for `UnityEngineDebug.__Log__SystemObject__SystemVoid`.</summary>
        public static Action<string> LogSink = _ => { };

        /// <summary>
        /// Hook for `VRCUdonCommonInterfacesIUdonEventReceiver.__SendCustomEvent__SystemString__SystemVoid`.
        /// The harness sets this to its RunProgram-replicating re-entry; the
        /// instance address is the first popped slot.
        /// </summary>
        public static Action<string> SendCustomEvent = name =>
            throw new InvalidOperationException(
                "SendCustomEvent '" + name + "' reached the stub with no harness bound");

        public static void RegisterAll(ExternRegistry r, TextWriter log)
        {
            RegisterInt32(r);
            RegisterInt64(r);
            RegisterUInt32(r);
            RegisterSingle(r);
            RegisterDouble(r);
            RegisterBoolean(r);
            RegisterConvert(r);
            RegisterBitConverter(r);
            RegisterMath(r);
            RegisterArrays(r);
            RegisterString(r);
            RegisterVector(r);
            RegisterDebug(r);
            RegisterEventReceiver(r);
        }

        // --- helpers ---------------------------------------------------------

        private static int I32(IUdonHeap h, uint a) => h.GetHeapVariable<int>(a);
        private static void SetI32(IUdonHeap h, uint a, int v) => h.SetHeapVariable(a, v, typeof(int));
        private static long I64(IUdonHeap h, uint a) => h.GetHeapVariable<long>(a);
        private static void SetI64(IUdonHeap h, uint a, long v) => h.SetHeapVariable(a, v, typeof(long));
        private static uint U32(IUdonHeap h, uint a) => h.GetHeapVariable<uint>(a);
        private static void SetU32(IUdonHeap h, uint a, uint v) => h.SetHeapVariable(a, v, typeof(uint));
        private static float F32(IUdonHeap h, uint a) => h.GetHeapVariable<float>(a);
        private static void SetF32(IUdonHeap h, uint a, float v) => h.SetHeapVariable(a, v, typeof(float));
        private static double F64(IUdonHeap h, uint a) => h.GetHeapVariable<double>(a);
        private static void SetF64(IUdonHeap h, uint a, double v) => h.SetHeapVariable(a, v, typeof(double));
        private static bool B(IUdonHeap h, uint a) => h.GetHeapVariable<bool>(a);
        private static void SetB(IUdonHeap h, uint a, bool v) => h.SetHeapVariable(a, v, typeof(bool));

        private static void Bin32(ExternRegistry r, string name, Func<int, int, int> op) =>
            r.Add("SystemInt32.__op_" + name + "__SystemInt32_SystemInt32__SystemInt32", 3,
                (IUdonHeap h, Span<uint> s) => SetI32(h, s[2], op(I32(h, s[0]), I32(h, s[1]))));

        private static void Cmp32(ExternRegistry r, string name, Func<int, int, bool> op) =>
            r.Add("SystemInt32.__op_" + name + "__SystemInt32_SystemInt32__SystemBoolean", 3,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[2], op(I32(h, s[0]), I32(h, s[1]))));

        private static void Bin64(ExternRegistry r, string name, Func<long, long, long> op) =>
            r.Add("SystemInt64.__op_" + name + "__SystemInt64_SystemInt64__SystemInt64", 3,
                (IUdonHeap h, Span<uint> s) => SetI64(h, s[2], op(I64(h, s[0]), I64(h, s[1]))));

        private static void Cmp64(ExternRegistry r, string name, Func<long, long, bool> op) =>
            r.Add("SystemInt64.__op_" + name + "__SystemInt64_SystemInt64__SystemBoolean", 3,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[2], op(I64(h, s[0]), I64(h, s[1]))));

        private static void BinU32(ExternRegistry r, string name, Func<uint, uint, uint> op) =>
            r.Add("SystemUInt32.__op_" + name + "__SystemUInt32_SystemUInt32__SystemUInt32", 3,
                (IUdonHeap h, Span<uint> s) => SetU32(h, s[2], op(U32(h, s[0]), U32(h, s[1]))));

        private static void CmpU32(ExternRegistry r, string name, Func<uint, uint, bool> op) =>
            r.Add("SystemUInt32.__op_" + name + "__SystemUInt32_SystemUInt32__SystemBoolean", 3,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[2], op(U32(h, s[0]), U32(h, s[1]))));

        private static void BinF32(ExternRegistry r, string name, Func<float, float, float> op) =>
            r.Add("SystemSingle.__op_" + name + "__SystemSingle_SystemSingle__SystemSingle", 3,
                (IUdonHeap h, Span<uint> s) => SetF32(h, s[2], op(F32(h, s[0]), F32(h, s[1]))));

        private static void CmpF32(ExternRegistry r, string name, Func<float, float, bool> op) =>
            r.Add("SystemSingle.__op_" + name + "__SystemSingle_SystemSingle__SystemBoolean", 3,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[2], op(F32(h, s[0]), F32(h, s[1]))));

        private static void BinF64(ExternRegistry r, string name, Func<double, double, double> op) =>
            r.Add("SystemDouble.__op_" + name + "__SystemDouble_SystemDouble__SystemDouble", 3,
                (IUdonHeap h, Span<uint> s) => SetF64(h, s[2], op(F64(h, s[0]), F64(h, s[1]))));

        private static void CmpF64(ExternRegistry r, string name, Func<double, double, bool> op) =>
            r.Add("SystemDouble.__op_" + name + "__SystemDouble_SystemDouble__SystemBoolean", 3,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[2], op(F64(h, s[0]), F64(h, s[1]))));

        // --- families --------------------------------------------------------

        private static void RegisterInt32(ExternRegistry r)
        {
            Bin32(r, "Addition", (a, b) => unchecked(a + b));
            Bin32(r, "Subtraction", (a, b) => unchecked(a - b));
            Bin32(r, "Multiplication", (a, b) => unchecked(a * b));
            Bin32(r, "Division", (a, b) => a / b);
            Bin32(r, "Remainder", (a, b) => a % b);
            Bin32(r, "LeftShift", (a, b) => a << b);
            Bin32(r, "RightShift", (a, b) => a >> b);
            Bin32(r, "LogicalAnd", (a, b) => a & b);
            Bin32(r, "LogicalOr", (a, b) => a | b);
            Bin32(r, "LogicalXor", (a, b) => a ^ b);
            Bin32(r, "ExclusiveOr", (a, b) => a ^ b);
            Cmp32(r, "Equality", (a, b) => a == b);
            Cmp32(r, "Inequality", (a, b) => a != b);
            Cmp32(r, "LessThan", (a, b) => a < b);
            Cmp32(r, "LessThanOrEqual", (a, b) => a <= b);
            Cmp32(r, "GreaterThan", (a, b) => a > b);
            Cmp32(r, "GreaterThanOrEqual", (a, b) => a >= b);
            r.Add("SystemInt32.__op_UnaryNegation__SystemInt32__SystemInt32", 2,
                (IUdonHeap h, Span<uint> s) => SetI32(h, s[1], unchecked(-I32(h, s[0]))));
            r.Add("SystemInt32.__op_UnaryPlus__SystemInt32__SystemInt32", 2,
                (IUdonHeap h, Span<uint> s) => SetI32(h, s[1], I32(h, s[0])));
            r.Add("SystemInt32.__op_LogicalNot__SystemInt32__SystemInt32", 2,
                (IUdonHeap h, Span<uint> s) => SetI32(h, s[1], ~I32(h, s[0])));
            r.Add("SystemInt32.__op_OnesComplement__SystemInt32__SystemInt32", 2,
                (IUdonHeap h, Span<uint> s) => SetI32(h, s[1], ~I32(h, s[0])));
            r.Add("SystemInt32.__ToString__SystemString", 2,
                (IUdonHeap h, Span<uint> s) =>
                    h.SetHeapVariable(s[1], I32(h, s[0]).ToString(CultureInfo.InvariantCulture), typeof(string)));
            // No __op_Remainder on Int64, no unsigned right shift on Int32: those
            // are absent from the live registry and must fail here too.
        }

        private static void RegisterInt64(ExternRegistry r)
        {
            Bin64(r, "Addition", (a, b) => unchecked(a + b));
            Bin64(r, "Subtraction", (a, b) => unchecked(a - b));
            Bin64(r, "Multiplication", (a, b) => unchecked(a * b));
            Bin64(r, "Division", (a, b) => a / b);
            Bin64(r, "LeftShift", (a, b) => a << (int)b);
            Bin64(r, "RightShift", (a, b) => a >> (int)b);
            Bin64(r, "LogicalAnd", (a, b) => a & b);
            Bin64(r, "LogicalOr", (a, b) => a | b);
            Bin64(r, "LogicalXor", (a, b) => a ^ b);
            Bin64(r, "ExclusiveOr", (a, b) => a ^ b);
            Cmp64(r, "Equality", (a, b) => a == b);
            Cmp64(r, "Inequality", (a, b) => a != b);
            Cmp64(r, "LessThan", (a, b) => a < b);
            Cmp64(r, "LessThanOrEqual", (a, b) => a <= b);
            Cmp64(r, "GreaterThan", (a, b) => a > b);
            Cmp64(r, "GreaterThanOrEqual", (a, b) => a >= b);
            r.Add("SystemInt64.__op_UnaryNegation__SystemInt64__SystemInt64", 2,
                (IUdonHeap h, Span<uint> s) => SetI64(h, s[1], unchecked(-I64(h, s[0]))));
            r.Add("SystemInt64.__op_UnaryPlus__SystemInt64__SystemInt64", 2,
                (IUdonHeap h, Span<uint> s) => SetI64(h, s[1], I64(h, s[0])));
            r.Add("SystemInt64.__op_LogicalNot__SystemInt64__SystemInt64", 2,
                (IUdonHeap h, Span<uint> s) => SetI64(h, s[1], ~I64(h, s[0])));
            r.Add("SystemInt64.__op_OnesComplement__SystemInt64__SystemInt64", 2,
                (IUdonHeap h, Span<uint> s) => SetI64(h, s[1], ~I64(h, s[0])));
        }

        private static void RegisterUInt32(ExternRegistry r)
        {
            BinU32(r, "Addition", (a, b) => unchecked(a + b));
            BinU32(r, "Subtraction", (a, b) => unchecked(a - b));
            BinU32(r, "Multiplication", (a, b) => unchecked(a * b));
            BinU32(r, "Division", (a, b) => a / b);
            BinU32(r, "LeftShift", (a, b) => a << (int)b);
            BinU32(r, "RightShift", (a, b) => a >> (int)b);
            BinU32(r, "LogicalAnd", (a, b) => a & b);
            BinU32(r, "LogicalOr", (a, b) => a | b);
            BinU32(r, "LogicalXor", (a, b) => a ^ b);
            CmpU32(r, "Equality", (a, b) => a == b);
            CmpU32(r, "Inequality", (a, b) => a != b);
            CmpU32(r, "LessThan", (a, b) => a < b);
            CmpU32(r, "LessThanOrEqual", (a, b) => a <= b);
            CmpU32(r, "GreaterThan", (a, b) => a > b);
            CmpU32(r, "GreaterThanOrEqual", (a, b) => a >= b);
            // __op_Remainder is deliberately absent: the registry has no
            // implementation for it and reaching one must fail.
        }

        private static void RegisterSingle(ExternRegistry r)
        {
            BinF32(r, "Addition", (a, b) => a + b);
            BinF32(r, "Subtraction", (a, b) => a - b);
            BinF32(r, "Multiplication", (a, b) => a * b);
            BinF32(r, "Division", (a, b) => a / b);
            BinF32(r, "Remainder", (a, b) => a % b);
            CmpF32(r, "Equality", (a, b) => a == b);
            CmpF32(r, "Inequality", (a, b) => a != b);
            CmpF32(r, "LessThan", (a, b) => a < b);
            CmpF32(r, "LessThanOrEqual", (a, b) => a <= b);
            CmpF32(r, "GreaterThan", (a, b) => a > b);
            CmpF32(r, "GreaterThanOrEqual", (a, b) => a >= b);
            r.Add("SystemSingle.__op_UnaryNegation__SystemSingle__SystemSingle", 2,
                (IUdonHeap h, Span<uint> s) => SetF32(h, s[1], -F32(h, s[0])));
            r.Add("SystemSingle.__op_UnaryPlus__SystemSingle__SystemSingle", 2,
                (IUdonHeap h, Span<uint> s) => SetF32(h, s[1], F32(h, s[0])));
            r.Add("SystemSingle.__IsNaN__SystemSingle__SystemBoolean", 2,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[1], float.IsNaN(F32(h, s[0]))));
            r.Add("SystemSingle.__IsInfinity__SystemSingle__SystemBoolean", 2,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[1], float.IsInfinity(F32(h, s[0]))));
        }

        private static void RegisterDouble(ExternRegistry r)
        {
            BinF64(r, "Addition", (a, b) => a + b);
            BinF64(r, "Subtraction", (a, b) => a - b);
            BinF64(r, "Multiplication", (a, b) => a * b);
            BinF64(r, "Division", (a, b) => a / b);
            BinF64(r, "Remainder", (a, b) => a % b);
            CmpF64(r, "Equality", (a, b) => a == b);
            CmpF64(r, "Inequality", (a, b) => a != b);
            CmpF64(r, "LessThan", (a, b) => a < b);
            CmpF64(r, "LessThanOrEqual", (a, b) => a <= b);
            CmpF64(r, "GreaterThan", (a, b) => a > b);
            CmpF64(r, "GreaterThanOrEqual", (a, b) => a >= b);
            r.Add("SystemDouble.__op_UnaryNegation__SystemDouble__SystemDouble", 2,
                (IUdonHeap h, Span<uint> s) => SetF64(h, s[1], -F64(h, s[0])));
            r.Add("SystemDouble.__op_UnaryPlus__SystemDouble__SystemDouble", 2,
                (IUdonHeap h, Span<uint> s) => SetF64(h, s[1], F64(h, s[0])));
            r.Add("SystemDouble.__IsNaN__SystemDouble__SystemBoolean", 2,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[1], double.IsNaN(F64(h, s[0]))));
            r.Add("SystemDouble.__IsInfinity__SystemDouble__SystemBoolean", 2,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[1], double.IsInfinity(F64(h, s[0]))));
        }

        private static void RegisterBoolean(ExternRegistry r)
        {
            r.Add("SystemBoolean.__op_Equality__SystemBoolean_SystemBoolean__SystemBoolean", 3,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[2], B(h, s[0]) == B(h, s[1])));
            r.Add("SystemBoolean.__op_Inequality__SystemBoolean_SystemBoolean__SystemBoolean", 3,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[2], B(h, s[0]) != B(h, s[1])));
            r.Add("SystemBoolean.__op_LogicalAnd__SystemBoolean_SystemBoolean__SystemBoolean", 3,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[2], B(h, s[0]) & B(h, s[1])));
            r.Add("SystemBoolean.__op_LogicalOr__SystemBoolean_SystemBoolean__SystemBoolean", 3,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[2], B(h, s[0]) | B(h, s[1])));
            r.Add("SystemBoolean.__op_LogicalXor__SystemBoolean_SystemBoolean__SystemBoolean", 3,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[2], B(h, s[0]) ^ B(h, s[1])));
            r.Add("SystemBoolean.__op_UnaryNegation__SystemBoolean__SystemBoolean", 2,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[1], !B(h, s[0])));
            r.Add("SystemBoolean.__op_LogicalNot__SystemBoolean__SystemBoolean", 2,
                (IUdonHeap h, Span<uint> s) => SetB(h, s[1], !B(h, s[0])));
            r.Add("SystemBoolean.__ToString__SystemString", 2,
                (IUdonHeap h, Span<uint> s) =>
                    h.SetHeapVariable(s[1], B(h, s[0]) ? "True" : "False", typeof(string)));
        }

        private static void RegisterConvert(ExternRegistry r)
        {
            // Every source type the writer can convert from. Convert.ToInt32 on a
            // byte aborts inside the VM, so no lowering may produce a byte slot;
            // the stub still implements it because the assembler may see one.
            void From(string src, Func<IUdonHeap, uint, object> read)
            {
                r.Add("SystemConvert.__ToInt32__" + src + "__SystemInt32", 2,
                    (IUdonHeap h, Span<uint> s) => SetI32(h, s[1], Convert.ToInt32(read(h, s[0]))));
                r.Add("SystemConvert.__ToInt64__" + src + "__SystemInt64", 2,
                    (IUdonHeap h, Span<uint> s) => SetI64(h, s[1], Convert.ToInt64(read(h, s[0]))));
                r.Add("SystemConvert.__ToSingle__" + src + "__SystemSingle", 2,
                    (IUdonHeap h, Span<uint> s) => SetF32(h, s[1], Convert.ToSingle(read(h, s[0]))));
                r.Add("SystemConvert.__ToDouble__" + src + "__SystemDouble", 2,
                    (IUdonHeap h, Span<uint> s) => SetF64(h, s[1], Convert.ToDouble(read(h, s[0]))));
                r.Add("SystemConvert.__ToBoolean__" + src + "__SystemBoolean", 2,
                    (IUdonHeap h, Span<uint> s) => SetB(h, s[1], Convert.ToBoolean(read(h, s[0]))));
                r.Add("SystemConvert.__ToUInt32__" + src + "__SystemUInt32", 2,
                    (IUdonHeap h, Span<uint> s) => SetU32(h, s[1], Convert.ToUInt32(read(h, s[0]))));
                r.Add("SystemConvert.__ToUInt64__" + src + "__SystemUInt64", 2,
                    (IUdonHeap h, Span<uint> s) =>
                        h.SetHeapVariable(s[1], Convert.ToUInt64(read(h, s[0])), typeof(ulong)));
            }

            From("SystemInt32", (h, a) => I32(h, a));
            From("SystemUInt32", (h, a) => U32(h, a));
            From("SystemInt64", (h, a) => I64(h, a));
            From("SystemUInt64", (h, a) => h.GetHeapVariable<ulong>(a));
            From("SystemSingle", (h, a) => F32(h, a));
            From("SystemDouble", (h, a) => F64(h, a));
            From("SystemBoolean", (h, a) => B(h, a));
            From("SystemChar", (h, a) => h.GetHeapVariable<char>(a));
            From("SystemInt16", (h, a) => h.GetHeapVariable<short>(a));
            From("SystemUInt16", (h, a) => h.GetHeapVariable<ushort>(a));
            From("SystemByte", (h, a) => h.GetHeapVariable<byte>(a));
            From("SystemSByte", (h, a) => h.GetHeapVariable<sbyte>(a));
        }

        private static void RegisterBitConverter(ExternRegistry r)
        {
            // The byte-array reads the whole memory model depends on. These work
            // at any offset, including past the last word, which is what the
            // 8 pad bytes at the end of __mem are for.
            r.Add("SystemBitConverter.__ToInt32__SystemByteArray_SystemInt32__SystemInt32", 3,
                (IUdonHeap h, Span<uint> s) =>
                {
                    byte[] mem = h.GetHeapVariable<byte[]>(s[0]);
                    int off = I32(h, s[1]);
                    SetI32(h, s[2], BitConverter.ToInt32(mem, off));
                });
            r.Add("SystemBitConverter.__ToInt64__SystemByteArray_SystemInt32__SystemInt64", 3,
                (IUdonHeap h, Span<uint> s) =>
                {
                    byte[] mem = h.GetHeapVariable<byte[]>(s[0]);
                    SetI64(h, s[2], BitConverter.ToInt64(mem, I32(h, s[1])));
                });
            r.Add("SystemBitConverter.__ToSingle__SystemByteArray_SystemInt32__SystemSingle", 3,
                (IUdonHeap h, Span<uint> s) =>
                {
                    byte[] mem = h.GetHeapVariable<byte[]>(s[0]);
                    SetF32(h, s[2], BitConverter.ToSingle(mem, I32(h, s[1])));
                });
            r.Add("SystemBitConverter.__ToDouble__SystemByteArray_SystemInt32__SystemDouble", 3,
                (IUdonHeap h, Span<uint> s) =>
                {
                    byte[] mem = h.GetHeapVariable<byte[]>(s[0]);
                    SetF64(h, s[2], BitConverter.ToDouble(mem, I32(h, s[1])));
                });
            r.Add("SystemBitConverter.__ToUInt32__SystemByteArray_SystemInt32__SystemUInt32", 3,
                (IUdonHeap h, Span<uint> s) =>
                {
                    byte[] mem = h.GetHeapVariable<byte[]>(s[0]);
                    SetU32(h, s[2], BitConverter.ToUInt32(mem, I32(h, s[1])));
                });
            r.Add("SystemBitConverter.__ToUInt64__SystemByteArray_SystemInt32__SystemUInt64", 3,
                (IUdonHeap h, Span<uint> s) =>
                {
                    byte[] mem = h.GetHeapVariable<byte[]>(s[0]);
                    h.SetHeapVariable(s[2], BitConverter.ToUInt64(mem, I32(h, s[1])), typeof(ulong));
                });

            // The write side: a fresh array into the destination slot, which
            // SystemByteArray.__CopyTo__ then copies into __mem. One allocation
            // per store; the whitelist has no span overload.
            void Bytes(string src, Func<IUdonHeap, uint, byte[]> toBytes)
            {
                r.Add("SystemBitConverter.__GetBytes__" + src + "__SystemByteArray", 2,
                    (IUdonHeap h, Span<uint> s) =>
                        h.SetHeapVariable(s[1], toBytes(h, s[0]), typeof(byte[])));
            }

            Bytes("SystemInt32", (h, a) => BitConverter.GetBytes(I32(h, a)));
            Bytes("SystemUInt32", (h, a) => BitConverter.GetBytes(U32(h, a)));
            Bytes("SystemInt64", (h, a) => BitConverter.GetBytes(I64(h, a)));
            Bytes("SystemUInt64", (h, a) => BitConverter.GetBytes(h.GetHeapVariable<ulong>(a)));
            Bytes("SystemSingle", (h, a) => BitConverter.GetBytes(F32(h, a)));
            Bytes("SystemDouble", (h, a) => BitConverter.GetBytes(F64(h, a)));

            // Reinterpret casts. The writer uses the __mem scratch round-trip
            // instead because these were never exercised; they are here so a
            // program that does name one still runs.
            r.Add("SystemBitConverter.__Int32BitsToSingle__SystemInt32__SystemSingle", 2,
                (IUdonHeap h, Span<uint> s) => SetF32(h, s[1], BitConverter.Int32BitsToSingle(I32(h, s[0]))));
            r.Add("SystemBitConverter.__SingleToInt32Bits__SystemSingle__SystemInt32", 2,
                (IUdonHeap h, Span<uint> s) => SetI32(h, s[1], BitConverter.SingleToInt32Bits(F32(h, s[0]))));
            r.Add("SystemBitConverter.__Int64BitsToDouble__SystemInt64__SystemDouble", 2,
                (IUdonHeap h, Span<uint> s) => SetF64(h, s[1], BitConverter.Int64BitsToDouble(I64(h, s[0]))));
            r.Add("SystemBitConverter.__DoubleToInt64Bits__SystemDouble__SystemInt64", 2,
                (IUdonHeap h, Span<uint> s) => SetI64(h, s[1], BitConverter.DoubleToInt64Bits(F64(h, s[0]))));
        }

        private static void RegisterMath(ExternRegistry r)
        {
            void M64(string name, Func<double, double> op) =>
                r.Add("SystemMath.__" + name + "__SystemDouble__SystemDouble", 2,
                    (IUdonHeap h, Span<uint> s) => SetF64(h, s[1], op(F64(h, s[0]))));

            void M64x2(string name, Func<double, double, double> op) =>
                r.Add("SystemMath.__" + name + "__SystemDouble_SystemDouble__SystemDouble", 3,
                    (IUdonHeap h, Span<uint> s) => SetF64(h, s[2], op(F64(h, s[0]), F64(h, s[1]))));

            M64("Truncate", Math.Truncate);
            M64("Floor", Math.Floor);
            M64("Ceiling", Math.Ceiling);
            M64("Sqrt", Math.Sqrt);
            M64("Abs", Math.Abs);
            M64("Sin", Math.Sin);
            M64("Cos", Math.Cos);
            M64("Tan", Math.Tan);
            M64("Round", Math.Round);
            M64x2("Min", Math.Min);
            M64x2("Max", Math.Max);
            M64x2("Pow", Math.Pow);
            M64x2("Atan2", Math.Atan2);

            r.Add("SystemMath.__DivRem__SystemInt64_SystemInt64_SystemInt64Ref__SystemInt64", 4,
                (IUdonHeap h, Span<uint> s) =>
                {
                    long q = Math.DivRem(I64(h, s[0]), I64(h, s[1]), out long rem);
                    // arity 4: instance-less, two operands, a ref destination and
                    // the return destination.
                    SetI64(h, s[2], rem);
                    SetI64(h, s[3], q);
                });
        }

        private static void RegisterArrays(ExternRegistry r)
        {
            void Array1<T>(string udon, string clr)
            {
                r.Add("System" + udon + "Array.__ctor__SystemInt32__System" + udon + "Array", 2,
                    (IUdonHeap h, Span<uint> s) =>
                        h.SetHeapVariable(s[1], new T[I32(h, s[0])], typeof(T[])));
                r.Add("System" + udon + "Array.__Get__SystemInt32__System" + udon, 3,
                    (IUdonHeap h, Span<uint> s) =>
                        h.SetHeapVariable(s[2], h.GetHeapVariable<T[]>(s[0])[I32(h, s[1])], typeof(T)));
                r.Add("System" + udon + "Array.__Set__SystemInt32_System" + udon + "__SystemVoid", 3,
                    (IUdonHeap h, Span<uint> s) =>
                        h.GetHeapVariable<T[]>(s[0])[I32(h, s[1])] = h.GetHeapVariable<T>(s[2]));
                r.Add("System" + udon + "Array.__get_Length__SystemInt32", 2,
                    (IUdonHeap h, Span<uint> s) =>
                        SetI32(h, s[1], h.GetHeapVariable<T[]>(s[0]).Length));
            }

            Array1<int>("Int32", "Int32");
            Array1<uint>("UInt32", "UInt32");
            Array1<long>("Int64", "Int64");
            Array1<float>("Single", "Single");
            Array1<double>("Double", "Double");
            Array1<bool>("Boolean", "Boolean");
            Array1<string>("String", "String");
            Array1<object>("Object", "Object");

            r.Add("SystemByteArray.__ctor__SystemInt32__SystemByteArray", 2,
                (IUdonHeap h, Span<uint> s) => h.SetHeapVariable(s[1], new byte[I32(h, s[0])], typeof(byte[])));
            r.Add("SystemByteArray.__Get__SystemInt32__SystemByte", 3,
                (IUdonHeap h, Span<uint> s) =>
                    h.SetHeapVariable(s[2], h.GetHeapVariable<byte[]>(s[0])[I32(h, s[1])], typeof(byte)));
            r.Add("SystemByteArray.__Set__SystemInt32_SystemByte__SystemVoid", 3,
                (IUdonHeap h, Span<uint> s) =>
                    h.GetHeapVariable<byte[]>(s[0])[I32(h, s[1])] = h.GetHeapVariable<byte>(s[2]));
            r.Add("SystemByteArray.__get_Length__SystemInt32", 2,
                (IUdonHeap h, Span<uint> s) => SetI32(h, s[1], h.GetHeapVariable<byte[]>(s[0]).Length));

            // __CopyTo__ is the verified 3-arity form the memory lowering and
            // the init block both use.
            r.Add("SystemByteArray.__CopyTo__SystemArray_SystemInt32__SystemVoid", 3,
                (IUdonHeap h, Span<uint> s) =>
                    h.GetHeapVariable<byte[]>(s[0])
                     .CopyTo(h.GetHeapVariable<byte[]>(s[1]), I32(h, s[2])));

            // The word-array variants. Init copies the globals image into
            // `__mem` with this one, so both sides are `int[]`; without it the
            // SDK registry lists the extern but no stub exists and the VM
            // reports `NotSupportedException` mid-init.
            r.Add("SystemInt32Array.__CopyTo__SystemArray_SystemInt32__SystemVoid", 3,
                (IUdonHeap h, Span<uint> s) =>
                    h.GetHeapVariable<int[]>(s[0])
                     .CopyTo(h.GetHeapVariable<int[]>(s[1]), I32(h, s[2])));
            r.Add("SystemUInt32Array.__CopyTo__SystemArray_SystemInt32__SystemVoid", 3,
                (IUdonHeap h, Span<uint> s) =>
                    h.GetHeapVariable<uint[]>(s[0])
                     .CopyTo(h.GetHeapVariable<uint[]>(s[1]), I32(h, s[2])));

            // The 5-arity forms memcpy/memmove/memset lower to.
            r.Add("SystemArray.__Copy__SystemArray_SystemInt32_SystemArray_SystemInt32_SystemInt32__SystemVoid", 5,
                (IUdonHeap h, Span<uint> s) =>
                    Array.Copy(h.GetHeapVariable<Array>(s[0]), I32(h, s[1]),
                               h.GetHeapVariable<Array>(s[2]), I32(h, s[3]), I32(h, s[4])));
            r.Add("SystemArray.__Clear__SystemArray_SystemInt32_SystemInt32__SystemVoid", 3,
                (IUdonHeap h, Span<uint> s) =>
                    Array.Clear(h.GetHeapVariable<Array>(s[0]), I32(h, s[1]), I32(h, s[2])));
        }

        private static void RegisterString(ExternRegistry r)
        {
            r.Add("SystemString.__Concat__SystemString_SystemString__SystemString", 3,
                (IUdonHeap h, Span<uint> s) =>
                    h.SetHeapVariable(s[2], h.GetHeapVariable<string>(s[0]) + h.GetHeapVariable<string>(s[1]),
                                      typeof(string)));
            r.Add("SystemString.__get_Length__SystemInt32", 2,
                (IUdonHeap h, Span<uint> s) => SetI32(h, s[1], h.GetHeapVariable<string>(s[0]).Length));
            r.Add("SystemString.__op_Equality__SystemString_SystemString__SystemBoolean", 3,
                (IUdonHeap h, Span<uint> s) =>
                    SetB(h, s[2], h.GetHeapVariable<string>(s[0]) == h.GetHeapVariable<string>(s[1])));
            r.Add("SystemString.__op_Inequality__SystemString_SystemString__SystemBoolean", 3,
                (IUdonHeap h, Span<uint> s) =>
                    SetB(h, s[2], h.GetHeapVariable<string>(s[0]) != h.GetHeapVariable<string>(s[1])));
        }

        private static void RegisterVector(ExternRegistry r)
        {
            r.Add("UnityEngineVector3.__op_Addition__UnityEngineVector3_UnityEngineVector3__UnityEngineVector3", 3,
                (IUdonHeap h, Span<uint> s) =>
                {
                    Vector3 a = h.GetHeapVariable<Vector3>(s[0]), b = h.GetHeapVariable<Vector3>(s[1]);
                    h.SetHeapVariable(s[2], new Vector3(a.x + b.x, a.y + b.y, a.z + b.z), typeof(Vector3));
                });
            r.Add("UnityEngineVector3.__op_Subtraction__UnityEngineVector3_UnityEngineVector3__UnityEngineVector3", 3,
                (IUdonHeap h, Span<uint> s) =>
                {
                    Vector3 a = h.GetHeapVariable<Vector3>(s[0]), b = h.GetHeapVariable<Vector3>(s[1]);
                    h.SetHeapVariable(s[2], new Vector3(a.x - b.x, a.y - b.y, a.z - b.z), typeof(Vector3));
                });
            r.Add("UnityEngineVector3.__op_Multiplication__UnityEngineVector3_SystemSingle__UnityEngineVector3", 3,
                (IUdonHeap h, Span<uint> s) =>
                {
                    Vector3 a = h.GetHeapVariable<Vector3>(s[0]);
                    float f = F32(h, s[1]);
                    h.SetHeapVariable(s[2], new Vector3(a.x * f, a.y * f, a.z * f), typeof(Vector3));
                });
            r.Add("UnityEngineVector3.__op_UnaryNegation__UnityEngineVector3__UnityEngineVector3", 2,
                (IUdonHeap h, Span<uint> s) =>
                {
                    Vector3 a = h.GetHeapVariable<Vector3>(s[0]);
                    h.SetHeapVariable(s[1], new Vector3(-a.x, -a.y, -a.z), typeof(Vector3));
                });
            r.Add("UnityEngineVector3.__get_x__SystemSingle", 2,
                (IUdonHeap h, Span<uint> s) => SetF32(h, s[1], h.GetHeapVariable<Vector3>(s[0]).x));
            r.Add("UnityEngineVector3.__get_y__SystemSingle", 2,
                (IUdonHeap h, Span<uint> s) => SetF32(h, s[1], h.GetHeapVariable<Vector3>(s[0]).y));
            r.Add("UnityEngineVector3.__get_z__SystemSingle", 2,
                (IUdonHeap h, Span<uint> s) => SetF32(h, s[1], h.GetHeapVariable<Vector3>(s[0]).z));
            r.Add("UnityEngineVector3.__ctor__SystemSingle_SystemSingle_SystemSingle__UnityEngineVector3", 4,
                (IUdonHeap h, Span<uint> s) =>
                    h.SetHeapVariable(s[3], new Vector3(F32(h, s[0]), F32(h, s[1]), F32(h, s[2])),
                                      typeof(Vector3)));

            r.Add("UnityEngineVector2.__op_Addition__UnityEngineVector2_UnityEngineVector2__UnityEngineVector2", 3,
                (IUdonHeap h, Span<uint> s) =>
                {
                    Vector2 a = h.GetHeapVariable<Vector2>(s[0]), b = h.GetHeapVariable<Vector2>(s[1]);
                    h.SetHeapVariable(s[2], new Vector2(a.x + b.x, a.y + b.y), typeof(Vector2));
                });
            r.Add("UnityEngineVector4.__op_Addition__UnityEngineVector4_UnityEngineVector4__UnityEngineVector4", 3,
                (IUdonHeap h, Span<uint> s) =>
                {
                    Vector4 a = h.GetHeapVariable<Vector4>(s[0]), b = h.GetHeapVariable<Vector4>(s[1]);
                    h.SetHeapVariable(s[2],
                        new Vector4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w), typeof(Vector4));
                });
        }

        private static void RegisterDebug(ExternRegistry r)
        {
            r.Add("UnityEngineDebug.__Log__SystemObject__SystemVoid", 1,
                (IUdonHeap h, Span<uint> s) =>
                {
                    object v = h.GetHeapVariable(s[0]);
                    LogSink(v == null ? "null" : Convert.ToString(v, CultureInfo.InvariantCulture));
                });
        }

        private static void RegisterEventReceiver(ExternRegistry r)
        {
            r.Add("VRCUdonCommonInterfacesIUdonEventReceiver.__SendCustomEvent__SystemString__SystemVoid", 2,
                (IUdonHeap h, Span<uint> s) =>
                {
                    // Instance first, then the event name.
                    string name = h.GetHeapVariable<string>(s[1]);
                    SendCustomEvent(name);
                });
        }
    }
}
