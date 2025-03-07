// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chromecast/media/cma/backend/video_decoder_null.h"

#include <memory>

#include "base/bind.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/threading/thread_task_runner_handle.h"
#include "chromecast/public/media/cast_decoder_buffer.h"

namespace chromecast {
namespace media {

std::unique_ptr<VideoDecoderForMixer> VideoDecoderForMixer::Create(
    const MediaPipelineDeviceParams& params) {
  return std::make_unique<VideoDecoderNull>();
}

VideoDecoderNull::VideoDecoderNull()
    : delegate_(nullptr), weak_factory_(this) {}

VideoDecoderNull::~VideoDecoderNull() {}

void VideoDecoderNull::SetDelegate(Delegate* delegate) {
  DCHECK(delegate);
  delegate_ = delegate;
}

MediaPipelineBackend::BufferStatus VideoDecoderNull::PushBuffer(
    CastDecoderBuffer* buffer) {
  DCHECK(delegate_);
  DCHECK(buffer);
  if (buffer->end_of_stream()) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE, base::BindOnce(&VideoDecoderNull::OnEndOfStream,
                                  weak_factory_.GetWeakPtr()));
  }
  return MediaPipelineBackend::kBufferSuccess;
}

void VideoDecoderNull::GetStatistics(Statistics* statistics) {}

bool VideoDecoderNull::SetConfig(const VideoConfig& config) {
  return true;
}

void VideoDecoderNull::OnEndOfStream() {
  delegate_->OnEndOfStream();
}

void VideoDecoderNull::Initialize() {}

bool VideoDecoderNull::Start(int64_t start_pts, bool need_avsync) {
  return true;
}

void VideoDecoderNull::Stop() {}

bool VideoDecoderNull::Pause() {
  return true;
}

bool VideoDecoderNull::Resume() {
  return true;
}

int64_t VideoDecoderNull::GetCurrentPts() const {
  return 0;
}

bool VideoDecoderNull::SetPlaybackRate(float rate) {
  return true;
}

bool VideoDecoderNull::SetCurrentPts(int64_t pts) {
  return true;
}

}  // namespace media
}  // namespace chromecast
