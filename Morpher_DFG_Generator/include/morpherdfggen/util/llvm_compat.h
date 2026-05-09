#ifndef MORPHERDFGGEN_UTIL_LLVM_COMPAT_H
#define MORPHERDFGGEN_UTIL_LLVM_COMPAT_H

#include "llvm/IR/Constants.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/Instructions.h"

namespace morpher {
namespace llvm_compat {

using namespace llvm;

inline LoadInst *createTypedLoad(IRBuilder<> &Builder, Type *ValueType, Value *Pointer,
                                 const Twine &Name = "") {
  return Builder.CreateLoad(ValueType, Pointer, Name);
}

inline Value *createTypedGEP(IRBuilder<> &Builder, Type *SourceElementType, Value *Pointer,
                             Value *Index, const Twine &Name = "") {
  return Builder.CreateGEP(SourceElementType, Pointer, Index, Name);
}

inline Value *createTypedGEP(IRBuilder<> &Builder, Type *SourceElementType, Value *Pointer,
                             ArrayRef<Value *> Indices, const Twine &Name = "") {
  return Builder.CreateGEP(SourceElementType, Pointer, Indices, Name);
}

inline PointerType *getInt8PtrTy(LLVMContext &Ctx, unsigned AddressSpace = 0) {
  return PointerType::get(Type::getInt8Ty(Ctx), AddressSpace);
}

inline unsigned getLogicalPointerDepthForGEP(const GetElementPtrInst *GEP) {
  Type *SourceType = GEP->getSourceElementType();
  if (!SourceType->isPointerTy()) {
    return 1;
  }
  return 2;
}

inline Type *getLoadStoreValueType(const Instruction *I) {
  if (const auto *LI = dyn_cast<LoadInst>(I)) {
    return LI->getType();
  }
  if (const auto *SI = dyn_cast<StoreInst>(I)) {
    return SI->getValueOperand()->getType();
  }
  return nullptr;
}

}  // namespace llvm_compat
}  // namespace morpher

#endif
