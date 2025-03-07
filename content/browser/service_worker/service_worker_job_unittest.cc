// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <stdint.h>
#include <tuple>

#include "base/files/scoped_temp_dir.h"
#include "base/logging.h"
#include "base/macros.h"
#include "base/message_loop/message_loop.h"
#include "base/optional.h"
#include "base/run_loop.h"
#include "base/test/test_simple_task_runner.h"
#include "base/time/time.h"
#include "content/browser/service_worker/embedded_worker_registry.h"
#include "content/browser/service_worker/embedded_worker_status.h"
#include "content/browser/service_worker/embedded_worker_test_helper.h"
#include "content/browser/service_worker/service_worker_context_core.h"
#include "content/browser/service_worker/service_worker_context_wrapper.h"
#include "content/browser/service_worker/service_worker_disk_cache.h"
#include "content/browser/service_worker/service_worker_dispatcher_host.h"
#include "content/browser/service_worker/service_worker_handle.h"
#include "content/browser/service_worker/service_worker_job_coordinator.h"
#include "content/browser/service_worker/service_worker_registration.h"
#include "content/browser/service_worker/service_worker_registration_object_host.h"
#include "content/browser/service_worker/service_worker_registration_status.h"
#include "content/browser/service_worker/service_worker_test_utils.h"
#include "content/common/service_worker/service_worker_messages.h"
#include "content/common/service_worker/service_worker_utils.h"
#include "content/public/test/test_browser_context.h"
#include "content/public/test/test_browser_thread_bundle.h"
#include "ipc/ipc_test_sink.h"
#include "net/base/io_buffer.h"
#include "net/base/net_errors.h"
#include "net/base/test_completion_callback.h"
#include "net/http/http_response_headers.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/WebKit/public/mojom/service_worker/service_worker.mojom.h"
#include "third_party/WebKit/public/mojom/service_worker/service_worker_event_status.mojom.h"
#include "third_party/WebKit/public/mojom/service_worker/service_worker_object.mojom.h"
#include "third_party/WebKit/public/mojom/service_worker/service_worker_registration.mojom.h"

using net::IOBuffer;
using net::TestCompletionCallback;
using net::WrappedIOBuffer;

// Unit tests for testing all job registration tasks.
namespace content {

namespace {

// A dispatcher host that holds on to all registered ServiceWorkerHandles.
class KeepHandlesDispatcherHost : public ServiceWorkerDispatcherHost {
 public:
  KeepHandlesDispatcherHost(int render_process_id,
                            ResourceContext* resource_context)
      : ServiceWorkerDispatcherHost(render_process_id, resource_context) {}
  void RegisterServiceWorkerHandle(
      std::unique_ptr<ServiceWorkerHandle> handle) override {
    handles_.push_back(std::move(handle));
  }

  void UnregisterServiceWorkerHandle(int handle_id) override {
    auto iter = handles_.begin();
    for (; iter != handles_.end(); ++iter) {
      if ((*iter)->handle_id() == handle_id)
        break;
    }
    ASSERT_NE(handles_.end(), iter);
    handles_.erase(iter);
  }

  void Clear() {
    handles_.clear();
  }

  const std::vector<std::unique_ptr<ServiceWorkerHandle>>& handles() {
    return handles_;
  }

 private:
  ~KeepHandlesDispatcherHost() override {}

  std::vector<std::unique_ptr<ServiceWorkerHandle>> handles_;
  DISALLOW_COPY_AND_ASSIGN(KeepHandlesDispatcherHost);
};

void SaveRegistrationCallback(
    ServiceWorkerStatusCode expected_status,
    bool* called,
    scoped_refptr<ServiceWorkerRegistration>* registration_out,
    ServiceWorkerStatusCode status,
    const std::string& status_message,
    ServiceWorkerRegistration* registration) {
  EXPECT_EQ(expected_status, status);
  *called = true;
  *registration_out = registration;
}

void SaveFoundRegistrationCallback(
    ServiceWorkerStatusCode expected_status,
    bool* called,
    scoped_refptr<ServiceWorkerRegistration>* registration,
    ServiceWorkerStatusCode status,
    scoped_refptr<ServiceWorkerRegistration> result) {
  EXPECT_EQ(expected_status, status);
  *called = true;
  *registration = std::move(result);
}

// Creates a callback which both keeps track of if it's been called,
// as well as the resulting registration. Whent the callback is fired,
// it ensures that the resulting status matches the expectation.
// 'called' is useful for making sure a sychronous callback is or
// isn't called.
ServiceWorkerRegisterJob::RegistrationCallback SaveRegistration(
    ServiceWorkerStatusCode expected_status,
    bool* called,
    scoped_refptr<ServiceWorkerRegistration>* registration) {
  *called = false;
  return base::Bind(
      &SaveRegistrationCallback, expected_status, called, registration);
}

ServiceWorkerStorage::FindRegistrationCallback SaveFoundRegistration(
    ServiceWorkerStatusCode expected_status,
    bool* called,
    scoped_refptr<ServiceWorkerRegistration>* registration) {
  *called = false;
  return base::BindOnce(&SaveFoundRegistrationCallback, expected_status, called,
                        registration);
}

void SaveUnregistrationCallback(ServiceWorkerStatusCode expected_status,
                                bool* called,
                                int64_t registration_id,
                                ServiceWorkerStatusCode status) {
  EXPECT_EQ(expected_status, status);
  *called = true;
}

ServiceWorkerUnregisterJob::UnregistrationCallback SaveUnregistration(
    ServiceWorkerStatusCode expected_status,
    bool* called) {
  *called = false;
  return base::Bind(&SaveUnregistrationCallback, expected_status, called);
}

}  // namespace

class ServiceWorkerJobTest : public testing::Test {
 public:
  ServiceWorkerJobTest()
      : browser_thread_bundle_(TestBrowserThreadBundle::IO_MAINLOOP) {}

  void SetUp() override {
    helper_.reset(new EmbeddedWorkerTestHelper(base::FilePath()));
  }

  void TearDown() override { helper_.reset(); }

  ServiceWorkerContextCore* context() const { return helper_->context(); }

  ServiceWorkerJobCoordinator* job_coordinator() const {
    return context()->job_coordinator();
  }
  ServiceWorkerStorage* storage() const { return context()->storage(); }

 protected:
  scoped_refptr<ServiceWorkerRegistration> RunRegisterJob(
      const GURL& script_url,
      const blink::mojom::ServiceWorkerRegistrationOptions& options,
      ServiceWorkerStatusCode expected_status = SERVICE_WORKER_OK);
  void RunUnregisterJob(
      const GURL& pattern,
      ServiceWorkerStatusCode expected_status = SERVICE_WORKER_OK);
  scoped_refptr<ServiceWorkerRegistration> FindRegistrationForPattern(
      const GURL& pattern,
      ServiceWorkerStatusCode expected_status = SERVICE_WORKER_OK);
  std::unique_ptr<ServiceWorkerProviderHost> CreateControllee();

  TestBrowserThreadBundle browser_thread_bundle_;
  std::unique_ptr<EmbeddedWorkerTestHelper> helper_;
  std::vector<ServiceWorkerRemoteProviderEndpoint> remote_endpoints_;
};

scoped_refptr<ServiceWorkerRegistration> ServiceWorkerJobTest::RunRegisterJob(
    const GURL& script_url,
    const blink::mojom::ServiceWorkerRegistrationOptions& options,
    ServiceWorkerStatusCode expected_status) {
  scoped_refptr<ServiceWorkerRegistration> registration;
  bool called;
  job_coordinator()->Register(
      script_url, options,
      SaveRegistration(expected_status, &called, &registration));
  EXPECT_FALSE(called);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(called);
  return registration;
}

void ServiceWorkerJobTest::RunUnregisterJob(
    const GURL& pattern,
    ServiceWorkerStatusCode expected_status) {
  bool called;
  job_coordinator()->Unregister(pattern,
                                SaveUnregistration(expected_status, &called));
  EXPECT_FALSE(called);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(called);
}

scoped_refptr<ServiceWorkerRegistration>
ServiceWorkerJobTest::FindRegistrationForPattern(
    const GURL& pattern,
    ServiceWorkerStatusCode expected_status) {
  bool called;
  scoped_refptr<ServiceWorkerRegistration> registration;
  storage()->FindRegistrationForPattern(
      pattern,
      SaveFoundRegistration(expected_status, &called, &registration));

  EXPECT_FALSE(called);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(called);
  return registration;
}

std::unique_ptr<ServiceWorkerProviderHost>
ServiceWorkerJobTest::CreateControllee() {
  remote_endpoints_.emplace_back();
  std::unique_ptr<ServiceWorkerProviderHost> host = CreateProviderHostForWindow(
      33 /* dummy render process id */, 1 /* dummy provider_id */,
      true /* is_parent_frame_secure */, helper_->context()->AsWeakPtr(),
      &remote_endpoints_.back());
  return host;
}

TEST_F(ServiceWorkerJobTest, SameDocumentSameRegistration) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> original_registration =
      RunRegisterJob(GURL("http://www.example.com/service_worker.js"), options);
  bool called;
  scoped_refptr<ServiceWorkerRegistration> registration1;
  storage()->FindRegistrationForDocument(
      GURL("http://www.example.com/"),
      SaveFoundRegistration(SERVICE_WORKER_OK, &called, &registration1));
  scoped_refptr<ServiceWorkerRegistration> registration2;
  storage()->FindRegistrationForDocument(
      GURL("http://www.example.com/"),
      SaveFoundRegistration(SERVICE_WORKER_OK, &called, &registration2));
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(called);
  ASSERT_TRUE(registration1.get());
  ASSERT_EQ(registration1, original_registration);
  ASSERT_EQ(registration1, registration2);
}

