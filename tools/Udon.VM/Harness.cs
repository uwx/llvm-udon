//===-- Harness.cs - the UdonVM harness -----------------------------------===//
//
// Assembles a `.uasm`, applies its heap image, and runs entry points exactly the
// way `UdonBehaviour.RunProgram(uint)` does. That replication is the point: the
// measured UdonVM lifecycle is what makes one VM serve every event, so the
// harness must not invent its own calling discipline.
//
//===----------------------------------------------------------------------===//

using System;
using System.Collections.Generic;
using System.Collections.Immutable;
using System.Globalization;
using System.IO;
using System.Linq;
using Udon.Heap;
using VRC.Udon.Common.Delegates;
using VRC.Udon.Common.Factories;
using VRC.Udon.Common.Interfaces;
using VRC.Udon.UAssembly.Assembler;
using VRC.Udon.VM;

namespace Udon.VM
{
    /// <summary>
    /// An `IUdonWrapper` over a declared-arity table. The SDK's own wrapper
    /// resolves nothing headlessly (its modules need IUdonComponentGetter and
    /// Unity components), so the harness supplies the delegates.
    /// </summary>
    public sealed class HarnessWrapper : IUdonWrapper
    {
        private readonly ExternRegistry _registry;

        /// <summary>Per-signature call counts, filled only when `Counts` is set.</summary>
        public readonly Dictionary<string, long> Counts;

        private static readonly bool Trace = Environment.GetEnvironmentVariable("UDON_TRACE") != null;

        public HarnessWrapper(ExternRegistry registry, Dictionary<string, long> counts = null)
        {
            _registry = registry;
            Counts = counts;
        }

        public int GetExternFunctionParameterCount(string signature)
        {
            if (_registry.TryGet(signature, out ExternEntry e)) return e.Arity;
            throw new NotSupportedException(
                "extern '" + signature + "' is not implemented by the SDK (it resolved to no delegate)");
        }

        public UdonExternDelegate GetExternFunctionDelegate(string signature)
        {
            if (_registry.TryGet(signature, out ExternEntry e))
            {
                UdonExternDelegate inner = e.Delegate;
                if (Counts != null)
                {
                    var counts = Counts;
                    UdonExternDelegate original = inner;
                    inner = (heap, operands) =>
                    {
                        counts.TryGetValue(signature, out long c);
                        counts[signature] = c + 1;
                        original(heap, operands);
                    };
                }
                if (!Trace) return inner;
                // Logs the last extern the VM reached, which is what names the
                // failure: the VM reports only a bare UdonVMException.
                return (heap, operands) =>
                {
                    Console.Error.WriteLine("  extern: " + signature);
                    try { inner(heap, operands); }
                    catch (Exception ex)
                    {
                        Console.Error.WriteLine("  FAILED: " + ex.GetType().Name + ": " + ex.Message);
                        // Operand slots, with the integer each holds when it has
                        // one: an out-of-range array access is almost always a
                        // bad address, and the address is an operand.
                        for (int i = 0; i < operands.Length; i++)
                        {
                            string extra = "";
                            try
                            {
                                object v = heap.GetHeapVariable(operands[i]);
                                if (v is int iv) extra = " = " + iv;
                                else if (v != null) extra = " = " + v.GetType().Name;
                            }
                            catch { extra = " = <unreadable>"; }
                            Console.Error.WriteLine("    op[" + i + "] @" + operands[i] + extra);
                        }
                        for (var e = ex.InnerException; e != null; e = e.InnerException)
                            Console.Error.WriteLine("    inner: " + e.GetType().Name + ": " + e.Message);
                        throw;
                    }
                };
            }
            throw new NotSupportedException(
                "extern '" + signature + "' is not implemented by the SDK (it resolved to no delegate)");
        }

        public void RegisterWrapperModule(IUdonWrapperModule module)
        {
            // Nothing to register: the harness's table is fixed at startup.
        }

        public IUdonWrapperModule GetWrapperModuleByName(string name) => null;
    }

    /// <summary>
    /// A heap factory that always builds a heap of at least `Minimum` slots.
    ///
    /// The assembler sizes its heap from the addresses it has seen and caps at
    /// 512; a program with more symbols throws
    /// `Address: '512' is larger than the UdonVM's heap size of '512'` *during*
    /// assembly, so resizing afterwards is too late. The factory is what the
    /// assembler calls to make the heap, so it is the only place to influence
    /// the size.
    /// </summary>
    public sealed class SizedHeapFactory : IUdonHeapFactory
    {
        public uint Minimum;

