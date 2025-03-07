// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/audio/StereoPanner.h"

#include <algorithm>
#include <memory>

#include "base/memory/ptr_util.h"
#include "platform/audio/AudioBus.h"
#include "platform/audio/AudioUtilities.h"
#include "platform/wtf/MathExtras.h"

namespace blink {

// Implement equal-power panning algorithm for mono or stereo input.
// See: http://webaudio.github.io/web-audio-api/#panning-algorithm

std::unique_ptr<StereoPanner> StereoPanner::Create(float sample_rate) {
  return base::WrapUnique(new StereoPanner(sample_rate));
}

StereoPanner::StereoPanner(float sample_rate) {}

void StereoPanner::PanWithSampleAccurateValues(const AudioBus* input_bus,
                                               AudioBus* output_bus,
                                               const float* pan_values,
                                               size_t frames_to_process) {
  bool is_input_safe = input_bus &&
                       (input_bus->NumberOfChannels() == 1 ||
                        input_bus->NumberOfChannels() == 2) &&
                       frames_to_process <= input_bus->length();
  DCHECK(is_input_safe);
  if (!is_input_safe)
    return;

  unsigned number_of_input_channels = input_bus->NumberOfChannels();

  bool is_output_safe = output_bus && output_bus->NumberOfChannels() == 2 &&
                        frames_to_process <= output_bus->length();
  DCHECK(is_output_safe);
  if (!is_output_safe)
    return;

  const float* source_l = input_bus->Channel(0)->Data();
  const float* source_r =
      number_of_input_channels > 1 ? input_bus->Channel(1)->Data() : source_l;
  float* destination_l =
      output_bus->ChannelByType(AudioBus::kChannelLeft)->MutableData();
  float* destination_r =
      output_bus->ChannelByType(AudioBus::kChannelRight)->MutableData();

  if (!source_l || !source_r || !destination_l || !destination_r)
    return;

  double gain_l, gain_r, pan_radian;

  int n = frames_to_process;

  if (number_of_input_channels == 1) {  // For mono source case.
    while (n--) {
      float input_l = *source_l++;
      double pan = clampTo(*pan_values++, -1.0, 1.0);
      // Pan from left to right [-1; 1] will be normalized as [0; 1].
      pan_radian = (pan * 0.5 + 0.5) * piOverTwoDouble;
      gain_l = std::cos(pan_radian);
      gain_r = std::sin(pan_radian);
      *destination_l++ = static_cast<float>(input_l * gain_l);
      *destination_r++ = static_cast<float>(input_l * gain_r);
    }
  } else {  // For stereo source case.
    while (n--) {
      float input_l = *source_l++;
      float input_r = *source_r++;
      double pan = clampTo(*pan_values++, -1.0, 1.0);
      // Normalize [-1; 0] to [0; 1]. Do nothing when [0; 1].
      pan_radian = (pan <= 0 ? pan + 1 : pan) * piOverTwoDouble;
      gain_l = std::cos(pan_radian);
      gain_r = std::sin(pan_radian);
      if (pan <= 0) {
        *destination_l++ = static_cast<float>(input_l + input_r * gain_l);
        *destination_r++ = static_cast<float>(input_r * gain_r);
      } else {
        *destination_l++ = static_cast<float>(input_l * gain_l);
        *destination_r++ = static_cast<float>(input_r + input_l * gain_r);
      }
    }
  }
}

void StereoPanner::PanToTargetValue(const AudioBus* input_bus,
                                    AudioBus* output_bus,
                                    float pan_value,
                                    size_t frames_to_process) {
  bool is_input_safe = input_bus &&
                       (input_bus->NumberOfChannels() == 1 ||
                        input_bus->NumberOfChannels() == 2) &&
                       frames_to_process <= input_bus->length();
  DCHECK(is_input_safe);
  if (!is_input_safe)
    return;

  unsigned number_of_input_channels = input_bus->NumberOfChannels();

  bool is_output_safe = output_bus && output_bus->NumberOfChannels() == 2 &&
                        frames_to_process <= output_bus->length();
  DCHECK(is_output_safe);
  if (!is_output_safe)
    return;

  const float* source_l = input_bus->Channel(0)->Data();
  const float* source_r =
      number_of_input_channels > 1 ? input_bus->Channel(1)->Data() : source_l;
  float* destination_l =
      output_bus->ChannelByType(AudioBus::kChannelLeft)->MutableData();
  float* destination_r =
      output_bus->ChannelByType(AudioBus::kChannelRight)->MutableData();

  if (!source_l || !source_r || !destination_l || !destination_r)
    return;

  float target_pan = clampTo(pan_value, -1.0, 1.0);

  int n = frames_to_process;

  if (number_of_input_channels == 1) {  // For mono source case.
    // Pan from left to right [-1; 1] will be normalized as [0; 1].
    double pan_radian = (target_pan * 0.5 + 0.5) * piOverTwoDouble;

    double gain_l = std::cos(pan_radian);
    double gain_r = std::sin(pan_radian);

    // TODO(rtoy): This can be vectorized using VectorMath::Vsmul
    while (n--) {
      float input_l = *source_l++;
      *destination_l++ = static_cast<float>(input_l * gain_l);
      *destination_r++ = static_cast<float>(input_l * gain_r);
    }
  } else {  // For stereo source case.
    // Normalize [-1; 0] to [0; 1] for the left pan position (<= 0), and
    // do nothing when [0; 1].
    double pan_radian =
        (target_pan <= 0 ? target_pan + 1 : target_pan) * piOverTwoDouble;

    double gain_l = std::cos(pan_radian);
    double gain_r = std::sin(pan_radian);

    // TODO(rtoy): Consider moving the if statement outside the loop
    // since |target_pan| is constant inside the loop.
    while (n--) {
      float input_l = *source_l++;
      float input_r = *source_r++;
      if (target_pan <= 0) {
        // When [-1; 0], keep left channel intact and equal-power pan the
        // right channel only.
        *destination_l++ = static_cast<float>(input_l + input_r * gain_l);
        *destination_r++ = static_cast<float>(input_r * gain_r);
      } else {
        // When [0; 1], keep right channel intact and equal-power pan the
        // left channel only.
        *destination_l++ = static_cast<float>(input_l * gain_l);
        *destination_r++ = static_cast<float>(input_r + input_l * gain_r);
      }
    }
  }
}

}  // namespace blink
