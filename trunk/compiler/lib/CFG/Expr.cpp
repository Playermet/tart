/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */

#include "tart/CFG/Expr.h"
#include "tart/CFG/Type.h"
#include "tart/CFG/Defn.h"
#include "tart/CFG/TypeDefn.h"
#include "tart/CFG/Block.h"
#include "tart/CFG/PrimitiveType.h"
#include "tart/CFG/CompositeType.h"
#include "tart/CFG/FunctionType.h"
#include "tart/CFG/FunctionDefn.h"
#include "tart/Sema/CallCandidate.h"
#include "tart/Common/Diagnostics.h"

namespace tart {

/// -------------------------------------------------------------------
/// Utility functions

#ifdef EXPR_TYPE
#undef EXPR_TYPE
#endif

#define EXPR_TYPE(x) #x,

namespace {
const char * ExprTypeNames[] = {
#include "tart/CFG/ExprType.def"
};
}

const char * exprTypeName(Expr::ExprType type) {
  uint32_t index = (uint32_t)type;
  if (index < Expr::TypeCount) {
    return ExprTypeNames[index];
  }
  return "<Invalid Expr Type>";
}

void formatExprList(FormatStream & out, const ExprList & exprs) {
  for (ExprList::const_iterator it = exprs.begin(); it != exprs.end(); ++it) {
    if (it != exprs.begin()) {
      out << ", ";
    }

    out << *it;
  }
}

void formatExprTypeList(FormatStream & out, const ExprList & exprs) {
  for (ExprList::const_iterator it = exprs.begin(); it != exprs.end(); ++it) {
    if (it != exprs.begin()) {
      out << ", ";
    }

    out << (*it)->type();
  }
}

void formatTypeList(FormatStream & out, const TypeList & types) {
  for (TypeList::const_iterator it = types.begin(); it != types.end(); ++it) {
    if (it != types.begin()) {
      out << ", ";
    }

    out << *it;
  }
}

bool isErrorResult(const Type * ex) {
  return BadType::instance.isEqual(ex);
}

/// -------------------------------------------------------------------
/// Expr

ErrorExpr Expr::ErrorVal;

const ExprList Expr::emptyList;

Expr::Expr(ExprType k, const SourceLocation & l, const TypeRef & type)
  : exprType_(k)
  , loc_(l)
  , type_(type.type())
{}

void Expr::format(FormatStream & out) const {
  out << exprTypeName(exprType_);
}

void Expr::trace() const {
  safeMark(type_);
}

void Expr::setType(const TypeRef & type) {
  type_ = type.type();
}

/// -------------------------------------------------------------------
/// ErrorExpr
ErrorExpr::ErrorExpr()
  : Expr(Invalid, SourceLocation(), &BadType::instance)
{}

/// -------------------------------------------------------------------
/// UnaryExpr
bool UnaryExpr::isSideEffectFree() const {
  return arg_->isSideEffectFree();
}

bool UnaryExpr::isConstant() const {
  return arg_->isConstant();
}

bool UnaryExpr::isSingular() const {
  return type()->isSingular() && arg_->isSingular();
}

void UnaryExpr::format(FormatStream & out) const {
  switch (exprType()) {
    case NoOp:
      out << arg_;
      break;

    case Not:
      out << "not " << arg_;
      break;

    default:
      out << exprTypeName(exprType()) << "(" << arg_ << ")";
      break;
  }
}

void UnaryExpr::trace() const {
  Expr::trace();
  safeMark(arg_);
}

/// -------------------------------------------------------------------
/// BinaryExpr
bool BinaryExpr::isSideEffectFree() const {
  return first_->isSideEffectFree() && second_->isSideEffectFree();
}

bool BinaryExpr::isConstant() const {
  return first_->isConstant() && second_->isConstant();
}

bool BinaryExpr::isSingular() const {
  return type()->isSingular() && first_->isSingular() && second_->isSingular();
}

void BinaryExpr::format(FormatStream & out) const {
  switch (exprType()) {
    case RefEq:
      out << first_ << " is " << second_;
      break;

    case ElementRef:
      out << first_ << "[" << second_ << "]";
      break;

    case And:
      out << first_ << " and " << second_;
      break;

    case Or:
      out << first_ << " or " << second_;
      break;

    default:
      out << exprTypeName(exprType()) << "(" << first_ << ", " << second_ << ")";
      break;
  }
}

void BinaryExpr::trace() const {
  Expr::trace();
  safeMark(first_);
  safeMark(second_);
}

/// -------------------------------------------------------------------
/// ArglistExpr

bool ArglistExpr::areArgsSideEffectFree() const {
  for (ExprList::const_iterator it = args_.begin(); it != args_.end(); ++it) {
    if (!(*it)->isSideEffectFree()) {
      return false;
    }
  }

  return true;
}

void ArglistExpr::appendArg(Expr * en) {
  DASSERT(en != NULL);
  args_.push_back(en);
}

bool ArglistExpr::isSingular() const {
  for (ExprList::const_iterator it = args_.begin(); it != args_.end(); ++it) {
    if (!(*it)->isSingular()) {
      return false;
    }
  }

  return true;
}

void ArglistExpr::trace() const {
  Expr::trace();
  markList(args_.begin(), args_.end());
}

/// -------------------------------------------------------------------
/// LValueExpr
LValueExpr::LValueExpr(const SourceLocation & loc, Expr * base, ValueDefn * value)
  : Expr(LValue, loc, value->type())
  , base_(base)
  , value_(value)
{
}

bool LValueExpr::isSingular() const {
  return (base_ == NULL || base_->isSingular()) && value_->isSingular();
}

void LValueExpr::format(FormatStream & out) const {
  if (base_ != NULL) {
    out << base_ << "." << value_->name();
  } else {
    out << value_;
  }
}

void LValueExpr::trace() const {
  Expr::trace();
  safeMark(base_);
  value_->mark();
}

Expr * LValueExpr::constValue(Expr * in) {
  if (LValueExpr * lv = dyn_cast<LValueExpr>(in)) {
    if (lv->value()->defnType() == Defn::Let) {
      if (VariableDefn * var = dyn_cast<VariableDefn>(lv->value())) {
        if (ConstantExpr * cexp = dyn_cast_or_null<ConstantExpr>(var->initValue())) {
          return cexp;
        }
      }
    }
  }

  return in;
}

/// -------------------------------------------------------------------
/// ScopeNameExpr
bool ScopeNameExpr::isSingular() const { return true; }

void ScopeNameExpr::format(FormatStream & out) const {
  out << value_;
}

void ScopeNameExpr::trace() const {
  Expr::trace();
  safeMark(value_);
}

/// -------------------------------------------------------------------
/// AssignmentExpr

AssignmentExpr::AssignmentExpr(const SourceLocation & loc, Expr * to, Expr * from)
  : Expr(Assign, loc, to->type())
  , fromExpr_(from)
  , toExpr_(to)
{
  DASSERT(to != NULL);
  DASSERT(from != NULL);
}

AssignmentExpr::AssignmentExpr(ExprType k, const SourceLocation & loc, Expr * to, Expr * from)
  : Expr(k, loc, to->type())
  , fromExpr_(from)
  , toExpr_(to)
{
  DASSERT(to != NULL);
  DASSERT(from != NULL);
}

void AssignmentExpr::format(FormatStream & out) const {
  if (exprType() == Expr::PostAssign) {
    out << toExpr() << " (=) " << fromExpr();
  } else {
    out << toExpr() << " = " << fromExpr();
  }
}

/// -------------------------------------------------------------------
/// InitVarExpr

InitVarExpr::InitVarExpr(
    const SourceLocation & loc, VariableDefn * v, Expr * expr)
  : Expr(InitVar, loc, v->type())
  , var(v)
  , initExpr_(expr)
{}

bool InitVarExpr::isSingular() const {
  return initExpr_->isSingular() && var->isSingular();
}

void InitVarExpr::format(FormatStream & out) const {
  out << var << " = " << initExpr_;
}

/// -------------------------------------------------------------------
/// BoundMethodExpr

BoundMethodExpr::BoundMethodExpr(const SourceLocation & loc, Expr * selfArg, FunctionDefn * method,
    Type * type)
  : Expr(BoundMethod, loc, type)
  , selfArg_(selfArg)
  , method_(method)
{
}

bool BoundMethodExpr::isSingular() const {
  return (selfArg_ == NULL || selfArg_->isSingular()) && method_->isSingular();
}

void BoundMethodExpr::format(FormatStream & out) const {
  if (selfArg_ != NULL) {
    out << selfArg_ << "." << method_->name();
  } else {
    out << method_;
  }
}

void BoundMethodExpr::trace() const {
  Expr::trace();
  safeMark(selfArg_);
  method_->mark();
}

/// -------------------------------------------------------------------
/// CallExpr

bool CallExpr::isSingular() const {
  if (!ArglistExpr::isSingular()) {
    return false;
  }

  if (!candidates_.empty()) {
    return candidates_.size() == 1 && candidates_.front()->isSingular();
  }

  return function_ != NULL && function_->isSingular();
}

Type * CallExpr::singularParamType(int index) {
  TypeRef singularType = NULL;
  for (Candidates::iterator it = candidates_.begin(); it != candidates_.end(); ++it) {
    if ((*it)->isCulled()) {
      continue;
    }

    TypeRef ty = (*it)->paramType(index);
    if (!singularType.isDefined()) {
      singularType = ty;
    } else if (!ty.isEqual(singularType)) {
      return NULL;
    }
  }

  return singularType.type();
}

Type * CallExpr::singularResultType() {
  TypeRef singularType;
  for (Candidates::iterator it = candidates_.begin(); it != candidates_.end(); ++it) {
    CallCandidate * cc = *it;
    if (cc->isCulled()) {
      continue;
    }

    TypeRef ty = cc->resultType();
    if (cc->method() != NULL && cc->method()->isCtor()) {
      ty = cc->functionType()->selfParam()->type();
    }

    if (!singularType.isDefined()) {
      singularType = ty;
    } else if (!ty.isEqual(singularType)) {
      return NULL;
    }
  }

  return singularType.type();
}

CallCandidate * CallExpr::singularCandidate() {
  CallCandidate * singularCandidate = NULL;
  for (Candidates::iterator it = candidates_.begin(); it != candidates_.end(); ++it) {
    if ((*it)->isCulled()) {
      continue;
    }

    if (singularCandidate == NULL) {
      singularCandidate = (*it);
    } else {
      return NULL;
    }
  }

  return singularCandidate;
}

bool CallExpr::hasAnyCandidates() const {
  for (Candidates::const_iterator it = candidates_.begin(); it != candidates_.end(); ++it) {
    if (!(*it)->isCulled()) {
      return true;
    }
  }

  return false;
}

void CallExpr::format(FormatStream & out) const {
  if (function_ != NULL) {
    //if (out.getShowType()) {
    //  out << "(" << function_ << ")";
    //} else {
      out << function_;
    //}
  } else if (candidates_.size() == 1) {
    FunctionDefn * func = candidates_.front()->method();
    if (func == NULL) {
      out << "(" << candidates_.front()->base() << ")";
    } else if (out.getShowType()) {
      out << "(" << func << ")";
    } else {
      out << func;
    }
  } else {
    FunctionDefn * func = candidates_.front()->method();
    out << func->name();
    //out << "{" << candidates_.size() << " candidates}";
  }

  out << "(";
  for (ExprList::const_iterator it = args_.begin(); it != args_.end(); ++it) {
    if (it != args_.begin()) {
      out << ", ";
    }

    out << (*it);
    /*if (out.getShowType()) {
      //out << ":" << (*it)->type();
      //out << (*it)->type();
    } else {
      //out << ":" << (*it)->type();
    }*/
  }
  out << ") ";

  if (out.getShowType() && expectedReturnType_) {
    out << "-> " << expectedReturnType_ << " ";
  }
}

void CallExpr::trace() const {
  ArglistExpr::trace();
  markList(candidates_.begin(), candidates_.end());
}

/// -------------------------------------------------------------------
/// FnCallExpr

void FnCallExpr::format(FormatStream & out) const {
  if (out.getShowType()) {
    out << "(" << function_ << ")";
  } else {
    out << function_;
  }

  out << "(";
  formatExprList(out, args_);
  out << ")";
}

bool FnCallExpr::isSingular() const {
  return (function_->isSingular() && ArglistExpr::isSingular());
}

void FnCallExpr::trace() const {
  ArglistExpr::trace();
  function_->mark();
}

/// -------------------------------------------------------------------
/// IndirectCallExpr

void IndirectCallExpr::format(FormatStream & out) const {
  if (out.getShowType()) {
    out << "(" << function_ << ")";
  } else {
    out << function_;
  }

  out << "(";
  formatExprList(out, args_);
  out << ")";
}

bool IndirectCallExpr::isSingular() const {
  return (function_->isSingular() && ArglistExpr::isSingular());
}

void IndirectCallExpr::trace() const {
  ArglistExpr::trace();
  function_->mark();
}

/// -------------------------------------------------------------------
/// NewExpr
bool NewExpr::isSingular() const {
  return type()->isSingular();
}

void NewExpr::format(FormatStream & out) const {
  out << "new " << type();
}

/// -------------------------------------------------------------------
/// CastExpr
void CastExpr::format(FormatStream & out) const {
  if (exprType() == ImplicitCast) {
    out << "implicitCast<" << type() << ">(" << arg() << ")";
  } else {
    out << "cast<" << type() << ">(" << arg() << ")";
  }
}

/// -------------------------------------------------------------------
/// BinaryOpcodeExpr

bool BinaryOpcodeExpr::isSingular() const {
  return type()->isSingular() && first()->isSingular() && second()->isSingular();
}

bool BinaryOpcodeExpr::isSideEffectFree() const {
  return first()->isSideEffectFree() && second()->isSideEffectFree();
}

void BinaryOpcodeExpr::format(FormatStream & out) const {
  switch (opCode_) {
    case llvm::Instruction::Add: {
      out << first() << " + " << second();
      break;
    }

    case llvm::Instruction::Sub: {
      out << first() << " - " << second();
      break;
    }

    case llvm::Instruction::Mul: {
      out << first() << " * " << second();
      break;
    }

    case llvm::Instruction::SDiv:
    case llvm::Instruction::UDiv:
    case llvm::Instruction::FDiv: {
      out << first() << " / " << second();
      break;
    }

    default: {
      out << "BinaryOpcode(" << first() << ", " << second() << ")";
      break;
    }
  }
}

/// -------------------------------------------------------------------
/// CompareExpr
CompareExpr::CompareExpr(const SourceLocation & loc, Predicate pred)
  : BinaryExpr(Compare, loc, &BoolType::instance)
  , predicate(pred)
{}

/** Constructor. */
CompareExpr::CompareExpr(const SourceLocation & loc, Predicate pred,
    Expr * f, Expr * s)
  : BinaryExpr(Compare, loc, &BoolType::instance, f, s)
  , predicate(pred)
{}

void CompareExpr::format(FormatStream & out) const {
  const char * oper;
  switch (getPredicate()) {
    case llvm::CmpInst::FCMP_OEQ:
    case llvm::CmpInst::FCMP_UEQ:
    case llvm::CmpInst::ICMP_EQ:
      oper = "==";
      break;

    case llvm::CmpInst::FCMP_ONE:
    case llvm::CmpInst::FCMP_UNE:
    case llvm::CmpInst::ICMP_NE:
      oper = "!=";
      break;

    case llvm::CmpInst::FCMP_OGT:
    case llvm::CmpInst::FCMP_UGT:
    case llvm::CmpInst::ICMP_UGT:
    case llvm::CmpInst::ICMP_SGT:
      oper = ">";
      break;

    case llvm::CmpInst::FCMP_OLT:
    case llvm::CmpInst::FCMP_ULT:
    case llvm::CmpInst::ICMP_ULT:
    case llvm::CmpInst::ICMP_SLT:
      oper = "<";
      break;

    case llvm::CmpInst::FCMP_OGE:
    case llvm::CmpInst::FCMP_UGE:
    case llvm::CmpInst::ICMP_UGE:
    case llvm::CmpInst::ICMP_SGE:
      oper = ">=";
      break;

    case llvm::CmpInst::FCMP_OLE:
    case llvm::CmpInst::FCMP_ULE:
    case llvm::CmpInst::ICMP_ULE:
    case llvm::CmpInst::ICMP_SLE:
      oper = "<=";
      break;

    default:
      DFAIL("IllegalState");
  }

  out << first() << " " << oper << " " << second();
}

/// -------------------------------------------------------------------
/// IRValueExpr
void IRValueExpr::format(FormatStream & out) const {
  out << "<IRValue>";
}

/// -------------------------------------------------------------------
/// LocalCallExpr
void LocalCallExpr::format(FormatStream & out) const {
  out << "local call " << target_ << " return=" << returnState_;
}

/// -------------------------------------------------------------------
/// InstanceOfExpr
InstanceOfExpr::InstanceOfExpr(const SourceLocation & loc, Expr * value, Type * ty)
  : Expr(InstanceOf, loc, &BoolType::instance)
  , value_(value)
  , toType_(ty)
{}

InstanceOfExpr::InstanceOfExpr(const SourceLocation & loc, Expr * value, const TypeRef & ty)
  : Expr(InstanceOf, loc, &BoolType::instance)
  , value_(value)
  , toType_(ty.type())
{}

void InstanceOfExpr::format(FormatStream & out) const {
  out << value_ << " isa " << toType_;
}

void InstanceOfExpr::trace() const {
  Expr::trace();
  safeMark(value_);
  safeMark(toType_);
}

bool InstanceOfExpr::isSingular() const {
  return value_->isSingular() && toType_->isSingular();
}

} // namespace tart
