// #include "Passes.h"

#include "mlir/Conversion/AffineToStandard/AffineToStandard.h"
#include "mlir/Conversion/LLVMCommon/ConversionTarget.h"
#include "mlir/Conversion/LLVMCommon/TypeConverter.h"
#include "mlir/Conversion/MemRefToLLVM/MemRefToLLVM.h"
#include "mlir/Conversion/ControlFlowToLLVM/ControlFlowToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVM.h"
#include "mlir/Conversion/FuncToLLVM/ConvertFuncToLLVMPass.h"
#include "mlir/Conversion/SCFToControlFlow/SCFToControlFlow.h"
#include "mlir/Conversion/SCFToOpenMP/SCFToOpenMP.h"
#include "mlir/Conversion/VectorToLLVM/ConvertVectorToLLVM.h"
#include "mlir/Conversion/ArithmeticToLLVM/ArithmeticToLLVM.h"
#include "mlir/Conversion/OpenMPToLLVM/ConvertOpenMPToLLVM.h"
#include "mlir/Conversion/GPUToNVVM/GPUToNVVMPass.h"
#include "mlir/Conversion/AsyncToLLVM/AsyncToLLVM.h"
#include "mlir/Conversion/GPUCommon/GPUCommonPass.h"

#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/LLVMIR/LLVMDialect.h"
#include "mlir/Dialect/LLVMIR/NVVMDialect.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/OpenMP/OpenMPDialect.h"
#include "mlir/Dialect/ControlFlow/IR/ControlFlowOps.h"
#include "mlir/Dialect/GPU/Passes.h"

#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"
#include "mlir/Transforms/DialectConversion.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "llvm/ADT/Sequence.h"

#include "mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h"
#include "mlir/Target/LLVMIR/Export.h"
#include "mlir/Transforms/Passes.h"
#include "mlir/ExecutionEngine/ExecutionEngine.h"
#include "mlir/ExecutionEngine/OptUtils.h"
#include "mlir/Conversion/ReconcileUnrealizedCasts/ReconcileUnrealizedCasts.h"

#include "mlir/Dialect/Math/IR/Math.h"
#include "mlir/Conversion/MathToLLVM/MathToLLVM.h"

#include "Dialect.h"

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/ErrorOr.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/SourceMgr.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm-c/Target.h"
#include "llvm-c/TargetMachine.h"
#include "llvm/MC/TargetRegistry.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/MemoryBufferRef.h"
#include "llvm/Support/MemoryBuffer.h"

using namespace mlir;

namespace
{

template <typename DerivedT>
class ConvertGpuOpsToNVVMOpsBase : public ::mlir::OperationPass<gpu::GPUModuleOp> {
public:
  using Base = ConvertGpuOpsToNVVMOpsBase;

  ConvertGpuOpsToNVVMOpsBase() : ::mlir::OperationPass<gpu::GPUModuleOp>(::mlir::TypeID::get<DerivedT>()) {}
  ConvertGpuOpsToNVVMOpsBase(const ConvertGpuOpsToNVVMOpsBase &other) : ::mlir::OperationPass<gpu::GPUModuleOp>(other) {}

  /// Returns the command-line argument attached to this pass.
  static constexpr ::llvm::StringLiteral getArgumentName() {
    return ::llvm::StringLiteral("convert-gpu-to-nvvm");
  }
  ::llvm::StringRef getArgument() const override { return "convert-gpu-to-nvvm"; }

  ::llvm::StringRef getDescription() const override { return "Generate NVVM operations for gpu operations"; }

  /// Returns the derived pass name.
  static constexpr ::llvm::StringLiteral getPassName() {
    return ::llvm::StringLiteral("ConvertGpuOpsToNVVMOps");
  }
  ::llvm::StringRef getName() const override { return "ConvertGpuOpsToNVVMOps"; }

  /// Support isa/dyn_cast functionality for the derived pass class.
  static bool classof(const ::mlir::Pass *pass) {
    return pass->getTypeID() == ::mlir::TypeID::get<DerivedT>();
  }

  /// A clone method to create a copy of this pass.
  std::unique_ptr<::mlir::Pass> clonePass() const override {
    return std::make_unique<DerivedT>(*static_cast<const DerivedT *>(this));
  }

