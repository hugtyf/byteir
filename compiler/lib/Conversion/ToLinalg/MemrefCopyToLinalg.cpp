//===- MemrefCopyToLinalg.cpp --------------------------------*--- C++ -*-===//
//
// Copyright 2022 ByteDance Ltd. and/or its affiliates. All rights reserved.
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//    http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
//===----------------------------------------------------------------------===//
// Some code comes from MemrefCopyInLinalg.cpp in IREE project
// Original license:
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "byteir/Conversion/ToLinalg/ToLinalg.h"
#include "byteir/Dialect/Byre/Common.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"
#include "mlir/Dialect/Linalg/IR/Linalg.h"
#include "mlir/Dialect/MemRef/IR/MemRef.h"
#include "mlir/IR/PatternMatch.h"
#include "mlir/Transforms/GreedyPatternRewriteDriver.h"

#include "../PassDetail.h"

namespace mlir {

namespace {
struct MemrefCopyOpToLinalg : public OpRewritePattern<memref::CopyOp> {
  using OpRewritePattern<memref::CopyOp>::OpRewritePattern;
  MemrefCopyOpToLinalg(MLIRContext *ctx, std::string anchorTag,
                       std::string attachAttr)
      : OpRewritePattern(ctx), anchorTag(anchorTag), attachAttr(attachAttr) {}

  LogicalResult matchAndRewrite(memref::CopyOp copyOp,
                                PatternRewriter &rewriter) const override {
    auto parentOp = copyOp->getParentOfType<func::FuncOp>();
    if (!parentOp)
      return failure();

    if (!anchorTag.empty() && !parentOp->hasAttr(anchorTag))
      return failure();

    Value src = copyOp.getSource(), dst = copyOp.getTarget();
    auto srcType = llvm::dyn_cast<MemRefType>(src.getType());
    auto dstType = llvm::dyn_cast<MemRefType>(dst.getType());
    if (!srcType || !dstType)
      return failure();
    if (srcType.getLayout().isIdentity() && dstType.getLayout().isIdentity())
      return failure();

    SmallVector<Operation *> ops;
    auto getViewSource = [&](Value value) {
      while (auto viewOp = value.getDefiningOp<ViewLikeOpInterface>()) {
        ops.push_back(viewOp);
        value = viewOp.getViewSource();
      }
      return value;
    };
    Value callSrc = getViewSource(src);
    Value callDst = getViewSource(dst);

    auto symbolTableOp = SymbolTable::getNearestSymbolTable(copyOp);
    SymbolTable symbolTable(symbolTableOp);
    auto funcType =
        rewriter.getFunctionType({callSrc.getType(), callDst.getType()}, {});

    OpBuilder::InsertionGuard guard(rewriter);
    // Insert before module terminator.
    rewriter.setInsertionPoint(parentOp);
    func::FuncOp funcOp = rewriter.create<func::FuncOp>(
        copyOp->getLoc(), "memref_copy_kernel", funcType);
    symbolTable.insert(funcOp);
    funcOp.setPrivate();

    Block *entryBlock = funcOp.addEntryBlock();
    rewriter.setInsertionPointToStart(entryBlock);
    IRMapping mapping;
    mapping.map(ValueRange{callSrc, callDst}, entryBlock->getArguments());
    for (auto &&op : llvm::reverse(ops)) {
      auto newOp = rewriter.clone(*op, mapping);
      mapping.map(op, newOp);
    }
    AffineMap id = AffineMap::getMultiDimIdentityMap(dstType.getRank(),
                                                     rewriter.getContext());
    SmallVector<utils::IteratorType> iteratorTypes(
        dstType.getRank(), utils::IteratorType::parallel);
    rewriter.create<linalg::GenericOp>(
        copyOp->getLoc(), mapping.lookup(copyOp.getSource()),
        mapping.lookup(copyOp.getTarget()), llvm::ArrayRef({id, id}),
        iteratorTypes,
        [](OpBuilder &b, Location loc, ValueRange args) {
          b.create<linalg::YieldOp>(loc, args.front());
        },
        copyOp->getAttrs());
    rewriter.create<func::ReturnOp>(copyOp->getLoc());
    if (!attachAttr.empty()) {
      funcOp->setAttr(attachAttr, rewriter.getUnitAttr());
    }

    rewriter.setInsertionPoint(copyOp);
    auto callOp = rewriter.replaceOpWithNewOp<func::CallOp>(
        copyOp, funcOp, ValueRange{callSrc, callDst});
    callOp->setAttr(byre::getByreCallOpReadonlyOperandNumAttrName(),
                    rewriter.getIndexAttr(1));
    return success();
  }

private:
  std::string anchorTag;
  std::string attachAttr;
};

struct MemrefCopyToLinalgPass
    : public MemrefCopyToLinalgPassBase<MemrefCopyToLinalgPass> {
  MemrefCopyToLinalgPass(std::string anchorTag, std::string attachAttr)
      : MemrefCopyToLinalgPassBase() {
    this->anchorTag = anchorTag;
    this->attachAttr = attachAttr;
  }

  void runOnOperation() override {
    MLIRContext *context = &getContext();
    RewritePatternSet patterns(&getContext());
    patterns.insert<MemrefCopyOpToLinalg>(context, this->anchorTag,
                                          this->attachAttr);
    if (failed(applyPatternsAndFoldGreedily(getOperation(),
                                            std::move(patterns)))) {
      return signalPassFailure();
    }
  }
};

} // namespace

std::unique_ptr<OperationPass<ModuleOp>>
createMemrefCopyToLinalgPass(std::string anchorTag, std::string attachAttr) {
  return std::make_unique<MemrefCopyToLinalgPass>(anchorTag, attachAttr);
}

} // namespace mlir
