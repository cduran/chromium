// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/http_cache_transaction.h"

#include "build/build_config.h"  // For OS_POSIX

#if defined(OS_POSIX)
#include <unistd.h>
#endif

#include <algorithm>
#include <string>

#include "base/auto_reset.h"
#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback_helpers.h"
#include "base/compiler_specific.h"
#include "base/location.h"
#include "base/macros.h"
#include "base/metrics/histogram_functions.h"
#include "base/metrics/histogram_macros.h"
#include "base/single_thread_task_runner.h"
#include "base/strings/string_number_conversions.h"  // For HexEncode.
#include "base/strings/string_util.h"  // For LowerCaseEqualsASCII.
#include "base/threading/thread_task_runner_handle.h"
#include "base/time/clock.h"
#include "base/trace_event/trace_event.h"
#include "net/base/auth.h"
#include "net/base/load_flags.h"
#include "net/base/load_timing_info.h"
#include "net/base/trace_constants.h"
#include "net/base/upload_data_stream.h"
#include "net/cert/cert_status_flags.h"
#include "net/cert/x509_certificate.h"
#include "net/disk_cache/disk_cache.h"
#include "net/http/http_cache_writers.h"
#include "net/http/http_network_session.h"
#include "net/http/http_request_info.h"
#include "net/http/http_util.h"
#include "net/log/net_log_event_type.h"
#include "net/ssl/ssl_cert_request_info.h"
#include "net/ssl/ssl_config_service.h"

using base::Time;
using base::TimeDelta;
using base::TimeTicks;

namespace net {

using CacheEntryStatus = HttpResponseInfo::CacheEntryStatus;

namespace {

// From http://tools.ietf.org/html/draft-ietf-httpbis-p6-cache-21#section-6
//      a "non-error response" is one with a 2xx (Successful) or 3xx
//      (Redirection) status code.
bool NonErrorResponse(int status_code) {
  int status_code_range = status_code / 100;
  return status_code_range == 2 || status_code_range == 3;
}

void RecordNoStoreHeaderHistogram(int load_flags,
                                  const HttpResponseInfo* response) {
  if (load_flags & LOAD_MAIN_FRAME_DEPRECATED) {
    UMA_HISTOGRAM_BOOLEAN(
        "Net.MainFrameNoStore",
        response->headers->HasHeaderValue("cache-control", "no-store"));
  }
}

}  // namespace

#define CACHE_STATUS_HISTOGRAMS(type)                                        \
  do {                                                                       \
    UMA_HISTOGRAM_ENUMERATION("HttpCache.Pattern" type, cache_entry_status_, \
                              CacheEntryStatus::ENTRY_MAX);                  \
    if (validation_request) {                                                \
      UMA_HISTOGRAM_ENUMERATION("HttpCache.ValidationCause" type,            \
                                validation_cause_, VALIDATION_CAUSE_MAX);    \
    }                                                                        \
    if (stale_request) {                                                     \
      UMA_HISTOGRAM_COUNTS_1M(                                               \
          "HttpCache.StaleEntry.FreshnessPeriodsSinceLastUsed" type,         \
          freshness_periods_since_last_used);                                \
    }                                                                        \
  } while (0)

struct HeaderNameAndValue {
  const char* name;
  const char* value;
};

// If the request includes one of these request headers, then avoid caching
// to avoid getting confused.
static const HeaderNameAndValue kPassThroughHeaders[] = {
  { "if-unmodified-since", NULL },  // causes unexpected 412s
  { "if-match", NULL },             // causes unexpected 412s
  { "if-range", NULL },
  { NULL, NULL }
};

struct ValidationHeaderInfo {
  const char* request_header_name;
  const char* related_response_header_name;
};

static const ValidationHeaderInfo kValidationHeaders[] = {
  { "if-modified-since", "last-modified" },
  { "if-none-match", "etag" },
};

// If the request includes one of these request headers, then avoid reusing
// our cached copy if any.
static const HeaderNameAndValue kForceFetchHeaders[] = {
  { "cache-control", "no-cache" },
  { "pragma", "no-cache" },
  { NULL, NULL }
};

// If the request includes one of these request headers, then force our
// cached copy (if any) to be revalidated before reusing it.
static const HeaderNameAndValue kForceValidateHeaders[] = {
  { "cache-control", "max-age=0" },
  { NULL, NULL }
};

static bool HeaderMatches(const HttpRequestHeaders& headers,
                          const HeaderNameAndValue* search) {
  for (; search->name; ++search) {
    std::string header_value;
    if (!headers.GetHeader(search->name, &header_value))
      continue;

    if (!search->value)
      return true;

    HttpUtil::ValuesIterator v(header_value.begin(), header_value.end(), ',');
    while (v.GetNext()) {
      if (base::LowerCaseEqualsASCII(
              base::StringPiece(v.value_begin(), v.value_end()), search->value))
        return true;
    }
  }
  return false;
}

//-----------------------------------------------------------------------------

HttpCache::Transaction::Transaction(RequestPriority priority, HttpCache* cache)
    : next_state_(STATE_NONE),
      initial_request_(nullptr),
      request_(NULL),
      priority_(priority),
      cache_(cache->GetWeakPtr()),
      entry_(NULL),
      new_entry_(NULL),
      new_response_(NULL),
      mode_(NONE),
      reading_(false),
      invalid_range_(false),
      truncated_(false),
      is_sparse_(false),
      range_requested_(false),
      handling_206_(false),
      cache_pending_(false),
      done_headers_create_new_entry_(false),
      vary_mismatch_(false),
      couldnt_conditionalize_request_(false),
      bypass_lock_for_test_(false),
      bypass_lock_after_headers_for_test_(false),
      fail_conditionalization_for_test_(false),
      io_buf_len_(0),
      read_offset_(0),
      effective_load_flags_(0),
      shared_writing_error_(OK),
      cache_entry_status_(CacheEntryStatus::ENTRY_UNDEFINED),
      validation_cause_(VALIDATION_CAUSE_UNDEFINED),
      cant_conditionalize_zero_freshness_from_memhint_(false),
      recorded_histograms_(false),
      parallel_writing_pattern_(PARALLEL_WRITING_NONE),
      moved_network_transaction_to_writers_(false),
      websocket_handshake_stream_base_create_helper_(NULL),
      in_do_loop_(false),
      weak_factory_(this) {
  TRACE_EVENT0("io", "HttpCacheTransaction::Transaction");
  static_assert(HttpCache::Transaction::kNumValidationHeaders ==
                    arraysize(kValidationHeaders),
                "invalid number of validation headers");

  io_callback_ = base::Bind(&Transaction::OnIOComplete,
                              weak_factory_.GetWeakPtr());
}

HttpCache::Transaction::~Transaction() {
  TRACE_EVENT0("io", "HttpCacheTransaction::~Transaction");
  // We may have to issue another IO, but we should never invoke the callback_
  // after this point.
  callback_.Reset();

  if (cache_) {
    if (entry_) {
      DoneWithEntry(false /* entry_is_complete */);
    } else if (cache_pending_) {
      cache_->RemovePendingTransaction(this);
    }
  }
}

HttpCache::Transaction::Mode HttpCache::Transaction::mode() const {
  return mode_;
}

int HttpCache::Transaction::WriteMetadata(IOBuffer* buf, int buf_len,
                                          const CompletionCallback& callback) {
  DCHECK(buf);
  DCHECK_GT(buf_len, 0);
  DCHECK(!callback.is_null());
  if (!cache_.get() || !entry_)
    return ERR_UNEXPECTED;

  // We don't need to track this operation for anything.
  // It could be possible to check if there is something already written and
  // avoid writing again (it should be the same, right?), but let's allow the
  // caller to "update" the contents with something new.
  return entry_->disk_entry->WriteData(kMetadataIndex, 0, buf, buf_len,
                                       callback, true);
}

LoadState HttpCache::Transaction::GetWriterLoadState() const {
  const HttpTransaction* transaction = network_transaction();
  if (transaction)
    return transaction->GetLoadState();
  if (entry_ || !request_)
    return LOAD_STATE_IDLE;
  return LOAD_STATE_WAITING_FOR_CACHE;
}

const NetLogWithSource& HttpCache::Transaction::net_log() const {
  return net_log_;
}

int HttpCache::Transaction::Start(const HttpRequestInfo* request,
                                  const CompletionCallback& callback,
                                  const NetLogWithSource& net_log) {
  DCHECK(request);
  DCHECK(!callback.is_null());

  // Ensure that we only have one asynchronous call at a time.
  DCHECK(callback_.is_null());
  DCHECK(!reading_);
  DCHECK(!network_trans_.get());
  DCHECK(!entry_);
  DCHECK_EQ(next_state_, STATE_NONE);

  if (!cache_.get())
    return ERR_UNEXPECTED;

  initial_request_ = request;
  SetRequest(net_log);

  // We have to wait until the backend is initialized so we start the SM.
  next_state_ = STATE_GET_BACKEND;
  int rv = DoLoop(OK);

  // Setting this here allows us to check for the existence of a callback_ to
  // determine if we are still inside Start.
  if (rv == ERR_IO_PENDING)
    callback_ = callback;

  return rv;
}

int HttpCache::Transaction::RestartIgnoringLastError(
    const CompletionCallback& callback) {
  DCHECK(!callback.is_null());

  // Ensure that we only have one asynchronous call at a time.
  DCHECK(callback_.is_null());

  if (!cache_.get())
    return ERR_UNEXPECTED;

  int rv = RestartNetworkRequest();

  if (rv == ERR_IO_PENDING)
    callback_ = callback;

  return rv;
}

int HttpCache::Transaction::RestartWithCertificate(
    scoped_refptr<X509Certificate> client_cert,
    scoped_refptr<SSLPrivateKey> client_private_key,
    const CompletionCallback& callback) {
  DCHECK(!callback.is_null());

  // Ensure that we only have one asynchronous call at a time.
  DCHECK(callback_.is_null());

  if (!cache_.get())
    return ERR_UNEXPECTED;

  int rv = RestartNetworkRequestWithCertificate(std::move(client_cert),
                                                std::move(client_private_key));

  if (rv == ERR_IO_PENDING)
    callback_ = callback;

  return rv;
}

int HttpCache::Transaction::RestartWithAuth(
    const AuthCredentials& credentials,
    const CompletionCallback& callback) {
  DCHECK(auth_response_.headers.get());
  DCHECK(!callback.is_null());

  // Ensure that we only have one asynchronous call at a time.
  DCHECK(callback_.is_null());

  if (!cache_.get())
    return ERR_UNEXPECTED;

  // Clear the intermediate response since we are going to start over.
  SetAuthResponse(HttpResponseInfo());

  int rv = RestartNetworkRequestWithAuth(credentials);

  if (rv == ERR_IO_PENDING)
    callback_ = callback;

  return rv;
}

bool HttpCache::Transaction::IsReadyToRestartForAuth() {
  if (!network_trans_.get())
    return false;
  return network_trans_->IsReadyToRestartForAuth();
}

int HttpCache::Transaction::Read(IOBuffer* buf, int buf_len,
                                 const CompletionCallback& callback) {
  DCHECK_EQ(next_state_, STATE_NONE);
  DCHECK(buf);
  DCHECK_GT(buf_len, 0);
  DCHECK(!callback.is_null());

  DCHECK(callback_.is_null());

  if (!cache_.get())
    return ERR_UNEXPECTED;

  // If we have an intermediate auth response at this point, then it means the
  // user wishes to read the network response (the error page).  If there is a
  // previous response in the cache then we should leave it intact.
  if (auth_response_.headers.get() && mode_ != NONE) {
    UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_OTHER);
    DCHECK(mode_ & WRITE);
    bool stopped = StopCachingImpl(mode_ == READ_WRITE);
    DCHECK(stopped);
  }

  reading_ = true;
  read_buf_ = buf;
  io_buf_len_ = buf_len;
  int rv = TransitionToReadingState();
  if (rv != OK || next_state_ == STATE_NONE)
    return rv;

  rv = DoLoop(OK);

  if (rv == ERR_IO_PENDING) {
    DCHECK(callback_.is_null());
    callback_ = callback;
  }
  return rv;
}

int HttpCache::Transaction::TransitionToReadingState() {
  if (!entry_) {
    if (network_trans_) {
      // This can happen when the request should be handled exclusively by
      // the network layer (skipping the cache entirely using
      // LOAD_DISABLE_CACHE) or there was an error during the headers phase
      // due to which the transaction cannot write to the cache or the consumer
      // is reading the auth response from the network.
      // TODO(http://crbug.com/740947) to get rid of this state in future.
      next_state_ = STATE_NETWORK_READ;
      return OK;
    }

    // If there is no network, and no cache entry, then there is nothing to read
    // from.
    next_state_ = STATE_NONE;

    // An error state should be set for the next read, else this transaction
    // should have been terminated once it reached this state. To assert we
    // could dcheck that shared_writing_error_ is set to a valid error value but
    // in some specific conditions (http://crbug.com/806344) it's possible that
    // the consumer does an extra Read in which case the assert will fail.
    return shared_writing_error_;
  }

  // If entry_ is present, the transaction is either a member of entry_->writers
  // or readers.
  if (!InWriters()) {
    // Since transaction is not a writer and we are in Read(), it must be a
    // reader.
    DCHECK(entry_->TransactionInReaders(this));
    DCHECK(mode_ == READ || (mode_ == READ_WRITE && partial_));
    next_state_ = STATE_CACHE_READ_DATA;
    return OK;
  }

  DCHECK(mode_ & WRITE || mode_ == NONE);

  // If it's a writer and it is partial then it may need to read from the cache
  // or from the network based on whether network transaction is present or not.
  if (partial_) {
    if (entry_->writers->network_transaction())
      next_state_ = STATE_NETWORK_READ_CACHE_WRITE;
    else
      next_state_ = STATE_CACHE_READ_DATA;
    return OK;
  }

  // Full request.
  // If it's a writer and a full request then it may read from the cache if its
  // offset is behind the current offset else from the network.
  int disk_entry_size = entry_->disk_entry->GetDataSize(kResponseContentIndex);
  if (read_offset_ == disk_entry_size || entry_->writers->network_read_only()) {
    next_state_ = STATE_NETWORK_READ_CACHE_WRITE;
  } else {
    DCHECK_LT(read_offset_, disk_entry_size);
    next_state_ = STATE_CACHE_READ_DATA;
  }
  return OK;
}

void HttpCache::Transaction::StopCaching() {
  // We really don't know where we are now. Hopefully there is no operation in
  // progress, but nothing really prevents this method to be called after we
  // returned ERR_IO_PENDING. We cannot attempt to truncate the entry at this
  // point because we need the state machine for that (and even if we are really
  // free, that would be an asynchronous operation). In other words, keep the
  // entry how it is (it will be marked as truncated at destruction), and let
  // the next piece of code that executes know that we are now reading directly
  // from the net.
  if (cache_.get() && (mode_ & WRITE) && !is_sparse_ && !range_requested_ &&
      network_transaction()) {
    StopCachingImpl(false);
  }
}

bool HttpCache::Transaction::GetFullRequestHeaders(
    HttpRequestHeaders* headers) const {
  const HttpTransaction* transaction = GetOwnedOrMovedNetworkTransaction();
  if (transaction) {
    return transaction->GetFullRequestHeaders(headers);
  } else if (!network_transaction_info_.full_request_headers.IsEmpty()) {
    *headers = network_transaction_info_.full_request_headers;
    return true;
  }

  // TODO(juliatuttle): Read headers from cache.
  return false;
}

int64_t HttpCache::Transaction::GetTotalReceivedBytes() const {
  int64_t total_received_bytes = network_transaction_info_.total_received_bytes;
  const HttpTransaction* transaction = GetOwnedOrMovedNetworkTransaction();
  if (transaction)
    total_received_bytes += transaction->GetTotalReceivedBytes();
  return total_received_bytes;
}

int64_t HttpCache::Transaction::GetTotalSentBytes() const {
  int64_t total_sent_bytes = network_transaction_info_.total_sent_bytes;
  const HttpTransaction* transaction = GetOwnedOrMovedNetworkTransaction();
  if (transaction)
    total_sent_bytes += transaction->GetTotalSentBytes();
  return total_sent_bytes;
}

