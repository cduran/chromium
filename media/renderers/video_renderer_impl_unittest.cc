// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>

#include <memory>
#include <utility>

#include "base/bind.h"
#include "base/callback.h"
#include "base/callback_helpers.h"
#include "base/containers/circular_deque.h"
#include "base/debug/stack_trace.h"
#include "base/macros.h"
#include "base/memory/memory_pressure_listener.h"
#include "base/memory/memory_pressure_monitor.h"
#include "base/memory/ptr_util.h"
#include "base/message_loop/message_loop.h"
#include "base/run_loop.h"
#include "base/single_thread_task_runner.h"
#include "base/stl_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/stringprintf.h"
#include "base/synchronization/lock.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/simple_test_tick_clock.h"
#include "media/base/data_buffer.h"
#include "media/base/gmock_callback_support.h"
#include "media/base/limits.h"
#include "media/base/media_switches.h"
#include "media/base/mock_filters.h"
#include "media/base/null_video_sink.h"
#include "media/base/test_helpers.h"
#include "media/base/video_frame.h"
#include "media/base/wall_clock_time_source.h"
#include "media/renderers/video_renderer_impl.h"
#include "testing/gmock_mutant.h"
#include "testing/gtest/include/gtest/gtest.h"

using ::testing::_;
using ::testing::AnyNumber;
using ::testing::CreateFunctor;
using ::testing::Invoke;
using ::testing::Mock;
using ::testing::NiceMock;
using ::testing::Return;
using ::testing::SaveArg;
using ::testing::StrictMock;

namespace media {

MATCHER_P(HasTimestampMatcher, ms, "") {
  *result_listener << "has timestamp " << arg->timestamp().InMilliseconds();
  return arg->timestamp().InMilliseconds() == ms;
}

class VideoRendererImplTest : public testing::Test {
 public:
  std::vector<std::unique_ptr<VideoDecoder>> CreateVideoDecodersForTest() {
    decoder_ = new NiceMock<MockVideoDecoder>();
    std::vector<std::unique_ptr<VideoDecoder>> decoders;
    decoders.push_back(base::WrapUnique(decoder_));
    ON_CALL(*decoder_, Initialize(_, _, _, _, _, _))
        .WillByDefault(DoAll(SaveArg<4>(&output_cb_),
                             RunCallback<3>(expect_init_success_)));
    // Monitor decodes from the decoder.
    ON_CALL(*decoder_, Decode(_, _))
        .WillByDefault(Invoke(this, &VideoRendererImplTest::DecodeRequested));
    ON_CALL(*decoder_, Reset(_))
        .WillByDefault(Invoke(this, &VideoRendererImplTest::FlushRequested));
    ON_CALL(*decoder_, GetMaxDecodeRequests()).WillByDefault(Return(1));
    return decoders;
  }

  VideoRendererImplTest()
      : decoder_(nullptr),
        demuxer_stream_(DemuxerStream::VIDEO),
        simulate_decode_delay_(false),
        expect_init_success_(true) {
    null_video_sink_.reset(new NullVideoSink(
        false, base::TimeDelta::FromSecondsD(1.0 / 60),
        base::Bind(&MockCB::FrameReceived, base::Unretained(&mock_cb_)),
        message_loop_.task_runner()));

    // Complexity based buffering does not affect any tests not specifically
    // written to test it, so enable it always.
    scoped_feature_list_.InitAndEnableFeature(kComplexityBasedVideoBuffering);
    renderer_.reset(new VideoRendererImpl(
        message_loop_.task_runner(), null_video_sink_.get(),
        base::Bind(&VideoRendererImplTest::CreateVideoDecodersForTest,
                   base::Unretained(this)),
        true, &media_log_));
    renderer_->SetTickClockForTesting(&tick_clock_);
    null_video_sink_->set_tick_clock_for_testing(&tick_clock_);
    time_source_.set_tick_clock_for_testing(&tick_clock_);

    // Start wallclock time at a non-zero value.
    AdvanceWallclockTimeInMs(12345);

    demuxer_stream_.set_video_decoder_config(TestVideoConfig::Normal());

    // We expect these to be called but we don't care how/when.
    EXPECT_CALL(demuxer_stream_, Read(_)).WillRepeatedly(
        RunCallback<0>(DemuxerStream::kOk,
                       scoped_refptr<DecoderBuffer>(new DecoderBuffer(0))));
  }

  virtual ~VideoRendererImplTest() = default;

  void Initialize() {
    InitializeWithLowDelay(false);
  }

  void InitializeWithLowDelay(bool low_delay) {
    // Initialize, we shouldn't have any reads.
    InitializeRenderer(&demuxer_stream_, low_delay, true);
  }

  void InitializeRenderer(MockDemuxerStream* demuxer_stream,
                          bool low_delay,
                          bool expect_success) {
    SCOPED_TRACE(base::StringPrintf("InitializeRenderer(%d)", expect_success));
    expect_init_success_ = expect_success;
    WaitableMessageLoopEvent event;
    CallInitialize(demuxer_stream, event.GetPipelineStatusCB(), low_delay,
                   expect_success);
    event.RunAndWaitForStatus(expect_success ? PIPELINE_OK
                                             : DECODER_ERROR_NOT_SUPPORTED);
  }

  void CallInitialize(MockDemuxerStream* demuxer_stream,
                      const PipelineStatusCB& status_cb,
                      bool low_delay,
                      bool expect_success) {
    if (low_delay)
      demuxer_stream->set_liveness(DemuxerStream::LIVENESS_LIVE);
    EXPECT_CALL(mock_cb_, OnWaitingForDecryptionKey()).Times(0);
    EXPECT_CALL(mock_cb_, OnAudioConfigChange(_)).Times(0);
    EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
    renderer_->Initialize(demuxer_stream, nullptr, &mock_cb_,
                          base::Bind(&WallClockTimeSource::GetWallClockTimes,
                                     base::Unretained(&time_source_)),
                          status_cb);
  }

