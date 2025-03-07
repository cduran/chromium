// Copyright 2013 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/test/base/chrome_test_launcher.h"

#include "base/command_line.h"
#include "build/build_config.h"
#include "chrome/test/base/chrome_test_suite.h"
#include "chrome/test/base/in_process_browser_test.h"
#include "chrome/test/base/interactive_test_utils.h"
#include "gpu/ipc/service/image_transport_surface.h"
#include "ui/base/test/ui_controls.h"

#if defined(USE_AURA)
#include "ui/aura/test/ui_controls_factory_aura.h"
#include "ui/base/test/ui_controls_aura.h"
#if defined(USE_X11)
#include "ui/views/test/ui_controls_factory_desktop_aurax11.h"
#endif
#endif

#if defined(OS_CHROMEOS)
#include "ash/test/ui_controls_factory_ash.h"
#endif

#if defined(OS_WIN)
#include "base/win/scoped_com_initializer.h"
#include "chrome/test/base/always_on_top_window_killer_win.h"
#endif

class InteractiveUITestSuite : public ChromeTestSuite {
 public:
  InteractiveUITestSuite(int argc, char** argv) : ChromeTestSuite(argc, argv) {}
  ~InteractiveUITestSuite() override {}

 protected:
  // ChromeTestSuite overrides:
  void Initialize() override {
    ChromeTestSuite::Initialize();

#if defined(OS_MACOSX)
    gpu::ImageTransportSurface::SetAllowOSMesaForTesting(true);
#endif

    // Only allow ui_controls to be used in interactive_ui_tests, since they
    // depend on focus and can't be sharded.
    ui_controls::EnableUIControls();

#if defined(OS_CHROMEOS)
    ui_controls::InstallUIControlsAura(ash::test::CreateAshUIControls());
#elif defined(USE_AURA)
#if defined(OS_WIN)
    com_initializer_.reset(new base::win::ScopedCOMInitializer());
#endif

#if defined(OS_LINUX)
#if defined(USE_OZONE)
    NOTIMPLEMENTED();
#else
    ui_controls::InstallUIControlsAura(
        views::test::CreateUIControlsDesktopAura());
#endif  // defined(USE_OZONE)
#else
    // TODO(win_ash): when running interactive_ui_tests for Win Ash, use above.
    ui_controls::InstallUIControlsAura(aura::test::CreateUIControlsAura(NULL));
#endif  // defined(OS_LINUX)
#endif  // defined(USE_AURA)
  }

  void Shutdown() override {
#if defined(OS_WIN)
    com_initializer_.reset();
#endif
  }

 private:
#if defined(OS_WIN)
  std::unique_ptr<base::win::ScopedCOMInitializer> com_initializer_;
#endif
};

class InteractiveUITestLauncherDelegate : public ChromeTestLauncherDelegate {
 public:
  explicit InteractiveUITestLauncherDelegate(ChromeTestSuiteRunner* runner)
      : ChromeTestLauncherDelegate(runner) {}

 protected:
  // content::TestLauncherDelegate:
  void PreSharding() override {
    ChromeTestLauncherDelegate::PreSharding();
#if defined(OS_WIN)
    // Check for any always-on-top windows present before any tests are run.
    // Take a snapshot if any are found and attempt to close any that are system
    // dialogs.
    KillAlwaysOnTopWindows(RunType::BEFORE_SHARD);
#endif
  }

  void OnTestTimedOut(const base::CommandLine& command_line) override {
#if defined(OS_WIN)
    // Take a snapshot of the screen and check for any always-on-top windows
    // present before terminating the test. Attempt to close any that are system
    // dialogs.
    KillAlwaysOnTopWindows(RunType::AFTER_TEST_TIMEOUT, &command_line);
#endif
    ChromeTestLauncherDelegate::OnTestTimedOut(command_line);
  }

#if defined(OS_MACOSX)
  std::unique_ptr<content::TestState> PreRunTest(
      base::CommandLine* command_line,
      base::TestLauncher::LaunchOptions* test_launch_options) override {
    auto test_state = ChromeTestLauncherDelegate::PreRunTest(
        command_line, test_launch_options);
    // Clear currently pressed modifier keys (if any) before the test starts.
    ui_test_utils::ClearKeyEventModifiers();
    return test_state;
  }

  void PostRunTest(base::TestResult* test_result) override {
    // Clear currently pressed modifier keys (if any) after the test finishes
    // and report an error if there were some.
    bool had_hanging_modifiers = ui_test_utils::ClearKeyEventModifiers();
    if (had_hanging_modifiers &&
        test_result->status == base::TestResult::TEST_SUCCESS) {
      test_result->status = base::TestResult::TEST_FAILURE_ON_EXIT;
    }
    ChromeTestLauncherDelegate::PostRunTest(test_result);
  }
#endif  // defined(OS_MACOSX)

 private:
  DISALLOW_COPY_AND_ASSIGN(InteractiveUITestLauncherDelegate);
};

class InteractiveUITestSuiteRunner : public ChromeTestSuiteRunner {
 public:
  int RunTestSuite(int argc, char** argv) override {
    return InteractiveUITestSuite(argc, argv).Run();
  }
};

int main(int argc, char** argv) {
  base::CommandLine::Init(argc, argv);
  // TODO(sky): this causes a crash in an autofill test on macosx, figure out
  // why: http://crbug.com/641969.
#if !defined(OS_MACOSX)
  // Without this it's possible for the first browser to start up in the
  // background, generally because the last test did something that causes the
  // test to run in the background. Most interactive ui tests assume they are in
  // the foreground and fail in weird ways if they aren't (for example, clicks
  // go to the wrong place). This ensures the first browser is always in the
  // foreground.
  InProcessBrowserTest::set_global_browser_set_up_function(
      &ui_test_utils::BringBrowserWindowToFront);
#endif

  // Run interactive_ui_tests serially, they do not support running in parallel.
  size_t parallel_jobs = 1U;

  InteractiveUITestSuiteRunner runner;
  InteractiveUITestLauncherDelegate delegate(&runner);
  return LaunchChromeTests(parallel_jobs, &delegate, argc, argv);
}
