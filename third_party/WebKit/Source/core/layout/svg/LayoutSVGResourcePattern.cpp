/*
 * Copyright (C) 2006 Nikolas Zimmermann <zimmermann@kde.org>
 * Copyright (C) Research In Motion Limited 2010. All rights reserved.
 * Copyright 2014 The Chromium Authors. All rights reserved.
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

#include "core/layout/svg/LayoutSVGResourcePattern.h"

#include <memory>

#include "base/memory/ptr_util.h"
#include "core/layout/svg/SVGLayoutSupport.h"
#include "core/layout/svg/SVGResources.h"
#include "core/layout/svg/SVGResourcesCache.h"
#include "core/paint/SVGPaintContext.h"
#include "core/paint/TransformRecorder.h"
#include "core/svg/SVGFitToViewBox.h"
#include "core/svg/SVGPatternElement.h"
#include "platform/graphics/GraphicsContext.h"
#include "platform/graphics/paint/PaintController.h"
#include "platform/graphics/paint/PaintRecord.h"
#include "platform/graphics/paint/PaintRecordBuilder.h"

namespace blink {

struct PatternData {
  USING_FAST_MALLOC(PatternData);

 public:
  scoped_refptr<Pattern> pattern;
  AffineTransform transform;
};

LayoutSVGResourcePattern::LayoutSVGResourcePattern(SVGPatternElement* node)
    : LayoutSVGResourcePaintServer(node),
      should_collect_pattern_attributes_(true),
      attributes_wrapper_(PatternAttributesWrapper::Create()) {}

void LayoutSVGResourcePattern::RemoveAllClientsFromCache(
    bool mark_for_invalidation) {
  pattern_map_.clear();
  should_collect_pattern_attributes_ = true;
  MarkAllClientsForInvalidation(
      mark_for_invalidation ? kPaintInvalidation : kParentOnlyInvalidation);
}

bool LayoutSVGResourcePattern::RemoveClientFromCache(LayoutObject& client) {
  auto entry = pattern_map_.find(&client);
  if (entry == pattern_map_.end())
    return false;
  pattern_map_.erase(entry);
  return true;
}

PatternData* LayoutSVGResourcePattern::PatternForClient(
    const LayoutObject& object,
    const FloatRect& object_bounding_box) {
  DCHECK(!should_collect_pattern_attributes_);

  // FIXME: the double hash lookup is needed to guard against paint-time
  // invalidation (painting animated images may trigger layout invals which
  // delete our map entry). Hopefully that will be addressed at some point, and
  // then we can optimize the lookup.
  if (PatternData* current_data = pattern_map_.at(&object))
    return current_data;

  return pattern_map_.Set(&object, BuildPatternData(object_bounding_box))
      .stored_value->value.get();
}

std::unique_ptr<PatternData> LayoutSVGResourcePattern::BuildPatternData(
    const FloatRect& object_bounding_box) {
  // If we couldn't determine the pattern content element root, stop here.
  const PatternAttributes& attributes = Attributes();
  if (!attributes.PatternContentElement())
    return nullptr;

  // An empty viewBox disables layout.
  if (attributes.HasViewBox() && attributes.ViewBox().IsEmpty())
    return nullptr;

  DCHECK(GetElement());
  // Compute tile metrics.
  FloatRect tile_bounds = SVGLengthContext::ResolveRectangle(
      GetElement(), attributes.PatternUnits(), object_bounding_box,
      *attributes.X(), *attributes.Y(), *attributes.Width(),
      *attributes.Height());
  if (tile_bounds.IsEmpty())
    return nullptr;

  AffineTransform tile_transform;
  if (attributes.HasViewBox()) {
    if (attributes.ViewBox().IsEmpty())
      return nullptr;
    tile_transform = SVGFitToViewBox::ViewBoxToViewTransform(
        attributes.ViewBox(), attributes.PreserveAspectRatio(),
        tile_bounds.Width(), tile_bounds.Height());
  } else {
    // A viewbox overrides patternContentUnits, per spec.
    if (attributes.PatternContentUnits() ==
        SVGUnitTypes::kSvgUnitTypeObjectboundingbox) {
      tile_transform.Scale(object_bounding_box.Width(),
                           object_bounding_box.Height());
    }
  }

  std::unique_ptr<PatternData> pattern_data = base::WrapUnique(new PatternData);
  pattern_data->pattern = Pattern::CreatePaintRecordPattern(
      AsPaintRecord(tile_bounds.Size(), tile_transform),
      FloatRect(FloatPoint(), tile_bounds.Size()));

  // Compute pattern space transformation.
  pattern_data->transform.Translate(tile_bounds.X(), tile_bounds.Y());
  pattern_data->transform.PreMultiply(attributes.PatternTransform());

  return pattern_data;
}

SVGPaintServer LayoutSVGResourcePattern::PreparePaintServer(
    const LayoutObject& object,
    const FloatRect& object_bounding_box) {
  ClearInvalidationMask();

  // Validate pattern DOM state before building the actual
  // pattern. This should avoid tearing down the pattern we're
  // currently working on. Preferably the state validation should have
  // no side-effects though.
  if (should_collect_pattern_attributes_) {
    attributes_wrapper_->Set(PatternAttributes());
    SVGPatternElement* pattern_element = ToSVGPatternElement(GetElement());
    pattern_element->CollectPatternAttributes(MutableAttributes());
    should_collect_pattern_attributes_ = false;
  }

  // Spec: When the geometry of the applicable element has no width or height
  // and objectBoundingBox is specified, then the given effect (e.g. a gradient
  // or a filter) will be ignored.
  if (Attributes().PatternUnits() ==
          SVGUnitTypes::kSvgUnitTypeObjectboundingbox &&
      object_bounding_box.IsEmpty())
    return SVGPaintServer::Invalid();

  PatternData* pattern_data = PatternForClient(object, object_bounding_box);
  if (!pattern_data || !pattern_data->pattern)
    return SVGPaintServer::Invalid();

  return SVGPaintServer(pattern_data->pattern, pattern_data->transform);
}

const LayoutSVGResourceContainer*
LayoutSVGResourcePattern::ResolveContentElement() const {
  DCHECK(Attributes().PatternContentElement());
  LayoutSVGResourceContainer* expected_layout_object =
      ToLayoutSVGResourceContainer(
          Attributes().PatternContentElement()->GetLayoutObject());
  // No content inheritance - avoid walking the inheritance chain.
  if (this == expected_layout_object)
    return this;
  // Walk the inheritance chain on the LayoutObject-side. If we reach the
  // expected LayoutObject, all is fine. If we don't, there's a cycle that
  // the cycle resolver did break, and the resource will be content-less.
  const LayoutSVGResourceContainer* content_layout_object = this;
  while (SVGResources* resources =
             SVGResourcesCache::CachedResourcesForLayoutObject(
                 *content_layout_object)) {
    LayoutSVGResourceContainer* linked_resource = resources->LinkedResource();
    if (!linked_resource)
      break;
    if (linked_resource == expected_layout_object)
      return expected_layout_object;
    content_layout_object = linked_resource;
  }
  // There was a cycle, just use this resource as the "content resource" even
  // though it will be empty (have no children).
  return this;
}

sk_sp<PaintRecord> LayoutSVGResourcePattern::AsPaintRecord(
    const FloatSize& size,
    const AffineTransform& tile_transform) const {
  DCHECK(!should_collect_pattern_attributes_);

  AffineTransform content_transform;
  if (Attributes().PatternContentUnits() ==
      SVGUnitTypes::kSvgUnitTypeObjectboundingbox)
    content_transform = tile_transform;

  FloatRect bounds(FloatPoint(), size);
  const LayoutSVGResourceContainer* pattern_layout_object =
      ResolveContentElement();
  DCHECK(pattern_layout_object);
  DCHECK(!pattern_layout_object->NeedsLayout());

  SubtreeContentTransformScope content_transform_scope(content_transform);

  PaintRecordBuilder builder;
  for (LayoutObject* child = pattern_layout_object->FirstChild(); child;
       child = child->NextSibling())
    SVGPaintContext::PaintResourceSubtree(builder.Context(), child);
  PaintRecorder paint_recorder;
  PaintCanvas* canvas = paint_recorder.beginRecording(bounds);
  canvas->save();
  canvas->concat(AffineTransformToSkMatrix(tile_transform));
  builder.EndRecording(*canvas);
  canvas->restore();
  return paint_recorder.finishRecordingAsPicture();
}

}  // namespace blink
