//===-- ToolRunner.cpp ----------------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file was developed by the LLVM research group and is distributed under
// the University of Illinois Open Source License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the interfaces described in the ToolRunner.h file.
//
//===----------------------------------------------------------------------===//

#define DEBUG_TYPE "toolrunner"
#include "llvm/Support/ToolRunner.h"
#include "llvm/Config/config.h"   // for HAVE_LINK_R
#include "llvm/System/Program.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FileUtilities.h"
#include <fstream>
#include <sstream>
using namespace llvm;

ToolExecutionError::~ToolExecutionError() throw() { }

/// RunProgramWithTimeout - This function provides an alternate interface to the
/// sys::Program::ExecuteAndWait interface.
/// @see sys:Program::ExecuteAndWait
static int RunProgramWithTimeout(const sys::Path &ProgramPath,
                                 const char **Args,
                                 const sys::Path &StdInFile,
                                 const sys::Path &StdOutFile,
                                 const sys::Path &StdErrFile,
                                 unsigned NumSeconds = 0) {
  const sys::Path* redirects[3];
  redirects[0] = &StdInFile;
  redirects[1] = &StdOutFile;
  redirects[2] = &StdErrFile;

  return
    sys::Program::ExecuteAndWait(ProgramPath, Args, 0, redirects, NumSeconds);
}



static void ProcessFailure(sys::Path ProgPath, const char** Args) {
  std::ostringstream OS;
  OS << "\nError running tool:\n ";
  for (const char **Arg = Args; *Arg; ++Arg)
    OS << " " << *Arg;
  OS << "\n";

  // Rerun the compiler, capturing any error messages to print them.
  sys::Path ErrorFilename("error_messages");
  ErrorFilename.makeUnique();
  RunProgramWithTimeout(ProgPath, Args, sys::Path(""), ErrorFilename,
                        ErrorFilename);

  // Print out the error messages generated by GCC if possible...
  std::ifstream ErrorFile(ErrorFilename.c_str());
  if (ErrorFile) {
    std::copy(std::istreambuf_iterator<char>(ErrorFile),
              std::istreambuf_iterator<char>(),
              std::ostreambuf_iterator<char>(OS));
    ErrorFile.close();
  }

  ErrorFilename.destroyFile();
  throw ToolExecutionError(OS.str());
}

//===---------------------------------------------------------------------===//
// LLI Implementation of AbstractIntepreter interface
//
namespace {
  class LLI : public AbstractInterpreter {
    std::string LLIPath;          // The path to the LLI executable
    std::vector<std::string> ToolArgs; // Args to pass to LLI
  public:
    LLI(const std::string &Path, const std::vector<std::string> *Args)
      : LLIPath(Path) {
      ToolArgs.clear ();
      if (Args) { ToolArgs = *Args; }
    }

    virtual int ExecuteProgram(const std::string &Bytecode,
                               const std::vector<std::string> &Args,
                               const std::string &InputFile,
                               const std::string &OutputFile,
                               const std::vector<std::string> &SharedLibs =
                               std::vector<std::string>(),
                               unsigned Timeout = 0);
  };
}

int LLI::ExecuteProgram(const std::string &Bytecode,
                        const std::vector<std::string> &Args,
                        const std::string &InputFile,
                        const std::string &OutputFile,
                        const std::vector<std::string> &SharedLibs,
                        unsigned Timeout) {
  if (!SharedLibs.empty())
    throw ToolExecutionError("LLI currently does not support "
                             "loading shared libraries.");

  std::vector<const char*> LLIArgs;
  LLIArgs.push_back(LLIPath.c_str());
  LLIArgs.push_back("-force-interpreter=true");

  // Add any extra LLI args.
  for (unsigned i = 0, e = ToolArgs.size(); i != e; ++i)
    LLIArgs.push_back(ToolArgs[i].c_str());

  LLIArgs.push_back(Bytecode.c_str());
  // Add optional parameters to the running program from Argv
  for (unsigned i=0, e = Args.size(); i != e; ++i)
    LLIArgs.push_back(Args[i].c_str());
  LLIArgs.push_back(0);

  std::cout << "<lli>" << std::flush;
  DEBUG(std::cerr << "\nAbout to run:\t";
        for (unsigned i=0, e = LLIArgs.size()-1; i != e; ++i)
          std::cerr << " " << LLIArgs[i];
        std::cerr << "\n";
        );
  return RunProgramWithTimeout(sys::Path(LLIPath), &LLIArgs[0],
      sys::Path(InputFile), sys::Path(OutputFile), sys::Path(OutputFile),
      Timeout);
}

