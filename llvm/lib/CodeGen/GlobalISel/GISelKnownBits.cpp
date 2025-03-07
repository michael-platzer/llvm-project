//===- lib/CodeGen/GlobalISel/GISelKnownBits.cpp --------------*- C++ *-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
/// Provides analysis for querying information about KnownBits during GISel
/// passes.
//
//===------------------
#include "llvm/CodeGen/GlobalISel/GISelKnownBits.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/CodeGen/GlobalISel/Utils.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/TargetLowering.h"
#include "llvm/CodeGen/TargetOpcodes.h"

#define DEBUG_TYPE "gisel-known-bits"

using namespace llvm;

char llvm::GISelKnownBitsAnalysis::ID = 0;

INITIALIZE_PASS(GISelKnownBitsAnalysis, DEBUG_TYPE,
                "Analysis for ComputingKnownBits", false, true)

GISelKnownBits::GISelKnownBits(MachineFunction &MF, unsigned MaxDepth)
    : MF(MF), MRI(MF.getRegInfo()), TL(*MF.getSubtarget().getTargetLowering()),
      DL(MF.getFunction().getParent()->getDataLayout()), MaxDepth(MaxDepth) {}

Align GISelKnownBits::computeKnownAlignment(Register R, unsigned Depth) {
  const MachineInstr *MI = MRI.getVRegDef(R);
  switch (MI->getOpcode()) {
  case TargetOpcode::COPY:
    return computeKnownAlignment(MI->getOperand(1).getReg(), Depth);
  case TargetOpcode::G_FRAME_INDEX: {
    int FrameIdx = MI->getOperand(1).getIndex();
    return MF.getFrameInfo().getObjectAlign(FrameIdx);
  }
  case TargetOpcode::G_INTRINSIC:
  case TargetOpcode::G_INTRINSIC_W_SIDE_EFFECTS:
  default:
    return TL.computeKnownAlignForTargetInstr(*this, R, MRI, Depth + 1);
  }
}

KnownBits GISelKnownBits::getKnownBits(MachineInstr &MI) {
  assert(MI.getNumExplicitDefs() == 1 &&
         "expected single return generic instruction");
  return getKnownBits(MI.getOperand(0).getReg());
}

KnownBits GISelKnownBits::getKnownBits(Register R) {
  const LLT Ty = MRI.getType(R);
  APInt DemandedElts =
      Ty.isVector() ? APInt::getAllOnesValue(Ty.getNumElements()) : APInt(1, 1);
  return getKnownBits(R, DemandedElts);
}

KnownBits GISelKnownBits::getKnownBits(Register R, const APInt &DemandedElts,
                                       unsigned Depth) {
  // For now, we only maintain the cache during one request.
  assert(ComputeKnownBitsCache.empty() && "Cache should have been cleared");

  KnownBits Known;
  computeKnownBitsImpl(R, Known, DemandedElts);
  ComputeKnownBitsCache.clear();
  return Known;
}

bool GISelKnownBits::signBitIsZero(Register R) {
  LLT Ty = MRI.getType(R);
  unsigned BitWidth = Ty.getScalarSizeInBits();
  return maskedValueIsZero(R, APInt::getSignMask(BitWidth));
}

APInt GISelKnownBits::getKnownZeroes(Register R) {
  return getKnownBits(R).Zero;
}

APInt GISelKnownBits::getKnownOnes(Register R) { return getKnownBits(R).One; }

LLVM_ATTRIBUTE_UNUSED static void
dumpResult(const MachineInstr &MI, const KnownBits &Known, unsigned Depth) {
  dbgs() << "[" << Depth << "] Compute known bits: " << MI << "[" << Depth
         << "] Computed for: " << MI << "[" << Depth << "] Known: 0x"
         << (Known.Zero | Known.One).toString(16, false) << "\n"
         << "[" << Depth << "] Zero: 0x" << Known.Zero.toString(16, false)
         << "\n"
         << "[" << Depth << "] One:  0x" << Known.One.toString(16, false)
         << "\n";
}

/// Compute known bits for the intersection of \p Src0 and \p Src1
void GISelKnownBits::computeKnownBitsMin(Register Src0, Register Src1,
                                         KnownBits &Known,
                                         const APInt &DemandedElts,
                                         unsigned Depth) {
  // Test src1 first, since we canonicalize simpler expressions to the RHS.
  computeKnownBitsImpl(Src1, Known, DemandedElts, Depth);

  // If we don't know any bits, early out.
  if (Known.isUnknown())
    return;

  KnownBits Known2;
  computeKnownBitsImpl(Src0, Known2, DemandedElts, Depth);

  // Only known if known in both the LHS and RHS.
  Known.Zero &= Known2.Zero;
  Known.One &= Known2.One;
}

