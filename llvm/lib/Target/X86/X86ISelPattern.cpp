//===-- X86ISelPattern.cpp - A pattern matching inst selector for X86 -----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
// 
//===----------------------------------------------------------------------===//
//
// This file defines a pattern matching instruction selector for X86.
//
//===----------------------------------------------------------------------===//

#include "X86.h"
#include "X86InstrBuilder.h"
#include "X86RegisterInfo.h"
#include "llvm/Constants.h"                   // FIXME: REMOVE
#include "llvm/Function.h"
#include "llvm/CodeGen/MachineConstantPool.h" // FIXME: REMOVE
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/SelectionDAG.h"
#include "llvm/CodeGen/SelectionDAGISel.h"
#include "llvm/CodeGen/SSARegMap.h"
#include "llvm/Target/TargetData.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/ADT/Statistic.h"
#include <set>
#include <algorithm>
using namespace llvm;

//===----------------------------------------------------------------------===//
//  X86TargetLowering - X86 Implementation of the TargetLowering interface
namespace {
  class X86TargetLowering : public TargetLowering {
    int VarArgsFrameIndex;            // FrameIndex for start of varargs area.
    int ReturnAddrIndex;              // FrameIndex for return slot.
  public:
    X86TargetLowering(TargetMachine &TM) : TargetLowering(TM) {
      // Set up the TargetLowering object.
      addRegisterClass(MVT::i8, X86::R8RegisterClass);
      addRegisterClass(MVT::i16, X86::R16RegisterClass);
      addRegisterClass(MVT::i32, X86::R32RegisterClass);
      addRegisterClass(MVT::f64, X86::RFPRegisterClass);
      
      // FIXME: Eliminate these two classes when legalize can handle promotions
      // well.
      addRegisterClass(MVT::i1, X86::R8RegisterClass);
      addRegisterClass(MVT::f32, X86::RFPRegisterClass);
      
      computeRegisterProperties();

      setOperationUnsupported(ISD::MEMMOVE, MVT::Other);

      setOperationUnsupported(ISD::MUL, MVT::i8);
      setOperationUnsupported(ISD::SELECT, MVT::i1);
      setOperationUnsupported(ISD::SELECT, MVT::i8);
      
      addLegalFPImmediate(+0.0); // FLD0
      addLegalFPImmediate(+1.0); // FLD1
      addLegalFPImmediate(-0.0); // FLD0/FCHS
      addLegalFPImmediate(-1.0); // FLD1/FCHS
    }

    /// LowerArguments - This hook must be implemented to indicate how we should
    /// lower the arguments for the specified function, into the specified DAG.
    virtual std::vector<SDOperand>
    LowerArguments(Function &F, SelectionDAG &DAG);

    /// LowerCallTo - This hook lowers an abstract call to a function into an
    /// actual call.
    virtual std::pair<SDOperand, SDOperand>
    LowerCallTo(SDOperand Chain, const Type *RetTy, SDOperand Callee,
                ArgListTy &Args, SelectionDAG &DAG);

    virtual std::pair<SDOperand, SDOperand>
    LowerVAStart(SDOperand Chain, SelectionDAG &DAG);

    virtual std::pair<SDOperand,SDOperand>
    LowerVAArgNext(bool isVANext, SDOperand Chain, SDOperand VAList,
                   const Type *ArgTy, SelectionDAG &DAG);

    virtual std::pair<SDOperand, SDOperand>
    LowerFrameReturnAddress(bool isFrameAddr, SDOperand Chain, unsigned Depth,
                            SelectionDAG &DAG);
  };
}


std::vector<SDOperand>
X86TargetLowering::LowerArguments(Function &F, SelectionDAG &DAG) {
  std::vector<SDOperand> ArgValues;

  // Add DAG nodes to load the arguments...  On entry to a function on the X86,
  // the stack frame looks like this:
  //
  // [ESP] -- return address
  // [ESP + 4] -- first argument (leftmost lexically)
  // [ESP + 8] -- second argument, if first argument is four bytes in size
  //    ... 
  //
  MachineFunction &MF = DAG.getMachineFunction();
  MachineFrameInfo *MFI = MF.getFrameInfo();
  
  unsigned ArgOffset = 0;   // Frame mechanisms handle retaddr slot
  for (Function::aiterator I = F.abegin(), E = F.aend(); I != E; ++I) {
    MVT::ValueType ObjectVT = getValueType(I->getType());
    unsigned ArgIncrement = 4;
    unsigned ObjSize;
    switch (ObjectVT) {
    default: assert(0 && "Unhandled argument type!");
    case MVT::i1:
    case MVT::i8:  ObjSize = 1;                break;
    case MVT::i16: ObjSize = 2;                break;
    case MVT::i32: ObjSize = 4;                break;
    case MVT::i64: ObjSize = ArgIncrement = 8; break;
    case MVT::f32: ObjSize = 4;                break;
    case MVT::f64: ObjSize = ArgIncrement = 8; break;
    }
    // Create the frame index object for this incoming parameter...
    int FI = MFI->CreateFixedObject(ObjSize, ArgOffset);
    
    // Create the SelectionDAG nodes corresponding to a load from this parameter
    SDOperand FIN = DAG.getFrameIndex(FI, MVT::i32);

    // Don't codegen dead arguments.  FIXME: remove this check when we can nuke
    // dead loads.
    SDOperand ArgValue;
    if (!I->use_empty())
      ArgValue = DAG.getLoad(ObjectVT, DAG.getEntryNode(), FIN);
    else {
      if (MVT::isInteger(ObjectVT))
        ArgValue = DAG.getConstant(0, ObjectVT);
      else
        ArgValue = DAG.getConstantFP(0, ObjectVT);
    }
    ArgValues.push_back(ArgValue);

    ArgOffset += ArgIncrement;   // Move on to the next argument...
  }

  // If the function takes variable number of arguments, make a frame index for
  // the start of the first vararg value... for expansion of llvm.va_start.
  if (F.isVarArg())
    VarArgsFrameIndex = MFI->CreateFixedObject(1, ArgOffset);
  ReturnAddrIndex = 0;  // No return address slot generated yet.
  return ArgValues;
}

std::pair<SDOperand, SDOperand>
X86TargetLowering::LowerCallTo(SDOperand Chain,
                               const Type *RetTy, SDOperand Callee,
                               ArgListTy &Args, SelectionDAG &DAG) {
  // Count how many bytes are to be pushed on the stack.
  unsigned NumBytes = 0;

  if (Args.empty()) {
    // Save zero bytes.
    Chain = DAG.getNode(ISD::ADJCALLSTACKDOWN, MVT::Other, Chain,
                        DAG.getConstant(0, getPointerTy()));
  } else {
    for (unsigned i = 0, e = Args.size(); i != e; ++i)
      switch (getValueType(Args[i].second)) {
      default: assert(0 && "Unknown value type!");
      case MVT::i1:
      case MVT::i8:
      case MVT::i16:
      case MVT::i32:
      case MVT::f32:
        NumBytes += 4;
        break;
      case MVT::i64:
      case MVT::f64:
        NumBytes += 8;
        break;
      }

    Chain = DAG.getNode(ISD::ADJCALLSTACKDOWN, MVT::Other, Chain,
                        DAG.getConstant(NumBytes, getPointerTy()));

    // Arguments go on the stack in reverse order, as specified by the ABI.
    unsigned ArgOffset = 0;
    SDOperand StackPtr = DAG.getCopyFromReg(X86::ESP, MVT::i32);
    for (unsigned i = 0, e = Args.size(); i != e; ++i) {
      unsigned ArgReg;
      SDOperand PtrOff = DAG.getConstant(ArgOffset, getPointerTy());
      PtrOff = DAG.getNode(ISD::ADD, MVT::i32, StackPtr, PtrOff);

      switch (getValueType(Args[i].second)) {
      default: assert(0 && "Unexpected ValueType for argument!");
      case MVT::i1:
      case MVT::i8:
      case MVT::i16:
        // Promote the integer to 32 bits.  If the input type is signed use a
        // sign extend, otherwise use a zero extend.
        if (Args[i].second->isSigned())
          Args[i].first =DAG.getNode(ISD::SIGN_EXTEND, MVT::i32, Args[i].first);
        else
          Args[i].first =DAG.getNode(ISD::ZERO_EXTEND, MVT::i32, Args[i].first);

        // FALL THROUGH
      case MVT::i32:
      case MVT::f32:
        // FIXME: Note that all of these stores are independent of each other.
        Chain = DAG.getNode(ISD::STORE, MVT::Other, Chain,
                            Args[i].first, PtrOff);
        ArgOffset += 4;
        break;
      case MVT::i64:
      case MVT::f64:
        // FIXME: Note that all of these stores are independent of each other.
        Chain = DAG.getNode(ISD::STORE, MVT::Other, Chain,
                            Args[i].first, PtrOff);
        ArgOffset += 8;
        break;
      }
    }
  }

  std::vector<MVT::ValueType> RetVals;
  MVT::ValueType RetTyVT = getValueType(RetTy);
  if (RetTyVT != MVT::isVoid)
    RetVals.push_back(RetTyVT);
  RetVals.push_back(MVT::Other);

  SDOperand TheCall = SDOperand(DAG.getCall(RetVals, Chain, Callee), 0);
  Chain = TheCall.getValue(RetTyVT != MVT::isVoid);
  Chain = DAG.getNode(ISD::ADJCALLSTACKUP, MVT::Other, Chain,
                      DAG.getConstant(NumBytes, getPointerTy()));
  return std::make_pair(TheCall, Chain);
}

std::pair<SDOperand, SDOperand>
X86TargetLowering::LowerVAStart(SDOperand Chain, SelectionDAG &DAG) {
  // vastart just returns the address of the VarArgsFrameIndex slot.
  return std::make_pair(DAG.getFrameIndex(VarArgsFrameIndex, MVT::i32), Chain);
}

std::pair<SDOperand,SDOperand> X86TargetLowering::
LowerVAArgNext(bool isVANext, SDOperand Chain, SDOperand VAList,
               const Type *ArgTy, SelectionDAG &DAG) {
  MVT::ValueType ArgVT = getValueType(ArgTy);
  SDOperand Result;
  if (!isVANext) {
    Result = DAG.getLoad(ArgVT, DAG.getEntryNode(), VAList);
  } else {
    unsigned Amt;
    if (ArgVT == MVT::i32)
      Amt = 4;
    else {
      assert((ArgVT == MVT::i64 || ArgVT == MVT::f64) &&
             "Other types should have been promoted for varargs!");
      Amt = 8;
    }
    Result = DAG.getNode(ISD::ADD, VAList.getValueType(), VAList,
                         DAG.getConstant(Amt, VAList.getValueType()));
  }
  return std::make_pair(Result, Chain);
}
               

std::pair<SDOperand, SDOperand> X86TargetLowering::
LowerFrameReturnAddress(bool isFrameAddress, SDOperand Chain, unsigned Depth,
                        SelectionDAG &DAG) {
  SDOperand Result;
  if (Depth)        // Depths > 0 not supported yet!
    Result = DAG.getConstant(0, getPointerTy());
  else {
    if (ReturnAddrIndex == 0) {
      // Set up a frame object for the return address.
      MachineFunction &MF = DAG.getMachineFunction();
      ReturnAddrIndex = MF.getFrameInfo()->CreateFixedObject(4, -4);
    }
    
    SDOperand RetAddrFI = DAG.getFrameIndex(ReturnAddrIndex, MVT::i32);

    if (!isFrameAddress)
      // Just load the return address
      Result = DAG.getLoad(MVT::i32, DAG.getEntryNode(), RetAddrFI);
    else
      Result = DAG.getNode(ISD::SUB, MVT::i32, RetAddrFI,
                           DAG.getConstant(4, MVT::i32));
  }
  return std::make_pair(Result, Chain);
}





namespace {
  Statistic<>
  NumFPKill("x86-codegen", "Number of FP_REG_KILL instructions added");

  //===--------------------------------------------------------------------===//
  /// ISel - X86 specific code to select X86 machine instructions for
  /// SelectionDAG operations.
  ///
  class ISel : public SelectionDAGISel {
    /// ContainsFPCode - Every instruction we select that uses or defines a FP
    /// register should set this to true.
    bool ContainsFPCode;

    /// X86Lowering - This object fully describes how to lower LLVM code to an
    /// X86-specific SelectionDAG.
    X86TargetLowering X86Lowering;

    /// RegPressureMap - This keeps an approximate count of the number of
    /// registers required to evaluate each node in the graph.
    std::map<SDNode*, unsigned> RegPressureMap;

    /// ExprMap - As shared expressions are codegen'd, we keep track of which
    /// vreg the value is produced in, so we only emit one copy of each compiled
    /// tree.
    std::map<SDOperand, unsigned> ExprMap;
    std::set<SDOperand> LoweredTokens;

  public:
    ISel(TargetMachine &TM) : SelectionDAGISel(X86Lowering), X86Lowering(TM) {
    }

    unsigned getRegPressure(SDOperand O) {
      return RegPressureMap[O.Val];
    }
    unsigned ComputeRegPressure(SDOperand O);

    /// InstructionSelectBasicBlock - This callback is invoked by
    /// SelectionDAGISel when it has created a SelectionDAG for us to codegen.
    virtual void InstructionSelectBasicBlock(SelectionDAG &DAG);

    bool isFoldableLoad(SDOperand Op);
    void EmitFoldedLoad(SDOperand Op, X86AddressMode &AM);


    void EmitCMP(SDOperand LHS, SDOperand RHS);
    bool EmitBranchCC(MachineBasicBlock *Dest, SDOperand Chain, SDOperand Cond);
    void EmitSelectCC(SDOperand Cond, MVT::ValueType SVT,
                      unsigned RTrue, unsigned RFalse, unsigned RDest);
    unsigned SelectExpr(SDOperand N);
    bool SelectAddress(SDOperand N, X86AddressMode &AM);
    void Select(SDOperand N);
  };
}

