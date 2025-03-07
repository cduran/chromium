// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/mirroring/service/video_capture_client.h"

#include "media/base/bind_to_current_loop.h"
#include "media/base/video_frame.h"

namespace mirroring {

namespace {
// Required by mojom::VideoCaptureHost interface. Can be any number.
constexpr int kDeviceId = 0;
}  // namespace

VideoCaptureClient::VideoCaptureClient(media::mojom::VideoCaptureHostPtr host)
    : video_capture_host_(std::move(host)),
      binding_(this),
      weak_factory_(this) {
  DCHECK(video_capture_host_);
}

VideoCaptureClient::~VideoCaptureClient() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  Stop();
}

void VideoCaptureClient::Start(FrameDeliverCallback deliver_callback,
                               base::OnceClosure error_callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(1) << __func__;
  DCHECK(!deliver_callback.is_null());
  frame_deliver_callback_ = std::move(deliver_callback);
  error_callback_ = std::move(error_callback);

  media::mojom::VideoCaptureObserverPtr observer;
  binding_.Bind(mojo::MakeRequest(&observer));
  video_capture_host_->Start(kDeviceId, 0, media::VideoCaptureParams(),
                             std::move(observer));
}

void VideoCaptureClient::Stop() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(1) << __func__;
  video_capture_host_->Stop(kDeviceId);
}

void VideoCaptureClient::Pause() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(2) << __func__;
  if (frame_deliver_callback_.is_null())
    return;
  frame_deliver_callback_.Reset();
  video_capture_host_->Pause(kDeviceId);
}

void VideoCaptureClient::Resume(FrameDeliverCallback deliver_callback) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(2) << __func__;
  DCHECK(!deliver_callback.is_null());
  if (!frame_deliver_callback_.is_null()) {
    return;
  }
  frame_deliver_callback_ = std::move(deliver_callback);
  video_capture_host_->Resume(kDeviceId, 0, media::VideoCaptureParams());
}

void VideoCaptureClient::RequestRefreshFrame() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (frame_deliver_callback_.is_null())
    return;
  video_capture_host_->RequestRefreshFrame(kDeviceId);
}

void VideoCaptureClient::OnStateChanged(media::mojom::VideoCaptureState state) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(2) << __func__ << " state: " << state;

  switch (state) {
    case media::mojom::VideoCaptureState::STARTED:
      RequestRefreshFrame();
      break;
    case media::mojom::VideoCaptureState::FAILED:
      if (!error_callback_.is_null())
        std::move(error_callback_).Run();
      break;
    case media::mojom::VideoCaptureState::PAUSED:
    case media::mojom::VideoCaptureState::RESUMED:
      break;
    case media::mojom::VideoCaptureState::STOPPED:
    case media::mojom::VideoCaptureState::ENDED:
      client_buffers_.clear();
      mapped_buffers_.clear();
      weak_factory_.InvalidateWeakPtrs();
      error_callback_.Reset();
      frame_deliver_callback_.Reset();
      binding_.Close();
      break;
  }
}

void VideoCaptureClient::OnBufferCreated(
    int32_t buffer_id,
    mojo::ScopedSharedBufferHandle handle) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(handle.is_valid());
  DVLOG(3) << __func__ << ": buffer_id=" << buffer_id;

  const auto insert_result =
      client_buffers_.emplace(std::make_pair(buffer_id, std::move(handle)));
  DCHECK(insert_result.second);
}