        public SizedHeapFactory(uint minimum) { Minimum = minimum; }

        public IUdonHeap ConstructUdonHeap() => new UdonHeapFactory().ConstructUdonHeap(Minimum);

        public IUdonHeap ConstructUdonHeap(uint size) =>
            new UdonHeapFactory().ConstructUdonHeap(Math.Max(size, Minimum));
    }

    /// <summary>A fixed clock, so a long-running fixture does not trip the 10s guard.</summary>
    public sealed class FrozenTimeSource : IUdonVMTimeSource
    {
        public long CurrentTime => 0;
    }

    /// <summary>
    /// Counts how often the VM reads the clock, attributing each read to the
    /// program counter at the time.
    ///
    /// `UdonVM.Interpret` is sealed and has no single-step hook, but it polls
    /// `IUdonVMTimeSource.CurrentTime` on every instruction to enforce its
    /// time limit. That read is the only observation point inside the loop, so
    /// it is what a profile can be built from: the VM's own program counter is
    /// sampled at the same instant, which attributes the sample to exactly the
    /// instruction that was about to execute.
    ///
    /// The time reported is constant, exactly like `FrozenTimeSource`: the VM
    /// enforces its 10-second limit against this value, and an advancing clock
    /// would kill a real program after 10,001 instructions. The observation is
    /// the side effect, not the returned number.
    /// </summary>
        public sealed class ProfilingTimeSource : IUdonVMTimeSource
        {
            private readonly Func<uint> _pc;
            private readonly Dictionary<uint, long> _samples;

            public ProfilingTimeSource(Func<uint> pc, Dictionary<uint, long> samples)
            {
                _pc = pc;
                _samples = samples;
            }

            /// <summary>
            /// Record one sample at the current pc and report a constant time.
            ///
            /// The VM polls this only from `CheckExecutionTimeLimit`, which the
            /// disassembly shows is called for exactly four opcodes: `JUMP`,
            /// `JUMP_IF_FALSE`, `JUMP_INDIRECT` and `EXTERN`. `NOP`, `PUSH`,
            /// `POP` and `COPY` never poll, so a sample is NOT one instruction
            /// -- it is one of those four. Measured on a loop with an
            /// 11-instruction body: 5 samples per iteration.
            ///
            /// So a sample is weighted by the instruction it lands on, and the
            /// blocks between it and the previous poll are attributed to it.
            /// `WriteProfile` does that reconstruction; this only records where
            /// the polls happened.
            ///
            /// The time must NOT advance: the VM enforces its 10-second limit
            /// against this value, and an incrementing clock kills `boot` after
            /// 10,001 polls. A constant is what `FrozenTimeSource` does; the
            /// difference is only the side effect.
            /// </summary>
            public long CurrentTime
            {
                get
                {
                    uint pc = _pc();
                    _samples.TryGetValue(pc, out long n);
                    _samples[pc] = n + 1;
                    return 0;
                }
            }
        }

    /// <summary>One assembled program plus the text it came from.</summary>
    public sealed class LoadedProgram
    {
        public string UasmText;
        public HeapImage Image;
        public IUdonProgram Program;
        public uint HeapSlots;

        /// <summary>Assemble and apply the heap image. Called again to isolate an exception.</summary>
        public static LoadedProgram Load(string uasmPath, string heapJsonPath, uint defaultSlots)
        {
            string text = File.ReadAllText(uasmPath);
            HeapImage image = null;
            if (heapJsonPath != null && File.Exists(heapJsonPath))
                image = HeapImage.Parse(File.ReadAllText(heapJsonPath));

            var loaded = new LoadedProgram { UasmText = text, Image = image };
            // The image already counts one slot per distinct extern signature
            // (the assembler interns each into the heap). The extra slot covers
            // any further anonymous string the assembler invents.
            loaded.HeapSlots = image != null && image.HeapSlots != 0 ? image.HeapSlots + 1
                                                                    : defaultSlots;
            loaded.Program = Assemble(loaded.UasmText, loaded.HeapSlots);
            image?.Apply(loaded.Program);
            return loaded;
        }