TEST_F(ServiceWorkerJobTest, SameMatchSameRegistration) {
  bool called;
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> original_registration =
      RunRegisterJob(GURL("http://www.example.com/service_worker.js"), options);
  ASSERT_NE(static_cast<ServiceWorkerRegistration*>(nullptr),
            original_registration.get());

  scoped_refptr<ServiceWorkerRegistration> registration1;
  storage()->FindRegistrationForDocument(
      GURL("http://www.example.com/one"),
      SaveFoundRegistration(SERVICE_WORKER_OK, &called, &registration1));
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(called);

  scoped_refptr<ServiceWorkerRegistration> registration2;
  storage()->FindRegistrationForDocument(
      GURL("http://www.example.com/two"),
      SaveFoundRegistration(SERVICE_WORKER_OK, &called, &registration2));
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(called);
  ASSERT_EQ(registration1, original_registration);
  ASSERT_EQ(registration1, registration2);
}

TEST_F(ServiceWorkerJobTest, DifferentMatchDifferentRegistration) {
  bool called1;
  blink::mojom::ServiceWorkerRegistrationOptions options1;
  options1.scope = GURL("http://www.example.com/one/");
  scoped_refptr<ServiceWorkerRegistration> original_registration1;
  job_coordinator()->Register(
      GURL("http://www.example.com/service_worker.js"), options1,
      SaveRegistration(SERVICE_WORKER_OK, &called1, &original_registration1));

  bool called2;
  blink::mojom::ServiceWorkerRegistrationOptions options2;
  options2.scope = GURL("http://www.example.com/two/");
  scoped_refptr<ServiceWorkerRegistration> original_registration2;
  job_coordinator()->Register(
      GURL("http://www.example.com/service_worker.js"), options2,
      SaveRegistration(SERVICE_WORKER_OK, &called2, &original_registration2));

  EXPECT_FALSE(called1);
  EXPECT_FALSE(called2);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(called2);
  EXPECT_TRUE(called1);

  scoped_refptr<ServiceWorkerRegistration> registration1;
  storage()->FindRegistrationForDocument(
      GURL("http://www.example.com/one/"),
      SaveFoundRegistration(SERVICE_WORKER_OK, &called1, &registration1));
  scoped_refptr<ServiceWorkerRegistration> registration2;
  storage()->FindRegistrationForDocument(
      GURL("http://www.example.com/two/"),
      SaveFoundRegistration(SERVICE_WORKER_OK, &called2, &registration2));

  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(called2);
  EXPECT_TRUE(called1);
  ASSERT_NE(registration1, registration2);
}

// Make sure basic registration is working.
TEST_F(ServiceWorkerJobTest, Register) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(GURL("http://www.example.com/service_worker.js"), options);

  EXPECT_TRUE(registration);
  EXPECT_EQ(EmbeddedWorkerTestHelper::Event::Install,
            helper_->dispatched_events()->at(0));
  EXPECT_EQ(EmbeddedWorkerTestHelper::Event::Activate,
            helper_->dispatched_events()->at(1));
}

// Make sure registrations are cleaned up when they are unregistered.
TEST_F(ServiceWorkerJobTest, Unregister) {
  // During registration, service worker handles will be created to host the
  // {installing,waiting,active} service worker objects for
  // ServiceWorkerGlobalScope#registration. KeepHandlesDispatcherHost will store
  // the handles.
  scoped_refptr<KeepHandlesDispatcherHost> dispatcher_host =
      base::MakeRefCounted<KeepHandlesDispatcherHost>(
          helper_->mock_render_process_id(),
          helper_->browser_context()->GetResourceContext());
  helper_->RegisterDispatcherHost(helper_->mock_render_process_id(),
                                  dispatcher_host);
  dispatcher_host->Init(helper_->context_wrapper());

  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(GURL("http://www.example.com/service_worker.js"), options);

  // During the above registration, a service worker registration object host
  // for ServiceWorkerGlobalScope#registration has been created/added into
  // |provider_host|.
  ServiceWorkerProviderHost* provider_host =
      registration->active_version()->provider_host();
  ASSERT_NE(nullptr, provider_host);
  EXPECT_EQ(1UL, provider_host->registration_object_hosts_.size());
  EXPECT_EQ(3UL, dispatcher_host->handles().size());

  RunUnregisterJob(options.scope);

  // Clear all service worker handles.
  dispatcher_host->Clear();
  EXPECT_EQ(0UL, dispatcher_host->handles().size());
  // The service worker registration object host has been destroyed together
  // with |provider_host| by the above unregistration. Then the only reference
  // to the registration should be |registration|.
  EXPECT_TRUE(registration->HasOneRef());

  registration =
      FindRegistrationForPattern(options.scope, SERVICE_WORKER_ERROR_NOT_FOUND);

  EXPECT_FALSE(registration);
}

TEST_F(ServiceWorkerJobTest, Unregister_NothingRegistered) {
  GURL pattern("http://www.example.com/");

  RunUnregisterJob(pattern, SERVICE_WORKER_ERROR_NOT_FOUND);
}

// Make sure registering a new script creates a new version and shares an
// existing registration.
TEST_F(ServiceWorkerJobTest, RegisterNewScript) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/");

  scoped_refptr<ServiceWorkerRegistration> old_registration =
      RunRegisterJob(GURL("http://www.example.com/service_worker.js"), options);

  scoped_refptr<ServiceWorkerRegistration> old_registration_by_pattern =
      FindRegistrationForPattern(options.scope);

  ASSERT_EQ(old_registration, old_registration_by_pattern);
  old_registration_by_pattern = nullptr;

  scoped_refptr<ServiceWorkerRegistration> new_registration = RunRegisterJob(
      GURL("http://www.example.com/service_worker_new.js"), options);

  ASSERT_EQ(old_registration, new_registration);

  scoped_refptr<ServiceWorkerRegistration> new_registration_by_pattern =
      FindRegistrationForPattern(options.scope);

  ASSERT_EQ(new_registration, new_registration_by_pattern);
}

// Make sure that when registering a duplicate pattern+script_url
// combination, that the same registration is used.
TEST_F(ServiceWorkerJobTest, RegisterDuplicateScript) {
  // During registration, handles will be created for hosting the worker's
  // context. KeepHandlesDispatcherHost will store the handles.
  scoped_refptr<KeepHandlesDispatcherHost> dispatcher_host =
      new KeepHandlesDispatcherHost(
          helper_->mock_render_process_id(),
          helper_->browser_context()->GetResourceContext());
  helper_->RegisterDispatcherHost(helper_->mock_render_process_id(),
                                  dispatcher_host);
  dispatcher_host->Init(helper_->context_wrapper());

  GURL script_url("http://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/");

  scoped_refptr<ServiceWorkerRegistration> old_registration =
      RunRegisterJob(script_url, options);

  // During the above registration, a service worker registration object host
  // for ServiceWorkerGlobalScope#registration has been created/added into
  // |provider_host|.
  ServiceWorkerProviderHost* provider_host =
      old_registration->active_version()->provider_host();
  ASSERT_NE(nullptr, provider_host);

  // Clear all service worker handles.
  dispatcher_host->Clear();
  // Ensure that the registration's object host doesn't have the reference.
  EXPECT_EQ(1UL, provider_host->registration_object_hosts_.size());
  provider_host->registration_object_hosts_.clear();
  EXPECT_EQ(0UL, provider_host->registration_object_hosts_.size());
  ASSERT_TRUE(old_registration->HasOneRef());

  scoped_refptr<ServiceWorkerRegistration> old_registration_by_pattern =
      FindRegistrationForPattern(options.scope);

  ASSERT_TRUE(old_registration_by_pattern.get());

  scoped_refptr<ServiceWorkerRegistration> new_registration =
      RunRegisterJob(script_url, options);

  ASSERT_EQ(old_registration, new_registration);

  ASSERT_FALSE(old_registration->HasOneRef());

  scoped_refptr<ServiceWorkerRegistration> new_registration_by_pattern =
      FindRegistrationForPattern(options.scope);

  EXPECT_EQ(new_registration_by_pattern, old_registration);
}

