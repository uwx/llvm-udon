//===-- UDONTypedArrays.cpp - typed float-array partition ------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A float-array global can leave `__mem` and become a `%SystemSingleArray`
// slot when every pointer derived from it is provably an ELEMENT INDEX the
// writer can lower against the array. The playground's geometry buffer is
// the motivating case: `tick_once` stores `@ud_geo` and three constant
// offsets into a stack struct, `sm64_mario_tick` forwards it to
// `gfx_adapter_bind_output_buffers`, which loads the four fields into
// cursor globals that then walk the array (`s_trianglePtr += 3`).
//
// The analysis has two outputs:
//
//   SafeTypedArrays   - arrays whose every derived-pointer use is modelled.
//                       `classifyFloatArrays` claims only members of this set.
//   TypedPointers      - pointer globals that provably denote one safe array.
//                       Their word becomes an element index, and a float
//                       load/store through them is one `SingleArray.__Get__`
//                       or `__Set__` instead of a bitcast pair plus a word
//                       read-modify-write.
//
// Anything unmodelled POISONS the array: a byte-typed load/store through a
// derived pointer, a derived pointer passed to a callee that was not itself
// provenanced end to end, or an instruction the writer cannot express
// against the array. Poisoned arrays stay byte-addressed, which is always
// sound -- the caller falls back to the `__mem` path.
//
//===----------------------------------------------------------------------===//

#include "UDONWriter.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Operator.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

namespace {

/// The array a pointer value denotes, plus the constant part of its element
/// index. The variable part of an index is never tracked here: the walk and
/// the GEP arithmetic keep it in the same slot.
struct ElemIndex {
  const GlobalVariable *Array = nullptr;
  int64_t Index = 0;
};

/// True when `V` denotes one element of one typed array. Constant GEPs fold
/// into the index; `Known` carries values proven earlier in the walk.
bool indexOf(const Value *V, const DenseMap<const Value *, ElemIndex> &Known,
             const DenseSet<const GlobalVariable *> &FloatArrays,
             DenseSet<const Value *> &IdxVals, const DataLayout &DL,
             ElemIndex &Out) {
  auto It = Known.find(V);
  if (It != Known.end()) {
    Out = It->second;
    return true;
  }
  if (auto *G = dyn_cast<GlobalVariable>(V)) {
    if (!FloatArrays.count(G))
      return false;
    Out = {G, 0};
    return true;
  }
  // A `ptrtoint` of an array-derived pointer is its ELEMENT INDEX (see
  // analyzeTypedPointers, where the value is recorded).
  if (auto *PTI = dyn_cast<PtrToIntOperator>(V))
    if (indexOf(PTI->getOperand(0), Known, FloatArrays, IdxVals, DL, Out)) {
      IdxVals.insert(V);
      return true;
    }
  // A `sub` of two element indices is an element difference, and `trunc`/
  // `ashr exact` by 2 of it degenerates to a copy: the IR believes the
  // difference is in bytes.
  if (auto *BO = dyn_cast<BinaryOperator>(V))
    if (BO->getOpcode() == Instruction::Sub &&
        IdxVals.count(BO->getOperand(0)) &&
        IdxVals.count(BO->getOperand(1))) {
      Out = {nullptr, 0};
      IdxVals.insert(V);
      return true;
    }
  // A GEP, instruction or constant-expression. Inlining rewrites
  // `getelementptr float, ptr %p, i32 3` into
  // `getelementptr i8, ptr %p, i32 12`, so the byte offset is what has to be
  // divided by the element size -- requiring a float-typed result misses
  // every GEP the inliner touched.
  if (auto *GEP = dyn_cast<GEPOperator>(V)) {
    ElemIndex Base;
    if (!indexOf(GEP->getPointerOperand(), Known, FloatArrays, IdxVals, DL, Base))
      return false;
    // Only constant offsets fold here; a variable index is a runtime slot.
    APInt Off(64, 0);
    if (!GEP->accumulateConstantOffset(DL, Off))
      return false;
    Out = {Base.Array, Base.Index + Off.getSExtValue() / 4};
    return true;
  }
  return false;
}

/// The byte offset of a constant-index struct/array GEP element, or false.
/// A GEP's source type is not required to be sized, and its index can name a
/// field the struct does not have (the playground module has 116 `[0 x T]`
// GEPs), so both are checked rather than dereferenced.
bool fieldByteOffset(const DataLayout &DL, const GetElementPtrInst &GEP,
                     int64_t &Out) {
  auto *CI = dyn_cast<ConstantInt>(GEP.getOperand(GEP.getNumOperands() - 1));
  if (!CI)
    return false;
  if (auto *ST = dyn_cast<StructType>(GEP.getSourceElementType())) {
    const StructLayout *SL = DL.getStructLayout(ST);
    unsigned Field = static_cast<unsigned>(CI->getZExtValue());
    if (!SL || Field >= ST->getNumElements())
      return false;
    TypeSize Off = SL->getElementOffset(Field);
    if (Off.isScalable())
      return false;
    Out = Off.getFixedValue();
    return true;
  }
  if (auto *AT = dyn_cast<ArrayType>(GEP.getSourceElementType())) {
    TypeSize Sz = DL.getTypeAllocSize(AT->getElementType());
    if (Sz.isScalable())
      return false;
    Out = Sz.getFixedValue() * CI->getSExtValue();
    return true;
  }
  return false;
}

} // namespace