void VideoCaptureClient::OnBufferReady(int32_t buffer_id,
                                       media::mojom::VideoFrameInfoPtr info) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(3) << __func__ << ": buffer_id=" << buffer_id;

  bool consume_buffer = !frame_deliver_callback_.is_null();
  if ((info->pixel_format != media::PIXEL_FORMAT_I420 &&
       info->pixel_format != media::PIXEL_FORMAT_Y16) ||
      info->storage_type != media::VideoPixelStorage::CPU) {
    consume_buffer = false;
    LOG(DFATAL) << "Wrong pixel format or storage, got pixel format:"
                << VideoPixelFormatToString(info->pixel_format)
                << ", storage:" << static_cast<int>(info->storage_type);
  }
  if (!consume_buffer) {
    video_capture_host_->ReleaseBuffer(kDeviceId, buffer_id, -1.0);
    return;
  }

  base::TimeTicks reference_time;
  media::VideoFrameMetadata frame_metadata;
  frame_metadata.MergeInternalValuesFrom(*info->metadata);
  const bool success = frame_metadata.GetTimeTicks(
      media::VideoFrameMetadata::REFERENCE_TIME, &reference_time);
  DCHECK(success);

  if (first_frame_ref_time_.is_null())
    first_frame_ref_time_ = reference_time;

  // If the timestamp is not prepared, we use reference time to make a rough
  // estimate. e.g. ThreadSafeCaptureOracle::DidCaptureFrame().
  // TODO(crbug/618407): Fix upstream capturers to always set timestamp and
  // reference time.
  if (info->timestamp.is_zero())
    info->timestamp = reference_time - first_frame_ref_time_;

  // Used by chrome/browser/extension/api/cast_streaming/performance_test.cc
  TRACE_EVENT_INSTANT2("cast_perf_test", "OnBufferReceived",
                       TRACE_EVENT_SCOPE_THREAD, "timestamp",
                       (reference_time - base::TimeTicks()).InMicroseconds(),
                       "time_delta", info->timestamp.InMicroseconds());

  const auto& buffer_iter = client_buffers_.find(buffer_id);
  DCHECK(buffer_iter != client_buffers_.end());

  auto mapping_iter = mapped_buffers_.find(buffer_id);
  const size_t buffer_size =
      media::VideoFrame::AllocationSize(info->pixel_format, info->coded_size);

  if (mapping_iter != mapped_buffers_.end() &&
      buffer_size > mapping_iter->second.second) {
    // Unmaps shared memory for too-small region.
    mapped_buffers_.erase(mapping_iter);
    mapping_iter = mapped_buffers_.end();
  }

  if (mapping_iter == mapped_buffers_.end()) {
    const auto insert_result = mapped_buffers_.emplace(std::make_pair(
        buffer_id,
        MappingAndSize(buffer_iter->second->Map(buffer_size), buffer_size)));
    DCHECK(insert_result.second);
    mapping_iter = insert_result.first;
    if (!mapping_iter->second.first) {
      VLOG(1) << __func__ << ": Mapping Error";
      mapped_buffers_.erase(mapping_iter);
      video_capture_host_->ReleaseBuffer(kDeviceId, buffer_id, -1.0);
      return;
    }
  }

  DCHECK(mapping_iter != mapped_buffers_.end());

  const auto& buffer = mapping_iter->second;
  scoped_refptr<media::VideoFrame> frame = media::VideoFrame::WrapExternalData(
      info->pixel_format, info->coded_size, info->visible_rect,
      info->visible_rect.size(), reinterpret_cast<uint8_t*>(buffer.first.get()),
      buffer.second, info->timestamp);
  if (!frame) {
    video_capture_host_->ReleaseBuffer(kDeviceId, buffer_id, -1.0);
    return;
  }

  BufferFinishedCallback buffer_finished_callback = media::BindToCurrentLoop(
      base::BindOnce(&VideoCaptureClient::OnClientBufferFinished,
                     weak_factory_.GetWeakPtr(), buffer_id));
  frame->AddDestructionObserver(
      base::BindOnce(&VideoCaptureClient::DidFinishConsumingFrame,
                     frame->metadata(), std::move(buffer_finished_callback)));

  frame->metadata()->MergeInternalValuesFrom(*info->metadata);

  frame_deliver_callback_.Run(frame, reference_time);
}

void VideoCaptureClient::OnBufferDestroyed(int32_t buffer_id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(3) << __func__ << ": buffer_id=" << buffer_id;

  const auto& buffer_iter = client_buffers_.find(buffer_id);
  if (buffer_iter != client_buffers_.end())
    client_buffers_.erase(buffer_iter);
  const auto& mapping_iter = mapped_buffers_.find(buffer_id);
  if (mapping_iter != mapped_buffers_.end())
    mapped_buffers_.erase(mapping_iter);
}

void VideoCaptureClient::OnClientBufferFinished(
    int buffer_id,
    double consumer_resource_utilization) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DVLOG(3) << __func__ << ": buffer_id=" << buffer_id;

  // Buffer was already destroyed.
  if (client_buffers_.find(buffer_id) == client_buffers_.end()) {
    DCHECK(mapped_buffers_.find(buffer_id) == mapped_buffers_.end());
    return;
  }

  video_capture_host_->ReleaseBuffer(kDeviceId, buffer_id,
                                     consumer_resource_utilization);
}

// static
void VideoCaptureClient::DidFinishConsumingFrame(
    const media::VideoFrameMetadata* metadata,
    BufferFinishedCallback callback) {
  // Note: This function may be called on any thread by the VideoFrame
  // destructor.  |metadata| is still valid for read-access at this point.
  double consumer_resource_utilization = -1.0;
  if (!metadata->GetDouble(media::VideoFrameMetadata::RESOURCE_UTILIZATION,
                           &consumer_resource_utilization)) {
    consumer_resource_utilization = -1.0;
  }
  DCHECK(!callback.is_null());
  std::move(callback).Run(consumer_resource_utilization);
}

}  // namespace mirroring
