// Copyright 2024 The IREE Authors
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "iree/compiler/Codegen/Dialect/Codegen/IR/IREECodegenAttrs.h"
#include "iree/compiler/Codegen/Dialect/Codegen/IR/IREECodegenDialect.h"
#include "iree/compiler/Codegen/LLVMGPU/ROCDLKernelConfig.h"
#include "iree/compiler/Codegen/LLVMGPU/ROCDLPassDetail.h"
#include "iree/compiler/Codegen/LLVMGPU/ROCDLPasses.h"
#include "iree/compiler/Dialect/HAL/IR/HALOps.h"
#include "mlir/Dialect/Bufferization/IR/Bufferization.h"
#include "mlir/Pass/Pass.h"
#include "mlir/Pass/PassManager.h"

namespace mlir::iree_compiler {

namespace {
/// Selects a strategy for lowering an IREE hal.executable.variant to ROCDL.
class ROCDLSelectLoweringStrategyPass
    : public ROCDLSelectLoweringStrategyBase<ROCDLSelectLoweringStrategyPass> {
public:
  void getDependentDialects(DialectRegistry &registry) const override {
    // clang-format off
    registry
        .insert<IREE::Codegen::IREECodegenDialect,
                bufferization::BufferizationDialect>();
    // clang-format on
  }

  void runOnOperation() override {
    auto moduleOp = getOperation();
    for (auto funcOp : moduleOp.getOps<FunctionOpInterface>()) {
      if (failed(initROCDLLaunchConfig(funcOp))) {
        funcOp.emitOpError("failed to set configuration");
        return signalPassFailure();
      }
    }
  }
};
} // namespace

std::unique_ptr<OperationPass<ModuleOp>>
createROCDLSelectLoweringStrategyPass() {
  return std::make_unique<ROCDLSelectLoweringStrategyPass>();
}

} // namespace mlir::iree_compiler