void HttpCache::Transaction::DoneReading() {
  if (cache_.get() && entry_) {
    DCHECK_NE(mode_, UPDATE);
    DoneWithEntry(true);
  }
}

const HttpResponseInfo* HttpCache::Transaction::GetResponseInfo() const {
  // Null headers means we encountered an error or haven't a response yet
  if (auth_response_.headers.get()) {
    DCHECK_EQ(cache_entry_status_, auth_response_.cache_entry_status)
        << "These must be in sync via SetResponse and SetAuthResponse.";
    return &auth_response_;
  }
  DCHECK_EQ(cache_entry_status_, response_.cache_entry_status)
      << "These must be in sync via SetResponse and SetAuthResponse.";
  return &response_;
}

LoadState HttpCache::Transaction::GetLoadState() const {
  LoadState state = GetWriterLoadState();
  if (state != LOAD_STATE_WAITING_FOR_CACHE)
    return state;

  if (cache_.get())
    return cache_->GetLoadStateForPendingTransaction(this);

  return LOAD_STATE_IDLE;
}

void HttpCache::Transaction::SetQuicServerInfo(
    QuicServerInfo* quic_server_info) {}

bool HttpCache::Transaction::GetLoadTimingInfo(
    LoadTimingInfo* load_timing_info) const {
  const HttpTransaction* transaction = GetOwnedOrMovedNetworkTransaction();
  if (transaction)
    return transaction->GetLoadTimingInfo(load_timing_info);

  if (network_transaction_info_.old_network_trans_load_timing) {
    *load_timing_info =
        *network_transaction_info_.old_network_trans_load_timing;
    return true;
  }

  if (first_cache_access_since_.is_null())
    return false;

  // If the cache entry was opened, return that time.
  load_timing_info->send_start = first_cache_access_since_;
  // This time doesn't make much sense when reading from the cache, so just use
  // the same time as send_start.
  load_timing_info->send_end = first_cache_access_since_;
  return true;
}

bool HttpCache::Transaction::GetRemoteEndpoint(IPEndPoint* endpoint) const {
  const HttpTransaction* transaction = GetOwnedOrMovedNetworkTransaction();
  if (transaction)
    return transaction->GetRemoteEndpoint(endpoint);

  if (!network_transaction_info_.old_remote_endpoint.address().empty()) {
    *endpoint = network_transaction_info_.old_remote_endpoint;
    return true;
  }

  return false;
}

void HttpCache::Transaction::PopulateNetErrorDetails(
    NetErrorDetails* details) const {
  const HttpTransaction* transaction = GetOwnedOrMovedNetworkTransaction();
  if (transaction)
    return transaction->PopulateNetErrorDetails(details);
  return;
}

void HttpCache::Transaction::SetPriority(RequestPriority priority) {
  priority_ = priority;

  if (network_trans_)
    network_trans_->SetPriority(priority_);

  if (InWriters()) {
    DCHECK(!network_trans_ || partial_);
    entry_->writers->UpdatePriority();
  }
}

void HttpCache::Transaction::SetWebSocketHandshakeStreamCreateHelper(
    WebSocketHandshakeStreamBase::CreateHelper* create_helper) {
  websocket_handshake_stream_base_create_helper_ = create_helper;

  // TODO(shivanisha). Since this function must be invoked before Start() as
  // per the API header, a network transaction should not exist at that point.
  HttpTransaction* transaction = network_transaction();
  if (transaction)
    transaction->SetWebSocketHandshakeStreamCreateHelper(create_helper);
}

void HttpCache::Transaction::SetBeforeNetworkStartCallback(
    const BeforeNetworkStartCallback& callback) {
  DCHECK(!network_trans_);
  before_network_start_callback_ = callback;
}

void HttpCache::Transaction::SetBeforeHeadersSentCallback(
    const BeforeHeadersSentCallback& callback) {
  DCHECK(!network_trans_);
  before_headers_sent_callback_ = callback;
}

void HttpCache::Transaction::SetRequestHeadersCallback(
    RequestHeadersCallback callback) {
  DCHECK(!network_trans_);
  request_headers_callback_ = std::move(callback);
}

void HttpCache::Transaction::SetResponseHeadersCallback(
    ResponseHeadersCallback callback) {
  DCHECK(!network_trans_);
  response_headers_callback_ = std::move(callback);
}

int HttpCache::Transaction::ResumeNetworkStart() {
  if (network_trans_)
    return network_trans_->ResumeNetworkStart();
  return ERR_UNEXPECTED;
}

void HttpCache::Transaction::GetConnectionAttempts(
    ConnectionAttempts* out) const {
  ConnectionAttempts new_connection_attempts;
  const HttpTransaction* transaction = GetOwnedOrMovedNetworkTransaction();
  if (transaction)
    transaction->GetConnectionAttempts(&new_connection_attempts);

  out->swap(new_connection_attempts);
  out->insert(out->begin(),
              network_transaction_info_.old_connection_attempts.begin(),
              network_transaction_info_.old_connection_attempts.end());
}

void HttpCache::Transaction::SetValidatingCannotProceed() {
  DCHECK(!reading_);
  // Ensure this transaction is waiting for a callback.
  DCHECK_NE(STATE_UNSET, next_state_);

  next_state_ = STATE_HEADERS_PHASE_CANNOT_PROCEED;
  entry_ = nullptr;
}

size_t HttpCache::Transaction::EstimateMemoryUsage() const {
  // TODO(xunjieli): Consider improving the coverage. crbug.com/669108.
  return 0;
}

void HttpCache::Transaction::WriterAboutToBeRemovedFromEntry(int result) {
  RecordHistograms();

  // Since the transaction can no longer access the network transaction, save
  // all network related info now.
  if (moved_network_transaction_to_writers_ &&
      entry_->writers->network_transaction()) {
    SaveNetworkTransactionInfo(*(entry_->writers->network_transaction()));
  }

  entry_ = nullptr;
  mode_ = NONE;

  // Transactions in the midst of a Read call through writers will get any error
  // code through the IO callback but for idle transactions/transactions reading
  // from the cache, the error for a future Read must be stored here.
  if (result < 0)
    shared_writing_error_ = result;
}

void HttpCache::Transaction::WriteModeTransactionAboutToBecomeReader() {
  mode_ = READ;
  if (moved_network_transaction_to_writers_ &&
      entry_->writers->network_transaction()) {
    SaveNetworkTransactionInfo(*(entry_->writers->network_transaction()));
  }
}

void HttpCache::Transaction::MaybeSetParallelWritingPatternForMetrics(
    HttpCache::ParallelWritingPattern pattern) {
  // It's possible a transaction could not join existing writers and then
  // creates a new writers. In that case the original reason for not being able
  // to join writers should be logged.
  if (parallel_writing_pattern_ == PARALLEL_WRITING_NONE)
    parallel_writing_pattern_ = pattern;
}

//-----------------------------------------------------------------------------

// A few common patterns: (Foo* means Foo -> FooComplete)
//
// 1. Not-cached entry:
//   Start():
//   GetBackend* -> InitEntry -> OpenEntry* -> CreateEntry* -> AddToEntry* ->
//   SendRequest* -> SuccessfulSendRequest -> OverwriteCachedResponse ->
//   CacheWriteResponse* -> TruncateCachedData* -> TruncateCachedMetadata* ->
//   PartialHeadersReceived -> FinishHeaders*
//
//   Read():
//   NetworkReadCacheWrite*/CacheReadData* (if other writers are also writing to
//   the cache)
//
// 2. Cached entry, no validation:
//   Start():
//   GetBackend* -> InitEntry -> OpenEntry* -> AddToEntry* -> CacheReadResponse*
//   -> CacheDispatchValidation -> BeginPartialCacheValidation() ->
//   BeginCacheValidation() -> SetupEntryForRead() -> FinishHeaders*
//
//   Read():
//   CacheReadData*
//
// 3. Cached entry, validation (304):
//   Start():
//   GetBackend* -> InitEntry -> OpenEntry* -> AddToEntry* -> CacheReadResponse*
//   -> CacheDispatchValidation -> BeginPartialCacheValidation() ->
//   BeginCacheValidation() -> SendRequest* -> SuccessfulSendRequest ->
//   UpdateCachedResponse -> CacheWriteUpdatedResponse* ->
//   UpdateCachedResponseComplete -> OverwriteCachedResponse ->
//   PartialHeadersReceived -> FinishHeaders*
//
//   Read():
//   CacheReadData*
//
// 4. Cached entry, validation and replace (200):
//   Start():
//   GetBackend* -> InitEntry -> OpenEntry* -> AddToEntry* -> CacheReadResponse*
//   -> CacheDispatchValidation -> BeginPartialCacheValidation() ->
//   BeginCacheValidation() -> SendRequest* -> SuccessfulSendRequest ->
//   OverwriteCachedResponse -> CacheWriteResponse* -> DoTruncateCachedData* ->
//   TruncateCachedMetadata* -> PartialHeadersReceived -> FinishHeaders*
//
//   Read():
//   NetworkReadCacheWrite*/CacheReadData* (if other writers are also writing to
//   the cache)
//
// 5. Sparse entry, partially cached, byte range request:
//   Start():
//   GetBackend* -> InitEntry -> OpenEntry* -> AddToEntry* -> CacheReadResponse*
//   -> CacheDispatchValidation -> BeginPartialCacheValidation() ->
//   CacheQueryData* -> ValidateEntryHeadersAndContinue() ->
//   StartPartialCacheValidation -> CompletePartialCacheValidation ->
//   BeginCacheValidation() -> SendRequest* -> SuccessfulSendRequest ->
//   UpdateCachedResponse -> CacheWriteUpdatedResponse* ->
//   UpdateCachedResponseComplete -> OverwriteCachedResponse ->
//   PartialHeadersReceived -> FinishHeaders*
//
//   Read() 1:
//   NetworkReadCacheWrite*
//
//   Read() 2:
//   NetworkReadCacheWrite* -> StartPartialCacheValidation ->
//   CompletePartialCacheValidation -> CacheReadData* ->
//
//   Read() 3:
//   CacheReadData* -> StartPartialCacheValidation ->
//   CompletePartialCacheValidation -> BeginCacheValidation() -> SendRequest* ->
//   SuccessfulSendRequest -> UpdateCachedResponse* -> OverwriteCachedResponse
//   -> PartialHeadersReceived -> NetworkReadCacheWrite*
//
// 6. HEAD. Not-cached entry:
//   Pass through. Don't save a HEAD by itself.
//   Start():
//   GetBackend* -> InitEntry -> OpenEntry* -> SendRequest*
//
// 7. HEAD. Cached entry, no validation:
//   Start():
//   The same flow as for a GET request (example #2)
//
//   Read():
//   CacheReadData (returns 0)
//
// 8. HEAD. Cached entry, validation (304):
//   The request updates the stored headers.
//   Start(): Same as for a GET request (example #3)
//
//   Read():
//   CacheReadData (returns 0)
//
// 9. HEAD. Cached entry, validation and replace (200):
//   Pass through. The request dooms the old entry, as a HEAD won't be stored by
//   itself.
//   Start():
//   GetBackend* -> InitEntry -> OpenEntry* -> AddToEntry* -> CacheReadResponse*
//   -> CacheDispatchValidation -> BeginPartialCacheValidation() ->
//   BeginCacheValidation() -> SendRequest* -> SuccessfulSendRequest ->
//   OverwriteCachedResponse -> FinishHeaders*
//
// 10. HEAD. Sparse entry, partially cached:
//   Serve the request from the cache, as long as it doesn't require
//   revalidation. Ignore missing ranges when deciding to revalidate. If the
//   entry requires revalidation, ignore the whole request and go to full pass
//   through (the result of the HEAD request will NOT update the entry).
//
//   Start(): Basically the same as example 7, as we never create a partial_
//   object for this request.
//
// 11. Prefetch, not-cached entry:
//   The same as example 1. The "unused_since_prefetch" bit is stored as true in
//   UpdateCachedResponse.
//
// 12. Prefetch, cached entry:
//   Like examples 2-4, only CacheToggleUnusedSincePrefetch* is inserted between
//   CacheReadResponse* and CacheDispatchValidation if the unused_since_prefetch
//   bit is unset.
//
// 13. Cached entry less than 5 minutes old, unused_since_prefetch is true:
//   Skip validation, similar to example 2.
//   GetBackend* -> InitEntry -> OpenEntry* -> AddToEntry* -> CacheReadResponse*
//   -> CacheToggleUnusedSincePrefetch* -> CacheDispatchValidation ->
//   BeginPartialCacheValidation() -> BeginCacheValidation() ->
//   SetupEntryForRead() -> FinishHeaders*
//
//   Read():
//   CacheReadData*
//
// 14. Cached entry more than 5 minutes old, unused_since_prefetch is true:
//   Like examples 2-4, only CacheToggleUnusedSincePrefetch* is inserted between
//   CacheReadResponse* and CacheDispatchValidation.
int HttpCache::Transaction::DoLoop(int result) {
  DCHECK_NE(STATE_UNSET, next_state_);
  DCHECK_NE(STATE_NONE, next_state_);
  DCHECK(!in_do_loop_);

  int rv = result;
  State state = next_state_;
  do {
    state = next_state_;
    next_state_ = STATE_UNSET;
    base::AutoReset<bool> scoped_in_do_loop(&in_do_loop_, true);

    switch (state) {
      case STATE_GET_BACKEND:
        DCHECK_EQ(OK, rv);
        rv = DoGetBackend();
        break;
      case STATE_GET_BACKEND_COMPLETE:
        rv = DoGetBackendComplete(rv);
        break;
      case STATE_INIT_ENTRY:
        DCHECK_EQ(OK, rv);
        rv = DoInitEntry();
        break;
      case STATE_OPEN_ENTRY:
        DCHECK_EQ(OK, rv);
        rv = DoOpenEntry();
        break;
      case STATE_OPEN_ENTRY_COMPLETE:
        rv = DoOpenEntryComplete(rv);
        break;
      case STATE_DOOM_ENTRY:
        DCHECK_EQ(OK, rv);
        rv = DoDoomEntry();
        break;
      case STATE_DOOM_ENTRY_COMPLETE:
        rv = DoDoomEntryComplete(rv);
        break;
      case STATE_CREATE_ENTRY:
        DCHECK_EQ(OK, rv);
        rv = DoCreateEntry();
        break;
      case STATE_CREATE_ENTRY_COMPLETE:
        rv = DoCreateEntryComplete(rv);
        break;
      case STATE_ADD_TO_ENTRY:
        DCHECK_EQ(OK, rv);
        rv = DoAddToEntry();
        break;
      case STATE_ADD_TO_ENTRY_COMPLETE:
        rv = DoAddToEntryComplete(rv);
        break;
      case STATE_DONE_HEADERS_ADD_TO_ENTRY_COMPLETE:
        rv = DoDoneHeadersAddToEntryComplete(rv);
        break;
      case STATE_CACHE_READ_RESPONSE:
        DCHECK_EQ(OK, rv);
        rv = DoCacheReadResponse();
        break;
      case STATE_CACHE_READ_RESPONSE_COMPLETE:
        rv = DoCacheReadResponseComplete(rv);
        break;
      case STATE_TOGGLE_UNUSED_SINCE_PREFETCH:
        DCHECK_EQ(OK, rv);
        rv = DoCacheToggleUnusedSincePrefetch();
        break;
      case STATE_TOGGLE_UNUSED_SINCE_PREFETCH_COMPLETE:
        rv = DoCacheToggleUnusedSincePrefetchComplete(rv);
        break;
      case STATE_CACHE_DISPATCH_VALIDATION:
        DCHECK_EQ(OK, rv);
        rv = DoCacheDispatchValidation();
        break;
      case STATE_CACHE_QUERY_DATA:
        DCHECK_EQ(OK, rv);
        rv = DoCacheQueryData();
        break;
      case STATE_CACHE_QUERY_DATA_COMPLETE:
        rv = DoCacheQueryDataComplete(rv);
        break;
      case STATE_START_PARTIAL_CACHE_VALIDATION:
        DCHECK_EQ(OK, rv);
        rv = DoStartPartialCacheValidation();
        break;
      case STATE_COMPLETE_PARTIAL_CACHE_VALIDATION:
        rv = DoCompletePartialCacheValidation(rv);
        break;
      case STATE_SEND_REQUEST:
        DCHECK_EQ(OK, rv);
        rv = DoSendRequest();
        break;
      case STATE_SEND_REQUEST_COMPLETE:
        rv = DoSendRequestComplete(rv);
        break;
      case STATE_SUCCESSFUL_SEND_REQUEST:
        DCHECK_EQ(OK, rv);
        rv = DoSuccessfulSendRequest();
        break;
      case STATE_UPDATE_CACHED_RESPONSE:
        DCHECK_EQ(OK, rv);
        rv = DoUpdateCachedResponse();
        break;
      case STATE_CACHE_WRITE_UPDATED_RESPONSE:
        DCHECK_EQ(OK, rv);
        rv = DoCacheWriteUpdatedResponse();
        break;
      case STATE_CACHE_WRITE_UPDATED_RESPONSE_COMPLETE:
        rv = DoCacheWriteUpdatedResponseComplete(rv);
        break;
      case STATE_UPDATE_CACHED_RESPONSE_COMPLETE:
        rv = DoUpdateCachedResponseComplete(rv);
        break;
      case STATE_OVERWRITE_CACHED_RESPONSE:
        DCHECK_EQ(OK, rv);
        rv = DoOverwriteCachedResponse();
        break;
      case STATE_CACHE_WRITE_RESPONSE:
        DCHECK_EQ(OK, rv);
        rv = DoCacheWriteResponse();
        break;
      case STATE_CACHE_WRITE_RESPONSE_COMPLETE:
        rv = DoCacheWriteResponseComplete(rv);
        break;
      case STATE_TRUNCATE_CACHED_DATA:
        DCHECK_EQ(OK, rv);
        rv = DoTruncateCachedData();
        break;
      case STATE_TRUNCATE_CACHED_DATA_COMPLETE:
        rv = DoTruncateCachedDataComplete(rv);
        break;
      case STATE_TRUNCATE_CACHED_METADATA:
        DCHECK_EQ(OK, rv);
        rv = DoTruncateCachedMetadata();
        break;
      case STATE_TRUNCATE_CACHED_METADATA_COMPLETE:
        rv = DoTruncateCachedMetadataComplete(rv);
        break;
      case STATE_PARTIAL_HEADERS_RECEIVED:
        DCHECK_EQ(OK, rv);
        rv = DoPartialHeadersReceived();
        break;
      case STATE_CACHE_READ_METADATA:
        DCHECK_EQ(OK, rv);
        rv = DoCacheReadMetadata();
        break;
      case STATE_CACHE_READ_METADATA_COMPLETE:
        rv = DoCacheReadMetadataComplete(rv);
        break;
      case STATE_HEADERS_PHASE_CANNOT_PROCEED:
        rv = DoHeadersPhaseCannotProceed(rv);
        break;
      case STATE_FINISH_HEADERS:
        rv = DoFinishHeaders(rv);
        break;
      case STATE_FINISH_HEADERS_COMPLETE:
        rv = DoFinishHeadersComplete(rv);
        break;
      case STATE_NETWORK_READ_CACHE_WRITE:
        DCHECK_EQ(OK, rv);
        rv = DoNetworkReadCacheWrite();
        break;
      case STATE_NETWORK_READ_CACHE_WRITE_COMPLETE:
        rv = DoNetworkReadCacheWriteComplete(rv);
        break;
      case STATE_CACHE_READ_DATA:
        DCHECK_EQ(OK, rv);
        rv = DoCacheReadData();
        break;
      case STATE_CACHE_READ_DATA_COMPLETE:
        rv = DoCacheReadDataComplete(rv);
        break;
      case STATE_NETWORK_READ:
        DCHECK_EQ(OK, rv);
        rv = DoNetworkRead();
        break;
      case STATE_NETWORK_READ_COMPLETE:
        rv = DoNetworkReadComplete(rv);
        break;
      default:
        NOTREACHED() << "bad state " << state;
        rv = ERR_FAILED;
        break;
    }
    DCHECK(next_state_ != STATE_UNSET) << "Previous state was " << state;

  } while (rv != ERR_IO_PENDING && next_state_ != STATE_NONE);

  // Assert Start() state machine's allowed last state in successful cases when
  // caching is happening.
  DCHECK(reading_ || rv != OK || !entry_ ||
         state == STATE_FINISH_HEADERS_COMPLETE);

  if (rv != ERR_IO_PENDING && !callback_.is_null()) {
    read_buf_ = nullptr;  // Release the buffer before invoking the callback.
    base::ResetAndReturn(&callback_).Run(rv);
  }

  return rv;
}

