// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_CHROME_BROWSER_UI_TAB_GRID_TAB_GRID_PAGE_CONTROL_H_
#define IOS_CHROME_BROWSER_UI_TAB_GRID_TAB_GRID_PAGE_CONTROL_H_

#import <UIKit/UIKit.h>

#import "ios/chrome/browser/ui/tab_grid/tab_grid_paging.h"

// A three-sectioned control for selecting a page in the tab grid.
// A "slider" is positioned over the section for the selected page.
// This is a fixed-size control; it's an error to set  or change its size.
// The sections are arranged in leading-to-trailing order:
//   incognito tabs, regular tabs, remote tabs.
@interface TabGridPageControl : UIControl

// The currently selected page in the control. When this value is changed by
// a user interaction, the UIControlEventValueChanged actions are sent.
// Setting this property will update the position of the slider without
// animation. When an instance of this control is created, this value defaults
// to TabGridPageRegularTabs.
@property(nonatomic, assign) TabGridPage selectedPage;
// The position of the slider, from 0.0 to 1.0, where 0.0 is as far as possible
// to the leading side of the control, and 1.0 is as far as possible to the
// trailing side of the control. Setting this property will update the position
// of the slider without animation. Setting a value below 0.0 or above 1.0 will
// set 0.0 or 1.0 instead.
// Setting this property will *not* update the selected page.
@property(nonatomic, assign) CGFloat sliderPosition;

// The numbers that the control should display in the appropriate sections.
// Numbers less than 1 are not displayed.
// Numbers greated than 99 are displayed as ':-)'.
@property(nonatomic, assign) NSUInteger incognitoTabCount;
@property(nonatomic, assign) NSUInteger regularTabCount;

// Create and return a new instance of this control. This is the preferred way
// to create instances of this class.
+ (instancetype)pageControl;

// Designated initializer.
- (instancetype)init NS_DESIGNATED_INITIALIZER;

- (instancetype)initWithFrame:(CGRect)frame NS_UNAVAILABLE;
- (instancetype)initWithCoder:(NSCoder*)aDecoder NS_UNAVAILABLE;

// Set |selectedPage| as the selected page. If |animated| is YES, the
// position change of the slider will be animated.
- (void)setSelectedPage:(TabGridPage)selectedPage animated:(BOOL)animated;

@end

#endif  // IOS_CHROME_BROWSER_UI_TAB_GRID_TAB_GRID_PAGE_CONTROL_H_
