/*
 * Copyright (C) 2013 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef EffectModel_h
#define EffectModel_h

#include "core/CoreExport.h"
#include "core/animation/PropertyHandle.h"
#include "core/css_property_names.h"
#include "platform/heap/Handle.h"
#include "platform/wtf/HashMap.h"

namespace blink {

class Interpolation;

// Time independent representation of an Animation's content.
// Can be sampled for the active pairs of Keyframes (represented by
// Interpolations) at a given time fraction.
class CORE_EXPORT EffectModel : public GarbageCollectedFinalized<EffectModel> {
 public:
  enum CompositeOperation {
    kCompositeReplace,
    kCompositeAdd,
  };
  static CompositeOperation StringToCompositeOperation(const String&);
  static String CompositeOperationToString(CompositeOperation);

  EffectModel() = default;
  virtual ~EffectModel() = default;
  virtual bool Sample(int iteration,
                      double fraction,
                      double iteration_duration,
                      Vector<scoped_refptr<Interpolation>>&) const = 0;

  virtual bool Affects(const PropertyHandle&) const { return false; }
  virtual bool AffectedByUnderlyingAnimations() const = 0;
  virtual bool IsTransformRelatedEffect() const { return false; }
  virtual bool IsKeyframeEffectModel() const { return false; }

  virtual void Trace(blink::Visitor* visitor) {}
};

}  // namespace blink

#endif  // EffectModel_h
