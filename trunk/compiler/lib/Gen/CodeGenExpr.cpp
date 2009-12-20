/* ================================================================ *
   TART - A Sweet Programming Language.
 * ================================================================ */

#include "tart/CFG/Expr.h"
#include "tart/CFG/Defn.h"
#include "tart/CFG/TypeDefn.h"
#include "tart/CFG/Constant.h"
#include "tart/CFG/CompositeType.h"
#include "tart/CFG/PrimitiveType.h"
#include "tart/CFG/FunctionType.h"
#include "tart/CFG/FunctionDefn.h"
#include "tart/CFG/NativeType.h"
#include "tart/CFG/EnumType.h"
#include "tart/CFG/Template.h"
#include "tart/CFG/UnionType.h"
#include "tart/CFG/TupleType.h"
#include "tart/CFG/Module.h"
#include "tart/CFG/Closure.h"
#include "tart/Gen/CodeGenerator.h"
#include "tart/Common/Diagnostics.h"
#include "tart/Objects/Builtins.h"
#include "tart/Objects/Intrinsic.h"

#include "llvm/Support/raw_ostream.h"
#include "llvm/Module.h"

namespace tart {

FormatStream & operator<<(FormatStream & out, const llvm::Type * type) {
  out << type->getDescription();
  return out;
}

FormatStream & operator<<(FormatStream & out, const llvm::Value * value) {
  // Use a temporary string stream to get the printed form of the value.
  std::string s;
  llvm::raw_string_ostream ss(s);
  value->print(ss);
  out << ss.str();
  return out;
}

FormatStream & operator<<(FormatStream & out, const ValueList & values) {
  for (ValueList::const_iterator it = values.begin(); it != values.end(); ++it) {
    if (it != values.begin()) {
      out << ", ";
    }

    out << *it;
  }

  return out;
}

using namespace llvm;

namespace {
/** Return the type that would be generated from a GEP instruction. */
const llvm::Type * getGEPType(const llvm::Type * type, ValueList::const_iterator first,
    ValueList::const_iterator last) {
  for (ValueList::const_iterator it = first; it != last; ++it) {
    if (const ArrayType * atype = dyn_cast<ArrayType>(type)) {
      type = type->getContainedType(0);
    } else {
      const ConstantInt * index = cast<ConstantInt> (*it);
      type = type->getContainedType(index->getSExtValue());
    }
  }

  return type;
}

#ifdef NDEBUG
#define DASSERT_TYPE_EQ(expected, actual)
#define DASSERT_TYPE_EQ_MSG(expected, actual, msg)
#else
#define DASSERT_TYPE_EQ(expected, actual) \
      if (expected != actual) {\
        diag.fatal() << "Expected '" << expected << "' == '" << actual << "'"; \
      }

#define DASSERT_TYPE_EQ_MSG(expected, actual, msg) \
      if (expected != actual) {\
        diag.fatal() << "Expected '" << expected << "' == '" << actual << \
            "' " << msg; \
      }

#endif
}

Value * CodeGenerator::genExpr(const Expr * in) {
  switch (in->exprType()) {
    case Expr::ConstInt:
      return static_cast<const ConstantInteger *> (in)->value();

    case Expr::ConstFloat: {
      const ConstantFloat * cfloat = static_cast<const ConstantFloat *> (in);
      return cfloat->value();
    }

    case Expr::ConstString:
      return genStringLiteral(static_cast<const ConstantString *> (in)->value());

    case Expr::ConstNull: {
      //DASSERT_OBJ(in->type()->isReferenceType(), in->type());
      return ConstantPointerNull::get(cast<llvm::PointerType>(in->type()->irParameterType()));
    }

    case Expr::ConstObjRef:
      return genConstantObjectPtr(static_cast<const ConstantObjectRef *>(in), "");

    case Expr::LValue: {
      return genLoadLValue(static_cast<const LValueExpr *>(in));
    }

    case Expr::BoundMethod: {
      return genBoundMethod(static_cast<const BoundMethodExpr *>(in));
    }

    case Expr::ElementRef: {
      Value * addr = genElementAddr(static_cast<const UnaryExpr *>(in));
      return addr != NULL ? builder_.CreateLoad(addr) : NULL;
    }

    case Expr::InitVar:
      return genInitVar(static_cast<const InitVarExpr *>(in));

    case Expr::BinaryOpcode:
      return genBinaryOpcode(static_cast<const BinaryOpcodeExpr *>(in));

    case Expr::Truncate:
    case Expr::SignExtend:
    case Expr::ZeroExtend:
    case Expr::IntToFloat:
      return genNumericCast(static_cast<const CastExpr *>(in));

    case Expr::UpCast:
      return genUpCast(static_cast<const CastExpr *>(in));

    case Expr::BitCast:
      return genBitCast(static_cast<const CastExpr *>(in));

    case Expr::UnionCtorCast:
      return genUnionCtorCast(static_cast<const CastExpr *>(in));

    case Expr::UnionMemberCast:
    case Expr::CheckedUnionMemberCast:
      return genUnionMemberCast(static_cast<const CastExpr *>(in));

    case Expr::TupleCtor:
      return genTupleCtor(static_cast<const TupleCtorExpr *>(in));

    case Expr::Assign:
    case Expr::PostAssign:
      return genAssignment(static_cast<const AssignmentExpr *>(in));

    case Expr::Compare:
      return genCompare(static_cast<const CompareExpr *>(in));

    case Expr::InstanceOf:
      return genInstanceOf(static_cast<const InstanceOfExpr *>(in));

    case Expr::RefEq:
      return genRefEq(static_cast<const BinaryExpr *>(in), false);

    case Expr::PtrDeref:
      return genPtrDeref(static_cast<const UnaryExpr *>(in));

    case Expr::Not:
      return genNot(static_cast<const UnaryExpr *>(in));

    case Expr::And:
    case Expr::Or:
      return genLogicalOper(static_cast<const BinaryExpr *>(in));

    case Expr::FnCall:
    case Expr::CtorCall:
    case Expr::VTableCall:
      return genCall(static_cast<const FnCallExpr *>(in));

    case Expr::IndirectCall:
      return genIndirectCall(static_cast<const IndirectCallExpr *>(in));

    case Expr::New:
      return genNew(static_cast<const NewExpr *>(in));

    case Expr::Prog2: {
      const BinaryExpr * binOp = static_cast<const BinaryExpr *>(in);
      genExpr(binOp->first());
      return genExpr(binOp->second());
    }

    case Expr::IRValue: {
      const IRValueExpr * irExpr = static_cast<const IRValueExpr *>(in);
      DASSERT_OBJ(irExpr->value() != NULL, irExpr);
      return irExpr->value();
    }

    case Expr::ArrayLiteral:
      return genArrayLiteral(static_cast<const ArrayLiteralExpr *>(in));

    case Expr::ClosureEnv:
      return genClosureEnv(static_cast<const ClosureEnvExpr *>(in));

    case Expr::NoOp:
      return NULL;

    default:
      diag.debug(in) << "No generator for " <<
      exprTypeName(in->exprType()) << " [" << in << "]";
      DFAIL("Implement");
  }
}

llvm::Constant * CodeGenerator::genConstExpr(const Expr * in) {
  switch (in->exprType()) {
    case Expr::ConstInt:
      return static_cast<const ConstantInteger *>(in)->value();

    case Expr::ConstObjRef:
      return genConstantObject(static_cast<const ConstantObjectRef *>(in));

    case Expr::ConstNArray:
      return genConstantArray(static_cast<const ConstantNativeArray *>(in));

    default:
      diag.fatal(in) << "Not a constant: " <<
      exprTypeName(in->exprType()) << " [" << in << "]";
      DFAIL("Implement");
  }
}

llvm::GlobalVariable * CodeGenerator::genConstRef(const Expr * in, StringRef name) {
  switch (in->exprType()) {
    case Expr::ConstObjRef:
      return genConstantObjectPtr(static_cast<const ConstantObjectRef *>(in), name);

    //case Expr::ConstNArray:
      //return genConstantArrayPtr(static_cast<const ConstantNativeArray *>(in));

    default:
      diag.fatal(in) << "Not a constant reference: " <<
      exprTypeName(in->exprType()) << " [" << in << "]";
      return NULL;
  }
}

Value * CodeGenerator::genInitVar(const InitVarExpr * in) {
  Value * initValue = genExpr(in->initExpr());
  if (initValue == NULL) {
    return NULL;
  }

  if (requiresImplicitDereference(in->initExpr()->type())) {
    initValue = builder_.CreateLoad(initValue);
  }

  VariableDefn * var = in->getVar();
  if (var->defnType() == Defn::Let) {
    DASSERT_OBJ(var->initValue() == NULL, var);
    DASSERT_OBJ(initValue != NULL, var);
    var->setIRValue(initValue);
  } else {
    builder_.CreateStore(initValue, var->irValue());
  }

  return initValue;
}

Value * CodeGenerator::genAssignment(const AssignmentExpr * in) {
  Value * rvalue = genExpr(in->fromExpr());
  Value * lvalue = genLValueAddress(in->toExpr());

  if (rvalue != NULL && lvalue != NULL) {
    if (in->exprType() == Expr::PostAssign) {
      Value * result = builder_.CreateLoad(lvalue);
      builder_.CreateStore(rvalue, lvalue);
      return result;
    } else {
      return builder_.CreateStore(rvalue, lvalue);
    }
  }

  return NULL;
}

Value * CodeGenerator::genBinaryOpcode(const BinaryOpcodeExpr * in) {
  Value * lOperand = genExpr(in->first());
  Value * rOperand = genExpr(in->second());
  return builder_.CreateBinOp(in->opCode(), lOperand, rOperand);
}

llvm::Value * CodeGenerator::genCompare(const tart::CompareExpr* in) {
  Value * first = genExpr(in->first());
  Value * second = genExpr(in->second());
  CmpInst::Predicate pred = in->getPredicate();
  if (pred >= CmpInst::FIRST_ICMP_PREDICATE &&
      pred <= CmpInst::LAST_ICMP_PREDICATE) {
    return builder_.CreateICmp(pred, first, second);
  } else if (pred <= CmpInst::LAST_FCMP_PREDICATE) {
    return builder_.CreateFCmp(pred, first, second);
  } else {
    DFAIL("Invalid predicate");
  }
}

Value * CodeGenerator::genInstanceOf(const tart::InstanceOfExpr* in) {
  DASSERT_OBJ(in->value()->type() != NULL, in);
  Value * val = genExpr(in->value());
  if (val == NULL) {
    return NULL;
  }

  if (const UnionType * utype = dyn_cast<UnionType>(in->value()->type())) {
    return genUnionTypeTest(val, utype, in->toType(), false);
  }

  const CompositeType * fromType = cast<CompositeType>(in->value()->type());
  const CompositeType * toType = cast<CompositeType>(in->toType());
  return genCompositeTypeTest(val, fromType, toType);
}

Value * CodeGenerator::genRefEq(const BinaryExpr * in, bool invert) {
  DASSERT_OBJ(in->first()->type()->isEqual(in->second()->type()), in);
  Value * first = genExpr(in->first());
  Value * second = genExpr(in->second());
  if (first != NULL && second != NULL) {
    if (invert) {
      return builder_.CreateICmpNE(first, second);
    } else {
      return builder_.CreateICmpEQ(first, second);
    }
  }

  return NULL;
}

Value * CodeGenerator::genPtrDeref(const UnaryExpr * in) {
  Value * ptrVal = genExpr(in->arg());
  if (ptrVal != NULL) {
    DASSERT(ptrVal->getType()->getTypeID() == llvm::Type::PointerTyID);
    DASSERT_TYPE_EQ_MSG(
        in->type()->irType(),
        ptrVal->getType()->getContainedType(0), "for expression " << in);
    return builder_.CreateLoad(ptrVal);
  }

  return NULL;
}

Value * CodeGenerator::genNot(const UnaryExpr * in) {
  switch (in->arg()->exprType()) {
    case Expr::RefEq:
    return genRefEq(static_cast<const BinaryExpr *>(in->arg()), true);

    default: {
      Value * result = genExpr(in->arg());
      return result ? builder_.CreateNot(result) : NULL;
    }
  }
}

Value * CodeGenerator::genLogicalOper(const BinaryExpr * in) {
  BasicBlock * blkTrue = BasicBlock::Create(context_, "true_branch", currentFn_);
  BasicBlock * blkFalse = BasicBlock::Create(context_, "false_branch", currentFn_);
  BasicBlock * blkNext = BasicBlock::Create(context_, "combine", currentFn_);

  blkTrue->moveAfter(builder_.GetInsertBlock());
  blkFalse->moveAfter(blkTrue);
  blkNext->moveAfter(blkFalse);

  if (!genTestExpr(in, blkTrue, blkFalse)) {
    return NULL;
  }

  builder_.SetInsertPoint(blkTrue);
  builder_.CreateBr(blkNext);

  builder_.SetInsertPoint(blkFalse);
  builder_.CreateBr(blkNext);

  builder_.SetInsertPoint(blkNext);
  PHINode * phi = builder_.CreatePHI(builder_.getInt1Ty());
  phi->addIncoming(ConstantInt::getTrue(context_), blkTrue);
  phi->addIncoming(ConstantInt::getFalse(context_), blkFalse);
  return phi;
}

Value * CodeGenerator::genLoadLValue(const LValueExpr * lval) {
  const ValueDefn * var = lval->value();

  // It's a member or element expression
  if (lval->base() != NULL) {
    Value * addr = genMemberFieldAddr(lval);
    return addr != NULL ? builder_.CreateLoad(addr, var->name()) : NULL;
  }

  // It's a global, static, or parameter
  if (var->defnType() == Defn::Let) {
    const VariableDefn * let = static_cast<const VariableDefn *>(var);
    Value * letValue = genLetValue(let);
    if (lval->type()->typeClass() == Type::Tuple) {
      return letValue;
    }

    if (let->hasStorage()) {
      letValue = builder_.CreateLoad(letValue, var->name());
    }

    return letValue;
  } else if (var->defnType() == Defn::Var) {
    Value * varValue = genVarValue(static_cast<const VariableDefn *>(var));
    if (var->type()->typeClass() == Type::Tuple) {
      return varValue;
    }

    return builder_.CreateLoad(varValue, var->name());
  } else if (var->defnType() == Defn::Parameter) {
    const ParameterDefn * param = static_cast<const ParameterDefn *>(var);
    if (param->irValue() == NULL) {
      diag.fatal(param) << "Invalid parameter IR value for parameter '" << param << "'";
    }
    DASSERT_OBJ(param->irValue() != NULL, param);

    if (param->type()->typeClass() == Type::Tuple) {
      return param->irValue();
    }

    if (param->isLValue()) {
      return builder_.CreateLoad(param->irValue(), param->name());
    }

    return param->irValue();
  } else {
    DFAIL("IllegalState");
  }
}

Value * CodeGenerator::genLValueAddress(const Expr * in) {
  switch (in->exprType()) {
    case Expr::LValue: {
      const LValueExpr * lval = static_cast<const LValueExpr *>(in);

      // It's a reference to a class member.
      if (lval->base() != NULL) {
        return genMemberFieldAddr(lval);
      }

      // It's a global, static, or parameter
      const ValueDefn * var = lval->value();
      if (var->defnType() == Defn::Var) {
        return genVarValue(static_cast<const VariableDefn *>(var));
      } else if (var->defnType() == Defn::Parameter) {
        const ParameterDefn * param = static_cast<const ParameterDefn *>(var);
        if (param->type()->typeClass() == Type::Struct) {
          return param->irValue();
        }

        DASSERT_OBJ(param->isLValue(), param);
        return param->irValue();
      } else {
        diag.fatal(lval) << Format_Type << "Can't take address of non-lvalue " << lval;
        DFAIL("IllegalState");
      }
    }

    case Expr::ElementRef: {
      return genElementAddr(static_cast<const UnaryExpr *>(in));
      break;
    }

    default:
      diag.fatal(in) << "Not an LValue " << exprTypeName(in->exprType()) << " [" << in << "]";
      DFAIL("Implement");
  }
}

Value * CodeGenerator::genMemberFieldAddr(const LValueExpr * lval) {
  const Defn * de = lval->value();
  DASSERT(lval->base() != NULL);
  ValueList indices;
  std::stringstream labelStream;
  FormatStream fs(labelStream);
  Value * baseVal = genGEPIndices(lval, indices, fs);
  if (baseVal == NULL) {
    return NULL;
  }

  return builder_.CreateInBoundsGEP(
      baseVal, indices.begin(), indices.end(), labelStream.str().c_str());
}

Value * CodeGenerator::genElementAddr(const UnaryExpr * in) {
  ValueList indices;
  std::stringstream labelStream;
  FormatStream fs(labelStream);
  Value * baseVal = genGEPIndices(in, indices, fs);
  if (baseVal == NULL) {
    return NULL;
  }

  if (in->type()->typeClass() == Type::Tuple) {
    DASSERT(isa<llvm::PointerType>(baseVal->getType()));
  }

  return builder_.CreateInBoundsGEP(baseVal, indices.begin(), indices.end(),
      labelStream.str().c_str());
}

Value * CodeGenerator::genGEPIndices(const Expr * expr, ValueList & indices, FormatStream & label) {

  switch (expr->exprType()) {
    case Expr::LValue: {
      // In this case, lvalue refers to a member of the base expression.
      const LValueExpr * lval = static_cast<const LValueExpr *>(expr);
      Value * baseAddr = genBaseExpr(lval->base(), indices, label);
      const VariableDefn * field = cast<VariableDefn>(lval->value());

      DASSERT(field->memberIndex() >= 0);
      indices.push_back(getInt32Val(field->memberIndex()));
      label << "." << field->name();

      // Assert that the type is what we expected: A pointer to the field type.
      if (expr->type()->isReferenceType()) {
        DASSERT_TYPE_EQ(
            llvm::PointerType::get(expr->type()->irType(), 0),
            getGEPType(baseAddr->getType(), indices.begin(), indices.end()));
      } else {
        DASSERT_TYPE_EQ(
            expr->type()->irType(),
            getGEPType(baseAddr->getType(), indices.begin(), indices.end()));
      }

      return baseAddr;
    }

    case Expr::ElementRef: {
      const BinaryExpr * indexOp = static_cast<const BinaryExpr *>(expr);
      const Expr * arrayExpr = indexOp->first();
      const Expr * indexExpr = indexOp->second();
      Value * arrayVal;

      if (arrayExpr->type()->typeClass() == Type::NAddress) {
        // Handle auto-deref of Address type.
        arrayVal = genExpr(arrayExpr);
        label << arrayExpr;
      } else {
        arrayVal = genBaseExpr(arrayExpr, indices, label);
      }

      // TODO: Make sure the dimensions are in the correct order here.
      // I think they might be backwards.
      label << "[" << indexExpr << "]";
      Value * indexVal = genExpr(indexExpr);
      if (indexVal == NULL) {
        return NULL;
      }

      indices.push_back(indexVal);

      // Assert that the type is what we expected: A pointer to the field or element type.
      if (expr->type()->isReferenceType()) {
        DASSERT_TYPE_EQ(
            llvm::PointerType::get(expr->type()->irType(), 0),
            getGEPType(arrayVal->getType(), indices.begin(), indices.end()));
      } else {
        //DASSERT_TYPE_EQ(
        //    expr->type()->irType(),
        //    getGEPType(arrayVal->getType(), indices.begin(), indices.end()));
      }

      return arrayVal;
    }

    default:
      DFAIL("Bad GEP call");
      break;
  }

  return NULL;
}

Value * CodeGenerator::genBaseExpr(const Expr * in, ValueList & indices,
    FormatStream & labelStream) {

  // If the base is a pointer
  bool needsDeref = false;

  // True if the base address itself has a base.
  bool hasBase = false;

  /*  Determine if the expression is actually a pointer that needs to be
   dereferenced. This happens under the following circumstances:

   1) The expression is an explicit pointer dereference.
   2) The expression is a variable or parameter containing a reference type.
   3) The expression is a parameter to a value type, but has the reference
      flag set (which should only be true for the 'self' parameter.)
   */

  const Expr * base = in;
  if (const LValueExpr * lval = dyn_cast<LValueExpr>(base)) {
    const ValueDefn * field = lval->value();
    const Type * fieldType = dealias(field->type());
    if (const ParameterDefn * param = dyn_cast<ParameterDefn>(field)) {
      fieldType = dealias(param->internalType());
      if (param->getFlag(ParameterDefn::Reference)) {
        needsDeref = true;
      }
    }

    if (fieldType->isReferenceType()) {
      needsDeref = true;
    } else if (fieldType->typeClass() == Type::Tuple) {
      needsDeref = true;
    }

    if (lval->base() != NULL) {
      hasBase = true;
    }
  } else if (base->exprType() == Expr::PtrDeref) {
    base = static_cast<const UnaryExpr *>(base)->arg();
    needsDeref = true;
  } else if (base->exprType() == Expr::ElementRef) {
    hasBase = true;
  } else if (base->type()->isReferenceType()) {
    needsDeref = true;
  }

  Value * baseAddr;
  if (hasBase && !needsDeref) {
    // If it's a field within a larger object, then we can simply take a
    // relative address from the base.
    baseAddr = genGEPIndices(base, indices, labelStream);
  } else {
    // Otherwise generate a pointer value.
    labelStream << base;
    baseAddr = genExpr(base);
    if (needsDeref) {
      // baseAddr is of pointer type, we need to add an extra 0 to convert it
      // to the type of thing being pointed to.
      indices.push_back(getInt32Val(0));
    }
  }

  // Assert that the type is what we expected.
  DASSERT_OBJ(in->type() != NULL, in);
  if (!indices.empty()) {
    DASSERT_TYPE_EQ(
        in->type()->irType(),
        getGEPType(baseAddr->getType(), indices.begin(), indices.end()));
  }

  return baseAddr;
}

Value * CodeGenerator::genCast(Value * in, const Type * fromType, const Type * toType) {
  // If types are the same, no need for a cast.
  if (fromType->isEqual(toType)) {
    return in;
  }

  const FunctionDefn * converter = NULL;
  TypePair conversionKey(fromType, toType);
  ConverterMap::iterator it = module_->converters().find(conversionKey);
  if (it != module_->converters().end()) {
    converter = it->second;
  } else {
    // TODO: This is kind of a hack - we don't know for sure if the converters
    // in the synthetic module are the correct ones to use, but we have no way
    // to know what the correct module is unless we add it to the type.
    it = Builtins::syntheticModule.converters().find(conversionKey);
    if (it != Builtins::syntheticModule.converters().end()) {
      converter = it->second;
    }
  }

  if (converter != NULL) {
    ValueList args;
    Value * fnVal = genFunctionValue(converter);
    args.push_back(in);
    return genCallInstr(fnVal, args.begin(), args.end(), "convert");
  }

  if (const CompositeType * cfrom = dyn_cast<CompositeType>(fromType)) {
    if (const CompositeType * cto = dyn_cast<CompositeType>(toType)) {
      if (cto->isReferenceType() && cfrom->isReferenceType()) {
        if (cfrom->isSubclassOf(cto)) {
          // Upcast, no need for type test.
          return genUpCastInstr(in, cfrom, cto);
        } else if (cto->isSubclassOf(cfrom)) {
        }

        // Composite to composite.
        Value * typeTest = genCompositeTypeTest(in, cfrom, cto);
        throwCondTypecastError(typeTest);
        return builder_.CreatePointerCast(in, cto->irEmbeddedType(), "typecast");
      }
    } else if (const PrimitiveType * pto = dyn_cast<PrimitiveType>(toType)) {
      diag.debug() << "Need unbox cast from " << fromType << " to " << toType;
      DFAIL("Implement");
    } else if (const EnumType * eto = dyn_cast<EnumType>(toType)) {
      return genCast(in, fromType, eto->baseType());
    }
  } else if (const PrimitiveType * pfrom = dyn_cast<PrimitiveType>(fromType)) {
    if (const PrimitiveType * pto = dyn_cast<PrimitiveType>(toType)) {
    } else if (toType == Builtins::typeObject) {
      const TemplateSignature * tsig = Builtins::objectCoerceFn()->templateSignature();
      const FunctionDefn * coerceFn = dyn_cast_or_null<FunctionDefn>(
          tsig->findSpecialization(TupleType::get(fromType)));
      if (coerceFn == NULL) {
        diag.error() << "Missing function Object.coerce[" << fromType << "]";
        DFAIL("Missing Object.coerce fn");
      }

      ValueList args;
      Value * fnVal = genFunctionValue(coerceFn);
      args.push_back(in);
      return genCallInstr(fnVal, args.begin(), args.end(), "coerce");
    } else if (const CompositeType * cto = dyn_cast<CompositeType>(toType)) {
      // TODO: This would be *much* easier to handle in the analysis phase.
      // But that means doing the invoke function in the analysis phase as well.
      //return tart.core.ValueRef[type].create(in).
    }
  } else if (const EnumType * efrom = dyn_cast<EnumType>(fromType)) {
    return genCast(in, efrom->baseType(), toType);
  }

  diag.debug() << "Unsupported cast from " << fromType << " to " << toType;
  DFAIL("Implement");
}

Value * CodeGenerator::genNumericCast(const CastExpr * in) {
  Value * value = genExpr(in->arg());
  TypeId fromTypeId = TypeId_Void;
  if (const PrimitiveType * ptype = dyn_cast<PrimitiveType>(in->arg()->type())) {
    fromTypeId = ptype->typeId();
  }

  if (value != NULL) {
    llvm::Instruction::CastOps castType;
    switch (in->exprType()) {
      case Expr::Truncate:
        if (isFloatingTypeId(fromTypeId)) {
          castType = llvm::Instruction::FPTrunc;
        } else {
          castType = llvm::Instruction::Trunc;
        }
        break;

      case Expr::SignExtend:
        if (isFloatingTypeId(fromTypeId)) {
          castType = llvm::Instruction::FPExt;
        } else {
          castType = llvm::Instruction::SExt;
        }
        break;

      case Expr::ZeroExtend:
        castType = llvm::Instruction::ZExt;
        break;

      case Expr::IntToFloat:
        if (isUnsignedIntegerTypeId(fromTypeId)) {
          castType = llvm::Instruction::UIToFP;
        } else {
          castType = llvm::Instruction::SIToFP;
        }
        break;

      default:
        DFAIL("IllegalState");
    }

    return builder_.CreateCast(castType, value, in->type()->irType());
  }

  return NULL;
}

Value * CodeGenerator::genUpCast(const CastExpr * in) {
  Value * value = genExpr(in->arg());
  const Type * fromType = in->arg()->type();
  const Type * toType = in->type();

  if (value != NULL && fromType != NULL && toType != NULL) {
    return genUpCastInstr(value, fromType, toType);
  }

  return NULL;
}

Value * CodeGenerator::genBitCast(const CastExpr * in) {
  Value * value = genExpr(in->arg());
  const Type * toType = in->type();

  if (value != NULL && toType != NULL) {
    //if (toType->typeClass() == Type::Function)
    return builder_.CreateBitCast(value, toType->irEmbeddedType(), "bitcast");
  }

  DFAIL("Bad bitcast");
  return NULL;
}

Value * CodeGenerator::genUnionCtorCast(const CastExpr * in) {
  const Type * fromType = in->arg()->type();
  const Type * toType = in->type();
  Value * value = NULL;

  if (!fromType->isVoidType()) {
    value = genExpr(in->arg());
    if (value == NULL) {
      return NULL;
    }
  }

  if (toType != NULL) {
    const UnionType * utype = cast<UnionType>(toType);
    if (utype->numValueTypes() > 0 || utype->hasVoidType()) {
      int index = utype->getTypeIndex(fromType);
      if (index < 0) {
        diag.error() << "Can't convert " << fromType << " to " << utype;
      }
      DASSERT(index >= 0);
      Value * indexVal = ConstantInt::get(utype->irType()->getContainedType(0), index);

      Value * uvalue = builder_.CreateAlloca(utype->irType());
      builder_.CreateStore(indexVal, builder_.CreateConstInBoundsGEP2_32(uvalue, 0, 0));
      if (value != NULL) {
        const llvm::Type * fieldType = fromType->irEmbeddedType();
        builder_.CreateStore(value,
            builder_.CreateBitCast(
                builder_.CreateConstInBoundsGEP2_32(uvalue, 0, 1),
                llvm::PointerType::get(fieldType, 0)));
      }

      return builder_.CreateLoad(uvalue);

#if 0
      // TODO: An alternate method of constructing the value that doesn't involve an alloca.
      // This won't work until union types are supported in LLVM.
      Value * uvalue = UndefValue::get(utype->irType());
      uvalue = builder_.CreateInsertValue(uvalue, indexVal, 0);
      uvalue = builder_.CreateInsertValue(uvalue, value, 1);
      return uvalue;
#endif
    } else {
      // The type returned from irType() is a pointer type.
      //Value * uvalue = builder_.CreateBitCast(utype->irType());
      return builder_.CreateBitCast(value, utype->irType());
    }
  }

  return NULL;
}

Value * CodeGenerator::genUnionMemberCast(const CastExpr * in) {
  // Retrieve a value from a union. Presumes that the type-test has already been done.
  bool checked = in->exprType() == Expr::CheckedUnionMemberCast;
  const Type * fromType = in->arg()->type();
  const Type * toType = in->type();
  Value * value;
  // Our current process for handling unions requires that the union be an LValue,
  // so that we can bitcast the pointer to the data.
  if (in->exprType() == Expr::LValue || in->exprType() == Expr::ElementRef) {
    value = genLValueAddress(in->arg());
    if (value == NULL) {
      return NULL;
    }
  } else {
    // Create a temp var.
    value = genExpr(in->arg());
    if (value == NULL) {
      return NULL;
    }

    Value * var = builder_.CreateAlloca(value->getType());
    builder_.CreateStore(value, var);
    value = var;
  }

  if (fromType != NULL) {
    const UnionType * utype = cast<UnionType>(fromType);

    if (utype->numValueTypes() > 0 || utype->hasVoidType()) {
      if (checked) {
        Value * test = genUnionTypeTest(value, utype, toType, true);
        throwCondTypecastError(test);
      }

      const llvm::Type * fieldType = toType->irEmbeddedType();
      return builder_.CreateLoad(
          builder_.CreateBitCast(
              builder_.CreateConstInBoundsGEP2_32(value, 0, 1),
              llvm::PointerType::get(fieldType, 0)));
    } else {
      // The union contains only pointer types, so we know that its representation is simply
      // a single pointer, so a bit cast will work.
      Value * refTypeVal = builder_.CreateLoad(
          builder_.CreateBitCast(value, llvm::PointerType::get(toType->irEmbeddedType(), 0)));

      if (checked) {
        const CompositeType * cto = cast<CompositeType>(toType);
        Value * test = genCompositeTypeTest(refTypeVal, Builtins::typeObject.get(), cto);
        throwCondTypecastError(test);
      }

      return refTypeVal;
    }
  }

  return NULL;
}

Value * CodeGenerator::genTupleCtor(const TupleCtorExpr * in) {
  const TupleType * tt = cast<TupleType>(dealias(in->type()));
  Value * tupleValue = builder_.CreateAlloca(tt->irType(), 0, "tuple");
  size_t index = 0;
  for (ExprList::const_iterator it = in->args().begin(); it != in->args().end(); ++it, ++index) {
    Value * fieldPtr = builder_.CreateConstInBoundsGEP2_32(tupleValue, 0, index);
    Value * fieldValue = genExpr(*it);
    builder_.CreateStore(fieldValue, fieldPtr, false);
  }

  return tupleValue;
  //return builder_.CreateLoad(tupleValue);
}

Value * CodeGenerator::genCall(const tart::FnCallExpr* in) {
  const FunctionDefn * fn = in->function();

  if (fn->isIntrinsic()) {
    return fn->intrinsic()->generate(*this, in);
  }

  ValueList args;

  Value * selfArg = NULL;
  if (in->selfArg() != NULL) {
    if (in->selfArg()->type()->typeClass() == Type::Struct) {
      if (in->exprType() == Expr::CtorCall) {
        selfArg = genExpr(in->selfArg());
      } else {
        selfArg = genLValueAddress(in->selfArg());
      }
    } else {
      selfArg = genExpr(in->selfArg());
    }

    DASSERT_OBJ(selfArg != NULL, in->selfArg());

    // Upcast the self argument type.
    if (fn->functionType()->selfParam() != NULL) {
      const Type * selfType = dealias(fn->functionType()->selfParam()->type());
      selfArg = genUpCastInstr(selfArg, in->selfArg()->type(), selfType);
    }

    if (fn->storageClass() == Storage_Instance) {
      args.push_back(selfArg);
    }
  }

  const ExprList & inArgs = in->args();
  for (ExprList::const_iterator it = inArgs.begin(); it != inArgs.end(); ++it) {
    Value * argVal = genExpr(*it);
    if (argVal == NULL) {
      return NULL;
    }

    args.push_back(argVal);
  }

  // Generate the function to call.
  Value * fnVal;
  if (in->exprType() == Expr::VTableCall) {
    DASSERT_OBJ(selfArg != NULL, in);
    const Type * classType = dealias(fn->functionType()->selfParam()->type());
    if (classType->typeClass() == Type::Class) {
      fnVal = genVTableLookup(fn, static_cast<const CompositeType *>(classType), selfArg);
    } else if (classType->typeClass() == Type::Interface) {
      fnVal = genITableLookup(fn, static_cast<const CompositeType *>(classType), selfArg);
    } else {
      // Struct or protocol.
      fnVal = genFunctionValue(fn);
    }
  } else {
    fnVal = genFunctionValue(fn);
  }

  Value * result = genCallInstr(fnVal, args.begin(), args.end(), fn->name());
  if (in->exprType() == Expr::CtorCall) {
    // Constructor call returns the 'self' argument.
    if (in->selfArg() != NULL && in->selfArg()->type()->typeClass() == Type::Struct) {
      return builder_.CreateLoad(selfArg);
    }

    return selfArg;
  } else {
    // Special handling for tuples.
    if (requiresImplicitDereference(fn->returnType())) {
      Value * aggResult = builder_.CreateAlloca(fn->returnType()->irType(), 0, "retval");
      builder_.CreateStore(result, aggResult);
      return aggResult;
    }

    return result;
  }
}

Value * CodeGenerator::genIndirectCall(const tart::IndirectCallExpr* in) {
  const Expr * fn = in->function();
  const Type * fnType = fn->type();

  Value * fnValue;
  ValueList args;

  if (const FunctionType * ft = dyn_cast<FunctionType>(fnType)) {
    fnValue = genExpr(fn);
    if (fnValue != NULL) {
      if (ft->isStatic()) {
        //fnValue = builder_.CreateLoad(fnValue);
      } else {
        //DFAIL("Implement");
      }
    }
  } else if (const BoundMethodType * bmType = dyn_cast<BoundMethodType>(fnType)) {
    Value * fnref = genExpr(fn);
    if (fnref == NULL) {
      return NULL;
    }

    fnValue = builder_.CreateExtractValue(fnref, 0, "method");
    Value * selfArg = builder_.CreateExtractValue(fnref, 1, "self");
    if (selfArg == NULL) {
      return NULL;
    }

    args.push_back(selfArg);
  } else {
    diag.info(in) << in->function() << " - " << in->function()->exprType();
    TFAIL << "Invalid function type: " << in->function() << " - " << in->function()->exprType();
  }

  const ExprList & inArgs = in->args();
  for (ExprList::const_iterator it = inArgs.begin(); it != inArgs.end(); ++it) {
    Value * argVal = genExpr(*it);
    if (argVal == NULL) {
      return NULL;
    }

    args.push_back(argVal);
  }

  return genCallInstr(fnValue, args.begin(), args.end(), "indirect");
}

Value * CodeGenerator::genVTableLookup(const FunctionDefn * method, const CompositeType * classType,
    Value * selfPtr) {
  DASSERT_OBJ(!method->isFinal(), method);
  DASSERT_OBJ(!method->isCtor(), method);
  int methodIndex = method->dispatchIndex();
  if (methodIndex < 0) {
    diag.fatal(method) << "Invalid member index of " << method;
    return NULL;
  }

  // Make sure it's a class.
  DASSERT(classType->typeClass() == Type::Class);
  DASSERT_TYPE_EQ(classType->irParameterType(), selfPtr->getType());

  // Upcast to type 'object' and load the vtable pointer.
  ValueList indices;
  for (const CompositeType * t = classType; t != NULL && t != Builtins::typeObject; t = t->super()) {
    indices.push_back(getInt32Val(0));
  }
  indices.push_back(getInt32Val(0));
  indices.push_back(getInt32Val(0));

  // Get the TIB
  Value * tib = builder_.CreateLoad(
      builder_.CreateInBoundsGEP(selfPtr, indices.begin(), indices.end()), "tib");
  DASSERT_TYPE_EQ(llvm::PointerType::get(Builtins::typeTypeInfoBlock.irType(), 0), tib->getType());

  indices.clear();
  indices.push_back(getInt32Val(0));
  indices.push_back(getInt32Val(TIB_METHOD_TABLE));
  indices.push_back(getInt32Val(methodIndex));
  Value * fptr = builder_.CreateLoad(
      builder_.CreateInBoundsGEP(tib, indices.begin(), indices.end()), method->name());
  return builder_.CreateBitCast(fptr, llvm::PointerType::getUnqual(method->type()->irType()));
}

Value * CodeGenerator::genITableLookup(const FunctionDefn * method, const CompositeType * classType,
    Value * objectPtr) {

  // Interface function table entry
  DASSERT(!method->isFinal());
  DASSERT(!method->isCtor());
  int methodIndex = method->dispatchIndex();
  if (methodIndex < 0) {
    diag.fatal(method) << "Invalid member index of " << method;
    return NULL;
  }

  // Make sure it's an interface.
  DASSERT(classType->typeClass() == Type::Interface);

  // Get the interface ID (which is just the type pointer).
  Constant * itype = getTypeInfoBlockPtr(classType);

  // Load the pointer to the TIB.
  Value * tib = builder_.CreateLoad(
      builder_.CreateConstInBoundsGEP2_32(objectPtr, 0, 0, "tib_ptr"), "tib");

  // Load the pointer to the dispatcher function.
  Value * dispatcher = builder_.CreateLoad(
      builder_.CreateConstInBoundsGEP2_32(tib, 0, TIB_IDISPATCH, "idispatch_ptr"), "idispatch");

  // Construct the call to the dispatcher
  ValueList args;
  args.push_back(itype);
  args.push_back(getInt32Val(methodIndex));
  Value * methodPtr = genCallInstr(dispatcher, args.begin(), args.end(), "method_ptr");
  return builder_.CreateBitCast(
      methodPtr, llvm::PointerType::getUnqual(method->type()->irType()), "method");
}

/** Get the address of a value. */
Value * CodeGenerator::genBoundMethod(const BoundMethodExpr * in) {
  const BoundMethodType * type = cast<BoundMethodType>(in->type());
  const FunctionDefn * fn = in->method();
  if (fn->isIntrinsic()) {
    diag.error(in) << "Intrinsic methods cannot be called indirectly.";
    return NULL;
  } else if (fn->isCtor()) {
    diag.error(in) << "Constructors cannot be called indirectly (yet).";
    return NULL;
  }

  Value * selfArg = NULL;
  if (in->selfArg() != NULL) {
    selfArg = genExpr(in->selfArg());

    // Upcast the self argument type.
    if (fn->functionType()->selfParam() != NULL) {
      const Type * selfType = dealias(fn->functionType()->selfParam()->type());
      selfArg = genUpCastInstr(selfArg, in->selfArg()->type(), selfType);
    }
  }

  // Generate the function to call.
  Value * fnVal;
  if (in->exprType() == Expr::VTableCall) {
    DASSERT_OBJ(selfArg != NULL, in);
    const Type * classType = dealias(fn->functionType()->selfParam()->type());
    if (classType->typeClass() == Type::Class) {
      fnVal = genVTableLookup(fn, static_cast<const CompositeType *>(classType), selfArg);
    } else if (classType->typeClass() == Type::Interface) {
      fnVal = genITableLookup(fn, static_cast<const CompositeType *>(classType), selfArg);
    } else {
      // Struct or protocol.
      fnVal = genFunctionValue(fn);
    }
  } else {
    fnVal = genFunctionValue(fn);
  }

  const llvm::Type * fnValType =
      StructType::get(context_, fnVal->getType(), selfArg->getType(), NULL);

  Value * result = builder_.CreateAlloca(fnValType);
  builder_.CreateStore(fnVal, builder_.CreateConstInBoundsGEP2_32(result, 0, 0, "method"));
  builder_.CreateStore(selfArg, builder_.CreateConstInBoundsGEP2_32(result, 0, 1, "self"));
  result = builder_.CreateLoad(
      builder_.CreateBitCast(result, llvm::PointerType::get(type->irType(), 0)));
  return result;
}

Value * CodeGenerator::genNew(const tart::NewExpr* in) {
  if (const CompositeType * ctdef = dyn_cast<CompositeType>(in->type())) {
    const llvm::Type * type = ctdef->irType();
    if (ctdef->typeClass() == Type::Struct) {
      return builder_.CreateAlloca(type, 0, ctdef->typeDefn()->name());
    } else if (ctdef->typeClass() == Type::Class) {
      Function * allocator = getTypeAllocator(ctdef);
      if (allocator != NULL) {
        return builder_.CreateCall(allocator, Twine(ctdef->typeDefn()->name(), StringRef("_new")));
      } else {
        diag.fatal(in) << "Cannot create an instance of type '" <<
        ctdef->typeDefn()->name() << "'";
      }
    }
  }

  DFAIL("IllegalState");
}

Value * CodeGenerator::genCallInstr(Value * func, ValueList::iterator firstArg,
    ValueList::iterator lastArg, const char * name) {
  if (unwindTarget_ != NULL) {
    Function * f = currentFn_;
    BasicBlock * normalDest = BasicBlock::Create(context_, "nounwind", f);
    normalDest->moveAfter(builder_.GetInsertBlock());
    Value * result = builder_.CreateInvoke(func, normalDest, unwindTarget_, firstArg, lastArg, name);
    builder_.SetInsertPoint(normalDest);
    return result;
  } else {
    return builder_.CreateCall(func, firstArg, lastArg, name);
  }
}

Value * CodeGenerator::genUpCastInstr(Value * val, const Type * from, const Type * to) {

  if (from == to) {
    return val;
  }

  DASSERT_OBJ(isa<CompositeType>(to), to);
  DASSERT_OBJ(isa<CompositeType>(from), from);

  const CompositeType * toType = dyn_cast<CompositeType>(to);
  const CompositeType * fromType = dyn_cast<CompositeType>(from);

  if (!fromType->isSubclassOf(toType)) {
    diag.fatal() << "'" << fromType << "' does not inherit from '" <<
    toType << "'";
    return val;
  }

  DASSERT(val->getType()->getTypeID() == llvm::Type::PointerTyID);

  // If it's an interface, then we'll need to simply bit-cast it.
  if (toType->typeClass() == Type::Interface) {
    return builder_.CreateBitCast(val, llvm::PointerType::get(toType->irType(), 0), "intf_ptr");
  }

  // List of GetElementPtr indices
  ValueList indices;

  // Once index to dereference the pointer.
  indices.push_back(getInt32Val(0));

  // One index for each supertype
  while (fromType != toType) {
    DASSERT_OBJ(fromType->super() != NULL, fromType);
    fromType = fromType->super();
    indices.push_back(getInt32Val(0));
  }

  return builder_.CreateInBoundsGEP(val, indices.begin(), indices.end(), "upcast");
}

llvm::Constant * CodeGenerator::genStringLiteral(const llvm::StringRef & strval,
    const llvm::StringRef & symName) {
  StringLiteralMap::iterator it = stringLiteralMap_.find(strval);
  if (it != stringLiteralMap_.end()) {
    return it->second;
  }

  const CompositeType * strType = Builtins::typeString.get();
  const llvm::Type * irType = strType->irType();

  Constant * strVal = ConstantArray::get(context_, strval, false);
  llvm::Type * charDataType = ArrayType::get(builder_.getInt8Ty(), 0);

  // Self-referential member values
  UndefValue * strDataStart = UndefValue::get(llvm::PointerType::getUnqual(charDataType));
  UndefValue * strSource = UndefValue::get(llvm::PointerType::getUnqual(irType));

  // Object type members
  std::vector<Constant *> objMembers;
  objMembers.push_back(getTypeInfoBlockPtr(strType));

  // String type members
  std::vector<Constant *> members;
  members.push_back(ConstantStruct::get(context_, objMembers, false));
  members.push_back(getInt32Val(strval.size()));
  members.push_back(strSource);
  members.push_back(strDataStart);
  members.push_back(ConstantArray::get(context_, strval, false));

  // If the name is blank, then the string is internal only.
  // If the name is non-blank, then it's assumed that this name is a globally unique
  // identifier of the string.
  Twine name;
  GlobalValue::LinkageTypes linkage = GlobalValue::LinkOnceODRLinkage;
  if (symName.empty()) {
    name = "string";
    linkage = GlobalValue::InternalLinkage;
  } else {
    name = "string." + symName;
  }

  Constant * strStruct = ConstantStruct::get(context_, members, false);
  Constant * strConstant = llvm::ConstantExpr::getPointerCast(
      new GlobalVariable(*irModule_,
          strStruct->getType(), true, linkage, strStruct, name),
      llvm::PointerType::getUnqual(irType));

  Constant * indices[2];
  indices[0] = getInt32Val(0);
  indices[1] = getInt32Val(4);

  strDataStart->replaceAllUsesWith(llvm::ConstantExpr::getGetElementPtr(strConstant, indices, 2));
  strSource->replaceAllUsesWith(strConstant);

  stringLiteralMap_[strval] = strConstant;
  return strConstant;
}

Value * CodeGenerator::genArrayLiteral(const ArrayLiteralExpr * in) {
  const CompositeType * arrayType = cast<CompositeType>(in->type());
  const Type * elementType = arrayType->typeDefn()->templateInstance()->typeArg(0);
  size_t arrayLength = in->args().size();

  //diag.debug() << "Generating array literal of type " << elementType << ", length " << arrayLength;

  const llvm::Type * etype = elementType->irEmbeddedType();

  // Arguments to the array-creation function
  ValueList args;
  args.push_back(getInt32Val(arrayLength));
  Function * allocFunc = findMethod(arrayType, "alloc");
  Value * result = genCallInstr(allocFunc, args.begin(), args.end(), "ArrayLiteral");

  // Evaluate the array elements.
  ValueList arrayVals;
  arrayVals.resize(arrayLength);
  for (size_t i = 0; i < arrayLength; ++i) {
    Value * el = genExpr(in->args()[i]);
    if (el == NULL) {
      return NULL;
    }

    arrayVals[i] = el;
  }

  // Store the array elements into their slots.
  if (arrayLength > 0) {
    Value * arrayData = builder_.CreateStructGEP(result, 2, "data");
    for (size_t i = 0; i < arrayLength; ++i) {
      Value * arraySlot = builder_.CreateStructGEP(arrayData, i);
      builder_.CreateStore(arrayVals[i], arraySlot);
    }
  }

  // TODO: Optimize array creation when most of the elements are constants.

  return result;
}

Value * CodeGenerator::genClosureEnv(const ClosureEnvExpr * in) {
  return llvm::ConstantPointerNull::get(llvm::PointerType::get(in->type()->irType(), 0));
}

Value * CodeGenerator::genCompositeTypeTest(Value * val, const CompositeType * fromType,
    const CompositeType * toType) {
  DASSERT(fromType != NULL);
  DASSERT(toType != NULL);

  // Make sure it's a class.
  DASSERT(toType->typeClass() == Type::Class || toType->typeClass() == Type::Interface);
  Constant * toTypeObj = getTypeInfoBlockPtr(toType);

  // Bitcast to object type
  Value * valueAsObjType = builder_.CreateBitCast(val,
      llvm::PointerType::getUnqual(Builtins::typeObject->irType()));

  // Upcast to type 'object' and load the TIB pointer.
  ValueList indices;
  indices.push_back(getInt32Val(0));
  indices.push_back(getInt32Val(0));
  Value * tib = builder_.CreateLoad(
      builder_.CreateInBoundsGEP(valueAsObjType, indices.begin(), indices.end()),
      "tib");

  ValueList args;
  args.push_back(tib);
  args.push_back(toTypeObj);
  Function * upcastTest = genFunctionValue(Builtins::funcHasBase);
  Value * result = builder_.CreateCall(upcastTest, args.begin(), args.end());
  return result;
}

Value * CodeGenerator::genUnionTypeTest(llvm::Value * in, const UnionType * unionType,
    const Type * toType, bool valIsLVal) {
  DASSERT(unionType != NULL);
  DASSERT(toType != NULL);

  if (unionType->numValueTypes() > 0 || unionType->hasVoidType()) {
    // The index of the actual type.
    Value * actualTypeIndex;
    if (valIsLVal) {
      // Load the type index field.
      actualTypeIndex = builder_.CreateLoad(builder_.CreateConstInBoundsGEP2_32(in, 0, 0));
    } else {
      // Extract the type index field.
      actualTypeIndex = builder_.CreateExtractValue(in, 0);
    }

    int testIndex = unionType->getTypeIndex(toType);
    if (testIndex < 0) {
      return ConstantInt::getFalse(context_);
    }

    Constant * testIndexValue = ConstantInt::get(actualTypeIndex->getType(), testIndex);
    Value * testResult = builder_.CreateICmpEQ(actualTypeIndex, testIndexValue, "isa");

#if 0
    // This section of code was based on a hybrid formula where all reference types
    // shared the same testIndex.
    if (testIndex == 0 && unionType->numRefTypes() > 1) {
      BasicBlock * blkIsRefType = BasicBlock::Create(context_, "is_ref_type", currentFn_);
      BasicBlock * blkEndTest = BasicBlock::Create(context_, "utest_end", currentFn_);

      // If it isn't a reference type, branch to the end (fail).
      BasicBlock * blkInitial = builder_.GetInsertBlock();
      builder_.CreateCondBr(testResult, blkIsRefType, blkEndTest);

      // If it is a reference type, then test if it's the right kind of reference type.
      builder_.SetInsertPoint(blkIsRefType);
      const CompositeType * cto = cast<CompositeType>(toType);
      if (valIsLVal) {
        in = builder_.CreateLoad(in);
      }

      Value * refTypeVal = builder_.CreateBitCast(in, toType->irEmbeddedType());
      Value * subclassTest = genCompositeTypeTest(refTypeVal, Builtins::typeObject.get(), cto);
      blkIsRefType = builder_.GetInsertBlock();
      builder_.CreateBr(blkEndTest);

      // Combine the two branches into one boolean test result.
      builder_.SetInsertPoint(blkEndTest);
      PHINode * phi = builder_.CreatePHI(builder_.getInt1Ty());
      phi->addIncoming(ConstantInt::getFalse(context_), blkInitial);
      phi->addIncoming(subclassTest, blkIsRefType);
      testResult = phi;
    }
#endif

    return testResult;
  } else {
    // It's only reference types.
    if (valIsLVal) {
      in = builder_.CreateLoad(in);
    }

    const CompositeType * cto = cast<CompositeType>(toType);
    Value * refTypeVal = builder_.CreateBitCast(in, toType->irEmbeddedType());
    return genCompositeTypeTest(refTypeVal, Builtins::typeObject.get(), cto);
  }
}

llvm::Constant * CodeGenerator::genSizeOf(Type * type, bool memberSize) {
  ValueList indices;
  indices.push_back(getInt32Val(1));

  const llvm::Type * irType = type->irType();
  if (memberSize && type->isReferenceType()) {
    irType = llvm::PointerType::get(irType, 0);
  }

  return llvm::ConstantExpr::getPtrToInt(
      llvm::ConstantExpr::getGetElementPtr(
          ConstantPointerNull::get(llvm::PointerType::get(irType, 0)),
          &indices[0], 1),
      builder_.getInt32Ty());
}

Value * CodeGenerator::genVarSizeAlloc(const SourceLocation & loc,
    const Type * objType, const Expr * sizeExpr) {

  if (!objType->isReferenceType()) {
    diag.fatal(loc) << "__valloc can only be used with reference types.";
    return NULL;
  }

  const llvm::Type * resultType = objType->irType();
  resultType = llvm::PointerType::get(resultType, 0);

  Value * sizeValue;
  switch (sizeExpr->exprType()) {
    case Expr::LValue:
    case Expr::ElementRef:
      sizeValue = genLValueAddress(sizeExpr);
      break;

    default:
      sizeValue = genExpr(sizeExpr);
      break;
  }

  if (isa<llvm::PointerType>(sizeValue->getType())) {
    if (Constant * c = dyn_cast<Constant>(sizeValue)) {
      sizeValue = llvm::ConstantExpr::getPtrToInt(c, builder_.getInt64Ty());
    } else {
      sizeValue = builder_.CreatePtrToInt(sizeValue, builder_.getInt64Ty());
    }
  }

  std::stringstream labelStream;
  FormatStream fs(labelStream);
  fs << objType;
  Value * alloc = builder_.CreateCall(getGlobalAlloc(), sizeValue, labelStream.str().c_str());
  Value * instance = builder_.CreateBitCast(alloc, resultType);

  if (const CompositeType * classType = dyn_cast<CompositeType>(objType)) {
    genInitObjVTable(classType, instance);
  }

  return instance;
}

GlobalVariable * CodeGenerator::genConstantObjectPtr(const ConstantObjectRef * obj,
    llvm::StringRef name) {
  Constant * constObject = genConstantObject(obj);
  if (name != "") {
    GlobalVariable * gv = irModule_->getGlobalVariable(name, true);
    if (gv != NULL) {
      return gv;
    }
  }

  return new GlobalVariable(
      *irModule_, constObject->getType(), true, GlobalValue::ExternalLinkage, constObject, name);
}

Constant * CodeGenerator::genConstantObject(const ConstantObjectRef * obj) {
  ConstantObjectMap::iterator it = constantObjectMap_.find(obj);
  if (it != constantObjectMap_.end()) {
    return it->second;
  }

  const CompositeType * type = cast<CompositeType>(obj->type());
  llvm::Constant * structVal = genConstantObjectStruct(obj, type);

  constantObjectMap_[obj] = structVal;
  return structVal;
}

Constant * CodeGenerator::genConstantObjectStruct(
    const ConstantObjectRef * obj, const CompositeType * type) {
  ConstantList fieldValues;
  if (type == Builtins::typeObject) {
    // Generate the TIB pointer.
    llvm::Constant * tibPtr = getTypeInfoBlockPtr(cast<CompositeType>(obj->type()));
    if (tibPtr == NULL) {
      return NULL;
    }

    fieldValues.push_back(tibPtr);
  } else {
    // Generate the superclass fields.
    if (type->super() != NULL) {
      llvm::Constant * superFields = genConstantObjectStruct(obj, type->super());
      if (superFields == NULL) {
        return NULL;
      }

      fieldValues.push_back(superFields);
    }

    // Now generate the values for each member.
    for (DefnList::const_iterator it = type->instanceFields().begin();
        it != type->instanceFields().end(); ++it) {
      if (VariableDefn * var = cast_or_null<VariableDefn>(*it)) {
        Expr * value = obj->getMemberValue(var);
        if (value == NULL) {
          diag.error(obj) << "Member value '" << var << "' has not been initialized.";
          return NULL;
        }

        Constant * irValue = genConstExpr(value);
        if (irValue == NULL) {
          return NULL;
        }

        fieldValues.push_back(irValue);
      }
    }
  }

  return ConstantStruct::get(context_, fieldValues, false);
}

llvm::Constant * CodeGenerator::genConstantArray(const ConstantNativeArray * array) {
  ConstantList elementValues;
  for (ExprList::const_iterator it = array->elements().begin(); it != array->elements().end(); ++it) {
    Constant * value = genConstExpr(*it);
    if (value == NULL) {
      return NULL;
    }

    elementValues.push_back(value);
  }

  return ConstantArray::get(cast<ArrayType>(array->type()->irType()), elementValues);
}

void CodeGenerator::throwCondTypecastError(Value * typeTestResult) {
  BasicBlock * blkCastFail = BasicBlock::Create(context_, "typecast_fail", currentFn_);
  BasicBlock * blkCastSucc = BasicBlock::Create(context_, "typecast_succ", currentFn_);
  builder_.CreateCondBr(typeTestResult, blkCastSucc, blkCastFail);
  builder_.SetInsertPoint(blkCastFail);
  throwTypecastError();
  builder_.SetInsertPoint(blkCastSucc);
}

void CodeGenerator::throwTypecastError() {
  Function * typecastFailure = genFunctionValue(Builtins::funcTypecastError);
  typecastFailure->setDoesNotReturn(true);
  if (unwindTarget_ != NULL) {
    Function * f = currentFn_;
    ValueList emptyArgs;
    BasicBlock * normalDest = BasicBlock::Create(context_, "nounwind", f);
    normalDest->moveAfter(builder_.GetInsertBlock());
    builder_.CreateInvoke(typecastFailure, normalDest, unwindTarget_,
        emptyArgs.begin(), emptyArgs.end(), "");
    builder_.SetInsertPoint(normalDest);
    builder_.CreateUnreachable();
  } else {
    builder_.CreateCall(typecastFailure);
    builder_.CreateUnreachable();
  }
}

} // namespace tart
