//===-- MAPIPISelDAGToDAG.cpp - A dag to dag inst selector for MAPIP ----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines an instruction selector for the MAPIP target.
//
//===----------------------------------------------------------------------===//

#include "Mapip.h"
#include "MapipTargetMachine.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Function.h"
#include "llvm/Intrinsics.h"
#include "llvm/CallingConv.h"
#include "llvm/Constants.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"
using namespace llvm;

namespace {
  struct MAPIPISelAddressMode {
    enum {
      RegBase,
      FrameIndexBase
    } BaseType;

    struct {            // This is really a union, discriminated by BaseType!
      SDValue Reg;
      int FrameIndex;
    } Base;

    int16_t Disp;
    const GlobalValue *GV;
    const Constant *CP;
    const BlockAddress *BlockAddr;
    const char *ES;
    int JT;
    unsigned Align;    // CP alignment.

    MAPIPISelAddressMode()
      : BaseType(RegBase), Disp(0), GV(0), CP(0), BlockAddr(0),
        ES(0), JT(-1), Align(0) {
    }

    bool hasSymbolicDisplacement() const {
      return GV != 0 || CP != 0 || ES != 0 || JT != -1;
    }

    void dump() {
      errs() << "MAPIPISelAddressMode " << this << '\n';
      if (BaseType == RegBase && Base.Reg.getNode() != 0) {
        errs() << "Base.Reg ";
        Base.Reg.getNode()->dump();
      } else if (BaseType == FrameIndexBase) {
        errs() << " Base.FrameIndex " << Base.FrameIndex << '\n';
      }
      errs() << " Disp " << Disp << '\n';
      if (GV) {
        errs() << "GV ";
        GV->dump();
      } else if (CP) {
        errs() << " CP ";
        CP->dump();
        errs() << " Align" << Align << '\n';
      } else if (ES) {
        errs() << "ES ";
        errs() << ES << '\n';
      } else if (JT != -1)
        errs() << " JT" << JT << " Align" << Align << '\n';
    }
  };
}

/// MAPIPDAGToDAGISel - MAPIP specific code to select MAPIP machine
/// instructions for SelectionDAG operations.
///
namespace {
  class MAPIPDAGToDAGISel : public SelectionDAGISel {
    const MAPIPTargetLowering &Lowering;
    const MAPIPSubtarget &Subtarget;

  public:
    MAPIPDAGToDAGISel(MAPIPTargetMachine &TM, CodeGenOpt::Level OptLevel)
      : SelectionDAGISel(TM, OptLevel),
        Lowering(*TM.getTargetLowering()),
        Subtarget(*TM.getSubtargetImpl()) { }

    virtual const char *getPassName() const {
      return "MAPIP DAG->DAG Pattern Instruction Selection";
    }

    bool MatchAddress(SDValue N, MAPIPISelAddressMode &AM);
    bool MatchWrapper(SDValue N, MAPIPISelAddressMode &AM);
    bool MatchAddressBase(SDValue N, MAPIPISelAddressMode &AM);

    virtual bool
    SelectInlineAsmMemoryOperand(const SDValue &Op, char ConstraintCode,
                                 std::vector<SDValue> &OutOps);

    // Include the pieces autogenerated from the target description.
  #include "MAPIPGenDAGISel.inc"

  private:
    SDNode *Select(SDNode *N);

    bool SelectAddr(SDValue Addr, SDValue &Base, SDValue &Disp);
  };
}  // end anonymous namespace

/// createMAPIPISelDag - This pass converts a legalized DAG into a
/// MAPIP-specific DAG, ready for instruction scheduling.
///
FunctionPass *llvm::createMAPIPISelDag(MAPIPTargetMachine &TM,
                                        CodeGenOpt::Level OptLevel) {
  return new MAPIPDAGToDAGISel(TM, OptLevel);
}