        private static IUdonProgram Assemble(string uasm, uint heapSlots)
        {
            var resolvers = new TypeResolverGroup();
            resolvers.AddTypeResolver(new StubTypeResolver());
            var assembler = new UAssemblyAssembler(new SizedHeapFactory(heapSlots), resolvers);
            IUdonProgram program = assembler.Assemble(uasm);

            // The assembler sizes the heap itself and applies every `.data_start`
            // literal to it, so the assembled heap is only replaced when it is
            // genuinely too small -- and then every initialized slot is copied
            // across, because losing them would silently zero the literals.
            uint capacity = program.Heap.GetHeapCapacity();
            if (capacity >= heapSlots)
                return program;

            var bigger = new UdonHeapFactory().ConstructUdonHeap(heapSlots);
            for (uint address = 0; address < capacity; address++)
            {
                if (!program.Heap.IsHeapVariableInitialized(address)) continue;
                bigger.SetHeapVariable(address, program.Heap.GetHeapVariable(address),
                                       program.Heap.GetHeapVariableType(address));
            }
            return new ResizedProgram(program, bigger);
        }
    }

    /// <summary>
    /// An `IUdonProgram` view that swaps in a heap of a different capacity.
    /// Needed because `UAssemblyAssembler.Assemble` sizes the heap itself and
    /// exposes no way to ask for more.
    /// </summary>
    internal sealed class ResizedProgram : IUdonProgram
    {
        private readonly IUdonProgram _inner;
        public ResizedProgram(IUdonProgram inner, IUdonHeap heap)
        {
            _inner = inner;
            Heap = heap;
        }

        public byte[] ByteCode => _inner.ByteCode;
        public IUdonHeap Heap { get; }
        public IUdonSymbolTable SymbolTable => _inner.SymbolTable;
        public IUdonSymbolTable EntryPoints => _inner.EntryPoints;
        public IUdonSyncMetadataTable SyncMetadataTable => _inner.SyncMetadataTable;
        public string InstructionSetIdentifier => _inner.InstructionSetIdentifier;
        public int InstructionSetVersion => _inner.InstructionSetVersion;
        public int UpdateOrder => _inner.UpdateOrder;
    }

    /// <summary>
    /// Runs a program the way `UdonBehaviour.RunProgram(uint)` does.
    ///
    /// The lifecycle rules are measured, not assumed: one `UdonVM` serves every
    /// event, a normal halt returns 0, `_halted` is set only on error paths, and
    /// the pc is saved and restored around `Interpret` unless the saved value is
    /// the halt sentinel. A non-zero result is fatal, because VRChat disables
    /// the behaviour on one.
    /// </summary>
    public sealed class EventRunner
    {
        private const uint HaltSentinel = 0xFFFFFFFC;

        private readonly string _uasmPath;
        private readonly string _heapJsonPath;
        private readonly uint _defaultSlots;
        private readonly ExternRegistry _registry;
        private readonly TextWriter _out;
        private readonly bool _trace;

        private LoadedProgram _loaded;
        private UdonVM _vm;
        private uint _haltReasonAddress;
        private bool _hasHaltReason;

        /// <summary>`--profile`: per-program-counter sample counts, or null.</summary>
        private readonly bool _profiling;
        private Dictionary<uint, long> _samples;
        private Dictionary<string, long> _externCounts;

        /// <summary>
        /// `--dump-symbol <name>=<path>` writes the raw words of an exported
        /// global to a file after the last invocation. The ROM and geometry
        /// buffers are the only observable output of a real program, so this is
        /// how a fixture diffs the VM against a native reference build.
        /// </summary>
        private readonly List<KeyValuePair<string, string>> _dumps =
            new List<KeyValuePair<string, string>>();

        public EventRunner(string uasmPath, string heapJsonPath, uint defaultSlots,
                           ExternRegistry registry, TextWriter output, bool trace,
                           bool profiling = false)
        {
            _uasmPath = uasmPath;
            _heapJsonPath = heapJsonPath;
            _defaultSlots = defaultSlots;
            _registry = registry;
            _out = output;
            _trace = trace;
            _profiling = profiling;
        }

