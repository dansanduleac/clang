#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/AST/StmtVisitor.h"
#include "clang/AST/AST.h"
#include "clang/Frontend/CompilerInstance.h"

#include "clang/Rewrite/Rewriter.h"
// This is in "${CLANG_SOURCE_DIR}/lib", mind you.
#include "Sema/TreeTransform.h"

#include "ClangUtils.h"

#include "llvm/Support/raw_ostream.h"

#include <sstream>
#include <memory>

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


class MyTreeTransform
  : public TreeTransform<MyTreeTransform> {
public:

};


class AnnotateVariablesVisitor
  : public RecursiveASTVisitor<AnnotateVariablesVisitor> {
public:
  explicit AnnotateVariablesVisitor(ASTContext* Context, Sema& SemaRef,
      Rewriter* Rewriter)
    : Context(Context), SemaRef(SemaRef), Rewriter(Rewriter) {}

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
  void warnLoc(T* X, StringRef s) {
    DiagnosticsEngine &D = Context->getDiagnostics();

    unsigned DiagID = D.getCustomDiagID(DiagnosticsEngine::Warning, s);
    D.Report(X->getLocStart(), DiagID).AddSourceRange(CharSourceRange(X->getSourceRange(), true));
  }

  // VISITORS
  // -------------------------------------------------

  // TODO what about int *x = new int;   or even with a parameter

  bool VisitVarDecl(VarDecl* VD) {
    raw_ostream &e = llvm::errs();
    if (DEBUG) {
      //(e.changeColor(yellow, true) << "VarDecl").resetColor()
      e << yellow << "VarDecl" << normal
        << " at " << printLoc(VD) << ": \""
        << VD->getName() << "\" [ ";
      VD->dump();
      e << " ]\n";
    }
    if (VD->hasAttr<AnnotateAttr>()) {
      /*
      for (specific_attr_iterator<AnnotateAttr>
           ai = VD->specific_attr_begin<AnnotateAttr>(),
           ae = VD->specific_attr_end<AnnotateAttr>(); ai != ae; ++ai) {
      */

      // Assign a unique ID.
      AnnotateAttr* attr = VD->getAttr<AnnotateAttr>();
      

      for (auto ai : VD->getAttrs()) {
        auto aa = dyn_cast<AnnotateAttr>(ai);
        if (aa == NULL) {
          continue;
        }
        if (DEBUG) {
          e << yellow << "Annotation: " << normal <<  aa->getAnnotation() << "\n";
        }
        //
      }
      // check for "monotonic"
      // TODO give it an unique translation unit id at this point.
        // well, arguably it already has that.
    }
    return true;
  }

  /* TODO
   * maybe also check DeclStmt which sets the initial value... We need to take
   * care of that case. Or might that be just BinaryOperator
   * (if x => x.isAssignmentOperator()) ?
   */


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
  bool VisitExpr(Expr* E) {
    auto &e = llvm::errs();
    //NamedDecl* orig = dre->getFoundDecl();
    //ValueDecl* me = dre->getDecl();
    // TODO: mark this dre->getDecl() with the attributes of dre->getFoundDecl().
    if (DEBUG) {
      e << yellow << ">>> Expr" << normal << " at " << printLoc(E) << " ";
      E->dump();
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
    /*
    DeclContext* dc = VD->getDeclContext();
    DeclContextLookupResult lr = dc->lookup(VD->getDeclName());
    e << "Looked up this decl at: " << "\n";
    for (auto I = lr.first, E = lr.second; I != E; ++I) {
      NamedDecl* nd = *I;
      e << printLoc(nd) << "\n";
    }
    e << "\n";
     */
  }

  // TODO
  // DeclStmt --- statement that includes a declaration
  // Check: is this always initialisation?


  // Use this to extract any DeclRefExpr from lvalues that we assign to
  //   (or that we pass to functions).
  class StmtDREExtractor : StmtVisitor<StmtDREExtractor> {
    ASTContext& Context;
  public:
    StmtDREExtractor(ASTContext& C) : Context(C) {}
    // For now, only allow 0 or 1 levels of indirection.

  };

  bool VisitBinaryOperator(BinaryOperator* bo) {
    if (bo->isAssignmentOp()) {
      Expr* lhs = bo->getLHS();

      // TODO: Perform some checks on LHS: Pry out the DeclRefExpr
      // and going there to check for initial annotation.
      // How to handle cases like *a = x;
      //  ---> use a StmtVisitor?

      // lhs's level of derefs must match the DeclRefExpr's level of
      // references.


      auto sourceRange = bo->getSourceRange();
      // Possibly: use ClangUtils.h, getStmtRangeWithSemicolon,
      // if we want a sourceRange including semicolon.

      const clang::Attr* attrs[] = {
        new (*Context) AnnotateAttr(sourceRange, *Context, "blabla")
      };

      AttributedStmt* wrapper = AttributedStmt::Create(
          *Context, bo->getExprLoc(), attrs, bo);

      if (DEBUG) {
        llvm::errs() << magenta << "Wrapped Text: " << normal;
        //wrapper->dump();
        // Get the new text.
          //bo->dumpPretty(*Context);
        llvm::errs() << Rewriter->ConvertToString(bo);
        llvm::errs() << "\n";
        warnLoc(bo, "operator=");
        llvm::errs() << magenta << "Wrapper: " << normal;
          //wrapper->dumpPretty(*Context);
        llvm::errs() << Rewriter->ConvertToString(wrapper);
        llvm::errs() << "\n";
      }
      // Use Rewriter::ReplaceStmt to replace this with an AttributedStmt.
      Rewriter->ReplaceStmt(bo, wrapper);
      // Rewriter->InsertTextAfter(lhs->getLocEnd(), "/* whats up */");
      
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