int HttpCache::Transaction::DoGetBackend() {
  cache_pending_ = true;
  TransitionToState(STATE_GET_BACKEND_COMPLETE);
  net_log_.BeginEvent(NetLogEventType::HTTP_CACHE_GET_BACKEND);
  return cache_->GetBackendForTransaction(this);
}

int HttpCache::Transaction::DoGetBackendComplete(int result) {
  DCHECK(result == OK || result == ERR_FAILED);
  net_log_.EndEventWithNetErrorCode(NetLogEventType::HTTP_CACHE_GET_BACKEND,
                                    result);
  cache_pending_ = false;

  // Reset mode_ that might get set in this function. This is done because this
  // function can be invoked multiple times for a transaction.
  mode_ = NONE;

  if (!ShouldPassThrough()) {
    cache_key_ = cache_->GenerateCacheKey(request_);

    // Requested cache access mode.
    if (effective_load_flags_ & LOAD_ONLY_FROM_CACHE) {
      if (effective_load_flags_ & LOAD_BYPASS_CACHE) {
        // The client has asked for nonsense.
        TransitionToState(STATE_FINISH_HEADERS);
        return ERR_CACHE_MISS;
      }
      mode_ = READ;
    } else if (effective_load_flags_ & LOAD_BYPASS_CACHE) {
      mode_ = WRITE;
    } else {
      mode_ = READ_WRITE;
    }

    // Downgrade to UPDATE if the request has been externally conditionalized.
    if (external_validation_.initialized) {
      if (mode_ & WRITE) {
        // Strip off the READ_DATA bit (and maybe add back a READ_META bit
        // in case READ was off).
        mode_ = UPDATE;
      } else {
        mode_ = NONE;
      }
    }
  }

  // Use PUT and DELETE only to invalidate existing stored entries.
  if ((method_ == "PUT" || method_ == "DELETE") && mode_ != READ_WRITE &&
      mode_ != WRITE) {
    mode_ = NONE;
  }

  // Note that if mode_ == UPDATE (which is tied to external_validation_), the
  // transaction behaves the same for GET and HEAD requests at this point: if it
  // was not modified, the entry is updated and a response is not returned from
  // the cache. If we receive 200, it doesn't matter if there was a validation
  // header or not.
  if (method_ == "HEAD" && mode_ == WRITE)
    mode_ = NONE;

  // If must use cache, then we must fail.  This can happen for back/forward
  // navigations to a page generated via a form post.
  if (!(mode_ & READ) && effective_load_flags_ & LOAD_ONLY_FROM_CACHE) {
    TransitionToState(STATE_FINISH_HEADERS);
    return ERR_CACHE_MISS;
  }

  if (mode_ == NONE) {
    if (partial_) {
      partial_->RestoreHeaders(&custom_request_->extra_headers);
      partial_.reset();
    }
    TransitionToState(STATE_SEND_REQUEST);
  } else {
    TransitionToState(STATE_INIT_ENTRY);
  }

  // This is only set if we have something to do with the response.
  range_requested_ = (partial_.get() != NULL);

  return OK;
}

int HttpCache::Transaction::DoInitEntry() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoInitEntry");
  DCHECK(!new_entry_);

  if (!cache_.get()) {
    TransitionToState(STATE_FINISH_HEADERS);
    return ERR_UNEXPECTED;
  }

  if (mode_ == WRITE) {
    TransitionToState(STATE_DOOM_ENTRY);
    return OK;
  }

  TransitionToState(STATE_OPEN_ENTRY);
  return OK;
}

int HttpCache::Transaction::DoOpenEntry() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoOpenEntry");
  DCHECK(!new_entry_);
  TransitionToState(STATE_OPEN_ENTRY_COMPLETE);
  cache_pending_ = true;
  net_log_.BeginEvent(NetLogEventType::HTTP_CACHE_OPEN_ENTRY);
  first_cache_access_since_ = TimeTicks::Now();

  // See if we already have something working with this cache key.
  new_entry_ = cache_->FindActiveEntry(cache_key_);
  if (new_entry_)
    return OK;

  // See if we could potentially quick-reject the entry based on hints the
  // backend keeps in memory.
  uint8_t in_memory_info =
      cache_->GetCurrentBackend()->GetEntryInMemoryData(cache_key_);
  if (MaybeRejectBasedOnEntryInMemoryData(in_memory_info)) {
    cache_->GetCurrentBackend()->DoomEntry(cache_key_, base::DoNothing());
    return net::ERR_CACHE_ENTRY_NOT_SUITABLE;
  }

  return cache_->OpenEntry(cache_key_, &new_entry_, this);
}

int HttpCache::Transaction::DoOpenEntryComplete(int result) {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoOpenEntryComplete");
  // It is important that we go to STATE_ADD_TO_ENTRY whenever the result is
  // OK, otherwise the cache will end up with an active entry without any
  // transaction attached.
  net_log_.EndEventWithNetErrorCode(NetLogEventType::HTTP_CACHE_OPEN_ENTRY,
                                    result);
  cache_pending_ = false;
  if (result == OK) {
    TransitionToState(STATE_ADD_TO_ENTRY);
    return OK;
  }

  if (result == ERR_CACHE_RACE) {
    TransitionToState(STATE_HEADERS_PHASE_CANNOT_PROCEED);
    return OK;
  }

  if (result == ERR_CACHE_ENTRY_NOT_SUITABLE) {
    // Documents the case this applies in
    DCHECK_EQ(mode_, READ_WRITE);
    // Record this as CantConditionalize, but otherwise proceed as we would
    // below --- as OpenEntry has already dropped the old entry for us.
    couldnt_conditionalize_request_ = true;
    validation_cause_ = VALIDATION_CAUSE_ZERO_FRESHNESS;
    cant_conditionalize_zero_freshness_from_memhint_ = true;
    UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_CANT_CONDITIONALIZE);
  }

  if (method_ == "PUT" || method_ == "DELETE" ||
      (method_ == "HEAD" && mode_ == READ_WRITE)) {
    DCHECK(mode_ == READ_WRITE || mode_ == WRITE || method_ == "HEAD");
    mode_ = NONE;
    TransitionToState(STATE_SEND_REQUEST);
    return OK;
  }

  if (mode_ == READ_WRITE) {
    mode_ = WRITE;
    TransitionToState(STATE_CREATE_ENTRY);
    return OK;
  }
  if (mode_ == UPDATE) {
    // There is no cache entry to update; proceed without caching.
    mode_ = NONE;
    TransitionToState(STATE_SEND_REQUEST);
    return OK;
  }

  // The entry does not exist, and we are not permitted to create a new entry,
  // so we must fail.
  TransitionToState(STATE_FINISH_HEADERS);
  return ERR_CACHE_MISS;
}

int HttpCache::Transaction::DoDoomEntry() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoDoomEntry");
  TransitionToState(STATE_DOOM_ENTRY_COMPLETE);
  cache_pending_ = true;
  if (first_cache_access_since_.is_null())
    first_cache_access_since_ = TimeTicks::Now();
  net_log_.BeginEvent(NetLogEventType::HTTP_CACHE_DOOM_ENTRY);
  return cache_->DoomEntry(cache_key_, this);
}

int HttpCache::Transaction::DoDoomEntryComplete(int result) {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoDoomEntryComplete");
  net_log_.EndEventWithNetErrorCode(NetLogEventType::HTTP_CACHE_DOOM_ENTRY,
                                    result);
  cache_pending_ = false;
  TransitionToState(result == ERR_CACHE_RACE
                        ? STATE_HEADERS_PHASE_CANNOT_PROCEED
                        : STATE_CREATE_ENTRY);
  return OK;
}

int HttpCache::Transaction::DoCreateEntry() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoCreateEntry");
  DCHECK(!new_entry_);
  TransitionToState(STATE_CREATE_ENTRY_COMPLETE);
  cache_pending_ = true;
  net_log_.BeginEvent(NetLogEventType::HTTP_CACHE_CREATE_ENTRY);
  return cache_->CreateEntry(cache_key_, &new_entry_, this);
}

int HttpCache::Transaction::DoCreateEntryComplete(int result) {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoCreateEntryComplete");
  // It is important that we go to STATE_ADD_TO_ENTRY whenever the result is
  // OK, otherwise the cache will end up with an active entry without any
  // transaction attached.
  net_log_.EndEventWithNetErrorCode(NetLogEventType::HTTP_CACHE_CREATE_ENTRY,
                                    result);
  cache_pending_ = false;
  switch (result) {
    case OK:
      TransitionToState(STATE_ADD_TO_ENTRY);
      break;

    case ERR_CACHE_RACE:
      TransitionToState(STATE_HEADERS_PHASE_CANNOT_PROCEED);
      break;

    default:
      // We have a race here: Maybe we failed to open the entry and decided to
      // create one, but by the time we called create, another transaction
      // already created the entry. If we want to eliminate this issue, we
      // need an atomic OpenOrCreate() method exposed by the disk cache.
      DLOG(WARNING) << "Unable to create cache entry";

      // Set the mode to NONE in order to bypass the cache entry and read from
      // the network directly.
      mode_ = NONE;
      if (!done_headers_create_new_entry_) {
        if (partial_)
          partial_->RestoreHeaders(&custom_request_->extra_headers);
        TransitionToState(STATE_SEND_REQUEST);
        return OK;
      }
      // The headers have already been received as a result of validation,
      // triggering the doom of the old entry.  So no network request needs to
      // be sent. Note that since mode_ is NONE, the response won't be written
      // to cache. Transition to STATE_CACHE_WRITE_RESPONSE as that's the state
      // the transaction left off on when it tried to create the new entry.
      done_headers_create_new_entry_ = false;
      TransitionToState(STATE_CACHE_WRITE_RESPONSE);
  }
  return OK;
}

int HttpCache::Transaction::DoAddToEntry() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoAddToEntry");
  DCHECK(new_entry_);
  cache_pending_ = true;
  net_log_.BeginEvent(NetLogEventType::HTTP_CACHE_ADD_TO_ENTRY);
  DCHECK(entry_lock_waiting_since_.is_null());
  int rv = cache_->AddTransactionToEntry(new_entry_, this);
  DCHECK_EQ(rv, ERR_IO_PENDING);

  // If headers phase is already done then we are here because of validation not
  // matching and creating a new entry. This transaction should be the
  // first transaction of that new entry and thus it will not have cache lock
  // delays, thus returning early from here.
  if (done_headers_create_new_entry_) {
    DCHECK_EQ(mode_, WRITE);
    TransitionToState(STATE_DONE_HEADERS_ADD_TO_ENTRY_COMPLETE);
    return rv;
  }

  TransitionToState(STATE_ADD_TO_ENTRY_COMPLETE);

  entry_lock_waiting_since_ = TimeTicks::Now();
  AddCacheLockTimeoutHandler(new_entry_);
  return rv;
}

