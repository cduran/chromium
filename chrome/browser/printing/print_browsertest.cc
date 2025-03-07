// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>

#include "base/auto_reset.h"
#include "base/run_loop.h"
#include "chrome/browser/printing/print_view_manager_common.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "chrome/browser/ui/webui/print_preview/print_preview_ui.h"
#include "chrome/common/pref_names.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/ui_test_utils.h"
#include "components/prefs/pref_service.h"
#include "components/printing/browser/print_composite_client.h"
#include "components/printing/browser/print_manager_utils.h"
#include "components/printing/common/print_messages.h"
#include "content/public/browser/browser_message_filter.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/render_process_host.h"
#include "content/public/common/content_switches.h"
#include "content/public/test/browser_test_utils.h"
#include "net/dns/mock_host_resolver.h"
#include "net/test/embedded_test_server/embedded_test_server.h"

namespace printing {

namespace {

static constexpr int kDefaultDocumentCookie = 1234;

class PrintPreviewObserver : PrintPreviewUI::TestingDelegate {
 public:
  PrintPreviewObserver() { PrintPreviewUI::SetDelegateForTesting(this); }
  ~PrintPreviewObserver() { PrintPreviewUI::SetDelegateForTesting(nullptr); }

  void WaitUntilPreviewIsReady() {
    if (rendered_page_count_ >= total_page_count_)
      return;

    base::RunLoop run_loop;
    base::AutoReset<base::RunLoop*> auto_reset(&run_loop_, &run_loop);
    run_loop.Run();
  }

 private:
  // PrintPreviewUI::TestingDelegate implementation.
  void DidGetPreviewPageCount(int page_count) override {
    total_page_count_ = page_count;
  }

  // PrintPreviewUI::TestingDelegate implementation.
  void DidRenderPreviewPage(content::WebContents* preview_dialog) override {
    ++rendered_page_count_;
    CHECK(rendered_page_count_ <= total_page_count_);
    if (rendered_page_count_ == total_page_count_ && run_loop_) {
      run_loop_->Quit();
    }
  }

  int total_page_count_ = 1;
  int rendered_page_count_ = 0;
  base::RunLoop* run_loop_ = nullptr;
};

class TestPrintFrameContentMsgFilter : public content::BrowserMessageFilter {
 public:
  TestPrintFrameContentMsgFilter(int document_cookie,
                                 base::RepeatingClosure msg_callback)
      : content::BrowserMessageFilter(PrintMsgStart),
        document_cookie_(document_cookie),
        task_runner_(base::SequencedTaskRunnerHandle::Get()),
        msg_callback_(msg_callback) {}

  bool OnMessageReceived(const IPC::Message& message) override {
    // Only expect PrintHostMsg_DidPrintFrameContent message.
    bool handled = true;
    IPC_BEGIN_MESSAGE_MAP(TestPrintFrameContentMsgFilter, message)
      IPC_MESSAGE_HANDLER(PrintHostMsg_DidPrintFrameContent, CheckMessage)
      IPC_MESSAGE_UNHANDLED(handled = false)
    IPC_END_MESSAGE_MAP()
    EXPECT_TRUE(handled);
    task_runner_->PostTask(FROM_HERE, msg_callback_);
    return true;
  }

 private:
  ~TestPrintFrameContentMsgFilter() override {}

  void CheckMessage(int document_cookie,
                    const PrintHostMsg_DidPrintContent_Params& param) {
    EXPECT_EQ(document_cookie, document_cookie_);
    EXPECT_TRUE(param.metafile_data_handle.IsValid());
    EXPECT_GT(param.data_size, 0u);
  }

  const int document_cookie_;
  scoped_refptr<base::SequencedTaskRunner> task_runner_;
  base::RepeatingClosure msg_callback_;
};

class KillPrintFrameContentMsgFilter : public content::BrowserMessageFilter {
 public:
  explicit KillPrintFrameContentMsgFilter(content::RenderProcessHost* rph)
      : content::BrowserMessageFilter(PrintMsgStart), rph_(rph) {}

