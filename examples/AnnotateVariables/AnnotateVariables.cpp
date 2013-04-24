#include "clang/AST/AST.h"
#include "clang/AST/ASTMutationListener.h" // temporary?
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/ASTConsumers.h"             // for CreateASTPrinter
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Rewrite/Rewriter.h"
#include "clang/Sema/SemaConsumer.h"
// This is in "${CLANG_SOURCE_DIR}/lib", mind you.
#include "Sema/TreeTransform.h"

#include "ClangUtils.h"
#include "Common.h"
#include "ReferenceExprExtractor.h"

#include <memory>

using namespace clang;
#if LLVM_USE_RVALUE_REFERENCES
using std::move;
using std::unique_ptr;
#else
#pragma error("Nooooo! No rvalue references.")
#endif

namespace {

// TODO useful at all?
class MyMutationListener : public ASTMutationListener {
  void AddedVisibleDecl(const DeclContext *DC, const Decl *D) override {
    llvm::errs() << "AddedVisibleDecl: ";
    D->dump();
  }
};


class MyTreeTransform : public TreeTransform<MyTreeTransform> {
  typedef TreeTransform<MyTreeTransform> Base;

  Common& Co;

public:
  MyTreeTransform(Common& Common, Sema& S)
    : Base(S), Co(Common) { }

  // FIXME needed? Do we actually need to?
  bool AlwaysRebuild() { return true; }

  // Do not need this after all? We will only transform statements
  /*
  Decl* TransformDeclContextHelper(DeclContext* DC) {
    if (!DC)
      return 0;

    for (DeclContext::decl_iterator Child = DC->decls_begin(),
             ChildEnd = DC->decls_end();
         Child != ChildEnd; ++Child) {
      // BlockDecls are traversed through BlockExprs.
      if (!isa<BlockDecl>(*Child))
        *Child = TransformDecl(*Child);
    }
    return DC; // any changes actually?
  }
  */

  // TODO
  // Need to figure out whether the transform recurses into blocks
  // 1) a block inside a function. BlockDecl?
  //    apparently traversed through BlockExpr so maybe that can be
  //    recursed through by Stmt rebuilding.
  //    Check TransformBlockExpr ......

  Decl* TransformDecl(SourceLocation Loc, Decl* D) {
    // Try to reimplement DeclContext traversal from RecursiveASTVisitor
    //  RecursiveASTVisitor<Derived>::TraverseDeclContextHelper
    return Base::TransformDecl(Loc, D);
  }

  // Transform Exprs that modify asserted variables by wrapping them
  // inside AttributedStmt.
  /*
  ExprResult TransformExpr(Expr* E) {
    // Normal Case
    if (true) {
      return Base::TransformExpr(E);
    } else {
      StmtResult res = Base::RebuildAttributedStmt( ... );
    }
  }
  */

  /*
  void transformAttrs(Decl *Old, Decl *New) {
    llvm::errs() << "transformAttrs called with: ";
    Old->dump();
    New->dump();
    llvm::errs() << "\n";
    // Guarantees that if it returns an attr, it is sane.
    AssertionAttr* attr = Co.getAssertionAttr(Old);
    if (attr) {
      New->addAttr(Co.QualifyAttr(attr));
    }
  }
  */

