#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Sema/SemaConsumer.h"
// This is in "${CLANG_SOURCE_DIR}/lib", mind you.
#include "Sema/TreeTransform.h"

#include "Common.h"
#include "ClangUtils.h"
#include "ReferenceExprExtractor.h"
#include "AnnotateVariablesAction.h"

#include <memory>

using namespace llvm;

using std::string;
using std::vector;

// TODO replce with llvm::OwningPtr, easier.
#if LLVM_USE_RVALUE_REFERENCES
using std::move;
using std::unique_ptr;
#else
#pragma error("Nooooo! No rvalue references.")
#endif

namespace assertions {

class MyTreeTransform : public TreeTransform<MyTreeTransform> {
  typedef TreeTransform<MyTreeTransform> Base;

  Common& Co;

public:
  MyTreeTransform(Common& Common, Sema& S)
    : Base(S), Co(Common) { }


  // FIXME Do not need this after all? We will only transform statements
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
  // What are BlockDecl, BlockExpr ?

  Decl* TransformDecl(SourceLocation Loc, Decl* D) {
    // Need to descend into initialisers!
    Decl* D2 = Base::TransformDecl(Loc, D);
    // Hasn't been transformed yet, and is a VarDecl.
    VarDecl* VD;
    if (D2 == D && (VD = dyn_cast<VarDecl>(D))) {
      // TODO
      // hasDefinition (if it's defined in this TU) : DefinitionKind
      Expr* Init = VD->getInit();
      if (Init) {
        ExprResult E = TransformExpr(Init);
        if (E.isInvalid()) {
          Co.diagnosticAt(Init, "couldn't transform initializer");
        } else {
          VD->setInit(E.get());
        }
      }
      return D;
      // The Decl itself doesn't change but we set a new Init.
      // We can't currently remember the fact that we touched it.
      // transformedLocalDecl(D, ?);
    } else {
      // Not going to happen in my case.
      return D2;
    }
  }

  ExprResult TransformAssertedAssignment(BinaryOperator* bo,
      ReferenceExprExtractor& extractor) {
    const clang::Attr* attrs[] = {
      // TODO this shouldn't call getAnnotation() directly,
      // should be some intermediary doing the AssertionAttr cloning
      // (with a different indirection info too, maybe).
      new (getSema().Context) AnnotateAttr(bo->getSourceRange(),
              getSema().Context, extractor.attr->getAnnotation())
    };

    ExprResult Res = Base::RebuildAttributedExpr(
        bo->getExprLoc(), attrs, bo);
    return Res;
  }

  /// TODO also treat Unary assignment operators (prefix ++, --).

  /// \brief If it assigns to a tracked variable, will wrap in an
  /// AttributedExpr.
  ExprResult TransformBinaryOperator(BinaryOperator* bo) {
    // TODO start with "normal case", error handle, use the
    // transformed BinaryOperator for further logic.
    if (bo->isAssignmentOp()) {
      Expr* lhs = bo->getLHS();

      ReferenceExprExtractor extractor(getSema().Context, Co, lhs);
      extractor.run();

      if (extractor.found()) {
        // extractor.getNewAnnotations --> pass to fct below
        if (DEBUG) {
          Co.warnAt(bo, "will be transformed");
        }
        return TransformAssertedAssignment(bo, extractor);
      }
    }
    // Normal Case
    return Base::TransformBinaryOperator(bo);
  }

  // TODO
  // Aside from BinaryOperator, also check function calls that pass
  // asserted variables, and call TransformAssertedAssignment on them too.

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

  // Sanitise the type that we are assigning to (VD->getType()).
  // VD->getType() is a  "T " + one or more "*const",
  //      i.e. int *const *const.
  bool IsDeeplyConstPointer(VarDecl const *VD, StringRef str,
        std::function<void(DiagnosticBuilder)> errorHandling
            = ([](DiagnosticBuilder) {})) {
    PointerType const *ptrType;
    QualType typ = VD->getType();
    TypeLoc TL = VD->getTypeSourceInfo()->getTypeLoc();
    for (; (ptrType = dyn_cast<PointerType>(typ.getTypePtr()));
         typ = ptrType->getPointeeType(),
         // advance typeloc past const and ptr.
         TL = TL.getUnqualifiedLoc().getNextTypeLoc()) {
      if (!typ.isConstQualified()) {
        // VD.getLocation() if we want to point to the VD's name.
        auto builder = Co.diagnosticAt(TL, str)
          //<< TL.getSourceRange()
          << FixItHint::CreateInsertion(TL.getLocEnd(), " const");
        // Pass the builder to the optional errorHandling callback.
        errorHandling(builder);
        return true;
      }
    }
    return false;
  }

