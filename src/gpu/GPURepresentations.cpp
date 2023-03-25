#include "Dialect.h"
#include "OpLoweringUtils.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/IR/BlockAndValueMapping.h"
#include "GPURepresentations.h"
#include "LIRLoweringHelpers.h"

using namespace mlir;
using namespace mlir::decisionforest::helpers;

namespace mlir
{
namespace decisionforest
{
// ===---------------------------------------------------=== //
// Helpers
// ===---------------------------------------------------=== //

void GenerateSimpleInitializer(const std::string& funcName, ConversionPatternRewriter &rewriter, Location location, 
                               ModuleOp module, MemRefType memrefType) {
  // TODO why did this not work when I used the rewriter instead of the builder?
  // auto insertPoint = rewriter.saveInsertionPoint();
  auto functionType = FunctionType::get(rewriter.getContext(), {memrefType}, {memrefType});
  NamedAttribute visibilityAttribute{module.getSymVisibilityAttrName(), rewriter.getStringAttr("public")};
  auto initFunc = func::FuncOp::create(location, funcName, functionType, ArrayRef<NamedAttribute>(visibilityAttribute));
  auto &entryBlock = *initFunc.addEntryBlock();
  // rewriter.setInsertionPointToStart(&entryBlock);
  mlir::OpBuilder builder(initFunc.getContext());
  builder.setInsertionPointToStart(&entryBlock);
  auto waitOp = builder.create<gpu::WaitOp>(location, gpu::AsyncTokenType::get(module.getContext()), ValueRange{});
  auto alloc = builder.create<gpu::AllocOp>(location, memrefType, waitOp.getAsyncToken().getType(), ValueRange{waitOp.getAsyncToken()}, ValueRange{}, ValueRange{});
  auto transfer = builder.create<gpu::MemcpyOp>(location, alloc.getAsyncToken().getType(), ValueRange{alloc.getAsyncToken()}, 
                                                      alloc.getMemref(), static_cast<Value>(initFunc.getArgument(0)));
  /*auto waitBeforeReturn =*/ builder.create<gpu::WaitOp>(location, gpu::AsyncTokenType::get(module.getContext()), ValueRange{transfer.getAsyncToken()});
  builder.create<mlir::func::ReturnOp>(location, static_cast<Value>(alloc.getMemref()));
  module.push_back(initFunc);
  // rewriter.setInsertionPoint(insertPoint.getBlock(), insertPoint.getPoint());
}

void GenerateCleanupProc(const std::string& funcName,
                         ConversionPatternRewriter &rewriter,
                         Location location, 
                         ModuleOp module,
                         const std::vector<Type>& memrefTypes) {
  auto functionType = FunctionType::get(rewriter.getContext(), memrefTypes, {rewriter.getI32Type()});
  NamedAttribute visibilityAttribute{module.getSymVisibilityAttrName(), rewriter.getStringAttr("public")};
  auto initFunc = func::FuncOp::create(location, funcName, functionType, ArrayRef<NamedAttribute>(visibilityAttribute));
  auto &entryBlock = *initFunc.addEntryBlock();
  // rewriter.setInsertionPointToStart(&entryBlock);
  mlir::OpBuilder builder(initFunc.getContext());
  builder.setInsertionPointToStart(&entryBlock);
  auto waitOp = builder.create<gpu::WaitOp>(location, gpu::AsyncTokenType::get(module.getContext()), ValueRange{});
  auto asyncToken = waitOp.getAsyncToken();
  for (size_t i=0 ; i<memrefTypes.size() ; ++i) {
    auto dealloc = builder.create<gpu::DeallocOp>(location, asyncToken.getType(), ValueRange{asyncToken}, initFunc.getArgument(i));
    asyncToken = dealloc.getAsyncToken();
  }

  /*auto waitBeforeReturn =*/ builder.create<gpu::WaitOp>(location, asyncToken.getType(), ValueRange{asyncToken});
  auto constRetVal = builder.create<arith::ConstantIntOp>(location, 0 /*value*/, 32 /*width*/);
  builder.create<mlir::func::ReturnOp>(location, static_cast<Value>(constRetVal));
  module.push_back(initFunc);
}

template<typename BodyCreator_t>
void GenerateModelMemrefInitializerImpl(const std::string& funcName, ConversionPatternRewriter &rewriter, Location location, 
                                        ModuleOp module, MemRefType memrefType, bool sparseRep, BodyCreator_t createBody) {
  assert (memrefType.getShape().size() == 1);
  // SaveAndRestoreInsertionPoint saveAndRestoreEntryPoint(rewriter);
  auto modelMemrefElementType = memrefType.getElementType().cast<decisionforest::TiledNumericalNodeType>();
  int32_t tileSize = modelMemrefElementType.getTileSize();
  auto thresholdArgType = MemRefType::get({ memrefType.getShape()[0] * tileSize }, modelMemrefElementType.getThresholdElementType());
  auto indexArgType = MemRefType::get({ memrefType.getShape()[0] * tileSize }, modelMemrefElementType.getIndexElementType());
  auto tileShapeIDArgType = MemRefType::get(memrefType.getShape(), modelMemrefElementType.getTileShapeType());

  MemRefType childIndexArgType;
  FunctionType initModelMemrefFuncType;
  if (sparseRep) {
    childIndexArgType = MemRefType::get(memrefType.getShape(), modelMemrefElementType.getChildIndexType());
    initModelMemrefFuncType = rewriter.getFunctionType(TypeRange{thresholdArgType, indexArgType, tileShapeIDArgType, childIndexArgType}, memrefType);
  }
  else {
    initModelMemrefFuncType = rewriter.getFunctionType(TypeRange{thresholdArgType, indexArgType, tileShapeIDArgType}, memrefType);
  }
  NamedAttribute visibilityAttribute{module.getSymVisibilityAttrName(), rewriter.getStringAttr("public")};
  auto initModelMemrefFunc = mlir::func::FuncOp::create(location, funcName, initModelMemrefFuncType, ArrayRef<NamedAttribute>(visibilityAttribute));
  auto &entryBlock = *initModelMemrefFunc.addEntryBlock();
  // rewriter.setInsertionPointToStart(&entryBlock);
  mlir::OpBuilder builder(initModelMemrefFunc.getContext());
  builder.setInsertionPointToStart(&entryBlock);
  // auto& builder = rewriter;

  // Allocate the model memref
  auto waitOp = builder.create<gpu::WaitOp>(location, gpu::AsyncTokenType::get(module.getContext()), ValueRange{});
  auto modelMemrefGPUAlloc = builder.create<gpu::AllocOp>(location, memrefType, waitOp.getAsyncToken().getType(), ValueRange{waitOp.getAsyncToken()}, ValueRange{}, ValueRange{});

  auto asyncTokenType = modelMemrefGPUAlloc.getAsyncToken().getType();
  // Allocate and transfer all the arguments
  auto allocThresholds = builder.create<gpu::AllocOp>(location, thresholdArgType, asyncTokenType, ValueRange{modelMemrefGPUAlloc.getAsyncToken()}, ValueRange{}, ValueRange{});
  auto transferThresholds = builder.create<gpu::MemcpyOp>(location, asyncTokenType, ValueRange{allocThresholds.getAsyncToken()}, 
                                                      allocThresholds.getMemref(), static_cast<Value>(initModelMemrefFunc.getArgument(0)));

  auto allocFeatureIndices = builder.create<gpu::AllocOp>(location, indexArgType, asyncTokenType, ValueRange{transferThresholds.getAsyncToken()}, ValueRange{}, ValueRange{});
  auto transferFeatureIndices = builder.create<gpu::MemcpyOp>(location, asyncTokenType, ValueRange{allocFeatureIndices.getAsyncToken()}, 
                                                      allocFeatureIndices.getMemref(), static_cast<Value>(initModelMemrefFunc.getArgument(1)));
  
  Value currentAsyncToken = transferFeatureIndices.getAsyncToken();
  mlir::gpu::AllocOp allocTileShapeIds;
  mlir::gpu::MemcpyOp transferTileShapeIds;
  if (tileSize != 1) {
    allocTileShapeIds = builder.create<gpu::AllocOp>(location, tileShapeIDArgType, asyncTokenType, ValueRange{transferFeatureIndices.getAsyncToken()}, 
                                                     ValueRange{}, ValueRange{});
    transferTileShapeIds = builder.create<gpu::MemcpyOp>(location, asyncTokenType, ValueRange{allocTileShapeIds.getAsyncToken()}, 
                                                        allocTileShapeIds.getMemref(), static_cast<Value>(initModelMemrefFunc.getArgument(2)));
    currentAsyncToken = transferTileShapeIds.getAsyncToken();
  }

  mlir::gpu::AllocOp allocChildIndices;
  mlir::gpu::MemcpyOp transferChildIndices;
  if (sparseRep) {
    allocChildIndices = builder.create<gpu::AllocOp>(location, childIndexArgType, 
                                                     asyncTokenType, 
                                                     ValueRange{currentAsyncToken}, 
                                                     ValueRange{}, ValueRange{});
    transferChildIndices = builder.create<gpu::MemcpyOp>(location, asyncTokenType, 
                                                        ValueRange{allocChildIndices.getAsyncToken()}, 
                                                        allocChildIndices.getMemref(),
                                                        static_cast<Value>(initModelMemrefFunc.getArgument(3)));
    currentAsyncToken = transferChildIndices.getAsyncToken();
  }

  // Create the gpu.launch op
  auto oneIndexConst = builder.create<arith::ConstantIndexOp>(location, 1);
  int32_t numThreadsPerBlock = 32;
  int32_t numBlocks = std::ceil((double)memrefType.getShape()[0]/numThreadsPerBlock);
  auto numThreadBlocksConst = builder.create<arith::ConstantIndexOp>(location, numBlocks);
  auto numThreadsPerBlockConst = builder.create<arith::ConstantIndexOp>(location, numThreadsPerBlock);
  auto gpuLaunch = builder.create<gpu::LaunchOp>(location, numThreadBlocksConst, oneIndexConst, oneIndexConst, 
                                                numThreadsPerBlockConst, oneIndexConst, oneIndexConst,
                                                nullptr, asyncTokenType, 
                                                currentAsyncToken);

  builder.setInsertionPointToStart(&gpuLaunch.getBody().front());
  
  // // Generate the body of the launch op
  auto memrefLengthConst = builder.create<arith::ConstantIndexOp>(location, memrefType.getShape()[0]);
  auto firstThreadNum = builder.create<arith::MulIOp>(location, gpuLaunch.getBlockSizeX(), gpuLaunch.getBlockIds().x);
  auto elementIndex = builder.create<arith::AddIOp>(location, firstThreadNum, gpuLaunch.getThreadIds().x);
  auto inBoundsCondition = builder.create<arith::CmpIOp>(location, arith::CmpIPredicate::slt, elementIndex, memrefLengthConst);
  auto ifInBounds = builder.create<scf::IfOp>(location, inBoundsCondition, false);
  {
    // Generate the initialization code
    auto thenBuilder = ifInBounds.getThenBodyBuilder();
    createBody(memrefType, modelMemrefGPUAlloc.getMemref(), 
               thenBuilder, location, elementIndex, 
               allocThresholds.getMemref(), 
               allocFeatureIndices.getMemref(), 
               tileSize!=1 ? allocTileShapeIds.getMemref() : Value(),
               sparseRep ? allocChildIndices.getMemref() : Value());
  }
  builder.create<gpu::TerminatorOp>(location);
  // Wait and return 
  builder.setInsertionPointAfter(gpuLaunch); 
  /*auto waitBeforeReturn =*/ builder.create<gpu::WaitOp>(location, gpu::AsyncTokenType::get(module.getContext()), ValueRange{gpuLaunch.getAsyncToken()});
  builder.create<mlir::func::ReturnOp>(location, static_cast<Value>(modelMemrefGPUAlloc.getMemref()));
  module.push_back(initModelMemrefFunc);
}


// ===---------------------------------------------------=== //
// GPU array based representation
// ===---------------------------------------------------=== //

void GPUArrayBasedRepresentation::GenerateModelMemrefInitializer(const std::string& funcName, ConversionPatternRewriter &rewriter, Location location, 
                                                                 ModuleOp module, MemRefType memrefType) {
  GenerateModelMemrefInitializerImpl(funcName, rewriter, location, module, memrefType, false /*sparseRep*/,
                                     [&](MemRefType memrefType, 
                                         Value memrefValue,
                                         mlir::OpBuilder &builder, 
                                         Location location,
                                         Value tileIndex,
                                         Value thresholdMemref,
                                         Value indexMemref,
                                         Value tileShapeIdMemref,
                                         Value childIndexMemref) {
                                      this->GenModelMemrefInitFunctionBody(memrefType, 
                                                                           memrefValue,
                                                                           builder,
                                                                           location,
                                                                           tileIndex,
                                                                           thresholdMemref,
                                                                           indexMemref,
                                                                           tileShapeIdMemref);
                                     });
}

mlir::LogicalResult GPUArrayBasedRepresentation::GenerateModelGlobals(Operation *op, ArrayRef<Value> operands, ConversionPatternRewriter &rewriter,
                                                                      std::shared_ptr<decisionforest::IModelSerializer> m_serializer) {
  auto location = op->getLoc();
  // Generate a new function with the extra arguments that are needed
  auto ensembleConstOp = AssertOpIsOfType<decisionforest::EnsembleConstantOp>(op);
  auto module = op->getParentOfType<mlir::ModuleOp>();
  assert (module);
  auto func = op->getParentOfType<func::FuncOp>();
  assert (func);

  mlir::decisionforest::DecisionForestAttribute forestAttribute = ensembleConstOp.getForest();
  mlir::decisionforest::DecisionForest<>& forest = forestAttribute.GetDecisionForest();
  auto forestType = ensembleConstOp.getResult().getType().cast<decisionforest::TreeEnsembleType>();
  assert (forestType.doAllTreesHaveSameTileSize()); // There is still an assumption here that all trees have the same tile size
  auto treeType = forestType.getTreeType(0).cast<decisionforest::TreeType>();

  auto thresholdType = treeType.getThresholdType();
  auto featureIndexType = treeType.getFeatureIndexType(); 
  auto tileSize = treeType.getTileSize();
  auto tileShapeType = treeType.getTileShapeType();
  auto childIndexType = treeType.getChildIndexType();

  m_tileSize = tileSize;
  m_thresholdType = thresholdType;
  m_featureIndexType = featureIndexType;
  m_tileShapeType = tileShapeType;

  Type modelMemrefElementType = decisionforest::TiledNumericalNodeType::get(thresholdType, featureIndexType, tileShapeType, 
                                                                            tileSize, childIndexType);

  m_serializer->Persist(forest, forestType);
  
  auto modelMemrefSize = decisionforest::GetTotalNumberOfTiles();
  auto modelMemrefType = MemRefType::get({modelMemrefSize}, modelMemrefElementType);
  func.insertArgument(func.getNumArguments(), modelMemrefType, mlir::DictionaryAttr(), location);
  m_modelMemrefArgIndex = func.getNumArguments() - 1;

  auto offsetSize = (int32_t)forest.NumTrees();
  auto offsetMemrefType = MemRefType::get({offsetSize}, rewriter.getIndexType());
  func.insertArgument(func.getNumArguments(), offsetMemrefType, mlir::DictionaryAttr(), location);
  m_offsetMemrefArgIndex = func.getNumArguments() - 1;

  // Add the length argument
  func.insertArgument(func.getNumArguments(), offsetMemrefType, mlir::DictionaryAttr(), location);
  m_lengthMemrefArgIndex = func.getNumArguments() - 1;

  // Add the class info argument
  auto classInfoSize = forest.IsMultiClassClassifier() ? offsetSize : 0;
  auto classInfoMemrefType = MemRefType::get({classInfoSize}, rewriter.getI8Type());
  func.insertArgument(func.getNumArguments(), classInfoMemrefType, mlir::DictionaryAttr(), location);
  m_classInfoMemrefArgIndex = func.getNumArguments() - 1;

  m_modelMemref = func.getArgument(m_modelMemrefArgIndex);
  
  GenerateModelMemrefInitializer("Init_Model", rewriter, location, module, modelMemrefType);
  GenerateSimpleInitializer("Init_Offsets", rewriter, location, module, offsetMemrefType);
  GenerateSimpleInitializer("Init_Lengths", rewriter, location, module, offsetMemrefType);
  GenerateSimpleInitializer("Init_ClassIds", rewriter, location, module, classInfoMemrefType);

  GenerateCleanupProc("Dealloc_Buffers", 
                      rewriter, 
                      location,
                      module, 
                      std::vector<Type>{modelMemrefType, offsetMemrefType, offsetMemrefType}); //, classInfoMemrefType})
  EnsembleConstantLoweringInfo info 
  {
    static_cast<Value>(m_modelMemref),
    static_cast<Value>(func.getArgument(m_offsetMemrefArgIndex)),
    static_cast<Value>(func.getArgument(m_lengthMemrefArgIndex)),
    static_cast<Value>(func.getArgument(m_classInfoMemrefArgIndex)),
    modelMemrefType,
    offsetMemrefType,
    offsetMemrefType,
    classInfoMemrefType,
  };
  ensembleConstantToMemrefsMap[op] = info;
  return mlir::success();
}

// mlir::Value GPUArrayBasedRepresentation::GetTreeMemref(mlir::Value treeValue) {
//   return m_modelMemref;
// }

// void GPUArrayBasedRepresentation::GenerateTreeMemref(mlir::ConversionPatternRewriter &rewriter, mlir::Operation *op, Value ensemble, Value treeIndex) {

// }

// mlir::Value GPUArrayBasedRepresentation::GenerateGetTreeClassId(mlir::ConversionPatternRewriter &rewriter, mlir::Operation *op, Value ensemble, Value treeIndex) {
//   return mlir::Value();
// }

void GPUArrayBasedRepresentation::LowerCacheTreeOp(ConversionPatternRewriter &rewriter, 
                                                   mlir::Operation *op,
                                                   ArrayRef<Value> operands,
                                                   std::shared_ptr<decisionforest::IModelSerializer> m_serializer) {
  // Get the values for the buffers inserted for the ensemble we are caching
  auto location = op->getLoc();
  auto cacheTreesOp = AssertOpIsOfType<decisionforest::CacheTreesFromEnsembleOp>(op);
  auto ensembleValue = cacheTreesOp.getForest();
  auto ensembleConst = AssertOpIsOfType<decisionforest::EnsembleConstantOp>(ensembleValue.getDefiningOp());
  auto forestType = ensembleValue.getType().cast<decisionforest::TreeEnsembleType>();
  assert (forestType.doAllTreesHaveSameType() && forestType.doAllTreesHaveSameTileSize());
  auto treeType = forestType.getTreeType(0).cast<decisionforest::TreeType>();

  assert (ensembleConstantToMemrefsMap.find(ensembleConst.getOperation()) != ensembleConstantToMemrefsMap.end());
  auto& ensembleInfo = ensembleConstantToMemrefsMap[ensembleConst.getOperation()];

  // Compute the size of the shared mem buffer (max tree size * step)
  std::vector<int32_t> lengths(decisionforest::ForestJSONReader::GetInstance().GetNumberOfTrees(), -1);
  auto tileSize = ensembleConst.getForest().GetDecisionForest().GetTree(0).TilingDescriptor().MaxTileSize();
  
  decisionforest::ForestJSONReader::GetInstance().InitializeLengthBuffer(lengths.data(), 
                                                                         tileSize,
                                                                         treeType.getThresholdType().getIntOrFloatBitWidth(),
                                                                         treeType.getFeatureIndexType().getIntOrFloatBitWidth());
  auto maxLen = *std::max_element(lengths.begin(), lengths.end());

  auto owningForLoop = cacheTreesOp->getParentOfType<scf::ForOp>();
  assert (owningForLoop);

  auto loopStep = owningForLoop.getStep();
  auto stepConst = AssertOpIsOfType<arith::ConstantIndexOp>(loopStep.getDefiningOp());
  auto stepValue = stepConst.value();

  int64_t bufferLen = maxLen * stepValue;

  // Add the required globals to the owning module
  auto owningModule = cacheTreesOp->getParentOfType<mlir::ModuleOp>();
  assert (owningModule);
  
  std::string globalCacheBufferName = std::string("treeCache_")+std::to_string(reinterpret_cast<long long>(op));
  auto treeMemrefType = ensembleInfo.modelGlobal.getType().cast<MemRefType>();
  // TODO_Ashwin Use the right memory space ID
  auto cacheBufferType = MemRefType::get({bufferLen}, treeMemrefType.getElementType());
  {
    SaveAndRestoreInsertionPoint saveAndRestoreInsertPoint(rewriter);
    rewriter.setInsertionPoint(&owningModule.front());
    rewriter.create<memref::GlobalOp>(location, globalCacheBufferName,
                                  /*sym_visibility=*/rewriter.getStringAttr("private"),
                                  /*type=*/cacheBufferType,
                                  /*initial_value=*/rewriter.getUnitAttr(),
                                  /*constant=*/false, 
                                  /*alignment*/IntegerAttr());
  }

  auto offsetsMemref = ensembleInfo.offsetGlobal;
  auto offsetsLength = offsetsMemref.getType().cast<MemRefType>().getShape()[0];
  auto offsetLenConst = rewriter.create<arith::ConstantIndexOp>(location, offsetsLength);

  auto modelMemref = ensembleInfo.modelGlobal;
  auto modelMemrefLength = ensembleInfo.modelGlobal.getType().cast<MemRefType>().getShape()[0];
  auto modelLenConst = rewriter.create<arith::ConstantIndexOp>(location, offsetsLength);

  auto sharedMemoryBuffer = rewriter.create<memref::GetGlobalOp>(location, cacheBufferType, globalCacheBufferName);
  // Compute the actual range of indices we need to read into the shared mem buffer
  auto startIndex = rewriter.create<memref::LoadOp>(location, offsetsMemref, ValueRange{cacheTreesOp.getStartTreeIndex()});

  // Since the end index can be out of range, we need to generate an "if"
  auto endIndexInRange = rewriter.create<arith::CmpIOp>(location, arith::CmpIPredicate::slt,
                                                        cacheTreesOp.getEndTreeIndex(),
                                                        static_cast<Value>(offsetLenConst));
  auto endIndexIfElse = rewriter.create<scf::IfOp>(location, TypeRange{rewriter.getIndexType()}, endIndexInRange, true);
  
  {
    auto thenBuilder = endIndexIfElse.getThenBodyBuilder();
    auto loadEndIndex = thenBuilder.create<memref::LoadOp>(location, offsetsMemref, ValueRange{cacheTreesOp.getEndTreeIndex()});
    thenBuilder.create<scf::YieldOp>(location, loadEndIndex.getResult());

    auto elseBuilder = endIndexIfElse.getElseBodyBuilder();
    elseBuilder.create<scf::YieldOp>(location, modelLenConst.getResult());
  }
  auto endIndex = endIndexIfElse.getResult(0);

  // Get the number of threads in the thread block
  auto owningGPULaunchOp = cacheTreesOp->getParentOfType<gpu::LaunchOp>();
  assert (owningGPULaunchOp);
  auto numThreadsX = owningGPULaunchOp.getBlockSizeX();
  auto numThreadsY = owningGPULaunchOp.getBlockSizeY();
  auto threadNum = owningGPULaunchOp.getThreadIds();

  // TODO_Ashwin everything below assumes that thread blocks are 2D!
  // index = numThreadsX*threadNum.Y + threadNum.X
  auto nxTimesTy = rewriter.create<arith::MulIOp>(location, numThreadsX, threadNum.y);
  auto index = rewriter.create<arith::AddIOp>(location, static_cast<Value>(nxTimesTy), threadNum.x);
  
  // TODO_Ashwin needs a loop around it if we don't have enough threads
  // Generate the stores into shared memory based on these loads
  //    numElementsToRead = endIndex - startIndex
  //    if (index < numElementsToRead) {
  //     globalIndex = index + startIndex
  //     threshold = loadThreshold(modelMemref, globalIndex, 0) -- any tree index is fine since we just ignore it here
  //     featureIndex = loadFeatureIndex(...)
  //     InitTile(shMemBuf, index, threshold, ...)
  //    }
  //    syncthreads()
  auto numElementsToRead = rewriter.create<arith::SubIOp>(location, endIndex, startIndex);
  auto indexLTElemsToRead = rewriter.create<arith::CmpIOp>(location, arith::CmpIPredicate::slt, index.getResult(), numElementsToRead.getResult());
  auto ifIndexInRange = rewriter.create<scf::IfOp>(location, TypeRange{}, indexLTElemsToRead.getResult(), false);
  {
    auto thenBuilder = ifIndexInRange.getThenBodyBuilder();
    auto globalIndex = thenBuilder.create<arith::AddIOp>(location, index.getResult(), startIndex.getResult());
    auto zeroIndexConst = thenBuilder.create<arith::ConstantIndexOp>(location, 0);
    auto threshold = thenBuilder.create<decisionforest::LoadTileThresholdsOp>(location,
                                                                              GetThresholdElementType(),
                                                                              modelMemref,
                                                                              globalIndex.getResult(),
                                                                              zeroIndexConst);
    auto featureIndex = thenBuilder.create<decisionforest::LoadTileFeatureIndicesOp>(location,
                                                                                    GetIndexElementType(),
                                                                                    modelMemref,
                                                                                    globalIndex.getResult(),
                                                                                    zeroIndexConst);
    auto tileShapeID = thenBuilder.create<arith::ConstantIntOp>(location, 0, thenBuilder.getI32Type());
    thenBuilder.create<decisionforest::InitTileOp>(location, 
                                                  sharedMemoryBuffer.getResult(),
                                                  index.getResult(),
                                                  threshold.getResult(),
                                                  featureIndex.getResult(),
                                                  tileShapeID.getResult());
  }
  rewriter.create<gpu::BarrierOp>(location);

  m_cacheTreesOpsMap[op] = {sharedMemoryBuffer};
}

void GPUArrayBasedRepresentation::GenerateTreeMemref(mlir::ConversionPatternRewriter &rewriter, 
                                                     mlir::Operation *op, 
                                                     Value ensemble,
                                                     Value treeIndex) {
  Operation* ensembleDefiningOp = ensemble.getDefiningOp();
  auto ensembleConstantOp = llvm::dyn_cast<mlir::decisionforest::EnsembleConstantOp>(ensembleDefiningOp);
  if (ensembleConstantOp) {
    ArrayBasedRepresentation::GenerateTreeMemref(rewriter, op, ensemble, treeIndex);
    return;
  }
  auto location = op->getLoc();
  auto cacheTreesOp = AssertOpIsOfType<decisionforest::CacheTreesFromEnsembleOp>(ensembleDefiningOp);
  ensembleConstantOp = AssertOpIsOfType<decisionforest::EnsembleConstantOp>(cacheTreesOp.getForest().getDefiningOp());

  auto mapIter = ensembleConstantToMemrefsMap.find(ensembleConstantOp);
  assert (mapIter != ensembleConstantToMemrefsMap.end());
  auto& ensembleInfo = mapIter->second;

  auto cacheTreesMapIter = m_cacheTreesOpsMap.find(cacheTreesOp.getOperation());
  assert (cacheTreesMapIter != m_cacheTreesOpsMap.end());
  auto cachedModelBuffer = cacheTreesMapIter->second.cachedModelBuffer;

  auto modelMemrefOffset = rewriter.create<memref::LoadOp>(location, ensembleInfo.offsetGlobal, cacheTreesOp.getStartTreeIndex()); 
  auto modelMemrefIndex = rewriter.create<memref::LoadOp>(location, ensembleInfo.offsetGlobal, treeIndex);
  auto cacheIndex = rewriter.create<arith::SubIOp>(location, modelMemrefIndex.getResult(), modelMemrefOffset.getResult());
  auto treeLength = rewriter.create<memref::LoadOp>(location, ensembleInfo.lengthGlobal, treeIndex);
  auto treeMemref = rewriter.create<memref::SubViewOp>(location,
                                                       cachedModelBuffer,
                                                       ArrayRef<OpFoldResult>({static_cast<Value>(cacheIndex)}),
                                                       ArrayRef<OpFoldResult>({static_cast<Value>(treeLength)}),
                                                       ArrayRef<OpFoldResult>({rewriter.getIndexAttr(1)}));
  
  // if (decisionforest::InsertDebugHelpers) {
  //   rewriter.create<decisionforest::PrintTreeToDOTFileOp>(location, treeMemref, treeIndex);
  // }
  getTreeOperationMap[op] = static_cast<Value>(treeMemref);
}


std::shared_ptr<IRepresentation> ConstructGPUArrayBasedRepresentation() {
  return std::make_shared<GPUArrayBasedRepresentation>();
}

REGISTER_REPRESENTATION(gpu_array, ConstructGPUArrayBasedRepresentation)

// ===---------------------------------------------------=== //
// GPU sparse representation
// ===---------------------------------------------------=== //

void GPUSparseRepresentation::GenerateModelMemrefInitializer(const std::string& funcName, 
                                                             ConversionPatternRewriter &rewriter, Location location, 
                                                             ModuleOp module, MemRefType memrefType) {
  GenerateModelMemrefInitializerImpl(funcName, rewriter, location, module, memrefType, true /*sparseRep*/,
                                     [&](MemRefType memrefType, 
                                         Value memrefValue,
                                         mlir::OpBuilder &builder, 
                                         Location location,
                                         Value tileIndex,
                                         Value thresholdMemref,
                                         Value indexMemref,
                                         Value tileShapeIdMemref,
                                         Value childIndexMemref) {
                                      this->GenModelMemrefInitFunctionBody(memrefType, 
                                                                           memrefValue,
                                                                           builder,
                                                                           location,
                                                                           tileIndex,
                                                                           thresholdMemref,
                                                                           indexMemref,
                                                                           tileShapeIdMemref,
                                                                           childIndexMemref);
                                     });
}

mlir::LogicalResult GPUSparseRepresentation::GenerateModelGlobals(Operation *op, 
                                                                  ArrayRef<Value> operands,
                                                                  ConversionPatternRewriter &rewriter,
                                                                  std::shared_ptr<decisionforest::IModelSerializer> m_serializer) {
  auto location = op->getLoc();
  // Generate a new function with the extra arguments that are needed
  auto ensembleConstOp = AssertOpIsOfType<decisionforest::EnsembleConstantOp>(op);
  auto module = op->getParentOfType<mlir::ModuleOp>();
  assert (module);
  auto func = op->getParentOfType<func::FuncOp>();
  assert (func);

  mlir::decisionforest::DecisionForestAttribute forestAttribute = ensembleConstOp.getForest();
  mlir::decisionforest::DecisionForest<>& forest = forestAttribute.GetDecisionForest();
  auto forestType = ensembleConstOp.getResult().getType().cast<decisionforest::TreeEnsembleType>();
  assert (forestType.doAllTreesHaveSameTileSize()); // There is still an assumption here that all trees have the same tile size
  auto treeType = forestType.getTreeType(0).cast<decisionforest::TreeType>();

  auto thresholdType = treeType.getThresholdType();
  auto featureIndexType = treeType.getFeatureIndexType(); 
  auto tileSize = treeType.getTileSize();
  auto tileShapeType = treeType.getTileShapeType();
  auto childIndexType = treeType.getChildIndexType();

  m_tileSize = tileSize;
  m_thresholdType = thresholdType;
  m_featureIndexType = featureIndexType;
  m_tileShapeType = tileShapeType;

  Type modelMemrefElementType = decisionforest::TiledNumericalNodeType::get(thresholdType, featureIndexType, tileShapeType, 
                                                                            tileSize, childIndexType);

  m_serializer->Persist(forest, forestType);
  
  auto modelMemrefSize = decisionforest::GetTotalNumberOfTiles();
  auto modelMemrefType = MemRefType::get({modelMemrefSize}, modelMemrefElementType);
  func.insertArgument(func.getNumArguments(), modelMemrefType, mlir::DictionaryAttr(), location);
  m_modelMemrefArgIndex = func.getNumArguments() - 1;

  auto offsetSize = (int32_t)forest.NumTrees();
  auto offsetMemrefType = MemRefType::get({offsetSize}, rewriter.getIndexType());
  func.insertArgument(func.getNumArguments(), offsetMemrefType, mlir::DictionaryAttr(), location);
  m_offsetMemrefArgIndex = func.getNumArguments() - 1;

  // Add the length argument
  func.insertArgument(func.getNumArguments(), offsetMemrefType, mlir::DictionaryAttr(), location);
  m_lengthMemrefArgIndex = func.getNumArguments() - 1;

  // Add the class info argument
  auto classInfoSize = forest.IsMultiClassClassifier() ? offsetSize : 0;
  auto classInfoMemrefType = MemRefType::get({classInfoSize}, treeType.getResultType());
  func.insertArgument(func.getNumArguments(), classInfoMemrefType, mlir::DictionaryAttr(), location);
  m_classInfoMemrefArgIndex = func.getNumArguments() - 1;

  m_modelMemref = func.getArgument(m_modelMemrefArgIndex);
  
  GenerateModelMemrefInitializer("Init_Model", rewriter, location, module, modelMemrefType);
  GenerateSimpleInitializer("Init_Offsets", rewriter, location, module, offsetMemrefType);
  GenerateSimpleInitializer("Init_Lengths", rewriter, location, module, offsetMemrefType);
  GenerateSimpleInitializer("Init_ClassIds", rewriter, location, module, classInfoMemrefType);

  GenerateCleanupProc("Dealloc_Buffers", 
                    rewriter, 
                    location,
                    module, 
                    std::vector<Type>{modelMemrefType, offsetMemrefType, offsetMemrefType}); //, classInfoMemrefType})

  SparseEnsembleConstantLoweringInfo info 
  {
    static_cast<Value>(m_modelMemref),
    static_cast<Value>(func.getArgument(m_offsetMemrefArgIndex)),
    static_cast<Value>(func.getArgument(m_lengthMemrefArgIndex)),
    Value(), // LUT
    Value(), // leaves 
    Value(), // leavesOffset
    Value(), // leavesLength
    static_cast<Value>(func.getArgument(m_classInfoMemrefArgIndex)),
    modelMemrefType,
    offsetMemrefType,
    offsetMemrefType,
    Type(), // LUT type
    Type(), // Leaves type
    classInfoMemrefType,
  };
  sparseEnsembleConstantToMemrefsMap[op] = info;
  return mlir::success();
}

std::shared_ptr<IRepresentation> ConstructGPUSparseRepresentation() {
  return std::make_shared<GPUSparseRepresentation>();
}

REGISTER_REPRESENTATION(gpu_sparse, ConstructGPUSparseRepresentation)

}
}