  ASTMutationListener *GetASTMutationListener() {
    return Listener;
  }

private:
  ASTMutationListener* Listener = new (*Co.getContext()) MyMutationListener();
};



class AnnotateVariablesVisitor
  : public RecursiveASTVisitor<AnnotateVariablesVisitor> {
  Common& Co;
  ASTContext* Context;
  // We want this to change when consumer gets InitializeSema called on it.
  // Therefore, reference.
  Sema*& SemaPtr;
  Rewriter* Rewriter;
  typedef Common::AssertionAttr AssertionAttr;

public:
  explicit AnnotateVariablesVisitor(Common& C, ASTContext* Context,
    Sema*& SemaPtr, class Rewriter* R)
    : Co(C), Context(Context), SemaPtr(SemaPtr), Rewriter(R) {}

  /*
  bool VisitDecl(Decl* D) {
    llvm::errs() << yellow << D->getDeclKindName() << ":\n" << normal;
    D->dump();
    llvm::errs() << "\n";
    return true;
  }
  */

  /*
  // Has been moved to HandleTopLevelDecl ...
  bool VisitFunctionDecl(FunctionDecl* FD) {
    // (Stmt*) Fd->getBody();  FD->setBody(Stmt*)

    // Decls can go like getContext() => the DeclContext* .
    //  but does decl_iterator allow changing the children?
    // they iterate with Decl::getNextDeclInContext(), 
    // how do I transform a Decl and preserve the order?
    // using llvm::PointerIntPair<Decl *, 2, unsigned> NextInContextAndBits;

    // Stmts have children that apparently can be iterated on
    //   by reference. But can we actually change that?
    // Check out StmtIterator.h 


    // Can't start creating a new CompoundStmt (which is what Body is)
    // when Sema has no function scope. 
    // It will call Sema::PushFunctionScope() which will try to
    // Sema::getCurFunction()..
    // Sema::PushFunctionScope()
    SemaRef.PushFunctionScope();
    StmtResult res = Transform.TransformStmt(FD->getBody());
    //SemaRef.PopFunctionScope();
    if (res.isUsable()) {
      //FD->setBody(res.get());
    } else {
      Co.diagnosticAt(FD, DiagnosticsEngine::Error,
        "Couldn't transform function body.");
    }
    return true;
  }
  */

  // VISITORS
  // -------------------------------------------------

  // TODO what about int *x = new int;   or even with a parameter

  bool VisitVarDecl(VarDecl* VD) {
    // TODO foreach decl that is a VarDecl (if there are multiple)

    raw_ostream &e = llvm::errs();
    if (DEBUG) {
      e << yellow << "VarDecl" << normal
        << " at " << Co.printLoc(VD) << ": \""
        << VD->getName() << "\" [ ";
      VD->dump();
      e << " ]\n";
    }
    
    AssertionAttr* attr = Co.getAssertionAttr(VD);

    // Deal with initialisation ("assertion stealing").
    // Conditions:
    //  1) VD->getType() is a  "T " + one or more "*const",
    //      i.e. int *const *const.

    //  2) (Expr representing the "T" in the initializer)->getFoundDecl() has
    //  to be asserted. (this one's the same as for the AssignmentOp case)
    //     TODO move logic for (2) in a separate function / class that
    //     somewhat emulates StmtVisitor.
    if (Expr* Init = VD->getInit()) {
      ReferenceExprExtractor extractor(*Context, Co, Init);
      extractor.run();

      if (extractor.found()) {
        // Raise an error if we found a reference to another assertion,
        // but we already have an AssertionAttr on our back.
        // This behaviour might change in the future.
        if (attr) {
          Co.diagnosticAt(attr, DiagnosticsEngine::Error,
            "This VarDecl's initialiser points to another asserted "
            "variable, but it already carries the shown assertion.");
        }
        attr = extractor.attr;
        // TODO We have to remember other things into the annotation, like
        // indirection...
        VD->addAttr(attr);
        return true;
      }
    }

    // If we were annotated, and no annotations were inherited from
    // the initializer.
    if (attr) {
      // Assign a unique ID. The Decl will now point to the "new" one
      // because it's been created at the same location.
      Co.QualifyAttrReplace(attr);
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
      e << yellow << ">>> Stmt" << normal << " at " << Co.printLoc(S) << " ";
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

  bool VisitBinaryOperator(BinaryOperator* bo) {
    if (bo->isAssignmentOp()) {
      Expr* lhs = bo->getLHS();

      // lhs's level of derefs must match the DeclRefExpr's level of
      // references.

      auto sourceRange = bo->getSourceRange();
      // Possibly: use ClangUtils.h, getStmtRangeWithSemicolon,
      // if we want a sourceRange including semicolon.

      ReferenceExprExtractor extractor(*Context, Co, lhs);
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
        Co.warnAt(bo, "operator=");
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
        Co.warnAt(bo, "<--- this");
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
  
};

class AnnotateVariablesConsumer : public SemaConsumer {
  ASTContext* Context;
  unique_ptr<Rewriter> Rewriter;
  Common Co;
  Sema* SemaPtr = nullptr;
  // A RecursiveASTVisitor implementation.
  AnnotateVariablesVisitor Visitor;

public:
  // TODO: change rewriter to be a OwningPtr<Rewriter>
  explicit AnnotateVariablesConsumer(ASTContext* Context,
      unique_ptr<class Rewriter>&& rewriter)
    : SemaConsumer(), Context(Context), Rewriter(move(rewriter)),
      Co(Context, Rewriter.get()),
      Visitor(Co, Context, SemaPtr, Rewriter.get()) {}

  void InitializeSema(Sema &S) {
    SemaPtr = &S;
  }

  void ForgetSema() {
    SemaPtr = 0;
  }

  virtual void HandleTranslationUnit(clang::ASTContext &Context) {
    // Traversing the translation unit decl via a RecursiveASTVisitor
    // will visit all nodes in the AST.
    Visitor.TraverseDecl(Context.getTranslationUnitDecl());

    if (Context.getDiagnostics().hasErrorOccurred()) {
      // Destroy the AST somehow?
      // Look at ParseAST.cpp - ParseAST(...)
      // Actually no need.. ParseAST doesn't do that either, it just carries
      // on..
    }

    // Print out the rewritten contents.

    // const RewriteBuffer *RewriteBuf =
    //     Rewriter->getRewriteBufferFor(Context.getSourceManager().getMainFileID());
    // llvm::outs() << std::string(RewriteBuf->begin(), RewriteBuf->end());

    //Rewriter->overwriteChangedFiles();
  }

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

llvm::StringRef theFrontendAction(clang::frontend::ActionKind K) {
  using namespace clang::frontend;
  switch (K) {
    case ASTDump:                 return "ASTDump";
    case ASTDumpXML:              return "ASTDumpXML";
    case ASTPrint:                return "ASTPrint";
    case ASTView:                 return "ASTView";
    case DumpRawTokens:           return "DumpRawTokens";
    case DumpTokens:              return "DumpTokens";
    case EmitAssembly:            return "EmitAssembly";
    case EmitBC:                  return "EmitBC";
    case EmitHTML:                return "EmitHTML";
    case EmitLLVM:                return "EmitLLVM";
    case EmitLLVMOnly:            return "EmitLLVMOnly";
    case EmitCodeGenOnly:         return "EmitCodeGenOnly";
    case EmitObj:                 return "EmitObj";
    case FixIt:                   return "FixIt";
    case GenerateModule:          return "GenerateModule";
    case GeneratePCH:             return "GeneratePCH";
    case GeneratePTH:             return "GeneratePTH";
    case InitOnly:                return "InitOnly";
    case ParseSyntaxOnly:         return "ParseSyntaxOnly";
    case PluginAction:            return "PluginAction";
  }
  return "<<UNKNOWN>>";
}

class AnnotateVariablesAction : public PluginASTAction {
protected:

  ASTConsumer *CreateASTConsumer(CompilerInstance &CI, llvm::StringRef file) {
    auto Rew = unique_ptr<Rewriter>(new Rewriter());
    Rew->setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
    /* Try to poke at ProgramAction to see what action we're doing,
       and ultimately if IRgen happens before our plugin. */
    // Also, where does the simple parsing happening?
    llvm::errs() << "ProgramAction = " 
                 << theFrontendAction(CI.getFrontendOpts().ProgramAction)
                 << "\n";
    
    ASTContext& Context = CI.getASTContext();
    // Let's be smarter! Combine our consumer with an ASTPrinter (from
    // ASTPrintAction) into a MultiplexConsumer.
    llvm::SmallVector<ASTConsumer*, 2> Consumers;
    Consumers.push_back(
      new AnnotateVariablesConsumer(&Context, move(Rew)));
    // And now print the AST!
    if (raw_ostream *OS = CI.createDefaultOutputFile(false, file)) {
      Consumers.push_back(CreateASTPrinter(OS));
    }
    return new MultiplexConsumer(Consumers);
  }

  bool ParseArgs(const CompilerInstance &CI,
                 const std::vector<std::string>& args) {
    for (unsigned i = 0, e = args.size(); i != e; ++i) {
      llvm::errs() << "AnnotateVariables arg = " << args[i] << "\n";

      if (args[i] == "-d" || args[i] == "--debug") {
        DEBUG = true;
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