void HttpCache::Transaction::AddCacheLockTimeoutHandler(ActiveEntry* entry) {
  DCHECK(next_state_ == STATE_ADD_TO_ENTRY_COMPLETE ||
         next_state_ == STATE_FINISH_HEADERS_COMPLETE);
  if ((bypass_lock_for_test_ && next_state_ == STATE_ADD_TO_ENTRY_COMPLETE) ||
      (bypass_lock_after_headers_for_test_ &&
       next_state_ == STATE_FINISH_HEADERS_COMPLETE)) {
    base::ThreadTaskRunnerHandle::Get()->PostTask(
        FROM_HERE,
        base::Bind(&HttpCache::Transaction::OnCacheLockTimeout,
                   weak_factory_.GetWeakPtr(), entry_lock_waiting_since_));
  } else {
    int timeout_milliseconds = 20 * 1000;
    if (partial_ && entry->writers && !entry->writers->IsEmpty() &&
        entry->writers->IsExclusive()) {
      // Even though entry_->writers takes care of allowing multiple writers to
      // simultaneously govern reading from the network and writing to the cache
      // for full requests, partial requests are still blocked by the
      // reader/writer lock.
      // Bypassing the cache after 25 ms of waiting for the cache lock
      // eliminates a long running issue, http://crbug.com/31014, where
      // two of the same media resources could not be played back simultaneously
      // due to one locking the cache entry until the entire video was
      // downloaded.
      // Bypassing the cache is not ideal, as we are now ignoring the cache
      // entirely for all range requests to a resource beyond the first. This
      // is however a much more succinct solution than the alternatives, which
      // would require somewhat significant changes to the http caching logic.
      //
      // Allow some timeout slack for the entry addition to complete in case
      // the writer lock is imminently released; we want to avoid skipping
      // the cache if at all possible. See http://crbug.com/408765
      timeout_milliseconds = 25;
    }
    base::ThreadTaskRunnerHandle::Get()->PostDelayedTask(
        FROM_HERE,
        base::Bind(&HttpCache::Transaction::OnCacheLockTimeout,
                   weak_factory_.GetWeakPtr(), entry_lock_waiting_since_),
        TimeDelta::FromMilliseconds(timeout_milliseconds));
  }
}

int HttpCache::Transaction::DoAddToEntryComplete(int result) {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoAddToEntryComplete");
  net_log_.EndEventWithNetErrorCode(NetLogEventType::HTTP_CACHE_ADD_TO_ENTRY,
                                    result);
  const TimeDelta entry_lock_wait =
      TimeTicks::Now() - entry_lock_waiting_since_;
  UMA_HISTOGRAM_TIMES("HttpCache.EntryLockWait", entry_lock_wait);

  entry_lock_waiting_since_ = TimeTicks();
  DCHECK(new_entry_);
  cache_pending_ = false;

  if (result == OK)
    entry_ = new_entry_;

  // If there is a failure, the cache should have taken care of new_entry_.
  new_entry_ = NULL;

  if (result == ERR_CACHE_RACE) {
    TransitionToState(STATE_HEADERS_PHASE_CANNOT_PROCEED);
    return OK;
  }

  if (result == ERR_CACHE_LOCK_TIMEOUT) {
    if (mode_ == READ) {
      TransitionToState(STATE_FINISH_HEADERS);
      return ERR_CACHE_MISS;
    }

    // The cache is busy, bypass it for this transaction.
    mode_ = NONE;
    TransitionToState(STATE_SEND_REQUEST);
    if (partial_) {
      partial_->RestoreHeaders(&custom_request_->extra_headers);
      partial_.reset();
    }
    return OK;
  }

  // TODO(crbug.com/713354) Access timestamp for histograms only if entry is
  // already written, to avoid data race since cache thread can also access
  // this.
  if (!cache_->IsWritingInProgress(entry_))
    open_entry_last_used_ = entry_->disk_entry->GetLastUsed();

  // TODO(jkarlin): We should either handle the case or DCHECK.
  if (result != OK) {
    NOTREACHED();
    TransitionToState(STATE_FINISH_HEADERS);
    return result;
  }

  if (mode_ == WRITE) {
    if (partial_)
      partial_->RestoreHeaders(&custom_request_->extra_headers);
    TransitionToState(STATE_SEND_REQUEST);
  } else {
    // We have to read the headers from the cached entry.
    DCHECK(mode_ & READ_META);
    TransitionToState(STATE_CACHE_READ_RESPONSE);
  }
  return OK;
}

int HttpCache::Transaction::DoDoneHeadersAddToEntryComplete(int result) {
  // This transaction's response headers did not match its ActiveEntry so it
  // created a new ActiveEntry (new_entry_) to write to (and doomed the old
  // one). Now that the new entry has been created, start writing the response.

  DCHECK_EQ(result, OK);
  DCHECK_EQ(mode_, WRITE);
  DCHECK(new_entry_);
  DCHECK(response_.headers);

  cache_pending_ = false;
  entry_ = new_entry_;
  done_headers_create_new_entry_ = false;
  DCHECK_NE(response_.headers->response_code(), 304);
  DCHECK(cache_->CanTransactionWriteResponseHeaders(
      entry_, this, partial_ != nullptr, false));
  TransitionToState(STATE_CACHE_WRITE_RESPONSE);
  return OK;
}

int HttpCache::Transaction::DoCacheReadResponse() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoCacheReadResponse");
  DCHECK(entry_);
  TransitionToState(STATE_CACHE_READ_RESPONSE_COMPLETE);

  io_buf_len_ = entry_->disk_entry->GetDataSize(kResponseInfoIndex);
  read_buf_ = new IOBuffer(io_buf_len_);

  net_log_.BeginEvent(NetLogEventType::HTTP_CACHE_READ_INFO);
  return entry_->disk_entry->ReadData(kResponseInfoIndex, 0, read_buf_.get(),
                                      io_buf_len_, io_callback_);
}

int HttpCache::Transaction::DoCacheReadResponseComplete(int result) {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoCacheReadResponseComplete");
  net_log_.EndEventWithNetErrorCode(NetLogEventType::HTTP_CACHE_READ_INFO,
                                    result);
  if (result != io_buf_len_ ||
      !HttpCache::ParseResponseInfo(read_buf_->data(), io_buf_len_, &response_,
                                    &truncated_)) {
    return OnCacheReadError(result, true);
  }

  // TODO(crbug.com/713354) Only get data size if there is no other transaction
  // currently writing the response body due to the data race mentioned in the
  // associated bug.
  if (!cache_->IsWritingInProgress(entry_)) {
    int current_size = entry_->disk_entry->GetDataSize(kResponseContentIndex);
    int64_t full_response_length = response_.headers->GetContentLength();

    // Some resources may have slipped in as truncated when they're not.
    if (full_response_length == current_size)
      truncated_ = false;

    // The state machine's handling of StopCaching unfortunately doesn't deal
    // well with resources that are larger than 2GB when there is a truncated or
    // sparse cache entry. While the state machine is reworked to resolve this,
    // the following logic is put in place to defer such requests to the
    // network. The cache should not be storing multi gigabyte resources. See
    // http://crbug.com/89567.
    if ((truncated_ || response_.headers->response_code() == 206) &&
        !range_requested_ &&
        full_response_length > std::numeric_limits<int32_t>::max()) {
      DCHECK(!partial_);

      // Doom the entry so that no other transaction gets added to this entry
      // and avoid a race of not being able to check this condition because
      // writing is in progress.
      DoneWithEntry(false);
      TransitionToState(STATE_SEND_REQUEST);
      return OK;
    }
  }

  if (response_.unused_since_prefetch !=
      !!(request_->load_flags & LOAD_PREFETCH)) {
    // Either this is the first use of an entry since it was prefetched XOR
    // this is a prefetch. The value of response.unused_since_prefetch is
    // valid for this transaction but the bit needs to be flipped in storage.
    TransitionToState(STATE_TOGGLE_UNUSED_SINCE_PREFETCH);
    return OK;
  }

  TransitionToState(STATE_CACHE_DISPATCH_VALIDATION);
  return OK;
}

int HttpCache::Transaction::DoCacheToggleUnusedSincePrefetch() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoCacheToggleUnusedSincePrefetch");
  // Write back the toggled value for the next use of this entry.
  response_.unused_since_prefetch = !response_.unused_since_prefetch;

  // TODO(jkarlin): If DoUpdateCachedResponse is also called for this
  // transaction then metadata will be written to cache twice. If prefetching
  // becomes more common, consider combining the writes.

  TransitionToState(STATE_TOGGLE_UNUSED_SINCE_PREFETCH_COMPLETE);
  return WriteResponseInfoToEntry(false);
}

int HttpCache::Transaction::DoCacheToggleUnusedSincePrefetchComplete(
    int result) {
  TRACE_EVENT0(
      kNetTracingCategory,
      "HttpCacheTransaction::DoCacheToggleUnusedSincePrefetchComplete");
  // Restore the original value for this transaction.
  response_.unused_since_prefetch = !response_.unused_since_prefetch;
  TransitionToState(STATE_CACHE_DISPATCH_VALIDATION);
  return OnWriteResponseInfoToEntryComplete(result);
}

int HttpCache::Transaction::DoCacheDispatchValidation() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoCacheDispatchValidation");
  // We now have access to the cache entry.
  //
  //  o if we are a reader for the transaction, then we can start reading the
  //    cache entry.
  //
  //  o if we can read or write, then we should check if the cache entry needs
  //    to be validated and then issue a network request if needed or just read
  //    from the cache if the cache entry is already valid.
  //
  //  o if we are set to UPDATE, then we are handling an externally
  //    conditionalized request (if-modified-since / if-none-match). We check
  //    if the request headers define a validation request.
  //
  int result = ERR_FAILED;
  switch (mode_) {
    case READ:
      UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_USED);
      result = BeginCacheRead();
      break;
    case READ_WRITE:
      result = BeginPartialCacheValidation();
      break;
    case UPDATE:
      result = BeginExternallyConditionalizedRequest();
      break;
    case WRITE:
    default:
      NOTREACHED();
  }
  return result;
}

int HttpCache::Transaction::DoCacheQueryData() {
  TransitionToState(STATE_CACHE_QUERY_DATA_COMPLETE);
  return entry_->disk_entry->ReadyForSparseIO(io_callback_);
}

int HttpCache::Transaction::DoCacheQueryDataComplete(int result) {
  DCHECK_EQ(OK, result);
  if (!cache_.get()) {
    TransitionToState(STATE_FINISH_HEADERS);
    return ERR_UNEXPECTED;
  }

  return ValidateEntryHeadersAndContinue();
}

// We may end up here multiple times for a given request.
int HttpCache::Transaction::DoStartPartialCacheValidation() {
  if (mode_ == NONE) {
    TransitionToState(STATE_FINISH_HEADERS);
    return OK;
  }

  TransitionToState(STATE_COMPLETE_PARTIAL_CACHE_VALIDATION);
  return partial_->ShouldValidateCache(entry_->disk_entry, io_callback_);
}

int HttpCache::Transaction::DoCompletePartialCacheValidation(int result) {
  if (!result) {
    // This is the end of the request.
    DoneWithEntry(true);
    TransitionToState(STATE_FINISH_HEADERS);
    return result;
  }

  if (result < 0) {
    TransitionToState(STATE_FINISH_HEADERS);
    return result;
  }

  partial_->PrepareCacheValidation(entry_->disk_entry,
                                   &custom_request_->extra_headers);

  if (reading_ && partial_->IsCurrentRangeCached()) {
    TransitionToState(STATE_CACHE_READ_DATA);
    return OK;
  }

  return BeginCacheValidation();
}

int HttpCache::Transaction::DoSendRequest() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoSendRequest");
  DCHECK(mode_ & WRITE || mode_ == NONE);
  DCHECK(!network_trans_.get());

  send_request_since_ = TimeTicks::Now();

  // Create a network transaction.
  int rv =
      cache_->network_layer_->CreateTransaction(priority_, &network_trans_);

  if (rv != OK) {
    TransitionToState(STATE_FINISH_HEADERS);
    return rv;
  }

  network_trans_->SetBeforeNetworkStartCallback(before_network_start_callback_);
  network_trans_->SetBeforeHeadersSentCallback(before_headers_sent_callback_);
  network_trans_->SetRequestHeadersCallback(request_headers_callback_);
  network_trans_->SetResponseHeadersCallback(response_headers_callback_);

  // Old load timing information, if any, is now obsolete.
  network_transaction_info_.old_network_trans_load_timing.reset();
  network_transaction_info_.old_remote_endpoint = IPEndPoint();

  if (websocket_handshake_stream_base_create_helper_)
    network_trans_->SetWebSocketHandshakeStreamCreateHelper(
        websocket_handshake_stream_base_create_helper_);

  TransitionToState(STATE_SEND_REQUEST_COMPLETE);
  rv = network_trans_->Start(request_, io_callback_, net_log_);
  return rv;
}

int HttpCache::Transaction::DoSendRequestComplete(int result) {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoSendRequestComplete");
  if (!cache_.get()) {
    TransitionToState(STATE_FINISH_HEADERS);
    return ERR_UNEXPECTED;
  }

  // If we tried to conditionalize the request and failed, we know
  // we won't be reading from the cache after this point.
  if (couldnt_conditionalize_request_)
    mode_ = WRITE;

  if (result == OK) {
    TransitionToState(STATE_SUCCESSFUL_SEND_REQUEST);
    return OK;
  }

  const HttpResponseInfo* response = network_trans_->GetResponseInfo();
  response_.network_accessed = response->network_accessed;

  // Do not record requests that have network errors or restarts.
  UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_OTHER);
  if (IsCertificateError(result)) {
    // If we get a certificate error, then there is a certificate in ssl_info,
    // so GetResponseInfo() should never return NULL here.
    DCHECK(response);
    response_.ssl_info = response->ssl_info;
  } else if (result == ERR_SSL_CLIENT_AUTH_CERT_NEEDED) {
    DCHECK(response);
    response_.cert_request_info = response->cert_request_info;
  } else if (response_.was_cached) {
    DoneWithEntry(true);
  }

  TransitionToState(STATE_FINISH_HEADERS);
  return result;
}

// We received the response headers and there is no error.
int HttpCache::Transaction::DoSuccessfulSendRequest() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoSuccessfulSendRequest");
  DCHECK(!new_response_);
  const HttpResponseInfo* new_response = network_trans_->GetResponseInfo();

  if (new_response->headers->response_code() == 401 ||
      new_response->headers->response_code() == 407) {
    SetAuthResponse(*new_response);
    if (!reading_) {
      TransitionToState(STATE_FINISH_HEADERS);
      return OK;
    }

    // We initiated a second request the caller doesn't know about. We should be
    // able to authenticate this request because we should have authenticated
    // this URL moments ago.
    if (IsReadyToRestartForAuth()) {
      DCHECK(!response_.auth_challenge.get());
      TransitionToState(STATE_SEND_REQUEST_COMPLETE);
      // In theory we should check to see if there are new cookies, but there
      // is no way to do that from here.
      return network_trans_->RestartWithAuth(AuthCredentials(), io_callback_);
    }

    // We have to perform cleanup at this point so that at least the next
    // request can succeed.  We do not retry at this point, because data
    // has been read and we have no way to gather credentials.  We would
    // fail again, and potentially loop.  This can happen if the credentials
    // expire while chrome is suspended.
    if (entry_)
      DoomPartialEntry(false);
    mode_ = NONE;
    partial_.reset();
    ResetNetworkTransaction();
    TransitionToState(STATE_FINISH_HEADERS);
    return ERR_CACHE_AUTH_FAILURE_AFTER_READ;
  }

  new_response_ = new_response;
  if (!ValidatePartialResponse() && !auth_response_.headers.get()) {
    // Something went wrong with this request and we have to restart it.
    // If we have an authentication response, we are exposed to weird things
    // hapenning if the user cancels the authentication before we receive
    // the new response.
    net_log_.AddEvent(NetLogEventType::HTTP_CACHE_RE_SEND_PARTIAL_REQUEST);
    UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_OTHER);
    SetResponse(HttpResponseInfo());
    ResetNetworkTransaction();
    new_response_ = NULL;
    TransitionToState(STATE_SEND_REQUEST);
    return OK;
  }

  if (handling_206_ && mode_ == READ_WRITE && !truncated_ && !is_sparse_) {
    // We have stored the full entry, but it changed and the server is
    // sending a range. We have to delete the old entry.
    UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_OTHER);
    DoneWithEntry(false);
  }

  if (mode_ == WRITE &&
      cache_entry_status_ != CacheEntryStatus::ENTRY_CANT_CONDITIONALIZE) {
    UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_NOT_IN_CACHE);
  }

  // Invalidate any cached GET with a successful PUT or DELETE.
  if (mode_ == WRITE && (method_ == "PUT" || method_ == "DELETE")) {
    if (NonErrorResponse(new_response->headers->response_code()) &&
        (entry_ && !entry_->doomed)) {
      int ret = cache_->DoomEntry(cache_key_, NULL);
      DCHECK_EQ(OK, ret);
    }
    // Do not invalidate the entry if its a failed Delete or Put.
    DoneWithEntry(true);
  }

  // Invalidate any cached GET with a successful POST.
  if (!(effective_load_flags_ & LOAD_DISABLE_CACHE) && method_ == "POST" &&
      NonErrorResponse(new_response->headers->response_code())) {
    cache_->DoomMainEntryForUrl(request_->url);
  }

  RecordNoStoreHeaderHistogram(request_->load_flags, new_response);

  if (new_response_->headers->response_code() == 416 &&
      (method_ == "GET" || method_ == "POST")) {
    // If there is an active entry it may be destroyed with this transaction.
    SetResponse(*new_response_);
    TransitionToState(STATE_FINISH_HEADERS);
    return OK;
  }

  // Are we expecting a response to a conditional query?
  if (mode_ == READ_WRITE || mode_ == UPDATE) {
    if (new_response->headers->response_code() == 304 || handling_206_) {
      UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_VALIDATED);
      TransitionToState(STATE_UPDATE_CACHED_RESPONSE);
      return OK;
    }
    UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_UPDATED);
    mode_ = WRITE;
  }

  TransitionToState(STATE_OVERWRITE_CACHED_RESPONSE);
  return OK;
}

