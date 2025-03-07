/*
 * Copyright (C) 2011 Apple Inc. All rights reserved.
 * Copyright (C) 2013 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL APPLE COMPUTER, INC. OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "core/paint/FilterEffectBuilder.h"

#include <algorithm>
#include "core/style/FilterOperations.h"
#include "core/svg/SVGFilterElement.h"
#include "core/svg/SVGLengthContext.h"
#include "core/svg/SVGResource.h"
#include "core/svg/graphics/filters/SVGFilterBuilder.h"
#include "platform/LengthFunctions.h"
#include "platform/graphics/CompositorFilterOperations.h"
#include "platform/graphics/InterpolationSpace.h"
#include "platform/graphics/filters/FEBoxReflect.h"
#include "platform/graphics/filters/FEColorMatrix.h"
#include "platform/graphics/filters/FEComponentTransfer.h"
#include "platform/graphics/filters/FEDropShadow.h"
#include "platform/graphics/filters/FEGaussianBlur.h"
#include "platform/graphics/filters/Filter.h"
#include "platform/graphics/filters/FilterEffect.h"
#include "platform/graphics/filters/PaintFilterBuilder.h"
#include "platform/graphics/filters/SourceGraphic.h"
#include "platform/wtf/MathExtras.h"
#include "public/platform/WebPoint.h"

namespace blink {

namespace {

inline void EndMatrixRow(Vector<float>& matrix) {
  matrix.UncheckedAppend(0);
  matrix.UncheckedAppend(0);
}

inline void LastMatrixRow(Vector<float>& matrix) {
  matrix.UncheckedAppend(0);
  matrix.UncheckedAppend(0);
  matrix.UncheckedAppend(0);
  matrix.UncheckedAppend(1);
  matrix.UncheckedAppend(0);
}

Vector<float> GrayscaleMatrix(double amount) {
  double one_minus_amount = clampTo(1 - amount, 0.0, 1.0);

  // See https://drafts.fxtf.org/filter-effects/#grayscaleEquivalent for
  // information on parameters.
  Vector<float> matrix;
  matrix.ReserveInitialCapacity(20);

  matrix.UncheckedAppend(clampTo<float>(0.2126 + 0.7874 * one_minus_amount));
  matrix.UncheckedAppend(clampTo<float>(0.7152 - 0.7152 * one_minus_amount));
  matrix.UncheckedAppend(clampTo<float>(0.0722 - 0.0722 * one_minus_amount));
  EndMatrixRow(matrix);

  matrix.UncheckedAppend(clampTo<float>(0.2126 - 0.2126 * one_minus_amount));
  matrix.UncheckedAppend(clampTo<float>(0.7152 + 0.2848 * one_minus_amount));
  matrix.UncheckedAppend(clampTo<float>(0.0722 - 0.0722 * one_minus_amount));
  EndMatrixRow(matrix);

  matrix.UncheckedAppend(clampTo<float>(0.2126 - 0.2126 * one_minus_amount));
  matrix.UncheckedAppend(clampTo<float>(0.7152 - 0.7152 * one_minus_amount));
  matrix.UncheckedAppend(clampTo<float>(0.0722 + 0.9278 * one_minus_amount));
  EndMatrixRow(matrix);

  LastMatrixRow(matrix);
  return matrix;
}

Vector<float> SepiaMatrix(double amount) {
  double one_minus_amount = clampTo(1 - amount, 0.0, 1.0);

  // See https://drafts.fxtf.org/filter-effects/#sepiaEquivalent for information
  // on parameters.
  Vector<float> matrix;
  matrix.ReserveInitialCapacity(20);

  matrix.UncheckedAppend(clampTo<float>(0.393 + 0.607 * one_minus_amount));
  matrix.UncheckedAppend(clampTo<float>(0.769 - 0.769 * one_minus_amount));
  matrix.UncheckedAppend(clampTo<float>(0.189 - 0.189 * one_minus_amount));
  EndMatrixRow(matrix);

  matrix.UncheckedAppend(clampTo<float>(0.349 - 0.349 * one_minus_amount));
  matrix.UncheckedAppend(clampTo<float>(0.686 + 0.314 * one_minus_amount));
  matrix.UncheckedAppend(clampTo<float>(0.168 - 0.168 * one_minus_amount));
  EndMatrixRow(matrix);

  matrix.UncheckedAppend(clampTo<float>(0.272 - 0.272 * one_minus_amount));
  matrix.UncheckedAppend(clampTo<float>(0.534 - 0.534 * one_minus_amount));
  matrix.UncheckedAppend(clampTo<float>(0.131 + 0.869 * one_minus_amount));
  EndMatrixRow(matrix);

  LastMatrixRow(matrix);
  return matrix;
}

}  // namespace

FilterEffectBuilder::FilterEffectBuilder(const FloatRect& reference_box,
                                         float zoom,
                                         const PaintFlags* fill_flags,
                                         const PaintFlags* stroke_flags)
    : reference_box_(reference_box),
      zoom_(zoom),
      fill_flags_(fill_flags),
      stroke_flags_(stroke_flags) {}

FilterEffect* FilterEffectBuilder::BuildFilterEffect(
    const FilterOperations& operations,
    bool input_tainted) const {
  // Create a parent filter for shorthand filters. These have already been
  // scaled by the CSS code for page zoom, so scale is 1.0 here.
  Filter* parent_filter = Filter::Create(1.0f);
  FilterEffect* previous_effect = parent_filter->GetSourceGraphic();
  if (input_tainted)
    previous_effect->SetOriginTainted();
  for (FilterOperation* filter_operation : operations.Operations()) {
    FilterEffect* effect = nullptr;
    switch (filter_operation->GetType()) {
      case FilterOperation::REFERENCE: {
        ReferenceFilterOperation& reference_operation =
            ToReferenceFilterOperation(*filter_operation);
        Filter* reference_filter =
            BuildReferenceFilter(reference_operation, previous_effect);
        if (reference_filter) {
          effect = reference_filter->LastEffect();
          // TODO(fs): This is essentially only needed for the
          // side-effects (mapRect). The filter differs from the one
          // computed just above in what the SourceGraphic is, and how
          // it's connected to the filter-chain.
          reference_filter = BuildReferenceFilter(reference_operation, nullptr);
        }
        reference_operation.SetFilter(reference_filter);
        break;
      }
      case FilterOperation::GRAYSCALE: {
        Vector<float> input_parameters = GrayscaleMatrix(
            ToBasicColorMatrixFilterOperation(filter_operation)->Amount());
        effect = FEColorMatrix::Create(parent_filter, FECOLORMATRIX_TYPE_MATRIX,
                                       input_parameters);
        break;
      }
      case FilterOperation::SEPIA: {
        Vector<float> input_parameters = SepiaMatrix(
            ToBasicColorMatrixFilterOperation(filter_operation)->Amount());
        effect = FEColorMatrix::Create(parent_filter, FECOLORMATRIX_TYPE_MATRIX,
                                       input_parameters);
        break;
      }
      case FilterOperation::SATURATE: {
        Vector<float> input_parameters;
        input_parameters.push_back(clampTo<float>(
            ToBasicColorMatrixFilterOperation(filter_operation)->Amount()));
        effect = FEColorMatrix::Create(
            parent_filter, FECOLORMATRIX_TYPE_SATURATE, input_parameters);
        break;
      }
      case FilterOperation::HUE_ROTATE: {
        Vector<float> input_parameters;
        input_parameters.push_back(clampTo<float>(
            ToBasicColorMatrixFilterOperation(filter_operation)->Amount()));
        effect = FEColorMatrix::Create(
            parent_filter, FECOLORMATRIX_TYPE_HUEROTATE, input_parameters);
        break;
      }
      case FilterOperation::INVERT: {
        BasicComponentTransferFilterOperation* component_transfer_operation =
            ToBasicComponentTransferFilterOperation(filter_operation);
        ComponentTransferFunction transfer_function;
        transfer_function.type = FECOMPONENTTRANSFER_TYPE_TABLE;
        Vector<float> transfer_parameters;
        transfer_parameters.push_back(
            clampTo<float>(component_transfer_operation->Amount()));
        transfer_parameters.push_back(
            clampTo<float>(1 - component_transfer_operation->Amount()));
        transfer_function.table_values = transfer_parameters;

        ComponentTransferFunction null_function;
        effect = FEComponentTransfer::Create(parent_filter, transfer_function,
                                             transfer_function,
                                             transfer_function, null_function);
        break;
      }
      case FilterOperation::OPACITY: {
        ComponentTransferFunction transfer_function;
        transfer_function.type = FECOMPONENTTRANSFER_TYPE_TABLE;
        Vector<float> transfer_parameters;
        transfer_parameters.push_back(0);
        transfer_parameters.push_back(clampTo<float>(
            ToBasicComponentTransferFilterOperation(filter_operation)
                ->Amount()));
        transfer_function.table_values = transfer_parameters;

        ComponentTransferFunction null_function;
        effect = FEComponentTransfer::Create(parent_filter, null_function,
                                             null_function, null_function,
                                             transfer_function);
        break;
      }
      case FilterOperation::BRIGHTNESS: {
        ComponentTransferFunction transfer_function;
        transfer_function.type = FECOMPONENTTRANSFER_TYPE_LINEAR;
        transfer_function.slope = clampTo<float>(
            ToBasicComponentTransferFilterOperation(filter_operation)
                ->Amount());
        transfer_function.intercept = 0;

        ComponentTransferFunction null_function;
        effect = FEComponentTransfer::Create(parent_filter, transfer_function,
                                             transfer_function,
                                             transfer_function, null_function);
        break;
      }
      case FilterOperation::CONTRAST: {
        ComponentTransferFunction transfer_function;
        transfer_function.type = FECOMPONENTTRANSFER_TYPE_LINEAR;
        float amount = clampTo<float>(
            ToBasicComponentTransferFilterOperation(filter_operation)
                ->Amount());
        transfer_function.slope = amount;
        transfer_function.intercept = -0.5 * amount + 0.5;

        ComponentTransferFunction null_function;
        effect = FEComponentTransfer::Create(parent_filter, transfer_function,
                                             transfer_function,
                                             transfer_function, null_function);
        break;
      }
      case FilterOperation::BLUR: {
        float std_deviation = FloatValueForLength(
            ToBlurFilterOperation(filter_operation)->StdDeviation(), 0);
        effect =
            FEGaussianBlur::Create(parent_filter, std_deviation, std_deviation);
        break;
      }
      case FilterOperation::DROP_SHADOW: {
        const ShadowData& shadow =
            ToDropShadowFilterOperation(*filter_operation).Shadow();
        effect = FEDropShadow::Create(parent_filter, shadow.Blur(),
                                      shadow.Blur(), shadow.X(), shadow.Y(),
                                      shadow.GetColor().GetColor(), 1);
        break;
      }
      case FilterOperation::BOX_REFLECT: {
        BoxReflectFilterOperation* box_reflect_operation =
            ToBoxReflectFilterOperation(filter_operation);
        effect = FEBoxReflect::Create(parent_filter,
                                      box_reflect_operation->Reflection());
        break;
      }
      default:
        break;
    }

    if (effect) {
      if (filter_operation->GetType() != FilterOperation::REFERENCE) {
        // Unlike SVG, filters applied here should not clip to their primitive
        // subregions.
        effect->SetClipsToBounds(false);
        effect->SetOperatingInterpolationSpace(kInterpolationSpaceSRGB);
        effect->InputEffects().push_back(previous_effect);
      }
      if (previous_effect->OriginTainted())
        effect->SetOriginTainted();
      previous_effect = effect;
    }
  }
  return previous_effect;
}

CompositorFilterOperations FilterEffectBuilder::BuildFilterOperations(
    const FilterOperations& operations) const {
  InterpolationSpace current_interpolation_space = kInterpolationSpaceSRGB;

  CompositorFilterOperations filters;
  for (FilterOperation* op : operations.Operations()) {
    switch (op->GetType()) {
      case FilterOperation::REFERENCE: {
        ReferenceFilterOperation& reference_operation =
            ToReferenceFilterOperation(*op);
        Filter* reference_filter =
            BuildReferenceFilter(reference_operation, nullptr);
        if (reference_filter && reference_filter->LastEffect()) {
          PaintFilterBuilder::PopulateSourceGraphicImageFilters(
              reference_filter->GetSourceGraphic(), nullptr,
              current_interpolation_space);

          FilterEffect* filter_effect = reference_filter->LastEffect();
          current_interpolation_space =
              filter_effect->OperatingInterpolationSpace();
          auto paint_filter = PaintFilterBuilder::Build(
              filter_effect, current_interpolation_space);
          if (!paint_filter)
            continue;
          filters.AppendReferenceFilter(std::move(paint_filter));
        }
        reference_operation.SetFilter(reference_filter);
        break;
      }
      case FilterOperation::GRAYSCALE:
      case FilterOperation::SEPIA:
      case FilterOperation::SATURATE:
      case FilterOperation::HUE_ROTATE: {
        float amount = ToBasicColorMatrixFilterOperation(*op).Amount();
        switch (op->GetType()) {
          case FilterOperation::GRAYSCALE:
            filters.AppendGrayscaleFilter(amount);
            break;
          case FilterOperation::SEPIA:
            filters.AppendSepiaFilter(amount);
            break;
          case FilterOperation::SATURATE:
            filters.AppendSaturateFilter(amount);
            break;
          case FilterOperation::HUE_ROTATE:
            filters.AppendHueRotateFilter(amount);
            break;
          default:
            NOTREACHED();
        }
        break;
      }
      case FilterOperation::INVERT:
      case FilterOperation::OPACITY:
      case FilterOperation::BRIGHTNESS:
      case FilterOperation::CONTRAST: {
        float amount = ToBasicComponentTransferFilterOperation(*op).Amount();
        switch (op->GetType()) {
          case FilterOperation::INVERT:
            filters.AppendInvertFilter(amount);
            break;
          case FilterOperation::OPACITY:
            filters.AppendOpacityFilter(amount);
            break;
          case FilterOperation::BRIGHTNESS:
            filters.AppendBrightnessFilter(amount);
            break;
          case FilterOperation::CONTRAST:
            filters.AppendContrastFilter(amount);
            break;
          default:
            NOTREACHED();
        }
        break;
      }
      case FilterOperation::BLUR: {
        float pixel_radius =
            ToBlurFilterOperation(*op).StdDeviation().GetFloatValue();
        filters.AppendBlurFilter(pixel_radius);
        break;
      }
      case FilterOperation::DROP_SHADOW: {
        const ShadowData& shadow = ToDropShadowFilterOperation(*op).Shadow();
        filters.AppendDropShadowFilter(WebPoint(shadow.X(), shadow.Y()),
                                       shadow.Blur(),
                                       shadow.GetColor().GetColor());
        break;
      }
      case FilterOperation::BOX_REFLECT: {
        // TODO(jbroman): Consider explaining box reflect to the compositor,
        // instead of calling this a "reference filter".
        const auto& reflection = ToBoxReflectFilterOperation(*op).Reflection();
        filters.AppendReferenceFilter(
            PaintFilterBuilder::BuildBoxReflectFilter(reflection, nullptr));
        break;
      }
      case FilterOperation::NONE:
        break;
    }
  }
  if (current_interpolation_space != kInterpolationSpaceSRGB) {
    // Transform to device color space at the end of processing, if required.
    sk_sp<PaintFilter> filter = PaintFilterBuilder::TransformInterpolationSpace(
        nullptr, current_interpolation_space, kInterpolationSpaceSRGB);
    filters.AppendReferenceFilter(std::move(filter));
  }

  if (!filters.IsEmpty())
    filters.SetReferenceBox(reference_box_);

  return filters;
}

Filter* FilterEffectBuilder::BuildReferenceFilter(
    const ReferenceFilterOperation& reference_operation,
    FilterEffect* previous_effect) const {
  SVGResource* resource = reference_operation.Resource();
  if (auto* filter =
          ToSVGFilterElementOrNull(resource ? resource->Target() : nullptr))
    return BuildReferenceFilter(*filter, previous_effect);
  return nullptr;
}

Filter* FilterEffectBuilder::BuildReferenceFilter(
    SVGFilterElement& filter_element,
    FilterEffect* previous_effect,
    SVGFilterGraphNodeMap* node_map) const {
  FloatRect filter_region =
      SVGLengthContext::ResolveRectangle<SVGFilterElement>(
          &filter_element,
          filter_element.filterUnits()->CurrentValue()->EnumValue(),
          reference_box_);
  // TODO(fs): We rely on the presence of a node map here to opt-in to the
  // check for an empty filter region. The reason for this is that we lack a
  // viewport to resolve against for HTML content. This is crbug.com/512453.
  if (node_map && filter_region.IsEmpty())
    return nullptr;

  bool primitive_bounding_box_mode =
      filter_element.primitiveUnits()->CurrentValue()->EnumValue() ==
      SVGUnitTypes::kSvgUnitTypeObjectboundingbox;
  Filter::UnitScaling unit_scaling =
      primitive_bounding_box_mode ? Filter::kBoundingBox : Filter::kUserSpace;
  Filter* result =
      Filter::Create(reference_box_, filter_region, zoom_, unit_scaling);
  if (!previous_effect)
    previous_effect = result->GetSourceGraphic();
  SVGFilterBuilder builder(previous_effect, node_map, fill_flags_,
                           stroke_flags_);
  builder.BuildGraph(result, filter_element, reference_box_);
  result->SetLastEffect(builder.LastEffect());
  return result;
}

}  // namespace blink
