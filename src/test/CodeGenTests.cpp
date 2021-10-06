#include <vector>
#include <sstream>
#include "Dialect.h"
#include "TestUtilsCommon.h"

#include "mlir/IR/Attributes.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/BuiltinOps.h"
#include "mlir/IR/BuiltinTypes.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/IR/Verifier.h"
#include "mlir/Dialect/StandardOps/IR/Ops.h"
#include "mlir/Dialect/SCF/SCF.h"
#include "llvm/ADT/STLExtras.h"

#include "ExecutionHelpers.h"

using namespace mlir;

namespace TreeBeard
{
namespace test
{

bool Test_LoadTileThresholdOp_DoubleInt32_TileSize1(TestArgs_t& args) {
  using TestTileType = decisionforest::TileType<double, int32_t, 1>;
  
  auto& context = args.context;
  mlir::OpBuilder builder(&context);
  auto location = builder.getUnknownLoc();
  auto module = mlir::ModuleOp::create(location, llvm::StringRef("Test_LoadTileThresholdOp_DoubleInt32_TileSize1"));
  
  int64_t length = 5;
  int64_t shape[] = { length };
  auto tileType = decisionforest::TiledNumericalNodeType::get(builder.getF64Type(), builder.getI32Type(), 1);
  auto inputMemrefType=MemRefType::get(shape, tileType);
  auto outputMemrefType = MemRefType::get(shape, builder.getF64Type());
  auto functionType = builder.getFunctionType({inputMemrefType, outputMemrefType}, builder.getI32Type());

  auto func = builder.create<FuncOp>(location, std::string("TestFunction"), functionType, builder.getStringAttr("public"));
  auto &entryBlock = *(func.addEntryBlock());
  builder.setInsertionPointToStart(&entryBlock);

  auto lengthConstant = builder.create<ConstantIndexOp>(location, length); 
  auto zeroConst = builder.create<ConstantIndexOp>(location, 0);
  auto oneIndexConst = builder.create<ConstantIndexOp>(location, 1);
  auto batchLoop = builder.create<scf::ForOp>(location, zeroConst, lengthConstant, oneIndexConst/*, static_cast<Value>(memrefResult)*/);

  builder.setInsertionPointToStart(batchLoop.getBody());
  auto i = batchLoop.getInductionVar();

  auto inputMemref = func.getArgument(0);
  auto outputMemref = func.getArgument(1);
  auto threshold = builder.create<decisionforest::LoadTileThresholdsOp>(location, builder.getF64Type(), static_cast<Value>(inputMemref), static_cast<Value>(i));

  builder.create<memref::StoreOp>(location, TypeRange({ }), static_cast<Value>(threshold), outputMemref, i);

  builder.setInsertionPointAfter(batchLoop);
  auto retVal = builder.create<mlir::ConstantIntOp>(location, 0, builder.getI32Type());
  builder.create<mlir::ReturnOp>(location, static_cast<Value>(retVal));

  module.push_back(func);
  // module.dump();
  decisionforest::LowerToLLVM(context, module);
  // module.dump();

  auto maybeEngine = mlir::decisionforest::InferenceRunner::CreateExecutionEngine(module);
  auto& engine = maybeEngine.get();
  
  // Memref<InputElementType, 2> resultMemref;
  std::vector<TestTileType> tiles = { {1.0, 1}, {2.0, 2}, {3.0, 3}, {4.0, 4}, {5.0, 5} };
  assert (static_cast<size_t>(length) == tiles.size());
  std::vector<double> thresholds(length, -1.0);
  TestTileType *ptr = tiles.data(), *alignedPtr = tiles.data();
  int64_t offset = 0, stride = 1;
  double *resultPtr = thresholds.data(), *resultAlignedPtr = thresholds.data();
  int64_t resultLen = length;
  int32_t returnVal = -1;
  void *funcArgs[] = { &ptr, &alignedPtr, &offset, &length, &stride, // Input memref fields
                       &resultPtr, &resultAlignedPtr, &offset, &resultLen, &stride, // Result memref fields 
                       &returnVal };
  auto invocationResult = engine->invokePacked("TestFunction", funcArgs);
  if (invocationResult) {
    llvm::errs() << "JIT invocation failed\n";
    return -1;
  }
  std::vector<double> expectedThresholds = {1.0, 2.0, 3.0, 4.0, 5.0};
  Test_ASSERT(thresholds == expectedThresholds);
  Test_ASSERT(returnVal == 0);
  return true;
}

bool Test_LoadTileFeatureIndicesOp_DoubleInt32_TileSize1(TestArgs_t& args) {
  using TestTileType = decisionforest::TileType<double, int32_t, 1>;
  
  auto& context = args.context;
  mlir::OpBuilder builder(&context);
  auto location = builder.getUnknownLoc();
  auto module = mlir::ModuleOp::create(location, llvm::StringRef("Test_LoadTileFeatureIndicesOp_DoubleInt32_TileSize1"));
  
  int64_t length = 5;
  int64_t shape[] = { length };
  auto tileType = decisionforest::TiledNumericalNodeType::get(builder.getF64Type(), builder.getI32Type(), 1);
  auto inputMemrefType=MemRefType::get(shape, tileType);
  auto outputMemrefType = MemRefType::get(shape, builder.getI32Type());
  auto functionType = builder.getFunctionType({inputMemrefType, outputMemrefType}, builder.getI32Type());

  auto func = builder.create<FuncOp>(location, std::string("TestFunction"), functionType, builder.getStringAttr("public"));
  auto &entryBlock = *(func.addEntryBlock());
  builder.setInsertionPointToStart(&entryBlock);

  auto lengthConstant = builder.create<ConstantIndexOp>(location, length); 
  auto zeroConst = builder.create<ConstantIndexOp>(location, 0);
  auto oneIndexConst = builder.create<ConstantIndexOp>(location, 1);
  auto batchLoop = builder.create<scf::ForOp>(location, zeroConst, lengthConstant, oneIndexConst/*, static_cast<Value>(memrefResult)*/);

  builder.setInsertionPointToStart(batchLoop.getBody());
  auto i = batchLoop.getInductionVar();

  auto inputMemref = func.getArgument(0);
  auto outputMemref = func.getArgument(1);
  auto featureIndex = builder.create<decisionforest::LoadTileFeatureIndicesOp>(location, builder.getI32Type(), static_cast<Value>(inputMemref), static_cast<Value>(i));

  builder.create<memref::StoreOp>(location, TypeRange({ }), static_cast<Value>(featureIndex), outputMemref, i);

  builder.setInsertionPointAfter(batchLoop);
  auto retVal = builder.create<mlir::ConstantIntOp>(location, 0, builder.getI32Type());
  builder.create<mlir::ReturnOp>(location, static_cast<Value>(retVal));

  module.push_back(func);
  // module.dump();
  decisionforest::LowerToLLVM(context, module);
  // module.dump();

  auto maybeEngine = mlir::decisionforest::InferenceRunner::CreateExecutionEngine(module);
  auto& engine = maybeEngine.get();
  
  // Memref<InputElementType, 2> resultMemref;
  std::vector<TestTileType> tiles = { {0.0, 1}, {0.0, 2}, {0.0, 3}, {0.0, 4}, {0.0, 5} };
  assert (static_cast<size_t>(length) == tiles.size());
  std::vector<int32_t> indices(length, -1);
  TestTileType *ptr = tiles.data(), *alignedPtr = tiles.data();
  int64_t offset = 0, stride = 1;
  int32_t *resultPtr = indices.data(), *resultAlignedPtr = indices.data();
  int64_t resultLen = length;
  int32_t returnVal = -1;
  void *funcArgs[] = { &ptr, &alignedPtr, &offset, &length, &stride, // Input memref fields
                       &resultPtr, &resultAlignedPtr, &offset, &resultLen, &stride, // Result memref fields 
                       &returnVal };
  auto invocationResult = engine->invokePacked("TestFunction", funcArgs);
  if (invocationResult) {
    llvm::errs() << "JIT invocation failed\n";
    return -1;
  }
  std::vector<int32_t> expectedIndices = {1, 2, 3, 4, 5};
  Test_ASSERT(indices == expectedIndices);
  Test_ASSERT(returnVal == 0);
  return true;
}

bool Test_LoadTileThresholdOp_Subview_DoubleInt32_TileSize1(TestArgs_t& args) {
  using TestTileType = decisionforest::TileType<double, int32_t, 1>;
  
  auto& context = args.context;
  mlir::OpBuilder builder(&context);
  auto location = builder.getUnknownLoc();
  auto module = mlir::ModuleOp::create(location, llvm::StringRef("Test_LoadTileThresholdOp_Subview_DoubleInt32_TileSize1"));
  
  int64_t length = 5;
  int64_t shape[] = { length };
  int64_t offset = 1;
  int64_t outputShape[] = { length - offset };
  auto tileType = decisionforest::TiledNumericalNodeType::get(builder.getF64Type(), builder.getI32Type(), 1);
  auto inputMemrefType=MemRefType::get(shape, tileType);
  auto outputMemrefType = MemRefType::get(outputShape, builder.getF64Type());
  auto functionType = builder.getFunctionType({inputMemrefType, outputMemrefType}, builder.getI32Type());

  auto func = builder.create<FuncOp>(location, std::string("TestFunction"), functionType, builder.getStringAttr("public"));
  auto &entryBlock = *(func.addEntryBlock());
  builder.setInsertionPointToStart(&entryBlock);

  auto inputMemref = func.getArgument(0);
  auto outputMemref = func.getArgument(1);

  auto memrefSubview = builder.create<memref::SubViewOp>(location, inputMemref, ArrayRef<OpFoldResult>(builder.getIndexAttr(offset)), ArrayRef<OpFoldResult>(builder.getIndexAttr(length-offset)),
                                                         ArrayRef<OpFoldResult>(builder.getIndexAttr(1)));

  auto lengthConstant = builder.create<ConstantIndexOp>(location, length-offset); 
  auto zeroConst = builder.create<ConstantIndexOp>(location, 0);
  auto oneIndexConst = builder.create<ConstantIndexOp>(location, 1);
  auto batchLoop = builder.create<scf::ForOp>(location, zeroConst, lengthConstant, oneIndexConst/*, static_cast<Value>(memrefResult)*/);

  builder.setInsertionPointToStart(batchLoop.getBody());
  auto i = batchLoop.getInductionVar();

  auto threshold = builder.create<decisionforest::LoadTileThresholdsOp>(location, builder.getF64Type(), static_cast<Value>(memrefSubview), static_cast<Value>(i));

  builder.create<memref::StoreOp>(location, TypeRange({ }), static_cast<Value>(threshold), outputMemref, i);

  builder.setInsertionPointAfter(batchLoop);
  auto retVal = builder.create<mlir::ConstantIntOp>(location, 0, builder.getI32Type());
  builder.create<mlir::ReturnOp>(location, static_cast<Value>(retVal));

  module.push_back(func);
  // module.dump();
  decisionforest::LowerToLLVM(context, module);
  // module.dump();

  auto maybeEngine = mlir::decisionforest::InferenceRunner::CreateExecutionEngine(module);
  auto& engine = maybeEngine.get();
  
  // Memref<InputElementType, 2> resultMemref;
  std::vector<TestTileType> tiles = { {1.0, 1}, {2.0, 2}, {3.0, 3}, {4.0, 4}, {5.0, 5} };
  assert (static_cast<size_t>(length) == tiles.size());
  std::vector<double> thresholds(length - offset, -1.0);
  TestTileType *ptr = tiles.data(), *alignedPtr = tiles.data();
  int64_t memrefOffset = 0, stride = 1;
  double *resultPtr = thresholds.data(), *resultAlignedPtr = thresholds.data();
  int64_t resultLen = length;
  int32_t returnVal = -1;
  void *funcArgs[] = { &ptr, &alignedPtr, &memrefOffset, &length, &stride, // Input memref fields
                       &resultPtr, &resultAlignedPtr, &memrefOffset, &resultLen, &stride, // Result memref fields 
                       &returnVal };
  auto invocationResult = engine->invokePacked("TestFunction", funcArgs);
  if (invocationResult) {
    llvm::errs() << "JIT invocation failed\n";
    return -1;
  }
  std::vector<double> expectedThresholds = {2.0, 3.0, 4.0, 5.0};
  Test_ASSERT(thresholds == expectedThresholds);
  Test_ASSERT(returnVal == 0);
  return true;
}

bool Test_LoadTileFeatureIndicesOp_Subview_DoubleInt32_TileSize1(TestArgs_t& args) {
  using TestTileType = decisionforest::TileType<double, int32_t, 1>;
  
  auto& context = args.context;
  mlir::OpBuilder builder(&context);
  auto location = builder.getUnknownLoc();
  auto module = mlir::ModuleOp::create(location, llvm::StringRef("Test_LoadTileFeatureIndicesOp_Subview_DoubleInt32_TileSize1"));
  
  int64_t length = 5;
  int64_t offset = 1;
  int64_t shape[] = { length };
  int64_t outputShape[] = { length - offset };
  auto tileType = decisionforest::TiledNumericalNodeType::get(builder.getF64Type(), builder.getI32Type(), 1);
  auto inputMemrefType=MemRefType::get(shape, tileType);
  auto outputMemrefType = MemRefType::get(outputShape, builder.getI32Type());
  auto functionType = builder.getFunctionType({inputMemrefType, outputMemrefType}, builder.getI32Type());

  auto func = builder.create<FuncOp>(location, std::string("TestFunction"), functionType, builder.getStringAttr("public"));
  auto &entryBlock = *(func.addEntryBlock());
  builder.setInsertionPointToStart(&entryBlock);

  auto inputMemref = func.getArgument(0);
  auto outputMemref = func.getArgument(1);

  auto memrefSubview = builder.create<memref::SubViewOp>(location, inputMemref, ArrayRef<OpFoldResult>(builder.getIndexAttr(offset)), ArrayRef<OpFoldResult>(builder.getIndexAttr(length-offset)),
                                                         ArrayRef<OpFoldResult>(builder.getIndexAttr(1)));

  auto lengthConstant = builder.create<ConstantIndexOp>(location, length-offset); 
  auto zeroConst = builder.create<ConstantIndexOp>(location, 0);
  auto oneIndexConst = builder.create<ConstantIndexOp>(location, 1);
  auto batchLoop = builder.create<scf::ForOp>(location, zeroConst, lengthConstant, oneIndexConst/*, static_cast<Value>(memrefResult)*/);

  builder.setInsertionPointToStart(batchLoop.getBody());
  auto i = batchLoop.getInductionVar();

  auto featureIndex = builder.create<decisionforest::LoadTileFeatureIndicesOp>(location, builder.getI32Type(), static_cast<Value>(memrefSubview), static_cast<Value>(i));

  builder.create<memref::StoreOp>(location, TypeRange({ }), static_cast<Value>(featureIndex), outputMemref, i);

  builder.setInsertionPointAfter(batchLoop);
  auto retVal = builder.create<mlir::ConstantIntOp>(location, 0, builder.getI32Type());
  builder.create<mlir::ReturnOp>(location, static_cast<Value>(retVal));

  module.push_back(func);
  // module.dump();
  decisionforest::LowerToLLVM(context, module);
  // module.dump();

  auto maybeEngine = mlir::decisionforest::InferenceRunner::CreateExecutionEngine(module);
  auto& engine = maybeEngine.get();
  
  // Memref<InputElementType, 2> resultMemref;
  std::vector<TestTileType> tiles = { {0.0, 1}, {0.0, 2}, {0.0, 3}, {0.0, 4}, {0.0, 5} };
  assert (static_cast<size_t>(length) == tiles.size());
  std::vector<int32_t> indices(length - offset, -1);
  TestTileType *ptr = tiles.data(), *alignedPtr = tiles.data();
  int64_t memrefOffset = 0, stride = 1;
  int32_t *resultPtr = indices.data(), *resultAlignedPtr = indices.data();
  int64_t resultLen = length;
  int32_t returnVal = -1;
  void *funcArgs[] = { &ptr, &alignedPtr, &memrefOffset, &length, &stride, // Input memref fields
                       &resultPtr, &resultAlignedPtr, &memrefOffset, &resultLen, &stride, // Result memref fields 
                       &returnVal };
  auto invocationResult = engine->invokePacked("TestFunction", funcArgs);
  if (invocationResult) {
    llvm::errs() << "JIT invocation failed\n";
    return -1;
  }
  std::vector<int32_t> expectedIndices = {2, 3, 4, 5};
  Test_ASSERT(indices == expectedIndices);
  Test_ASSERT(returnVal == 0);
  return true;
}

}
}