//===--- TFUtilities.cpp - TensorFlow lowering utilities ------------------===//
//
// This source file is part of the Swift.org open source project
//
// Copyright (c) 2014 - 2017 Apple Inc. and the Swift project authors
// Licensed under Apache License v2.0 with Runtime Library Exception
//
// See https://swift.org/LICENSE.txt for license information
// See https://swift.org/CONTRIBUTORS.txt for the list of Swift project authors
//
//===----------------------------------------------------------------------===//

#include "TFUtilities.h"
#include "swift/AST/ASTContext.h"
#include "swift/AST/DiagnosticsSIL.h"
#include "swift/SIL/SILModule.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSwitch.h"
#include "llvm/Support/CommandLine.h"
#ifdef SWIFT_ENABLE_TENSORFLOW
#ifdef CMAKE_INTDIR
#include "tensorflow/c/c_api.h"
#else
#include "tensorflow/c/c_api.h"
#endif
#endif

using namespace swift;
using namespace tf;

template<typename...T, typename...U>
static InFlightDiagnostic
diagnose(ASTContext &Context, SourceLoc loc, Diag<T...> diag, U &&...args) {
  return Context.Diags.diagnose(loc, diag, std::forward<U>(args)...);
}

static llvm::cl::opt<bool>
TFDumpIntermediates("tf-dump-intermediates", llvm::cl::init(false),
                    llvm::cl::desc("Dump intermediate results in TensorFlow passes"));

/// This returns true if we should dump out intermediate results to standard
/// out.  This is used for integration unit tests.
bool tf::shouldDumpIntermediates() {
  return TFDumpIntermediates;
}


/// If the specified type is the well-known TensorHandle<T> type, then return
/// "T".  If not, return a null type.
Type tf::isTensorHandle(Type ty) {
  if (auto bgct = ty->getAs<BoundGenericClassType>()) {
    if (bgct->getDecl()->getNameStr() == "TensorHandle") {
      assert(bgct->getGenericArgs().size() == 1 && "Expected one generic arg");
      return bgct->getGenericArgs()[0];
    }
  }
  return Type();
}

static bool is64(Type ty) {
  return ty->getASTContext().LangOpts.Target.isArch64Bit();
}

/// This function maps a Swift type (either a language type like Float or an
/// LLVM Builtin type like Builtin.f32) into the TensorFlow TF_DataType value.
///
/// This returns 0 (which is an invalid tensorflow type ID) on error.
///
unsigned tf::convertSwiftTypeToTF(Type ty) {
#ifdef SWIFT_ENABLE_TENSORFLOW
  // Handle wrappers like Float, which come up in TensorHandle<Float>
  if (auto *s = ty->getAs<StructType>()) {
    // Make sure the type is defined inside the Swift module.
    auto context = s->getDecl()->getDeclContext()->getParentModule();
    if (!context || context->getName().str() != "Swift")
      return 0;

    return llvm::StringSwitch<unsigned>(s->getDecl()->getNameStr())
      .Case("Bool", TF_BOOL)
      .Case("Int8", TF_INT8)
      .Case("UInt8", TF_UINT8)
      .Case("Int16", TF_INT16)
      .Case("UInt16", TF_UINT16)
      .Case("Int32", TF_INT32)
      .Case("UInt32", TF_UINT32)
      .Case("Int64", TF_INT64)
      .Case("UInt64", TF_UINT64)
      .Case("Int8", TF_INT8)
      .Case("UInt8", TF_UINT8)
      .Case("Float", TF_FLOAT)
      .Case("Double", TF_DOUBLE)
      .Case("Int", is64(s) ? TF_INT64 : TF_INT32)
      .Case("UInt", is64(s) ? TF_UINT64 : TF_UINT32)
      .Default(0);
  }

  // BuiltinIntegerType doesn't carry sign information, which TensorFlow needs,
  // so we can't rely on getting type information from the builtin types
  // themselves.  For now we'll just use signed types.
  if (auto *BII = ty->getAs<BuiltinIntegerType>()) {
    if (BII->getWidth().isPointerWidth())
      return is64(ty) ? TF_INT64 : TF_INT32;

    switch (BII->getFixedWidth()) {
    case 1: return TF_BOOL;
    case 8: return TF_INT8;
    case 16: return TF_INT16;
    case 32: return TF_INT32;
    case 64: return TF_INT64;
    }
  }

  if (auto *BIF = ty->getAs<BuiltinFloatType>()) {
    switch (BIF->getFPKind()) {
    case BuiltinFloatType::IEEE16: return TF_HALF;
    case BuiltinFloatType::IEEE32: return TF_FLOAT;
    case BuiltinFloatType::IEEE64: return TF_DOUBLE;
    case BuiltinFloatType::IEEE80:
    case BuiltinFloatType::IEEE128:
    case BuiltinFloatType::PPC128:
      return 0;
    }
  }
#endif
  return 0;
}