void GISelKnownBits::computeKnownBitsImpl(Register R, KnownBits &Known,
                                          const APInt &DemandedElts,
                                          unsigned Depth) {
  MachineInstr &MI = *MRI.getVRegDef(R);
  unsigned Opcode = MI.getOpcode();
  LLT DstTy = MRI.getType(R);

  // Handle the case where this is called on a register that does not have a
  // type constraint (i.e. it has a register class constraint instead). This is
  // unlikely to occur except by looking through copies but it is possible for
  // the initial register being queried to be in this state.
  if (!DstTy.isValid()) {
    Known = KnownBits();
    return;
  }

  unsigned BitWidth = DstTy.getSizeInBits();
  auto CacheEntry = ComputeKnownBitsCache.find(R);
  if (CacheEntry != ComputeKnownBitsCache.end()) {
    Known = CacheEntry->second;
    LLVM_DEBUG(dbgs() << "Cache hit at ");
    LLVM_DEBUG(dumpResult(MI, Known, Depth));
    assert(Known.getBitWidth() == BitWidth && "Cache entry size doesn't match");
    return;
  }
  Known = KnownBits(BitWidth); // Don't know anything

  if (DstTy.isVector())
    return; // TODO: Handle vectors.

  // Depth may get bigger than max depth if it gets passed to a different
  // GISelKnownBits object.
  // This may happen when say a generic part uses a GISelKnownBits object
  // with some max depth, but then we hit TL.computeKnownBitsForTargetInstr
  // which creates a new GISelKnownBits object with a different and smaller
  // depth. If we just check for equality, we would never exit if the depth
  // that is passed down to the target specific GISelKnownBits object is
  // already bigger than its max depth.
  if (Depth >= getMaxDepth())
    return;

  if (!DemandedElts)
    return; // No demanded elts, better to assume we don't know anything.

  KnownBits Known2;

  switch (Opcode) {
  default:
    TL.computeKnownBitsForTargetInstr(*this, R, Known, DemandedElts, MRI,
                                      Depth);
    break;
  case TargetOpcode::COPY:
  case TargetOpcode::G_PHI:
  case TargetOpcode::PHI: {
    Known.One = APInt::getAllOnesValue(BitWidth);
    Known.Zero = APInt::getAllOnesValue(BitWidth);
    // Destination registers should not have subregisters at this
    // point of the pipeline, otherwise the main live-range will be
    // defined more than once, which is against SSA.
    assert(MI.getOperand(0).getSubReg() == 0 && "Is this code in SSA?");
    // Record in the cache that we know nothing for MI.
    // This will get updated later and in the meantime, if we reach that
    // phi again, because of a loop, we will cut the search thanks to this
    // cache entry.
    // We could actually build up more information on the phi by not cutting
    // the search, but that additional information is more a side effect
    // than an intended choice.
    // Therefore, for now, save on compile time until we derive a proper way
    // to derive known bits for PHIs within loops.
    ComputeKnownBitsCache[R] = KnownBits(BitWidth);
    // PHI's operand are a mix of registers and basic blocks interleaved.
    // We only care about the register ones.
    for (unsigned Idx = 1; Idx < MI.getNumOperands(); Idx += 2) {
      const MachineOperand &Src = MI.getOperand(Idx);
      Register SrcReg = Src.getReg();
      // Look through trivial copies and phis but don't look through trivial
      // copies or phis of the form `%1:(s32) = OP %0:gpr32`, known-bits
      // analysis is currently unable to determine the bit width of a
      // register class.
      //
      // We can't use NoSubRegister by name as it's defined by each target but
      // it's always defined to be 0 by tablegen.
      if (SrcReg.isVirtual() && Src.getSubReg() == 0 /*NoSubRegister*/ &&
          MRI.getType(SrcReg).isValid()) {
        // For COPYs we don't do anything, don't increase the depth.
        computeKnownBitsImpl(SrcReg, Known2, DemandedElts,
                             Depth + (Opcode != TargetOpcode::COPY));
        Known.One &= Known2.One;
        Known.Zero &= Known2.Zero;
        // If we reach a point where we don't know anything
        // just stop looking through the operands.
        if (Known.One == 0 && Known.Zero == 0)
          break;
      } else {
        // We know nothing.
        Known = KnownBits(BitWidth);
        break;
      }
    }
    break;
  }
  case TargetOpcode::G_CONSTANT: {
    auto CstVal = getConstantVRegVal(R, MRI);
    if (!CstVal)
      break;
    Known.One = *CstVal;
    Known.Zero = ~Known.One;
    break;
  }
  case TargetOpcode::G_FRAME_INDEX: {
    int FrameIdx = MI.getOperand(1).getIndex();
    TL.computeKnownBitsForFrameIndex(FrameIdx, Known, MF);
    break;
  }
  case TargetOpcode::G_SUB: {
    computeKnownBitsImpl(MI.getOperand(1).getReg(), Known, DemandedElts,
                         Depth + 1);
    computeKnownBitsImpl(MI.getOperand(2).getReg(), Known2, DemandedElts,
                         Depth + 1);
    Known = KnownBits::computeForAddSub(/*Add*/ false, /*NSW*/ false, Known,
                                        Known2);
    break;
  }
  case TargetOpcode::G_XOR: {
    computeKnownBitsImpl(MI.getOperand(2).getReg(), Known, DemandedElts,
                         Depth + 1);
    computeKnownBitsImpl(MI.getOperand(1).getReg(), Known2, DemandedElts,
                         Depth + 1);

    Known ^= Known2;
    break;
  }
  case TargetOpcode::G_PTR_ADD: {
    // G_PTR_ADD is like G_ADD. FIXME: Is this true for all targets?
    LLT Ty = MRI.getType(MI.getOperand(1).getReg());
    if (DL.isNonIntegralAddressSpace(Ty.getAddressSpace()))
      break;
    LLVM_FALLTHROUGH;
  }
  case TargetOpcode::G_ADD: {
    computeKnownBitsImpl(MI.getOperand(1).getReg(), Known, DemandedElts,
                         Depth + 1);
    computeKnownBitsImpl(MI.getOperand(2).getReg(), Known2, DemandedElts,
                         Depth + 1);
    Known =
        KnownBits::computeForAddSub(/*Add*/ true, /*NSW*/ false, Known, Known2);
    break;
  }
  case TargetOpcode::G_AND: {
    // If either the LHS or the RHS are Zero, the result is zero.
    computeKnownBitsImpl(MI.getOperand(2).getReg(), Known, DemandedElts,
                         Depth + 1);
    computeKnownBitsImpl(MI.getOperand(1).getReg(), Known2, DemandedElts,
                         Depth + 1);

    Known &= Known2;
    break;
  }
  case TargetOpcode::G_OR: {
    // If either the LHS or the RHS are Zero, the result is zero.
    computeKnownBitsImpl(MI.getOperand(2).getReg(), Known, DemandedElts,
                         Depth + 1);
    computeKnownBitsImpl(MI.getOperand(1).getReg(), Known2, DemandedElts,
                         Depth + 1);

    Known |= Known2;
    break;
  }
  case TargetOpcode::G_MUL: {
    computeKnownBitsImpl(MI.getOperand(2).getReg(), Known, DemandedElts,
                         Depth + 1);
    computeKnownBitsImpl(MI.getOperand(1).getReg(), Known2, DemandedElts,
                         Depth + 1);
    Known = KnownBits::computeForMul(Known, Known2);
    break;
  }
  case TargetOpcode::G_SELECT: {
    computeKnownBitsMin(MI.getOperand(2).getReg(), MI.getOperand(3).getReg(),
                        Known, DemandedElts, Depth + 1);
    break;
  }
  case TargetOpcode::G_SMIN: {
    // TODO: Handle clamp pattern with number of sign bits
    KnownBits KnownRHS;
    computeKnownBitsImpl(MI.getOperand(1).getReg(), Known, DemandedElts,
                         Depth + 1);
    computeKnownBitsImpl(MI.getOperand(2).getReg(), KnownRHS, DemandedElts,
                         Depth + 1);
    Known = KnownBits::smin(Known, KnownRHS);
    break;
  }
  case TargetOpcode::G_SMAX: {
    // TODO: Handle clamp pattern with number of sign bits
    KnownBits KnownRHS;
    computeKnownBitsImpl(MI.getOperand(1).getReg(), Known, DemandedElts,
                         Depth + 1);
    computeKnownBitsImpl(MI.getOperand(2).getReg(), KnownRHS, DemandedElts,
                         Depth + 1);
    Known = KnownBits::smax(Known, KnownRHS);
    break;
  }
  case TargetOpcode::G_UMIN: {
    KnownBits KnownRHS;
    computeKnownBitsImpl(MI.getOperand(1).getReg(), Known,
                         DemandedElts, Depth + 1);
    computeKnownBitsImpl(MI.getOperand(2).getReg(), KnownRHS,
                         DemandedElts, Depth + 1);
    Known = KnownBits::umin(Known, KnownRHS);
    break;
  }
  case TargetOpcode::G_UMAX: {
    KnownBits KnownRHS;
    computeKnownBitsImpl(MI.getOperand(1).getReg(), Known,
                         DemandedElts, Depth + 1);
    computeKnownBitsImpl(MI.getOperand(2).getReg(), KnownRHS,
                         DemandedElts, Depth + 1);
    Known = KnownBits::umax(Known, KnownRHS);
    break;
  }
  case TargetOpcode::G_FCMP:
  case TargetOpcode::G_ICMP: {
    if (TL.getBooleanContents(DstTy.isVector(),
                              Opcode == TargetOpcode::G_FCMP) ==
            TargetLowering::ZeroOrOneBooleanContent &&
        BitWidth > 1)
      Known.Zero.setBitsFrom(1);
    break;
  }
  case TargetOpcode::G_SEXT: {
    computeKnownBitsImpl(MI.getOperand(1).getReg(), Known, DemandedElts,
                         Depth + 1);
    // If the sign bit is known to be zero or one, then sext will extend
    // it to the top bits, else it will just zext.
    Known = Known.sext(BitWidth);
    break;
  }
  case TargetOpcode::G_ANYEXT: {
    computeKnownBitsImpl(MI.getOperand(1).getReg(), Known, DemandedElts,
                         Depth + 1);
    Known = Known.anyext(BitWidth);
    break;
  }
  case TargetOpcode::G_LOAD: {
    const MachineMemOperand *MMO = *MI.memoperands_begin();
    if (const MDNode *Ranges = MMO->getRanges()) {
      computeKnownBitsFromRangeMetadata(*Ranges, Known);
    }

    break;
  }
  case TargetOpcode::G_ZEXTLOAD: {
    // Everything above the retrieved bits is zero
    Known.Zero.setBitsFrom((*MI.memoperands_begin())->getSizeInBits());
    break;
  }
  case TargetOpcode::G_ASHR:
  case TargetOpcode::G_LSHR:
  case TargetOpcode::G_SHL: {
    KnownBits RHSKnown;
    computeKnownBitsImpl(MI.getOperand(2).getReg(), RHSKnown, DemandedElts,
                         Depth + 1);
    if (!RHSKnown.isConstant()) {
      LLVM_DEBUG(
          MachineInstr *RHSMI = MRI.getVRegDef(MI.getOperand(2).getReg());
          dbgs() << '[' << Depth << "] Shift not known constant: " << *RHSMI);
      break;
    }
    uint64_t Shift = RHSKnown.getConstant().getZExtValue();
    LLVM_DEBUG(dbgs() << '[' << Depth << "] Shift is " << Shift << '\n');

    // Guard against oversized shift amounts
    if (Shift >= MRI.getType(MI.getOperand(1).getReg()).getScalarSizeInBits())
      break;

    computeKnownBitsImpl(MI.getOperand(1).getReg(), Known, DemandedElts,
                         Depth + 1);

    switch (Opcode) {
    case TargetOpcode::G_ASHR:
      Known.Zero = Known.Zero.ashr(Shift);
      Known.One = Known.One.ashr(Shift);
      break;
    case TargetOpcode::G_LSHR:
      Known.Zero = Known.Zero.lshr(Shift);
      Known.One = Known.One.lshr(Shift);
      Known.Zero.setBitsFrom(Known.Zero.getBitWidth() - Shift);
      break;
    case TargetOpcode::G_SHL:
      Known.Zero = Known.Zero.shl(Shift);
      Known.One = Known.One.shl(Shift);
      Known.Zero.setBits(0, Shift);
      break;
    }
    break;
  }
  case TargetOpcode::G_INTTOPTR:
  case TargetOpcode::G_PTRTOINT:
    // Fall through and handle them the same as zext/trunc.
    LLVM_FALLTHROUGH;
  case TargetOpcode::G_ZEXT:
  case TargetOpcode::G_TRUNC: {
    Register SrcReg = MI.getOperand(1).getReg();
    LLT SrcTy = MRI.getType(SrcReg);
    unsigned SrcBitWidth = SrcTy.isPointer()
                               ? DL.getIndexSizeInBits(SrcTy.getAddressSpace())
                               : SrcTy.getSizeInBits();
    assert(SrcBitWidth && "SrcBitWidth can't be zero");
    Known = Known.zextOrTrunc(SrcBitWidth);
    computeKnownBitsImpl(SrcReg, Known, DemandedElts, Depth + 1);
    Known = Known.zextOrTrunc(BitWidth);
    if (BitWidth > SrcBitWidth)
      Known.Zero.setBitsFrom(SrcBitWidth);
    break;
  }
  case TargetOpcode::G_MERGE_VALUES: {
    Register NumOps = MI.getNumOperands();
    unsigned OpSize = MRI.getType(MI.getOperand(1).getReg()).getSizeInBits();

    for (unsigned I = 0; I != NumOps - 1; ++I) {
      KnownBits SrcOpKnown;
      computeKnownBitsImpl(MI.getOperand(I + 1).getReg(), SrcOpKnown,
                           DemandedElts, Depth + 1);
      Known.insertBits(SrcOpKnown, I * OpSize);
    }
    break;
  }
  case TargetOpcode::G_UNMERGE_VALUES: {
    Register NumOps = MI.getNumOperands();
    Register SrcReg = MI.getOperand(NumOps - 1).getReg();
    if (MRI.getType(SrcReg).isVector())
      return; // TODO: Handle vectors.

    KnownBits SrcOpKnown;
    computeKnownBitsImpl(SrcReg, SrcOpKnown, DemandedElts, Depth + 1);

    // Figure out the result operand index
    unsigned DstIdx = 0;
    for (; DstIdx != NumOps - 1 && MI.getOperand(DstIdx).getReg() != R;
         ++DstIdx)
      ;

    Known = SrcOpKnown.extractBits(BitWidth, BitWidth * DstIdx);
    break;
  }
  case TargetOpcode::G_BSWAP: {
    Register SrcReg = MI.getOperand(1).getReg();
    computeKnownBitsImpl(SrcReg, Known, DemandedElts, Depth + 1);
    Known.byteSwap();
    break;
  }
  case TargetOpcode::G_BITREVERSE: {
    Register SrcReg = MI.getOperand(1).getReg();
    computeKnownBitsImpl(SrcReg, Known, DemandedElts, Depth + 1);
    Known.reverseBits();
    break;
  }
  }

  assert(!Known.hasConflict() && "Bits known to be one AND zero?");
  LLVM_DEBUG(dumpResult(MI, Known, Depth));

  // Update the cache.
  ComputeKnownBitsCache[R] = Known;
}