// Make sure that the same registration is used and the update_via_cache value
// is updated when registering a duplicate pattern+script_url with a different
// update_via_cache value.
TEST_F(ServiceWorkerJobTest, RegisterWithDifferentUpdateViaCache) {
  // During registration, handles will be created for hosting the worker's
  // context. KeepHandlesDispatcherHost will store the handles.
  auto dispatcher_host = base::MakeRefCounted<KeepHandlesDispatcherHost>(
      helper_->mock_render_process_id(),
      helper_->browser_context()->GetResourceContext());
  helper_->RegisterDispatcherHost(helper_->mock_render_process_id(),
                                  dispatcher_host);
  dispatcher_host->Init(helper_->context_wrapper());

  GURL script_url("https://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("https://www.example.com/");

  scoped_refptr<ServiceWorkerRegistration> old_registration =
      RunRegisterJob(script_url, options);

  EXPECT_EQ(blink::mojom::ServiceWorkerUpdateViaCache::kImports,
            old_registration->update_via_cache());

  // During the above registration, a service worker registration object host
  // for ServiceWorkerGlobalScope#registration has been created/added into
  // |provider_host|.
  ServiceWorkerProviderHost* provider_host =
      old_registration->active_version()->provider_host();
  ASSERT_NE(nullptr, provider_host);

  // Clear all service worker handles.
  dispatcher_host->Clear();
  // Ensure that the registration's object host doesn't have the reference.
  EXPECT_EQ(1UL, provider_host->registration_object_hosts_.size());
  provider_host->registration_object_hosts_.clear();
  EXPECT_EQ(0UL, provider_host->registration_object_hosts_.size());
  ASSERT_TRUE(old_registration->HasOneRef());

  scoped_refptr<ServiceWorkerRegistration> old_registration_by_pattern =
      FindRegistrationForPattern(options.scope);

  ASSERT_TRUE(old_registration_by_pattern.get());

  options.update_via_cache = blink::mojom::ServiceWorkerUpdateViaCache::kNone;
  scoped_refptr<ServiceWorkerRegistration> new_registration =
      RunRegisterJob(script_url, options);

  // Ensure that the registration object is not copied.
  ASSERT_EQ(old_registration, new_registration);
  EXPECT_EQ(blink::mojom::ServiceWorkerUpdateViaCache::kNone,
            new_registration->update_via_cache());

  ASSERT_FALSE(old_registration->HasOneRef());

  scoped_refptr<ServiceWorkerRegistration> new_registration_by_pattern =
      FindRegistrationForPattern(options.scope);

  EXPECT_EQ(new_registration_by_pattern, old_registration);
}

class FailToStartWorkerTestHelper : public EmbeddedWorkerTestHelper {
 public:
  FailToStartWorkerTestHelper() : EmbeddedWorkerTestHelper(base::FilePath()) {}

  void OnStartWorker(
      int embedded_worker_id,
      int64_t service_worker_version_id,
      const GURL& scope,
      const GURL& script_url,
      bool pause_after_download,
      mojom::ServiceWorkerEventDispatcherRequest dispatcher_request,
      mojom::ControllerServiceWorkerRequest controller_request,
      blink::mojom::ServiceWorkerHostAssociatedPtrInfo service_worker_host,
      mojom::EmbeddedWorkerInstanceHostAssociatedPtrInfo instance_host,
      mojom::ServiceWorkerProviderInfoForStartWorkerPtr provider_info,
      blink::mojom::ServiceWorkerInstalledScriptsInfoPtr installed_scripts_info)
      override {
    mojom::EmbeddedWorkerInstanceHostAssociatedPtr instance_host_ptr;
    instance_host_ptr.Bind(std::move(instance_host));
    instance_host_ptr->OnStopped();
    base::RunLoop().RunUntilIdle();
  }
};

TEST_F(ServiceWorkerJobTest, Register_FailToStartWorker) {
  helper_.reset(new FailToStartWorkerTestHelper);

  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(GURL("http://www.example.com/service_worker.js"), options,
                     SERVICE_WORKER_ERROR_START_WORKER_FAILED);

  ASSERT_EQ(scoped_refptr<ServiceWorkerRegistration>(nullptr), registration);
}

// Register and then unregister the pattern, in parallel. Job coordinator should
// process jobs until the last job.
TEST_F(ServiceWorkerJobTest, ParallelRegUnreg) {
  GURL script_url("http://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/");

  bool registration_called = false;
  scoped_refptr<ServiceWorkerRegistration> registration;
  job_coordinator()->Register(
      script_url, options,
      SaveRegistration(SERVICE_WORKER_OK, &registration_called, &registration));

  bool unregistration_called = false;
  job_coordinator()->Unregister(
      options.scope,
      SaveUnregistration(SERVICE_WORKER_OK, &unregistration_called));

  ASSERT_FALSE(registration_called);
  ASSERT_FALSE(unregistration_called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(registration_called);
  ASSERT_TRUE(unregistration_called);

  registration =
      FindRegistrationForPattern(options.scope, SERVICE_WORKER_ERROR_NOT_FOUND);

  ASSERT_EQ(scoped_refptr<ServiceWorkerRegistration>(), registration);
}

// Register conflicting scripts for the same pattern. The most recent
// registration should win, and the old registration should have been
// shutdown.
TEST_F(ServiceWorkerJobTest, ParallelRegNewScript) {
  GURL pattern("http://www.example.com/");

  GURL script_url1("http://www.example.com/service_worker1.js");
  bool registration1_called = false;
  scoped_refptr<ServiceWorkerRegistration> registration1;
  job_coordinator()->Register(
      script_url1,
      blink::mojom::ServiceWorkerRegistrationOptions(
          pattern, blink::mojom::ServiceWorkerUpdateViaCache::kNone),
      SaveRegistration(SERVICE_WORKER_OK, &registration1_called,
                       &registration1));

  GURL script_url2("http://www.example.com/service_worker2.js");
  bool registration2_called = false;
  scoped_refptr<ServiceWorkerRegistration> registration2;
  job_coordinator()->Register(
      script_url2,
      blink::mojom::ServiceWorkerRegistrationOptions(
          pattern, blink::mojom::ServiceWorkerUpdateViaCache::kAll),
      SaveRegistration(SERVICE_WORKER_OK, &registration2_called,
                       &registration2));

  ASSERT_FALSE(registration1_called);
  ASSERT_FALSE(registration2_called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(registration1_called);
  ASSERT_TRUE(registration2_called);

  scoped_refptr<ServiceWorkerRegistration> registration =
      FindRegistrationForPattern(pattern);

  ASSERT_EQ(registration2, registration);
}

// Register the exact same pattern + script. Requests should be
// coalesced such that both callers get the exact same registration
// object.
TEST_F(ServiceWorkerJobTest, ParallelRegSameScript) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/");

  GURL script_url("http://www.example.com/service_worker1.js");
  bool registration1_called = false;
  scoped_refptr<ServiceWorkerRegistration> registration1;
  job_coordinator()->Register(
      script_url, options,
      SaveRegistration(SERVICE_WORKER_OK, &registration1_called,
                       &registration1));

  bool registration2_called = false;
  scoped_refptr<ServiceWorkerRegistration> registration2;
  job_coordinator()->Register(
      script_url, options,
      SaveRegistration(SERVICE_WORKER_OK, &registration2_called,
                       &registration2));

  ASSERT_FALSE(registration1_called);
  ASSERT_FALSE(registration2_called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(registration1_called);
  ASSERT_TRUE(registration2_called);

  ASSERT_EQ(registration1, registration2);

  scoped_refptr<ServiceWorkerRegistration> registration =
      FindRegistrationForPattern(options.scope);

  ASSERT_EQ(registration, registration1);
}

// Call simulataneous unregister calls.
TEST_F(ServiceWorkerJobTest, ParallelUnreg) {
  GURL pattern("http://www.example.com/");

  GURL script_url("http://www.example.com/service_worker.js");
  bool unregistration1_called = false;
  job_coordinator()->Unregister(
      pattern,
      SaveUnregistration(SERVICE_WORKER_ERROR_NOT_FOUND,
                         &unregistration1_called));

  bool unregistration2_called = false;
  job_coordinator()->Unregister(
      pattern,
      SaveUnregistration(SERVICE_WORKER_ERROR_NOT_FOUND,
                         &unregistration2_called));

  ASSERT_FALSE(unregistration1_called);
  ASSERT_FALSE(unregistration2_called);
  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(unregistration1_called);
  ASSERT_TRUE(unregistration2_called);

  // There isn't really a way to test that they are being coalesced,
  // but we can make sure they can exist simultaneously without
  // crashing.
  scoped_refptr<ServiceWorkerRegistration> registration =
      FindRegistrationForPattern(pattern, SERVICE_WORKER_ERROR_NOT_FOUND);

  ASSERT_EQ(scoped_refptr<ServiceWorkerRegistration>(), registration);
}

TEST_F(ServiceWorkerJobTest, AbortAll_Register) {
  GURL script_url1("http://www1.example.com/service_worker.js");
  GURL script_url2("http://www2.example.com/service_worker.js");

  blink::mojom::ServiceWorkerRegistrationOptions options1;
  options1.scope = GURL("http://www1.example.com/");
  blink::mojom::ServiceWorkerRegistrationOptions options2;
  options2.scope = GURL("http://www2.example.com/");

  bool registration_called1 = false;
  scoped_refptr<ServiceWorkerRegistration> registration1;
  job_coordinator()->Register(
      script_url1, options1,
      SaveRegistration(SERVICE_WORKER_ERROR_ABORT, &registration_called1,
                       &registration1));

  bool registration_called2 = false;
  scoped_refptr<ServiceWorkerRegistration> registration2;
  job_coordinator()->Register(
      script_url2, options2,
      SaveRegistration(SERVICE_WORKER_ERROR_ABORT, &registration_called2,
                       &registration2));

  ASSERT_FALSE(registration_called1);
  ASSERT_FALSE(registration_called2);
  job_coordinator()->AbortAll();

  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(registration_called1);
  ASSERT_TRUE(registration_called2);

  bool find_called1 = false;
  storage()->FindRegistrationForPattern(
      options1.scope, SaveFoundRegistration(SERVICE_WORKER_ERROR_NOT_FOUND,
                                            &find_called1, &registration1));

  bool find_called2 = false;
  storage()->FindRegistrationForPattern(
      options2.scope, SaveFoundRegistration(SERVICE_WORKER_ERROR_NOT_FOUND,
                                            &find_called2, &registration2));

  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(find_called1);
  ASSERT_TRUE(find_called2);
  EXPECT_EQ(scoped_refptr<ServiceWorkerRegistration>(), registration1);
  EXPECT_EQ(scoped_refptr<ServiceWorkerRegistration>(), registration2);
}

TEST_F(ServiceWorkerJobTest, AbortAll_Unregister) {
  GURL pattern1("http://www1.example.com/");
  GURL pattern2("http://www2.example.com/");

  bool unregistration_called1 = false;
  scoped_refptr<ServiceWorkerRegistration> registration1;
  job_coordinator()->Unregister(
      pattern1,
      SaveUnregistration(SERVICE_WORKER_ERROR_ABORT,
                         &unregistration_called1));

  bool unregistration_called2 = false;
  job_coordinator()->Unregister(
      pattern2,
      SaveUnregistration(SERVICE_WORKER_ERROR_ABORT,
                         &unregistration_called2));

  ASSERT_FALSE(unregistration_called1);
  ASSERT_FALSE(unregistration_called2);
  job_coordinator()->AbortAll();

  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(unregistration_called1);
  ASSERT_TRUE(unregistration_called2);
}

TEST_F(ServiceWorkerJobTest, AbortAll_RegUnreg) {
  GURL script_url("http://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/");

  bool registration_called = false;
  scoped_refptr<ServiceWorkerRegistration> registration;
  job_coordinator()->Register(
      script_url, options,
      SaveRegistration(SERVICE_WORKER_ERROR_ABORT, &registration_called,
                       &registration));

  bool unregistration_called = false;
  job_coordinator()->Unregister(
      options.scope,
      SaveUnregistration(SERVICE_WORKER_ERROR_ABORT, &unregistration_called));

  ASSERT_FALSE(registration_called);
  ASSERT_FALSE(unregistration_called);
  job_coordinator()->AbortAll();

  base::RunLoop().RunUntilIdle();
  ASSERT_TRUE(registration_called);
  ASSERT_TRUE(unregistration_called);

  registration =
      FindRegistrationForPattern(options.scope, SERVICE_WORKER_ERROR_NOT_FOUND);

  EXPECT_EQ(scoped_refptr<ServiceWorkerRegistration>(), registration);
}

// Tests that the waiting worker enters the 'redundant' state upon
// unregistration.
TEST_F(ServiceWorkerJobTest, UnregisterWaitingSetsRedundant) {
  GURL script_url("http://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script_url, options);
  ASSERT_TRUE(registration.get());

  // Manually create the waiting worker since there is no way to become a
  // waiting worker until Update is implemented.
  scoped_refptr<ServiceWorkerVersion> version = new ServiceWorkerVersion(
      registration.get(), script_url, 1L, helper_->context()->AsWeakPtr());
  ServiceWorkerStatusCode status = SERVICE_WORKER_ERROR_FAILED;
  version->StartWorker(ServiceWorkerMetrics::EventType::UNKNOWN,
                       CreateReceiverOnCurrentThread(&status));
  base::RunLoop().RunUntilIdle();
  ASSERT_EQ(SERVICE_WORKER_OK, status);

  version->set_fetch_handler_existence(
      ServiceWorkerVersion::FetchHandlerExistence::EXISTS);
  version->SetStatus(ServiceWorkerVersion::INSTALLED);
  registration->SetWaitingVersion(version);
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::INSTALLED, version->status());

  RunUnregisterJob(GURL("http://www.example.com/"));

  // The version should be stopped since there is no controllee after
  // unregistration.
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, version->status());
}

// Tests that the active worker enters the 'redundant' state upon
// unregistration.
TEST_F(ServiceWorkerJobTest, UnregisterActiveSetsRedundant) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(GURL("http://www.example.com/service_worker.js"), options);
  ASSERT_TRUE(registration.get());

  scoped_refptr<ServiceWorkerVersion> version = registration->active_version();
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED, version->status());

  RunUnregisterJob(GURL("http://www.example.com/"));

  // The version should be stopped since there is no controllee after
  // unregistration.
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, version->status());
}