// LLI create method - Try to find the LLI executable
AbstractInterpreter *AbstractInterpreter::createLLI(const std::string &ProgPath,
                                                    std::string &Message,
                                     const std::vector<std::string> *ToolArgs) {
  std::string LLIPath = FindExecutable("lli", ProgPath).toString();
  if (!LLIPath.empty()) {
    Message = "Found lli: " + LLIPath + "\n";
    return new LLI(LLIPath, ToolArgs);
  }

  Message = "Cannot find `lli' in executable directory or PATH!\n";
  return 0;
}

//===----------------------------------------------------------------------===//
// LLC Implementation of AbstractIntepreter interface
//
void LLC::OutputAsm(const std::string &Bytecode, sys::Path &OutputAsmFile) {
  sys::Path uniqueFile(Bytecode+".llc.s");
  uniqueFile.makeUnique();
  OutputAsmFile = uniqueFile;
  std::vector<const char *> LLCArgs;
  LLCArgs.push_back (LLCPath.c_str());

  // Add any extra LLC args.
  for (unsigned i = 0, e = ToolArgs.size(); i != e; ++i)
    LLCArgs.push_back(ToolArgs[i].c_str());

  LLCArgs.push_back ("-o");
  LLCArgs.push_back (OutputAsmFile.c_str()); // Output to the Asm file
  LLCArgs.push_back ("-f");                  // Overwrite as necessary...
  LLCArgs.push_back (Bytecode.c_str());      // This is the input bytecode
  LLCArgs.push_back (0);

  std::cout << "<llc>" << std::flush;
  DEBUG(std::cerr << "\nAbout to run:\t";
        for (unsigned i=0, e = LLCArgs.size()-1; i != e; ++i)
          std::cerr << " " << LLCArgs[i];
        std::cerr << "\n";
        );
  if (RunProgramWithTimeout(sys::Path(LLCPath), &LLCArgs[0],
                            sys::Path(), sys::Path(), sys::Path()))
    ProcessFailure(sys::Path(LLCPath), &LLCArgs[0]);
}

void LLC::compileProgram(const std::string &Bytecode) {
  sys::Path OutputAsmFile;
  OutputAsm(Bytecode, OutputAsmFile);
  OutputAsmFile.destroyFile();
}

int LLC::ExecuteProgram(const std::string &Bytecode,
                        const std::vector<std::string> &Args,
                        const std::string &InputFile,
                        const std::string &OutputFile,
                        const std::vector<std::string> &SharedLibs,
                        unsigned Timeout) {

  sys::Path OutputAsmFile;
  OutputAsm(Bytecode, OutputAsmFile);
  FileRemover OutFileRemover(OutputAsmFile);

  // Assuming LLC worked, compile the result with GCC and run it.
  return gcc->ExecuteProgram(OutputAsmFile.toString(), Args, GCC::AsmFile,
                             InputFile, OutputFile, SharedLibs, Timeout);
}

/// createLLC - Try to find the LLC executable
///
LLC *AbstractInterpreter::createLLC(const std::string &ProgramPath,
                                    std::string &Message,
                                    const std::vector<std::string> *Args) {
  std::string LLCPath = FindExecutable("llc", ProgramPath).toString();
  if (LLCPath.empty()) {
    Message = "Cannot find `llc' in executable directory or PATH!\n";
    return 0;
  }

  Message = "Found llc: " + LLCPath + "\n";
  GCC *gcc = GCC::create(ProgramPath, Message);
  if (!gcc) {
    std::cerr << Message << "\n";
    exit(1);
  }
  return new LLC(LLCPath, gcc, Args);
}

//===---------------------------------------------------------------------===//
// JIT Implementation of AbstractIntepreter interface
//
namespace {
  class JIT : public AbstractInterpreter {
    std::string LLIPath;          // The path to the LLI executable
    std::vector<std::string> ToolArgs; // Args to pass to LLI
  public:
    JIT(const std::string &Path, const std::vector<std::string> *Args)
      : LLIPath(Path) {
      ToolArgs.clear ();
      if (Args) { ToolArgs = *Args; }
    }

    virtual int ExecuteProgram(const std::string &Bytecode,
                               const std::vector<std::string> &Args,
                               const std::string &InputFile,
                               const std::string &OutputFile,
                               const std::vector<std::string> &SharedLibs =
                               std::vector<std::string>(), unsigned Timeout =0);
  };
}

