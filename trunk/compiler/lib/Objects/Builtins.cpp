/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */

#include "tart/CFG/Module.h"
#include "tart/CFG/PrimitiveType.h"
#include "tart/CFG/NativeType.h"
#include "tart/CFG/CompositeType.h"
#include "tart/CFG/TypeDefn.h"
#include "tart/CFG/TypeLiteral.h"
#include "tart/CFG/FunctionDefn.h"
#include "tart/Objects/Builtins.h"
#include "tart/Common/PackageMgr.h"
#include "tart/Common/Diagnostics.h"
#include "tart/Sema/AnalyzerBase.h"
#include "tart/Sema/ScopeBuilder.h"
#include "tart/Parse/Parser.h"

namespace tart {

namespace {
  SourceFile builtinSource("");

  struct EssentialType {
    const char * name;
    Type ** type;
  };

  EssentialType annexTypes[] = {
    { "tart.core.Iterable", &Builtins::typeIterable },
    { "tart.core.Iterator", &Builtins::typeIterator },
  };
}

Module Builtins::module(&builtinSource, "$builtin");
Module Builtins::syntheticModule(&builtinSource, "$synthetic");

SystemClass Builtins::typeTypeInfoBlock("tart.core.TypeInfoBlock");
SystemClass Builtins::typeObject("tart.core.Object");
SystemClass Builtins::typeString("tart.core.String");
SystemClass Builtins::typeArray("tart.core.Array");
SystemClass Builtins::typeRange("tart.core.Range");
SystemClass Builtins::typeThrowable("tart.core.Throwable");
Type * Builtins::typeIterable;
Type * Builtins::typeIterator;
SystemClass Builtins::typeUnsupportedOperationException("tart.core.UnsupportedOperationException");

SystemClass Builtins::typeType("tart.reflect.Type");
SystemClass Builtins::typeSimpleType("tart.reflect.SimpleType");
SystemClass Builtins::typeComplexType("tart.reflect.ComplexType");
SystemClass Builtins::typeEnumType("tart.reflect.EnumType");
SystemClass Builtins::typeFunctionType("tart.reflect.FunctionType");
SystemClass Builtins::typeDerivedType("tart.reflect.DerivedType");
SystemClass Builtins::typeMember("tart.reflect.Member");
SystemClass Builtins::typeField("tart.reflect.Field");
SystemClass Builtins::typeProperty("tart.reflect.Property");
SystemClass Builtins::typeMethod("tart.reflect.Method");
SystemClass Builtins::typeModule("tart.reflect.Module");

SystemClass Builtins::typeAttribute("tart.core.Attribute");
SystemClass Builtins::typeIntrinsicAttribute("tart.annex.Intrinsic");

SystemClass Builtins::typeRef("tart.core.Ref");
SystemClass Builtins::typeValueRef("tart.core.ValueRef");

Type * Builtins::typeUnwindException;

TypeAlias Builtins::typeAliasString = NULL;

FunctionDefn * Builtins::funcHasBase;
FunctionDefn * Builtins::funcTypecastError;

void Builtins::init() {
  // Initialize primitive types
  PrimitiveType::initPrimitiveTypes(&module);

  // Initialize all intrinsic operators and functions.
  initOperators();

  // Other built-in types
  AddressType::initBuiltin();
  NativeArrayType::initBuiltin();
  TypeLiteralType::initBuiltin();

  DASSERT(module.module() != NULL);
  ScopeBuilder::createScopeMembers(&module);
}

Module * Builtins::loadSystemModule(const char * name) {
  Module * mod = PackageMgr::get().getModuleForImportPath(name);
  if (mod != NULL) {
    return mod;
  }

  diag.fatal() << "Error: Can't load builtin definition for '" << name << "'";
  abort();
}

Defn * Builtins::loadSystemDef(const char * name) {
  Module * mod = loadSystemModule(name);
  return mod->primaryDefn();
}

Type * Builtins::loadSystemType(const char * name) {
  Type * result = cast<TypeDefn>(loadSystemDef(name))->typeValue();
  DASSERT(result != NULL);
  return result;
}

Defn * Builtins::getSingleDefn(Type * type, const char * name) {
  DefnList defs;
  if (CompositeType * ctype = dyn_cast<CompositeType>(type)) {
    AnalyzerBase::analyzeType(ctype, Task_PrepMemberLookup);
    if (!ctype->lookupMember(name, defs, false)) {
      diag.info() << "Couldn't find system definition for " << name;
      DFAIL("Couldn't find system definition");
    }

    if (defs.size() > 1) {
      DFAIL("Couldn't find system definition");
    }
  }

  return defs.front();
}

void Builtins::loadSystemClasses() {
  typeArray.get();
  typeAttribute.get();
  typeTypeInfoBlock.get();
  typeType.get();
  typeObject.get();
  typeString.get();
  typeThrowable.get();
  typeUnsupportedOperationException.get();
  typeIntrinsicAttribute.get();

  // Analyze class Object.
  //AnalyzerBase::analyzeType(typeObject, Task_PrepCodeGeneration);
  AnalyzerBase::analyzeType(typeObject, Task_PrepMemberLookup);

  // Get the function that tests for a type
  funcHasBase = getMember<FunctionDefn>(typeTypeInfoBlock.get(), "hasBase");
  funcTypecastError = getMember<FunctionDefn>(typeTypeInfoBlock.get(), "typecastError");

  // Get the low-level exception structure
  typeUnwindException = getMember<TypeDefn>(typeThrowable.get(), "UnwindException")->typeValue();

  // Set the aliases
  typeAliasString.setValue(typeString);
}

bool Builtins::compileBuiltins(ProgramSource & source) {
  Parser parser(&source, &module);
  return parser.parse();
}

#define elementsof(x)   (sizeof(x)/sizeof(x[0]))

void Builtins::registerEssentialType(const Type * type) {
  for (size_t i = 0; i < elementsof(annexTypes); ++i) {
    EssentialType * atype = &annexTypes[i];
    if (type->typeDefn()->qualifiedName() == atype->name) {
      *atype->type = const_cast<Type *>(type);
      break;
    }
  }
}

FunctionDefn * Builtins::objectCoerceFn() {
  if (!AnalyzerBase::analyzeType(Builtins::typeObject, Task_PrepConversion)) {
    return NULL;
  }

  // Get the coerce function that's a template.
  FunctionDefn * coerceFn = NULL;
  const MethodList & coercers = Builtins::typeObject->coercers();
  for (MethodList::const_iterator it = coercers.begin(); it != coercers.end(); ++it) {
    if ((*it)->isTemplate()) {
      coerceFn = *it;
      break;
    }
  }

  return coerceFn;
}

CompositeType * SystemClass::get() const {
  if (type_ == NULL) {
    type_ = cast<CompositeType>(Builtins::loadSystemType(typeName_));
  }

  return type_;
}

const llvm::Type * SystemClass::irType() const {
  return get()->irType();
}

const llvm::Type * SystemClass::irEmbeddedType() const {
  return get()->irEmbeddedType();
}

const llvm::Type * SystemClass::irParameterType() const {
  return get()->irParameterType();
}

const llvm::Type * SystemClass::irReturnType() const {
  return get()->irReturnType();
}

TypeDefn * SystemClass::typeDefn() const {
  return get()->typeDefn();
}

}
