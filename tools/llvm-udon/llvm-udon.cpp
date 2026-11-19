//===-- llvm-udon.cpp - LLVM to VRChat Udon translator --------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Translates an LLVM module into UASM text, which the Udon assembler turns into
// a program a VRChat UdonBehaviour can run, plus the heap image that carries
// every constant the assembler's literal syntax cannot express.
//
//===----------------------------------------------------------------------===//

#include "UDONTargetMachine.h"
#include "UDONWriter.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/IRReader/IRReader.h"
#include "llvm/InitializePasses.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/PassRegistry.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/InitLLVM.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Target/TargetMachine.h"
#include <cstdlib>
#include <memory>

using namespace llvm;

extern "C" void LLVMInitializeUDONTargetInfo();
extern "C" void LLVMInitializeUDONTarget();

/// Print a fatal diagnostic as one line and exit, with no crash banner, stack
/// dump or dialog.
///
/// The writer rejects a construct it cannot express through
/// report_fatal_error, whose default path also runs the interrupt handlers.
/// On Windows those put up a modal dialog, so a batch run stops until someone
/// clicks it, and on both hosts they print a backtrace of the compiler rather
/// than of the program being compiled. None of that helps the person reading
/// the message: the rejection names the symbol or type that caused it.
static void reportAsDiagnostic(void *, const char *Reason, bool) {
  errs() << "llvm-udon: error: " << Reason << "\n";
  exit(1);
}

static cl::opt<std::string> InputFilename(cl::Positional, cl::desc("<input>"),
                                          cl::init("-"));
static cl::opt<std::string> OutputFilename("o", cl::desc("Output filename"),
                                           cl::value_desc("filename"));

static cl::opt<uint64_t> MemoryBytes(
    "udon-memory-bytes",
    cl::desc("End of the globals image, i.e. the byte offset where the shadow "
             "stack begins"),
    cl::init(1u << 20));
static cl::opt<uint64_t> HeapBytes(
    "udon-heap-bytes",
    cl::desc("Bytes of malloc heap between the globals image and the shadow "
             "stack; 0 for a program that never calls malloc"),
    cl::init(0));
static cl::opt<uint64_t> ShadowStackBytes(
    "udon-shadow-stack-bytes", cl::desc("Bytes of alloca shadow stack"),
    cl::init(1u << 16));
static cl::opt<uint64_t> StackDepth(
    "udon-stack-depth", cl::desc("Entries per caller-save stack array"),
    cl::init(1024));
static cl::opt<uint64_t> HeapSlots(
    "udon-heap-slots",
    cl::desc("Heap slot count; 0 computes it from the module"),
    cl::init(0));
static cl::opt<bool> NoRuntime("udon-runtime-none",
                               cl::desc("Do not link the built-in runtime"),
                               cl::init(false));
static cl::opt<std::string> ManifestPath(
    "udon-manifest", cl::desc("Write the used-extern manifest here"),
    cl::value_desc("path"), cl::init(""));
static cl::opt<bool> ExportLabels(
    "udon-export-labels",
    cl::desc("Export every internal label with a __ prefix (test only)"),
    cl::init(false));
static cl::opt<bool> ExportMemory(
    "udon-export-memory",
    cl::desc("Export __mem itself so a host can read and write it by name"),
    cl::init(false));

/// Validate the size flags before anything is written, so a bad flag is one
/// line rather than a program that assembles and then misbehaves.
static bool validateOptions() {
  if (MemoryBytes == 0 || MemoryBytes % 16 != 0) {
    errs() << "llvm-udon: error: -udon-memory-bytes must be a positive "
              "multiple of 16, got "
           << MemoryBytes << "\n";
    return false;
  }
  if (HeapBytes % 16 != 0) {
    errs() << "llvm-udon: error: -udon-heap-bytes must be a multiple of 16, "
              "got "
           << HeapBytes << "\n";
    return false;
  }
  if (ShadowStackBytes == 0 || ShadowStackBytes % 16 != 0) {
    errs() << "llvm-udon: error: -udon-shadow-stack-bytes must be a positive "
              "multiple of 16, got "
           << ShadowStackBytes << "\n";
    return false;
  }
  if (StackDepth == 0) {
    errs() << "llvm-udon: error: -udon-stack-depth must be positive\n";
    return false;
  }
  return true;
}

