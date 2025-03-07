/*
 * Copyright (C) 2013 Google Inc. All rights reserved.
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
 */

#ifndef WebEmbeddedWorkerImpl_h
#define WebEmbeddedWorkerImpl_h

#include <memory>
#include "core/exported/WorkerShadowPage.h"
#include "core/workers/GlobalScopeCreationParams.h"
#include "core/workers/WorkerClients.h"
#include "modules/ModulesExport.h"
#include "modules/serviceworkers/ServiceWorkerContentSettingsProxy.h"
#include "platform/heap/Handle.h"
#include "public/web/WebEmbeddedWorker.h"
#include "public/web/WebEmbeddedWorkerStartData.h"
#include "services/service_manager/public/cpp/interface_provider.h"
#include "services/service_manager/public/mojom/interface_provider.mojom-blink.h"

namespace blink {

class ContentSecurityPolicy;
class ServiceWorkerInstalledScriptsManager;
class WorkerClassicScriptLoader;
class WorkerInspectorProxy;
class WorkerThread;

class MODULES_EXPORT WebEmbeddedWorkerImpl final
    : public WebEmbeddedWorker,
      public WorkerShadowPage::Client {
  WTF_MAKE_NONCOPYABLE(WebEmbeddedWorkerImpl);

 public:
  WebEmbeddedWorkerImpl(
      std::unique_ptr<WebServiceWorkerContextClient>,
      std::unique_ptr<WebServiceWorkerInstalledScriptsManager>,
      std::unique_ptr<ServiceWorkerContentSettingsProxy>,
      service_manager::mojom::blink::InterfaceProviderPtrInfo);
  ~WebEmbeddedWorkerImpl() override;

  // WebEmbeddedWorker overrides.
  void StartWorkerContext(const WebEmbeddedWorkerStartData&) override;
  void TerminateWorkerContext() override;
  void ResumeAfterDownload() override;
  void AddMessageToConsole(const WebConsoleMessage&) override;
  void BindDevToolsAgent(
      mojo::ScopedInterfaceEndpointHandle devtools_agent_request) override;

  void PostMessageToPageInspector(int session_id, const WTF::String&);

  // Applies the specified CSP and referrer policy to the worker, so that
  // fetches initiated by the worker (other than for the main worker script
  // itself) are affected by these policies. This must be called before starting
  // script execution on the worker thread.
  void SetContentSecurityPolicyAndReferrerPolicy(ContentSecurityPolicy*,
                                                 String referrer_policy);

  // WorkerShadowPage::Client overrides.
  std::unique_ptr<WebApplicationCacheHost> CreateApplicationCacheHost(
      WebApplicationCacheHostClient*) override;
  void OnShadowPageInitialized() override;

 private:
  // WebDevToolsAgentImpl::Client overrides.
  void ResumeStartup() override;
  const base::UnguessableToken& GetDevToolsWorkerToken() override;

  void OnScriptLoaderFinished();
  void StartWorkerThread();

  WebEmbeddedWorkerStartData worker_start_data_;

  std::unique_ptr<WebServiceWorkerContextClient> worker_context_client_;

  // These are valid until StartWorkerThread() is called. After the worker
  // thread is created, these are passed to the worker thread.
  std::unique_ptr<ServiceWorkerInstalledScriptsManager>
      installed_scripts_manager_;
  std::unique_ptr<ServiceWorkerContentSettingsProxy> content_settings_client_;

  // Kept around only while main script loading is ongoing.
  scoped_refptr<WorkerClassicScriptLoader> main_script_loader_;

  std::unique_ptr<WorkerThread> worker_thread_;
  Persistent<WorkerInspectorProxy> worker_inspector_proxy_;

  std::unique_ptr<WorkerShadowPage> shadow_page_;

  bool asked_to_terminate_ = false;

  enum WaitingForDebuggerState { kWaitingForDebugger, kNotWaitingForDebugger };

  enum {
    kDontPauseAfterDownload,
    kDoPauseAfterDownload,
    kIsPausedAfterDownload
  } pause_after_download_state_;

  // Whether to pause the initialization and wait for debugger to attach
  // before proceeding. This technique allows debugging worker startup.
  WaitingForDebuggerState waiting_for_debugger_state_;
  // Unique worker token used by DevTools to attribute different instrumentation
  // to the same worker.
  base::UnguessableToken devtools_worker_token_;

  service_manager::mojom::blink::InterfaceProviderPtrInfo
      interface_provider_info_;
};

}  // namespace blink

#endif  // WebEmbeddedWorkerImpl_h