  // Just a convenience method for using ReferenceExprExtractor to try and
  // find a DRE that refers to an asserted variable.
  ReferenceExprExtractor ExtractAssertedDRE(Expr *E) {
    ReferenceExprExtractor extractor(*Context, Co, E);
    extractor.run();
    return extractor;
  }

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
    // TODO
    // if VD has an assertion, make sure that pointers are const, or error
    // (don't want to assign something to it later).
    QualType typ = VD->getType();
    bool isPtr = isa<PointerType>(typ.getTypePtr());
    if (attr && isPtr) {
      if (IsDeeplyConstPointer(VD,
            "asserted variable must be a deeply const pointer")) {
        return true;
      }
    }

    // Deal with initialisation ("assertion stealing").

    if (Expr* Init = VD->getInit()) {
      auto extractor = ExtractAssertedDRE(Init);
      // Esure that we are a pointer, otherwise we don't care.

      // extractor.found() returns true if (DRE found in Init)->getFoundDecl()
      // is asserted, and correctly addressed (using only deref / addrOf
      // operations in front of it).

      if (isPtr && extractor.found()) {
        // TODO in ReferenceExprExtractor: indirection should be the sum
        // between the DRE type indirection (count '*') and our indirection
        // referring to it.

        // Raise an error if we found a reference to another assertion,
        // but we already have an AssertionAttr on our back.
        // This behaviour might change in the future.
        if (attr) {
          Co.diagnosticAt(attr, "initialiser references asserted variable, "
                                "but already has an assertion")
            << extractor.dre;
          return true;
        }

        // TODO IsDeeplyConstPointer -> functor, allow "returning" the
        // DiagnosticBuilder.
        auto query =
          IsDeeplyConstPointer(VD, "mutable pointer to asserted variable",
            [&](DiagnosticBuilder db) {
               db << extractor.dre->getSourceRange();
            });
        if (query) {
          Co.diagnosticAt(extractor.attr, "the variable's assertion",
                          DiagnosticsEngine::Note);
          return true;
        }

        // Everything's OK, qualify VD with stolen assertion.
        VD->addAttr(extractor.attr);
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

  /*
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
        Co.warnAt(bo, "<--- this");
      }
      // Use Rewriter::ReplaceStmt to replace this with an AttributedStmt.
      Rewriter->ReplaceStmt(bo, wrapper);
    }
    return true;
  }
  */

  /// Ensure that parameters in function calls don't lose their assertion when
  /// passed, unless explicitly requested by user.  It is allowed for regular
  /// passed values and even pointers (for now) to become asserted inside the
  /// function.
  /// TODO Consider issuing a warning when passing a regular pointer to a
  /// function that will assert its value.
  bool VisitCallExpr(CallExpr const *Call) {
    if (FunctionDecl const *Callee = Call->getDirectCallee()) {
      if (DEBUG) {
        llvm::errs() << yellow << "Call to " << normal
          << Callee->getNameAsString()
          << "\n";
      }
      if (!Callee->isDefined()) {
        // TODO call to implicit function, or smth
        // Check parameters anyway for any assertion, and throw an error
        // if they contain one.
        return true;
      }
      // Iterate through parameters passed by caller, check each against
      // type of VarDecl in Callee.
      auto cb = Call->arg_begin(), ce = Call->arg_end();
      auto fb = Callee->param_begin(), fe = Callee->param_end();
      for (; cb != ce; ++cb, ++fb) {
        assert(fb != fe &&
          "Ran out of function parameters, please treat varargs?");
        auto extractor = ExtractAssertedDRE(const_cast<Expr*>(*cb));
        if (extractor.found()) {
          // Does the param have the same kind of attribute?
          auto parmAttr = Co.getAssertionAttr(*fb);
          if (!Co.IsSameAssertion(extractor.attr, parmAttr)) {
            if (extractor.attr) {
              // TODO
              // DUBIOUS, how do we get around this in a straightforward manner?
              // By annotating the TYPES themselves in VarDecl.
              Co.warnAt(*cb, "dropping assertion on function call, use a cast "
                             "to silence")
                << FixItHint::CreateInsertion((*cb)->getLocStart(),
                     "("+ (*fb)->getType().getAsString() +")");
              return true;
            }
            Co.diagnosticAt(*cb, "argument's assertion (%1) doesn't match "
              "that of parameter '%0' (%2)")
              << (*fb)->getName()
              << (extractor.attr ? Co.AssertionKindAsString(extractor.attr)
                    : "none")
              << (parmAttr ? Co.AssertionKindAsString(parmAttr) : "none");
            if (parmAttr) {
              Co.diagnosticAt(parmAttr, "parameter's assertion",
                DiagnosticsEngine::Note);
            }
          }
        }
      }
    }
    return true;
  }
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

  virtual bool HandleTopLevelDecl(DeclGroupRef DG) {
    for (Decl* D : DG) {
      // Traversing the translation unit decl via a RecursiveASTVisitor will
      // visit all nodes in the AST. This will propagate annotations across to
      // all VarDecls which should have them.
      // Context->getTopLevelDecl()  no more!
      Visitor.TraverseDecl(D);

      // Now transform the assignments, if the Decl is a function.
      // TODO can other Decls contain assignments, e.g. BlockDecl?
      //   --> can BlockDecl appear outside functions?
      FunctionDecl* FD;
      if (!(FD = dyn_cast<FunctionDecl>(D))) {
        continue;
      }

      // Decls can go like getContext() => the DeclContext* .
      //  but does decl_iterator allow changing the children?
      // they iterate with Decl::getNextDeclInContext(),
      // how do I transform a Decl and preserve the order?
      // using llvm::PointerIntPair<Decl *, 2, unsigned> NextInContextAndBits;

      // Stmts have children that apparently can be iterated on
      //   by reference. But can we actually change that?
      // Check out StmtIterator.h

      MyTreeTransform Transform(Co, *SemaPtr);
      SemaPtr->PushFunctionScope();
      StmtResult Res;
      {
        Sema::ContextRAII FuncDeclContext(*SemaPtr, FD);
        Res = Transform.TransformStmt(FD->getBody());
      }
      SemaPtr->PopFunctionScopeInfo();
      if (Res.isUsable()) {
        FD->setBody(Res.get());
      } else {
        Co.diagnosticAt(FD, "Couldn't transform function body.",
          DiagnosticsEngine::Fatal);
      }
    }
    return true;
  }

  virtual void HandleTranslationUnit(clang::ASTContext &Context) {
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
};

// Taken from ExecuteCompilerInvocation.cpp for debugging.
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

ASTConsumer *AnnotateVariablesAction::CreateASTConsumer(
    CompilerInstance &CI, llvm::StringRef file) {
  auto Rew = unique_ptr<Rewriter>(new Rewriter());
  Rew->setSourceMgr(CI.getSourceManager(), CI.getLangOpts());
  // Try to poke at ProgramAction to see what action we're doing.
  if (DEBUG) {
    llvm::errs() << "ProgramAction = "
                 << theFrontendAction(CI.getFrontendOpts().ProgramAction)
                 << "\n";
    llvm::errs() << "FrontendOptions.OutputFile = "
                 << CI.getFrontendOpts().OutputFile
                 << "\n";
  }

  ASTContext& Context = CI.getASTContext();
  // Let's be smarter! Combine our consumer with an ASTPrinter (from
  // ASTPrintAction) into a MultiplexConsumer.
  llvm::SmallVector<ASTConsumer*, 2> Consumers;
  Consumers.push_back(
    new AnnotateVariablesConsumer(&Context, move(Rew)));

  // And now the MultiplexConsumer belonging to the WrappedAction.
  Consumers.push_back(WrapperFrontendAction::CreateASTConsumer(CI, file));
  return new MultiplexConsumer(Consumers);
}

}