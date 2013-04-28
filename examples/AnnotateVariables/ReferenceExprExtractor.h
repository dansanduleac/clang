#ifndef ANNOTATEVARIABLES_REFERENCEEXPREXTRACTOR_H
#define ANNOTATEVARIABLES_REFERENCEEXPREXTRACTOR_H

#include "clang/AST/EvaluatedExprVisitor.h"

#include "Common.h"

namespace {

  using namespace clang;

  // Use this to extract any DeclRefExpr from lvalues that we assign to
  //   (or that we pass to functions).
  class ReferenceExprExtractor
      : public EvaluatedExprVisitor<ReferenceExprExtractor> {
    Common& Co;
    Expr* toVisit;
    bool multipleAssertedDREFound = false;

    typedef EvaluatedExprVisitor<ReferenceExprExtractor> Base;
  public:
    // We need Common ref to use warnAt, or any other stuff that depends
    // on the ASTContext..
    ReferenceExprExtractor(ASTContext& C, Common& Common,
                     Expr* toVisit)
      : EvaluatedExprVisitor<ReferenceExprExtractor>(C),
        Co(Common),
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

    void VisitDeclRefExpr(DeclRefExpr *DRE) {
      NamedDecl *orig = DRE->getFoundDecl();
      //Co.warnAt(orig, "DRE referenced this object");
      if ((attr = Co.getAssertionAttr(orig))) {
        if (dre != NULL) {
          multipleAssertedDREFound = true;
          return;
        }
        dre = DRE;

        // Some debugging on the type...
        clang::QualType tt = DRE->getType();
        // TODO Find the "indirection" on tt, if we even care?

        if (DEBUG) {
          std::string S; llvm::raw_string_ostream os(S);
          os << "DRE->getType() == " << tt.getAsString();
          Co.warnAt(DRE, os.str());
        }
      }
    }

    void VisitUnaryAddrOf(UnaryOperator *UO) {
      // indirection ++;
      Visit(UO->getSubExpr());
    }

    void VisitUnaryDeref(UnaryOperator *UO) {
      // indirection --;
      Visit(UO->getSubExpr());
    }

    void Visit(Stmt* S) {
      if (BinaryOperator *BinOp = dyn_cast<BinaryOperator>(S)) {
        if (BinOp->isAssignmentOp()) {
          // then it's ok, continue looking in LHS
          Base::Visit(BinOp->getLHS());
        }
        return;
      } else if (UnaryOperator *UnOp = dyn_cast<UnaryOperator>(S)) {
        switch (UnOp->getOpcode()) {
          case UO_AddrOf:
          case UO_Deref:
            Base::Visit(S);
          default: return;
        }
        return;
      }
      // TODO remember if there is a cast, and if we find a suitable DRE, warn
      // that this is unsupported

      // TODO only explicitly allow:
      // ImplicitCastExpr, DeclRefExpr, ...
      //   ExplicitCastExpr, for instance, wd be bad to allow... or at least
      //   force a warning!
      Base::Visit(S);
    }

    void run() {
      Visit(toVisit);
      if (multipleAssertedDREFound) {
        Co.
        diagnosticAt(toVisit, DiagnosticsEngine::Fatal,
                     "found more than 1 asserted DRE inside this Expr");
      }
    }

    DeclRefExpr *dre = NULL;
    AssertionAttr *attr = NULL;
  };

}

#endif