/// InstructionSelectBasicBlock - This callback is invoked by SelectionDAGISel
/// when it has created a SelectionDAG for us to codegen.
void ISel::InstructionSelectBasicBlock(SelectionDAG &DAG) {
  // While we're doing this, keep track of whether we see any FP code for
  // FP_REG_KILL insertion.
  ContainsFPCode = false;

  // Scan the PHI nodes that already are inserted into this basic block.  If any
  // of them is a PHI of a floating point value, we need to insert an
  // FP_REG_KILL.
  SSARegMap *RegMap = BB->getParent()->getSSARegMap();
  for (MachineBasicBlock::iterator I = BB->begin(), E = BB->end();
       I != E; ++I) {
    assert(I->getOpcode() == X86::PHI &&
           "Isn't just PHI nodes?");
    if (RegMap->getRegClass(I->getOperand(0).getReg()) ==
        X86::RFPRegisterClass) {
      ContainsFPCode = true;
      break;
    }
  }

  // Compute the RegPressureMap, which is an approximation for the number of
  // registers required to compute each node.
  ComputeRegPressure(DAG.getRoot());

  // Codegen the basic block.
  Select(DAG.getRoot());

  // Finally, look at all of the successors of this block.  If any contain a PHI
  // node of FP type, we need to insert an FP_REG_KILL in this block.
  for (MachineBasicBlock::succ_iterator SI = BB->succ_begin(),
         E = BB->succ_end(); SI != E && !ContainsFPCode; ++SI)
    for (MachineBasicBlock::iterator I = (*SI)->begin(), E = (*SI)->end();
         I != E && I->getOpcode() == X86::PHI; ++I) {
      if (RegMap->getRegClass(I->getOperand(0).getReg()) ==
          X86::RFPRegisterClass) {
        ContainsFPCode = true;
        break;
      }
    }
  
  // Insert FP_REG_KILL instructions into basic blocks that need them.  This
  // only occurs due to the floating point stackifier not being aggressive
  // enough to handle arbitrary global stackification.
  //
  // Currently we insert an FP_REG_KILL instruction into each block that uses or
  // defines a floating point virtual register.
  //
  // When the global register allocators (like linear scan) finally update live
  // variable analysis, we can keep floating point values in registers across
  // basic blocks.  This will be a huge win, but we are waiting on the global
  // allocators before we can do this.
  //
  if (ContainsFPCode && BB->succ_size()) {
    BuildMI(*BB, BB->getFirstTerminator(), X86::FP_REG_KILL, 0);
    ++NumFPKill;
  }
  
  // Clear state used for selection.
  ExprMap.clear();
  LoweredTokens.clear();
  RegPressureMap.clear();
}


// ComputeRegPressure - Compute the RegPressureMap, which is an approximation
// for the number of registers required to compute each node.  This is basically
// computing a generalized form of the Sethi-Ullman number for each node.
unsigned ISel::ComputeRegPressure(SDOperand O) {
  SDNode *N = O.Val;
  unsigned &Result = RegPressureMap[N];
  if (Result) return Result;

  // FIXME: Should operations like CALL (which clobber lots o regs) have a
  // higher fixed cost??

  if (N->getNumOperands() == 0) {
    Result = 1;
  } else {
    unsigned MaxRegUse = 0;
    unsigned NumExtraMaxRegUsers = 0;
    for (unsigned i = 0, e = N->getNumOperands(); i != e; ++i) {
      unsigned Regs;
      if (N->getOperand(i).getOpcode() == ISD::Constant)
        Regs = 0;
      else
        Regs = ComputeRegPressure(N->getOperand(i));
      if (Regs > MaxRegUse) {
        MaxRegUse = Regs;
        NumExtraMaxRegUsers = 0;
      } else if (Regs == MaxRegUse &&
                 N->getOperand(i).getValueType() != MVT::Other) {
        ++NumExtraMaxRegUsers;
      }
    }
  
    Result = MaxRegUse+NumExtraMaxRegUsers;
  }

  //std::cerr << " WEIGHT: " << Result << " ";  N->dump(); std::cerr << "\n";
  return Result;
}

/// SelectAddress - Add the specified node to the specified addressing mode,
/// returning true if it cannot be done.
bool ISel::SelectAddress(SDOperand N, X86AddressMode &AM) {
  switch (N.getOpcode()) {
  default: break;
  case ISD::FrameIndex:
    if (AM.BaseType == X86AddressMode::RegBase && AM.Base.Reg == 0) {
      AM.BaseType = X86AddressMode::FrameIndexBase;
      AM.Base.FrameIndex = cast<FrameIndexSDNode>(N)->getIndex();
      return false;
    }
    break;
  case ISD::GlobalAddress:
    if (AM.GV == 0) {
      AM.GV = cast<GlobalAddressSDNode>(N)->getGlobal();
      return false;
    }
    break;
  case ISD::Constant:
    AM.Disp += cast<ConstantSDNode>(N)->getValue();
    return false;
  case ISD::SHL:
    // We might have folded the load into this shift, so don't regen the value
    // if so.
    if (ExprMap.count(N)) break;

    if (AM.IndexReg == 0 && AM.Scale == 1)
      if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N.Val->getOperand(1))) {
        unsigned Val = CN->getValue();
        if (Val == 1 || Val == 2 || Val == 3) {
          AM.Scale = 1 << Val;
          SDOperand ShVal = N.Val->getOperand(0);

          // Okay, we know that we have a scale by now.  However, if the scaled
          // value is an add of something and a constant, we can fold the
          // constant into the disp field here.
          if (ShVal.Val->getOpcode() == ISD::ADD && !ExprMap.count(ShVal) &&
              isa<ConstantSDNode>(ShVal.Val->getOperand(1))) {
            AM.IndexReg = SelectExpr(ShVal.Val->getOperand(0));
            ConstantSDNode *AddVal =
              cast<ConstantSDNode>(ShVal.Val->getOperand(1));
            AM.Disp += AddVal->getValue() << Val;
          } else {
            AM.IndexReg = SelectExpr(ShVal);
          }
          return false;
        }
      }
    break;
  case ISD::MUL:
    // We might have folded the load into this mul, so don't regen the value if
    // so.
    if (ExprMap.count(N)) break;

    // X*[3,5,9] -> X+X*[2,4,8]
    if (AM.IndexReg == 0 && AM.BaseType == X86AddressMode::RegBase &&
        AM.Base.Reg == 0)
      if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N.Val->getOperand(1)))
        if (CN->getValue() == 3 || CN->getValue() == 5 || CN->getValue() == 9) {
          AM.Scale = unsigned(CN->getValue())-1;

          SDOperand MulVal = N.Val->getOperand(0);
          unsigned Reg;

          // Okay, we know that we have a scale by now.  However, if the scaled
          // value is an add of something and a constant, we can fold the
          // constant into the disp field here.
          if (MulVal.Val->getOpcode() == ISD::ADD && !ExprMap.count(MulVal) &&
              isa<ConstantSDNode>(MulVal.Val->getOperand(1))) {
            Reg = SelectExpr(MulVal.Val->getOperand(0));
            ConstantSDNode *AddVal =
              cast<ConstantSDNode>(MulVal.Val->getOperand(1));
            AM.Disp += AddVal->getValue() * CN->getValue();
          } else {          
            Reg = SelectExpr(N.Val->getOperand(0));
          }

          AM.IndexReg = AM.Base.Reg = Reg;
          return false;
        }
    break;

  case ISD::ADD: {
    // We might have folded the load into this mul, so don't regen the value if
    // so.
    if (ExprMap.count(N)) break;

    X86AddressMode Backup = AM;
    if (!SelectAddress(N.Val->getOperand(0), AM) &&
        !SelectAddress(N.Val->getOperand(1), AM))
      return false;
    AM = Backup;
    if (!SelectAddress(N.Val->getOperand(1), AM) &&
        !SelectAddress(N.Val->getOperand(0), AM))
      return false;
    AM = Backup;
    break;
  }
  }

  // Is the base register already occupied?
  if (AM.BaseType != X86AddressMode::RegBase || AM.Base.Reg) {
    // If so, check to see if the scale index register is set.
    if (AM.IndexReg == 0) {
      AM.IndexReg = SelectExpr(N);
      AM.Scale = 1;
      return false;
    }

    // Otherwise, we cannot select it.
    return true;
  }

  // Default, generate it as a register.
  AM.BaseType = X86AddressMode::RegBase;
  AM.Base.Reg = SelectExpr(N);
  return false;
}

/// Emit2SetCCsAndLogical - Emit the following sequence of instructions,
/// assuming that the temporary registers are in the 8-bit register class.
///
///  Tmp1 = setcc1
///  Tmp2 = setcc2
///  DestReg = logicalop Tmp1, Tmp2
///
static void Emit2SetCCsAndLogical(MachineBasicBlock *BB, unsigned SetCC1,
                                  unsigned SetCC2, unsigned LogicalOp,
                                  unsigned DestReg) {
  SSARegMap *RegMap = BB->getParent()->getSSARegMap();
  unsigned Tmp1 = RegMap->createVirtualRegister(X86::R8RegisterClass);
  unsigned Tmp2 = RegMap->createVirtualRegister(X86::R8RegisterClass);
  BuildMI(BB, SetCC1, 0, Tmp1);
  BuildMI(BB, SetCC2, 0, Tmp2);
  BuildMI(BB, LogicalOp, 2, DestReg).addReg(Tmp1).addReg(Tmp2);
}

/// EmitSetCC - Emit the code to set the specified 8-bit register to 1 if the
/// condition codes match the specified SetCCOpcode.  Note that some conditions
/// require multiple instructions to generate the correct value.
static void EmitSetCC(MachineBasicBlock *BB, unsigned DestReg,
                      ISD::CondCode SetCCOpcode, bool isFP) {
  unsigned Opc;
  if (!isFP) {
    switch (SetCCOpcode) {
    default: assert(0 && "Illegal integer SetCC!");
    case ISD::SETEQ: Opc = X86::SETEr; break;
    case ISD::SETGT: Opc = X86::SETGr; break;
    case ISD::SETGE: Opc = X86::SETGEr; break;
    case ISD::SETLT: Opc = X86::SETLr; break;
    case ISD::SETLE: Opc = X86::SETLEr; break;
    case ISD::SETNE: Opc = X86::SETNEr; break;
    case ISD::SETULT: Opc = X86::SETBr; break;
    case ISD::SETUGT: Opc = X86::SETAr; break;
    case ISD::SETULE: Opc = X86::SETBEr; break;
    case ISD::SETUGE: Opc = X86::SETAEr; break;
    }
  } else {
    // On a floating point condition, the flags are set as follows:
    // ZF  PF  CF   op
    //  0 | 0 | 0 | X > Y
    //  0 | 0 | 1 | X < Y
    //  1 | 0 | 0 | X == Y
    //  1 | 1 | 1 | unordered
    //
    switch (SetCCOpcode) {
    default: assert(0 && "Invalid FP setcc!");
    case ISD::SETUEQ:
    case ISD::SETEQ:
      Opc = X86::SETEr;    // True if ZF = 1
      break;
    case ISD::SETOGT:
    case ISD::SETGT:
      Opc = X86::SETAr;    // True if CF = 0 and ZF = 0
      break;
    case ISD::SETOGE:
    case ISD::SETGE:
      Opc = X86::SETAEr;   // True if CF = 0
      break;
    case ISD::SETULT:
    case ISD::SETLT:
      Opc = X86::SETBr;    // True if CF = 1
      break;
    case ISD::SETULE:
    case ISD::SETLE:
      Opc = X86::SETBEr;   // True if CF = 1 or ZF = 1
      break;
    case ISD::SETONE:
    case ISD::SETNE:
      Opc = X86::SETNEr;   // True if ZF = 0
      break;
    case ISD::SETUO:
      Opc = X86::SETPr;    // True if PF = 1
      break;
    case ISD::SETO:
      Opc = X86::SETNPr;   // True if PF = 0
      break;
    case ISD::SETOEQ:      // !PF & ZF
      Emit2SetCCsAndLogical(BB, X86::SETNPr, X86::SETEr, X86::AND8rr, DestReg);
      return;
    case ISD::SETOLT:      // !PF & CF
      Emit2SetCCsAndLogical(BB, X86::SETNPr, X86::SETBr, X86::AND8rr, DestReg);
      return;
    case ISD::SETOLE:      // !PF & (CF || ZF)
      Emit2SetCCsAndLogical(BB, X86::SETNPr, X86::SETBEr, X86::AND8rr, DestReg);
      return;
    case ISD::SETUGT:      // PF | (!ZF & !CF)
      Emit2SetCCsAndLogical(BB, X86::SETPr, X86::SETAr, X86::OR8rr, DestReg);
      return;
    case ISD::SETUGE:      // PF | !CF
      Emit2SetCCsAndLogical(BB, X86::SETPr, X86::SETAEr, X86::OR8rr, DestReg);
      return;
    case ISD::SETUNE:      // PF | !ZF
      Emit2SetCCsAndLogical(BB, X86::SETPr, X86::SETNEr, X86::OR8rr, DestReg);
      return;
    }
  }
  BuildMI(BB, Opc, 0, DestReg);
}


