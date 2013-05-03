#include "ClangUtils.h"



using namespace clang;

// Adapted from:
// http://ccons.googlecode.com/svn-history/r217/trunk/ClangUtils.cpp

// Returns CharSourceRange because it's more straightforward to convert this
// to SourceRange than vice versa.  (CharSourceRange::getAsRange)
static
CharSourceRange
getRangeWithSemicolon(SourceLocation SLoc,
  SourceLocation ELoc,
  const SourceManager& SM,
  const LangOptions& LO)
{
  //unsigned start = SM.getFileOffset(SLoc);
  //unsigned end = SM.getFileOffset(ELoc);
  
  // Below code copied from clang::Lexer::MeasureTokenLength():
  clang::SourceLocation Loc = SM.getExpansionLoc(ELoc);
  std::pair<clang::FileID, unsigned> LocInfo = SM.getDecomposedLoc(Loc);
  llvm::StringRef Buffer = SM.getBufferData(LocInfo.first);
  const char *StrData = Buffer.data()+LocInfo.second;
  clang::Lexer TheLexer(Loc, LO, Buffer.begin(), StrData, Buffer.end());
  clang::Token TheTok;
  TheLexer.LexFromRawLexer(TheTok);
  // End copied code.
  int offset = TheTok.getLength();

  // Check if we the source range did include the semicolon.
  if (TheTok.isNot(clang::tok::semi) && TheTok.isNot(clang::tok::r_brace)) {
    TheLexer.LexFromRawLexer(TheTok);
    if (TheTok.is(clang::tok::semi)) {
      offset += TheTok.getLength();
    }
  }
  return CharSourceRange(
    SourceRange(SLoc, ELoc.getLocWithOffset(offset)), false);
}

//----------------------------------------------------------------------------

static
CharSourceRange
getStmtRangeWithSemicolon(const clang::Stmt* S,
  const clang::SourceManager& SM,
  const clang::LangOptions& LO)
{
  // Get the source range of the specified Stmt, ensuring that a semicolon is
  // included, if necessary - since the clang ranges do not guarantee this.
  clang::SourceLocation SLoc = SM.getExpansionLoc(S->getLocStart());
  clang::SourceLocation ELoc = SM.getExpansionLoc(S->getLocEnd());
  return getRangeWithSemicolon(SLoc, ELoc, SM, LO);
}