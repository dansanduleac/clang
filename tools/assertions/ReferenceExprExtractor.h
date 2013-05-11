#ifndef ANNOTATEVARIABLES_REFERENCEEXPREXTRACTOR_H
#define ANNOTATEVARIABLES_REFERENCEEXPREXTRACTOR_H

#include "clang/AST/EvaluatedExprVisitor.h"

#include "Common.h"

namespace assertions {

  using namespace clang;

  // Use this to:
  // * Extract any asserted Decl referred by the lvalues that we assign to or
  //   that we pass to functions (found either through a DeclRefExpr or a
  //   MemberExpr), or
  // * Extract updated DeclRefExpr, if any, in a passed UnaryExpression
  //   (e.g. "(*a)++" extracts the DRE associated with "a")
  class ReferenceExprExtractor
      : public EvaluatedExprVisitor<ReferenceExprExtractor> {
    Common& Co;
    Expr* toVisit;
    // Allow visiting unary update ops too.
    bool UnaryOps;
    bool multipleAssertedDREFound = false;
    Expr *RefExpr = nullptr;
    AssertionAttr *attr = nullptr;

    typedef EvaluatedExprVisitor<ReferenceExprExtractor> Base;
  public:
    // We need the Common ref for diagnostics and ASTContext.
    ReferenceExprExtractor(Common& Common, Expr* toVisit,
                           bool checkIfUnaryUpdate = false)
      : EvaluatedExprVisitor<ReferenceExprExtractor>(*Common.getContext()),
        Co(Common), toVisit(toVisit), UnaryOps(checkIfUnaryUpdate) {}

    AssertionAttr *getAttr() {
      run();
      return attr;
    }

    Expr *getRefExpr() {
      run();
      return RefExpr;
    }

    // Returns true if we found a DRE/MemberExpr in "toVisit" using these
    // visitation rules, and furthermore, that referenced Decl is asserted.
    bool found() {
      run();
      return attr != nullptr;
    }

    void VisitMemberExpr(MemberExpr *ME) {
      ValueDecl *orig = ME->getMemberDecl();
      if ((attr = Co.getAssertionAttr(orig))) {
        if (RefExpr != nullptr) {
          multipleAssertedDREFound = true;
          return;
        }
        RefExpr = ME;
      }
      // Don't visit further, we're not interesting in the record's DRE for
      // now.
    }

    void VisitDeclRefExpr(DeclRefExpr *DRE) {
      NamedDecl *orig = DRE->getFoundDecl();
      //Co.warnAt(orig, "DRE referenced this object");
      if ((attr = Co.getAssertionAttr(orig))) {
        if (RefExpr != nullptr) {
          multipleAssertedDREFound = true;
          return;
        }
        RefExpr = DRE;

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

    /// Called for PostInc, PostDec, PreInc, PreDec
    void VisitUnaryAssignment(UnaryOperator *UO) {
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
        // Are we a unary modifying argument?
        // (UO_PostInc, UO_PostDec, UO_PreInc, UO_PreDec)
        // Only perform this check if UnaryOps is enabled.
        if (UnaryOps && UnOp->isIncrementDecrementOp()) {
          VisitUnaryAssignment(UnOp);
        } else
          switch (UnOp->getOpcode()) {
            case UO_AddrOf:
            case UO_Deref:
              Base::Visit(S);
            default: return;
          }
        return;
      }
      switch (S->getStmtClass()) {
        case Expr::ImplicitCastExprClass:
        case Expr::DeclRefExprClass:
        case Expr::MemberExprClass:
        case Expr::ParenExprClass:
          break;
        case Expr::CStyleCastExprClass:
        default:
          return; // Ignore!
      }
      Base::Visit(S);
    }

    void run() {
      if (toVisit) {
        Visit(toVisit);
        if (multipleAssertedDREFound) {
          Co.
          diagnosticAt(toVisit,
                       "found more than 1 asserted DRE inside this Expr");
        }
        toVisit = nullptr;
        assert(!(RefExpr == nullptr ^ attr == nullptr));
      }
    }

  };

}

#endif