/// EmitBranchCC - Emit code into BB that arranges for control to transfer to
/// the Dest block if the Cond condition is true.  If we cannot fold this
/// condition into the branch, return true.
///
bool ISel::EmitBranchCC(MachineBasicBlock *Dest, SDOperand Chain,
                        SDOperand Cond) {
  // FIXME: Evaluate whether it would be good to emit code like (X < Y) | (A >
  // B) using two conditional branches instead of one condbr, two setcc's, and
  // an or.
  if ((Cond.getOpcode() == ISD::OR ||
       Cond.getOpcode() == ISD::AND) && Cond.Val->hasOneUse()) {
    // And and or set the flags for us, so there is no need to emit a TST of the
    // result.  It is only safe to do this if there is only a single use of the
    // AND/OR though, otherwise we don't know it will be emitted here.
    Select(Chain);
    SelectExpr(Cond);
    BuildMI(BB, X86::JNE, 1).addMBB(Dest);
    return false;
  }

  // Codegen br not C -> JE.
  if (Cond.getOpcode() == ISD::XOR)
    if (ConstantSDNode *NC = dyn_cast<ConstantSDNode>(Cond.Val->getOperand(1)))
      if (NC->isAllOnesValue()) {
        unsigned CondR;
        if (getRegPressure(Chain) > getRegPressure(Cond)) {
          Select(Chain);
          CondR = SelectExpr(Cond.Val->getOperand(0));
        } else {
          CondR = SelectExpr(Cond.Val->getOperand(0));
          Select(Chain);
        }
        BuildMI(BB, X86::TEST8rr, 2).addReg(CondR).addReg(CondR);
        BuildMI(BB, X86::JE, 1).addMBB(Dest);
        return false;
      }

  SetCCSDNode *SetCC = dyn_cast<SetCCSDNode>(Cond);
  if (SetCC == 0)
    return true;                       // Can only handle simple setcc's so far.

  unsigned Opc;

  // Handle integer conditions first.
  if (MVT::isInteger(SetCC->getOperand(0).getValueType())) {
    switch (SetCC->getCondition()) {
    default: assert(0 && "Illegal integer SetCC!");
    case ISD::SETEQ: Opc = X86::JE; break;
    case ISD::SETGT: Opc = X86::JG; break;
    case ISD::SETGE: Opc = X86::JGE; break;
    case ISD::SETLT: Opc = X86::JL; break;
    case ISD::SETLE: Opc = X86::JLE; break;
    case ISD::SETNE: Opc = X86::JNE; break;
    case ISD::SETULT: Opc = X86::JB; break;
    case ISD::SETUGT: Opc = X86::JA; break;
    case ISD::SETULE: Opc = X86::JBE; break;
    case ISD::SETUGE: Opc = X86::JAE; break;
    }
    Select(Chain);
    EmitCMP(SetCC->getOperand(0), SetCC->getOperand(1));
    BuildMI(BB, Opc, 1).addMBB(Dest);
    return false;
  }

  unsigned Opc2 = 0;  // Second branch if needed.

  // On a floating point condition, the flags are set as follows:
  // ZF  PF  CF   op
  //  0 | 0 | 0 | X > Y
  //  0 | 0 | 1 | X < Y
  //  1 | 0 | 0 | X == Y
  //  1 | 1 | 1 | unordered
  //
  switch (SetCC->getCondition()) {
  default: assert(0 && "Invalid FP setcc!");
  case ISD::SETUEQ:
  case ISD::SETEQ:   Opc = X86::JE;  break;     // True if ZF = 1
  case ISD::SETOGT:
  case ISD::SETGT:   Opc = X86::JA;  break;     // True if CF = 0 and ZF = 0
  case ISD::SETOGE:
  case ISD::SETGE:   Opc = X86::JAE; break;     // True if CF = 0
  case ISD::SETULT:
  case ISD::SETLT:   Opc = X86::JB;  break;     // True if CF = 1
  case ISD::SETULE:
  case ISD::SETLE:   Opc = X86::JBE; break;     // True if CF = 1 or ZF = 1
  case ISD::SETONE:
  case ISD::SETNE:   Opc = X86::JNE; break;     // True if ZF = 0
  case ISD::SETUO:   Opc = X86::JP;  break;     // True if PF = 1
  case ISD::SETO:    Opc = X86::JNP; break;     // True if PF = 0
  case ISD::SETUGT:      // PF = 1 | (ZF = 0 & CF = 0)
    Opc = X86::JA;       // ZF = 0 & CF = 0
    Opc2 = X86::JP;      // PF = 1
    break;
  case ISD::SETUGE:      // PF = 1 | CF = 0
    Opc = X86::JAE;      // CF = 0
    Opc2 = X86::JP;      // PF = 1
    break;
  case ISD::SETUNE:      // PF = 1 | ZF = 0
    Opc = X86::JNE;      // ZF = 0
    Opc2 = X86::JP;      // PF = 1
    break;
  case ISD::SETOEQ:      // PF = 0 & ZF = 1
    //X86::JNP, X86::JE
    //X86::AND8rr
    return true;    // FIXME: Emit more efficient code for this branch.
  case ISD::SETOLT:      // PF = 0 & CF = 1
    //X86::JNP, X86::JB
    //X86::AND8rr
    return true;    // FIXME: Emit more efficient code for this branch.
  case ISD::SETOLE:      // PF = 0 & (CF = 1 || ZF = 1)
    //X86::JNP, X86::JBE
    //X86::AND8rr
    return true;    // FIXME: Emit more efficient code for this branch.
  }

  Select(Chain);
  EmitCMP(SetCC->getOperand(0), SetCC->getOperand(1));
  BuildMI(BB, Opc, 1).addMBB(Dest);
  if (Opc2)
    BuildMI(BB, Opc2, 1).addMBB(Dest);
  return false;
}

/// EmitSelectCC - Emit code into BB that performs a select operation between
/// the two registers RTrue and RFalse, generating a result into RDest.  Return
/// true if the fold cannot be performed.
///
void ISel::EmitSelectCC(SDOperand Cond, MVT::ValueType SVT,
                        unsigned RTrue, unsigned RFalse, unsigned RDest) {
  enum Condition {
    EQ, NE, LT, LE, GT, GE, B, BE, A, AE, P, NP,
    NOT_SET
  } CondCode = NOT_SET;

  static const unsigned CMOVTAB16[] = {
    X86::CMOVE16rr,  X86::CMOVNE16rr, X86::CMOVL16rr,  X86::CMOVLE16rr,
    X86::CMOVG16rr,  X86::CMOVGE16rr, X86::CMOVB16rr,  X86::CMOVBE16rr,
    X86::CMOVA16rr,  X86::CMOVAE16rr, X86::CMOVP16rr,  X86::CMOVNP16rr, 
  };
  static const unsigned CMOVTAB32[] = {
    X86::CMOVE32rr,  X86::CMOVNE32rr, X86::CMOVL32rr,  X86::CMOVLE32rr,
    X86::CMOVG32rr,  X86::CMOVGE32rr, X86::CMOVB32rr,  X86::CMOVBE32rr,
    X86::CMOVA32rr,  X86::CMOVAE32rr, X86::CMOVP32rr,  X86::CMOVNP32rr, 
  };
  static const unsigned CMOVTABFP[] = {
    X86::FCMOVE ,  X86::FCMOVNE, /*missing*/0, /*missing*/0,
    /*missing*/0,  /*missing*/0, X86::FCMOVB , X86::FCMOVBE,
    X86::FCMOVA ,  X86::FCMOVAE, X86::FCMOVP , X86::FCMOVNP
  };

  if (SetCCSDNode *SetCC = dyn_cast<SetCCSDNode>(Cond)) {
    if (MVT::isInteger(SetCC->getOperand(0).getValueType())) {
      switch (SetCC->getCondition()) {
      default: assert(0 && "Unknown integer comparison!");
      case ISD::SETEQ:  CondCode = EQ; break;
      case ISD::SETGT:  CondCode = GT; break;
      case ISD::SETGE:  CondCode = GE; break;
      case ISD::SETLT:  CondCode = LT; break;
      case ISD::SETLE:  CondCode = LE; break;
      case ISD::SETNE:  CondCode = NE; break;
      case ISD::SETULT: CondCode = B; break;
      case ISD::SETUGT: CondCode = A; break;
      case ISD::SETULE: CondCode = BE; break;
      case ISD::SETUGE: CondCode = AE; break;
      }
    } else {
      // On a floating point condition, the flags are set as follows:
      // ZF  PF  CF   op
      //  0 | 0 | 0 | X > Y
      //  0 | 0 | 1 | X < Y
      //  1 | 0 | 0 | X == Y
      //  1 | 1 | 1 | unordered
      //
      switch (SetCC->getCondition()) {
      default: assert(0 && "Unknown FP comparison!");
      case ISD::SETUEQ:
      case ISD::SETEQ:  CondCode = EQ; break;     // True if ZF = 1
      case ISD::SETOGT:
      case ISD::SETGT:  CondCode = A;  break;     // True if CF = 0 and ZF = 0
      case ISD::SETOGE:
      case ISD::SETGE:  CondCode = AE; break;     // True if CF = 0
      case ISD::SETULT:
      case ISD::SETLT:  CondCode = B;  break;     // True if CF = 1
      case ISD::SETULE:
      case ISD::SETLE:  CondCode = BE; break;     // True if CF = 1 or ZF = 1
      case ISD::SETONE:
      case ISD::SETNE:  CondCode = NE; break;     // True if ZF = 0
      case ISD::SETUO:  CondCode = P;  break;     // True if PF = 1
      case ISD::SETO:   CondCode = NP; break;     // True if PF = 0
      case ISD::SETUGT:      // PF = 1 | (ZF = 0 & CF = 0)
      case ISD::SETUGE:      // PF = 1 | CF = 0
      case ISD::SETUNE:      // PF = 1 | ZF = 0
      case ISD::SETOEQ:      // PF = 0 & ZF = 1
      case ISD::SETOLT:      // PF = 0 & CF = 1
      case ISD::SETOLE:      // PF = 0 & (CF = 1 || ZF = 1)
        // We cannot emit this comparison as a single cmov.
        break;
      }
    }
  }

  unsigned Opc = 0;
  if (CondCode != NOT_SET) {
    switch (SVT) {
    default: assert(0 && "Cannot select this type!");
    case MVT::i16: Opc = CMOVTAB16[CondCode]; break;
    case MVT::i32: Opc = CMOVTAB32[CondCode]; break;
    case MVT::f32:
    case MVT::f64: Opc = CMOVTABFP[CondCode]; break;
    }
  }

  // Finally, if we weren't able to fold this, just emit the condition and test
  // it.
  if (CondCode == NOT_SET || Opc == 0) {
    // Get the condition into the zero flag.
    unsigned CondReg = SelectExpr(Cond);
    BuildMI(BB, X86::TEST8rr, 2).addReg(CondReg).addReg(CondReg);

    switch (SVT) {
    default: assert(0 && "Cannot select this type!");
    case MVT::i16: Opc = X86::CMOVE16rr; break;
    case MVT::i32: Opc = X86::CMOVE32rr; break;
    case MVT::f32:
    case MVT::f64: Opc = X86::FCMOVE; break;
    }
  } else {
    // FIXME: CMP R, 0 -> TEST R, R
    EmitCMP(Cond.getOperand(0), Cond.getOperand(1));
    std::swap(RTrue, RFalse);
  }
  BuildMI(BB, Opc, 2, RDest).addReg(RTrue).addReg(RFalse);
}

void ISel::EmitCMP(SDOperand LHS, SDOperand RHS) {
  unsigned Opc;
  if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(RHS)) {
    Opc = 0;
    if (isFoldableLoad(LHS)) {
      switch (RHS.getValueType()) {
      default: break;
      case MVT::i1:
      case MVT::i8:  Opc = X86::CMP8mi;  break;
      case MVT::i16: Opc = X86::CMP16mi; break;
      case MVT::i32: Opc = X86::CMP32mi; break;
      }
      if (Opc) {
        X86AddressMode AM;
        EmitFoldedLoad(LHS, AM);
        addFullAddress(BuildMI(BB, Opc, 5), AM).addImm(CN->getValue());
        return;
      }
    }

    switch (RHS.getValueType()) {
    default: break;
    case MVT::i1:
    case MVT::i8:  Opc = X86::CMP8ri;  break;
    case MVT::i16: Opc = X86::CMP16ri; break;
    case MVT::i32: Opc = X86::CMP32ri; break;
    }
    if (Opc) {
      unsigned Tmp1 = SelectExpr(LHS);
      BuildMI(BB, Opc, 2).addReg(Tmp1).addImm(CN->getValue());
      return;
    }
  }

  Opc = 0;
  if (isFoldableLoad(LHS)) {
    switch (RHS.getValueType()) {
    default: break;
    case MVT::i1:
    case MVT::i8:  Opc = X86::CMP8mr;  break;
    case MVT::i16: Opc = X86::CMP16mr; break;
    case MVT::i32: Opc = X86::CMP32mr; break;
    }
    if (Opc) {
      X86AddressMode AM;
      EmitFoldedLoad(LHS, AM);
      unsigned Reg = SelectExpr(RHS);
      addFullAddress(BuildMI(BB, Opc, 5), AM).addReg(Reg);
      return;
    }
  }

  switch (LHS.getValueType()) {
  default: assert(0 && "Cannot compare this value!");
  case MVT::i1:
  case MVT::i8:  Opc = X86::CMP8rr;  break;
  case MVT::i16: Opc = X86::CMP16rr; break;
  case MVT::i32: Opc = X86::CMP32rr; break;
  case MVT::f32:
  case MVT::f64: Opc = X86::FUCOMIr; break;
  }
  unsigned Tmp1, Tmp2;
  if (getRegPressure(LHS) > getRegPressure(RHS)) {
    Tmp1 = SelectExpr(LHS);
    Tmp2 = SelectExpr(RHS);
  } else {
    Tmp2 = SelectExpr(RHS);
    Tmp1 = SelectExpr(LHS);
  }
  BuildMI(BB, Opc, 2).addReg(Tmp1).addReg(Tmp2);
}