  /// Return the dialect that must be loaded in the context before this pass.
  void getDependentDialects(::mlir::DialectRegistry &registry) const override {
    registry.insert<memref::MemRefDialect>();
    registry.insert<NVVM::NVVMDialect>();
    // registry.insert<StandardOpsDialect>();
  }

protected:
  ::mlir::Pass::Option<unsigned> indexBitwidth{*this, "index-bitwidth", ::llvm::cl::desc("Bitwidth of the index type, 0 to use size of machine word"), ::llvm::cl::init(0)};
};
/// A pass that replaces all occurrences of GPU device operations with their
/// corresponding NVVM equivalent.
///
/// This pass only handles device code and is not meant to be run on GPU host
/// code.
struct LowerGpuOpsToNVVMOpsPass
    : public ConvertGpuOpsToNVVMOpsBase<LowerGpuOpsToNVVMOpsPass> {
  LowerGpuOpsToNVVMOpsPass() = default;
  LowerGpuOpsToNVVMOpsPass(unsigned indexBitwidth) {
    this->indexBitwidth = indexBitwidth;
  }

  void runOnOperation() override {
    gpu::GPUModuleOp m = getOperation();

    /// Customize the bitwidth used for the device side index computations.
    LowerToLLVMOptions options(
        m.getContext(),
        DataLayout(cast<DataLayoutOpInterface>(m.getOperation())));
    options.emitCWrappers = true;
    if (indexBitwidth != kDeriveIndexBitwidthFromDataLayout)
      options.overrideIndexBitwidth(indexBitwidth);

    /// MemRef conversion for GPU to NVVM lowering. The GPU dialect uses memory
    /// space 5 for private memory attributions, but NVVM represents private
    /// memory allocations as local `alloca`s in the default address space. This
    /// converter drops the private memory space to support the use case above.
    LLVMTypeConverter converter(m.getContext(), options);
    converter.addConversion([&](MemRefType type) -> Optional<Type> {
      if (type.getMemorySpaceAsInt() !=
          gpu::GPUDialect::getPrivateAddressSpace())
        return llvm::None;
      return converter.convertType(MemRefType::Builder(type).setMemorySpace(0));
    });
    /// device-side async tokens cannot be materialized in nvvm. We just convert
    /// them to a dummy i32 type in order to easily drop them during conversion.
    converter.addConversion([&](gpu::DeviceAsyncTokenType type) -> Type {
      return converter.convertType(IntegerType::get(type.getContext(), 32));
    });
    // Lowering for MMAMatrixType.
    converter.addConversion([&](gpu::MMAMatrixType type) -> Type {
      return convertMMAToLLVMType(type);
    });

    RewritePatternSet patterns(m.getContext());
    RewritePatternSet llvmPatterns(m.getContext());

    // Apply in-dialect lowering first. In-dialect lowering will replace ops
    // which need to be lowered further, which is not supported by a single
    // conversion pass.
    populateGpuRewritePatterns(patterns);
    (void)applyPatternsAndFoldGreedily(m, std::move(patterns));

    arith::populateArithmeticToLLVMConversionPatterns(converter, llvmPatterns);
    populateAffineToStdConversionPatterns(llvmPatterns);
    populateSCFToControlFlowConversionPatterns(llvmPatterns);
    cf::populateControlFlowToLLVMConversionPatterns(converter, llvmPatterns);
    populateFuncToLLVMConversionPatterns(converter, llvmPatterns);
    populateMemRefToLLVMConversionPatterns(converter, llvmPatterns);
    populateGpuToNVVMConversionPatterns(converter, llvmPatterns);
    populateGpuWMMAToNVVMConversionPatterns(converter, llvmPatterns);
    decisionforest::populateDecisionTreeToLLVMConversionPatterns(converter, llvmPatterns);

    LLVMConversionTarget target(getContext());
    configureGpuToNVVMConversionLegality(target);
    target.addIllegalDialect<decisionforest::DecisionForestDialect>();

    if (failed(applyPartialConversion(m, target, std::move(llvmPatterns))))
      signalPassFailure();
  }
};

template <typename DerivedT>
class GpuToLLVMConversionPassBase : public ::mlir::OperationPass<ModuleOp> {
public:
  using Base = GpuToLLVMConversionPassBase;

  GpuToLLVMConversionPassBase() : ::mlir::OperationPass<ModuleOp>(::mlir::TypeID::get<DerivedT>()) {}
  GpuToLLVMConversionPassBase(const GpuToLLVMConversionPassBase &other) : ::mlir::OperationPass<ModuleOp>(other) {}

  /// Returns the command-line argument attached to this pass.
  static constexpr ::llvm::StringLiteral getArgumentName() {
    return ::llvm::StringLiteral("gpu-to-llvm");
  }
  ::llvm::StringRef getArgument() const override { return "gpu-to-llvm"; }