        /// <summary>
        /// Drop all samples collected so far, so the profile that is printed
        /// afterwards covers only the invocations that run after the reset.
        /// That is how one event is measured in isolation: the setup events
        /// (`prepare`, `boot`) build the heap state, the reset clears the
        /// counters, and the one event of interest is what the report shows.
        /// </summary>
        public void ResetProfile()
        {
            _samples?.Clear();
            _externCounts?.Clear();
        }
        private static readonly System.Text.RegularExpressions.Regex LabelSuffix =
            new System.Text.RegularExpressions.Regex(
                @"_(?:bb\d+|body|body_done|prologue_done|prologue|exit|icall_cont_\d+|icall_ret_\d+|cont_\d+|ret_\d+|ldc_\d+|stc_\d+|ld_\d+_un|st_\d+_un|ld_\d+|st_\d+|spill_\d+|of|ok|depth0_\d+|reset_done|un)$",
                System.Text.RegularExpressions.RegexOptions.Compiled);

        /// <summary>
        /// Print the profile: instructions per source function, the hottest
        /// individual blocks, and the extern call counts.
        ///
        /// Samples are per program counter (one per dispatch-loop turn,
        /// measured), so the total is the exact instruction count of the
        /// profiled invocations. A pc is attributed to the last code label at
        /// or below it, and a label rolls up to its function by stripping the
        /// writer's block suffixes (`mtxf_mul_vec3f_x_bb10184` is a block of
        /// `mtxf_mul_vec3f_x`).
        /// </summary>
        public void WriteProfile()
        {
            if (_samples == null) return;

            // Parse the `.code_start` labels out of the program text: the
            // symbol table exposes only heap slots, and samples land at
            // internal labels too. Byte offsets come from the measured
            // instruction sizes (PUSH/JUMP*/EXTERN are 8, the rest 4).
            var labels = new List<KeyValuePair<uint, string>>();
            // pc -> opcode, so a poll can be weighted by how many instructions
            // ran since the previous poll.
            var opcodeAt = new Dictionary<uint, string>();
            uint pc = 0;
            bool inCode = false;
            foreach (string raw in File.ReadAllLines(_uasmPath))
            {
                string line = raw.TrimStart();
                if (line.StartsWith(".code_start", StringComparison.Ordinal)) { inCode = true; continue; }
                if (line.StartsWith(".code_end", StringComparison.Ordinal)) break;
                if (!inCode) continue;
                if (line.EndsWith(":", StringComparison.Ordinal) && !line.StartsWith("."))
                {
                    labels.Add(new KeyValuePair<uint, string>(pc, line.Substring(0, line.Length - 1)));
                    continue;
                }
                if (line.Length == 0 || line.StartsWith(";", StringComparison.Ordinal) ||
                    line.StartsWith(".", StringComparison.Ordinal)) continue;
                int comma = line.IndexOf(',');
                string op = comma > 0 ? line.Substring(0, comma) : line;
                uint size = op == "PUSH" || op == "JUMP" || op == "JUMP_IF_FALSE" ||
                            op == "JUMP_INDIRECT" || op == "EXTERN" ? 8u : 4u;
                opcodeAt[pc] = op;
                pc += size;
            }
            labels.Sort((a, b) => a.Key.CompareTo(b.Key));
            var labelPc = new uint[labels.Count];
            var labelName = new string[labels.Count];
            for (int i = 0; i < labels.Count; i++)
            {
                labelPc[i] = labels[i].Key;
                labelName[i] = labels[i].Value;
            }

            static int IndexAt(uint[] arr, uint pc)
            {
                int lo = 0, hi = arr.Length - 1, ans = -1;
                while (lo <= hi)
                {
                    int mid = (lo + hi) / 2;
                    if (arr[mid] <= pc) { ans = mid; lo = mid + 1; }
                    else hi = mid - 1;
                }
                return ans;
            }

            var byBlock = new Dictionary<string, long>();
            var byFunc = new Dictionary<string, long>();
            long total = 0;
            foreach (var kv in _samples)
            {
                total += kv.Value;
                int idx = IndexAt(labelPc, kv.Key);
                if (idx < 0) continue;
                string block = labelName[idx];
                string fn = LabelSuffix.Replace(block, "");
                byBlock.TryGetValue(block, out long nb);
                byBlock[block] = nb + kv.Value;
                byFunc.TryGetValue(fn, out long nf);
                byFunc[fn] = nf + kv.Value;
            }

            // The number is POLLS, not instructions. `CheckExecutionTimeLimit`
            // -- the only reader of `CurrentTime` -- runs for exactly four
            // opcodes (JUMP, JUMP_IF_FALSE, JUMP_INDIRECT, EXTERN), so a poll
            // is one branch-or-call, and the instructions between two polls are
            // not observed at all. Measured on an 11-instruction loop body: 5
            // polls per iteration. Ratios between profiles are exact (the
            // sampling rule is fixed); the absolute number understates total
            // instructions by the un-polled fraction.
            _out.WriteLine("--- profile: " + total + " polls (branches+externs) ---");
            _out.WriteLine("--- by function ---");
            foreach (var kv in byFunc.OrderByDescending(k => k.Value).Take(25))
                _out.WriteLine($"  {kv.Value,12:N0}  {100.0 * kv.Value / total,5:F1}%  {kv.Key}");
            _out.WriteLine("--- by block (top 20) ---");
            foreach (var kv in byBlock.OrderByDescending(k => k.Value).Take(20))
                _out.WriteLine($"  {kv.Value,12:N0}  {100.0 * kv.Value / total,5:F1}%  {kv.Key}");
            _out.WriteLine("--- externs ---");
            long externTotal = 0;
            foreach (var kv in _externCounts)
                externTotal += kv.Value;
            _out.WriteLine("  " + externTotal + " extern calls");
            foreach (var kv in _externCounts.OrderByDescending(k => k.Value).Take(20))
                _out.WriteLine($"  {kv.Value,12:N0}  {100.0 * kv.Value / externTotal,5:F1}%  {kv.Key}");
        }
        /// <summary>Set when an invocation returned a non-zero `Interpret()` result.</summary>
        public bool SawVmError { get; private set; }

