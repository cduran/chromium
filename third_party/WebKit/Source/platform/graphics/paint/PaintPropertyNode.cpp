// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/graphics/paint/PaintPropertyNode.h"

#include "platform/graphics/paint/ClipPaintPropertyNode.h"
#include "platform/graphics/paint/EffectPaintPropertyNode.h"
#include "platform/graphics/paint/ScrollPaintPropertyNode.h"
#include "platform/graphics/paint/TransformPaintPropertyNode.h"

namespace blink {

namespace {

// Returns -1 if |maybe_ancestor| is found in the ancestor chain, or returns
// the depth of the node from the root.
template <typename NodeType>
int NodeDepthOrFoundAncestor(const NodeType& node,
                             const NodeType& maybe_ancestor) {
  int depth = 0;
  for (const NodeType* n = &node; n; n = n->Parent()) {
    if (n == &maybe_ancestor)
      return -1;
    depth++;
  }
  return depth;
}

template <typename NodeType>
const NodeType& LowestCommonAncestorTemplate(const NodeType& a,
                                             const NodeType& b) {
  // Measure both depths.
  auto depth_a = NodeDepthOrFoundAncestor(a, b);
  if (depth_a == -1)
    return b;
  auto depth_b = NodeDepthOrFoundAncestor(b, a);
  if (depth_b == -1)
    return a;

  const auto* a_ptr = &a;
  const auto* b_ptr = &b;

  // Make it so depthA >= depthB.
  if (depth_a < depth_b) {
    std::swap(a_ptr, b_ptr);
    std::swap(depth_a, depth_b);
  }

  // Make it so depthA == depthB.
  while (depth_a > depth_b) {
    a_ptr = a_ptr->Parent();
    depth_a--;
  }

  // Walk up until we find the ancestor.
  while (a_ptr != b_ptr) {
    a_ptr = a_ptr->Parent();
    b_ptr = b_ptr->Parent();
  }

  DCHECK(a_ptr) << "Malformed property tree. All nodes must be descendant of "
                   "the same root.";
  return *a_ptr;
}

}  // namespace

const TransformPaintPropertyNode& LowestCommonAncestorInternal(
    const TransformPaintPropertyNode& a,
    const TransformPaintPropertyNode& b) {
  return LowestCommonAncestorTemplate(a, b);
}

const ClipPaintPropertyNode& LowestCommonAncestorInternal(
    const ClipPaintPropertyNode& a,
    const ClipPaintPropertyNode& b) {
  return LowestCommonAncestorTemplate(a, b);
}

const EffectPaintPropertyNode& LowestCommonAncestorInternal(
    const EffectPaintPropertyNode& a,
    const EffectPaintPropertyNode& b) {
  return LowestCommonAncestorTemplate(a, b);
}

const ScrollPaintPropertyNode& LowestCommonAncestorInternal(
    const ScrollPaintPropertyNode& a,
    const ScrollPaintPropertyNode& b) {
  return LowestCommonAncestorTemplate(a, b);
}

}  // namespace blink