/// isFoldableLoad - Return true if this is a load instruction that can safely
/// be folded into an operation that uses it.
bool ISel::isFoldableLoad(SDOperand Op) {
  if (Op.getOpcode() != ISD::LOAD ||
      // FIXME: currently can't fold constant pool indexes.
      isa<ConstantPoolSDNode>(Op.getOperand(1)))
    return false;

  // If this load has already been emitted, we clearly can't fold it.
  assert(Op.ResNo == 0 && "Not a use of the value of the load?");
  if (ExprMap.count(Op.getValue(1))) return false;
  assert(!ExprMap.count(Op.getValue(0)) && "Value in map but not token chain?");
  assert(!LoweredTokens.count(Op.getValue(1)) &&
         "Token lowered but value not in map?");

  // Finally, there can only be one use of its value.
  return Op.Val->hasNUsesOfValue(1, 0);
}

/// EmitFoldedLoad - Ensure that the arguments of the load are code generated,
/// and compute the address being loaded into AM.
void ISel::EmitFoldedLoad(SDOperand Op, X86AddressMode &AM) {
  SDOperand Chain   = Op.getOperand(0);
  SDOperand Address = Op.getOperand(1);
  if (getRegPressure(Chain) > getRegPressure(Address)) {
    Select(Chain);
    SelectAddress(Address, AM);
  } else {
    SelectAddress(Address, AM);
    Select(Chain);
  }

  // The chain for this load is now lowered.
  assert(ExprMap.count(SDOperand(Op.Val, 1)) == 0 &&
         "Load emitted more than once?");
  ExprMap[SDOperand(Op.Val, 1)] = 1;
  if (!LoweredTokens.insert(Op.getValue(1)).second)
    assert(0 && "Load emitted more than once!");
}

