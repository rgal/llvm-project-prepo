//===- FunctionHash.cpp - Function Hash Calculation ----------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the FunctionHash and GlobalNumberState classes which
// are used by the ProgramRepository pass for comparing functions hashes.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Utils/HashCalculator.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/GetElementPtrTypeIterator.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;

#define DEBUG_TYPE "hashcaculator"

/// \brief Adds \param V to the hash.

void HashCalculator::memHash(StringRef V) {
  Hash.update(ArrayRef<uint8_t>(HashKind::TAG_StringRef));
  numberHash(V.size());
  Hash.update(V);
}

void HashCalculator::APIntHash(const APInt &V) {
  Hash.update(HashKind::TAG_APInt);
  const uint64_t *Words = V.getRawData();
  for (unsigned I = 0, E = V.getNumWords(); I != E; ++I) {
    numberHash(Words[I]);
  }
}

void HashCalculator::APFloatHash(const APFloat &V) {
  Hash.update(HashKind::TAG_APFloat);
  // Floats are ordered first by semantics (i.e. float, double, half, etc.),
  // then by value interpreted as a bitstring (aka APInt).
  const fltSemantics &SV = V.getSemantics();
  //  unsigned int SPrec = APFloat::semanticsPrecision(SV);
  //  Hash.update(ArrayRef<uint8_t>((uint8_t *)&SPrec, sizeof(unsigned int)));
  numberHash(APFloat::semanticsPrecision(SV));
  signed short SMax = APFloat::semanticsMaxExponent(SV);
  Hash.update(ArrayRef<uint8_t>((uint8_t *)&SMax, sizeof(signed short)));
  signed short SMin = APFloat::semanticsMinExponent(SV);
  Hash.update(ArrayRef<uint8_t>((uint8_t *)&SMin, sizeof(signed short)));
  //  unsigned int SSize = APFloat::semanticsSizeInBits(SV);
  //  Hash.update(ArrayRef<uint8_t>((uint8_t *)&SSize, sizeof(unsigned int)));
  numberHash(APFloat::semanticsSizeInBits(SV));
  APIntHash(V.bitcastToAPInt());
}

void HashCalculator::orderingHash(AtomicOrdering V) {
  Hash.update(HashKind::TAG_AtomicOrdering);
  Hash.update(static_cast<uint8_t>(V));
}

void HashCalculator::attributeHash(const Attribute &V) {
  if (V.isEnumAttribute()) {
    // Enum attribute uses the attribute kind to calculate the hash.
    Hash.update(HashKind::TAG_AttributeEnum);
    auto EnunKind = V.getKindAsEnum();
    Hash.update(
        ArrayRef<uint8_t>((uint8_t *)&EnunKind, sizeof(Attribute::AttrKind)));
  } else if (V.isIntAttribute()) {
    // Int attribute uses the attribute kind and int value to calculate the
    // hash.
    Hash.update(HashKind::TAG_AttributeInt);
    auto EnunKind = V.getKindAsEnum();
    Hash.update(
        ArrayRef<uint8_t>((uint8_t *)&EnunKind, sizeof(Attribute::AttrKind)));
    numberHash(V.getValueAsInt());
  } else {
    // String attribute uses the attribute kind and string value to calculate
    // the hash.
    Hash.update(HashKind::TAG_AttributeString);
    memHash(V.getKindAsString());
    memHash(V.getValueAsString());
  }
}

void HashCalculator::attributeListHash(const AttributeList &V) {
  Hash.update(HashKind::TAG_AttributeList);
  for (unsigned I = 0, E = V.getNumAttrSets(); I != E; ++I) {
    for (unsigned I = V.index_begin(), E = V.index_end(); I != E; ++I) {
      AttributeSet AS = V.getAttributes(I);
      for (AttributeSet::iterator VI = AS.begin(), VE = AS.end(); VI != VE;
           ++VI) {
        attributeHash(*VI);
      }
    }
  }
}

