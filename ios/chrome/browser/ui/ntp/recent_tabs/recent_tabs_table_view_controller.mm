// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#import "ios/chrome/browser/ui/ntp/recent_tabs/recent_tabs_table_view_controller.h"

#include "base/logging.h"
#include "base/metrics/user_metrics.h"
#include "base/metrics/user_metrics_action.h"
#include "base/strings/sys_string_conversions.h"
#include "components/browser_sync/profile_sync_service.h"
#include "components/sessions/core/tab_restore_service.h"
#include "components/strings/grit/components_strings.h"
#include "components/sync/driver/sync_service.h"
#include "components/sync_sessions/open_tabs_ui_delegate.h"
#include "ios/chrome/browser/browser_state/chrome_browser_state.h"
#import "ios/chrome/browser/metrics/new_tab_page_uma.h"
#include "ios/chrome/browser/sessions/tab_restore_service_delegate_impl_ios.h"
#include "ios/chrome/browser/sessions/tab_restore_service_delegate_impl_ios_factory.h"
#include "ios/chrome/browser/sync/ios_chrome_profile_sync_service_factory.h"
#import "ios/chrome/browser/ui/authentication/signin_promo_view.h"
#import "ios/chrome/browser/ui/authentication/signin_promo_view_configurator.h"
#import "ios/chrome/browser/ui/authentication/signin_promo_view_consumer.h"
#import "ios/chrome/browser/ui/authentication/signin_promo_view_mediator.h"
#include "ios/chrome/browser/ui/commands/application_commands.h"
#import "ios/chrome/browser/ui/commands/show_signin_command.h"
#import "ios/chrome/browser/ui/context_menu/context_menu_coordinator.h"
#import "ios/chrome/browser/ui/ntp/recent_tabs/legacy_recent_tabs_table_view_controller_delegate.h"
#import "ios/chrome/browser/ui/ntp/recent_tabs/recent_tabs_constants.h"
#import "ios/chrome/browser/ui/ntp/recent_tabs/recent_tabs_handset_view_controller.h"
#include "ios/chrome/browser/ui/ntp/recent_tabs/synced_sessions.h"
#import "ios/chrome/browser/ui/settings/sync_utils/sync_presenter.h"
#import "ios/chrome/browser/ui/signin_interaction/public/signin_presenter.h"
#import "ios/chrome/browser/ui/table_view/cells/table_view_text_header_footer_item.h"
#import "ios/chrome/browser/ui/table_view/cells/table_view_text_item.h"
#import "ios/chrome/browser/ui/table_view/cells/table_view_url_item.h"
#include "ios/chrome/browser/ui/ui_util.h"
#import "ios/chrome/browser/ui/url_loader.h"
#import "ios/chrome/browser/ui/util/top_view_controller.h"
#include "ios/chrome/grit/ios_chromium_strings.h"
#include "ios/chrome/grit/ios_strings.h"
#import "ios/web/public/web_state/context_menu_params.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/l10n/time_format.h"

#if !defined(__has_feature) || !__has_feature(objc_arc)
#error "This file requires ARC support."
#endif

namespace {

typedef NS_ENUM(NSInteger, SectionIdentifier) {
  SectionIdentifierRecentlyClosedTabs = kSectionIdentifierEnumZero,
  SectionIdentifierOtherDevices,
  // The first SessionsSectionIdentifier index.
  kFirstSessionSectionIdentifier,
};

typedef NS_ENUM(NSInteger, ItemType) {
  ItemTypeRecentlyClosedHeader = kItemTypeEnumZero,
  ItemTypeRecentlyClosed,
  ItemTypeOtherDevicesHeader,
  ItemTypeOtherDevicesSyncOff,
  ItemTypeOtherDevicesNoSessions,
  ItemTypeOtherDevicesSigninPromo,
  ItemTypeOtherDevicesSyncInProgress,
  ItemTypeSessionHeader,
  ItemTypeSessionTabData,
  ItemTypeShowFullHistory,
};

// Key for saving whether the Other Device section is collapsed.
NSString* const kOtherDeviceCollapsedKey = @"OtherDevicesCollapsed";
// Key for saving whether the Recently Closed section is collapsed.
NSString* const kRecentlyClosedCollapsedKey = @"RecentlyClosedCollapsed";
// There are 2 static sections before the first SessionSection.
int const kNumberOfSectionsBeforeSessions = 2;
// Estimated Table Row height.
const CGFloat kEstimatedRowHeight = 56;
// The UI displays relative time for up to this number of hours and then
// switches to absolute values.
const int kRelativeTimeMaxHours = 4;

}  // namespace

@interface RecentTabsTableViewController ()<SigninPromoViewConsumer,
                                            SigninPresenter,
                                            SyncPresenter,
                                            UIGestureRecognizerDelegate> {
  std::unique_ptr<synced_sessions::SyncedSessions> _syncedSessions;
}
// The service that manages the recently closed tabs
@property(nonatomic, assign) sessions::TabRestoreService* tabRestoreService;
// The sync state.
@property(nonatomic, assign) SessionsSyncUserState sessionState;
// Handles displaying the context menu for all form factors.
@property(nonatomic, strong) ContextMenuCoordinator* contextMenuCoordinator;
@property(nonatomic, strong) SigninPromoViewMediator* signinPromoViewMediator;
// The sectionIdentifier for the last tapped header, 0 if no header was tapped.
@property(nonatomic, assign) NSInteger lastTappedHeaderSectionIdentifier;
@end

