// Copyright 2016 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/arc/arc_session_manager.h"

#include <utility>

#include "base/bind.h"
#include "base/callback_helpers.h"
#include "base/command_line.h"
#include "base/logging.h"
#include "base/strings/string16.h"
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/time.h"
#include "chrome/browser/chromeos/arc/arc_migration_guide_notification.h"
#include "chrome/browser/chromeos/arc/arc_optin_uma.h"
#include "chrome/browser/chromeos/arc/arc_support_host.h"
#include "chrome/browser/chromeos/arc/arc_util.h"
#include "chrome/browser/chromeos/arc/auth/arc_auth_context.h"
#include "chrome/browser/chromeos/arc/auth/arc_auth_service.h"
#include "chrome/browser/chromeos/arc/optin/arc_terms_of_service_default_negotiator.h"
#include "chrome/browser/chromeos/arc/optin/arc_terms_of_service_oobe_negotiator.h"
#include "chrome/browser/chromeos/arc/policy/arc_android_management_checker.h"
#include "chrome/browser/lifetime/application_lifetime.h"
#include "chrome/browser/policy/profile_policy_connector.h"
#include "chrome/browser/policy/profile_policy_connector_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/app_list/arc/arc_app_launcher.h"
#include "chrome/browser/ui/app_list/arc/arc_app_utils.h"
#include "chrome/browser/ui/app_list/arc/arc_pai_starter.h"
#include "chrome/browser/ui/ash/multi_user/multi_user_util.h"
#include "chrome/browser/ui/browser_commands.h"
#include "chromeos/chromeos_switches.h"
#include "chromeos/cryptohome/cryptohome_parameters.h"
#include "chromeos/dbus/dbus_thread_manager.h"
#include "chromeos/dbus/session_manager_client.h"
#include "components/arc/arc_data_remover.h"
#include "components/arc/arc_instance_mode.h"
#include "components/arc/arc_prefs.h"
#include "components/arc/arc_session_runner.h"
#include "components/arc/arc_util.h"
#include "components/arc/metrics/arc_metrics_service.h"
#include "components/prefs/pref_service.h"
#include "components/signin/core/account_id/account_id.h"
#include "content/public/browser/browser_thread.h"
#include "ui/display/types/display_constants.h"

namespace arc {

namespace {

// Weak pointer.  This class is owned by ArcServiceManager.
ArcSessionManager* g_arc_session_manager = nullptr;

// Skip creating UI in unit tests
bool g_disable_ui_for_testing = false;

// The Android management check is disabled by default, it's used only for
// testing.
bool g_enable_check_android_management_for_testing = false;

// Maximum amount of time we'll wait for ARC to finish booting up. Once this
// timeout expires, keep ARC running in case the user wants to file feedback,
// but present the UI to try again.
constexpr base::TimeDelta kArcSignInTimeout = base::TimeDelta::FromMinutes(5);

// Updates UMA with user cancel only if error is not currently shown.
void MaybeUpdateOptInCancelUMA(const ArcSupportHost* support_host) {
  if (!support_host ||
      support_host->ui_page() == ArcSupportHost::UIPage::NO_PAGE ||
      support_host->ui_page() == ArcSupportHost::UIPage::ERROR) {
    return;
  }

  UpdateOptInCancelUMA(OptInCancelReason::USER_CANCEL);
}

chromeos::SessionManagerClient* GetSessionManagerClient() {
  // If the DBusThreadManager or the SessionManagerClient aren't available,
  // there isn't much we can do. This should only happen when running tests.
  if (!chromeos::DBusThreadManager::IsInitialized() ||
      !chromeos::DBusThreadManager::Get() ||
      !chromeos::DBusThreadManager::Get()->GetSessionManagerClient())
    return nullptr;
  return chromeos::DBusThreadManager::Get()->GetSessionManagerClient();
}

}  // namespace

// This class is used to track statuses on OptIn flow. It is created in case ARC
// is activated, and it needs to OptIn. Once started OptInFlowResult::STARTED is
// recorded via UMA. If it finishes successfully OptInFlowResult::SUCCEEDED is
// recorded. Optional OptInFlowResult::SUCCEEDED_AFTER_RETRY is recorded in this
// case if an error occurred during OptIn flow, and user pressed Retry. In case
// the user cancels OptIn flow before it was completed then
// OptInFlowResult::CANCELED is recorded and if an error occurred optional
// OptInFlowResult::CANCELED_AFTER_ERROR. If a shutdown happens during the OptIn
// nothing is recorded, except initial OptInFlowResult::STARTED.
// OptInFlowResult::STARTED = OptInFlowResult::SUCCEEDED +
// OptInFlowResult::CANCELED + cases happened during the shutdown.
class ArcSessionManager::ScopedOptInFlowTracker {
 public:
  ScopedOptInFlowTracker() {
    UpdateOptInFlowResultUMA(OptInFlowResult::STARTED);
  }

  ~ScopedOptInFlowTracker() {
    if (shutdown_)
      return;

    UpdateOptInFlowResultUMA(success_ ? OptInFlowResult::SUCCEEDED
                                      : OptInFlowResult::CANCELED);
    if (error_) {
      UpdateOptInFlowResultUMA(success_
                                   ? OptInFlowResult::SUCCEEDED_AFTER_RETRY
                                   : OptInFlowResult::CANCELED_AFTER_ERROR);
    }
  }

