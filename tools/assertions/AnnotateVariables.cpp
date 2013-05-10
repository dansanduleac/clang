#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendPluginRegistry.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Sema/SemaConsumer.h"
// This is in "${CLANG_SOURCE_DIR}/lib", mind you.
#include "Sema/TreeTransform.h"

#include "Common.h"
#include "ReferenceExprExtractor.h"
#include "AnnotateVariablesAction.h"

#include <memory>

using namespace llvm;

using std::string;
using std::vector;

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
          llvm_unreachable("couldn't transform initializer");
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

  ExprResult TransformAssertedAssignment(Expr* assignment,
      ReferenceExprExtractor& extractor) {
    const clang::Attr* attrs[] = {
      // TODO this shouldn't call getAnnotation() directly,
      // should be some intermediary doing the AssertionAttr cloning
      // (with a different indirection info too, maybe).
      new (getSema().Context) AnnotateAttr(assignment->getSourceRange(),
              getSema().Context, extractor.getAttr()->getAnnotation())
    };

    ExprResult Res = Base::RebuildAttributedExpr(
        assignment->getExprLoc(), attrs, assignment);
    return Res;
  }

  /// \brief If it's a modifying operator (++/--), will wrap in an
  /// AttributedExpr.
  ExprResult TransformUnaryOperator(UnaryOperator* UO) {
    ExprResult Transformed = Base::TransformUnaryOperator(UO);
    assert(!Transformed.isInvalid() && "Couldn't transform operator");
    if (UO->isIncrementDecrementOp()) {
      Expr* lvalue = UO->getSubExpr();
      ReferenceExprExtractor extractor(Co, lvalue,
          /*checkIfUnaryUpdate*/true);
      extractor.run();

      if (extractor.found()) {
        // extractor.getNewAnnotations --> pass to fct below
        if (DEBUG) {
          Co.warnAt(UO, "will be transformed");
        }
        return TransformAssertedAssignment(UO, extractor);
      }
    }
    // Normal Case
    return Transformed;
  }

  /// \brief If it assigns to a tracked variable, will wrap in an
  /// AttributedExpr.
  ExprResult TransformBinaryOperator(BinaryOperator* BO) {
    ExprResult Transformed = Base::TransformBinaryOperator(BO);
    assert(!Transformed.isInvalid() && "Couldn't transform operator");
    if (BO->isAssignmentOp()) {
      Expr* lvalue = BO->getLHS();
      ReferenceExprExtractor extractor(Co, lvalue);
      extractor.run();

      if (extractor.found()) {
        // extractor.getNewAnnotations --> pass to fct below
        if (DEBUG) {
          Co.warnAt(BO, "will be transformed");
        }
        return TransformAssertedAssignment(Transformed.get(), extractor);
      }
    }
    // Normal Case
    return Transformed;
  }

  // TODO Also check function calls that pass asserted variables, and call
  // TransformAssertedAssignment on them too.
};



