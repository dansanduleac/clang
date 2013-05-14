#ifndef ANNOTATEVARIABLES_COMMON_H
#define ANNOTATEVARIABLES_COMMON_H

#include "Assertion.h"
#include "Flags.h"

#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"

#include "clang/AST/AST.h"

#include <new>
#include <sstream>

namespace assertions {

using namespace clang;

struct Color {
  raw_ostream::Colors color;
  bool bold;

  Color(raw_ostream::Colors col, bool bol = true) : color(col), bold(bol) {}
};

struct Normal {} normal;

raw_ostream& operator<<(raw_ostream& OS, const Color& c) {
  return OS.changeColor(c.color, c.bold);
}

raw_ostream& operator<<(raw_ostream& OS, const Normal&) {
  return OS.resetColor();
}

static const Color green = raw_ostream::GREEN;
static const Color blue = raw_ostream::BLUE;
static const Color red = raw_ostream::RED;
static const Color magenta = raw_ostream::MAGENTA;
static const Color yellow = raw_ostream::YELLOW;
static const Color savedColor = raw_ostream::SAVEDCOLOR;


class Common {
  ASTContext* Context;

public:
  Common(ASTContext* C)
    : Context(C) { }

  ASTContext* getContext() { return Context; }

  // Convenience functions for debugging and diagnostics.
  // ----------------------------------------------------

  template <typename T>
  std::string printLocInternal(T* D) {
    FullSourceLoc FullLocation = Context->getFullLoc(D->getLocStart());
    if (FullLocation.isValid()) {
      std::ostringstream ss;
      ss << FullLocation.getSpellingLineNumber() << ":"
         << FullLocation.getSpellingColumnNumber();
      return ss.str();
    } else {
      return "[INVALID LOCATION]";
    }
  }

  std::string printLoc(Stmt* S) {
    return printLocInternal(S);
  }

  std::string printLoc(Decl* D) {
    return printLocInternal(D);
  }

  template <typename T>
  DiagnosticBuilder warnAt(T* X, StringRef s) {
    return diagnosticAt(X, s, DiagnosticsEngine::Warning);
  }


  template <typename T>
  static SourceRange getSourceRange(T&& X, const llvm::false_type&) {
    return X.getSourceRange();
  }

  static SourceRange getSourceRange(Attr&& X, const llvm::true_type&) {
    return X.getRange();
  }

  // Get the implicit SourceRange for the token pointed to by this
  // SourceLocation.
  static SourceRange getSourceRange(SourceLocation L) {
    return L;
  }

  template <bool Cond>
  struct as_truth : llvm::conditional<Cond, llvm::true_type, llvm::false_type> {};

  template <typename T>
  static SourceRange getSourceRange(T* X) {
    typename as_truth<llvm::is_base_of<Attr,T>::value>::type choice;
    return getSourceRange(std::move(*X), choice);
  }

  // For exact types being sent.
  template <typename T>
  static SourceRange getSourceRange(T&& X) {
    typename as_truth<llvm::is_base_of<Attr, llvm::remove_reference<T>>::value>::type choice;
    return getSourceRange(X, choice);
  }

  // For convenience, to highlight an AST node (of type T).
  template <typename T>
  DiagnosticBuilder
  diagnosticAt(T&& X, StringRef str,
               DiagnosticsEngine::Level lvl = DiagnosticsEngine::Error) {
    // All AST node ranges are token ranges from what I know.
    // TODO apart from the SourceLocation needed, we can just << X
    // everything... (making the below function obsolete-ish..)
    return diagnosticAt(CharSourceRange::getTokenRange(getSourceRange(X)),
                        str, lvl);
  }

  DiagnosticBuilder
  diagnosticAt(CharSourceRange csr, StringRef str,
               DiagnosticsEngine::Level lvl) {
    DiagnosticsEngine &D = Context->getDiagnostics();
    unsigned DiagID = D.getCustomDiagID(lvl, str);
    // << does .AddSourceRange() too!
    return D.Report(csr.getBegin(), DiagID) << csr;
  }

// === Assertion related stuff =================================

  // Map holding the function parameter assertions for pointer params. We
  // don't want to allow them to remain attached to the ParmVarDecls because
  // then they get codegen'd, and we only want to codegen annotations where
  // we need to initialize state.
  llvm::DenseMap<ParmVarDecl *, AssertionAttr *> ParmAttrMap;

  AssertionManager AM;

  /// Extract the valid AssertionAttr, if any.
  AssertionAttr *getAssertionAttr(Decl *D) {
    auto attr = AM.getAssertionAttr(D);
    if (!attr && isa<ParmVarDecl>(D)) {
      // Retrieve it from the ParmAttrMap.
      attr = ParmAttrMap.lookup(cast<ParmVarDecl>(D));
    }
    return attr;
  }

  /// Qualifies an existing attr with an unique ID like QualifyAttr, but
  /// replaces the original attr in memory.
  /// @return the UID for convenience, no point returning same address.
  int QualifyAttrReplace(AssertionAttr* attr) {
    (void) ::new(attr) AssertionAttr(attr->getRange(), *Context,
                                     AM.GetQualifiedAttrString(attr));
    return AM.getUID();
  }

  // Return a new AssertionAttr representing the qualified attr.
  // Does NOT check whether attr already qualified, or not sane.
  AssertionAttr* QualifyAttr(AssertionAttr* attr) {
    return new(*Context) AnnotateAttr(attr->getRange(), *Context,
                                      AM.GetQualifiedAttrString(attr));
  }
};

}

#endif