@implementation RecentTabsTableViewController : ChromeTableViewController
@synthesize browserState = _browserState;
@synthesize contextMenuCoordinator = _contextMenuCoordinator;
@synthesize delegate = delegate_;
@synthesize dispatcher = _dispatcher;
@synthesize handsetCommandHandler = _handsetCommandHandler;
@synthesize lastTappedHeaderSectionIdentifier =
    _lastTappedHeaderSectionIdentifier;
@synthesize loader = _loader;
@synthesize sessionState = _sessionState;
@synthesize signinPromoViewMediator = _signinPromoViewMediator;
@synthesize tabRestoreService = _tabRestoreService;

#pragma mark - Public Interface

- (instancetype)init {
  self = [super initWithStyle:UITableViewStylePlain];
  if (self) {
    _sessionState = SessionsSyncUserState::USER_SIGNED_OUT;
    _syncedSessions.reset(new synced_sessions::SyncedSessions());
    _lastTappedHeaderSectionIdentifier = 0;
  }
  return self;
}

- (void)dealloc {
  [_signinPromoViewMediator signinPromoViewRemoved];
}

- (void)viewDidLoad {
  [super viewDidLoad];
  [self loadModel];
  self.view.accessibilityIdentifier =
      kRecentTabsTableViewControllerAccessibilityIdentifier;
  [self.tableView setDelegate:self];
  self.tableView.estimatedRowHeight = kEstimatedRowHeight;
  self.tableView.rowHeight = UITableViewAutomaticDimension;
  self.tableView.estimatedSectionHeaderHeight = kEstimatedRowHeight;
  self.tableView.sectionFooterHeight = 0.0;
  // Gesture recognizer for long press context menu on Session Headers.
  UILongPressGestureRecognizer* longPress =
      [[UILongPressGestureRecognizer alloc]
          initWithTarget:self
                  action:@selector(handleLongPress:)];
  longPress.delegate = self;
  [self.tableView addGestureRecognizer:longPress];
  // Gesture recognizer for header collapsing/expanding.
  UITapGestureRecognizer* tapGesture =
      [[UITapGestureRecognizer alloc] initWithTarget:self
                                              action:@selector(handleTap:)];
  tapGesture.delegate = self;
  [self.tableView addGestureRecognizer:tapGesture];
}

#pragma mark - TableViewModel

- (void)loadModel {
  [super loadModel];
  [self addRecentlyClosedSection];

  // The other Devices header section is always present regardless if it has any
  // items or not.
  [self addOtherDevicesSectionHeader];
  if (self.sessionState ==
      SessionsSyncUserState::USER_SIGNED_IN_SYNC_ON_WITH_SESSIONS) {
    [self addSessionSections];
  } else {
    [self addOtherDevicesItemForState:self.sessionState];
  }
}

#pragma mark Recently Closed Section

- (void)addRecentlyClosedSection {
  TableViewModel* model = self.tableViewModel;

  // Recently Closed Section.
  [model addSectionWithIdentifier:SectionIdentifierRecentlyClosedTabs];
  [model setSectionIdentifier:SectionIdentifierRecentlyClosedTabs
                 collapsedKey:kRecentlyClosedCollapsedKey];
  TableViewTextHeaderFooterItem* header = [[TableViewTextHeaderFooterItem alloc]
      initWithType:ItemTypeRecentlyClosedHeader];
  header.text = l10n_util::GetNSString(IDS_IOS_RECENT_TABS_RECENTLY_CLOSED);
  [model setHeader:header
      forSectionWithIdentifier:SectionIdentifierRecentlyClosedTabs];

  // Add Recently Closed Tabs Cells.
  [self addRecentlyClosedTabItems];

  // Add show full history item last.
  TableViewURLItem* historyItem =
      [[TableViewURLItem alloc] initWithType:ItemTypeShowFullHistory];
  historyItem.title = l10n_util::GetNSString(IDS_HISTORY_SHOWFULLHISTORY_LINK);
  historyItem.favicon = [UIImage imageNamed:@"ntp_opentabs_clock"];
  [model addItem:historyItem
      toSectionWithIdentifier:SectionIdentifierRecentlyClosedTabs];
}

- (void)addRecentlyClosedTabItems {
  if (!self.tabRestoreService)
    return;

  // Note that std:list<> can only be accessed sequentially, which is
  // suboptimal when using Cocoa table APIs. This list doesn't appear
  // to get very long, so it probably won't matter for perf.
  for (auto iter = self.tabRestoreService->entries().begin();
       iter != self.tabRestoreService->entries().end(); ++iter) {
    DCHECK(iter->get());
    const sessions::TabRestoreService::Entry* entry = iter->get();
    DCHECK(entry);
    // Use the page's title for the label, or its URL if title is empty.
    NSString* entryTitle;
    NSString* entryURL = @"";
    GURL entryGURL;
    switch (entry->type) {
      case sessions::TabRestoreService::TAB: {
        const sessions::TabRestoreService::Tab* tab =
            static_cast<const sessions::TabRestoreService::Tab*>(entry);
        const sessions::SerializedNavigationEntry& entry =
            tab->navigations[tab->current_navigation_index];
        // Use the page's title for the label, or its URL if title is empty.
        if (entry.title().size()) {
          entryTitle = base::SysUTF16ToNSString(entry.title());
          entryURL = base::SysUTF8ToNSString(entry.virtual_url().spec());
        } else {
          entryTitle = base::SysUTF8ToNSString(entry.virtual_url().spec());
        }
        entryGURL = entry.virtual_url();
        break;
      }
      case sessions::TabRestoreService::WINDOW: {
        // Only TAB type is handled.
        entryTitle = @"Window type - NOTIMPLEMENTED";
        break;
      }
    }

    // Configure and add the Item.
    TableViewURLItem* recentlyClosedTab =
        [[TableViewURLItem alloc] initWithType:ItemTypeRecentlyClosed];
    recentlyClosedTab.favicon = [UIImage imageNamed:@"default_favicon"];
    recentlyClosedTab.title = entryTitle;
    recentlyClosedTab.URL = entryURL;
    [self.tableViewModel addItem:recentlyClosedTab
         toSectionWithIdentifier:SectionIdentifierRecentlyClosedTabs];
  }
}