// Tests that the active worker enters the 'redundant' state upon
// unregistration.
TEST_F(ServiceWorkerJobTest,
       UnregisterActiveSetsRedundant_WaitForNoControllee) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(GURL("http://www.example.com/service_worker.js"), options);
  ASSERT_TRUE(registration.get());

  std::unique_ptr<ServiceWorkerProviderHost> host = CreateControllee();
  registration->active_version()->AddControllee(host.get());

  scoped_refptr<ServiceWorkerVersion> version = registration->active_version();
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED, version->status());

  RunUnregisterJob(GURL("http://www.example.com/"));

  // The version should be running since there is still a controllee.
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED, version->status());

  registration->active_version()->RemoveControllee(host.get());
  base::RunLoop().RunUntilIdle();

  // The version should be stopped since there is no controllee.
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, version->status());
}

namespace {  // Helpers for the update job tests.

const GURL kNoChangeOrigin("http://nochange/");
const GURL kNewVersionOrigin("http://newversion/");
const std::string kScope("scope/");
const std::string kScript("script.js");

void RunNestedUntilIdle() {
  base::MessageLoop::ScopedNestableTaskAllower allow(
      base::MessageLoop::current());
  base::RunLoop().RunUntilIdle();
}

void OnIOComplete(int* rv_out, int rv) {
  *rv_out = rv;
}

void WriteResponse(ServiceWorkerStorage* storage,
                   int64_t id,
                   const std::string& headers,
                   IOBuffer* body,
                   int length) {
  std::unique_ptr<ServiceWorkerResponseWriter> writer =
      storage->CreateResponseWriter(id);

  std::unique_ptr<net::HttpResponseInfo> info(new net::HttpResponseInfo);
  info->request_time = base::Time::Now();
  info->response_time = base::Time::Now();
  info->was_cached = false;
  info->headers = new net::HttpResponseHeaders(headers);
  scoped_refptr<HttpResponseInfoIOBuffer> info_buffer =
      new HttpResponseInfoIOBuffer(info.release());

  int rv = -1234;
  writer->WriteInfo(info_buffer.get(), base::BindOnce(&OnIOComplete, &rv));
  RunNestedUntilIdle();
  EXPECT_LT(0, rv);

  rv = -1234;
  writer->WriteData(body, length, base::BindOnce(&OnIOComplete, &rv));
  RunNestedUntilIdle();
  EXPECT_EQ(length, rv);
}

void WriteStringResponse(ServiceWorkerStorage* storage,
                         int64_t id,
                         const std::string& body) {
  scoped_refptr<IOBuffer> body_buffer(new WrappedIOBuffer(body.data()));
  const char kHttpHeaders[] = "HTTP/1.0 200 HONKYDORY\0\0";
  std::string headers(kHttpHeaders, arraysize(kHttpHeaders));
  WriteResponse(storage, id, headers, body_buffer.get(), body.length());
}

class UpdateJobTestHelper
    : public EmbeddedWorkerTestHelper,
      public ServiceWorkerRegistration::Listener,
      public ServiceWorkerVersion::Listener {
 public:
  struct AttributeChangeLogEntry {
    int64_t registration_id;
    ChangedVersionAttributesMask mask;
    ServiceWorkerRegistrationInfo info;
  };

  struct StateChangeLogEntry {
    int64_t version_id;
    ServiceWorkerVersion::Status status;
  };

  UpdateJobTestHelper() : EmbeddedWorkerTestHelper(base::FilePath()) {}
  ~UpdateJobTestHelper() override {
    if (observed_registration_.get())
      observed_registration_->RemoveListener(this);
    for (auto& version : observed_versions_)
      version->RemoveListener(this);
  }

  ServiceWorkerStorage* storage() { return context()->storage(); }
  ServiceWorkerJobCoordinator* job_coordinator() {
    return context()->job_coordinator();
  }

  void set_force_start_worker_failure(bool force_start_worker_failure) {
    force_start_worker_failure_ = force_start_worker_failure;
  }

  scoped_refptr<ServiceWorkerRegistration> SetupInitialRegistration(
      const GURL& test_origin) {
    blink::mojom::ServiceWorkerRegistrationOptions options;
    options.scope = test_origin.Resolve(kScope);
    scoped_refptr<ServiceWorkerRegistration> registration;
    bool called = false;
    job_coordinator()->Register(
        test_origin.Resolve(kScript), options,
        SaveRegistration(SERVICE_WORKER_OK, &called, &registration));
    base::RunLoop().RunUntilIdle();
    EXPECT_TRUE(called);
    EXPECT_TRUE(registration.get());
    EXPECT_TRUE(registration->active_version());
    EXPECT_FALSE(registration->installing_version());
    EXPECT_FALSE(registration->waiting_version());
    observed_registration_ = registration;
    return registration;
  }

  // EmbeddedWorkerTestHelper overrides
  void OnStartWorker(
      int embedded_worker_id,
      int64_t version_id,
      const GURL& scope,
      const GURL& script,
      bool pause_after_download,
      mojom::ServiceWorkerEventDispatcherRequest dispatcher_request,
      mojom::ControllerServiceWorkerRequest controller_request,
      blink::mojom::ServiceWorkerHostAssociatedPtrInfo service_worker_host,
      mojom::EmbeddedWorkerInstanceHostAssociatedPtrInfo instance_host,
      mojom::ServiceWorkerProviderInfoForStartWorkerPtr provider_info,
      blink::mojom::ServiceWorkerInstalledScriptsInfoPtr installed_scripts_info)
      override {
    const std::string kMockScriptBody = "mock_script";
    const uint64_t kMockScriptSize = 19284;
    ServiceWorkerVersion* version = context()->GetLiveVersion(version_id);
    ServiceWorkerRegistration* registration =
        context()->GetLiveRegistration(version->registration_id());
    bool is_update = registration->active_version() &&
                     version != registration->active_version();

    ASSERT_TRUE(version);
    observed_versions_.push_back(base::WrapRefCounted(version));
    version->AddListener(this);

    // Simulate network access.
    base::TimeDelta time_since_last_check =
        base::Time::Now() - registration->last_update_check();
    if (!is_update || script.GetOrigin() != kNoChangeOrigin ||
        time_since_last_check > kServiceWorkerScriptMaxCacheAge) {
      version->embedded_worker()->OnNetworkAccessedForScriptLoad();
    }

    int64_t resource_id = storage()->NewResourceId();
    version->script_cache_map()->NotifyStartedCaching(script, resource_id);
    if (!is_update) {
      // Spoof caching the script for the initial version.
      WriteStringResponse(storage(), resource_id, kMockScriptBody);
      version->script_cache_map()->NotifyFinishedCaching(
          script, kMockScriptSize, net::OK, std::string());
      version->SetMainScriptHttpResponseInfo(
          EmbeddedWorkerTestHelper::CreateHttpResponseInfo());
    } else {
      if (script.GetOrigin() == kNoChangeOrigin) {
        // Simulate fetching the updated script and finding it's identical to
        // the incumbent.
        version->script_cache_map()->NotifyFinishedCaching(
            script, kMockScriptSize, net::ERR_FILE_EXISTS, std::string());
        version->SetMainScriptHttpResponseInfo(
            EmbeddedWorkerTestHelper::CreateHttpResponseInfo());

        mojom::EmbeddedWorkerInstanceHostAssociatedPtr instance_host_ptr;
        instance_host_ptr.Bind(std::move(instance_host));
        instance_host_ptr->OnScriptLoaded();
        base::RunLoop().RunUntilIdle();
        return;
      }

      // Spoof caching the script for the new version.
      WriteStringResponse(storage(), resource_id, "mock_different_script");
      version->script_cache_map()->NotifyFinishedCaching(
          script, kMockScriptSize, net::OK, std::string());
      version->SetMainScriptHttpResponseInfo(
          EmbeddedWorkerTestHelper::CreateHttpResponseInfo());
    }

    started_workers_.insert(embedded_worker_id);
    EmbeddedWorkerTestHelper::OnStartWorker(
        embedded_worker_id, version_id, scope, script, pause_after_download,
        std::move(dispatcher_request), std::move(controller_request),
        std::move(service_worker_host), std::move(instance_host),
        std::move(provider_info), std::move(installed_scripts_info));
  }

  void OnStopWorker(int embedded_worker_id) override {
    // Some of our tests don't call the base class in OnStartWorker(), so we
    // can't call the base class's OnStopWorker() either.
    auto iter = started_workers_.find(embedded_worker_id);
    if (iter == started_workers_.end())
      return;
    EmbeddedWorkerTestHelper::OnStopWorker(embedded_worker_id);
    started_workers_.erase(iter);
  }

  void OnResumeAfterDownload(int embedded_worker_id) override {
    if (!force_start_worker_failure_) {
      EmbeddedWorkerTestHelper::OnResumeAfterDownload(embedded_worker_id);
    } else {
      SimulateWorkerThreadStarted(GetNextThreadId(), embedded_worker_id);
      SimulateWorkerScriptEvaluated(embedded_worker_id, false);
    }
  }

  // ServiceWorkerRegistration::Listener overrides
  void OnVersionAttributesChanged(
      ServiceWorkerRegistration* registration,
      ChangedVersionAttributesMask changed_mask,
      const ServiceWorkerRegistrationInfo& info) override {
    AttributeChangeLogEntry entry;
    entry.registration_id = registration->id();
    entry.mask = changed_mask;
    entry.info = info;
    attribute_change_log_.push_back(entry);
  }

  void OnRegistrationFailed(ServiceWorkerRegistration* registration) override {
    NOTREACHED();
  }

  void OnUpdateFound(ServiceWorkerRegistration* registration) override {
    update_found_ = true;
  }

  // ServiceWorkerVersion::Listener overrides
  void OnVersionStateChanged(ServiceWorkerVersion* version) override {
    StateChangeLogEntry entry;
    entry.version_id = version->version_id();
    entry.status = version->status();
    state_change_log_.push_back(entry);
  }

  scoped_refptr<ServiceWorkerRegistration> observed_registration_;
  std::vector<scoped_refptr<ServiceWorkerVersion>> observed_versions_;

  std::vector<AttributeChangeLogEntry> attribute_change_log_;
  std::vector<StateChangeLogEntry> state_change_log_;
  std::set<int /* embedded_worker_id */> started_workers_;
  bool update_found_ = false;
  bool force_start_worker_failure_ = false;
};

// Helper class for update tests that evicts the active version when the update
// worker is about to be started.
class EvictIncumbentVersionHelper : public UpdateJobTestHelper {
 public:
  EvictIncumbentVersionHelper() {}
  ~EvictIncumbentVersionHelper() override {}