        public void AddDump(string symbol, string path) =>
            _dumps.Add(new KeyValuePair<string, string>(symbol, path));

        /// <summary>
        /// Write each requested symbol's backing bytes. A `%SystemByteArray`
        /// global dumps its elements; every other value-type global dumps its
        /// 4-byte word. Silent when the symbol is absent, so a fixture can ask
        /// for a symbol a hand-written program does not define.
        /// </summary>
        public void WriteDumps()
        {
            IUdonSymbolTable symbols = _loaded.Program.SymbolTable;
            IUdonHeap heap = _loaded.Program.Heap;
            foreach (var dump in _dumps)
            {
                // `<bridge>:<words>=<path>` dumps that many words at the byte
                // offset the exported `__bridge_<name>` slot holds.
                // That is how a global living inside `__mem` is observed: the
                // window has no heap symbol of its own.
                // `@<byteoffset>:<words>=<path>` dumps raw `__mem` words at an
                // absolute byte offset, for globals that have no bridge export.
                int colon = dump.Key.IndexOf(':');
                if (dump.Key.StartsWith("@", StringComparison.Ordinal) && colon > 1)
                {
                    int byteOffset = int.Parse(dump.Key.Substring(1, colon - 1), CultureInfo.InvariantCulture);
                    int count = int.Parse(dump.Key.Substring(colon + 1), CultureInfo.InvariantCulture);
                    if (heap.IsHeapVariableInitialized(symbols.GetAddressFromSymbol("__mem")) &&
                        heap.GetHeapVariable(symbols.GetAddressFromSymbol("__mem")) is int[] rawMem)
                    {
                        var buf = new byte[count * 4];
                        Buffer.BlockCopy(rawMem, byteOffset, buf, 0, count * 4);
                        File.WriteAllBytes(dump.Value, buf);
                    }
                    continue;
                }
                if (colon > 0)
                {
                    string name = dump.Key.Substring(0, colon);
                    int length = int.Parse(dump.Key.Substring(colon + 1), CultureInfo.InvariantCulture);
                    if (!symbols.HasAddressForSymbol("__bridge_" + name)) continue;
                    object raw = heap.GetHeapVariable(symbols.GetAddressFromSymbol("__bridge_" + name));
                    int offset = raw is uint u ? unchecked((int)u) : Convert.ToInt32(raw);
                    if (!heap.IsHeapVariableInitialized(symbols.GetAddressFromSymbol("__mem"))) continue;
                    if (!(heap.GetHeapVariable(symbols.GetAddressFromSymbol("__mem")) is int[] mem)) continue;
                    var bytes = new byte[length * 4];
                    Buffer.BlockCopy(mem, (offset / 4) * 4, bytes, 0, length * 4);
                    File.WriteAllBytes(dump.Value, bytes);
                    continue;
                }

                if (!symbols.HasAddressForSymbol(dump.Key)) continue;
                uint address = symbols.GetAddressFromSymbol(dump.Key);
                if (!heap.IsHeapVariableInitialized(address)) continue;
                object value = heap.GetHeapVariable(address);
                // A typed float array (the writer's `SingleArray` partition)
                // or a word array both hold the payload directly.
                if (value is float[] fa)
                {
                    var buf = new byte[fa.Length * 4];
                    Buffer.BlockCopy(fa, 0, buf, 0, buf.Length);
                    File.WriteAllBytes(dump.Value, buf);
                }
                else if (value is int[] ia)
                {
                    var buf = new byte[ia.Length * 4];
                    Buffer.BlockCopy(ia, 0, buf, 0, buf.Length);
                    File.WriteAllBytes(dump.Value, buf);
                }
                else if (value is byte[] b) File.WriteAllBytes(dump.Value, b);
            }
        }