  void StartPlayingFrom(int milliseconds) {
    SCOPED_TRACE(base::StringPrintf("StartPlayingFrom(%d)", milliseconds));
    const base::TimeDelta media_time =
        base::TimeDelta::FromMilliseconds(milliseconds);
    time_source_.SetMediaTime(media_time);
    renderer_->StartPlayingFrom(media_time);
    base::RunLoop().RunUntilIdle();
  }

  void Flush() {
    SCOPED_TRACE("Flush()");
    WaitableMessageLoopEvent event;
    renderer_->Flush(event.GetClosure());
    event.RunAndWait();
  }

  void Destroy() {
    SCOPED_TRACE("Destroy()");
    renderer_.reset();
    base::RunLoop().RunUntilIdle();
  }

  // Parses a string representation of video frames and generates corresponding
  // VideoFrame objects in |decode_results_|.
  //
  // Syntax:
  //   nn - Queue a decoder buffer with timestamp nn * 1000us
  //   abort - Queue an aborted read
  //   error - Queue a decoder error
  //
  // Examples:
  //   A clip that is four frames long: "0 10 20 30"
  //   A clip that has a decode error: "60 70 error"
  void QueueFrames(const std::string& str) {
    for (const std::string& token :
         base::SplitString(str, " ", base::TRIM_WHITESPACE,
                           base::SPLIT_WANT_ALL)) {
      if (token == "abort") {
        scoped_refptr<VideoFrame> null_frame;
        QueueFrame(DecodeStatus::ABORTED, null_frame);
        continue;
      }

      if (token == "error") {
        scoped_refptr<VideoFrame> null_frame;
        QueueFrame(DecodeStatus::DECODE_ERROR, null_frame);
        continue;
      }

      int timestamp_in_ms = 0;
      if (base::StringToInt(token, &timestamp_in_ms)) {
        gfx::Size natural_size = TestVideoConfig::NormalCodedSize();
        scoped_refptr<VideoFrame> frame = VideoFrame::CreateFrame(
            PIXEL_FORMAT_I420, natural_size, gfx::Rect(natural_size),
            natural_size, base::TimeDelta::FromMilliseconds(timestamp_in_ms));
        QueueFrame(DecodeStatus::OK, frame);
        continue;
      }

      CHECK(false) << "Unrecognized decoder buffer token: " << token;
    }
  }

  // Queues video frames to be served by the decoder during rendering.
  void QueueFrame(DecodeStatus status, scoped_refptr<VideoFrame> frame) {
    decode_results_.push_back(std::make_pair(status, frame));
  }

  bool IsReadPending() {
    return !decode_cb_.is_null();
  }

  void WaitForError(PipelineStatus expected) {
    SCOPED_TRACE(base::StringPrintf("WaitForError(%d)", expected));

    WaitableMessageLoopEvent event;
    PipelineStatusCB error_cb = event.GetPipelineStatusCB();
    EXPECT_CALL(mock_cb_, OnError(_)).WillOnce(Invoke(CreateFunctor(error_cb)));
    event.RunAndWaitForStatus(expected);
  }

  void WaitForEnded() {
    SCOPED_TRACE("WaitForEnded()");

    WaitableMessageLoopEvent event;
    EXPECT_CALL(mock_cb_, OnEnded()).WillOnce(RunClosure(event.GetClosure()));
    event.RunAndWait();
  }

  void WaitForPendingDecode() {
    SCOPED_TRACE("WaitForPendingDecode()");
    if (!decode_cb_.is_null())
      return;

    DCHECK(wait_for_pending_decode_cb_.is_null());

    WaitableMessageLoopEvent event;
    wait_for_pending_decode_cb_ = event.GetClosure();
    event.RunAndWait();

    DCHECK(!decode_cb_.is_null());
    DCHECK(wait_for_pending_decode_cb_.is_null());
  }

  void SatisfyPendingDecode() {
    CHECK(!decode_cb_.is_null());
    CHECK(!decode_results_.empty());

    // Post tasks for OutputCB and DecodeCB.
    scoped_refptr<VideoFrame> frame = decode_results_.front().second;
    if (frame.get())
      message_loop_.task_runner()->PostTask(FROM_HERE,
                                            base::Bind(output_cb_, frame));
    message_loop_.task_runner()->PostTask(
        FROM_HERE, base::Bind(base::ResetAndReturn(&decode_cb_),
                              decode_results_.front().first));
    decode_results_.pop_front();
  }

  void SatisfyPendingDecodeWithEndOfStream() {
    DCHECK(!decode_cb_.is_null());

    // Return EOS buffer to trigger EOS frame.
    EXPECT_CALL(demuxer_stream_, Read(_))
        .WillOnce(RunCallback<0>(DemuxerStream::kOk,
                                 DecoderBuffer::CreateEOSBuffer()));

    // Satify pending |decode_cb_| to trigger a new DemuxerStream::Read().
    message_loop_.task_runner()->PostTask(
        FROM_HERE,
        base::Bind(base::ResetAndReturn(&decode_cb_), DecodeStatus::OK));

    WaitForPendingDecode();

    message_loop_.task_runner()->PostTask(
        FROM_HERE,
        base::Bind(base::ResetAndReturn(&decode_cb_), DecodeStatus::OK));
  }

  void AdvanceWallclockTimeInMs(int time_ms) {
    DCHECK_EQ(&message_loop_, base::MessageLoop::current());
    base::AutoLock l(lock_);
    tick_clock_.Advance(base::TimeDelta::FromMilliseconds(time_ms));
  }

  void AdvanceTimeInMs(int time_ms) {
    DCHECK_EQ(&message_loop_, base::MessageLoop::current());
    base::AutoLock l(lock_);
    time_ += base::TimeDelta::FromMilliseconds(time_ms);
    time_source_.StopTicking();
    time_source_.SetMediaTime(time_);
    time_source_.StartTicking();
  }

  enum class UnderflowTestType {
    NORMAL,
    LOW_DELAY,
    CANT_READ_WITHOUT_STALLING
  };
  void BasicUnderflowTest(UnderflowTestType type) {
    InitializeWithLowDelay(type == UnderflowTestType::LOW_DELAY);
    if (type == UnderflowTestType::CANT_READ_WITHOUT_STALLING)
      ON_CALL(*decoder_, CanReadWithoutStalling()).WillByDefault(Return(false));

    QueueFrames("0 30 60 90");

    {
      WaitableMessageLoopEvent event;
      EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
      EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH))
          .WillOnce(RunClosure(event.GetClosure()));
      EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
      EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
      EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
      StartPlayingFrom(0);
      event.RunAndWait();
      Mock::VerifyAndClearExpectations(&mock_cb_);
    }

