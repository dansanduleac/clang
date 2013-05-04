#ifndef ANNOTATEVARIABLES_ANNOTATEVARIABLESACTION_H
#define ANNOTATEVARIABLES_ANNOTATEVARIABLESACTION_H

#include "clang/Frontend/FrontendAction.h"

namespace assertions {

using namespace clang;

class AnnotateVariablesAction : public WrapperFrontendAction {
protected:

  ASTConsumer *CreateASTConsumer(CompilerInstance &CI,
                                 llvm::StringRef file) override;

public:
  /// Construct a WrapperFrontendAction from an existing action, taking
  /// ownership of it.
  AnnotateVariablesAction(FrontendAction *WrappedAction)
    : WrapperFrontendAction(WrappedAction) {}

  // TODO
  // Look at bool BeginInvocation(CompilerInstance &CI)
  //   FrontendAction.h:67
};

}

#endif