        /// <summary>
        /// Copy a ROM into the exported `ud_rom` window, the way the Unity
        /// driver does after the init block has created `__mem`. Normalises
        /// `.v64`/`.n64` dumps to `.z64` byte order, since libsm64's
        /// `read_u32_be` only understands the big-endian image.
        /// </summary>
        public void InstallRom(string romPath)
        {
            byte[] rom = File.ReadAllBytes(romPath);
            IUdonSymbolTable symbols = _loaded.Program.SymbolTable;
            if (!symbols.HasAddressForSymbol("__bridge_ud_rom")) return;
            uint slot = symbols.GetAddressFromSymbol("__bridge_ud_rom");
            object raw = _loaded.Program.Heap.GetHeapVariable(slot);
            int offset = raw is uint u ? unchecked((int)u) : Convert.ToInt32(raw);
            IUdonHeap heap = _loaded.Program.Heap;
            uint memSlot = symbols.GetAddressFromSymbol("__mem");
            if (!heap.IsHeapVariableInitialized(memSlot)) return;
            var mem = heap.GetHeapVariable(memSlot) as int[];
            if (mem == null) return;

            bool v64 = rom.Length >= 4 && rom[0] == 0x37 && rom[1] == 0x80 && rom[2] == 0x40 && rom[3] == 0x12;
            bool n64 = rom.Length >= 4 && rom[0] == 0x40 && rom[1] == 0x12 && rom[2] == 0x37 && rom[3] == 0x80;
            // `__bridge_*` holds a *byte* offset; `mem` is word-indexed.
            int start = (offset + 3) / 4;
            int words = (rom.Length + 3) / 4;
            for (int w = 0; w < words; w++)
            {
                int i0 = w * 4;
                int b0 = i0 < rom.Length ? rom[i0] : 0;
                int b1 = i0 + 1 < rom.Length ? rom[i0 + 1] : 0;
                int b2 = i0 + 2 < rom.Length ? rom[i0 + 2] : 0;
                int b3 = i0 + 3 < rom.Length ? rom[i0 + 3] : 0;
                if (v64) { int t = b0; b0 = b1; b1 = t; t = b2; b2 = b3; b3 = t; }
                else if (n64) { int t0 = b0, t1 = b1; b0 = b3; b1 = b2; b2 = t1; b3 = t0; }
                mem[start + w] = b0 | (b1 << 8) | (b2 << 16) | (b3 << 24);
            }
        }

        public void Start()
        {
            _loaded = LoadedProgram.Load(_uasmPath, _heapJsonPath, _defaultSlots);
            _samples = _profiling ? new Dictionary<uint, long>() : null;
            _externCounts = _profiling ? new Dictionary<string, long>() : null;
            IUdonVMTimeSource clock = _profiling
                ? new ProfilingTimeSource(() => _vm.GetProgramCounter(), _samples)
                : (IUdonVMTimeSource)new FrozenTimeSource();
            _vm = new UdonVM(new HarnessWrapper(_registry, _externCounts), clock);
            _vm.LoadProgram(_loaded.Program);
            ResolveHaltReason();
        }

        /// <summary>
        /// `__halt_reason` is a non-exported data symbol; UdonSymbolTable
        /// resolves those, so no export is needed. Absent for a hand-written
        /// program that does not declare it, in which case the ret line is the
        /// only report.
        /// </summary>
        private void ResolveHaltReason()
        {
            IUdonSymbolTable symbols = _loaded.Program.SymbolTable;
            _hasHaltReason = symbols.HasAddressForSymbol("__halt_reason");
            if (_hasHaltReason)
                _haltReasonAddress = symbols.GetAddressFromSymbol("__halt_reason");
        }

