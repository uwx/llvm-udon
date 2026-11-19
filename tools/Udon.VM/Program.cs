//===-- Program.cs - udon-vm command line --------------------------------===//
//
//   udon-vm run <program.uasm> [--heap-json <json>] [--entry <name>]...
//                              [--stubs <json>] [--trace] [--time-limit]
//                              [--repeat N] [--heap-slots N] [--profile]
//
// Exit codes: 0 when every invocation returned or threw, 1 when an invocation
// hit a VM error or the harness itself failed.
//
//===----------------------------------------------------------------------===//

using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Linq;
using System.Text.RegularExpressions;
using Udon.Heap;
using VRC.Udon.Common.Factories;
using VRC.Udon.Common.Interfaces;
using VRC.Udon.UAssembly.Assembler;

namespace Udon.VM
{
    public static class Program
    {
        private static int Usage()
        {
            Console.Error.WriteLine(
                "usage: udon-vm run <program.uasm> [--heap-json <json>] [--entry <name>]...\n" +
                "                        [--stubs <json>] [--trace] [--time-limit]\n" +
                "                        [--repeat N] [--heap-slots N] [--rom <rom>]\n" +
                "                        [--dump-symbol <name>=<path>]... [--profile]\n" +
                "       udon-vm assemble <program.uasm> --heap-slots N\n" +
                "       udon-vm arity <signature>\n" +
                "       udon-vm verify <program.uasm>");
            return 2;
        }

        public static int Main(string[] args)
        {
            if (args.Length == 0) return Usage();

            // The harness reports LOG lines as they happen, in call order, so
            // they interleave correctly with ret/HALT/EXCEPTION.
            Stubs.LogSink = text => Console.Out.WriteLine("LOG: " + text);

            ExternRegistry registry = ExternRegistry.CreateDefault(Console.Error);
            string stubsPath = null;
            for (int i = 0; i + 1 < args.Length; i++)
                if (args[i] == "--stubs") stubsPath = args[i + 1];

            switch (args[0])
            {
                case "run": return Run(args, registry, stubsPath);
                case "assemble": return AssembleCmd(args);
                case "arity": return ArityCmd(args, registry);
                case "verify": return VerifyCmd(args, registry);
                default: return Usage();
            }
        }

        private static List<string> Positional(string[] args)
        {
            var list = new List<string>();
            for (int i = 1; i < args.Length; i++)
            {
                if (args[i].StartsWith("--", StringComparison.Ordinal))
                {
                    // Skip the value of a flag that takes one.
                    if (args[i] == "--heap-json" || args[i] == "--entry" || args[i] == "--stubs" ||
                        args[i] == "--repeat" || args[i] == "--heap-slots" ||
                        args[i] == "--rom" || args[i] == "--dump-symbol" ||
                        args[i] == "--profile-entry")
                        i++;
                    continue;
                }
                list.Add(args[i]);
            }
            return list;
        }

        private static string FlagValue(string[] args, string flag)
        {
            for (int i = 0; i + 1 < args.Length; i++)
                if (args[i] == flag) return args[i + 1];
            return null;
        }

        private static int Run(string[] args, ExternRegistry registry, string stubsPath)
        {
            List<string> positional = Positional(args);
            if (positional.Count != 1) return Usage();
            string uasm = positional[0];

            if (stubsPath != null)
            {
                if (!File.Exists(stubsPath))
                {
                    Console.Error.WriteLine("udon-vm: no such stubs file: " + stubsPath);
                    return 1;
                }
                registry.LoadArityFile(stubsPath);
            }
            else
            {
                // test/externs.json sits beside the fixtures; use it when the
                // caller did not name one, so arities match the live registry.
                string beside = Path.Combine(Path.GetDirectoryName(Path.GetFullPath(uasm)) ?? ".", "externs.json");
                if (File.Exists(beside)) registry.LoadArityFile(beside);
            }

            var entries = new List<string>();
            for (int i = 1; i + 1 < args.Length; i++)
                if (args[i] == "--entry") entries.Add(args[i + 1]);

            int repeat = 1;
            string repeatText = FlagValue(args, "--repeat");
            if (repeatText != null &&
                (!int.TryParse(repeatText, NumberStyles.Integer, CultureInfo.InvariantCulture, out repeat) || repeat < 1))
            {
                Console.Error.WriteLine("udon-vm: --repeat takes a positive integer, got " + repeatText);
                return 1;
            }

            uint slots = 0;
            string slotsText = FlagValue(args, "--heap-slots");
            if (slotsText != null &&
                (!uint.TryParse(slotsText, NumberStyles.Integer, CultureInfo.InvariantCulture, out slots) || slots == 0))
            {
                Console.Error.WriteLine("udon-vm: --heap-slots takes a positive integer, got " + slotsText);
                return 1;
            }

            bool profiling = args.Contains("--profile");
            var runner = new EventRunner(uasm, FlagValue(args, "--heap-json"), slots == 0 ? 4096u : slots,
                                         registry, Console.Out, args.Contains("--trace"), profiling);
            try
            {
                runner.Start();
            }
            catch (Exception ex) when (ex is VRC.Udon.UAssembly.Assembler.AssemblyException ||
                                       ex.GetType().Name == "ParseException")
            {
                Console.Error.WriteLine("udon-vm: " + ex.GetType().Name + ": " + ex.Message);
                return 1;
            }
            catch (InvalidOperationException ex)
            {
                Console.Error.WriteLine("udon-vm: " + ex.Message);
                return 1;
            }

            // The re-entry stub closes over the runner, so it must be bound
            // after the runner exists and before any event runs.
            Stubs.SendCustomEvent = name =>
            {
                try { runner.Reenter(name); }
                catch (Exception ex)
                {
                    // The VM wraps every extern failure in a bare
                    // `UdonVMException`; the cause names the extern, so print
                    // the whole chain when tracing is on.
                    if (Environment.GetEnvironmentVariable("UDON_TRACE") != null)
                        for (var e = ex; e != null; e = e.InnerException)
                            Console.Error.WriteLine("  trace: " + e.GetType().Name +
                                                    ": " + e.Message);
                    throw;
                }
            };

            if (entries.Count == 0)
                entries.AddRange(runner.ExportedSymbols());

            if (entries.Count == 0)
            {
                Console.Error.WriteLine("udon-vm: program exports no entry points; pass --entry");
                return 1;
            }

            // The ROM window lives inside `__mem`, which the writer's inlined
            // init block creates on the first entry invocation. So a ROM run
            // is: run `prepare` (init block only), copy the ROM in, then run
            // whatever entries the caller asked for -- normally `boot`.
            string romPath = FlagValue(args, "--rom");
            if (romPath != null)
            {
                if (!File.Exists(romPath))
                {
                    Console.Error.WriteLine("udon-vm: no such ROM: " + romPath);
                    return 1;
                }
                if (!runner.Invoke("prepare")) return 1;
                runner.InstallRom(romPath);
            }

            string profileEntry = FlagValue(args, "--profile-entry");
            for (int n = 0; n < repeat; n++)
                foreach (string entry in entries)
                {
                    if (profiling && profileEntry != null && entry == profileEntry)
                        runner.ResetProfile();
                    if (!runner.Invoke(entry))
                        return 1;
                }

            for (int i = 1; i + 1 < args.Length; i++)
                if (args[i] == "--dump-symbol")
                {
                    int eq = args[i + 1].IndexOf('=');
                    if (eq > 0)
                        runner.AddDump(args[i + 1].Substring(0, eq), args[i + 1].Substring(eq + 1));
                }
            runner.WriteDumps();
            runner.WriteProfile();
            return 0;
        }

