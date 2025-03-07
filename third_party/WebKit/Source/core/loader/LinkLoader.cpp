/*
 * Copyright (C) 2011 Google Inc. All rights reserved.
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
 *
 */

#include "core/loader/LinkLoader.h"

#include "bindings/core/v8/V8BindingForCore.h"
#include "core/css/MediaList.h"
#include "core/css/MediaQueryEvaluator.h"
#include "core/css/parser/SizesAttributeParser.h"
#include "core/dom/Document.h"
#include "core/frame/FrameConsole.h"
#include "core/frame/LocalFrame.h"
#include "core/frame/Settings.h"
#include "core/frame/UseCounter.h"
#include "core/html/CrossOriginAttribute.h"
#include "core/html/LinkRelAttribute.h"
#include "core/html/parser/HTMLPreloadScanner.h"
#include "core/html/parser/HTMLSrcsetParser.h"
#include "core/inspector/ConsoleMessage.h"
#include "core/loader/DocumentLoader.h"
#include "core/loader/NetworkHintsInterface.h"
#include "core/loader/SubresourceIntegrityHelper.h"
#include "core/loader/modulescript/ModuleScriptFetchRequest.h"
#include "core/loader/private/PrerenderHandle.h"
#include "core/loader/resource/LinkFetchResource.h"
#include "core/script/ModuleScript.h"
#include "core/script/ScriptLoader.h"
#include "platform/Prerender.h"
#include "platform/loader/LinkHeader.h"
#include "platform/loader/SubresourceIntegrity.h"
#include "platform/loader/fetch/FetchParameters.h"
#include "platform/loader/fetch/ResourceClient.h"
#include "platform/loader/fetch/ResourceFetcher.h"
#include "platform/loader/fetch/ResourceFinishObserver.h"
#include "platform/loader/fetch/ResourceLoaderOptions.h"
#include "platform/loader/fetch/fetch_initiator_type_names.h"
#include "platform/network/mime/MIMETypeRegistry.h"
#include "public/platform/WebPrerender.h"

