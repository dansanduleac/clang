# Build a module not a shared library
set(MODULE TRUE)

set( LLVM_LINK_COMPONENTS support mc)

project(AnnotateVariables)

add_clang_library(${PROJECT_NAME} AnnotateVariables.cpp RewriterUtils.cpp)

# We're going to be using lib/Sema/TreeTransform.h so let's
# add clang/lib to the includes.
include_directories("${CLANG_SOURCE_DIR}/lib")

target_link_libraries(${PROJECT_NAME}
  clangFrontend
  clangAST
  # for analyzer-plugin:SampleAnalyzerPlugin
  clangStaticAnalyzerCore
  )

set_target_properties(${PROJECT_NAME}
  PROPERTIES
  LINKER_LANGUAGE CXX
  PREFIX "")