/// Given a function name that might refer to a tensorflow op function, this
/// returns the op name and operand description and returns true.  If the
/// function name doesn't correspond to an op, this returns false.
static bool decodeTensorOpName(StringRef name, StringRef &opName,
                               StringRef &typeDescriptorStr,
                               SmallVectorImpl<StringRef> &attributeNames) {
  // Op functions are expected to be of the form:
  //  __tfop_<OPNAME>__<OPERANDDESC>__<ATTRIBUTES>
  if (!name.startswith("__tfop_")) return false;
  name = name.substr(strlen("__tfop_"));

  auto pos = name.find(",");
  if (pos == StringRef::npos) return false;
  opName = name.substr(0, pos);
  name = name.substr(pos+strlen(","));

  pos = name.find(",");
  typeDescriptorStr = name.substr(0, pos);
  if (pos == StringRef::npos)
    return true;
  name = name.substr(pos);

  // Parse out any attribute names.
  while (!name.empty()) {
    assert(name[0] == ',');
    name = name.drop_front(1);

    pos = name.find(",");
    if (pos == StringRef::npos) pos = name.size();

    attributeNames.push_back(name.substr(0, pos));
    name = name.substr(pos);
  }

  return true;
}


SILValue SILTensorOpInfo::getScalarOperand(SILValue v) {
  // We have to handle two kinds of operands: SIL address operands and normal
  // values.
  if (!v->getType().isAddress()) {
    // If we have a normal operand, handle the form where a StructInst is
    // Swift stdlib type (e.g. Int/Float) wrapping an underlying LLVM value.
    if (auto *SI = dyn_cast<StructInst>(v))
      if (SI->getNumOperands() == 1)
        return SI->getOperand(0);

    return v;
  }

  // Because we're often coming from generic code, we frequently get a value
  // passed by-address.  Check for an alloc_stack with a single store to it and
  // consume the stored value.
  if (auto *ASI = dyn_cast<AllocStackInst>(v)) {
    if (auto *store = ASI->getSingleUserOfType<StoreInst>())
      return getScalarOperand(store->getSrc());
  }

  // Otherwise this is a by-address value that we can't handle:
  // FIXME: The proper way to deal with this is with a deabstraction pass,
  // which will guarantee generic specialization promotes the builtin operand
  // to never be an address.
  return SILValue();
}

/// If the specified value is a valid value for a constant operand, return the
/// literal it is initialized to, otherwise null.
LiteralInst *SILTensorOpInfo::getTensorConstantOperand(SILValue v) {
  // Simplify scalar operands in general.
  v = getScalarOperand(v);
  if (!v) return nullptr;

  // If this is an integer or fp literal, we succeed.
  if (isa<IntegerLiteralInst>(v) || isa<FloatLiteralInst>(v))
    return cast<LiteralInst>(v);

  return nullptr;
}