void UDONWriter::analyzeTypedPointers() {
  // Every flat float-array global is a candidate; `classifyFloatArrays` (and
  // the poison audit below) decides which ones are claimed.
  DenseSet<const GlobalVariable *> FloatArrays;
  for (GlobalVariable &G : Mod->globals()) {
    auto *AT = dyn_cast<ArrayType>(G.getValueType());
    if (AT && AT->getElementType()->isFloatTy())
      FloatArrays.insert(&G);
  }

  // Argument provenance, keyed by (argument index, field byte offset).
  // -1 is the whole pointer; >= 0 is a struct field at that offset.
  using ArgMap = DenseMap<std::pair<unsigned, int64_t>, ElemIndex>;
  DenseMap<const Function *, ArgMap> ArgIn;

  // Struct-pointer provenance round-tripped through a holder global:
  // `store ptr %s, ptr @holder` then `load ptr, ptr @holder` gives back a
  // pointer whose fields carry provenance (`s_outBuffers`).
  DenseMap<const GlobalVariable *, DenseMap<int64_t, ElemIndex>> HeldFields;

  // Recomputed every pass: a use is unmodelled only given what the walk has
  // proven so far, so an early pass poisons values a later pass can handle.
  // The last pass's set is the one computed with the most complete knowledge.
  DenseSet<const GlobalVariable *> Poisoned;

  const unsigned MaxPasses = 8;
  for (unsigned Pass = 0; Pass < MaxPasses; Pass++) {
    DenseMap<const Function *, ArgMap> NewArgIn;
    DenseMap<const GlobalVariable *, const GlobalVariable *> NewProv;
    DenseSet<const GlobalVariable *> PassPoisoned;
    DenseMap<const GlobalVariable *, DenseMap<int64_t, ElemIndex>>
        NewHeldFields;
    bool Changed = false;

    for (Function &F : *Mod) {
      if (F.isDeclaration())
        continue;
      DenseMap<const Value *, ElemIndex> Known;
      DenseMap<const Value *, DenseMap<int64_t, ElemIndex>> Fields;

      auto ArgIt = ArgIn.find(&F);
      if (ArgIt != ArgIn.end())
        for (auto &[Key, Idx] : ArgIt->second) {
          if (Key.first >= F.arg_size() || !Idx.Array)
            continue;
          Argument *A = F.getArg(Key.first);
          if (!A)
            continue;
          if (Key.second < 0)
            Known[A] = Idx;
          else
            Fields[A][Key.second] = Idx;
        }

      for (BasicBlock &BB : F)
        for (Instruction &I : BB) {
          // --- stores ---------------------------------------------------
          if (auto *SI = dyn_cast<StoreInst>(&I)) {
            // A non-float, non-pointer store THROUGH an array-derived pointer
            // is a byte access: the array no longer lives in `__mem`.
            if (!SI->getValueOperand()->getType()->isFloatTy() &&
                !SI->getValueOperand()->getType()->isPointerTy()) {
              ElemIndex PIdx;
              if (indexOf(SI->getPointerOperand(), Known, FloatArrays, ElementIndexValues, *DL, PIdx))
                PassPoisoned.insert(PIdx.Array);
            }
            ElemIndex Idx;
            // A struct pointer stored into a holder global transfers its
            // Fields even when the pointer itself denotes no array
            // (`s_outBuffers = outBuffers`).
            if (auto *HG = dyn_cast<GlobalVariable>(SI->getPointerOperand())) {
              auto VF = Fields.find(SI->getValueOperand());
              if (VF != Fields.end())
                for (auto &[Off, FIdx] : VF->second)
                  if (FIdx.Array)
                    NewHeldFields[HG][Off] = FIdx;
            }
            if (!indexOf(SI->getValueOperand(), Known, FloatArrays,
                         ElementIndexValues, *DL, Idx))
              continue;
            Value *Ptr = SI->getPointerOperand();
            if (auto *GEP = dyn_cast<GetElementPtrInst>(Ptr)) {
              int64_t Off;
              if (!fieldByteOffset(*DL, *GEP, Off))
                continue;
              auto &Slot = Fields[GEP->getPointerOperand()][Off];
              if (!Slot.Array)
                Slot = Idx;
              else if (Slot.Array != Idx.Array)
                Slot.Array = nullptr;
              continue;
            }
            // A store straight to the base is field 0: `-O` folds
            // `&s.field[0]` into a plain store to `s`.
            if (Ptr->getType()->isPointerTy() && !isa<GlobalVariable>(Ptr)) {
              auto &Slot = Fields[Ptr][0];
              if (!Slot.Array)
                Slot = Idx;
              else if (Slot.Array != Idx.Array)
                Slot.Array = nullptr;
              continue;
            }
            if (auto *G = dyn_cast<GlobalVariable>(Ptr)) {
              // A struct pointer stored into a holder global: the struct's
              // Fields become the holder's, so a later
              // `load holder; load field` keeps provenance
              // (`s_outBuffers` -> `->position`).
              auto VF = Fields.find(SI->getValueOperand());
              if (VF != Fields.end())
                for (auto &[Off, FIdx] : VF->second)
                  if (FIdx.Array)
                    NewHeldFields[G][Off] = FIdx;
              auto [It, Inserted] = NewProv.try_emplace(G, Idx.Array);
              if (!Inserted && It->second != Idx.Array)
                It->second = nullptr;
              Changed = true;
            }
            continue;
          }
          // --- loads ----------------------------------------------------
          if (auto *LI = dyn_cast<LoadInst>(&I)) {
            // A non-float, non-pointer load THROUGH an array-derived pointer
            // is a byte access: poison.
            if (!LI->getType()->isFloatTy() &&
                !LI->getType()->isPointerTy()) {
              ElemIndex PIdx;
              if (indexOf(LI->getPointerOperand(), Known, FloatArrays, ElementIndexValues, *DL, PIdx))
                PassPoisoned.insert(PIdx.Array);
            }
            if (!LI->getType()->isPointerTy())
              continue;
            const Value *Ptr = LI->getPointerOperand();
            // A GEP'd field load.
            if (auto *GEP = dyn_cast<GetElementPtrInst>(Ptr)) {
              int64_t Off;
              if (fieldByteOffset(*DL, *GEP, Off)) {
                const Value *Base = GEP->getPointerOperand();
                auto Fit = Fields.find(Base);
                if (Fit != Fields.end()) {
                  auto FIt = Fit->second.find(Off);
                  if (FIt != Fit->second.end() && FIt->second.Array) {
                    Known[&I] = FIt->second;
                    ProvenancedLoads[&I] = FIt->second.Array;
                    continue;
                  }
                }
                if (auto *A = dyn_cast<Argument>(Base)) {
                  auto AIt = ArgIn.find(&F);
                  if (AIt != ArgIn.end()) {
                    auto KIt = AIt->second.find({A->getArgNo(), Off});
                    if (KIt != AIt->second.end() && KIt->second.Array) {
                      Known[&I] = KIt->second;
                      ProvenancedLoads[&I] = KIt->second.Array;
                      continue;
                    }
                  }
                }
              }
            }
            // A load straight from the base is field 0.
            if (!isa<GlobalVariable>(Ptr)) {
              auto Fit = Fields.find(Ptr);
              if (Fit != Fields.end()) {
                auto FIt = Fit->second.find(0);
                if (FIt != Fit->second.end() && FIt->second.Array) {
                  Known[&I] = FIt->second;
                  ProvenancedLoads[&I] = FIt->second.Array;
                  continue;
                }
              }
              if (auto *A = dyn_cast<Argument>(Ptr)) {
                auto AIt = ArgIn.find(&F);
                if (AIt != ArgIn.end()) {
                  auto KIt = AIt->second.find({A->getArgNo(), 0});
                  if (KIt != AIt->second.end() && KIt->second.Array) {
                    Known[&I] = KIt->second;
                    ProvenancedLoads[&I] = KIt->second.Array;
                    continue;
                  }
                }
              }
            }
            // A load of a provenanced pointer global carries the array on.
            if (auto *G = dyn_cast<GlobalVariable>(Ptr)) {
              auto It = TypedPointers.find(G);
              if (It != TypedPointers.end() && It->second)
                Known[&I] = {It->second, 0};
              // A holder global yields a struct pointer whose fields carry
              // provenance: copy them into this function's Fields.
              auto HF = HeldFields.find(G);
              if (HF != HeldFields.end()) {
                auto &MyFields = Fields[&I];
                for (auto &[Off, Idx2] : HF->second)
                  MyFields[Off] = Idx2;
              }
            }
            continue;
          }
          // --- calls ----------------------------------------------------
          if (auto *CB = dyn_cast<CallBase>(&I)) {
            const Function *Callee = CB->getCalledFunction();
            for (unsigned A = 0; A < CB->arg_size(); A++) {
              const Value *Arg = CB->getArgOperand(A);
              ElemIndex Idx;
              if (indexOf(Arg, Known, FloatArrays, ElementIndexValues, *DL, Idx) && Idx.Array) {
                // The whole array pointer passed to a callee is modelled
                // only through the struct-field path; a direct pass is a
                // `memset(ud_input, ...)`-style byte use.
                PassPoisoned.insert(Idx.Array);
                continue;
              }
              auto It = Known.find(Arg);
              if (It != Known.end() && It->second.Array) {
                // A cursor index handed to a callee that will treat it as a
                // byte address (`vec3f_normalize(s_normalPtr)`): poison.
                PassPoisoned.insert(It->second.Array);
                continue;
              }
              if (auto *G = dyn_cast<GlobalVariable>(Arg)) {
                auto TP = TypedPointers.find(G);
                if (TP != TypedPointers.end() && TP->second) {
                  // The cursor GLOBAL passed by value is the same hazard.
                  PassPoisoned.insert(TP->second);
                  continue;
                }
              }
            }
            if (!Callee || Callee->isDeclaration())
              continue;
              for (unsigned A = 0; A < CB->arg_size(); A++)
            // Propagate: a direct array-pointer argument, and each field of
            // a struct-pointer argument whose provenance this function knows.
            for (unsigned A = 0; A < CB->arg_size(); A++) {
              const Value *Arg = CB->getArgOperand(A);
              ElemIndex Idx;
              if (indexOf(Arg, Known, FloatArrays, ElementIndexValues, *DL, Idx) && Idx.Array) {
                PassPoisoned.erase(Idx.Array); // whole-pointer path: modelled
                auto &Slot = NewArgIn[Callee][{A, -1}];
                if (!Slot.Array)
                  Slot = Idx;
                else if (Slot.Array != Idx.Array)
                  Slot.Array = nullptr;
                Changed = true;
                continue;
              }
              auto Fit = Fields.find(Arg);
              if (Fit != Fields.end())
                for (auto &[Off, FIdx] : Fit->second) {
                  if (!FIdx.Array)
                    continue;
                  auto &Slot = NewArgIn[Callee][{A, Off}];
                  if (!Slot.Array)
                    Slot = FIdx;
                  else if (Slot.Array != FIdx.Array)
                    Slot.Array = nullptr;
                  Changed = true;
                }
            }
          }
          // --- anything else ----------------------------------------------
          // An instruction consuming an array-derived pointer that is not a
          // GEP, load, store or call cannot be expressed against the array.
          // A `sub` of two element indices is element-wise and modelled.
          // Probing each operand first is what records a `ptrtoint` operand
          // into `ElementIndexValues`; checking the set before probing sees it
          // empty and poisons on the pass the ptrtoint is first proven.
          bool IndexSub = false;
          if (auto *BO = dyn_cast<BinaryOperator>(&I))
            if (BO->getOpcode() == Instruction::Sub) {
              ElemIndex A, B;
              bool HaveA = indexOf(BO->getOperand(0), Known, FloatArrays,
                                   ElementIndexValues, *DL, A);
              bool HaveB = indexOf(BO->getOperand(1), Known, FloatArrays,
                                   ElementIndexValues, *DL, B);
              IndexSub = HaveA && HaveB &&
                         ElementIndexValues.count(BO->getOperand(0)) &&
                         ElementIndexValues.count(BO->getOperand(1));
            }
          if (!isa<GetElementPtrInst>(&I) && !isa<LoadInst>(&I) &&
              !isa<StoreInst>(&I) && !isa<CallBase>(&I) &&
              !isa<BitCastInst>(&I) && !isa<PtrToIntInst>(&I) &&
              !isa<PtrToIntOperator>(&I) && !IndexSub)
            for (const Use &Op : I.operands()) {
              ElemIndex Idx;
              if (indexOf(Op.get(), Known, FloatArrays, ElementIndexValues, *DL, Idx) && Idx.Array) {
                PassPoisoned.insert(Idx.Array);
              }
            }
        }
    }

    // Merge this pass's global stores; a conflict (two arrays) goes to null.
    for (auto &[G, Array] : NewProv) {
      if (!Array)
        continue;
      auto [It, Inserted] = TypedPointers.try_emplace(G, Array);
      if (!Inserted && It->second != Array) {
        It->second = nullptr;
        Changed = true;
      }
    }

    for (auto &[G, FM] : NewHeldFields)
      for (auto &[Off, Idx2] : FM)
        if (Idx2.Array) {
          HeldFields[G][Off] = Idx2;
          Changed = true;
        }
    ArgIn = std::move(NewArgIn);
    Poisoned = std::move(PassPoisoned);
    if (!Changed)
      break;
  }

  // Drop conflicted pointer globals and poisoned arrays.
  SmallVector<const GlobalVariable *, 8> Conflicted;
  for (auto &[G, Array] : TypedPointers)
    if (!Array)
      Conflicted.push_back(G);
  for (const GlobalVariable *G : Conflicted)
    TypedPointers.erase(G);
  for (const GlobalVariable *G : Poisoned)
    TypedPointers.erase(G);

  // The safe set: candidates minus the poisoned. An array nobody derived a
  // pointer from is safe too -- plain float GEP stores need no provenance.
  if (!getenv("UDON_NO_TYPED"))
    for (const GlobalVariable *G : FloatArrays)
      if (!Poisoned.count(G))
        SafeTypedArrays.insert(G);

}