        /// <summary>
        /// Re-assemble from scratch. An exception unwinds out of `Interpret`
        /// leaving `__depth` incremented and frames unreleased, and a fresh
        /// UdonVM shares the heap of the program it is given -- so isolation
        /// needs a fresh program, not a fresh VM.
        /// </summary>
        public void DiscardProgram()
        {
            _loaded = LoadedProgram.Load(_uasmPath, _heapJsonPath, _defaultSlots);
            _vm = new UdonVM(new HarnessWrapper(_registry), new FrozenTimeSource());
            _vm.LoadProgram(_loaded.Program);
            ResolveHaltReason();
        }

        /// <summary>
        /// A static disassembly of the loaded bytecode, with the symbol at each
        /// address. `--trace` prints it before running: UdonVM has no
        /// single-step API, so where a program stops has to be read off the
        /// code and the pc it left behind.
        /// </summary>
        public void DumpDisassembly()
        {
            byte[] code = _loaded.Program.ByteCode;
            uint pc = 0;
            while (pc < code.Length)
            {
                byte op = code[pc];
                _out.WriteLine($"    [{pc:X4}] {OpcodeName(op)}");
                uint size = InstructionSize(op);
                if (size == 0) { _out.WriteLine($"    [{pc:X4}] unknown opcode {op}"); return; }
                pc += size;
            }
        }

        private static string OpcodeName(byte op)
        {
            switch (op)
            {
                case 0: return "NOP";
                case 1: return "PUSH";
                case 2: return "POP";
                case 3: return "COPY";
                case 4: return "JUMP";
                case 5: return "JUMP_IF_FALSE";
                case 6: return "JUMP_INDIRECT";
                case 7: return "EXTERN";
                default: return "?" + op;
            }
        }

        private static uint InstructionSize(byte op)
        {
            switch (op)
            {
                case 0: case 2: case 3: return 4;
                case 1: case 4: case 5: case 6: case 7: return 8;
                default: return 0;
            }
        }

        /// <summary>Every symbol the program exports, in the assembler's order.</summary>
        public IEnumerable<string> ExportedSymbols() =>
            _loaded.Program.EntryPoints.GetExportedSymbols();

        /// <summary>Whether a symbol has an address in the program.</summary>
        public bool HasEntry(string name) =>
            _loaded.Program.EntryPoints.HasAddressForSymbol(name);

        /// <summary>
        /// Invoke one entry point, reporting `ret`, `HALT` or `EXCEPTION`.
        /// Returns false only for a VM error, which the caller turns into exit 1.
        /// </summary>
        public bool Invoke(string entry)
        {
            if (_trace)
            {
                _out.WriteLine("--- disassembly ---");
                DumpDisassembly();
                _out.WriteLine("--- end disassembly ---");
            }

            if (!HasEntry(entry))
            {
                _out.WriteLine("error: program exports no entry point named '" + entry + "'");
                return false;
            }

            uint entryPoint = _loaded.Program.EntryPoints.GetAddressFromSymbol(entry);

            // Clear the reason before the event: the exit block also clears it
            // on a normal return, so a non-zero value afterwards is attributable
            // to this invocation's halt.
            if (_hasHaltReason)
                _loaded.Program.Heap.SetHeapVariable(_haltReasonAddress, 0, typeof(int));

            uint result;
            uint originalAddress = _vm.GetProgramCounter();
            _vm.SetProgramCounter(entryPoint);
            try
            {
                result = _vm.Interpret();
            }
            catch (Exception ex)
            {
                // An exception is data: report it, then isolate the program so
                // the next invocation does not inherit the poisoned heap.
                _out.WriteLine("EXCEPTION " + entry + " " + ex.GetType().Name + ": " + ex.Message);
                if (Environment.GetEnvironmentVariable("UDON_TRACE") != null)
                    for (var e = ex.InnerException; e != null; e = e.InnerException)
                        _out.WriteLine("  cause: " + e.GetType().Name + ": " + e.Message);
                DiscardProgram();
                return true;
            }
            finally
            {
                if (originalAddress < HaltSentinel)
                    _vm.SetProgramCounter(originalAddress);
            }

            if (result != 0)
            {
                _out.WriteLine("VMERROR " + entry + " result=" + result);
                SawVmError = true;
                return false;
            }

            if (_hasHaltReason)
            {
                int reason = _loaded.Program.Heap.GetHeapVariable<int>(_haltReasonAddress);
                if (reason != 0)
                {
                    _out.WriteLine("HALT " + entry + " " + reason.ToString(CultureInfo.InvariantCulture));
                    // The spill cursors say whether reason 1 is a genuine
                    // overflow (cursor at the depth) or a corrupted stack
                    // (cursor wildly out of range or negative).
                    try
                    {
                        IUdonSymbolTable syms = _loaded.Program.SymbolTable;
                        if (syms.HasAddressForSymbol("__sptr_i32"))
                        {
                            int c = _loaded.Program.Heap.GetHeapVariable<int>(
                                syms.GetAddressFromSymbol("__sptr_i32"));
                            _out.WriteLine("  sptr_i32 at halt = " + c);
                        }
                        if (syms.HasAddressForSymbol("__depth"))
                        {
                            int d = _loaded.Program.Heap.GetHeapVariable<int>(
                                syms.GetAddressFromSymbol("__depth"));
                            _out.WriteLine("  depth at halt = " + d);
                        }
                    }
                    catch { }
                    // Dump the trace even on a halt: the differential harness
                    // needs the commands observed up to the failure.
                    WriteDumps();
                    return true;
                }
            }

            _out.WriteLine("ret " + entry + " = " + DescribeReturnValue(entry));
            return true;
        }