void HashCalculator::inlineAsmHash(const InlineAsm *V) {
  Hash.update(HashKind::TAG_InlineAsm);
  typeHash(V->getFunctionType());
  memHash(V->getAsmString());
  memHash(V->getConstraintString());
  Hash.update(HashKind::TAG_InlineAsm_SideEffects);
  Hash.update(V->hasSideEffects());
  Hash.update(HashKind::TAG_InlineAsm_AlignStack);
  Hash.update(V->isAlignStack());
  Hash.update(HashKind::TAG_InlineAsm_Dialect);
  Hash.update(V->getDialect());
}

void HashCalculator::rangeMetadataHash(const MDNode *V) {
  if (!V)
    return;
  Hash.update(HashKind::TAG_RangeMetadata);
  // Range metadata is a sequence of numbers.
  for (size_t I = 0; I < V->getNumOperands(); ++I) {
    ConstantInt *VLow = mdconst::extract<ConstantInt>(V->getOperand(I));
    APIntHash(VLow->getValue());
  }
}

/// typeHash - calculate a type hash.
void HashCalculator::typeHash(Type *Ty) {
  Hash.update(HashKind::TAG_Type);
  Hash.update(Ty->getTypeID());

  switch (Ty->getTypeID()) {
  default:
    llvm_unreachable("Unknown type!");
    // Fall through in Release mode.
    LLVM_FALLTHROUGH;
  // PrimitiveTypes
  case Type::VoidTyID:
  case Type::FloatTyID:
  case Type::DoubleTyID:
  case Type::X86_FP80TyID:
  case Type::FP128TyID:
  case Type::PPC_FP128TyID:
  case Type::LabelTyID:
  case Type::MetadataTyID:
  case Type::TokenTyID:
    break;

  // Derived types
  case Type::IntegerTyID:
    // unsigned int BWidth = cast<IntegerType>(Ty)->getBitWidth();
    // Hash.update(ArrayRef<uint8_t>((uint8_t *)&BWidth, sizeof(unsigned int)));
    numberHash(cast<IntegerType>(Ty)->getBitWidth());
    break;
  case Type::FunctionTyID: {
    FunctionType *FTy = cast<FunctionType>(Ty);
    for (Type *ParamTy : FTy->params()) {
      typeHash(ParamTy);
    }
    Hash.update(FTy->isVarArg());
    typeHash(FTy->getReturnType());
    break;
  }
  case Type::PointerTyID: {
    PointerType *PTy = dyn_cast<PointerType>(Ty);
    assert(PTy && "Ty type must be pointers here.");
    numberHash(PTy->getAddressSpace());
    break;
  }
  case Type::StructTyID: {
    StructType *STy = cast<StructType>(Ty);
    for (Type *ElemTy : STy->elements()) {
      typeHash(ElemTy);
    }
    if (STy->isPacked())
      Hash.update(STy->isPacked());
    break;
  }
  case Type::ArrayTyID:
  case Type::VectorTyID: {
    auto *STy = cast<SequentialType>(Ty);
    numberHash(STy->getNumElements());
    typeHash(STy->getElementType());
    break;
  }
  }
}

