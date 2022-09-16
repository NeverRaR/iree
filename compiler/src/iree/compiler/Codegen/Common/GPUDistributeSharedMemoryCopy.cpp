// Copyright 2021 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <algorithm>
#include <numeric>

#include "iree-dialects/Dialect/LinalgExt/Transforms/Transforms.h"
#include "iree/compiler/Codegen/Dialect/LoweringConfig.h"
#include "iree/compiler/Codegen/PassDetail.h"
#include "iree/compiler/Codegen/Passes.h"
#include "iree/compiler/Codegen/Transforms/Transforms.h"
#include "iree/compiler/Codegen/Utils/GPUUtils.h"
#include "iree/compiler/Codegen/Utils/MarkerUtils.h"
#include "mlir/Dialect/Affine/IR/AffineOps.h"
#include "mlir/Dialect/Arithmetic/IR/Arithmetic.h"
#include "mlir/Dialect/GPU/IR/GPUDialect.h"
#include "mlir/Dialect/SCF/Utils/Utils.h"
#include "mlir/Dialect/Vector/Transforms/VectorTransforms.h"
#include "mlir/IR/Builders.h"
#include "mlir/IR/MLIRContext.h"
#include "mlir/Support/MathExtras.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"
#include "mlir/Transforms/Passes.h"

using mlir::iree_compiler::IREE::LinalgExt::LinalgVectorizationPattern;
using mlir::iree_compiler::IREE::LinalgExt::VectorizationPatterns;

//====---------------------------------------------------------------------===//
// Pass to lower workgroup memory copy to distibuted
// transfer_read/transfer_write ops.
//====---------------------------------------------------------------------===//

// Markers for intermediate transformations.
static const llvm::StringRef kCopyToDistribute = "copy_to_distribute";
static const llvm::StringRef kCopyDistributed = "copy_distributed";