/// Compute number of sign bits for the intersection of \p Src0 and \p Src1
unsigned GISelKnownBits::computeNumSignBitsMin(Register Src0, Register Src1,
                                               const APInt &DemandedElts,
                                               unsigned Depth) {
  // Test src1 first, since we canonicalize simpler expressions to the RHS.
  unsigned Src1SignBits = computeNumSignBits(Src1, DemandedElts, Depth);
  if (Src1SignBits == 1)
    return 1;
  return std::min(computeNumSignBits(Src0, DemandedElts, Depth), Src1SignBits);
}

unsigned GISelKnownBits::computeNumSignBits(Register R,
                                            const APInt &DemandedElts,
                                            unsigned Depth) {
  MachineInstr &MI = *MRI.getVRegDef(R);
  unsigned Opcode = MI.getOpcode();

  if (Opcode == TargetOpcode::G_CONSTANT)
    return MI.getOperand(1).getCImm()->getValue().getNumSignBits();

  if (Depth == getMaxDepth())
    return 1;

  if (!DemandedElts)
    return 1; // No demanded elts, better to assume we don't know anything.

  LLT DstTy = MRI.getType(R);
  const unsigned TyBits = DstTy.getScalarSizeInBits();

  // Handle the case where this is called on a register that does not have a
  // type constraint. This is unlikely to occur except by looking through copies
  // but it is possible for the initial register being queried to be in this
  // state.
  if (!DstTy.isValid())
    return 1;

  unsigned FirstAnswer = 1;
  switch (Opcode) {
  case TargetOpcode::COPY: {
    MachineOperand &Src = MI.getOperand(1);
    if (Src.getReg().isVirtual() && Src.getSubReg() == 0 &&
        MRI.getType(Src.getReg()).isValid()) {
      // Don't increment Depth for this one since we didn't do any work.
      return computeNumSignBits(Src.getReg(), DemandedElts, Depth);
    }

    return 1;
  }
  case TargetOpcode::G_SEXT: {
    Register Src = MI.getOperand(1).getReg();
    LLT SrcTy = MRI.getType(Src);
    unsigned Tmp = DstTy.getScalarSizeInBits() - SrcTy.getScalarSizeInBits();
    return computeNumSignBits(Src, DemandedElts, Depth + 1) + Tmp;
  }
  case TargetOpcode::G_SEXT_INREG: {
    // Max of the input and what this extends.
    Register Src = MI.getOperand(1).getReg();
    unsigned SrcBits = MI.getOperand(2).getImm();
    unsigned InRegBits = TyBits - SrcBits + 1;
    return std::max(computeNumSignBits(Src, DemandedElts, Depth + 1), InRegBits);
  }
  case TargetOpcode::G_SEXTLOAD: {
    // FIXME: We need an in-memory type representation.
    if (DstTy.isVector())
      return 1;

    // e.g. i16->i32 = '17' bits known.
    const MachineMemOperand *MMO = *MI.memoperands_begin();
    return TyBits - MMO->getSizeInBits() + 1;
  }
  case TargetOpcode::G_ZEXTLOAD: {
    // FIXME: We need an in-memory type representation.
    if (DstTy.isVector())
      return 1;

    // e.g. i16->i32 = '16' bits known.
    const MachineMemOperand *MMO = *MI.memoperands_begin();
    return TyBits - MMO->getSizeInBits();
  }
  case TargetOpcode::G_TRUNC: {
    Register Src = MI.getOperand(1).getReg();
    LLT SrcTy = MRI.getType(Src);

    // Check if the sign bits of source go down as far as the truncated value.
    unsigned DstTyBits = DstTy.getScalarSizeInBits();
    unsigned NumSrcBits = SrcTy.getScalarSizeInBits();
    unsigned NumSrcSignBits = computeNumSignBits(Src, DemandedElts, Depth + 1);
    if (NumSrcSignBits > (NumSrcBits - DstTyBits))
      return NumSrcSignBits - (NumSrcBits - DstTyBits);
    break;
  }
  case TargetOpcode::G_SELECT: {
    return computeNumSignBitsMin(MI.getOperand(2).getReg(),
                                 MI.getOperand(3).getReg(), DemandedElts,
                                 Depth + 1);
  }
  case TargetOpcode::G_INTRINSIC:
  case TargetOpcode::G_INTRINSIC_W_SIDE_EFFECTS:
  default: {
    unsigned NumBits =
      TL.computeNumSignBitsForTargetInstr(*this, R, DemandedElts, MRI, Depth);
    if (NumBits > 1)
      FirstAnswer = std::max(FirstAnswer, NumBits);
    break;
  }
  }

  // Finally, if we can prove that the top bits of the result are 0's or 1's,
  // use this information.
  KnownBits Known = getKnownBits(R, DemandedElts, Depth);
  APInt Mask;
  if (Known.isNonNegative()) {        // sign bit is 0
    Mask = Known.Zero;
  } else if (Known.isNegative()) {  // sign bit is 1;
    Mask = Known.One;
  } else {
    // Nothing known.
    return FirstAnswer;
  }

  // Okay, we know that the sign bit in Mask is set.  Use CLO to determine
  // the number of identical bits in the top of the input value.
  Mask <<= Mask.getBitWidth() - TyBits;
  return std::max(FirstAnswer, Mask.countLeadingOnes());
}

unsigned GISelKnownBits::computeNumSignBits(Register R, unsigned Depth) {
  LLT Ty = MRI.getType(R);
  APInt DemandedElts = Ty.isVector()
                           ? APInt::getAllOnesValue(Ty.getNumElements())
                           : APInt(1, 1);
  return computeNumSignBits(R, DemandedElts, Depth);
}

void GISelKnownBitsAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  MachineFunctionPass::getAnalysisUsage(AU);
}

bool GISelKnownBitsAnalysis::runOnMachineFunction(MachineFunction &MF) {
  return false;
}