  void OnStartWorker(
      int embedded_worker_id,
      int64_t version_id,
      const GURL& scope,
      const GURL& script,
      bool pause_after_download,
      mojom::ServiceWorkerEventDispatcherRequest dispatcher_request,
      mojom::ControllerServiceWorkerRequest controller_request,
      blink::mojom::ServiceWorkerHostAssociatedPtrInfo service_worker_host,
      mojom::EmbeddedWorkerInstanceHostAssociatedPtrInfo instance_host,
      mojom::ServiceWorkerProviderInfoForStartWorkerPtr provider_info,
      blink::mojom::ServiceWorkerInstalledScriptsInfoPtr installed_scripts_info)
      override {
    ServiceWorkerVersion* version = context()->GetLiveVersion(version_id);
    ServiceWorkerRegistration* registration =
        context()->GetLiveRegistration(version->registration_id());
    bool is_update = registration->active_version() &&
                     version != registration->active_version();
    if (is_update) {
      // Evict the incumbent worker.
      ASSERT_FALSE(registration->waiting_version());
      registration->DeleteVersion(
          base::WrapRefCounted(registration->active_version()));
    }
    UpdateJobTestHelper::OnStartWorker(
        embedded_worker_id, version_id, scope, script, pause_after_download,
        std::move(dispatcher_request), std::move(controller_request),
        std::move(service_worker_host), std::move(instance_host),
        std::move(provider_info), std::move(installed_scripts_info));
  }

  void OnRegistrationFailed(ServiceWorkerRegistration* registration) override {
    registration_failed_ = true;
  }

  bool registration_failed_ = false;
};

}  // namespace

TEST_F(ServiceWorkerJobTest, Update_NoChange) {
  UpdateJobTestHelper* update_helper = new UpdateJobTestHelper;
  helper_.reset(update_helper);
  scoped_refptr<ServiceWorkerRegistration> registration =
      update_helper->SetupInitialRegistration(kNoChangeOrigin);
  ASSERT_TRUE(registration.get());
  ASSERT_EQ(4u, update_helper->state_change_log_.size());
  EXPECT_EQ(ServiceWorkerVersion::INSTALLING,
            update_helper->state_change_log_[0].status);
  EXPECT_EQ(ServiceWorkerVersion::INSTALLED,
            update_helper->state_change_log_[1].status);
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATING,
            update_helper->state_change_log_[2].status);
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED,
            update_helper->state_change_log_[3].status);
  update_helper->state_change_log_.clear();

  // Run the update job.
  registration->AddListener(update_helper);
  scoped_refptr<ServiceWorkerVersion> first_version =
      registration->active_version();
  first_version->StartUpdate();
  base::RunLoop().RunUntilIdle();

  // Verify results.
  ASSERT_TRUE(registration->active_version());
  EXPECT_EQ(first_version.get(), registration->active_version());
  EXPECT_FALSE(registration->installing_version());
  EXPECT_FALSE(registration->waiting_version());
  EXPECT_TRUE(update_helper->attribute_change_log_.empty());
  ASSERT_EQ(1u, update_helper->state_change_log_.size());
  EXPECT_NE(registration->active_version()->version_id(),
            update_helper->state_change_log_[0].version_id);
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT,
            update_helper->state_change_log_[0].status);
  EXPECT_FALSE(update_helper->update_found_);
}