#pragma mark Sessions Section

// Cleans up the model in order to update the Session sections. Needs to be
// called inside a [UITableView beginUpdates] block on iOS10, or
// performBatchUpdates on iOS11+.
- (void)updateSessionSections {
  SessionsSyncUserState previousState = self.sessionState;
  if (previousState !=
      SessionsSyncUserState::USER_SIGNED_IN_SYNC_ON_WITH_SESSIONS) {
    // The previous state was one of the OtherDevices states, remove it to clean
    // it up and re-add just the header.
    [self.tableViewModel
        removeSectionWithIdentifier:SectionIdentifierOtherDevices];
    [self addOtherDevicesSectionHeader];
    // Reload the TableViews section.
    [self.tableView reloadSections:[self otherDevicesSectionIndexSet]
                  withRowAnimation:UITableViewRowAnimationNone];
  }

  // Clean up any previously added SessionSections.
  [self removeSessionSections];

  // Re-Add the session sections to |self.tableViewModel| and insert them into
  // |self.tableView|.
  [self addSessionSections];
  [self.tableView insertSections:[self sessionSectionIndexSet]
                withRowAnimation:UITableViewRowAnimationNone];
}

// Adds all the Remote Sessions sections with its respective items.
- (void)addSessionSections {
  TableViewModel* model = self.tableViewModel;
  for (NSUInteger i = 0; i < [self numberOfSessions]; i++) {
    synced_sessions::DistantSession const* session =
        _syncedSessions->GetSession(i);
    NSInteger sessionIdentifier = [self sectionIdentifierForSession:session];
    [model addSectionWithIdentifier:sessionIdentifier];
    NSString* sessionCollapsedKey = base::SysUTF8ToNSString(session->tag);
    [model setSectionIdentifier:sessionIdentifier
                   collapsedKey:sessionCollapsedKey];
    TableViewTextHeaderFooterItem* header =
        [[TableViewTextHeaderFooterItem alloc]
            initWithType:ItemTypeSessionHeader];
    header.text = base::SysUTF8ToNSString(session->name);
    header.subtitleText = l10n_util::GetNSStringF(
        IDS_IOS_OPEN_TABS_LAST_USED,
        base::SysNSStringToUTF16([self lastSyncStringForSesssion:session]));
    [model setHeader:header forSectionWithIdentifier:sessionIdentifier];
    [self addItemsForSession:session];
  }
}

- (void)addItemsForSession:(synced_sessions::DistantSession const*)session {
  TableViewModel* model = self.tableViewModel;
  NSInteger numberOfTabs = static_cast<NSInteger>(session->tabs.size());
  for (int i = 0; i < numberOfTabs; i++) {
    synced_sessions::DistantTab const* sessionTab = session->tabs[i].get();
    NSString* title = base::SysUTF16ToNSString(sessionTab->title);
    GURL url = sessionTab->virtual_url;

    TableViewURLItem* sessionTabItem =
        [[TableViewURLItem alloc] initWithType:ItemTypeSessionTabData];
    sessionTabItem.favicon = [UIImage imageNamed:@"default_favicon"];
    sessionTabItem.title = title;
    sessionTabItem.URL = base::SysUTF8ToNSString(url.host());
    [model addItem:sessionTabItem
        toSectionWithIdentifier:[self sectionIdentifierForSession:session]];
  }
}

// Remove all SessionSections from |self.tableViewModel| and |self.tableView|
// Needs to be called inside a [UITableView beginUpdates] block on iOS10, or
// performBatchUpdates on iOS11+.
- (void)removeSessionSections {
  // |_syncedSessions| has been updated by now, that means that
  // |self.tableViewModel| does not reflect |_syncedSessions| data. A
  // SectionIdentifier could've been deleted previously, do not rely on these
  // being in sequential order at this point.
  NSInteger sectionIdentifierToRemove = kFirstSessionSectionIdentifier;
  NSInteger sectionToDelete = kNumberOfSectionsBeforeSessions;
  while ([self.tableViewModel numberOfSections] >
         kNumberOfSectionsBeforeSessions) {
    if ([self.tableViewModel
            hasSectionForSectionIdentifier:sectionIdentifierToRemove]) {
      [self.tableView
            deleteSections:[NSIndexSet indexSetWithIndex:sectionToDelete]
          withRowAnimation:UITableViewRowAnimationNone];
      sectionToDelete++;
      [self.tableViewModel
          removeSectionWithIdentifier:sectionIdentifierToRemove];
    }
    sectionIdentifierToRemove++;
  }
}

