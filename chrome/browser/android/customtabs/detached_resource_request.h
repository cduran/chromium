// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CHROME_BROWSER_ANDROID_CUSTOMTABS_DETACHED_RESOURCE_REQUEST_H_
#define CHROME_BROWSER_ANDROID_CUSTOMTABS_DETACHED_RESOURCE_REQUEST_H_

#include <memory>
#include <string>

#include "base/bind_helpers.h"
#include "base/callback.h"
#include "base/time/time.h"
#include "url/gurl.h"

namespace network {
class SimpleURLLoader;
}

namespace content {
class BrowserContext;
}

namespace customtabs {

// Detached resource request, that is a resource request initiated from the
// browser process, and which starts detached from any client.
//
// It is intended to provide "detached" request capabilities from the browser
// process, that is like <a ping> or <link rel="prefetch">.
//
// This is a UI thread class.
class DetachedResourceRequest {
 public:
  using OnResultCallback = base::OnceCallback<void(bool success)>;

  ~DetachedResourceRequest();

  // Creates a detached request to a |url|, with a given initiating URL,
  // |first_party_for_cookies|. Called on the UI thread.
  // Optional |cb| to get notified about the fetch result, for testing.
  static void CreateAndStart(content::BrowserContext* browser_context,
                             const GURL& url,
                             const GURL& first_party_for_cookies,
                             OnResultCallback cb = base::DoNothing());

 private:
  DetachedResourceRequest(const GURL& url,
                          const GURL& site_for_cookies,
                          OnResultCallback cb);

  static void Start(std::unique_ptr<DetachedResourceRequest> request,
                    content::BrowserContext* browser_context);
  void OnResponseCallback(std::unique_ptr<std::string> response_body);

  const GURL url_;
  const GURL site_for_cookies_;
  base::TimeTicks start_time_;
  OnResultCallback cb_;
  std::unique_ptr<network::SimpleURLLoader> url_loader_;

  DISALLOW_COPY_AND_ASSIGN(DetachedResourceRequest);
};

}  // namespace customtabs

#endif  // CHROME_BROWSER_ANDROID_CUSTOMTABS_DETACHED_RESOURCE_REQUEST_H_
