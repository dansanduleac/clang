#ifndef ANNOTATEVARIABLES_STRINGJOIN_H
#define ANNOTATEVARIABLES_STRINGJOIN_H

#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Compiler.h" // LLVM_LIKELY

// Adds the llvm::join operation, which takes an ArrayRef<T>, or a higher-
// order type convertible to ArrayRef<T>, where T is convertible to StringRef,
// and produces the concatenation of those strings, with separator placed in-
// between, like Python's str.join method.

namespace llvm {

namespace dont_use {
  struct nothing {};
  template<typename To> char convertible_to_helper(const volatile To);
  template<typename To> double convertible_to_helper(...);

  template<typename T> nothing operator<<(raw_ostream &, T);
}

template <typename From, typename To>
struct is_convertible_to {
  static const bool value
    = sizeof(char) == sizeof(dont_use::convertible_to_helper<To>( *(From*)0 ));
};

namespace {
  using dont_use::operator<<;
  using dont_use::nothing;

  template <typename T>
  struct is_writeable {
    static raw_ostream &OS;
    static T &Elem;
    static const bool value = sizeof(nothing) != sizeof(OS << Elem);
};
}

// Optionally:
// struct is_writeable_c : is_same<decltype(*(raw_stream*)0 << *(T*)0), nothing> {}
// and use it in enable_if_c

// TODO .. nicer than comparing with sizeof
#if __has_feature(cxx_decltype)
#endif

class Concatenation : public raw_string_ostream {
  std::string S;
  bool start = true;
public:
  Concatenation() : raw_string_ostream(S) {}

  void writeSep(StringRef separator) {
    if (LLVM_UNLIKELY(start)) {
      start = false;
    } else {
      *this << separator;
    }
  }

  template <class T>
  inline Concatenation &appendJoin(T Strings, StringRef separator) {
    return join(Strings, separator, *this);
  }
};

template <typename T>
typename llvm::enable_if<is_writeable<T>, Concatenation&>::type
join(ArrayRef<T> Strings, StringRef separator, Concatenation &Out) {
  for (const T& String : Strings) {
    Out.writeSep(separator);
    Out << String;
  }
  return Out;
}

template <class T>
inline Concatenation &join(T Strings, StringRef separator,
                           Concatenation &Out) {
  join(makeArrayRef(Strings), separator, Out);
  return Out;
}

// Can't return the Concatentation, no copy constructor.
template <class T>
inline std::string join(T Strings, StringRef separator) {
  Concatenation Out;
  return join(makeArrayRef(Strings), separator, Out).str();
}

}

#endif