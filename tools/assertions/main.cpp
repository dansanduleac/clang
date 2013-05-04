#include "AnnotateVariablesAction.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/CommandLine.h"
// #include "llvm/Support/PathV1.h"           // for GetMainExecutable
// for getDefaultTargetTriple, replace it with?..
#include "llvm/Support/Host.h"

// Why not these... meh never used in cc1_main either! FIXME remove
// #include "clang/Driver/Arg.h"
// #include "clang/Driver/ArgList.h"
// #include "clang/Driver/Options.h"
// #include "clang/Driver/DriverDiagnostic.h"
// #include "clang/Driver/OptTable.h"
#include "clang/Driver/Driver.h"           // main() but remove?
#include "clang/Driver/Tool.h"
 // includes llvm/Support/Path, clang/Driver/Job. FIXME needed?
#include "clang/Driver/Compilation.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"

#include "clang/FrontendTool/Utils.h" // ExecuteCompilerInvocation

#include "llvm/ADT/Statistic.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ManagedStatic.h"    // llvm_shutdown
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/LinkAllPasses.h"

using namespace assertions;
using namespace clang;
using namespace llvm;

cl::opt<bool> Debug(
  "d",
  cl::desc("Print debugging information for the assertion annotating process.")
);

namespace assertions {
  bool DEBUG = false;
}

// Copied from examples/clang-interpreter/main.cpp

// This function isn't referenced outside its translation unit, but it
// can't use the "static" keyword because its address is used for
// GetMainExecutable (since some platforms don't support taking the
// address of main, and some platforms can't implement GetMainExecutable
// without being given the address of a function in the main executable).
llvm::sys::Path GetExecutablePath(const char *Argv0) {
  // This just needs to be some symbol in the binary; C++ doesn't
  // allow taking the address of ::main however.
  void *MainAddr = (void*) (intptr_t) GetExecutablePath;
  return llvm::sys::Path::GetMainExecutable(Argv0, MainAddr);
}

// So much code duplication from Tooling.cpp :(

/*

// FIXME: This file contains structural duplication with other parts of the
// code that sets up a compiler to run tools on it, and we should refactor
// it to be based on the same framework.

/// \brief Builds a clang driver initialized for running clang tools.
static clang::driver::Driver *newDriver(clang::DiagnosticsEngine *Diagnostics,
                                        StringRef BinaryName) {
  const std::string DefaultOutputName = "a.out";
  clang::driver::Driver *CompilerDriver = new clang::driver::Driver(
    BinaryName, llvm::sys::getDefaultTargetTriple(),
    DefaultOutputName, false, *Diagnostics);
  CompilerDriver->setTitle("clang_based_tool");
  return CompilerDriver;
}

/// \brief Retrieves the clang CC1 specific flags out of the compilation's jobs.
///
/// Returns NULL on error.
static const clang::driver::ArgStringList *getCC1Arguments(
    clang::DiagnosticsEngine *Diagnostics,
    clang::driver::Compilation *Compilation) {
  // We expect to get back exactly one Command job, if we didn't something
  // failed. Extract that job from the Compilation.
  const clang::driver::JobList &Jobs = Compilation->getJobs();
  if (Jobs.size() != 1 || !isa<clang::driver::Command>(*Jobs.begin())) {
    llvm::SmallString<256> error_msg;
    llvm::raw_svector_ostream error_stream(error_msg);
    Compilation->PrintJob(error_stream, Compilation->getJobs(), "; ", true);
    Diagnostics->Report(clang::diag::err_fe_expected_compiler_job)
        << error_stream.str();
    return NULL;
  }

  // The one job we find should be to invoke clang again.
  const clang::driver::Command *Cmd =
      cast<clang::driver::Command>(*Jobs.begin());
  if (StringRef(Cmd->getCreator().getName()) != "clang") {
    Diagnostics->Report(clang::diag::err_fe_expected_clang_command);
    return NULL;
  }

  return &Cmd->getArguments();
}

/// \brief Returns a clang build invocation initialized from the CC1 flags.
static clang::CompilerInvocation *newInvocation(
    clang::DiagnosticsEngine *Diagnostics,
    const clang::driver::ArgStringList &CC1Args) {
  assert(!CC1Args.empty() && "Must at least contain the program name!");
  clang::CompilerInvocation *Invocation = new clang::CompilerInvocation;
  clang::CompilerInvocation::CreateFromArgs(
      *Invocation, CC1Args.data() + 1, CC1Args.data() + CC1Args.size(),
      *Diagnostics);
  Invocation->getFrontendOpts().DisableFree = false;
  return Invocation;
}


static bool runTool(clang::driver::ArgStringList& Argv) {
  // BinaryName was taken from Argv[0] before
  std::string BinaryName = GetExecutablePath("clang_tool").str();
  // From ToolInvocation::run()
  DiagnosticOptions DefaultDiagnosticOptions;
  TextDiagnosticPrinter DiagnosticPrinter(
      llvm::errs(), DefaultDiagnosticOptions);
  DiagnosticsEngine Diagnostics(llvm::IntrusiveRefCntPtr<clang::DiagnosticIDs>(
      new DiagnosticIDs()), &DiagnosticPrinter, false);

  // newDriver!
  const llvm::OwningPtr<clang::driver::Driver> Driver(
      newDriver(&Diagnostics, BinaryName));
  const llvm::OwningPtr<clang::driver::Compilation> Compilation(
      Driver->BuildCompilation(makeArrayRef(Argv)));
  const clang::driver::ArgStringList *const CC1Args = getCC1Arguments(
      &Diagnostics, Compilation.get());
  if (CC1Args == NULL) {
    return false;
  }
  llvm::OwningPtr<clang::CompilerInvocation> Invocation(
      newInvocation(&Diagnostics, *CC1Args));
  return runInvocation(BinaryName, Compilation.get(), Invocation.take(),
                       *CC1Args);
}
*/