  bool OnMessageReceived(const IPC::Message& message) override {
    // Only handle PrintHostMsg_DidPrintFrameContent message.
    bool handled = true;
    IPC_BEGIN_MESSAGE_MAP(KillPrintFrameContentMsgFilter, message)
      IPC_MESSAGE_HANDLER(PrintHostMsg_DidPrintFrameContent, KillRenderProcess)
      IPC_MESSAGE_UNHANDLED(handled = false)
    IPC_END_MESSAGE_MAP()
    return handled;
  }

 private:
  ~KillPrintFrameContentMsgFilter() override {}

  void KillRenderProcess(int document_cookie,
                         const PrintHostMsg_DidPrintContent_Params& param) {
    rph_->Shutdown(0);
  }

  content::RenderProcessHost* rph_;
};

}  // namespace

class PrintBrowserTest : public InProcessBrowserTest {
 public:
  PrintBrowserTest() {}
  ~PrintBrowserTest() override {}

  void SetUp() override {
    num_expected_messages_ = 1;  // By default, only wait on one message.
    num_received_messages_ = 0;
    run_loop_.reset();
    InProcessBrowserTest::SetUp();
  }

  void SetUpOnMainThread() override {
    host_resolver()->AddRule("*", "127.0.0.1");
    content::SetupCrossSiteRedirector(embedded_test_server());
    ASSERT_TRUE(embedded_test_server()->Start());
  }

  void PrintAndWaitUntilPreviewIsReady(bool print_only_selection) {
    PrintPreviewObserver print_preview_observer;

    printing::StartPrint(browser()->tab_strip_model()->GetActiveWebContents(),
                         /*print_preview_disabled=*/false,
                         print_only_selection);

    print_preview_observer.WaitUntilPreviewIsReady();
  }

  // The following are helper functions for having a wait loop in the test and
  // exit when all expected messages are received.
  void SetNumExpectedMessages(unsigned int num) {
    num_expected_messages_ = num;
  }

  void WaitUntilMessagesReceived() {
    run_loop_ = std::make_unique<base::RunLoop>();
    run_loop_->Run();
  }

  void CheckForQuit() {
    if (++num_received_messages_ == num_expected_messages_) {
      run_loop_->QuitWhenIdle();
    }
  }

  void AddFilterForFrame(content::RenderFrameHost* frame_host) {
    auto filter = base::MakeRefCounted<TestPrintFrameContentMsgFilter>(
        kDefaultDocumentCookie,
        base::BindRepeating(&PrintBrowserTest::CheckForQuit,
                            base::Unretained(this)));
    frame_host->GetProcess()->AddFilter(filter.get());
  }

  PrintMsg_PrintFrame_Params GetDefaultPrintParams() {
    PrintMsg_PrintFrame_Params params;
    gfx::Rect area(800, 600);
    params.printable_area = area;
    params.document_cookie = kDefaultDocumentCookie;
    return params;
  }

 private:
  unsigned int num_expected_messages_;
  unsigned int num_received_messages_;
  std::unique_ptr<base::RunLoop> run_loop_;
};

class SitePerProcessPrintBrowserTest : public PrintBrowserTest {
 public:
  SitePerProcessPrintBrowserTest() {}
  ~SitePerProcessPrintBrowserTest() override {}

  // content::BrowserTestBase
  void SetUpCommandLine(base::CommandLine* command_line) override {
    content::IsolateAllSitesForTesting(command_line);
  }
};

class IsolateOriginsPrintBrowserTest : public PrintBrowserTest {
 public:
  static constexpr char kIsolatedSite[] = "b.com";

  IsolateOriginsPrintBrowserTest() {}
  ~IsolateOriginsPrintBrowserTest() override {}

  // content::BrowserTestBase
  void SetUpCommandLine(base::CommandLine* command_line) override {
    ASSERT_TRUE(embedded_test_server()->Start());

    std::string origin_list =
        embedded_test_server()->GetURL(kIsolatedSite, "/").spec();
    command_line->AppendSwitchASCII(switches::kIsolateOrigins, origin_list);
  }

