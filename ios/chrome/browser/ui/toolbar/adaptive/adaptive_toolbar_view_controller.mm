// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/toolbar/adaptive/adaptive_toolbar_view_controller.h"

#import "base/logging.h"
#include "base/metrics/user_metrics.h"
#import "ios/chrome/browser/ui/commands/browser_commands.h"
#import "ios/chrome/browser/ui/toolbar/adaptive/adaptive_toolbar_view.h"
#import "ios/chrome/browser/ui/toolbar/buttons/toolbar_button.h"
#import "ios/chrome/browser/ui/toolbar/buttons/toolbar_button_factory.h"
#import "ios/chrome/browser/ui/toolbar/buttons/toolbar_configuration.h"
#import "ios/chrome/browser/ui/toolbar/buttons/toolbar_constants.h"
#import "ios/chrome/browser/ui/toolbar/buttons/toolbar_tab_grid_button.h"
#import "ios/chrome/browser/ui/toolbar/buttons/toolbar_tools_menu_button.h"
#import "ios/chrome/browser/ui/toolbar/public/omnibox_focuser.h"
#import "ios/third_party/material_components_ios/src/components/ProgressView/src/MaterialProgressView.h"
#import "ios/third_party/material_components_ios/src/components/Typography/src/MaterialTypography.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

@interface AdaptiveToolbarViewController ()

// Redefined to be an AdaptiveToolbarView.
@property(nonatomic, strong) UIView<AdaptiveToolbarView>* view;
// Whether a page is loading.
@property(nonatomic, assign, getter=isLoading) BOOL loading;

@end

@implementation AdaptiveToolbarViewController

@dynamic view;
@synthesize buttonFactory = _buttonFactory;
@synthesize dispatcher = _dispatcher;
@synthesize loading = _loading;

#pragma mark - Public

- (void)updateForSideSwipeSnapshotOnNTP:(BOOL)onNTP {
  self.view.progressBar.hidden = YES;
  self.view.blur.hidden = YES;
  self.view.backgroundColor =
      self.buttonFactory.toolbarConfiguration.backgroundColor;
  // TODO(crbug.com/804850): Have the correct background color for incognito
  // NTP.
}

- (void)resetAfterSideSwipeSnapshot {
  self.view.blur.hidden = NO;
  self.view.backgroundColor = [UIColor clearColor];
}

#pragma mark - UIViewController

- (void)viewDidAppear:(BOOL)animated {
  [super viewDidAppear:animated];
  [self updateAllButtonsVisibility];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  [self addStandardActionsForAllButtons];

  // Adds the layout guide to the buttons.
  self.view.toolsMenuButton.guideName = kTabSwitcherGuide;
  self.view.forwardButton.guideName = kForwardButtonGuide;
  self.view.backButton.guideName = kBackButtonGuide;

  // Add navigation popup menu triggers.
  [self addLongPressGestureToView:self.view.backButton];
  [self addLongPressGestureToView:self.view.forwardButton];
}

- (void)traitCollectionDidChange:(UITraitCollection*)previousTraitCollection {
  [super traitCollectionDidChange:previousTraitCollection];
  [self updateAllButtonsVisibility];
}

#pragma mark - Public

- (ToolbarToolsMenuButton*)toolsMenuButton {
  return self.view.toolsMenuButton;
}

#pragma mark - ToolbarConsumer

- (void)setCanGoForward:(BOOL)canGoForward {
  self.view.forwardButton.enabled = canGoForward;
}

- (void)setCanGoBack:(BOOL)canGoBack {
  self.view.backButton.enabled = canGoBack;
}

- (void)setLoadingState:(BOOL)loading {
  if (self.loading == loading)
    return;

  self.loading = loading;
  self.view.reloadButton.hiddenInCurrentState = loading;
  self.view.stopButton.hiddenInCurrentState = !loading;
  if (!loading) {
    [self stopProgressBar];
  } else if (self.view.progressBar.hidden) {
    [self.view.progressBar setProgress:0];
    [self.view.progressBar setHidden:NO animated:YES completion:nil];
    // Layout if needed the progress bar to avoid having the progress bar going
    // backward when opening a page from the NTP.
    [self.view.progressBar layoutIfNeeded];
  }
}

- (void)setLoadingProgressFraction:(double)progress {
  [self.view.progressBar setProgress:progress animated:YES completion:nil];
}

- (void)setTabCount:(int)tabCount {
  [self.view.tabGridButton setTabCount:tabCount];
}

- (void)setPageBookmarked:(BOOL)bookmarked {
  self.view.bookmarkButton.selected = bookmarked;
}

- (void)setVoiceSearchEnabled:(BOOL)enabled {
  // No-op, should be handled by the location bar.
}