ArrayRef<const char *> ExtractCompilationArgs(
      int &argc, char const **argv /*, char const *BinaryName*/) {
  // Load C11 arguments after "--".
  // Copied from FixedCompilationDatabase::loadFromCommandLine
  const char **DoubleDash = std::find(argv, argv + argc, StringRef("--"));
  if (DoubleDash == argv + argc)
    llvm::report_fatal_error(
      "Need driver args, e.g. assertions -- [driver args]");
  // Replace the -- with the BinaryName, we will pass all those arguments to
  // the clang driver.
  // *DoubleDash = BinaryName;
  auto CompilationArgs = makeArrayRef(DoubleDash+1, argv + argc);
  argc = DoubleDash - argv;
  return CompilationArgs;
}

static void LLVMErrorHandler(void *UserData, const std::string &Message) {
  DiagnosticsEngine &Diags = *static_cast<DiagnosticsEngine*>(UserData);

  Diags.Report(diag::err_fe_error_backend) << Message;

  // We cannot recover from llvm errors.
  exit(1);
}

int main(int argc, char const **argv) {
  clang::driver::ArgStringList args(argv, argv + argc);
  // Add a preprocessor definition to indicate we're doing assertions parsing.
  args.push_back("-D");
  args.push_back("__ASSERTIONS_ANALYSER__");

  // Change argc and argv to refer to the vector's memory.
  // The CompilationDatabase will modify these, so we shouldn't pass in
  // args.data() directly.
  argc = (int) args.size();
  assert((size_t) argc == args.size());    // check for overflow

  argv = args.data();

  cl::extrahelp MoreHelp("\nMore help text...");

  // llvm::OwningPtr<CompilationDatabase> Compilations(
  //   FixedCompilationDatabase::loadFromCommandLine(argc, argv));

  void *MainAddr = (void*) (intptr_t) GetExecutablePath;
  std::string BinaryName = GetExecutablePath(argv[0]).str();
  auto Args = ExtractCompilationArgs(argc, argv);

  cl::ParseCommandLineOptions(argc, argv);
  // Set the global DEBUG var from the cmdline flag.
  DEBUG = Debug;

  // XXX
  // CI.getFrontendOpts()    --> FrontendOptions
  // when FrontendOptions is created, ProgramAction = frontend::ParseSyntaxOnly.


  // XXX ToolInvocation approach:

  // // CompilationArgs : ArrayRef<std::string>
  // llvm::OwningPtr<FrontendActionFactory> Factory(
  //   newFrontendActionFactory<AnnotateVariablesAction>());
  // FileManager Files((FileSystemOptions()));
  // ToolInvocation Invocation(CompilationArgs,
  //   Factory->create(), &Files);
  // Invocation.run();

  // ---- Doesn't work because it enforces that only 1 job would run.

  // XXX clang-interpreter/main.cpp approach:

  // DiagnosticOptions DiagOpts;
  // TextDiagnosticPrinter *DiagClient =
  //   new TextDiagnosticPrinter(llvm::errs(), DiagOpts);

  OwningPtr<CompilerInstance> Clang(new CompilerInstance());
  IntrusiveRefCntPtr<DiagnosticIDs> DiagID(new DiagnosticIDs());

  // Initialize targets first, so that --version shows registered targets.
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();

  // Buffer diagnostics from argument parsing so that we can output them using a
  // well formed diagnostic object.
  TextDiagnosticBuffer *DiagsBuffer = new TextDiagnosticBuffer;
  DiagnosticsEngine Diags(DiagID, DiagsBuffer);

  bool Success;

  // start umm...

  // Driver TheDriver(BinaryName, llvm::sys::getDefaultTargetTriple(),
  //                  "a.out", /*IsProduction=*/false, Diags);
  // TheDriver.setTitle("clang + assertions");

  // OwningPtr<Compilation> C(TheDriver.BuildCompilation(Args));
  // if (!C)
  //   return 0;

  // const driver::JobList &Jobs = C->getJobs();

  // const driver::Command *Cmd = cast<driver::Command>(*Jobs.begin());
  // if (llvm::StringRef(Cmd->getCreator().getName()) != "clang") {
  //   Diags.Report(diag::err_fe_expected_clang_command);
  //   return 1;
  // }

  // // Initialize a compiler invocation object from the clang (-cc1) arguments.
  // const driver::ArgStringList &CCArgs = Cmd->getArguments();
  // CompilerInvocation& CI = Clang->getInvocation();
  // CompilerInvocation::CreateFromArgs(CI,
  //                                    const_cast<const char **>(CCArgs.data()),
  //                                    const_cast<const char **>(CCArgs.data()) +
  //                                      CCArgs.size(),
  //                                    Diags);

  // // Show the invocation, with -v.
  // if (CI.getHeaderSearchOpts().Verbose) {
  //   llvm::errs() << "clang invocation:\n";
  //   C->PrintJob(llvm::errs(), C->getJobs(), "\n", true);
  //   llvm::errs() << "\n";
  // }
  // // Create the actual diagnostics engine.
  // Clang->createDiagnostics(int(CCArgs.size()),const_cast<char**>(CCArgs.data()));
  // if (!Clang->hasDiagnostics())
  //   return 1;

  // end umm...
  // why not  cc1_main.cpp:141

  Success = CompilerInvocation::CreateFromArgs(Clang->getInvocation(),
                                               Args.begin(),
                                               Args.end(), Diags);

  // Infer the builtin include path if unspecified.
  if (Clang->getHeaderSearchOpts().UseBuiltinIncludes &&
      Clang->getHeaderSearchOpts().ResourceDir.empty())
    Clang->getHeaderSearchOpts().ResourceDir =
      CompilerInvocation::GetResourcesPath(argv[0], MainAddr);

  // Create the actual diagnostics engine.
  Clang->createDiagnostics(Args.size(), Args.data());
  if (!Clang->hasDiagnostics())
    return 1;

  // Set an error handler, so that any LLVM backend diagnostics go through our
  // error handler.
  llvm::install_fatal_error_handler(LLVMErrorHandler,
                                  static_cast<void*>(&Clang->getDiagnostics()));

  DiagsBuffer->FlushDiagnostics(Clang->getDiagnostics());
  if (!Success)
    return 1;

  //llvm::errs() << theFrontendAction(Clang->getFrontendOpts().ProgramAction) << "\n";


  class MyFactory : public WrapperFrontendActionFactory {
    WrapperFrontendAction *create(FrontendAction *Wrapped) override {
      return new AnnotateVariablesAction(Wrapped);
    }
  } Factory;
  // Execute the frontend actions.
  Success = ExecuteCompilerInvocation(Clang.get(), &Factory);

  // If any timers were active but haven't been destroyed yet, print their
  // results now.  This happens in -disable-free mode.
  llvm::TimerGroup::printAll(llvm::errs());

  // Our error handler depends on the Diagnostics object, which we're
  // potentially about to delete. Uninstall the handler now so that any
  // later errors use the default handling behavior instead.
  llvm::remove_fatal_error_handler();

  // When running with -disable-free, don't do any destruction or shutdown.
  if (Clang->getFrontendOpts().DisableFree) {
    if (llvm::AreStatisticsEnabled() || Clang->getFrontendOpts().ShowStats)
      llvm::PrintStatistics();
    Clang.take();
    return !Success;
  }

  // Managed static deconstruction. Useful for making things like
  // -time-passes usable.
  llvm::llvm_shutdown();

  return !Success;

  // Create and execute the frontend to generate an LLVM bitcode module.
  // OwningPtr<CodeGenAction> Act(new EmitLLVMOnlyAction());
  // if (!Clang.ExecuteAction(*Act))
  //   return 1;
}

// vim:sw=2
