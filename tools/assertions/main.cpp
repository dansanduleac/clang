#include "AnnotateVariablesAction.h"
#include "Flags.h"

#include "llvm/ADT/ArrayRef.h"
#include "llvm/Support/CommandLine.h"
// #include "llvm/Support/PathV1.h"           // for GetMainExecutable
// for getDefaultTargetTriple, replace it with?..
// #include "llvm/Support/Host.h"

// Why not these... meh never used in cc1_main either! FIXME remove
// #include "clang/Driver/Arg.h"
// #include "clang/Driver/ArgList.h"
// #include "clang/Driver/DriverDiagnostic.h"
// #include "clang/Driver/OptTable.h"
// #include "clang/Driver/Options.h"
// llvm/Option/Option.h also acceptable for this
#include "clang/Driver/Util.h" // ArgStringList
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Frontend/FrontendDiagnostic.h"
#include "clang/Frontend/TextDiagnosticBuffer.h"
#include "clang/Frontend/TextDiagnosticPrinter.h"

#include "clang/FrontendTool/Utils.h" // ExecuteCompilerInvocation

#include "llvm/ADT/Statistic.h"
#include "llvm/LinkAllPasses.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/ManagedStatic.h"    // llvm_shutdown(_obj)?
#include "llvm/Support/Signals.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/Timer.h"
#include "llvm/Support/raw_ostream.h"

using namespace assertions;
using namespace clang;
using namespace llvm;

DebugLvl DEBUG;

cl::opt<DebugLvl, true> Debug(
  "d",
  cl::desc("Debug the assertion annotating process"),
  cl::values(
    clEnumValN(DebugLvl::None, "none", "No debug info is printed"),
    clEnumValN(DebugLvl::Basic, "basic", "Info about tool action"),
    clEnumValN(DebugLvl::All, "all", "Also dump AST nodes"),
    clEnumValEnd
  ),
  cl::location(DEBUG)
);



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

static void LLVMErrorHandler(void *UserData, const std::string &Message,
                             bool GenCrashDiag) {
  DiagnosticsEngine &Diags = *static_cast<DiagnosticsEngine*>(UserData);

  Diags.Report(diag::err_fe_error_backend) << Message;

    // Run the interrupt handlers to make sure any special cleanups get done, in
  // particular that we remove files registered with RemoveFileOnSignal.
  llvm::sys::RunInterruptHandlers();

  // We cannot recover from llvm errors.  When reporting a fatal error, exit
  // with status 70 to generate crash diagnostics.  For BSD systems this is
  // defined as an internal software error.  Otherwise, exit with status 1.
  exit(GenCrashDiag ? 70 : 1);
}

int my_cc1_main(ArrayRef<const char *> Args, const char *Argv0) {
  void *MainAddr = (void*) (intptr_t) GetExecutablePath;
  OwningPtr<CompilerInstance> Clang(new CompilerInstance());
  IntrusiveRefCntPtr<DiagnosticIDs> DiagID(new DiagnosticIDs());

  // Initialize targets first, so that --version shows registered targets.
  llvm::InitializeAllTargets();
  llvm::InitializeAllTargetMCs();
  llvm::InitializeAllAsmPrinters();
  llvm::InitializeAllAsmParsers();

  // Buffer diagnostics from argument parsing so that we can output them using a
  // well formed diagnostic object.
  IntrusiveRefCntPtr<DiagnosticOptions> DiagOpts = new DiagnosticOptions();
  TextDiagnosticBuffer *DiagsBuffer = new TextDiagnosticBuffer;
  DiagnosticsEngine Diags(DiagID, &*DiagOpts, DiagsBuffer);

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
      CompilerInvocation::GetResourcesPath(Argv0, MainAddr);

  // Create the actual diagnostics engine.
  Clang->createDiagnostics();
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

  // TODO this following section is all that differs compared to cc1_main.cpp:main
  // We should push this functionality to a custom ExecuteCompilerInvocation
  {
    class MyFactory : public WrapperFrontendActionFactory {
      WrapperFrontendAction *create(FrontendAction *Wrapped) override {
        return new AnnotateVariablesAction(Wrapped);
      }
    } Factory;
    // Execute the frontend actions.
    Success = ExecuteCompilerInvocation(Clang.get(), &Factory);
  }
  // END section

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

  llvm_shutdown();

  return !Success;
}

int main(int argc, char const **argv) {
  sys::PrintStackTraceOnErrorSignal();
  llvm::PrettyStackTraceProgram X(argc, argv);

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

  std::string BinaryName = GetExecutablePath(argv[0]).str();
  auto Args = ExtractCompilationArgs(argc, argv);

  cl::ParseCommandLineOptions(argc, argv);
  // Set the global DEBUG var from the cmdline flag.
  DEBUG = Debug;

  return my_cc1_main(Args, argv[0]);
}

// vim:sw=2