- (void)setShareMenuEnabled:(BOOL)enabled {
  self.view.shareButton.enabled = enabled;
}

- (void)setIsNTP:(BOOL)isNTP {
  // No-op, should be handled by the primary toolbar.
}

- (void)setSearchIcon:(UIImage*)searchIcon {
  [self.view.omniboxButton setImage:searchIcon forState:UIControlStateNormal];
}

#pragma mark - NewTabPageControllerDelegate

- (void)setToolbarBackgroundToIncognitoNTPColorWithAlpha:(CGFloat)alpha {
  // TODO(crbug.com/803379): Implement that.
}

- (void)setScrollProgressForTabletOmnibox:(CGFloat)progress {
  // No-op, should be handled by the primary toolbar.
}

#pragma mark - Protected

- (void)stopProgressBar {
  __weak MDCProgressView* weakProgressBar = self.view.progressBar;
  __weak AdaptiveToolbarViewController* weakSelf = self;
  [self.view.progressBar
      setProgress:1
         animated:YES
       completion:^(BOOL finished) {
         if (!weakSelf.loading) {
           [weakProgressBar setHidden:YES animated:YES completion:nil];
         }
       }];
}

#pragma mark - TabHistoryUIUpdater

- (void)updateUIForTabHistoryPresentationFrom:(ToolbarButtonType)buttonType {
  if (buttonType == ToolbarButtonTypeBack) {
    self.view.backButton.selected = YES;
  } else {
    self.view.forwardButton.selected = YES;
  }
}

- (void)updateUIForTabHistoryWasDismissed {
  self.view.backButton.selected = NO;
  self.view.forwardButton.selected = NO;
}

#pragma mark - Private

// Updates all buttons visibility to match any recent WebState or SizeClass
// change.
- (void)updateAllButtonsVisibility {
  for (ToolbarButton* button in self.view.allButtons) {
    [button updateHiddenInCurrentSizeClass];
  }
}

// Registers the actions which will be triggered when tapping a button.
- (void)addStandardActionsForAllButtons {
  for (ToolbarButton* button in self.view.allButtons) {
    if (button != self.view.toolsMenuButton &&
        button != self.view.omniboxButton) {
      [button addTarget:self.dispatcher
                    action:@selector(cancelOmniboxEdit)
          forControlEvents:UIControlEventTouchUpInside];
    }
    [button addTarget:self
                  action:@selector(recordUserMetrics:)
        forControlEvents:UIControlEventTouchUpInside];
  }
}

// Records the use of a button.
- (IBAction)recordUserMetrics:(id)sender {
  if (!sender)
    return;

  if (sender == self.view.backButton) {
    base::RecordAction(base::UserMetricsAction("MobileToolbarBack"));
  } else if (sender == self.view.forwardButton) {
    base::RecordAction(base::UserMetricsAction("MobileToolbarForward"));
  } else if (sender == self.view.reloadButton) {
    base::RecordAction(base::UserMetricsAction("MobileToolbarReload"));
  } else if (sender == self.view.stopButton) {
    base::RecordAction(base::UserMetricsAction("MobileToolbarStop"));
  } else if (sender == self.view.bookmarkButton) {
    base::RecordAction(base::UserMetricsAction("MobileToolbarToggleBookmark"));
  } else if (sender == self.view.toolsMenuButton) {
    base::RecordAction(base::UserMetricsAction("MobileToolbarShowMenu"));
  } else if (sender == self.view.tabGridButton) {
    base::RecordAction(base::UserMetricsAction("MobileToolbarShowStackView"));
  } else if (sender == self.view.shareButton) {
    base::RecordAction(base::UserMetricsAction("MobileToolbarShareMenu"));
  } else if (sender == self.view.omniboxButton) {
    base::RecordAction(base::UserMetricsAction("MobileToolbarOmniboxShortcut"));
  } else {
    NOTREACHED();
  }
}

// Adds a LongPressGesture to the |view|, with target on -|handleLongPress:|.
- (void)addLongPressGestureToView:(UIView*)view {
  UILongPressGestureRecognizer* navigationHistoryLongPress =
      [[UILongPressGestureRecognizer alloc]
          initWithTarget:self
                  action:@selector(handleLongPress:)];
  [view addGestureRecognizer:navigationHistoryLongPress];
}

// Handles the long press on the views.
- (void)handleLongPress:(UILongPressGestureRecognizer*)gesture {
  if (gesture.state != UIGestureRecognizerStateBegan)
    return;

  if (gesture.view == self.view.backButton) {
    [self.dispatcher showTabHistoryPopupForBackwardHistory];
  } else if (gesture.view == self.view.forwardButton) {
    [self.dispatcher showTabHistoryPopupForForwardHistory];
  }
}

@end
