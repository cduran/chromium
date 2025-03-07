// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/assistant/platform_audio_input_host.h"

#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/logging.h"
#include "media/audio/audio_input_controller.h"
#include "media/audio/audio_manager.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_parameters.h"
#include "media/base/audio_sample_types.h"
#include "media/base/channel_layout.h"

namespace chromeos {
namespace assistant {

class PlatformAudioInputHost::Writer
    : public media::AudioInputController::SyncWriter {
 public:
  explicit Writer(base::WeakPtr<PlatformAudioInputHost> host)
      : host_(std::move(host)) {}
  ~Writer() override = default;

  // media::AudioInputController::SyncWriter overrides:
  void Write(const media::AudioBus* data,
             double volume,
             bool key_pressed,
             base::TimeTicks capture_time) override {
    if (!host_)
      return;
    // 2 channels * # of frames.
    std::vector<int32_t> buffer(2 * data->frames());
    data->ToInterleaved<media::SignedInt32SampleTypeTraits>(data->frames(),
                                                            buffer.data());
    host_->NotifyDataAvailable(std::move(buffer), data->frames(), capture_time);
  }

  void Close() override {
    if (host_)
      host_->NotifyAudioClosed();
  }

 private:
  base::WeakPtr<PlatformAudioInputHost> host_;

  DISALLOW_COPY_AND_ASSIGN(Writer);
};

class PlatformAudioInputHost::EventHandler
    : public media::AudioInputController::EventHandler {
 public:
  EventHandler() = default;
  ~EventHandler() override = default;

  // media::AudioInputController::EventHandler overrides:
  void OnCreated(bool initially_muted) override {}
  void OnError(media::AudioInputController::ErrorCode error_code) override {}
  void OnLog(base::StringPiece log) override {}
  void OnMuted(bool is_muted) override {}

 private:
  DISALLOW_COPY_AND_ASSIGN(EventHandler);
};

PlatformAudioInputHost::PlatformAudioInputHost() : weak_factory_(this) {
  sync_writer_ = std::make_unique<Writer>(weak_factory_.GetWeakPtr());
  event_handler_ = std::make_unique<EventHandler>();
  audio_input_controller_ = media::AudioInputController::Create(
      media::AudioManager::Get(), event_handler_.get(), sync_writer_.get(),
      nullptr,
      media::AudioParameters(media::AudioParameters::AUDIO_PCM_LINEAR,
                             media::CHANNEL_LAYOUT_STEREO,
                             16000 /* 16000 frames per second */,
                             32 /* 32 bits per frame */,
                             1600 /* 1600 (16000 / 10) frames per buffer */),
      "default" /* device_id */, false /* agc_is_enabled */);
}

PlatformAudioInputHost::~PlatformAudioInputHost() {
  // Bind |sync_writer_| and |event_handler_| to the callback closure to ensure
  // they live longer than the |Close| call, which is async.
  audio_input_controller_->Close(
      base::BindOnce([](std::unique_ptr<EventHandler> event_handler,
                        std::unique_ptr<Writer> writer) {},
                     std::move(event_handler_), std::move(sync_writer_)));
}

void PlatformAudioInputHost::NotifyDataAvailable(
    const std::vector<int32_t>& data,
    int32_t frames,
    base::TimeTicks capture_time) {
  observers_.ForAllPtrs([&data, frames, capture_time](auto* observer) {
    observer->OnAudioInputFramesAvailable(data, frames, capture_time);
  });

  if (observers_.empty() && recording_) {
    recording_ = false;
    audio_input_controller_->Close(base::DoNothing());
  }
}

void PlatformAudioInputHost::NotifyAudioClosed() {
  recording_ = false;
  observers_.ForAllPtrs([](auto* observer) { observer->OnAudioInputClosed(); });
  observers_.CloseAll();
}

void PlatformAudioInputHost::AddObserver(
    mojom::AudioInputObserverPtr observer) {
  observers_.AddPtr(std::move(observer));
  if (!recording_) {
    audio_input_controller_->Record();
    recording_ = true;
  }
}

}  // namespace assistant
}  // namespace chromeos
