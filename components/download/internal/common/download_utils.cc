// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "components/download/public/common/download_utils.h"

#include "base/format_macros.h"
#include "base/strings/stringprintf.h"
#include "components/download/public/common/download_create_info.h"
#include "components/download/public/common/download_interrupt_reasons_utils.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_save_info.h"
#include "components/download/public/common/download_stats.h"
#include "net/http/http_status_code.h"

namespace download {

const uint32_t DownloadItem::kInvalidId = 0;

DownloadInterruptReason HandleRequestCompletionStatus(
    net::Error error_code,
    bool has_strong_validators,
    net::CertStatus cert_status,
    DownloadInterruptReason abort_reason) {
  // ERR_CONTENT_LENGTH_MISMATCH can be caused by 1 of the following reasons:
  // 1. Server or proxy closes the connection too early.
  // 2. The content-length header is wrong.
  // If the download has strong validators, we can interrupt the download
  // and let it resume automatically. Otherwise, resuming the download will
  // cause it to restart and the download may never complete if the error was
  // caused by reason 2. As a result, downloads without strong validators are
  // treated as completed here.
  // TODO(qinmin): check the metrics from downloads with strong validators,
  // and decide whether we should interrupt downloads without strong validators
  // rather than complete them.
  if (error_code == net::ERR_CONTENT_LENGTH_MISMATCH &&
      !has_strong_validators) {
    error_code = net::OK;
    RecordDownloadCount(COMPLETED_WITH_CONTENT_LENGTH_MISMATCH_COUNT);
  }

  if (error_code == net::ERR_ABORTED) {
    // ERR_ABORTED == something outside of the network
    // stack cancelled the request.  There aren't that many things that
    // could do this to a download request (whose lifetime is separated from
    // the tab from which it came).  We map this to USER_CANCELLED as the
    // case we know about (system suspend because of laptop close) corresponds
    // to a user action.
    // TODO(asanka): A lid close or other power event should result in an
    // interruption that doesn't discard the partial state, unlike
    // USER_CANCELLED. (https://crbug.com/166179)
    if (net::IsCertStatusError(cert_status))
      return DOWNLOAD_INTERRUPT_REASON_SERVER_CERT_PROBLEM;
    else
      return DOWNLOAD_INTERRUPT_REASON_USER_CANCELED;
  } else if (abort_reason != DOWNLOAD_INTERRUPT_REASON_NONE) {
    // If a more specific interrupt reason was specified before the request
    // was explicitly cancelled, then use it.
    return abort_reason;
  }

  return ConvertNetErrorToInterruptReason(error_code,
                                          DOWNLOAD_INTERRUPT_FROM_NETWORK);
}

DownloadInterruptReason HandleSuccessfulServerResponse(
    const net::HttpResponseHeaders& http_headers,
    DownloadSaveInfo* save_info,
    bool fetch_error_body) {
  DownloadInterruptReason result = DOWNLOAD_INTERRUPT_REASON_NONE;
  switch (http_headers.response_code()) {
    case -1:  // Non-HTTP request.
    case net::HTTP_OK:
    case net::HTTP_NON_AUTHORITATIVE_INFORMATION:
    case net::HTTP_PARTIAL_CONTENT:
      // Expected successful codes.
      break;

    case net::HTTP_CREATED:
    case net::HTTP_ACCEPTED:
      // Per RFC 7231 the entity being transferred is metadata about the
      // resource at the target URL and not the resource at that URL (or the
      // resource that would be at the URL once processing is completed in the
      // case of HTTP_ACCEPTED). However, we currently don't have special
      // handling for these response and they are downloaded the same as a
      // regular response.
      break;

    case net::HTTP_NO_CONTENT:
    case net::HTTP_RESET_CONTENT:
    // These two status codes don't have an entity (or rather RFC 7231
    // requires that there be no entity). They are treated the same as the
    // resource not being found since there is no entity to download.

    case net::HTTP_NOT_FOUND:
      result = DOWNLOAD_INTERRUPT_REASON_SERVER_BAD_CONTENT;
      break;

    case net::HTTP_REQUESTED_RANGE_NOT_SATISFIABLE:
      // Retry by downloading from the start automatically:
      // If we haven't received data when we get this error, we won't.
      result = DOWNLOAD_INTERRUPT_REASON_SERVER_NO_RANGE;
      break;
    case net::HTTP_UNAUTHORIZED:
    case net::HTTP_PROXY_AUTHENTICATION_REQUIRED:
      // Server didn't authorize this request.
      result = DOWNLOAD_INTERRUPT_REASON_SERVER_UNAUTHORIZED;
      break;
    case net::HTTP_FORBIDDEN:
      // Server forbids access to this resource.
      result = DOWNLOAD_INTERRUPT_REASON_SERVER_FORBIDDEN;
      break;
    default:  // All other errors.
      // Redirection and informational codes should have been handled earlier
      // in the stack.
      // TODO(xingliu): Handle HTTP_PRECONDITION_FAILED and resurrect
      // DOWNLOAD_INTERRUPT_REASON_SERVER_PRECONDITION for range
      // requests. This will change extensions::api::InterruptReason.
      DCHECK_NE(3, http_headers.response_code() / 100);
      DCHECK_NE(1, http_headers.response_code() / 100);
      result = DOWNLOAD_INTERRUPT_REASON_SERVER_FAILED;
  }

  if (result != DOWNLOAD_INTERRUPT_REASON_NONE && !fetch_error_body)
    return result;

  // The caller is expecting a partial response.
  if (save_info && (save_info->offset > 0 || save_info->length > 0)) {
    if (http_headers.response_code() != net::HTTP_PARTIAL_CONTENT) {
      // Server should send partial content when "If-Match" or
      // "If-Unmodified-Since" check passes, and the range request header has
      // last byte position. e.g. "Range:bytes=50-99".
      if (save_info->length != DownloadSaveInfo::kLengthFullContent &&
          !fetch_error_body)
        return DOWNLOAD_INTERRUPT_REASON_SERVER_BAD_CONTENT;

      // Requested a partial range, but received the entire response, when
      // the range request header is "Range:bytes={offset}-".
      // The response can be HTTP 200 or other error code when
      // |fetch_error_body| is true.
      save_info->offset = 0;
      save_info->hash_of_partial_file.clear();
      save_info->hash_state.reset();
      return DOWNLOAD_INTERRUPT_REASON_NONE;
    }

    int64_t first_byte = -1;
    int64_t last_byte = -1;
    int64_t length = -1;
    if (!http_headers.GetContentRangeFor206(&first_byte, &last_byte, &length))
      return DOWNLOAD_INTERRUPT_REASON_SERVER_BAD_CONTENT;
    DCHECK_GE(first_byte, 0);

    if (first_byte != save_info->offset ||
        (save_info->length > 0 &&
         last_byte != save_info->offset + save_info->length - 1)) {
      // The server returned a different range than the one we requested. Assume
      // the response is bad.
      //
      // In the future we should consider allowing offsets that are less than
      // the offset we've requested, since in theory we can truncate the partial
      // file at the offset and continue.
      return DOWNLOAD_INTERRUPT_REASON_SERVER_BAD_CONTENT;
    }

    return DOWNLOAD_INTERRUPT_REASON_NONE;
  }

  if (http_headers.response_code() == net::HTTP_PARTIAL_CONTENT)
    return DOWNLOAD_INTERRUPT_REASON_SERVER_BAD_CONTENT;

  return DOWNLOAD_INTERRUPT_REASON_NONE;
}

void HandleResponseHeaders(const net::HttpResponseHeaders* headers,
                           DownloadCreateInfo* create_info) {
  if (!headers)
    return;

  if (headers->HasStrongValidators()) {
    // If we don't have strong validators as per RFC 7232 section 2, then
    // we neither store nor use them for range requests.
    if (!headers->EnumerateHeader(nullptr, "Last-Modified",
                                  &create_info->last_modified))
      create_info->last_modified.clear();
    if (!headers->EnumerateHeader(nullptr, "ETag", &create_info->etag))
      create_info->etag.clear();
  }

  // Grab the first content-disposition header.  There may be more than one,
  // though as of this writing, the network stack ensures if there are, they
  // are all duplicates.
  headers->EnumerateHeader(nullptr, "Content-Disposition",
                           &create_info->content_disposition);

  // Parse the original mime type from the header, notice that actual mime type
  // might be different due to mime type sniffing.
  if (!headers->GetMimeType(&create_info->original_mime_type))
    create_info->original_mime_type.clear();

  // Content-Range is validated in HandleSuccessfulServerResponse.
  // In RFC 7233, a single part 206 partial response must generate
  // Content-Range. Accept-Range may be sent in 200 response to indicate the
  // server can handle range request, but optional in 206 response.
  create_info->accept_range =
      headers->HasHeaderValue("Accept-Ranges", "bytes") ||
      (headers->HasHeader("Content-Range") &&
       headers->response_code() == net::HTTP_PARTIAL_CONTENT);
}

}  // namespace download