// We received 304 or 206 and we want to update the cached response headers.
int HttpCache::Transaction::DoUpdateCachedResponse() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoUpdateCachedResponse");
  int rv = OK;
  // Update the cached response based on the headers and properties of
  // new_response_.
  response_.headers->Update(*new_response_->headers.get());
  response_.response_time = new_response_->response_time;
  response_.request_time = new_response_->request_time;
  response_.network_accessed = new_response_->network_accessed;
  response_.unused_since_prefetch = new_response_->unused_since_prefetch;
  response_.ssl_info = new_response_->ssl_info;
  if (new_response_->vary_data.is_valid()) {
    response_.vary_data = new_response_->vary_data;
  } else if (response_.vary_data.is_valid()) {
    // There is a vary header in the stored response but not in the current one.
    // Update the data with the new request headers.
    HttpVaryData new_vary_data;
    new_vary_data.Init(*request_, *response_.headers.get());
    response_.vary_data = new_vary_data;
  }

  if (response_.headers->HasHeaderValue("cache-control", "no-store")) {
    if (!entry_->doomed) {
      int ret = cache_->DoomEntry(cache_key_, NULL);
      DCHECK_EQ(OK, ret);
    }
    TransitionToState(STATE_UPDATE_CACHED_RESPONSE_COMPLETE);
  } else {
    // If we are already reading, we already updated the headers for this
    // request; doing it again will change Content-Length.
    if (!reading_) {
      TransitionToState(STATE_CACHE_WRITE_UPDATED_RESPONSE);
      rv = OK;
    } else {
      TransitionToState(STATE_UPDATE_CACHED_RESPONSE_COMPLETE);
    }
  }

  return rv;
}

int HttpCache::Transaction::DoCacheWriteUpdatedResponse() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoCacheWriteUpdatedResponse");

  TransitionToState(STATE_CACHE_WRITE_UPDATED_RESPONSE_COMPLETE);
  return WriteResponseInfoToEntry(false);
}

int HttpCache::Transaction::DoCacheWriteUpdatedResponseComplete(int result) {
  TRACE_EVENT0("io",
               "HttpCacheTransaction::DoCacheWriteUpdatedResponseComplete");
  TransitionToState(STATE_UPDATE_CACHED_RESPONSE_COMPLETE);
  return OnWriteResponseInfoToEntryComplete(result);
}

int HttpCache::Transaction::DoUpdateCachedResponseComplete(int result) {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoUpdateCachedResponseComplete");
  if (mode_ == UPDATE) {
    DCHECK(!handling_206_);
    // We got a "not modified" response and already updated the corresponding
    // cache entry above.
    //
    // By stopping to write to the cache now, we make sure that the 304 rather
    // than the cached 200 response, is what will be returned to the user.
    DoneWithEntry(true);
  } else if (entry_ && !handling_206_) {
    DCHECK_EQ(READ_WRITE, mode_);
    if ((!partial_ && !cache_->IsWritingInProgress(entry_)) ||
        (partial_ && partial_->IsLastRange())) {
      mode_ = READ;
    }
    // We no longer need the network transaction, so destroy it.
    if (network_trans_)
      ResetNetworkTransaction();
  } else if (entry_ && handling_206_ && truncated_ &&
             partial_->initial_validation()) {
    // We just finished the validation of a truncated entry, and the server
    // is willing to resume the operation. Now we go back and start serving
    // the first part to the user.
    if (network_trans_)
      ResetNetworkTransaction();
    new_response_ = NULL;
    TransitionToState(STATE_START_PARTIAL_CACHE_VALIDATION);
    partial_->SetRangeToStartDownload();
    return OK;
  }
  TransitionToState(STATE_OVERWRITE_CACHED_RESPONSE);
  return OK;
}

int HttpCache::Transaction::DoOverwriteCachedResponse() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoOverwriteCachedResponse");
  if (mode_ & READ) {
    TransitionToState(STATE_PARTIAL_HEADERS_RECEIVED);
    return OK;
  }

  // We change the value of Content-Length for partial content.
  if (handling_206_ && partial_)
    partial_->FixContentLength(new_response_->headers.get());

  SetResponse(*new_response_);

  if (method_ == "HEAD") {
    // This response is replacing the cached one.
    DoneWithEntry(false);
    new_response_ = NULL;
    TransitionToState(STATE_FINISH_HEADERS);
    return OK;
  }

  if (handling_206_ && !CanResume(false)) {
    // There is no point in storing this resource because it will never be used.
    // This may change if we support LOAD_ONLY_FROM_CACHE with sparse entries.
    DoneWithEntry(false);
    if (partial_)
      partial_->FixResponseHeaders(response_.headers.get(), true);
    TransitionToState(STATE_PARTIAL_HEADERS_RECEIVED);
    return OK;
  }

  TransitionToState(STATE_CACHE_WRITE_RESPONSE);
  return OK;
}

int HttpCache::Transaction::DoCacheWriteResponse() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoCacheWriteResponse");

  // Invalidate any current entry with a successful response if this transaction
  // cannot write to this entry. This transaction then continues to read from
  // the network without writing to the backend.
  bool is_match = response_.headers->response_code() == 304;
  if (entry_ && response_.headers &&
      !cache_->CanTransactionWriteResponseHeaders(
          entry_, this, partial_ != nullptr, is_match)) {
    done_headers_create_new_entry_ = true;

    // The transaction needs to overwrite this response. Doom the current entry,
    // create a new one (by going to STATE_INIT_ENTRY), and then jump straight
    // to writing out the response, bypassing the headers checks. The mode_ is
    // set to WRITE in order to doom any other existing entries that might exist
    // so that this transaction can go straight to writing a response.
    mode_ = WRITE;
    TransitionToState(STATE_INIT_ENTRY);
    cache_->DoomEntryValidationNoMatch(entry_);
    entry_ = nullptr;
    return OK;
  }

  TransitionToState(STATE_CACHE_WRITE_RESPONSE_COMPLETE);
  return WriteResponseInfoToEntry(truncated_);
}

int HttpCache::Transaction::DoCacheWriteResponseComplete(int result) {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoCacheWriteResponseComplete");
  TransitionToState(STATE_TRUNCATE_CACHED_DATA);
  return OnWriteResponseInfoToEntryComplete(result);
}

int HttpCache::Transaction::DoTruncateCachedData() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoTruncateCachedData");
  TransitionToState(STATE_TRUNCATE_CACHED_DATA_COMPLETE);
  if (!entry_)
    return OK;
  if (net_log_.IsCapturing())
    net_log_.BeginEvent(NetLogEventType::HTTP_CACHE_WRITE_DATA);
  // Truncate the stream.
  return WriteToEntry(kResponseContentIndex, 0, NULL, 0, io_callback_);
}

int HttpCache::Transaction::DoTruncateCachedDataComplete(int result) {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoInitEntry");
  if (entry_) {
    if (net_log_.IsCapturing()) {
      net_log_.EndEventWithNetErrorCode(NetLogEventType::HTTP_CACHE_WRITE_DATA,
                                        result);
    }
  }

  TransitionToState(STATE_TRUNCATE_CACHED_METADATA);
  return OK;
}

int HttpCache::Transaction::DoTruncateCachedMetadata() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoTruncateCachedMetadata");
  TransitionToState(STATE_TRUNCATE_CACHED_METADATA_COMPLETE);
  if (!entry_)
    return OK;

  if (net_log_.IsCapturing())
    net_log_.BeginEvent(NetLogEventType::HTTP_CACHE_WRITE_INFO);
  return WriteToEntry(kMetadataIndex, 0, NULL, 0, io_callback_);
}

int HttpCache::Transaction::DoTruncateCachedMetadataComplete(int result) {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoTruncateCachedMetadataComplete");
  if (entry_) {
    if (net_log_.IsCapturing()) {
      net_log_.EndEventWithNetErrorCode(NetLogEventType::HTTP_CACHE_WRITE_INFO,
                                        result);
    }
  }

  TransitionToState(STATE_PARTIAL_HEADERS_RECEIVED);
  return OK;
}

int HttpCache::Transaction::DoPartialHeadersReceived() {
  new_response_ = NULL;

  if (!partial_) {
    if (entry_ && entry_->disk_entry->GetDataSize(kMetadataIndex)) {
      TransitionToState(STATE_CACHE_READ_METADATA);
    } else {
      TransitionToState(STATE_FINISH_HEADERS);
    }
    return OK;
  }

  if (mode_ != NONE && !reading_) {
    // We are about to return the headers for a byte-range request to the user,
    // so let's fix them.
    partial_->FixResponseHeaders(response_.headers.get(), true);
  }
  TransitionToState(STATE_FINISH_HEADERS);
  return OK;
}

int HttpCache::Transaction::DoHeadersPhaseCannotProceed(int result) {
  // If its the Start state machine and it cannot proceed due to a cache
  // failure, restart this transaction.
  DCHECK(!reading_);

  // Reset before invoking SetRequest() which can reset the request info sent to
  // network transaction.
  if (network_trans_)
    network_trans_.reset();

  new_response_ = nullptr;

  SetRequest(net_log_);

  entry_ = nullptr;

  // Bypass the cache for timeout scenario.
  if (result == ERR_CACHE_LOCK_TIMEOUT)
    effective_load_flags_ |= LOAD_DISABLE_CACHE;

  TransitionToState(STATE_GET_BACKEND);
  return OK;
}

int HttpCache::Transaction::DoFinishHeaders(int result) {
  if (!cache_.get() || !entry_ || result != OK) {
    TransitionToState(STATE_NONE);
    return result;
  }

  TransitionToState(STATE_FINISH_HEADERS_COMPLETE);

  // If it was an auth failure, this transaction should continue to be
  // headers_transaction till consumer takes an action, so no need to do
  // anything now.
  // TODO(crbug.com/740947). See the issue for a suggestion for cleaning the
  // state machine to be able to remove this condition.
  if (auth_response_.headers.get())
    return OK;

  // If the transaction needs to wait because another transaction is still
  // writing the response body, it will return ERR_IO_PENDING now and the
  // io_callback_ will be invoked when the wait is done.
  int rv = cache_->DoneWithResponseHeaders(entry_, this, partial_ != nullptr);
  DCHECK(!reading_ || rv == OK) << "Expected OK, but got " << rv;

  if (rv == ERR_IO_PENDING) {
    DCHECK(entry_lock_waiting_since_.is_null());
    entry_lock_waiting_since_ = TimeTicks::Now();
    AddCacheLockTimeoutHandler(entry_);
  }
  return rv;
}

int HttpCache::Transaction::DoFinishHeadersComplete(int rv) {
  entry_lock_waiting_since_ = TimeTicks();
  if (rv == ERR_CACHE_RACE || rv == ERR_CACHE_LOCK_TIMEOUT) {
    TransitionToState(STATE_HEADERS_PHASE_CANNOT_PROCEED);
    return rv;
  }

  if (network_trans_ && InWriters()) {
    entry_->writers->SetNetworkTransaction(this, std::move(network_trans_));
    moved_network_transaction_to_writers_ = true;
  }

  // If already reading, that means it is a partial request coming back to the
  // headers phase, continue to the appropriate reading state.
  if (reading_) {
    int rv = TransitionToReadingState();
    DCHECK_EQ(OK, rv);
    return OK;
  }

  TransitionToState(STATE_NONE);
  return rv;
}

int HttpCache::Transaction::DoCacheReadMetadata() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoCacheReadMetadata");
  DCHECK(entry_);
  DCHECK(!response_.metadata.get());
  TransitionToState(STATE_CACHE_READ_METADATA_COMPLETE);

  response_.metadata =
      new IOBufferWithSize(entry_->disk_entry->GetDataSize(kMetadataIndex));

  net_log_.BeginEvent(NetLogEventType::HTTP_CACHE_READ_INFO);
  return entry_->disk_entry->ReadData(kMetadataIndex, 0,
                                      response_.metadata.get(),
                                      response_.metadata->size(),
                                      io_callback_);
}

int HttpCache::Transaction::DoCacheReadMetadataComplete(int result) {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoCacheReadMetadataComplete");
  net_log_.EndEventWithNetErrorCode(NetLogEventType::HTTP_CACHE_READ_INFO,
                                    result);
  if (result != response_.metadata->size())
    return OnCacheReadError(result, false);

  TransitionToState(STATE_FINISH_HEADERS);
  return OK;
}

int HttpCache::Transaction::DoNetworkReadCacheWrite() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoNetworkReadCacheWrite");
  DCHECK(InWriters());
  TransitionToState(STATE_NETWORK_READ_CACHE_WRITE_COMPLETE);
  return entry_->writers->Read(read_buf_, io_buf_len_, io_callback_, this);
}

int HttpCache::Transaction::DoNetworkReadCacheWriteComplete(int result) {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoNetworkReadCacheWriteComplete");
  if (!cache_.get()) {
    TransitionToState(STATE_NONE);
    return ERR_UNEXPECTED;
  }
  // |result| will be error code in case of network read failure and |this|
  // cannot proceed further, so set entry_ to null. |result| will not be error
  // in case of cache write failure since |this| can continue to read from the
  // network. If response is completed, then also set entry to null.
  if (result < 0) {
    // We should have discovered this error in WriterAboutToBeRemovedFromEntry
    DCHECK_EQ(result, shared_writing_error_);
    DCHECK_EQ(NONE, mode_);
    DCHECK(!entry_);
    TransitionToState(STATE_NONE);
    return result;
  }

  if (partial_) {
    return DoPartialNetworkReadCompleted(result);
  }

  if (result == 0) {
    DCHECK_EQ(NONE, mode_);
    DCHECK(!entry_);
  } else {
    read_offset_ += result;
  }
  TransitionToState(STATE_NONE);
  return result;
}

int HttpCache::Transaction::DoPartialNetworkReadCompleted(int result) {
  DCHECK(partial_);

  // Go to the next range if nothing returned or return the result.
  // TODO(shivanisha) Simplify this condition if possible. It was introduced
  // in https://codereview.chromium.org/545101
  if (result != 0 || truncated_ ||
      !(partial_->IsLastRange() || mode_ == WRITE)) {
    partial_->OnNetworkReadCompleted(result);

    if (result == 0) {
      // We need to move on to the next range.
      if (network_trans_) {
        ResetNetworkTransaction();
      } else if (InWriters() && entry_->writers->network_transaction()) {
        SaveNetworkTransactionInfo(*(entry_->writers->network_transaction()));
        entry_->writers->ResetNetworkTransaction();
      }
      TransitionToState(STATE_START_PARTIAL_CACHE_VALIDATION);
    } else {
      TransitionToState(STATE_NONE);
    }
    return result;
  }

  // Request completed.
  if (result == 0) {
    DoneWithEntry(true);
  }

  TransitionToState(STATE_NONE);
  return result;
}

