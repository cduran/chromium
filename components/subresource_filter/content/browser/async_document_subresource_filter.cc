// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/subresource_filter/content/browser/async_document_subresource_filter.h"

#include <utility>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/task_runner_util.h"
#include "base/threading/sequenced_task_runner_handle.h"
#include "components/subresource_filter/core/common/memory_mapped_ruleset.h"
#include "components/subresource_filter/core/common/scoped_timers.h"
#include "components/subresource_filter/core/common/time_measurements.h"
#include "components/url_pattern_index/proto/rules.pb.h"

namespace subresource_filter {

ActivationState ComputeActivationState(
    const GURL& document_url,
    const url::Origin& parent_document_origin,
    const ActivationState& parent_activation_state,
    const MemoryMappedRuleset* ruleset) {
  DCHECK(ruleset);

  SCOPED_UMA_HISTOGRAM_MICRO_TIMER(
      "SubresourceFilter.DocumentLoad.Activation.WallDuration");
  SCOPED_UMA_HISTOGRAM_MICRO_THREAD_TIMER(
      "SubresourceFilter.DocumentLoad.Activation.CPUDuration");

  auto page_wall_duration_timer = ScopedTimers::StartIf(
      parent_document_origin.unique(), [](base::TimeDelta delta) {
        UMA_HISTOGRAM_MICRO_TIMES(
            "SubresourceFilter.PageLoad.Activation.WallDuration", delta);
      });
  auto page_cpu_duration_timer = ScopedThreadTimers::StartIf(
      parent_document_origin.unique(), [](base::TimeDelta delta) {
        UMA_HISTOGRAM_MICRO_TIMES(
            "SubresourceFilter.PageLoad.Activation.CPUDuration", delta);
      });

  IndexedRulesetMatcher matcher(ruleset->data(), ruleset->length());
  ActivationState activation_state = parent_activation_state;
  if (activation_state.filtering_disabled_for_document)
    return activation_state;

  // TODO(pkalinnikov): Match several activation types in a batch.
  if (matcher.ShouldDisableFilteringForDocument(
          document_url, parent_document_origin,
          url_pattern_index::proto::ACTIVATION_TYPE_DOCUMENT)) {
    activation_state.filtering_disabled_for_document = true;
  } else if (!activation_state.generic_blocking_rules_disabled &&
             matcher.ShouldDisableFilteringForDocument(
                 document_url, parent_document_origin,
                 url_pattern_index::proto::ACTIVATION_TYPE_GENERICBLOCK)) {
    activation_state.generic_blocking_rules_disabled = true;
  }
  return activation_state;
}

// AsyncDocumentSubresourceFilter::InitializationParams ------------------------

using InitializationParams =
    AsyncDocumentSubresourceFilter::InitializationParams;

InitializationParams::InitializationParams() = default;

InitializationParams::InitializationParams(GURL document_url,
                                           ActivationLevel activation_level,
                                           bool measure_performance)
    : document_url(std::move(document_url)),
      parent_activation_state(activation_level) {
  DCHECK_NE(ActivationLevel::DISABLED, activation_level);
  parent_activation_state.measure_performance = measure_performance;
}

InitializationParams::InitializationParams(
    GURL document_url,
    url::Origin parent_document_origin,
    ActivationState parent_activation_state)
    : document_url(std::move(document_url)),
      parent_document_origin(std::move(parent_document_origin)),
      parent_activation_state(parent_activation_state) {
  DCHECK_NE(ActivationLevel::DISABLED,
            parent_activation_state.activation_level);
}

InitializationParams::~InitializationParams() = default;
InitializationParams::InitializationParams(InitializationParams&&) = default;
InitializationParams& InitializationParams::operator=(InitializationParams&&) =
    default;

// AsyncDocumentSubresourceFilter ----------------------------------------------

AsyncDocumentSubresourceFilter::AsyncDocumentSubresourceFilter(
    VerifiedRuleset::Handle* ruleset_handle,
    InitializationParams params,
    base::Callback<void(ActivationState)> activation_state_callback)
    : task_runner_(ruleset_handle->task_runner()),
      core_(new Core(), base::OnTaskRunnerDeleter(task_runner_)),
      weak_ptr_factory_(this) {
  DCHECK_NE(ActivationLevel::DISABLED,
            params.parent_activation_state.activation_level);

  // Note: It is safe to post |ruleset_handle|'s VerifiedRuleset pointer,
  // because a task to delete it can only be posted to (and, therefore,
  // processed by) |task_runner| after this method returns, hence after the
  // below task is posted.
  base::PostTaskAndReplyWithResult(
      task_runner_, FROM_HERE,
      base::BindOnce(&Core::Initialize, base::Unretained(core_.get()),
                     std::move(params), ruleset_handle->ruleset_.get()),
      base::BindOnce(&AsyncDocumentSubresourceFilter::OnActivateStateCalculated,
                     weak_ptr_factory_.GetWeakPtr(),
                     std::move(activation_state_callback)));
}

AsyncDocumentSubresourceFilter::~AsyncDocumentSubresourceFilter() {
  DCHECK(sequence_checker_.CalledOnValidSequence());
}

void AsyncDocumentSubresourceFilter::OnActivateStateCalculated(
    base::Callback<void(ActivationState)> activation_state_callback,
    ActivationState activation_state) {
  activation_state_ = activation_state;
  activation_state_callback.Run(activation_state);
}

void AsyncDocumentSubresourceFilter::GetLoadPolicyForSubdocument(
    const GURL& subdocument_url,
    LoadPolicyCallback result_callback) {
  DCHECK(sequence_checker_.CalledOnValidSequence());

  // TODO(pkalinnikov): Think about avoiding copy of |subdocument_url| if it is
  // too big and won't be allowed anyway (e.g., it's a data: URI).
  base::PostTaskAndReplyWithResult(
      task_runner_, FROM_HERE,
      base::Bind(
          [](AsyncDocumentSubresourceFilter::Core* core,
             const GURL& subdocument_url) {
            DCHECK(core);
            DocumentSubresourceFilter* filter = core->filter();
            return filter
                       ? filter->GetLoadPolicy(
                             subdocument_url,
                             url_pattern_index::proto::ELEMENT_TYPE_SUBDOCUMENT)
                       : LoadPolicy::ALLOW;
          },
          core_.get(), subdocument_url),
      std::move(result_callback));
}

void AsyncDocumentSubresourceFilter::ReportDisallowedLoad() {
  if (!first_disallowed_load_callback_.is_null())
    std::move(first_disallowed_load_callback_).Run();
}

const ActivationState& AsyncDocumentSubresourceFilter::activation_state()
    const {
  CHECK(activation_state_);
  return activation_state_.value();
}

// AsyncDocumentSubresourceFilter::Core ----------------------------------------

AsyncDocumentSubresourceFilter::Core::Core() {
  sequence_checker_.DetachFromSequence();
}

AsyncDocumentSubresourceFilter::Core::~Core() {
  DCHECK(sequence_checker_.CalledOnValidSequence());
}

ActivationState AsyncDocumentSubresourceFilter::Core::Initialize(
    InitializationParams params,
    VerifiedRuleset* verified_ruleset) {
  DCHECK(sequence_checker_.CalledOnValidSequence());
  DCHECK(verified_ruleset);

  if (!verified_ruleset->Get())
    return ActivationState(ActivationLevel::DISABLED);

  ActivationState activation_state = ComputeActivationState(
      params.document_url, params.parent_document_origin,
      params.parent_activation_state, verified_ruleset->Get());

  DCHECK_NE(ActivationLevel::DISABLED, activation_state.activation_level);
  filter_.emplace(url::Origin::Create(params.document_url), activation_state,
                  verified_ruleset->Get());

  return activation_state;
}

}  // namespace subresource_filter