namespace mlir {
namespace iree_compiler {

// For optimal performance we always want to copy 128 bits
static constexpr int copyVectorNumBits = 128;

/// Patterns for copy to shared memory mapping. Copy to shared memory are not
/// part of the launch config but needs to be distributed on the workgroup
/// picked by the root op.
static void populateTilingCopyToWorkgroupMemPatterns(
    RewritePatternSet &patterns, ArrayRef<int64_t> workgroupSize) {
  // Tile and distribute copy to workgroup memory.
  linalg::TileSizeComputationFunction wgCopyTileSizeFn =
      [](OpBuilder &builder, Operation *operation) {
        // We tile to 4 as we want each thread to load 4 element in a cyclic
        // distribution.
        SmallVector<Value, 4> tileSizesVal;
        MemRefType lhsMemRefType = cast<linalg::GenericOp>(operation)
                                       .getOperand(0)
                                       .getType()
                                       .cast<MemRefType>();

        unsigned rank = lhsMemRefType.getRank();
        int copyTileSize =
            copyVectorNumBits / lhsMemRefType.getElementTypeBitWidth();
        for (unsigned i = 0; i < rank - 1; i++) {
          int64_t t = (rank - i) <= kNumGPUDims ? 1 : 0;
          tileSizesVal.push_back(
              builder.create<arith::ConstantIndexOp>(operation->getLoc(), t));
        }
        tileSizesVal.push_back(builder.create<arith::ConstantIndexOp>(
            operation->getLoc(), copyTileSize));
        return tileSizesVal;
      };
  auto getCopyThreadProcInfoFn = [workgroupSize](
                                     OpBuilder &builder, Location loc,
                                     ArrayRef<Range> parallelLoopRanges) {
    return getGPUThreadIdsAndCounts(builder, loc, parallelLoopRanges.size(),
                                    workgroupSize);
  };
  linalg::LinalgLoopDistributionOptions copyInvocationDistributionOptions;
  copyInvocationDistributionOptions.procInfo = getCopyThreadProcInfoFn;

  auto tilingOptions =
      linalg::LinalgTilingOptions()
          .setLoopType(linalg::LinalgTilingLoopType::Loops)
          .setTileSizeComputationFunction(wgCopyTileSizeFn)
          .setDistributionOptions(copyInvocationDistributionOptions);
  patterns.insert<linalg::LinalgTilingPattern>(
      linalg::GenericOp::getOperationName(), patterns.getContext(),
      tilingOptions,
      linalg::LinalgTransformationFilter(
          {StringAttr::get(patterns.getContext(),
                           getCopyToWorkgroupMemoryMarker())},
          StringAttr::get(patterns.getContext(), getVectorizeMarker())));
}

/// Compute a tile size so that the numer of iteraton is equal to the flat
/// workgroup size.
static Optional<SmallVector<int64_t>> getTileToDistributableSize(
    linalg::GenericOp copyOp, int64_t flatWorkgroupSize) {
  SmallVector<int64_t, 4> shape = copyOp.getStaticLoopRanges();
  unsigned bitWidth = copyOp->getOperand(0)
                          .getType()
                          .cast<MemRefType>()
                          .getElementTypeBitWidth();
  int targetVectorSize = copyVectorNumBits / bitWidth;
  SmallVector<int64_t> unroll;
  assert(shape.back() % targetVectorSize == 0);
  int64_t threadsAvailable = flatWorkgroupSize;
  for (auto &dim : llvm::enumerate(llvm::reverse(shape))) {
    int64_t numElementPerThread = dim.index() == 0 ? targetVectorSize : 1;
    int64_t numThreads = dim.value() / numElementPerThread;
    numThreads = std::min(numThreads, threadsAvailable);
    unroll.push_back(numThreads * numElementPerThread);
    assert(threadsAvailable % numThreads == 0);
    threadsAvailable = threadsAvailable / numThreads;
    if (threadsAvailable == 1) break;
  }
  assert(threadsAvailable == 1);
  unroll.resize(shape.size(), 1);
  std::reverse(unroll.begin(), unroll.end());
  return unroll;
}

/// Pattern to tile copies using serial loops into a shape that can be
/// distributed onto thread.
static void populateTileToUnroll(RewritePatternSet &patterns,
                                 int64_t flatWorkgroupSize) {
  linalg::TileSizeComputationFunction wgCopyTileSizeFn =
      [flatWorkgroupSize](OpBuilder &builder, Operation *operation) {
        SmallVector<Value, 4> tileSizesVal;
        auto copyOp = dyn_cast<linalg::GenericOp>(operation);
        if (!copyOp) return tileSizesVal;
        Optional<SmallVector<int64_t>> staticSize =
            getTileToDistributableSize(copyOp, flatWorkgroupSize);
        for (int64_t dim : *staticSize) {
          tileSizesVal.push_back(
              builder.create<arith::ConstantIndexOp>(operation->getLoc(), dim));
        }
        return tileSizesVal;
      };

  auto tilingOptions = linalg::LinalgTilingOptions()
                           .setLoopType(linalg::LinalgTilingLoopType::Loops)
                           .setTileSizeComputationFunction(wgCopyTileSizeFn);
  patterns.insert<linalg::LinalgTilingPattern>(
      linalg::GenericOp::getOperationName(), patterns.getContext(),
      tilingOptions,
      linalg::LinalgTransformationFilter(
          {StringAttr::get(patterns.getContext(),
                           getCopyToWorkgroupMemoryMarker())},
          StringAttr::get(patterns.getContext(), kCopyToDistribute)));
}

/// Break up the flat id onto the static loop ranges.
SmallVector<linalg::ProcInfo> getIds(OpBuilder &b, Location loc,
                                     ArrayRef<Range> parallelLoopRanges,
                                     Value flatThreadId) {
  SmallVector<linalg::ProcInfo> infos;
  Value id = flatThreadId;
  AffineExpr d0 = getAffineDimExpr(0, b.getContext());
  for (Range r : llvm::reverse(parallelLoopRanges)) {
    linalg::ProcInfo info;
    auto offset = r.offset.dyn_cast<Attribute>();
    auto stride = r.stride.dyn_cast<Attribute>();
    auto size = r.size.dyn_cast<Attribute>();
    assert(offset && stride && size);
    int64_t numThreadsDim = (size.cast<IntegerAttr>().getInt() -
                             offset.cast<IntegerAttr>().getInt()) /
                            stride.cast<IntegerAttr>().getInt();
    Value dimId = id;
    if (infos.size() != parallelLoopRanges.size() - 1)
      dimId = makeComposedAffineApply(b, loc, d0 % numThreadsDim, {dimId});
    info.procId = dimId;
    info.nprocs = b.create<arith::ConstantIndexOp>(loc, numThreadsDim);
    info.distributionMethod =
        linalg::DistributionMethod::CyclicNumProcsEqNumIters;
    infos.push_back(info);
    id = makeComposedAffineApply(b, loc, d0.floorDiv(numThreadsDim), {id});
  }
  std::reverse(infos.begin(), infos.end());
  return infos;
}

/// Return the shape of copy op that can be vectorized to a
/// transfer_read/transfer_write of size `targetVectorSize`.
SmallVector<int64_t> getNativeDstShape(linalg::GenericOp copyOp) {
  unsigned bitWidth = copyOp->getOperand(0)
                          .getType()
                          .cast<MemRefType>()
                          .getElementTypeBitWidth();
  int targetVectorSize = copyVectorNumBits / bitWidth;
  SmallVector<int64_t> dstShape;
  for (int64_t dim : copyOp.getStaticLoopRanges()) {
    // Skip tiling of dimension of size 1 to simplify distribution.
    dstShape.push_back(dim == 1 ? 0 : 1);
  }
  dstShape.back() = targetVectorSize;
  return dstShape;
}

/// Distribute linalg copy onto threads based on the flat id.
static void populateTilingAndDistribute(RewritePatternSet &patterns,
                                        Value flatThreadId) {
  linalg::TileSizeComputationFunction wgCopyTileSizeFn =
      [](OpBuilder &builder, Operation *operation) {
        SmallVector<Value, 4> tileSizesVal;
        auto copyOp = dyn_cast<linalg::GenericOp>(operation);
        if (!copyOp) return tileSizesVal;
        SmallVector<int64_t> staticSize = getNativeDstShape(copyOp);
        for (int64_t dim : staticSize) {
          tileSizesVal.push_back(
              builder.create<arith::ConstantIndexOp>(operation->getLoc(), dim));
        }
        return tileSizesVal;
      };
  auto getCopyThreadProcInfoFn = [flatThreadId](
                                     OpBuilder &builder, Location loc,
                                     ArrayRef<Range> parallelLoopRanges) {
    return getIds(builder, loc, parallelLoopRanges, flatThreadId);
  };
  linalg::LinalgLoopDistributionOptions copyInvocationDistributionOptions;
  copyInvocationDistributionOptions.procInfo = getCopyThreadProcInfoFn;

  auto tilingOptions =
      linalg::LinalgTilingOptions()
          .setLoopType(linalg::LinalgTilingLoopType::ParallelLoops)
          .setTileSizeComputationFunction(wgCopyTileSizeFn)
          .setDistributionOptions(copyInvocationDistributionOptions);
  patterns.insert<linalg::LinalgTilingPattern>(
      linalg::GenericOp::getOperationName(), patterns.getContext(),
      tilingOptions,
      linalg::LinalgTransformationFilter(
          {StringAttr::get(patterns.getContext(), kCopyToDistribute)},
          StringAttr::get(patterns.getContext(), kCopyDistributed)));
}

static void populateVectorizationPatterns(RewritePatternSet &patterns) {
  VectorizationPatterns<linalg::GenericOp>::insert(
      patterns, linalg::LinalgVectorizationOptions(),
      linalg::LinalgTransformationFilter(
          {StringAttr::get(patterns.getContext(),
                           getCopyToWorkgroupMemoryMarker()),
           StringAttr::get(patterns.getContext(), kCopyDistributed)},
          llvm::None));
}

/// Return a flattened Id Value by combining the 3D gpu thread IDs.
static Value createFlatId(func::FuncOp funcOp,
                          ArrayRef<int64_t> workgroupSize) {
  OpBuilder b(funcOp.getBody());
  Type indexType = b.getIndexType();
  AffineExpr d0 = getAffineDimExpr(0, b.getContext());
  AffineExpr d1 = getAffineDimExpr(1, b.getContext());
  AffineExpr d2 = getAffineDimExpr(2, b.getContext());
  Value threadX =
      b.create<gpu::ThreadIdOp>(funcOp.getLoc(), indexType, gpu::Dimension::x);
  Value threadY =
      b.create<gpu::ThreadIdOp>(funcOp.getLoc(), indexType, gpu::Dimension::y);
  Value threadZ =
      b.create<gpu::ThreadIdOp>(funcOp.getLoc(), indexType, gpu::Dimension::z);
  Value flatThreadId = makeComposedAffineApply(
      b, funcOp.getLoc(),
      d0 + workgroupSize[0] * d1 + (workgroupSize[0] * workgroupSize[1]) * d2,
      {threadX, threadY, threadZ});
  return flatThreadId;
}

/// Hoist allocations to the top of the loop if they have no dependencies.
static void hoistAlloc(func::FuncOp funcOp) {
  SmallVector<memref::AllocOp> allocs;
  funcOp.walk([&](memref::AllocOp alloc) {
    if (alloc.getOperands().empty()) allocs.push_back(alloc);
  });
  for (memref::AllocOp alloc : allocs) {
    alloc->moveBefore(&(*funcOp.getBlocks().begin()),
                      funcOp.getBlocks().begin()->begin());
  }
}

/// We insert barriers conservatively, remove barriers that are obviously not
/// needed.
static void removeRedundantBarriers(func::FuncOp funcOp) {
  funcOp.walk([](linalg::GenericOp copyOp) {
    if (hasMarker(copyOp, getCopyToWorkgroupMemoryMarker())) {
      Operation *prevOp = copyOp->getPrevNode();
      SmallVector<Operation *> redundantBarriers;
      while (prevOp) {
        if (isa<gpu::BarrierOp>(prevOp))
          redundantBarriers.push_back(prevOp);
        else
          break;
        prevOp = prevOp->getPrevNode();
      }
      if (prevOp && hasMarker(prevOp, getCopyToWorkgroupMemoryMarker())) {
        for (Operation *op : redundantBarriers) op->erase();
      }
    }
  });
}

/// Return the number of iteration if it is static, otherwise returns 0.
static int64_t numIteration(scf::ForOp forOp) {
  auto lbCstOp = forOp.getLowerBound().getDefiningOp<arith::ConstantIndexOp>();
  auto ubCstOp = forOp.getUpperBound().getDefiningOp<arith::ConstantIndexOp>();
  auto stepCstOp = forOp.getStep().getDefiningOp<arith::ConstantIndexOp>();
  if (!lbCstOp || !ubCstOp || !stepCstOp || lbCstOp.value() < 0 ||
      ubCstOp.value() < 0 || stepCstOp.value() < 0)
    return 0;
  int64_t tripCount =
      mlir::ceilDiv(ubCstOp.value() - lbCstOp.value(), stepCstOp.value());
  return tripCount;
}

/// Fully unroll all the static loops unless they are part of the ignore map.
static void UnrollSharedMemoryLoops(
    func::FuncOp funcOp, const llvm::SmallDenseSet<scf::ForOp> &loopsToIgnore) {
  SmallVector<scf::ForOp> forOpsToUnroll;
  funcOp.walk([&](scf::ForOp forOp) {
    if (!loopsToIgnore.count(forOp)) forOpsToUnroll.push_back(forOp);
  });
  for (scf::ForOp forOp : llvm::reverse(forOpsToUnroll)) {
    (void)loopUnrollByFactor(forOp, numIteration(forOp));
  }
}

namespace {

class GPUDistributeSharedMemoryCopyPass
    : public GPUDistributeSharedMemoryCopyBase<
          GPUDistributeSharedMemoryCopyPass> {
  void getDependentDialects(DialectRegistry &registry) const override {
    registry.insert<vector::VectorDialect, scf::SCFDialect>();
  }
  void runOnOperation() override {
    func::FuncOp funcOp = getOperation();
    FailureOr<IREE::HAL::ExecutableExportOp> exportOp = getEntryPoint(funcOp);
    if (failed(exportOp)) return;
    auto workgroupSize = getWorkgroupSize(exportOp.value());
    workgroupSize.resize(3, 1);
    MLIRContext *context = &getContext();
    SmallVector<linalg::GenericOp> copiesToWorkgroupMem;
    funcOp.walk([&](linalg::GenericOp copyOp) {
      if (hasMarker(copyOp, getCopyToWorkgroupMemoryMarker()))
        copiesToWorkgroupMem.push_back(copyOp);
    });
    if (copiesToWorkgroupMem.empty()) return;

    // Step 0. First clean up the IR.
    hoistAlloc(funcOp);
    removeRedundantBarriers(funcOp);

    int64_t flatWorkgroupSize =
        workgroupSize[0] * workgroupSize[1] * workgroupSize[2];
    bool isAligned = llvm::all_of(
        copiesToWorkgroupMem, [flatWorkgroupSize](linalg::GenericOp copyOp) {
          MemRefType lhsMemRefType =
              copyOp.getOperand(0).getType().cast<MemRefType>();
          auto shape = lhsMemRefType.getShape();
          int targetVectorSize =
              copyVectorNumBits / lhsMemRefType.getElementTypeBitWidth();
          return canPerformVectorAccessUsingAllThreads(shape, flatWorkgroupSize,
                                                       targetVectorSize);
        });
    if (isAligned) {
      // Ignore all the exisiting loop
      llvm::SmallDenseSet<scf::ForOp> loopsToIgnore;
      funcOp.walk([&](scf::ForOp loop) { loopsToIgnore.insert(loop); });

      // Step 1. tile copies to get to a shape that can be distributed to
      // 128bits per lane copies.
      RewritePatternSet serialTilingPatterns(context);
      populateTileToUnroll(serialTilingPatterns, flatWorkgroupSize);
      if (failed(applyPatternsAndFoldGreedily(
              funcOp, std::move(serialTilingPatterns)))) {
        return signalPassFailure();
      }

      // Calculate a flat id that will then be broken down during distribution.
      Value flatId = createFlatId(funcOp, workgroupSize);
      // Step 2. Distribute the linalg op onto threads.
      RewritePatternSet tileAndDistributePatterns(context);
      populateTilingAndDistribute(tileAndDistributePatterns, flatId);
      if (failed(applyPatternsAndFoldGreedily(
              funcOp, std::move(tileAndDistributePatterns)))) {
        return signalPassFailure();
      }

      // Step 3. Vectorize the distributed copies.
      RewritePatternSet vectorizationPatterns(context);
      populateVectorizationPatterns(vectorizationPatterns);
      if (failed(applyPatternsAndFoldGreedily(
              funcOp, std::move(vectorizationPatterns)))) {
        return signalPassFailure();
      }

      // Step4. Finally unroll all the loop created
      UnrollSharedMemoryLoops(funcOp, loopsToIgnore);
    } else {
      // Fall back to basic tiling for cases where workgroup memory size is not
      // well aligned on the number of threads.
      // TODO(thomasraoux): Handle this case with padding instead so that we get
      // good performance for more complex shapes.
      RewritePatternSet threadLevelTilingPatterns(context);
      populateTilingCopyToWorkgroupMemPatterns(threadLevelTilingPatterns,
                                               workgroupSize);
      if (failed(applyPatternsAndFoldGreedily(
              funcOp, std::move(threadLevelTilingPatterns)))) {
        return signalPassFailure();
      }
      // Apply canonicalization patterns.
      RewritePatternSet threadTilingCanonicalizationPatterns =
          linalg::getLinalgTilingCanonicalizationPatterns(context);
      populateAffineMinSCFCanonicalizationPattern(
          threadTilingCanonicalizationPatterns);
      if (failed(applyPatternsAndFoldGreedily(
              funcOp, std::move(threadTilingCanonicalizationPatterns)))) {
        return signalPassFailure();
      }
    }
  }
};

}  // namespace

std::unique_ptr<OperationPass<func::FuncOp>>
createGPUDistributeSharedMemoryCopy() {
  return std::make_unique<GPUDistributeSharedMemoryCopyPass>();
}

}  // namespace iree_compiler
}  // namespace mlir
