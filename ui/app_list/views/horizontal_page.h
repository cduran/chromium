// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UI_APP_LIST_VIEWS_HORIZONTAL_PAGE_H_
#define UI_APP_LIST_VIEWS_HORIZONTAL_PAGE_H_

#include "ash/app_list/model/app_list_model.h"
#include "base/macros.h"
#include "ui/app_list/app_list_export.h"
#include "ui/views/view.h"

namespace app_list {

// HorizontalPage is laid out horizontally in HorizontalPageContainer and its
// visibility is controlled by horizontal gesture scrolling.
class APP_LIST_EXPORT HorizontalPage : public views::View {
 public:
  // Triggered when the page is about to be hidden.
  virtual void OnWillBeHidden();

  // Gets the first and last focusable view in this page, this will be used in
  // focus traversal.
  virtual views::View* GetFirstFocusableView();
  virtual views::View* GetLastFocusableView();

 protected:
  HorizontalPage();
  ~HorizontalPage() override;

 private:
  DISALLOW_COPY_AND_ASSIGN(HorizontalPage);
};

}  // namespace app_list

#endif  // UI_APP_LIST_VIEWS_HORIZONTAL_PAGE_H_