    renderer_->OnTimeProgressing();

    // Advance time slightly, but enough to exceed the duration of the last
    // frame.
    // Frames should be dropped and we should NOT signal having nothing.
    {
      SCOPED_TRACE("Waiting for frame drops");
      WaitableMessageLoopEvent event;

      // Note: Starting the TimeSource will cause the old VideoRendererImpl to
      // start rendering frames on its own thread, so the first frame may be
      // received.
      time_source_.StartTicking();
      EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(30))).Times(0);

      EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(60))).Times(0);
      EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(90)))
          .WillOnce(RunClosure(event.GetClosure()));
      EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
      AdvanceTimeInMs(91);

      event.RunAndWait();
      Mock::VerifyAndClearExpectations(&mock_cb_);
    }

    // Advance time more. Now we should signal having nothing. And put
    // the last frame up for display.
    {
      SCOPED_TRACE("Waiting for BUFFERING_HAVE_NOTHING");
      WaitableMessageLoopEvent event;
      EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_NOTHING))
          .WillOnce(RunClosure(event.GetClosure()));
      AdvanceTimeInMs(30);
      event.RunAndWait();
      Mock::VerifyAndClearExpectations(&mock_cb_);
    }

    // Simulate delayed buffering state callbacks.
    renderer_->OnTimeStopped();
    renderer_->OnTimeProgressing();

    // Receiving end of stream should signal having enough.
    {
      SCOPED_TRACE("Waiting for BUFFERING_HAVE_ENOUGH");
      WaitableMessageLoopEvent event;
      EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
      EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH))
          .WillOnce(RunClosure(event.GetClosure()));
      EXPECT_CALL(mock_cb_, OnEnded());
      SatisfyPendingDecodeWithEndOfStream();
      event.RunAndWait();
    }

    Destroy();
  }

  void UnderflowRecoveryTest(UnderflowTestType type) {
    InitializeWithLowDelay(type == UnderflowTestType::LOW_DELAY);
    if (type == UnderflowTestType::CANT_READ_WITHOUT_STALLING)
      ON_CALL(*decoder_, CanReadWithoutStalling()).WillByDefault(Return(false));

    QueueFrames("0 20 40 60");
    {
      WaitableMessageLoopEvent event;
      EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
      EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH))
          .WillOnce(RunClosure(event.GetClosure()));
      EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
      EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
      EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
      StartPlayingFrom(0);
      event.RunAndWait();
      Mock::VerifyAndClearExpectations(&mock_cb_);
    }

    renderer_->OnTimeProgressing();
    time_source_.StartTicking();

    // Advance time, this should cause have nothing to be signaled.
    {
      SCOPED_TRACE("Waiting for BUFFERING_HAVE_NOTHING");
      WaitableMessageLoopEvent event;
      EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_NOTHING))
          .WillOnce(RunClosure(event.GetClosure()));
      EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(60))).Times(1);
      EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
      AdvanceTimeInMs(79);
      event.RunAndWait();
      Mock::VerifyAndClearExpectations(&mock_cb_);
    }

    EXPECT_EQ(1u, renderer_->frames_queued_for_testing());
    time_source_.StopTicking();
    renderer_->OnTimeStopped();
    EXPECT_EQ(0u, renderer_->frames_queued_for_testing());
    ASSERT_TRUE(IsReadPending());

    // Queue some frames, satisfy reads, and make sure expired frames are gone
    // when the renderer paints the first frame.
    {
      SCOPED_TRACE("Waiting for BUFFERING_HAVE_ENOUGH");
      WaitableMessageLoopEvent event;
      EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(80))).Times(1);
      EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
      EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH))
          .WillOnce(RunClosure(event.GetClosure()));

      // Note: In the normal underflow case we queue 5 frames here instead of
      // four since the underflow increases the number of required frames to
      // reach the have enough state.
      if (type == UnderflowTestType::NORMAL)
        QueueFrames("80 100 120 140 160");
      else
        QueueFrames("40 60 80 90 100");
      SatisfyPendingDecode();
      event.RunAndWait();
    }

    Destroy();
  }

  MOCK_METHOD0(OnSimulateDecodeDelay, base::TimeDelta(void));

 protected:
  base::MessageLoop message_loop_;
  MediaLog media_log_;

  // Fixture members.
  base::test::ScopedFeatureList scoped_feature_list_;
  std::unique_ptr<VideoRendererImpl> renderer_;
  base::SimpleTestTickClock tick_clock_;
  NiceMock<MockVideoDecoder>* decoder_;    // Owned by |renderer_|.
  NiceMock<MockDemuxerStream> demuxer_stream_;
  bool simulate_decode_delay_;

  bool expect_init_success_;

  // Use StrictMock<T> to catch missing/extra callbacks.
  class MockCB : public MockRendererClient {
   public:
    MOCK_METHOD1(FrameReceived, void(const scoped_refptr<VideoFrame>&));
  };
  StrictMock<MockCB> mock_cb_;

  // Must be destroyed before |renderer_| since they share |tick_clock_|.
  std::unique_ptr<NullVideoSink> null_video_sink_;

  WallClockTimeSource time_source_;

 private:
  void DecodeRequested(const scoped_refptr<DecoderBuffer>& buffer,
                       const VideoDecoder::DecodeCB& decode_cb) {
    DCHECK_EQ(&message_loop_, base::MessageLoop::current());
    CHECK(decode_cb_.is_null());
    decode_cb_ = decode_cb;

    // Wake up WaitForPendingDecode() if needed.
    if (!wait_for_pending_decode_cb_.is_null())
      base::ResetAndReturn(&wait_for_pending_decode_cb_).Run();

    if (decode_results_.empty())
      return;

    if (simulate_decode_delay_)
      tick_clock_.Advance(OnSimulateDecodeDelay());

    SatisfyPendingDecode();
  }

  void FlushRequested(const base::Closure& callback) {
    DCHECK_EQ(&message_loop_, base::MessageLoop::current());
    decode_results_.clear();
    if (!decode_cb_.is_null()) {
      QueueFrames("abort");
      SatisfyPendingDecode();
    }

    message_loop_.task_runner()->PostTask(FROM_HERE, callback);
  }

  // Used to protect |time_|.
  base::Lock lock_;
  base::TimeDelta time_;

  // Used for satisfying reads.
  VideoDecoder::OutputCB output_cb_;
  VideoDecoder::DecodeCB decode_cb_;
  base::TimeDelta next_frame_timestamp_;

  // Run during DecodeRequested() to unblock WaitForPendingDecode().
  base::Closure wait_for_pending_decode_cb_;

  base::circular_deque<std::pair<DecodeStatus, scoped_refptr<VideoFrame>>>
      decode_results_;

  DISALLOW_COPY_AND_ASSIGN(VideoRendererImplTest);
};

