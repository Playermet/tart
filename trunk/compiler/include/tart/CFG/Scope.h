/* ================================================================ *
    TART - A Sweet Programming Language.
 * ================================================================ */
 
#ifndef TART_CFG_SCOPE_H
#define TART_CFG_SCOPE_H

#ifndef TART_CFG_SYMBOLTABLE_H
#include "tart/CFG/SymbolTable.h"
#endif

#ifndef TART_COMMON_GC_H
#include "tart/Common/GC.h"
#endif

#include <string>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/SetVector.h>

namespace tart {

class ProgramSource;
class Defn;
class Expr;

/// -------------------------------------------------------------------
/// Scope interface
class Scope {
protected:
  virtual ~Scope() {}
  
public:
  /** Return the next outer scope. */
  virtual Scope * parentScope() const = 0;
  
  /** Add a new declaration to this scope. */
  virtual void addMember(Defn * d) = 0;
  
  /** Find a declaration by name */
  virtual bool lookupMember(const char * ident, DefnList & defs, bool inherit = false) const = 0;
      
  /** Convenience function used to look up a member with no overloads. */
  Defn * lookupSingleMember(const char * ident, bool inherit = false) const;

  /** Return true if this scope allows overloading. Local scopes and parameter scopes do not. */
  virtual bool allowOverloads() { return false; }

  /** Get the base pointer needed to access members found in this scope.  */
  virtual Expr * getBaseExpr() { return NULL; }

  /** Debugging function to dump the current hierarchy. */
  virtual void dumpHierarchy(bool full = true) const = 0;
};

typedef llvm::SetVector<Scope *> ScopeSet;

/// -------------------------------------------------------------------
/// An implementation of a scope.
class IterableScope : public Scope {
private:
  OrderedSymbolTable members_;
  Scope * parentScope_;
  Defn * defn_;

#ifndef NDEBUG
  // For debugging
  const char * scopeName_;
#endif

public:
  IterableScope()
      : parentScope_(NULL)
#ifndef NDEBUG
      , scopeName_(NULL)
#endif
  {}

  IterableScope(Scope * parent)
      : parentScope_(parent)
#ifndef NDEBUG
      , scopeName_(NULL)
#endif
  {}

  /** Get the scope which encloses this one. */
  Scope * parentScope() const;
  
  /** Set the scope which encloses this one. */
  void setParentScope(Scope * parent);
  
  /** Return the first symbol in this scope. */
  Defn * firstMember() const { return members_.first(); }

  /** Return the symbol table entry for the specified symbol name. */
  const SymbolTable::Entry * findSymbol(const char * key) const {
    return members_.findSymbol(key);
  }

  /** Return a reference to the symbol table. */
  const SymbolTable & members() const { return members_; }
  SymbolTable & members() { return members_; }
  
  // Overrides

  void addMember(Defn * d);
  bool lookupMember(const char * ident, DefnList & defs, bool inherit) const;
  bool allowOverloads() { return true; }
  size_t getCount() { return members_.getCount(); }
  void trace() const;
  void setScopeName(const char * name) {
#ifndef NDEBUG
    scopeName_ = name;
#endif
  }
  void dumpHierarchy(bool full) const;
};

/// -------------------------------------------------------------------
/// A block scope
class LocalScope : public GC, public IterableScope {
public:
  LocalScope(Scope * parent) : IterableScope(parent) {
    assert(parent != NULL);
  }

  void addMember(Defn * d);
  void trace() const;
};

/// -------------------------------------------------------------------
/// A scope which delegates all methods to another scope. Allows us to
/// non-destructively modify the current scope.
class DelegatingScope : public Scope {
  Scope * delegate;
  Scope * parent;
  
public:
  DelegatingScope(Scope * s, Scope * p) : delegate(s), parent(p) {
    p = s->parentScope();
  }
  
  void setDelegate(Scope * scope) { delegate = scope; }
  void setParentScope(Scope * scope) { parent = scope; }

  Scope * parentScope() const { return parent; }
  void addMember(Defn * d) { delegate->addMember(d); }
  bool lookupMember(const char * ident, DefnList & defs, bool inherit) const {
    return delegate->lookupMember(ident, defs, inherit);
  }

  bool allowOverloads() { return delegate->allowOverloads(); }
  Expr * getBaseExpr() { return delegate->getBaseExpr(); }
  void dumpHierarchy(bool full = true) const { delegate->dumpHierarchy(); }
};

typedef llvm::SmallVector<LocalScope *, 4> LocalScopeList;

} // namespace tart

#endif