#pragma mark Other Devices Section

// Cleans up the model in order to update the Other devices section. Needs to be
// called inside a [UITableView beginUpdates] block on iOS10, or
// performBatchUpdates on iOS11+.
- (void)updateOtherDevicesSectionForState:(SessionsSyncUserState)newState {
  TableViewModel* model = self.tableViewModel;
  SessionsSyncUserState previousState = self.sessionState;
  switch (previousState) {
    case SessionsSyncUserState::USER_SIGNED_IN_SYNC_OFF:
    case SessionsSyncUserState::USER_SIGNED_IN_SYNC_ON_NO_SESSIONS:
    case SessionsSyncUserState::USER_SIGNED_OUT:
    case SessionsSyncUserState::USER_SIGNED_IN_SYNC_IN_PROGRESS:
      // The OtherDevices section will be updated, remove it in order to clean
      // it up. After the clean up add just the header.
      [model removeSectionWithIdentifier:SectionIdentifierOtherDevices];
      [self addOtherDevicesSectionHeader];
      break;
    case SessionsSyncUserState::USER_SIGNED_IN_SYNC_ON_WITH_SESSIONS:
      // The Sessions Section exists, remove it.
      [self removeSessionSections];
      break;
  }
  // Add updated OtherDevices Item.
  [self addOtherDevicesItemForState:newState];

  // Update OtherDevices TableView Section.
  [self.tableView reloadSections:[self otherDevicesSectionIndexSet]
                withRowAnimation:UITableViewRowAnimationNone];
}

// Adds Other Devices Section and its header.
- (void)addOtherDevicesSectionHeader {
  TableViewModel* model = self.tableViewModel;
  [model addSectionWithIdentifier:SectionIdentifierOtherDevices];
  [model setSectionIdentifier:SectionIdentifierOtherDevices
                 collapsedKey:kOtherDeviceCollapsedKey];
  TableViewTextHeaderFooterItem* header = [[TableViewTextHeaderFooterItem alloc]
      initWithType:ItemTypeRecentlyClosedHeader];
  header.text = l10n_util::GetNSString(IDS_IOS_RECENT_TABS_OTHER_DEVICES);
  [model setHeader:header
      forSectionWithIdentifier:SectionIdentifierOtherDevices];
}

// Adds Other Devices item for |state|.
- (void)addOtherDevicesItemForState:(SessionsSyncUserState)state {
  // Add item depending on |state|.
  TableViewTextItem* dummyCell = nil;
  switch (state) {
    case SessionsSyncUserState::USER_SIGNED_IN_SYNC_ON_WITH_SESSIONS:
      NOTREACHED();
      return;
    case SessionsSyncUserState::USER_SIGNED_IN_SYNC_OFF:
      dummyCell =
          [[TableViewTextItem alloc] initWithType:ItemTypeOtherDevicesSyncOff];
      dummyCell.text =
          l10n_util::GetNSString(IDS_IOS_OPEN_TABS_ENABLE_SYNC_MOBILE);
      break;
    case SessionsSyncUserState::USER_SIGNED_IN_SYNC_ON_NO_SESSIONS:
      dummyCell = [[TableViewTextItem alloc]
          initWithType:ItemTypeOtherDevicesNoSessions];
      dummyCell.text =
          l10n_util::GetNSString(IDS_IOS_OPEN_TABS_NO_SESSION_INSTRUCTIONS);
      break;
    case SessionsSyncUserState::USER_SIGNED_OUT:
      dummyCell = [[TableViewTextItem alloc]
          initWithType:ItemTypeOtherDevicesSigninPromo];
      dummyCell.text = l10n_util::GetNSString(IDS_IOS_SIGNIN_PROMO_RECENT_TABS);
      break;
    case SessionsSyncUserState::USER_SIGNED_IN_SYNC_IN_PROGRESS:
      dummyCell = [[TableViewTextItem alloc]
          initWithType:ItemTypeOtherDevicesSyncInProgress];
      dummyCell.text = @"Sync in progress";
  }
  [self.tableViewModel addItem:dummyCell
       toSectionWithIdentifier:SectionIdentifierOtherDevices];
}

#pragma mark - TableViewModel Helpers

// Ordered array of all section identifiers.
- (NSArray*)allSessionSectionIdentifiers {
  NSMutableArray* allSessionSectionIdentifiers = [[NSMutableArray alloc] init];
  for (NSUInteger i = 0; i < [self numberOfSessions]; i++) {
    [allSessionSectionIdentifiers
        addObject:@(i + kFirstSessionSectionIdentifier)];
  }
  return allSessionSectionIdentifiers;
}

// Returns the TableViewModel SectionIdentifier for |distantSession|. Returns -1
// if |distantSession| doesn't exists.
- (NSInteger)sectionIdentifierForSession:
    (synced_sessions::DistantSession const*)distantSession {
  for (NSUInteger i = 0; i < [self numberOfSessions]; i++) {
    synced_sessions::DistantSession const* session =
        _syncedSessions->GetSession(i);
    if (session->tag == distantSession->tag)
      return i + kFirstSessionSectionIdentifier;
  }
  NOTREACHED();
  return -1;
}

