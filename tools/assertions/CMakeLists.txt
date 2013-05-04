# Build a module not a shared library
#set(MODULE TRUE)

#set( LLVM_LINK_COMPONENTS support mc)

set( LLVM_LINK_COMPONENTS
  ${LLVM_TARGETS_TO_BUILD}
  asmparser
  bitreader
  bitwriter
  # nativecodegen
  codegen
  instrumentation
  ipo
  linker
  selectiondag
  )

project(assertions)

add_clang_executable(${PROJECT_NAME}
  main.cpp
  AnnotateVariables.cpp
  ClangUtils.cpp
  )

add_dependencies(${PROJECT_NAME}
  clang-headers
  )

# We're going to be using lib/Sema/TreeTransform.h so let's
# add clang/lib to the includes.
include_directories("${CLANG_SOURCE_DIR}/lib")

target_link_libraries(${PROJECT_NAME}
  clangFrontend
  clangAST
  clangTooling
  clangFrontendTool # ExecuteCompilerInvocation.cpp
  )

set_target_properties(${PROJECT_NAME}
  PROPERTIES
  LINKER_LANGUAGE CXX
  PREFIX "")
