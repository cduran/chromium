// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "net/http/partial_data.h"

#include <limits>

#include "base/bind.h"
#include "base/bind_helpers.h"
#include "base/callback_helpers.h"
#include "base/format_macros.h"
#include "base/logging.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/strings/stringprintf.h"
#include "net/base/net_errors.h"
#include "net/disk_cache/disk_cache.h"
#include "net/http/http_response_headers.h"
#include "net/http/http_util.h"

namespace net {

namespace {

// The headers that we have to process.
const char kLengthHeader[] = "Content-Length";
const char kRangeHeader[] = "Content-Range";
const int kDataStream = 1;

}  // namespace

PartialData::PartialData()
    : current_range_start_(0),
      current_range_end_(0),
      cached_start_(0),
      cached_min_len_(0),
      resource_size_(0),
      range_present_(false),
      final_range_(false),
      sparse_entry_(true),
      truncated_(false),
      initial_validation_(false),
      weak_factory_(this) {}

PartialData::~PartialData() = default;

bool PartialData::Init(const HttpRequestHeaders& headers) {
  std::string range_header;
  if (!headers.GetHeader(HttpRequestHeaders::kRange, &range_header))
    return false;

  std::vector<HttpByteRange> ranges;
  if (!HttpUtil::ParseRangeHeader(range_header, &ranges) || ranges.size() != 1)
    return false;

  // We can handle this range request.
  byte_range_ = ranges[0];
  if (!byte_range_.IsValid())
    return false;

  current_range_start_ = byte_range_.first_byte_position();

  DVLOG(1) << "Range start: " << current_range_start_ << " end: " <<
               byte_range_.last_byte_position();
  return true;
}

void PartialData::SetHeaders(const HttpRequestHeaders& headers) {
  DCHECK(extra_headers_.IsEmpty());
  extra_headers_.CopyFrom(headers);
}

void PartialData::RestoreHeaders(HttpRequestHeaders* headers) const {
  DCHECK(current_range_start_ >= 0 || byte_range_.IsSuffixByteRange());
  int64_t end = byte_range_.IsSuffixByteRange()
                    ? byte_range_.suffix_length()
                    : byte_range_.last_byte_position();

  headers->CopyFrom(extra_headers_);
  if (truncated_ || !byte_range_.IsValid())
    return;

  if (current_range_start_ < 0) {
    headers->SetHeader(HttpRequestHeaders::kRange,
                       HttpByteRange::Suffix(end).GetHeaderValue());
  } else {
    headers->SetHeader(HttpRequestHeaders::kRange,
                       HttpByteRange::Bounded(
                           current_range_start_, end).GetHeaderValue());
  }
}

int PartialData::ShouldValidateCache(disk_cache::Entry* entry,
                                     const CompletionCallback& callback) {
  DCHECK_GE(current_range_start_, 0);

  // Scan the disk cache for the first cached portion within this range.
  int len = GetNextRangeLen();
  if (!len)
    return 0;

  DVLOG(3) << "ShouldValidateCache len: " << len;

  if (sparse_entry_) {
    DCHECK(callback_.is_null());
    int64_t* start = new int64_t;
    // This callback now owns "start". We make sure to keep it
    // in a local variable since we want to use it later.
    CompletionCallback cb =
        base::Bind(&PartialData::GetAvailableRangeCompleted,
                   weak_factory_.GetWeakPtr(), base::Owned(start));
    cached_min_len_ =
        entry->GetAvailableRange(current_range_start_, len, start, cb);

    if (cached_min_len_ == ERR_IO_PENDING) {
      callback_ = callback;
      return ERR_IO_PENDING;
    } else {
      cached_start_ = *start;
    }
  } else if (!truncated_) {
    if (byte_range_.HasFirstBytePosition() &&
        byte_range_.first_byte_position() >= resource_size_) {
      // The caller should take care of this condition because we should have
      // failed IsRequestedRangeOK(), but it's better to be consistent here.
      len = 0;
    }
    cached_min_len_ = len;
    cached_start_ = current_range_start_;
  }

  if (cached_min_len_ < 0)
    return cached_min_len_;

  // Return a positive number to indicate success (versus error or finished).
  return 1;
}

void PartialData::PrepareCacheValidation(disk_cache::Entry* entry,
                                         HttpRequestHeaders* headers) {
  DCHECK_GE(current_range_start_, 0);
  DCHECK_GE(cached_min_len_, 0);

  int len = GetNextRangeLen();
  DCHECK_NE(0, len);
  range_present_ = false;

  headers->CopyFrom(extra_headers_);

  if (!cached_min_len_) {
    // We don't have anything else stored.
    final_range_ = true;
    cached_start_ =
        byte_range_.HasLastBytePosition() ? current_range_start_  + len : 0;
  }

  if (current_range_start_ == cached_start_) {
    // The data lives in the cache.
    range_present_ = true;
    current_range_end_ = cached_start_ + cached_min_len_ - 1;
    if (len == cached_min_len_)
      final_range_ = true;
  } else {
    // This range is not in the cache.
    current_range_end_ = cached_start_ - 1;
  }
  headers->SetHeader(
      HttpRequestHeaders::kRange,
      HttpByteRange::Bounded(current_range_start_, current_range_end_)
          .GetHeaderValue());
}

bool PartialData::IsCurrentRangeCached() const {
  return range_present_;
}

bool PartialData::IsLastRange() const {
  return final_range_;
}

bool PartialData::UpdateFromStoredHeaders(const HttpResponseHeaders* headers,
                                          disk_cache::Entry* entry,
                                          bool truncated) {
  resource_size_ = 0;
  if (truncated) {
    DCHECK_EQ(headers->response_code(), 200);
    // We don't have the real length and the user may be trying to create a
    // sparse entry so let's not write to this entry.
    if (byte_range_.IsValid())
      return false;

    if (!headers->HasStrongValidators())
      return false;

    // Now we avoid resume if there is no content length, but that was not
    // always the case so double check here.
    int64_t total_length = headers->GetContentLength();
    if (total_length <= 0)
      return false;

    // In case we see a truncated entry, we first send a network request for
    // 1 byte range with If-Range: to probe server support for resumption.
    // The setting of |current_range_start_| and |cached_start_| below (with any
    // positive value of |cached_min_len_|) results in that.
    //
    // Setting |initial_validation_| to true is how this communicates to
    // HttpCache::Transaction that we're doing that (and that it's not the user
    // asking for one byte), so if it sees a 206 with that flag set it will call
    // SetRangeToStartDownload(), and then restart the process looking for the
    // entire file (which is what the user wanted), with the cache handling
    // the previous portion, and then a second network request for the entire
    // rest of the range. A 200 in response to the probe request can be simply
    // returned directly to the user.
    truncated_ = true;
    initial_validation_ = true;
    sparse_entry_ = false;
    int current_len = entry->GetDataSize(kDataStream);
    byte_range_.set_first_byte_position(current_len);
    resource_size_ = total_length;
    current_range_start_ = current_len;
    cached_min_len_ = current_len;
    cached_start_ = current_len + 1;
    return true;
  }

  if (headers->response_code() != 206) {
    DCHECK(byte_range_.IsValid());
    sparse_entry_ = false;
    resource_size_ = entry->GetDataSize(kDataStream);
    DVLOG(2) << "UpdateFromStoredHeaders size: " << resource_size_;
    return true;
  }

  if (!headers->HasStrongValidators())
    return false;

  int64_t length_value = headers->GetContentLength();
  if (length_value <= 0)
    return false;  // We must have stored the resource length.

  resource_size_ = length_value;

  // Make sure that this is really a sparse entry.
  return entry->CouldBeSparse();
}

void PartialData::SetRangeToStartDownload() {
  DCHECK(truncated_);
  DCHECK(!sparse_entry_);
  current_range_start_ = 0;
  cached_start_ = 0;
  initial_validation_ = false;
}

bool PartialData::IsRequestedRangeOK() {
  if (byte_range_.IsValid()) {
    if (!byte_range_.ComputeBounds(resource_size_))
      return false;
    if (truncated_)
      return true;

    if (current_range_start_ < 0)
      current_range_start_ = byte_range_.first_byte_position();
  } else {
    // This is not a range request but we have partial data stored.
    current_range_start_ = 0;
    byte_range_.set_last_byte_position(resource_size_ - 1);
  }

  bool rv = current_range_start_ >= 0;
  if (!rv)
    current_range_start_ = 0;

  return rv;
}

bool PartialData::ResponseHeadersOK(const HttpResponseHeaders* headers) {
  if (headers->response_code() == 304) {
    if (!byte_range_.IsValid() || truncated_)
      return true;

    // We must have a complete range here.
    return byte_range_.HasFirstBytePosition() &&
        byte_range_.HasLastBytePosition();
  }

  int64_t start, end, total_length;
  if (!headers->GetContentRangeFor206(&start, &end, &total_length))
    return false;
  if (total_length <= 0)
    return false;

  DCHECK_EQ(headers->response_code(), 206);

  // A server should return a valid content length with a 206 (per the standard)
  // but relax the requirement because some servers don't do that.
  int64_t content_length = headers->GetContentLength();
  if (content_length > 0 && content_length != end - start + 1)
    return false;

  if (!resource_size_) {
    // First response. Update our values with the ones provided by the server.
    resource_size_ = total_length;
    if (!byte_range_.HasFirstBytePosition()) {
      byte_range_.set_first_byte_position(start);
      current_range_start_ = start;
    }
    if (!byte_range_.HasLastBytePosition())
      byte_range_.set_last_byte_position(end);
  } else if (resource_size_ != total_length) {
    return false;
  }

  if (truncated_) {
    if (!byte_range_.HasLastBytePosition())
      byte_range_.set_last_byte_position(end);
  }

  if (start != current_range_start_)
    return false;

  if (!current_range_end_) {
    // There is nothing in the cache.
    DCHECK(byte_range_.HasLastBytePosition());
    current_range_end_ = byte_range_.last_byte_position();
    if (current_range_end_ >= resource_size_) {
      // We didn't know the real file size, and the server is saying that the
      // requested range goes beyond the size. Fix it.
      current_range_end_ = end;
      byte_range_.set_last_byte_position(end);
    }
  }

  // If we received a range, but it's not exactly the range we asked for, avoid
  // trouble and signal an error.
  if (end != current_range_end_)
    return false;

  return true;
}

// We are making multiple requests to complete the range requested by the user.
// Just assume that everything is fine and say that we are returning what was
// requested.
void PartialData::FixResponseHeaders(HttpResponseHeaders* headers,
                                     bool success) {
  if (truncated_)
    return;

  if (byte_range_.IsValid() && success) {
    headers->UpdateWithNewRange(byte_range_, resource_size_, !sparse_entry_);
    return;
  }

  headers->RemoveHeader(kLengthHeader);
  headers->RemoveHeader(kRangeHeader);

  if (byte_range_.IsValid()) {
    headers->ReplaceStatusLine("HTTP/1.1 416 Requested Range Not Satisfiable");
    headers->AddHeader(base::StringPrintf("%s: bytes 0-0/%" PRId64,
                                          kRangeHeader, resource_size_));
    headers->AddHeader(base::StringPrintf("%s: 0", kLengthHeader));
  } else {
    // TODO(rvargas): Is it safe to change the protocol version?
    headers->ReplaceStatusLine("HTTP/1.1 200 OK");
    DCHECK_NE(resource_size_, 0);
    headers->AddHeader(base::StringPrintf("%s: %" PRId64, kLengthHeader,
                                          resource_size_));
  }
}

void PartialData::FixContentLength(HttpResponseHeaders* headers) {
  headers->RemoveHeader(kLengthHeader);
  headers->AddHeader(base::StringPrintf("%s: %" PRId64, kLengthHeader,
                                        resource_size_));
}

int PartialData::CacheRead(disk_cache::Entry* entry,
                           IOBuffer* data,
                           int data_len,
                           const CompletionCallback& callback) {
  int read_len = std::min(data_len, cached_min_len_);
  if (!read_len)
    return 0;

  int rv = 0;
  if (sparse_entry_) {
    rv = entry->ReadSparseData(current_range_start_, data, read_len,
                               callback);
  } else {
    if (current_range_start_ > std::numeric_limits<int32_t>::max())
      return ERR_INVALID_ARGUMENT;

    rv = entry->ReadData(kDataStream, static_cast<int>(current_range_start_),
                         data, read_len, callback);
  }
  return rv;
}

int PartialData::CacheWrite(disk_cache::Entry* entry,
                            IOBuffer* data,
                            int data_len,
                            const CompletionCallback& callback) {
  DVLOG(3) << "To write: " << data_len;
  if (sparse_entry_) {
    return entry->WriteSparseData(
        current_range_start_, data, data_len, callback);
  } else  {
    if (current_range_start_ > std::numeric_limits<int32_t>::max())
      return ERR_INVALID_ARGUMENT;

    return entry->WriteData(kDataStream, static_cast<int>(current_range_start_),
                            data, data_len, callback, true);
  }
}

void PartialData::OnCacheReadCompleted(int result) {
  DVLOG(3) << "Read: " << result;
  if (result > 0) {
    current_range_start_ += result;
    cached_min_len_ -= result;
    DCHECK_GE(cached_min_len_, 0);
  }
}

void PartialData::OnNetworkReadCompleted(int result) {
  if (result > 0)
    current_range_start_ += result;
}

int PartialData::GetNextRangeLen() {
  int64_t range_len =
      byte_range_.HasLastBytePosition()
          ? byte_range_.last_byte_position() - current_range_start_ + 1
          : std::numeric_limits<int32_t>::max();
  if (range_len > std::numeric_limits<int32_t>::max())
    range_len = std::numeric_limits<int32_t>::max();
  return static_cast<int32_t>(range_len);
}

void PartialData::GetAvailableRangeCompleted(int64_t* start, int result) {
  DCHECK(!callback_.is_null());
  DCHECK_NE(ERR_IO_PENDING, result);

  cached_start_ = *start;
  cached_min_len_ = result;
  if (result >= 0)
    result = 1;  // Return success, go ahead and validate the entry.

  base::ResetAndReturn(&callback_).Run(result);
}

}  // namespace net