        private static int AssembleCmd(string[] args)
        {
            List<string> positional = Positional(args);
            if (positional.Count != 1) return Usage();
            string slotsText = FlagValue(args, "--heap-slots");
            if (slotsText == null)
            {
                Console.Error.WriteLine("udon-vm assemble: --heap-slots is required");
                return 1;
            }
            if (!uint.TryParse(slotsText, NumberStyles.Integer, CultureInfo.InvariantCulture, out uint slots) ||
                slots == 0)
            {
                Console.Error.WriteLine("udon-vm assemble: --heap-slots takes a positive integer, got " + slotsText);
                return 1;
            }

            try
            {
                var loaded = LoadedProgram.Load(positional[0], null, slots);
                Console.Out.WriteLine("heapSlots=" + loaded.HeapSlots);
                Console.Out.WriteLine("heapCapacity=" + loaded.Program.Heap.GetHeapCapacity());
                Console.Out.WriteLine("byteCodeLength=" + loaded.Program.ByteCode.Length);
                foreach (string symbol in loaded.Program.EntryPoints.GetExportedSymbols())
                    Console.Out.WriteLine("export " + symbol + " @ 0x" +
                        loaded.Program.EntryPoints.GetAddressFromSymbol(symbol).ToString("X8"));
                foreach (string symbol in loaded.Program.SymbolTable.GetSymbols())
                    Console.Out.WriteLine("symbol " + symbol + " @ 0x" +
                        loaded.Program.SymbolTable.GetAddressFromSymbol(symbol).ToString("X8"));
                return 0;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("udon-vm: " + ex.GetType().Name + ": " + ex.Message);
                return 1;
            }
        }

        private static int ArityCmd(string[] args, ExternRegistry registry)
        {
            List<string> positional = Positional(args);
            if (positional.Count != 1) return Usage();
            string signature = positional[0];
            if (registry.TryGet(signature, out ExternEntry e))
            {
                Console.Out.WriteLine(e.Arity);
                return 0;
            }
            Console.Error.WriteLine("udon-vm: extern '" + signature +
                                    "' is not implemented by the SDK (it resolved to no delegate)");
            return 1;
        }

        /// <summary>
        /// Every `EXTERN, "signature"` the program names must resolve. This is
        /// the check that turns the registry-over-reports problem into a build
        /// failure instead of a runtime surprise.
        /// </summary>
        private static int VerifyCmd(string[] args, ExternRegistry registry)
        {
            List<string> positional = Positional(args);
            if (positional.Count != 1) return Usage();
            string text = File.ReadAllText(positional[0]);

            var missing = new List<string>();
            var seen = new HashSet<string>(StringComparer.Ordinal);
            foreach (Match m in Regex.Matches(text, "EXTERN\\s*,\\s*\"([^\"]+)\""))
            {
                string signature = m.Groups[1].Value;
                if (!seen.Add(signature)) continue;
                if (!registry.TryGet(signature, out _)) missing.Add(signature);
            }

            if (missing.Count == 0)
            {
                Console.Out.WriteLine("verify: " + seen.Count + " externs, all implemented");
                return 0;
            }

            foreach (string signature in missing)
                Console.Out.WriteLine("verify: " + signature +
                                      " is not implemented by the SDK (it resolved to no delegate)");
            return 1;
        }
    }
}