  ::llvm::StringRef getDescription() const override { return "Convert GPU dialect to LLVM dialect with GPU runtime calls"; }

  /// Returns the derived pass name.
  static constexpr ::llvm::StringLiteral getPassName() {
    return ::llvm::StringLiteral("GpuToLLVMConversionPass");
  }
  ::llvm::StringRef getName() const override { return "GpuToLLVMConversionPass"; }

  /// Support isa/dyn_cast functionality for the derived pass class.
  static bool classof(const ::mlir::Pass *pass) {
    return pass->getTypeID() == ::mlir::TypeID::get<DerivedT>();
  }

  /// A clone method to create a copy of this pass.
  std::unique_ptr<::mlir::Pass> clonePass() const override {
    return std::make_unique<DerivedT>(*static_cast<const DerivedT *>(this));
  }

  /// Return the dialect that must be loaded in the context before this pass.
  void getDependentDialects(::mlir::DialectRegistry &registry) const override {
    
  registry.insert<LLVM::LLVMDialect>();

  }

  /// Explicitly declare the TypeID for this class. We declare an explicit private
  /// instantiation because Pass classes should only be visible by the current
  /// library.
  MLIR_DEFINE_EXPLICIT_INTERNAL_INLINE_TYPE_ID(GpuToLLVMConversionPassBase<DerivedT>)

protected:
};

class GpuToLLVMConversionPass
    : public GpuToLLVMConversionPassBase<GpuToLLVMConversionPass> {
public:
  GpuToLLVMConversionPass() = default;

  GpuToLLVMConversionPass(const GpuToLLVMConversionPass &other)
      : GpuToLLVMConversionPassBase(other) {}

  // Run the dialect converter on the module.
  void runOnOperation() override;

private:
  Option<std::string> gpuBinaryAnnotation{
      *this, "gpu-binary-annotation",
      llvm::cl::desc("Annotation attribute string for GPU binary"),
      llvm::cl::init(gpu::getDefaultGpuBinaryAnnotation())};
};

void GpuToLLVMConversionPass::runOnOperation() {
  LLVMTypeConverter converter(&getContext());
  RewritePatternSet patterns(&getContext());
  LLVMConversionTarget target(getContext());

  target.addIllegalDialect<gpu::GPUDialect>();
  target.addIllegalDialect<decisionforest::DecisionForestDialect>();

  decisionforest::AddTreebeardTypeConversions(getContext(), converter);

  mlir::arith::populateArithmeticToLLVMConversionPatterns(converter, patterns);
  mlir::cf::populateControlFlowToLLVMConversionPatterns(converter, patterns);
  populateVectorToLLVMConversionPatterns(converter, patterns);
  populateMemRefToLLVMConversionPatterns(converter, patterns);
  populateFuncToLLVMConversionPatterns(converter, patterns);
  populateAsyncStructuralTypeConversionsAndLegality(converter, patterns,
                                                    target);
  populateGpuToLLVMConversionPatterns(converter, patterns, gpuBinaryAnnotation);

  if (failed(
          applyPartialConversion(getOperation(), target, std::move(patterns))))
    signalPassFailure();
}

}

namespace mlir
{
namespace decisionforest
{
void LowerGPUToLLVM(mlir::MLIRContext& context, mlir::ModuleOp module) {
  // llvm::DebugFlag = true;
  // Lower from high-level IR to mid-level IR
  mlir::PassManager pm(&context);
  // pm.addPass(createConvertSCFToCFPass());
  pm.addPass(createGpuKernelOutliningPass());
  // pm.addPass(createMemRefToLLVMPass());
  pm.addNestedPass<gpu::GPUModuleOp>(createStripDebugInfoPass());
  pm.addNestedPass<gpu::GPUModuleOp>(std::make_unique<LowerGpuOpsToNVVMOpsPass>());
  // pm.addNestedPass<gpu::GPUModuleOp>(createGpuToC)
  // pm.addPass(createConvertSCFToCFPass());
  // pm.addPass(std::make_unique<LowerOMPToLLVMPass>());
  // // pm.addPass(std::make_unique<PrintModulePass>());
  pm.addPass(createReconcileUnrealizedCastsPass());
  pm.addPass(std::make_unique<GpuToLLVMConversionPass>());
  pm.addPass(createReconcileUnrealizedCastsPass());
  
  if (mlir::failed(pm.run(module))) {
    llvm::errs() << "Lowering to LLVM failed.\n";
  }
  // module->dump();
}
}
}