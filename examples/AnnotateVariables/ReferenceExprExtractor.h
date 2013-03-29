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

    void VisitDeclRefExpr(DeclRefExpr* DRE) {
      NamedDecl* orig = DRE->getFoundDecl();
      //Co.warnAt(orig, "DRE referenced this object");
      if ((attr = Co.getAssertionAttr(orig))) {
        if (dre != NULL) {
          success = false;
          return;
        } 
        dre = DRE;

        // Some debugging on the type...
        std::string S; llvm::raw_string_ostream os(S);
        clang::QualType tt = DRE->getType();
        os << "DRE->getType() == " << tt.getAsString();
        Co.warnAt(DRE, os.str());
      }
    }

    void run() {
      Visit(toVisit);
      if (!success) {
        Co.
        diagnosticAt(toVisit, DiagnosticsEngine::Fatal,
                     "Found more than 1 DRE inside this Expr");
      }
    }

    DeclRefExpr* dre = NULL;
    AssertionAttr* attr = NULL;
    int indirection = 0;
    bool success = true;
  };

}

#endif