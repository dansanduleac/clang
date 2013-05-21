#ifndef ANNOTATEVARIABLES_ASSERTION_H
#define ANNOTATEVARIABLES_ASSERTION_H

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/ErrorHandling.h"

namespace clang {
  class AnnotateAttr;
  class Decl;
}

namespace assertions {

using namespace llvm;

struct Assertion {
  StringRef Kind; // e.g. "monotonic"
  SmallVector<StringRef, 2> Params; // random(1, 2, 3)
  int UID;

  bool isCompatible(Assertion &A) {
    return Kind == A.Kind &&
           Params == A.Params;
  }
};

typedef clang::AnnotateAttr AssertionAttr;

const llvm::StringRef GLOBAL_PREFIX = "assertion,";

class AssertionManager {
  // TODO:
  // Update these to actually use a custom attribute, not just AnnotateAttr,
  // because this is quite inflexible. That would require modifying the
  // accepted input language, though.

private:
  // Cache for parsed annotations.
  llvm::StringMap<Assertion> ParsedAssertions;

public:
  static bool IsSaneAssertion(StringRef attr) {
    return attr.startswith(GLOBAL_PREFIX);
  }

  bool SameAssertion(StringRef a1, StringRef a2) {
    Assertion &pa1 = getParsedAssertion(a1),
              &pa2 = getParsedAssertion(a2);
    return pa1.isCompatible(pa2);
  }

  Assertion &getParsedAssertion(StringRef anno) {
    if (!ParsedAssertions.count(anno)) {
      auto &Entry =
        ParsedAssertions.GetOrCreateValue(anno, ParseAssertion(anno));
      return Entry.getValue();
    } else
      return ParsedAssertions[anno];
  }

private:
  static Assertion ParseAssertion(StringRef anno) {
    assert(anno.startswith(GLOBAL_PREFIX));
    Assertion Ret;

    size_t paramStart = anno.find('(');
    if (paramStart != StringRef::npos) {
      size_t paramEnd = anno.rfind(')');
      anno.slice(paramStart+1, paramEnd).split(Ret.Params, ",");
      Ret.Kind = anno.slice( anno.find(',')+1, paramStart );
      if (anno.size() > paramEnd+1) {
        assert(anno[paramEnd+1] == ',');
        anno.slice(paramEnd+2, anno.size()).getAsInteger(10, Ret.UID);
      } else
        Ret.UID = -1;
    } else {
      SmallVector<StringRef, 3> Arr;
      anno.split(Arr, ",");
      assert(Arr.size() < 4 && "Too many fields in assertion, max 3");
      Ret.Kind = Arr[1];
      if (Arr.size() > 2) {
        if (Arr[2].getAsInteger(10, Ret.UID))
          llvm_unreachable("Can't parse UID");
      } else
        Ret.UID = -1;
    }
    return Ret;
  }
};

}

#endif