  // Tracks error occurred during the OptIn flow.
  void TrackError() {
    DCHECK(!success_ && !shutdown_);
    error_ = true;
  }

  // Tracks that OptIn finished successfully.
  void TrackSuccess() {
    DCHECK(!success_ && !shutdown_);
    success_ = true;
  }

  // Tracks that OptIn was not completed before shutdown.
  void TrackShutdown() {
    DCHECK(!success_ && !shutdown_);
    shutdown_ = true;
  }

 private:
  bool error_ = false;
  bool success_ = false;
  bool shutdown_ = false;

  DISALLOW_COPY_AND_ASSIGN(ScopedOptInFlowTracker);
};

ArcSessionManager::ArcSessionManager(
    std::unique_ptr<ArcSessionRunner> arc_session_runner)
    : arc_session_runner_(std::move(arc_session_runner)),
      attempt_user_exit_callback_(base::Bind(chrome::AttemptUserExit)),
      weak_ptr_factory_(this) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(!g_arc_session_manager);
  g_arc_session_manager = this;
  arc_session_runner_->AddObserver(this);
  chromeos::SessionManagerClient* client = GetSessionManagerClient();
  if (client)
    client->AddObserver(this);
}

ArcSessionManager::~ArcSessionManager() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  chromeos::SessionManagerClient* client = GetSessionManagerClient();
  if (client)
    client->RemoveObserver(this);

  Shutdown();
  arc_session_runner_->RemoveObserver(this);

  DCHECK_EQ(this, g_arc_session_manager);
  g_arc_session_manager = nullptr;
}

// static
ArcSessionManager* ArcSessionManager::Get() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return g_arc_session_manager;
}

// static
void ArcSessionManager::DisableUIForTesting() {
  g_disable_ui_for_testing = true;
}

// static
void ArcSessionManager::EnableCheckAndroidManagementForTesting() {
  g_enable_check_android_management_for_testing = true;
}

void ArcSessionManager::OnSessionStopped(ArcStopReason reason,
                                         bool restarting) {
  if (restarting) {
    DCHECK_EQ(state_, State::ACTIVE);
    // If ARC is being restarted, here do nothing, and just wait for its
    // next run.
    VLOG(1) << "ARC session is stopped, but being restarted: " << reason;
    return;
  }

  DCHECK(state_ == State::ACTIVE || state_ == State::STOPPING) << state_;
  state_ = State::STOPPED;

  // TODO(crbug.com/625923): Use |reason| to report more detailed errors.
  if (arc_sign_in_timer_.IsRunning())
    OnProvisioningFinished(ProvisioningResult::ARC_STOPPED);

  for (auto& observer : observer_list_)
    observer.OnArcSessionStopped(reason);

  MaybeStartArcDataRemoval();
}

void ArcSessionManager::OnSessionRestarting() {
  for (auto& observer : observer_list_)
    observer.OnArcSessionRestarting();
}