/// Accumulate the constants hash.
void HashCalculator::constantHash(const Constant *V) {
  DEBUG(dbgs() << "Constant V name:  " << V->getName() << "\n");

  Hash.update(HashKind::TAG_Constant);
  Type *Ty = V->getType();
  // Calculate type hash.
  typeHash(Ty);

  auto GlobalValueV = dyn_cast<GlobalValue>(V);
  if (GlobalValueV) {
    auto *GV = dyn_cast<GlobalVariable>(GlobalValueV);
    if (GV && GV->hasDefinitiveInitializer()) {
      DenseMap<const GlobalValue *, unsigned>::iterator GVI =
          global_numbers.find(GV);
      if (GVI != global_numbers.end())
        globalValueHash(GlobalValueV);
      else {
        global_numbers.insert(std::make_pair(GV, global_numbers.size()));
        constantHash(GV->getInitializer());
      }
    }
    return;
  }

  unsigned int VID = V->getValueID();
  // Hash.update(ArrayRef<uint8_t>((uint8_t *)&VID, sizeof(unsigned int)));
  numberHash(VID);

  if (const auto *SeqV = dyn_cast<ConstantDataSequential>(V)) {
    // This handles ConstantDataArray and ConstantDataVector.
    memHash(SeqV->getRawDataValues());
    return;
  }

  switch (VID) {
  case Value::UndefValueVal:
  case Value::ConstantTokenNoneVal:
  case Value::ConstantAggregateZeroVal:
  case Value::ConstantPointerNullVal:
    break;
  case Value::ConstantIntVal: {
    APIntHash(cast<ConstantInt>(V)->getValue());
    break;
  }
  case Value::ConstantFPVal: {
    APFloatHash(cast<ConstantFP>(V)->getValueAPF());
    break;
  }
  case Value::ConstantArrayVal: {
    const ConstantArray *VA = cast<ConstantArray>(V);
    for (uint64_t I = 0; I < cast<ArrayType>(Ty)->getNumElements(); ++I) {
      constantHash(cast<Constant>(VA->getOperand(I)));
    }
    break;
  }
  case Value::ConstantStructVal: {
    const ConstantStruct *VS = cast<ConstantStruct>(V);
    for (unsigned I = 0; I < cast<StructType>(Ty)->getNumElements(); ++I) {
      constantHash(cast<Constant>(VS->getOperand(I)));
    }
    break;
  }
  case Value::ConstantVectorVal: {
    const ConstantVector *VV = cast<ConstantVector>(V);
    for (uint64_t I = 0; I < cast<VectorType>(Ty)->getNumElements(); ++I) {
      constantHash(cast<Constant>(VV->getOperand(I)));
    }
    break;
  }
  case Value::ConstantExprVal: {
    const ConstantExpr *VE = cast<ConstantExpr>(V);
    for (unsigned i = 0; i < VE->getNumOperands(); ++i) {
      constantHash(cast<Constant>(VE->getOperand(i)));
    }
    break;
  }
  case Value::BlockAddressVal: {
    const BlockAddress *BA = cast<BlockAddress>(V);
    valueHash(BA->getFunction());
    // valueHash will tell us if these are equivalent BasicBlocks, in the
    // context of their respective functions.
    valueHash(BA->getBasicBlock());
    break;
  }
  default: // Unknown constant, abort.
    DEBUG(dbgs() << "Looking at valueID " << V->getValueID() << "\n");
    llvm_unreachable("Constant ValueID not recognized.");
  }
}

/// Calculate the value hash under pair-wise comparison. If this is the first
/// time the values are seen, they're added to the mapping so that we will
/// detect mismatches on next use. See comments in declaration for more details.
void HashCalculator::valueHash(const Value *V) {
  Hash.update(HashKind::TAG_Value);
  const Constant *ConstV = dyn_cast<Constant>(V);
  if (ConstV) {
    constantHash(ConstV);
    return;
  }

  const InlineAsm *InlineAsmV = dyn_cast<InlineAsm>(V);
  if (InlineAsmV) {
    inlineAsmHash(InlineAsmV);
    return;
  }

  auto *GV = dyn_cast<GlobalVariable>(V);
  if (!GV) {
    auto GA = dyn_cast<GlobalAlias>(V);
    if (GA)
      GV = dyn_cast<GlobalVariable>(GA->getAliasee()->stripPointerCasts());
  }
  if (GV && !GV->getName().empty()) {
    memHash(GV->getName());
    return;
  }

  auto SN = sn_map.insert(std::make_pair(V, sn_map.size()));
  numberHash(SN.first->second);
}

void HashCalculator::globalValueHash(const GlobalValue *V) {
  numberHash(V->getGUID());
  auto *GV = dyn_cast<GlobalVariable>(V);
  if (GV && GV->hasDefinitiveInitializer()) {
    DenseMap<const GlobalValue *, unsigned>::iterator GVI =
        global_numbers.find(GV);
    if (GVI == global_numbers.end()) {
      global_numbers.insert(std::make_pair(GV, global_numbers.size()));
      constantHash(GV->getInitializer());
    } else {
      numberHash(GVI->second);
    }
  }
}

std::string &HashCalculator::get(MD5::MD5Result &HashRes) {
  SmallString<32> Result;
  MD5::stringifyResult(HashRes, Result);
  TheHash = Result.str();
  return TheHash;
}