unsigned ISel::SelectExpr(SDOperand N) {
  unsigned Result;
  unsigned Tmp1, Tmp2, Tmp3;
  unsigned Opc = 0;
  SDNode *Node = N.Val;
  SDOperand Op0, Op1;

  if (Node->getOpcode() == ISD::CopyFromReg)
    // Just use the specified register as our input.
    return dyn_cast<RegSDNode>(Node)->getReg();
  
  unsigned &Reg = ExprMap[N];
  if (Reg) return Reg;
  
  if (N.getOpcode() != ISD::CALL)
    Reg = Result = (N.getValueType() != MVT::Other) ?
      MakeReg(N.getValueType()) : 1;
  else {
    // If this is a call instruction, make sure to prepare ALL of the result
    // values as well as the chain.
    if (Node->getNumValues() == 1)
      Reg = Result = 1;  // Void call, just a chain.
    else {
      Result = MakeReg(Node->getValueType(0));
      ExprMap[N.getValue(0)] = Result;
      for (unsigned i = 1, e = N.Val->getNumValues()-1; i != e; ++i)
        ExprMap[N.getValue(i)] = MakeReg(Node->getValueType(i));
      ExprMap[SDOperand(Node, Node->getNumValues()-1)] = 1;
    }
  }
  
  switch (N.getOpcode()) {
  default:
    Node->dump();
    assert(0 && "Node not handled!\n");
  case ISD::FrameIndex:
    Tmp1 = cast<FrameIndexSDNode>(N)->getIndex();
    addFrameReference(BuildMI(BB, X86::LEA32r, 4, Result), (int)Tmp1);
    return Result;
  case ISD::ConstantPool:
    Tmp1 = cast<ConstantPoolSDNode>(N)->getIndex();
    addConstantPoolReference(BuildMI(BB, X86::LEA32r, 4, Result), Tmp1);
    return Result;
  case ISD::ConstantFP:
    ContainsFPCode = true;
    Tmp1 = Result;   // Intermediate Register
    if (cast<ConstantFPSDNode>(N)->getValue() < 0.0 ||
        cast<ConstantFPSDNode>(N)->isExactlyValue(-0.0))
      Tmp1 = MakeReg(MVT::f64);

    if (cast<ConstantFPSDNode>(N)->isExactlyValue(+0.0) ||
        cast<ConstantFPSDNode>(N)->isExactlyValue(-0.0))
      BuildMI(BB, X86::FLD0, 0, Tmp1);
    else if (cast<ConstantFPSDNode>(N)->isExactlyValue(+1.0) ||
             cast<ConstantFPSDNode>(N)->isExactlyValue(-1.0))
      BuildMI(BB, X86::FLD1, 0, Tmp1);
    else
      assert(0 && "Unexpected constant!");
    if (Tmp1 != Result)
      BuildMI(BB, X86::FCHS, 1, Result).addReg(Tmp1);
    return Result;
  case ISD::Constant:
    switch (N.getValueType()) {
    default: assert(0 && "Cannot use constants of this type!");
    case MVT::i1:
    case MVT::i8:  Opc = X86::MOV8ri;  break;
    case MVT::i16: Opc = X86::MOV16ri; break;
    case MVT::i32: Opc = X86::MOV32ri; break;
    }
    BuildMI(BB, Opc, 1,Result).addImm(cast<ConstantSDNode>(N)->getValue());
    return Result;
  case ISD::GlobalAddress: {
    GlobalValue *GV = cast<GlobalAddressSDNode>(N)->getGlobal();
    BuildMI(BB, X86::MOV32ri, 1, Result).addGlobalAddress(GV);
    return Result;
  }
  case ISD::ExternalSymbol: {
    const char *Sym = cast<ExternalSymbolSDNode>(N)->getSymbol();
    BuildMI(BB, X86::MOV32ri, 1, Result).addExternalSymbol(Sym);
    return Result;
  }
  case ISD::FP_EXTEND:
    Tmp1 = SelectExpr(N.getOperand(0));
    BuildMI(BB, X86::FpMOV, 1, Result).addReg(Tmp1);
    return Result;
  case ISD::ZERO_EXTEND: {
    int DestIs16 = N.getValueType() == MVT::i16;
    int SrcIs16  = N.getOperand(0).getValueType() == MVT::i16;

    // FIXME: This hack is here for zero extension casts from bool to i8.  This
    // would not be needed if bools were promoted by Legalize.
    if (N.getValueType() == MVT::i8) {
      Tmp1 = SelectExpr(N.getOperand(0));
      BuildMI(BB, X86::MOV8rr, 1, Result).addReg(Tmp1);
      return Result;
    }

    if (isFoldableLoad(N.getOperand(0))) {
      static const unsigned Opc[3] = {
        X86::MOVZX32rm8, X86::MOVZX32rm16, X86::MOVZX16rm8
      };

      X86AddressMode AM;
      EmitFoldedLoad(N.getOperand(0), AM);
      addFullAddress(BuildMI(BB, Opc[SrcIs16+DestIs16*2], 4, Result), AM);
                             
      return Result;
    }

    static const unsigned Opc[3] = {
      X86::MOVZX32rr8, X86::MOVZX32rr16, X86::MOVZX16rr8
    };
    Tmp1 = SelectExpr(N.getOperand(0));
    BuildMI(BB, Opc[SrcIs16+DestIs16*2], 1, Result).addReg(Tmp1);
    return Result;
  }    
  case ISD::SIGN_EXTEND: {
    int DestIs16 = N.getValueType() == MVT::i16;
    int SrcIs16  = N.getOperand(0).getValueType() == MVT::i16;

    // FIXME: Legalize should promote bools to i8!
    assert(N.getOperand(0).getValueType() != MVT::i1 &&
           "Sign extend from bool not implemented!");

   if (isFoldableLoad(N.getOperand(0))) {
      static const unsigned Opc[3] = {
        X86::MOVSX32rm8, X86::MOVSX32rm16, X86::MOVSX16rm8
      };

      X86AddressMode AM;
      EmitFoldedLoad(N.getOperand(0), AM);
      addFullAddress(BuildMI(BB, Opc[SrcIs16+DestIs16*2], 4, Result), AM);
      return Result;
    }

    static const unsigned Opc[3] = {
      X86::MOVSX32rr8, X86::MOVSX32rr16, X86::MOVSX16rr8
    };
    Tmp1 = SelectExpr(N.getOperand(0));
    BuildMI(BB, Opc[SrcIs16+DestIs16*2], 1, Result).addReg(Tmp1);
    return Result;
  }
  case ISD::TRUNCATE:
    // Fold TRUNCATE (LOAD P) into a smaller load from P.
    if (isFoldableLoad(N.getOperand(0))) {
      switch (N.getValueType()) {
      default: assert(0 && "Unknown truncate!");
      case MVT::i1:
      case MVT::i8:  Opc = X86::MOV8rm;  break;
      case MVT::i16: Opc = X86::MOV16rm; break;
      }
      X86AddressMode AM;
      EmitFoldedLoad(N.getOperand(0), AM);
      addFullAddress(BuildMI(BB, Opc, 4, Result), AM);
      return Result;
    }

    // Handle cast of LARGER int to SMALLER int using a move to EAX followed by
    // a move out of AX or AL.
    switch (N.getOperand(0).getValueType()) {
    default: assert(0 && "Unknown truncate!");
    case MVT::i8:  Tmp2 = X86::AL;  Opc = X86::MOV8rr;  break;
    case MVT::i16: Tmp2 = X86::AX;  Opc = X86::MOV16rr; break;
    case MVT::i32: Tmp2 = X86::EAX; Opc = X86::MOV32rr; break;
    }
    Tmp1 = SelectExpr(N.getOperand(0));
    BuildMI(BB, Opc, 1, Tmp2).addReg(Tmp1);

    switch (N.getValueType()) {
    default: assert(0 && "Unknown truncate!");
    case MVT::i1:
    case MVT::i8:  Tmp2 = X86::AL;  Opc = X86::MOV8rr;  break;
    case MVT::i16: Tmp2 = X86::AX;  Opc = X86::MOV16rr; break;
    }
    BuildMI(BB, Opc, 1, Result).addReg(Tmp2);
    return Result;

  case ISD::FP_ROUND:
    // Truncate from double to float by storing to memory as float,
    // then reading it back into a register.

    // Create as stack slot to use.
    // FIXME: This should automatically be made by the Legalizer!
    Tmp1 = TLI.getTargetData().getFloatAlignment();
    Tmp2 = BB->getParent()->getFrameInfo()->CreateStackObject(4, Tmp1);

    // Codegen the input.
    Tmp1 = SelectExpr(N.getOperand(0));

    // Emit the store, then the reload.
    addFrameReference(BuildMI(BB, X86::FST32m, 5), Tmp2).addReg(Tmp1);
    addFrameReference(BuildMI(BB, X86::FLD32m, 5, Result), Tmp2);
    return Result;

  case ISD::SINT_TO_FP:
  case ISD::UINT_TO_FP: {
    // FIXME: Most of this grunt work should be done by legalize!
    ContainsFPCode = true;

    // Promote the integer to a type supported by FLD.  We do this because there
    // are no unsigned FLD instructions, so we must promote an unsigned value to
    // a larger signed value, then use FLD on the larger value.
    //
    MVT::ValueType PromoteType = MVT::Other;
    MVT::ValueType SrcTy = N.getOperand(0).getValueType();
    unsigned PromoteOpcode = 0;
    unsigned RealDestReg = Result;
    switch (SrcTy) {
    case MVT::i1:
    case MVT::i8:
      // We don't have the facilities for directly loading byte sized data from
      // memory (even signed).  Promote it to 16 bits.
      PromoteType = MVT::i16;
      PromoteOpcode = Node->getOpcode() == ISD::SINT_TO_FP ?
        X86::MOVSX16rr8 : X86::MOVZX16rr8;
      break;
    case MVT::i16:
      if (Node->getOpcode() == ISD::UINT_TO_FP) {
        PromoteType = MVT::i32;
        PromoteOpcode = X86::MOVZX32rr16;
      }
      break;
    default:
      // Don't fild into the real destination.
      if (Node->getOpcode() == ISD::UINT_TO_FP)
        Result = MakeReg(Node->getValueType(0));
      break;
    }

    Tmp1 = SelectExpr(N.getOperand(0));  // Get the operand register
    
    if (PromoteType != MVT::Other) {
      Tmp2 = MakeReg(PromoteType);
      BuildMI(BB, PromoteOpcode, 1, Tmp2).addReg(Tmp1);
      SrcTy = PromoteType;
      Tmp1 = Tmp2;
    }

    // Spill the integer to memory and reload it from there.
    unsigned Size = MVT::getSizeInBits(SrcTy)/8;
    MachineFunction *F = BB->getParent();
    int FrameIdx = F->getFrameInfo()->CreateStackObject(Size, Size);

    switch (SrcTy) {
    case MVT::i64:
      assert(0 && "Cast ulong to FP not implemented yet!");
      // FIXME: this won't work for cast [u]long to FP
      addFrameReference(BuildMI(BB, X86::MOV32mr, 5),
                        FrameIdx).addReg(Tmp1);
      addFrameReference(BuildMI(BB, X86::MOV32mr, 5),
                        FrameIdx, 4).addReg(Tmp1+1);
      addFrameReference(BuildMI(BB, X86::FILD64m, 5, Result), FrameIdx);
      break;
    case MVT::i32:
      addFrameReference(BuildMI(BB, X86::MOV32mr, 5),
                        FrameIdx).addReg(Tmp1);
      addFrameReference(BuildMI(BB, X86::FILD32m, 5, Result), FrameIdx);
      break;
    case MVT::i16:
      addFrameReference(BuildMI(BB, X86::MOV16mr, 5),
                        FrameIdx).addReg(Tmp1);
      addFrameReference(BuildMI(BB, X86::FILD16m, 5, Result), FrameIdx);
      break;
    default: break; // No promotion required.
    }

    if (Node->getOpcode() == ISD::UINT_TO_FP && Result != RealDestReg) {
      // If this is a cast from uint -> double, we need to be careful when if
      // the "sign" bit is set.  If so, we don't want to make a negative number,
      // we want to make a positive number.  Emit code to add an offset if the
      // sign bit is set.

      // Compute whether the sign bit is set by shifting the reg right 31 bits.
      unsigned IsNeg = MakeReg(MVT::i32);
      BuildMI(BB, X86::SHR32ri, 2, IsNeg).addReg(Tmp1).addImm(31);

      // Create a CP value that has the offset in one word and 0 in the other.
      static ConstantInt *TheOffset = ConstantUInt::get(Type::ULongTy,
                                                        0x4f80000000000000ULL);
      unsigned CPI = F->getConstantPool()->getConstantPoolIndex(TheOffset);
      BuildMI(BB, X86::FADD32m, 5, RealDestReg).addReg(Result)
        .addConstantPoolIndex(CPI).addZImm(4).addReg(IsNeg).addSImm(0);

    } else if (Node->getOpcode() == ISD::UINT_TO_FP && SrcTy == MVT::i64) {
      // We need special handling for unsigned 64-bit integer sources.  If the
      // input number has the "sign bit" set, then we loaded it incorrectly as a
      // negative 64-bit number.  In this case, add an offset value.

      // Emit a test instruction to see if the dynamic input value was signed.
      BuildMI(BB, X86::TEST32rr, 2).addReg(Tmp1+1).addReg(Tmp1+1);

      // If the sign bit is set, get a pointer to an offset, otherwise get a
      // pointer to a zero.
      MachineConstantPool *CP = F->getConstantPool();
      unsigned Zero = MakeReg(MVT::i32);
      Constant *Null = Constant::getNullValue(Type::UIntTy);
      addConstantPoolReference(BuildMI(BB, X86::LEA32r, 5, Zero), 
                               CP->getConstantPoolIndex(Null));
      unsigned Offset = MakeReg(MVT::i32);
      Constant *OffsetCst = ConstantUInt::get(Type::UIntTy, 0x5f800000);
                                             
      addConstantPoolReference(BuildMI(BB, X86::LEA32r, 5, Offset),
                               CP->getConstantPoolIndex(OffsetCst));
      unsigned Addr = MakeReg(MVT::i32);
      BuildMI(BB, X86::CMOVS32rr, 2, Addr).addReg(Zero).addReg(Offset);

      // Load the constant for an add.  FIXME: this could make an 'fadd' that
      // reads directly from memory, but we don't support these yet.
      unsigned ConstReg = MakeReg(MVT::f64);
      addDirectMem(BuildMI(BB, X86::FLD32m, 4, ConstReg), Addr);

      BuildMI(BB, X86::FpADD, 2, RealDestReg).addReg(ConstReg).addReg(Result);
    }
    return RealDestReg;
  }
  case ISD::FP_TO_SINT:
  case ISD::FP_TO_UINT: {
    // FIXME: Most of this grunt work should be done by legalize!
    Tmp1 = SelectExpr(N.getOperand(0));  // Get the operand register

    // Change the floating point control register to use "round towards zero"
    // mode when truncating to an integer value.
    //
    MachineFunction *F = BB->getParent();
    int CWFrameIdx = F->getFrameInfo()->CreateStackObject(2, 2);
    addFrameReference(BuildMI(BB, X86::FNSTCW16m, 4), CWFrameIdx);

    // Load the old value of the high byte of the control word...
    unsigned HighPartOfCW = MakeReg(MVT::i8);
    addFrameReference(BuildMI(BB, X86::MOV8rm, 4, HighPartOfCW),
                      CWFrameIdx, 1);

    // Set the high part to be round to zero...
    addFrameReference(BuildMI(BB, X86::MOV8mi, 5),
                      CWFrameIdx, 1).addImm(12);

    // Reload the modified control word now...
    addFrameReference(BuildMI(BB, X86::FLDCW16m, 4), CWFrameIdx);
    
    // Restore the memory image of control word to original value
    addFrameReference(BuildMI(BB, X86::MOV8mr, 5),
                      CWFrameIdx, 1).addReg(HighPartOfCW);

    // We don't have the facilities for directly storing byte sized data to
    // memory.  Promote it to 16 bits.  We also must promote unsigned values to
    // larger classes because we only have signed FP stores.
    MVT::ValueType StoreClass = Node->getValueType(0);
    if (StoreClass == MVT::i8 || Node->getOpcode() == ISD::FP_TO_UINT)
      switch (StoreClass) {
      case MVT::i8:  StoreClass = MVT::i16; break;
      case MVT::i16: StoreClass = MVT::i32; break;
      case MVT::i32: StoreClass = MVT::i64; break;
        // The following treatment of cLong may not be perfectly right,
        // but it survives chains of casts of the form
        // double->ulong->double.
      case MVT::i64:  StoreClass = MVT::i64;  break;
      default: assert(0 && "Unknown store class!");
      }

    // Spill the integer to memory and reload it from there.
    unsigned Size = MVT::getSizeInBits(StoreClass)/8;
    int FrameIdx = F->getFrameInfo()->CreateStackObject(Size, Size);

    switch (StoreClass) {
    default: assert(0 && "Unknown store class!");
    case MVT::i16:
      addFrameReference(BuildMI(BB, X86::FIST16m, 5), FrameIdx).addReg(Tmp1);
      break;
    case MVT::i32:
      addFrameReference(BuildMI(BB, X86::FIST32m, 5), FrameIdx).addReg(Tmp1);
      break;
    case MVT::i64:
      addFrameReference(BuildMI(BB, X86::FISTP64m, 5), FrameIdx).addReg(Tmp1);
      break;
    }

    switch (Node->getValueType(0)) {
    default:
      assert(0 && "Unknown integer type!");
    case MVT::i64:
      // FIXME: this isn't gunna work.
      assert(0 && "Cast FP to long not implemented yet!");
      addFrameReference(BuildMI(BB, X86::MOV32rm, 4, Result), FrameIdx);
      addFrameReference(BuildMI(BB, X86::MOV32rm, 4, Result+1), FrameIdx, 4);
    case MVT::i32:
      addFrameReference(BuildMI(BB, X86::MOV32rm, 4, Result), FrameIdx);
      break;
    case MVT::i16:
      addFrameReference(BuildMI(BB, X86::MOV16rm, 4, Result), FrameIdx);
      break;
    case MVT::i8:
      addFrameReference(BuildMI(BB, X86::MOV8rm, 4, Result), FrameIdx);
      break;
    }

    // Reload the original control word now.
    addFrameReference(BuildMI(BB, X86::FLDCW16m, 4), CWFrameIdx);
    return Result;
  }
  case ISD::ADD:
    Op0 = N.getOperand(0);
    Op1 = N.getOperand(1);

    if (isFoldableLoad(Op0))
      std::swap(Op0, Op1);

    if (isFoldableLoad(Op1)) {
      switch (N.getValueType()) {
      default: assert(0 && "Cannot add this type!");
      case MVT::i1:
      case MVT::i8:  Opc = X86::ADD8rm;  break;
      case MVT::i16: Opc = X86::ADD16rm; break;
      case MVT::i32: Opc = X86::ADD32rm; break;
      case MVT::f32: Opc = X86::FADD32m; break;
      case MVT::f64: Opc = X86::FADD64m; break;
      }
      X86AddressMode AM;
      EmitFoldedLoad(Op1, AM);
      Tmp1 = SelectExpr(Op0);
      addFullAddress(BuildMI(BB, Opc, 5, Result).addReg(Tmp1), AM);
      return Result;
    }

    // See if we can codegen this as an LEA to fold operations together.
    if (N.getValueType() == MVT::i32) {
      X86AddressMode AM;
      if (!SelectAddress(Op0, AM) && !SelectAddress(Op1, AM)) {
	// If this is not just an add, emit the LEA.  For a simple add (like
	// reg+reg or reg+imm), we just emit an add.  It might be a good idea to
	// leave this as LEA, then peephole it to 'ADD' after two address elim
	// happens.
        if (AM.Scale != 1 || AM.BaseType == X86AddressMode::FrameIndexBase ||
            AM.GV || (AM.Base.Reg && AM.IndexReg && AM.Disp)) {
          addFullAddress(BuildMI(BB, X86::LEA32r, 4, Result), AM);
          return Result;
        }
      }
    }

    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Op1)) {
      Opc = 0;
      if (CN->getValue() == 1) {   // add X, 1 -> inc X
        switch (N.getValueType()) {
        default: assert(0 && "Cannot integer add this type!");
        case MVT::i8:  Opc = X86::INC8r; break;
        case MVT::i16: Opc = X86::INC16r; break;
        case MVT::i32: Opc = X86::INC32r; break;
        }
      } else if (CN->isAllOnesValue()) { // add X, -1 -> dec X
        switch (N.getValueType()) {
        default: assert(0 && "Cannot integer add this type!");
        case MVT::i8:  Opc = X86::DEC8r; break;
        case MVT::i16: Opc = X86::DEC16r; break;
        case MVT::i32: Opc = X86::DEC32r; break;
        }
      }

      if (Opc) {
        Tmp1 = SelectExpr(Op0);
        BuildMI(BB, Opc, 1, Result).addReg(Tmp1);
        return Result;
      }

      switch (N.getValueType()) {
      default: assert(0 && "Cannot add this type!");
      case MVT::i8:  Opc = X86::ADD8ri; break;
      case MVT::i16: Opc = X86::ADD16ri; break;
      case MVT::i32: Opc = X86::ADD32ri; break;
      }
      if (Opc) {
        Tmp1 = SelectExpr(Op0);
        BuildMI(BB, Opc, 2, Result).addReg(Tmp1).addImm(CN->getValue());
        return Result;
      }
    }

    switch (N.getValueType()) {
    default: assert(0 && "Cannot add this type!");
    case MVT::i8:  Opc = X86::ADD8rr; break;
    case MVT::i16: Opc = X86::ADD16rr; break;
    case MVT::i32: Opc = X86::ADD32rr; break;
    case MVT::f32: 
    case MVT::f64: Opc = X86::FpADD; break;
    }

    if (getRegPressure(Op0) > getRegPressure(Op1)) {
      Tmp1 = SelectExpr(Op0);
      Tmp2 = SelectExpr(Op1);
    } else {
      Tmp2 = SelectExpr(Op1);
      Tmp1 = SelectExpr(Op0);
    }

    BuildMI(BB, Opc, 2, Result).addReg(Tmp1).addReg(Tmp2);
    return Result;
  case ISD::SUB:
  case ISD::MUL:
  case ISD::AND:
  case ISD::OR:
  case ISD::XOR: {
    static const unsigned SUBTab[] = {
      X86::SUB8ri, X86::SUB16ri, X86::SUB32ri, 0, 0,
      X86::SUB8rm, X86::SUB16rm, X86::SUB32rm, X86::FSUB32m, X86::FSUB64m,
      X86::SUB8rr, X86::SUB16rr, X86::SUB32rr, X86::FpSUB  , X86::FpSUB,
    };
    static const unsigned MULTab[] = {
      0, X86::IMUL16rri, X86::IMUL32rri, 0, 0,
      0, X86::IMUL16rm , X86::IMUL32rm, X86::FMUL32m, X86::FMUL64m,
      0, X86::IMUL16rr , X86::IMUL32rr, X86::FpMUL  , X86::FpMUL,
    };
    static const unsigned ANDTab[] = {
      X86::AND8ri, X86::AND16ri, X86::AND32ri, 0, 0,
      X86::AND8rm, X86::AND16rm, X86::AND32rm, 0, 0,
      X86::AND8rr, X86::AND16rr, X86::AND32rr, 0, 0, 
    };
    static const unsigned ORTab[] = {
      X86::OR8ri, X86::OR16ri, X86::OR32ri, 0, 0,
      X86::OR8rm, X86::OR16rm, X86::OR32rm, 0, 0,
      X86::OR8rr, X86::OR16rr, X86::OR32rr, 0, 0,
    };
    static const unsigned XORTab[] = {
      X86::XOR8ri, X86::XOR16ri, X86::XOR32ri, 0, 0,
      X86::XOR8rm, X86::XOR16rm, X86::XOR32rm, 0, 0,
      X86::XOR8rr, X86::XOR16rr, X86::XOR32rr, 0, 0,
    };

    Op0 = Node->getOperand(0);
    Op1 = Node->getOperand(1);

    if (Node->getOpcode() == ISD::SUB && MVT::isInteger(N.getValueType()))
      if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N.getOperand(0)))
        if (CN->isNullValue()) {   // 0 - N -> neg N
          switch (N.getValueType()) {
          default: assert(0 && "Cannot sub this type!");
          case MVT::i1:
          case MVT::i8:  Opc = X86::NEG8r;  break;
          case MVT::i16: Opc = X86::NEG16r; break;
          case MVT::i32: Opc = X86::NEG32r; break;
          }
          Tmp1 = SelectExpr(N.getOperand(1));
          BuildMI(BB, Opc, 1, Result).addReg(Tmp1);
          return Result;
        }

    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Op1)) {
      if (CN->isAllOnesValue() && Node->getOpcode() == ISD::XOR) {
        switch (N.getValueType()) {
        default: assert(0 && "Cannot add this type!");
        case MVT::i1:
        case MVT::i8:  Opc = X86::NOT8r;  break;
        case MVT::i16: Opc = X86::NOT16r; break;
        case MVT::i32: Opc = X86::NOT32r; break;
        }
        Tmp1 = SelectExpr(Op0);
        BuildMI(BB, Opc, 1, Result).addReg(Tmp1);
        return Result;
      }

      switch (N.getValueType()) {
      default: assert(0 && "Cannot xor this type!");
      case MVT::i1:
      case MVT::i8:  Opc = 0; break;
      case MVT::i16: Opc = 1; break;
      case MVT::i32: Opc = 2; break;
      }
      switch (Node->getOpcode()) {
      default: assert(0 && "Unreachable!");
      case ISD::SUB: Opc = SUBTab[Opc]; break;
      case ISD::MUL: Opc = MULTab[Opc]; break;
      case ISD::AND: Opc = ANDTab[Opc]; break;
      case ISD::OR:  Opc =  ORTab[Opc]; break;
      case ISD::XOR: Opc = XORTab[Opc]; break;
      }
      if (Opc) {  // Can't fold MUL:i8 R, imm
        Tmp1 = SelectExpr(Op0);
        BuildMI(BB, Opc, 2, Result).addReg(Tmp1).addImm(CN->getValue());
        return Result;
      }
    }

    if (isFoldableLoad(Op0))
      if (Node->getOpcode() != ISD::SUB) {
        std::swap(Op0, Op1);
      } else {
        // Emit 'reverse' subract, with a memory operand.
        switch (N.getValueType()) {
        default: Opc = 0; break;
        case MVT::f32: Opc = X86::FSUBR32m; break;
        case MVT::f64: Opc = X86::FSUBR64m; break;
        }
        if (Opc) {
          X86AddressMode AM;
          EmitFoldedLoad(Op0, AM);
          Tmp1 = SelectExpr(Op1);
          addFullAddress(BuildMI(BB, Opc, 5, Result).addReg(Tmp1), AM);
          return Result;
        }
      }

    if (isFoldableLoad(Op1)) {
      switch (N.getValueType()) {
      default: assert(0 && "Cannot operate on this type!");
      case MVT::i1:
      case MVT::i8:  Opc = 5; break;
      case MVT::i16: Opc = 6; break;
      case MVT::i32: Opc = 7; break;
      case MVT::f32: Opc = 8; break;
      case MVT::f64: Opc = 9; break;
      }
      switch (Node->getOpcode()) {
      default: assert(0 && "Unreachable!");
      case ISD::SUB: Opc = SUBTab[Opc]; break;
      case ISD::MUL: Opc = MULTab[Opc]; break;
      case ISD::AND: Opc = ANDTab[Opc]; break;
      case ISD::OR:  Opc =  ORTab[Opc]; break;
      case ISD::XOR: Opc = XORTab[Opc]; break;
      }

      X86AddressMode AM;
      EmitFoldedLoad(Op1, AM);
      Tmp1 = SelectExpr(Op0);
      if (Opc) {
        addFullAddress(BuildMI(BB, Opc, 5, Result).addReg(Tmp1), AM);
      } else {
        assert(Node->getOpcode() == ISD::MUL &&
               N.getValueType() == MVT::i8 && "Unexpected situation!");
        // Must use the MUL instruction, which forces use of AL.
        BuildMI(BB, X86::MOV8rr, 1, X86::AL).addReg(Tmp1);
        addFullAddress(BuildMI(BB, X86::MUL8m, 1), AM);
        BuildMI(BB, X86::MOV8rr, 1, Result).addReg(X86::AL);
      }
      return Result;
    }

    if (getRegPressure(Op0) > getRegPressure(Op1)) {
      Tmp1 = SelectExpr(Op0);
      Tmp2 = SelectExpr(Op1);
    } else {
      Tmp2 = SelectExpr(Op1);
      Tmp1 = SelectExpr(Op0);
    }

    switch (N.getValueType()) {
    default: assert(0 && "Cannot add this type!");
    case MVT::i1:
    case MVT::i8:  Opc = 10; break;
    case MVT::i16: Opc = 11; break;
    case MVT::i32: Opc = 12; break;
    case MVT::f32: Opc = 13; break;
    case MVT::f64: Opc = 14; break;
    }
    switch (Node->getOpcode()) {
    default: assert(0 && "Unreachable!");
    case ISD::SUB: Opc = SUBTab[Opc]; break;
    case ISD::MUL: Opc = MULTab[Opc]; break;
    case ISD::AND: Opc = ANDTab[Opc]; break;
    case ISD::OR:  Opc =  ORTab[Opc]; break;
    case ISD::XOR: Opc = XORTab[Opc]; break;
    }
    if (Opc) {
      BuildMI(BB, Opc, 2, Result).addReg(Tmp1).addReg(Tmp2);
    } else {
      assert(Node->getOpcode() == ISD::MUL &&
             N.getValueType() == MVT::i8 && "Unexpected situation!");
      // Must use the MUL instruction, which forces use of AL.
      BuildMI(BB, X86::MOV8rr, 1, X86::AL).addReg(Tmp1);
      BuildMI(BB, X86::MUL8r, 1).addReg(Tmp2);
      BuildMI(BB, X86::MOV8rr, 1, Result).addReg(X86::AL);
    }
    return Result;
  }
  case ISD::SELECT:
    if (N.getValueType() != MVT::i1 && N.getValueType() != MVT::i8) {
      if (getRegPressure(N.getOperand(1)) > getRegPressure(N.getOperand(2))) {
        Tmp2 = SelectExpr(N.getOperand(1));
        Tmp3 = SelectExpr(N.getOperand(2));
      } else {
        Tmp3 = SelectExpr(N.getOperand(2));
        Tmp2 = SelectExpr(N.getOperand(1));
      }
      EmitSelectCC(N.getOperand(0), N.getValueType(), Tmp2, Tmp3, Result);
      return Result;
    } else {
      // FIXME: This should not be implemented here, it should be in the generic
      // code!
      if (getRegPressure(N.getOperand(1)) > getRegPressure(N.getOperand(2))) {
        Tmp2 = SelectExpr(CurDAG->getNode(ISD::ZERO_EXTEND, MVT::i16,
                                          N.getOperand(1)));
        Tmp3 = SelectExpr(CurDAG->getNode(ISD::ZERO_EXTEND, MVT::i16,
                                          N.getOperand(2)));
      } else {
        Tmp3 = SelectExpr(CurDAG->getNode(ISD::ZERO_EXTEND, MVT::i16,
                                          N.getOperand(2)));
        Tmp2 = SelectExpr(CurDAG->getNode(ISD::ZERO_EXTEND, MVT::i16,
                                          N.getOperand(1)));
      }
      unsigned TmpReg = MakeReg(MVT::i16);
      EmitSelectCC(N.getOperand(0), MVT::i16, Tmp2, Tmp3, TmpReg);
      // FIXME: need subregs to do better than this!
      BuildMI(BB, X86::MOV16rr, 1, X86::AX).addReg(TmpReg);
      BuildMI(BB, X86::MOV8rr, 1, Result).addReg(X86::AL);
      return Result;
    }

  case ISD::SDIV:
  case ISD::UDIV:
  case ISD::SREM:
  case ISD::UREM: {
    if (N.getOpcode() == ISD::SDIV)
      if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
        // FIXME: These special cases should be handled by the lowering impl!
        unsigned RHS = CN->getValue();
        bool isNeg = false;
        if ((int)RHS < 0) {
          isNeg = true;
          RHS = -RHS;
        }
        if (RHS && (RHS & (RHS-1)) == 0) {   // Signed division by power of 2?
          unsigned Log = log2(RHS);
          unsigned TmpReg = MakeReg(N.getValueType());
          unsigned SAROpc, SHROpc, ADDOpc, NEGOpc;
          switch (N.getValueType()) {
          default: assert("Unknown type to signed divide!");
          case MVT::i8:
            SAROpc = X86::SAR8ri;
            SHROpc = X86::SHR8ri;
            ADDOpc = X86::ADD8rr;
            NEGOpc = X86::NEG8r;
            break;
          case MVT::i16:
            SAROpc = X86::SAR16ri;
            SHROpc = X86::SHR16ri;
            ADDOpc = X86::ADD16rr;
            NEGOpc = X86::NEG16r;
            break;
          case MVT::i32:
            SAROpc = X86::SAR32ri;
            SHROpc = X86::SHR32ri;
            ADDOpc = X86::ADD32rr;
            NEGOpc = X86::NEG32r;
            break;
          }
          Tmp1 = SelectExpr(N.getOperand(0));
          BuildMI(BB, SAROpc, 2, TmpReg).addReg(Tmp1).addImm(Log-1);
          unsigned TmpReg2 = MakeReg(N.getValueType());
          BuildMI(BB, SHROpc, 2, TmpReg2).addReg(TmpReg).addImm(32-Log);
          unsigned TmpReg3 = MakeReg(N.getValueType());
          BuildMI(BB, ADDOpc, 2, TmpReg3).addReg(Tmp1).addReg(TmpReg2);
          
          unsigned TmpReg4 = isNeg ? MakeReg(N.getValueType()) : Result;
          BuildMI(BB, SAROpc, 2, TmpReg4).addReg(TmpReg3).addImm(Log);
          if (isNeg)
            BuildMI(BB, NEGOpc, 1, Result).addReg(TmpReg4);
          return Result;
        }
      }

    if (getRegPressure(N.getOperand(0)) > getRegPressure(N.getOperand(1))) {
      Tmp1 = SelectExpr(N.getOperand(0));
      Tmp2 = SelectExpr(N.getOperand(1));
    } else {
      Tmp2 = SelectExpr(N.getOperand(1));
      Tmp1 = SelectExpr(N.getOperand(0));
    }

    bool isSigned = N.getOpcode() == ISD::SDIV || N.getOpcode() == ISD::SREM;
    bool isDiv    = N.getOpcode() == ISD::SDIV || N.getOpcode() == ISD::UDIV;
    unsigned LoReg, HiReg, DivOpcode, MovOpcode, ClrOpcode, SExtOpcode;
    switch (N.getValueType()) {
    default: assert(0 && "Cannot sdiv this type!");
    case MVT::i8:
      DivOpcode = isSigned ? X86::IDIV8r : X86::DIV8r;
      LoReg = X86::AL;
      HiReg = X86::AH;
      MovOpcode = X86::MOV8rr;
      ClrOpcode = X86::MOV8ri;
      SExtOpcode = X86::CBW;
      break;
    case MVT::i16:
      DivOpcode = isSigned ? X86::IDIV16r : X86::DIV16r;
      LoReg = X86::AX;
      HiReg = X86::DX;
      MovOpcode = X86::MOV16rr;
      ClrOpcode = X86::MOV16ri;
      SExtOpcode = X86::CWD;
      break;
    case MVT::i32:
      DivOpcode = isSigned ? X86::IDIV32r : X86::DIV32r;
      LoReg = X86::EAX;
      HiReg = X86::EDX;
      MovOpcode = X86::MOV32rr;
      ClrOpcode = X86::MOV32ri;
      SExtOpcode = X86::CDQ;
      break;
    case MVT::i64: assert(0 && "FIXME: implement i64 DIV/REM libcalls!");
    case MVT::f32: 
    case MVT::f64:
      if (N.getOpcode() == ISD::SDIV)
        BuildMI(BB, X86::FpDIV, 2, Result).addReg(Tmp1).addReg(Tmp2);
      else
        assert(0 && "FIXME: Emit frem libcall to fmod!");
      return Result;
    }

    // Set up the low part.
    BuildMI(BB, MovOpcode, 1, LoReg).addReg(Tmp1);

    if (isSigned) {
      // Sign extend the low part into the high part.
      BuildMI(BB, SExtOpcode, 0);
    } else {
      // Zero out the high part, effectively zero extending the input.
      BuildMI(BB, ClrOpcode, 1, HiReg).addImm(0);
    }

    // Emit the DIV/IDIV instruction.
    BuildMI(BB, DivOpcode, 1).addReg(Tmp2);    

    // Get the result of the divide or rem.
    BuildMI(BB, MovOpcode, 1, Result).addReg(isDiv ? LoReg : HiReg);
    return Result;
  }

  case ISD::SHL:
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      if (CN->getValue() == 1) {   // X = SHL Y, 1  -> X = ADD Y, Y
        switch (N.getValueType()) {
        default: assert(0 && "Cannot shift this type!");
        case MVT::i8:  Opc = X86::ADD8rr; break;
        case MVT::i16: Opc = X86::ADD16rr; break;
        case MVT::i32: Opc = X86::ADD32rr; break;
        }
        Tmp1 = SelectExpr(N.getOperand(0));
        BuildMI(BB, Opc, 2, Result).addReg(Tmp1).addReg(Tmp1);
        return Result;
      }
      
      switch (N.getValueType()) {
      default: assert(0 && "Cannot shift this type!");
      case MVT::i8:  Opc = X86::SHL8ri; break;
      case MVT::i16: Opc = X86::SHL16ri; break;
      case MVT::i32: Opc = X86::SHL32ri; break;
      }
      Tmp1 = SelectExpr(N.getOperand(0));
      BuildMI(BB, Opc, 2, Result).addReg(Tmp1).addImm(CN->getValue());
      return Result;
    }

    if (getRegPressure(N.getOperand(0)) > getRegPressure(N.getOperand(1))) {
      Tmp1 = SelectExpr(N.getOperand(0));
      Tmp2 = SelectExpr(N.getOperand(1));
    } else {
      Tmp2 = SelectExpr(N.getOperand(1));
      Tmp1 = SelectExpr(N.getOperand(0));
    }

    switch (N.getValueType()) {
    default: assert(0 && "Cannot shift this type!");
    case MVT::i8 : Opc = X86::SHL8rCL; break;
    case MVT::i16: Opc = X86::SHL16rCL; break;
    case MVT::i32: Opc = X86::SHL32rCL; break;
    }
    BuildMI(BB, X86::MOV8rr, 1, X86::CL).addReg(Tmp2);
    BuildMI(BB, Opc, 2, Result).addReg(Tmp1).addReg(Tmp2);
    return Result;
  case ISD::SRL:
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      switch (N.getValueType()) {
      default: assert(0 && "Cannot shift this type!");
      case MVT::i8:  Opc = X86::SHR8ri; break;
      case MVT::i16: Opc = X86::SHR16ri; break;
      case MVT::i32: Opc = X86::SHR32ri; break;
      }
      Tmp1 = SelectExpr(N.getOperand(0));
      BuildMI(BB, Opc, 2, Result).addReg(Tmp1).addImm(CN->getValue());
      return Result;
    }

    if (getRegPressure(N.getOperand(0)) > getRegPressure(N.getOperand(1))) {
      Tmp1 = SelectExpr(N.getOperand(0));
      Tmp2 = SelectExpr(N.getOperand(1));
    } else {
      Tmp2 = SelectExpr(N.getOperand(1));
      Tmp1 = SelectExpr(N.getOperand(0));
    }

    switch (N.getValueType()) {
    default: assert(0 && "Cannot shift this type!");
    case MVT::i8 : Opc = X86::SHR8rCL; break;
    case MVT::i16: Opc = X86::SHR16rCL; break;
    case MVT::i32: Opc = X86::SHR32rCL; break;
    }
    BuildMI(BB, X86::MOV8rr, 1, X86::CL).addReg(Tmp2);
    BuildMI(BB, Opc, 2, Result).addReg(Tmp1).addReg(Tmp2);
    return Result;
  case ISD::SRA:
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      switch (N.getValueType()) {
      default: assert(0 && "Cannot shift this type!");
      case MVT::i8:  Opc = X86::SAR8ri; break;
      case MVT::i16: Opc = X86::SAR16ri; break;
      case MVT::i32: Opc = X86::SAR32ri; break;
      }
      Tmp1 = SelectExpr(N.getOperand(0));
      BuildMI(BB, Opc, 2, Result).addReg(Tmp1).addImm(CN->getValue());
      return Result;
    }

    if (getRegPressure(N.getOperand(0)) > getRegPressure(N.getOperand(1))) {
      Tmp1 = SelectExpr(N.getOperand(0));
      Tmp2 = SelectExpr(N.getOperand(1));
    } else {
      Tmp2 = SelectExpr(N.getOperand(1));
      Tmp1 = SelectExpr(N.getOperand(0));
    }

    switch (N.getValueType()) {
    default: assert(0 && "Cannot shift this type!");
    case MVT::i8 : Opc = X86::SAR8rCL; break;
    case MVT::i16: Opc = X86::SAR16rCL; break;
    case MVT::i32: Opc = X86::SAR32rCL; break;
    }
    BuildMI(BB, X86::MOV8rr, 1, X86::CL).addReg(Tmp2);
    BuildMI(BB, Opc, 2, Result).addReg(Tmp1).addReg(Tmp2);
    return Result;

  case ISD::SETCC:
    EmitCMP(N.getOperand(0), N.getOperand(1));
    EmitSetCC(BB, Result, cast<SetCCSDNode>(N)->getCondition(),
              MVT::isFloatingPoint(N.getOperand(1).getValueType()));
    return Result;
  case ISD::LOAD: {
    // Make sure we generate both values.
    if (Result != 1)
      ExprMap[N.getValue(1)] = 1;   // Generate the token
    else
      Result = ExprMap[N.getValue(0)] = MakeReg(N.getValue(0).getValueType());

    switch (Node->getValueType(0)) {
    default: assert(0 && "Cannot load this type!");
    case MVT::i1:
    case MVT::i8:  Opc = X86::MOV8rm; break;
    case MVT::i16: Opc = X86::MOV16rm; break;
    case MVT::i32: Opc = X86::MOV32rm; break;
    case MVT::f32: Opc = X86::FLD32m; ContainsFPCode = true; break;
    case MVT::f64: Opc = X86::FLD64m; ContainsFPCode = true; break;
    }

    if (ConstantPoolSDNode *CP = dyn_cast<ConstantPoolSDNode>(N.getOperand(1))){
      Select(N.getOperand(0));
      addConstantPoolReference(BuildMI(BB, Opc, 4, Result), CP->getIndex());
    } else {
      X86AddressMode AM;

      SDOperand Chain   = N.getOperand(0);
      SDOperand Address = N.getOperand(1);
      if (getRegPressure(Chain) > getRegPressure(Address)) {
        Select(Chain);
        SelectAddress(Address, AM);
      } else {
        SelectAddress(Address, AM);
        Select(Chain);
      }

      addFullAddress(BuildMI(BB, Opc, 4, Result), AM);
    }
    return Result;
  }
  case ISD::DYNAMIC_STACKALLOC:
    // Generate both result values.
    if (Result != 1)
      ExprMap[N.getValue(1)] = 1;   // Generate the token
    else
      Result = ExprMap[N.getValue(0)] = MakeReg(N.getValue(0).getValueType());

    // FIXME: We are currently ignoring the requested alignment for handling
    // greater than the stack alignment.  This will need to be revisited at some
    // point.  Align = N.getOperand(2);

    if (!isa<ConstantSDNode>(N.getOperand(2)) ||
        cast<ConstantSDNode>(N.getOperand(2))->getValue() != 0) {
      std::cerr << "Cannot allocate stack object with greater alignment than"
                << " the stack alignment yet!";
      abort();
    }
  
    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      Select(N.getOperand(0));
      BuildMI(BB, X86::SUB32ri, 2, X86::ESP).addReg(X86::ESP)
        .addImm(CN->getValue());
    } else {
      if (getRegPressure(N.getOperand(0)) > getRegPressure(N.getOperand(1))) {
        Select(N.getOperand(0));
        Tmp1 = SelectExpr(N.getOperand(1));
      } else {
        Tmp1 = SelectExpr(N.getOperand(1));
        Select(N.getOperand(0));
      }

      // Subtract size from stack pointer, thereby allocating some space.
      BuildMI(BB, X86::SUB32rr, 2, X86::ESP).addReg(X86::ESP).addReg(Tmp1);
    }

    // Put a pointer to the space into the result register, by copying the stack
    // pointer.
    BuildMI(BB, X86::MOV32rr, 1, Result).addReg(X86::ESP);
    return Result;

  case ISD::CALL:
    // The chain for this call is now lowered.
    LoweredTokens.insert(N.getValue(Node->getNumValues()-1));

    if (GlobalAddressSDNode *GASD =
               dyn_cast<GlobalAddressSDNode>(N.getOperand(1))) {
      Select(N.getOperand(0));
      BuildMI(BB, X86::CALLpcrel32, 1).addGlobalAddress(GASD->getGlobal(),true);
    } else if (ExternalSymbolSDNode *ESSDN =
               dyn_cast<ExternalSymbolSDNode>(N.getOperand(1))) {
      Select(N.getOperand(0));
      BuildMI(BB, X86::CALLpcrel32,
              1).addExternalSymbol(ESSDN->getSymbol(), true);
    } else {
      if (getRegPressure(N.getOperand(0)) > getRegPressure(N.getOperand(1))) {
        Select(N.getOperand(0));
        Tmp1 = SelectExpr(N.getOperand(1));
      } else {
        Tmp1 = SelectExpr(N.getOperand(1));
        Select(N.getOperand(0));
      }

      BuildMI(BB, X86::CALL32r, 1).addReg(Tmp1);
    }
    switch (Node->getValueType(0)) {
    default: assert(0 && "Unknown value type for call result!");
    case MVT::Other: return 1;
    case MVT::i1:
    case MVT::i8:
      BuildMI(BB, X86::MOV8rr, 1, Result).addReg(X86::AL);
      break;
    case MVT::i16:
      BuildMI(BB, X86::MOV16rr, 1, Result).addReg(X86::AX);
      break;
    case MVT::i32:
      BuildMI(BB, X86::MOV32rr, 1, Result).addReg(X86::EAX);
      if (Node->getValueType(1) == MVT::i32)
        BuildMI(BB, X86::MOV32rr, 1, Result+1).addReg(X86::EDX);
      break;
    case MVT::f32:
    case MVT::f64:     // Floating-point return values live in %ST(0)
      ContainsFPCode = true;
      BuildMI(BB, X86::FpGETRESULT, 1, Result);
      break;
    }
    return Result+N.ResNo;
  }

  return 0;
}