void ArcSessionManager::OnProvisioningFinished(ProvisioningResult result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  // If the Mojo message to notify finishing the provisioning is already sent
  // from the container, it will be processed even after requesting to stop the
  // container. Ignore all |result|s arriving while ARC is disabled, in order to
  // avoid popping up an error message triggered below. This code intentionally
  // does not support the case of reenabling.
  if (!enable_requested_) {
    LOG(WARNING) << "Provisioning result received after ARC was disabled. "
                 << "Ignoring result " << static_cast<int>(result) << ".";
    return;
  }

  // Due asynchronous nature of stopping the ARC instance,
  // OnProvisioningFinished may arrive after setting the |State::STOPPED| state
  // and |State::Active| is not guaranteed to be set here.
  // prefs::kArcDataRemoveRequested also can be active for now.

  const bool provisioning_successful =
      result == ProvisioningResult::SUCCESS ||
      result == ProvisioningResult::SUCCESS_ALREADY_PROVISIONED;
  if (provisioning_reported_) {
    // We don't expect ProvisioningResult::SUCCESS or
    // ProvisioningResult::SUCCESS_ALREADY_PROVISIONED to be reported twice or
    // reported after an error.
    DCHECK(!provisioning_successful);
    // TODO(khmel): Consider changing LOG to NOTREACHED once we guaranty that
    // no double message can happen in production.
    LOG(WARNING) << "Provisioning result was already reported. Ignoring "
                 << "additional result " << result << ".";
    return;
  }
  provisioning_reported_ = true;
  if (scoped_opt_in_tracker_ && !provisioning_successful)
    scoped_opt_in_tracker_->TrackError();

  if (result == ProvisioningResult::CHROME_SERVER_COMMUNICATION_ERROR) {
    // TODO(poromov): Consider PublicSession offline mode.
    if (IsRobotAccountMode()) {
      VLOG(1) << "Robot account auth code fetching error";
      // Log out the user. All the cleanup will be done in Shutdown() method.
      // The callback is not called because auth code is empty.
      attempt_user_exit_callback_.Run();
      return;
    }

    // For backwards compatibility, use NETWORK_ERROR for
    // CHROME_SERVER_COMMUNICATION_ERROR case.
    UpdateOptInCancelUMA(OptInCancelReason::NETWORK_ERROR);
  } else if (!sign_in_start_time_.is_null()) {
    arc_sign_in_timer_.Stop();

    UpdateProvisioningTiming(base::Time::Now() - sign_in_start_time_,
                             provisioning_successful, profile_);
    UpdateProvisioningResultUMA(result, profile_);
    if (!provisioning_successful)
      UpdateOptInCancelUMA(OptInCancelReason::CLOUD_PROVISION_FLOW_FAIL);
  }

  if (provisioning_successful) {
    if (support_host_)
      support_host_->Close();

    if (scoped_opt_in_tracker_) {
      scoped_opt_in_tracker_->TrackSuccess();
      scoped_opt_in_tracker_.reset();
    }

    if (profile_->GetPrefs()->GetBoolean(prefs::kArcSignedIn))
      return;

    profile_->GetPrefs()->SetBoolean(prefs::kArcSignedIn, true);

    // Launch Play Store app, except for the following cases:
    // * When Opt-in verification is disabled (for tests);
    // * In case ARC is enabled from OOBE.
    // * In ARC Kiosk mode, because the only one UI in kiosk mode must be the
    //   kiosk app and device is not needed for opt-in;
    // * In Public Session mode, because Play Store will be hidden from users
    //   and only apps configured by policy should be installed.
    // * When ARC is managed and all OptIn preferences are managed/unused, too,
    //   because the whole OptIn flow should happen as seamless as possible for
    //   the user.
    // For Active Directory users we always show a page notifying them that they
    // have to authenticate with their identity provider (through SAML) to make
    // it less weird that a browser window pops up.
    const bool suppress_play_store_app =
        !IsPlayStoreAvailable() || IsArcOptInVerificationDisabled() ||
        IsRobotAccountMode() || oobe_start_ ||
        (IsArcPlayStoreEnabledPreferenceManagedForProfile(profile_) &&
         AreArcAllOptInPreferencesIgnorableForProfile(profile_));
    if (!suppress_play_store_app) {
      playstore_launcher_ = std::make_unique<ArcAppLauncher>(
          profile_, kPlayStoreAppId,
          GetLaunchIntent(kPlayStorePackage, kPlayStoreActivity,
                          {kInitialStartParam}),
          false /* deferred_launch_allowed */, display::kInvalidDisplayId);
    }

    for (auto& observer : observer_list_)
      observer.OnArcInitialStart();
    return;
  }

  ArcSupportHost::Error error;
  VLOG(1) << "ARC provisioning failed: " << result << ".";
  switch (result) {
    case ProvisioningResult::GMS_NETWORK_ERROR:
      error = ArcSupportHost::Error::SIGN_IN_NETWORK_ERROR;
      break;
    case ProvisioningResult::GMS_SERVICE_UNAVAILABLE:
    case ProvisioningResult::GMS_SIGN_IN_FAILED:
    case ProvisioningResult::GMS_SIGN_IN_TIMEOUT:
    case ProvisioningResult::GMS_SIGN_IN_INTERNAL_ERROR:
      error = ArcSupportHost::Error::SIGN_IN_SERVICE_UNAVAILABLE_ERROR;
      break;
    case ProvisioningResult::GMS_BAD_AUTHENTICATION:
      error = ArcSupportHost::Error::SIGN_IN_BAD_AUTHENTICATION_ERROR;
      break;
    case ProvisioningResult::DEVICE_CHECK_IN_FAILED:
    case ProvisioningResult::DEVICE_CHECK_IN_TIMEOUT:
    case ProvisioningResult::DEVICE_CHECK_IN_INTERNAL_ERROR:
      error = ArcSupportHost::Error::SIGN_IN_GMS_NOT_AVAILABLE_ERROR;
      break;
    case ProvisioningResult::CLOUD_PROVISION_FLOW_FAILED:
    case ProvisioningResult::CLOUD_PROVISION_FLOW_TIMEOUT:
    case ProvisioningResult::CLOUD_PROVISION_FLOW_INTERNAL_ERROR:
      error = ArcSupportHost::Error::SIGN_IN_CLOUD_PROVISION_FLOW_FAIL_ERROR;
      break;
    case ProvisioningResult::CHROME_SERVER_COMMUNICATION_ERROR:
      error = ArcSupportHost::Error::SERVER_COMMUNICATION_ERROR;
      break;
    case ProvisioningResult::NO_NETWORK_CONNECTION:
      error = ArcSupportHost::Error::NETWORK_UNAVAILABLE_ERROR;
      break;
    case ProvisioningResult::ARC_DISABLED:
      error = ArcSupportHost::Error::ANDROID_MANAGEMENT_REQUIRED_ERROR;
      break;
    default:
      error = ArcSupportHost::Error::SIGN_IN_UNKNOWN_ERROR;
      break;
  }

  if (result == ProvisioningResult::ARC_STOPPED ||
      result == ProvisioningResult::CHROME_SERVER_COMMUNICATION_ERROR) {
    if (profile_->GetPrefs()->HasPrefPath(prefs::kArcSignedIn))
      profile_->GetPrefs()->SetBoolean(prefs::kArcSignedIn, false);
    ShutdownSession();
    ShowArcSupportHostError(error, true);
    return;
  }

  if (result == ProvisioningResult::CLOUD_PROVISION_FLOW_FAILED ||
      result == ProvisioningResult::CLOUD_PROVISION_FLOW_TIMEOUT ||
      result == ProvisioningResult::CLOUD_PROVISION_FLOW_INTERNAL_ERROR ||
      // OVERALL_SIGN_IN_TIMEOUT might be an indication that ARC believes it is
      // fully setup, but Chrome does not.
      result == ProvisioningResult::OVERALL_SIGN_IN_TIMEOUT ||
      // Just to be safe, remove data if we don't know the cause.
      result == ProvisioningResult::UNKNOWN_ERROR) {
    VLOG(1) << "ARC provisioning failed permanently. Removing user data";
    RequestArcDataRemoval();
  }

  // We'll delay shutting down the ARC instance in this case to allow people
  // to send feedback.
  ShowArcSupportHostError(error, true /* = show send feedback button */);
}