void FunctionHashCalculator::signatureHash(const Function *F) {
  FnHash.Hash.update(HashKind::TAG_Signature);
  // TODO: review all the attributes to find the stardard c++ attributes to
  // affect the generated codes.
  FnHash.attributeListHash(F->getAttributes());
  if (F->hasGC()) {
    FnHash.Hash.update(HashKind::TAG_Signature_GC);
    FnHash.memHash(F->getGC());
  }
  if (F->hasSection()) {
    FnHash.Hash.update(HashKind::TAG_Signature_Sec);
    FnHash.memHash(F->getSection());
  }
  FnHash.Hash.update(HashKind::TAG_Signature_VarArg);
  FnHash.Hash.update(F->isVarArg());

  // Calling conventions may differ in where parameters, return values and
  // return addresses are placed (in registers, on the call stack, a mix of
  // both, or in other memory structures). If the function has input
  // paramaters, the generated codes will be different, the calling conventions
  // need to be consided in the hash calculation. If the function return type
  // is not void type, the generated code would be changed. Again, the calling
  // conventions need to be considered.
  if (F->getFunctionType()->getNumParams() != 0 ||
      F->getReturnType()->getTypeID() == Type::VoidTyID) {
    FnHash.Hash.update(HashKind::TAG_Signature_CC);
    CallingConv::ID CC = F->getCallingConv();
    FnHash.Hash.update(
        ArrayRef<uint8_t>((uint8_t *)&CC, sizeof(CallingConv::ID)));
  }

  FnHash.typeHash(F->getFunctionType());
  // Visit the arguments so that they get enumerated in the order they're
  // passed in.
  FnHash.Hash.update(HashKind::TAG_Signature_Arg);
  for (Function::const_arg_iterator ArgI = F->arg_begin(), ArgE = F->arg_end();
       ArgI != ArgE; ++ArgI) {
    FnHash.valueHash(&*ArgI);
  }
}

void FunctionHashCalculator::moduleHash(Module &M) {
  FnHash.Hash.update(HashKind::TAG_Datalayout);
  FnHash.memHash(M.getDataLayoutStr());
  FnHash.Hash.update(HashKind::TAG_Triple);
  FnHash.memHash(M.getTargetTriple());
}

// Calculate either CallInst or InvokeInst instruction hash.
void FunctionHashCalculator::operandBundlesHash(const Instruction *V) {
  FnHash.Hash.update(HashKind::TAG_OperandBundles);
  ImmutableCallSite VCS(V);
  assert(VCS && "Must not be empty!");
  assert((VCS.isCall() || VCS.isInvoke()) && "Must be calls or invokes!");

  for (unsigned i = 0, e = VCS.getNumOperandBundles(); i != e; ++i) {
    auto VOB = VCS.getOperandBundleAt(i);
    FnHash.memHash(VOB.getTagName());
    // Since input values have been used to calculate the instruction hash for
    // all instructions, we only consider the input sizes here.
    FnHash.numberHash(VOB.Inputs.size());
  }
}