  void SetUpOnMainThread() override {
    host_resolver()->AddRule("*", "127.0.0.1");
  }
};

constexpr char IsolateOriginsPrintBrowserTest::kIsolatedSite[];

// Printing only a selection containing iframes is partially supported.
// Iframes aren't currently displayed. This test passes whenever the print
// preview is rendered (i.e. no timeout in the test).
// This test shouldn't crash. See https://crbug.com/732780.
IN_PROC_BROWSER_TEST_F(PrintBrowserTest, SelectionContainsIframe) {
  ASSERT_TRUE(embedded_test_server()->Started());
  GURL url(embedded_test_server()->GetURL("/printing/selection_iframe.html"));
  ui_test_utils::NavigateToURL(browser(), url);

  PrintAndWaitUntilPreviewIsReady(/*print_only_selection=*/true);
}

// Printing frame content for the main frame of a generic webpage.
// This test passes when the printed result is sent back and checked in
// TestPrintFrameContentMsgFilter::CheckMessage().
IN_PROC_BROWSER_TEST_F(PrintBrowserTest, PrintFrameContent) {
  ASSERT_TRUE(embedded_test_server()->Started());
  GURL url(embedded_test_server()->GetURL("/printing/test1.html"));
  ui_test_utils::NavigateToURL(browser(), url);

  content::WebContents* original_contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  content::RenderFrameHost* rfh = original_contents->GetMainFrame();
  AddFilterForFrame(rfh);

  rfh->Send(new PrintMsg_PrintFrameContent(rfh->GetRoutingID(),
                                           GetDefaultPrintParams()));

  // The printed result will be received and checked in
  // TestPrintFrameContentMsgFilter.
  WaitUntilMessagesReceived();
}

// Printing frame content for a cross-site iframe.
// This test passes when the iframe responds to the print message.
// The response is checked in TestPrintFrameContentMsgFilter::CheckMessage().
IN_PROC_BROWSER_TEST_F(PrintBrowserTest, PrintSubframeContent) {
  ASSERT_TRUE(embedded_test_server()->Started());
  GURL url(
      embedded_test_server()->GetURL("/printing/content_with_iframe.html"));
  ui_test_utils::NavigateToURL(browser(), url);

  content::WebContents* original_contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_EQ(2u, original_contents->GetAllFrames().size());
  content::RenderFrameHost* test_frame = original_contents->GetAllFrames()[1];
  ASSERT_TRUE(test_frame);

  AddFilterForFrame(test_frame);

  test_frame->Send(new PrintMsg_PrintFrameContent(test_frame->GetRoutingID(),
                                                  GetDefaultPrintParams()));

  // The printed result will be received and checked in
  // TestPrintFrameContentMsgFilter.
  WaitUntilMessagesReceived();
}

// Printing frame content with a cross-site iframe which also has a cross-site
// iframe. The site reference chain is a.com --> b.com --> c.com.
// This test passes when both cross-site frames are printed and their
// responses which are checked in
// TestPrintFrameContentMsgFilter::CheckMessage().
IN_PROC_BROWSER_TEST_F(PrintBrowserTest, PrintSubframeChain) {
  ASSERT_TRUE(embedded_test_server()->Started());
  GURL url(embedded_test_server()->GetURL(
      "/printing/content_with_iframe_chain.html"));
  ui_test_utils::NavigateToURL(browser(), url);
  content::WebContents* original_contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_EQ(3u, original_contents->GetAllFrames().size());
  // Create composite client so subframe print message can be forwarded.
  PrintCompositeClient::CreateForWebContents(original_contents);

  content::RenderFrameHost* main_frame = original_contents->GetMainFrame();
  content::RenderFrameHost* child_frame = content::ChildFrameAt(main_frame, 0);
  ASSERT_TRUE(child_frame);
  ASSERT_NE(child_frame, main_frame);
  bool oopif_enabled = child_frame->GetProcess() != main_frame->GetProcess();

  content::RenderFrameHost* grandchild_frame =
      content::ChildFrameAt(child_frame, 0);
  ASSERT_TRUE(grandchild_frame);
  ASSERT_NE(grandchild_frame, child_frame);
  if (oopif_enabled) {
    ASSERT_NE(grandchild_frame->GetProcess(), child_frame->GetProcess());
    ASSERT_NE(grandchild_frame->GetProcess(), main_frame->GetProcess());
  }

  AddFilterForFrame(main_frame);
  if (oopif_enabled) {
    AddFilterForFrame(child_frame);
    AddFilterForFrame(grandchild_frame);
  }

  main_frame->Send(new PrintMsg_PrintFrameContent(main_frame->GetRoutingID(),
                                                  GetDefaultPrintParams()));

  // The printed result will be received and checked in
  // TestPrintFrameContentMsgFilter.
  SetNumExpectedMessages(oopif_enabled ? 3 : 1);
  WaitUntilMessagesReceived();
}

// Printing frame content with a cross-site iframe who also has a cross site
// iframe, but this iframe resides in the same site as the main frame.
// The site reference loop is a.com --> b.com --> a.com.
// This test passes when both cross-site frames are printed and send back
// responses which are checked in
// TestPrintFrameContentMsgFilter::CheckMessage().
IN_PROC_BROWSER_TEST_F(PrintBrowserTest, PrintSubframeABA) {
  ASSERT_TRUE(embedded_test_server()->Started());
  GURL url(embedded_test_server()->GetURL(
      "a.com", "/printing/content_with_iframe_loop.html"));
  ui_test_utils::NavigateToURL(browser(), url);
  content::WebContents* original_contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_EQ(3u, original_contents->GetAllFrames().size());
  // Create composite client so subframe print message can be forwarded.
  PrintCompositeClient::CreateForWebContents(original_contents);

  content::RenderFrameHost* main_frame = original_contents->GetMainFrame();
  content::RenderFrameHost* child_frame = content::ChildFrameAt(main_frame, 0);
  ASSERT_TRUE(child_frame);
  ASSERT_NE(child_frame, main_frame);
  bool oopif_enabled = main_frame->GetProcess() != child_frame->GetProcess();

  content::RenderFrameHost* grandchild_frame =
      content::ChildFrameAt(child_frame, 0);
  ASSERT_TRUE(grandchild_frame);
  ASSERT_NE(grandchild_frame, child_frame);
  // |grandchild_frame| is in the same site as |frame|, so whether OOPIF is
  // enabled, they will be in the same process.
  ASSERT_EQ(grandchild_frame->GetProcess(), main_frame->GetProcess());

  AddFilterForFrame(main_frame);
  if (oopif_enabled)
    AddFilterForFrame(child_frame);

  main_frame->Send(new PrintMsg_PrintFrameContent(main_frame->GetRoutingID(),
                                                  GetDefaultPrintParams()));

  // The printed result will be received and checked in
  // TestPrintFrameContentMsgFilter.
  SetNumExpectedMessages(oopif_enabled ? 3 : 1);
  WaitUntilMessagesReceived();
}

// Printing preview a simple webpage when site per process is enabled.
// Test that the basic oopif printing should succeed. The test should not crash
// or timed out. There could be other reasons that cause the test fail, but the
// most obvious ones would be font access outage or web sandbox support being
// absent because we explicitly check these when pdf compositor service starts.
IN_PROC_BROWSER_TEST_F(SitePerProcessPrintBrowserTest, BasicPrint) {
  ASSERT_TRUE(embedded_test_server()->Started());
  GURL url(embedded_test_server()->GetURL("/printing/test1.html"));
  ui_test_utils::NavigateToURL(browser(), url);

  PrintAndWaitUntilPreviewIsReady(/*print_only_selection=*/false);
}

// Printing a web page with a dead subframe for site per process should succeed.
// This test passes whenever the print preview is rendered. This should not be
// a timed out test which indicates the print preview hung.
IN_PROC_BROWSER_TEST_F(SitePerProcessPrintBrowserTest,
                       SubframeUnavailableBeforePrint) {
  ASSERT_TRUE(embedded_test_server()->Started());
  GURL url(
      embedded_test_server()->GetURL("/printing/content_with_iframe.html"));
  ui_test_utils::NavigateToURL(browser(), url);

  content::WebContents* original_contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_EQ(2u, original_contents->GetAllFrames().size());
  content::RenderFrameHost* test_frame = original_contents->GetAllFrames()[1];
  ASSERT_TRUE(test_frame);
  ASSERT_TRUE(test_frame->IsRenderFrameLive());
  // Wait for the renderer to be down.
  content::RenderProcessHostWatcher render_process_watcher(
      test_frame->GetProcess(),
      content::RenderProcessHostWatcher::WATCH_FOR_PROCESS_EXIT);
  // Shutdown the subframe.
  ASSERT_TRUE(test_frame->GetProcess()->Shutdown(0));
  render_process_watcher.Wait();
  ASSERT_FALSE(test_frame->IsRenderFrameLive());

  PrintAndWaitUntilPreviewIsReady(/*print_only_selection=*/false);
}

// If a subframe dies during printing, the page printing should still succeed.
// This test passes whenever the print preview is rendered. This should not be
// a timed out test which indicates the print preview hung.
IN_PROC_BROWSER_TEST_F(SitePerProcessPrintBrowserTest,
                       SubframeUnavailableDuringPrint) {
  ASSERT_TRUE(embedded_test_server()->Started());
  GURL url(
      embedded_test_server()->GetURL("/printing/content_with_iframe.html"));
  ui_test_utils::NavigateToURL(browser(), url);

  content::WebContents* original_contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  ASSERT_EQ(2u, original_contents->GetAllFrames().size());
  content::RenderFrameHost* subframe = original_contents->GetAllFrames()[1];
  ASSERT_TRUE(subframe);
  auto* subframe_rph = subframe->GetProcess();

  auto filter =
      base::MakeRefCounted<KillPrintFrameContentMsgFilter>(subframe_rph);
  subframe_rph->AddFilter(filter.get());

  PrintAndWaitUntilPreviewIsReady(/*print_only_selection=*/false);
}

// Printing preview a web page with an iframe from an isolated origin.
// This test passes whenever the print preview is rendered. This should not be
// a timed out test which indicates the print preview hung or crash.
IN_PROC_BROWSER_TEST_F(IsolateOriginsPrintBrowserTest, PrintIsolatedSubframe) {
  ASSERT_TRUE(embedded_test_server()->Started());
  GURL url(embedded_test_server()->GetURL(
      "/printing/content_with_same_site_iframe.html"));
  GURL isolated_url(
      embedded_test_server()->GetURL(kIsolatedSite, "/printing/test1.html"));
  ui_test_utils::NavigateToURL(browser(), url);

  content::WebContents* original_contents =
      browser()->tab_strip_model()->GetActiveWebContents();
  EXPECT_TRUE(NavigateIframeToURL(original_contents, "iframe", isolated_url));

  ASSERT_EQ(2u, original_contents->GetAllFrames().size());
  auto* main_frame = original_contents->GetMainFrame();
  auto* subframe = original_contents->GetAllFrames()[1];
  ASSERT_NE(main_frame->GetProcess(), subframe->GetProcess());

  PrintAndWaitUntilPreviewIsReady(/*print_only_selection=*/false);
}

// Printing preview a webpage.
// Test that we won't use oopif printing by default, unless the
// test is run with site-per-process flag enabled.
IN_PROC_BROWSER_TEST_F(PrintBrowserTest, RegularPrinting) {
  ASSERT_TRUE(embedded_test_server()->Started());
  GURL url(embedded_test_server()->GetURL("/printing/test1.html"));
  ui_test_utils::NavigateToURL(browser(), url);

  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kSitePerProcess)) {
    EXPECT_TRUE(IsOopifEnabled());
  } else {
    EXPECT_FALSE(IsOopifEnabled());
  }
}

// Printing preview a webpage with isolate-origins enabled.
// Test that we will use oopif printing for this case.
IN_PROC_BROWSER_TEST_F(IsolateOriginsPrintBrowserTest, OopifPrinting) {
  ASSERT_TRUE(embedded_test_server()->Started());
  GURL url(embedded_test_server()->GetURL("/printing/test1.html"));
  ui_test_utils::NavigateToURL(browser(), url);

  EXPECT_TRUE(IsOopifEnabled());
}

}  // namespace printing
