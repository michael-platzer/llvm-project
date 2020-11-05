//==- RISCVSchedVectorProc.td - VectorProc Scheduling Definitions -*- tablegen -*-=//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

// ===---------------------------------------------------------------------===//
// The following definitions describe the simpler per-operand machine model.
// This works with MachineScheduler. See MCSchedule.h for details.

// VectorProc machine model for scheduling and other instruction cost heuristics.
def VectorProcModel : SchedMachineModel {
  let MicroOpBufferSize = 0; // Explicitly set to zero since VectorProc is in-order.
  let IssueWidth = 1;        // 1 micro-ops are dispatched per cycle.
  let LoadLatency = 2;
  let MispredictPenalty = 3;
}

//===----------------------------------------------------------------------===//
// Define each kind of processor resource and number available.

// Modeling each pipeline as a ProcResource using the BufferSize = 0 since
// Rocket is in-order.

let BufferSize = 0 in {
def VectorProcUnitMain       : ProcResource<1>; // Main core (for non-vector stuff)
def VectorProcUnitVLSU       : ProcResource<1>; // Vector Load/Store
def VectorProcUnitVALU       : ProcResource<1>; // Vector ALU
def VectorProcUnitVMUL       : ProcResource<1>; // Vector Multiplier
def VectorProcUnitSLDU       : ProcResource<1>; // Vector Slide Unit
}

//===----------------------------------------------------------------------===//
// Subtarget-specific SchedWrite types which both map the ProcResources and
// set the latency.