void ISel::Select(SDOperand N) {
  unsigned Tmp1, Tmp2, Opc;

  // FIXME: Disable for our current expansion model!
  if (/*!N->hasOneUse() &&*/ !LoweredTokens.insert(N).second)
    return;  // Already selected.

  SDNode *Node = N.Val;

  switch (Node->getOpcode()) {
  default:
    Node->dump(); std::cerr << "\n";
    assert(0 && "Node not handled yet!");
  case ISD::EntryToken: return;  // Noop
  case ISD::TokenFactor:
    if (Node->getNumOperands() == 2) {
      bool OneFirst = 
        getRegPressure(Node->getOperand(1))>getRegPressure(Node->getOperand(0));
      Select(Node->getOperand(OneFirst));
      Select(Node->getOperand(!OneFirst));
    } else {
      std::vector<std::pair<unsigned, unsigned> > OpsP;
      for (unsigned i = 0, e = Node->getNumOperands(); i != e; ++i)
        OpsP.push_back(std::make_pair(getRegPressure(Node->getOperand(i)), i));
      std::sort(OpsP.begin(), OpsP.end());
      std::reverse(OpsP.begin(), OpsP.end());
      for (unsigned i = 0, e = Node->getNumOperands(); i != e; ++i)
        Select(Node->getOperand(OpsP[i].second));
    }
    return;
  case ISD::CopyToReg:
    if (getRegPressure(N.getOperand(0)) > getRegPressure(N.getOperand(1))) {
      Select(N.getOperand(0));
      Tmp1 = SelectExpr(N.getOperand(1));
    } else {
      Tmp1 = SelectExpr(N.getOperand(1));
      Select(N.getOperand(0));
    }
    Tmp2 = cast<RegSDNode>(N)->getReg();
    
    if (Tmp1 != Tmp2) {
      switch (N.getOperand(1).getValueType()) {
      default: assert(0 && "Invalid type for operation!");
      case MVT::i1:
      case MVT::i8:  Opc = X86::MOV8rr; break;
      case MVT::i16: Opc = X86::MOV16rr; break;
      case MVT::i32: Opc = X86::MOV32rr; break;
      case MVT::f32:
      case MVT::f64: Opc = X86::FpMOV; ContainsFPCode = true; break;
      }
      BuildMI(BB, Opc, 1, Tmp2).addReg(Tmp1);
    }
    return;
  case ISD::RET:
    switch (N.getNumOperands()) {
    default:
      assert(0 && "Unknown return instruction!");
    case 3:
      assert(N.getOperand(1).getValueType() == MVT::i32 &&
	     N.getOperand(2).getValueType() == MVT::i32 &&
	     "Unknown two-register value!");
      if (getRegPressure(N.getOperand(1)) > getRegPressure(N.getOperand(2))) {
        Tmp1 = SelectExpr(N.getOperand(1));
        Tmp2 = SelectExpr(N.getOperand(2));
      } else {
        Tmp2 = SelectExpr(N.getOperand(2));
        Tmp1 = SelectExpr(N.getOperand(1));
      }
      Select(N.getOperand(0));

      BuildMI(BB, X86::MOV32rr, 1, X86::EAX).addReg(Tmp1);
      BuildMI(BB, X86::MOV32rr, 1, X86::EDX).addReg(Tmp2);
      // Declare that EAX & EDX are live on exit.
      BuildMI(BB, X86::IMPLICIT_USE, 3).addReg(X86::EAX).addReg(X86::EDX)
	.addReg(X86::ESP);
      break;
    case 2:
      if (getRegPressure(N.getOperand(0)) > getRegPressure(N.getOperand(1))) {
        Select(N.getOperand(0));
        Tmp1 = SelectExpr(N.getOperand(1));
      } else {
        Tmp1 = SelectExpr(N.getOperand(1));
        Select(N.getOperand(0));
      }
      switch (N.getOperand(1).getValueType()) {
      default: assert(0 && "All other types should have been promoted!!");
      case MVT::f64:
	BuildMI(BB, X86::FpSETRESULT, 1).addReg(Tmp1);
	// Declare that top-of-stack is live on exit
	BuildMI(BB, X86::IMPLICIT_USE, 2).addReg(X86::ST0).addReg(X86::ESP);
	break;
      case MVT::i32:
	BuildMI(BB, X86::MOV32rr, 1, X86::EAX).addReg(Tmp1);
	BuildMI(BB, X86::IMPLICIT_USE, 2).addReg(X86::EAX).addReg(X86::ESP);
	break;
      }
      break;
    case 1:
      Select(N.getOperand(0));
      break;
    }
    BuildMI(BB, X86::RET, 0); // Just emit a 'ret' instruction
    return;
  case ISD::BR: {
    Select(N.getOperand(0));
    MachineBasicBlock *Dest =
      cast<BasicBlockSDNode>(N.getOperand(1))->getBasicBlock();
    BuildMI(BB, X86::JMP, 1).addMBB(Dest);
    return;
  }

  case ISD::BRCOND: {
    MachineBasicBlock *Dest =
      cast<BasicBlockSDNode>(N.getOperand(2))->getBasicBlock();

    // Try to fold a setcc into the branch.  If this fails, emit a test/jne
    // pair.
    if (EmitBranchCC(Dest, N.getOperand(0), N.getOperand(1))) {
      if (getRegPressure(N.getOperand(0)) > getRegPressure(N.getOperand(1))) {
        Select(N.getOperand(0));
        Tmp1 = SelectExpr(N.getOperand(1));
      } else {
        Tmp1 = SelectExpr(N.getOperand(1));
        Select(N.getOperand(0));
      }
      BuildMI(BB, X86::TEST8rr, 2).addReg(Tmp1).addReg(Tmp1);
      BuildMI(BB, X86::JNE, 1).addMBB(Dest);
    }

    return;
  }
  case ISD::LOAD:
  case ISD::CALL:
  case ISD::DYNAMIC_STACKALLOC:
    SelectExpr(N);
    return;
  case ISD::STORE: {
    X86AddressMode AM;

    if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(N.getOperand(1))) {
      Opc = 0;
      switch (CN->getValueType(0)) {
      default: assert(0 && "Invalid type for operation!");
      case MVT::i1:
      case MVT::i8:  Opc = X86::MOV8mi; break;
      case MVT::i16: Opc = X86::MOV16mi; break;
      case MVT::i32: Opc = X86::MOV32mi; break;
      case MVT::f32:
      case MVT::f64: break;
      }
      if (Opc) {
        if (getRegPressure(N.getOperand(0)) > getRegPressure(N.getOperand(2))) {
          Select(N.getOperand(0));
          SelectAddress(N.getOperand(2), AM);
        } else {
          SelectAddress(N.getOperand(2), AM);
          Select(N.getOperand(0));
        }
        addFullAddress(BuildMI(BB, Opc, 4+1), AM).addImm(CN->getValue());
        return;
      }
    }

    // Check to see if this is a load/op/store combination.
    if (N.getOperand(1).Val->hasOneUse() &&
        isFoldableLoad(N.getOperand(0).getValue(0)) &&
        !MVT::isFloatingPoint(N.getOperand(0).getValue(0).getValueType())) {
      SDOperand TheLoad = N.getOperand(0).getValue(0);
      // Check to see if we are loading the same pointer that we're storing to.
      if (TheLoad.getOperand(1) == N.getOperand(2)) {
        // See if the stored value is a simple binary operator that uses the
        // load as one of its operands.
        SDOperand Op = N.getOperand(1);
        if (Op.Val->getNumOperands() == 2 &&
            (Op.getOperand(0) == TheLoad || Op.getOperand(1) == TheLoad)) {
          // Finally, check to see if this is one of the ops we can handle!
          static const unsigned ADDTAB[] = {
            X86::ADD8mi, X86::ADD16mi, X86::ADD32mi,
            X86::ADD8mr, X86::ADD16mr, X86::ADD32mr,
          };
          static const unsigned SUBTAB[] = {
            X86::SUB8mi, X86::SUB16mi, X86::SUB32mi,
            X86::SUB8mr, X86::SUB16mr, X86::SUB32mr,
          };
          static const unsigned ANDTAB[] = {
            X86::AND8mi, X86::AND16mi, X86::AND32mi,
            X86::AND8mr, X86::AND16mr, X86::AND32mr,
          };
          static const unsigned ORTAB[] = {
            X86::OR8mi, X86::OR16mi, X86::OR32mi,
            X86::OR8mr, X86::OR16mr, X86::OR32mr,
          };
          static const unsigned XORTAB[] = {
            X86::XOR8mi, X86::XOR16mi, X86::XOR32mi,
            X86::XOR8mr, X86::XOR16mr, X86::XOR32mr,
          };
          static const unsigned SHLTAB[] = {
            X86::SHL8mi, X86::SHL16mi, X86::SHL32mi,
            /*Have to put the reg in CL*/0, 0, 0,
          };
          static const unsigned SARTAB[] = {
            X86::SAR8mi, X86::SAR16mi, X86::SAR32mi,
            /*Have to put the reg in CL*/0, 0, 0,
          };
          static const unsigned SHRTAB[] = {
            X86::SHR8mi, X86::SHR16mi, X86::SHR32mi,
            /*Have to put the reg in CL*/0, 0, 0,
          };

          const unsigned *TabPtr = 0;
          switch (Op.getOpcode()) {
          default: std::cerr << "CANNOT [mem] op= val: "; Op.Val->dump(); std::cerr << "\n"; break;
          case ISD::ADD: TabPtr = ADDTAB; break;
          case ISD::SUB: TabPtr = SUBTAB; break;
          case ISD::AND: TabPtr = ANDTAB; break;
          case ISD:: OR: TabPtr =  ORTAB; break;
          case ISD::XOR: TabPtr = XORTAB; break;
          case ISD::SHL: TabPtr = SHLTAB; break;
          case ISD::SRA: TabPtr = SARTAB; break;
          case ISD::SRL: TabPtr = SHRTAB; break;
          }
          
          if (TabPtr) {
            // Handle: [mem] op= CST
            SDOperand Op0 = Op.getOperand(0);
            SDOperand Op1 = Op.getOperand(1);
            if (ConstantSDNode *CN = dyn_cast<ConstantSDNode>(Op1)) {
              switch (Op0.getValueType()) { // Use Op0's type because of shifts.
              default: break;
              case MVT::i1:
              case MVT::i8:  Opc = TabPtr[0]; break;
              case MVT::i16: Opc = TabPtr[1]; break;
              case MVT::i32: Opc = TabPtr[2]; break;
              }

              if (Opc) {
                if (getRegPressure(TheLoad.getOperand(0)) >
                    getRegPressure(TheLoad.getOperand(1))) {
                  Select(TheLoad.getOperand(0));
                  SelectAddress(TheLoad.getOperand(1), AM);
                } else {
                  SelectAddress(TheLoad.getOperand(1), AM);
                  Select(TheLoad.getOperand(0));
                }            

                addFullAddress(BuildMI(BB, Opc, 4+1),AM).addImm(CN->getValue());
                return;
              }
            }
            
            // If we have [mem] = V op [mem], try to turn it into:
            // [mem] = [mem] op V.
            if (Op1 == TheLoad && Op.getOpcode() != ISD::SUB &&
                Op.getOpcode() != ISD::SHL && Op.getOpcode() != ISD::SRA &&
                Op.getOpcode() != ISD::SRL)
              std::swap(Op0, Op1);

            if (Op0 == TheLoad) {
              switch (Op0.getValueType()) {
              default: break;
              case MVT::i1:
              case MVT::i8:  Opc = TabPtr[3]; break;
              case MVT::i16: Opc = TabPtr[4]; break;
              case MVT::i32: Opc = TabPtr[5]; break;
              }
              
              if (Opc) {
                Select(TheLoad.getOperand(0));
                SelectAddress(TheLoad.getOperand(1), AM);
                unsigned Reg = SelectExpr(Op1);
                addFullAddress(BuildMI(BB, Opc, 4+1),AM).addReg(Reg);
                return;
              }
            }
          }
        }
      }
    }


    switch (N.getOperand(1).getValueType()) {
    default: assert(0 && "Cannot store this type!");
    case MVT::i1:
    case MVT::i8:  Opc = X86::MOV8mr; break;
    case MVT::i16: Opc = X86::MOV16mr; break;
    case MVT::i32: Opc = X86::MOV32mr; break;
    case MVT::f32: Opc = X86::FST32m; break;
    case MVT::f64: Opc = X86::FST64m; break;
    }
    
    std::vector<std::pair<unsigned, unsigned> > RP;
    RP.push_back(std::make_pair(getRegPressure(N.getOperand(0)), 0));
    RP.push_back(std::make_pair(getRegPressure(N.getOperand(1)), 1));
    RP.push_back(std::make_pair(getRegPressure(N.getOperand(2)), 2));
    std::sort(RP.begin(), RP.end());

    for (unsigned i = 0; i != 3; ++i)
      switch (RP[2-i].second) {
      default: assert(0 && "Unknown operand number!");
      case 0: Select(N.getOperand(0)); break;
      case 1: Tmp1 = SelectExpr(N.getOperand(1)); break;
      case 2: SelectAddress(N.getOperand(2), AM); break;
      }

    addFullAddress(BuildMI(BB, Opc, 4+1), AM).addReg(Tmp1);
    return;
  }
  case ISD::ADJCALLSTACKDOWN:
  case ISD::ADJCALLSTACKUP:
    Select(N.getOperand(0));
    Tmp1 = cast<ConstantSDNode>(N.getOperand(1))->getValue();
    
    Opc = N.getOpcode() == ISD::ADJCALLSTACKDOWN ? X86::ADJCALLSTACKDOWN :
                                                   X86::ADJCALLSTACKUP;
    BuildMI(BB, Opc, 1).addImm(Tmp1);
    return;
  case ISD::MEMSET: {
    Select(N.getOperand(0));  // Select the chain.
    unsigned Align =
      (unsigned)cast<ConstantSDNode>(Node->getOperand(4))->getValue();
    if (Align == 0) Align = 1;

    // Turn the byte code into # iterations
    unsigned CountReg;
    unsigned Opcode;
    if (ConstantSDNode *ValC = dyn_cast<ConstantSDNode>(Node->getOperand(2))) {
      unsigned Val = ValC->getValue() & 255;

      // If the value is a constant, then we can potentially use larger sets.
      switch (Align & 3) {
      case 2:   // WORD aligned
        CountReg = MakeReg(MVT::i32);
        if (ConstantSDNode *I = dyn_cast<ConstantSDNode>(Node->getOperand(3))) {
          BuildMI(BB, X86::MOV32ri, 1, CountReg).addImm(I->getValue()/2);
        } else {
          unsigned ByteReg = SelectExpr(Node->getOperand(3));
          BuildMI(BB, X86::SHR32ri, 2, CountReg).addReg(ByteReg).addImm(1);
        }
        BuildMI(BB, X86::MOV16ri, 1, X86::AX).addImm((Val << 8) | Val);
        Opcode = X86::REP_STOSW;
        break;
      case 0:   // DWORD aligned
        CountReg = MakeReg(MVT::i32);
        if (ConstantSDNode *I = dyn_cast<ConstantSDNode>(Node->getOperand(3))) {
          BuildMI(BB, X86::MOV32ri, 1, CountReg).addImm(I->getValue()/4);
        } else {
          unsigned ByteReg = SelectExpr(Node->getOperand(3));
          BuildMI(BB, X86::SHR32ri, 2, CountReg).addReg(ByteReg).addImm(2);
        }
        Val = (Val << 8) | Val;
        BuildMI(BB, X86::MOV32ri, 1, X86::EAX).addImm((Val << 16) | Val);
        Opcode = X86::REP_STOSD;
        break;
      default:  // BYTE aligned
        CountReg = SelectExpr(Node->getOperand(3));
        BuildMI(BB, X86::MOV8ri, 1, X86::AL).addImm(Val);
        Opcode = X86::REP_STOSB;
        break;
      }
    } else {
      // If it's not a constant value we are storing, just fall back.  We could
      // try to be clever to form 16 bit and 32 bit values, but we don't yet.
      unsigned ValReg = SelectExpr(Node->getOperand(2));
      BuildMI(BB, X86::MOV8rr, 1, X86::AL).addReg(ValReg);
      CountReg = SelectExpr(Node->getOperand(3));
      Opcode = X86::REP_STOSB;
    }

    // No matter what the alignment is, we put the source in ESI, the
    // destination in EDI, and the count in ECX.
    unsigned TmpReg1 = SelectExpr(Node->getOperand(1));
    BuildMI(BB, X86::MOV32rr, 1, X86::ECX).addReg(CountReg);
    BuildMI(BB, X86::MOV32rr, 1, X86::EDI).addReg(TmpReg1);
    BuildMI(BB, Opcode, 0);
    return;
  }
  case ISD::MEMCPY:
    Select(N.getOperand(0));  // Select the chain.
    unsigned Align =
      (unsigned)cast<ConstantSDNode>(Node->getOperand(4))->getValue();
    if (Align == 0) Align = 1;

    // Turn the byte code into # iterations
    unsigned CountReg;
    unsigned Opcode;
    switch (Align & 3) {
    case 2:   // WORD aligned
      CountReg = MakeReg(MVT::i32);
      if (ConstantSDNode *I = dyn_cast<ConstantSDNode>(Node->getOperand(3))) {
        BuildMI(BB, X86::MOV32ri, 1, CountReg).addImm(I->getValue()/2);
      } else {
        unsigned ByteReg = SelectExpr(Node->getOperand(3));
        BuildMI(BB, X86::SHR32ri, 2, CountReg).addReg(ByteReg).addImm(1);
      }
      Opcode = X86::REP_MOVSW;
      break;
    case 0:   // DWORD aligned
      CountReg = MakeReg(MVT::i32);
      if (ConstantSDNode *I = dyn_cast<ConstantSDNode>(Node->getOperand(3))) {
        BuildMI(BB, X86::MOV32ri, 1, CountReg).addImm(I->getValue()/4);
      } else {
        unsigned ByteReg = SelectExpr(Node->getOperand(3));
        BuildMI(BB, X86::SHR32ri, 2, CountReg).addReg(ByteReg).addImm(2);
      }
      Opcode = X86::REP_MOVSD;
      break;
    default:  // BYTE aligned
      CountReg = SelectExpr(Node->getOperand(3));
      Opcode = X86::REP_MOVSB;
      break;
    }

    // No matter what the alignment is, we put the source in ESI, the
    // destination in EDI, and the count in ECX.
    unsigned TmpReg1 = SelectExpr(Node->getOperand(1));
    unsigned TmpReg2 = SelectExpr(Node->getOperand(2));
    BuildMI(BB, X86::MOV32rr, 1, X86::ECX).addReg(CountReg);
    BuildMI(BB, X86::MOV32rr, 1, X86::EDI).addReg(TmpReg1);
    BuildMI(BB, X86::MOV32rr, 1, X86::ESI).addReg(TmpReg2);
    BuildMI(BB, Opcode, 0);
    return;
  }
  assert(0 && "Should not be reached!");
}


/// createX86PatternInstructionSelector - This pass converts an LLVM function
/// into a machine code representation using pattern matching and a machine
/// description file.
///
FunctionPass *llvm::createX86PatternInstructionSelector(TargetMachine &TM) {
  return new ISel(TM);  
}
