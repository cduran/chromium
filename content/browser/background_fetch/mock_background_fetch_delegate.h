// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_BROWSER_BACKGROUND_FETCH_MOCK_BACKGROUND_FETCH_DELEGATE_H_
#define CONTENT_BROWSER_BACKGROUND_FETCH_MOCK_BACKGROUND_FETCH_DELEGATE_H_

#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "base/files/scoped_temp_dir.h"
#include "content/public/browser/background_fetch_delegate.h"
#include "url/gurl.h"

namespace net {
class HttpResponseHeaders;
}  // namespace net

namespace content {

class MockBackgroundFetchDelegate : public BackgroundFetchDelegate {
 public:
  // Structure encapsulating the data for a injected response. Should only be
  // created by the builder, which also defines the ownership semantics.
  struct TestResponse {
    TestResponse();
    ~TestResponse();

    bool succeeded_;
    scoped_refptr<net::HttpResponseHeaders> headers;
    std::string data;

   private:
    DISALLOW_COPY_AND_ASSIGN(TestResponse);
  };

  // Builder for creating a TestResponse object with the given data.
  // MockBackgroundFetchDelegate will respond to the corresponding request based
  // on this.
  class TestResponseBuilder {
   public:
    explicit TestResponseBuilder(int response_code);
    ~TestResponseBuilder();

    TestResponseBuilder& AddResponseHeader(const std::string& name,
                                           const std::string& value);

    TestResponseBuilder& SetResponseData(std::string data);

    // Finalizes the builder and invalidates the underlying response.
    std::unique_ptr<TestResponse> Build();

   private:
    std::unique_ptr<TestResponse> response_;

    DISALLOW_COPY_AND_ASSIGN(TestResponseBuilder);
  };

  MockBackgroundFetchDelegate();
  ~MockBackgroundFetchDelegate() override;

  // BackgroundFetchDelegate implementation:
  void CreateDownloadJob(
      const std::string& job_unique_id,
      const std::string& title,
      const url::Origin& origin,
      const SkBitmap& icon,
      int completed_parts,
      int total_parts,
      const std::vector<std::string>& current_guids) override;
  void DownloadUrl(const std::string& job_unique_id,
                   const std::string& guid,
                   const std::string& method,
                   const GURL& url,
                   const net::NetworkTrafficAnnotationTag& traffic_annotation,
                   const net::HttpRequestHeaders& headers) override;
  void Abort(const std::string& job_unique_id) override;

  void RegisterResponse(const GURL& url,
                        std::unique_ptr<TestResponse> response);

 private:
  // Posts (to the default task runner) a callback that is only run if the job
  // indicated by |job_unique_id| has not been aborted.
  void PostAbortCheckingTask(const std::string& job_unique_id,
                             base::OnceCallback<void()> callback);

  // Immediately runs the callback if the job indicated by |job_unique_id| has
  // not been aborted.
  void RunAbortCheckingTask(const std::string& job_unique_id,
                            base::OnceCallback<void()> callback);

  // Single-use responses registered for specific URLs.
  std::map<const GURL, std::unique_ptr<TestResponse>> url_responses_;

  // GUIDs that have been registered via DownloadUrl and thus cannot be reused.
  std::set<std::string> seen_guids_;

  // Temporary directory in which successfully downloaded files will be stored.
  base::ScopedTempDir temp_directory_;

  // Set of unique job ids that have been aborted.
  std::set<std::string> aborted_jobs_;

  // Map from download GUIDs to unique job ids.
  std::map<std::string, std::string> download_guid_to_job_id_map_;

  DISALLOW_COPY_AND_ASSIGN(MockBackgroundFetchDelegate);
};

}  // namespace content

#endif  // CONTENT_BROWSER_BACKGROUND_FETCH_MOCK_BACKGROUND_FETCH_DELEGATE_H_