bool ArcSessionManager::IsAllowed() const {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  return profile_ != nullptr;
}

void ArcSessionManager::SetProfile(Profile* profile) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(!profile || !profile_);
  DCHECK(!profile || IsArcAllowedForProfile(profile));
  profile_ = profile;
}

void ArcSessionManager::Initialize() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(profile_);

  DCHECK_EQ(state_, State::NOT_INITIALIZED);
  state_ = State::STOPPED;

  // Create the support host at initialization. Note that, practically,
  // ARC support Chrome app is rarely used (only opt-in and re-auth flow).
  // So, it may be better to initialize it lazily.
  // TODO(hidehiko): Revisit to think about lazy initialization.
  //
  // Don't show UI for ARC Kiosk because the only one UI in kiosk mode must
  // be the kiosk app. In case of error the UI will be useless as well, because
  // in typical use case there will be no one nearby the kiosk device, who can
  // do some action to solve the problem be means of UI.
  if (!g_disable_ui_for_testing && !IsArcOptInVerificationDisabled() &&
      !IsRobotAccountMode()) {
    DCHECK(!support_host_);
    support_host_ = std::make_unique<ArcSupportHost>(profile_);
    support_host_->SetErrorDelegate(this);
  }
  data_remover_ = std::make_unique<ArcDataRemover>(
      profile_->GetPrefs(),
      cryptohome::Identification(
          multi_user_util::GetAccountIdFromProfile(profile_)));

  context_ = std::make_unique<ArcAuthContext>(profile_);

  if (!g_disable_ui_for_testing ||
      g_enable_check_android_management_for_testing) {
    ArcAndroidManagementChecker::StartClient();
  }

  // Chrome may be shut down before completing ARC data removal.
  // For such a case, start removing the data now, if necessary.
  MaybeStartArcDataRemoval();
}

void ArcSessionManager::Shutdown() {
  enable_requested_ = false;
  ResetArcState();
  arc_session_runner_->OnShutdown();
  data_remover_.reset();
  if (support_host_) {
    support_host_->SetErrorDelegate(nullptr);
    support_host_->Close();
    support_host_.reset();
  }
  context_.reset();
  pai_starter_.reset();
  profile_ = nullptr;
  state_ = State::NOT_INITIALIZED;
  if (scoped_opt_in_tracker_) {
    scoped_opt_in_tracker_->TrackShutdown();
    scoped_opt_in_tracker_.reset();
  }
}

void ArcSessionManager::ShutdownSession() {
  ResetArcState();
  switch (state_) {
    case State::NOT_INITIALIZED:
      // Ignore in NOT_INITIALIZED case. This is called in initial SetProfile
      // invocation.
      // TODO(hidehiko): Remove this along with the clean up.
      break;
    case State::STOPPED:
      // Currently, ARC is stopped. Do nothing.
      break;
    case State::NEGOTIATING_TERMS_OF_SERVICE:
    case State::CHECKING_ANDROID_MANAGEMENT:
      // We need to kill the mini-container that might be running here.
      arc_session_runner_->RequestStop();
      // While RequestStop is asynchronous, ArcSessionManager is agnostic to the
      // state of the mini-container, so we can set it's state_ to STOPPED
      // immediately.
      state_ = State::STOPPED;
      break;
    case State::REMOVING_DATA_DIR:
      // When data removing is done, |state_| will be set to STOPPED.
      // Do nothing here.
      break;
    case State::ACTIVE:
      // Request to stop the ARC. |state_| will be set to STOPPED eventually.
      // Set state before requesting the runner to stop in order to prevent the
      // case when |OnSessionStopped| can be called inline and as result
      // |state_| might be changed.
      state_ = State::STOPPING;
      arc_session_runner_->RequestStop();
      break;
    case State::STOPPING:
      // Now ARC is stopping. Do nothing here.
      break;
  }
}

void ArcSessionManager::ResetArcState() {
  arc_sign_in_timer_.Stop();
  playstore_launcher_.reset();
  terms_of_service_negotiator_.reset();
  android_management_checker_.reset();
}

void ArcSessionManager::AddObserver(Observer* observer) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  observer_list_.AddObserver(observer);
}

void ArcSessionManager::RemoveObserver(Observer* observer) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  observer_list_.RemoveObserver(observer);
}

void ArcSessionManager::NotifyArcPlayStoreEnabledChanged(bool enabled) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  for (auto& observer : observer_list_)
    observer.OnArcPlayStoreEnabledChanged(enabled);
}

