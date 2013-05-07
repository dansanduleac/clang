#ifndef ANNOTATEVARIABLES_STRINGJOIN_H
#define ANNOTATEVARIABLES_STRINGJOIN_H

// Adds the llvm::join operation, which takes an ArrayRef<T>, or a higher-
// order type convertible to ArrayRef<T>, where T is convertible to StringRef,
// and produces the concatenation of those strings, with separator placed in-
// between, like Python's str.join method.

namespace llvm {

namespace dont_use {
  template<typename To> char convertible_to_helper(const volatile To);
  template<typename To> double convertible_to_helper(...);
}

template <typename From, typename To>
struct is_convertible_to {
  static const bool value
    = sizeof(char) == sizeof(dont_use::convertible_to_helper<To>( *(From*)0 ));
};

//     typename llvm::enable_if<is_convertible_to<ArrT, ArrayRef<T>>, int>::type ignore=0
// template <typename ArrT>
// std::string join(ArrT Strings, StringRef separator) {
//   return _join(Strings, separator);

template <typename T>
typename llvm::enable_if<is_convertible_to<T, StringRef>,
                         std::string>::type
join(ArrayRef<T> Strings, StringRef separator) {
  std::string S;
  raw_string_ostream Out(S);
  bool start = true;
  for (const T& String : Strings) {
    if (start)
      start = false;
    else
      Out << separator;
    Out << String;
  }
  return Out.str();
}

template <class T>
std::string join(T Strings, StringRef separator) {
  return join(makeArrayRef(Strings), separator);
}

}

#endif