/// MatchWrapper - Try to match MAPIPISD::Wrapper node into an addressing mode.
/// These wrap things that will resolve down into a symbol reference.  If no
/// match is possible, this returns true, otherwise it returns false.
bool MAPIPDAGToDAGISel::MatchWrapper(SDValue N, MAPIPISelAddressMode &AM) {
  // If the addressing mode already has a symbol as the displacement, we can
  // never match another symbol.
  if (AM.hasSymbolicDisplacement())
    return true;

  SDValue N0 = N.getOperand(0);

  if (GlobalAddressSDNode *G = dyn_cast<GlobalAddressSDNode>(N0)) {
    AM.GV = G->getGlobal();
    AM.Disp += G->getOffset();
    //AM.SymbolFlags = G->getTargetFlags();
  } else if (ConstantPoolSDNode *CP = dyn_cast<ConstantPoolSDNode>(N0)) {
    AM.CP = CP->getConstVal();
    AM.Align = CP->getAlignment();
    AM.Disp += CP->getOffset();
    //AM.SymbolFlags = CP->getTargetFlags();
  } else if (ExternalSymbolSDNode *S = dyn_cast<ExternalSymbolSDNode>(N0)) {
    AM.ES = S->getSymbol();
    //AM.SymbolFlags = S->getTargetFlags();
  } else if (JumpTableSDNode *J = dyn_cast<JumpTableSDNode>(N0)) {
    AM.JT = J->getIndex();
    //AM.SymbolFlags = J->getTargetFlags();
  } else {
    AM.BlockAddr = cast<BlockAddressSDNode>(N0)->getBlockAddress();
    //AM.SymbolFlags = cast<BlockAddressSDNode>(N0)->getTargetFlags();
  }
  return false;
}

/// MatchAddressBase - Helper for MatchAddress. Add the specified node to the
/// specified addressing mode without any further recursion.
bool MAPIPDAGToDAGISel::MatchAddressBase(SDValue N, MAPIPISelAddressMode &AM) {
  // Is the base register already occupied?
  if (AM.BaseType != MAPIPISelAddressMode::RegBase || AM.Base.Reg.getNode()) {
    // If so, we cannot select it.
    return true;
  }

  // Default, generate it as a register.
  AM.BaseType = MAPIPISelAddressMode::RegBase;
  AM.Base.Reg = N;
  return false;
}

bool MAPIPDAGToDAGISel::MatchAddress(SDValue N, MAPIPISelAddressMode &AM) {
  DEBUG(errs() << "MatchAddress: "; AM.dump());

  switch (N.getOpcode()) {
  default: break;
  case ISD::Constant: {
    uint64_t Val = cast<ConstantSDNode>(N)->getSExtValue();
    AM.Disp += Val;
    return false;
  }

  case MAPIPISD::Wrapper:
    if (!MatchWrapper(N, AM))
      return false;
    break;

  case ISD::FrameIndex:
    if (AM.BaseType == MAPIPISelAddressMode::RegBase
        && AM.Base.Reg.getNode() == 0) {
      AM.BaseType = MAPIPISelAddressMode::FrameIndexBase;
      AM.Base.FrameIndex = cast<FrameIndexSDNode>(N)->getIndex();
      return false;
    }
    break;

  case ISD::ADD: {
    MAPIPISelAddressMode Backup = AM;
    if (!MatchAddress(N.getNode()->getOperand(0), AM) &&
        !MatchAddress(N.getNode()->getOperand(1), AM))
      return false;
    AM = Backup;
    if (!MatchAddress(N.getNode()->getOperand(1), AM) &&
        !MatchAddress(N.getNode()->getOperand(0), AM))
      return false;
    AM = Backup;

    break;
  }

  case ISD::OR:
    // Handle "X | C" as "X + C" iff X is known to have C bits clear.
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      MAPIPISelAddressMode Backup = AM;
      uint64_t Offset = CN->getSExtValue();
      // Start with the LHS as an addr mode.
      if (!MatchAddress(N.getOperand(0), AM) &&
          // Address could not have picked a GV address for the displacement.
          AM.GV == NULL &&
          // Check to see if the LHS & C is zero.
          CurDAG->MaskedValueIsZero(N.getOperand(0), CN->getAPIntValue())) {
        AM.Disp += Offset;
        return false;
      }
      AM = Backup;
    }
    break;
  }

  return MatchAddressBase(N, AM);
}