// This is the special method to support enterprise mojo API.
// TODO(hidehiko): Remove this.
void ArcSessionManager::StopAndEnableArc() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  reenable_arc_ = true;
  StopArc();
}

void ArcSessionManager::OnArcSignInTimeout() {
  LOG(ERROR) << "Timed out waiting for first sign in.";
  OnProvisioningFinished(ProvisioningResult::OVERALL_SIGN_IN_TIMEOUT);
}

void ArcSessionManager::CancelAuthCode() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);

  if (state_ == State::NOT_INITIALIZED) {
    NOTREACHED();
    return;
  }

  // If ARC failed to boot normally, stop ARC. Similarly, if the current page is
  // ACTIVE_DIRECTORY_AUTH, closing the window should stop ARC since the user
  // chooses to not sign in. In any other case, ARC is booting normally and
  // the instance should not be stopped.
  if ((state_ != State::NEGOTIATING_TERMS_OF_SERVICE &&
       state_ != State::CHECKING_ANDROID_MANAGEMENT) &&
      (!support_host_ ||
       (support_host_->ui_page() != ArcSupportHost::UIPage::ERROR &&
        support_host_->ui_page() !=
            ArcSupportHost::UIPage::ACTIVE_DIRECTORY_AUTH))) {
    return;
  }

  MaybeUpdateOptInCancelUMA(support_host_.get());
  StopArc();
  SetArcPlayStoreEnabledForProfile(profile_, false);
}

void ArcSessionManager::RecordArcState() {
  // Only record Enabled state if ARC is allowed in the first place, so we do
  // not split the ARC population by devices that cannot run ARC.
  if (!IsAllowed())
    return;

  UpdateEnabledStateUMA(enable_requested_);

  ArcMetricsService* service =
      ArcMetricsService::GetForBrowserContext(profile_);
  service->RecordNativeBridgeUMA();
}

void ArcSessionManager::RequestEnable() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(profile_);

  if (enable_requested_) {
    VLOG(1) << "ARC is already enabled. Do nothing.";
    return;
  }
  enable_requested_ = true;

  VLOG(1) << "ARC opt-in. Starting ARC session.";

  // |directly_started_| flag must be preserved during the internal ARC restart.
  // So set it only when ARC is externally requested to start.
  directly_started_ = RequestEnableImpl();
}

bool ArcSessionManager::IsPlaystoreLaunchRequestedForTesting() const {
  return playstore_launcher_.get();
}

bool ArcSessionManager::RequestEnableImpl() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(profile_);
  DCHECK(enable_requested_);
  DCHECK(state_ == State::STOPPED || state_ == State::STOPPING ||
         state_ == State::REMOVING_DATA_DIR)
      << state_;

  if (state_ != State::STOPPED) {
    // If the previous invocation of ARC is still running (but currently being
    // stopped) or ARC data removal is in progress, postpone the enabling
    // procedure.
    reenable_arc_ = true;
    return false;
  }

  oobe_start_ = IsArcOobeOptInActive();

  PrefService* const prefs = profile_->GetPrefs();

  // If it is marked that sign in has been successfully done or if Play Store is
  // not available, then directly start ARC with skipping Play Store ToS.
  // For Kiosk mode, skip ToS because it is very likely that near the device
  // there will be no one who is eligible to accept them.
  // In Public Session mode ARC should be started silently without user
  // interaction. If opt-in verification is disabled, skip negotiation, too.
  // This is for testing purpose.
  const bool start_arc_directly =
      prefs->GetBoolean(prefs::kArcSignedIn) || ShouldArcAlwaysStart() ||
      IsRobotAccountMode() || IsArcOptInVerificationDisabled();

  // When ARC is blocked because of filesystem compatibility, do not proceed
  // to starting ARC nor follow further state transitions.
  if (IsArcBlockedDueToIncompatibleFileSystem(profile_)) {
    // If the next step was the ToS negotiation, show a notification instead.
    // Otherwise, be silent now. Users are notified when clicking ARC app icons.
    if (!start_arc_directly && !g_disable_ui_for_testing)
      arc::ShowArcMigrationGuideNotification(profile_);
    return false;
  }

  if (!pai_starter_ && IsPlayStoreAvailable()) {
    pai_starter_ =
        ArcPaiStarter::CreateIfNeeded(profile_, profile_->GetPrefs());
  }

  if (start_arc_directly) {
    StartArc();
    // When in ARC kiosk mode, there's no Chrome tabs to restore. Remove the
    // cgroups now.
    if (IsArcKioskMode())
      SetArcCpuRestriction(false /* do_restrict */);
    // Check Android management in parallel.
    // Note: StartBackgroundAndroidManagementCheck() may call
    // OnBackgroundAndroidManagementChecked() synchronously (or
    // asynchronously). In the callback, Google Play Store enabled preference
    // can be set to false if managed, and it triggers RequestDisable() via
    // ArcPlayStoreEnabledPreferenceHandler.
    // Thus, StartArc() should be called so that disabling should work even
    // if synchronous call case.
    StartBackgroundAndroidManagementCheck();
    return true;
  }

  MaybeStartTermsOfServiceNegotiation();
  return false;
}

