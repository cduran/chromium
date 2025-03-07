// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/arc/accessibility/ax_tree_source_arc.h"

#include <string>

#include "base/strings/stringprintf.h"
#include "chrome/browser/extensions/api/automation_internal/automation_event_router.h"
#include "chrome/browser/ui/aura/accessibility/automation_manager_aura.h"
#include "chrome/common/extensions/chrome_extension_messages.h"
#include "components/exo/wm_helper.h"
#include "ui/accessibility/ax_enum_util.h"
#include "ui/accessibility/platform/ax_android_constants.h"
#include "ui/aura/window.h"
#include "ui/views/focus/focus_manager.h"
#include "ui/views/view.h"
#include "ui/views/widget/widget.h"

namespace {

ax::mojom::Event ToAXEvent(arc::mojom::AccessibilityEventType arc_event_type) {
  switch (arc_event_type) {
    case arc::mojom::AccessibilityEventType::VIEW_FOCUSED:
    case arc::mojom::AccessibilityEventType::VIEW_ACCESSIBILITY_FOCUSED:
      return ax::mojom::Event::kFocus;
    case arc::mojom::AccessibilityEventType::VIEW_CLICKED:
    case arc::mojom::AccessibilityEventType::VIEW_LONG_CLICKED:
      return ax::mojom::Event::kClicked;
    case arc::mojom::AccessibilityEventType::VIEW_TEXT_CHANGED:
      return ax::mojom::Event::kTextChanged;
    case arc::mojom::AccessibilityEventType::VIEW_TEXT_SELECTION_CHANGED:
      return ax::mojom::Event::kTextSelectionChanged;
    case arc::mojom::AccessibilityEventType::WINDOW_STATE_CHANGED:
    case arc::mojom::AccessibilityEventType::NOTIFICATION_STATE_CHANGED:
    case arc::mojom::AccessibilityEventType::WINDOW_CONTENT_CHANGED:
    case arc::mojom::AccessibilityEventType::WINDOWS_CHANGED:
      return ax::mojom::Event::kLayoutComplete;
    case arc::mojom::AccessibilityEventType::VIEW_HOVER_ENTER:
      return ax::mojom::Event::kHover;
    case arc::mojom::AccessibilityEventType::ANNOUNCEMENT:
      return ax::mojom::Event::kAlert;
    case arc::mojom::AccessibilityEventType::VIEW_SCROLLED:
      return ax::mojom::Event::kScrollPositionChanged;
    case arc::mojom::AccessibilityEventType::VIEW_SELECTED:
    case arc::mojom::AccessibilityEventType::VIEW_HOVER_EXIT:
    case arc::mojom::AccessibilityEventType::TOUCH_EXPLORATION_GESTURE_START:
    case arc::mojom::AccessibilityEventType::TOUCH_EXPLORATION_GESTURE_END:
    case arc::mojom::AccessibilityEventType::
        VIEW_TEXT_TRAVERSED_AT_MOVEMENT_GRANULARITY:
    case arc::mojom::AccessibilityEventType::GESTURE_DETECTION_START:
    case arc::mojom::AccessibilityEventType::GESTURE_DETECTION_END:
    case arc::mojom::AccessibilityEventType::TOUCH_INTERACTION_START:
    case arc::mojom::AccessibilityEventType::TOUCH_INTERACTION_END:
    case arc::mojom::AccessibilityEventType::VIEW_CONTEXT_CLICKED:
    case arc::mojom::AccessibilityEventType::ASSIST_READING_CONTEXT:
      return ax::mojom::Event::kChildrenChanged;
    default:
      return ax::mojom::Event::kChildrenChanged;
  }
  return ax::mojom::Event::kChildrenChanged;
}

bool GetBooleanProperty(arc::mojom::AccessibilityNodeInfoData* node,
                        arc::mojom::AccessibilityBooleanProperty prop) {
  if (!node->boolean_properties)
    return false;

  auto it = node->boolean_properties->find(prop);
  if (it == node->boolean_properties->end())
    return false;

  return it->second;
}

bool GetIntProperty(arc::mojom::AccessibilityNodeInfoData* node,
                    arc::mojom::AccessibilityIntProperty prop,
                    int32_t* out_value) {
  if (!node->int_properties)
    return false;

  auto it = node->int_properties->find(prop);
  if (it == node->int_properties->end())
    return false;

  *out_value = it->second;
  return true;
}

bool GetStringProperty(arc::mojom::AccessibilityNodeInfoData* node,
                       arc::mojom::AccessibilityStringProperty prop,
                       std::string* out_value) {
  if (!node->string_properties)
    return false;

  auto it = node->string_properties->find(prop);
  if (it == node->string_properties->end())
    return false;

  *out_value = it->second;
  return true;
}

bool GetIntListProperty(arc::mojom::AccessibilityNodeInfoData* node,
                        arc::mojom::AccessibilityIntListProperty prop,
                        std::vector<int32_t>* out_value) {
  if (!node->int_list_properties)
    return false;

  auto it = node->int_list_properties->find(prop);
  if (it == node->int_list_properties->end())
    return false;

  *out_value = it->second;
  return true;
}

bool GetStringListProperty(arc::mojom::AccessibilityNodeInfoData* node,
                           arc::mojom::AccessibilityStringListProperty prop,
                           std::vector<std::string>* out_value) {
  if (!node->string_list_properties)
    return false;

  auto it = node->string_list_properties->find(prop);
  if (it == node->string_list_properties->end())
    return false;

  *out_value = it->second;
  return true;
}

bool HasCoveringSpan(arc::mojom::AccessibilityNodeInfoData* data,
                     arc::mojom::AccessibilityStringProperty prop,
                     arc::mojom::SpanType span_type) {
  if (!data->spannable_string_properties)
    return false;

  std::string text;
  GetStringProperty(data, prop, &text);
  if (text.empty())
    return false;

  auto span_entries_it = data->spannable_string_properties->find(prop);
  if (span_entries_it == data->spannable_string_properties->end())
    return false;

  for (size_t i = 0; i < span_entries_it->second.size(); ++i) {
    if (span_entries_it->second[i]->span_type != span_type)
      continue;

    size_t span_size =
        span_entries_it->second[i]->end - span_entries_it->second[i]->start;
    if (span_size == text.size())
      return true;
  }
  return false;
}

void PopulateAXRole(arc::mojom::AccessibilityNodeInfoData* node,
                    ui::AXNodeData* out_data) {
  std::string class_name;
  if (GetStringProperty(node,
                        arc::mojom::AccessibilityStringProperty::CLASS_NAME,
                        &class_name)) {
    out_data->AddStringAttribute(ax::mojom::StringAttribute::kClassName,
                                 class_name);
  }

  if (GetBooleanProperty(node,
                         arc::mojom::AccessibilityBooleanProperty::EDITABLE)) {
    out_data->role = ax::mojom::Role::kTextField;
    return;
  }

  if (HasCoveringSpan(node, arc::mojom::AccessibilityStringProperty::TEXT,
                      arc::mojom::SpanType::URL) ||
      HasCoveringSpan(
          node, arc::mojom::AccessibilityStringProperty::CONTENT_DESCRIPTION,
          arc::mojom::SpanType::URL)) {
    out_data->role = ax::mojom::Role::kLink;
    return;
  }

  arc::mojom::AccessibilityCollectionItemInfoData* collection_item_info =
      node->collection_item_info.get();
  if (collection_item_info) {
    if (collection_item_info->is_heading) {
      out_data->role = ax::mojom::Role::kHeading;
    } else {
      out_data->role = ax::mojom::Role::kListItem;
      out_data->AddIntAttribute(ax::mojom::IntAttribute::kPosInSet,
                                collection_item_info->row_index);
    }
    return;
  }

  std::string chrome_role;
  if (GetStringProperty(node,
                        arc::mojom::AccessibilityStringProperty::CHROME_ROLE,
                        &chrome_role)) {
    ax::mojom::Role role_value = ui::ParseRole(chrome_role.c_str());
    if (role_value != ax::mojom::Role::kNone) {
      // The webView and rootWebArea roles differ between Android and Chrome. In
      // particular, Android includes far fewer attributes which leads to
      // undesirable behavior. Exclude their direct mapping.
      out_data->role = (role_value != ax::mojom::Role::kWebView &&
                        role_value != ax::mojom::Role::kRootWebArea)
                           ? role_value
                           : ax::mojom::Role::kGenericContainer;
      return;
    }
  }

#define MAP_ROLE(android_class_name, chrome_role) \
  if (class_name == android_class_name) {         \
    out_data->role = chrome_role;                 \
    return;                                       \
  }

  // These mappings were taken from accessibility utils (Android -> Chrome) and
  // BrowserAccessibilityAndroid. They do not completely match the above two
  // sources.
  MAP_ROLE(ui::kAXAbsListViewClassname, ax::mojom::Role::kList);
  MAP_ROLE(ui::kAXButtonClassname, ax::mojom::Role::kButton);
  MAP_ROLE(ui::kAXCheckBoxClassname, ax::mojom::Role::kCheckBox);
  MAP_ROLE(ui::kAXCheckedTextViewClassname, ax::mojom::Role::kStaticText);
  MAP_ROLE(ui::kAXCompoundButtonClassname, ax::mojom::Role::kCheckBox);
  MAP_ROLE(ui::kAXDialogClassname, ax::mojom::Role::kDialog);
  MAP_ROLE(ui::kAXEditTextClassname, ax::mojom::Role::kTextField);
  MAP_ROLE(ui::kAXGridViewClassname, ax::mojom::Role::kTable);
  MAP_ROLE(ui::kAXImageClassname, ax::mojom::Role::kImage);
  MAP_ROLE(ui::kAXImageButtonClassname, ax::mojom::Role::kButton);
  if (GetBooleanProperty(node,
                         arc::mojom::AccessibilityBooleanProperty::CLICKABLE)) {
    MAP_ROLE(ui::kAXImageViewClassname, ax::mojom::Role::kButton);
  } else {
    MAP_ROLE(ui::kAXImageViewClassname, ax::mojom::Role::kImage);
  }
  MAP_ROLE(ui::kAXListViewClassname, ax::mojom::Role::kList);
  MAP_ROLE(ui::kAXMenuItemClassname, ax::mojom::Role::kMenuItem);
  MAP_ROLE(ui::kAXPagerClassname, ax::mojom::Role::kGroup);
  MAP_ROLE(ui::kAXProgressBarClassname, ax::mojom::Role::kProgressIndicator);
  MAP_ROLE(ui::kAXRadioButtonClassname, ax::mojom::Role::kRadioButton);
  MAP_ROLE(ui::kAXSeekBarClassname, ax::mojom::Role::kSlider);
  MAP_ROLE(ui::kAXSpinnerClassname, ax::mojom::Role::kPopUpButton);
  MAP_ROLE(ui::kAXSwitchClassname, ax::mojom::Role::kSwitch);
  MAP_ROLE(ui::kAXTabWidgetClassname, ax::mojom::Role::kTabList);
  MAP_ROLE(ui::kAXToggleButtonClassname, ax::mojom::Role::kToggleButton);
  MAP_ROLE(ui::kAXViewClassname, ax::mojom::Role::kGenericContainer);
  MAP_ROLE(ui::kAXViewGroupClassname, ax::mojom::Role::kGroup);

#undef MAP_ROLE

  std::string text;
  GetStringProperty(node, arc::mojom::AccessibilityStringProperty::TEXT, &text);
  if (!text.empty())
    out_data->role = ax::mojom::Role::kStaticText;
  else
    out_data->role = ax::mojom::Role::kGenericContainer;
}

void PopulateAXState(arc::mojom::AccessibilityNodeInfoData* node,
                     ui::AXNodeData* out_data) {
#define MAP_STATE(android_boolean_property, chrome_state) \
  if (GetBooleanProperty(node, android_boolean_property)) \
    out_data->AddState(chrome_state);

  using AXBooleanProperty = arc::mojom::AccessibilityBooleanProperty;

  // These mappings were taken from accessibility utils (Android -> Chrome) and
  // BrowserAccessibilityAndroid. They do not completely match the above two
  // sources.
  MAP_STATE(AXBooleanProperty::EDITABLE, ax::mojom::State::kEditable);
  MAP_STATE(AXBooleanProperty::FOCUSABLE, ax::mojom::State::kFocusable);
  MAP_STATE(AXBooleanProperty::MULTI_LINE, ax::mojom::State::kMultiline);
  MAP_STATE(AXBooleanProperty::PASSWORD, ax::mojom::State::kProtected);
  MAP_STATE(AXBooleanProperty::SELECTED, ax::mojom::State::kSelected);

#undef MAP_STATE

  if (GetBooleanProperty(node, AXBooleanProperty::CHECKABLE)) {
    const bool is_checked =
        GetBooleanProperty(node, AXBooleanProperty::CHECKED);
    out_data->SetCheckedState(is_checked ? ax::mojom::CheckedState::kTrue
                                         : ax::mojom::CheckedState::kFalse);
  }

  if (!GetBooleanProperty(node, AXBooleanProperty::ENABLED)) {
    out_data->SetRestriction(ax::mojom::Restriction::kDisabled);
  }

  if (!GetBooleanProperty(node, AXBooleanProperty::VISIBLE_TO_USER)) {
    out_data->AddState(ax::mojom::State::kInvisible);
  }
}

}  // namespace