TEST_F(VideoRendererImplTest, DoNothing) {
  // Test that creation and deletion doesn't depend on calls to Initialize()
  // and/or Destroy().
}

TEST_F(VideoRendererImplTest, DestroyWithoutInitialize) {
  Destroy();
}

TEST_F(VideoRendererImplTest, Initialize) {
  Initialize();
  Destroy();
}

TEST_F(VideoRendererImplTest, InitializeAndStartPlayingFrom) {
  Initialize();
  QueueFrames("0 10 20 30");
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
  StartPlayingFrom(0);
  Destroy();
}

TEST_F(VideoRendererImplTest, InitializeAndEndOfStream) {
  Initialize();
  StartPlayingFrom(0);
  WaitForPendingDecode();
  {
    SCOPED_TRACE("Waiting for BUFFERING_HAVE_ENOUGH");
    WaitableMessageLoopEvent event;
    EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH))
        .WillOnce(RunClosure(event.GetClosure()));
    EXPECT_CALL(mock_cb_, OnEnded());
    SatisfyPendingDecodeWithEndOfStream();
    event.RunAndWait();
  }
  // Firing a time state changed to true should be ignored...
  renderer_->OnTimeProgressing();
  EXPECT_FALSE(null_video_sink_->is_started());
  Destroy();
}

TEST_F(VideoRendererImplTest, ReinitializeForAnotherStream) {
  Initialize();
  StartPlayingFrom(0);
  Flush();
  NiceMock<MockDemuxerStream> new_stream(DemuxerStream::VIDEO);
  new_stream.set_video_decoder_config(TestVideoConfig::Normal());
  InitializeRenderer(&new_stream, false, true);
}

TEST_F(VideoRendererImplTest, DestroyWhileInitializing) {
  CallInitialize(&demuxer_stream_, NewExpectedStatusCB(PIPELINE_ERROR_ABORT),
                 false, PIPELINE_OK);
  Destroy();
}

TEST_F(VideoRendererImplTest, DestroyWhileFlushing) {
  Initialize();
  QueueFrames("0 10 20 30");
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
  StartPlayingFrom(0);
  renderer_->Flush(NewExpectedClosure());
  Destroy();
}

TEST_F(VideoRendererImplTest, Play) {
  Initialize();
  QueueFrames("0 10 20 30");
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
  StartPlayingFrom(0);
  Destroy();
}

TEST_F(VideoRendererImplTest, FlushWithNothingBuffered) {
  Initialize();
  StartPlayingFrom(0);

  // We shouldn't expect a buffering state change since we never reached
  // BUFFERING_HAVE_ENOUGH.
  Flush();
  Destroy();
}

// Verify that the flush callback is invoked outside of VideoRenderer lock, so
// we should be able to call other renderer methods from the Flush callback.
static void VideoRendererImplTest_FlushDoneCB(VideoRendererImplTest* test,
                                              VideoRenderer* renderer,
                                              const base::Closure& success_cb) {
  test->QueueFrames("0 10 20 30");
  renderer->StartPlayingFrom(base::TimeDelta::FromSeconds(0));
  success_cb.Run();
}

TEST_F(VideoRendererImplTest, FlushCallbackNoLock) {
  Initialize();
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
  StartPlayingFrom(0);
  WaitableMessageLoopEvent event;
  renderer_->Flush(
      base::Bind(&VideoRendererImplTest_FlushDoneCB, base::Unretained(this),
                 base::Unretained(renderer_.get()), event.GetClosure()));
  event.RunAndWait();
  Destroy();
}

TEST_F(VideoRendererImplTest, DecodeError_Playing) {
  Initialize();
  QueueFrames("0 10 20 30");
  EXPECT_CALL(mock_cb_, FrameReceived(_)).Times(testing::AtLeast(1));

  // Consider the case that rendering is faster than we setup the test event.
  // In that case, when we run out of the frames, BUFFERING_HAVE_NOTHING will
  // be called.
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH));
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_NOTHING))
      .Times(testing::AtMost(1));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);

  StartPlayingFrom(0);
  renderer_->OnTimeProgressing();
  time_source_.StartTicking();
  AdvanceTimeInMs(10);

  QueueFrames("error");
  SatisfyPendingDecode();
  WaitForError(PIPELINE_ERROR_DECODE);
  Destroy();
}

TEST_F(VideoRendererImplTest, DecodeError_DuringStartPlayingFrom) {
  Initialize();
  QueueFrames("error");
  EXPECT_CALL(mock_cb_, OnError(PIPELINE_ERROR_DECODE));
  StartPlayingFrom(0);
  Destroy();
}

TEST_F(VideoRendererImplTest, StartPlayingFrom_Exact) {
  Initialize();
  QueueFrames("50 60 70 80 90");

  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(60)));
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
  StartPlayingFrom(60);
  Destroy();
}

TEST_F(VideoRendererImplTest, StartPlayingFrom_RightBefore) {
  Initialize();
  QueueFrames("50 60 70 80 90");

  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(50)));
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
  StartPlayingFrom(59);
  Destroy();
}

