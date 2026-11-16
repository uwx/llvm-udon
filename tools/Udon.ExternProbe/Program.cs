//===-- Program.cs - probe the real SDK wrapper for extern signatures -----===//
//
// Reads a JSON list of candidate signatures, asks the real
// `VRC.Udon.Wrapper.UdonWrapper` about each, and writes the result as JSON.
//
// Why this exists rather than an editor round-trip: the wrapper's modules take
// only `IUdonComponentGetter` and `IUdonSecurityFilter`, both of which are pure
// dispatch interfaces, so the wrapper constructs in plain .NET with no Unity
// runtime. `GetExternFunctionParameterCount` and `GetExternFunctionDelegate`
// then answer exactly as they do in the editor, including the "not implemented
// yet" throw for a registry entry that has no implementation. That distinction
// is what the extern table has to record, and it is the one the node registry
// cannot tell you.
//
// The Unity-facing modules (`UnityEngineDebug`, `UnityEngineVector3`, ...) take
// `IUdonSecurityFilter<UnityEngine.Object>` instead of the non-generic filter,
// so the probe resolves Unity's managed assemblies from `--unity-managed` (or
// the UNITY_MANAGED_DIR environment variable) and builds the generic filter by
// reflection. Without that directory those modules cannot construct, and their
// signatures are reported unresolved -- which would be a lie about the SDK, so
// the probe says so on stderr rather than silently emitting a smaller table.
//
// Exit codes: 0 all candidates probed, 1 the wrapper could not be built.
//
//===----------------------------------------------------------------------===//

using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Text.Json;
using VRC.Udon.Common.Interfaces;

namespace Udon.ExternProbe
{
    /// <summary>
    /// `IUdonComponentGetter` is how a wrapper module reaches Unity components.
    /// Nothing here runs a scene, so every member is a no-op: a module that asks
    /// for a component gets nothing, which is what a headless probe wants.
    /// </summary>
    internal sealed class NullComponentGetter : IUdonComponentGetter
    {
        public void GetComponent__T(IUdonHeap h, Span<uint> a) { }
        public void GetComponentInChildren__T(IUdonHeap h, Span<uint> a) { }
        public void GetComponentInChildren__SystemBoolean__T(IUdonHeap h, Span<uint> a) { }
        public void GetComponentInParent__T(IUdonHeap h, Span<uint> a) { }
        public void GetComponentInParent__SystemBoolean__T(IUdonHeap h, Span<uint> a) { }
        public void GetComponents__TArray(IUdonHeap h, Span<uint> a) { }
        public void GetComponents__ListT__SystemVoid(IUdonHeap h, Span<uint> a) { }
        public void GetComponentsInChildren__SystemBoolean__TArray(IUdonHeap h, Span<uint> a) { }
        public void GetComponentsInChildren__SystemBoolean_ListT__SystemVoid(IUdonHeap h, Span<uint> a) { }
        public void GetComponentsInChildren__TArray(IUdonHeap h, Span<uint> a) { }
        public void GetComponentsInChildren__ListT__SystemVoid(IUdonHeap h, Span<uint> a) { }
        public void GetComponentsInParent__SystemBoolean_ListT__SystemVoid(IUdonHeap h, Span<uint> a) { }
        public void GetComponentsInParent__SystemBoolean__TArray(IUdonHeap h, Span<uint> a) { }
        public void GetComponentsInParent__TArray(IUdonHeap h, Span<uint> a) { }
    }

    /// <summary>The non-generic security filter's members, all pass-through.</summary>
    internal sealed class PermissiveSecurityFilter : IUdonSecurityFilter
    {
        public void ApplyFilter<T>(ref T value) where T : class { }
        public void ApplyLightCullingMaskFilter(ref int mask) { }
        public int LightReservedLayerMask { get; set; }
    }

