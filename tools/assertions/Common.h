#ifndef ANNOTATEVARIABLES_COMMON_H
#define ANNOTATEVARIABLES_COMMON_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"

#include "clang/AST/AST.h"
#include "clang/Rewrite/Rewriter.h"

#include <new>
#include <sstream>

namespace assertions {

using namespace clang;

extern bool DEBUG;

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


const llvm::StringRef GLOBAL_PREFIX = "assertion";

class Common {
  ASTContext* Context;
  Rewriter* Rewriter;

public:
  Common(ASTContext* C, class Rewriter* Rewriter)
    : Context(C), Rewriter(Rewriter) { }

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


  // MY CUSTOM ATTRIBUTE
  // -------------------------------------------------
  // TODO:
  // Update these to actually use a custom attribute, not just AnnotateAttr,
  // because that's quite inflexible.

  typedef AnnotateAttr AssertionAttr;

  enum Assertion {
    Initial,
    Qualified
  };

  // There will be a different Visitor for each translation unit, but we do
  // not need globally unique UIDs. Each translation unit can have its own set
  // of UIDs, but the programmer will have to be consistent with assertion
  // annotations.
  // TODO
  // If such an annotated variable is passed to an un-annotated, or differently
  // annotated function, a warning will be issued.
  int uid = 1;

  // Cache for our unpacked attribute data, in this case the getAnnotation() split
  // by the separator.
  // TODO need  define DenseMapInfo for AssertionAttr*
  // llvm::DenseMap<AssertionAttr*, llvm::SmallVector<StringRef,4>> annotations;

  // Extract the valid AssertionAttr, if any.

  template <typename T>
  static AssertionAttr* getAssertionAttr(T* obj) {
    AssertionAttr* attr = obj->template getAttr<AssertionAttr>();
    return attr && IsSaneAssertionAttr(attr) ? attr : NULL;
  }

  template <typename T>
  static bool hasAssertionAttr(T* obj) {
    return getAssertionAttr(obj) != NULL;
  }

  // To check if this Attr is in a sane state, representing a correct AssertionAttr.
  // Using it here since we're emulating on top of AnnotateAttr, and not annotations
  // are valid in our context.
  // TODO: deprecate after we implement a separate Attr.
  static bool IsSaneAssertionAttr(const AssertionAttr* attr) {
    return attr->getAnnotation().startswith(GLOBAL_PREFIX);
  }

  void AddAssertionAttr(DeclStmt* DS, Decl* D, AssertionAttr* attr) {
    D->addAttr(attr);

    // Rewrite the source code.

    // Obtain the replacement string, Str (by printPretty'ing the Stmt).
    std::string Str = Rewriter->ConvertToString(DS);
    if (DEBUG) {
      llvm::errs() << red << "Rewriting DeclStmt: " << normal << Str << "\n";
      warnAt(DS, " <--- old DeclStmt");
    }

    // TODO just rewrite the Decl... not the entire Stmt

    SourceManager& SM = Context->getSourceManager();
    SourceLocation SLoc, ELoc;
    // Confusingly enough, Stmt had getLoc(Start|End), but DeclStmt defines
    // get(Start|End)Loc() as they're passed to DeclStmt in the constructor.

    // Can't just use Rewriter->ReplaceStmt() because Rewriter->isRewritable(
    // DS->getStartLoc()) == false, if it's a macro expansion. So get location
    // where expansion occurred (which will be in the source code).
    SLoc = SM.getExpansionLoc(DS->getStartLoc());
    ELoc = SM.getExpansionLoc(DS->getEndLoc());

    //diagnosticAt(CharSourceRange::getTokenRange(SourceRange(SLoc, ELoc)),
    //  DiagnosticsEngine::Warning, "ExpandedLocation");
    int Size = Rewriter->getRangeSize(SourceRange(SLoc, ELoc));
    /*
    llvm::errs() << "Rewriter->isRewritable(...) == "
                 << Rewriter->isRewritable(SLoc) << "\n";
    llvm::errs() << "Size = " << Size << "\n";
    */

    // TOTO this is a problem if further down we rewrite the initialiser
    // contained in this DeclExpr as well...
    // How does it figure out the new position of the initialiser btw....
    Rewriter->ReplaceText(SLoc, Size, Str);
  }

