// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef HEADLESS_PUBLIC_HEADLESS_BROWSER_CONTEXT_H_
#define HEADLESS_PUBLIC_HEADLESS_BROWSER_CONTEXT_H_

#include <list>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/callback.h"
#include "base/optional.h"
#include "content/public/browser/resource_request_info.h"
#include "content/public/common/web_preferences.h"
#include "headless/lib/browser/headless_network_conditions.h"
#include "headless/public/headless_export.h"
#include "headless/public/headless_web_contents.h"
#include "net/proxy_resolution/proxy_resolution_service.h"
#include "net/url_request/url_request_job_factory.h"

namespace base {
class FilePath;
}

namespace headless {
class HeadlessBrowserImpl;
class HeadlessBrowserContextOptions;

// Imported into headless namespace for
// Builder::SetOverrideWebPreferencesCallback().
using content::WebPreferences;

using DevToolsStatus = content::ResourceRequestInfo::DevToolsStatus;

using ProtocolHandlerMap = std::unordered_map<
    std::string,
    std::unique_ptr<net::URLRequestJobFactory::ProtocolHandler>>;

// Represents an isolated session with a unique cache, cookies, and other
// profile/session related data.
// When browser context is deleted, all associated web contents are closed.
class HEADLESS_EXPORT HeadlessBrowserContext {
 public:
  class Observer;
  class Builder;

  virtual ~HeadlessBrowserContext() {}

  // Open a new tab. Returns a builder object which can be used to set
  // properties for the new tab.
  // Pointer to HeadlessWebContents becomes invalid after:
  // a) Calling HeadlessWebContents::Close, or
  // b) Calling HeadlessBrowserContext::Close on associated browser context, or
  // c) Calling HeadlessBrowser::Shutdown.
  virtual HeadlessWebContents::Builder CreateWebContentsBuilder() = 0;

  // Returns all web contents owned by this browser context.
  virtual std::vector<HeadlessWebContents*> GetAllWebContents() = 0;

  // See HeadlessBrowser::GetWebContentsForDevToolsAgentHostId.
  virtual HeadlessWebContents* GetWebContentsForDevToolsAgentHostId(
      const std::string& devtools_agent_host_id) = 0;

  // Destroy this BrowserContext and all WebContents associated with it.
  virtual void Close() = 0;

  // GUID for this browser context.
  virtual const std::string& Id() const = 0;

  virtual void AddObserver(Observer* observer) = 0;
  virtual void RemoveObserver(Observer* observer) = 0;

  virtual HeadlessNetworkConditions GetNetworkConditions() = 0;

  // TODO(skyostil): Allow saving and restoring contexts (crbug.com/617931).

 protected:
  HeadlessBrowserContext() {}

 private:
  DISALLOW_COPY_AND_ASSIGN(HeadlessBrowserContext);
};

class HEADLESS_EXPORT HeadlessBrowserContext::Observer {
 public:
  // This will be delivered on the UI thread.
  virtual void OnChildContentsCreated(HeadlessWebContents* parent,
                                      HeadlessWebContents* child) {}

  // Indicates that a network request failed or was canceled. This will be
  // delivered on the IO thread.
  virtual void UrlRequestFailed(net::URLRequest* request,
                                int net_error,
                                DevToolsStatus devtools_status) {}

  // Indicates the HeadlessBrowserContext is about to be deleted.
  virtual void OnHeadlessBrowserContextDestruct() {}

 protected:
  virtual ~Observer() {}
};

class HEADLESS_EXPORT HeadlessBrowserContext::Builder {
 public:
  Builder(Builder&&);
  ~Builder();

  // Set custom network protocol handlers. These can be used to override URL
  // fetching for different network schemes.
  Builder& SetProtocolHandlers(ProtocolHandlerMap protocol_handlers);

  Builder& AddTabSocketMojoBindings();

  // By default if you add mojo bindings, http and https are disabled because
  // its almost certinly unsafe for arbitary sites on the internet to have
  // access to these bindings.  If you know what you're doing it may be OK to
  // turn them back on. E.g. if headless_lib is being used in a testing
  // framework which serves the web content from disk that's likely ok.
  //
  // That said, best pratice is to add a ProtocolHandler to serve the
  // webcontent over a custom protocol. That way you can be sure that only the
  // things you intend have access to mojo.
  Builder& EnableUnsafeNetworkAccessWithMojoBindings(
      bool enable_http_and_https_if_mojo_used);

  // By default |HeadlessBrowserContext| inherits the following options from
  // the browser instance. The methods below can be used to override these
  // settings. See HeadlessBrowser::Options for their meaning.
  Builder& SetProductNameAndVersion(
      const std::string& product_name_and_version);
  Builder& SetAcceptLanguage(const std::string& accept_language);
  Builder& SetUserAgent(const std::string& user_agent);
  Builder& SetProxyConfig(std::unique_ptr<net::ProxyConfig> proxy_config);
  Builder& SetHostResolverRules(const std::string& host_resolver_rules);
  Builder& SetWindowSize(const gfx::Size& window_size);
  Builder& SetUserDataDir(const base::FilePath& user_data_dir);
  Builder& SetIncognitoMode(bool incognito_mode);
  Builder& SetSitePerProcess(bool site_per_process);
  Builder& SetBlockNewWebContents(bool block_new_web_contents);
  Builder& SetInitialVirtualTime(base::Time initial_virtual_time);
  Builder& SetAllowCookies(bool incognito_mode);
  Builder& SetOverrideWebPreferencesCallback(
      base::RepeatingCallback<void(WebPreferences*)> callback);

  HeadlessBrowserContext* Build();

 private:
  friend class HeadlessBrowserImpl;
  friend class HeadlessBrowserContextImpl;

  explicit Builder(HeadlessBrowserImpl* browser);

  struct MojoBindings {
    MojoBindings();
    MojoBindings(const std::string& mojom_name, const std::string& js_bindings);
    ~MojoBindings();

    std::string mojom_name;
    std::string js_bindings;

   private:
    DISALLOW_COPY_AND_ASSIGN(MojoBindings);
  };

  HeadlessBrowserImpl* browser_;
  std::unique_ptr<HeadlessBrowserContextOptions> options_;

  std::list<MojoBindings> mojo_bindings_;
  bool enable_http_and_https_if_mojo_used_;

  DISALLOW_COPY_AND_ASSIGN(Builder);
};

}  // namespace headless

#endif  // HEADLESS_PUBLIC_HEADLESS_BROWSER_CONTEXT_H_