TEST_F(ServiceWorkerJobTest, Update_BumpLastUpdateCheckTime) {
  const base::Time kToday = base::Time::Now();
  const base::Time kYesterday =
      kToday - base::TimeDelta::FromDays(1) - base::TimeDelta::FromHours(1);
  UpdateJobTestHelper* update_helper = new UpdateJobTestHelper;
  helper_.reset(update_helper);
  scoped_refptr<ServiceWorkerRegistration> registration =
      update_helper->SetupInitialRegistration(kNoChangeOrigin);
  ASSERT_TRUE(registration.get());

  registration->AddListener(update_helper);

  // Run an update where the script did not change and the network was not
  // accessed. The check time should not be updated.
  registration->set_last_update_check(kToday);
  registration->active_version()->StartUpdate();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(kToday, registration->last_update_check());
  EXPECT_FALSE(update_helper->update_found_);

  // Run an update where the script did not change and the network was accessed.
  // The check time should be updated.
  registration->set_last_update_check(kYesterday);
  registration->active_version()->StartUpdate();
  base::RunLoop().RunUntilIdle();
  EXPECT_LT(kYesterday, registration->last_update_check());
  EXPECT_FALSE(update_helper->update_found_);
  registration->RemoveListener(update_helper);

  registration = update_helper->SetupInitialRegistration(kNewVersionOrigin);
  ASSERT_TRUE(registration.get());

  registration->AddListener(update_helper);

  // Run an update where the script changed. The check time should be updated.
  registration->set_last_update_check(kYesterday);
  registration->active_version()->StartUpdate();
  base::RunLoop().RunUntilIdle();
  EXPECT_LT(kYesterday, registration->last_update_check());

  // Run an update to a worker that loads successfully but fails to start up
  // (script evaluation failure). The check time should be updated.
  update_helper->set_force_start_worker_failure(true);
  registration->set_last_update_check(kYesterday);
  registration->active_version()->StartUpdate();
  base::RunLoop().RunUntilIdle();
  EXPECT_LT(kYesterday, registration->last_update_check());
}

TEST_F(ServiceWorkerJobTest, Update_NewVersion) {
  UpdateJobTestHelper* update_helper = new UpdateJobTestHelper;
  helper_.reset(update_helper);
  scoped_refptr<ServiceWorkerRegistration> registration =
      update_helper->SetupInitialRegistration(kNewVersionOrigin);
  ASSERT_TRUE(registration.get());
  update_helper->state_change_log_.clear();

  // Run the update job.
  registration->AddListener(update_helper);
  scoped_refptr<ServiceWorkerVersion> first_version =
      registration->active_version();
  first_version->StartUpdate();
  base::RunLoop().RunUntilIdle();

  // Verify results.
  ASSERT_TRUE(registration->active_version());
  EXPECT_NE(first_version.get(), registration->active_version());
  EXPECT_FALSE(registration->installing_version());
  EXPECT_FALSE(registration->waiting_version());
  ASSERT_EQ(3u, update_helper->attribute_change_log_.size());

  UpdateJobTestHelper::AttributeChangeLogEntry entry;
  entry = update_helper->attribute_change_log_[0];
  EXPECT_TRUE(entry.mask.installing_changed());
  EXPECT_FALSE(entry.mask.waiting_changed());
  EXPECT_FALSE(entry.mask.active_changed());
  EXPECT_NE(entry.info.installing_version.version_id,
            blink::mojom::kInvalidServiceWorkerVersionId);
  EXPECT_EQ(entry.info.waiting_version.version_id,
            blink::mojom::kInvalidServiceWorkerVersionId);
  EXPECT_NE(entry.info.active_version.version_id,
            blink::mojom::kInvalidServiceWorkerVersionId);

  entry = update_helper->attribute_change_log_[1];
  EXPECT_TRUE(entry.mask.installing_changed());
  EXPECT_TRUE(entry.mask.waiting_changed());
  EXPECT_FALSE(entry.mask.active_changed());
  EXPECT_EQ(entry.info.installing_version.version_id,
            blink::mojom::kInvalidServiceWorkerVersionId);
  EXPECT_NE(entry.info.waiting_version.version_id,
            blink::mojom::kInvalidServiceWorkerVersionId);
  EXPECT_NE(entry.info.active_version.version_id,
            blink::mojom::kInvalidServiceWorkerVersionId);

  entry = update_helper->attribute_change_log_[2];
  EXPECT_FALSE(entry.mask.installing_changed());
  EXPECT_TRUE(entry.mask.waiting_changed());
  EXPECT_TRUE(entry.mask.active_changed());
  EXPECT_EQ(entry.info.installing_version.version_id,
            blink::mojom::kInvalidServiceWorkerVersionId);
  EXPECT_EQ(entry.info.waiting_version.version_id,
            blink::mojom::kInvalidServiceWorkerVersionId);
  EXPECT_NE(entry.info.active_version.version_id,
            blink::mojom::kInvalidServiceWorkerVersionId);

  // expected version state transitions:
  // new.installing, new.installed,
  // old.redundant,
  // new.activating, new.activated
  ASSERT_EQ(5u, update_helper->state_change_log_.size());

  EXPECT_EQ(registration->active_version()->version_id(),
            update_helper->state_change_log_[0].version_id);
  EXPECT_EQ(ServiceWorkerVersion::INSTALLING,
            update_helper->state_change_log_[0].status);

  EXPECT_EQ(registration->active_version()->version_id(),
            update_helper->state_change_log_[1].version_id);
  EXPECT_EQ(ServiceWorkerVersion::INSTALLED,
            update_helper->state_change_log_[1].status);

  EXPECT_EQ(first_version->version_id(),
            update_helper->state_change_log_[2].version_id);
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT,
            update_helper->state_change_log_[2].status);

  EXPECT_EQ(registration->active_version()->version_id(),
            update_helper->state_change_log_[3].version_id);
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATING,
            update_helper->state_change_log_[3].status);

  EXPECT_EQ(registration->active_version()->version_id(),
            update_helper->state_change_log_[4].version_id);
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED,
            update_helper->state_change_log_[4].status);

  EXPECT_TRUE(update_helper->update_found_);
}

// Test that the update job uses the script URL of the newest worker when the
// job starts, rather than when it is scheduled.
TEST_F(ServiceWorkerJobTest, Update_ScriptUrlChanged) {
  // Create a registration with an active version.
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/one/");
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(GURL("http://www.example.com/service_worker.js"), options);

  // Queue an Update. When this runs, it will use the waiting version's script.
  job_coordinator()->Update(registration.get(), false);

  // Add a waiting version with a new script.
  GURL new_script("http://www.example.com/new_worker.js");
  scoped_refptr<ServiceWorkerVersion> version = new ServiceWorkerVersion(
      registration.get(), new_script, 2L /* dummy version id */,
      helper_->context()->AsWeakPtr());
  registration->SetWaitingVersion(version);

  // Run the update job.
  base::RunLoop().RunUntilIdle();

  // The update job should have created a new version with the new script,
  // and promoted it to the active version.
  EXPECT_EQ(new_script, registration->active_version()->script_url());
  EXPECT_EQ(nullptr, registration->waiting_version());
  EXPECT_EQ(nullptr, registration->installing_version());
}

// Test that update succeeds if the incumbent worker was evicted
// during the update job (this can happen on disk cache failure).
TEST_F(ServiceWorkerJobTest, Update_EvictedIncumbent) {
  EvictIncumbentVersionHelper* update_helper = new EvictIncumbentVersionHelper;
  helper_.reset(update_helper);
  scoped_refptr<ServiceWorkerRegistration> registration =
      update_helper->SetupInitialRegistration(kNewVersionOrigin);
  ASSERT_TRUE(registration.get());
  update_helper->state_change_log_.clear();

  // Run the update job.
  registration->AddListener(update_helper);
  scoped_refptr<ServiceWorkerVersion> first_version =
      registration->active_version();
  first_version->StartUpdate();
  base::RunLoop().RunUntilIdle();

  // Verify results.
  ASSERT_TRUE(registration->active_version());
  EXPECT_NE(first_version.get(), registration->active_version());
  EXPECT_FALSE(registration->installing_version());
  EXPECT_FALSE(registration->waiting_version());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, first_version->status());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED,
            registration->active_version()->status());
  ASSERT_EQ(4u, update_helper->attribute_change_log_.size());
  EXPECT_TRUE(update_helper->update_found_);
  EXPECT_TRUE(update_helper->registration_failed_);
  EXPECT_FALSE(registration->is_uninstalled());
}

TEST_F(ServiceWorkerJobTest, Update_UninstallingRegistration) {
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/one/");
  bool called;
  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(GURL("http://www.example.com/service_worker.js"), options);

  // Add a controllee and queue an unregister to force the uninstalling state.
  std::unique_ptr<ServiceWorkerProviderHost> host = CreateControllee();
  ServiceWorkerVersion* active_version = registration->active_version();
  active_version->AddControllee(host.get());
  job_coordinator()->Unregister(GURL("http://www.example.com/one/"),
                                SaveUnregistration(SERVICE_WORKER_OK, &called));

  // Update should abort after it starts and sees uninstalling.
  job_coordinator()->Update(registration.get(), false);

  EXPECT_FALSE(called);
  base::RunLoop().RunUntilIdle();
  EXPECT_TRUE(called);

  // Verify the registration was not modified by the Update.
  EXPECT_TRUE(registration->is_uninstalling());
  EXPECT_EQ(active_version, registration->active_version());
  EXPECT_EQ(nullptr, registration->waiting_version());
  EXPECT_EQ(nullptr, registration->installing_version());
}

