// Copyright (c) 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ui/app_list/views/assistant_container_view.h"

#include "ui/app_list/app_list_constants.h"
#include "ui/app_list/views/contents_view.h"

namespace app_list {

AssistantContainerView::AssistantContainerView(ContentsView* contents_view)
    : contents_view_(contents_view) {}

gfx::Size AssistantContainerView::CalculatePreferredSize() const {
  if (!GetWidget()) {
    return gfx::Size();
  }
  return gfx::Size(contents_view_->GetDisplayWidth(),
                   kHorizontalPagePreferredHeight);
}

}  // namespace app_list