/// Accumulate the instruction hash. The opcodes, type, operand types, operands
/// value and any other factors affecting the operation must be considered.
void FunctionHashCalculator::instructionHash(const Instruction *V) {
  FnHash.Hash.update(HashKind::TAG_Instruction);
  // Accumulate the hash of the instruction opcode.
  FnHash.numberHash(V->getOpcode());
  // Instruction return type.
  FnHash.typeHash(V->getType());
  FnHash.numberHash(V->getRawSubclassOptionalData());

  // Accumulate the instruction operands type and value.
  for (unsigned I = 0, E = V->getNumOperands(); I != E; ++I) {
    FnHash.typeHash(V->getOperand(I)->getType());
    FnHash.valueHash(V->getOperand(I));
  }

  // special GetElementPtrInst instruction.
  if (const GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(V)) {
    FnHash.Hash.update(HashKind::TAG_GetElementPtrInst);
    FnHash.typeHash(GEP->getSourceElementType());
    return;
  }
  // Check special state that is a part of some instructions.
  if (const AllocaInst *AI = dyn_cast<AllocaInst>(V)) {
    FnHash.Hash.update(HashKind::TAG_AllocaInst);
    FnHash.typeHash(AI->getAllocatedType());
    FnHash.numberHash(AI->getAlignment());
    return;
  }
  if (const LoadInst *LI = dyn_cast<LoadInst>(V)) {
    FnHash.Hash.update(HashKind::TAG_LoadInst);
    FnHash.Hash.update(LI->isVolatile());
    FnHash.numberHash(LI->getAlignment());
    FnHash.orderingHash(LI->getOrdering());
    FnHash.Hash.update(LI->getSynchScope());
    // FIXME: Is there any other Metadata need to be considered??
    FnHash.rangeMetadataHash(LI->getMetadata(LLVMContext::MD_range));
    return;
  }
  if (const StoreInst *SI = dyn_cast<StoreInst>(V)) {
    FnHash.Hash.update(HashKind::TAG_StoreInst);
    FnHash.Hash.update(SI->isVolatile());
    FnHash.numberHash(SI->getAlignment());
    FnHash.orderingHash(SI->getOrdering());
    FnHash.Hash.update(SI->getSynchScope());
    return;
  }
  if (const CmpInst *CI = dyn_cast<CmpInst>(V)) {
    FnHash.Hash.update(HashKind::TAG_CmpInst);
    FnHash.Hash.update(CI->getPredicate());
    return;
  }
  if (const CallInst *CI = dyn_cast<CallInst>(V)) {
    FnHash.Hash.update(HashKind::TAG_CallInst);
    FnHash.Hash.update(CI->isTailCall());
    // FnHash.numberHash(CI->getCallingConv());
    FnHash.attributeListHash(CI->getAttributes());
    operandBundlesHash(CI);
    FnHash.rangeMetadataHash(CI->getMetadata(LLVMContext::MD_range));
    if (const Function *F = CI->getCalledFunction()) {
      FnHash.memHash(F->getName());
    }
    return;
  }
  if (const InvokeInst *II = dyn_cast<InvokeInst>(V)) {
    FnHash.Hash.update(HashKind::TAG_InvokeInst);
    FnHash.numberHash(II->getCallingConv());
    FnHash.attributeListHash(II->getAttributes());
    operandBundlesHash(II);
    FnHash.rangeMetadataHash(II->getMetadata(LLVMContext::MD_range));
    if (const Function *F = II->getCalledFunction()) {
      FnHash.memHash(F->getName());
    }
    return;
  }
  if (const InsertValueInst *IVI = dyn_cast<InsertValueInst>(V)) {
    FnHash.Hash.update(HashKind::TAG_InsertValueInst);
    ArrayRef<unsigned> Indices = IVI->getIndices();
    // for (size_t I = 0, E = Indices.size(); I != E; ++I) {
    //  FnHash.numberHash(Indices[I]);
    //}
    FnHash.Hash.update(ArrayRef<uint8_t>((uint8_t *)&Indices,
                                         sizeof(unsigned) * Indices.size()));
    return;
  }
  if (const ExtractValueInst *EVI = dyn_cast<ExtractValueInst>(V)) {
    FnHash.Hash.update(HashKind::TAG_ExtractValueInst);
    ArrayRef<unsigned> Indices = EVI->getIndices();
    // for (size_t I = 0, E = Indices.size(); I != E; ++I) {
    //  FnHash.numberHash(Indices[I]);
    //}
    FnHash.Hash.update(ArrayRef<uint8_t>((uint8_t *)&Indices,
                                         sizeof(unsigned) * Indices.size()));
    return;
  }
  if (const FenceInst *FI = dyn_cast<FenceInst>(V)) {
    FnHash.Hash.update(HashKind::TAG_FenceInst);
    FnHash.orderingHash(FI->getOrdering());
    FnHash.Hash.update(FI->getSynchScope());
    return;
  }
  if (const AtomicCmpXchgInst *CXI = dyn_cast<AtomicCmpXchgInst>(V)) {
    FnHash.Hash.update(HashKind::TAG_AtomicCmpXchgInst);
    FnHash.Hash.update(CXI->isVolatile());
    FnHash.Hash.update(CXI->isWeak());
    FnHash.orderingHash(CXI->getSuccessOrdering());
    FnHash.orderingHash(CXI->getFailureOrdering());
    FnHash.Hash.update(CXI->getSynchScope());
    return;
  }
  if (const AtomicRMWInst *RMWI = dyn_cast<AtomicRMWInst>(V)) {
    FnHash.Hash.update(HashKind::TAG_AtomicRMWInst);
    FnHash.Hash.update(RMWI->getOperation());
    FnHash.Hash.update(RMWI->isVolatile());
    FnHash.orderingHash(RMWI->getOrdering());
    FnHash.Hash.update(RMWI->getSynchScope());
    return;
  }
  if (const PHINode *PN = dyn_cast<PHINode>(V)) {
    FnHash.Hash.update(HashKind::TAG_PHINode);
    // Ensure that in addition to the incoming values being identical
    // (checked by the caller of this function), the incoming blocks
    // are also identical.
    for (unsigned I = 0, E = PN->getNumIncomingValues(); I != E; ++I) {
      FnHash.valueHash(PN->getIncomingBlock(I));
    }
  }
}