TEST_F(VideoRendererImplTest, StartPlayingFrom_RightAfter) {
  Initialize();
  QueueFrames("50 60 70 80 90");

  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(60)));
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
  StartPlayingFrom(61);
  Destroy();
}

TEST_F(VideoRendererImplTest, StartPlayingFrom_LowDelay) {
  // In low-delay mode only one frame is required to finish preroll. But frames
  // prior to the start time will not be used.
  InitializeWithLowDelay(true);
  QueueFrames("0 10");

  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(10)));
  // Expect some amount of have enough/nothing due to only requiring one frame.
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH))
      .Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_NOTHING))
      .Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
  StartPlayingFrom(10);

  QueueFrames("20");
  SatisfyPendingDecode();

  renderer_->OnTimeProgressing();
  time_source_.StartTicking();

  WaitableMessageLoopEvent event;
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(20)))
      .WillOnce(RunClosure(event.GetClosure()));
  AdvanceTimeInMs(20);
  event.RunAndWait();

  Destroy();
}

// Ensures that we don't waste memory trying to keep up with decoders that are
// too slow to playback video in real time.
TEST_F(VideoRendererImplTest, ComplexityBasedBufferingRealtimeIncapable) {
  Initialize();

  QueueFrames("0 1000 2000 3000 4000 5000 6000 7000 8000");
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);

  simulate_decode_delay_ = true;
  null_video_sink_->set_clockless(true);

  // Set a decode delay of 4s per 1s frame; too slow for realtime playback.
  EXPECT_CALL(*this, OnSimulateDecodeDelay())
      .WillRepeatedly(Return(base::TimeDelta::FromSeconds(4)));

  StartPlayingFrom(0);
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->min_buffered_frames_for_testing());
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->max_buffered_frames_for_testing());

  renderer_->OnTimeProgressing();
  time_source_.StartTicking();

  WaitableMessageLoopEvent event;
  EXPECT_CALL(mock_cb_, FrameReceived(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(8000)))
      .WillOnce(RunClosure(event.GetClosure()));
  event.RunAndWait();

  // No buffering should have been triggered due to speed.
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->min_buffered_frames_for_testing());
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->max_buffered_frames_for_testing());
  Destroy();
}

class TestMemoryPressureMonitor : public base::MemoryPressureMonitor {
 public:
  TestMemoryPressureMonitor() = default;
  ~TestMemoryPressureMonitor() override = default;

  MemoryPressureLevel GetCurrentPressureLevel() override {
    return base::MemoryPressureListener::MEMORY_PRESSURE_LEVEL_MODERATE;
  }

  void SetDispatchCallback(const DispatchCallback& callback) override {
    NOTREACHED();
  }

 private:
  DISALLOW_COPY_AND_ASSIGN(TestMemoryPressureMonitor);
};

// Ensures that we don't waste memory during low memory situations.
TEST_F(VideoRendererImplTest, ComplexityBasedBufferingMemoryPressure) {
  Initialize();

  QueueFrames("0 1000 2000 3000 4000 5000 6000 7000 8000");
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);

  simulate_decode_delay_ = true;
  null_video_sink_->set_clockless(true);

  TestMemoryPressureMonitor test_monitor;
  EXPECT_CALL(*this, OnSimulateDecodeDelay())
      .WillOnce(Return(base::TimeDelta::FromSeconds(4)))
      .WillRepeatedly(Return(base::TimeDelta::FromMilliseconds(100)));

  StartPlayingFrom(0);
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->min_buffered_frames_for_testing());
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->max_buffered_frames_for_testing());

  renderer_->OnTimeProgressing();
  time_source_.StartTicking();

  WaitableMessageLoopEvent event;
  EXPECT_CALL(mock_cb_, FrameReceived(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(8000)))
      .WillOnce(RunClosure(event.GetClosure()));
  event.RunAndWait();

  // No buffering should have been triggered due to memory pressure.
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->min_buffered_frames_for_testing());

  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->max_buffered_frames_for_testing());
  Destroy();
}

// Ensures that we don't waste memory during low memory situations.
TEST_F(VideoRendererImplTest, ComplexityBasedBufferingBackgroundRendering) {
  Initialize();

  QueueFrames("0 1000 2000 3000 4000 5000 6000 7000 8000");
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);

  simulate_decode_delay_ = true;
  null_video_sink_->set_clockless(true);
  null_video_sink_->set_background_render(true);

  TestMemoryPressureMonitor test_monitor;
  EXPECT_CALL(*this, OnSimulateDecodeDelay())
      .WillOnce(Return(base::TimeDelta::FromSeconds(4)))
      .WillRepeatedly(Return(base::TimeDelta::FromMilliseconds(100)));

  StartPlayingFrom(0);
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->min_buffered_frames_for_testing());
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->max_buffered_frames_for_testing());

  renderer_->OnTimeProgressing();
  time_source_.StartTicking();

  WaitableMessageLoopEvent event;
  EXPECT_CALL(mock_cb_, FrameReceived(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(8000)))
      .WillOnce(RunClosure(event.GetClosure()));
  event.RunAndWait();

  // No buffering should have been triggered due to background rendering.
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->min_buffered_frames_for_testing());
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->max_buffered_frames_for_testing());
  Destroy();
}

TEST_F(VideoRendererImplTest, ComplexityBasedBuffering) {
  Initialize();

  QueueFrames("0 1000 2000 3000 4000 5000 6000 7000 8000");
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);

  simulate_decode_delay_ = true;
  null_video_sink_->set_clockless(true);
  EXPECT_CALL(*this, OnSimulateDecodeDelay())
      .WillOnce(Return(base::TimeDelta::FromSeconds(4)))
      .WillRepeatedly(Return(base::TimeDelta::FromMilliseconds(100)));

  StartPlayingFrom(0);

  // Prior to playback start no extended buffering should be triggered.
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->min_buffered_frames_for_testing());
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->max_buffered_frames_for_testing());

  renderer_->OnTimeProgressing();
  time_source_.StartTicking();

  {
    // Not enough frames have been played to trigger extended buffering yet;
    // start from frame 1s, since 0s is painted as the poster image.
    WaitableMessageLoopEvent event;
    EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(1000)))
        .WillOnce(RunClosure(event.GetClosure()));
    event.RunAndWait();
    EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
              renderer_->min_buffered_frames_for_testing());
    EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
              renderer_->max_buffered_frames_for_testing());
  }

  WaitableMessageLoopEvent event;
  EXPECT_CALL(mock_cb_, FrameReceived(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(8000)))
      .WillOnce(RunClosure(event.GetClosure()));
  event.RunAndWait();

  // 4 frames * (4s - 1s) / 1s
  EXPECT_EQ(12u, renderer_->max_buffered_frames_for_testing());

  time_source_.StopTicking();
  renderer_->OnTimeStopped();
  Flush();

  // Ensure min/max buffered frames is reset.
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->min_buffered_frames_for_testing());
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->max_buffered_frames_for_testing());
  Destroy();
}

