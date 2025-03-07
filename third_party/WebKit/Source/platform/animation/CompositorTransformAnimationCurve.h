// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CompositorTransformAnimationCurve_h
#define CompositorTransformAnimationCurve_h

#include <memory>

#include "base/memory/ptr_util.h"
#include "platform/PlatformExport.h"
#include "platform/animation/CompositorAnimationCurve.h"
#include "platform/animation/CompositorTransformKeyframe.h"
#include "platform/animation/TimingFunction.h"
#include "platform/wtf/Noncopyable.h"

namespace cc {
class KeyframedTransformAnimationCurve;
}

namespace blink {
class CompositorTransformKeyframe;
}

namespace blink {

// A keyframed transform animation curve.
class PLATFORM_EXPORT CompositorTransformAnimationCurve
    : public CompositorAnimationCurve {
  WTF_MAKE_NONCOPYABLE(CompositorTransformAnimationCurve);

 public:
  static std::unique_ptr<CompositorTransformAnimationCurve> Create() {
    return base::WrapUnique(new CompositorTransformAnimationCurve());
  }

  ~CompositorTransformAnimationCurve() override;

  void AddKeyframe(const CompositorTransformKeyframe&);
  void SetTimingFunction(const TimingFunction&);
  void SetScaledDuration(double);

  // CompositorAnimationCurve implementation.
  std::unique_ptr<cc::AnimationCurve> CloneToAnimationCurve() const override;

 private:
  CompositorTransformAnimationCurve();

  std::unique_ptr<cc::KeyframedTransformAnimationCurve> curve_;
};

}  // namespace blink

#endif  // CompositorTransformAnimationCurve_h