void ArcSessionManager::RequestDisable() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(profile_);

  if (!enable_requested_) {
    VLOG(1) << "ARC is already disabled. "
            << "Killing an instance for login screen (if any).";
    arc_session_runner_->RequestStop();
    return;
  }

  oobe_start_ = false;
  directly_started_ = false;
  enable_requested_ = false;
  scoped_opt_in_tracker_.reset();
  pai_starter_.reset();

  // Reset any pending request to re-enable ARC.
  reenable_arc_ = false;
  StopArc();
  VLOG(1) << "ARC opt-out. Removing user data.";
  RequestArcDataRemoval();
}

void ArcSessionManager::RequestArcDataRemoval() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(profile_);
  DCHECK(data_remover_);

  // TODO(hidehiko): DCHECK the previous state. This is called for four cases;
  // 1) Supporting managed user initial disabled case (Please see also
  //    ArcPlayStoreEnabledPreferenceHandler::Start() for details).
  // 2) Supporting enterprise triggered data removal.
  // 3) One called in OnProvisioningFinished().
  // 4) On request disabling.
  // After the state machine is fixed, 2) should be replaced by
  // RequestDisable() immediately followed by RequestEnable().
  // 3) and 4) are internal state transition. So, as for public interface, 1)
  // should be the only use case, and the |state_| should be limited to
  // STOPPED, then.
  // TODO(hidehiko): Think a way to get rid of 1), too.

  data_remover_->Schedule();
  // To support 1) case above, maybe start data removal.
  if (state_ == State::STOPPED)
    MaybeStartArcDataRemoval();
}

void ArcSessionManager::MaybeStartTermsOfServiceNegotiation() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(profile_);
  DCHECK(!terms_of_service_negotiator_);
  // In Kiosk and Public Session mode, Terms of Service negotiation should be
  // skipped. See also RequestEnableImpl().
  DCHECK(!IsRobotAccountMode());
  // If opt-in verification is disabled, Terms of Service negotiation should
  // be skipped, too. See also RequestEnableImpl().
  DCHECK(!IsArcOptInVerificationDisabled());

  DCHECK_EQ(state_, State::STOPPED);
  state_ = State::NEGOTIATING_TERMS_OF_SERVICE;

  // TODO(hidehiko): In kArcSignedIn = true case, this method should never
  // be called. Remove the check.
  // Conceptually, this is starting ToS negotiation, rather than opt-in flow.
  // Move to RequestEnabledImpl.
  if (!scoped_opt_in_tracker_ &&
      !profile_->GetPrefs()->GetBoolean(prefs::kArcSignedIn)) {
    scoped_opt_in_tracker_ = std::make_unique<ScopedOptInFlowTracker>();
  }

  if (!IsArcTermsOfServiceNegotiationNeeded(profile_)) {
    // Moves to next state, Android management check, immediately, as if
    // Terms of Service negotiation is done successfully.
    StartAndroidManagementCheck();
    return;
  }

  if (IsArcOobeOptInActive()) {
    VLOG(1) << "Use OOBE negotiator.";
    terms_of_service_negotiator_ =
        std::make_unique<ArcTermsOfServiceOobeNegotiator>();
  } else if (support_host_) {
    VLOG(1) << "Use default negotiator.";
    terms_of_service_negotiator_ =
        std::make_unique<ArcTermsOfServiceDefaultNegotiator>(
            profile_->GetPrefs(), support_host_.get());
  }

  if (!terms_of_service_negotiator_) {
    // The only case reached here is when g_disable_ui_for_testing is set
    // so ARC support host is not created in SetProfile(), for testing purpose.
    DCHECK(g_disable_ui_for_testing)
        << "Negotiator is not created on production.";
    return;
  }

  // Start the mini-container here to save time starting the container if the
  // user decides to opt-in.
  arc_session_runner_->RequestStart(ArcInstanceMode::MINI_INSTANCE);

  terms_of_service_negotiator_->StartNegotiation(
      base::Bind(&ArcSessionManager::OnTermsOfServiceNegotiated,
                 weak_ptr_factory_.GetWeakPtr()));
}

void ArcSessionManager::OnTermsOfServiceNegotiated(bool accepted) {
  DCHECK_EQ(state_, State::NEGOTIATING_TERMS_OF_SERVICE);
  DCHECK(profile_);
  DCHECK(terms_of_service_negotiator_ || g_disable_ui_for_testing);
  terms_of_service_negotiator_.reset();

  if (!accepted) {
    // User does not accept the Terms of Service. Disable Google Play Store.
    MaybeUpdateOptInCancelUMA(support_host_.get());
    SetArcPlayStoreEnabledForProfile(profile_, false);
    return;
  }

  // Terms were accepted.
  profile_->GetPrefs()->SetBoolean(prefs::kArcTermsAccepted, true);
  StartAndroidManagementCheck();
}

