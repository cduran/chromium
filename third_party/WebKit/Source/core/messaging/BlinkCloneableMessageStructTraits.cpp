// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "core/messaging/BlinkCloneableMessageStructTraits.h"

#include "platform/blob/BlobData.h"

namespace mojo {

Vector<scoped_refptr<blink::BlobDataHandle>> StructTraits<
    blink::mojom::blink::CloneableMessage::DataView,
    blink::BlinkCloneableMessage>::blobs(blink::BlinkCloneableMessage& input) {
  Vector<scoped_refptr<blink::BlobDataHandle>> result;
  result.ReserveInitialCapacity(input.message->BlobDataHandles().size());
  for (const auto& blob : input.message->BlobDataHandles())
    result.push_back(blob.value);
  return result;
}

bool StructTraits<blink::mojom::blink::CloneableMessage::DataView,
                  blink::BlinkCloneableMessage>::
    Read(blink::mojom::blink::CloneableMessage::DataView data,
         blink::BlinkCloneableMessage* out) {
  mojo::ArrayDataView<uint8_t> message_data;
  data.GetEncodedMessageDataView(&message_data);
  out->message = blink::SerializedScriptValue::Create(
      reinterpret_cast<const char*>(message_data.data()), message_data.size());

  Vector<scoped_refptr<blink::BlobDataHandle>> blobs;
  if (!data.ReadBlobs(&blobs))
    return false;
  for (auto& blob : blobs) {
    out->message->BlobDataHandles().Set(blob->Uuid(), blob);
  }
  out->sender_stack_trace_id = v8_inspector::V8StackTraceId(
      static_cast<uintptr_t>(data.stack_trace_id()),
      std::make_pair(data.stack_trace_debugger_id_first(),
                     data.stack_trace_debugger_id_second()));

  return true;
}

}  // namespace mojo