    public static class Program
    {
        public static int Main(string[] args)
        {
            string inputPath = null;
            string outputPath = null;
            string unityManaged = Environment.GetEnvironmentVariable("UNITY_MANAGED_DIR");

            for (int i = 0; i < args.Length; i++)
            {
                if (args[i] == "--unity-managed" && i + 1 < args.Length) { unityManaged = args[++i]; continue; }
                if (inputPath == null) { inputPath = args[i]; continue; }
                if (outputPath == null) { outputPath = args[i]; continue; }
            }

            if (!string.IsNullOrEmpty(unityManaged) && Directory.Exists(unityManaged))
            {
                AppDomain.CurrentDomain.AssemblyResolve += (_, e) =>
                {
                    string candidate = Path.Combine(unityManaged, new AssemblyName(e.Name).Name + ".dll");
                    if (!File.Exists(candidate)) return null;
                    try { return Assembly.LoadFrom(candidate); } catch { return null; }
                };
            }

            string json = inputPath != null ? File.ReadAllText(inputPath) : Console.In.ReadToEnd();
            List<string> candidates;
            try
            {
                candidates = JsonSerializer.Deserialize<List<string>>(json) ?? new List<string>();
            }
            catch (JsonException ex)
            {
                Console.Error.WriteLine("udon-extern-probe: input is not a JSON array of strings: " + ex.Message);
                return 1;
            }

            IUdonWrapper wrapper;
            Type wrapperAssemblyType;
            try
            {
                Type wrapperType = Assembly.Load("VRC.Udon.Wrapper").GetType("VRC.Udon.Wrapper.UdonWrapper");
                wrapper = (IUdonWrapper)Activator.CreateInstance(wrapperType);
                wrapperAssemblyType = wrapperType;
            }
            catch (Exception ex)
            {
                Console.Error.WriteLine("udon-extern-probe: cannot construct the SDK wrapper: " +
                                        ex.GetType().Name + ": " + ex.Message);
                return 1;
            }

            var getter = new NullComponentGetter();
            var plainFilter = new PermissiveSecurityFilter();
            object unityFilter = TryMakeUnityFilter();

            int registered = 0;
            var unavailable = new List<string>();
            foreach (Type moduleType in wrapperAssemblyType.Assembly.GetTypes())
            {
                if (moduleType.IsInterface || moduleType.IsAbstract) continue;
                if (!typeof(IUdonWrapperModule).IsAssignableFrom(moduleType)) continue;

                // The module's own constructor says which filter it wants; the
                // Unity-facing ones take the generic filter over UnityEngine.Object.
                object filter = plainFilter;
                try
                {
                    ConstructorInfo ctor = moduleType.GetConstructors().FirstOrDefault();
                    if (ctor != null)
                    {
                        Type[] parameterTypes = ctor.GetParameters().Select(p => p.ParameterType).ToArray();
                        if (parameterTypes.Length == 2 && parameterTypes[1] != typeof(IUdonSecurityFilter))
                            filter = unityFilter;
                    }
                }
                catch
                {
                    // A signature that cannot even be read is an unavailable module.
                }

                try
                {
                    var module = (IUdonWrapperModule)Activator.CreateInstance(moduleType, getter, filter);
                    wrapper.RegisterWrapperModule(module);
                    registered++;
                }
                catch (Exception ex)
                {
                    unavailable.Add(moduleType.Name + " (" + ex.GetType().Name + ")");
                }
            }

            var results = new List<Dictionary<string, object>>();
            foreach (string signature in candidates)
            {
                var entry = new Dictionary<string, object> { ["signature"] = signature };
                try
                {
                    int arity = wrapper.GetExternFunctionParameterCount(signature);
                    var del = wrapper.GetExternFunctionDelegate(signature);
                    entry["arity"] = arity;
                    entry["implemented"] = del != null;
                    if (del == null) entry["error"] = "no delegate";
                }
                catch (Exception ex)
                {
                    entry["implemented"] = false;
                    entry["error"] = ex.GetType().Name + ": " + ex.Message;
                }
                results.Add(entry);
            }

            var payload = new Dictionary<string, object>
            {
                ["registeredModules"] = registered,
                ["unavailableModules"] = unavailable,
                ["results"] = results,
            };
            string outJson = JsonSerializer.Serialize(payload,
                new JsonSerializerOptions { WriteIndented = false });

            if (outputPath != null) File.WriteAllText(outputPath, outJson, new UTF8Encoding(false));
            else Console.Out.Write(outJson);

            Console.Error.WriteLine($"udon-extern-probe: {registered} modules registered, " +
                                    $"{unavailable.Count} unavailable, " +
                                    $"{results.Count(r => (bool)r["implemented"])}/{results.Count} signatures implemented");
            if (unavailable.Count > 0)
                Console.Error.WriteLine("udon-extern-probe: unavailable modules: " +
                                        string.Join(", ", unavailable.Take(10)) +
                                        (unavailable.Count > 10 ? ", ..." : ""));
            return 0;
        }

        /// <summary>
        /// Build an instance of `IUdonSecurityFilter&lt;UnityEngine.Object&gt;`
        /// without referencing Unity at compile time: the interface is
        /// `ApplyFilter(T ByRef)` and nothing else, so a dynamic type over
        /// `System.Reflection.Emit` is unnecessary -- a shared instance of a
        /// runtime-generated implementation would be, but the interface is
        /// generic over a Unity type the probe cannot name, so reflection over a
        /// helper in this assembly cannot satisfy it either.
        ///
        /// Instead the probe asks the SDK for a concrete implementation the SDK
        /// itself ships, if it has one.
        /// </summary>
        private static object TryMakeUnityFilter()
        {
            try
            {
                Assembly common = typeof(IUdonSecurityFilter).Assembly;
                foreach (Type t in common.GetTypes())
                {
                    if (t.IsInterface || t.IsAbstract) continue;
                    if (!t.Name.Contains("SecurityFilter")) continue;
                    if (t.IsGenericTypeDefinition) continue;
                    ConstructorInfo ctor = t.GetConstructors().FirstOrDefault(c => c.GetParameters().Length == 0);
                    if (ctor == null) continue;
                    return ctor.Invoke(null);
                }
            }
            catch
            {
                // Fall through: the caller reports the module as unavailable.
            }
            return null;
        }
    }
}
