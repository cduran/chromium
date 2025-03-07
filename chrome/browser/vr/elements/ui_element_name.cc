// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/vr/elements/ui_element_name.h"

#include "base/logging.h"
#include "base/macros.h"

namespace vr {

namespace {

static const char* g_ui_element_name_strings[] = {
    "kNone",
    "kRoot",
    "k2dBrowsingRepositioner",
    "k2dBrowsingRoot",
    "k2dBrowsingBackground",
    "k2dBrowsingDefaultBackground",
    "k2dBrowsingTexturedBackground",
    "k2dBrowsingForeground",
    "k2dBrowsingContentGroup",
    "k2dBrowsingViewportAwareRoot",
    "kWebVrRoot",
    "kWebVrViewportAwareRoot",
    "kContentResizer",
    "kContentQuad",
    "kContentQuadShadow",
    "kContentQuadRepositionButton",
    "kControllerRoot",
    "kControllerGroup",
    "kLaser",
    "kController",
    "kRepositionCursor",
    "kReticle",
    "kReticleLaserGroup",
    "kKeyboardVisibilityControlForVoice",
    "kKeyboardDmmRoot",
    "kKeyboard",
    "kBackplane",
    "kCeiling",
    "kFloor",
    "kStars",
    "kUpdateKeyboardPrompt",
    "kUrlBarBackplane",
    "kUrlBarPositioner",
    "kUrlBarDmmRoot",
    "kUrlBar",
    "kUrlBarLayout",
    "kUrlBarBackButton",
    "kUrlBarBackButtonIcon",
    "kUrlBarSeparator",
    "kUrlBarOriginRegion",
    "kUrlBarOriginContent",
    "kUrlBarHintText",
    "kOmniboxVisibiltyControlForVoice",
    "kOmniboxVisibilityControlForAudioPermissionPrompt",
    "kOmniboxDmmRoot",
    "kOmniboxRoot",
    "kOmniboxContainer",
    "kOmniboxTextField",
    "kOmniboxTextFieldLayout",
    "kOmniboxVoiceSearchButton",
    "kOmniboxCloseButton",
    "kOmniboxSuggestions",
    "kOmniboxSuggestionsOuterLayout",
    "kOmniboxOuterLayout",
    "kOmniboxOuterLayoutSpacer",
    "kOmniboxShadow",
    "k2dBrowsingVisibiltyControlForVoice",
    "k2dBrowsingVisibilityControlForPrompt",
    "k2dBrowsingVisibiltyControlForSiteInfoPrompt",
    "k2dBrowsingOpacityControlForAudioPermissionPrompt",
    "k2dBrowsingOpacityControlForNativeDialogPrompt",
    "k2dBrowsingOpacityControlForUpdateKeyboardPrompt",
    "kIndicatorLayout",
    "kAudioCaptureIndicator",
    "kVideoCaptureIndicator",
    "kScreenCaptureIndicator",
    "kLocationAccessIndicator",
    "kBluetoothConnectedIndicator",
    "kLoadingIndicator",
    "kLoadingIndicatorForeground",
    "kCloseButton",
    "kScreenDimmer",
    "kExitWarningText",
    "kExitWarningBackground",
    "kExitPrompt",
    "kExitPromptBackplane",
    "kAudioPermissionPrompt",
    "kAudioPermissionPromptShadow",
    "kAudioPermissionPromptBackplane",
    "kPermissionDialogBackplane",
    "kHostedUi",
    "kHostedUiBackplane",
    "kWebVrUrlToastTransientParent",
    "kWebVrUrlToast",
    "kExclusiveScreenToastTransientParent",
    "kExclusiveScreenToast",
    "kExclusiveScreenToastViewportAwareTransientParent",
    "kExclusiveScreenToastViewportAware",
    "kSplashScreenRoot",
    "kSplashScreenTransientParent",
    "kSplashScreenViewportAwareRoot",
    "kSplashScreenText",
    "kBackgroundFront",
    "kBackgroundLeft",
    "kBackgroundBack",
    "kBackgroundRight",
    "kBackgroundTop",
    "kBackgroundBottom",
    "kWebVrTimeoutRoot",
    "kWebVrTimeoutSpinner",
    "kWebVrBackground",
    "kWebVrTimeoutMessage",
    "kWebVrTimeoutMessageLayout",
    "kWebVrTimeoutMessageIcon",
    "kWebVrTimeoutMessageText",
    "kWebVrTimeoutMessageButton",
    "kWebVrTimeoutMessageButtonText",
    "kSpeechRecognitionRoot",
    "kSpeechRecognitionCircle",
    "kSpeechRecognitionMicrophoneIcon",
    "kSpeechRecognitionResult",
    "kSpeechRecognitionResultText",
    "kSpeechRecognitionResultBackplane",
    "kSpeechRecognitionListening",
    "kSpeechRecognitionListeningGrowingCircle",
    "kSpeechRecognitionListeningCloseButton",
    "kDownloadedSnackbar",
    "kControllerTrackpadLabel",
    "kControllerTrackpadRepositionLabel",
    "kControllerExitButtonLabel",
    "kControllerBackButtonLabel",
    "kControllerTouchpadButton",
    "kControllerAppButton",
    "kControllerHomeButton",
    "kContentRepositionLabel",
    "kContentRepositionHitPlane",
    "kContentRepositionVisibilityToggle",
};

static_assert(
    kNumUiElementNames == arraysize(g_ui_element_name_strings),
    "Mismatch between the kUiElementName enum and the corresponding array "
    "of strings.");

}  // namespace

std::string UiElementNameToString(UiElementName name) {
  DCHECK_GT(kNumUiElementNames, name);
  return g_ui_element_name_strings[name];
}

}  // namespace vr
