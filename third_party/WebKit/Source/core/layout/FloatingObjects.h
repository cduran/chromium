/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 *           (C) 2007 David Smith (catfish.man@gmail.com)
 * Copyright (C) 2003, 2004, 2005, 2006, 2007, 2008, 2009, 2010, 2011 Apple Inc.
 * All rights reserved.
 * Copyright (C) Research In Motion Limited 2010. All rights reserved.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#ifndef FloatingObjects_h
#define FloatingObjects_h

#include <memory>
#include "base/macros.h"
#include "platform/PODFreeListArena.h"
#include "platform/PODIntervalTree.h"
#include "platform/geometry/LayoutRect.h"
#include "platform/wtf/ListHashSet.h"

namespace blink {

class LayoutBlockFlow;
class LayoutBox;
class RootInlineBox;

class FloatingObject {
  USING_FAST_MALLOC(FloatingObject);

 public:
#ifndef NDEBUG
  // Used by the PODIntervalTree for debugging the FloatingObject.
  template <class>
  friend struct ValueToString;
#endif

  // Note that Type uses bits so you can use FloatLeftRight as a mask to query
  // for both left and right.
  enum Type { kFloatLeft = 1, kFloatRight = 2, kFloatLeftRight = 3 };

  static std::unique_ptr<FloatingObject> Create(LayoutBox*);

  std::unique_ptr<FloatingObject> CopyToNewContainer(
      LayoutSize,
      bool should_paint = false,
      bool is_descendant = false) const;

  std::unique_ptr<FloatingObject> UnsafeClone() const;

  Type GetType() const { return static_cast<Type>(type_); }
  LayoutBox* GetLayoutObject() const { return layout_object_; }

  bool IsPlaced() const { return is_placed_; }
  void SetIsPlaced(bool placed = true) { is_placed_ = placed; }

  LayoutUnit X() const {
    DCHECK(IsPlaced());
    return frame_rect_.X();
  }
  LayoutUnit MaxX() const {
    DCHECK(IsPlaced());
    return frame_rect_.MaxX();
  }
  LayoutUnit Y() const {
    DCHECK(IsPlaced());
    return frame_rect_.Y();
  }
  LayoutUnit MaxY() const {
    DCHECK(IsPlaced());
    return frame_rect_.MaxY();
  }
  LayoutUnit Width() const { return frame_rect_.Width(); }
  LayoutUnit Height() const { return frame_rect_.Height(); }

  void SetX(LayoutUnit x) {
    DCHECK(!IsInPlacedTree());
    frame_rect_.SetX(x);
  }
  void SetY(LayoutUnit y) {
    DCHECK(!IsInPlacedTree());
    frame_rect_.SetY(y);
  }
  void SetWidth(LayoutUnit width) {
    DCHECK(!IsInPlacedTree());
    frame_rect_.SetWidth(width);
  }
  void SetHeight(LayoutUnit height) {
    DCHECK(!IsInPlacedTree());
    frame_rect_.SetHeight(height);
  }

  const LayoutRect& FrameRect() const {
    DCHECK(IsPlaced());
    return frame_rect_;
  }

  bool IsInPlacedTree() const { return is_in_placed_tree_; }
  void SetIsInPlacedTree(bool value) { is_in_placed_tree_ = value; }

  bool ShouldPaint() const { return should_paint_; }
  void SetShouldPaint(bool should_paint) { should_paint_ = should_paint; }
  bool IsDescendant() const { return is_descendant_; }
  void SetIsDescendant(bool is_descendant) { is_descendant_ = is_descendant; }
  bool IsLowestNonOverhangingFloatInChild() const {
    return is_lowest_non_overhanging_float_in_child_;
  }
  void SetIsLowestNonOverhangingFloatInChild(
      bool is_lowest_non_overhanging_float_in_child) {
    is_lowest_non_overhanging_float_in_child_ =
        is_lowest_non_overhanging_float_in_child;
  }

  // FIXME: Callers of these methods are dangerous and should be whitelisted
  // explicitly or removed.
  RootInlineBox* OriginatingLine() const { return originating_line_; }
  void SetOriginatingLine(RootInlineBox* line) { originating_line_ = line; }

 private:
  explicit FloatingObject(LayoutBox*);
  FloatingObject(LayoutBox*,
                 Type,
                 const LayoutRect&,
                 bool should_paint,
                 bool is_descendant,
                 bool is_lowest_non_overhanging_float_in_child);

  LayoutBox* layout_object_;
  RootInlineBox* originating_line_;
  LayoutRect frame_rect_;

  unsigned type_ : 2;  // Type (left or right aligned)
  unsigned should_paint_ : 1;
  unsigned is_descendant_ : 1;
  unsigned is_placed_ : 1;
  unsigned is_lowest_non_overhanging_float_in_child_ : 1;
  unsigned is_in_placed_tree_ : 1;
  DISALLOW_COPY_AND_ASSIGN(FloatingObject);
};

struct FloatingObjectHashFunctions {
  STATIC_ONLY(FloatingObjectHashFunctions);
  static unsigned GetHash(FloatingObject* key) {
    return DefaultHash<LayoutBox*>::Hash::GetHash(key->GetLayoutObject());
  }
  static unsigned GetHash(const std::unique_ptr<FloatingObject>& key) {
    return GetHash(key.get());
  }
  static bool Equal(std::unique_ptr<FloatingObject>& a, FloatingObject* b) {
    return a->GetLayoutObject() == b->GetLayoutObject();
  }
  static bool Equal(std::unique_ptr<FloatingObject>& a,
                    const std::unique_ptr<FloatingObject>& b) {
    return Equal(a, b.get());
  }