TEST_F(VideoRendererImplTest, ComplexityBasedBufferingUnderflow) {
  Initialize();

  QueueFrames("0 1000 2000 3000 4000 5000 6000 7000 8000");
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);

  simulate_decode_delay_ = true;
  null_video_sink_->set_clockless(true);
  EXPECT_CALL(*this, OnSimulateDecodeDelay())
      .WillOnce(Return(base::TimeDelta::FromSeconds(4)))
      .WillRepeatedly(Return(base::TimeDelta::FromMilliseconds(100)));

  StartPlayingFrom(0);

  // Prior to playback start no extended buffering should be triggered.
  EXPECT_EQ(static_cast<size_t>(limits::kMaxVideoFrames),
            renderer_->max_buffered_frames_for_testing());

  renderer_->OnTimeProgressing();
  time_source_.StartTicking();

  WaitableMessageLoopEvent event;
  EXPECT_CALL(mock_cb_, FrameReceived(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_NOTHING))
      .WillOnce(RunClosure(event.GetClosure()));
  event.RunAndWait();

  // 4 frames * (4s - 1s) / 1s
  const size_t kExpectedFrames = 12u;

  EXPECT_EQ(kExpectedFrames, renderer_->max_buffered_frames_for_testing());

  time_source_.StopTicking();
  renderer_->OnTimeStopped();

  // Since we stopped in an underflow situation, ensure max buffered frames is
  // promoted to the new recommended minimum.
  EXPECT_EQ(kExpectedFrames, renderer_->min_buffered_frames_for_testing());
  EXPECT_EQ(kExpectedFrames, renderer_->max_buffered_frames_for_testing());
  Destroy();
}

// Verify that a late decoder response doesn't break invariants in the renderer.
TEST_F(VideoRendererImplTest, DestroyDuringOutstandingRead) {
  Initialize();
  QueueFrames("0 10 20 30");
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
  StartPlayingFrom(0);

  // Check that there is an outstanding Read() request.
  EXPECT_TRUE(IsReadPending());

  Destroy();
}

TEST_F(VideoRendererImplTest, VideoDecoder_InitFailure) {
  InitializeRenderer(&demuxer_stream_, false, false);
  Destroy();
}

TEST_F(VideoRendererImplTest, Underflow) {
  BasicUnderflowTest(UnderflowTestType::NORMAL);
}

TEST_F(VideoRendererImplTest, Underflow_LowDelay) {
  BasicUnderflowTest(UnderflowTestType::LOW_DELAY);
}

TEST_F(VideoRendererImplTest, Underflow_CantReadWithoutStalling) {
  BasicUnderflowTest(UnderflowTestType::CANT_READ_WITHOUT_STALLING);
}

TEST_F(VideoRendererImplTest, UnderflowRecovery) {
  UnderflowRecoveryTest(UnderflowTestType::NORMAL);
}

TEST_F(VideoRendererImplTest, UnderflowRecovery_LowDelay) {
  UnderflowRecoveryTest(UnderflowTestType::LOW_DELAY);
}

TEST_F(VideoRendererImplTest, UnderflowRecovery_CantReadWithoutStalling) {
  UnderflowRecoveryTest(UnderflowTestType::CANT_READ_WITHOUT_STALLING);
}

// Verifies that the first frame is painted w/o rendering being started.
TEST_F(VideoRendererImplTest, RenderingStopsAfterFirstFrame) {
  InitializeWithLowDelay(true);
  QueueFrames("0");

  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnEnded()).Times(0);

  {
    SCOPED_TRACE("Waiting for first frame to be painted.");
    WaitableMessageLoopEvent event;

    EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)))
        .WillOnce(RunClosure(event.GetClosure()));
    StartPlayingFrom(0);

    EXPECT_TRUE(IsReadPending());
    SatisfyPendingDecodeWithEndOfStream();

    event.RunAndWait();
  }

  Destroy();
}

// Verifies that the sink is stopped after rendering the first frame if
// playback has started.
TEST_F(VideoRendererImplTest, RenderingStopsAfterOneFrameWithEOS) {
  InitializeWithLowDelay(true);
  QueueFrames("0");

  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0))).Times(1);
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);

  {
    SCOPED_TRACE("Waiting for sink to stop.");
    WaitableMessageLoopEvent event;

    null_video_sink_->set_stop_cb(event.GetClosure());
    StartPlayingFrom(0);
    renderer_->OnTimeProgressing();

    EXPECT_TRUE(IsReadPending());
    SatisfyPendingDecodeWithEndOfStream();
    WaitForEnded();

    renderer_->OnTimeStopped();
    event.RunAndWait();
  }

  Destroy();
}