int main(int argc, char **argv) {
  InitLLVM X(argc, argv);
  LLVMInitializeUDONTargetInfo();
  LLVMInitializeUDONTarget();

  PassRegistry &Registry = *PassRegistry::getPassRegistry();
  initializeCore(Registry);
  initializeAnalysis(Registry);
  initializeCodeGen(Registry);
  initializeScalarOpts(Registry);
  initializeTransformUtils(Registry);
  initializeTarget(Registry);

  cl::ParseCommandLineOptions(argc, argv, "LLVM to VRChat Udon translator\n");
  install_fatal_error_handler(reportAsDiagnostic);

  if (!validateOptions())
    return 1;

  LLVMContext Context;
  SMDiagnostic Err;
  std::unique_ptr<Module> M = parseIRFile(InputFilename, Err, Context);
  if (!M) {
    Err.print(argv[0], errs());
    return 1;
  }

  // Every memory decode goes through BitConverter, which reads host-order
  // bytes, and every pointer is a byte offset. A module built for a
  // big-endian or 64-bit-pointer target would silently miscompile, so check
  // rather than trust.
  std::string Error;
  const Target *TheTarget = TargetRegistry::lookupTarget(Triple("udon"), Error);
  if (!TheTarget) {
    errs() << argv[0] << ": " << Error << "\n";
    return 1;
  }
  std::unique_ptr<TargetMachine> Probe(TheTarget->createTargetMachine(
      Triple(M->getTargetTriple()), "", "", TargetOptions(), std::nullopt));
  if (!Probe) {
    errs() << argv[0] << ": could not create the UDON target machine\n";
    return 1;
  }

  if (!M->getDataLayoutStr().empty()) {
    const DataLayout &Layout = M->getDataLayout();
    if (!Layout.isLittleEndian() || Layout.getPointerSizeInBits() != 32 ||
        Layout.getTypeSizeInBits(Type::getInt64Ty(Context)) != 64) {
      errs() << "llvm-udon: error: module data layout is "
             << M->getDataLayoutStr()
             << ", expected little-endian with 32-bit pointers; compile for "
                "wasm32 or a 32-bit target\n";
      return 1;
    }
  }

  // Adopt this target's layout unconditionally. A frontend's 32-bit layout
  // agrees with it on everything the writer depends on -- byte order, pointer
  // width, and the alignment of i32, i64, f32 and f64 -- but not on the entries
  // that only affect IR the frontend has already generated (native integer
  // widths, x87 long double, stack alignment). Keeping the frontend's would
  // make the runtime module's layout differ from the program's, which the linker
  // reports as a mismatch on every compile.
  M->setDataLayout(Probe->createDataLayout());

  // The registry's constructor signature is fixed, so the writer options go
  // through a process-wide accessor instead.
  UDONOptions &Opts = udonWriterOptions();
  Opts.MemoryBytes = MemoryBytes;
  Opts.HeapBytes = HeapBytes;
  Opts.ShadowStackBytes = ShadowStackBytes;
  Opts.StackDepth = StackDepth;
  Opts.HeapSlots = HeapSlots;
  Opts.LinkRuntime = !NoRuntime;
  Opts.ExportLabels = ExportLabels;
  Opts.ExportMemory = ExportMemory;
  Opts.ManifestPath = ManifestPath;

  // The runtime IR lives beside the executable in a build tree and beside the
  // source in a checkout, so both are tried before giving up.
  if (Opts.LinkRuntime) {
    SmallString<256> ExeDir(sys::path::filename(
        sys::fs::getMainExecutable(argv[0], nullptr)));
    SmallString<256> Beside(
        sys::path::parent_path(sys::fs::getMainExecutable(argv[0], nullptr)));
    SmallVector<SmallString<256>, 3> Candidates;
    Candidates.push_back(Beside);
    // From build/tools/llvm-udon, four levels up reaches the project root;
    // from a checkout's bin/ directory it is the same depth.
    for (const char *Rel :
         {"../../../../lib/Target/UDON/runtime/udon_rt.ll",
          "../../../lib/Target/UDON/runtime/udon_rt.ll",
          "../../lib/Target/UDON/runtime/udon_rt.ll",
          "../lib/Target/UDON/runtime/udon_rt.ll"}) {
      SmallString<256> P(Beside);
      sys::path::append(P, Rel);
      Candidates.push_back(P);
    }
    for (SmallString<256> &C : Candidates) {
      SmallString<256> Full(C);
      if (sys::path::filename(Full) != "udon_rt.ll") {
        Full = C;
        sys::path::append(Full, "udon_rt.ll");
      }
      if (sys::fs::exists(Full)) {
        Opts.RuntimePath = Full.str().str();
        break;
      }
    }
    if (Opts.RuntimePath.empty())
      errs() << "llvm-udon: warning: udon_rt.ll not found; bit-counting and "
                "non-zero memset will fail to link\n";
    (void)ExeDir;
  }

  std::unique_ptr<TargetMachine> TM(TheTarget->createTargetMachine(
      Triple(M->getTargetTriple()), "", "", TargetOptions(), std::nullopt));
  if (!TM) {
    errs() << argv[0] << ": could not create the UDON target machine\n";
    return 1;
  }

  if (OutputFilename.empty()) {
    if (InputFilename == "-")
      OutputFilename = "-";
    else {
      OutputFilename = InputFilename;
      for (StringRef Ext : {".bc", ".ll"})
        if (StringRef(OutputFilename).ends_with(Ext))
          OutputFilename.resize(OutputFilename.size() - Ext.size());
      OutputFilename += ".uasm";
    }
  }

  std::error_code EC;
  ToolOutputFile Out(OutputFilename, EC, sys::fs::OF_Text);
  if (EC) {
    errs() << argv[0] << ": " << EC.message() << "\n";
    return 1;
  }

  legacy::PassManager PM;
  if (TM->addPassesToEmitFile(PM, Out.os(), nullptr,
                              CodeGenFileType::AssemblyFile)) {
    errs() << argv[0] << ": UDON cannot emit this file type\n";
    return 1;
  }
  PM.run(*M);

  auto *UDON = static_cast<UDONTargetMachine *>(TM.get());

  // A caller that asked for a slot count must not be given fewer than the
  // module needs: the assembler would reject the first address past the cap.
  if (HeapSlots != 0 && HeapSlots < UDON->slotCount()) {
    errs() << "llvm-udon: error: -udon-heap-slots=" << HeapSlots
           << " is below the " << UDON->slotCount()
           << " slots the module needs\n";
    return 1;
  }

  Out.keep();

  // The heap image goes beside the `.uasm`, named after it. `-` output has no
  // directory to write into, so it gets none.
  if (OutputFilename != "-") {
    SmallString<256> HeapPath(OutputFilename);
    for (StringRef Ext : {".uasm"})
      if (StringRef(HeapPath).ends_with(Ext))
        HeapPath.resize(HeapPath.size() - Ext.size());
    HeapPath += ".udonheap.json";
    std::error_code HeapEC;
    raw_fd_ostream HeapOut(HeapPath, HeapEC, sys::fs::OF_Text);
    if (HeapEC) {
      errs() << "llvm-udon: error: cannot write " << HeapPath << ": "
             << HeapEC.message() << "\n";
      return 1;
    }
    // The image records the slot count too, so the importer and the harness
    // size the heap the same way the writer did.
    HeapOut << "{\"heapSlots\":" << (HeapSlots ? HeapSlots : UDON->slotCount())
            << ",\"slots\":" << UDON->heapImage() << "}\n";
  }

  return 0;
}