namespace arc {

// This class keeps focus on a |ShellSurface| without interfering with default
// focus management in |ShellSurface|. For example, touch causes the
// |ShellSurface| to lose focus to its ancestor containing View.
class AXTreeSourceArc::FocusStealer : public views::View {
 public:
  explicit FocusStealer(int32_t id) : id_(id) { set_owned_by_client(); }

  void Steal() {
    SetFocusBehavior(views::View::FocusBehavior::ALWAYS);
    RequestFocus();
  }

  // views::View overrides.
  void GetAccessibleNodeData(ui::AXNodeData* node_data) override {
    node_data->AddIntAttribute(ax::mojom::IntAttribute::kChildTreeId, id_);
    node_data->role = ax::mojom::Role::kClient;
  }

 private:
  const int32_t id_;
  DISALLOW_COPY_AND_ASSIGN(FocusStealer);
};

AXTreeSourceArc::AXTreeSourceArc(Delegate* delegate)
    : current_tree_serializer_(new AXTreeArcSerializer(this)),
      root_id_(-1),
      window_id_(-1),
      focused_node_id_(-1),
      is_notification_(false),
      delegate_(delegate),
      focus_stealer_(new FocusStealer(tree_id())) {}

AXTreeSourceArc::~AXTreeSourceArc() {
  Reset();
}

void AXTreeSourceArc::NotifyAccessibilityEvent(
    mojom::AccessibilityEventData* event_data) {
  tree_map_.clear();
  parent_map_.clear();
  cached_computed_bounds_.clear();
  root_id_ = -1;

  window_id_ = event_data->window_id;
  is_notification_ = event_data->notification_key.has_value();

  // The following loops perform caching to prepare for AXTreeSerializer.
  // First, we need to cache parent links, which are implied by a node's child
  // ids.
  // Next, we cache the nodes by id. During this process, we can detect the root
  // node based upon the parent links we cached above.
  // Finally, we cache each node's computed bounds, based on its descendants.
  for (size_t i = 0; i < event_data->node_data.size(); ++i) {
    if (!event_data->node_data[i]->int_list_properties)
      continue;
    auto it = event_data->node_data[i]->int_list_properties->find(
        arc::mojom::AccessibilityIntListProperty::CHILD_NODE_IDS);
    if (it != event_data->node_data[i]->int_list_properties->end()) {
      for (size_t j = 0; j < it->second.size(); ++j)
        parent_map_[it->second[j]] = event_data->node_data[i]->id;
    }
  }

  for (size_t i = 0; i < event_data->node_data.size(); ++i) {
    int32_t id = event_data->node_data[i]->id;
    mojom::AccessibilityNodeInfoData* node = event_data->node_data[i].get();
    tree_map_[id] = node;
    if (parent_map_.find(id) == parent_map_.end()) {
      CHECK_EQ(-1, root_id_) << "Duplicated root";
      root_id_ = id;
    }

    if (GetBooleanProperty(node,
                           arc::mojom::AccessibilityBooleanProperty::FOCUSED)) {
      focused_node_id_ = id;
    }
  }

  // Assuming |nodeData| is in pre-order, compute cached bounds in post-order to
  // avoid an O(n^2) amount of work as the computed bounds uses descendant
  // bounds.
  for (int i = event_data->node_data.size() - 1; i >= 0; --i) {
    mojom::AccessibilityNodeInfoData* node = event_data->node_data[i].get();
    cached_computed_bounds_[node] = ComputeEnclosingBounds(node);
  }

  ExtensionMsg_AccessibilityEventParams params;
  params.event_type = ToAXEvent(event_data->event_type);

  params.tree_id = tree_id();
  params.id = event_data->source_id;

  ui::AXTreeUpdate update;

  if (event_data->event_type ==
      arc::mojom::AccessibilityEventType::WINDOW_CONTENT_CHANGED) {
    current_tree_serializer_->DeleteClientSubtree(
        GetFromId(event_data->source_id));
  }

  current_tree_serializer_->SerializeChanges(GetFromId(event_data->source_id),
                                             &params.update);

  extensions::AutomationEventRouter* router =
      extensions::AutomationEventRouter::GetInstance();
  router->DispatchAccessibilityEvent(params);
}

void AXTreeSourceArc::NotifyActionResult(const ui::AXActionData& data,
                                         bool result) {
  extensions::AutomationEventRouter::GetInstance()->DispatchActionResult(
      data, result);
}

void AXTreeSourceArc::Focus(aura::Window* window) {
  if (focus_stealer_->HasFocus())
    return;

  views::Widget* widget = views::Widget::GetWidgetForNativeView(window);
  if (!widget || !widget->GetContentsView())
    return;

  views::View* view = widget->GetContentsView();
  if (!view->Contains(focus_stealer_.get()))
    view->AddChildView(focus_stealer_.get());
  focus_stealer_->Steal();
}

bool AXTreeSourceArc::GetTreeData(ui::AXTreeData* data) const {
  data->tree_id = tree_id();
  if (focused_node_id_ >= 0)
    data->focus_id = focused_node_id_;
  else if (root_id_ >= 0)
    data->focus_id = root_id_;
  return true;
}

mojom::AccessibilityNodeInfoData* AXTreeSourceArc::GetRoot() const {
  mojom::AccessibilityNodeInfoData* root = GetFromId(root_id_);
  return root;
}

mojom::AccessibilityNodeInfoData* AXTreeSourceArc::GetFromId(int32_t id) const {
  auto it = tree_map_.find(id);
  if (it == tree_map_.end())
    return nullptr;
  return it->second;
}

int32_t AXTreeSourceArc::GetId(mojom::AccessibilityNodeInfoData* node) const {
  if (!node)
    return -1;
  return node->id;
}

void AXTreeSourceArc::GetChildren(
    mojom::AccessibilityNodeInfoData* node,
    std::vector<mojom::AccessibilityNodeInfoData*>* out_children) const {
  if (!node || !node->int_list_properties)
    return;

  auto it = node->int_list_properties->find(
      arc::mojom::AccessibilityIntListProperty::CHILD_NODE_IDS);
  if (it == node->int_list_properties->end())
    return;

  for (size_t i = 0; i < it->second.size(); ++i)
    out_children->push_back(GetFromId(it->second[i]));

  // Sort children based on their enclosing bounding rectangles, based on their
  // descendants.
  std::sort(out_children->begin(), out_children->end(),
            [this](auto left, auto right) {
              auto left_bounds = ComputeEnclosingBounds(left);
              auto right_bounds = ComputeEnclosingBounds(right);

              // Top to bottom sort (non-overlapping).
              if (!left_bounds.Intersects(right_bounds))
                return left_bounds.y() < right_bounds.y();

              // Overlapping
              // Left to right.
              int left_difference = left_bounds.x() - right_bounds.x();
              if (left_difference != 0)
                return left_difference < 0;

              // Top to bottom.
              int top_difference = left_bounds.y() - right_bounds.y();
              if (top_difference != 0)
                return top_difference < 0;

              // Larger to smaller.
              int height_difference =
                  left_bounds.height() - right_bounds.height();
              if (height_difference != 0)
                return height_difference > 0;

              int width_difference = left_bounds.width() - right_bounds.width();
              if (width_difference != 0)
                return width_difference > 0;

              // The rects are equal.
              return false;
            });
}

mojom::AccessibilityNodeInfoData* AXTreeSourceArc::GetParent(
    mojom::AccessibilityNodeInfoData* node) const {
  if (!node)
    return nullptr;
  auto it = parent_map_.find(node->id);
  if (it != parent_map_.end())
    return GetFromId(it->second);
  return nullptr;
}

bool AXTreeSourceArc::IsValid(mojom::AccessibilityNodeInfoData* node) const {
  return node;
}

bool AXTreeSourceArc::IsEqual(mojom::AccessibilityNodeInfoData* node1,
                              mojom::AccessibilityNodeInfoData* node2) const {
  if (!node1 || !node2)
    return false;
  return node1->id == node2->id;
}

mojom::AccessibilityNodeInfoData* AXTreeSourceArc::GetNull() const {
  return nullptr;
}

void AXTreeSourceArc::SerializeNode(mojom::AccessibilityNodeInfoData* node,
                                    ui::AXNodeData* out_data) const {
  if (!node)
    return;

  int32_t id = node->id;
  out_data->id = id;
  if (id == root_id_)
    out_data->role = ax::mojom::Role::kRootWebArea;
  else
    PopulateAXRole(node, out_data);

  using AXIntListProperty = arc::mojom::AccessibilityIntListProperty;
  using AXIntProperty = arc::mojom::AccessibilityIntProperty;
  using AXStringListProperty = arc::mojom::AccessibilityStringListProperty;
  using AXStringProperty = arc::mojom::AccessibilityStringProperty;

  // String properties.
  int labelled_by = -1;

  // Accessible name computation picks the first non-empty string from content
  // description, text, or labelled by text.
  std::string name;
  bool has_name =
      GetStringProperty(node, AXStringProperty::CONTENT_DESCRIPTION, &name);
  if (name.empty())
    has_name |= GetStringProperty(node, AXStringProperty::TEXT, &name);
  if (name.empty() &&
      GetIntProperty(node, arc::mojom::AccessibilityIntProperty::LABELED_BY,
                     &labelled_by)) {
    mojom::AccessibilityNodeInfoData* labelled_by_node = GetFromId(labelled_by);
    if (labelled_by_node) {
      ui::AXNodeData labelled_by_data;
      SerializeNode(labelled_by_node, &labelled_by_data);
      has_name |= labelled_by_data.GetStringAttribute(
          ax::mojom::StringAttribute::kName, &name);
    }
  }

  if (has_name) {
    if (out_data->role == ax::mojom::Role::kTextField)
      out_data->AddStringAttribute(ax::mojom::StringAttribute::kValue, name);
    else
      out_data->SetName(name);
  }

  std::string role_description;
  if (GetStringProperty(node, AXStringProperty::ROLE_DESCRIPTION,
                        &role_description)) {
    out_data->AddStringAttribute(ax::mojom::StringAttribute::kRoleDescription,
                                 role_description);
  }

  if (out_data->role == ax::mojom::Role::kRootWebArea) {
    std::string package_name;
    if (GetStringProperty(node, AXStringProperty::PACKAGE_NAME,
                          &package_name)) {
      const std::string& url =
          base::StringPrintf("%s/%d", package_name.c_str(), tree_id());
      out_data->AddStringAttribute(ax::mojom::StringAttribute::kUrl, url);
    }
  }

  // Int properties.
  int traversal_before = -1, traversal_after = -1;
  if (GetIntProperty(node,
                     arc::mojom::AccessibilityIntProperty::TRAVERSAL_BEFORE,
                     &traversal_before)) {
    out_data->AddIntAttribute(ax::mojom::IntAttribute::kPreviousFocusId,
                              traversal_before);
  }

  if (GetIntProperty(node,
                     arc::mojom::AccessibilityIntProperty::TRAVERSAL_AFTER,
                     &traversal_after)) {
    out_data->AddIntAttribute(ax::mojom::IntAttribute::kNextFocusId,
                              traversal_after);
  }

  // Boolean properties.
  PopulateAXState(node, out_data);
  if (GetBooleanProperty(
          node, arc::mojom::AccessibilityBooleanProperty::SCROLLABLE)) {
    out_data->AddBoolAttribute(ax::mojom::BoolAttribute::kScrollable, true);
  }
  if (GetBooleanProperty(node,
                         arc::mojom::AccessibilityBooleanProperty::CLICKABLE)) {
    out_data->AddBoolAttribute(ax::mojom::BoolAttribute::kClickable, true);
  }

  // Range info.
  arc::mojom::AccessibilityRangeInfoData* range_info = node->range_info.get();
  if (range_info) {
    out_data->AddFloatAttribute(ax::mojom::FloatAttribute::kValueForRange,
                                range_info->current);
    out_data->AddFloatAttribute(ax::mojom::FloatAttribute::kMinValueForRange,
                                range_info->min);
    out_data->AddFloatAttribute(ax::mojom::FloatAttribute::kMaxValueForRange,
                                range_info->max);
  }

  exo::WMHelper* wm_helper =
      exo::WMHelper::HasInstance() ? exo::WMHelper::GetInstance() : nullptr;

  // To get bounds of a node which can be passed to AXNodeData.location,
  // - Root node must exist.
  // - Window where this tree is attached to need to be focused.
  if (root_id_ != -1 && wm_helper) {
    aura::Window* focused_window =
        is_notification_ ? nullptr : wm_helper->GetFocusedWindow();
    const gfx::Rect& local_bounds = GetBounds(node, focused_window);
    out_data->location.SetRect(local_bounds.x(), local_bounds.y(),
                               local_bounds.width(), local_bounds.height());
  }

  // Integer properties.
  int32_t val;
  if (GetIntProperty(node, AXIntProperty::TEXT_SELECTION_START, &val) &&
      val >= 0)
    out_data->AddIntAttribute(ax::mojom::IntAttribute::kTextSelStart, val);

  if (GetIntProperty(node, AXIntProperty::TEXT_SELECTION_END, &val) && val >= 0)
    out_data->AddIntAttribute(ax::mojom::IntAttribute::kTextSelEnd, val);

  // Custom actions.
  std::vector<int32_t> custom_action_ids;
  if (GetIntListProperty(node, AXIntListProperty::CUSTOM_ACTION_IDS,
                         &custom_action_ids)) {
    std::vector<std::string> custom_action_descriptions;

    CHECK(GetStringListProperty(
        node, AXStringListProperty::CUSTOM_ACTION_DESCRIPTIONS,
        &custom_action_descriptions));
    CHECK(!custom_action_ids.empty());
    CHECK_EQ(custom_action_ids.size(), custom_action_descriptions.size());

    out_data->AddAction(ax::mojom::Action::kCustomAction);
    out_data->AddIntListAttribute(ax::mojom::IntListAttribute::kCustomActionIds,
                                  custom_action_ids);
    out_data->AddStringListAttribute(
        ax::mojom::StringListAttribute::kCustomActionDescriptions,
        custom_action_descriptions);
  }
}

const gfx::Rect AXTreeSourceArc::GetBounds(
    mojom::AccessibilityNodeInfoData* node,
    aura::Window* focused_window) const {
  DCHECK_NE(root_id_, -1);

  gfx::Rect node_bounds = node->bounds_in_screen;

  if (focused_window && node->id == root_id_) {
    // Top level window returns its bounds in dip.
    aura::Window* toplevel_window = focused_window->GetToplevelWindow();
    float scale = toplevel_window->layer()->device_scale_factor();

    // Bounds of root node is relative to its container, i.e. focused window.
    node_bounds.Offset(
        static_cast<int>(-1.0f * scale *
                         static_cast<float>(toplevel_window->bounds().x())),
        static_cast<int>(-1.0f * scale *
                         static_cast<float>(toplevel_window->bounds().y())));

    return node_bounds;
  }

  // Bounds of non-root node is relative to its tree's root.
  gfx::Rect root_bounds = GetFromId(root_id_)->bounds_in_screen;
  node_bounds.Offset(-1 * root_bounds.x(), -1 * root_bounds.y());
  return node_bounds;
}

gfx::Rect AXTreeSourceArc::ComputeEnclosingBounds(
    mojom::AccessibilityNodeInfoData* node) const {
  gfx::Rect computed_bounds;
  ComputeEnclosingBoundsInternal(node, computed_bounds);
  return computed_bounds.IsEmpty() ? node->bounds_in_screen : computed_bounds;
}

void AXTreeSourceArc::ComputeEnclosingBoundsInternal(
    mojom::AccessibilityNodeInfoData* node,
    gfx::Rect& computed_bounds) const {
  auto cached_bounds = cached_computed_bounds_.find(node);
  if (cached_bounds != cached_computed_bounds_.end()) {
    computed_bounds.Union(cached_bounds->second);
    return;
  }

  // Only consider nodes that can possibly be accessibility focused. In Chrome,
  // this amounts to nodes with a non-generic container role.
  ui::AXNodeData data;
  PopulateAXRole(node, &data);
  const gfx::Rect& bounds = node->bounds_in_screen;
  if (data.role != ax::mojom::Role::kGenericContainer &&
      data.role != ax::mojom::Role::kGroup && !bounds.IsEmpty() &&
      GetBooleanProperty(
          node, arc::mojom::AccessibilityBooleanProperty::VISIBLE_TO_USER)) {
    computed_bounds.Union(bounds);
    return;
  }

  if (!node->int_list_properties)
    return;

  auto it = node->int_list_properties->find(
      arc::mojom::AccessibilityIntListProperty::CHILD_NODE_IDS);
  if (it == node->int_list_properties->end())
    return;

  for (size_t i = 0; i < it->second.size(); ++i) {
    ComputeEnclosingBoundsInternal(GetFromId(it->second[i]), computed_bounds);
  }

  return;
}

void AXTreeSourceArc::PerformAction(const ui::AXActionData& data) {
  delegate_->OnAction(data);
}

void AXTreeSourceArc::Reset() {
  tree_map_.clear();
  parent_map_.clear();
  cached_computed_bounds_.clear();
  current_tree_serializer_.reset(new AXTreeArcSerializer(this));
  root_id_ = -1;
  focused_node_id_ = -1;
  if (focus_stealer_->parent()) {
    views::View* parent = focus_stealer_->parent();
    parent->RemoveChildView(focus_stealer_.get());
    parent->NotifyAccessibilityEvent(ax::mojom::Event::kChildrenChanged, false);
  }
  focus_stealer_.reset();
  extensions::AutomationEventRouter* router =
      extensions::AutomationEventRouter::GetInstance();
  if (!router)
    return;

  router->DispatchTreeDestroyedEvent(tree_id(), nullptr);
}

}  // namespace arc
