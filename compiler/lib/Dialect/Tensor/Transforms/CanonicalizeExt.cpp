//===- CanonicalizeExt.cpp ------------------------------------*--- C++ -*-===//
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
// Some code comes from DropUnitDims.cpp in LLVM project
// Original license:
//
// Licensed under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "byteir/Dialect/Tensor/Transforms/CanonicalizeExt.h"
#include "byteir/Utils/AttrUtils.h"
#include "byteir/Utils/Utils.h"
#include "mlir/Dialect/Arith/IR/Arith.h"
#include "mlir/Dialect/Tensor/IR/Tensor.h"
#include "mlir/IR/Matchers.h"
#include "mlir/IR/TypeUtilities.h"
#include "mlir/Interfaces/ViewLikeInterface.h"
#include "llvm/ADT/APFloat.h"
#include "llvm/ADT/APInt.h"
#include "llvm/ADT/APSInt.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/Support/Debug.h"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <numeric>
#include <set>
#include <unordered_map>
#include <utility>

#define DEBUG_TYPE "tensor-canonicalize-ext"

#define K_INITIAL -999

using namespace mlir;

namespace {
std::optional<SmallVector<ReassociationIndices>>
getReassociationMapForFoldingUnitDims(ArrayRef<OpFoldResult> mixedSizes) {
  SmallVector<ReassociationIndices> reassociation;
  ReassociationIndices curr;
  for (const auto &it : llvm::enumerate(mixedSizes)) {
    auto dim = it.index();
    auto size = it.value();
    curr.push_back(dim);
    auto attr = size.dyn_cast<Attribute>();
    if (attr && attr.cast<IntegerAttr>().getInt() == 1)
      continue;
    reassociation.emplace_back(ReassociationIndices{});
    std::swap(reassociation.back(), curr);
  }
  // When the reassociations are not empty, then fold the remaining
  // unit-dimensions into the last dimension.  If the reassociations so far is
  // empty, then leave it emtpy. This will fold everything to a rank-0 tensor.
  if (!curr.empty() && !reassociation.empty())
    reassociation.back().append(curr.begin(), curr.end());
  return reassociation;
}

/// Fold extract_slice + collapse_shape into rank reduced extract_slice
///
/// Example:
///
/// %0 = tensor.extract_slice %arg0[0, 0, 0][1, 1024, 1][1, 1, 1] :
///        tensor<19x1024xi32> to tensor<1x1024x1xi32>
/// %1 = tensor.collapse_shape %0 [[0, 1, 2]] :
///        tensor<1x1024x1xi32> into tensor<1024xi32>
///
/// will be folded into
///
/// %0 = tensor.extract_slice %arg0[0, 0, 0][1, 1024, 1][1, 1, 1] :
///        tensor<19x1024xi32> to tensor<1024xi32>
struct RankReducedExtractSliceCollapseShape
    : public OpRewritePattern<tensor::CollapseShapeOp> {
  using OpRewritePattern<tensor::CollapseShapeOp>::OpRewritePattern;

  LogicalResult matchAndRewrite(tensor::CollapseShapeOp collapseOp,
                                PatternRewriter &rewriter) const override {
    tensor::ExtractSliceOp sliceOp =
        collapseOp.getSrc().getDefiningOp<tensor::ExtractSliceOp>();
    if (!sliceOp)
      return failure();

    RankedTensorType resultType = sliceOp.getType();
    SmallVector<OpFoldResult> offsets = sliceOp.getMixedOffsets();
    SmallVector<OpFoldResult> sizes = sliceOp.getMixedSizes();
    SmallVector<OpFoldResult> strides = sliceOp.getMixedStrides();
    auto reassociation = getReassociationMapForFoldingUnitDims(sizes);
    if (!reassociation ||
        reassociation->size() == static_cast<size_t>(resultType.getRank()) ||
        *reassociation != collapseOp.getReassociationIndices())
      return failure();

    auto rankReducedType =
        tensor::ExtractSliceOp::inferCanonicalRankReducedResultType(
            reassociation->size(), sliceOp.getSourceType(), offsets, sizes,
            strides)
            .cast<RankedTensorType>();
    if (rankReducedType != collapseOp.getType())
      return failure();

    rewriter.replaceOpWithNewOp<tensor::ExtractSliceOp>(
        collapseOp, rankReducedType, sliceOp.getSource(), offsets, sizes,
        strides);
    return success();
  }
};
} // namespace

void mlir::tensor::populateCanonicalizeExtPatterns(RewritePatternSet &patterns,
                                                   MLIRContext *ctx,
                                                   bool blindFold) {
  if (blindFold) {
    populateFoldConstantExtractSlicePatterns(
        patterns, [](ExtractSliceOp op) { return true; });
  }

  patterns.add<RankReducedExtractSliceCollapseShape>(ctx);
}

void mlir::tensor::getCanonicalizationExtPatterns(RewritePatternSet &patterns,
                                                  MLIRContext *ctx,
                                                  bool blindFold) {

  // add dialect level getCanonicalizationPatterns
  auto tensorDialect = ctx->getOrLoadDialect<tensor::TensorDialect>();
  if (tensorDialect) {
    tensorDialect->getCanonicalizationPatterns(patterns);
  }

  // add op level  getCanonicalizationPatterns
  for (RegisteredOperationName op : ctx->getRegisteredOperations()) {
    // only add tensor-related
    if (isa<tensor::TensorDialect>(op.getDialect())) {
      op.getCanonicalizationPatterns(patterns, ctx);
    }
  }

  // add our extension
  populateCanonicalizeExtPatterns(patterns, ctx, blindFold);
}