/// SelectAddr - returns true if it is able pattern match an addressing mode.
/// It returns the operands which make up the maximal addressing mode it can
/// match by reference.
bool MAPIPDAGToDAGISel::SelectAddr(SDValue N,
                                    SDValue &Base, SDValue &Disp) {
  MAPIPISelAddressMode AM;

  if (MatchAddress(N, AM))
    return false;

  EVT VT = N.getValueType();
  if (AM.BaseType == MAPIPISelAddressMode::RegBase) {
    if (!AM.Base.Reg.getNode())
      AM.Base.Reg = CurDAG->getRegister(0, VT);
  }

  Base  = (AM.BaseType == MAPIPISelAddressMode::FrameIndexBase) ?
    CurDAG->getTargetFrameIndex(AM.Base.FrameIndex, TLI.getPointerTy()) :
    AM.Base.Reg;

  if (AM.GV)
    Disp = CurDAG->getTargetGlobalAddress(AM.GV, N->getDebugLoc(),
                                          MVT::i16, AM.Disp,
                                          0/*AM.SymbolFlags*/);
  else if (AM.CP)
    Disp = CurDAG->getTargetConstantPool(AM.CP, MVT::i16,
                                         AM.Align, AM.Disp, 0/*AM.SymbolFlags*/);
  else if (AM.ES)
    Disp = CurDAG->getTargetExternalSymbol(AM.ES, MVT::i16, 0/*AM.SymbolFlags*/);
  else if (AM.JT != -1)
    Disp = CurDAG->getTargetJumpTable(AM.JT, MVT::i16, 0/*AM.SymbolFlags*/);
  else if (AM.BlockAddr)
    Disp = CurDAG->getBlockAddress(AM.BlockAddr, MVT::i32,
                                   true, 0/*AM.SymbolFlags*/);
  else
    Disp = CurDAG->getTargetConstant(AM.Disp, MVT::i16);

  return true;
}

bool MAPIPDAGToDAGISel::
SelectInlineAsmMemoryOperand(const SDValue &Op, char ConstraintCode,
                             std::vector<SDValue> &OutOps) {
  SDValue Op0, Op1;
  switch (ConstraintCode) {
  default: return true;
  case 'm':   // memory
    if (!SelectAddr(Op, Op0, Op1))
      return true;
    break;
  }

  OutOps.push_back(Op0);
  OutOps.push_back(Op1);
  return false;
}

SDNode *MAPIPDAGToDAGISel::Select(SDNode *Node) {
  DebugLoc dl = Node->getDebugLoc();

  // Dump information about the Node being selected
  DEBUG(errs() << "Selecting: ");
  DEBUG(Node->dump(CurDAG));
  DEBUG(errs() << "\n");

  // If we have a custom node, we already have selected!
  if (Node->isMachineOpcode()) {
    DEBUG(errs() << "== ";
          Node->dump(CurDAG);
          errs() << "\n");
    return NULL;
  }

  // Few custom selection stuff.
  switch (Node->getOpcode()) {
  default: break;
  case ISD::FrameIndex: {
    assert(Node->getValueType(0) == MVT::i16);
    int FI = cast<FrameIndexSDNode>(Node)->getIndex();
    SDValue TFI = CurDAG->getTargetFrameIndex(FI, MVT::i16);
    if (Node->hasOneUse())
      return CurDAG->SelectNodeTo(Node, MAPIP::ADD16ri, MVT::i16,
                                  TFI, CurDAG->getTargetConstant(0, MVT::i16));
    return CurDAG->getMachineNode(MAPIP::ADD16ri, dl, MVT::i16,
                                  TFI, CurDAG->getTargetConstant(0, MVT::i16));
  }
  }

  // Select the default instruction
  SDNode *ResNode = SelectCode(Node);

  DEBUG(errs() << "=> ");
  if (ResNode == NULL || ResNode == Node)
    DEBUG(Node->dump(CurDAG));
  else
    DEBUG(ResNode->dump(CurDAG));
  DEBUG(errs() << "\n");

  return ResNode;
}
