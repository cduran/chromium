// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "ash/app_list/model/search/search_model.h"

#include <map>
#include <string>
#include <utility>

namespace app_list {

namespace {

void UpdateResult(SearchResult* source, SearchResult* target) {
  target->set_display_type(source->display_type());
  target->set_title(source->title());
  target->set_title_tags(source->title_tags());
  target->set_details(source->details());
  target->set_details_tags(source->details_tags());
}

}  // namespace

SearchModel::SearchModel()
    : search_box_(std::make_unique<SearchBoxModel>()),
      results_(std::make_unique<SearchResults>()) {}

SearchModel::~SearchModel() {}

void SearchModel::SetTabletMode(bool started) {
  is_tablet_mode_ = started;
  search_box_->SetTabletMode(started);
}

std::vector<SearchResult*> SearchModel::FilterSearchResultsByDisplayType(
    SearchResults* results,
    SearchResult::DisplayType display_type,
    size_t max_results) {
  std::vector<SearchResult*> matches;
  for (size_t i = 0; i < results->item_count(); ++i) {
    SearchResult* item = results->GetItemAt(i);
    if (item->display_type() == display_type) {
      matches.push_back(item);
      if (matches.size() == max_results)
        break;
    }
  }
  return matches;
}

void SearchModel::PublishResults(
    std::vector<std::unique_ptr<SearchResult>> new_results) {
  // The following algorithm is used:
  // 1. Transform the |results_| list into an unordered map from result ID
  // to item.
  // 2. Use the order of items in |new_results| to build an ordered list. If the
  // result IDs exist in the map, update and use the item in the map and delete
  // it from the map afterwards. Otherwise, clone new items from |new_results|.
  // 3. Delete the objects remaining in the map as they are unused.

  // We have to erase all results at once so that observers are notified with
  // meaningful indexes.
  auto current_results = results_->RemoveAll();
  std::map<std::string, std::unique_ptr<SearchResult>> results_map;
  for (auto& ui_result : current_results)
    results_map[ui_result->id()] = std::move(ui_result);

  // Add items back to |results_| in the order of |new_results|.
  for (auto&& new_result : new_results) {
    auto ui_result_it = results_map.find(new_result->id());
    if (ui_result_it != results_map.end() &&
        new_result->answer_card_contents_token() ==
            ui_result_it->second->answer_card_contents_token()) {
      // Update and use the old result if it exists.
      std::unique_ptr<SearchResult> ui_result = std::move(ui_result_it->second);
      UpdateResult(new_result.get(), ui_result.get());
      ui_result->set_relevance(new_result->relevance());

      results_->Add(std::move(ui_result));

      // Remove the item from the map so that it ends up only with unused
      // results.
      results_map.erase(ui_result_it);
    } else {
      // Copy the result from |new_results| otherwise.
      results_->Add(std::move(new_result));
    }
  }

  // Any remaining results in |results_map| will be automatically deleted.
}

SearchResult* SearchModel::FindSearchResult(const std::string& id) {
  for (const auto& result : *results_) {
    if (result->id() == id)
      return result.get();
  }
  return nullptr;
}

void SearchModel::DeleteAllResults() {
  PublishResults(std::vector<std::unique_ptr<SearchResult>>());
}

}  // namespace app_list
