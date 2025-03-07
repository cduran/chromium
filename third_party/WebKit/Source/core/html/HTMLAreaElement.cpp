/*
 * Copyright (C) 1999 Lars Knoll (knoll@kde.org)
 *           (C) 1999 Antti Koivisto (koivisto@kde.org)
 * Copyright (C) 2004, 2005, 2006, 2009, 2011 Apple Inc. All rights reserved.
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

#include "core/html/HTMLAreaElement.h"

#include "core/dom/ElementTraversal.h"
#include "core/html/HTMLImageElement.h"
#include "core/html/HTMLMapElement.h"
#include "core/html/parser/HTMLParserIdioms.h"
#include "core/html_names.h"
#include "core/layout/HitTestResult.h"
#include "core/layout/LayoutImage.h"
#include "platform/graphics/Path.h"
#include "platform/transforms/AffineTransform.h"

namespace blink {

namespace {

// Adapt a double to the allowed range of a LayoutUnit and narrow it to float
// precision.
float ClampCoordinate(double value) {
  return LayoutUnit(value).ToFloat();
}
}

using namespace HTMLNames;

inline HTMLAreaElement::HTMLAreaElement(Document& document)
    : HTMLAnchorElement(areaTag, document), shape_(kRect) {}

// An explicit empty destructor should be in HTMLAreaElement.cpp, because
// if an implicit destructor is used or an empty destructor is defined in
// HTMLAreaElement.h, when including HTMLAreaElement.h, msvc tries to expand
// the destructor and causes a compile error because of lack of blink::Path
// definition.
HTMLAreaElement::~HTMLAreaElement() = default;

DEFINE_NODE_FACTORY(HTMLAreaElement)

void HTMLAreaElement::ParseAttribute(
    const AttributeModificationParams& params) {
  const AtomicString& value = params.new_value;
  if (params.name == shapeAttr) {
    if (EqualIgnoringASCIICase(value, "default")) {
      shape_ = kDefault;
    } else if (EqualIgnoringASCIICase(value, "circle") ||
               EqualIgnoringASCIICase(value, "circ")) {
      shape_ = kCircle;
    } else if (EqualIgnoringASCIICase(value, "polygon") ||
               EqualIgnoringASCIICase(value, "poly")) {
      shape_ = kPoly;
    } else {
      // The missing (and implicitly invalid) value default for the
      // 'shape' attribute is 'rect'.
      shape_ = kRect;
    }
    InvalidateCachedPath();
  } else if (params.name == coordsAttr) {
    coords_ = ParseHTMLListOfFloatingPointNumbers(value.GetString());
    InvalidateCachedPath();
  } else if (params.name == altAttr || params.name == accesskeyAttr) {
    // Do nothing.
  } else {
    HTMLAnchorElement::ParseAttribute(params);
  }
}

void HTMLAreaElement::InvalidateCachedPath() {
  path_ = nullptr;
}

bool HTMLAreaElement::PointInArea(const LayoutPoint& location,
                                  const LayoutObject* container_object) const {
  return GetPath(container_object).Contains(FloatPoint(location));
}

LayoutRect HTMLAreaElement::ComputeAbsoluteRect(
    const LayoutObject* container_object) const {
  if (!container_object)
    return LayoutRect();

  // FIXME: This doesn't work correctly with transforms.
  FloatPoint abs_pos = container_object->LocalToAbsolute();

  Path path = GetPath(container_object);
  path.Translate(ToFloatSize(abs_pos));
  return EnclosingLayoutRect(path.BoundingRect());
}

Path HTMLAreaElement::GetPath(const LayoutObject* container_object) const {
  if (!container_object)
    return Path();

  // Always recompute for default shape because it depends on container object's
  // size and is cheap.
  if (shape_ == kDefault) {
    Path path;
    // No need to zoom because it is already applied in
    // containerObject->borderBoxRect().
    if (container_object->IsBox())
      path.AddRect(FloatRect(ToLayoutBox(container_object)->BorderBoxRect()));
    path_ = nullptr;
    return path;
  }

  Path path;
  if (path_) {
    path = *path_;
  } else {
    if (coords_.IsEmpty())
      return path;

    switch (shape_) {
      case kPoly:
        if (coords_.size() >= 6) {
          int num_points = coords_.size() / 2;
          path.MoveTo(FloatPoint(ClampCoordinate(coords_[0]),
                                 ClampCoordinate(coords_[1])));
          for (int i = 1; i < num_points; ++i)
            path.AddLineTo(FloatPoint(ClampCoordinate(coords_[i * 2]),
                                      ClampCoordinate(coords_[i * 2 + 1])));
          path.CloseSubpath();
          path.SetWindRule(RULE_EVENODD);
        }
        break;
      case kCircle:
        if (coords_.size() >= 3 && coords_[2] > 0) {
          float r = ClampCoordinate(coords_[2]);
          path.AddEllipse(FloatRect(ClampCoordinate(coords_[0]) - r,
                                    ClampCoordinate(coords_[1]) - r, 2 * r,
                                    2 * r));
        }
        break;
      case kRect:
        if (coords_.size() >= 4) {
          float x0 = ClampCoordinate(coords_[0]);
          float y0 = ClampCoordinate(coords_[1]);
          float x1 = ClampCoordinate(coords_[2]);
          float y1 = ClampCoordinate(coords_[3]);
          path.AddRect(FloatRect(x0, y0, x1 - x0, y1 - y0));
        }
        break;
      default:
        NOTREACHED();
        break;
    }

    // Cache the original path, not depending on containerObject.
    path_ = std::make_unique<Path>(path);
  }

  // Zoom the path into coordinates of the container object.
  float zoom_factor = container_object->StyleRef().EffectiveZoom();
  if (zoom_factor != 1.0f) {
    AffineTransform zoom_transform;
    zoom_transform.Scale(zoom_factor);
    path.Transform(zoom_transform);
  }
  return path;
}

HTMLImageElement* HTMLAreaElement::ImageElement() const {
  if (HTMLMapElement* map_element =
          Traversal<HTMLMapElement>::FirstAncestor(*this))
    return map_element->ImageElement();
  return nullptr;
}

bool HTMLAreaElement::IsKeyboardFocusable() const {
  return IsFocusable();
}

bool HTMLAreaElement::IsMouseFocusable() const {
  return IsFocusable();
}

bool HTMLAreaElement::IsFocusableStyle() const {
  HTMLImageElement* image = ImageElement();
  if (!image || !image->GetLayoutObject() ||
      image->GetLayoutObject()->Style()->Visibility() != EVisibility::kVisible)
    return false;

  return SupportsFocus() && Element::tabIndex() >= 0;
}

void HTMLAreaElement::SetFocused(bool should_be_focused,
                                 WebFocusType focus_type) {
  if (IsFocused() == should_be_focused)
    return;

  HTMLAnchorElement::SetFocused(should_be_focused, focus_type);

  HTMLImageElement* image_element = ImageElement();
  if (!image_element)
    return;

  LayoutObject* layout_object = image_element->GetLayoutObject();
  if (!layout_object || !layout_object->IsImage())
    return;

  ToLayoutImage(layout_object)->AreaElementFocusChanged(this);
}

void HTMLAreaElement::UpdateFocusAppearanceWithOptions(
    SelectionBehaviorOnFocus selection_behavior,
    const FocusOptions& options) {
  GetDocument().UpdateStyleAndLayoutTreeForNode(this);
  if (!IsFocusable())
    return;

  if (HTMLImageElement* image_element = ImageElement()) {
    image_element->UpdateFocusAppearanceWithOptions(selection_behavior,
                                                    options);
  }
}

}  // namespace blink
