//===------------------------------------------------------------*- C++ -*-===//
//
// This file is distributed under MIT License. See LICENSE.txt for details.
//
//===----------------------------------------------------------------------===//
//
// Copyright (c) 2017 University of Kaiserslautern.
//

#include "llvm/Support/CommandLine.h"
#include "ExprAnalyzer/FPExprAnalyzer.h"
#include "CodeGen/FPExprLibGenerator.h"
#include "CodeGen/FPExprCodeGenerator.h"
#include "IRGen/FPIRGenerator.h"
#include "Utils/fpa_util.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/MCJIT.h"
#include "llvm/Support/TargetSelect.h"
#include <nlopt.h>
#include <Optimizer/NLoptOptimizer.h>
#include <iomanip>

typedef std::numeric_limits<double> dbl;

enum goSATMode {
    kUndefinedMode = 0,
    kFormulaAnalysis,
    kCCodeGeneration,
    kNativeSolving
};

enum goSATAlgorithm{
    kUndefinedAlg = 0,
    kCRS2 = NLOPT_GN_CRS2_LM,
    kISRES = NLOPT_GN_ISRES,
    kMLSL = NLOPT_G_MLSL,
    kDirect = NLOPT_GN_DIRECT_L
};

llvm::cl::OptionCategory
        SolverCategory("Solver Options", "Options for controlling FP solver.");

static llvm::cl::opt<std::string>
        opt_input_file(llvm::cl::Required,
                      "f",
                      llvm::cl::desc("path to SMT file"),
                      llvm::cl::value_desc("filename"),
                      llvm::cl::cat(SolverCategory));

static llvm::cl::opt<goSATMode>
        opt_tool_mode("mode", llvm::cl::Optional,
                     llvm::cl::desc("Tool operation mode:"),
                     llvm::cl::cat(SolverCategory),
                     llvm::cl::values(clEnumValN(kNativeSolving,
                                                 "go",
                                                 "SMT solving for FP (default) "),
                                      clEnumValN(kFormulaAnalysis,
                                                 "fa",
                                                 "formula analysis"),
                                      clEnumValN(kCCodeGeneration,
                                                 "cg",
                                                 "C code generation"),
                                      clEnumValEnd));

static llvm::cl::opt<gosat::LibAPIGenMode>
        opt_api_dump_mode("fmt", llvm::cl::Optional,
                      llvm::cl::desc("API dump format in code generation mode:"),
                      llvm::cl::cat(SolverCategory),
                      llvm::cl::values(clEnumValN(gosat::kPlainAPI,
                                                  "plain",
                                                  "CSV dump of API (default)"),
                                       clEnumValN(gosat::kCppAPI,
                                                  "cpp",
                                                  "C++ dump of API"),
                                       clEnumValEnd));

static llvm::cl::opt<goSATAlgorithm>
        opt_go_algorithm("alg",llvm::cl::Optional,
                     llvm::cl::desc("Applied GO algorithm:"),
                     llvm::cl::cat(SolverCategory),
                     llvm::cl::values(clEnumValN(kDirect,
                                                 "direct",
                                                 "Direct algorithm"),
                                      clEnumValN(kCRS2,
                                                 "crs2",
                                                 "CRS2 algorithm (default)"),
                                      clEnumValN(kISRES,
                                                 "isres",
                                                 "ISRES algorithm"),
                                      clEnumValN(kMLSL,
                                                 "mlsl",
                                                 "MLSL algorithm"),
                                      clEnumValEnd));

void versionPrinter(void) {
    std::cout << "goSAT v0.1 \n"
              << "Copyright (c) 2017 University of Kaiserslautern\n";
}

inline float elapsedTimeFrom(std::chrono::steady_clock::time_point &st_time) {
    const auto res = std::chrono::duration_cast<std::chrono::milliseconds>
            (std::chrono::steady_clock::now() - st_time).count();
    return static_cast<float>(res) / 1000;
}