void ArcSessionManager::StartAndroidManagementCheck() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(state_ == State::NEGOTIATING_TERMS_OF_SERVICE ||
         state_ == State::CHECKING_ANDROID_MANAGEMENT)
      << state_;
  state_ = State::CHECKING_ANDROID_MANAGEMENT;

  // Show loading UI only if ARC support app's window is already shown.
  // User may not see any ARC support UI if everything needed is done in
  // background. In such a case, showing loading UI here (then closed sometime
  // soon later) would look just noisy.
  if (support_host_ &&
      support_host_->ui_page() != ArcSupportHost::UIPage::NO_PAGE) {
    support_host_->ShowArcLoading();
  }

  for (auto& observer : observer_list_)
    observer.OnArcOptInManagementCheckStarted();

  if (g_disable_ui_for_testing)
    return;

  android_management_checker_ = std::make_unique<ArcAndroidManagementChecker>(
      profile_, context_->token_service(), context_->account_id(),
      false /* retry_on_error */);
  android_management_checker_->StartCheck(
      base::Bind(&ArcSessionManager::OnAndroidManagementChecked,
                 weak_ptr_factory_.GetWeakPtr()));
}

void ArcSessionManager::OnAndroidManagementChecked(
    policy::AndroidManagementClient::Result result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK_EQ(state_, State::CHECKING_ANDROID_MANAGEMENT);
  DCHECK(android_management_checker_);
  android_management_checker_.reset();

  switch (result) {
    case policy::AndroidManagementClient::Result::UNMANAGED:
      VLOG(1) << "Starting ARC for first sign in.";
      sign_in_start_time_ = base::Time::Now();
      arc_sign_in_timer_.Start(
          FROM_HERE, kArcSignInTimeout,
          base::Bind(&ArcSessionManager::OnArcSignInTimeout,
                     weak_ptr_factory_.GetWeakPtr()));
      StartArc();
      // Since opt-in is an explicit user (or admin) action, relax the
      // cgroups restriction now.
      SetArcCpuRestriction(false /* do_restrict */);
      break;
    case policy::AndroidManagementClient::Result::MANAGED:
      ShowArcSupportHostError(
          ArcSupportHost::Error::ANDROID_MANAGEMENT_REQUIRED_ERROR, false);
      UpdateOptInCancelUMA(OptInCancelReason::ANDROID_MANAGEMENT_REQUIRED);
      break;
    case policy::AndroidManagementClient::Result::ERROR:
      ShowArcSupportHostError(ArcSupportHost::Error::SERVER_COMMUNICATION_ERROR,
                              true);
      UpdateOptInCancelUMA(OptInCancelReason::NETWORK_ERROR);
      break;
  }
}

void ArcSessionManager::StartBackgroundAndroidManagementCheck() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK_EQ(state_, State::ACTIVE);
  DCHECK(!android_management_checker_);

  // Skip Android management check for testing.
  // We also skip if Android management check for Kiosk and Public Session mode,
  // because there are no managed human users for them exist.
  if (IsArcOptInVerificationDisabled() || IsRobotAccountMode() ||
      (g_disable_ui_for_testing &&
       !g_enable_check_android_management_for_testing)) {
    return;
  }

  android_management_checker_ = std::make_unique<ArcAndroidManagementChecker>(
      profile_, context_->token_service(), context_->account_id(),
      true /* retry_on_error */);
  android_management_checker_->StartCheck(
      base::Bind(&ArcSessionManager::OnBackgroundAndroidManagementChecked,
                 weak_ptr_factory_.GetWeakPtr()));
}

void ArcSessionManager::OnBackgroundAndroidManagementChecked(
    policy::AndroidManagementClient::Result result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(android_management_checker_);
  android_management_checker_.reset();

  switch (result) {
    case policy::AndroidManagementClient::Result::UNMANAGED:
      // Do nothing. ARC should be started already.
      break;
    case policy::AndroidManagementClient::Result::MANAGED:
      SetArcPlayStoreEnabledForProfile(profile_, false);
      break;
    case policy::AndroidManagementClient::Result::ERROR:
      // This code should not be reached. For background check,
      // retry_on_error should be set.
      NOTREACHED();
  }
}

void ArcSessionManager::StartArc() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(state_ == State::STOPPED ||
         state_ == State::CHECKING_ANDROID_MANAGEMENT)
      << state_;
  state_ = State::ACTIVE;

  // ARC must be started only if no pending data removal request exists.
  DCHECK(!profile_->GetPrefs()->GetBoolean(prefs::kArcDataRemoveRequested));

  for (auto& observer : observer_list_)
    observer.OnArcStarted();

  arc_start_time_ = base::Time::Now();
  provisioning_reported_ = false;
  arc_session_runner_->RequestStart(ArcInstanceMode::FULL_INSTANCE);
}

void ArcSessionManager::StopArc() {
  // TODO(hidehiko): This STOPPED guard should be unnecessary. Remove it later.
  // |reenable_arc_| may be set in |StopAndEnableArc| in case enterprise
  // management state is lost.
  if (!reenable_arc_ && state_ != State::STOPPED) {
    profile_->GetPrefs()->SetBoolean(prefs::kArcSignedIn, false);
    profile_->GetPrefs()->SetBoolean(prefs::kArcPaiStarted, false);
    profile_->GetPrefs()->SetBoolean(prefs::kArcTermsAccepted, false);
  }
  ShutdownSession();
  if (support_host_)
    support_host_->Close();
}

void ArcSessionManager::MaybeStartArcDataRemoval() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK(profile_);

  // Data removal cannot run in parallel with ARC session.
  // LoginScreen instance does not use data directory, so removing should work.
  DCHECK_EQ(state_, State::STOPPED);

  state_ = State::REMOVING_DATA_DIR;
  data_remover_->Run(base::BindOnce(&ArcSessionManager::OnArcDataRemoved,
                                    weak_ptr_factory_.GetWeakPtr()));
}