/// If the specified value is a valid value for an attribute, return the
/// instruction that provides the value, otherwise null.
SILInstruction *SILTensorOpInfo::getAttrOperand(SILValue v) {
  // Simplify scalar operands in general.
  v = getScalarOperand(v);
  if (!v) return nullptr;

  // If we have an acceptable values for an attribute, return it.
  if (auto *fli = dyn_cast<FloatLiteralInst>(v))
    return fli;
  if (auto *ili = dyn_cast<IntegerLiteralInst>(v))
    return ili->getValue().getBitWidth() <= 64 ? ili : nullptr;
  if (auto *sli = dyn_cast<StringLiteralInst>(v))
    return sli->getEncoding() == StringLiteralInst::Encoding::UTF8
           ? sli : nullptr;
  if (auto *mti = dyn_cast<MetatypeInst>(v)) {
    auto ty = mti->getType().castTo<AnyMetatypeType>()->getInstanceType();
    if (convertSwiftTypeToTF(ty) != 0) return mti;
  }

  return nullptr;
}


/// Analyze the specified SIL instruction and return a SILTensorOpInfo result if
/// the instruction is a valid tensor operation.  This is the way that
/// SILTensorOpInfo's are created.
Optional<SILTensorOpInfo>
SILTensorOpInfo::decode(SILInstruction *inst) {
  // Tuple extracts of tensor ops are considered to be themselves Tensor
  // operations, since they are part of the core representation of nodes that
  // produce multiple results.
  if (auto *ti = dyn_cast<TupleExtractInst>(inst))
    if (auto *ai = dyn_cast<BuiltinInst>(ti->getOperand()))
      return decode(ai);

  SILTensorOpInfo toiInfo(*inst);

  // Tensor operations are builtin instructions and apply instructions.
  if (auto *builtinInst = dyn_cast<BuiltinInst>(inst))
    if (toiInfo.decodeBuiltin(builtinInst))
      return toiInfo;

  // Operations which can conditionally run on the host or the accelerator are
  // modeled as well-known function calls.  If they satisfy the requirements
  // (e.g. that their parameters are constants we can analyze) then they get
  // promoted to notional "ops".
  if (auto *applyInst = dyn_cast<ApplyInst>(inst))
    if (auto *fnRef = dyn_cast<FunctionRefInst>(applyInst->getCallee())) {
      auto callee = fnRef->getReferencedFunction()->getName();
      if (callee == "__tf_init_scalar")
        if (toiInfo.decodeTFInitScalar(applyInst))
          return toiInfo;
    }

  return None;
}