void FunctionHashCalculator::basicBlockHash(const BasicBlock *BB) {
  FnHash.Hash.update(HashKind::TAG_BasicBlock);
  BasicBlock::const_iterator Inst = BB->begin(), InstE = BB->end();
  do {
    instructionHash(&*Inst);
    ++Inst;
  } while (Inst != InstE);
}

void FunctionHashCalculator::calculateFunctionHash(Module &M) {
  FnHash.beginCalculate();
  FnHash.Hash.update(HashKind::TAG_GlobalFunction);
  moduleHash(M);
  signatureHash(Fn);

  // We do a CFG-ordered walk since the actual ordering of the blocks in the
  // linked list is immaterial. Our walk starts at the entry block for both
  // functions, then takes each block from each terminator in order. As an
  // artifact, this also means that unreachable blocks are ignored.
  SmallVector<const BasicBlock *, 8> FnBBs;
  SmallPtrSet<const BasicBlock *, 32> VisitedBBs; // in terms of F1.

  FnBBs.push_back(&Fn->getEntryBlock());

  VisitedBBs.insert(FnBBs[0]);
  while (!FnBBs.empty()) {
    const BasicBlock *BB = FnBBs.pop_back_val();
    FnHash.valueHash(BB);
    basicBlockHash(BB);

    const TerminatorInst *Term = BB->getTerminator();
    for (unsigned I = 0, E = Term->getNumSuccessors(); I != E; ++I) {
      if (!VisitedBBs.insert(Term->getSuccessor(I)).second)
        continue;
      FnBBs.push_back(Term->getSuccessor(I));
    }
  }
}

/// Calculate the function hash and return the result as the words.
void FunctionHashCalculator::getHashResult(MD5::MD5Result &HashRes) {
  return FnHash.getHashResult(HashRes);
}

void VaribleHashCalculator::comdatHash() {
  GvHash.Hash.update(HashKind::TAG_GVComdat);
  const Comdat *GvC = Gv->getComdat();
  GvHash.Hash.update(GvC->getName());
  GvHash.Hash.update(GvC->getSelectionKind());
}

void VaribleHashCalculator::moduleHash(Module &M) {
  GvHash.Hash.update(HashKind::TAG_Datalayout);
  GvHash.memHash(M.getDataLayoutStr());
  GvHash.Hash.update(HashKind::TAG_Triple);
  GvHash.memHash(M.getTargetTriple());
}