// Returns an IndexSet containing the Other Devices Section.
- (NSIndexSet*)otherDevicesSectionIndexSet {
  NSUInteger otherDevicesSection = [self.tableViewModel
      sectionForSectionIdentifier:SectionIdentifierOtherDevices];
  return [NSIndexSet indexSetWithIndex:otherDevicesSection];
}

// Returns an IndexSet containing the all the Session Sections.
- (NSIndexSet*)sessionSectionIndexSet {
  // Create a range of all Session Sections.
  NSRange rangeOfSessionSections =
      NSMakeRange(kNumberOfSectionsBeforeSessions, [self numberOfSessions]);
  NSIndexSet* sessionSectionIndexes =
      [NSIndexSet indexSetWithIndexesInRange:rangeOfSessionSections];
  return sessionSectionIndexes;
}

// Returns YES if |sectionIdentifier| is a Sessions sectionIdentifier.
- (BOOL)isSessionSectionIdentifier:(NSInteger)sectionIdentifier {
  NSArray* sessionSectionIdentifiers = [self allSessionSectionIdentifiers];
  NSNumber* sectionIdentifierObject = @(sectionIdentifier);
  return [sessionSectionIdentifiers containsObject:sectionIdentifierObject];
}

#pragma mark - Consumer Protocol

- (void)refreshUserState:(SessionsSyncUserState)newSessionState {
  if ((newSessionState == self.sessionState &&
       self.sessionState !=
           SessionsSyncUserState::USER_SIGNED_IN_SYNC_ON_WITH_SESSIONS) ||
      self.signinPromoViewMediator.isSigninInProgress) {
    // No need to refresh the sections since all states other than
    // USER_SIGNED_IN_SYNC_ON_WITH_SESSIONS only have static content. This means
    // that if the previous State is the same as the new one the static content
    // won't change.
    return;
  }
  syncer::SyncService* syncService =
      IOSChromeProfileSyncServiceFactory::GetForBrowserState(self.browserState);
  _syncedSessions.reset(new synced_sessions::SyncedSessions(syncService));

  // Update the TableView and TableViewModel sections to match the new
  // sessionState.
  // Turn Off animations since UITableViewRowAnimationNone still animates.
  [UIView setAnimationsEnabled:NO];
  // If iOS11+ use performBatchUpdates: instead of begin/endUpdates.
  if (@available(iOS 11, *)) {
    if (newSessionState ==
        SessionsSyncUserState::USER_SIGNED_IN_SYNC_ON_WITH_SESSIONS) {
      [self.tableView performBatchUpdates:^{
        [self updateSessionSections];
      }
                               completion:nil];
    } else {
      [self.tableView performBatchUpdates:^{
        [self updateOtherDevicesSectionForState:newSessionState];
      }
                               completion:nil];
    }
  } else {
    [self.tableView beginUpdates];
    if (newSessionState ==
        SessionsSyncUserState::USER_SIGNED_IN_SYNC_ON_WITH_SESSIONS) {
      [self updateSessionSections];
    } else {
      [self updateOtherDevicesSectionForState:newSessionState];
    }
    [self.tableView endUpdates];
  }
  [UIView setAnimationsEnabled:YES];

  self.sessionState = newSessionState;
  if (self.sessionState != SessionsSyncUserState::USER_SIGNED_OUT) {
    [self.signinPromoViewMediator signinPromoViewRemoved];
    self.signinPromoViewMediator = nil;
  }
}

- (void)refreshRecentlyClosedTabs {
  [self.tableView reloadData];
}

- (void)setTabRestoreService:(sessions::TabRestoreService*)tabRestoreService {
  _tabRestoreService = tabRestoreService;
}

- (void)dismissModals {
  [self.contextMenuCoordinator stop];
}

#pragma mark - UITableViewDelegate

- (void)tableView:(UITableView*)tableView
    didSelectRowAtIndexPath:(NSIndexPath*)indexPath {
  DCHECK_EQ(tableView, self.tableView);
  [self.tableView deselectRowAtIndexPath:indexPath animated:YES];
  NSInteger itemTypeSelected =
      [self.tableViewModel itemTypeForIndexPath:indexPath];
  switch (itemTypeSelected) {
    case ItemTypeRecentlyClosed:
      [self
          openTabWithTabRestoreEntry:[self
                                         tabRestoreEntryAtIndexPath:indexPath]];
      break;
    case ItemTypeSessionTabData:
      [self
          openTabWithContentOfDistantTab:[self
                                             distantTabAtIndexPath:indexPath]];
      break;
    case ItemTypeShowFullHistory:
      [tableView deselectRowAtIndexPath:indexPath animated:NO];
      [self showFullHistory];
      break;
    case ItemTypeOtherDevicesSyncOff:
    case ItemTypeOtherDevicesNoSessions:
    case ItemTypeOtherDevicesSigninPromo:
    case ItemTypeOtherDevicesSyncInProgress:
      break;
  }
}

#pragma mark - Recently closed tab helpers

- (NSInteger)numberOfRecentlyClosedTabs {
  if (!self.tabRestoreService)
    return 0;
  return static_cast<NSInteger>(self.tabRestoreService->entries().size());
}