// Tests the case where the video started and received a single Render() call,
// then the video was put into the background.
TEST_F(VideoRendererImplTest, RenderingStartedThenStopped) {
  Initialize();
  QueueFrames("0 30 60 90");

  // Start the sink and wait for the first callback.  Set statistics to a non
  // zero value, once we have some decoded frames they should be overwritten.
  PipelineStatistics last_pipeline_statistics;
  last_pipeline_statistics.video_frames_dropped = 1;
  {
    WaitableMessageLoopEvent event;
    EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH))
        .WillOnce(RunClosure(event.GetClosure()));
    EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_))
        .Times(4)
        .WillRepeatedly(SaveArg<0>(&last_pipeline_statistics));
    EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
    EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
    EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
    StartPlayingFrom(0);
    event.RunAndWait();
    Mock::VerifyAndClearExpectations(&mock_cb_);
  }

  // Four calls to update statistics should have been made, each reporting a
  // single decoded frame and one frame worth of memory usage. No dropped frames
  // should be reported later since we're in background rendering mode. These
  // calls must all have occurred before playback starts.
  EXPECT_EQ(0u, last_pipeline_statistics.video_frames_dropped);
  EXPECT_EQ(1u, last_pipeline_statistics.video_frames_decoded);
  EXPECT_EQ(115200, last_pipeline_statistics.video_memory_usage);

  // Consider the case that rendering is faster than we setup the test event.
  // In that case, when we run out of the frames, BUFFERING_HAVE_NOTHING will
  // be called. And then during SatisfyPendingDecodeWithEndOfStream,
  // BUFFER_HAVE_ENOUGH will be called again.
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH))
      .Times(testing::AtMost(1));
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_NOTHING))
      .Times(testing::AtMost(1));
  renderer_->OnTimeProgressing();
  time_source_.StartTicking();

  // Suspend all future callbacks and synthetically advance the media time,
  // because this is a background render, we won't underflow by waiting until
  // a pending read is ready.
  null_video_sink_->set_background_render(true);
  AdvanceTimeInMs(91);
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(90)));
  WaitForPendingDecode();
  SatisfyPendingDecodeWithEndOfStream();

  AdvanceTimeInMs(30);
  WaitForEnded();
  Destroy();
}

// Tests the case where underflow evicts all frames before EOS.
TEST_F(VideoRendererImplTest, UnderflowEvictionBeforeEOS) {
  Initialize();
  QueueFrames("0 30 60 90 100");

  {
    SCOPED_TRACE("Waiting for BUFFERING_HAVE_ENOUGH");
    WaitableMessageLoopEvent event;
    EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH))
        .WillOnce(RunClosure(event.GetClosure()));
    EXPECT_CALL(mock_cb_, FrameReceived(_)).Times(AnyNumber());
    EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
    EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
    EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
    StartPlayingFrom(0);
    event.RunAndWait();
  }

  {
    SCOPED_TRACE("Waiting for BUFFERING_HAVE_NOTHING");
    WaitableMessageLoopEvent event;
    EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_NOTHING))
        .WillOnce(RunClosure(event.GetClosure()));
    renderer_->OnTimeProgressing();
    time_source_.StartTicking();
    // Jump time far enough forward that no frames are valid.
    AdvanceTimeInMs(1000);
    event.RunAndWait();
  }

  WaitForPendingDecode();

  renderer_->OnTimeStopped();
  time_source_.StopTicking();

  // Providing the end of stream packet should remove all frames and exit.
  SatisfyPendingDecodeWithEndOfStream();
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH));
  WaitForEnded();
  Destroy();
}

// Tests the case where underflow evicts all frames in the HAVE_ENOUGH state.
TEST_F(VideoRendererImplTest, UnderflowEvictionWhileHaveEnough) {
  Initialize();
  QueueFrames("0 30 60 90 100");

  {
    SCOPED_TRACE("Waiting for BUFFERING_HAVE_ENOUGH");
    WaitableMessageLoopEvent event;
    EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH))
        .WillOnce(RunClosure(event.GetClosure()));
    EXPECT_CALL(mock_cb_, FrameReceived(_)).Times(AnyNumber());
    EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
    EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
    EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
    StartPlayingFrom(0);
    event.RunAndWait();
  }

  // Now wait until we have no effective frames.
  {
    SCOPED_TRACE("Waiting for zero effective frames.");
    WaitableMessageLoopEvent event;
    null_video_sink_->set_background_render(true);
    time_source_.StartTicking();
    AdvanceTimeInMs(1000);
    renderer_->OnTimeProgressing();
    EXPECT_CALL(mock_cb_, FrameReceived(_))
        .WillOnce(RunClosure(event.GetClosure()));
    event.RunAndWait();
    ASSERT_EQ(renderer_->effective_frames_queued_for_testing(), 0u);
  }

  // When OnTimeStopped() is called it should transition to HAVE_NOTHING.
  {
    SCOPED_TRACE("Waiting for BUFFERING_HAVE_NOTHING");
    WaitableMessageLoopEvent event;
    EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_NOTHING))
        .WillOnce(RunClosure(event.GetClosure()));
    renderer_->OnTimeStopped();
    event.RunAndWait();
  }

  Destroy();
}

TEST_F(VideoRendererImplTest, StartPlayingFromThenFlushThenEOS) {
  Initialize();
  QueueFrames("0 30 60 90");

  WaitableMessageLoopEvent event;
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH))
      .WillOnce(RunClosure(event.GetClosure()));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
  StartPlayingFrom(0);
  event.RunAndWait();

  // Cycle ticking so that we get a non-null reference time.
  time_source_.StartTicking();
  time_source_.StopTicking();

  // Flush and simulate a seek past EOS, where some error prevents the decoder
  // from returning any frames.
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_NOTHING));
  Flush();

  StartPlayingFrom(200);
  WaitForPendingDecode();
  SatisfyPendingDecodeWithEndOfStream();
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH));
  WaitForEnded();
  Destroy();
}

TEST_F(VideoRendererImplTest, FramesAreNotExpiredDuringPreroll) {
  Initialize();
  // !CanReadWithoutStalling() puts the renderer in state BUFFERING_HAVE_ENOUGH
  // after the first frame.
  ON_CALL(*decoder_, CanReadWithoutStalling()).WillByDefault(Return(false));
  // Set background rendering to simulate the first couple of Render() calls
  // by VFC.
  null_video_sink_->set_background_render(true);
  QueueFrames("0 10 20");
  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH))
      .Times(testing::AtMost(1));
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(_)).Times(1);
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);
  StartPlayingFrom(0);

  renderer_->OnTimeProgressing();
  time_source_.StartTicking();

  WaitableMessageLoopEvent event;
  // Frame "10" should not have been expired.
  EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(10)))
      .WillOnce(RunClosure(event.GetClosure()));
  AdvanceTimeInMs(10);
  event.RunAndWait();

  Destroy();
}

