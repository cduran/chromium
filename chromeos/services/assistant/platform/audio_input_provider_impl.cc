// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromeos/services/assistant/platform/audio_input_provider_impl.h"

#include "base/logging.h"
#include "base/stl_util.h"
#include "libassistant/shared/public/platform_audio_buffer.h"

namespace chromeos {
namespace assistant {

namespace {

// This format should match //c/b/c/assistant/platform_audio_input_host.cc.
constexpr assistant_client::BufferFormat kFormat{
    16000 /* sample_rate */, assistant_client::INTERLEAVED_S32, 2 /* channels */
};

}  // namespace

AudioInputConfigImpl::AudioInputConfigImpl() = default;

AudioInputConfigImpl::~AudioInputConfigImpl() = default;

std::vector<int> AudioInputConfigImpl::GetSelectedChannels() const {
  return std::vector<int>{};
}

float AudioInputConfigImpl::GetMicSensitivity() const {
  return 0.f;
}

assistant_client::AudioInputConfig::InputType
AudioInputConfigImpl::GetInputType() const {
  return assistant_client::AudioInputConfig::MICROPHONE_NEAR_FIELD;
}

std::string AudioInputConfigImpl::GetMicManufacturer() const {
  return "default";
}

std::string AudioInputConfigImpl::GetMicModel() const {
  return "default";
}

std::string AudioInputConfigImpl::GetMicVersion() const {
  return "0";
}

assistant_client::AudioInputConfig::MicState AudioInputConfigImpl::GetMicState()
    const {
  return assistant_client::AudioInputConfig::ENABLED;
}

class AudioInputBufferImpl : public assistant_client::AudioBuffer {
 public:
  AudioInputBufferImpl(const void* data, uint32_t frame_count)
      : data_(data), frame_count_(frame_count) {}
  ~AudioInputBufferImpl() override = default;

  // assistant_client::AudioBuffer overrides:
  assistant_client::BufferFormat GetFormat() const override { return kFormat; }
  const void* GetData() const override { return data_; }
  void* GetWritableData() override {
    NOTREACHED();
    return nullptr;
  }
  int GetFrameCount() const override { return frame_count_; }

 private:
  const void* data_;
  int frame_count_;
  DISALLOW_COPY_AND_ASSIGN(AudioInputBufferImpl);
};

AudioInputImpl::AudioInputImpl(mojom::AudioInputPtr audio_input)
    : binding_(this) {
  mojom::AudioInputObserverPtr observer;
  binding_.Bind(mojo::MakeRequest(&observer));
  audio_input->AddAudioInputObserver(std::move(observer));
}

AudioInputImpl::~AudioInputImpl() = default;

void AudioInputImpl::OnAudioInputFramesAvailable(
    const std::vector<int32_t>& buffer,
    uint32_t frame_count,
    base::TimeTicks timestamp) {
  AudioInputBufferImpl input_buffer(buffer.data(), frame_count);
  int64_t time = timestamp.since_origin().InMilliseconds();
  {
    base::AutoLock auto_lock(lock_);
    for (auto* observer : observers_)
      observer->OnBufferAvailable(input_buffer, time);
  }
}

void AudioInputImpl::OnAudioInputClosed() {
  base::AutoLock auto_lock(lock_);
  for (auto* observer : observers_)
    observer->OnStopped();
}

assistant_client::BufferFormat AudioInputImpl::GetFormat() const {
  return kFormat;
}

void AudioInputImpl::AddObserver(
    assistant_client::AudioInput::Observer* observer) {
  base::AutoLock auto_lock(lock_);
  observers_.push_back(observer);
}

void AudioInputImpl::RemoveObserver(
    assistant_client::AudioInput::Observer* observer) {
  base::AutoLock auto_lock(lock_);
  base::Erase(observers_, observer);
}

AudioInputProviderImpl::AudioInputProviderImpl(mojom::AudioInputPtr audio_input)
    : audio_input_(std::move(audio_input)) {}

AudioInputProviderImpl::~AudioInputProviderImpl() = default;

assistant_client::AudioInputConfig&
AudioInputProviderImpl::GetAudioInputConfig() {
  return audio_input_config_;
}

assistant_client::AudioInput& AudioInputProviderImpl::GetAudioInput() {
  return audio_input_;
}

}  // namespace assistant
}  // namespace chromeos
