// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef IOS_CHROME_BROWSER_UI_TAB_GRID_TAB_GRID_CONSTANTS_H_
#define IOS_CHROME_BROWSER_UI_TAB_GRID_TAB_GRID_CONSTANTS_H_

#import <UIKit/UIKit.h>

// All kxxxColor constants are RGB values stored in a Hex integer. These will be
// converted into UIColors using the UIColorFromRGB() function, from
// uikit_ui_util.h

// The color of the text buttons in the toolbars.
extern const int kTabGridToolbarTextButtonColor;

// Colors for the empty state.
extern const int kTabGridEmptyStateTitleTextColor;
extern const int kTabGridEmptyStateBodyTextColor;

// The distance the toolbar content is inset from either side.
extern const CGFloat kTabGridToolbarHorizontalInset;

// The distance between the title and body of the empty state view.
extern const CGFloat kTabGridEmptyStateVerticalMargin;

// Intrinsic heights of the tab grid toolbars.
extern const CGFloat kTabGridTopToolbarHeight;
extern const CGFloat kTabGridBottomToolbarHeight;

#endif  // IOS_CHROME_BROWSER_UI_TAB_GRID_TAB_GRID_CONSTANTS_H_