class AnnotateVariablesVisitor
  : public RecursiveASTVisitor<AnnotateVariablesVisitor> {
  Common& Co;
  ASTContext* Context;
  // We want this to change when consumer gets InitializeSema called on it.
  // Therefore, reference.
  Sema*& SemaPtr;
  Rewriter* Rewriter;
  // Remember the asserted VarDecls that are references (pointers), because
  // we're going to come back at the end, and remove their assertion
  // attribute. We are using @llvm.var.annotation to indicate when to allocate
  // a new assertion structure, and for pointers, that doesn't apply.
  SmallVector<VarDecl *, 20> ReferenceDecls;

  typedef Common::AssertionAttr AssertionAttr;

public:
  explicit AnnotateVariablesVisitor(Common& C, ASTContext* Context,
    Sema*& SemaPtr, class Rewriter* R)
    : Co(C), Context(Context), SemaPtr(SemaPtr), Rewriter(R) {}

  ~AnnotateVariablesVisitor() {
    // Just in case.
    cleanup();
  }

  // Remove attrs from the VarDecls which inherited them automatically by
  // acting as references to other asserted vars.
  void cleanup() {
    for (VarDecl *VD : ReferenceDecls) {
      VD->dropAttr<AssertionAttr>();
    }
    ReferenceDecls.clear();
  }

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
        return false;
      }
    }
    return true;
  }

  // Just a convenience method for using ReferenceExprExtractor to try and
  // find a DRE that refers to an asserted variable.
  ReferenceExprExtractor ExtractAssertedDRE(Expr *E) {
    ReferenceExprExtractor extractor(Co, E);
    extractor.run();
    return extractor;
  }

  // VISITORS
  // -------------------------------------------------

  bool VisitFunctionDecl(FunctionDecl *FD) {
    AssertionAttr* attr = Co.getAssertionAttr(FD);
    if (attr == nullptr)
      return true;
    // We allow any function return type, except void.
    if (FD->getResultType().getTypePtr()->isVoidType()) {
      Co.diagnosticAt(FD, "asserted function cannot return void");
      return false;
    }
    Co.QualifyAttrReplace(attr);
    return true;
  }

  bool VisitVarDecl(VarDecl* VD) {
    raw_ostream &e = llvm::errs();
    if (DEBUG) {
      e << yellow << "VarDecl" << normal
        << " at " << Co.printLoc(VD) << ": \""
        << VD->getName() << "\":\n";
      VD->dump();
      e << "\n";
    }

    AssertionAttr* attr = Co.getAssertionAttr(VD);
    // If VD has an assertion, make sure ALL pointers are const, or error
    // (don't want to allow assigning to it later because we're statically
    // assuming that the value accessible by this pointer is going to be
    // asserted in this way).

    // "int *const", and "int *const *const" are deeply-const types, but
    // "int **const a" is not (can do *a = ...).
    QualType typ = VD->getType();
    bool isPtr = isa<PointerType>(typ.getTypePtr());
    if (attr && isPtr) {
      if (!IsDeeplyConstPointer(VD,
            "asserted variable must be a deeply const pointer")) {
        return false;
      }
    }

    if (isPtr) {
      Expr *Init = VD->getInit();
      bool isParm = isa<ParmVarDecl>(VD);
      // When it's a normal pointer (not a function parameter), don't allow
      // manually specifying an assertion on it. The only case this would be
      // useful would be to hint to the compiler that the value at a certain
      // address should be asserted, but then we'd have to inject runtime
      // checks to figure out which asserted variable state we should match it
      // to (which UID).
      if (attr) {
        if (!isParm) {
          Co.diagnosticAt(VD, "not allowed to specify assertion on pointer "
              "type, unless function parameter");
          return false;
        }
        // XXX C function parameters can't have initialisers, useless.
         else if (Init) {
          Co.diagnosticAt(VD, "can't init asserted function parameter");
          return true;
        }
      }
      // If we're a pointer in a block of code (i.e. in a DeclStmt), deal with
      // initialisation ("assertion stealing").
      if (!isParm) {
        // Since it's a const, it can't possibly not have an init (in C).
        // TODO what if inside a structure though? The init will be somewhere else.
        assert(Init);
        auto extractor = ExtractAssertedDRE(Init);

        if (extractor.found()) {
          // Raise an error if we found a reference to another assertion,
          // but we already have an AssertionAttr on our back.
          // This behaviour might change in the future.
          if (attr) {
            Co.diagnosticAt(attr, "initialiser references asserted variable, "
                                  "but already has an assertion")
              << extractor.getDRE();
            return true;
          }

          auto query =
            IsDeeplyConstPointer(VD, "mutable pointer to asserted variable",
              [&](DiagnosticBuilder db) {
                 db << extractor.getDRE()->getSourceRange();
              });
          if (!query) {
            Co.diagnosticAt(extractor.getAttr(), "the variable's assertion",
                            DiagnosticsEngine::Note);
            return true;
          }

          // Everything's OK, qualify VD with stolen assertion.
          VD->addAttr(extractor.getAttr());
          return true;
        }
      } // end !isParm
      else {
        // state var shd be called: %assertions.state_<UID>_
        // parameters shd be called %assertions.state_<UID>_ directly
        auto *FD = dyn_cast<FunctionDecl>(VD->getDeclContext());
        assert(FD && "ParmVarDecl's DeclContext is not a FunctionDecl");
        // Annotate the function with information regarding the asserted
        // parameter. Also, skip creating UIDs -- last stage -- unless the
        // function is DEFINED! (otherwise just do the FuncParmInfo
        // annotations but skip UIDs)
        // TODO!!!
        //FuncParmInfo[FD].append();
       }
       // Remember to remove VD's llvm.var.annotation either way.
        ReferenceDecls.push_back(VD);
    }   // end isPtr

    if (attr)
      Co.QualifyAttrReplace(attr);
    return true;
  }

  // This is for some serious debugging, basically to show the entire Stmt
  // class hierarchy of each Stmt.
  /*
  bool VisitStmt(Stmt* S) {
    auto &e = llvm::errs();
    if (DEBUG) {
      S->dump();
    }
    return true;
  }
  */

  /// Ensure that parameters in function calls don't lose their assertion when
  /// passed, unless explicitly requested by user.  It's forbidden for a value
  /// to become asserted inside the function when it is passed by address.
  /// That would make it ambiguous whether to allocate a control structure or
  /// not: yes for regular value, but no for already-asserted value.
  bool VisitCallExpr(CallExpr const *Call) {
    if (FunctionDecl const *Callee = Call->getDirectCallee()) {
      if (DEBUG) {
        llvm::errs() << yellow << "Call to " << normal
          << Callee->getNameAsString()
          << "\n";
      }
      // Iterate through parameters passed by caller, check each against
      // type of VarDecl in Callee.
      auto cb = Call->arg_begin(), ce = Call->arg_end();
      auto fb = Callee->param_begin(), fe = Callee->param_end();
      for (; cb != ce; ++cb, ++fb) {
        assert(fb != fe &&
          "Ran out of function parameters, please treat varargs?");
        // Only run this analysis if the ParmVarDecl is a pointer-type.
        bool isPtrParm = isa<PointerType>((*fb)->getType().getTypePtr());
        if (!isPtrParm) {
          continue;
        }
        auto extractor = ExtractAssertedDRE(const_cast<Expr*>(*cb));
        extractor.run();
        // Does the param have the same kind of attribute?
        auto parmAttr = Co.getAssertionAttr(*fb);
        auto argAttr  = extractor.getAttr();
        if (!Co.IsSameAssertion(argAttr, parmAttr)) {
          if (argAttr && !parmAttr) {
            // FIXME
            // DUBIOUS, how do we get around this in a straightforward manner?
            // By annotating the TYPES themselves in VarDecl.
            Co.warnAt(*cb, "dropping assertion on function call, use a cast "
                           "to silence")
              << FixItHint::CreateInsertion((*cb)->getLocStart(),
                   "("+ (*fb)->getType().getAsString() +")");
            continue;
          }
          Co.diagnosticAt(*cb, "argument's assertion (%1) doesn't match "
            "that of parameter '%0' (%2)")
            << (*fb)->getName()
            << (argAttr ? "'" + Twine(Co.AssertionKindAsString(argAttr)) + "'"
                        : "none").str()
            << (parmAttr ? "'" + Twine(Co.AssertionKindAsString(parmAttr)) + "'"
                        : "none").str();
          if (parmAttr) {
            Co.diagnosticAt(parmAttr, "parameter's assertion",
              DiagnosticsEngine::Note);
          }
        }
      
      }
    }
    return true;
  }
};