- (const sessions::TabRestoreService::Entry*)tabRestoreEntryAtIndexPath:
    (NSIndexPath*)indexPath {
  DCHECK_EQ([self.tableViewModel sectionIdentifierForSection:indexPath.section],
            SectionIdentifierRecentlyClosedTabs);
  NSInteger index = indexPath.row;
  DCHECK_LE(index, [self numberOfRecentlyClosedTabs]);
  if (!self.tabRestoreService)
    return nullptr;

  // Advance the entry iterator to the correct index.
  // Note that std:list<> can only be accessed sequentially, which is
  // suboptimal when using Cocoa table APIs. This list doesn't appear
  // to get very long, so it probably won't matter for perf.
  sessions::TabRestoreService::Entries::const_iterator iter =
      self.tabRestoreService->entries().begin();
  std::advance(iter, index);
  CHECK(*iter);
  return iter->get();
}

// TO IMPLEMENT. Remove this depending on which TableStyle we'll use.
- (CGFloat)tableView:(UITableView*)tableView
    heightForFooterInSection:(NSInteger)section {
  return 1.0;
}

#pragma mark - Distant Sessions helpers

- (NSUInteger)numberOfSessions {
  if (!_syncedSessions)
    return 0;
  return _syncedSessions->GetSessionCount();
}

// Returns the Session Index for a given Session Tab |indexPath|.
- (size_t)indexOfSessionForTabAtIndexPath:(NSIndexPath*)indexPath {
  DCHECK_EQ([self.tableViewModel itemTypeForIndexPath:indexPath],
            ItemTypeSessionTabData);
  // Get the sectionIdentifier for |indexPath|,
  NSNumber* sectionIdentifierForIndexPath =
      @([self.tableViewModel sectionIdentifierForSection:indexPath.section]);
  // Get the index of this sectionIdentifier.
  size_t indexOfSession = [[self allSessionSectionIdentifiers]
      indexOfObject:sectionIdentifierForIndexPath];
  DCHECK_LT(indexOfSession, _syncedSessions->GetSessionCount());
  return indexOfSession;
}

- (synced_sessions::DistantSession const*)sessionForTabAtIndexPath:
    (NSIndexPath*)indexPath {
  return _syncedSessions->GetSession(
      [self indexOfSessionForTabAtIndexPath:indexPath]);
}

- (synced_sessions::DistantSession const*)sessionForSection:(NSInteger)section {
  return _syncedSessions->GetSession(section - kNumberOfSectionsBeforeSessions);
}

- (synced_sessions::DistantTab const*)distantTabAtIndexPath:
    (NSIndexPath*)indexPath {
  DCHECK_EQ([self.tableViewModel itemTypeForIndexPath:indexPath],
            ItemTypeSessionTabData);
  size_t indexOfDistantTab = indexPath.row;
  synced_sessions::DistantSession const* session =
      [self sessionForTabAtIndexPath:indexPath];
  DCHECK_LT(indexOfDistantTab, session->tabs.size());
  return session->tabs[indexOfDistantTab].get();
}

- (NSString*)lastSyncStringForSesssion:
    (synced_sessions::DistantSession const*)session {
  base::Time time = session->modified_time;
  NSDate* lastUsedDate = [NSDate dateWithTimeIntervalSince1970:time.ToTimeT()];
  NSString* dateString =
      [NSDateFormatter localizedStringFromDate:lastUsedDate
                                     dateStyle:NSDateFormatterShortStyle
                                     timeStyle:NSDateFormatterNoStyle];

  NSString* timeString;
  base::TimeDelta last_used_delta;
  if (base::Time::Now() > time)
    last_used_delta = base::Time::Now() - time;

  if (last_used_delta.InMicroseconds() < base::Time::kMicrosecondsPerMinute) {
    timeString = l10n_util::GetNSString(IDS_IOS_OPEN_TABS_RECENTLY_SYNCED);
    // This will return something similar to "Seconds ago mm/dd/yy"
    return [NSString stringWithFormat:@"%@ %@", timeString, dateString];
  }

  if (last_used_delta.InHours() < kRelativeTimeMaxHours) {
    timeString = base::SysUTF16ToNSString(
        ui::TimeFormat::Simple(ui::TimeFormat::FORMAT_ELAPSED,
                               ui::TimeFormat::LENGTH_SHORT, last_used_delta));
    // This will return something similar to "1 min/hour ago mm/dd/yy"
    return [NSString stringWithFormat:@"%@ %@", timeString, dateString];
  }

  NSDate* date = [NSDate dateWithTimeIntervalSince1970:time.ToTimeT()];
  timeString =
      [NSDateFormatter localizedStringFromDate:date
                                     dateStyle:NSDateFormatterNoStyle
                                     timeStyle:NSDateFormatterShortStyle];
  // This will return something similar to "H:MM mm/dd/yy"
  return [NSString stringWithFormat:@"%@ %@", timeString, dateString];
}

#pragma mark - Navigation helpers

- (void)dismissRecentTabsModal {
  [self.handsetCommandHandler dismissRecentTabsWithCompletion:nil];
}

- (void)openTabWithContentOfDistantTab:
    (synced_sessions::DistantTab const*)distantTab {
  sync_sessions::OpenTabsUIDelegate* openTabs =
      IOSChromeProfileSyncServiceFactory::GetForBrowserState(self.browserState)
          ->GetOpenTabsUIDelegate();
  const sessions::SessionTab* toLoad = nullptr;
  [self dismissRecentTabsModal];
  if (openTabs->GetForeignTab(distantTab->session_tag, distantTab->tab_id,
                              &toLoad)) {
    base::RecordAction(base::UserMetricsAction(
        "MobileRecentTabManagerTabFromOtherDeviceOpened"));
    new_tab_page_uma::RecordAction(
        self.browserState, new_tab_page_uma::ACTION_OPENED_FOREIGN_SESSION);
    [self.loader loadSessionTab:toLoad];
  }
}

