#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/EvaluatedExprVisitor.h"
#include "clang/AST/AST.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Rewrite/Rewriter.h"
// This is in "${CLANG_SOURCE_DIR}/lib", mind you.
#include "Sema/TreeTransform.h"

#include "ClangUtils.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/raw_ostream.h"

#include <sstream>
#include <memory>
#include <new>

using namespace clang;
#if LLVM_USE_RVALUE_REFERENCES
using std::move;
using std::unique_ptr;
#else
#pragma error("Nooooo! No rvalue references.")
#endif

namespace {

bool DEBUG = false;

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

// TODO when we figure out how to get ahold of the Sema before it gets
// reset by CompilerInstance (PluginASTAction == too late already).
// If I remember correctly, derive ASTFrontendAction and save the Sema.

class MyTreeTransform
  : public TreeTransform<MyTreeTransform> {
public:

};




const llvm::StringRef GLOBAL_PREFIX = "assertion";


class AnnotateVariablesVisitor
  : public RecursiveASTVisitor<AnnotateVariablesVisitor> {

public:
  explicit AnnotateVariablesVisitor(ASTContext* Context, Sema& SemaRef,
      Rewriter* Rewriter)
    : Context(Context), SemaRef(SemaRef), Rewriter(Rewriter) {}

  // Convenience stuff, only needs Context.
  // --------------------------------------
  // TODO: move out of this class

  template <typename T>
  std::string printLoc(T* D) {
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

  template <typename T>
  void warnAt(T* X, StringRef s) {
    diagnosticAt(X, DiagnosticsEngine::Warning, s);
  }


  template <typename T>
  static SourceRange getSourceRange(T* X, const llvm::false_type&) {
    return X->getSourceRange();
  }
  
  static SourceRange getSourceRange(Attr* X, const llvm::true_type&) {
    return X->getRange();
  }

  template <bool Cond>
  struct as_truth : llvm::conditional<Cond, llvm::true_type, llvm::false_type> {};

  template <typename T>
  static SourceRange getSourceRange(T* X) {
    typename as_truth<llvm::is_base_of<Attr,T>::value>::type choice;
    return getSourceRange(X, choice);
  }

  // For convenience, to highlight an AST node (of type T).
  template <typename T>
  void diagnosticAt(T* X, DiagnosticsEngine::Level lvl, StringRef str) {
    // All AST node ranges are token ranges from what I know.
    diagnosticAt(CharSourceRange::getTokenRange(getSourceRange(X)), lvl, str);
  }

  void diagnosticAt(CharSourceRange csr, DiagnosticsEngine::Level lvl,
                    StringRef str) {
    DiagnosticsEngine &D = Context->getDiagnostics();
    unsigned DiagID = D.getCustomDiagID(lvl, str);
    D.Report(csr.getBegin(), DiagID).AddSourceRange(csr);
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

private:
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
  llvm::DenseMap<AssertionAttr*, llvm::SmallVector<StringRef,4>> annotations;

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
  void ReplaceAssertionAttr(DeclStmt* DS, Decl* D, llvm::StringRef newS) {
    AssertionAttr* attr = D->getAttr<AssertionAttr>();
    assert(attr && "Tried to replace AssertionAttr for Decl which has none");
    SourceRange range = attr->getRange();
    D->dropAttr<AssertionAttr>();
    // Using placement new. Why allocate another copy when we can reuse.
    // ASTContext will deallocate it at the end of things. 
    attr = ::new(attr) AssertionAttr(range, *Context, newS);

    AddAssertionAttr(DS, D, attr);
  }

  // Adds UID to the attribute.
  void QualifyAttr(DeclStmt* DS, Decl* D, AssertionAttr* attr) {
    // TODO a map or smth to hold the "split" state of each Annotation.
    // For now, append directly to the string.
    SmallString<20> an = attr->getAnnotation();
    an.append(",");
    llvm::raw_svector_ostream S(an);
    S << uid++;
    ReplaceAssertionAttr(DS, D, S.str());
    if (DEBUG) {
      llvm::errs() << yellow << "Qualified: " << normal <<  S.str() << "\n";
    }
  }

public:
  // VISITORS
  // -------------------------------------------------

  // TODO what about int *x = new int;   or even with a parameter

  // Visit DeclStmt instead of VarDecl, because we want the Stmt itself to replace
  // in the Rewriter.
  bool VisitDeclStmt(DeclStmt* DS) {
    //warnAt(DS, "DeclStmt");
    Decl* D;
    if (DS->isSingleDecl()) {
      D = DS->getSingleDecl();
    } else {
      assert(false && "Not implemented.");
    }

    // TODO foreach decl that is a VarDecl (if there are multiple)

    if (VarDecl* VD = dyn_cast<VarDecl>(D)) {
      raw_ostream &e = llvm::errs();
      if (DEBUG) {
        //(e.changeColor(yellow, true) << "VarDecl").resetColor()
        e << yellow << "VarDecl" << normal
          << " at " << printLoc(VD) << ": \""
          << VD->getName() << "\" [ ";
        VD->dump();
        e << " ]\n";
      }
      
      AssertionAttr* attr = getAssertionAttr(VD);

      if (attr) {
        // Assign a unique ID.
        // Pass DS so that the Rewriter has something to replace.
        QualifyAttr(DS, VD, attr);
      }
      // Deal with initialisation ("assertion stealing").
      // Conditions:
      //  1) VD->getType() is a  "T " + one or more "*const",
      //      i.e. int *const *const.

      //  2) (Expr representing the "T" in the initializer)->getFoundDecl() has
      //  to be asserted. (this one's the same as for the AssignmentOp case)
      //     TODO move logic for (2) in a separate function / class that
      //     somewhat emulates StmtVisitor.
      if (Expr* Init = VD->getInit()) {
        ExprDREExtractor extractor(*Context, *this, Init);
        extractor.run();

        if (extractor.found()) {
          // Raise an error if we found a reference to another assertion,
          // but we already have an AssertionAttr on our back.
          // This behaviour might change in the future.
          if (attr) {
            diagnosticAt(attr, DiagnosticsEngine::Error,
              "This VarDecl's initialisation points to another asserted "
              " variable, but it already carries the shown assertion.");
          }
          attr = extractor.attr;
          // TODO We have to take other things into account like indirection,
          // other info...
          AddAssertionAttr(DS, VD, attr);
        }
      }
    }
    return true;
  }


  // TODO
  // TODO
  // TODO: really!
  // TODO: We probably don't need this here. Put this in a special
  // StmtVisitor that analyses LeftHandSides (of assignment operators),
  // as well as arguments in function calls.
  bool VisitUnaryOperator(UnaryOperator* E) {
    Expr* SE = E->getSubExpr();
    DeclRefExpr* DRE = dyn_cast<DeclRefExpr>(SE);
    Decl* orig;
    // We're looking to propagate the attrs of SE to E (if any).
    // PROBLEM is that visitor first visits children, then parent...?
    if (!DRE || ! (orig = DRE->getFoundDecl())->hasAttrs()) {
      return true;
    }
    // getOpcode() -> UO_AddrOf | UO_Deref
    if (E->getOpcode() == UO_AddrOf) {
      // TODO
      // Tag E with tag of SE + 1. 
      // In reality, tag the statements with all the required information.
    } else if (E->getOpcode() == UO_Deref) {
      // Tag 
    }
    return true;
  }


  /*
  // This is for some serious debugging, basically to show the entire Stmt
  // class hierarchy of each Stmt.
  bool VisitStmt(Stmt* S) {
    auto &e = llvm::errs();
    //NamedDecl* orig = dre->getFoundDecl();
    //ValueDecl* me = dre->getDecl();
    // TODO: mark this dre->getDecl() with the attributes of dre->getFoundDecl().
    if (DEBUG) {
      e << yellow << ">>> Stmt" << normal << " at " << printLoc(S) << " ";
      S->dump();
        // ": \""
        // << dre->getDecl()->getName() << "\" " << blue << "referencing " << normal
        // << printLoc(orig) << " ";
    }
    return true;
  }
  */

  // TODO 
  // Maybe not necessary to do this here. Should use such a function within
  // StmtDREExtractor?
  
  /*
  bool VisitDeclRefExpr(DeclRefExpr* dre) {
    auto &e = llvm::errs();
    NamedDecl* orig = dre->getFoundDecl();
    ValueDecl* me = dre->getDecl();
    // TODO: mark this dre->getDecl() with the attributes of dre->getFoundDecl().
    if (DEBUG) {
      e << yellow << "DeclRefExpr" << normal << " at " << printLoc(dre) << ": \""
        << dre->getDecl()->getName() << "\" " << blue << "referencing " << normal
        << printLoc(orig) << " ";
      dre->dump();
      e << magenta << "   original: " << normal;
      orig->dump();
      e << normal;
      // Attrs
      if (orig->hasAttr<AnnotateAttr>()) {
        auto an = orig->getAttr<AnnotateAttr>();
        e << magenta << " having " << normal;
        e << an->getAnnotation() << " ";
      }
      e << "\n";
    }

    return true;
    
    // DeclContext* dc = VD->getDeclContext();
    // DeclContextLookupResult lr = dc->lookup(VD->getDeclName());
    // e << "Looked up this decl at: " << "\n";
    // for (auto I = lr.first, E = lr.second; I != E; ++I) {
    //   NamedDecl* nd = *I;
    //   e << printLoc(nd) << "\n";
    // }
    // e << "\n"; 
  }
  */

  class ReferencingExprExtractor {

  };

  // Use this to extract any DeclRefExpr from lvalues that we assign to
  //   (or that we pass to functions).
  class ExprDREExtractor : public EvaluatedExprVisitor<ExprDREExtractor> {
    AnnotateVariablesVisitor& Visitor;
    Expr* toVisit;
  public:
    // We need Visitor ref to use warnAt, or any other stuff that depends
    // on the ASTContext..
    ExprDREExtractor(ASTContext& C, AnnotateVariablesVisitor& Visitor,
                     Expr* toVisit)
      : EvaluatedExprVisitor<ExprDREExtractor>(C),
        Visitor(Visitor),
        toVisit(toVisit) {}
    // For pointers, should only work if the pointer is const,
    // so, in regex terms, "T (\*const)*".

    // This has a problem if we have some weird lvalue like:  *(A + B) ..
    // TODO: how do we enforce that this Expr is a combination of
    // UnaryOperators applied to a DeclRefExpr?

    // Yup definitely needs to be more tight...
    // Therefore: 


    // TODO reimplement void Visit(Expr* E), to not allow visitation unless
    // it's following our required pattern.

    bool found() {
      return dre != NULL;
    }

    void VisitDeclRefExpr(DeclRefExpr* DRE) {
      NamedDecl* orig = DRE->getFoundDecl();
      //Visitor.warnAt(orig, "DRE referenced this object");
      if ((attr = Visitor.getAssertionAttr(orig))) {
        if (dre != NULL) {
          success = false;
          return;
        } 
        dre = DRE;

        // Some debugging on the type...
        std::string S; llvm::raw_string_ostream os(S);
        clang::QualType tt = DRE->getType();
        os << "DRE->getType() == " << tt.getAsString();
        Visitor.warnAt(DRE, os.str());
      }
    }

    void run() {
      Visit(toVisit);
      if (!success) {
        Visitor.
        diagnosticAt(toVisit, DiagnosticsEngine::Fatal,
                     "Found more than 1 DRE inside this Expr");
      }
    }

    DeclRefExpr* dre = NULL;
    AssertionAttr* attr = NULL;
    int indirection = 0;
    bool success = true;
  };

  bool VisitBinaryOperator(BinaryOperator* bo) {
    if (bo->isAssignmentOp()) {
      Expr* lhs = bo->getLHS();

      // lhs's level of derefs must match the DeclRefExpr's level of
      // references.

      auto sourceRange = bo->getSourceRange();
      // Possibly: use ClangUtils.h, getStmtRangeWithSemicolon,
      // if we want a sourceRange including semicolon.

      ExprDREExtractor extractor(*Context, *this, lhs);
      // why does VisitStmt sometimes not find the DRE....
      extractor.run();
      
      // Maybe: use type of lhs vs type of DeclRefExpr ...

      if (!extractor.found()) {
        return true;
      }

      const clang::Attr* attrs[] = {
        // TODO this shouldn't call getAnnotation() directly,
        // should be some intermediary doing the AssertionAttr cloning
        // (with a different indirection info too).
        new (*Context) AnnotateAttr(sourceRange, *Context, 
                                    extractor.attr->getAnnotation())
      };

      AttributedStmt* wrapper = AttributedStmt::Create(
          *Context, bo->getExprLoc(), attrs, bo);

      if (DEBUG) {
        llvm::errs() << magenta << "Wrapped Text: " << normal;
        // Get the new text.
        llvm::errs() << Rewriter->ConvertToString(bo);
        llvm::errs() << "\n";
        warnAt(bo, "operator=");
        llvm::errs() << magenta << "Wrapper: " << normal;
        llvm::errs() << Rewriter->ConvertToString(wrapper);
        llvm::errs() << "\n";

        llvm::errs() << red << "Actually replacing:\n" << normal;
        int Size = Rewriter->getRangeSize(
          CharSourceRange::getTokenRange(bo->getSourceRange()));
        /*
        diagnosticAt(CharSourceRange::getCharRange(
                        bo->getLocStart(),
                        bo->getLocStart().getLocWithOffset(Size)),
                     DiagnosticsEngine::Warning,
                     "<--- this");
        */
        warnAt(bo, "<--- this");
      }
      // Use Rewriter::ReplaceStmt to replace this with an AttributedStmt.
      Rewriter->ReplaceStmt(bo, wrapper);      
    }
    return true;
  }

  // Needed for figuring out if any arguments are tagged.
  // TODO figure out how, or do it in DeclRefExpr ?
  //
  // "Note that since WalkUpFromFoo() calls WalkUpFromBar() (where Bar is Foo's
  // super class) before calling VisitFoo(), the result is that the Visit*()
  // methods for a given node are called in the top-down order (e.g. for a node
  // of type NamedDecl, the order will be VisitDecl(), VisitNamedDecl(), and
  // then VisitNamespaceDecl())."
  // ==> may be the case that we can only do that analysis in DeclRefExpr.

  bool VisitCallExpr(CallExpr *Call) {
    if (FunctionDecl *Callee = Call->getDirectCallee()) {
      if (DEBUG) {
        llvm::errs() << yellow << "Call to " << normal
          << Callee->getNameAsString()
          << "\n";
          
      }
    }
    return true;
  }

private:
  ASTContext* Context;
  // FIXME Are we using this Sema?
  Sema& SemaRef;
  Rewriter* Rewriter;
};

class AnnotateVariablesConsumer : public ASTConsumer {
public:
  explicit AnnotateVariablesConsumer(ASTContext* Context,
      Sema& SemaRef, unique_ptr<Rewriter>&& rewriter)
    : Rewriter(move(rewriter)), SemaRef(SemaRef),
      Visitor(Context, SemaRef, Rewriter.get()) {}

  virtual void HandleTranslationUnit(clang::ASTContext &Context) {
    // Traversing the translation unit decl via a RecursiveASTVisitor
    // will visit all nodes in the AST.
    Visitor.TraverseDecl(Context.getTranslationUnitDecl());

    // Print out the rewritten contents.
    const RewriteBuffer *RewriteBuf =
        Rewriter->getRewriteBufferFor(Context.getSourceManager().getMainFileID());
    llvm::outs() << std::string(RewriteBuf->begin(), RewriteBuf->end());

    //Rewriter->overwriteChangedFiles();
  }
private:
  unique_ptr<Rewriter> Rewriter;
  Sema& SemaRef;
  // A RecursiveASTVisitor implementation.
  AnnotateVariablesVisitor Visitor;

  /*
  virtual bool HandleTopLevelDecl(DeclGroupRef DG) {
    for (DeclGroupRef::iterator i = DG.begin(), e = DG.end(); i != e; ++i) {
      const Decl *D = *i;
      if (const NamedDecl *ND = dyn_cast<NamedDecl>(D))
        llvm::errs() << "top-level-decl: \"" << ND->getNameAsString() << "\"\n";
    }

    return true;
  }
  */
};

class AnnotateVariablesAction : public PluginASTAction {
protected:

  ASTConsumer *CreateASTConsumer(CompilerInstance &CI, llvm::StringRef) {
    auto Rew = unique_ptr<Rewriter>(new Rewriter());
    Rew->setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    // CI.hasSema() == 0 :(
    Sema* no_sema = 0;
    return new AnnotateVariablesConsumer(&CI.getASTContext(),
      *no_sema, move(Rew));
  }

  bool ParseArgs(const CompilerInstance &CI,
                 const std::vector<std::string>& args) {
    for (unsigned i = 0, e = args.size(); i != e; ++i) {
      llvm::errs() << "AnnotateVariables arg = " << args[i] << "\n";

      if (args[i] == "-d" || args[i] == "--debug") {
        DEBUG = true;
      }
      // Example error handling.
      if (args[i] == "-an-error") {
        DiagnosticsEngine &D = CI.getDiagnostics();
        unsigned DiagID = D.getCustomDiagID(
          DiagnosticsEngine::Error, "invalid argument '" + args[i] + "'");
        D.Report(DiagID);
        return false;
      }
    }
    if (args.size() && args[0] == "help")
      PrintHelp(llvm::errs());

    return true;
  }

  void PrintHelp(llvm::raw_ostream& ros) {
    ros << "Help for AnnotateVariables plugin goes here\n";
  }

};

}

static FrontendPluginRegistry::Add<AnnotateVariablesAction>
X("annotate-vars", "Try to obtain the transitive closure of variable annotations over variables being assigned to.");

// vim:sw=2