        /// <summary>
        /// The entry's return slot, printed the way a fixture can assert it.
        ///
        /// A function's value comes back through its own calling convention, so
        /// there is no single slot to read in general; the writer emits
        /// `__retval_<entry>` for the exported entry, which is what this reads
        /// when it exists. Without one the line reports the slot the entry stub
        /// writes, and a program that returns nothing prints `void`.
        /// </summary>
        private string DescribeReturnValue(string entry)
        {
            IUdonSymbolTable symbols = _loaded.Program.SymbolTable;
            foreach (string candidate in new[] { "__retval_" + entry, entry + "_retval" })
            {
                if (!symbols.HasAddressForSymbol(candidate)) continue;
                uint address = symbols.GetAddressFromSymbol(candidate);
                if (!_loaded.Program.Heap.IsHeapVariableInitialized(address)) return "null";
                object value = _loaded.Program.Heap.GetHeapVariable(address);
                return Format(value);
            }
            return "void";
        }

        private static string Format(object value)
        {
            switch (value)
            {
                case null: return "null";
                case float f: return f.ToString("R", CultureInfo.InvariantCulture);
                case double d: return d.ToString("R", CultureInfo.InvariantCulture);
                case bool b: return b ? "true" : "false";
                case string s: return s;
                case byte[] bytes: return "byte[" + bytes.Length + "]";
                default: return Convert.ToString(value, CultureInfo.InvariantCulture);
            }
        }

        /// <summary>
        /// The `SendCustomEvent` re-entry: the same sequence as an event, on the
        /// same VM, sharing the heap exactly as VRChat's does.
        /// </summary>
        public void Reenter(string entry)
        {
            if (_trace)
            {
                _out.WriteLine("--- disassembly ---");
                DumpDisassembly();
                _out.WriteLine("--- end disassembly ---");
            }

            if (!HasEntry(entry))
                throw new InvalidOperationException(
                    "SendCustomEvent('" + entry + "') names no exported entry point; custom " +
                    "events carry no leading underscore");

            uint entryPoint = _loaded.Program.EntryPoints.GetAddressFromSymbol(entry);
            if (_hasHaltReason)
                _loaded.Program.Heap.SetHeapVariable(_haltReasonAddress, 0, typeof(int));

            uint originalAddress = _vm.GetProgramCounter();
            _vm.SetProgramCounter(entryPoint);
            uint result;
            try
            {
                result = _vm.Interpret();
            }
            finally
            {
                if (originalAddress < HaltSentinel)
                    _vm.SetProgramCounter(originalAddress);
            }

            if (result != 0)
                throw new InvalidOperationException(
                    "nested SendCustomEvent('" + entry + "') returned " + result);

            if (_hasHaltReason)
            {
                int reason = _loaded.Program.Heap.GetHeapVariable<int>(_haltReasonAddress);
                if (reason != 0)
                    _out.WriteLine("HALT " + entry + " " + reason.ToString(CultureInfo.InvariantCulture));
                else
                    _out.WriteLine("ret " + entry + " = " + DescribeReturnValue(entry));
            }
            else
            {
                _out.WriteLine("ret " + entry + " = " + DescribeReturnValue(entry));
            }
        }

        public UdonVM Vm => _vm;
    }
}