  // TODO change to use something more high-level than a string (as the new
  // value). DeclStmt used only for Rewriter to have a Stmt to replace
  // (otherwise we could have done this in VarDecl.
  /*
  void ReplaceAssertionAttr(DeclStmt* DS, Decl* D, llvm::StringRef newS) {
    AssertionAttr* attr = D->getAttr<AssertionAttr>();
    assert(attr && "Tried to replace AssertionAttr for Decl which has none");
    D->dropAttr<AssertionAttr>();
    // Using placement new. Why allocate another copy when we can reuse.
    // ASTContext will deallocate it at the end of things.
    attr = ::new(attr) AssertionAttr(range, *Context, newS);

    AddAssertionAttr(DS, D, attr);
  }
  */

  // Adds UID to the attribute, by replacing the original attribute object.
  void QualifyDeclInPlace(DeclStmt* DS, Decl* D, AssertionAttr* attr) {
    assert(IsSaneAssertionAttr(attr) &&
      "Tried to replace AssertionAttr for Decl which has none");
    D->dropAttr<AssertionAttr>();
    attr = QualifyAttrReplace(attr);
    AddAssertionAttr(DS, D, attr);
    if (DEBUG) {
      llvm::errs() << yellow << "Qualified: " << normal
                   << attr->getAnnotation() << "\n";
    }
  }

  // Just returns a string that represents the qualified AnnotateAttr.
  //
  std::string GetQualifiedAttrString(AnnotateAttr* attr) {
    SmallString<20> an = attr->getAnnotation();
    an.append(",");
    llvm::raw_svector_ostream S(an);
    S << uid++;
    return S.str();
  }

  // Qualifies an existing attr like QualifyAttr, but replaces the original
  // attr.
  AssertionAttr* QualifyAttrReplace(AssertionAttr* attr) {
    return ::new(attr) AssertionAttr(attr->getRange(), *Context,
                                     GetQualifiedAttrString(attr));
  }

  // Return a new AssertionAttr representing the qualified attr.
  // Does NOT check whether attr already qualified, or not sane.
  AssertionAttr* QualifyAttr(AssertionAttr* attr) {
    return new(*Context) AnnotateAttr(attr->getRange(), *Context,
                                      GetQualifiedAttrString(attr));
  }

  bool IsSameAssertion(AssertionAttr* a1, AssertionAttr* a2) {
    if (a1 == nullptr || a2 == nullptr) {
      return a1 == nullptr && a2 == nullptr;
    }
    SmallVector<StringRef, 3> Arr1, Arr2;
    // Only compare the first 2 elements of each.
    a1->getAnnotation().split(Arr1, ",", 2); Arr1.resize(2);
    a2->getAnnotation().split(Arr2, ",", 2); Arr2.resize(2);
    return Arr1 == Arr2;
  }

  StringRef AssertionKindAsString(AssertionAttr* attr) {
    SmallVector<StringRef, 3> Arr;
    attr->getAnnotation().split(Arr, ",", 2);
    assert(Arr[0] == "assertion");
    return Arr[1];
  }
};

// And I really want this to be available outside:
typedef Common::AssertionAttr AssertionAttr;

}

namespace llvm {

namespace dont_use {
  template<typename To> char convertible_to_helper(const volatile To);
  template<typename To> double convertible_to_helper(...);
}

template <typename From, typename To>
struct is_convertible_to {
  static const bool value
    = sizeof(char) == sizeof(dont_use::convertible_to_helper<To>( *(From*)0 ));
};

//     typename llvm::enable_if<is_convertible_to<ArrT, ArrayRef<T>>, int>::type ignore=0
// template <typename ArrT>
// std::string join(ArrT Strings, StringRef separator) {
//   return _join(Strings, separator);

template <typename T>
typename llvm::enable_if<is_convertible_to<T, StringRef>,
                         std::string>::type
join(ArrayRef<T> Strings, StringRef separator) {
  std::ostringstream Out;
  bool start = true;
  for (const T& String : Strings) {
    if (start)
      start = false;
    else
      Out << separator.str();
    Out << (std::string) String;
  }
  return Out.str();
}

template <class T>
std::string join(T Strings, StringRef separator) {
  return join(makeArrayRef(Strings), separator);
}

}

#endif