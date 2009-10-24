/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */

#include "tart/CFG/Module.h"
#include "tart/CFG/Constant.h"
#include "tart/CFG/PrimitiveType.h"
#include "tart/CFG/FunctionType.h"
#include "tart/CFG/FunctionDefn.h"
#include "tart/CFG/NativeType.h"
#include "tart/CFG/TypeDefn.h"
#include "tart/CFG/TypeConstraint.h"
#include "tart/Objects/Builtins.h"
#include "tart/Common/Diagnostics.h"
#include "tart/Common/InternedString.h"
#include "tart/Sema/ExprAnalyzer.h"
#include "tart/Sema/TypeAnalyzer.h"
#include "tart/Sema/CallCandidate.h"
#include <llvm/DerivedTypes.h>

namespace tart {

Expr * ExprAnalyzer::reduceCall(const ASTCall * call, Type * expected) {
  const ASTNode * callable = call->func();
  const ASTNodeList & args = call->args();

  if (callable->nodeType() == ASTNode::Id ||
      callable->nodeType() == ASTNode::Member ||
      callable->nodeType() == ASTNode::Specialize) {
    return callName(call->location(), callable, args, expected);
  } else if (callable->nodeType() == ASTNode::Super) {
    return callSuper(call->location(), args, expected);
  } else if (callable->nodeType() == ASTNode::BuiltIn) {
    // Built-in type constructor
    Defn * tdef = static_cast<const ASTBuiltIn *>(callable)->value();
    return callExpr(call->location(), cast<TypeDefn>(tdef)->asExpr(), args, expected);
  } else if (callable->nodeType() == ASTNode::GetElement) {
    return callExpr(call->location(), reduceElementRef(
        static_cast<const ASTOper *>(callable), false), args, expected);
  }

  diag.fatal(call) << "Not a callable expression " << call;
  DFAIL("Invalid call type");
}

Expr * ExprAnalyzer::callName(SLC & loc, const ASTNode * callable, const ASTNodeList & args,
    Type * expected, bool isOptional) {

  // Specialize works here because lookupName handles explicit specializations.
  DASSERT(callable->nodeType() == ASTNode::Id ||
      callable->nodeType() == ASTNode::Member ||
      callable->nodeType() == ASTNode::Specialize);

  bool isUnqualified = callable->nodeType() == ASTNode::Id;
  bool success = true;

  ExprList results;
  lookupName(results, callable);

  // If there were no results, and it was a qualified search, then
  // it's an error. (If it's unqualified, then there are still things
  // left to try.)
  if (results.empty() && !isUnqualified) {
    if (isOptional) {
      return NULL;
    }

    diag.error(loc) << "Undefined method " << callable;
    diag.writeLnIndent("Scopes searched:");
    dumpScopeHierarchy();
    return &Expr::ErrorVal;
  }

  // Try getting the lookup results as a type definition.
  TypeList typeList;
  if (!results.empty() && getTypesFromExprs(loc, results, typeList)) {
    // TODO: Handle ambiguous type resolution.
    if (typeList.size() > 1) {
      diag.error(loc) << "Multiple definitions for '" << callable << "'";
      return &Expr::ErrorVal;
    }

    // TODO: We could pass the whole list to callName, and add them as overloads.
    Type * type = typeList.front();
    if (type->typeDefn() == NULL) {
      diag.error(loc) << "Type '" << type << "' is not constructable";
      return &Expr::ErrorVal;
    }

    return callConstructor(loc, type->typeDefn(), args);
  }

  CallExpr * call = new CallExpr(Expr::Call, loc, NULL);
  call->setExpectedReturnType(expected);
  for (ExprList::iterator it = results.begin(); it != results.end(); ++it) {
    if (LValueExpr * lv = dyn_cast<LValueExpr>(*it)) {
      if (FunctionDefn * func = dyn_cast<FunctionDefn>(lv->value())) {
        success &= addOverload(call, lvalueBase(lv), func, args);

        // If there's a base pointer, then it's not really unqualified.
        //if (lv->base() != NULL) {
        //  isUnqualified = false;
        //}
      } else if (VariableDefn * var = dyn_cast<VariableDefn>(lv->value())) {
        if (!analyzeValueDefn(var, Task_PrepTypeComparison)) {
          return false;
        }

        if (FunctionType * ft = dyn_cast<FunctionType>(var->type().type())) {
          success &= addOverload(call, lv, ft, args);
        } else if (BoundMethodType * bmt = dyn_cast<BoundMethodType>(var->type().type())) {
          success &= addOverload(call, lv, bmt->fnType(), args);
        }

        /*else if (NativePointerType * npt = dyn_cast<NativePointerType>(var->type().type())) {
          TypeRef targetType = npt->typeParam(0);
          if (analyzeType(targetType, Task_PrepTypeComparison)) {
            if (FunctionType * ft = dyn_cast<FunctionType>(targetType.type())) {
              success &= addOverload(call, lv, ft, args);
            }
          }
        } else if (AddressType * mat = dyn_cast<AddressType>(var->type().type())) {
          TypeRef targetType = mat->typeParam(0);
          if (analyzeType(targetType, Task_PrepTypeComparison)) {
            if (FunctionType * ft = dyn_cast<FunctionType>(targetType.type())) {
              success &= addOverload(call, lv, ft, args);
            }
          }
        }*/
      }
    } else {
      diag.fatal(loc) << *it << " is not callable.";
    }
  }

  if (!reduceArgList(args, call)) {
    return &Expr::ErrorVal;
  }

  // If it's unqualified, then do ADL.
  if (isUnqualified && !args.empty()) {
    const char * name = static_cast<const ASTIdent *>(callable)->value();
    lookupByArgType(call, name, args);
  }

  if (!success) {
    return &Expr::ErrorVal;
  } else if (results.empty()) {
    diag.error(loc) << "Undefined method " << callable;
    diag.writeLnIndent("Scopes searched:");
    dumpScopeHierarchy();
    return &Expr::ErrorVal;
  } else if (call->candidates().empty()) {
    // Generate the calling signature in a buffer.
    std::stringstream callsig;
    FormatStream fs(callsig);
    fs << Format_Dealias << callable << "(";
    formatExprTypeList(fs, call->args());
    fs << ")";
    if (expected != NULL) {
      fs << " -> " << expected;
    }

    diag.error(loc) << "No matching method for call to " << callsig.str() << ", candidates are:";
    for (ExprList::iterator it = results.begin(); it != results.end(); ++it) {
      Expr * resultMethod = *it;
      if (LValueExpr * lval = dyn_cast<LValueExpr>(*it)) {
        diag.info(lval->value()) << Format_Type << lval->value();
      } else {
        diag.info(*it) << *it;
      }
    }
    return &Expr::ErrorVal;
  }

  call->setType(reduceReturnType(call));
  return call;
}

void ExprAnalyzer::lookupByArgType(CallExpr * call, const char * name, const ASTNodeList & args) {
  //diag.debug(call) << "ADL: " << name;
  DefnList defns;
  llvm::SmallPtrSet<Type *, 16> typesSearched;

  const ExprList & callArgs = call->args();
  for (ExprList::const_iterator it = callArgs.begin(); it != callArgs.end(); ++it) {
    Expr * arg = *it;
    if (arg->type() != NULL && arg->type()->isSingular()) {
      Type * argType = dealias(arg->type());
      if (argType != NULL && typesSearched.insert(argType)) {
        AnalyzerBase::analyzeType(argType, Task_PrepMemberLookup);

        // TODO: Also include overrides
        // TODO: Refactor
        Defn * argTypeDefn = argType->typeDefn();
        //Scope * argTypeScope = argType->memberScope();
        if (argTypeDefn != NULL && argTypeDefn->definingScope() != NULL) {
          //diag.debug(call) << "ADL: " << call << " - " << arg << ":" << argType;
          argTypeDefn->definingScope()->lookupMember(name, defns, true);
        }
      }
    }
  }

  llvm::SmallPtrSet<FunctionDefn *, 32> methodsFound;

  // Set of methods already found.
  Candidates & cclist = call->candidates();
  for (Candidates::iterator cc = cclist.begin(); cc != cclist.end(); ++cc) {
    methodsFound.insert((*cc)->method());
  }

  for (DefnList::iterator it = defns.begin(); it != defns.end(); ++it) {
    if (FunctionDefn * f = dyn_cast<FunctionDefn>(*it)) {
      if ((f->storageClass() == Storage_Static || f->storageClass() == Storage_Global) &&
          methodsFound.insert(f)) {
        //diag.debug(f) << "Adding ADL overload " << f;
        addOverload(call, NULL, f, args);
      }
    }
  }
}

Expr * ExprAnalyzer::callExpr(SLC & loc, Expr * func, const ASTNodeList & args, Type * expected) {
  if (isErrorResult(func)) {
    return func;
  } else if (TypeLiteralExpr * typeExpr = dyn_cast<TypeLiteralExpr>(func)) {
    // It's a type.
    return callConstructor(loc, typeExpr->value()->typeDefn(), args);
  } else if (LValueExpr * lval = dyn_cast<LValueExpr>(func)) {
    CallExpr * call = new CallExpr(Expr::Call, loc, NULL);
    call->setExpectedReturnType(expected);
    if (FunctionDefn * func = dyn_cast<FunctionDefn>(lval->value())) {
      addOverload(call, lvalueBase(lval), func, args);
    } else {
      diag.error(loc) << func << " is not a callable expression.";
      return &Expr::ErrorVal;
    }

    if (!reduceArgList(args, call)) {
      return &Expr::ErrorVal;
    }

    call->setType(reduceReturnType(call));
    return call;

  } else {
    diag.fatal(func) << Format_Verbose << "Unimplemented function type";
    DFAIL("Unimplemented");
  }
}

Expr * ExprAnalyzer::callSuper(SLC & loc, const ASTNodeList & args, Type * expected) {
  if (currentFunction_ == NULL || currentFunction_->storageClass() != Storage_Instance) {
    diag.fatal(loc) << "'super' only callable from instance methods";
    return &Expr::ErrorVal;
  }

  TypeDefn * enclosingClassDefn = currentFunction_->enclosingClassDefn();
  CompositeType * enclosingClass = cast<CompositeType>(enclosingClassDefn->typeValue());
  CompositeType * superClass = enclosingClass->super();

  if (superClass == NULL) {
    diag.fatal(loc) << "class '" << enclosingClass << "' has no super class";
    return &Expr::ErrorVal;
  }

  DefnList methods;
  if (!superClass->memberScope()->lookupMember(currentFunction_->name(), methods, true)) {
    diag.error(loc) << "Superclass method '" << currentFunction_->name() <<
        " not found in class " << enclosingClass;
    return &Expr::ErrorVal;
  }

  ParameterDefn * selfParam = currentFunction_->functionType()->selfParam();
  DASSERT_OBJ(selfParam != NULL, currentFunction_);
  DASSERT_OBJ(selfParam->type().isDefined(), currentFunction_);
  TypeDefn * selfType = selfParam->type().defn();
  DASSERT_OBJ(selfType != NULL, currentFunction_);
  Expr * selfExpr = new LValueExpr(selfParam->location(), NULL, selfParam);
  selfExpr = superClass->implicitCast(loc, selfExpr);

  CallExpr * call = new CallExpr(Expr::ExactCall, loc, NULL);
  call->setExpectedReturnType(expected);
  for (DefnList::iterator it = methods.begin(); it != methods.end(); ++it) {
    if (FunctionDefn * func = dyn_cast<FunctionDefn>(*it)) {
      addOverload(call, selfExpr, func, args);
    } else {
      diag.fatal(loc) << *it << " is not callable.";
    }
  }

  if (!reduceArgList(args, call)) {
    return &Expr::ErrorVal;
  }

  call->setType(reduceReturnType(call));
  return call;
}

Expr * ExprAnalyzer::callConstructor(SLC & loc, TypeDefn * tdef, const ASTNodeList & args) {
  Type * type = tdef->typeValue();
  //TypeDefn * tdef = type->typeDefn();
  checkAccess(loc, tdef);
  if (!tdef->isTemplate() && !tdef->isTemplateMember()) {
    module->addSymbol(tdef);
  }

  // First thing we need to know is how much tdef has been analyzed.
  if (!AnalyzerBase::analyzeType(type, Task_PrepConstruction)) {
    return &Expr::ErrorVal;
  }

  /*if (tdef->isSynthetic()) {
    if (CompositeType * ctype = dyn_cast<CompositeType>(type)) {

    }
  }*/

  ExprList templateArgs;
  ExprList coercedArgs;
  FunctionDefn * constructor = NULL;
  DefnList methods;

  CallExpr * call = new CallExpr(Expr::Construct, loc, tdef->asExpr());
  call->setExpectedReturnType(type);
  if (tdef->isTemplate()) {
    if (lookupTemplateMember(methods, tdef, istrings.idConstruct, loc)) {
      Expr * newExpr = new NewExpr(loc, type);
      DASSERT(!methods.empty());
      for (DefnList::const_iterator it = methods.begin(); it != methods.end(); ++it) {
        FunctionDefn * cons = cast<FunctionDefn>(*it);
        if (analyzeDefn(cons, Task_PrepTypeComparison)) {
          DASSERT(cons->type().isDefined());
          DASSERT(!cons->returnType().isDefined() || cons->returnType().isVoidType());
          DASSERT(cons->storageClass() == Storage_Instance);
          DASSERT(cons->isTemplate() || cons->isTemplateMember());
          cons->addTrait(Defn::Ctor);
          addOverload(call, newExpr, cons, args);
        }
      }
    } else if (lookupTemplateMember(methods, tdef, istrings.idCreate, loc)) {
      DASSERT(!methods.empty());
      for (DefnList::const_iterator it = methods.begin(); it != methods.end(); ++it) {
        FunctionDefn * create = cast<FunctionDefn>(*it);
        if (create->storageClass() == Storage_Static) {
          if (analyzeDefn(create, Task_PrepTypeComparison)) {
            DASSERT(create->type().isDefined());
            //Type * returnType = create->returnType();
            addOverload(call, NULL, create, args);
          }
        }
      }
    } else {
      diag.error(loc) << "No constructors found for type " << tdef;
      DFAIL("Implement constructor inheritance for templates.");
      return &Expr::ErrorVal;
    }
  } else {
    if (type->memberScope()->lookupMember(istrings.idConstruct, methods, false)) {
      Expr * newExpr = new NewExpr(loc, type);
      DASSERT(!methods.empty());
      for (DefnList::const_iterator it = methods.begin(); it != methods.end(); ++it) {
        FunctionDefn * cons = cast<FunctionDefn>(*it);
        DASSERT(cons->type().isDefined());
        DASSERT(cons->isCtor());
        DASSERT(!cons->returnType().isDefined() || cons->returnType().isVoidType());
        DASSERT(cons->storageClass() == Storage_Instance);
        addOverload(call, newExpr, cons, args);
      }
    } else if (type->memberScope()->lookupMember(istrings.idCreate, methods, false)) {
      DASSERT(!methods.empty());
      for (DefnList::const_iterator it = methods.begin(); it != methods.end(); ++it) {
        FunctionDefn * create = cast<FunctionDefn>(*it);
        DASSERT(create->type().isDefined());
        if (create->storageClass() == Storage_Static) {
          //const Type * returnType = create->returnType();
          addOverload(call, NULL, create, args);
        }
      }
    } else if (type->memberScope()->lookupMember(istrings.idConstruct, methods, true)) {
      Expr * newExpr = new NewExpr(loc, type);
      DASSERT(!methods.empty());
      for (DefnList::const_iterator it = methods.begin(); it != methods.end(); ++it) {
        FunctionDefn * cons = cast<FunctionDefn>(*it);
        DASSERT(cons->type().isDefined());
        DASSERT(cons->isCtor());
        DASSERT(!cons->returnType().isDefined() || cons->returnType().isVoidType());
        DASSERT(cons->storageClass() == Storage_Instance);
        addOverload(call, newExpr, cons, args);
      }
    } else {
      diag.error(loc) << "No constructors found for type " << tdef;
      return &Expr::ErrorVal;
    }
  }

  if (!call->hasAnyCandidates()) {
    diag.error(loc) << "No constructor found matching input arguments (" <<
      args << "), candidates are:";
    for (DefnList::const_iterator it = methods.begin(); it != methods.end(); ++it) {
      diag.info(*it) << Format_Verbose << *it;
    }

    return &Expr::ErrorVal;
  }

  if (!reduceArgList(args, call)) {
    return &Expr::ErrorVal;
  }

  call->setType(reduceReturnType(call));
  return call;

#if 0
    // Do compile-time evaluation of the constructor if possible.
    //coercedArgs.insert(coercedArgs.begin(), newInst);
    FunctionDef * constructorFunc = cast<FunctionDef>(const_cast<Declaration *>(constructor));
    const Expr * ctObj = evalCallable(loc, constructorFunc, coercedArgs, NULL);
    if (ctObj != NULL) {
      return ctObj;
    }
#endif
}

/** Attempt a coercive cast, that is, try to find a 'coerce' method that will convert
    to 'toType'. */
CallExpr * ExprAnalyzer::tryCoerciveCast(Expr * in, Type * toType) {
  if (CompositeType * ctype = dyn_cast<CompositeType>(toType)) {
    if (!ctype->coercers().empty()) {
      if (!analyzeType(ctype, Task_PrepConversion)) {
        return NULL;
      }

      CallExpr * call = new CallExpr(Expr::Call, in->location(), NULL);
      call->setExpectedReturnType(toType);
      call->args().push_back(in);

      for (MethodList::const_iterator it = ctype->coercers().begin();
          it != ctype->coercers().end(); ++it) {
        addOverload(call, NULL, *it, call->args());
      }

      if (call->candidates().empty()) {
        return NULL;
      }

      call->setType(reduceReturnType(call));
      return call;
    }
  }

  return NULL;
}

bool ExprAnalyzer::reduceArgList(const ASTNodeList & in, CallExpr * call) {
  ExprList & args = call->args();
  for (size_t i = 0; i < in.size(); ++i) {
    const ASTNode * arg = in[i];
    if (arg->nodeType() == ASTNode::Keyword) {
      arg = static_cast<const ASTKeywordArg *>(arg)->arg();
    }

    Type * paramType = getMappedParameterType(call, i);
    if (paramType == NULL) {
      return false;
    }

    Expr * ex = reduceExpr(arg, paramType);
    if (isErrorResult(ex)) {
      return false;
    }

    args.push_back(ex);
  }

  return true;
}

Type * ExprAnalyzer::reduceReturnType(CallExpr * call) {
  Type * ty = call->singularResultType();
  if (ty != NULL) {
    if (call->isSingular()) {
      DASSERT_OBJ(ty->isSingular(), call);
    }

    return ty;
  }

  return new ResultOfConstraint(call);
}

Type * ExprAnalyzer::getMappedParameterType(CallExpr * call, int index) {
  Type * ty = call->singularParamType(index);
  if (ty != NULL) {
    return ty;
  }

  return new ParameterOfConstraint(call, index);
}

bool ExprAnalyzer::addOverload(CallExpr * call, Expr * baseExpr, FunctionDefn * method,
    const ASTNodeList & args) {
  if (!analyzeValueDefn(method, Task_PrepConversion)) {
    return false;
  }

  DASSERT_OBJ(method->type().isDefined(), method);
  ParameterAssignments pa;
  ParameterAssignmentsBuilder builder(pa, method->functionType());
  if (builder.assignFromAST(args)) {
    call->candidates().push_back(new CallCandidate(call, baseExpr, method, pa));
  }

  return true;
}

bool ExprAnalyzer::addOverload(CallExpr * call, LValueExpr * fn, const FunctionType * ftype,
    const ASTNodeList & args) {
  //if (!analyzeType(ftype, Task_PrepOverloadSelection)) {
  //  return false;
  //}

  DASSERT_OBJ(ftype != NULL, fn);
  ParameterAssignments pa;
  ParameterAssignmentsBuilder builder(pa, ftype);
  if (builder.assignFromAST(args)) {
    call->candidates().push_back(new CallCandidate(call, fn, ftype, pa));
  }

  return true;
}

bool ExprAnalyzer::addOverload(CallExpr * call, Expr * baseExpr, FunctionDefn * method,
    const ExprList & args) {
  if (!analyzeValueDefn(method, Task_PrepConversion)) {
    return false;
  }

  DASSERT_OBJ(method->type().isDefined(), method);
  ParameterAssignments pa;
  ParameterAssignmentsBuilder builder(pa, method->functionType());
  for (int i = 0; i < args.size(); ++i) {
    builder.addPositionalArg();
  }

  if (!builder.check()) {
    return false;
  }

  call->candidates().push_back(new CallCandidate(call, baseExpr, method, pa));
  return true;
}

} // namespace tart
