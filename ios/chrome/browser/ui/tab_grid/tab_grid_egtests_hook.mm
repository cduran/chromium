// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ios/chrome/app/tests_hook.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

namespace tests_hook {

bool DisableContentSuggestions() {
  return true;
}

bool DisableContextualSearch() {
  return true;
}

bool DisableFirstRun() {
  return true;
}

bool DisableGeolocation() {
  return true;
}

bool DisableSigninRecallPromo() {
  return true;
}

bool DisableUpdateService() {
  return true;
}

bool ForceUIRefreshPhase1() {
  return true;
}

// TODO(crbug.com/818560) : Remove this hook.
bool ForceTabSwitcherTabGrid() {
  return true;
}

void SetUpTestsIfPresent() {
  // No-op for Earl Grey.
}

void RunTestsIfPresent() {
  // No-op for Earl Grey.
}

}  // namespace tests_hook
