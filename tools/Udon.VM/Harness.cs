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

        public HarnessWrapper(ExternRegistry registry)
        {
            _registry = registry;
        }

        public int GetExternFunctionParameterCount(string signature)
        {
            if (_registry.TryGet(signature, out ExternEntry e)) return e.Arity;
            throw new NotSupportedException(
                "extern '" + signature + "' is not implemented by the SDK (it resolved to no delegate)");
        }

        public UdonExternDelegate GetExternFunctionDelegate(string signature)
        {
            if (_registry.TryGet(signature, out ExternEntry e)) return e.Delegate;
            throw new NotSupportedException(
                "extern '" + signature + "' is not implemented by the SDK (it resolved to no delegate)");
        }

        public void RegisterWrapperModule(IUdonWrapperModule module)
        {
            // Nothing to register: the harness's table is fixed at startup.
        }

        public IUdonWrapperModule GetWrapperModuleByName(string name) => null;
    }

    /// <summary>A fixed clock, so a long-running fixture does not trip the 10s guard.</summary>
    public sealed class FrozenTimeSource : IUdonVMTimeSource
    {
        public long CurrentTime => 0;
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
            loaded.HeapSlots = image != null && image.HeapSlots != 0 ? image.HeapSlots : defaultSlots;
            loaded.Program = Assemble(loaded.UasmText, loaded.HeapSlots);
            image?.Apply(loaded.Program);
            return loaded;
        }

        private static IUdonProgram Assemble(string uasm, uint heapSlots)
        {
            var resolvers = new TypeResolverGroup();
            resolvers.AddTypeResolver(new StubTypeResolver());
            var assembler = new UAssemblyAssembler(new UdonHeapFactory(), resolvers);
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

        public EventRunner(string uasmPath, string heapJsonPath, uint defaultSlots,
                           ExternRegistry registry, TextWriter output, bool trace)
        {
            _uasmPath = uasmPath;
            _heapJsonPath = heapJsonPath;
            _defaultSlots = defaultSlots;
            _registry = registry;
            _out = output;
            _trace = trace;
        }

        /// <summary>Set when an invocation returned a non-zero `Interpret()` result.</summary>
        public bool SawVmError { get; private set; }

        public void Start()
        {
            _loaded = LoadedProgram.Load(_uasmPath, _heapJsonPath, _defaultSlots);
            _vm = new UdonVM(new HarnessWrapper(_registry), new FrozenTimeSource());
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