// Calculate the global varible hash value.
void VaribleHashCalculator::calculateVaribleHash(Module &M) {
  GvHash.Hash.update(HashKind::TAG_GlobalVarible);
  GvHash.beginCalculate();
  moduleHash(M);

  // Accumulate the global variable name
  // if (Gv->hasName()) {
  //  GvHash.Hash.update(HashKind::TAG_GVName);
  //  GvHash.memHash(Gv->getName());
  //}
  // Global variables which are defined in the different file have differetn
  // hash values.
  // GvHash.Hash.update(HashKind::TAG_GVSourceFileName);
  // GvHash.memHash(Gv->getParent()->getSourceFileName());
  // Value type.
  GvHash.typeHash(Gv->getValueType());
  //// Accumulate the linkage type.
  // GvHash.Hash.update(Gv->getLinkage());
  // If global variable is constant, accumulate the const attribute.
  GvHash.Hash.update(HashKind::TAG_GVConstant);
  GvHash.Hash.update(Gv->isConstant());
  //// Accumulate meaningful attributes for global variable.
  // GvHash.Hash.update(HashKind::TAG_GVVisibility);
  // GvHash.Hash.update(Gv->getVisibility());
  // Accumulate the thread local mode.
  GvHash.Hash.update(HashKind::TAG_GVThreadLocalMode);
  GvHash.Hash.update(Gv->getThreadLocalMode());
  // Accumulate the alignment of global variable.
  GvHash.Hash.update(HashKind::TAG_GVAlignment);
  GvHash.numberHash(Gv->getAlignment());
  // Accumulate an optional unnamed_addr or local_unnamed_addr attribute.
  GvHash.Hash.update(HashKind::TAG_GVUnnamedAddr);
  GvHash.Hash.update (static_cast <uint8_t> (Gv->getUnnamedAddr()));
  //// Accumulate the DLL storage class type.
  // GvHash.Hash.update(HashKind::TAG_GVDLLStorageClassType);
  // GvHash.Hash.update(Gv->getDLLStorageClass());
  // Accumulate the Comdat section name.
  if (Gv->hasComdat()) {
    comdatHash();
  }
  if (Gv->hasName() && Gv->hasDefinitiveInitializer()) {
    // Global variable is constant type. Accumulate the initial value.
    // This accumulation also cover the "llvm.global_ctors",
    // "llvm.global_dtors", "llvm.used" and "llvm.compiler.used" cases.
    GvHash.Hash.update(HashKind::TAG_GVInitValue);
    GvHash.constantHash(Gv->getInitializer());
  }
}

// Calculate the global varible hash value.
HashType AliasHashCalculator::calculate() {
  GaHash.Hash.update(HashKind::TAG_GlobalAlias);
  GaHash.beginCalculate();
  // Accumulate the global variable name
  // if (Ga->hasName()) {
  //  GaHash.Hash.update(HashKind::TAG_GVName);
  //  GaHash.memHash(Ga->getName());
  //}
  // Global variables which are defined in the different file have differetn
  // hash values.
  // GaHash.Hash.update(HashKind::TAG_GVSourceFileName);
  // GaHash.memHash(Ga->getParent()->getSourceFileName());
  // Value type.
  GaHash.typeHash(Ga->getValueType());
  // Accumulate the linkage type.
  GaHash.Hash.update(Ga->getLinkage());
  // Accumulate meaningful attributes for global variable.
  GaHash.Hash.update(HashKind::TAG_GVVisibility);
  GaHash.Hash.update(Ga->getVisibility());
  // Accumulate the thread local mode.
  GaHash.Hash.update(HashKind::TAG_GVThreadLocalMode);
  GaHash.Hash.update(Ga->getThreadLocalMode());
  // Accumulate the alignment of global variable.
  GaHash.Hash.update(HashKind::TAG_GVAlignment);
  GaHash.numberHash(Ga->getAlignment());
  // Accumulate an optional unnamed_addr or local_unnamed_addr attribute.
  GaHash.Hash.update(HashKind::TAG_GVUnnamedAddr);
  GaHash.Hash.update (static_cast <uint8_t> (Ga->getUnnamedAddr()));
  // Accumulate the DLL storage class type.
  GaHash.Hash.update(HashKind::TAG_GVDLLStorageClassType);
  GaHash.Hash.update(Ga->getDLLStorageClass());

  GaHash.constantHash(Ga->getAliasee());

  // Now return the result.
  MD5::MD5Result Result;
  GaHash.Hash.final(Result);

  // ... take the least significant 8 bytes and return those. Our MD5
  // implementation always returns its results in little endian, so we actually
  // need the "high" word.
  return Result.words();
}