/// The vast majority of interesting tensor operations are builtin instructions,
/// which come from the user-exposed #tfop() syntax.
bool SILTensorOpInfo::decodeBuiltin(BuiltinInst *inst) {
  builtinName = inst->getName().str();

  // If the name is valid, it isn't an op.
  StringRef typeDescriptorStr;
  if (!decodeTensorOpName(builtinName, opName, typeDescriptorStr,
                          attributeNames))
    return false;

  auto diagInvalid = [&](std::string problem) {
    diagnose(inst->getModule().getASTContext(), inst->getLoc().getSourceLoc(),
             diag::tfop_incorrect_operandinfo,
             operandDescriptorStr, problem);
  };

  // The type descriptor has operand and result info separated by a colon.
  auto colonLoc = typeDescriptorStr.find(':');
  if (colonLoc == StringRef::npos) {
    diagInvalid("no colon in type descriptor");
    return false;
  }

  auto errInfo = decodeDescriptorString(typeDescriptorStr);
  if (errInfo.isError()) {
    diagInvalid(errInfo.message);
    return false;
  }

  // Validate that this instruction is ok.
  unsigned nextOperand = 0;
  auto getNextOperand = [&]() -> SILValue {
    // If we ran out of operands, something is wrong.
    if (nextOperand >= inst->getNumOperands()) {
      diagInvalid("expected more operands than the " +
                  llvm::utostr(inst->getNumOperands()-1) + " present");
      return SILValue();
    }
    return inst->getOperand(nextOperand++);
  };

  for (auto opInfo : operandDescriptors) {
    switch (opInfo) {
    case OpDescriptor::Tensor: {
      auto op = getNextOperand();
      if (!op) return false;  // diagnostic already emitted.
      if (!isTensorHandle(op->getType().getSwiftRValueType())) {
        diagInvalid("expected " +
                    llvm::utostr(nextOperand-2) + " to be a tensor");
        return false;
      }
      break;
    }
    case OpDescriptor::AddDType: {
      // 'd' doesn't correspond to an operand, but requires a tensor result.
      if (!isTensorHandle(inst->getType().getSwiftRValueType())) {
        diagInvalid("'d' operand requires a Tensor result");
        return false;
      }
      break;
    }
    case OpDescriptor::Scalar: {
      // This requires a scalar value.
      auto op = getNextOperand();
      if (!op) return false; // diagnostic already emitted.

      if (isTensorHandle(op->getType().getSwiftRValueType())) {
        diagInvalid("'s' operand requires a scalar value");
        return false;
      }

      assert(getScalarOperand(op) && "Invalid scalar operand");
      break;
    }

    case OpDescriptor::Constant: {
      // If this requires a constant value and doesn't have one (i.e., it's a
      // variable), then we reject it.
      auto op = getNextOperand();
      if (!op) return false; // diagnostic already emitted.

      // If it isn't a literal, don't treat it like a tensor op.
      if (!getTensorConstantOperand(op)) {
        diagInvalid("tensor operation requires an immediate constant argument");
        return false;
      }
      break;
    }
    }
  }

  // Attribute values require constant values.  If we don't have one then this
  // op is invalid and must be rejected.
  for (auto attrName : attributeNames) {
    auto op = getNextOperand();
    if (!op) return false; // diagnostic already emitted.

    if (!getAttrOperand(op)) {
      diagInvalid("attribute '" + attrName.str() +
                  "' requires a constant argument");
      return false;
    }
  }

  // Diagnose when the type descriptor didn't specify enough args.
  if (nextOperand != inst->getNumOperands()) {
    diagInvalid("more arguments present than type descriptors specified");
    return false;
  }

  return true;
}

/// Handle calls to __tf_init_scalar, which take a pointer to a stack slot
/// and return a TensorHandle<T>.
bool SILTensorOpInfo::decodeTFInitScalar(ApplyInst *inst) {
  assert(inst->getNumOperands() == 2 &&
         isTensorHandle(inst->getType().getSwiftRValueType()) &&
         "Unexpected type signature for __tf_init_scalar");

  // If we can't analyze the operand as a constant value, then give up.
  if (getTensorConstantOperand(1) == nullptr)
    return false;

  // Otherwise, good news everyone!  We can treat this as a Const op.
  opName = "Const";
  operandDescriptorStr = "cd";
  resultDescriptorStr = "t";
  builtinName = "__tfop_Const,cd:t";
  operandDescriptors = { OpDescriptor::Constant, OpDescriptor::AddDType };
  return true;
}




/// The SIL location for operations we process are usually deep in the bowels
/// of the tensor library code, which are all implementation details to the
/// user.  As such, walk the inlining location of the specified node to return
/// the first location *outside* of the tensor implementation goop.
SILDebugLocation tf::skipInternalLocations(SILDebugLocation loc) {
  auto ds = loc.getScope();

  if (!ds) return loc;

  // If this location hasn't been inlined at all, just keep it unmodified.
  if (!ds->InlinedCallSite && loc.getLocation().getSourceLoc().isValid())
    return loc;

  // Zip through inlined call site information that came from the
  // implementation guts of the tensor library.  We want to report the
  // message inside the user's code, not in the guts we inlined through.
  for (; auto ics = ds->InlinedCallSite; ds = ics) {
    // If we found a valid inlined-into location, then we are good.
    if (ds->Loc.getSourceLoc().isValid())
      return SILDebugLocation(ds->Loc, ds);
    if (SILFunction *F = ds->getInlinedFunction()) {
      if (F->getLocation().getSourceLoc().isValid())
        break;
    }
  }

  if (!ds->Loc.isNull())
    return SILDebugLocation(ds->Loc, ds);

  return loc;
}