int HttpCache::Transaction::DoNetworkRead() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoNetworkRead");
  TransitionToState(STATE_NETWORK_READ_COMPLETE);
  return network_trans_->Read(read_buf_.get(), io_buf_len_, io_callback_);
}

int HttpCache::Transaction::DoNetworkReadComplete(int result) {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoNetworkReadComplete");

  if (!cache_.get()) {
    TransitionToState(STATE_NONE);
    return ERR_UNEXPECTED;
  }

  if (partial_)
    return DoPartialNetworkReadCompleted(result);

  TransitionToState(STATE_NONE);
  return result;
}

int HttpCache::Transaction::DoCacheReadData() {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoCacheReadData");

  if (method_ == "HEAD") {
    TransitionToState(STATE_NONE);
    return 0;
  }

  DCHECK(entry_);
  TransitionToState(STATE_CACHE_READ_DATA_COMPLETE);

  if (net_log_.IsCapturing())
    net_log_.BeginEvent(NetLogEventType::HTTP_CACHE_READ_DATA);
  if (partial_) {
    return partial_->CacheRead(entry_->disk_entry, read_buf_.get(), io_buf_len_,
                               io_callback_);
  }

  return entry_->disk_entry->ReadData(kResponseContentIndex, read_offset_,
                                      read_buf_.get(), io_buf_len_,
                                      io_callback_);
}

int HttpCache::Transaction::DoCacheReadDataComplete(int result) {
  TRACE_EVENT0("io", "HttpCacheTransaction::DoCacheReadDataComplete");
  if (net_log_.IsCapturing()) {
    net_log_.EndEventWithNetErrorCode(NetLogEventType::HTTP_CACHE_READ_DATA,
                                      result);
  }

  if (!cache_.get()) {
    TransitionToState(STATE_NONE);
    return ERR_UNEXPECTED;
  }

  if (partial_) {
    // Partial requests are confusing to report in histograms because they may
    // have multiple underlying requests.
    UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_OTHER);
    return DoPartialCacheReadCompleted(result);
  }

  if (result > 0) {
    read_offset_ += result;
  } else if (result == 0) {  // End of file.
    DoneWithEntry(true);
  } else {
    return OnCacheReadError(result, false);
  }

  TransitionToState(STATE_NONE);
  return result;
}

//-----------------------------------------------------------------------------

void HttpCache::Transaction::SetRequest(const NetLogWithSource& net_log) {
  net_log_ = net_log;

  // Reset the variables that might get set in this function. This is done
  // because this function can be invoked multiple times for a transaction.
  cache_entry_status_ = CacheEntryStatus::ENTRY_UNDEFINED;
  external_validation_.Reset();
  range_requested_ = false;
  partial_.reset();

  request_ = initial_request_;
  custom_request_.reset();

  effective_load_flags_ = request_->load_flags;
  method_ = request_->method;

  if (cache_->mode() == DISABLE)
    effective_load_flags_ |= LOAD_DISABLE_CACHE;

  // Some headers imply load flags.  The order here is significant.
  //
  //   LOAD_DISABLE_CACHE   : no cache read or write
  //   LOAD_BYPASS_CACHE    : no cache read
  //   LOAD_VALIDATE_CACHE  : no cache read unless validation
  //
  // The former modes trump latter modes, so if we find a matching header we
  // can stop iterating kSpecialHeaders.
  //
  static const struct {
    const HeaderNameAndValue* search;
    int load_flag;
  } kSpecialHeaders[] = {
    { kPassThroughHeaders, LOAD_DISABLE_CACHE },
    { kForceFetchHeaders, LOAD_BYPASS_CACHE },
    { kForceValidateHeaders, LOAD_VALIDATE_CACHE },
  };

  bool range_found = false;
  bool external_validation_error = false;
  bool special_headers = false;

  if (request_->extra_headers.HasHeader(HttpRequestHeaders::kRange))
    range_found = true;

  for (size_t i = 0; i < arraysize(kSpecialHeaders); ++i) {
    if (HeaderMatches(request_->extra_headers, kSpecialHeaders[i].search)) {
      effective_load_flags_ |= kSpecialHeaders[i].load_flag;
      special_headers = true;
      break;
    }
  }

  // Check for conditionalization headers which may correspond with a
  // cache validation request.
  for (size_t i = 0; i < arraysize(kValidationHeaders); ++i) {
    const ValidationHeaderInfo& info = kValidationHeaders[i];
    std::string validation_value;
    if (request_->extra_headers.GetHeader(
            info.request_header_name, &validation_value)) {
      if (!external_validation_.values[i].empty() ||
          validation_value.empty()) {
        external_validation_error = true;
      }
      external_validation_.values[i] = validation_value;
      external_validation_.initialized = true;
    }
  }

  if (range_found || special_headers || external_validation_.initialized) {
    // Log the headers before request_ is modified.
    std::string empty;
    net_log_.AddEvent(
        NetLogEventType::HTTP_CACHE_CALLER_REQUEST_HEADERS,
        base::Bind(&HttpRequestHeaders::NetLogCallback,
                   base::Unretained(&request_->extra_headers), &empty));
  }

  // We don't support ranges and validation headers.
  if (range_found && external_validation_.initialized) {
    LOG(WARNING) << "Byte ranges AND validation headers found.";
    effective_load_flags_ |= LOAD_DISABLE_CACHE;
  }

  // If there is more than one validation header, we can't treat this request as
  // a cache validation, since we don't know for sure which header the server
  // will give us a response for (and they could be contradictory).
  if (external_validation_error) {
    LOG(WARNING) << "Multiple or malformed validation headers found.";
    effective_load_flags_ |= LOAD_DISABLE_CACHE;
  }

  if (range_found && !(effective_load_flags_ & LOAD_DISABLE_CACHE)) {
    UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_OTHER);
    partial_.reset(new PartialData);
    if (method_ == "GET" && partial_->Init(request_->extra_headers)) {
      // We will be modifying the actual range requested to the server, so
      // let's remove the header here.
      // Note that custom_request_ is a shallow copy so will keep the same
      // pointer to upload data stream as in the original request.
      custom_request_.reset(new HttpRequestInfo(*request_));
      custom_request_->extra_headers.RemoveHeader(HttpRequestHeaders::kRange);
      request_ = custom_request_.get();
      partial_->SetHeaders(custom_request_->extra_headers);
    } else {
      // The range is invalid or we cannot handle it properly.
      VLOG(1) << "Invalid byte range found.";
      effective_load_flags_ |= LOAD_DISABLE_CACHE;
      partial_.reset(NULL);
    }
  }
}

bool HttpCache::Transaction::ShouldPassThrough() {
  // We may have a null disk_cache if there is an error we cannot recover from,
  // like not enough disk space, or sharing violations.
  if (!cache_->disk_cache_.get())
    return true;

  if (effective_load_flags_ & LOAD_DISABLE_CACHE)
    return true;

  if (method_ == "GET" || method_ == "HEAD")
    return false;

  if (method_ == "POST" && request_->upload_data_stream &&
      request_->upload_data_stream->identifier()) {
    return false;
  }

  if (method_ == "PUT" && request_->upload_data_stream)
    return false;

  if (method_ == "DELETE")
    return false;

  return true;
}

int HttpCache::Transaction::BeginCacheRead() {
  // We don't support any combination of LOAD_ONLY_FROM_CACHE and byte ranges.
  // TODO(jkarlin): Either handle this case or DCHECK.
  if (response_.headers->response_code() == 206 || partial_) {
    NOTREACHED();
    TransitionToState(STATE_FINISH_HEADERS);
    return ERR_CACHE_MISS;
  }

  // We don't have the whole resource.
  if (truncated_) {
    TransitionToState(STATE_FINISH_HEADERS);
    return ERR_CACHE_MISS;
  }

  if (RequiresValidation()) {
    TransitionToState(STATE_FINISH_HEADERS);
    return ERR_CACHE_MISS;
  }

  if (method_ == "HEAD")
    FixHeadersForHead();

  if (entry_->disk_entry->GetDataSize(kMetadataIndex))
    TransitionToState(STATE_CACHE_READ_METADATA);
  else
    TransitionToState(STATE_FINISH_HEADERS);

  return OK;
}

int HttpCache::Transaction::BeginCacheValidation() {
  DCHECK_EQ(mode_, READ_WRITE);

  bool skip_validation = !RequiresValidation();

  if (method_ == "HEAD" &&
      (truncated_ || response_.headers->response_code() == 206)) {
    DCHECK(!partial_);
    if (skip_validation)
      return SetupEntryForRead();

    // Bail out!
    TransitionToState(STATE_SEND_REQUEST);
    mode_ = NONE;
    return OK;
  }

  if (truncated_) {
    // Truncated entries can cause partial gets, so we shouldn't record this
    // load in histograms.
    UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_OTHER);
    skip_validation = !partial_->initial_validation();
  }

  if (partial_ && (is_sparse_ || truncated_) &&
      (!partial_->IsCurrentRangeCached() || invalid_range_)) {
    // Force revalidation for sparse or truncated entries. Note that we don't
    // want to ignore the regular validation logic just because a byte range was
    // part of the request.
    skip_validation = false;
  }

  if (skip_validation) {
    UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_USED);
    return SetupEntryForRead();
  } else {
    // Make the network request conditional, to see if we may reuse our cached
    // response.  If we cannot do so, then we just resort to a normal fetch.
    // Our mode remains READ_WRITE for a conditional request.  Even if the
    // conditionalization fails, we don't switch to WRITE mode until we
    // know we won't be falling back to using the cache entry in the
    // LOAD_FROM_CACHE_IF_OFFLINE case.
    if (!ConditionalizeRequest()) {
      couldnt_conditionalize_request_ = true;
      UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_CANT_CONDITIONALIZE);
      if (partial_)
        return DoRestartPartialRequest();

      DCHECK_NE(206, response_.headers->response_code());
    }
    TransitionToState(STATE_SEND_REQUEST);
  }
  return OK;
}

int HttpCache::Transaction::BeginPartialCacheValidation() {
  DCHECK_EQ(mode_, READ_WRITE);

  if (response_.headers->response_code() != 206 && !partial_ && !truncated_)
    return BeginCacheValidation();

  // Partial requests should not be recorded in histograms.
  UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_OTHER);
  if (method_ == "HEAD")
    return BeginCacheValidation();

  if (!range_requested_) {
    // The request is not for a range, but we have stored just ranges.

    partial_.reset(new PartialData());
    partial_->SetHeaders(request_->extra_headers);
    if (!custom_request_.get()) {
      custom_request_.reset(new HttpRequestInfo(*request_));
      request_ = custom_request_.get();
    }
  }

  TransitionToState(STATE_CACHE_QUERY_DATA);
  return OK;
}

// This should only be called once per request.
int HttpCache::Transaction::ValidateEntryHeadersAndContinue() {
  DCHECK_EQ(mode_, READ_WRITE);

  if (!partial_->UpdateFromStoredHeaders(
          response_.headers.get(), entry_->disk_entry, truncated_)) {
    return DoRestartPartialRequest();
  }

  if (response_.headers->response_code() == 206)
    is_sparse_ = true;

  if (!partial_->IsRequestedRangeOK()) {
    // The stored data is fine, but the request may be invalid.
    invalid_range_ = true;
  }

  TransitionToState(STATE_START_PARTIAL_CACHE_VALIDATION);
  return OK;
}

int HttpCache::Transaction::BeginExternallyConditionalizedRequest() {
  DCHECK_EQ(UPDATE, mode_);
  DCHECK(external_validation_.initialized);

  for (size_t i = 0;  i < arraysize(kValidationHeaders); i++) {
    if (external_validation_.values[i].empty())
      continue;
    // Retrieve either the cached response's "etag" or "last-modified" header.
    std::string validator;
    response_.headers->EnumerateHeader(
        NULL,
        kValidationHeaders[i].related_response_header_name,
        &validator);

    if (response_.headers->response_code() != 200 || truncated_ ||
        validator.empty() || validator != external_validation_.values[i]) {
      // The externally conditionalized request is not a validation request
      // for our existing cache entry. Proceed with caching disabled.
      UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_OTHER);
      DoneWithEntry(true);
    }
  }

  TransitionToState(STATE_SEND_REQUEST);
  return OK;
}

int HttpCache::Transaction::RestartNetworkRequest() {
  DCHECK(mode_ & WRITE || mode_ == NONE);
  DCHECK(network_trans_.get());
  DCHECK_EQ(STATE_NONE, next_state_);

  next_state_ = STATE_SEND_REQUEST_COMPLETE;
  int rv = network_trans_->RestartIgnoringLastError(io_callback_);
  if (rv != ERR_IO_PENDING)
    return DoLoop(rv);
  return rv;
}

int HttpCache::Transaction::RestartNetworkRequestWithCertificate(
    scoped_refptr<X509Certificate> client_cert,
    scoped_refptr<SSLPrivateKey> client_private_key) {
  DCHECK(mode_ & WRITE || mode_ == NONE);
  DCHECK(network_trans_.get());
  DCHECK_EQ(STATE_NONE, next_state_);

  next_state_ = STATE_SEND_REQUEST_COMPLETE;
  int rv = network_trans_->RestartWithCertificate(
      std::move(client_cert), std::move(client_private_key), io_callback_);
  if (rv != ERR_IO_PENDING)
    return DoLoop(rv);
  return rv;
}

int HttpCache::Transaction::RestartNetworkRequestWithAuth(
    const AuthCredentials& credentials) {
  DCHECK(mode_ & WRITE || mode_ == NONE);
  DCHECK(network_trans_.get());
  DCHECK_EQ(STATE_NONE, next_state_);

  next_state_ = STATE_SEND_REQUEST_COMPLETE;
  int rv = network_trans_->RestartWithAuth(credentials, io_callback_);
  if (rv != ERR_IO_PENDING)
    return DoLoop(rv);
  return rv;
}

bool HttpCache::Transaction::RequiresValidation() {
  // TODO(darin): need to do more work here:
  //  - make sure we have a matching request method
  //  - watch out for cached responses that depend on authentication

  if (!(effective_load_flags_ & LOAD_SKIP_VARY_CHECK) &&
      response_.vary_data.is_valid() &&
      !response_.vary_data.MatchesRequest(*request_,
                                          *response_.headers.get())) {
    vary_mismatch_ = true;
    validation_cause_ = VALIDATION_CAUSE_VARY_MISMATCH;
    return true;
  }

  if (effective_load_flags_ & LOAD_SKIP_CACHE_VALIDATION)
    return false;

  if (response_.unused_since_prefetch &&
      !(effective_load_flags_ & LOAD_PREFETCH) &&
      response_.headers->GetCurrentAge(
          response_.request_time, response_.response_time,
          cache_->clock_->Now()) < TimeDelta::FromMinutes(kPrefetchReuseMins)) {
    // The first use of a resource after prefetch within a short window skips
    // validation.
    return false;
  }

  if (effective_load_flags_ & LOAD_VALIDATE_CACHE) {
    validation_cause_ = VALIDATION_CAUSE_VALIDATE_FLAG;
    return true;
  }

  if (method_ == "PUT" || method_ == "DELETE")
    return true;

  bool validation_required_by_headers = response_.headers->RequiresValidation(
      response_.request_time, response_.response_time, cache_->clock_->Now());

  if (validation_required_by_headers) {
    HttpResponseHeaders::FreshnessLifetimes lifetimes =
        response_.headers->GetFreshnessLifetimes(response_.response_time);
    if (lifetimes.freshness == base::TimeDelta()) {
      validation_cause_ = VALIDATION_CAUSE_ZERO_FRESHNESS;
    } else {
      validation_cause_ = VALIDATION_CAUSE_STALE;
      stale_entry_freshness_ = lifetimes.freshness;
      stale_entry_age_ = response_.headers->GetCurrentAge(
          response_.request_time, response_.response_time,
          cache_->clock_->Now());
    }
  }

  return validation_required_by_headers;
}

