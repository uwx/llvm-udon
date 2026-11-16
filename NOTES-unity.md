# The Unity integration, as measured

What the importer needs, what the SDK already owns, and the traps that only show
up inside a live editor.

## `.uasm` is already a Unity asset type

The SDK registers a `ScriptedImporter` for the extension:

`VRC.Udon.Editor.ProgramSources.UdonAssemblyProgramAssetImporter`
(`Editor/Udon/ProgramSources/UdonAssemblyProgram/`), `[ScriptedImporter(1,
"uasm")]`. It creates a `UdonAssemblyProgramAsset`, copies the file into its
`udonAssembly` field, and calls `RefreshProgram()`.

Two consequences:

* **A second `ScriptedImporter` for `uasm` is rejected.** Unity logs
  `Multiple scripted importers are targeting the extension 'uasm' and have all
  been rejected` and neither one runs.
* **A plain `AssetPostprocessor` never sees a `.uasm`.** `OnPostprocessAllAssets`
  is called for assets the database actually imports, and an extension with no
  importer gets no importer at all — so the callback does not fire, and a
  pending-work queue built there stays empty.

So the integration does not claim the extension. `.uasm` stays owned by the SDK
and the import path is explicit:

* `Tools > Udon > Import UASM Programs` — every `.uasm` under `Assets/`
* `Assets > Udon > Import UASM Program` — the selection
* `UdonProgramImporter.Import(path)` — from a build script

The SDK's own importer is unusable for this purpose anyway: `RefreshProgram`
assembles through `UdonEditorManager.Instance.Assemble`, which caps the heap at
512 slots.

## `AssetDatabase.CreateAsset` is restricted during import

Creating an asset from inside an import callback logs:

```
Calls to "AssetDatabase.CreateAsset" are restricted during asset importing.
```

and produces a file named `00000000000000000000000000000000.asset` — an empty
GUID. `SaveAssets` and `Refresh` are restricted the same way. That is the second
reason the import is not driven from a callback.

## The asmdef

`Assets/Udon/Editor/Udon.Importer.asmdef`. Two rules, both learned by getting
them wrong:

**Reference SDK assemblies by asmdef name, not by precompiled DLL.**
`SerializedUdonProgramAsset` lives in the `VRC.Udon` *script assembly*
(`Runtime/Udon/VRC.Udon.asmdef`), not in a precompiled DLL — so
`VRC.Udon.Common.dll` in `precompiledReferences` does not provide it, and the
error is `CS0234: The type or namespace name 'ProgramSources' does not exist in
the namespace 'VRC.Udon'`.

**`overrideReferences: true` silently drops everything unlisted.** With it set,
only `precompiledReferences` are visible, so `IUdonProgram` and
`HeapImage.Parse` disappear. The SDK's own asmdefs use `references` by name and
no precompiled list at all; matching that pattern is what makes it compile.

The working set:

```json
"references": ["VRC.Udon", "VRC.Udon.Editor", "UdonSharp.Editor"],
"overrideReferences": true,
"precompiledReferences": [
  "Udon.Heap.dll", "VRC.Udon.Common.dll", "VRC.Udon.UAssembly.dll",
  "VRC.Udon.VM.dll", "Collections.Pooled.dll",
  "System.Collections.Immutable.dll"
]
```

`VRC.Udon` is a plain reference (a script assembly); the rest genuinely are
precompiled DLLs.

## `StoreProgram`

`SerializedUdonProgramAsset` has two overloads:

```csharp
public override void StoreProgram(IUdonProgram udonProgram);
public override void StoreProgram(IUdonProgram udonProgram,
                                  NetworkCallingEntrypointMetadata[] meta);
```

The two-argument one names `NetworkCallingEntrypointMetadata`, which lives in
`VRCSDK3` — dragging that assembly into the reference set for no gain, since the
overload just calls the first and then stores the null metadata. Use the
single-argument form.

## Running a program in the editor

`UdonManager.Instance` calls `DontDestroyOnLoad` and therefore only exists in
play mode:

```
InvalidOperationException: The following game object is invoking the
DontDestroyOnLoad method: UdonManager. Notice that DontDestroyOnLoad can only be
used in play mode, and as such, cannot be part of an editor script.
```

To execute a program headlessly inside the editor:

1. Enter play mode.
2. Create a `GameObject`, add a `UdonBehaviour`, set its
   `serializedProgramAsset` field, and call `InitializeUdonContent()` — the
   program loads lazily, so `_program` is null until this runs.
3. Take `_udonVM` and drive it with the `RunProgram` sequence.

A `UdonBehaviour`'s `_udonVM` **stays halted after a run**: a second
`Interpret()` on it returns 0 without executing anything, leaving the heap
untouched. Each verification needs a fresh `UdonBehaviour`, or a fresh
`UdonVM` borrowing the behaviour's `_udonWrapper`.

The wrapper itself is `VRC.Udon.Wrapper.UdonWrapper`, constructed empty and
populated by registering its modules — which need `IUdonComponentGetter` and
`IUdonSecurityFilter` (or `IUdonSecurityFilter<UnityEngine.Object>` for the
`UnityEngine*` modules). `UdonDefaultWrapperFactory` exists for this, but its
constructor takes the generic filter and the SDK ships only the interface, so
borrowing a wrapper from an initialized `UdonBehaviour` is the practical route.

## Verified result

`test/programs/sum.rs` → `.uasm` → importer → `SerializedUdonProgramAsset` →
Unity's `UdonVM` with the real registered wrapper:

```
interpret=0 ret=9
```

matching the headless harness exactly.

## The 512-slot cap, revisited

See `NOTES-udonvm.md` for the measurement. In short:
`Assemble(uasm, N)` needs headroom over the symbol count, and the importer
passes `N + N/4 + 64`.