TEST_F(ServiceWorkerJobTest, RegisterWhileUninstalling) {
  GURL script1("http://www.example.com/service_worker.js");
  GURL script2("http://www.example.com/service_worker.js?new");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/one/");

  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script1, options);
  scoped_refptr<base::TestSimpleTaskRunner> runner(
      new base::TestSimpleTaskRunner());
  registration->SetTaskRunnerForTest(runner);

  // Add a controllee and queue an unregister to force the uninstalling state.
  std::unique_ptr<ServiceWorkerProviderHost> host = CreateControllee();
  scoped_refptr<ServiceWorkerVersion> old_version =
      registration->active_version();
  old_version->AddControllee(host.get());
  RunUnregisterJob(options.scope);

  // Register another script.
  EXPECT_EQ(registration, RunRegisterJob(script2, options));

  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_EQ(old_version, registration->active_version());

  scoped_refptr<ServiceWorkerVersion> new_version =
      registration->waiting_version();

  // Verify the new version is installed but not activated yet.
  EXPECT_EQ(nullptr, registration->installing_version());
  EXPECT_TRUE(new_version);
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, new_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::INSTALLED, new_version->status());

  old_version->RemoveControllee(host.get());
  base::RunLoop().RunUntilIdle();

  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_FALSE(registration->is_uninstalled());

  // Verify the new version is activated.
  EXPECT_EQ(nullptr, registration->installing_version());
  EXPECT_EQ(nullptr, registration->waiting_version());
  EXPECT_EQ(new_version, registration->active_version());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATING, new_version->status());

  runner->RunUntilIdle();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, new_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED, new_version->status());
}

TEST_F(ServiceWorkerJobTest, RegisterAndUnregisterWhileUninstalling) {
  GURL script1("http://www.example.com/service_worker.js");
  GURL script2("http://www.example.com/service_worker.js?new");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/one/");

  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script1, options);

  // Add a controllee and queue an unregister to force the uninstalling state.
  std::unique_ptr<ServiceWorkerProviderHost> host = CreateControllee();
  scoped_refptr<ServiceWorkerVersion> old_version =
      registration->active_version();
  old_version->AddControllee(host.get());
  RunUnregisterJob(options.scope);

  EXPECT_EQ(registration, RunRegisterJob(script2, options));

  EXPECT_EQ(registration, FindRegistrationForPattern(options.scope));
  scoped_refptr<ServiceWorkerVersion> new_version =
      registration->waiting_version();
  ASSERT_TRUE(new_version);

  // Unregister the registration (but it's still live).
  RunUnregisterJob(options.scope);
  // Now it's not found in the storage.
  RunUnregisterJob(options.scope, SERVICE_WORKER_ERROR_NOT_FOUND);

  FindRegistrationForPattern(options.scope, SERVICE_WORKER_ERROR_NOT_FOUND);
  EXPECT_TRUE(registration->is_uninstalling());
  EXPECT_EQ(old_version, registration->active_version());

  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, old_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED, old_version->status());
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, new_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::INSTALLED, new_version->status());

  old_version->RemoveControllee(host.get());
  base::RunLoop().RunUntilIdle();

  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_TRUE(registration->is_uninstalled());

  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, old_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, old_version->status());
  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, new_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, new_version->status());
}

TEST_F(ServiceWorkerJobTest, RegisterSameScriptMultipleTimesWhileUninstalling) {
  GURL script1("http://www.example.com/service_worker.js");
  GURL script2("http://www.example.com/service_worker.js?new");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/one/");

  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script1, options);
  scoped_refptr<base::TestSimpleTaskRunner> runner(
      new base::TestSimpleTaskRunner());
  registration->SetTaskRunnerForTest(runner);

  // Add a controllee and queue an unregister to force the uninstalling state.
  std::unique_ptr<ServiceWorkerProviderHost> host = CreateControllee();
  scoped_refptr<ServiceWorkerVersion> old_version =
      registration->active_version();
  old_version->AddControllee(host.get());
  RunUnregisterJob(options.scope);

  EXPECT_EQ(registration, RunRegisterJob(script2, options));

  scoped_refptr<ServiceWorkerVersion> new_version =
      registration->waiting_version();
  ASSERT_TRUE(new_version);

  RunUnregisterJob(options.scope);

  EXPECT_TRUE(registration->is_uninstalling());

  EXPECT_EQ(registration, RunRegisterJob(script2, options));

  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_EQ(new_version, registration->waiting_version());

  old_version->RemoveControllee(host.get());
  base::RunLoop().RunUntilIdle();

  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_FALSE(registration->is_uninstalled());

  // Verify the new version is activated.
  EXPECT_EQ(nullptr, registration->installing_version());
  EXPECT_EQ(nullptr, registration->waiting_version());
  EXPECT_EQ(new_version, registration->active_version());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATING, new_version->status());

  runner->RunUntilIdle();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, new_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED, new_version->status());
}

TEST_F(ServiceWorkerJobTest, RegisterMultipleTimesWhileUninstalling) {
  GURL script1("http://www.example.com/service_worker.js?first");
  GURL script2("http://www.example.com/service_worker.js?second");
  GURL script3("http://www.example.com/service_worker.js?third");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/one/");

  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script1, options);
  scoped_refptr<base::TestSimpleTaskRunner> runner(
      new base::TestSimpleTaskRunner());
  registration->SetTaskRunnerForTest(runner);

  // Add a controllee and queue an unregister to force the uninstalling state.
  std::unique_ptr<ServiceWorkerProviderHost> host = CreateControllee();
  scoped_refptr<ServiceWorkerVersion> first_version =
      registration->active_version();
  first_version->AddControllee(host.get());
  RunUnregisterJob(options.scope);

  EXPECT_EQ(registration, RunRegisterJob(script2, options));

  scoped_refptr<ServiceWorkerVersion> second_version =
      registration->waiting_version();
  ASSERT_TRUE(second_version);

  RunUnregisterJob(options.scope);

  EXPECT_TRUE(registration->is_uninstalling());

  EXPECT_EQ(registration, RunRegisterJob(script3, options));

  scoped_refptr<ServiceWorkerVersion> third_version =
      registration->waiting_version();
  ASSERT_TRUE(third_version);

  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, second_version->status());

  first_version->RemoveControllee(host.get());
  base::RunLoop().RunUntilIdle();

  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_FALSE(registration->is_uninstalled());

  // Verify the new version is activated.
  EXPECT_EQ(nullptr, registration->installing_version());
  EXPECT_EQ(nullptr, registration->waiting_version());
  EXPECT_EQ(third_version, registration->active_version());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATING, third_version->status());

  runner->RunUntilIdle();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, third_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED, third_version->status());
}

class EventCallbackHelper : public EmbeddedWorkerTestHelper {
 public:
  EventCallbackHelper()
      : EmbeddedWorkerTestHelper(base::FilePath()),
        install_event_result_(
            blink::mojom::ServiceWorkerEventStatus::COMPLETED),
        activate_event_result_(
            blink::mojom::ServiceWorkerEventStatus::COMPLETED) {}

  void OnInstallEvent(
      mojom::ServiceWorkerEventDispatcher::DispatchInstallEventCallback
          callback) override {
    if (!install_callback_.is_null())
      install_callback_.Run();
    std::move(callback).Run(install_event_result_, has_fetch_handler_,
                            base::Time::Now());
  }

  void OnActivateEvent(
      mojom::ServiceWorkerEventDispatcher::DispatchActivateEventCallback
          callback) override {
    std::move(callback).Run(activate_event_result_, base::Time::Now());
  }

  void set_install_callback(const base::Closure& callback) {
    install_callback_ = callback;
  }
  void set_install_event_result(blink::mojom::ServiceWorkerEventStatus result) {
    install_event_result_ = result;
  }
  void set_activate_event_result(
      blink::mojom::ServiceWorkerEventStatus result) {
    activate_event_result_ = result;
  }
  void set_has_fetch_handler(bool has_fetch_handler) {
    has_fetch_handler_ = has_fetch_handler;
  }

 private:
  base::Closure install_callback_;
  blink::mojom::ServiceWorkerEventStatus install_event_result_;
  blink::mojom::ServiceWorkerEventStatus activate_event_result_;
  bool has_fetch_handler_ = true;
};

TEST_F(ServiceWorkerJobTest, RemoveControlleeDuringInstall) {
  EventCallbackHelper* helper = new EventCallbackHelper;
  helper_.reset(helper);

  GURL script1("http://www.example.com/service_worker.js");
  GURL script2("http://www.example.com/service_worker.js?new");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/one/");

  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script1, options);

  // Add a controllee and queue an unregister to force the uninstalling state.
  std::unique_ptr<ServiceWorkerProviderHost> host = CreateControllee();
  scoped_refptr<ServiceWorkerVersion> old_version =
      registration->active_version();
  old_version->AddControllee(host.get());
  RunUnregisterJob(options.scope);

  // Register another script. While installing, old_version loses controllee.
  helper->set_install_callback(
      base::Bind(&ServiceWorkerVersion::RemoveControllee,
                 old_version, host.get()));
  EXPECT_EQ(registration, RunRegisterJob(script2, options));

  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_FALSE(registration->is_uninstalled());

  // Verify the new version is activated.
  scoped_refptr<ServiceWorkerVersion> new_version =
      registration->active_version();
  EXPECT_NE(old_version, new_version);
  EXPECT_EQ(nullptr, registration->installing_version());
  EXPECT_EQ(nullptr, registration->waiting_version());
  EXPECT_EQ(new_version, registration->active_version());
  EXPECT_EQ(EmbeddedWorkerStatus::RUNNING, new_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATED, new_version->status());

  EXPECT_EQ(registration, FindRegistrationForPattern(options.scope));
}