bool HttpCache::Transaction::IsResponseConditionalizable(
    std::string* etag_value,
    std::string* last_modified_value) const {
  DCHECK(response_.headers.get());

  // This only makes sense for cached 200 or 206 responses.
  if (response_.headers->response_code() != 200 &&
      response_.headers->response_code() != 206) {
    return false;
  }

  // Just use the first available ETag and/or Last-Modified header value.
  // TODO(darin): Or should we use the last?

  if (response_.headers->GetHttpVersion() >= HttpVersion(1, 1))
    response_.headers->EnumerateHeader(NULL, "etag", etag_value);

  response_.headers->EnumerateHeader(NULL, "last-modified",
                                     last_modified_value);

  if (etag_value->empty() && last_modified_value->empty())
    return false;

  return true;
}

bool HttpCache::Transaction::ConditionalizeRequest() {
  DCHECK(response_.headers.get());

  if (method_ == "PUT" || method_ == "DELETE")
    return false;

  if (fail_conditionalization_for_test_)
    return false;

  std::string etag_value;
  std::string last_modified_value;
  if (!IsResponseConditionalizable(&etag_value, &last_modified_value))
    return false;

  DCHECK(response_.headers->response_code() != 206 ||
         response_.headers->HasStrongValidators());

  if (vary_mismatch_) {
    // Can't rely on last-modified if vary is different.
    last_modified_value.clear();
    if (etag_value.empty())
      return false;
  }

  if (!partial_) {
    // Need to customize the request, so this forces us to allocate :(
    custom_request_.reset(new HttpRequestInfo(*request_));
    request_ = custom_request_.get();
  }
  DCHECK(custom_request_.get());

  bool use_if_range =
      partial_ && !partial_->IsCurrentRangeCached() && !invalid_range_;

  if (!etag_value.empty()) {
    if (use_if_range) {
      // We don't want to switch to WRITE mode if we don't have this block of a
      // byte-range request because we may have other parts cached.
      custom_request_->extra_headers.SetHeader(
          HttpRequestHeaders::kIfRange, etag_value);
    } else {
      custom_request_->extra_headers.SetHeader(
          HttpRequestHeaders::kIfNoneMatch, etag_value);
    }
    // For byte-range requests, make sure that we use only one way to validate
    // the request.
    if (partial_ && !partial_->IsCurrentRangeCached())
      return true;
  }

  if (!last_modified_value.empty()) {
    if (use_if_range) {
      custom_request_->extra_headers.SetHeader(
          HttpRequestHeaders::kIfRange, last_modified_value);
    } else {
      custom_request_->extra_headers.SetHeader(
          HttpRequestHeaders::kIfModifiedSince, last_modified_value);
    }
  }

  return true;
}

bool HttpCache::Transaction::MaybeRejectBasedOnEntryInMemoryData(
    uint8_t in_memory_info) {
  // Not going to be clever with those...
  if (partial_)
    return false;

  // Avoiding open based on in-memory hints requires us to be permitted to
  // modify the cache, including deleting an old entry. Only the READ_WRITE
  // and WRITE modes permit that... and WRITE never tries to open entries in the
  // first place, so we shouldn't see it here.
  DCHECK_NE(mode_, WRITE);
  if (mode_ != READ_WRITE)
    return false;

  // If we are loading ignoring cache validity (aka back button), obviously
  // can't reject things based on it.  Also if LOAD_ONLY_FROM_CACHE there is no
  // hope of network offering anything better.
  if (effective_load_flags_ & LOAD_SKIP_CACHE_VALIDATION ||
      effective_load_flags_ & LOAD_ONLY_FROM_CACHE)
    return false;

  return (in_memory_info & HINT_UNUSABLE_PER_CACHING_HEADERS) ==
         HINT_UNUSABLE_PER_CACHING_HEADERS;
}

bool HttpCache::Transaction::ComputeUnusablePerCachingHeaders() {
  // unused_since_prefetch overrides some caching headers, so it may be useful
  // regardless of what they say.
  if (response_.unused_since_prefetch)
    return false;

  // Has an e-tag or last-modified: we can probably send a conditional request,
  // so it's potentially useful.
  std::string etag_ignored, last_modified_ignored;
  if (IsResponseConditionalizable(&etag_ignored, &last_modified_ignored))
    return false;

  // If none of the above is true and the entry has zero freshness, then it
  // won't be usable absent load flag override.
  return response_.headers->GetFreshnessLifetimes(response_.response_time)
      .freshness.is_zero();
}

// We just received some headers from the server. We may have asked for a range,
// in which case partial_ has an object. This could be the first network request
// we make to fulfill the original request, or we may be already reading (from
// the net and / or the cache). If we are not expecting a certain response, we
// just bypass the cache for this request (but again, maybe we are reading), and
// delete partial_ (so we are not able to "fix" the headers that we return to
// the user). This results in either a weird response for the caller (we don't
// expect it after all), or maybe a range that was not exactly what it was asked
// for.
//
// If the server is simply telling us that the resource has changed, we delete
// the cached entry and restart the request as the caller intended (by returning
// false from this method). However, we may not be able to do that at any point,
// for instance if we already returned the headers to the user.
//
// WARNING: Whenever this code returns false, it has to make sure that the next
// time it is called it will return true so that we don't keep retrying the
// request.
bool HttpCache::Transaction::ValidatePartialResponse() {
  const HttpResponseHeaders* headers = new_response_->headers.get();
  int response_code = headers->response_code();
  bool partial_response = (response_code == 206);
  handling_206_ = false;

  if (!entry_ || method_ != "GET")
    return true;

  if (invalid_range_) {
    // We gave up trying to match this request with the stored data. If the
    // server is ok with the request, delete the entry, otherwise just ignore
    // this request
    DCHECK(!reading_);
    if (partial_response || response_code == 200) {
      DoomPartialEntry(true);
      mode_ = NONE;
    } else {
      if (response_code == 304) {
        // Change the response code of the request to be 416 (Requested range
        // not satisfiable).
        SetResponse(*new_response_);
        partial_->FixResponseHeaders(response_.headers.get(), false);
      }
      IgnoreRangeRequest();
    }
    return true;
  }

  if (!partial_) {
    // We are not expecting 206 but we may have one.
    if (partial_response)
      IgnoreRangeRequest();

    return true;
  }

  // TODO(rvargas): Do we need to consider other results here?.
  bool failure = response_code == 200 || response_code == 416;

  if (partial_->IsCurrentRangeCached()) {
    // We asked for "If-None-Match: " so a 206 means a new object.
    if (partial_response)
      failure = true;

    if (response_code == 304 && partial_->ResponseHeadersOK(headers))
      return true;
  } else {
    // We asked for "If-Range: " so a 206 means just another range.
    if (partial_response) {
      if (partial_->ResponseHeadersOK(headers)) {
        handling_206_ = true;
        return true;
      } else {
        failure = true;
      }
    }

    if (!reading_ && !is_sparse_ && !partial_response) {
      // See if we can ignore the fact that we issued a byte range request.
      // If the server sends 200, just store it. If it sends an error, redirect
      // or something else, we may store the response as long as we didn't have
      // anything already stored.
      if (response_code == 200 ||
          (!truncated_ && response_code != 304 && response_code != 416)) {
        // The server is sending something else, and we can save it.
        DCHECK((truncated_ && !partial_->IsLastRange()) || range_requested_);
        partial_.reset();
        truncated_ = false;
        return true;
      }
    }

    // 304 is not expected here, but we'll spare the entry (unless it was
    // truncated).
    if (truncated_)
      failure = true;
  }

  if (failure) {
    // We cannot truncate this entry, it has to be deleted.
    UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_OTHER);
    mode_ = NONE;
    if (is_sparse_ || truncated_) {
      // There was something cached to start with, either sparsed data (206), or
      // a truncated 200, which means that we probably modified the request,
      // adding a byte range or modifying the range requested by the caller.
      if (!reading_ && !partial_->IsLastRange()) {
        // We have not returned anything to the caller yet so it should be safe
        // to issue another network request, this time without us messing up the
        // headers.
        ResetPartialState(true);
        return false;
      }
      LOG(WARNING) << "Failed to revalidate partial entry";
    }
    DoomPartialEntry(true);
    return true;
  }

  IgnoreRangeRequest();
  return true;
}

void HttpCache::Transaction::IgnoreRangeRequest() {
  // We have a problem. We may or may not be reading already (in which case we
  // returned the headers), but we'll just pretend that this request is not
  // using the cache and see what happens. Most likely this is the first
  // response from the server (it's not changing its mind midway, right?).
  UpdateCacheEntryStatus(CacheEntryStatus::ENTRY_OTHER);
  DoneWithEntry(mode_ != WRITE);
  partial_.reset(NULL);
}

void HttpCache::Transaction::FixHeadersForHead() {
  if (response_.headers->response_code() == 206) {
    response_.headers->RemoveHeader("Content-Range");
    response_.headers->ReplaceStatusLine("HTTP/1.1 200 OK");
  }
}

int HttpCache::Transaction::SetupEntryForRead() {
  if (network_trans_)
    ResetNetworkTransaction();
  if (partial_) {
    if (truncated_ || is_sparse_ || !invalid_range_) {
      // We are going to return the saved response headers to the caller, so
      // we may need to adjust them first.
      TransitionToState(STATE_PARTIAL_HEADERS_RECEIVED);
      return OK;
    } else {
      partial_.reset();
    }
  }

  if (!cache_->IsWritingInProgress(entry_))
    mode_ = READ;

  if (method_ == "HEAD")
    FixHeadersForHead();

  if (entry_->disk_entry->GetDataSize(kMetadataIndex))
    TransitionToState(STATE_CACHE_READ_METADATA);
  else
    TransitionToState(STATE_FINISH_HEADERS);
  return OK;
}

int HttpCache::Transaction::WriteToEntry(int index, int offset,
                                         IOBuffer* data, int data_len,
                                         const CompletionCallback& callback) {
  if (!entry_)
    return data_len;

  int rv = 0;
  if (!partial_ || !data_len) {
    rv = entry_->disk_entry->WriteData(index, offset, data, data_len, callback,
                                       true);
  } else {
    rv = partial_->CacheWrite(entry_->disk_entry, data, data_len, callback);
  }
  return rv;
}

int HttpCache::Transaction::WriteResponseInfoToEntry(bool truncated) {
  if (!entry_)
    return OK;

  if (net_log_.IsCapturing())
    net_log_.BeginEvent(NetLogEventType::HTTP_CACHE_WRITE_INFO);

  // Do not cache no-store content.  Do not cache content with cert errors
  // either.  This is to prevent not reporting net errors when loading a
  // resource from the cache.  When we load a page over HTTPS with a cert error
  // we show an SSL blocking page.  If the user clicks proceed we reload the
  // resource ignoring the errors.  The loaded resource is then cached.  If that
  // resource is subsequently loaded from the cache, no net error is reported
  // (even though the cert status contains the actual errors) and no SSL
  // blocking page is shown.  An alternative would be to reverse-map the cert
  // status to a net error and replay the net error.
  if ((response_.headers->HasHeaderValue("cache-control", "no-store")) ||
      IsCertStatusError(response_.ssl_info.cert_status)) {
    bool stopped = StopCachingImpl(false);
    DCHECK(stopped);
    if (net_log_.IsCapturing())
      net_log_.EndEvent(NetLogEventType::HTTP_CACHE_WRITE_INFO);
    return OK;
  }

  if (truncated)
    DCHECK_EQ(200, response_.headers->response_code());

  // When writing headers, we normally only write the non-transient headers.
  bool skip_transient_headers = true;
  scoped_refptr<PickledIOBuffer> data(new PickledIOBuffer());
  response_.Persist(data->pickle(), skip_transient_headers, truncated);
  data->Done();

  io_buf_len_ = data->pickle()->size();

  // Summarize some info on cacheability in memory. Don't do it if doomed
  // since then |entry_| isn't definitive for |cache_key_|.
  if (!entry_->doomed) {
    cache_->GetCurrentBackend()->SetEntryInMemoryData(
        cache_key_, ComputeUnusablePerCachingHeaders()
                        ? HINT_UNUSABLE_PER_CACHING_HEADERS
                        : 0);
  }

  return entry_->disk_entry->WriteData(kResponseInfoIndex, 0, data.get(),
                                       io_buf_len_, io_callback_, true);
}

int HttpCache::Transaction::OnWriteResponseInfoToEntryComplete(int result) {
  if (!entry_)
    return OK;
  if (net_log_.IsCapturing()) {
    net_log_.EndEventWithNetErrorCode(NetLogEventType::HTTP_CACHE_WRITE_INFO,
                                      result);
  }

  if (result != io_buf_len_) {
    DLOG(ERROR) << "failed to write response info to cache";
    DoneWithEntry(false);
  }
  return OK;
}

bool HttpCache::Transaction::StopCachingImpl(bool success) {
  bool stopped = false;
  // Let writers know so that it doesn't attempt to write to the cache.
  if (InWriters()) {
    stopped = entry_->writers->StopCaching(success /* keep_entry */);
    if (stopped)
      mode_ = NONE;
  } else if (entry_) {
    stopped = true;
    DoneWithEntry(success /* entry_is_complete */);
  }
  return stopped;
}

void HttpCache::Transaction::DoneWithEntry(bool entry_is_complete) {
  if (!entry_)
    return;

  // For a writer, histograms will be recorded in
  // WriterAboutToBeRemovedFromEntry.
  if (!InWriters())
    RecordHistograms();

  cache_->DoneWithEntry(entry_, this, entry_is_complete, partial_ != nullptr);
  entry_ = nullptr;
  mode_ = NONE;  // switch to 'pass through' mode
}

int HttpCache::Transaction::OnCacheReadError(int result, bool restart) {
  DLOG(ERROR) << "ReadData failed: " << result;
  const int result_for_histogram = std::max(0, -result);
  if (restart) {
    base::UmaHistogramSparse("HttpCache.ReadErrorRestartable",
                             result_for_histogram);
  } else {
    base::UmaHistogramSparse("HttpCache.ReadErrorNonRestartable",
                             result_for_histogram);
  }

  // Avoid using this entry in the future.
  if (cache_.get())
    cache_->DoomActiveEntry(cache_key_);

  if (restart) {
    DCHECK(!reading_);
    DCHECK(!network_trans_.get());

    // Since we are going to add this to a new entry, not recording histograms
    // or setting mode to NONE at this point by invoking the wrapper
    // DoneWithEntry.
    cache_->DoneWithEntry(entry_, this, true /* entry_is_complete */,
                          partial_ != nullptr);
    entry_ = NULL;
    is_sparse_ = false;
    partial_.reset();
    TransitionToState(STATE_GET_BACKEND);
    return OK;
  }

  TransitionToState(STATE_NONE);
  return ERR_CACHE_READ_FAILURE;
}

void HttpCache::Transaction::OnCacheLockTimeout(base::TimeTicks start_time) {
  if (entry_lock_waiting_since_ != start_time)
    return;

  DCHECK(next_state_ == STATE_ADD_TO_ENTRY_COMPLETE ||
         next_state_ == STATE_FINISH_HEADERS_COMPLETE);

  if (!cache_)
    return;

  if (next_state_ == STATE_ADD_TO_ENTRY_COMPLETE)
    cache_->RemovePendingTransaction(this);
  else
    DoneWithEntry(false /* entry_is_complete */);
  OnIOComplete(ERR_CACHE_LOCK_TIMEOUT);
}