int JIT::ExecuteProgram(const std::string &Bytecode,
                        const std::vector<std::string> &Args,
                        const std::string &InputFile,
                        const std::string &OutputFile,
                        const std::vector<std::string> &SharedLibs,
                        unsigned Timeout) {
  // Construct a vector of parameters, incorporating those from the command-line
  std::vector<const char*> JITArgs;
  JITArgs.push_back(LLIPath.c_str());
  JITArgs.push_back("-force-interpreter=false");

  // Add any extra LLI args.
  for (unsigned i = 0, e = ToolArgs.size(); i != e; ++i)
    JITArgs.push_back(ToolArgs[i].c_str());

  for (unsigned i = 0, e = SharedLibs.size(); i != e; ++i) {
    JITArgs.push_back("-load");
    JITArgs.push_back(SharedLibs[i].c_str());
  }
  JITArgs.push_back(Bytecode.c_str());
  // Add optional parameters to the running program from Argv
  for (unsigned i=0, e = Args.size(); i != e; ++i)
    JITArgs.push_back(Args[i].c_str());
  JITArgs.push_back(0);

  std::cout << "<jit>" << std::flush;
  DEBUG(std::cerr << "\nAbout to run:\t";
        for (unsigned i=0, e = JITArgs.size()-1; i != e; ++i)
          std::cerr << " " << JITArgs[i];
        std::cerr << "\n";
        );
  DEBUG(std::cerr << "\nSending output to " << OutputFile << "\n");
  return RunProgramWithTimeout(sys::Path(LLIPath), &JITArgs[0],
      sys::Path(InputFile), sys::Path(OutputFile), sys::Path(OutputFile),
      Timeout);
}

/// createJIT - Try to find the LLI executable
///
AbstractInterpreter *AbstractInterpreter::createJIT(const std::string &ProgPath,
                   std::string &Message, const std::vector<std::string> *Args) {
  std::string LLIPath = FindExecutable("lli", ProgPath).toString();
  if (!LLIPath.empty()) {
    Message = "Found lli: " + LLIPath + "\n";
    return new JIT(LLIPath, Args);
  }

  Message = "Cannot find `lli' in executable directory or PATH!\n";
  return 0;
}

void CBE::OutputC(const std::string &Bytecode, sys::Path& OutputCFile) {
  sys::Path uniqueFile(Bytecode+".cbe.c");
  uniqueFile.makeUnique();
  OutputCFile = uniqueFile;
  std::vector<const char *> LLCArgs;
  LLCArgs.push_back (LLCPath.c_str());

  // Add any extra LLC args.
  for (unsigned i = 0, e = ToolArgs.size(); i != e; ++i)
    LLCArgs.push_back(ToolArgs[i].c_str());

  LLCArgs.push_back ("-o");
  LLCArgs.push_back (OutputCFile.c_str());   // Output to the C file
  LLCArgs.push_back ("-march=c");            // Output C language
  LLCArgs.push_back ("-f");                  // Overwrite as necessary...
  LLCArgs.push_back (Bytecode.c_str());      // This is the input bytecode
  LLCArgs.push_back (0);

  std::cout << "<cbe>" << std::flush;
  DEBUG(std::cerr << "\nAbout to run:\t";
        for (unsigned i=0, e = LLCArgs.size()-1; i != e; ++i)
          std::cerr << " " << LLCArgs[i];
        std::cerr << "\n";
        );
  if (RunProgramWithTimeout(LLCPath, &LLCArgs[0], sys::Path(), sys::Path(),
                            sys::Path()))
    ProcessFailure(LLCPath, &LLCArgs[0]);
}

void CBE::compileProgram(const std::string &Bytecode) {
  sys::Path OutputCFile;
  OutputC(Bytecode, OutputCFile);
  OutputCFile.destroyFile();
}

int CBE::ExecuteProgram(const std::string &Bytecode,
                        const std::vector<std::string> &Args,
                        const std::string &InputFile,
                        const std::string &OutputFile,
                        const std::vector<std::string> &SharedLibs,
                        unsigned Timeout) {
  sys::Path OutputCFile;
  OutputC(Bytecode, OutputCFile);

  FileRemover CFileRemove(OutputCFile);

  return gcc->ExecuteProgram(OutputCFile.toString(), Args, GCC::CFile,
                             InputFile, OutputFile, SharedLibs, Timeout);
}

/// createCBE - Try to find the 'llc' executable
///
CBE *AbstractInterpreter::createCBE(const std::string &ProgramPath,
                                    std::string &Message,
                                    const std::vector<std::string> *Args) {
  sys::Path LLCPath = FindExecutable("llc", ProgramPath);
  if (LLCPath.isEmpty()) {
    Message =
      "Cannot find `llc' in executable directory or PATH!\n";
    return 0;
  }

  Message = "Found llc: " + LLCPath.toString() + "\n";
  GCC *gcc = GCC::create(ProgramPath, Message);
  if (!gcc) {
    std::cerr << Message << "\n";
    exit(1);
  }
  return new CBE(LLCPath, gcc, Args);
}

