// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

package org.chromium.chrome.browser.contextualsearch;

import android.app.Activity;
import android.view.ContextMenu;

import org.chromium.base.annotations.CalledByNative;
import org.chromium.chrome.browser.ChromeActivity;
import org.chromium.chrome.browser.compositor.bottombar.OverlayPanel.StateChangeReason;
import org.chromium.chrome.browser.firstrun.FirstRunStatus;
import org.chromium.chrome.browser.locale.LocaleManager;
import org.chromium.chrome.browser.preferences.PrefServiceBridge;
import org.chromium.chrome.browser.profiles.Profile;
import org.chromium.chrome.browser.search_engines.TemplateUrlService;
import org.chromium.chrome.browser.search_engines.TemplateUrlService.TemplateUrlServiceObserver;
import org.chromium.chrome.browser.tab.EmptyTabObserver;
import org.chromium.chrome.browser.tab.Tab;
import org.chromium.content_public.browser.GestureListenerManager;
import org.chromium.content_public.browser.GestureStateListener;
import org.chromium.content_public.browser.SelectionPopupController;
import org.chromium.content_public.browser.WebContents;
import org.chromium.net.NetworkChangeNotifier;

/** Manages the activation and gesture listeners for ContextualSearch on a given tab. */
public class ContextualSearchTabHelper
        extends EmptyTabObserver implements NetworkChangeNotifier.ConnectionTypeObserver {
    /** The Tab that this helper tracks. */
    private final Tab mTab;

    // Device scale factor.
    private final float mPxToDp;

    /** Notification handler for Contextual Search events. */
    private TemplateUrlServiceObserver mTemplateUrlObserver;

    /**
     * The WebContents associated with the Tab which this helper is monitoring, unless detached.
     */
    private WebContents mWebContents;

    /**
     * The {@link ContextualSearchManager} that's managing this tab. This may point to
     * the manager from another activity during reparenting, or be {@code null} during startup.
     */
    private ContextualSearchManager mContextualSearchManager;

    /** The GestureListener used for handling events from the current WebContents. */
    private GestureStateListener mGestureStateListener;

    /**
     * Manages incoming calls to Smart Select when available, for the current base WebContents.
     */
    private SelectionClientManager mSelectionClientManager;

    private long mNativeHelper;

    /**
     * Creates a contextual search tab helper for the given tab.
     * @param tab The tab whose contextual search actions will be handled by this helper.
     */
    public static void createForTab(Tab tab) {
        new ContextualSearchTabHelper(tab);
    }

    /**
     * Constructs a Tab helper that can enable and disable Contextual Search based on Tab activity.
     * @param tab The {@link Tab} to track with this helper.
     */
    private ContextualSearchTabHelper(Tab tab) {
        mTab = tab;
        tab.addObserver(this);
        // Connect to a network, unless under test.
        if (NetworkChangeNotifier.isInitialized()) {
            NetworkChangeNotifier.addConnectionTypeObserver(this);
        }
        float scaleFactor = 1.f;
        if (tab != null && tab.getActivity() != null && tab.getActivity().getResources() != null) {
            scaleFactor /= tab.getActivity().getResources().getDisplayMetrics().density;
        }
        mPxToDp = scaleFactor;
    }

    // ============================================================================================
    // EmptyTabObserver overrides.
    // ============================================================================================

    @Override
    public void onPageLoadStarted(Tab tab, String url) {
        updateHooksForTab(tab);
        ContextualSearchManager manager = getContextualSearchManager(tab);
        if (manager != null) manager.onBasePageLoadStarted();
    }

    @Override
    public void onContentChanged(Tab tab) {
        // Native initialization happens after a page loads or content is changed to ensure profile
        // is initialized.
        if (mNativeHelper == 0) {
            mNativeHelper = nativeInit(tab.getProfile());
        }
        if (mTemplateUrlObserver == null) {
            mTemplateUrlObserver = new TemplateUrlServiceObserver() {
                @Override
                public void onTemplateURLServiceChanged() {
                    updateContextualSearchHooks(mWebContents);
                }
            };
            TemplateUrlService.getInstance().addObserver(mTemplateUrlObserver);
        }
        updateHooksForTab(tab);
    }

    @Override
    public void onWebContentsSwapped(Tab tab, boolean didStartLoad, boolean didFinishLoad) {
        updateHooksForTab(tab);
    }

    @Override
    public void onDestroyed(Tab tab) {
        if (mNativeHelper != 0) {
            nativeDestroy(mNativeHelper);
            mNativeHelper = 0;
        }
        if (mTemplateUrlObserver != null) {
            TemplateUrlService.getInstance().removeObserver(mTemplateUrlObserver);
        }
        if (NetworkChangeNotifier.isInitialized()) {
            NetworkChangeNotifier.removeConnectionTypeObserver(this);
        }
        removeContextualSearchHooks(mWebContents);
        mWebContents = null;
        mContextualSearchManager = null;
        mSelectionClientManager = null;
        mGestureStateListener = null;
    }

    @Override
    public void onToggleFullscreenMode(Tab tab, boolean enable) {
        ContextualSearchManager manager = getContextualSearchManager(tab);
        if (manager != null) {
            manager.hideContextualSearch(StateChangeReason.UNKNOWN);
        }
    }

    @Override
    public void onActivityAttachmentChanged(Tab tab, boolean isAttached) {
        if (isAttached) {
            updateHooksForTab(tab);
        } else {
            removeContextualSearchHooks(mWebContents);
            mContextualSearchManager = null;
        }
    }

    @Override
    public void onContextMenuShown(Tab tab, ContextMenu menu) {
        ContextualSearchManager manager = getContextualSearchManager(tab);
        if (manager != null) {
            manager.onContextMenuShown();
        }
    }

    // ============================================================================================
    // NetworkChangeNotifier.ConnectionTypeObserver overrides.
    // ============================================================================================

    @Override
    public void onConnectionTypeChanged(int connectionType) {
        updateContextualSearchHooks(mWebContents);
    }

    // ============================================================================================
    // Private helpers.
    // ============================================================================================

    /**
     * Should be called whenever the Tab's WebContents may have changed. Removes hooks from the
     * existing WebContents, if necessary, and then adds hooks for the new ContentViewCore.
     * @param tab The current tab.
     */
    private void updateHooksForTab(Tab tab) {
        WebContents currentWebContents = tab.getWebContents();
        if (currentWebContents != mWebContents
                || mContextualSearchManager != getContextualSearchManager(tab)) {
            mWebContents = currentWebContents;
            mContextualSearchManager = getContextualSearchManager(tab);
            if (mWebContents != null && mSelectionClientManager == null) {
                mSelectionClientManager = new SelectionClientManager(mWebContents);
            }
            updateContextualSearchHooks(mWebContents);
        }
    }

    /**
     * Updates the Contextual Search hooks, adding or removing them depending on whether it is
     * currently active. If the current tab's {@link WebContents} may have changed, call {@link
     * #updateHooksForTab(Tab)} instead.
     *
     * @param webContents The WebContents to attach the gesture state listener to.
     */
    private void updateContextualSearchHooks(WebContents webContents) {
        if (webContents == null) return;

        removeContextualSearchHooks(webContents);
        if (isContextualSearchActive(webContents)) addContextualSearchHooks(webContents);
    }

    /**
     * Adds Contextual Search hooks for its client and listener to the given WebContents.
     * @param webContents The WebContents to attach the gesture state listener to.
     */
    private void addContextualSearchHooks(WebContents webContents) {
        assert mTab.getWebContents() == null || mTab.getWebContents() == webContents;
        ContextualSearchManager contextualSearchManager = getContextualSearchManager(mTab);
        if (mGestureStateListener == null && contextualSearchManager != null) {
            mGestureStateListener = contextualSearchManager.getGestureStateListener();
            GestureListenerManager.fromWebContents(webContents).addListener(mGestureStateListener);

            // If we needed to add our listener, we also need to add our selection client.
            SelectionPopupController controller =
                    SelectionPopupController.fromWebContents(webContents);
            controller.setSelectionClient(
                    mSelectionClientManager.addContextualSearchSelectionClient(
                            contextualSearchManager.getContextualSearchSelectionClient()));
            contextualSearchManager.suppressContextualSearchForSmartSelection(
                    mSelectionClientManager.isSmartSelectionEnabledInChrome());
            nativeInstallUnhandledTapNotifierIfNeeded(mNativeHelper, webContents, mPxToDp);
        }
    }

    /**
     * Removes Contextual Search hooks for its client and listener from the given WebContents.
     * @param webContents The WebContents to detach the gesture state listener from.
     */
    private void removeContextualSearchHooks(WebContents webContents) {
        if (webContents == null) return;

        if (mGestureStateListener != null) {
            GestureListenerManager.fromWebContents(webContents)
                    .removeListener(mGestureStateListener);
            mGestureStateListener = null;

            // If we needed to remove our listener, we also need to remove our selection client.
            if (mSelectionClientManager != null) {
                SelectionPopupController controller =
                        SelectionPopupController.fromWebContents(webContents);
                controller.setSelectionClient(
                        mSelectionClientManager.removeContextualSearchSelectionClient());
            }
            // Also make sure the UI is hidden if the device is offline.
            ContextualSearchManager contextualSearchManager = getContextualSearchManager(mTab);
            if (contextualSearchManager != null && !isDeviceOnline(contextualSearchManager)) {
                contextualSearchManager.hideContextualSearch(StateChangeReason.UNKNOWN);
            }
        }
    }

    /** @return whether Contextual Search is enabled and active in this tab. */
    private boolean isContextualSearchActive(WebContents webContents) {
        assert mTab.getWebContents() == null || mTab.getWebContents() == webContents;
        ContextualSearchManager manager = getContextualSearchManager(mTab);
        if (manager == null) return false;

        return !webContents.isIncognito() && FirstRunStatus.getFirstRunFlowComplete()
                && !PrefServiceBridge.getInstance().isContextualSearchDisabled()
                && TemplateUrlService.getInstance().isDefaultSearchEngineGoogle()
                && !LocaleManager.getInstance().needToCheckForSearchEnginePromo()
                // Svelte and Accessibility devices are incompatible with the first-run flow and
                // Talkback has poor interaction with tap to search (see http://crbug.com/399708 and
                // http://crbug.com/396934).
                && !manager.isRunningInCompatibilityMode()
                && !(mTab.isShowingErrorPage() || mTab.isShowingInterstitialPage())
                && isDeviceOnline(manager);
    }

    /** @return Whether the device is online, or we have disabled online-detection. */
    private boolean isDeviceOnline(ContextualSearchManager manager) {
        if (ContextualSearchFieldTrial.isOnlineDetectionDisabled()) return true;

        return manager.isDeviceOnline();
    }

    /**
     * Gets the {@link ContextualSearchManager} associated with the given tab's activity.
     * @param tab The {@link Tab} that we're getting the manager for.
     * @return The Contextual Search manager controlling that Tab.
     */
    private ContextualSearchManager getContextualSearchManager(Tab tab) {
        Activity activity = tab.getWindowAndroid().getActivity().get();
        if (activity instanceof ChromeActivity) {
            return ((ChromeActivity) activity).getContextualSearchManager();
        }
        return null;
    }

    // ============================================================================================
    // Native support.
    // ============================================================================================

    @CalledByNative
    void onContextualSearchPrefChanged() {
        updateContextualSearchHooks(mWebContents);

        ContextualSearchManager manager = getContextualSearchManager(mTab);
        if (manager != null) {
            boolean isEnabled = !PrefServiceBridge.getInstance().isContextualSearchDisabled()
                    && !PrefServiceBridge.getInstance().isContextualSearchUninitialized();
            manager.onContextualSearchPrefChanged(isEnabled);
        }
    }

    /**
     * Notifies this helper to show the Unhandled Tap UI due to a tap at the given pixel
     * coordinates.
     */
    @CalledByNative
    void onShowUnhandledTapUIIfNeeded(int x, int y) {
        // Only notify the manager if we currently have a valid listener.
        if (mGestureStateListener != null && getContextualSearchManager(mTab) != null) {
            getContextualSearchManager(mTab).onShowUnhandledTapUIIfNeeded(x, y);
        }
    }

    private native long nativeInit(Profile profile);
    private native void nativeInstallUnhandledTapNotifierIfNeeded(
            long nativeContextualSearchTabHelper, WebContents webContents, float pxToDpScaleFactor);
    private native void nativeDestroy(long nativeContextualSearchTabHelper);
}