namespace blink {

static unsigned PrerenderRelTypesFromRelAttribute(
    const LinkRelAttribute& rel_attribute,
    Document& document) {
  unsigned result = 0;
  if (rel_attribute.IsLinkPrerender()) {
    result |= kPrerenderRelTypePrerender;
    UseCounter::Count(document, WebFeature::kLinkRelPrerender);
  }
  if (rel_attribute.IsLinkNext()) {
    result |= kPrerenderRelTypeNext;
    UseCounter::Count(document, WebFeature::kLinkRelNext);
  }

  return result;
}

LinkLoadParameters::LinkLoadParameters(const LinkHeader& header,
                                       const KURL& base_url)
    : rel(LinkRelAttribute(header.Rel())),
      cross_origin(GetCrossOriginAttributeValue(header.CrossOrigin())),
      type(header.MimeType()),
      as(header.As()),
      media(header.Media()),
      nonce(header.Nonce()),
      integrity(header.Integrity()),
      referrer_policy(kReferrerPolicyDefault),
      href(KURL(base_url, header.Url())),
      srcset(header.Srcset()),
      sizes(header.Imgsizes()) {}

class LinkLoader::FinishObserver final
    : public GarbageCollectedFinalized<ResourceFinishObserver>,
      public ResourceFinishObserver {
  USING_GARBAGE_COLLECTED_MIXIN(FinishObserver);
  USING_PRE_FINALIZER(FinishObserver, ClearResource);

 public:
  FinishObserver(LinkLoader* loader, Resource* resource)
      : loader_(loader), resource_(resource) {
    resource_->AddFinishObserver(
        this, loader_->client_->GetLoadingTaskRunner().get());
  }

  // ResourceFinishObserver implementation
  void NotifyFinished() override {
    if (!resource_)
      return;
    loader_->NotifyFinished();
    ClearResource();
  }
  String DebugName() const override {
    return "LinkLoader::ResourceFinishObserver";
  }

  Resource* GetResource() { return resource_; }
  void ClearResource() {
    if (!resource_)
      return;
    resource_->RemoveFinishObserver(this);
    resource_ = nullptr;
  }

  void Trace(blink::Visitor* visitor) override {
    visitor->Trace(loader_);
    visitor->Trace(resource_);
    blink::ResourceFinishObserver::Trace(visitor);
  }

 private:
  Member<LinkLoader> loader_;
  Member<Resource> resource_;
};

LinkLoader::LinkLoader(LinkLoaderClient* client,
                       scoped_refptr<base::SingleThreadTaskRunner> task_runner)
    : client_(client) {
  DCHECK(client_);
}

LinkLoader::~LinkLoader() = default;

void LinkLoader::NotifyFinished() {
  DCHECK(finish_observer_);
  Resource* resource = finish_observer_->GetResource();
  if (resource->ErrorOccurred())
    client_->LinkLoadingErrored();
  else
    client_->LinkLoaded();
}

// https://html.spec.whatwg.org/#link-type-modulepreload
void LinkLoader::NotifyModuleLoadFinished(ModuleScript* module) {
  // Step 11. "If result is null, fire an event named error at the link element,
  // and return." [spec text]
  // Step 12. "Fire an event named load at the link element." [spec text]
  if (!module)
    client_->LinkLoadingErrored();
  else
    client_->LinkLoaded();
}

void LinkLoader::DidStartPrerender() {
  client_->DidStartLinkPrerender();
}

void LinkLoader::DidStopPrerender() {
  client_->DidStopLinkPrerender();
}

void LinkLoader::DidSendLoadForPrerender() {
  client_->DidSendLoadForLinkPrerender();
}

void LinkLoader::DidSendDOMContentLoadedForPrerender() {
  client_->DidSendDOMContentLoadedForLinkPrerender();
}

enum LinkCaller {
  kLinkCalledFromHeader,
  kLinkCalledFromMarkup,
};

static void SendMessageToConsoleForPossiblyNullDocument(
    ConsoleMessage* console_message,
    Document* document,
    LocalFrame* frame) {
  DCHECK(document || frame);
  DCHECK(!document || document->GetFrame() == frame);
  // Route the console message through Document if possible, so that script line
  // numbers can be included. Otherwise, route directly to the FrameConsole, to
  // ensure we never drop a message.
  if (document)
    document->AddConsoleMessage(console_message);
  else
    frame->Console().AddMessage(console_message);
}

static void DnsPrefetchIfNeeded(
    const LinkLoadParameters& params,
    Document* document,
    LocalFrame* frame,
    const NetworkHintsInterface& network_hints_interface,
    LinkCaller caller) {
  if (params.rel.IsDNSPrefetch()) {
    UseCounter::Count(frame, WebFeature::kLinkRelDnsPrefetch);
    if (caller == kLinkCalledFromHeader)
      UseCounter::Count(frame, WebFeature::kLinkHeaderDnsPrefetch);
    Settings* settings = frame ? frame->GetSettings() : nullptr;
    // FIXME: The href attribute of the link element can be in "//hostname"
    // form, and we shouldn't attempt to complete that as URL
    // <https://bugs.webkit.org/show_bug.cgi?id=48857>.
    if (settings && settings->GetDNSPrefetchingEnabled() &&
        params.href.IsValid() && !params.href.IsEmpty()) {
      if (settings->GetLogDnsPrefetchAndPreconnect()) {
        SendMessageToConsoleForPossiblyNullDocument(
            ConsoleMessage::Create(
                kOtherMessageSource, kVerboseMessageLevel,
                String("DNS prefetch triggered for " + params.href.Host())),
            document, frame);
      }
      network_hints_interface.DnsPrefetchHost(params.href.Host());
    }
  }
}

static void PreconnectIfNeeded(
    const LinkLoadParameters& params,
    Document* document,
    LocalFrame* frame,
    const NetworkHintsInterface& network_hints_interface,
    LinkCaller caller) {
  if (params.rel.IsPreconnect() && params.href.IsValid() &&
      params.href.ProtocolIsInHTTPFamily()) {
    UseCounter::Count(frame, WebFeature::kLinkRelPreconnect);
    if (caller == kLinkCalledFromHeader)
      UseCounter::Count(frame, WebFeature::kLinkHeaderPreconnect);
    Settings* settings = frame ? frame->GetSettings() : nullptr;
    if (settings && settings->GetLogDnsPrefetchAndPreconnect()) {
      SendMessageToConsoleForPossiblyNullDocument(
          ConsoleMessage::Create(
              kOtherMessageSource, kVerboseMessageLevel,
              String("Preconnect triggered for ") + params.href.GetString()),
          document, frame);
      if (params.cross_origin != kCrossOriginAttributeNotSet) {
        SendMessageToConsoleForPossiblyNullDocument(
            ConsoleMessage::Create(kOtherMessageSource, kVerboseMessageLevel,
                                   String("Preconnect CORS setting is ") +
                                       String((params.cross_origin ==
                                               kCrossOriginAttributeAnonymous)
                                                  ? "anonymous"
                                                  : "use-credentials")),
            document, frame);
      }
    }
    network_hints_interface.PreconnectHost(params.href, params.cross_origin);
  }
}

WTF::Optional<Resource::Type> LinkLoader::GetResourceTypeFromAsAttribute(
    const String& as) {
  DCHECK_EQ(as.DeprecatedLower(), as);
  if (as == "image") {
    return Resource::kImage;
  } else if (as == "script") {
    return Resource::kScript;
  } else if (as == "style") {
    return Resource::kCSSStyleSheet;
  } else if (as == "video") {
    return Resource::kVideo;
  } else if (as == "audio") {
    return Resource::kAudio;
  } else if (as == "track") {
    return Resource::kTextTrack;
  } else if (as == "font") {
    return Resource::kFont;
  } else if (as == "fetch") {
    return Resource::kRaw;
  }
  return WTF::nullopt;
}

Resource* LinkLoader::GetResourceForTesting() {
  return finish_observer_ ? finish_observer_->GetResource() : nullptr;
}

static bool IsSupportedType(Resource::Type resource_type,
                            const String& mime_type) {
  if (mime_type.IsEmpty())
    return true;
  switch (resource_type) {
    case Resource::kImage:
      return MIMETypeRegistry::IsSupportedImagePrefixedMIMEType(mime_type);
    case Resource::kScript:
      return MIMETypeRegistry::IsSupportedJavaScriptMIMEType(mime_type);
    case Resource::kCSSStyleSheet:
      return MIMETypeRegistry::IsSupportedStyleSheetMIMEType(mime_type);
    case Resource::kFont:
      return MIMETypeRegistry::IsSupportedFontMIMEType(mime_type);
    case Resource::kAudio:
    case Resource::kVideo:
      return MIMETypeRegistry::IsSupportedMediaMIMEType(mime_type, String());
    case Resource::kTextTrack:
      return MIMETypeRegistry::IsSupportedTextTrackMIMEType(mime_type);
    case Resource::kRaw:
      return true;
    default:
      NOTREACHED();
  }
  return false;
}

static MediaValues* CreateMediaValues(
    Document& document,
    ViewportDescription* viewport_description) {
  MediaValues* media_values =
      MediaValues::CreateDynamicIfFrameExists(document.GetFrame());
  if (viewport_description) {
    media_values->OverrideViewportDimensions(
        viewport_description->max_width.GetFloatValue(),
        viewport_description->max_height.GetFloatValue());
  }
  return media_values;
}

static bool MediaMatches(const String& media, MediaValues* media_values) {
  scoped_refptr<MediaQuerySet> media_queries = MediaQuerySet::Create(media);
  MediaQueryEvaluator evaluator(*media_values);
  return evaluator.Eval(*media_queries);
}

// |base_url| is used in Link HTTP Header based preloads to resolve relative
// URLs in srcset, which should be based on the resource's URL, not the
// document's base URL. If |base_url| is a null URL, relative URLs are resolved
// using |document.CompleteURL()|.
static Resource* PreloadIfNeeded(const LinkLoadParameters& params,
                                 Document& document,
                                 const KURL& base_url,
                                 LinkCaller caller,
                                 ViewportDescription* viewport_description) {
  if (!document.Loader() || !params.rel.IsLinkPreload())
    return nullptr;

  Optional<Resource::Type> resource_type =
      LinkLoader::GetResourceTypeFromAsAttribute(params.as);

  MediaValues* media_values = nullptr;
  KURL url;
  if (resource_type == Resource::kImage && !params.srcset.IsEmpty() &&
      RuntimeEnabledFeatures::PreloadImageSrcSetEnabled()) {
    media_values = CreateMediaValues(document, viewport_description);
    float source_size =
        SizesAttributeParser(media_values, params.sizes).length();
    ImageCandidate candidate = BestFitSourceForImageAttributes(
        media_values->DevicePixelRatio(), source_size, params.href,
        params.srcset);
    url = base_url.IsNull() ? document.CompleteURL(candidate.ToString())
                            : KURL(base_url, candidate.ToString());
  } else {
    url = params.href;
  }

  UseCounter::Count(document, WebFeature::kLinkRelPreload);
  if (!url.IsValid() || url.IsEmpty()) {
    document.AddConsoleMessage(ConsoleMessage::Create(
        kOtherMessageSource, kWarningMessageLevel,
        String("<link rel=preload> has an invalid `href` value")));
    return nullptr;
  }

  // Preload only if media matches
  if (!params.media.IsEmpty()) {
    if (!media_values)
      media_values = CreateMediaValues(document, viewport_description);
    if (!MediaMatches(params.media, media_values))
      return nullptr;
  }

  if (caller == kLinkCalledFromHeader)
    UseCounter::Count(document, WebFeature::kLinkHeaderPreload);
  if (resource_type == WTF::nullopt) {
    document.AddConsoleMessage(ConsoleMessage::Create(
        kOtherMessageSource, kWarningMessageLevel,
        String("<link rel=preload> must have a valid `as` value")));
    return nullptr;
  }

  if (!IsSupportedType(resource_type.value(), params.type)) {
    document.AddConsoleMessage(ConsoleMessage::Create(
        kOtherMessageSource, kWarningMessageLevel,
        String("<link rel=preload> has an unsupported `type` value")));
    return nullptr;
  }
  ResourceRequest resource_request(url);
  resource_request.SetRequestContext(ResourceFetcher::DetermineRequestContext(
      resource_type.value(), ResourceFetcher::kImageNotImageSet, false));

  if (params.referrer_policy != kReferrerPolicyDefault) {
    resource_request.SetHTTPReferrer(SecurityPolicy::GenerateReferrer(
        params.referrer_policy, url, document.OutgoingReferrer()));
  }

  ResourceLoaderOptions options;
  options.initiator_info.name = FetchInitiatorTypeNames::link;
  FetchParameters link_fetch_params(resource_request, options);
  link_fetch_params.SetCharset(document.Encoding());

  if (params.cross_origin != kCrossOriginAttributeNotSet) {
    link_fetch_params.SetCrossOriginAccessControl(document.GetSecurityOrigin(),
                                                  params.cross_origin);
  }
  link_fetch_params.SetContentSecurityPolicyNonce(params.nonce);
  Settings* settings = document.GetSettings();
  if (settings && settings->GetLogPreload()) {
    document.AddConsoleMessage(ConsoleMessage::Create(
        kOtherMessageSource, kVerboseMessageLevel,
        String("Preload triggered for " + url.Host() + url.GetPath())));
  }
  link_fetch_params.SetLinkPreload(true);
  return document.Loader()->StartPreload(resource_type.value(),
                                         link_fetch_params, nullptr);
}

// https://html.spec.whatwg.org/#link-type-modulepreload
static void ModulePreloadIfNeeded(const LinkLoadParameters& params,
                                  Document& document,
                                  ViewportDescription* viewport_description,
                                  LinkLoader* link_loader) {
  if (!document.Loader() || !params.rel.IsModulePreload())
    return;

  UseCounter::Count(document, WebFeature::kLinkRelModulePreload);

  // Step 1. "If the href attribute's value is the empty string, then return."
  // [spec text]
  if (params.href.IsEmpty()) {
    document.AddConsoleMessage(
        ConsoleMessage::Create(kOtherMessageSource, kWarningMessageLevel,
                               "<link rel=modulepreload> has no `href` value"));
    return;
  }

  // Step 2. "Let destination be the current state of the as attribute (a
  // destination), or "script" if it is in no state." [spec text]
  // Step 3. "If destination is not script-like, then queue a task on the
  // networking task source to fire an event named error at the link element,
  // and return." [spec text]
  // Currently we only support as="script".
  if (!params.as.IsEmpty() && params.as != "script") {
    document.AddConsoleMessage(ConsoleMessage::Create(
        kOtherMessageSource, kWarningMessageLevel,
        String("<link rel=modulepreload> has an invalid `as` value " +
               params.as)));
    if (link_loader)
      link_loader->DispatchLinkLoadingErroredAsync();
    return;
  }

  // Step 4. "Parse the URL given by the href attribute, relative to the
  // element's node document. If that fails, then return. Otherwise, let url be
  // the resulting URL record." [spec text]
  // |href| is already resolved in caller side.
  if (!params.href.IsValid()) {
    document.AddConsoleMessage(ConsoleMessage::Create(
        kOtherMessageSource, kWarningMessageLevel,
        "<link rel=modulepreload> has an invalid `href` value " +
            params.href.GetString()));
    return;
  }

  // Preload only if media matches.
  // https://html.spec.whatwg.org/#processing-the-media-attribute
  if (!params.media.IsEmpty()) {
    MediaValues* media_values =
        CreateMediaValues(document, viewport_description);
    if (!MediaMatches(params.media, media_values))
      return;
  }

  // Step 5. "Let settings object be the link element's node document's relevant
  // settings object." [spec text]
  // |document| is the node document here, and its context document is the
  // relevant settings object.
  Document* context_document = document.ContextDocument();

  Modulator* modulator =
      Modulator::From(ToScriptStateForMainWorld(context_document->GetFrame()));
  DCHECK(modulator);
  if (!modulator)
    return;

  // Step 6. "Let credentials mode be the module script credentials mode for the
  // crossorigin attribute." [spec text]
  network::mojom::FetchCredentialsMode credentials_mode =
      ScriptLoader::ModuleScriptCredentialsMode(params.cross_origin);

  // Step 7. "Let cryptographic nonce be the value of the nonce attribute, if it
  // is specified, or the empty string otherwise." [spec text]
  // |nonce| parameter is the value of the nonce attribute.

  // Step 8. "Let integrity metadata be the value of the integrity attribute, if
  // it is specified, or the empty string otherwise." [spec text]
  IntegrityMetadataSet integrity_metadata;
  if (!params.integrity.IsEmpty()) {
    SubresourceIntegrity::IntegrityFeatures integrity_features =
        SubresourceIntegrityHelper::GetFeatures(&document);
    SubresourceIntegrity::ReportInfo report_info;
    SubresourceIntegrity::ParseIntegrityAttribute(
        params.integrity, integrity_features, integrity_metadata, &report_info);
    SubresourceIntegrityHelper::DoReport(document, report_info);
  }

  // Step 9. "Let options be a script fetch options whose cryptographic nonce is
  // cryptographic nonce, integrity metadata is integrity metadata, parser
  // metadata is "not-parser-inserted", and credentials mode is credentials
  // mode." [spec text]
  ModuleScriptFetchRequest request(
      params.href, params.referrer_policy,
      ScriptFetchOptions(params.nonce, integrity_metadata, params.integrity,
                         kNotParserInserted, credentials_mode));

  // Step 10. "Fetch a single module script given url, settings object,
  // destination, options, settings object, "client", and with the top-level
  // module fetch flag set. Wait until algorithm asynchronously completes with
  // result." [spec text]
  modulator->FetchSingle(request, ModuleGraphLevel::kDependentModuleFetch,
                         link_loader);

  Settings* settings = document.GetSettings();
  if (settings && settings->GetLogPreload()) {
    document.AddConsoleMessage(
        ConsoleMessage::Create(kOtherMessageSource, kVerboseMessageLevel,
                               "Module preload triggered for " +
                                   params.href.Host() + params.href.GetPath()));
  }

  // Asynchronously continue processing after
  // LinkLoader::NotifyModuleLoadFinished() is called.
}

static Resource* PrefetchIfNeeded(const LinkLoadParameters& params,
                                  Document& document) {
  if (params.rel.IsLinkPrefetch() && params.href.IsValid() &&
      document.GetFrame()) {
    UseCounter::Count(document, WebFeature::kLinkRelPrefetch);

    ResourceRequest resource_request(params.href);
    if (params.referrer_policy != kReferrerPolicyDefault) {
      resource_request.SetHTTPReferrer(SecurityPolicy::GenerateReferrer(
          params.referrer_policy, params.href, document.OutgoingReferrer()));
    }

    ResourceLoaderOptions options;
    options.initiator_info.name = FetchInitiatorTypeNames::link;
    auto service = document.GetFrame()->PrefetchURLLoaderService();
    if (service) {
      network::mojom::blink::URLLoaderFactoryPtr prefetch_url_loader_factory;
      service->GetFactory(mojo::MakeRequest(&prefetch_url_loader_factory));
      options.url_loader_factory = base::MakeRefCounted<
          base::RefCountedData<network::mojom::blink::URLLoaderFactoryPtr>>(
          std::move(prefetch_url_loader_factory));
    }

    FetchParameters link_fetch_params(resource_request, options);
    if (params.cross_origin != kCrossOriginAttributeNotSet) {
      link_fetch_params.SetCrossOriginAccessControl(
          document.GetSecurityOrigin(), params.cross_origin);
    }
    return LinkFetchResource::Fetch(Resource::kLinkPrefetch, link_fetch_params,
                                    document.Fetcher());
  }
  return nullptr;
}

void LinkLoader::LoadLinksFromHeader(
    const String& header_value,
    const KURL& base_url,
    LocalFrame& frame,
    Document* document,
    const NetworkHintsInterface& network_hints_interface,
    CanLoadResources can_load_resources,
    MediaPreloadPolicy media_policy,
    ViewportDescriptionWrapper* viewport_description_wrapper) {
  if (header_value.IsEmpty())
    return;
  LinkHeaderSet header_set(header_value);
  for (auto& header : header_set) {
    if (!header.Valid() || header.Url().IsEmpty() || header.Rel().IsEmpty())
      continue;

    if (media_policy == kOnlyLoadMedia && header.Media().IsEmpty())
      continue;
    if (media_policy == kOnlyLoadNonMedia && !header.Media().IsEmpty())
      continue;

    const LinkLoadParameters params(header, base_url);
    // Sanity check to avoid re-entrancy here.
    if (params.href == base_url)
      continue;
    if (can_load_resources != kOnlyLoadResources) {
      DnsPrefetchIfNeeded(params, document, &frame, network_hints_interface,
                          kLinkCalledFromHeader);

      PreconnectIfNeeded(params, document, &frame, network_hints_interface,
                         kLinkCalledFromHeader);
    }
    if (can_load_resources != kDoNotLoadResources) {
      DCHECK(document);
      ViewportDescription* viewport_description =
          (viewport_description_wrapper && viewport_description_wrapper->set)
              ? &(viewport_description_wrapper->description)
              : nullptr;

      PreloadIfNeeded(params, *document, base_url, kLinkCalledFromHeader,
                      viewport_description);
      PrefetchIfNeeded(params, *document);
      ModulePreloadIfNeeded(params, *document, viewport_description, nullptr);
    }
    if (params.rel.IsServiceWorker()) {
      UseCounter::Count(&frame, WebFeature::kLinkHeaderServiceWorker);
    }
    // TODO(yoav): Add more supported headers as needed.
  }
}

bool LinkLoader::LoadLink(
    const LinkLoadParameters& params,
    Document& document,
    const NetworkHintsInterface& network_hints_interface) {
  // If any loading process is in progress, abort it.
  Abort();

  if (!client_->ShouldLoadLink())
    return false;

  DnsPrefetchIfNeeded(params, &document, document.GetFrame(),
                      network_hints_interface, kLinkCalledFromMarkup);

  PreconnectIfNeeded(params, &document, document.GetFrame(),
                     network_hints_interface, kLinkCalledFromMarkup);

  Resource* resource = PreloadIfNeeded(params, document, NullURL(),
                                       kLinkCalledFromMarkup, nullptr);
  if (!resource) {
    resource = PrefetchIfNeeded(params, document);
  }
  if (resource)
    finish_observer_ = new FinishObserver(this, resource);

  ModulePreloadIfNeeded(params, document, nullptr, this);

  if (const unsigned prerender_rel_types =
          PrerenderRelTypesFromRelAttribute(params.rel, document)) {
    if (!prerender_) {
      prerender_ = PrerenderHandle::Create(document, this, params.href,
                                           prerender_rel_types);
    } else if (prerender_->Url() != params.href) {
      prerender_->Cancel();
      prerender_ = PrerenderHandle::Create(document, this, params.href,
                                           prerender_rel_types);
    }
    // TODO(gavinp): Handle changes to rel types of existing prerenders.
  } else if (prerender_) {
    prerender_->Cancel();
    prerender_.Clear();
  }
  return true;
}

void LinkLoader::DispatchLinkLoadingErroredAsync() {
  client_->GetLoadingTaskRunner()->PostTask(
      FROM_HERE, WTF::Bind(&LinkLoaderClient::LinkLoadingErrored,
                           WrapPersistent(client_.Get())));
}

void LinkLoader::Abort() {
  if (prerender_) {
    prerender_->Cancel();
    prerender_.Clear();
  }
  if (finish_observer_) {
    finish_observer_->ClearResource();
    finish_observer_ = nullptr;
  }
}

void LinkLoader::Trace(blink::Visitor* visitor) {
  visitor->Trace(finish_observer_);
  visitor->Trace(client_);
  visitor->Trace(prerender_);
  SingleModuleClient::Trace(visitor);
  PrerenderClient::Trace(visitor);
}

}  // namespace blink