int main(int argc, const char **argv) {
    llvm::cl::SetVersionPrinter(versionPrinter);
    llvm::cl::HideUnrelatedOptions(SolverCategory);
    llvm::cl::ParseCommandLineOptions
            (argc, argv, "goSAT v0.1 Copyright (c) 2017 University of Kaiserslautern\n");
    try {
        z3::context cont;
        z3::expr expr = cont.parse_file(opt_input_file.c_str());
        if (opt_tool_mode == kFormulaAnalysis) {
            gosat::FPExprAnalyzer analyzer;
            analyzer.analyze(expr);
            analyzer.prettyPrintSummary(gosat::FPExprCodeGenerator::getFuncNameFrom(opt_input_file));
            return 0;
        }
        if (opt_tool_mode == kCCodeGeneration) {
            using namespace gosat;
            FPExprCodeGenerator code_generator;
            std::string func_name =
                    FPExprCodeGenerator::getFuncNameFrom(opt_input_file);
            code_generator.genFuncCode(func_name, expr);
            if (opt_api_dump_mode != kUnsetAPI) {
                FPExprLibGenerator lib_generator;
                // TODO: implement error handling for output files
                lib_generator.init(opt_api_dump_mode);
                auto func_sig =
                        FPExprCodeGenerator::genFuncSignature(func_name);
                lib_generator.appendFunction
                        (code_generator.getVarCount(),
                         func_name,
                         func_sig,
                         code_generator.getFuncCode());
                std::cout << func_name << ". code generated successfully!" << "\n";
            } else {
                std::cout << code_generator.getFuncCode() << "\n";
            }
            return 0;
        }
        std::chrono::steady_clock::time_point
                time_start = std::chrono::steady_clock::now();
        using namespace llvm;
        std::string func_name = gosat::FPExprCodeGenerator::getFuncNameFrom(opt_input_file);

        // JIT formula to a function
        InitializeNativeTarget();
        InitializeNativeTargetAsmPrinter();
        atexit(llvm_shutdown);
        LLVMContext context;
        std::unique_ptr<Module> module = std::make_unique<Module>(StringRef(func_name), context);
        gosat::FPIRGenerator ir_gen(&context, module.get());
        auto ll_func_ptr = ir_gen.genFunction(expr);
        std::string err_str;
        std::unique_ptr<ExecutionEngine> exec_engine(
                EngineBuilder(std::move(module))
                        .setEngineKind(EngineKind::JIT)
                        .setOptLevel(CodeGenOpt::Less)
                        .setErrorStr(&err_str)
                        .create());
        if (exec_engine == nullptr) {
            std::cerr << func_name << ": Failed to construct ExecutionEngine: " << err_str
                      << "\n";
            return 1;
        }
        exec_engine->addGlobalMapping(ir_gen.getDistanceFunction(), (void *)fp64_dis);
        exec_engine->finalizeObject();
        auto func_ptr = reinterpret_cast<nlopt_func>(exec_engine->getPointerToFunction(ll_func_ptr));

        // Now working with optimization backend
        goSATAlgorithm current_alg = (opt_go_algorithm == kUndefinedAlg) ? kCRS2: opt_go_algorithm;

        gosat::NLoptOptimizer nl_opt(static_cast<nlopt_algorithm>(current_alg));
        int status = 0;
        double minima = 1.0; /* minimum value */
        std::vector<double> x(ir_gen.getVarCount(), 0.0);
        status = nl_opt.optimize(func_ptr, ir_gen.getVarCount(), x.data(), &minima);
        if (status < 0) {
            std::cout << std::setprecision(4);
            std::cout << func_name << ",error,"
                      << elapsedTimeFrom(time_start) << ",INF," << status << std::endl;
        } else {
            std::string result = (minima == 0) ? "sat" : "unsat";
            std::cout << std::setprecision(4);
            std::cout << func_name << "," << result << ",";
            std::cout << elapsedTimeFrom(time_start) << ",";
            std::cout << std::setprecision(dbl::digits10) << minima
                      << "," << status << std::endl;
        }
    } catch (z3::exception &exp) {
        std::cerr << "Error while parsing SMTLIB file: "
                  << exp.msg() << "\n";
    }
    return 0;
}