- (void)openTabWithTabRestoreEntry:
    (const sessions::TabRestoreService::Entry*)entry {
  DCHECK(entry);
  if (!entry)
    return;
  // Only TAB type is handled.
  if (entry->type != sessions::TabRestoreService::TAB)
    return;
  TabRestoreServiceDelegateImplIOS* delegate =
      TabRestoreServiceDelegateImplIOSFactory::GetForBrowserState(
          self.browserState);
  [self dismissRecentTabsModal];
  base::RecordAction(
      base::UserMetricsAction("MobileRecentTabManagerRecentTabOpened"));
  new_tab_page_uma::RecordAction(
      self.browserState, new_tab_page_uma::ACTION_OPENED_RECENTLY_CLOSED_ENTRY);
  self.tabRestoreService->RestoreEntryById(delegate, entry->id,
                                           WindowOpenDisposition::CURRENT_TAB);
}

- (void)showFullHistory {
  __weak RecentTabsTableViewController* weakSelf = self;
  ProceduralBlock openHistory = ^{
    [weakSelf.dispatcher showHistory];
  };
  DCHECK(self.handsetCommandHandler);
  [self.handsetCommandHandler dismissRecentTabsWithCompletion:openHistory];
}

#pragma mark - Collapse/Expand sections

- (void)handleTap:(UITapGestureRecognizer*)tapGesture {
  DCHECK_EQ(self.tableView, tapGesture.view);
  if (tapGesture.state == UIGestureRecognizerStateEnded) {
    [self toggleExpansionOfSectionIdentifier:
              self.lastTappedHeaderSectionIdentifier];
  }
}

- (void)toggleExpansionOfSectionIdentifier:(NSInteger)sectionIdentifier {
  NSMutableArray* cellIndexPathsToDeleteOrInsert = [NSMutableArray array];
  NSInteger sectionIndex =
      [self.tableViewModel sectionForSectionIdentifier:sectionIdentifier];
  NSArray* items =
      [self.tableViewModel itemsInSectionWithIdentifier:sectionIdentifier];
  for (NSUInteger i = 0; i < [items count]; i++) {
    NSIndexPath* tabIndexPath =
        [NSIndexPath indexPathForRow:i inSection:sectionIndex];
    [cellIndexPathsToDeleteOrInsert addObject:tabIndexPath];
  }

  void (^tableUpdates)(void) = ^{
    if ([self.tableViewModel sectionIsCollapsed:sectionIdentifier]) {
      [self.tableViewModel setSection:sectionIdentifier collapsed:NO];
      [self.tableView insertRowsAtIndexPaths:cellIndexPathsToDeleteOrInsert
                            withRowAnimation:UITableViewRowAnimationFade];
    } else {
      [self.tableViewModel setSection:sectionIdentifier collapsed:YES];
      [self.tableView deleteRowsAtIndexPaths:cellIndexPathsToDeleteOrInsert
                            withRowAnimation:UITableViewRowAnimationFade];
    }
  };

  // If iOS11+ use performBatchUpdates: instead of beginUpdates/endUpdates.
  if (@available(iOS 11, *)) {
    [self.tableView performBatchUpdates:tableUpdates completion:nil];
  } else {
    [self.tableView beginUpdates];
    tableUpdates();
    [self.tableView endUpdates];
  }
}

#pragma mark - Long press and context menus

- (void)handleLongPress:(UILongPressGestureRecognizer*)longPressGesture {
  DCHECK_EQ(self.tableView, longPressGesture.view);
  if (longPressGesture.state == UIGestureRecognizerStateBegan) {
    NSInteger sectionIdentifier = self.lastTappedHeaderSectionIdentifier;
    if (![self isSessionSectionIdentifier:sectionIdentifier]) {
      // Only handle LongPress for SessionHeaders.
      return;
    }

    web::ContextMenuParams params;
    // Get view coordinates in local space.
    CGPoint viewCoordinate = [longPressGesture locationInView:self.tableView];
    params.location = viewCoordinate;
    params.view = self.tableView;

    // Present sheet/popover using controller that is added to view hierarchy.
    // TODO(crbug.com/754642): Remove TopPresentedViewController().
    UIViewController* topController =
        top_view_controller::TopPresentedViewController();

    self.contextMenuCoordinator =
        [[ContextMenuCoordinator alloc] initWithBaseViewController:topController
                                                            params:params];

    // Fill the sheet/popover with buttons.
    __weak RecentTabsTableViewController* weakSelf = self;

    // "Open all tabs" button.
    NSString* openAllButtonLabel =
        l10n_util::GetNSString(IDS_IOS_RECENT_TABS_OPEN_ALL_MENU_OPTION);
    [self.contextMenuCoordinator
        addItemWithTitle:openAllButtonLabel
                  action:^{
                    [weakSelf
                        openTabsFromSessionSectionIdentifier:sectionIdentifier];
                  }];

    // "Hide for now" button.
    NSString* hideButtonLabel =
        l10n_util::GetNSString(IDS_IOS_RECENT_TABS_HIDE_MENU_OPTION);
    [self.contextMenuCoordinator
        addItemWithTitle:hideButtonLabel
                  action:^{
                    [weakSelf removeSessionAtSessionSectionIdentifier:
                                  sectionIdentifier];
                  }];

    [self.contextMenuCoordinator start];
  }
}

