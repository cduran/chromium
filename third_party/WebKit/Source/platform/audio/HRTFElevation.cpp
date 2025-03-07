/*
 * Copyright (C) 2010 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Computer, Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "platform/audio/HRTFElevation.h"

#include <math.h>
#include <algorithm>
#include <memory>
#include <utility>

#include "base/memory/ptr_util.h"
#include "platform/audio/AudioBus.h"
#include "platform/audio/HRTFPanner.h"
#include "platform/wtf/ThreadingPrimitives.h"
#include "platform/wtf/text/StringHash.h"

namespace blink {

const unsigned HRTFElevation::kAzimuthSpacing = 15;
const unsigned HRTFElevation::kNumberOfRawAzimuths = 360 / kAzimuthSpacing;
const unsigned HRTFElevation::kInterpolationFactor = 8;
const unsigned HRTFElevation::kNumberOfTotalAzimuths =
    kNumberOfRawAzimuths * kInterpolationFactor;

// Total number of components of an HRTF database.
const size_t kTotalNumberOfResponses = 240;

// Number of frames in an individual impulse response.
const size_t kResponseFrameSize = 256;

// Sample-rate of the spatialization impulse responses as stored in the resource
// file.  The impulse responses may be resampled to a different sample-rate
// (depending on the audio hardware) when they are loaded.
const float kResponseSampleRate = 44100;

// This table maps the index into the elevation table with the corresponding
// angle. See https://bugs.webkit.org/show_bug.cgi?id=98294#c9 for the
// elevation angles and their order in the concatenated response.
const int kElevationIndexTableSize = 10;
const int kElevationIndexTable[kElevationIndexTableSize] = {
    0, 15, 30, 45, 60, 75, 90, 315, 330, 345};

// Lazily load a concatenated HRTF database for given subject and store it in a
// local hash table to ensure quick efficient future retrievals.
static scoped_refptr<AudioBus> GetConcatenatedImpulseResponsesForSubject(
    const String& subject_name) {
  typedef HashMap<String, scoped_refptr<AudioBus>> AudioBusMap;
  DEFINE_THREAD_SAFE_STATIC_LOCAL(AudioBusMap, audio_bus_map, ());
  DEFINE_THREAD_SAFE_STATIC_LOCAL(Mutex, mutex, ());

  MutexLocker locker(mutex);
  scoped_refptr<AudioBus> bus;
  AudioBusMap::iterator iterator = audio_bus_map.find(subject_name);
  if (iterator == audio_bus_map.end()) {
    scoped_refptr<AudioBus> concatenated_impulse_responses(
        AudioBus::GetDataResource(subject_name.Utf8().data(),
                                  kResponseSampleRate));
    DCHECK(concatenated_impulse_responses);
    if (!concatenated_impulse_responses)
      return nullptr;

    bus = concatenated_impulse_responses;
    audio_bus_map.Set(subject_name, bus);
  } else
    bus = iterator->value;

  size_t response_length = bus->length();
  size_t expected_length =
      static_cast<size_t>(kTotalNumberOfResponses * kResponseFrameSize);

  // Check number of channels and length. For now these are fixed and known.
  bool is_bus_good =
      response_length == expected_length && bus->NumberOfChannels() == 2;
  DCHECK(is_bus_good);
  if (!is_bus_good)
    return nullptr;

  return bus;
}

bool HRTFElevation::CalculateKernelsForAzimuthElevation(
    int azimuth,
    int elevation,
    float sample_rate,
    const String& subject_name,
    std::unique_ptr<HRTFKernel>& kernel_l,
    std::unique_ptr<HRTFKernel>& kernel_r) {
  // Valid values for azimuth are 0 -> 345 in 15 degree increments.
  // Valid values for elevation are -45 -> +90 in 15 degree increments.

  bool is_azimuth_good =
      azimuth >= 0 && azimuth <= 345 && (azimuth / 15) * 15 == azimuth;
  DCHECK(is_azimuth_good);
  if (!is_azimuth_good)
    return false;

  bool is_elevation_good =
      elevation >= -45 && elevation <= 90 && (elevation / 15) * 15 == elevation;
  DCHECK(is_elevation_good);
  if (!is_elevation_good)
    return false;

  // Construct the resource name from the subject name, azimuth, and elevation,
  // for example:
  // "IRC_Composite_C_R0195_T015_P000"
  // Note: the passed in subjectName is not a string passed in via JavaScript or
  // the web.  It's passed in as an internal ASCII identifier and is an
  // implementation detail.
  int positive_elevation = elevation < 0 ? elevation + 360 : elevation;

  scoped_refptr<AudioBus> bus(
      GetConcatenatedImpulseResponsesForSubject(subject_name));

  if (!bus)
    return false;

  // Just sequentially search the table to find the correct index.
  int elevation_index = -1;

  for (int k = 0; k < kElevationIndexTableSize; ++k) {
    if (kElevationIndexTable[k] == positive_elevation) {
      elevation_index = k;
      break;
    }
  }

  bool is_elevation_index_good =
      (elevation_index >= 0) && (elevation_index < kElevationIndexTableSize);
  DCHECK(is_elevation_index_good);
  if (!is_elevation_index_good)
    return false;

  // The concatenated impulse response is a bus containing all
  // the elevations per azimuth, for all azimuths by increasing
  // order. So for a given azimuth and elevation we need to compute
  // the index of the wanted audio frames in the concatenated table.
  unsigned index =
      ((azimuth / kAzimuthSpacing) * HRTFDatabase::kNumberOfRawElevations) +
      elevation_index;
  bool is_index_good = index < kTotalNumberOfResponses;
  DCHECK(is_index_good);
  if (!is_index_good)
    return false;

  // Extract the individual impulse response from the concatenated
  // responses and potentially sample-rate convert it to the desired
  // (hardware) sample-rate.
  unsigned start_frame = index * kResponseFrameSize;
  unsigned stop_frame = start_frame + kResponseFrameSize;
  scoped_refptr<AudioBus> pre_sample_rate_converted_response(
      AudioBus::CreateBufferFromRange(bus.get(), start_frame, stop_frame));
  scoped_refptr<AudioBus> response(AudioBus::CreateBySampleRateConverting(
      pre_sample_rate_converted_response.get(), false, sample_rate));
  AudioChannel* left_ear_impulse_response =
      response->Channel(AudioBus::kChannelLeft);
  AudioChannel* right_ear_impulse_response =
      response->Channel(AudioBus::kChannelRight);

  // Note that depending on the fftSize returned by the panner, we may be
  // truncating the impulse response we just loaded in.
  const size_t fft_size = HRTFPanner::FftSizeForSampleRate(sample_rate);
  kernel_l =
      HRTFKernel::Create(left_ear_impulse_response, fft_size, sample_rate);
  kernel_r =
      HRTFKernel::Create(right_ear_impulse_response, fft_size, sample_rate);

  return true;
}

// The range of elevations for the IRCAM impulse responses varies depending on
// azimuth, but the minimum elevation appears to always be -45.
//
// Here's how it goes:
static int g_max_elevations[] = {
    //  Azimuth
    //
    90,  // 0
    45,  // 15
    60,  // 30
    45,  // 45
    75,  // 60
    45,  // 75
    60,  // 90
    45,  // 105
    75,  // 120
    45,  // 135
    60,  // 150
    45,  // 165
    75,  // 180
    45,  // 195
    60,  // 210
    45,  // 225
    75,  // 240
    45,  // 255
    60,  // 270
    45,  // 285
    75,  // 300
    45,  // 315
    60,  // 330
    45   //  345
};

std::unique_ptr<HRTFElevation> HRTFElevation::CreateForSubject(
    const String& subject_name,
    int elevation,
    float sample_rate) {
  bool is_elevation_good =
      elevation >= -45 && elevation <= 90 && (elevation / 15) * 15 == elevation;
  DCHECK(is_elevation_good);
  if (!is_elevation_good)
    return nullptr;

  std::unique_ptr<HRTFKernelList> kernel_list_l =
      std::make_unique<HRTFKernelList>(kNumberOfTotalAzimuths);
  std::unique_ptr<HRTFKernelList> kernel_list_r =
      std::make_unique<HRTFKernelList>(kNumberOfTotalAzimuths);

  // Load convolution kernels from HRTF files.
  int interpolated_index = 0;
  for (unsigned raw_index = 0; raw_index < kNumberOfRawAzimuths; ++raw_index) {
    // Don't let elevation exceed maximum for this azimuth.
    int max_elevation = g_max_elevations[raw_index];
    int actual_elevation = std::min(elevation, max_elevation);

    bool success = CalculateKernelsForAzimuthElevation(
        raw_index * kAzimuthSpacing, actual_elevation, sample_rate,
        subject_name, kernel_list_l->at(interpolated_index),
        kernel_list_r->at(interpolated_index));
    if (!success)
      return nullptr;

    interpolated_index += kInterpolationFactor;
  }

  // Now go back and interpolate intermediate azimuth values.
  for (unsigned i = 0; i < kNumberOfTotalAzimuths; i += kInterpolationFactor) {
    int j = (i + kInterpolationFactor) % kNumberOfTotalAzimuths;

    // Create the interpolated convolution kernels and delays.
    for (unsigned jj = 1; jj < kInterpolationFactor; ++jj) {
      float x =
          float(jj) / float(kInterpolationFactor);  // interpolate from 0 -> 1

      (*kernel_list_l)[i + jj] = HRTFKernel::CreateInterpolatedKernel(
          kernel_list_l->at(i).get(), kernel_list_l->at(j).get(), x);
      (*kernel_list_r)[i + jj] = HRTFKernel::CreateInterpolatedKernel(
          kernel_list_r->at(i).get(), kernel_list_r->at(j).get(), x);
    }
  }

  std::unique_ptr<HRTFElevation> hrtf_elevation = base::WrapUnique(
      new HRTFElevation(std::move(kernel_list_l), std::move(kernel_list_r),
                        elevation, sample_rate));
  return hrtf_elevation;
}

std::unique_ptr<HRTFElevation> HRTFElevation::CreateByInterpolatingSlices(
    HRTFElevation* hrtf_elevation1,
    HRTFElevation* hrtf_elevation2,
    float x,
    float sample_rate) {
  DCHECK(hrtf_elevation1);
  DCHECK(hrtf_elevation2);
  if (!hrtf_elevation1 || !hrtf_elevation2)
    return nullptr;

  DCHECK_GE(x, 0.0);
  DCHECK_LT(x, 1.0);

  std::unique_ptr<HRTFKernelList> kernel_list_l =
      std::make_unique<HRTFKernelList>(kNumberOfTotalAzimuths);
  std::unique_ptr<HRTFKernelList> kernel_list_r =
      std::make_unique<HRTFKernelList>(kNumberOfTotalAzimuths);

  HRTFKernelList* kernel_list_l1 = hrtf_elevation1->KernelListL();
  HRTFKernelList* kernel_list_r1 = hrtf_elevation1->KernelListR();
  HRTFKernelList* kernel_list_l2 = hrtf_elevation2->KernelListL();
  HRTFKernelList* kernel_list_r2 = hrtf_elevation2->KernelListR();

  // Interpolate kernels of corresponding azimuths of the two elevations.
  for (unsigned i = 0; i < kNumberOfTotalAzimuths; ++i) {
    (*kernel_list_l)[i] = HRTFKernel::CreateInterpolatedKernel(
        kernel_list_l1->at(i).get(), kernel_list_l2->at(i).get(), x);
    (*kernel_list_r)[i] = HRTFKernel::CreateInterpolatedKernel(
        kernel_list_r1->at(i).get(), kernel_list_r2->at(i).get(), x);
  }

  // Interpolate elevation angle.
  double angle = (1.0 - x) * hrtf_elevation1->ElevationAngle() +
                 x * hrtf_elevation2->ElevationAngle();

  std::unique_ptr<HRTFElevation> hrtf_elevation = base::WrapUnique(
      new HRTFElevation(std::move(kernel_list_l), std::move(kernel_list_r),
                        static_cast<int>(angle), sample_rate));
  return hrtf_elevation;
}

void HRTFElevation::GetKernelsFromAzimuth(double azimuth_blend,
                                          unsigned azimuth_index,
                                          HRTFKernel*& kernel_l,
                                          HRTFKernel*& kernel_r,
                                          double& frame_delay_l,
                                          double& frame_delay_r) {
  bool check_azimuth_blend = azimuth_blend >= 0.0 && azimuth_blend < 1.0;
  DCHECK(check_azimuth_blend);
  if (!check_azimuth_blend)
    azimuth_blend = 0.0;

  unsigned num_kernels = kernel_list_l_->size();

  bool is_index_good = azimuth_index < num_kernels;
  DCHECK(is_index_good);
  if (!is_index_good) {
    kernel_l = nullptr;
    kernel_r = nullptr;
    return;
  }

  // Return the left and right kernels.
  kernel_l = kernel_list_l_->at(azimuth_index).get();
  kernel_r = kernel_list_r_->at(azimuth_index).get();

  frame_delay_l = kernel_list_l_->at(azimuth_index)->FrameDelay();
  frame_delay_r = kernel_list_r_->at(azimuth_index)->FrameDelay();

  int azimuth_index2 = (azimuth_index + 1) % num_kernels;
  double frame_delay2l = kernel_list_l_->at(azimuth_index2)->FrameDelay();
  double frame_delay2r = kernel_list_r_->at(azimuth_index2)->FrameDelay();

  // Linearly interpolate delays.
  frame_delay_l =
      (1.0 - azimuth_blend) * frame_delay_l + azimuth_blend * frame_delay2l;
  frame_delay_r =
      (1.0 - azimuth_blend) * frame_delay_r + azimuth_blend * frame_delay2r;
}

}  // namespace blink