//===---------------------------------------------------------------------===//
// GCC abstraction
//
int GCC::ExecuteProgram(const std::string &ProgramFile,
                        const std::vector<std::string> &Args,
                        FileType fileType,
                        const std::string &InputFile,
                        const std::string &OutputFile,
                        const std::vector<std::string> &SharedLibs,
                        unsigned Timeout) {
  std::vector<const char*> GCCArgs;

  GCCArgs.push_back(GCCPath.c_str());

  // Specify the shared libraries to link in...
  for (unsigned i = 0, e = SharedLibs.size(); i != e; ++i)
    GCCArgs.push_back(SharedLibs[i].c_str());

  // Specify -x explicitly in case the extension is wonky
  GCCArgs.push_back("-x");
  if (fileType == CFile) {
    GCCArgs.push_back("c");
    GCCArgs.push_back("-fno-strict-aliasing");
  } else {
    GCCArgs.push_back("assembler");
  }
  GCCArgs.push_back(ProgramFile.c_str());  // Specify the input filename...
  GCCArgs.push_back("-o");
  sys::Path OutputBinary (ProgramFile+".gcc.exe");
  OutputBinary.makeUnique();
  GCCArgs.push_back(OutputBinary.c_str()); // Output to the right file...
  GCCArgs.push_back("-lm");                // Hard-code the math library...
  GCCArgs.push_back("-O2");                // Optimize the program a bit...
#if defined (HAVE_LINK_R)
  GCCArgs.push_back("-Wl,-R.");            // Search this dir for .so files
#endif
  GCCArgs.push_back(0);                    // NULL terminator

  std::cout << "<gcc>" << std::flush;
  if (RunProgramWithTimeout(GCCPath, &GCCArgs[0], sys::Path(), sys::Path(),
        sys::Path())) {
    ProcessFailure(GCCPath, &GCCArgs[0]);
    exit(1);
  }

  std::vector<const char*> ProgramArgs;

  ProgramArgs.push_back(OutputBinary.c_str());
  // Add optional parameters to the running program from Argv
  for (unsigned i=0, e = Args.size(); i != e; ++i)
    ProgramArgs.push_back(Args[i].c_str());
  ProgramArgs.push_back(0);                // NULL terminator

  // Now that we have a binary, run it!
  std::cout << "<program>" << std::flush;
  DEBUG(std::cerr << "\nAbout to run:\t";
        for (unsigned i=0, e = ProgramArgs.size()-1; i != e; ++i)
          std::cerr << " " << ProgramArgs[i];
        std::cerr << "\n";
        );

  FileRemover OutputBinaryRemover(OutputBinary);
  return RunProgramWithTimeout(OutputBinary, &ProgramArgs[0],
      sys::Path(InputFile), sys::Path(OutputFile), sys::Path(OutputFile),
      Timeout);
}

int GCC::MakeSharedObject(const std::string &InputFile, FileType fileType,
                          std::string &OutputFile) {
  sys::Path uniqueFilename(InputFile+LTDL_SHLIB_EXT);
  uniqueFilename.makeUnique();
  OutputFile = uniqueFilename.toString();

  // Compile the C/asm file into a shared object
  const char* GCCArgs[] = {
    GCCPath.c_str(),
    "-x", (fileType == AsmFile) ? "assembler" : "c",
    "-fno-strict-aliasing",
    InputFile.c_str(),           // Specify the input filename...
#if defined(sparc) || defined(__sparc__) || defined(__sparcv9)
    "-G",                        // Compile a shared library, `-G' for Sparc
#elif (defined(__POWERPC__) || defined(__ppc__)) && defined(__APPLE__)
    "-single_module",            // link all source files into a single module
    "-dynamiclib",               // `-dynamiclib' for MacOS X/PowerPC
    "-undefined",                // in data segment, rather than generating
    "dynamic_lookup",            // blocks. dynamic_lookup requires that you set
                                 // MACOSX_DEPLOYMENT_TARGET=10.3 in your env.
#else
    "-shared",                   // `-shared' for Linux/X86, maybe others
#endif

#if defined(__ia64__) || defined(__alpha__)
    "-fPIC",                     // IA64 requires shared objs to contain PIC
#endif
    "-o", OutputFile.c_str(),    // Output to the right filename...
    "-O2",                       // Optimize the program a bit...
    0
  };

  std::cout << "<gcc>" << std::flush;
  if (RunProgramWithTimeout(GCCPath, GCCArgs, sys::Path(), sys::Path(),
                            sys::Path())) {
    ProcessFailure(GCCPath, GCCArgs);
    return 1;
  }
  return 0;
}

/// create - Try to find the `gcc' executable
///
GCC *GCC::create(const std::string &ProgramPath, std::string &Message) {
  sys::Path GCCPath = FindExecutable("gcc", ProgramPath);
  if (GCCPath.isEmpty()) {
    Message = "Cannot find `gcc' in executable directory or PATH!\n";
    return 0;
  }

  Message = "Found gcc: " + GCCPath.toString() + "\n";
  return new GCC(GCCPath);
}