class AnnotateVariablesConsumer : public SemaConsumer {
  ASTContext* Context;
  OwningPtr<Rewriter> Rewriter;
  Common Co;
  Sema* SemaPtr = nullptr;
  // A RecursiveASTVisitor implementation.
  AnnotateVariablesVisitor Visitor;

public:
  // TODO: change rewriter to be a OwningPtr<Rewriter>
  explicit AnnotateVariablesConsumer(ASTContext* Context,
                                     class Rewriter *rewriter)
    : SemaConsumer(), Context(Context), Rewriter(rewriter),
      Co(Context, Rewriter.get()),
      Visitor(Co, Context, SemaPtr, Rewriter.get()) {}

  void InitializeSema(Sema &S) {
    SemaPtr = &S;
  }

  void ForgetSema() {
    SemaPtr = 0;
  }

  virtual bool HandleTopLevelDecl(DeclGroupRef DG) {
    for (Decl *D : DG) {
      // Traversing the translation unit decl via a RecursiveASTVisitor will
      // visit all nodes in the AST. This will propagate annotations across to
      // all VarDecls which should have them.
      // Context->getTopLevelDecl()  no more!
      Visitor.TraverseDecl(D);

      // Now transform the assignments, if the Decl is a function.
      // TODO can other Decls contain assignments, e.g. BlockDecl?
      //   --> can BlockDecl appear outside functions?
      FunctionDecl *FD;
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
    // Cleanup unwanted VarDecl annotations, but only AFTER the Transform
    // has run (it depends on those to annotate the assignments).
    // Actually moved all the way here because we are now removing attrs from
    // ParmVarDecl too, but we need those to persist until we finish with the
    // TU.
    Visitor.cleanup();
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
  OwningPtr<Rewriter> Rew(new Rewriter());
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
    new AnnotateVariablesConsumer(&Context, Rew.take()));

  // And now the MultiplexConsumer belonging to the WrappedAction.
  Consumers.push_back(WrapperFrontendAction::CreateASTConsumer(CI, file));
  return new MultiplexConsumer(Consumers);
}

}