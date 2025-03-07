/*
 * Copyright (C) 2007, 2008, 2009, 2010, 2011, 2012, 2013 Apple Inc. All rights
 * reserved.
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
 * THIS SOFTWARE IS PROVIDED BY APPLE COMPUTER, INC. ``AS IS'' AND ANY
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

#include "core/html/media/HTMLMediaElement.h"

#include <algorithm>
#include <limits>

#include "base/memory/ptr_util.h"
#include "bindings/core/v8/ExceptionState.h"
#include "bindings/core/v8/ScriptController.h"
#include "bindings/core/v8/ScriptEventListener.h"
#include "bindings/core/v8/ScriptPromiseResolver.h"
#include "core/CoreInitializer.h"
#include "core/css/MediaList.h"
#include "core/dom/Attribute.h"
#include "core/dom/DOMException.h"
#include "core/dom/ElementTraversal.h"
#include "core/dom/ShadowRoot.h"
#include "core/dom/events/Event.h"
#include "core/frame/LocalFrame.h"
#include "core/frame/LocalFrameClient.h"
#include "core/frame/LocalFrameView.h"
#include "core/frame/Settings.h"
#include "core/frame/UseCounter.h"
#include "core/frame/csp/ContentSecurityPolicy.h"
#include "core/fullscreen/Fullscreen.h"
#include "core/html/HTMLSourceElement.h"
#include "core/html/TimeRanges.h"
#include "core/html/media/AutoplayPolicy.h"
#include "core/html/media/HTMLMediaElementControlsList.h"
#include "core/html/media/HTMLMediaSource.h"
#include "core/html/media/MediaControls.h"
#include "core/html/media/MediaError.h"
#include "core/html/media/MediaFragmentURIParser.h"
#include "core/html/track/AudioTrack.h"
#include "core/html/track/AudioTrackList.h"
#include "core/html/track/AutomaticTrackSelection.h"
#include "core/html/track/CueTimeline.h"
#include "core/html/track/HTMLTrackElement.h"
#include "core/html/track/InbandTextTrack.h"
#include "core/html/track/TextTrackContainer.h"
#include "core/html/track/TextTrackList.h"
#include "core/html/track/VideoTrack.h"
#include "core/html/track/VideoTrackList.h"
#include "core/html_names.h"
#include "core/inspector/ConsoleMessage.h"
#include "core/layout/IntersectionGeometry.h"
#include "core/layout/LayoutMedia.h"
#include "core/layout/LayoutView.h"
#include "core/page/ChromeClient.h"
#include "core/page/Page.h"
#include "core/paint/compositing/PaintLayerCompositor.h"
#include "platform/Histogram.h"
#include "platform/LayoutTestSupport.h"
#include "platform/audio/AudioBus.h"
#include "platform/audio/AudioSourceProviderClient.h"
#include "platform/bindings/Microtask.h"
#include "platform/graphics/GraphicsLayer.h"
#include "platform/mediastream/MediaStreamDescriptor.h"
#include "platform/network/NetworkStateNotifier.h"
#include "platform/network/ParsedContentType.h"
#include "platform/network/mime/ContentType.h"
#include "platform/network/mime/MIMETypeFromURL.h"
#include "platform/runtime_enabled_features.h"
#include "platform/weborigin/SecurityOrigin.h"
#include "platform/wtf/AutoReset.h"
#include "platform/wtf/MathExtras.h"
#include "platform/wtf/Time.h"
#include "platform/wtf/text/CString.h"
#include "public/platform/Platform.h"
#include "public/platform/TaskType.h"
#include "public/platform/WebAudioSourceProvider.h"
#include "public/platform/WebContentDecryptionModule.h"
#include "public/platform/WebInbandTextTrack.h"
#include "public/platform/WebMediaPlayer.h"
#include "public/platform/WebMediaPlayerSource.h"
#include "public/platform/WebMediaStream.h"
#include "public/platform/WebScreenInfo.h"
#include "public/platform/modules/remoteplayback/WebRemotePlaybackAvailability.h"
#include "public/platform/modules/remoteplayback/WebRemotePlaybackClient.h"
#include "public/platform/modules/remoteplayback/WebRemotePlaybackState.h"

#ifndef BLINK_MEDIA_LOG
#define BLINK_MEDIA_LOG DVLOG(3)
#endif

#ifndef LOG_MEDIA_EVENTS
// Default to not logging events because so many are generated they can
// overwhelm the rest of the logging.
#define LOG_MEDIA_EVENTS 0
#endif

#ifndef LOG_OFFICIAL_TIME_STATUS
// Default to not logging status of official time because it adds a fair amount
// of overhead and logging.
#define LOG_OFFICIAL_TIME_STATUS 0
#endif

namespace blink {

using namespace HTMLNames;

using WeakMediaElementSet = HeapHashSet<WeakMember<HTMLMediaElement>>;
using DocumentElementSetMap =
    HeapHashMap<WeakMember<Document>, Member<WeakMediaElementSet>>;

namespace {

constexpr float kMostlyFillViewportThreshold = 0.85f;
constexpr TimeDelta kCheckViewportIntersectionInterval =
    TimeDelta::FromSeconds(1);

// This enum is used to record histograms. Do not reorder.
enum MediaControlsShow {
  kMediaControlsShowAttribute = 0,
  kMediaControlsShowFullscreen,
  kMediaControlsShowNoScript,
  kMediaControlsShowNotShown,
  kMediaControlsShowDisabledSettings,
  kMediaControlsShowMax
};

// These values are used for the Media.MediaElement.ContentTypeResult histogram.
// Do not reorder.
enum ContentTypeParseableResult {
  kIsSupportedParseable = 0,
  kMayBeSupportedParseable,
  kIsNotSupportedParseable,
  kIsSupportedNotParseable,
  kMayBeSupportedNotParseable,
  kIsNotSupportedNotParseable,
  kContentTypeParseableMax
};

// This enum is used to record histograms. Do not reorder.
enum class PlayPromiseRejectReason {
  kFailedAutoplayPolicy = 0,
  kNoSupportedSources,
  kInterruptedByPause,
  kInterruptedByLoad,
  kCount,
};

// Limits the range of media playback rate.
const double kMinRate = 0.0625;
const double kMaxRate = 16.0;

void ReportContentTypeResultToUMA(String content_type,
                                  MIMETypeRegistry::SupportsType result) {
  DEFINE_THREAD_SAFE_STATIC_LOCAL(
      EnumerationHistogram, content_type_parseable_histogram,
      ("Media.MediaElement.ContentTypeParseable", kContentTypeParseableMax));
  ParsedContentType parsed_content_type(content_type);
  ContentTypeParseableResult uma_result = kIsNotSupportedNotParseable;
  switch (result) {
    case MIMETypeRegistry::kIsSupported:
      uma_result = parsed_content_type.IsValid() ? kIsSupportedParseable
                                                 : kIsSupportedNotParseable;
      break;
    case MIMETypeRegistry::kMayBeSupported:
      uma_result = parsed_content_type.IsValid() ? kMayBeSupportedParseable
                                                 : kMayBeSupportedNotParseable;
      break;
    case MIMETypeRegistry::kIsNotSupported:
      uma_result = parsed_content_type.IsValid() ? kIsNotSupportedParseable
                                                 : kIsNotSupportedNotParseable;
      break;
  }
  content_type_parseable_histogram.Count(uma_result);
}

String UrlForLoggingMedia(const KURL& url) {
  static const unsigned kMaximumURLLengthForLogging = 128;

  if (url.GetString().length() < kMaximumURLLengthForLogging)
    return url.GetString();
  return url.GetString().Substring(0, kMaximumURLLengthForLogging) + "...";
}

const char* BoolString(bool val) {
  return val ? "true" : "false";
}

DocumentElementSetMap& DocumentToElementSetMap() {
  DEFINE_STATIC_LOCAL(DocumentElementSetMap, map, (new DocumentElementSetMap));
  return map;
}

void AddElementToDocumentMap(HTMLMediaElement* element, Document* document) {
  DocumentElementSetMap& map = DocumentToElementSetMap();
  WeakMediaElementSet* set = nullptr;
  auto it = map.find(document);
  if (it == map.end()) {
    set = new WeakMediaElementSet;
    map.insert(document, set);
  } else {
    set = it->value;
  }
  set->insert(element);
}

void RemoveElementFromDocumentMap(HTMLMediaElement* element,
                                  Document* document) {
  DocumentElementSetMap& map = DocumentToElementSetMap();
  auto it = map.find(document);
  DCHECK(it != map.end());
  WeakMediaElementSet* set = it->value;
  set->erase(element);
  if (set->IsEmpty())
    map.erase(it);
}

String BuildElementErrorMessage(const String& error) {
  // Prepend a UA-specific-error code before the first ':', to enable better
  // collection and aggregation of UA-specific-error codes from
  // MediaError.message by web apps. WebMediaPlayer::GetErrorMessage() should
  // similarly conform to this format.
  DEFINE_STATIC_LOCAL(const String, element_error_prefix,
                      ("MEDIA_ELEMENT_ERROR: "));
  StringBuilder builder;
  builder.Append(element_error_prefix);
  builder.Append(error);
  return builder.ToString();
}

class AudioSourceProviderClientLockScope {
  STACK_ALLOCATED();

 public:
  AudioSourceProviderClientLockScope(HTMLMediaElement& element)
      : client_(element.AudioSourceNode()) {
    if (client_)
      client_->lock();
  }
  ~AudioSourceProviderClientLockScope() {
    if (client_)
      client_->unlock();
  }

 private:
  Member<AudioSourceProviderClient> client_;
};

const AtomicString& AudioKindToString(
    WebMediaPlayerClient::AudioTrackKind kind) {
  switch (kind) {
    case WebMediaPlayerClient::kAudioTrackKindNone:
      return g_empty_atom;
    case WebMediaPlayerClient::kAudioTrackKindAlternative:
      return AudioTrack::AlternativeKeyword();
    case WebMediaPlayerClient::kAudioTrackKindDescriptions:
      return AudioTrack::DescriptionsKeyword();
    case WebMediaPlayerClient::kAudioTrackKindMain:
      return AudioTrack::MainKeyword();
    case WebMediaPlayerClient::kAudioTrackKindMainDescriptions:
      return AudioTrack::MainDescriptionsKeyword();
    case WebMediaPlayerClient::kAudioTrackKindTranslation:
      return AudioTrack::TranslationKeyword();
    case WebMediaPlayerClient::kAudioTrackKindCommentary:
      return AudioTrack::CommentaryKeyword();
  }

  NOTREACHED();
  return g_empty_atom;
}

const AtomicString& VideoKindToString(
    WebMediaPlayerClient::VideoTrackKind kind) {
  switch (kind) {
    case WebMediaPlayerClient::kVideoTrackKindNone:
      return g_empty_atom;
    case WebMediaPlayerClient::kVideoTrackKindAlternative:
      return VideoTrack::AlternativeKeyword();
    case WebMediaPlayerClient::kVideoTrackKindCaptions:
      return VideoTrack::CaptionsKeyword();
    case WebMediaPlayerClient::kVideoTrackKindMain:
      return VideoTrack::MainKeyword();
    case WebMediaPlayerClient::kVideoTrackKindSign:
      return VideoTrack::SignKeyword();
    case WebMediaPlayerClient::kVideoTrackKindSubtitles:
      return VideoTrack::SubtitlesKeyword();
    case WebMediaPlayerClient::kVideoTrackKindCommentary:
      return VideoTrack::CommentaryKeyword();
  }

  NOTREACHED();
  return g_empty_atom;
}

bool CanLoadURL(const KURL& url, const String& content_type_str) {
  DEFINE_STATIC_LOCAL(const String, codecs, ("codecs"));

  ContentType content_type(content_type_str);
  String content_mime_type = content_type.GetType().DeprecatedLower();
  String content_type_codecs = content_type.Parameter(codecs);

  // If the MIME type is missing or is not meaningful, try to figure it out from
  // the URL.
  if (content_mime_type.IsEmpty() ||
      content_mime_type == "application/octet-stream" ||
      content_mime_type == "text/plain") {
    if (url.ProtocolIsData())
      content_mime_type = MimeTypeFromDataURL(url.GetString());
  }

  // If no MIME type is specified, always attempt to load.
  if (content_mime_type.IsEmpty())
    return true;

  // 4.8.12.3 MIME types - In the absence of a specification to the contrary,
  // the MIME type "application/octet-stream" when used with parameters, e.g.
  // "application/octet-stream;codecs=theora", is a type that the user agent
  // knows it cannot render.
  if (content_mime_type != "application/octet-stream" ||
      content_type_codecs.IsEmpty()) {
    return MIMETypeRegistry::SupportsMediaMIMEType(content_mime_type,
                                                   content_type_codecs) !=
           MIMETypeRegistry::kIsNotSupported;
  }

  return false;
}

String PreloadTypeToString(WebMediaPlayer::Preload preload_type) {
  switch (preload_type) {
    case WebMediaPlayer::kPreloadNone:
      return "none";
    case WebMediaPlayer::kPreloadMetaData:
      return "metadata";
    case WebMediaPlayer::kPreloadAuto:
      return "auto";
  }

  NOTREACHED();
  return String();
}

bool IsDocumentCrossOrigin(Document& document) {
  const LocalFrame* frame = document.GetFrame();
  return frame && frame->IsCrossOriginSubframe();
}

void RecordPlayPromiseRejected(PlayPromiseRejectReason reason) {
  DEFINE_STATIC_LOCAL(EnumerationHistogram, histogram,
                      ("Media.MediaElement.PlayPromiseReject",
                       static_cast<int>(PlayPromiseRejectReason::kCount)));
  histogram.Count(static_cast<int>(reason));
}

}  // anonymous namespace

MIMETypeRegistry::SupportsType HTMLMediaElement::GetSupportsType(
    const ContentType& content_type) {
  DEFINE_STATIC_LOCAL(const String, codecs, ("codecs"));

  String type = content_type.GetType().DeprecatedLower();
  // The codecs string is not lower-cased because MP4 values are case sensitive
  // per http://tools.ietf.org/html/rfc4281#page-7.
  String type_codecs = content_type.Parameter(codecs);

  if (type.IsEmpty())
    return MIMETypeRegistry::kIsNotSupported;

  // 4.8.12.3 MIME types - The canPlayType(type) method must return the empty
  // string if type is a type that the user agent knows it cannot render or is
  // the type "application/octet-stream"
  if (type == "application/octet-stream")
    return MIMETypeRegistry::kIsNotSupported;

  // Check if stricter parsing of |contentType| will cause problems.
  // TODO(jrummell): Either switch to ParsedContentType or remove this UMA,
  // depending on the results reported.
  MIMETypeRegistry::SupportsType result =
      MIMETypeRegistry::SupportsMediaMIMEType(type, type_codecs);
  ReportContentTypeResultToUMA(content_type.Raw(), result);
  return result;
}

URLRegistry* HTMLMediaElement::media_stream_registry_ = nullptr;

void HTMLMediaElement::SetMediaStreamRegistry(URLRegistry* registry) {
  DCHECK(!media_stream_registry_);
  media_stream_registry_ = registry;
}

bool HTMLMediaElement::IsMediaStreamURL(const String& url) {
  return media_stream_registry_ ? media_stream_registry_->Contains(url) : false;
}

bool HTMLMediaElement::IsHLSURL(const KURL& url) {
  // Keep the same logic as in media_codec_util.h.
  if (url.IsNull() || url.IsEmpty())
    return false;

  if (!url.IsLocalFile() && !url.ProtocolIs("http") && !url.ProtocolIs("https"))
    return false;

  return url.GetString().Contains("m3u8");
}

bool HTMLMediaElement::MediaTracksEnabledInternally() {
  return RuntimeEnabledFeatures::AudioVideoTracksEnabled() ||
         RuntimeEnabledFeatures::BackgroundVideoTrackOptimizationEnabled();
}

// static
void HTMLMediaElement::OnMediaControlsEnabledChange(Document* document) {
  auto it = DocumentToElementSetMap().find(document);
  if (it == DocumentToElementSetMap().end())
    return;
  DCHECK(it->value);
  WeakMediaElementSet& elements = *it->value;
  for (const auto& element : elements) {
    element->UpdateControlsVisibility();
    if (element->GetMediaControls())
      element->GetMediaControls()->OnMediaControlsEnabledChange();
  }
}

HTMLMediaElement::HTMLMediaElement(const QualifiedName& tag_name,
                                   Document& document)
    : HTMLElement(tag_name, document),
      PausableObject(&document),
      load_timer_(document.GetTaskRunner(TaskType::kUnthrottled),
                  this,
                  &HTMLMediaElement::LoadTimerFired),
      progress_event_timer_(document.GetTaskRunner(TaskType::kUnthrottled),
                            this,
                            &HTMLMediaElement::ProgressEventTimerFired),
      playback_progress_timer_(document.GetTaskRunner(TaskType::kUnthrottled),
                               this,
                               &HTMLMediaElement::PlaybackProgressTimerFired),
      audio_tracks_timer_(document.GetTaskRunner(TaskType::kUnthrottled),
                          this,
                          &HTMLMediaElement::AudioTracksTimerFired),
      check_viewport_intersection_timer_(
          document.GetTaskRunner(TaskType::kUnthrottled),
          this,
          &HTMLMediaElement::CheckViewportIntersectionTimerFired),
      played_time_ranges_(),
      async_event_queue_(MediaElementEventQueue::Create(this, &document)),
      playback_rate_(1.0f),
      default_playback_rate_(1.0f),
      network_state_(kNetworkEmpty),
      ready_state_(kHaveNothing),
      ready_state_maximum_(kHaveNothing),
      volume_(1.0f),
      last_seek_time_(0),
      previous_progress_time_(std::numeric_limits<double>::max()),
      duration_(std::numeric_limits<double>::quiet_NaN()),
      last_time_update_event_media_time_(
          std::numeric_limits<double>::quiet_NaN()),
      default_playback_start_position_(0),
      load_state_(kWaitingForSource),
      deferred_load_state_(kNotDeferred),
      deferred_load_timer_(document.GetTaskRunner(TaskType::kUnthrottled),
                           this,
                           &HTMLMediaElement::DeferredLoadTimerFired),
      web_layer_(nullptr),
      display_mode_(kUnknown),
      official_playback_position_(0),
      official_playback_position_needs_update_(true),
      fragment_end_time_(std::numeric_limits<double>::quiet_NaN()),
      pending_action_flags_(0),
      playing_(false),
      should_delay_load_event_(false),
      have_fired_loaded_data_(false),
      can_autoplay_(true),
      muted_(false),
      paused_(true),
      seeking_(false),
      sent_stalled_event_(false),
      ignore_preload_none_(false),
      text_tracks_visible_(false),
      should_perform_automatic_track_selection_(true),
      tracks_are_ready_(true),
      processing_preference_change_(false),
      playing_remotely_(false),
      in_overlay_fullscreen_video_(false),
      mostly_filling_viewport_(false),
      audio_tracks_(AudioTrackList::Create(*this)),
      video_tracks_(VideoTrackList::Create(*this)),
      audio_source_node_(nullptr),
      autoplay_policy_(new AutoplayPolicy(this)),
      remote_playback_client_(nullptr),
      media_controls_(nullptr),
      controls_list_(HTMLMediaElementControlsList::Create(this)) {
  BLINK_MEDIA_LOG << "HTMLMediaElement(" << (void*)this << ")";

  LocalFrame* frame = document.GetFrame();
  if (frame) {
    remote_playback_client_ =
        frame->Client()->CreateWebRemotePlaybackClient(*this);
  }

  SetHasCustomStyleCallbacks();
  AddElementToDocumentMap(this, &document);

  UseCounter::Count(document, WebFeature::kHTMLMediaElement);
}

HTMLMediaElement::~HTMLMediaElement() {
  BLINK_MEDIA_LOG << "~HTMLMediaElement(" << (void*)this << ")";

  // audio_source_node_ is explicitly cleared by AudioNode::dispose().
  // Since AudioNode::dispose() is guaranteed to be always called before
  // the AudioNode is destructed, audio_source_node_ is explicitly cleared
  // even if the AudioNode and the HTMLMediaElement die together.
  DCHECK(!audio_source_node_);
}

void HTMLMediaElement::Dispose() {
  CloseMediaSource();

  // Destroying the player may cause a resource load to be canceled,
  // which could result in LocalDOMWindow::dispatchWindowLoadEvent() being
  // called via ResourceFetch::didLoadResource(), then
  // FrameLoader::checkCompleted(). But it's guaranteed that the load event
  // doesn't get dispatched during the object destruction.
  // See Document::isDelayingLoadEvent().
  // Also see http://crbug.com/275223 for more details.
  ClearMediaPlayerAndAudioSourceProviderClientWithoutLocking();
}

void HTMLMediaElement::DidMoveToNewDocument(Document& old_document) {
  BLINK_MEDIA_LOG << "didMoveToNewDocument(" << (void*)this << ")";

  load_timer_.MoveToNewTaskRunner(
      GetDocument().GetTaskRunner(TaskType::kUnthrottled));
  progress_event_timer_.MoveToNewTaskRunner(
      GetDocument().GetTaskRunner(TaskType::kUnthrottled));
  playback_progress_timer_.MoveToNewTaskRunner(
      GetDocument().GetTaskRunner(TaskType::kUnthrottled));
  audio_tracks_timer_.MoveToNewTaskRunner(
      GetDocument().GetTaskRunner(TaskType::kUnthrottled));
  check_viewport_intersection_timer_.MoveToNewTaskRunner(
      GetDocument().GetTaskRunner(TaskType::kUnthrottled));
  deferred_load_timer_.MoveToNewTaskRunner(
      GetDocument().GetTaskRunner(TaskType::kUnthrottled));

  autoplay_policy_->DidMoveToNewDocument(old_document);

  if (should_delay_load_event_) {
    GetDocument().IncrementLoadEventDelayCount();
    // Note: Keeping the load event delay count increment on oldDocument that
    // was added when should_delay_load_event_ was set so that destruction of
    // web_media_player_ can not cause load event dispatching in oldDocument.
  } else {
    // Incrementing the load event delay count so that destruction of
    // web_media_player_ can not cause load event dispatching in oldDocument.
    old_document.IncrementLoadEventDelayCount();
  }

  RemoveElementFromDocumentMap(this, &old_document);
  AddElementToDocumentMap(this, &GetDocument());

  // FIXME: This is a temporary fix to prevent this object from causing the
  // MediaPlayer to dereference LocalFrame and FrameLoader pointers from the
  // previous document. This restarts the load, as if the src attribute had been
  // set.  A proper fix would provide a mechanism to allow this object to
  // refresh the MediaPlayer's LocalFrame and FrameLoader references on document
  // changes so that playback can be resumed properly.
  ignore_preload_none_ = false;
  InvokeLoadAlgorithm();

  // Decrement the load event delay count on oldDocument now that
  // web_media_player_ has been destroyed and there is no risk of dispatching a
  // load event from within the destructor.
  old_document.DecrementLoadEventDelayCount();

  PausableObject::DidMoveToNewExecutionContext(&GetDocument());
  HTMLElement::DidMoveToNewDocument(old_document);
}

bool HTMLMediaElement::SupportsFocus() const {
  if (ownerDocument()->IsMediaDocument())
    return false;

  // If no controls specified, we should still be able to focus the element if
  // it has tabIndex.
  return ShouldShowControls() || HTMLElement::SupportsFocus();
}

bool HTMLMediaElement::IsMouseFocusable() const {
  return false;
}

void HTMLMediaElement::ParseAttribute(
    const AttributeModificationParams& params) {
  const QualifiedName& name = params.name;
  if (name == srcAttr) {
    BLINK_MEDIA_LOG << "parseAttribute(" << (void*)this
                    << ", srcAttr, old=" << params.old_value
                    << ", new=" << params.new_value << ")";
    // Trigger a reload, as long as the 'src' attribute is present.
    if (!params.new_value.IsNull()) {
      ignore_preload_none_ = false;
      InvokeLoadAlgorithm();
    }
  } else if (name == controlsAttr) {
    UseCounter::Count(GetDocument(),
                      WebFeature::kHTMLMediaElementControlsAttribute);
    UpdateControlsVisibility();
  } else if (name == controlslistAttr) {
    UseCounter::Count(GetDocument(),
                      WebFeature::kHTMLMediaElementControlsListAttribute);
    if (params.old_value != params.new_value) {
      controls_list_->DidUpdateAttributeValue(params.old_value,
                                              params.new_value);
      if (GetMediaControls())
        GetMediaControls()->OnControlsListUpdated();
    }
  } else if (name == preloadAttr) {
    SetPlayerPreload();
  } else if (name == disableremoteplaybackAttr) {
    // This attribute is an extension described in the Remote Playback API spec.
    // Please see: https://w3c.github.io/remote-playback
    UseCounter::Count(GetDocument(),
                      WebFeature::kDisableRemotePlaybackAttribute);
    if (params.old_value != params.new_value) {
      if (web_media_player_) {
        web_media_player_->RequestRemotePlaybackDisabled(
            !params.new_value.IsNull());
      }
    }
  } else {
    HTMLElement::ParseAttribute(params);
  }
}

void HTMLMediaElement::ParserDidSetAttributes() {
  HTMLElement::ParserDidSetAttributes();

  if (FastHasAttribute(mutedAttr))
    muted_ = true;
}

// This method is being used as a way to know that cloneNode finished cloning
// attribute as there is no callback notifying about the end of a cloning
// operation. Indeed, it is required per spec to set the muted state based on
// the content attribute when the object is created.
void HTMLMediaElement::CloneNonAttributePropertiesFrom(const Element& other,
                                                       CloneChildrenFlag flag) {
  HTMLElement::CloneNonAttributePropertiesFrom(other, flag);

  if (FastHasAttribute(mutedAttr))
    muted_ = true;
}

void HTMLMediaElement::FinishParsingChildren() {
  HTMLElement::FinishParsingChildren();

  if (Traversal<HTMLTrackElement>::FirstChild(*this))
    ScheduleTextTrackResourceLoad();
}

bool HTMLMediaElement::LayoutObjectIsNeeded(const ComputedStyle& style) {
  return ShouldShowControls() && HTMLElement::LayoutObjectIsNeeded(style);
}

LayoutObject* HTMLMediaElement::CreateLayoutObject(const ComputedStyle&) {
  return new LayoutMedia(this);
}

Node::InsertionNotificationRequest HTMLMediaElement::InsertedInto(
    ContainerNode* insertion_point) {
  BLINK_MEDIA_LOG << "insertedInto(" << (void*)this << ", " << insertion_point
                  << ")";

  HTMLElement::InsertedInto(insertion_point);
  if (insertion_point->isConnected()) {
    UseCounter::Count(GetDocument(), WebFeature::kHTMLMediaElementInDocument);
    if ((!getAttribute(srcAttr).IsEmpty() || src_object_) &&
        network_state_ == kNetworkEmpty) {
      ignore_preload_none_ = false;
      InvokeLoadAlgorithm();
    }
  }

  return kInsertionShouldCallDidNotifySubtreeInsertions;
}

void HTMLMediaElement::DidNotifySubtreeInsertionsToDocument() {
  UpdateControlsVisibility();
}

void HTMLMediaElement::RemovedFrom(ContainerNode* insertion_point) {
  BLINK_MEDIA_LOG << "removedFrom(" << (void*)this << ", " << insertion_point
                  << ")";

  HTMLElement::RemovedFrom(insertion_point);
  if (insertion_point->InActiveDocument()) {
    UpdateControlsVisibility();
    if (network_state_ > kNetworkEmpty)
      PauseInternal();
  }
}

void HTMLMediaElement::AttachLayoutTree(AttachContext& context) {
  HTMLElement::AttachLayoutTree(context);

  if (GetLayoutObject())
    GetLayoutObject()->UpdateFromElement();
}

void HTMLMediaElement::DidRecalcStyle(StyleRecalcChange) {
  if (GetLayoutObject())
    GetLayoutObject()->UpdateFromElement();
}

void HTMLMediaElement::ScheduleTextTrackResourceLoad() {
  BLINK_MEDIA_LOG << "scheduleTextTrackResourceLoad(" << (void*)this << ")";

  pending_action_flags_ |= kLoadTextTrackResource;

  if (!load_timer_.IsActive())
    load_timer_.StartOneShot(TimeDelta(), FROM_HERE);
}

void HTMLMediaElement::ScheduleNextSourceChild() {
  // Schedule the timer to try the next <source> element WITHOUT resetting state
  // ala invokeLoadAlgorithm.
  pending_action_flags_ |= kLoadMediaResource;
  load_timer_.StartOneShot(TimeDelta(), FROM_HERE);
}

void HTMLMediaElement::ScheduleEvent(const AtomicString& event_name) {
  ScheduleEvent(Event::CreateCancelable(event_name));
}

void HTMLMediaElement::ScheduleEvent(Event* event) {
#if LOG_MEDIA_EVENTS
  BLINK_MEDIA_LOG << "ScheduleEvent(" << (void*)this << ")"
                  << " - scheduling '" << event->type() << "'";
#endif
  async_event_queue_->EnqueueEvent(FROM_HERE, event);
}

void HTMLMediaElement::LoadTimerFired(TimerBase*) {
  if (pending_action_flags_ & kLoadTextTrackResource)
    HonorUserPreferencesForAutomaticTextTrackSelection();

  if (pending_action_flags_ & kLoadMediaResource) {
    if (load_state_ == kLoadingFromSourceElement)
      LoadNextSourceChild();
    else
      LoadInternal();
  }

  pending_action_flags_ = 0;
}

MediaError* HTMLMediaElement::error() const {
  return error_;
}

void HTMLMediaElement::SetSrc(const AtomicString& url) {
  setAttribute(srcAttr, url);
}

void HTMLMediaElement::SetSrcObject(MediaStreamDescriptor* src_object) {
  BLINK_MEDIA_LOG << "setSrcObject(" << (void*)this << ")";
  src_object_ = src_object;
  InvokeLoadAlgorithm();
}

HTMLMediaElement::NetworkState HTMLMediaElement::getNetworkState() const {
  return network_state_;
}

String HTMLMediaElement::canPlayType(const String& mime_type) const {
  MIMETypeRegistry::SupportsType support =
      GetSupportsType(ContentType(mime_type));
  String can_play;

  // 4.8.12.3
  switch (support) {
    case MIMETypeRegistry::kIsNotSupported:
      can_play = g_empty_string;
      break;
    case MIMETypeRegistry::kMayBeSupported:
      can_play = "maybe";
      break;
    case MIMETypeRegistry::kIsSupported:
      can_play = "probably";
      break;
  }

  BLINK_MEDIA_LOG << "canPlayType(" << (void*)this << ", " << mime_type
                  << ") -> " << can_play;

  return can_play;
}

void HTMLMediaElement::load() {
  BLINK_MEDIA_LOG << "load(" << (void*)this << ")";

  autoplay_policy_->TryUnlockingUserGesture();

  ignore_preload_none_ = true;
  InvokeLoadAlgorithm();
}

// TODO(srirama.m): Currently ignore_preload_none_ is reset before calling
// invokeLoadAlgorithm() in all places except load(). Move it inside here
// once microtask is implemented for "Await a stable state" step
// in resource selection algorithm.
void HTMLMediaElement::InvokeLoadAlgorithm() {
  BLINK_MEDIA_LOG << "invokeLoadAlgorithm(" << (void*)this << ")";

  // Perform the cleanup required for the resource load algorithm to run.
  StopPeriodicTimers();
  load_timer_.Stop();
  CancelDeferredLoad();
  // FIXME: Figure out appropriate place to reset LoadTextTrackResource if
  // necessary and set pending_action_flags_ to 0 here.
  pending_action_flags_ &= ~kLoadMediaResource;
  sent_stalled_event_ = false;
  have_fired_loaded_data_ = false;
  display_mode_ = kUnknown;

  autoplay_policy_->StopAutoplayMutedWhenVisible();

  // 1 - Abort any already-running instance of the resource selection algorithm
  // for this element.
  load_state_ = kWaitingForSource;
  current_source_node_ = nullptr;

  // 2 - Let pending tasks be a list of tasks from the media element's media
  // element task source in one of the task queues.
  //
  // 3 - For each task in the pending tasks that would run resolve pending
  // play promises or project pending play prmoises algorithms, immediately
  // resolve or reject those promises in the order the corresponding tasks
  // were queued.
  //
  // TODO(mlamouri): the promises are first resolved then rejected but the
  // order between resolved/rejected promises isn't respected. This could be
  // improved when the same task is used for both cases.
  //
  // TODO(mlamouri): don't run the callback synchronously if we are not allowed
  // to run scripts. It can happen in some edge cases. https://crbug.com/660382
  if (play_promise_resolve_task_handle_.IsActive() &&
      !ScriptForbiddenScope::IsScriptForbidden()) {
    play_promise_resolve_task_handle_.Cancel();
    ResolveScheduledPlayPromises();
  }
  if (play_promise_reject_task_handle_.IsActive() &&
      !ScriptForbiddenScope::IsScriptForbidden()) {
    play_promise_reject_task_handle_.Cancel();
    RejectScheduledPlayPromises();
  }

  // 4 - Remove each task in pending tasks from its task queue.
  CancelPendingEventsAndCallbacks();

  // 5 - If the media element's networkState is set to NETWORK_LOADING or
  // NETWORK_IDLE, queue a task to fire a simple event named abort at the media
  // element.
  if (network_state_ == kNetworkLoading || network_state_ == kNetworkIdle)
    ScheduleEvent(EventTypeNames::abort);

  ResetMediaPlayerAndMediaSource();

  // 6 - If the media element's networkState is not set to NETWORK_EMPTY, then
  // run these substeps
  if (network_state_ != kNetworkEmpty) {
    // 4.1 - Queue a task to fire a simple event named emptied at the media
    // element.
    ScheduleEvent(EventTypeNames::emptied);

    // 4.2 - If a fetching process is in progress for the media element, the
    // user agent should stop it.
    SetNetworkState(kNetworkEmpty);

    // 4.4 - Forget the media element's media-resource-specific tracks.
    ForgetResourceSpecificTracks();

    // 4.5 - If readyState is not set to kHaveNothing, then set it to that
    // state.
    ready_state_ = kHaveNothing;
    ready_state_maximum_ = kHaveNothing;

    DCHECK(!paused_ || play_promise_resolvers_.IsEmpty());

    // 4.6 - If the paused attribute is false, then run these substeps
    if (!paused_) {
      // 4.6.1 - Set the paused attribute to true.
      paused_ = true;

      // 4.6.2 - Take pending play promises and reject pending play promises
      // with the result and an "AbortError" DOMException.
      RecordPlayPromiseRejected(PlayPromiseRejectReason::kInterruptedByLoad);
      RejectPlayPromises(kAbortError,
                         "The play() request was interrupted by a new load "
                         "request. https://goo.gl/LdLk22");
    }

    // 4.7 - If seeking is true, set it to false.
    seeking_ = false;

    // 4.8 - Set the current playback position to 0.
    //       Set the official playback position to 0.
    //       If this changed the official playback position, then queue a task
    //       to fire a simple event named timeupdate at the media element.
    // 4.9 - Set the initial playback position to 0.
    SetOfficialPlaybackPosition(0);
    ScheduleTimeupdateEvent(false);

    // 4.10 - Set the timeline offset to Not-a-Number (NaN).
    // 4.11 - Update the duration attribute to Not-a-Number (NaN).

    GetCueTimeline().UpdateActiveCues(0);
  } else if (!paused_) {
    // TODO(foolip): There is a proposal to always reset the paused state
    // in the media element load algorithm, to avoid a bogus play() promise
    // rejection: https://github.com/whatwg/html/issues/869
    // This is where that change would have an effect, and it is measured to
    // verify the assumption that it's a very rare situation.
    UseCounter::Count(GetDocument(),
                      WebFeature::kHTMLMediaElementLoadNetworkEmptyNotPaused);
  }

  // 7 - Set the playbackRate attribute to the value of the defaultPlaybackRate
  // attribute.
  setPlaybackRate(defaultPlaybackRate());

  // 8 - Set the error attribute to null and the can autoplay flag to true.
  error_ = nullptr;
  can_autoplay_ = true;

  // 9 - Invoke the media element's resource selection algorithm.
  InvokeResourceSelectionAlgorithm();

  // 10 - Note: Playback of any previously playing media resource for this
  // element stops.
}

void HTMLMediaElement::InvokeResourceSelectionAlgorithm() {
  BLINK_MEDIA_LOG << "invokeResourceSelectionAlgorithm(" << (void*)this << ")";
  // The resource selection algorithm
  // 1 - Set the networkState to NETWORK_NO_SOURCE
  SetNetworkState(kNetworkNoSource);

  // 2 - Set the element's show poster flag to true
  // TODO(srirama.m): Introduce show poster flag and update it as per spec

  played_time_ranges_ = TimeRanges::Create();

  // FIXME: Investigate whether these can be moved into network_state_ !=
  // kNetworkEmpty block above
  // so they are closer to the relevant spec steps.
  last_seek_time_ = 0;
  duration_ = std::numeric_limits<double>::quiet_NaN();

  // 3 - Set the media element's delaying-the-load-event flag to true (this
  // delays the load event)
  SetShouldDelayLoadEvent(true);
  if (GetMediaControls())
    GetMediaControls()->Reset();

  // 4 - Await a stable state, allowing the task that invoked this algorithm to
  // continue
  // TODO(srirama.m): Remove scheduleNextSourceChild() and post a microtask
  // instead.  See http://crbug.com/593289 for more details.
  ScheduleNextSourceChild();
}

void HTMLMediaElement::LoadInternal() {
  // HTMLMediaElement::textTracksAreReady will need "... the text tracks whose
  // mode was not in the disabled state when the element's resource selection
  // algorithm last started".
  text_tracks_when_resource_selection_began_.clear();
  if (text_tracks_) {
    for (unsigned i = 0; i < text_tracks_->length(); ++i) {
      TextTrack* track = text_tracks_->AnonymousIndexedGetter(i);
      if (track->mode() != TextTrack::DisabledKeyword())
        text_tracks_when_resource_selection_began_.push_back(track);
    }
  }

  SelectMediaResource();
}

void HTMLMediaElement::SelectMediaResource() {
  BLINK_MEDIA_LOG << "selectMediaResource(" << (void*)this << ")";

  enum Mode { kObject, kAttribute, kChildren, kNothing };
  Mode mode = kNothing;

  // 6 - If the media element has an assigned media provider object, then let
  //     mode be object.
  if (src_object_) {
    mode = kObject;
  } else if (FastHasAttribute(srcAttr)) {
    // Otherwise, if the media element has no assigned media provider object
    // but has a src attribute, then let mode be attribute.
    mode = kAttribute;
  } else if (HTMLSourceElement* element =
                 Traversal<HTMLSourceElement>::FirstChild(*this)) {
    // Otherwise, if the media element does not have an assigned media
    // provider object and does not have a src attribute, but does have a
    // source element child, then let mode be children and let candidate be
    // the first such source element child in tree order.
    mode = kChildren;
    next_child_node_to_consider_ = element;
    current_source_node_ = nullptr;
  } else {
    // Otherwise the media element has no assigned media provider object and
    // has neither a src attribute nor a source element child: set the
    // networkState to kNetworkEmpty, and abort these steps; the synchronous
    // section ends.
    // TODO(mlamouri): Setting the network state to empty implies that there
    // should be no |web_media_player_|. However, if a previous playback ended
    // due to an error, we can get here and still have one. Decide on a plan
    // to deal with this properly. https://crbug.com/789737
    load_state_ = kWaitingForSource;
    SetShouldDelayLoadEvent(false);
    if (!GetWebMediaPlayer() || (ready_state_ < kHaveFutureData &&
                                 ready_state_maximum_ < kHaveFutureData)) {
      SetNetworkState(kNetworkEmpty);
    } else {
      UseCounter::Count(GetDocument(),
                        WebFeature::kHTMLMediaElementEmptyLoadWithFutureData);
    }
    UpdateDisplayState();

    BLINK_MEDIA_LOG << "selectMediaResource(" << (void*)this
                    << "), nothing to load";
    return;
  }

  // 7 - Set the media element's networkState to NETWORK_LOADING.
  SetNetworkState(kNetworkLoading);

  // 8 - Queue a task to fire a simple event named loadstart at the media
  // element.
  ScheduleEvent(EventTypeNames::loadstart);

  // 9 - Run the appropriate steps...
  switch (mode) {
    case kObject:
      LoadSourceFromObject();
      BLINK_MEDIA_LOG << "selectMediaResource(" << (void*)this
                      << ", using 'srcObject' attribute";
      break;
    case kAttribute:
      LoadSourceFromAttribute();
      BLINK_MEDIA_LOG << "selectMediaResource(" << (void*)this
                      << "), using 'src' attribute url";
      break;
    case kChildren:
      LoadNextSourceChild();
      BLINK_MEDIA_LOG << "selectMediaResource(" << (void*)this
                      << "), using source element";
      break;
    default:
      NOTREACHED();
  }
}

void HTMLMediaElement::LoadSourceFromObject() {
  DCHECK(src_object_);
  load_state_ = kLoadingFromSrcObject;

  // No type is available when the resource comes from the 'srcObject'
  // attribute.
  LoadResource(WebMediaPlayerSource(WebMediaStream(src_object_)), String());
}

void HTMLMediaElement::LoadSourceFromAttribute() {
  load_state_ = kLoadingFromSrcAttr;
  const AtomicString& src_value = FastGetAttribute(srcAttr);

  // If the src attribute's value is the empty string ... jump down to the
  // failed step below
  if (src_value.IsEmpty()) {
    BLINK_MEDIA_LOG << "LoadSourceFromAttribute(" << (void*)this
                    << "), empty 'src'";
    MediaLoadingFailed(WebMediaPlayer::kNetworkStateFormatError,
                       BuildElementErrorMessage("Empty src attribute"));
    return;
  }

  KURL media_url = GetDocument().CompleteURL(src_value);
  if (!IsSafeToLoadURL(media_url, kComplain)) {
    MediaLoadingFailed(
        WebMediaPlayer::kNetworkStateFormatError,
        BuildElementErrorMessage("Media load rejected by URL safety check"));
    return;
  }

  // No type is available when the url comes from the 'src' attribute so
  // MediaPlayer will have to pick a media engine based on the file extension.
  LoadResource(WebMediaPlayerSource(WebURL(media_url)), String());
}

void HTMLMediaElement::LoadNextSourceChild() {
  String content_type;
  KURL media_url = SelectNextSourceChild(&content_type, kComplain);
  if (!media_url.IsValid()) {
    WaitForSourceChange();
    return;
  }

  // Reset the MediaPlayer and MediaSource if any
  ResetMediaPlayerAndMediaSource();

  load_state_ = kLoadingFromSourceElement;
  LoadResource(WebMediaPlayerSource(WebURL(media_url)), content_type);
}

void HTMLMediaElement::LoadResource(const WebMediaPlayerSource& source,
                                    const String& content_type) {
  DCHECK(IsMainThread());
  KURL url;
  if (source.IsURL()) {
    url = source.GetAsURL();
    DCHECK(IsSafeToLoadURL(url, kComplain));
    BLINK_MEDIA_LOG << "loadResource(" << (void*)this << ", "
                    << UrlForLoggingMedia(url) << ", " << content_type << ")";
  }

  LocalFrame* frame = GetDocument().GetFrame();
  if (!frame) {
    MediaLoadingFailed(WebMediaPlayer::kNetworkStateFormatError,
                       BuildElementErrorMessage(
                           "Resource load failure: document has no frame"));
    return;
  }

  // The resource fetch algorithm
  SetNetworkState(kNetworkLoading);

  // Set current_src_ *before* changing to the cache url, the fact that we are
  // loading from the app cache is an internal detail not exposed through the
  // media element API.
  current_src_ = url;

  if (audio_source_node_)
    audio_source_node_->OnCurrentSrcChanged(current_src_);

  // Update remote playback client with the new src and consider it incompatible
  // until proved otherwise.
  RemotePlaybackCompatibilityChanged(current_src_, false);

  BLINK_MEDIA_LOG << "loadResource(" << (void*)this << ") - current_src_ -> "
                  << UrlForLoggingMedia(current_src_);

  StartProgressEventTimer();

  // Reset display mode to force a recalculation of what to show because we are
  // resetting the player.
  SetDisplayMode(kUnknown);

  SetPlayerPreload();

  DCHECK(!media_source_);

  bool attempt_load = true;

  media_source_ = HTMLMediaSource::Lookup(url.GetString());
  if (media_source_ && !media_source_->AttachToElement(this)) {
    // Forget our reference to the MediaSource, so we leave it alone
    // while processing remainder of load failure.
    media_source_ = nullptr;
    attempt_load = false;
  }

  bool can_load_resource =
      source.IsMediaStream() || CanLoadURL(url, content_type);
  if (attempt_load && can_load_resource) {
    DCHECK(!GetWebMediaPlayer());

    // Conditionally defer the load if effective preload is 'none'.
    // Skip this optional deferral for MediaStream sources or any blob URL,
    // including MediaSource blob URLs.
    if (!source.IsMediaStream() && !url.ProtocolIs("blob") &&
        EffectivePreloadType() == WebMediaPlayer::kPreloadNone) {
      BLINK_MEDIA_LOG << "loadResource(" << (void*)this
                      << ") : Delaying load because preload == 'none'";
      DeferLoad();
    } else {
      StartPlayerLoad();
    }
  } else {
    MediaLoadingFailed(
        WebMediaPlayer::kNetworkStateFormatError,
        BuildElementErrorMessage(attempt_load
                                     ? "Unable to load URL due to content type"
                                     : "Unable to attach MediaSource"));
  }

  // If there is no poster to display, allow the media engine to render video
  // frames as soon as they are available.
  UpdateDisplayState();

  if (GetLayoutObject())
    GetLayoutObject()->UpdateFromElement();
}

void HTMLMediaElement::StartPlayerLoad() {
  DCHECK(!web_media_player_);

  WebMediaPlayerSource source;
  if (src_object_) {
    source = WebMediaPlayerSource(WebMediaStream(src_object_));
  } else {
    // Filter out user:pass as those two URL components aren't
    // considered for media resource fetches (including for the CORS
    // use-credentials mode.) That behavior aligns with Gecko, with IE
    // being more restrictive and not allowing fetches to such URLs.
    //
    // Spec reference: http://whatwg.org/c/#concept-media-load-resource
    //
    // FIXME: when the HTML spec switches to specifying resource
    // fetches in terms of Fetch (http://fetch.spec.whatwg.org), and
    // along with that potentially also specifying a setting for its
    // 'authentication flag' to control how user:pass embedded in a
    // media resource URL should be treated, then update the handling
    // here to match.
    KURL request_url = current_src_;
    if (!request_url.User().IsEmpty())
      request_url.SetUser(String());
    if (!request_url.Pass().IsEmpty())
      request_url.SetPass(String());

    KURL kurl(request_url);
    source = WebMediaPlayerSource(WebURL(kurl));
  }

  LocalFrame* frame = GetDocument().GetFrame();
  // TODO(srirama.m): Figure out how frame can be null when
  // coming from executeDeferredLoad()
  if (!frame) {
    MediaLoadingFailed(
        WebMediaPlayer::kNetworkStateFormatError,
        BuildElementErrorMessage("Player load failure: document has no frame"));
    return;
  }

  web_media_player_ = frame->Client()->CreateWebMediaPlayer(
      *this, source, this,
      frame->GetPage()->GetChromeClient().GetWebLayerTreeView(frame));

  if (!web_media_player_) {
    MediaLoadingFailed(WebMediaPlayer::kNetworkStateFormatError,
                       BuildElementErrorMessage(
                           "Player load failure: error creating media player"));
    return;
  }

  if (GetLayoutObject())
    GetLayoutObject()->SetShouldDoFullPaintInvalidation();
  // Make sure if we create/re-create the WebMediaPlayer that we update our
  // wrapper.
  audio_source_provider_.Wrap(web_media_player_->GetAudioSourceProvider());
  web_media_player_->SetVolume(EffectiveMediaVolume());

  web_media_player_->SetPoster(PosterImageURL());

  web_media_player_->SetPreload(EffectivePreloadType());

  web_media_player_->RequestRemotePlaybackDisabled(
      FastHasAttribute(disableremoteplaybackAttr));

  web_media_player_->Load(GetLoadType(), source, CorsMode());

  if (IsFullscreen())
    web_media_player_->EnteredFullscreen();

  web_media_player_->BecameDominantVisibleContent(mostly_filling_viewport_);
}

void HTMLMediaElement::SetPlayerPreload() {
  if (web_media_player_)
    web_media_player_->SetPreload(EffectivePreloadType());

  if (LoadIsDeferred() &&
      EffectivePreloadType() != WebMediaPlayer::kPreloadNone)
    StartDeferredLoad();
}

bool HTMLMediaElement::LoadIsDeferred() const {
  return deferred_load_state_ != kNotDeferred;
}

void HTMLMediaElement::DeferLoad() {
  // This implements the "optional" step 4 from the resource fetch algorithm
  // "If mode is remote".
  DCHECK(!deferred_load_timer_.IsActive());
  DCHECK_EQ(deferred_load_state_, kNotDeferred);
  // 1. Set the networkState to NETWORK_IDLE.
  // 2. Queue a task to fire a simple event named suspend at the element.
  ChangeNetworkStateFromLoadingToIdle();
  // 3. Queue a task to set the element's delaying-the-load-event
  // flag to false. This stops delaying the load event.
  deferred_load_timer_.StartOneShot(TimeDelta(), FROM_HERE);
  // 4. Wait for the task to be run.
  deferred_load_state_ = kWaitingForStopDelayingLoadEventTask;
  // Continued in executeDeferredLoad().
}

void HTMLMediaElement::CancelDeferredLoad() {
  deferred_load_timer_.Stop();
  deferred_load_state_ = kNotDeferred;
}

void HTMLMediaElement::ExecuteDeferredLoad() {
  DCHECK_GE(deferred_load_state_, kWaitingForTrigger);

  // resource fetch algorithm step 4 - continued from deferLoad().

  // 5. Wait for an implementation-defined event (e.g. the user requesting that
  // the media element begin playback).  This is assumed to be whatever 'event'
  // ended up calling this method.
  CancelDeferredLoad();
  // 6. Set the element's delaying-the-load-event flag back to true (this
  // delays the load event again, in case it hasn't been fired yet).
  SetShouldDelayLoadEvent(true);
  // 7. Set the networkState to NETWORK_LOADING.
  SetNetworkState(kNetworkLoading);

  StartProgressEventTimer();

  StartPlayerLoad();
}

void HTMLMediaElement::StartDeferredLoad() {
  if (deferred_load_state_ == kWaitingForTrigger) {
    ExecuteDeferredLoad();
    return;
  }
  if (deferred_load_state_ == kExecuteOnStopDelayingLoadEventTask)
    return;
  DCHECK_EQ(deferred_load_state_, kWaitingForStopDelayingLoadEventTask);
  deferred_load_state_ = kExecuteOnStopDelayingLoadEventTask;
}

void HTMLMediaElement::DeferredLoadTimerFired(TimerBase*) {
  SetShouldDelayLoadEvent(false);

  if (deferred_load_state_ == kExecuteOnStopDelayingLoadEventTask) {
    ExecuteDeferredLoad();
    return;
  }
  DCHECK_EQ(deferred_load_state_, kWaitingForStopDelayingLoadEventTask);
  deferred_load_state_ = kWaitingForTrigger;
}

WebMediaPlayer::LoadType HTMLMediaElement::GetLoadType() const {
  if (media_source_)
    return WebMediaPlayer::kLoadTypeMediaSource;

  if (src_object_ ||
      (!current_src_.IsNull() && IsMediaStreamURL(current_src_.GetString())))
    return WebMediaPlayer::kLoadTypeMediaStream;

  return WebMediaPlayer::kLoadTypeURL;
}

bool HTMLMediaElement::TextTracksAreReady() const {
  // 4.8.12.11.1 Text track model
  // ...
  // The text tracks of a media element are ready if all the text tracks whose
  // mode was not in the disabled state when the element's resource selection
  // algorithm last started now have a text track readiness state of loaded or
  // failed to load.
  for (const auto& text_track : text_tracks_when_resource_selection_began_) {
    if (text_track->GetReadinessState() == TextTrack::kLoading ||
        text_track->GetReadinessState() == TextTrack::kNotLoaded)
      return false;
  }

  return true;
}

void HTMLMediaElement::TextTrackReadyStateChanged(TextTrack* track) {
  if (GetWebMediaPlayer() &&
      text_tracks_when_resource_selection_began_.Contains(track)) {
    if (track->GetReadinessState() != TextTrack::kLoading) {
      SetReadyState(
          static_cast<ReadyState>(GetWebMediaPlayer()->GetReadyState()));
    }
  } else {
    // The track readiness state might have changed as a result of the user
    // clicking the captions button. In this case, a check whether all the
    // resources have failed loading should be done in order to hide the CC
    // button.
    // TODO(mlamouri): when an HTMLTrackElement fails to load, it is not
    // propagated to the TextTrack object in a web exposed fashion. We have to
    // keep relying on a custom glue to the controls while this is taken care
    // of on the web side. See https://crbug.com/669977
    if (GetMediaControls() &&
        track->GetReadinessState() == TextTrack::kFailedToLoad) {
      GetMediaControls()->OnTrackElementFailedToLoad();
    }
  }
}

void HTMLMediaElement::TextTrackModeChanged(TextTrack* track) {
  // Mark this track as "configured" so configureTextTracks won't change the
  // mode again.
  if (track->TrackType() == TextTrack::kTrackElement)
    track->SetHasBeenConfigured(true);

  ConfigureTextTrackDisplay();

  DCHECK(textTracks()->Contains(track));
  textTracks()->ScheduleChangeEvent();
}

void HTMLMediaElement::DisableAutomaticTextTrackSelection() {
  should_perform_automatic_track_selection_ = false;
}

bool HTMLMediaElement::IsSafeToLoadURL(const KURL& url,
                                       InvalidURLAction action_if_invalid) {
  if (!url.IsValid()) {
    BLINK_MEDIA_LOG << "isSafeToLoadURL(" << (void*)this << ", "
                    << UrlForLoggingMedia(url)
                    << ") -> FALSE because url is invalid";
    return false;
  }

  LocalFrame* frame = GetDocument().GetFrame();
  if (!frame || !GetDocument().GetSecurityOrigin()->CanDisplay(url)) {
    if (action_if_invalid == kComplain) {
      GetDocument().AddConsoleMessage(ConsoleMessage::Create(
          kSecurityMessageSource, kErrorMessageLevel,
          "Not allowed to load local resource: " + url.ElidedString()));
    }
    BLINK_MEDIA_LOG << "isSafeToLoadURL(" << (void*)this << ", "
                    << UrlForLoggingMedia(url)
                    << ") -> FALSE rejected by SecurityOrigin";
    return false;
  }

  if (!GetDocument().GetContentSecurityPolicy()->AllowMediaFromSource(url)) {
    BLINK_MEDIA_LOG << "isSafeToLoadURL(" << (void*)this << ", "
                    << UrlForLoggingMedia(url)
                    << ") -> rejected by Content Security Policy";
    return false;
  }

  return true;
}

bool HTMLMediaElement::IsMediaDataCORSSameOrigin(
    const SecurityOrigin* origin) const {
  // If a service worker handled the request, we don't know if the origin in the
  // src is the same as the actual response URL so can't rely on URL checks
  // alone. So detect an opaque response via
  // DidGetOpaqueResponseFromServiceWorker().
  if (GetWebMediaPlayer() &&
      GetWebMediaPlayer()->DidGetOpaqueResponseFromServiceWorker()) {
    return false;
  }

  // At this point, either a service worker was not used, or it didn't provide
  // an opaque response, so continue with the normal checks.

  // HasSingleSecurityOrigin() tells us whether the origin in the src
  // is the same as the actual request (i.e. after redirects).
  if (!HasSingleSecurityOrigin())
    return false;

  // DidPassCORSAccessCheck() means it was a successful CORS-enabled fetch (vs.
  // non-CORS-enabled or failed). CanReadContent() does CheckAccess() on the
  // URL plus allows data sources, to ensure that it is not a URL that requires
  // CORS (basically same origin).
  return (GetWebMediaPlayer() &&
          GetWebMediaPlayer()->DidPassCORSAccessCheck()) ||
         origin->CanReadContent(currentSrc());
}

bool HTMLMediaElement::IsInCrossOriginFrame() const {
  return IsDocumentCrossOrigin(GetDocument());
}

void HTMLMediaElement::StartProgressEventTimer() {
  if (progress_event_timer_.IsActive())
    return;

  previous_progress_time_ = WTF::CurrentTime();
  // 350ms is not magic, it is in the spec!
  progress_event_timer_.StartRepeating(TimeDelta::FromMilliseconds(350),
                                       FROM_HERE);
}

void HTMLMediaElement::WaitForSourceChange() {
  BLINK_MEDIA_LOG << "waitForSourceChange(" << (void*)this << ")";

  StopPeriodicTimers();
  load_state_ = kWaitingForSource;

  // 6.17 - Waiting: Set the element's networkState attribute to the
  // NETWORK_NO_SOURCE value
  SetNetworkState(kNetworkNoSource);

  // 6.18 - Set the element's delaying-the-load-event flag to false. This stops
  // delaying the load event.
  SetShouldDelayLoadEvent(false);

  UpdateDisplayState();

  if (GetLayoutObject())
    GetLayoutObject()->UpdateFromElement();
}

void HTMLMediaElement::NoneSupported(const String& message) {
  BLINK_MEDIA_LOG << "NoneSupported(" << (void*)this << ", message='" << message
                  << "')";

  StopPeriodicTimers();
  load_state_ = kWaitingForSource;
  current_source_node_ = nullptr;

  // 4.8.12.5
  // The dedicated media source failure steps are the following steps:

  // 1 - Set the error attribute to a new MediaError object whose code attribute
  // is set to MEDIA_ERR_SRC_NOT_SUPPORTED.
  error_ = MediaError::Create(MediaError::kMediaErrSrcNotSupported, message);

  // 2 - Forget the media element's media-resource-specific text tracks.
  ForgetResourceSpecificTracks();

  // 3 - Set the element's networkState attribute to the NETWORK_NO_SOURCE
  // value.
  SetNetworkState(kNetworkNoSource);

  // 4 - Set the element's show poster flag to true.
  UpdateDisplayState();

  // 5 - Fire a simple event named error at the media element.
  ScheduleEvent(EventTypeNames::error);

  // 6 - Reject pending play promises with NotSupportedError.
  ScheduleRejectPlayPromises(kNotSupportedError);

  CloseMediaSource();

  // 7 - Set the element's delaying-the-load-event flag to false. This stops
  // delaying the load event.
  SetShouldDelayLoadEvent(false);

  if (GetLayoutObject())
    GetLayoutObject()->UpdateFromElement();
}

void HTMLMediaElement::MediaEngineError(MediaError* err) {
  DCHECK_GE(ready_state_, kHaveMetadata);
  BLINK_MEDIA_LOG << "mediaEngineError(" << (void*)this << ", "
                  << static_cast<int>(err->code()) << ")";

  // 1 - The user agent should cancel the fetching process.
  StopPeriodicTimers();
  load_state_ = kWaitingForSource;

  // 2 - Set the error attribute to a new MediaError object whose code attribute
  // is set to MEDIA_ERR_NETWORK/MEDIA_ERR_DECODE.
  error_ = err;

  // 3 - Queue a task to fire a simple event named error at the media element.
  ScheduleEvent(EventTypeNames::error);

  // 4 - Set the element's networkState attribute to the NETWORK_IDLE value.
  SetNetworkState(kNetworkIdle);

  // 5 - Set the element's delaying-the-load-event flag to false. This stops
  // delaying the load event.
  SetShouldDelayLoadEvent(false);

  // 6 - Abort the overall resource selection algorithm.
  current_source_node_ = nullptr;
}

void HTMLMediaElement::CancelPendingEventsAndCallbacks() {
  BLINK_MEDIA_LOG << "cancelPendingEventsAndCallbacks(" << (void*)this << ")";
  async_event_queue_->CancelAllEvents();

  for (HTMLSourceElement* source =
           Traversal<HTMLSourceElement>::FirstChild(*this);
       source; source = Traversal<HTMLSourceElement>::NextSibling(*source))
    source->CancelPendingErrorEvent();
}

void HTMLMediaElement::NetworkStateChanged() {
  SetNetworkState(GetWebMediaPlayer()->GetNetworkState());
}

void HTMLMediaElement::MediaLoadingFailed(WebMediaPlayer::NetworkState error,
                                          const String& message) {
  BLINK_MEDIA_LOG << "MediaLoadingFailed(" << (void*)this << ", "
                  << static_cast<int>(error) << ", message='" << message
                  << "')";

  StopPeriodicTimers();

  // If we failed while trying to load a <source> element, the movie was never
  // parsed, and there are more <source> children, schedule the next one
  if (ready_state_ < kHaveMetadata &&
      load_state_ == kLoadingFromSourceElement) {
    // resource selection algorithm
    // Step 9.Otherwise.9 - Failed with elements: Queue a task, using the DOM
    // manipulation task source, to fire a simple event named error at the
    // candidate element.
    if (current_source_node_) {
      current_source_node_->ScheduleErrorEvent();
    } else {
      BLINK_MEDIA_LOG << "mediaLoadingFailed(" << (void*)this
                      << ") - error event not sent, <source> was removed";
    }

    // 9.Otherwise.10 - Asynchronously await a stable state. The synchronous
    // section consists of all the remaining steps of this algorithm until the
    // algorithm says the synchronous section has ended.

    // 9.Otherwise.11 - Forget the media element's media-resource-specific
    // tracks.
    ForgetResourceSpecificTracks();

    if (HavePotentialSourceChild()) {
      BLINK_MEDIA_LOG << "mediaLoadingFailed(" << (void*)this
                      << ") - scheduling next <source>";
      ScheduleNextSourceChild();
    } else {
      BLINK_MEDIA_LOG << "mediaLoadingFailed(" << (void*)this
                      << ") - no more <source> elements, waiting";
      WaitForSourceChange();
    }

    return;
  }

  if (error == WebMediaPlayer::kNetworkStateNetworkError &&
      ready_state_ >= kHaveMetadata) {
    MediaEngineError(MediaError::Create(MediaError::kMediaErrNetwork, message));
  } else if (error == WebMediaPlayer::kNetworkStateDecodeError) {
    MediaEngineError(MediaError::Create(MediaError::kMediaErrDecode, message));
  } else if ((error == WebMediaPlayer::kNetworkStateFormatError ||
              error == WebMediaPlayer::kNetworkStateNetworkError) &&
             load_state_ == kLoadingFromSrcAttr) {
    if (message.IsEmpty()) {
      // Generate a more meaningful error message to differentiate the two types
      // of MEDIA_SRC_ERR_NOT_SUPPORTED.
      NoneSupported(BuildElementErrorMessage(
          error == WebMediaPlayer::kNetworkStateFormatError ? "Format error"
                                                            : "Network error"));
    } else {
      NoneSupported(message);
    }
  }

  UpdateDisplayState();
}

void HTMLMediaElement::SetNetworkState(WebMediaPlayer::NetworkState state) {
  BLINK_MEDIA_LOG << "setNetworkState(" << (void*)this << ", "
                  << static_cast<int>(state) << ") - current state is "
                  << static_cast<int>(network_state_);

  if (state == WebMediaPlayer::kNetworkStateEmpty) {
    // Just update the cached state and leave, we can't do anything.
    SetNetworkState(kNetworkEmpty);
    return;
  }

  if (state == WebMediaPlayer::kNetworkStateFormatError ||
      state == WebMediaPlayer::kNetworkStateNetworkError ||
      state == WebMediaPlayer::kNetworkStateDecodeError) {
    MediaLoadingFailed(state, web_media_player_->GetErrorMessage());
    return;
  }

  if (state == WebMediaPlayer::kNetworkStateIdle) {
    if (network_state_ > kNetworkIdle) {
      ChangeNetworkStateFromLoadingToIdle();
      SetShouldDelayLoadEvent(false);
    } else {
      SetNetworkState(kNetworkIdle);
    }
  }

  if (state == WebMediaPlayer::kNetworkStateLoading) {
    if (network_state_ < kNetworkLoading || network_state_ == kNetworkNoSource)
      StartProgressEventTimer();
    SetNetworkState(kNetworkLoading);
  }

  if (state == WebMediaPlayer::kNetworkStateLoaded) {
    if (network_state_ != kNetworkIdle)
      ChangeNetworkStateFromLoadingToIdle();
  }
}

void HTMLMediaElement::ChangeNetworkStateFromLoadingToIdle() {
  progress_event_timer_.Stop();

  // Schedule one last progress event so we guarantee that at least one is fired
  // for files that load very quickly.
  if (GetWebMediaPlayer() && GetWebMediaPlayer()->DidLoadingProgress())
    ScheduleEvent(EventTypeNames::progress);
  ScheduleEvent(EventTypeNames::suspend);
  SetNetworkState(kNetworkIdle);
}

void HTMLMediaElement::ReadyStateChanged() {
  SetReadyState(static_cast<ReadyState>(GetWebMediaPlayer()->GetReadyState()));
}

void HTMLMediaElement::SetReadyState(ReadyState state) {
  BLINK_MEDIA_LOG << "setReadyState(" << (void*)this << ", "
                  << static_cast<int>(state) << ") - current state is "
                  << static_cast<int>(ready_state_);

  // Set "wasPotentiallyPlaying" BEFORE updating ready_state_,
  // potentiallyPlaying() uses it
  bool was_potentially_playing = PotentiallyPlaying();

  ReadyState old_state = ready_state_;
  ReadyState new_state = state;

  bool tracks_are_ready = TextTracksAreReady();

  if (new_state == old_state && tracks_are_ready_ == tracks_are_ready)
    return;

  tracks_are_ready_ = tracks_are_ready;

  if (tracks_are_ready) {
    ready_state_ = new_state;
  } else {
    // If a media file has text tracks the readyState may not progress beyond
    // kHaveFutureData until the text tracks are ready, regardless of the state
    // of the media file.
    if (new_state <= kHaveMetadata)
      ready_state_ = new_state;
    else
      ready_state_ = kHaveCurrentData;
  }

  if (old_state > ready_state_maximum_)
    ready_state_maximum_ = old_state;

  if (network_state_ == kNetworkEmpty)
    return;

  if (seeking_) {
    // 4.8.12.9, step 9 note: If the media element was potentially playing
    // immediately before it started seeking, but seeking caused its readyState
    // attribute to change to a value lower than kHaveFutureData, then a waiting
    // will be fired at the element.
    if (was_potentially_playing && ready_state_ < kHaveFutureData)
      ScheduleEvent(EventTypeNames::waiting);

    // 4.8.12.9 steps 12-14
    if (ready_state_ >= kHaveCurrentData)
      FinishSeek();
  } else {
    if (was_potentially_playing && ready_state_ < kHaveFutureData) {
      // Force an update to official playback position. Automatic updates from
      // currentPlaybackPosition() will be blocked while ready_state_ remains
      // < kHaveFutureData. This blocking is desired after 'waiting' has been
      // fired, but its good to update it one final time to accurately reflect
      // media time at the moment we ran out of data to play.
      SetOfficialPlaybackPosition(CurrentPlaybackPosition());

      // 4.8.12.8
      ScheduleTimeupdateEvent(false);
      ScheduleEvent(EventTypeNames::waiting);
    }
  }

  // Once enough of the media data has been fetched to determine the duration of
  // the media resource, its dimensions, and other metadata...
  if (ready_state_ >= kHaveMetadata && old_state < kHaveMetadata) {
    CreatePlaceholderTracksIfNecessary();

    SelectInitialTracksIfNecessary();

    MediaFragmentURIParser fragment_parser(current_src_);
    fragment_end_time_ = fragment_parser.EndTime();

    // Set the current playback position and the official playback position to
    // the earliest possible position.
    SetOfficialPlaybackPosition(EarliestPossiblePosition());

    duration_ = web_media_player_->Duration();
    ScheduleEvent(EventTypeNames::durationchange);

    if (IsHTMLVideoElement())
      ScheduleEvent(EventTypeNames::resize);
    ScheduleEvent(EventTypeNames::loadedmetadata);

    bool jumped = false;
    if (default_playback_start_position_ > 0) {
      Seek(default_playback_start_position_);
      jumped = true;
    }
    default_playback_start_position_ = 0;

    double initial_playback_position = fragment_parser.StartTime();
    if (std::isnan(initial_playback_position))
      initial_playback_position = 0;

    if (!jumped && initial_playback_position > 0) {
      UseCounter::Count(GetDocument(),
                        WebFeature::kHTMLMediaElementSeekToFragmentStart);
      Seek(initial_playback_position);
      jumped = true;
    }

    if (GetLayoutObject())
      GetLayoutObject()->UpdateFromElement();
  }

  bool should_update_display_state = false;

  if (ready_state_ >= kHaveCurrentData && old_state < kHaveCurrentData &&
      !have_fired_loaded_data_) {
    // Force an update to official playback position to catch non-zero start
    // times that were not known at kHaveMetadata, but are known now that the
    // first packets have been demuxed.
    SetOfficialPlaybackPosition(CurrentPlaybackPosition());

    have_fired_loaded_data_ = true;
    should_update_display_state = true;
    ScheduleEvent(EventTypeNames::loadeddata);
    SetShouldDelayLoadEvent(false);
  }

  bool is_potentially_playing = PotentiallyPlaying();
  if (ready_state_ == kHaveFutureData && old_state <= kHaveCurrentData &&
      tracks_are_ready) {
    ScheduleEvent(EventTypeNames::canplay);
    if (is_potentially_playing)
      ScheduleNotifyPlaying();
    should_update_display_state = true;
  }

  if (ready_state_ == kHaveEnoughData && old_state < kHaveEnoughData &&
      tracks_are_ready) {
    if (old_state <= kHaveCurrentData) {
      ScheduleEvent(EventTypeNames::canplay);
      if (is_potentially_playing)
        ScheduleNotifyPlaying();
    }

    if (autoplay_policy_->RequestAutoplayByAttribute()) {
      paused_ = false;
      ScheduleEvent(EventTypeNames::play);
      ScheduleNotifyPlaying();
      can_autoplay_ = false;
    }

    ScheduleEvent(EventTypeNames::canplaythrough);

    should_update_display_state = true;
  }

  if (should_update_display_state)
    UpdateDisplayState();

  UpdatePlayState();
  GetCueTimeline().UpdateActiveCues(currentTime());
}

void HTMLMediaElement::ProgressEventTimerFired(TimerBase*) {
  if (network_state_ != kNetworkLoading)
    return;

  double time = WTF::CurrentTime();
  double timedelta = time - previous_progress_time_;

  if (GetWebMediaPlayer() && GetWebMediaPlayer()->DidLoadingProgress()) {
    ScheduleEvent(EventTypeNames::progress);
    previous_progress_time_ = time;
    sent_stalled_event_ = false;
    if (GetLayoutObject())
      GetLayoutObject()->UpdateFromElement();
  } else if (timedelta > 3.0 && !sent_stalled_event_) {
    ScheduleEvent(EventTypeNames::stalled);
    sent_stalled_event_ = true;
    SetShouldDelayLoadEvent(false);
  }
}

void HTMLMediaElement::AddPlayedRange(double start, double end) {
  BLINK_MEDIA_LOG << "addPlayedRange(" << (void*)this << ", " << start << ", "
                  << end << ")";
  if (!played_time_ranges_)
    played_time_ranges_ = TimeRanges::Create();
  played_time_ranges_->Add(start, end);
}

bool HTMLMediaElement::SupportsSave() const {
  // Check if download is disabled per settings.
  if (GetDocument().GetSettings() &&
      GetDocument().GetSettings()->GetHideDownloadUI()) {
    return false;
  }

  // URLs that lead to nowhere are ignored.
  if (current_src_.IsNull() || current_src_.IsEmpty())
    return false;

  // If we have no source, we can't download.
  if (network_state_ == kNetworkEmpty || network_state_ == kNetworkNoSource)
    return false;

  // It is not useful to offer a save feature on local files.
  if (current_src_.IsLocalFile())
    return false;

  // MediaStream can't be downloaded.
  if (IsMediaStreamURL(current_src_.GetString()))
    return false;

  // MediaSource can't be downloaded.
  if (HasMediaSource())
    return false;

  // HLS stream shouldn't have a download button.
  if (IsHLSURL(current_src_))
    return false;

  // Infinite streams don't have a clear end at which to finish the download.
  if (duration() == std::numeric_limits<double>::infinity())
    return false;

  return true;
}

void HTMLMediaElement::SetIgnorePreloadNone() {
  BLINK_MEDIA_LOG << "setIgnorePreloadNone(" << (void*)this << ")";
  ignore_preload_none_ = true;
  SetPlayerPreload();
}

void HTMLMediaElement::Seek(double time) {
  BLINK_MEDIA_LOG << "seek(" << (void*)this << ", " << time << ")";

  // 2 - If the media element's readyState is HAVE_NOTHING, abort these steps.
  // FIXME: remove web_media_player_ check once we figure out how
  // web_media_player_ is going out of sync with readystate.
  // web_media_player_ is cleared but readystate is not set to HAVE_NOTHING.
  if (!web_media_player_ || ready_state_ == kHaveNothing)
    return;

  // Ignore preload none and start load if necessary.
  SetIgnorePreloadNone();

  // Get the current time before setting seeking_, last_seek_time_ is returned
  // once it is set.
  double now = currentTime();

  // 3 - If the element's seeking IDL attribute is true, then another instance
  // of this algorithm is already running. Abort that other instance of the
  // algorithm without waiting for the step that it is running to complete.
  // Nothing specific to be done here.

  // 4 - Set the seeking IDL attribute to true.
  // The flag will be cleared when the engine tells us the time has actually
  // changed.
  seeking_ = true;

  // 6 - If the new playback position is later than the end of the media
  // resource, then let it be the end of the media resource instead.
  time = std::min(time, duration());

  // 7 - If the new playback position is less than the earliest possible
  // position, let it be that position instead.
  time = std::max(time, EarliestPossiblePosition());

  // Ask the media engine for the time value in the movie's time scale before
  // comparing with current time. This is necessary because if the seek time is
  // not equal to currentTime but the delta is less than the movie's time scale,
  // we will ask the media engine to "seek" to the current movie time, which may
  // be a noop and not generate a timechanged callback. This means seeking_
  // will never be cleared and we will never fire a 'seeked' event.
  double media_time = GetWebMediaPlayer()->MediaTimeForTimeValue(time);
  if (time != media_time) {
    BLINK_MEDIA_LOG << "seek(" << (void*)this << ", " << time
                    << ") - media timeline equivalent is " << media_time;
    time = media_time;
  }

  // 8 - If the (possibly now changed) new playback position is not in one of
  // the ranges given in the seekable attribute, then let it be the position in
  // one of the ranges given in the seekable attribute that is the nearest to
  // the new playback position. ... If there are no ranges given in the seekable
  // attribute then set the seeking IDL attribute to false and abort these
  // steps.
  TimeRanges* seekable_ranges = seekable();

  if (!seekable_ranges->length()) {
    seeking_ = false;
    return;
  }
  time = seekable_ranges->Nearest(time, now);

  if (playing_ && last_seek_time_ < now)
    AddPlayedRange(last_seek_time_, now);

  last_seek_time_ = time;

  // 10 - Queue a task to fire a simple event named seeking at the element.
  ScheduleEvent(EventTypeNames::seeking);

  // 11 - Set the current playback position to the given new playback position.
  GetWebMediaPlayer()->Seek(time);

  // 14-17 are handled, if necessary, when the engine signals a readystate
  // change or otherwise satisfies seek completion and signals a time change.
}

void HTMLMediaElement::FinishSeek() {
  BLINK_MEDIA_LOG << "finishSeek(" << (void*)this << ")";

  // 14 - Set the seeking IDL attribute to false.
  seeking_ = false;

  // Force an update to officialPlaybackPosition. Periodic updates generally
  // handle this, but may be skipped paused or waiting for data.
  SetOfficialPlaybackPosition(CurrentPlaybackPosition());

  // 16 - Queue a task to fire a simple event named timeupdate at the element.
  ScheduleTimeupdateEvent(false);

  // 17 - Queue a task to fire a simple event named seeked at the element.
  ScheduleEvent(EventTypeNames::seeked);

  SetDisplayMode(kVideo);
}

HTMLMediaElement::ReadyState HTMLMediaElement::getReadyState() const {
  return ready_state_;
}

bool HTMLMediaElement::HasVideo() const {
  return GetWebMediaPlayer() && GetWebMediaPlayer()->HasVideo();
}

bool HTMLMediaElement::HasAudio() const {
  return GetWebMediaPlayer() && GetWebMediaPlayer()->HasAudio();
}

bool HTMLMediaElement::seeking() const {
  return seeking_;
}

// https://www.w3.org/TR/html51/semantics-embedded-content.html#earliest-possible-position
// The earliest possible position is not explicitly exposed in the API; it
// corresponds to the start time of the first range in the seekable attribute’s
// TimeRanges object, if any, or the current playback position otherwise.
double HTMLMediaElement::EarliestPossiblePosition() const {
  TimeRanges* seekable_ranges = seekable();
  if (seekable_ranges && seekable_ranges->length() > 0)
    return seekable_ranges->start(0, ASSERT_NO_EXCEPTION);

  return CurrentPlaybackPosition();
}

double HTMLMediaElement::CurrentPlaybackPosition() const {
  // "Official" playback position won't take updates from "current" playback
  // position until ready_state_ > kHaveMetadata, but other callers (e.g.
  // pauseInternal) may still request currentPlaybackPosition at any time.
  // From spec: "Media elements have a current playback position, which must
  // initially (i.e., in the absence of media data) be zero seconds."
  if (ready_state_ == kHaveNothing)
    return 0;

  if (GetWebMediaPlayer())
    return GetWebMediaPlayer()->CurrentTime();

  if (ready_state_ >= kHaveMetadata) {
    BLINK_MEDIA_LOG
        << __func__ << " readyState = " << ready_state_
        << " but no webMediaPlayer to provide currentPlaybackPosition";
  }

  return 0;
}

double HTMLMediaElement::OfficialPlaybackPosition() const {
  // Hold updates to official playback position while paused or waiting for more
  // data. The underlying media player may continue to make small advances in
  // currentTime (e.g. as samples in the last rendered audio buffer are played
  // played out), but advancing currentTime while paused/waiting sends a mixed
  // signal about the state of playback.
  bool waiting_for_data = ready_state_ <= kHaveCurrentData;
  if (official_playback_position_needs_update_ && !paused_ &&
      !waiting_for_data) {
    SetOfficialPlaybackPosition(CurrentPlaybackPosition());
  }

#if LOG_OFFICIAL_TIME_STATUS
  static const double kMinCachedDeltaForWarning = 0.01;
  double delta =
      std::abs(official_playback_position_ - CurrentPlaybackPosition());
  if (delta > kMinCachedDeltaForWarning) {
    BLINK_MEDIA_LOG << "CurrentTime(" << (void*)this
                    << ") - WARNING, cached time is " << delta
                    << "seconds off of media time when paused/waiting";
  }
#endif

  return official_playback_position_;
}

void HTMLMediaElement::SetOfficialPlaybackPosition(double position) const {
#if LOG_OFFICIAL_TIME_STATUS
  BLINK_MEDIA_LOG << "SetOfficialPlaybackPosition(" << (void*)this
                  << ") was:" << official_playback_position_
                  << " now:" << position;
#endif

  // Internal player position may advance slightly beyond duration because
  // many files use imprecise duration. Clamp official position to duration when
  // known. Duration may be unknown when readyState < HAVE_METADATA.
  official_playback_position_ =
      std::isnan(duration()) ? position : std::min(duration(), position);

  if (official_playback_position_ != position) {
    BLINK_MEDIA_LOG << "setOfficialPlaybackPosition(" << (void*)this
                    << ") position:" << position
                    << " truncated to duration:" << official_playback_position_;
  }

  // Once set, official playback position should hold steady until the next
  // stable state. We approximate this by using a microtask to mark the
  // need for an update after the current (micro)task has completed. When
  // needed, the update is applied in the next call to
  // officialPlaybackPosition().
  official_playback_position_needs_update_ = false;
  Microtask::EnqueueMicrotask(
      WTF::Bind(&HTMLMediaElement::RequireOfficialPlaybackPositionUpdate,
                WrapWeakPersistent(this)));
}

void HTMLMediaElement::RequireOfficialPlaybackPositionUpdate() const {
  official_playback_position_needs_update_ = true;
}

double HTMLMediaElement::currentTime() const {
  if (default_playback_start_position_)
    return default_playback_start_position_;

  if (seeking_) {
    BLINK_MEDIA_LOG << "currentTime(" << (void*)this
                    << ") - seeking, returning " << last_seek_time_;
    return last_seek_time_;
  }

  return OfficialPlaybackPosition();
}

void HTMLMediaElement::setCurrentTime(double time) {
  // If the media element's readyState is kHaveNothing, then set the default
  // playback start position to that time.
  if (ready_state_ == kHaveNothing) {
    default_playback_start_position_ = time;
    return;
  }

  Seek(time);
}

double HTMLMediaElement::duration() const {
  return duration_;
}

bool HTMLMediaElement::paused() const {
  return paused_;
}

double HTMLMediaElement::defaultPlaybackRate() const {
  return default_playback_rate_;
}

void HTMLMediaElement::setDefaultPlaybackRate(double rate) {
  if (default_playback_rate_ == rate)
    return;

  default_playback_rate_ = rate;
  ScheduleEvent(EventTypeNames::ratechange);
}

double HTMLMediaElement::playbackRate() const {
  return playback_rate_;
}

void HTMLMediaElement::setPlaybackRate(double rate,
                                       ExceptionState& exception_state) {
  BLINK_MEDIA_LOG << "setPlaybackRate(" << (void*)this << ", " << rate << ")";

  if (rate != 0.0 && (rate < kMinRate || rate > kMaxRate)) {
    UseCounter::Count(GetDocument(),
                      WebFeature::kHTMLMediaElementMediaPlaybackRateOutOfRange);

    // When the proposed playbackRate is unsupported, throw a NotSupportedError
    // DOMException and don't update the value.
    exception_state.ThrowDOMException(
        kNotSupportedError, "The provided playback rate (" +
                                String::Number(rate) + ") is not in the " +
                                "supported playback range.");

    // Do not update |playback_rate_|.
    return;
  }

  if (playback_rate_ != rate) {
    playback_rate_ = rate;
    ScheduleEvent(EventTypeNames::ratechange);
  }

  UpdatePlaybackRate();
}

HTMLMediaElement::DirectionOfPlayback HTMLMediaElement::GetDirectionOfPlayback()
    const {
  return playback_rate_ >= 0 ? kForward : kBackward;
}

void HTMLMediaElement::UpdatePlaybackRate() {
  // FIXME: remove web_media_player_ check once we figure out how
  // web_media_player_ is going out of sync with readystate.
  // web_media_player_ is cleared but readystate is not set to kHaveNothing.
  if (web_media_player_ && PotentiallyPlaying())
    GetWebMediaPlayer()->SetRate(playbackRate());
}

bool HTMLMediaElement::ended() const {
  // 4.8.12.8 Playing the media resource
  // The ended attribute must return true if the media element has ended
  // playback and the direction of playback is forwards, and false otherwise.
  return EndedPlayback() && GetDirectionOfPlayback() == kForward;
}

bool HTMLMediaElement::Autoplay() const {
  return FastHasAttribute(autoplayAttr);
}

String HTMLMediaElement::preload() const {
  return PreloadTypeToString(PreloadType());
}

void HTMLMediaElement::setPreload(const AtomicString& preload) {
  BLINK_MEDIA_LOG << "setPreload(" << (void*)this << ", " << preload << ")";
  setAttribute(preloadAttr, preload);
}

WebMediaPlayer::Preload HTMLMediaElement::PreloadType() const {
  const AtomicString& preload = FastGetAttribute(preloadAttr);
  if (DeprecatedEqualIgnoringCase(preload, "none")) {
    UseCounter::Count(GetDocument(), WebFeature::kHTMLMediaElementPreloadNone);
    return WebMediaPlayer::kPreloadNone;
  }

  // If the source scheme is requires network, force preload to 'none' on Data
  // Saver and for low end devices.
  if (GetDocument().GetSettings() &&
      (GetNetworkStateNotifier().SaveDataEnabled() ||
       GetDocument().GetSettings()->GetForcePreloadNoneForMediaElements()) &&
      (current_src_.Protocol() != "blob" && current_src_.Protocol() != "data" &&
       current_src_.Protocol() != "file")) {
    UseCounter::Count(GetDocument(),
                      WebFeature::kHTMLMediaElementPreloadForcedNone);
    return WebMediaPlayer::kPreloadNone;
  }

  if (DeprecatedEqualIgnoringCase(preload, "metadata")) {
    UseCounter::Count(GetDocument(),
                      WebFeature::kHTMLMediaElementPreloadMetadata);
    return WebMediaPlayer::kPreloadMetaData;
  }

  // Force preload to 'metadata' on cellular connections.
  if (GetNetworkStateNotifier().IsCellularConnectionType()) {
    UseCounter::Count(GetDocument(),
                      WebFeature::kHTMLMediaElementPreloadForcedMetadata);
    return WebMediaPlayer::kPreloadMetaData;
  }

  // Per HTML spec, "The empty string ... maps to the Automatic state."
  // https://html.spec.whatwg.org/#attr-media-preload
  if (DeprecatedEqualIgnoringCase(preload, "auto") ||
      DeprecatedEqualIgnoringCase(preload, "")) {
    UseCounter::Count(GetDocument(), WebFeature::kHTMLMediaElementPreloadAuto);
    return WebMediaPlayer::kPreloadAuto;
  }

  // "The attribute's missing value default is user-agent defined, though the
  // Metadata state is suggested as a compromise between reducing server load
  // and providing an optimal user experience."

  // The spec does not define an invalid value default:
  // https://www.w3.org/Bugs/Public/show_bug.cgi?id=28950
  UseCounter::Count(GetDocument(), WebFeature::kHTMLMediaElementPreloadDefault);
  return RuntimeEnabledFeatures::PreloadDefaultIsMetadataEnabled()
             ? WebMediaPlayer::kPreloadMetaData
             : WebMediaPlayer::kPreloadAuto;
}

String HTMLMediaElement::EffectivePreload() const {
  return PreloadTypeToString(EffectivePreloadType());
}

WebMediaPlayer::Preload HTMLMediaElement::EffectivePreloadType() const {
  if (Autoplay() && !autoplay_policy_->IsGestureNeededForPlayback())
    return WebMediaPlayer::kPreloadAuto;

  WebMediaPlayer::Preload preload = PreloadType();
  if (ignore_preload_none_ && preload == WebMediaPlayer::kPreloadNone)
    return WebMediaPlayer::kPreloadMetaData;

  return preload;
}

ScriptPromise HTMLMediaElement::playForBindings(ScriptState* script_state) {
  // We have to share the same logic for internal and external callers. The
  // internal callers do not want to receive a Promise back but when ::play()
  // is called, |play_promise_resolvers_| needs to be populated. What this code
  // does is to populate |play_promise_resolvers_| before calling ::play() and
  // remove the Promise if ::play() failed.
  ScriptPromiseResolver* resolver = ScriptPromiseResolver::Create(script_state);
  ScriptPromise promise = resolver->Promise();
  play_promise_resolvers_.push_back(resolver);

  Optional<ExceptionCode> code = Play();
  if (code) {
    DCHECK(!play_promise_resolvers_.IsEmpty());
    play_promise_resolvers_.pop_back();

    String message;
    switch (code.value()) {
      case kNotAllowedError:
        message = autoplay_policy_->GetPlayErrorMessage();
        RecordPlayPromiseRejected(
            PlayPromiseRejectReason::kFailedAutoplayPolicy);
        break;
      case kNotSupportedError:
        message = "The element has no supported sources.";
        RecordPlayPromiseRejected(PlayPromiseRejectReason::kNoSupportedSources);
        break;
      default:
        NOTREACHED();
    }
    resolver->Reject(DOMException::Create(code.value(), message));
    return promise;
  }

  return promise;
}

Optional<ExceptionCode> HTMLMediaElement::Play() {
  BLINK_MEDIA_LOG << "play(" << (void*)this << ")";

  Optional<ExceptionCode> exception_code = autoplay_policy_->RequestPlay();

  if (exception_code == kNotAllowedError) {
    // If we're already playing, then this play would do nothing anyway.
    // Call playInternal to handle scheduling the promise resolution.
    if (!paused_) {
      PlayInternal();
      return WTF::nullopt;
    }
    return exception_code;
  }

  autoplay_policy_->StopAutoplayMutedWhenVisible();

  if (error_ && error_->code() == MediaError::kMediaErrSrcNotSupported)
    return kNotSupportedError;

  DCHECK(!exception_code.has_value());

  PlayInternal();

  return WTF::nullopt;
}

void HTMLMediaElement::PlayInternal() {
  BLINK_MEDIA_LOG << "playInternal(" << (void*)this << ")";

  // 4.8.12.8. Playing the media resource
  if (network_state_ == kNetworkEmpty)
    InvokeResourceSelectionAlgorithm();

  // Generally "ended" and "looping" are exclusive. Here, the loop attribute
  // is ignored to seek back to start in case loop was set after playback
  // ended. See http://crbug.com/364442
  if (EndedPlayback(LoopCondition::kIgnored))
    Seek(0);

  if (paused_) {
    paused_ = false;
    ScheduleEvent(EventTypeNames::play);

    if (ready_state_ <= kHaveCurrentData)
      ScheduleEvent(EventTypeNames::waiting);
    else if (ready_state_ >= kHaveFutureData)
      ScheduleNotifyPlaying();
  } else if (ready_state_ >= kHaveFutureData) {
    ScheduleResolvePlayPromises();
  }

  can_autoplay_ = false;

  SetIgnorePreloadNone();
  UpdatePlayState();
}

void HTMLMediaElement::pause() {
  BLINK_MEDIA_LOG << "pause(" << (void*)this << ")";

  autoplay_policy_->StopAutoplayMutedWhenVisible();
  PauseInternal();
}

void HTMLMediaElement::PauseInternal() {
  BLINK_MEDIA_LOG << "pauseInternal(" << (void*)this << ")";

  if (network_state_ == kNetworkEmpty)
    InvokeResourceSelectionAlgorithm();

  can_autoplay_ = false;

  if (!paused_) {
    paused_ = true;
    ScheduleTimeupdateEvent(false);
    ScheduleEvent(EventTypeNames::pause);

    // Force an update to official playback position. Automatic updates from
    // currentPlaybackPosition() will be blocked while paused_ = true. This
    // blocking is desired while paused, but its good to update it one final
    // time to accurately reflect movie time at the moment we paused.
    SetOfficialPlaybackPosition(CurrentPlaybackPosition());

    ScheduleRejectPlayPromises(kAbortError);
  }

  UpdatePlayState();
}

void HTMLMediaElement::RequestRemotePlayback() {
  if (GetWebMediaPlayer())
    GetWebMediaPlayer()->RequestRemotePlayback();
}

void HTMLMediaElement::RequestRemotePlaybackControl() {
  if (GetWebMediaPlayer())
    GetWebMediaPlayer()->RequestRemotePlaybackControl();
}

void HTMLMediaElement::RequestRemotePlaybackStop() {
  if (GetWebMediaPlayer())
    GetWebMediaPlayer()->RequestRemotePlaybackStop();
}

void HTMLMediaElement::CloseMediaSource() {
  if (!media_source_)
    return;

  media_source_->Close();
  media_source_ = nullptr;
}

bool HTMLMediaElement::Loop() const {
  return FastHasAttribute(loopAttr);
}

void HTMLMediaElement::SetLoop(bool b) {
  BLINK_MEDIA_LOG << "setLoop(" << (void*)this << ", " << BoolString(b) << ")";
  SetBooleanAttribute(loopAttr, b);
}

bool HTMLMediaElement::ShouldShowControls(
    const RecordMetricsBehavior record_metrics) const {
  Settings* settings = GetDocument().GetSettings();
  if (settings && !settings->GetMediaControlsEnabled()) {
    if (record_metrics == RecordMetricsBehavior::kDoRecord)
      ShowControlsHistogram().Count(kMediaControlsShowDisabledSettings);
    return false;
  }

  if (FastHasAttribute(controlsAttr)) {
    if (record_metrics == RecordMetricsBehavior::kDoRecord)
      ShowControlsHistogram().Count(kMediaControlsShowAttribute);
    return true;
  }

  if (IsFullscreen()) {
    if (record_metrics == RecordMetricsBehavior::kDoRecord)
      ShowControlsHistogram().Count(kMediaControlsShowFullscreen);
    return true;
  }

  LocalFrame* frame = GetDocument().GetFrame();
  if (frame && !GetDocument().CanExecuteScripts(kNotAboutToExecuteScript)) {
    if (record_metrics == RecordMetricsBehavior::kDoRecord)
      ShowControlsHistogram().Count(kMediaControlsShowNoScript);
    return true;
  }

  if (record_metrics == RecordMetricsBehavior::kDoRecord)
    ShowControlsHistogram().Count(kMediaControlsShowNotShown);
  return false;
}

DOMTokenList* HTMLMediaElement::controlsList() const {
  return controls_list_.Get();
}

HTMLMediaElementControlsList* HTMLMediaElement::ControlsListInternal() const {
  return controls_list_.Get();
}

double HTMLMediaElement::volume() const {
  return volume_;
}

void HTMLMediaElement::setVolume(double vol, ExceptionState& exception_state) {
  BLINK_MEDIA_LOG << "setVolume(" << (void*)this << ", " << vol << ")";

  if (volume_ == vol)
    return;

  if (vol < 0.0f || vol > 1.0f) {
    exception_state.ThrowDOMException(
        kIndexSizeError,
        ExceptionMessages::IndexOutsideRange(
            "volume", vol, 0.0, ExceptionMessages::kInclusiveBound, 1.0,
            ExceptionMessages::kInclusiveBound));
    return;
  }

  volume_ = vol;

  if (GetWebMediaPlayer())
    GetWebMediaPlayer()->SetVolume(EffectiveMediaVolume());
  ScheduleEvent(EventTypeNames::volumechange);
}

bool HTMLMediaElement::muted() const {
  return muted_;
}

void HTMLMediaElement::setMuted(bool muted) {
  BLINK_MEDIA_LOG << "setMuted(" << (void*)this << ", " << BoolString(muted)
                  << ")";

  if (muted_ == muted)
    return;

  muted_ = muted;

  ScheduleEvent(EventTypeNames::volumechange);

  // If it is unmute and AutoplayPolicy doesn't want the playback to continue,
  // pause the playback.
  if (!muted_ && !autoplay_policy_->RequestAutoplayUnmute())
    pause();

  // This is called after the volumechange event to make sure isAutoplayingMuted
  // returns the right value when webMediaPlayer receives the volume update.
  if (GetWebMediaPlayer())
    GetWebMediaPlayer()->SetVolume(EffectiveMediaVolume());

  autoplay_policy_->StopAutoplayMutedWhenVisible();
}

void HTMLMediaElement::enterPictureInPicture() {
  if (GetWebMediaPlayer())
    GetWebMediaPlayer()->EnterPictureInPicture();
}

double HTMLMediaElement::EffectiveMediaVolume() const {
  if (muted_)
    return 0;

  return volume_;
}

// The spec says to fire periodic timeupdate events (those sent while playing)
// every "15 to 250ms", we choose the slowest frequency
static const TimeDelta kMaxTimeupdateEventFrequency =
    TimeDelta::FromMilliseconds(250);

void HTMLMediaElement::StartPlaybackProgressTimer() {
  if (playback_progress_timer_.IsActive())
    return;

  previous_progress_time_ = WTF::CurrentTime();
  playback_progress_timer_.StartRepeating(kMaxTimeupdateEventFrequency,
                                          FROM_HERE);
}

void HTMLMediaElement::PlaybackProgressTimerFired(TimerBase*) {
  if (!std::isnan(fragment_end_time_) && currentTime() >= fragment_end_time_ &&
      GetDirectionOfPlayback() == kForward) {
    fragment_end_time_ = std::numeric_limits<double>::quiet_NaN();
    if (!paused_) {
      UseCounter::Count(GetDocument(),
                        WebFeature::kHTMLMediaElementPauseAtFragmentEnd);
      // changes paused to true and fires a simple event named pause at the
      // media element.
      PauseInternal();
    }
  }

  if (!seeking_)
    ScheduleTimeupdateEvent(true);

  if (!playbackRate())
    return;

  GetCueTimeline().UpdateActiveCues(currentTime());
}

void HTMLMediaElement::ScheduleTimeupdateEvent(bool periodic_event) {
  // Per spec, consult current playback position to check for changing time.
  double media_time = CurrentPlaybackPosition();
  bool media_time_has_progressed =
      media_time != last_time_update_event_media_time_;

  if (periodic_event && !media_time_has_progressed)
    return;

  ScheduleEvent(EventTypeNames::timeupdate);

  last_time_update_event_media_time_ = media_time;

  // Ensure periodic event fires 250ms from _this_ event. Restarting the timer
  // cancels pending callbacks.
  if (!periodic_event && playback_progress_timer_.IsActive()) {
    playback_progress_timer_.StartRepeating(kMaxTimeupdateEventFrequency,
                                            FROM_HERE);
  }
}

void HTMLMediaElement::TogglePlayState() {
  if (paused())
    Play();
  else
    pause();
}

AudioTrackList& HTMLMediaElement::audioTracks() {
  return *audio_tracks_;
}

void HTMLMediaElement::AudioTrackChanged(AudioTrack* track) {
  BLINK_MEDIA_LOG << "audioTrackChanged(" << (void*)this
                  << ") trackId= " << String(track->id())
                  << " enabled=" << BoolString(track->enabled());
  DCHECK(MediaTracksEnabledInternally());

  audioTracks().ScheduleChangeEvent();

  if (media_source_)
    media_source_->OnTrackChanged(track);

  if (!audio_tracks_timer_.IsActive())
    audio_tracks_timer_.StartOneShot(TimeDelta(), FROM_HERE);
}

void HTMLMediaElement::AudioTracksTimerFired(TimerBase*) {
  Vector<WebMediaPlayer::TrackId> enabled_track_ids;
  for (unsigned i = 0; i < audioTracks().length(); ++i) {
    AudioTrack* track = audioTracks().AnonymousIndexedGetter(i);
    if (track->enabled())
      enabled_track_ids.push_back(track->id());
  }

  GetWebMediaPlayer()->EnabledAudioTracksChanged(enabled_track_ids);
}

WebMediaPlayer::TrackId HTMLMediaElement::AddAudioTrack(
    const WebString& id,
    WebMediaPlayerClient::AudioTrackKind kind,
    const WebString& label,
    const WebString& language,
    bool enabled) {
  AtomicString kind_string = AudioKindToString(kind);
  BLINK_MEDIA_LOG << "addAudioTrack(" << (void*)this << ", '" << (String)id
                  << "', ' " << (AtomicString)kind_string << "', '"
                  << (String)label << "', '" << (String)language << "', "
                  << BoolString(enabled) << ")";

  AudioTrack* audio_track =
      AudioTrack::Create(id, kind_string, label, language, enabled);
  audioTracks().Add(audio_track);

  return audio_track->id();
}

void HTMLMediaElement::RemoveAudioTrack(WebMediaPlayer::TrackId track_id) {
  BLINK_MEDIA_LOG << "removeAudioTrack(" << (void*)this << ")";

  audioTracks().Remove(track_id);
}

VideoTrackList& HTMLMediaElement::videoTracks() {
  return *video_tracks_;
}

void HTMLMediaElement::SelectedVideoTrackChanged(VideoTrack* track) {
  BLINK_MEDIA_LOG << "selectedVideoTrackChanged(" << (void*)this
                  << ") selectedTrackId="
                  << (track->selected() ? String(track->id()) : "none");
  DCHECK(MediaTracksEnabledInternally());

  if (track->selected())
    videoTracks().TrackSelected(track->id());

  videoTracks().ScheduleChangeEvent();

  if (media_source_)
    media_source_->OnTrackChanged(track);

  WebMediaPlayer::TrackId id = track->id();
  GetWebMediaPlayer()->SelectedVideoTrackChanged(track->selected() ? &id
                                                                   : nullptr);
}

WebMediaPlayer::TrackId HTMLMediaElement::AddVideoTrack(
    const WebString& id,
    WebMediaPlayerClient::VideoTrackKind kind,
    const WebString& label,
    const WebString& language,
    bool selected) {
  AtomicString kind_string = VideoKindToString(kind);
  BLINK_MEDIA_LOG << "addVideoTrack(" << (void*)this << ", '" << (String)id
                  << "', '" << (AtomicString)kind_string << "', '"
                  << (String)label << "', '" << (String)language << "', "
                  << BoolString(selected) << ")";

  // If another track was selected (potentially by the user), leave it selected.
  if (selected && videoTracks().selectedIndex() != -1)
    selected = false;

  VideoTrack* video_track =
      VideoTrack::Create(id, kind_string, label, language, selected);
  videoTracks().Add(video_track);

  return video_track->id();
}

void HTMLMediaElement::RemoveVideoTrack(WebMediaPlayer::TrackId track_id) {
  BLINK_MEDIA_LOG << "removeVideoTrack(" << (void*)this << ")";

  videoTracks().Remove(track_id);
}

void HTMLMediaElement::AddTextTrack(WebInbandTextTrack* web_track) {
  // 4.8.12.11.2 Sourcing in-band text tracks
  // 1. Associate the relevant data with a new text track and its corresponding
  // new TextTrack object.
  InbandTextTrack* text_track = InbandTextTrack::Create(web_track);

  // 2. Set the new text track's kind, label, and language based on the
  // semantics of the relevant data, as defined by the relevant specification.
  // If there is no label in that data, then the label must be set to the empty
  // string.
  // 3. Associate the text track list of cues with the rules for updating the
  // text track rendering appropriate for the format in question.
  // 4. If the new text track's kind is metadata, then set the text track
  // in-band metadata track dispatch type as follows, based on the type of the
  // media resource:
  // 5. Populate the new text track's list of cues with the cues parsed so far,
  // folllowing the guidelines for exposing cues, and begin updating it
  // dynamically as necessary.
  //   - Thess are all done by the media engine.

  // 6. Set the new text track's readiness state to loaded.
  text_track->SetReadinessState(TextTrack::kLoaded);

  // 7. Set the new text track's mode to the mode consistent with the user's
  // preferences and the requirements of the relevant specification for the
  // data.
  //  - This will happen in honorUserPreferencesForAutomaticTextTrackSelection()
  ScheduleTextTrackResourceLoad();

  // 8. Add the new text track to the media element's list of text tracks.
  // 9. Fire an event with the name addtrack, that does not bubble and is not
  // cancelable, and that uses the TrackEvent interface, with the track
  // attribute initialized to the text track's TextTrack object, at the media
  // element's textTracks attribute's TextTrackList object.
  textTracks()->Append(text_track);
}

void HTMLMediaElement::RemoveTextTrack(WebInbandTextTrack* web_track) {
  if (!text_tracks_)
    return;

  // This cast is safe because InbandTextTrack is the only concrete
  // implementation of WebInbandTextTrackClient.
  InbandTextTrack* text_track = ToInbandTextTrack(web_track->Client());
  if (!text_track)
    return;

  text_tracks_->Remove(text_track);
}

void HTMLMediaElement::ForgetResourceSpecificTracks() {
  // Implements the "forget the media element's media-resource-specific tracks"
  // algorithm.  The order is explicitly specified as text, then audio, and
  // finally video.  Also 'removetrack' events should not be fired.
  if (text_tracks_) {
    TrackDisplayUpdateScope scope(GetCueTimeline());
    text_tracks_->RemoveAllInbandTracks();
  }

  audio_tracks_->RemoveAll();
  video_tracks_->RemoveAll();

  audio_tracks_timer_.Stop();
}

TextTrack* HTMLMediaElement::addTextTrack(const AtomicString& kind,
                                          const AtomicString& label,
                                          const AtomicString& language,
                                          ExceptionState& exception_state) {
  // https://html.spec.whatwg.org/multipage/embedded-content.html#dom-media-addtexttrack

  // The addTextTrack(kind, label, language) method of media elements, when
  // invoked, must run the following steps:

  // 1. Create a new TextTrack object.
  // 2. Create a new text track corresponding to the new object, and set its
  //    text track kind to kind, its text track label to label, its text
  //    track language to language, ..., and its text track list of cues to
  //    an empty list.
  TextTrack* text_track = TextTrack::Create(kind, label, language);
  //    ..., its text track readiness state to the text track loaded state, ...
  text_track->SetReadinessState(TextTrack::kLoaded);

  // 3. Add the new text track to the media element's list of text tracks.
  // 4. Queue a task to fire a trusted event with the name addtrack, that
  //    does not bubble and is not cancelable, and that uses the TrackEvent
  //    interface, with the track attribute initialised to the new text
  //    track's TextTrack object, at the media element's textTracks
  //    attribute's TextTrackList object.
  textTracks()->Append(text_track);

  // Note: Due to side effects when changing track parameters, we have to
  // first append the track to the text track list.
  // FIXME: Since setMode() will cause a 'change' event to be queued on the
  // same task source as the 'addtrack' event (see above), the order is
  // wrong. (The 'change' event shouldn't be fired at all in this case...)

  // ..., its text track mode to the text track hidden mode, ...
  text_track->setMode(TextTrack::HiddenKeyword());

  // 5. Return the new TextTrack object.
  return text_track;
}

TextTrackList* HTMLMediaElement::textTracks() {
  if (!text_tracks_)
    text_tracks_ = TextTrackList::Create(this);

  return text_tracks_.Get();
}

void HTMLMediaElement::DidAddTrackElement(HTMLTrackElement* track_element) {
  // 4.8.12.11.3 Sourcing out-of-band text tracks
  // When a track element's parent element changes and the new parent is a media
  // element, then the user agent must add the track element's corresponding
  // text track to the media element's list of text tracks ... [continues in
  // TextTrackList::append]
  TextTrack* text_track = track_element->track();
  if (!text_track)
    return;

  textTracks()->Append(text_track);

  // Do not schedule the track loading until parsing finishes so we don't start
  // before all tracks in the markup have been added.
  if (IsFinishedParsingChildren())
    ScheduleTextTrackResourceLoad();
}

void HTMLMediaElement::DidRemoveTrackElement(HTMLTrackElement* track_element) {
  KURL url = track_element->GetNonEmptyURLAttribute(srcAttr);
  BLINK_MEDIA_LOG << "didRemoveTrackElement(" << (void*)this << ") - 'src' is "
                  << UrlForLoggingMedia(url);

  TextTrack* text_track = track_element->track();
  if (!text_track)
    return;

  text_track->SetHasBeenConfigured(false);

  if (!text_tracks_)
    return;

  // 4.8.12.11.3 Sourcing out-of-band text tracks
  // When a track element's parent element changes and the old parent was a
  // media element, then the user agent must remove the track element's
  // corresponding text track from the media element's list of text tracks.
  text_tracks_->Remove(text_track);

  size_t index = text_tracks_when_resource_selection_began_.Find(text_track);
  if (index != kNotFound)
    text_tracks_when_resource_selection_began_.EraseAt(index);
}

void HTMLMediaElement::HonorUserPreferencesForAutomaticTextTrackSelection() {
  if (!text_tracks_ || !text_tracks_->length())
    return;

  if (!should_perform_automatic_track_selection_)
    return;

  AutomaticTrackSelection::Configuration configuration;
  if (processing_preference_change_)
    configuration.disable_currently_enabled_tracks = true;
  if (text_tracks_visible_)
    configuration.force_enable_subtitle_or_caption_track = true;

  Settings* settings = GetDocument().GetSettings();
  if (settings) {
    configuration.text_track_kind_user_preference =
        settings->GetTextTrackKindUserPreference();
  }

  AutomaticTrackSelection track_selection(configuration);
  track_selection.Perform(*text_tracks_);
}

bool HTMLMediaElement::HavePotentialSourceChild() {
  // Stash the current <source> node and next nodes so we can restore them after
  // checking to see there is another potential.
  HTMLSourceElement* current_source_node = current_source_node_;
  Node* next_node = next_child_node_to_consider_;

  KURL next_url = SelectNextSourceChild(nullptr, kDoNothing);

  current_source_node_ = current_source_node;
  next_child_node_to_consider_ = next_node;

  return next_url.IsValid();
}

KURL HTMLMediaElement::SelectNextSourceChild(
    String* content_type,
    InvalidURLAction action_if_invalid) {
  // Don't log if this was just called to find out if there are any valid
  // <source> elements.
  bool should_log = action_if_invalid != kDoNothing;
  if (should_log)
    BLINK_MEDIA_LOG << "selectNextSourceChild(" << (void*)this << ")";

  if (!next_child_node_to_consider_) {
    if (should_log) {
      BLINK_MEDIA_LOG << "selectNextSourceChild(" << (void*)this
                      << ") -> 0x0000, \"\"";
    }
    return KURL();
  }

  KURL media_url;
  Node* node;
  HTMLSourceElement* source = nullptr;
  String type;
  bool looking_for_start_node = next_child_node_to_consider_;
  bool can_use_source_element = false;

  NodeVector potential_source_nodes;
  GetChildNodes(*this, potential_source_nodes);

  for (unsigned i = 0;
       !can_use_source_element && i < potential_source_nodes.size(); ++i) {
    node = potential_source_nodes[i].Get();
    if (looking_for_start_node && next_child_node_to_consider_ != node)
      continue;
    looking_for_start_node = false;

    if (!IsHTMLSourceElement(*node))
      continue;
    if (node->parentNode() != this)
      continue;

    source = ToHTMLSourceElement(node);

    // 2. If candidate does not have a src attribute, or if its src
    // attribute's value is the empty string ... jump down to the failed
    // step below
    const AtomicString& src_value = source->FastGetAttribute(srcAttr);
    if (should_log) {
      BLINK_MEDIA_LOG << "selectNextSourceChild(" << (void*)this
                      << ") - 'src' is " << UrlForLoggingMedia(media_url);
    }
    if (src_value.IsEmpty())
      goto checkAgain;

    // 3. Let urlString be the resulting URL string that would have resulted
    // from parsing the URL specified by candidate's src attribute's value
    // relative to the candidate's node document when the src attribute was
    // last changed.
    media_url = source->GetDocument().CompleteURL(src_value);

    // 4. If urlString was not obtained successfully, then end the
    // synchronous section, and jump down to the failed with elements step
    // below.
    if (!IsSafeToLoadURL(media_url, action_if_invalid))
      goto checkAgain;

    // 5. If candidate has a type attribute whose value, when parsed as a
    // MIME type ...
    type = source->type();
    if (type.IsEmpty() && media_url.ProtocolIsData())
      type = MimeTypeFromDataURL(media_url);
    if (!type.IsEmpty()) {
      if (should_log) {
        BLINK_MEDIA_LOG << "selectNextSourceChild(" << (void*)this
                        << ") - 'type' is '" << type << "'";
      }
      if (!GetSupportsType(ContentType(type)))
        goto checkAgain;
    }

    // Making it this far means the <source> looks reasonable.
    can_use_source_element = true;

  checkAgain:
    if (!can_use_source_element && action_if_invalid == kComplain && source)
      source->ScheduleErrorEvent();
  }

  if (can_use_source_element) {
    if (content_type)
      *content_type = type;
    current_source_node_ = source;
    next_child_node_to_consider_ = source->nextSibling();
  } else {
    current_source_node_ = nullptr;
    next_child_node_to_consider_ = nullptr;
  }

  if (should_log) {
    BLINK_MEDIA_LOG << "selectNextSourceChild(" << (void*)this << ") -> "
                    << current_source_node_.Get() << ", "
                    << (can_use_source_element ? UrlForLoggingMedia(media_url)
                                               : "");
  }

  return can_use_source_element ? media_url : KURL();
}

void HTMLMediaElement::SourceWasAdded(HTMLSourceElement* source) {
  BLINK_MEDIA_LOG << "sourceWasAdded(" << (void*)this << ", " << source << ")";

  KURL url = source->GetNonEmptyURLAttribute(srcAttr);
  BLINK_MEDIA_LOG << "sourceWasAdded(" << (void*)this << ") - 'src' is "
                  << UrlForLoggingMedia(url);

  // We should only consider a <source> element when there is not src attribute
  // at all.
  if (FastHasAttribute(srcAttr))
    return;

  // 4.8.8 - If a source element is inserted as a child of a media element that
  // has no src attribute and whose networkState has the value NETWORK_EMPTY,
  // the user agent must invoke the media element's resource selection
  // algorithm.
  if (getNetworkState() == HTMLMediaElement::kNetworkEmpty) {
    InvokeResourceSelectionAlgorithm();
    // Ignore current |next_child_node_to_consider_| and consider |source|.
    next_child_node_to_consider_ = source;
    return;
  }

  if (current_source_node_ && source == current_source_node_->nextSibling()) {
    BLINK_MEDIA_LOG << "sourceWasAdded(" << (void*)this
                    << ") - <source> inserted immediately after current source";
    // Ignore current |next_child_node_to_consider_| and consider |source|.
    next_child_node_to_consider_ = source;
    return;
  }

  // Consider current |next_child_node_to_consider_| as it is already in the
  // middle of processing.
  if (next_child_node_to_consider_)
    return;

  if (load_state_ != kWaitingForSource)
    return;

  // 4.8.9.5, resource selection algorithm, source elements section:
  // 21. Wait until the node after pointer is a node other than the end of the
  // list. (This step might wait forever.)
  // 22. Asynchronously await a stable state...
  // 23. Set the element's delaying-the-load-event flag back to true (this
  // delays the load event again, in case it hasn't been fired yet).
  SetShouldDelayLoadEvent(true);

  // 24. Set the networkState back to NETWORK_LOADING.
  SetNetworkState(kNetworkLoading);

  // 25. Jump back to the find next candidate step above.
  next_child_node_to_consider_ = source;
  ScheduleNextSourceChild();
}

void HTMLMediaElement::SourceWasRemoved(HTMLSourceElement* source) {
  BLINK_MEDIA_LOG << "sourceWasRemoved(" << (void*)this << ", " << source
                  << ")";

  KURL url = source->GetNonEmptyURLAttribute(srcAttr);
  BLINK_MEDIA_LOG << "sourceWasRemoved(" << (void*)this << ") - 'src' is "
                  << UrlForLoggingMedia(url);

  if (source != current_source_node_ && source != next_child_node_to_consider_)
    return;

  if (source == next_child_node_to_consider_) {
    if (current_source_node_)
      next_child_node_to_consider_ = current_source_node_->nextSibling();
    BLINK_MEDIA_LOG << "sourceWasRemoved(" << (void*)this
                    << ") - next_child_node_to_consider_ set to "
                    << next_child_node_to_consider_.Get();
  } else if (source == current_source_node_) {
    // Clear the current source node pointer, but don't change the movie as the
    // spec says:
    // 4.8.8 - Dynamically modifying a source element and its attribute when the
    // element is already inserted in a video or audio element will have no
    // effect.
    current_source_node_ = nullptr;
    BLINK_MEDIA_LOG << "SourceWasRemoved(" << (void*)this
                    << ") - current_source_node_ set to 0";
  }
}

void HTMLMediaElement::TimeChanged() {
  BLINK_MEDIA_LOG << "timeChanged(" << (void*)this << ")";

  GetCueTimeline().UpdateActiveCues(currentTime());

  // 4.8.12.9 steps 12-14. Needed if no ReadyState change is associated with the
  // seek.
  if (seeking_ && ready_state_ >= kHaveCurrentData &&
      !GetWebMediaPlayer()->Seeking())
    FinishSeek();

  double now = CurrentPlaybackPosition();
  double dur = duration();

  // When the current playback position reaches the end of the media resource
  // when the direction of playback is forwards, then the user agent must follow
  // these steps:
  if (!std::isnan(dur) && dur && now >= dur &&
      GetDirectionOfPlayback() == kForward) {
    // If the media element has a loop attribute specified
    if (Loop()) {
      //  then seek to the earliest possible position of the media resource and
      //  abort these steps.
      Seek(EarliestPossiblePosition());
    } else {
      // Queue a task to fire a simple event named timeupdate at the media
      // element.
      ScheduleTimeupdateEvent(false);

      // If the media element has still ended playback, and the direction of
      // playback is still forwards, and paused is false,
      if (!paused_) {
        // changes paused to true and fires a simple event named pause at the
        // media element.
        paused_ = true;
        ScheduleEvent(EventTypeNames::pause);
        ScheduleRejectPlayPromises(kAbortError);
      }
      // Queue a task to fire a simple event named ended at the media element.
      ScheduleEvent(EventTypeNames::ended);
    }
  }
  UpdatePlayState();
}

void HTMLMediaElement::DurationChanged() {
  BLINK_MEDIA_LOG << "durationChanged(" << (void*)this << ")";

  // durationChanged() is triggered by media player.
  CHECK(web_media_player_);
  double new_duration = web_media_player_->Duration();

  // If the duration is changed such that the *current playback position* ends
  // up being greater than the time of the end of the media resource, then the
  // user agent must also seek to the time of the end of the media resource.
  DurationChanged(new_duration, CurrentPlaybackPosition() > new_duration);
}

void HTMLMediaElement::DurationChanged(double duration, bool request_seek) {
  BLINK_MEDIA_LOG << "durationChanged(" << (void*)this << ", " << duration
                  << ", " << BoolString(request_seek) << ")";

  // Abort if duration unchanged.
  if (duration_ == duration)
    return;

  BLINK_MEDIA_LOG << "durationChanged(" << (void*)this << ") : " << duration_
                  << " -> " << duration;
  duration_ = duration;
  ScheduleEvent(EventTypeNames::durationchange);

  if (GetLayoutObject())
    GetLayoutObject()->UpdateFromElement();

  if (request_seek)
    Seek(duration);
}

void HTMLMediaElement::PlaybackStateChanged() {
  BLINK_MEDIA_LOG << "playbackStateChanged(" << (void*)this << ")";

  if (!GetWebMediaPlayer())
    return;

  if (GetWebMediaPlayer()->Paused())
    PauseInternal();
  else
    PlayInternal();
}

void HTMLMediaElement::RequestSeek(double time) {
  // The player is the source of this seek request.
  setCurrentTime(time);
}

void HTMLMediaElement::RemoteRouteAvailabilityChanged(
    WebRemotePlaybackAvailability availability) {
  if (RemotePlaybackClient() &&
      !RuntimeEnabledFeatures::NewRemotePlaybackPipelineEnabled()) {
    // The new remote playback pipeline is using the Presentation API for
    // remote playback device availability monitoring.
    RemotePlaybackClient()->AvailabilityChanged(availability);
  }
}

bool HTMLMediaElement::HasRemoteRoutes() const {
  // TODO(mlamouri): used by MediaControlsPainter; should be refactored out.
  return RemotePlaybackClient() &&
         RemotePlaybackClient()->RemotePlaybackAvailable();
}

void HTMLMediaElement::ConnectedToRemoteDevice() {
  playing_remotely_ = true;
  if (RemotePlaybackClient())
    RemotePlaybackClient()->StateChanged(WebRemotePlaybackState::kConnecting);
}

void HTMLMediaElement::DisconnectedFromRemoteDevice() {
  playing_remotely_ = false;
  if (RemotePlaybackClient())
    RemotePlaybackClient()->StateChanged(WebRemotePlaybackState::kDisconnected);
}

void HTMLMediaElement::CancelledRemotePlaybackRequest() {
  if (RemotePlaybackClient())
    RemotePlaybackClient()->PromptCancelled();
}

void HTMLMediaElement::RemotePlaybackStarted() {
  if (RemotePlaybackClient())
    RemotePlaybackClient()->StateChanged(WebRemotePlaybackState::kConnected);
}

void HTMLMediaElement::RemotePlaybackCompatibilityChanged(const WebURL& url,
                                                          bool is_compatible) {
  if (RuntimeEnabledFeatures::NewRemotePlaybackPipelineEnabled() &&
      RemotePlaybackClient()) {
    RemotePlaybackClient()->SourceChanged(url, is_compatible);
  }
}

bool HTMLMediaElement::HasSelectedVideoTrack() {
  DCHECK(RuntimeEnabledFeatures::BackgroundVideoTrackOptimizationEnabled());

  return video_tracks_ && video_tracks_->selectedIndex() != -1;
}

WebMediaPlayer::TrackId HTMLMediaElement::GetSelectedVideoTrackId() {
  DCHECK(RuntimeEnabledFeatures::BackgroundVideoTrackOptimizationEnabled());
  DCHECK(HasSelectedVideoTrack());

  int selected_track_index = video_tracks_->selectedIndex();
  VideoTrack* track =
      video_tracks_->AnonymousIndexedGetter(selected_track_index);
  return track->id();
}

bool HTMLMediaElement::IsAutoplayingMuted() {
  return autoplay_policy_->IsAutoplayingMuted();
}

// MediaPlayerPresentation methods
void HTMLMediaElement::Repaint() {
  if (web_layer_)
    web_layer_->Invalidate();

  UpdateDisplayState();
  if (GetLayoutObject())
    GetLayoutObject()->SetShouldDoFullPaintInvalidation();
}

void HTMLMediaElement::SizeChanged() {
  BLINK_MEDIA_LOG << "sizeChanged(" << (void*)this << ")";

  DCHECK(HasVideo());  // "resize" makes no sense in absence of video.
  if (ready_state_ > kHaveNothing && IsHTMLVideoElement())
    ScheduleEvent(EventTypeNames::resize);

  if (GetLayoutObject())
    GetLayoutObject()->UpdateFromElement();
}

TimeRanges* HTMLMediaElement::buffered() const {
  if (media_source_)
    return media_source_->Buffered();

  if (!GetWebMediaPlayer())
    return TimeRanges::Create();

  return TimeRanges::Create(GetWebMediaPlayer()->Buffered());
}

TimeRanges* HTMLMediaElement::played() {
  if (playing_) {
    double time = currentTime();
    if (time > last_seek_time_)
      AddPlayedRange(last_seek_time_, time);
  }

  if (!played_time_ranges_)
    played_time_ranges_ = TimeRanges::Create();

  return played_time_ranges_->Copy();
}

TimeRanges* HTMLMediaElement::seekable() const {
  if (!GetWebMediaPlayer())
    return TimeRanges::Create();

  if (media_source_)
    return media_source_->Seekable();

  return TimeRanges::Create(GetWebMediaPlayer()->Seekable());
}

bool HTMLMediaElement::PotentiallyPlaying() const {
  // "pausedToBuffer" means the media engine's rate is 0, but only because it
  // had to stop playing when it ran out of buffered data. A movie in this state
  // is "potentially playing", modulo the checks in couldPlayIfEnoughData().
  bool paused_to_buffer =
      ready_state_maximum_ >= kHaveFutureData && ready_state_ < kHaveFutureData;
  return (paused_to_buffer || ready_state_ >= kHaveFutureData) &&
         CouldPlayIfEnoughData();
}

bool HTMLMediaElement::CouldPlayIfEnoughData() const {
  return !paused() && !EndedPlayback() && !StoppedDueToErrors();
}

bool HTMLMediaElement::EndedPlayback(LoopCondition loop_condition) const {
  double dur = duration();
  if (std::isnan(dur))
    return false;

  // 4.8.12.8 Playing the media resource

  // A media element is said to have ended playback when the element's
  // readyState attribute is HAVE_METADATA or greater,
  if (ready_state_ < kHaveMetadata)
    return false;

  // and the current playback position is the end of the media resource and the
  // direction of playback is forwards, Either the media element does not have a
  // loop attribute specified,
  double now = CurrentPlaybackPosition();
  if (GetDirectionOfPlayback() == kForward) {
    return dur > 0 && now >= dur &&
           (loop_condition == LoopCondition::kIgnored || !Loop());
  }

  // or the current playback position is the earliest possible position and the
  // direction of playback is backwards
  DCHECK_EQ(GetDirectionOfPlayback(), kBackward);
  return now <= EarliestPossiblePosition();
}

bool HTMLMediaElement::StoppedDueToErrors() const {
  if (ready_state_ >= kHaveMetadata && error_) {
    TimeRanges* seekable_ranges = seekable();
    if (!seekable_ranges->Contain(currentTime()))
      return true;
  }

  return false;
}

void HTMLMediaElement::UpdatePlayState() {
  bool is_playing = GetWebMediaPlayer() && !GetWebMediaPlayer()->Paused();
  bool should_be_playing = PotentiallyPlaying();

  BLINK_MEDIA_LOG << "updatePlayState(" << (void*)this
                  << ") - shouldBePlaying = " << BoolString(should_be_playing)
                  << ", isPlaying = " << BoolString(is_playing);

  if (should_be_playing) {
    SetDisplayMode(kVideo);

    if (!is_playing) {
      // Set rate, muted before calling play in case they were set before the
      // media engine was setup.  The media engine should just stash the rate
      // and muted values since it isn't already playing.
      GetWebMediaPlayer()->SetRate(playbackRate());
      GetWebMediaPlayer()->SetVolume(EffectiveMediaVolume());
      GetWebMediaPlayer()->Play();
    }

    StartPlaybackProgressTimer();
    playing_ = true;
  } else {  // Should not be playing right now
    if (is_playing) {
      GetWebMediaPlayer()->Pause();
    }

    playback_progress_timer_.Stop();
    playing_ = false;
    double time = currentTime();
    if (time > last_seek_time_)
      AddPlayedRange(last_seek_time_, time);
  }

  if (GetLayoutObject())
    GetLayoutObject()->UpdateFromElement();
}

void HTMLMediaElement::StopPeriodicTimers() {
  progress_event_timer_.Stop();
  playback_progress_timer_.Stop();
  check_viewport_intersection_timer_.Stop();
}

void HTMLMediaElement::
    ClearMediaPlayerAndAudioSourceProviderClientWithoutLocking() {
  GetAudioSourceProvider().SetClient(nullptr);
  if (web_media_player_) {
    audio_source_provider_.Wrap(nullptr);
    web_media_player_.reset();
  }
}

void HTMLMediaElement::ClearMediaPlayer() {
  ForgetResourceSpecificTracks();

  CloseMediaSource();

  CancelDeferredLoad();

  {
    AudioSourceProviderClientLockScope scope(*this);
    ClearMediaPlayerAndAudioSourceProviderClientWithoutLocking();
  }

  StopPeriodicTimers();
  load_timer_.Stop();

  pending_action_flags_ = 0;
  load_state_ = kWaitingForSource;

  // We can't cast if we don't have a media player.
  playing_remotely_ = false;
  RemoteRouteAvailabilityChanged(WebRemotePlaybackAvailability::kUnknown);

  if (GetLayoutObject())
    GetLayoutObject()->SetShouldDoFullPaintInvalidation();
}

void HTMLMediaElement::ContextDestroyed(ExecutionContext*) {
  BLINK_MEDIA_LOG << "contextDestroyed(" << (void*)this << ")";

  // Close the async event queue so that no events are enqueued.
  CancelPendingEventsAndCallbacks();
  async_event_queue_->Close();

  // Clear everything in the Media Element
  ClearMediaPlayer();
  ready_state_ = kHaveNothing;
  ready_state_maximum_ = kHaveNothing;
  SetNetworkState(kNetworkEmpty);
  SetShouldDelayLoadEvent(false);
  current_source_node_ = nullptr;
  official_playback_position_ = 0;
  official_playback_position_needs_update_ = true;
  GetCueTimeline().UpdateActiveCues(0);
  playing_ = false;
  paused_ = true;
  seeking_ = false;

  if (GetLayoutObject())
    GetLayoutObject()->UpdateFromElement();

  StopPeriodicTimers();

  // Ensure that hasPendingActivity() is not preventing garbage collection,
  // since otherwise this media element will simply leak.
  DCHECK(!HasPendingActivity());
}

bool HTMLMediaElement::HasPendingActivity() const {
  // The delaying-the-load-event flag is set by resource selection algorithm
  // when looking for a resource to load, before networkState has reached to
  // kNetworkLoading.
  if (should_delay_load_event_)
    return true;

  // When networkState is kNetworkLoading, progress and stalled events may be
  // fired.
  if (network_state_ == kNetworkLoading)
    return true;

  {
    // Disable potential updating of playback position, as that will
    // require v8 allocations; not allowed while GCing
    // (hasPendingActivity() is called during a v8 GC.)
    AutoReset<bool> scope(&official_playback_position_needs_update_, false);

    // When playing or if playback may continue, timeupdate events may be fired.
    if (CouldPlayIfEnoughData())
      return true;
  }

  // When the seek finishes timeupdate and seeked events will be fired.
  if (seeking_)
    return true;

  // When connected to a MediaSource, e.g. setting MediaSource.duration will
  // cause a durationchange event to be fired.
  if (media_source_)
    return true;

  // Wait for any pending events to be fired.
  if (async_event_queue_->HasPendingEvents())
    return true;

  return false;
}

bool HTMLMediaElement::IsFullscreen() const {
  return Fullscreen::IsFullscreenElement(*this);
}

void HTMLMediaElement::DidEnterFullscreen() {
  UpdateControlsVisibility();

  if (web_media_player_) {
    // FIXME: There is no embedder-side handling in layout test mode.
    if (!LayoutTestSupport::IsRunningLayoutTest())
      web_media_player_->EnteredFullscreen();
    web_media_player_->OnDisplayTypeChanged(DisplayType());
  }

  // Cache this in case the player is destroyed before leaving fullscreen.
  in_overlay_fullscreen_video_ = UsesOverlayFullscreenVideo();
  if (in_overlay_fullscreen_video_) {
    GetDocument().GetLayoutView()->Compositor()->SetNeedsCompositingUpdate(
        kCompositingUpdateRebuildTree);
  }
}

void HTMLMediaElement::DidExitFullscreen() {
  UpdateControlsVisibility();

  if (GetWebMediaPlayer()) {
    GetWebMediaPlayer()->ExitedFullscreen();
    GetWebMediaPlayer()->OnDisplayTypeChanged(DisplayType());
  }

  if (in_overlay_fullscreen_video_) {
    GetDocument().GetLayoutView()->Compositor()->SetNeedsCompositingUpdate(
        kCompositingUpdateRebuildTree);
  }
  in_overlay_fullscreen_video_ = false;
}

WebLayer* HTMLMediaElement::PlatformLayer() const {
  return web_layer_;
}

bool HTMLMediaElement::HasClosedCaptions() const {
  if (!text_tracks_)
    return false;

  for (unsigned i = 0; i < text_tracks_->length(); ++i) {
    if (text_tracks_->AnonymousIndexedGetter(i)->CanBeRendered())
      return true;
  }

  return false;
}

bool HTMLMediaElement::TextTracksVisible() const {
  return text_tracks_visible_;
}

// static
void HTMLMediaElement::AssertShadowRootChildren(ShadowRoot& shadow_root) {
#if DCHECK_IS_ON()
  // There can be up to three children: an interstitial (media remoting or
  // picture in picture), text track container, and media controls. The media
  // controls has to be the last child if present, and has to be the next
  // sibling of the text track container if both present. When present, media
  // remoting interstitial has to be the first child.
  unsigned number_of_children = shadow_root.CountChildren();
  DCHECK_LE(number_of_children, 3u);
  Node* first_child = shadow_root.firstChild();
  Node* last_child = shadow_root.lastChild();
  if (number_of_children == 1) {
    DCHECK(first_child->IsTextTrackContainer() ||
           first_child->IsMediaControls() ||
           first_child->IsMediaRemotingInterstitial() ||
           first_child->IsPictureInPictureInterstitial());
  } else if (number_of_children == 2) {
    DCHECK(first_child->IsTextTrackContainer() ||
           first_child->IsMediaRemotingInterstitial() ||
           first_child->IsPictureInPictureInterstitial());
    DCHECK(last_child->IsTextTrackContainer() || last_child->IsMediaControls());
    if (first_child->IsTextTrackContainer())
      DCHECK(last_child->IsMediaControls());
  } else if (number_of_children == 3) {
    Node* second_child = first_child->nextSibling();
    DCHECK(first_child->IsMediaRemotingInterstitial() ||
           first_child->IsPictureInPictureInterstitial());
    DCHECK(second_child->IsTextTrackContainer());
    DCHECK(last_child->IsMediaControls());
  }
#endif
}

TextTrackContainer& HTMLMediaElement::EnsureTextTrackContainer() {
  ShadowRoot& shadow_root = EnsureUserAgentShadowRoot();
  AssertShadowRootChildren(shadow_root);

  Node* first_child = shadow_root.firstChild();
  if (first_child && first_child->IsTextTrackContainer())
    return ToTextTrackContainer(*first_child);
  Node* to_be_inserted = first_child;

  if (first_child && first_child->IsMediaRemotingInterstitial()) {
    Node* second_child = first_child->nextSibling();
    if (second_child && second_child->IsTextTrackContainer())
      return ToTextTrackContainer(*second_child);
    to_be_inserted = second_child;
  }

  TextTrackContainer* text_track_container = TextTrackContainer::Create(*this);

  // The text track container should be inserted before the media controls,
  // so that they are rendered behind them.
  shadow_root.InsertBefore(text_track_container, to_be_inserted);

  AssertShadowRootChildren(shadow_root);

  return *text_track_container;
}

void HTMLMediaElement::UpdateTextTrackDisplay() {
  BLINK_MEDIA_LOG << "updateTextTrackDisplay(" << (void*)this << ")";

  EnsureTextTrackContainer().UpdateDisplay(
      *this, TextTrackContainer::kDidNotStartExposingControls);
}

void HTMLMediaElement::MediaControlsDidBecomeVisible() {
  BLINK_MEDIA_LOG << "mediaControlsDidBecomeVisible(" << (void*)this << ")";

  // When the user agent starts exposing a user interface for a video element,
  // the user agent should run the rules for updating the text track rendering
  // of each of the text tracks in the video element's list of text tracks ...
  if (IsHTMLVideoElement() && TextTracksVisible()) {
    EnsureTextTrackContainer().UpdateDisplay(
        *this, TextTrackContainer::kDidStartExposingControls);
  }
}

void HTMLMediaElement::SetTextTrackKindUserPreferenceForAllMediaElements(
    Document* document) {
  auto it = DocumentToElementSetMap().find(document);
  if (it == DocumentToElementSetMap().end())
    return;
  DCHECK(it->value);
  WeakMediaElementSet& elements = *it->value;
  for (const auto& element : elements)
    element->AutomaticTrackSelectionForUpdatedUserPreference();
}

void HTMLMediaElement::AutomaticTrackSelectionForUpdatedUserPreference() {
  if (!text_tracks_ || !text_tracks_->length())
    return;

  MarkCaptionAndSubtitleTracksAsUnconfigured();
  processing_preference_change_ = true;
  text_tracks_visible_ = false;
  HonorUserPreferencesForAutomaticTextTrackSelection();
  processing_preference_change_ = false;

  // If a track is set to 'showing' post performing automatic track selection,
  // set text tracks state to visible to update the CC button and display the
  // track.
  text_tracks_visible_ = text_tracks_->HasShowingTracks();
  UpdateTextTrackDisplay();
}

void HTMLMediaElement::MarkCaptionAndSubtitleTracksAsUnconfigured() {
  if (!text_tracks_)
    return;

  // Mark all tracks as not "configured" so that
  // honorUserPreferencesForAutomaticTextTrackSelection() will reconsider
  // which tracks to display in light of new user preferences (e.g. default
  // tracks should not be displayed if the user has turned off captions and
  // non-default tracks should be displayed based on language preferences if
  // the user has turned captions on).
  for (unsigned i = 0; i < text_tracks_->length(); ++i) {
    TextTrack* text_track = text_tracks_->AnonymousIndexedGetter(i);
    if (text_track->IsVisualKind())
      text_track->SetHasBeenConfigured(false);
  }
}

unsigned HTMLMediaElement::webkitAudioDecodedByteCount() const {
  if (!GetWebMediaPlayer())
    return 0;
  return GetWebMediaPlayer()->AudioDecodedByteCount();
}

unsigned HTMLMediaElement::webkitVideoDecodedByteCount() const {
  if (!GetWebMediaPlayer())
    return 0;
  return GetWebMediaPlayer()->VideoDecodedByteCount();
}

bool HTMLMediaElement::IsURLAttribute(const Attribute& attribute) const {
  return attribute.GetName() == srcAttr ||
         HTMLElement::IsURLAttribute(attribute);
}

void HTMLMediaElement::SetShouldDelayLoadEvent(bool should_delay) {
  if (should_delay_load_event_ == should_delay)
    return;

  BLINK_MEDIA_LOG << "setShouldDelayLoadEvent(" << (void*)this << ", "
                  << BoolString(should_delay) << ")";

  should_delay_load_event_ = should_delay;
  if (should_delay)
    GetDocument().IncrementLoadEventDelayCount();
  else
    GetDocument().DecrementLoadEventDelayCount();
}

MediaControls* HTMLMediaElement::GetMediaControls() const {
  return media_controls_;
}

void HTMLMediaElement::EnsureMediaControls() {
  if (GetMediaControls())
    return;

  ShadowRoot& shadow_root = EnsureUserAgentShadowRoot();
  media_controls_ =
      CoreInitializer::GetInstance().CreateMediaControls(*this, shadow_root);

  // The media controls should be inserted after the text track container,
  // so that they are rendered in front of captions and subtitles. This check
  // is verifying the contract.
  AssertShadowRootChildren(shadow_root);
}

void HTMLMediaElement::UpdateControlsVisibility() {
  if (!isConnected()) {
    if (GetMediaControls())
      GetMediaControls()->Hide();
    return;
  }

  bool native_controls = ShouldShowControls(RecordMetricsBehavior::kDoRecord);

  // When LazyInitializeMediaControls is enabled, initialize the controls only
  // if native controls should be used or if using the cast overlay.
  if (!RuntimeEnabledFeatures::LazyInitializeMediaControlsEnabled() ||
      RuntimeEnabledFeatures::MediaCastOverlayButtonEnabled() ||
      native_controls) {
    EnsureMediaControls();

    // TODO(mlamouri): this doesn't sound needed but the following tests, on
    // Android fails when removed:
    // fullscreen/compositor-touch-hit-rects-fullscreen-video-controls.html
    GetMediaControls()->Reset();
  }

  if (native_controls)
    GetMediaControls()->MaybeShow();
  else if (GetMediaControls())
    GetMediaControls()->Hide();

  if (web_media_player_)
    web_media_player_->OnHasNativeControlsChanged(native_controls);
}

CueTimeline& HTMLMediaElement::GetCueTimeline() {
  if (!cue_timeline_)
    cue_timeline_ = new CueTimeline(*this);
  return *cue_timeline_;
}

void HTMLMediaElement::ConfigureTextTrackDisplay() {
  DCHECK(text_tracks_);
  BLINK_MEDIA_LOG << "configureTextTrackDisplay(" << (void*)this << ")";

  if (processing_preference_change_)
    return;

  bool have_visible_text_track = text_tracks_->HasShowingTracks();
  text_tracks_visible_ = have_visible_text_track;

  if (!have_visible_text_track && !GetMediaControls())
    return;

  GetCueTimeline().UpdateActiveCues(currentTime());

  // Note: The "time marches on" algorithm (updateActiveCues) runs the "rules
  // for updating the text track rendering" (updateTextTrackDisplay) only for
  // "affected tracks", i.e. tracks where the the active cues have changed.
  // This misses cues in tracks that changed mode between hidden and showing.
  // This appears to be a spec bug, which we work around here:
  // https://www.w3.org/Bugs/Public/show_bug.cgi?id=28236
  UpdateTextTrackDisplay();
}

// TODO(srirama.m): Merge it to resetMediaElement if possible and remove it.
void HTMLMediaElement::ResetMediaPlayerAndMediaSource() {
  CloseMediaSource();

  {
    AudioSourceProviderClientLockScope scope(*this);
    ClearMediaPlayerAndAudioSourceProviderClientWithoutLocking();
  }

  // We haven't yet found out if any remote routes are available.
  playing_remotely_ = false;
  RemoteRouteAvailabilityChanged(WebRemotePlaybackAvailability::kUnknown);

  if (audio_source_node_)
    GetAudioSourceProvider().SetClient(audio_source_node_);
}

void HTMLMediaElement::SetAudioSourceNode(
    AudioSourceProviderClient* source_node) {
  DCHECK(IsMainThread());
  audio_source_node_ = source_node;

  // No need to lock the |audio_source_node| because it locks itself when
  // setFormat() is invoked.
  GetAudioSourceProvider().SetClient(audio_source_node_);
}

WebMediaPlayer::CORSMode HTMLMediaElement::CorsMode() const {
  const AtomicString& cross_origin_mode = FastGetAttribute(crossoriginAttr);
  if (cross_origin_mode.IsNull())
    return WebMediaPlayer::kCORSModeUnspecified;
  if (DeprecatedEqualIgnoringCase(cross_origin_mode, "use-credentials"))
    return WebMediaPlayer::kCORSModeUseCredentials;
  return WebMediaPlayer::kCORSModeAnonymous;
}

void HTMLMediaElement::SetWebLayer(WebLayer* web_layer) {
  if (web_layer == web_layer_)
    return;

  // If either of the layers is null we need to enable or disable compositing.
  // This is done by triggering a style recalc.
  if (!web_layer_ || !web_layer)
    SetNeedsCompositingUpdate();

  if (web_layer_)
    GraphicsLayer::UnregisterContentsLayer(web_layer_);
  web_layer_ = web_layer;
  if (web_layer_)
    GraphicsLayer::RegisterContentsLayer(web_layer_);
}

void HTMLMediaElement::MediaSourceOpened(WebMediaSource* web_media_source) {
  SetShouldDelayLoadEvent(false);
  media_source_->SetWebMediaSourceAndOpen(base::WrapUnique(web_media_source));
}

bool HTMLMediaElement::IsInteractiveContent() const {
  return FastHasAttribute(controlsAttr);
}

void HTMLMediaElement::Trace(blink::Visitor* visitor) {
  visitor->Trace(played_time_ranges_);
  visitor->Trace(async_event_queue_);
  visitor->Trace(error_);
  visitor->Trace(current_source_node_);
  visitor->Trace(next_child_node_to_consider_);
  visitor->Trace(media_source_);
  visitor->Trace(audio_tracks_);
  visitor->Trace(video_tracks_);
  visitor->Trace(cue_timeline_);
  visitor->Trace(text_tracks_);
  visitor->Trace(text_tracks_when_resource_selection_began_);
  visitor->Trace(play_promise_resolvers_);
  visitor->Trace(play_promise_resolve_list_);
  visitor->Trace(play_promise_reject_list_);
  visitor->Trace(audio_source_provider_);
  visitor->Trace(src_object_);
  visitor->Trace(autoplay_policy_);
  visitor->Trace(media_controls_);
  visitor->Trace(controls_list_);
  visitor->template RegisterWeakMembers<HTMLMediaElement,
                                        &HTMLMediaElement::ClearWeakMembers>(
      this);
  Supplementable<HTMLMediaElement>::Trace(visitor);
  HTMLElement::Trace(visitor);
  PausableObject::Trace(visitor);
}

void HTMLMediaElement::TraceWrappers(
    const ScriptWrappableVisitor* visitor) const {
  visitor->TraceWrappers(video_tracks_);
  visitor->TraceWrappers(audio_tracks_);
  visitor->TraceWrappers(text_tracks_);
  HTMLElement::TraceWrappers(visitor);
  Supplementable<HTMLMediaElement>::TraceWrappers(visitor);
}

void HTMLMediaElement::CreatePlaceholderTracksIfNecessary() {
  if (!MediaTracksEnabledInternally())
    return;

  // Create a placeholder audio track if the player says it has audio but it
  // didn't explicitly announce the tracks.
  if (HasAudio() && !audioTracks().length()) {
    AddAudioTrack("audio", WebMediaPlayerClient::kAudioTrackKindMain,
                  "Audio Track", "", false);
  }

  // Create a placeholder video track if the player says it has video but it
  // didn't explicitly announce the tracks.
  if (HasVideo() && !videoTracks().length()) {
    AddVideoTrack("video", WebMediaPlayerClient::kVideoTrackKindMain,
                  "Video Track", "", false);
  }
}

void HTMLMediaElement::SelectInitialTracksIfNecessary() {
  if (!MediaTracksEnabledInternally())
    return;

  // Enable the first audio track if an audio track hasn't been enabled yet.
  if (audioTracks().length() > 0 && !audioTracks().HasEnabledTrack())
    audioTracks().AnonymousIndexedGetter(0)->setEnabled(true);

  // Select the first video track if a video track hasn't been selected yet.
  if (videoTracks().length() > 0 && videoTracks().selectedIndex() == -1)
    videoTracks().AnonymousIndexedGetter(0)->setSelected(true);
}

void HTMLMediaElement::SetNetworkState(NetworkState state) {
  if (network_state_ == state)
    return;

  network_state_ = state;
  if (GetMediaControls())
    GetMediaControls()->NetworkStateChanged();
}

void HTMLMediaElement::VideoWillBeDrawnToCanvas() const {
  DCHECK(IsHTMLVideoElement());
  UseCounter::Count(GetDocument(), WebFeature::kVideoInCanvas);
  autoplay_policy_->VideoWillBeDrawnToCanvas();
}

void HTMLMediaElement::ScheduleResolvePlayPromises() {
  // TODO(mlamouri): per spec, we should create a new task but we can't create
  // a new cancellable task without cancelling the previous one. There are two
  // approaches then: cancel the previous task and create a new one with the
  // appended promise list or append the new promise to the current list. The
  // latter approach is preferred because it might be the less observable
  // change.
  DCHECK(play_promise_resolve_list_.IsEmpty() ||
         play_promise_resolve_task_handle_.IsActive());
  if (play_promise_resolvers_.IsEmpty())
    return;

  play_promise_resolve_list_.AppendVector(play_promise_resolvers_);
  play_promise_resolvers_.clear();

  if (play_promise_resolve_task_handle_.IsActive())
    return;

  play_promise_resolve_task_handle_ = PostCancellableTask(
      *GetDocument().GetTaskRunner(TaskType::kMediaElementEvent), FROM_HERE,
      WTF::Bind(&HTMLMediaElement::ResolveScheduledPlayPromises,
                WrapWeakPersistent(this)));
}

void HTMLMediaElement::ScheduleRejectPlayPromises(ExceptionCode code) {
  // TODO(mlamouri): per spec, we should create a new task but we can't create
  // a new cancellable task without cancelling the previous one. There are two
  // approaches then: cancel the previous task and create a new one with the
  // appended promise list or append the new promise to the current list. The
  // latter approach is preferred because it might be the less observable
  // change.
  DCHECK(play_promise_reject_list_.IsEmpty() ||
         play_promise_reject_task_handle_.IsActive());
  if (play_promise_resolvers_.IsEmpty())
    return;

  play_promise_reject_list_.AppendVector(play_promise_resolvers_);
  play_promise_resolvers_.clear();

  if (play_promise_reject_task_handle_.IsActive())
    return;

  // TODO(nhiroki): Bind this error code to a cancellable task instead of a
  // member field.
  play_promise_error_code_ = code;
  play_promise_reject_task_handle_ = PostCancellableTask(
      *GetDocument().GetTaskRunner(TaskType::kMediaElementEvent), FROM_HERE,
      WTF::Bind(&HTMLMediaElement::RejectScheduledPlayPromises,
                WrapWeakPersistent(this)));
}

void HTMLMediaElement::ScheduleNotifyPlaying() {
  ScheduleEvent(EventTypeNames::playing);
  ScheduleResolvePlayPromises();
}

void HTMLMediaElement::ResolveScheduledPlayPromises() {
  for (auto& resolver : play_promise_resolve_list_)
    resolver->Resolve();

  play_promise_resolve_list_.clear();
}

void HTMLMediaElement::RejectScheduledPlayPromises() {
  // TODO(mlamouri): the message is generated based on the code because
  // arguments can't be passed to a cancellable task. In order to save space
  // used by the object, the string isn't saved.
  DCHECK(play_promise_error_code_ == kAbortError ||
         play_promise_error_code_ == kNotSupportedError);
  if (play_promise_error_code_ == kAbortError) {
    RecordPlayPromiseRejected(PlayPromiseRejectReason::kInterruptedByPause);
    RejectPlayPromisesInternal(kAbortError,
                               "The play() request was interrupted by a call "
                               "to pause(). https://goo.gl/LdLk22");
  } else {
    RecordPlayPromiseRejected(PlayPromiseRejectReason::kNoSupportedSources);
    RejectPlayPromisesInternal(
        kNotSupportedError,
        "Failed to load because no supported source was found.");
  }
}

void HTMLMediaElement::RejectPlayPromises(ExceptionCode code,
                                          const String& message) {
  play_promise_reject_list_.AppendVector(play_promise_resolvers_);
  play_promise_resolvers_.clear();
  RejectPlayPromisesInternal(code, message);
}

void HTMLMediaElement::RejectPlayPromisesInternal(ExceptionCode code,
                                                  const String& message) {
  DCHECK(code == kAbortError || code == kNotSupportedError);

  for (auto& resolver : play_promise_reject_list_)
    resolver->Reject(DOMException::Create(code, message));

  play_promise_reject_list_.clear();
}

EnumerationHistogram& HTMLMediaElement::ShowControlsHistogram() const {
  if (IsHTMLVideoElement()) {
    DEFINE_STATIC_LOCAL(EnumerationHistogram, histogram,
                        ("Media.Controls.Show.Video", kMediaControlsShowMax));
    return histogram;
  }

  DEFINE_STATIC_LOCAL(EnumerationHistogram, histogram,
                      ("Media.Controls.Show.Audio", kMediaControlsShowMax));
  return histogram;
}

void HTMLMediaElement::ClearWeakMembers(Visitor* visitor) {
  if (!ThreadHeap::IsHeapObjectAlive(audio_source_node_)) {
    GetAudioSourceProvider().SetClient(nullptr);
    audio_source_node_ = nullptr;
  }
}

void HTMLMediaElement::AudioSourceProviderImpl::Wrap(
    WebAudioSourceProvider* provider) {
  MutexLocker locker(provide_input_lock);

  if (web_audio_source_provider_ && provider != web_audio_source_provider_)
    web_audio_source_provider_->SetClient(nullptr);

  web_audio_source_provider_ = provider;
  if (web_audio_source_provider_)
    web_audio_source_provider_->SetClient(client_.Get());
}

void HTMLMediaElement::AudioSourceProviderImpl::SetClient(
    AudioSourceProviderClient* client) {
  MutexLocker locker(provide_input_lock);

  if (client)
    client_ = new HTMLMediaElement::AudioClientImpl(client);
  else
    client_.Clear();

  if (web_audio_source_provider_)
    web_audio_source_provider_->SetClient(client_.Get());
}

void HTMLMediaElement::AudioSourceProviderImpl::ProvideInput(
    AudioBus* bus,
    size_t frames_to_process) {
  DCHECK(bus);

  MutexTryLocker try_locker(provide_input_lock);
  if (!try_locker.Locked() || !web_audio_source_provider_ || !client_.Get()) {
    bus->Zero();
    return;
  }

  // Wrap the AudioBus channel data using WebVector.
  size_t n = bus->NumberOfChannels();
  WebVector<float*> web_audio_data(n);
  for (size_t i = 0; i < n; ++i)
    web_audio_data[i] = bus->Channel(i)->MutableData();

  web_audio_source_provider_->ProvideInput(web_audio_data, frames_to_process);
}

void HTMLMediaElement::AudioClientImpl::SetFormat(size_t number_of_channels,
                                                  float sample_rate) {
  if (client_)
    client_->SetFormat(number_of_channels, sample_rate);
}

void HTMLMediaElement::AudioClientImpl::Trace(blink::Visitor* visitor) {
  visitor->Trace(client_);
}

void HTMLMediaElement::AudioSourceProviderImpl::Trace(blink::Visitor* visitor) {
  visitor->Trace(client_);
}

void HTMLMediaElement::ActivateViewportIntersectionMonitoring(bool activate) {
  if (activate && !check_viewport_intersection_timer_.IsActive()) {
    check_viewport_intersection_timer_.StartRepeating(
        kCheckViewportIntersectionInterval, FROM_HERE);
  } else if (!activate) {
    check_viewport_intersection_timer_.Stop();
  }
}

bool HTMLMediaElement::HasNativeControls() {
  return ShouldShowControls(RecordMetricsBehavior::kDoRecord);
}

bool HTMLMediaElement::IsAudioElement() {
  return IsHTMLAudioElement();
}

WebMediaPlayer::DisplayType HTMLMediaElement::DisplayType() const {
  return IsFullscreen() ? WebMediaPlayer::DisplayType::kFullscreen
                        : WebMediaPlayer::DisplayType::kInline;
}

gfx::ColorSpace HTMLMediaElement::TargetColorSpace() {
  const LocalFrame* frame = GetDocument().GetFrame();
  if (!frame)
    return gfx::ColorSpace();
  return frame->GetPage()->GetChromeClient().GetScreenInfo().color_space;
}

bool HTMLMediaElement::WasAutoplayInitiated() {
  return autoplay_policy_->WasAutoplayInitiated();
}

void HTMLMediaElement::CheckViewportIntersectionTimerFired(TimerBase*) {
  bool should_report_root_bounds = true;
  IntersectionGeometry geometry(nullptr, *this, Vector<Length>(),
                                should_report_root_bounds);
  geometry.ComputeGeometry();
  IntRect intersect_rect = geometry.IntersectionIntRect();
  if (current_intersect_rect_ == intersect_rect)
    return;

  current_intersect_rect_ = intersect_rect;
  bool is_mostly_filling_viewport =
      (current_intersect_rect_.Size().Area() >
       kMostlyFillViewportThreshold * geometry.RootIntRect().Size().Area());
  if (mostly_filling_viewport_ == is_mostly_filling_viewport)
    return;

  mostly_filling_viewport_ = is_mostly_filling_viewport;
  if (web_media_player_)
    web_media_player_->BecameDominantVisibleContent(mostly_filling_viewport_);
}

STATIC_ASSERT_ENUM(WebMediaPlayer::kReadyStateHaveNothing,
                   HTMLMediaElement::kHaveNothing);
STATIC_ASSERT_ENUM(WebMediaPlayer::kReadyStateHaveMetadata,
                   HTMLMediaElement::kHaveMetadata);
STATIC_ASSERT_ENUM(WebMediaPlayer::kReadyStateHaveCurrentData,
                   HTMLMediaElement::kHaveCurrentData);
STATIC_ASSERT_ENUM(WebMediaPlayer::kReadyStateHaveFutureData,
                   HTMLMediaElement::kHaveFutureData);
STATIC_ASSERT_ENUM(WebMediaPlayer::kReadyStateHaveEnoughData,
                   HTMLMediaElement::kHaveEnoughData);

}  // namespace blink