TEST_F(VideoRendererImplTest, VideoConfigChange) {
  Initialize();

  // Configure demuxer stream to allow config changes.
  EXPECT_CALL(demuxer_stream_, SupportsConfigChanges())
      .WillRepeatedly(Return(true));

  // Signal a config change at the next DemuxerStream::Read().
  EXPECT_CALL(demuxer_stream_, Read(_))
      .WillOnce(RunCallback<0>(DemuxerStream::kConfigChanged, nullptr));

  // Use LargeEncrypted config (non-default) to ensure its plumbed through to
  // callback.
  demuxer_stream_.set_video_decoder_config(TestVideoConfig::LargeEncrypted());

  EXPECT_CALL(mock_cb_, OnVideoConfigChange(
                            DecoderConfigEq(TestVideoConfig::LargeEncrypted())))
      .Times(1);

  // Start plyaing to trigger DemuxerStream::Read(), surfacing the config change
  StartPlayingFrom(0);

  Destroy();
}

TEST_F(VideoRendererImplTest, NaturalSizeChange) {
  Initialize();

  gfx::Size initial_size(8, 8);
  gfx::Size larger_size(16, 16);

  QueueFrame(DecodeStatus::OK,
             VideoFrame::CreateFrame(PIXEL_FORMAT_I420, initial_size,
                                     gfx::Rect(initial_size), initial_size,
                                     base::TimeDelta::FromMilliseconds(0)));
  QueueFrame(DecodeStatus::OK,
             VideoFrame::CreateFrame(PIXEL_FORMAT_I420, larger_size,
                                     gfx::Rect(larger_size), larger_size,
                                     base::TimeDelta::FromMilliseconds(10)));
  QueueFrame(DecodeStatus::OK,
             VideoFrame::CreateFrame(PIXEL_FORMAT_I420, larger_size,
                                     gfx::Rect(larger_size), larger_size,
                                     base::TimeDelta::FromMilliseconds(20)));
  QueueFrame(DecodeStatus::OK,
             VideoFrame::CreateFrame(PIXEL_FORMAT_I420, initial_size,
                                     gfx::Rect(initial_size), initial_size,
                                     base::TimeDelta::FromMilliseconds(30)));

  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoOpacityChange(_)).Times(1);

  {
    // Callback is fired for the first frame.
    EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(initial_size));
    EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
    StartPlayingFrom(0);
    renderer_->OnTimeProgressing();
    time_source_.StartTicking();
  }
  {
    // Callback should be fired once when switching to the larger size.
    EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(larger_size));
    WaitableMessageLoopEvent event;
    EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(10)))
        .WillOnce(RunClosure(event.GetClosure()));
    AdvanceTimeInMs(10);
    event.RunAndWait();
  }
  {
    // Called is not fired because frame size does not change.
    WaitableMessageLoopEvent event;
    EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(20)))
        .WillOnce(RunClosure(event.GetClosure()));
    AdvanceTimeInMs(10);
    event.RunAndWait();
  }
  {
    // Callback is fired once when switching to the larger size.
    EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(initial_size));
    WaitableMessageLoopEvent event;
    EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(30)))
        .WillOnce(RunClosure(event.GetClosure()));
    AdvanceTimeInMs(10);
    event.RunAndWait();
  }

  Destroy();
}

TEST_F(VideoRendererImplTest, OpacityChange) {
  Initialize();

  gfx::Size frame_size(8, 8);
  VideoPixelFormat opaque_format = PIXEL_FORMAT_I420;
  VideoPixelFormat non_opaque_format = PIXEL_FORMAT_I420A;

  QueueFrame(DecodeStatus::OK,
             VideoFrame::CreateFrame(non_opaque_format, frame_size,
                                     gfx::Rect(frame_size), frame_size,
                                     base::TimeDelta::FromMilliseconds(0)));
  QueueFrame(DecodeStatus::OK,
             VideoFrame::CreateFrame(non_opaque_format, frame_size,
                                     gfx::Rect(frame_size), frame_size,
                                     base::TimeDelta::FromMilliseconds(10)));
  QueueFrame(DecodeStatus::OK,
             VideoFrame::CreateFrame(opaque_format, frame_size,
                                     gfx::Rect(frame_size), frame_size,
                                     base::TimeDelta::FromMilliseconds(20)));
  QueueFrame(DecodeStatus::OK,
             VideoFrame::CreateFrame(opaque_format, frame_size,
                                     gfx::Rect(frame_size), frame_size,
                                     base::TimeDelta::FromMilliseconds(30)));

  EXPECT_CALL(mock_cb_, OnBufferingStateChange(BUFFERING_HAVE_ENOUGH));
  EXPECT_CALL(mock_cb_, OnStatisticsUpdate(_)).Times(AnyNumber());
  EXPECT_CALL(mock_cb_, OnVideoNaturalSizeChange(frame_size)).Times(1);

  {
    // Callback is fired for the first frame.
    EXPECT_CALL(mock_cb_, OnVideoOpacityChange(false));
    EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(0)));
    StartPlayingFrom(0);
    renderer_->OnTimeProgressing();
    time_source_.StartTicking();
  }
  {
    // Callback is not fired because opacity does not change.
    WaitableMessageLoopEvent event;
    EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(10)))
        .WillOnce(RunClosure(event.GetClosure()));
    AdvanceTimeInMs(10);
    event.RunAndWait();
  }
  {
    // Called is fired when opacity changes.
    EXPECT_CALL(mock_cb_, OnVideoOpacityChange(true));
    WaitableMessageLoopEvent event;
    EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(20)))
        .WillOnce(RunClosure(event.GetClosure()));
    AdvanceTimeInMs(10);
    event.RunAndWait();
  }
  {
    // Callback is not fired because opacity does not change.
    WaitableMessageLoopEvent event;
    EXPECT_CALL(mock_cb_, FrameReceived(HasTimestampMatcher(30)))
        .WillOnce(RunClosure(event.GetClosure()));
    AdvanceTimeInMs(10);
    event.RunAndWait();
  }

  Destroy();
}

}  // namespace media