  static const bool safe_to_compare_to_empty_or_deleted = true;
};
struct FloatingObjectHashTranslator {
  STATIC_ONLY(FloatingObjectHashTranslator);
  static unsigned GetHash(LayoutBox* key) {
    return DefaultHash<LayoutBox*>::Hash::GetHash(key);
  }
  static bool Equal(FloatingObject* a, LayoutBox* b) {
    return a->GetLayoutObject() == b;
  }
  static bool Equal(const std::unique_ptr<FloatingObject>& a, LayoutBox* b) {
    return a->GetLayoutObject() == b;
  }
};
typedef ListHashSet<std::unique_ptr<FloatingObject>,
                    4,
                    FloatingObjectHashFunctions>
    FloatingObjectSet;
typedef FloatingObjectSet::const_iterator FloatingObjectSetIterator;
typedef PODInterval<LayoutUnit, FloatingObject*> FloatingObjectInterval;
typedef PODIntervalTree<LayoutUnit, FloatingObject*> FloatingObjectTree;
typedef PODFreeListArena<PODRedBlackTree<FloatingObjectInterval>::Node>
    IntervalArena;
typedef HashMap<LayoutBox*, std::unique_ptr<FloatingObject>>
    LayoutBoxToFloatInfoMap;

class FloatingObjects {
  USING_FAST_MALLOC(FloatingObjects);

 public:
  FloatingObjects(const LayoutBlockFlow*, bool horizontal_writing_mode);
  ~FloatingObjects();

  void Clear();
  void MoveAllToFloatInfoMap(LayoutBoxToFloatInfoMap&);
  FloatingObject* Add(std::unique_ptr<FloatingObject>);
  void Remove(FloatingObject*);
  void AddPlacedObject(FloatingObject&);
  void RemovePlacedObject(FloatingObject&);
  void SetHorizontalWritingMode(bool b = true) { horizontal_writing_mode_ = b; }

  bool HasLeftObjects() const { return left_objects_count_ > 0; }
  bool HasRightObjects() const { return right_objects_count_ > 0; }
  const FloatingObjectSet& Set() const { return set_; }
  FloatingObjectSet& MutableSet() { return set_; }
  void ClearLineBoxTreePointers();

  LayoutUnit LogicalLeftOffset(LayoutUnit fixed_offset,
                               LayoutUnit logical_top,
                               LayoutUnit logical_height);
  LayoutUnit LogicalRightOffset(LayoutUnit fixed_offset,
                                LayoutUnit logical_top,
                                LayoutUnit logical_height);

  LayoutUnit LogicalLeftOffsetForPositioningFloat(LayoutUnit fixed_offset,
                                                  LayoutUnit logical_top,
                                                  LayoutUnit* height_remaining);
  LayoutUnit LogicalRightOffsetForPositioningFloat(
      LayoutUnit fixed_offset,
      LayoutUnit logical_top,
      LayoutUnit* height_remaining);

  LayoutUnit LogicalLeftOffsetForAvoidingFloats(LayoutUnit fixed_offset,
                                                LayoutUnit logical_top,
                                                LayoutUnit logical_height);
  LayoutUnit LogicalRightOffsetForAvoidingFloats(LayoutUnit fixed_offset,
                                                 LayoutUnit logical_top,
                                                 LayoutUnit logical_height);

  LayoutUnit FindNextFloatLogicalBottomBelow(LayoutUnit logical_height);
  LayoutUnit FindNextFloatLogicalBottomBelowForBlock(LayoutUnit logical_height);

  LayoutUnit LowestFloatLogicalBottom(FloatingObject::Type);
  FloatingObject* LowestFloatingObject() const;

 private:
  bool HasLowestFloatLogicalBottomCached(bool is_horizontal,
                                         FloatingObject::Type float_type) const;
  LayoutUnit GetCachedlowestFloatLogicalBottom(
      FloatingObject::Type float_type) const;
  void SetCachedLowestFloatLogicalBottom(bool is_horizontal,
                                         FloatingObject::Type float_type,
                                         FloatingObject*);
  void MarkLowestFloatLogicalBottomCacheAsDirty();

  void ComputePlacedFloatsTree();
  const FloatingObjectTree& PlacedFloatsTree() {
    if (!placed_floats_tree_.IsInitialized())
      ComputePlacedFloatsTree();
    return placed_floats_tree_;
  }
  void IncreaseObjectsCount(FloatingObject::Type);
  void DecreaseObjectsCount(FloatingObject::Type);
  FloatingObjectInterval IntervalForFloatingObject(FloatingObject&);

  FloatingObjectSet set_;
  FloatingObjectTree placed_floats_tree_;
  unsigned left_objects_count_;
  unsigned right_objects_count_;
  bool horizontal_writing_mode_;
  const LayoutBlockFlow* layout_object_;

  struct FloatBottomCachedValue {
    FloatBottomCachedValue();
    FloatingObject* floating_object;
    bool dirty;
  };
  FloatBottomCachedValue lowest_float_bottom_cache_[2];
  bool cached_horizontal_writing_mode_;
  DISALLOW_COPY_AND_ASSIGN(FloatingObjects);
};

#ifndef NDEBUG
// These structures are used by PODIntervalTree for debugging purposes.
template <>
struct ValueToString<LayoutUnit> {
  static String ToString(const LayoutUnit value);
};
template <>
struct ValueToString<FloatingObject*> {
  static String ToString(const FloatingObject*);
};
#endif

}  // namespace blink

#endif  // FloatingObjects_h