void ArcSessionManager::OnArcDataRemoved(base::Optional<bool> result) {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK_EQ(state_, State::REMOVING_DATA_DIR);
  DCHECK(profile_);
  state_ = State::STOPPED;

  if (result.has_value()) {
    // Remove Play user ID for Active Directory managed devices.
    profile_->GetPrefs()->SetString(prefs::kArcActiveDirectoryPlayUserId,
                                    std::string());

    // Regardless of whether it is successfully done or not, notify observers.
    for (auto& observer : observer_list_)
      observer.OnArcDataRemoved();

    // Note: Currently, we may re-enable ARC even if data removal fails.
    // We may have to avoid it.
  }

  MaybeReenableArc();
}

void ArcSessionManager::MaybeReenableArc() {
  DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
  DCHECK_EQ(state_, State::STOPPED);

  if (!reenable_arc_) {
    // Reenabling is not triggered. Do nothing.
    return;
  }
  DCHECK(enable_requested_);

  // Restart ARC anyway. Let the enterprise reporting instance decide whether
  // the ARC user data wipe is still required or not.
  reenable_arc_ = false;
  VLOG(1) << "Reenable ARC";
  RequestEnableImpl();
}

void ArcSessionManager::OnWindowClosed() {
  CancelAuthCode();
}

void ArcSessionManager::OnRetryClicked() {
  DCHECK(support_host_);
  DCHECK_EQ(support_host_->ui_page(), ArcSupportHost::UIPage::ERROR);
  DCHECK(!terms_of_service_negotiator_);
  DCHECK(!support_host_->HasAuthDelegate());

  UpdateOptInActionUMA(OptInActionType::RETRY);

  if (!profile_->GetPrefs()->GetBoolean(prefs::kArcTermsAccepted)) {
    // This can currently happen when an error page is shown when re-opt-in
    // right after opt-out (this is a bug as it should not show an error).  When
    // the user click the retry button on this error page, we may start terms of
    // service negotiation instead of recreating the instance.
    // TODO(hidehiko): consider removing this case after fixing the bug.
    MaybeStartTermsOfServiceNegotiation();
  } else if (state_ == State::ACTIVE) {
    // ERROR_WITH_FEEDBACK is set in OnSignInFailed(). In the case, stopping
    // ARC was postponed to contain its internal state into the report.
    // Here, on retry, stop it, then restart.
    support_host_->ShowArcLoading();
    ShutdownSession();
    reenable_arc_ = true;
  } else {
    // Otherwise, we restart ARC. Note: this is the first boot case.
    // For second or later boot, either ERROR_WITH_FEEDBACK case or ACTIVE
    // case must hit.
    StartAndroidManagementCheck();
  }
}

void ArcSessionManager::OnSendFeedbackClicked() {
  DCHECK(support_host_);
  chrome::OpenFeedbackDialog(nullptr, chrome::kFeedbackSourceArcApp);
}

void ArcSessionManager::SetArcSessionRunnerForTesting(
    std::unique_ptr<ArcSessionRunner> arc_session_runner) {
  DCHECK(arc_session_runner);
  DCHECK(arc_session_runner_);
  arc_session_runner_->RemoveObserver(this);
  arc_session_runner_ = std::move(arc_session_runner);
  arc_session_runner_->AddObserver(this);
}

ArcSessionRunner* ArcSessionManager::GetArcSessionRunnerForTesting() {
  return arc_session_runner_.get();
}

void ArcSessionManager::SetAttemptUserExitCallbackForTesting(
    const base::Closure& callback) {
  DCHECK(!callback.is_null());
  attempt_user_exit_callback_ = callback;
}

void ArcSessionManager::ShowArcSupportHostError(
    ArcSupportHost::Error error,
    bool should_show_send_feedback) {
  if (support_host_)
    support_host_->ShowError(error, should_show_send_feedback);
  for (auto& observer : observer_list_)
    observer.OnArcErrorShowRequested(error);
}

void ArcSessionManager::EmitLoginPromptVisibleCalled() {
  // Since 'login-prompt-visible' Upstart signal starts all Upstart jobs the
  // instance may depend on such as cras, EmitLoginPromptVisibleCalled() is the
  // safe place to start a mini instance.
  if (!IsArcAvailable())
    return;

  arc_session_runner_->RequestStart(ArcInstanceMode::MINI_INSTANCE);
}

std::ostream& operator<<(std::ostream& os,
                         const ArcSessionManager::State& state) {
#define MAP_STATE(name)                \
  case ArcSessionManager::State::name: \
    return os << #name

  switch (state) {
    MAP_STATE(NOT_INITIALIZED);
    MAP_STATE(STOPPED);
    MAP_STATE(NEGOTIATING_TERMS_OF_SERVICE);
    MAP_STATE(CHECKING_ANDROID_MANAGEMENT);
    MAP_STATE(REMOVING_DATA_DIR);
    MAP_STATE(ACTIVE);
    MAP_STATE(STOPPING);
  }

#undef MAP_STATE

  // Some compilers report an error even if all values of an enum-class are
  // covered exhaustively in a switch statement.
  NOTREACHED() << "Invalid value " << static_cast<int>(state);
  return os;
}

}  // namespace arc
