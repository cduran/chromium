// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/**
 * @fileoverview Handles automation from a desktop automation node.
 */

goog.provide('DesktopAutomationHandler');

goog.require('AutomationObjectConstructorInstaller');
goog.require('BaseAutomationHandler');
goog.require('ChromeVoxState');
goog.require('CommandHandler');
goog.require('CustomAutomationEvent');
goog.require('editing.TextEditHandler');

goog.scope(function() {
var AutomationEvent = chrome.automation.AutomationEvent;
var AutomationNode = chrome.automation.AutomationNode;
var Dir = constants.Dir;
var EventType = chrome.automation.EventType;
var RoleType = chrome.automation.RoleType;
var StateType = chrome.automation.StateType;

/**
 * @param {!AutomationNode} node
 * @constructor
 * @extends {BaseAutomationHandler}
 */
DesktopAutomationHandler = function(node) {
  BaseAutomationHandler.call(this, node);

  /**
   * The object that speaks changes to an editable text field.
   * @type {editing.TextEditHandler}
   * @private
   */
  this.textEditHandler_ = null;

  /**
   * The last time we handled a value changed event.
   * @type {!Date}
   * @private
   */
  this.lastValueChanged_ = new Date(0);

  /** @private {AutomationNode} */
  this.lastValueTarget_ = null;

  /** @private {string} */
  this.lastRootUrl_ = '';

  this.addListener_(
      EventType.ACTIVEDESCENDANTCHANGED, this.onActiveDescendantChanged);
  this.addListener_(EventType.ALERT, this.onAlert);
  this.addListener_(
      EventType.ARIA_ATTRIBUTE_CHANGED, this.onAriaAttributeChanged);
  this.addListener_(EventType.AUTOCORRECTION_OCCURED, this.onEventIfInRange);
  this.addListener_(EventType.BLUR, this.onBlur);
  this.addListener_(
      EventType.CHECKED_STATE_CHANGED, this.onCheckedStateChanged);
  this.addListener_(EventType.CHILDREN_CHANGED, this.onChildrenChanged);
  this.addListener_(EventType.EXPANDED_CHANGED, this.onEventIfInRange);
  this.addListener_(EventType.FOCUS, this.onFocus);
  this.addListener_(EventType.HOVER, this.onHover);
  this.addListener_(EventType.INVALID_STATUS_CHANGED, this.onEventIfInRange);
  this.addListener_(EventType.LOAD_COMPLETE, this.onLoadComplete);
  this.addListener_(EventType.LOCATION_CHANGED, this.onLocationChanged);
  this.addListener_(EventType.MENU_END, this.onMenuEnd);
  this.addListener_(EventType.MENU_LIST_ITEM_SELECTED, this.onEventIfSelected);
  this.addListener_(EventType.MENU_START, this.onMenuStart);
  this.addListener_(EventType.ROW_COLLAPSED, this.onEventIfInRange);
  this.addListener_(EventType.ROW_EXPANDED, this.onEventIfInRange);
  this.addListener_(
      EventType.SCROLL_POSITION_CHANGED, this.onScrollPositionChanged);
  this.addListener_(EventType.SELECTION, this.onSelection);
  this.addListener_(EventType.TEXT_CHANGED, this.onEditableChanged_);
  this.addListener_(EventType.TEXT_SELECTION_CHANGED, this.onEditableChanged_);
  this.addListener_(EventType.VALUE_CHANGED, this.onValueChanged);

  AutomationObjectConstructorInstaller.init(node, function() {
    chrome.automation.getFocus(
        (function(focus) {
          if (focus) {
            var event =
                new CustomAutomationEvent(EventType.FOCUS, focus, 'page');
            this.onFocus(event);
          }
        }).bind(this));
  }.bind(this));
};

/**
 * Time to wait until processing more value changed events.
 * @const {number}
 */
DesktopAutomationHandler.VMIN_VALUE_CHANGE_DELAY_MS = 500;

/**
 * Controls announcement of non-user-initiated events.
 * @type {boolean}
 */
DesktopAutomationHandler.announceActions = false;

/**
 * The url of the keyboard.
 * @const {string}
 */
DesktopAutomationHandler.KEYBOARD_URL =
    'chrome-extension://jkghodnilhceideoidjikpgommlajknk/inputview.html';

DesktopAutomationHandler.prototype = {
  __proto__: BaseAutomationHandler.prototype,

  /** @type {editing.TextEditHandler} */
  get textEditHandler() {
    return this.textEditHandler_;
  },

  /** @override */
  willHandleEvent_: function(evt) {
    return !cvox.ChromeVox.isActive;
  },

  /**
   * Provides all feedback once ChromeVox's focus changes.
   * @param {!AutomationEvent} evt
   */
  onEventDefault: function(evt) {
    var node = evt.target;
    if (!node)
      return;

    // Decide whether to announce and sync this event.
    if (!DesktopAutomationHandler.announceActions && evt.eventFrom == 'action')
      return;

    var prevRange = ChromeVoxState.instance.currentRange;

    ChromeVoxState.instance.setCurrentRange(cursors.Range.fromNode(node));

    // Don't output if focused node hasn't changed.
    if (prevRange && evt.type == 'focus' &&
        ChromeVoxState.instance.currentRange.equals(prevRange))
      return;

    var output = new Output();
    output.withRichSpeech(
        ChromeVoxState.instance.currentRange, prevRange, evt.type);
    if (!this.textEditHandler_) {
      output.withBraille(
          ChromeVoxState.instance.currentRange, prevRange, evt.type);
    } else {
      // Delegate event handling to the text edit handler for braille.
      this.textEditHandler_.onEvent(evt);
    }
    output.go();
  },

  /**
   * @param {!AutomationEvent} evt
   */
  onEventIfInRange: function(evt) {
    if (!DesktopAutomationHandler.announceActions && evt.eventFrom == 'action')
      return;

    var prev = ChromeVoxState.instance.currentRange;
    if (!prev)
      return;

    if (prev.contentEquals(cursors.Range.fromNode(evt.target)) ||
        evt.target.state.focused) {
      // Intentionally skip setting range.
      new Output()
          .withRichSpeechAndBraille(
              cursors.Range.fromNode(evt.target), prev,
              Output.EventType.NAVIGATE)
          .go();
    }
  },

  /**
   * @param {!AutomationEvent} evt
   */
  onEventIfSelected: function(evt) {
    if (evt.target.state.selected)
      this.onEventDefault(evt);
  },

  /**
   * @param {!AutomationEvent} evt
   */
  onAriaAttributeChanged: function(evt) {
    if (evt.target.state.editable)
      return;
    this.onEventIfInRange(evt);
  },

  /**
   * Handles the result of a hit test.
   * @param {!AutomationNode} node The hit result.
   */
  onHitTestResult: function(node) {
    chrome.automation.getFocus(function(focus) {
      if (!focus && !node)
        return;

      focus = node || focus;
      var focusedRoot = AutomationUtil.getTopLevelRoot(focus);
      var output = new Output();
      if (focus != focusedRoot && focusedRoot)
        output.format('$name', focusedRoot);

      // Even though we usually don't output events from actions, hit test
      // results should generate output.
      var range = cursors.Range.fromNode(focus);
      ChromeVoxState.instance.setCurrentRange(range);
      output.withRichSpeechAndBraille(range, null, Output.EventType.NAVIGATE)
          .go();
    });
  },

  /**
   * @param {!AutomationEvent} evt
   */
  onHover: function(evt) {
    var target = evt.target;
    if (!AutomationPredicate.object(target)) {
      target = AutomationUtil.findNodePre(
                   target, Dir.FORWARD, AutomationPredicate.object) ||
          target;
    }
    if (ChromeVoxState.instance.currentRange &&
        target == ChromeVoxState.instance.currentRange.start.node)
      return;
    Output.forceModeForNextSpeechUtterance(cvox.QueueMode.FLUSH);
    this.onEventDefault(
        new CustomAutomationEvent(evt.type, target, evt.eventFrom));
  },

  /**
   * Handles active descendant changes.
   * @param {!AutomationEvent} evt
   */
  onActiveDescendantChanged: function(evt) {
    if (!evt.target.activeDescendant || !evt.target.state.focused)
      return;
    var prevRange = ChromeVoxState.instance.currentRange;
    var range = cursors.Range.fromNode(evt.target.activeDescendant);
    ChromeVoxState.instance.setCurrentRange(range);
    new Output()
        .withRichSpeechAndBraille(range, prevRange, Output.EventType.NAVIGATE)
        .go();
  },

  /**
   * Makes an announcement without changing focus.
   * @param {!AutomationEvent} evt
   */
  onAlert: function(evt) {
    var node = evt.target;
    var range = cursors.Range.fromNode(node);

    new Output()
        .withSpeechCategory(cvox.TtsCategory.LIVE)
        .withSpeechAndBraille(range, null, evt.type)
        .go();
  },

  onBlur: function(evt) {
    // Nullify focus if it no longer exists.
    chrome.automation.getFocus(function(focus) {
      if (!focus)
        ChromeVoxState.instance.setCurrentRange(null);
    });
  },

  /**
   * Provides all feedback once a checked state changed event fires.
   * @param {!AutomationEvent} evt
   */
  onCheckedStateChanged: function(evt) {
    if (!AutomationPredicate.checkable(evt.target))
      return;

    var event = new CustomAutomationEvent(
        EventType.CHECKED_STATE_CHANGED, evt.target, evt.eventFrom);
    this.onEventIfInRange(event);
  },

  /**
   * @param {!AutomationEvent} evt
   */
  onChildrenChanged: function(evt) {
    var curRange = ChromeVoxState.instance.currentRange;

    // Always refresh the braille contents.
    if (curRange && curRange.equals(cursors.Range.fromNode(evt.target))) {
      new Output()
          .withBraille(curRange, curRange, Output.EventType.NAVIGATE)
          .go();
    }
  },

  /**
   * Provides all feedback once a focus event fires.
   * @param {!AutomationEvent} evt
   */
  onFocus: function(evt) {
    if (evt.target.role == RoleType.ROOT_WEB_AREA) {
      chrome.automation.getFocus(
          this.maybeRecoverFocusAndOutput_.bind(this, evt));
      return;
    }

    // Invalidate any previous editable text handler state.
    if (!this.createTextEditHandlerIfNeeded_(evt.target, true))
      this.textEditHandler_ = null;

    var node = evt.target;

    // Discard focus events on embeddedObject and webView.
    if (node.role == RoleType.EMBEDDED_OBJECT || node.role == RoleType.WEB_VIEW)
      return;

    if (!node.root)
      return;

    var event = new CustomAutomationEvent(EventType.FOCUS, node, evt.eventFrom);
    this.onEventDefault(event);

    // Refresh the handler, if needed, now that ChromeVox focus is up to date.
    this.createTextEditHandlerIfNeeded_(node);
  },

  /**
   * Provides all feedback once a load complete event fires.
   * @param {!AutomationEvent} evt
   */
  onLoadComplete: function(evt) {
    // We are only interested in load completes on top level roots.
    if (AutomationUtil.getTopLevelRoot(evt.target) != evt.target.root)
      return;

    this.lastRootUrl_ = '';
    chrome.automation.getFocus(function(focus) {
      // In some situations, ancestor windows get focused before a descendant
      // webView/rootWebArea. In particular, a window that gets opened but no
      // inner focus gets set. We catch this generically by re-targetting focus
      // if focus is the ancestor of the load complete target (below).
      var focusIsAncestor = AutomationUtil.isDescendantOf(evt.target, focus);
      var focusIsDescendant = AutomationUtil.isDescendantOf(focus, evt.target);
      if (!focus || (!focusIsAncestor && !focusIsDescendant))
        return;

      if (focusIsAncestor) {
        focus = evt.target;
      }

      // Create text edit handler, if needed, now in order not to miss initial
      // value change if text field has already been focused when initializing
      // ChromeVox.
      this.createTextEditHandlerIfNeeded_(focus);

      // If auto read is set, skip focus recovery and start reading from the
      // top.
      if (localStorage['autoRead'] == 'true' &&
          AutomationUtil.getTopLevelRoot(evt.target) == evt.target) {
        ChromeVoxState.instance.setCurrentRange(
            cursors.Range.fromNode(evt.target));
        cvox.ChromeVox.tts.stop();
        CommandHandler.onCommand('readFromHere');
        return;
      }

      this.maybeRecoverFocusAndOutput_(evt, focus);
    }.bind(this));
  },

  /**
   * Updates the focus ring if the location of the current range, or
   * an ancestor of the current range, changes.
   * @param {!AutomationEvent} evt
   */
  onLocationChanged: function(evt) {
    var cur = ChromeVoxState.instance.currentRange;
    if (AutomationUtil.isDescendantOf(cur.start.node, evt.target) ||
        AutomationUtil.isDescendantOf(cur.end.node, evt.target)) {
      new Output().withLocation(cur, null, evt.type).go();
    }
  },

  /**
   * Provides all feedback once a change event in a text field fires.
   * @param {!AutomationEvent} evt
   * @private
   */
  onEditableChanged_: function(evt) {
    if (!this.createTextEditHandlerIfNeeded_(evt.target))
      return;

    if (!ChromeVoxState.instance.currentRange) {
      this.onEventDefault(evt);
      ChromeVoxState.instance.setCurrentRange(
          cursors.Range.fromNode(evt.target));
    }

    // Sync the ChromeVox range to the editable, if a selection exists.
    var anchorObject = evt.target.root.anchorObject;
    var anchorOffset = evt.target.root.anchorOffset || 0;
    var focusObject = evt.target.root.focusObject;
    var focusOffset = evt.target.root.focusOffset || 0;
    if (anchorObject && focusObject) {
      var selectedRange = new cursors.Range(
          new cursors.WrappingCursor(anchorObject, anchorOffset),
          new cursors.WrappingCursor(focusObject, focusOffset));

      // Sync ChromeVox range with selection.
      ChromeVoxState.instance.setCurrentRange(selectedRange);
    }
    this.textEditHandler_.onEvent(evt);
  },

  /**
   * Provides all feedback once a value changed event fires.
   * @param {!AutomationEvent} evt
   */
  onValueChanged: function(evt) {
    // Skip root web areas.
    if (evt.target.role == RoleType.ROOT_WEB_AREA)
      return;

    // Skip all unfocused text fields.
    if (!evt.target.state[StateType.FOCUSED] &&
        evt.target.state[StateType.EDITABLE])
      return;

    // Skip richly editables.
    if (evt.target.state[StateType.RICHLY_EDITABLE])
      return;

    // Delegate to the edit text handler if this is an editable.
    if (evt.target.state[StateType.EDITABLE]) {
      this.onEditableChanged_(evt);
      return;
    }

    var t = evt.target;
    var fromDesktop = t.root.role == RoleType.DESKTOP;
    if (t.state.focused || fromDesktop ||
        AutomationUtil.isDescendantOf(
            ChromeVoxState.instance.currentRange.start.node, t)) {
      if (new Date() - this.lastValueChanged_ <=
          DesktopAutomationHandler.VMIN_VALUE_CHANGE_DELAY_MS)
        return;

      this.lastValueChanged_ = new Date();

      var output = new Output();

      if (fromDesktop &&
          (!this.lastValueTarget_ || this.lastValueTarget_ !== t)) {
        output.withQueueMode(cvox.QueueMode.CATEGORY_FLUSH);
        var range = cursors.Range.fromNode(t);
        output.withRichSpeechAndBraille(
            range, range, Output.EventType.NAVIGATE);
        this.lastValueTarget_ = t;
      } else {
        output.format('$value', t);
      }
      output.go();
    }
  },

  /**
   * Handle updating the active indicator when the document scrolls.
   * @param {!AutomationEvent} evt
   */
  onScrollPositionChanged: function(evt) {
    var currentRange = ChromeVoxState.instance.currentRange;
    if (currentRange && currentRange.isValid())
      new Output().withLocation(currentRange, null, evt.type).go();
  },

  /**
   * @param {!AutomationEvent} evt
   */
  onSelection: function(evt) {
    // Invalidate any previous editable text handler state since some nodes,
    // like menuitems, can receive selection while focus remains on an editable
    // leading to braille output routing to the editable.
    this.textEditHandler_ = null;

    chrome.automation.getFocus(function(focus) {
      // Desktop tabs get "selection" when there's a focused webview during tab
      // switching.
      if (focus.role == RoleType.WEB_VIEW && evt.target.role == RoleType.TAB) {
        ChromeVoxState.instance.setCurrentRange(
            cursors.Range.fromNode(focus.firstChild));
        return;
      }

      // Some cases (e.g. in overview mode), require overriding the assumption
      // that focus is an ancestor of a selection target.
      var override = evt.target.role == RoleType.MENU_ITEM ||
          (evt.target.root == focus.root &&
           focus.root.role == RoleType.DESKTOP);
      if (override || AutomationUtil.isDescendantOf(evt.target, focus))
        this.onEventDefault(evt);
    }.bind(this));
  },

  /**
   * Provides all feedback once a menu start event fires.
   * @param {!AutomationEvent} evt
   */
  onMenuStart: function(evt) {
    ChromeVoxState.instance.markCurrentRange();
    this.onEventDefault(evt);
  },

  /**
   * Provides all feedback once a menu end event fires.
   * @param {!AutomationEvent} evt
   */
  onMenuEnd: function(evt) {
    this.onEventDefault(evt);

    // This is a work around for Chrome context menus not firing a focus event
    // after you close them.
    chrome.automation.getFocus(function(focus) {
      if (focus) {
        var event = new CustomAutomationEvent(EventType.FOCUS, focus, 'page');
        this.onFocus(event);
      }
    }.bind(this));
  },

  /**
   * Create an editable text handler for the given node if needed.
   * @param {!AutomationNode} node
   * @param {boolean=} opt_onFocus True if called within a focus event handler.
   *     False by default.
   * @return {boolean} True if the handler exists (created/already present).
   */
  createTextEditHandlerIfNeeded_: function(node, opt_onFocus) {
    if (!node.state.editable)
      return false;

    if (!ChromeVoxState.instance.currentRange ||
        !ChromeVoxState.instance.currentRange.start ||
        !ChromeVoxState.instance.currentRange.start.node)
      return false;

    var topRoot = AutomationUtil.getTopLevelRoot(node);
    if (!node.state.focused ||
        (topRoot && topRoot.parent && !topRoot.parent.state.focused))
      return false;

    // Re-target the node to the root of the editable.
    var target = node;
    target = AutomationUtil.getEditableRoot(target);
    var voxTarget = ChromeVoxState.instance.currentRange.start.node;
    voxTarget = AutomationUtil.getEditableRoot(voxTarget) || voxTarget;

    // It is possible that ChromeVox has range over some other node when a text
    // field is focused. Only allow this when focus is on a desktop node,
    // ChromeVox is over the keyboard, or during focus events.
    if (!target || !voxTarget ||
        (!opt_onFocus && target != voxTarget &&
         target.root.role != RoleType.DESKTOP &&
         voxTarget.root.role != RoleType.DESKTOP &&
         voxTarget.root.url.indexOf(DesktopAutomationHandler.KEYBOARD_URL) !=
             0))
      return false;

    if (target.restriction == chrome.automation.Restriction.READ_ONLY)
      return false;

    if (!this.textEditHandler_ || this.textEditHandler_.node !== target) {
      this.textEditHandler_ = editing.TextEditHandler.createForNode(target);
    }

    return !!this.textEditHandler_;
  },

  /**
   * @param {AutomationEvent} evt
   * @private
   */
  maybeRecoverFocusAndOutput_: function(evt, focus) {
    var focusedRoot = AutomationUtil.getTopLevelRoot(focus);
    if (!focusedRoot)
      return;

    var curRoot;
    if (ChromeVoxState.instance.currentRange) {
      curRoot = AutomationUtil.getTopLevelRoot(
          ChromeVoxState.instance.currentRange.start.node);
    }

    // If initial focus was already placed inside this page (e.g. if a user
    // starts tabbing before load complete), then don't move ChromeVox's
    // position on the page.
    if (curRoot && focusedRoot == curRoot &&
        this.lastRootUrl_ == focusedRoot.docUrl)
      return;

    this.lastRootUrl_ = focusedRoot.docUrl || '';
    var o = new Output();
    // Restore to previous position.
    var url = focusedRoot.docUrl;
    url = url.substring(0, url.indexOf('#')) || url;
    var pos = cvox.ChromeVox.position[url];

    // Disallow recovery for chrome urls.
    if (pos && url.indexOf('chrome://') != 0) {
      focusedRoot.hitTestWithReply(
          pos.x, pos.y, this.onHitTestResult.bind(this));
      return;
    }

    // This catches initial focus (i.e. on startup).
    if (!curRoot && focus != focusedRoot)
      o.format('$name', focusedRoot);

    ChromeVoxState.instance.setCurrentRange(cursors.Range.fromNode(focus));

    o.withRichSpeechAndBraille(
         ChromeVoxState.instance.currentRange, null, evt.type)
        .go();
  }
};

/**
 * Global instance.
 * @type {DesktopAutomationHandler}
 */
DesktopAutomationHandler.instance;

/**
 * Initializes global state for DesktopAutomationHandler.
 * @private
 */
DesktopAutomationHandler.init_ = function() {
  chrome.automation.getDesktop(function(desktop) {
    DesktopAutomationHandler.instance = new DesktopAutomationHandler(desktop);
  });
};

DesktopAutomationHandler.init_();

});  // goog.scope
