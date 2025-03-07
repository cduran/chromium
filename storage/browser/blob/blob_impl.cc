// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "storage/browser/blob/blob_impl.h"

#include <memory>
#include <utility>

#include "storage/browser/blob/blob_data_handle.h"
#include "storage/browser/blob/mojo_blob_reader.h"

namespace storage {
namespace {

class ReaderDelegate : public MojoBlobReader::Delegate {
 public:
  ReaderDelegate(mojo::ScopedDataPipeProducerHandle handle,
                 blink::mojom::BlobReaderClientPtr client)
      : handle_(std::move(handle)), client_(std::move(client)) {}

  mojo::ScopedDataPipeProducerHandle PassDataPipe() override {
    return std::move(handle_);
  }

  MojoBlobReader::Delegate::RequestSideData DidCalculateSize(
      uint64_t total_size,
      uint64_t content_size) override {
    if (client_)
      client_->OnCalculatedSize(total_size, content_size);
    return MojoBlobReader::Delegate::DONT_REQUEST_SIDE_DATA;
  }

  void OnComplete(net::Error result, uint64_t total_written_bytes) override {
    if (client_)
      client_->OnComplete(result, total_written_bytes);
  }

 private:
  mojo::ScopedDataPipeProducerHandle handle_;
  blink::mojom::BlobReaderClientPtr client_;

  DISALLOW_COPY_AND_ASSIGN(ReaderDelegate);
};

class DataPipeGetterReaderDelegate : public MojoBlobReader::Delegate {
 public:
  DataPipeGetterReaderDelegate(
      mojo::ScopedDataPipeProducerHandle handle,
      network::mojom::DataPipeGetter::ReadCallback callback)
      : handle_(std::move(handle)), callback_(std::move(callback)) {}

  mojo::ScopedDataPipeProducerHandle PassDataPipe() override {
    return std::move(handle_);
  }

  MojoBlobReader::Delegate::RequestSideData DidCalculateSize(
      uint64_t total_size,
      uint64_t content_size) override {
    // Check if null since it's conceivable OnComplete() was already called
    // with error.
    if (!callback_.is_null())
      std::move(callback_).Run(net::OK, content_size);
    return MojoBlobReader::Delegate::DONT_REQUEST_SIDE_DATA;
  }

  void OnComplete(net::Error result, uint64_t total_written_bytes) override {
    // Check if null since DidCalculateSize() may have already been called
    // and an error occurred later.
    if (!callback_.is_null() && result != net::OK) {
      // On error, signal failure immediately. On success, OnCalculatedSize()
      // is guaranteed to be called, and the result will be signaled from
      // there.
      std::move(callback_).Run(result, 0);
    }
  }

 private:
  mojo::ScopedDataPipeProducerHandle handle_;
  network::mojom::DataPipeGetter::ReadCallback callback_;

  DISALLOW_COPY_AND_ASSIGN(DataPipeGetterReaderDelegate);
};

}  // namespace

// static
base::WeakPtr<BlobImpl> BlobImpl::Create(std::unique_ptr<BlobDataHandle> handle,
                                         blink::mojom::BlobRequest request) {
  return (new BlobImpl(std::move(handle), std::move(request)))
      ->weak_ptr_factory_.GetWeakPtr();
}

void BlobImpl::Clone(blink::mojom::BlobRequest request) {
  bindings_.AddBinding(this, std::move(request));
}

void BlobImpl::AsDataPipeGetter(network::mojom::DataPipeGetterRequest request) {
  data_pipe_getter_bindings_.AddBinding(this, std::move(request));
}

void BlobImpl::ReadRange(uint64_t offset,
                         uint64_t length,
                         mojo::ScopedDataPipeProducerHandle handle,
                         blink::mojom::BlobReaderClientPtr client) {
  MojoBlobReader::Create(
      handle_.get(), net::HttpByteRange::Bounded(offset, offset + length - 1),
      std::make_unique<ReaderDelegate>(std::move(handle), std::move(client)));
}

void BlobImpl::ReadAll(mojo::ScopedDataPipeProducerHandle handle,
                       blink::mojom::BlobReaderClientPtr client) {
  MojoBlobReader::Create(
      handle_.get(), net::HttpByteRange(),
      std::make_unique<ReaderDelegate>(std::move(handle), std::move(client)));
}

void BlobImpl::GetInternalUUID(GetInternalUUIDCallback callback) {
  std::move(callback).Run(handle_->uuid());
}

void BlobImpl::Clone(network::mojom::DataPipeGetterRequest request) {
  data_pipe_getter_bindings_.AddBinding(this, std::move(request));
}

void BlobImpl::Read(mojo::ScopedDataPipeProducerHandle pipe,
                    ReadCallback callback) {
  MojoBlobReader::Create(handle_.get(), net::HttpByteRange(),
                         std::make_unique<DataPipeGetterReaderDelegate>(
                             std::move(pipe), std::move(callback)));
}

void BlobImpl::FlushForTesting() {
  bindings_.FlushForTesting();
  data_pipe_getter_bindings_.FlushForTesting();
  if (bindings_.empty() && data_pipe_getter_bindings_.empty())
    delete this;
}

BlobImpl::BlobImpl(std::unique_ptr<BlobDataHandle> handle,
                   blink::mojom::BlobRequest request)
    : handle_(std::move(handle)), weak_ptr_factory_(this) {
  DCHECK(handle_);
  bindings_.AddBinding(this, std::move(request));
  bindings_.set_connection_error_handler(base::BindRepeating(
      &BlobImpl::OnConnectionError, base::Unretained(this)));
  data_pipe_getter_bindings_.set_connection_error_handler(base::BindRepeating(
      &BlobImpl::OnConnectionError, base::Unretained(this)));
}

BlobImpl::~BlobImpl() = default;

void BlobImpl::OnConnectionError() {
  if (!bindings_.empty())
    return;
  if (!data_pipe_getter_bindings_.empty())
    return;
  delete this;
}

}  // namespace storage