- (void)openTabsFromSessionSectionIdentifier:(NSInteger)sectionIdentifier {
  NSInteger section =
      [self.tableViewModel sectionForSectionIdentifier:sectionIdentifier];
  synced_sessions::DistantSession const* session =
      [self sessionForSection:section];
  [self dismissRecentTabsModal];
  for (auto const& tab : session->tabs) {
    [self.loader webPageOrderedOpen:tab->virtual_url
                           referrer:web::Referrer()
                       inBackground:YES
                           appendTo:kLastTab];
  }
}

- (void)removeSessionAtSessionSectionIdentifier:(NSInteger)sectionIdentifier {
  DCHECK([self isSessionSectionIdentifier:sectionIdentifier]);
  NSInteger section =
      [self.tableViewModel sectionForSectionIdentifier:sectionIdentifier];
  synced_sessions::DistantSession const* session =
      [self sessionForSection:section];
  std::string sessionTagCopy = session->tag;
  syncer::SyncService* syncService =
      IOSChromeProfileSyncServiceFactory::GetForBrowserState(self.browserState);
  sync_sessions::OpenTabsUIDelegate* openTabs =
      syncService->GetOpenTabsUIDelegate();

  [self.tableViewModel removeSectionWithIdentifier:sectionIdentifier];
  _syncedSessions->EraseSession(sectionIdentifier -
                                kFirstSessionSectionIdentifier);

  void (^tableUpdates)(void) = ^{
    [self.tableView
          deleteSections:[NSIndexSet
                             indexSetWithIndex:sectionIdentifier -
                                               kSectionIdentifierEnumZero]
        withRowAnimation:UITableViewRowAnimationLeft];
  };

  // If iOS11+ use performBatchUpdates: instead of beginUpdates/endUpdates.
  if (@available(iOS 11, *)) {
    [self.tableView performBatchUpdates:tableUpdates
                             completion:^(BOOL) {
                               openTabs->DeleteForeignSession(sessionTagCopy);
                             }];
  } else {
    [self.tableView beginUpdates];
    tableUpdates();
    // DeleteForeignSession will cause |self refreshUserState:| to be called,
    // thus refreshing the TableView, running this inside the updates block will
    // make sure that the tableView animations are performed in order.
    openTabs->DeleteForeignSession(sessionTagCopy);
    [self.tableView endUpdates];
  }
}

#pragma mark - UIGestureRecognizerDelegate

- (BOOL)gestureRecognizerShouldBegin:(UIGestureRecognizer*)gestureRecognizer {
  CGPoint point = [gestureRecognizer locationInView:self.tableView];
  // Context menus can be opened on a Session section header.
  for (NSInteger section = 0; section < [self.tableViewModel numberOfSections];
       section++) {
    NSInteger itemType =
        [self.tableViewModel sectionIdentifierForSection:section];
    if (CGRectContainsPoint([self.tableView rectForHeaderInSection:section],
                            point)) {
      self.lastTappedHeaderSectionIdentifier = itemType;
      return YES;
    }
  }
  return NO;
}

#pragma mark - SigninPromoViewConsumer

- (void)configureSigninPromoWithConfigurator:
            (SigninPromoViewConfigurator*)configurator
                             identityChanged:(BOOL)identityChanged {
  DCHECK(self.signinPromoViewMediator);
  if ([self.tableViewModel sectionIsCollapsed:SectionIdentifierOtherDevices])
    return;
  NSIndexPath* indexPath =
      [self.tableViewModel indexPathForItemType:ItemTypeOtherDevicesSigninPromo
                              sectionIdentifier:SectionIdentifierOtherDevices];
  if (identityChanged) {
    [self.tableView reloadRowsAtIndexPaths:@[ indexPath ]
                          withRowAnimation:UITableViewRowAnimationNone];
    return;
  }
  UITableViewCell* cell = [self.tableView cellForRowAtIndexPath:indexPath];
  NSArray<UIView*>* contentViews = cell.contentView.subviews;
  DCHECK(contentViews.count == 1);
  UIView* subview = contentViews[0];
  DCHECK([subview isKindOfClass:[SigninPromoView class]]);
  SigninPromoView* signinPromoView = (SigninPromoView*)subview;
  [configurator configureSigninPromoView:signinPromoView];
}

- (void)signinDidFinish {
  [self.delegate refreshSessionsView];
}

#pragma mark - SyncPresenter

- (void)showReauthenticateSignin {
  [self.dispatcher
              showSignin:
                  [[ShowSigninCommand alloc]
                      initWithOperation:AUTHENTICATION_OPERATION_REAUTHENTICATE
                            accessPoint:signin_metrics::AccessPoint::
                                            ACCESS_POINT_UNKNOWN]
      baseViewController:self];
}

- (void)showSyncSettings {
  [self.dispatcher showSyncSettingsFromViewController:self];
}

- (void)showSyncPassphraseSettings {
  [self.dispatcher showSyncPassphraseSettingsFromViewController:self];
}

#pragma mark - SigninPresenter

- (void)showSignin:(ShowSigninCommand*)command {
  [self.dispatcher showSignin:command baseViewController:self];
}

@end