TEST_F(ServiceWorkerJobTest, RemoveControlleeDuringRejectedInstall) {
  EventCallbackHelper* helper = new EventCallbackHelper;
  helper_.reset(helper);

  GURL script1("http://www.example.com/service_worker.js");
  GURL script2("http://www.example.com/service_worker.js?new");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/one/");

  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script1, options);

  // Add a controllee and queue an unregister to force the uninstalling state.
  std::unique_ptr<ServiceWorkerProviderHost> host = CreateControllee();
  scoped_refptr<ServiceWorkerVersion> old_version =
      registration->active_version();
  old_version->AddControllee(host.get());
  RunUnregisterJob(options.scope);

  // Register another script that fails to install. While installing,
  // old_version loses controllee.
  helper->set_install_callback(
      base::Bind(&ServiceWorkerVersion::RemoveControllee,
                 old_version, host.get()));
  helper->set_install_event_result(
      blink::mojom::ServiceWorkerEventStatus::REJECTED);
  EXPECT_EQ(registration, RunRegisterJob(script2, options));

  // Verify the registration was uninstalled.
  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_TRUE(registration->is_uninstalled());

  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, old_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, old_version->status());

  FindRegistrationForPattern(options.scope, SERVICE_WORKER_ERROR_NOT_FOUND);
}

TEST_F(ServiceWorkerJobTest, RemoveControlleeDuringInstall_RejectActivate) {
  EventCallbackHelper* helper = new EventCallbackHelper;
  helper_.reset(helper);

  GURL script1("http://www.example.com/service_worker.js");
  GURL script2("http://www.example.com/service_worker.js?new");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/one/");

  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script1, options);

  // Add a controllee and queue an unregister to force the uninstalling state.
  std::unique_ptr<ServiceWorkerProviderHost> host = CreateControllee();
  scoped_refptr<ServiceWorkerVersion> old_version =
      registration->active_version();
  old_version->AddControllee(host.get());
  RunUnregisterJob(options.scope);

  // Register another script that fails to activate. While installing,
  // old_version loses controllee.
  helper->set_install_callback(
      base::Bind(&ServiceWorkerVersion::RemoveControllee,
                 old_version, host.get()));
  helper->set_activate_event_result(
      blink::mojom::ServiceWorkerEventStatus::REJECTED);
  EXPECT_EQ(registration, RunRegisterJob(script2, options));

  // Verify the registration remains.
  EXPECT_FALSE(registration->is_uninstalling());
  EXPECT_FALSE(registration->is_uninstalled());

  EXPECT_EQ(EmbeddedWorkerStatus::STOPPED, old_version->running_status());
  EXPECT_EQ(ServiceWorkerVersion::REDUNDANT, old_version->status());

  FindRegistrationForPattern(options.scope, SERVICE_WORKER_OK);
}

TEST_F(ServiceWorkerJobTest, HasFetchHandler) {
  EventCallbackHelper* helper = new EventCallbackHelper;
  helper_.reset(helper);

  GURL script("http://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/");
  scoped_refptr<ServiceWorkerRegistration> registration;

  helper->set_has_fetch_handler(true);
  RunRegisterJob(script, options);
  registration = FindRegistrationForPattern(options.scope);
  EXPECT_EQ(ServiceWorkerVersion::FetchHandlerExistence::EXISTS,
            registration->active_version()->fetch_handler_existence());
  RunUnregisterJob(options.scope);

  helper->set_has_fetch_handler(false);
  RunRegisterJob(script, options);
  registration = FindRegistrationForPattern(options.scope);
  EXPECT_EQ(ServiceWorkerVersion::FetchHandlerExistence::DOES_NOT_EXIST,
            registration->active_version()->fetch_handler_existence());
  RunUnregisterJob(options.scope);
}

class CheckPauseAfterDownloadEmbeddedWorkerInstanceClient
    : public EmbeddedWorkerTestHelper::MockEmbeddedWorkerInstanceClient {
 public:
  explicit CheckPauseAfterDownloadEmbeddedWorkerInstanceClient(
      base::WeakPtr<EmbeddedWorkerTestHelper> helper)
      : EmbeddedWorkerTestHelper::MockEmbeddedWorkerInstanceClient(helper) {}
  int num_of_startworker() const { return num_of_startworker_; }
  void set_next_pause_after_download(bool expectation) {
    next_pause_after_download_ = expectation;
  }

 protected:
  void StartWorker(mojom::EmbeddedWorkerStartParamsPtr params) override {
    ASSERT_TRUE(next_pause_after_download_.has_value());
    EXPECT_EQ(next_pause_after_download_.value(), params->pause_after_download);
    num_of_startworker_++;
    EmbeddedWorkerTestHelper::MockEmbeddedWorkerInstanceClient::StartWorker(
        std::move(params));
  }

 private:
  base::Optional<bool> next_pause_after_download_;
  int num_of_startworker_ = 0;
  DISALLOW_COPY_AND_ASSIGN(CheckPauseAfterDownloadEmbeddedWorkerInstanceClient);
};

TEST_F(ServiceWorkerJobTest, Update_PauseAfterDownload) {
  UpdateJobTestHelper* update_helper = new UpdateJobTestHelper;
  helper_.reset(update_helper);

  std::vector<CheckPauseAfterDownloadEmbeddedWorkerInstanceClient*> clients;
  clients.push_back(helper_->CreateAndRegisterMockInstanceClient<
                    CheckPauseAfterDownloadEmbeddedWorkerInstanceClient>(
      helper_->AsWeakPtr()));
  clients.push_back(helper_->CreateAndRegisterMockInstanceClient<
                    CheckPauseAfterDownloadEmbeddedWorkerInstanceClient>(
      helper_->AsWeakPtr()));

  // The initial version should not pause after download.
  clients[0]->set_next_pause_after_download(false);
  scoped_refptr<ServiceWorkerRegistration> registration =
      update_helper->SetupInitialRegistration(kNewVersionOrigin);
  ASSERT_EQ(1, clients[0]->num_of_startworker());

  // The updated version should pause after download.
  clients[1]->set_next_pause_after_download(true);
  registration->AddListener(update_helper);
  registration->active_version()->StartUpdate();
  base::RunLoop().RunUntilIdle();
  ASSERT_EQ(1, clients[1]->num_of_startworker());
}

// Test that activation doesn't complete if it's triggered by removing a
// controllee and starting the worker failed due to shutdown.
TEST_F(ServiceWorkerJobTest, ActivateCancelsOnShutdown) {
  UpdateJobTestHelper* update_helper = new UpdateJobTestHelper;
  helper_.reset(update_helper);
  GURL script("http://www.example.com/service_worker.js");
  blink::mojom::ServiceWorkerRegistrationOptions options;
  options.scope = GURL("http://www.example.com/");

  scoped_refptr<ServiceWorkerRegistration> registration =
      RunRegisterJob(script, options);
  scoped_refptr<base::TestSimpleTaskRunner> runner(
      new base::TestSimpleTaskRunner());
  registration->SetTaskRunnerForTest(runner);

  // Add a controllee.
  std::unique_ptr<ServiceWorkerProviderHost> host = CreateControllee();
  scoped_refptr<ServiceWorkerVersion> first_version =
      registration->active_version();
  first_version->AddControllee(host.get());

  // Update. The new version should be waiting.
  registration->AddListener(update_helper);
  first_version->StartUpdate();
  base::RunLoop().RunUntilIdle();
  scoped_refptr<ServiceWorkerVersion> new_version =
      registration->waiting_version();
  ASSERT_TRUE(new_version);
  EXPECT_EQ(ServiceWorkerVersion::INSTALLED, new_version->status());

  // Stop the worker so that it must start again when activation is attempted.
  // (This is not strictly necessary to exercise the codepath, but it makes it
  // easy to cause a failure with set_force_start_worker_failure after
  // shutdown is simulated. Otherwise our test helper often fails on
  // DCHECK(context)).
  new_version->StopWorker(base::DoNothing());

  // Remove the controllee. The new version should be activating, and delayed
  // until the runner runs again.
  first_version->RemoveControllee(host.get());
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(new_version.get(), registration->active_version());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATING, new_version->status());

  // Shutdown.
  update_helper->context()->wrapper()->Shutdown();
  update_helper->set_force_start_worker_failure(true);

  // Allow the activation to continue. It will fail, and the worker
  // should not be promoted to ACTIVATED because failure occur
  // during shutdown.
  runner->RunPendingTasks();
  base::RunLoop().RunUntilIdle();
  EXPECT_EQ(new_version.get(), registration->active_version());
  EXPECT_EQ(ServiceWorkerVersion::ACTIVATING, new_version->status());
  registration->RemoveListener(update_helper);
}

}  // namespace content