void HttpCache::Transaction::DoomPartialEntry(bool delete_object) {
  DVLOG(2) << "DoomPartialEntry";
  if (entry_ && !entry_->doomed) {
    int rv = cache_->DoomEntry(cache_key_, NULL);
    DCHECK_EQ(OK, rv);
  }

  cache_->DoneWithEntry(entry_, this, false /* entry_is_complete */,
                        partial_ != nullptr);
  entry_ = NULL;
  is_sparse_ = false;
  truncated_ = false;
  if (delete_object)
    partial_.reset(NULL);
}

int HttpCache::Transaction::DoPartialCacheReadCompleted(int result) {
  partial_->OnCacheReadCompleted(result);

  if (result == 0 && mode_ == READ_WRITE) {
    // We need to move on to the next range.
    TransitionToState(STATE_START_PARTIAL_CACHE_VALIDATION);
  } else if (result < 0) {
    return OnCacheReadError(result, false);
  } else {
    TransitionToState(STATE_NONE);
  }
  return result;
}

int HttpCache::Transaction::DoRestartPartialRequest() {
  // The stored data cannot be used. Get rid of it and restart this request.
  net_log_.AddEvent(NetLogEventType::HTTP_CACHE_RESTART_PARTIAL_REQUEST);

  // WRITE + Doom + STATE_INIT_ENTRY == STATE_CREATE_ENTRY (without an attempt
  // to Doom the entry again).
  mode_ = WRITE;
  ResetPartialState(!range_requested_);
  TransitionToState(STATE_CREATE_ENTRY);
  return OK;
}

void HttpCache::Transaction::ResetPartialState(bool delete_object) {
  partial_->RestoreHeaders(&custom_request_->extra_headers);
  DoomPartialEntry(delete_object);

  if (!delete_object) {
    // The simplest way to re-initialize partial_ is to create a new object.
    partial_.reset(new PartialData());

    // Reset the range header to the original value (http://crbug.com/820599).
    custom_request_->extra_headers.RemoveHeader(HttpRequestHeaders::kRange);
    if (partial_->Init(initial_request_->extra_headers))
      partial_->SetHeaders(custom_request_->extra_headers);
    else
      partial_.reset();
  }
}

void HttpCache::Transaction::ResetNetworkTransaction() {
  SaveNetworkTransactionInfo(*network_trans_);
  network_trans_.reset();
}

const HttpTransaction* HttpCache::Transaction::network_transaction() const {
  if (network_trans_)
    return network_trans_.get();
  if (InWriters())
    return entry_->writers->network_transaction();
  return nullptr;
}

const HttpTransaction*
HttpCache::Transaction::GetOwnedOrMovedNetworkTransaction() const {
  if (network_trans_)
    return network_trans_.get();
  if (InWriters() && moved_network_transaction_to_writers_)
    return entry_->writers->network_transaction();
  return nullptr;
}

HttpTransaction* HttpCache::Transaction::network_transaction() {
  return const_cast<HttpTransaction*>(
      static_cast<const Transaction*>(this)->network_transaction());
}

// Histogram data from the end of 2010 show the following distribution of
// response headers:
//
//   Content-Length............... 87%
//   Date......................... 98%
//   Last-Modified................ 49%
//   Etag......................... 19%
//   Accept-Ranges: bytes......... 25%
//   Accept-Ranges: none.......... 0.4%
//   Strong Validator............. 50%
//   Strong Validator + ranges.... 24%
//   Strong Validator + CL........ 49%
//
bool HttpCache::Transaction::CanResume(bool has_data) {
  // Double check that there is something worth keeping.
  if (has_data && !entry_->disk_entry->GetDataSize(kResponseContentIndex))
    return false;

  if (method_ != "GET")
    return false;

  // Note that if this is a 206, content-length was already fixed after calling
  // PartialData::ResponseHeadersOK().
  if (response_.headers->GetContentLength() <= 0 ||
      response_.headers->HasHeaderValue("Accept-Ranges", "none") ||
      !response_.headers->HasStrongValidators()) {
    return false;
  }

  return true;
}

void HttpCache::Transaction::SetResponse(const HttpResponseInfo& response) {
  response_ = response;
  SyncCacheEntryStatusToResponse();
}

void HttpCache::Transaction::SetAuthResponse(
    const HttpResponseInfo& auth_response) {
  auth_response_ = auth_response;
  SyncCacheEntryStatusToResponse();
}

void HttpCache::Transaction::UpdateCacheEntryStatus(
    CacheEntryStatus new_cache_entry_status) {
  DCHECK_NE(CacheEntryStatus::ENTRY_UNDEFINED, new_cache_entry_status);
  if (cache_entry_status_ == CacheEntryStatus::ENTRY_OTHER)
    return;
  DCHECK(cache_entry_status_ == CacheEntryStatus::ENTRY_UNDEFINED ||
         new_cache_entry_status == CacheEntryStatus::ENTRY_OTHER);
  cache_entry_status_ = new_cache_entry_status;
  SyncCacheEntryStatusToResponse();
}

void HttpCache::Transaction::SyncCacheEntryStatusToResponse() {
  if (cache_entry_status_ == CacheEntryStatus::ENTRY_UNDEFINED)
    return;
  response_.cache_entry_status = cache_entry_status_;
  if (auth_response_.headers.get()) {
    auth_response_.cache_entry_status = cache_entry_status_;
  }
}

void HttpCache::Transaction::RecordHistograms() {
  DCHECK(!recorded_histograms_);
  recorded_histograms_ = true;

  UMA_HISTOGRAM_ENUMERATION("HttpCache.ParallelWritingPattern",
                            parallel_writing_pattern_, PARALLEL_WRITING_MAX);

  if (CacheEntryStatus::ENTRY_UNDEFINED == cache_entry_status_)
    return;

  if (!cache_.get() || !cache_->GetCurrentBackend() ||
      cache_->GetCurrentBackend()->GetCacheType() != DISK_CACHE ||
      cache_->mode() != NORMAL || method_ != "GET") {
    return;
  }

  bool validation_request =
      cache_entry_status_ == CacheEntryStatus::ENTRY_VALIDATED ||
      cache_entry_status_ == CacheEntryStatus::ENTRY_UPDATED;

  bool stale_request =
      validation_cause_ == VALIDATION_CAUSE_STALE &&
      (validation_request ||
       cache_entry_status_ == CacheEntryStatus::ENTRY_CANT_CONDITIONALIZE);
  int64_t freshness_periods_since_last_used = 0;

  if (stale_request && !open_entry_last_used_.is_null()) {
    // Note that we are not able to capture those transactions' histograms which
    // when added to entry, the response was being written by another
    // transaction because getting the last used timestamp might lead to a data
    // race in that case. TODO(crbug.com/713354).

    // For stale entries, record how many freshness periods have elapsed since
    // the entry was last used.
    DCHECK(!stale_entry_freshness_.is_zero());
    base::TimeDelta time_since_use = base::Time::Now() - open_entry_last_used_;
    freshness_periods_since_last_used =
        (time_since_use * 1000) / stale_entry_freshness_;

    if (validation_request) {
      int64_t age_in_freshness_periods =
          (stale_entry_age_ * 100) / stale_entry_freshness_;
      if (cache_entry_status_ == CacheEntryStatus::ENTRY_VALIDATED) {
        UMA_HISTOGRAM_COUNTS_1M("HttpCache.StaleEntry.Validated.Age",
                                stale_entry_age_.InSeconds());
        UMA_HISTOGRAM_COUNTS_1M(
            "HttpCache.StaleEntry.Validated.AgeInFreshnessPeriods",
            age_in_freshness_periods);

      } else {
        UMA_HISTOGRAM_COUNTS_1M("HttpCache.StaleEntry.Updated.Age",
                                stale_entry_age_.InSeconds());
        UMA_HISTOGRAM_COUNTS_1M(
            "HttpCache.StaleEntry.Updated.AgeInFreshnessPeriods",
            age_in_freshness_periods);
      }
    }
  }

  std::string mime_type;
  HttpResponseHeaders* response_headers = GetResponseInfo()->headers.get();
  if (response_headers && response_headers->GetMimeType(&mime_type)) {
    // Record the cache pattern by resource type. The type is inferred by
    // response header mime type, which could be incorrect, so this is just an
    // estimate.
    if (mime_type == "text/html" &&
        (effective_load_flags_ & LOAD_MAIN_FRAME_DEPRECATED)) {
      CACHE_STATUS_HISTOGRAMS(".MainFrameHTML");
    } else if (mime_type == "text/html") {
      CACHE_STATUS_HISTOGRAMS(".NonMainFrameHTML");
    } else if (mime_type == "text/css") {
      CACHE_STATUS_HISTOGRAMS(".CSS");
    } else if (base::StartsWith(mime_type, "image/",
                                base::CompareCase::SENSITIVE)) {
      int64_t content_length = response_headers->GetContentLength();
      if (content_length >= 0 && content_length < 100) {
        CACHE_STATUS_HISTOGRAMS(".TinyImage");
      } else if (content_length >= 100) {
        CACHE_STATUS_HISTOGRAMS(".NonTinyImage");
      }
      CACHE_STATUS_HISTOGRAMS(".Image");
    } else if (base::EndsWith(mime_type, "javascript",
                              base::CompareCase::SENSITIVE) ||
               base::EndsWith(mime_type, "ecmascript",
                              base::CompareCase::SENSITIVE)) {
      CACHE_STATUS_HISTOGRAMS(".JavaScript");
    } else if (mime_type.find("font") != std::string::npos) {
      CACHE_STATUS_HISTOGRAMS(".Font");
    } else if (base::StartsWith(mime_type, "audio/",
                                base::CompareCase::SENSITIVE)) {
      CACHE_STATUS_HISTOGRAMS(".Audio");
    } else if (base::StartsWith(mime_type, "video/",
                                base::CompareCase::SENSITIVE)) {
      CACHE_STATUS_HISTOGRAMS(".Video");
    }
  }

  CACHE_STATUS_HISTOGRAMS("");

  if (cache_entry_status_ == CacheEntryStatus::ENTRY_CANT_CONDITIONALIZE) {
    UMA_HISTOGRAM_ENUMERATION("HttpCache.CantConditionalizeCause",
                              validation_cause_, VALIDATION_CAUSE_MAX);
    if (validation_cause_ == VALIDATION_CAUSE_ZERO_FRESHNESS) {
      UMA_HISTOGRAM_BOOLEAN(
          "HttpCache.CantConditionalizeZeroFreshnessFromMemHint",
          cant_conditionalize_zero_freshness_from_memhint_);
    }
  }

  if (cache_entry_status_ == CacheEntryStatus::ENTRY_OTHER)
    return;

  DCHECK(!range_requested_) << "Cache entry status " << cache_entry_status_;
  DCHECK(!first_cache_access_since_.is_null());

  base::TimeTicks now = base::TimeTicks::Now();
  TimeDelta total_time = now - first_cache_access_since_;

  UMA_HISTOGRAM_TIMES("HttpCache.AccessToDone", total_time);

  bool did_send_request = !send_request_since_.is_null();
  DCHECK(
      (did_send_request &&
       (cache_entry_status_ == CacheEntryStatus::ENTRY_NOT_IN_CACHE ||
        cache_entry_status_ == CacheEntryStatus::ENTRY_VALIDATED ||
        cache_entry_status_ == CacheEntryStatus::ENTRY_UPDATED ||
        cache_entry_status_ == CacheEntryStatus::ENTRY_CANT_CONDITIONALIZE)) ||
      (!did_send_request &&
       cache_entry_status_ == CacheEntryStatus::ENTRY_USED));

  if (!did_send_request) {
    DCHECK(cache_entry_status_ == CacheEntryStatus::ENTRY_USED);
    UMA_HISTOGRAM_TIMES("HttpCache.AccessToDone.Used", total_time);
    return;
  }

  TimeDelta before_send_time = send_request_since_ - first_cache_access_since_;
  TimeDelta after_send_time = now - send_request_since_;
  int64_t before_send_percent = (total_time.ToInternalValue() == 0)
                                    ? 0
                                    : before_send_time * 100 / total_time;
  DCHECK_GE(before_send_percent, 0);
  DCHECK_LE(before_send_percent, 100);
  base::HistogramBase::Sample before_send_sample =
      static_cast<base::HistogramBase::Sample>(before_send_percent);

  UMA_HISTOGRAM_TIMES("HttpCache.AccessToDone.SentRequest", total_time);
  UMA_HISTOGRAM_TIMES("HttpCache.BeforeSend", before_send_time);
  UMA_HISTOGRAM_PERCENTAGE("HttpCache.PercentBeforeSend", before_send_sample);

  // TODO(gavinp): Remove or minimize these histograms, particularly the ones
  // below this comment after we have received initial data.
  switch (cache_entry_status_) {
    case CacheEntryStatus::ENTRY_CANT_CONDITIONALIZE: {
      UMA_HISTOGRAM_TIMES("HttpCache.BeforeSend.CantConditionalize",
                          before_send_time);
      UMA_HISTOGRAM_TIMES("HttpCache.AfterSend.CantConditionalize",
                          after_send_time);
      UMA_HISTOGRAM_PERCENTAGE("HttpCache.PercentBeforeSend.CantConditionalize",
                               before_send_sample);
      break;
    }
    case CacheEntryStatus::ENTRY_NOT_IN_CACHE: {
      UMA_HISTOGRAM_TIMES("HttpCache.BeforeSend.NotCached", before_send_time);
      UMA_HISTOGRAM_TIMES("HttpCache.AfterSend.NotCached", after_send_time);
      UMA_HISTOGRAM_PERCENTAGE("HttpCache.PercentBeforeSend.NotCached",
                               before_send_sample);
      break;
    }
    case CacheEntryStatus::ENTRY_VALIDATED: {
      UMA_HISTOGRAM_TIMES("HttpCache.BeforeSend.Validated", before_send_time);
      UMA_HISTOGRAM_TIMES("HttpCache.AfterSend.Validated", after_send_time);
      UMA_HISTOGRAM_PERCENTAGE("HttpCache.PercentBeforeSend.Validated",
                               before_send_sample);
      break;
    }
    case CacheEntryStatus::ENTRY_UPDATED: {
      UMA_HISTOGRAM_TIMES("HttpCache.AfterSend.Updated", after_send_time);
      UMA_HISTOGRAM_TIMES("HttpCache.BeforeSend.Updated", before_send_time);
      UMA_HISTOGRAM_PERCENTAGE("HttpCache.PercentBeforeSend.Updated",
                               before_send_sample);
      break;
    }
    default:
      NOTREACHED();
  }
}

bool HttpCache::Transaction::InWriters() const {
  return entry_ && entry_->writers && entry_->writers->HasTransaction(this);
}

HttpCache::Transaction::NetworkTransactionInfo::NetworkTransactionInfo() =
    default;
HttpCache::Transaction::NetworkTransactionInfo::~NetworkTransactionInfo() =
    default;

void HttpCache::Transaction::SaveNetworkTransactionInfo(
    const HttpTransaction& transaction) {
  DCHECK(!network_transaction_info_.old_network_trans_load_timing);
  LoadTimingInfo load_timing;
  if (transaction.GetLoadTimingInfo(&load_timing))
    network_transaction_info_.old_network_trans_load_timing.reset(
        new LoadTimingInfo(load_timing));

  network_transaction_info_.total_received_bytes +=
      transaction.GetTotalReceivedBytes();
  network_transaction_info_.total_sent_bytes += transaction.GetTotalSentBytes();

  ConnectionAttempts attempts;
  transaction.GetConnectionAttempts(&attempts);
  for (const auto& attempt : attempts)
    network_transaction_info_.old_connection_attempts.push_back(attempt);
  network_transaction_info_.old_remote_endpoint = IPEndPoint();
  transaction.GetRemoteEndpoint(&network_transaction_info_.old_remote_endpoint);

  transaction.GetFullRequestHeaders(
      &network_transaction_info_.full_request_headers);
}

void HttpCache::Transaction::OnIOComplete(int result) {
  DoLoop(result);
}

void HttpCache::Transaction::TransitionToState(State state) {
  // Ensure that the state is only set once per Do* state.
  DCHECK(in_do_loop_);
  DCHECK_EQ(STATE_UNSET, next_state_) << "Next state is " << state;
  next_state_ = state;
}

}  // namespace net
