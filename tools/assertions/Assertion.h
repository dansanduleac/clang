#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallString.h"

namespace assertions {

struct Assertion {
  StringRef Kind; // e.g. "monotonic"
  SmallVector<StringRef, 2> Params; // random(1, 2, 3)
  int UID;

  bool isCompatible(Assertion &A) {
    return Kind == A.Kind &&
           Params == A.Params;
  }
};

typedef AnnotateAttr AssertionAttr;

const llvm::StringRef GLOBAL_PREFIX = "assertion,";

class AssertionManager {
  // TODO:
  // Update these to actually use a custom attribute, not just AnnotateAttr,
  // because this is quite inflexible. That would require modifying the
  // accepted input language, though.

private:
  // Cache for parsed annotations.
  llvm::DenseMap<AssertionAttr*, Assertion> ParsedAssertions;


  // There will be a different Visitor for each translation unit, but that's
  // ok because we do not need globally unique UIDs. Each translation unit
  // will have its own set of UIDs, and their mappings to the run-time
  // structures holding the asserted variables' states are maintained
  // internally per TU.
  int uid = 0;

public:
  int getUID() {
    return uid;
  }

  /// Extract the valid AssertionAttr, if any.
  AssertionAttr *getAssertionAttr(Decl *D) {
    AssertionAttr* attr = D->template getAttr<AssertionAttr>();
    return attr && IsSaneAssertionAttr(attr) ? attr : nullptr;
  }

  template <typename T>
  bool hasAssertionAttr(Decl *D) {
    return getAssertionAttr(D) != nullptr;
  }

  // To check if this Attr is in a sane state, representing a correct AssertionAttr.
  // Using it here since we're emulating on top of AnnotateAttr, and not annotations
  // are valid in our context.
  // TODO: deprecate after we implement a separate Attr.
  static bool IsSaneAssertionAttr(const AssertionAttr *attr) {
    return attr->getAnnotation().startswith(GLOBAL_PREFIX);
  }

  // Returns a string that represents the qualified AnnotateAttr's annotation.
  std::string GetQualifiedAttrString(AnnotateAttr* attr) {
    if (!IsSaneAssertionAttr(attr)) {
      llvm_unreachable("Tried to qualify bad AssertionAttr");
    }
    // TODO also pass an ArrayRef to specify additional "parameters"
    SmallString<30> an = attr->getAnnotation();
    an.append(",");
    llvm::raw_svector_ostream S(an);
    S << ++uid;
    return S.str();
  }

  bool IsSameAssertion(AssertionAttr* a1, AssertionAttr* a2) {
    if (a1 == nullptr || a2 == nullptr) {
      return a1 == nullptr && a2 == nullptr;
    }
    Assertion &pa1 = getParsedAssertion(a1),
              &pa2 = getParsedAssertion(a2);
    return pa1.isCompatible(pa2);
  }

  Assertion &getParsedAssertion(AssertionAttr *attr) {
    if (!ParsedAssertions.count(attr)) {
      auto pair = ParsedAssertions.insert(
        std::make_pair(attr, ParseAssertion(attr)));
      assert(pair.second);
      return pair.first->second;
    } else
      return ParsedAssertions[attr];
  }

  StringRef AssertionKindAsString(AssertionAttr* attr) {
    return getParsedAssertion(attr).Kind;
  }

private:
  static Assertion ParseAssertion(AssertionAttr *attr) {
    StringRef anno = attr->getAnnotation();
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
      Ret.Kind = Arr[1];
      if (Arr.size() > 2) {
        Arr[2].getAsInteger(10, Ret.UID);
      } else
        Ret.UID = -1;
      assert(Arr.size() < 4);
    }
    return Ret;
  }
};

}