let SchedModel = VectorProcModel in {

def : WriteRes<WriteJmp, [VectorProcUnitMain]>;
def : WriteRes<WriteJal, [VectorProcUnitMain]>;
def : WriteRes<WriteJalr, [VectorProcUnitMain]>;
def : WriteRes<WriteJmpReg, [VectorProcUnitMain]>;
def : WriteRes<WriteIALU, [VectorProcUnitMain]>;
def : WriteRes<WriteShift, [VectorProcUnitMain]>;
def : WriteRes<WriteIMul, [VectorProcUnitMain]> {
  let Latency = 4;
}
def : WriteRes<WriteIDiv, [VectorProcUnitMain]> {
  let Latency = 32;
  let ResourceCycles = [32];
}

// Memory
def : WriteRes<WriteSTB, [VectorProcUnitMain]>;
def : WriteRes<WriteSTH, [VectorProcUnitMain]>;
def : WriteRes<WriteSTW, [VectorProcUnitMain]>;

let Latency = 2 in {
def : WriteRes<WriteLDB, [VectorProcUnitMain]>;
def : WriteRes<WriteLDH, [VectorProcUnitMain]>;
def : WriteRes<WriteCSR, [VectorProcUnitMain]>;
def : WriteRes<WriteLDW, [VectorProcUnitMain]>;
}

def : WriteRes<WriteNop, []>;

def : InstRW<[WriteIALU], (instrs COPY)>;

// Vector
let Latency = 9, ResourceCycles = [4] in {
def : WriteRes<WriteVUnitStrideLoad,         [VectorProcUnitVLSU]>;
def : WriteRes<WriteVStridedLoad,            [VectorProcUnitVLSU]>;
def : WriteRes<WriteVIndexedLoad,            [VectorProcUnitVLSU]>;
def : WriteRes<WriteVWholeLoad,              [VectorProcUnitVLSU]>;
def : WriteRes<WriteVUnitStrideSegmentLoad,  [VectorProcUnitVLSU]>;
def : WriteRes<WriteVStridedSegmentLoad,     [VectorProcUnitVLSU]>;
def : WriteRes<WriteVIndexedSegmentLoad,     [VectorProcUnitVLSU]>;
}
let Latency = 7, ResourceCycles = [4] in {
def : WriteRes<WriteVUnitStrideStore,        [VectorProcUnitVLSU]>;
def : WriteRes<WriteVStridedStore,           [VectorProcUnitVLSU]>;
def : WriteRes<WriteVIndexedStore,           [VectorProcUnitVLSU]>;
def : WriteRes<WriteVWholeStore,             [VectorProcUnitVLSU]>;
def : WriteRes<WriteVUnitStrideSegmentStore, [VectorProcUnitVLSU]>;
def : WriteRes<WriteVStridedSegmentStore,    [VectorProcUnitVLSU]>;
def : WriteRes<WriteVIndexedSegmentStore,    [VectorProcUnitVLSU]>;
}
let Latency = 7, ResourceCycles = [4] in {
def : WriteRes<WriteVALUVV,                  [VectorProcUnitVALU]>;
def : WriteRes<WriteVALUVX,                  [VectorProcUnitVALU]>;
def : WriteRes<WriteVALUVI,                  [VectorProcUnitVALU]>;
def : WriteRes<WriteVALUVF,                  [VectorProcUnitVALU]>;
def : WriteRes<WriteVALUVs2,                 [VectorProcUnitVALU]>;
}

let Unsupported = 1 in {
def : WriteRes<WriteIALU32, []>;
def : WriteRes<WriteShift32, []>;
def : WriteRes<WriteIMul32, []>;
def : WriteRes<WriteIDiv32, []>;
def : WriteRes<WriteSTD, []>;
def : WriteRes<WriteLDWU, []>;
def : WriteRes<WriteLDD, []>;
def : WriteRes<WriteAtomicD, []>;
def : WriteRes<WriteAtomicLDD, []>;
def : WriteRes<WriteAtomicSTD, []>;
def : WriteRes<WriteAtomicW, [Rocket64UnitMem]>;
def : WriteRes<WriteAtomicLDW, [Rocket64UnitMem]>;
def : WriteRes<WriteAtomicSTW, [Rocket64UnitMem]>;
def : WriteRes<WriteFST32, []>;
def : WriteRes<WriteFST64, []>;
def : WriteRes<WriteFLD32, []>;
def : WriteRes<WriteFLD64, []>;
def : WriteRes<WriteFALU32, []>;
def : WriteRes<WriteFSGNJ32, []>;
def : WriteRes<WriteFMinMax32, []>;
def : WriteRes<WriteFALU64, []>;
def : WriteRes<WriteFSGNJ64, []>;
def : WriteRes<WriteFMinMax64, []>;
def : WriteRes<WriteFCvtI32ToF32, []>;
def : WriteRes<WriteFCvtI32ToF64, []>;
def : WriteRes<WriteFCvtF32ToI32, []>;
def : WriteRes<WriteFCvtF64ToI32, []>;
def : WriteRes<WriteFCvtF32ToF64, []>;
def : WriteRes<WriteFCvtF64ToF32, []>;
def : WriteRes<WriteFClass32, []>;
def : WriteRes<WriteFClass64, []>;
def : WriteRes<WriteFCmp32, []>;
def : WriteRes<WriteFCmp64, []>;
def : WriteRes<WriteFMovF32ToI32, []>;
def : WriteRes<WriteFMovI32ToF32, []>;
def : WriteRes<WriteFMul32, []>;
def : WriteRes<WriteFMulAdd32, []>;
def : WriteRes<WriteFMulSub32, []>;
def : WriteRes<WriteFMul64, []>;
def : WriteRes<WriteFMulAdd64, []>;
def : WriteRes<WriteFMulSub64, []>;
def : WriteRes<WriteFDiv32, []>;
def : WriteRes<WriteFDiv64, []>;
def : WriteRes<WriteFSqrt32, []>;
def : WriteRes<WriteFSqrt64, []>;
def : WriteRes<WriteFCvtI64ToF32, []>;
def : WriteRes<WriteFCvtI64ToF64, []>;
def : WriteRes<WriteFCvtF64ToI64, []>;
def : WriteRes<WriteFCvtF32ToI64, []>;
def : WriteRes<WriteFMovI64ToF64, []>;
def : WriteRes<WriteFMovF64ToI64, []>;

def : WriteRes<WriteVAMOWd, []>;
def : WriteRes<WriteVAMONoWd, []>;
}

//===----------------------------------------------------------------------===//
// Subtarget-specific SchedRead types with cycles.
// Dummy definitions for VectorProc.
def : ReadAdvance<ReadJmp, 0>;
def : ReadAdvance<ReadJalr, 0>;
def : ReadAdvance<ReadCSR, 0>;
def : ReadAdvance<ReadStoreData, 0>;
def : ReadAdvance<ReadMemBase, 0>;
def : ReadAdvance<ReadIALU, 0>;
def : ReadAdvance<ReadIALU32, 0>;
def : ReadAdvance<ReadShift, 0>;
def : ReadAdvance<ReadShift32, 0>;
def : ReadAdvance<ReadIDiv, 0>;
def : ReadAdvance<ReadIDiv32, 0>;
def : ReadAdvance<ReadIMul, 0>;
def : ReadAdvance<ReadIMul32, 0>;
def : ReadAdvance<ReadAtomicWA, 0>;
def : ReadAdvance<ReadAtomicWD, 0>;
def : ReadAdvance<ReadAtomicDA, 0>;
def : ReadAdvance<ReadAtomicDD, 0>;
def : ReadAdvance<ReadAtomicLDW, 0>;
def : ReadAdvance<ReadAtomicLDD, 0>;
def : ReadAdvance<ReadAtomicSTW, 0>;
def : ReadAdvance<ReadAtomicSTD, 0>;
def : ReadAdvance<ReadFMemBase, 0>;
def : ReadAdvance<ReadFALU32, 0>;
def : ReadAdvance<ReadFALU64, 0>;
def : ReadAdvance<ReadFMul32, 0>;
def : ReadAdvance<ReadFMulAdd32, 0>;
def : ReadAdvance<ReadFMulSub32, 0>;
def : ReadAdvance<ReadFMul64, 0>;
def : ReadAdvance<ReadFMulAdd64, 0>;
def : ReadAdvance<ReadFMulSub64, 0>;
def : ReadAdvance<ReadFDiv32, 0>;
def : ReadAdvance<ReadFDiv64, 0>;
def : ReadAdvance<ReadFSqrt32, 0>;
def : ReadAdvance<ReadFSqrt64, 0>;
def : ReadAdvance<ReadFCmp32, 0>;
def : ReadAdvance<ReadFCmp64, 0>;
def : ReadAdvance<ReadFSGNJ32, 0>;
def : ReadAdvance<ReadFSGNJ64, 0>;
def : ReadAdvance<ReadFMinMax32, 0>;
def : ReadAdvance<ReadFMinMax64, 0>;
def : ReadAdvance<ReadFCvtF32ToI32, 0>;
def : ReadAdvance<ReadFCvtF32ToI64, 0>;
def : ReadAdvance<ReadFCvtF64ToI32, 0>;
def : ReadAdvance<ReadFCvtF64ToI64, 0>;
def : ReadAdvance<ReadFCvtI32ToF32, 0>;
def : ReadAdvance<ReadFCvtI32ToF64, 0>;
def : ReadAdvance<ReadFCvtI64ToF32, 0>;
def : ReadAdvance<ReadFCvtI64ToF64, 0>;
def : ReadAdvance<ReadFCvtF32ToF64, 0>;
def : ReadAdvance<ReadFCvtF64ToF32, 0>;
def : ReadAdvance<ReadFMovF32ToI32, 0>;
def : ReadAdvance<ReadFMovI32ToF32, 0>;
def : ReadAdvance<ReadFMovF64ToI64, 0>;
def : ReadAdvance<ReadFMovI64ToF64, 0>;
def : ReadAdvance<ReadFClass32, 0>;
def : ReadAdvance<ReadFClass64, 0>;

def : ReadAdvance<ReadVUnitStrideLoad, 0>;
def : ReadAdvance<ReadVStridedLoad, 0>;
def : ReadAdvance<ReadVIndexedLoad, 0>;
def : ReadAdvance<ReadVWholeLoad, 0>;
def : ReadAdvance<ReadVUnitStrideSegmentLoad, 0>;
def : ReadAdvance<ReadVStridedSegmentLoad, 0>;
def : ReadAdvance<ReadVIndexedSegmentLoad, 0>;
def : ReadAdvance<ReadVUnitStrideStore, 0>;
def : ReadAdvance<ReadVStridedStore, 0>;
def : ReadAdvance<ReadVIndexedStore, 0>;
def : ReadAdvance<ReadVWholeStore, 0>;
def : ReadAdvance<ReadVUnitStrideSegmentStore, 0>;
def : ReadAdvance<ReadVStridedSegmentStore, 0>;
def : ReadAdvance<ReadVIndexedSegmentStore, 0>;
def : ReadAdvance<ReadVAMOWd, 0>;
def : ReadAdvance<ReadVAMONoWd, 0>;
def : ReadAdvance<ReadVALUVV, 0>;
def : ReadAdvance<ReadVALUVX, 0>;
def : ReadAdvance<ReadVALUVI, 0>;
def : ReadAdvance<ReadVALUVF, 0>;
def : ReadAdvance<ReadVALUVs2, 0>;
}
