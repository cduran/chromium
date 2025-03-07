// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/edk/system/user_message_impl.h"

#include <algorithm>
#include <vector>

#include "base/memory/ptr_util.h"
#include "base/metrics/histogram_macros_local.h"
#include "base/numerics/safe_conversions.h"
#include "base/numerics/safe_math.h"
#include "mojo/edk/embedder/embedder_internal.h"
#include "mojo/edk/system/core.h"
#include "mojo/edk/system/node_channel.h"
#include "mojo/edk/system/node_controller.h"
#include "mojo/edk/system/ports/event.h"
#include "mojo/edk/system/ports/message_filter.h"
#include "mojo/edk/system/ports/node.h"
#include "mojo/public/c/system/types.h"

namespace mojo {
namespace edk {

namespace {

// The minimum amount of memory to allocate for a new serialized message buffer.
// This should be sufficiently large such that most seiralized messages do not
// incur any reallocations as they're expanded to full size.
const uint32_t kMinimumPayloadBufferSize = 4096;

// Indicates whether handle serialization failure should be emulated in testing.
bool g_always_fail_handle_serialization = false;

#pragma pack(push, 1)
// Header attached to every message.
struct MessageHeader {
  // The number of serialized dispatchers included in this header.
  uint32_t num_dispatchers;

  // Total size of the header, including serialized dispatcher data.
  uint32_t header_size;
};

// Header for each dispatcher in a message, immediately following the message
// header.
struct DispatcherHeader {
  // The type of the dispatcher, correpsonding to the Dispatcher::Type enum.
  int32_t type;

  // The size of the serialized dispatcher, not including this header.
  uint32_t num_bytes;

  // The number of ports needed to deserialize this dispatcher.
  uint32_t num_ports;

  // The number of platform handles needed to deserialize this dispatcher.
  uint32_t num_platform_handles;
};
#pragma pack(pop)

static_assert(sizeof(MessageHeader) % 8 == 0, "Invalid MessageHeader size.");
static_assert(sizeof(DispatcherHeader) % 8 == 0,
              "Invalid DispatcherHeader size.");

// Creates a new Channel message with sufficient storage for |num_bytes| user
// message payload and all |dispatchers| given. If |original_message| is not
// null, its contents are copied and extended by the other parameters given
// here.
MojoResult CreateOrExtendSerializedEventMessage(
    ports::UserMessageEvent* event,
    size_t payload_size,
    size_t payload_buffer_size,
    const Dispatcher::DispatcherInTransit* new_dispatchers,
    size_t num_new_dispatchers,
    Channel::MessagePtr* out_message,
    void** out_header,
    size_t* out_header_size,
    void** out_user_payload) {
  // A structure for tracking information about every Dispatcher that will be
  // serialized into the message. This is NOT part of the message itself.
  struct DispatcherInfo {
    uint32_t num_bytes;
    uint32_t num_ports;
    uint32_t num_handles;
  };

  size_t original_header_size = sizeof(MessageHeader);
  size_t original_num_ports = 0;
  size_t original_num_handles = 0;
  size_t original_payload_size = 0;
  MessageHeader* original_header = nullptr;
  void* original_user_payload = nullptr;
  Channel::MessagePtr original_message;
  if (*out_message) {
    original_message = std::move(*out_message);
    original_header = static_cast<MessageHeader*>(*out_header);
    original_header_size = *out_header_size;
    original_num_ports = event->num_ports();
    original_num_handles = original_message->num_handles();
    original_user_payload = *out_user_payload;
    original_payload_size =
        original_message->payload_size() -
        (static_cast<char*>(original_user_payload) -
         static_cast<char*>(original_message->mutable_payload()));
  }

  // This is only the base header size. It will grow as we accumulate the
  // size of serialized state for each dispatcher.
  base::CheckedNumeric<size_t> safe_header_size = num_new_dispatchers;
  safe_header_size *= sizeof(DispatcherHeader);
  safe_header_size += original_header_size;
  size_t header_size = safe_header_size.ValueOrDie();
  size_t num_new_ports = 0;
  size_t num_new_handles = 0;
  std::vector<DispatcherInfo> new_dispatcher_info(num_new_dispatchers);
  for (size_t i = 0; i < num_new_dispatchers; ++i) {
    Dispatcher* d = new_dispatchers[i].dispatcher.get();
    d->StartSerialize(&new_dispatcher_info[i].num_bytes,
                      &new_dispatcher_info[i].num_ports,
                      &new_dispatcher_info[i].num_handles);
    header_size += new_dispatcher_info[i].num_bytes;
    num_new_ports += new_dispatcher_info[i].num_ports;
    num_new_handles += new_dispatcher_info[i].num_handles;
  }

  size_t num_ports = original_num_ports + num_new_ports;
  size_t num_handles = original_num_handles + num_new_handles;

  // We now have enough information to fully allocate the message storage.
  if (num_ports > event->num_ports())
    event->ReservePorts(num_ports);
  const size_t event_size = event->GetSerializedSize();
  const size_t total_size = event_size + header_size + payload_size;
  const size_t total_buffer_size =
      event_size + header_size + payload_buffer_size;
  void* data;
  Channel::MessagePtr message = NodeChannel::CreateEventMessage(
      total_buffer_size, total_size, &data, num_handles);
  auto* header = reinterpret_cast<MessageHeader*>(static_cast<uint8_t*>(data) +
                                                  event_size);

  // Populate the message header with information about serialized dispatchers.
  // The front of the message is always a MessageHeader followed by a
  // DispatcherHeader for each dispatcher to be sent.
  DispatcherHeader* new_dispatcher_headers;
  char* new_dispatcher_data;
  size_t total_num_dispatchers = num_new_dispatchers;
  std::vector<ScopedPlatformHandle> handles;
  if (original_message) {
    DCHECK(original_header);
    size_t original_dispatcher_headers_size =
        original_header->num_dispatchers * sizeof(DispatcherHeader);
    memcpy(header, original_header,
           original_dispatcher_headers_size + sizeof(MessageHeader));
    new_dispatcher_headers = reinterpret_cast<DispatcherHeader*>(
        reinterpret_cast<uint8_t*>(header + 1) +
        original_dispatcher_headers_size);
    total_num_dispatchers += original_header->num_dispatchers;
    size_t total_dispatcher_headers_size =
        total_num_dispatchers * sizeof(DispatcherHeader);
    char* original_dispatcher_data =
        reinterpret_cast<char*>(original_header + 1) +
        original_dispatcher_headers_size;
    char* dispatcher_data =
        reinterpret_cast<char*>(header + 1) + total_dispatcher_headers_size;
    size_t original_dispatcher_data_size = original_header_size -
                                           sizeof(MessageHeader) -
                                           original_dispatcher_headers_size;
    memcpy(dispatcher_data, original_dispatcher_data,
           original_dispatcher_data_size);
    new_dispatcher_data = dispatcher_data + original_dispatcher_data_size;
    handles = original_message->TakeHandles();
    if (!handles.empty())
      handles.resize(num_handles);
    memcpy(reinterpret_cast<char*>(header) + header_size,
           reinterpret_cast<char*>(original_header) + original_header_size,
           original_payload_size);
  } else {
    new_dispatcher_headers = reinterpret_cast<DispatcherHeader*>(header + 1);
    // Serialized dispatcher state immediately follows the series of
    // DispatcherHeaders.
    new_dispatcher_data =
        reinterpret_cast<char*>(new_dispatcher_headers + num_new_dispatchers);
  }

  if (handles.empty() && num_new_handles) {
    handles.resize(num_new_handles);
  }

  header->num_dispatchers =
      base::CheckedNumeric<uint32_t>(total_num_dispatchers).ValueOrDie();

  // |header_size| is the total number of bytes preceding the message payload,
  // including all dispatcher headers and serialized dispatcher state.
  if (!base::IsValueInRangeForNumericType<uint32_t>(header_size))
    return MOJO_RESULT_OUT_OF_RANGE;

  header->header_size = static_cast<uint32_t>(header_size);

  if (num_new_dispatchers > 0) {
    size_t port_index = original_num_ports;
    size_t handle_index = original_num_handles;
    bool fail = false;
    for (size_t i = 0; i < num_new_dispatchers; ++i) {
      Dispatcher* d = new_dispatchers[i].dispatcher.get();
      DispatcherHeader* dh = &new_dispatcher_headers[i];
      const DispatcherInfo& info = new_dispatcher_info[i];

      // Fill in the header for this dispatcher.
      dh->type = static_cast<int32_t>(d->GetType());
      dh->num_bytes = info.num_bytes;
      dh->num_ports = info.num_ports;
      dh->num_platform_handles = info.num_handles;

      // Fill in serialized state, ports, and platform handles. We'll cancel
      // the send if the dispatcher implementation rejects for some reason.
      if (g_always_fail_handle_serialization ||
          !d->EndSerialize(
              static_cast<void*>(new_dispatcher_data),
              event->ports() + port_index,
              !handles.empty() ? handles.data() + handle_index : nullptr)) {
        fail = true;
        break;
      }

      new_dispatcher_data += info.num_bytes;
      port_index += info.num_ports;
      handle_index += info.num_handles;
    }

    if (fail) {
      // Release any platform handles we've accumulated. Their dispatchers
      // retain ownership when message creation fails, so these are not actually
      // leaking.
      for (auto& handle : handles)
        ignore_result(handle.release());

      // Leave the original message in place on failure if applicable.
      if (original_message)
        *out_message = std::move(original_message);
      return MOJO_RESULT_INVALID_ARGUMENT;
    }

    // Take ownership of all the handles and move them into message storage.
    message->SetHandles(std::move(handles));
  }

  *out_message = std::move(message);
  *out_header = header;
  *out_header_size = header_size;
  *out_user_payload = reinterpret_cast<uint8_t*>(header) + header_size;
  return MOJO_RESULT_OK;
}

}  // namespace

// static
const ports::UserMessage::TypeInfo UserMessageImpl::kUserMessageTypeInfo = {};

UserMessageImpl::~UserMessageImpl() {
  if (HasContext() && context_destructor_) {
    DCHECK(!channel_message_);
    DCHECK(!has_serialized_handles_);
    context_destructor_(context_);
  } else if (IsSerialized() && has_serialized_handles_) {
    // Ensure that any handles still serialized within this message are
    // extracted and closed so they don't leak.
    std::vector<MojoHandle> handles(num_handles());
    MojoResult result =
        ExtractSerializedHandles(ExtractBadHandlePolicy::kSkip, handles.data());
    if (result == MOJO_RESULT_OK) {
      for (auto handle : handles) {
        if (handle != MOJO_HANDLE_INVALID)
          internal::g_core->Close(handle);
      }
    }

    if (!pending_handle_attachments_.empty()) {
      internal::g_core->ReleaseDispatchersForTransit(
          pending_handle_attachments_, false);
      for (const auto& dispatcher : pending_handle_attachments_)
        internal::g_core->Close(dispatcher.local_handle);
    }
  }
}

// static
std::unique_ptr<ports::UserMessageEvent>
UserMessageImpl::CreateEventForNewMessage() {
  auto message_event = std::make_unique<ports::UserMessageEvent>(0);
  message_event->AttachMessage(
      base::WrapUnique(new UserMessageImpl(message_event.get())));
  return message_event;
}

// static
MojoResult UserMessageImpl::CreateEventForNewSerializedMessage(
    uint32_t num_bytes,
    const Dispatcher::DispatcherInTransit* dispatchers,
    uint32_t num_dispatchers,
    std::unique_ptr<ports::UserMessageEvent>* out_event) {
  Channel::MessagePtr channel_message;
  void* header = nullptr;
  void* user_payload = nullptr;
  auto event = std::make_unique<ports::UserMessageEvent>(0);
  size_t header_size = 0;
  MojoResult rv = CreateOrExtendSerializedEventMessage(
      event.get(), num_bytes, num_bytes, dispatchers, num_dispatchers,
      &channel_message, &header, &header_size, &user_payload);
  if (rv != MOJO_RESULT_OK)
    return rv;
  event->AttachMessage(base::WrapUnique(
      new UserMessageImpl(event.get(), std::move(channel_message), header,
                          header_size, user_payload, num_bytes)));
  *out_event = std::move(event);
  return MOJO_RESULT_OK;
}

// static
std::unique_ptr<UserMessageImpl> UserMessageImpl::CreateFromChannelMessage(
    ports::UserMessageEvent* message_event,
    Channel::MessagePtr channel_message,
    void* payload,
    size_t payload_size) {
  DCHECK(channel_message);
  if (payload_size < sizeof(MessageHeader))
    return nullptr;

  auto* header = static_cast<MessageHeader*>(payload);
  const size_t header_size = header->header_size;
  if (header_size > payload_size)
    return nullptr;

  void* user_payload = static_cast<uint8_t*>(payload) + header_size;
  const size_t user_payload_size = payload_size - header_size;
  return base::WrapUnique(
      new UserMessageImpl(message_event, std::move(channel_message), header,
                          header_size, user_payload, user_payload_size));
}

// static
Channel::MessagePtr UserMessageImpl::FinalizeEventMessage(
    std::unique_ptr<ports::UserMessageEvent> message_event) {
  auto* message = message_event->GetMessage<UserMessageImpl>();
  DCHECK(message->IsSerialized());

  if (!message->is_committed_)
    return nullptr;

  Channel::MessagePtr channel_message = std::move(message->channel_message_);
  message->user_payload_ = nullptr;
  message->user_payload_size_ = 0;

  // Serialize the UserMessageEvent into the front of the message payload where
  // there is already space reserved for it.
  if (channel_message) {
    void* data;
    size_t size;
    NodeChannel::GetEventMessageData(channel_message.get(), &data, &size);
    message_event->Serialize(data);
  }

  return channel_message;
}

size_t UserMessageImpl::user_payload_capacity() const {
  DCHECK(IsSerialized());
  const size_t user_payload_offset =
      static_cast<uint8_t*>(user_payload_) -
      static_cast<const uint8_t*>(channel_message_->payload());
  const size_t message_capacity = channel_message_->capacity();
  DCHECK_LE(user_payload_offset, message_capacity);
  return message_capacity - user_payload_offset;
}

size_t UserMessageImpl::num_handles() const {
  DCHECK(IsSerialized());
  DCHECK(header_);
  return static_cast<const MessageHeader*>(header_)->num_dispatchers;
}

MojoResult UserMessageImpl::SetContext(
    uintptr_t context,
    MojoMessageContextSerializer serializer,
    MojoMessageContextDestructor destructor) {
  if (!context && (serializer || destructor))
    return MOJO_RESULT_INVALID_ARGUMENT;
  if (context && HasContext())
    return MOJO_RESULT_ALREADY_EXISTS;
  if (IsSerialized())
    return MOJO_RESULT_FAILED_PRECONDITION;
  context_ = context;
  context_serializer_ = serializer;
  context_destructor_ = destructor;
  return MOJO_RESULT_OK;
}

MojoResult UserMessageImpl::AppendData(uint32_t additional_payload_size,
                                       const MojoHandle* handles,
                                       uint32_t num_handles) {
  if (HasContext())
    return MOJO_RESULT_FAILED_PRECONDITION;

  std::vector<Dispatcher::DispatcherInTransit> dispatchers;
  if (num_handles > 0) {
    MojoResult acquire_result = internal::g_core->AcquireDispatchersForTransit(
        handles, num_handles, &dispatchers);
    if (acquire_result != MOJO_RESULT_OK)
      return acquire_result;
  }

  if (!IsSerialized()) {
    // First data for this message.
    Channel::MessagePtr channel_message;
    MojoResult rv = CreateOrExtendSerializedEventMessage(
        message_event_, additional_payload_size,
        std::max(additional_payload_size, kMinimumPayloadBufferSize),
        dispatchers.data(), num_handles, &channel_message, &header_,
        &header_size_, &user_payload_);
    if (num_handles > 0) {
      internal::g_core->ReleaseDispatchersForTransit(dispatchers,
                                                     rv == MOJO_RESULT_OK);
    }
    if (rv != MOJO_RESULT_OK)
      return MOJO_RESULT_ABORTED;

    user_payload_size_ = additional_payload_size;
    channel_message_ = std::move(channel_message);
    has_serialized_handles_ = true;
  } else {
    // Extend the existing message payload.

    // In order to avoid rather expensive message resizing on every handle
    // attachment operation, we merely lock and prepare the handle for transit
    // here, deferring serialization until |CommitSize()|.
    std::copy(dispatchers.begin(), dispatchers.end(),
              std::back_inserter(pending_handle_attachments_));

    if (additional_payload_size) {
      size_t header_offset =
          static_cast<uint8_t*>(header_) -
          static_cast<const uint8_t*>(channel_message_->payload());
      size_t user_payload_offset =
          static_cast<uint8_t*>(user_payload_) -
          static_cast<const uint8_t*>(channel_message_->payload());
      channel_message_->ExtendPayload(user_payload_offset + user_payload_size_ +
                                      additional_payload_size);
      header_ = static_cast<uint8_t*>(channel_message_->mutable_payload()) +
                header_offset;
      user_payload_ =
          static_cast<uint8_t*>(channel_message_->mutable_payload()) +
          user_payload_offset;
      user_payload_size_ += additional_payload_size;
    }
  }

  return MOJO_RESULT_OK;
}

MojoResult UserMessageImpl::CommitSize() {
  if (!IsSerialized())
    return MOJO_RESULT_FAILED_PRECONDITION;

  if (is_committed_)
    return MOJO_RESULT_OK;

  if (!pending_handle_attachments_.empty()) {
    CreateOrExtendSerializedEventMessage(
        message_event_, user_payload_size_, user_payload_size_,
        pending_handle_attachments_.data(), pending_handle_attachments_.size(),
        &channel_message_, &header_, &header_size_, &user_payload_);
    internal::g_core->ReleaseDispatchersForTransit(pending_handle_attachments_,
                                                   true);
    pending_handle_attachments_.clear();
  }

  is_committed_ = true;
  return MOJO_RESULT_OK;
}

MojoResult UserMessageImpl::SerializeIfNecessary() {
  if (IsSerialized())
    return MOJO_RESULT_FAILED_PRECONDITION;

  DCHECK(HasContext());
  DCHECK(!has_serialized_handles_);
  if (!context_serializer_)
    return MOJO_RESULT_NOT_FOUND;

  uintptr_t context = context_;
  context_ = 0;
  context_serializer_(reinterpret_cast<MojoMessageHandle>(message_event_),
                      context);

  if (context_destructor_)
    context_destructor_(context);

  has_serialized_handles_ = true;
  return MOJO_RESULT_OK;
}

MojoResult UserMessageImpl::ExtractSerializedHandles(
    ExtractBadHandlePolicy bad_handle_policy,
    MojoHandle* handles) {
  if (!IsSerialized())
    return MOJO_RESULT_FAILED_PRECONDITION;

  if (!has_serialized_handles_)
    return MOJO_RESULT_NOT_FOUND;

  const MessageHeader* header = static_cast<const MessageHeader*>(header_);
  const DispatcherHeader* dispatcher_headers =
      reinterpret_cast<const DispatcherHeader*>(header + 1);

  if (header->num_dispatchers > std::numeric_limits<uint16_t>::max())
    return MOJO_RESULT_ABORTED;

  if (header->num_dispatchers == 0)
    return MOJO_RESULT_OK;

  has_serialized_handles_ = false;

  std::vector<Dispatcher::DispatcherInTransit> dispatchers(
      header->num_dispatchers);

  size_t data_payload_index =
      sizeof(MessageHeader) +
      header->num_dispatchers * sizeof(DispatcherHeader);
  if (data_payload_index > header->header_size)
    return MOJO_RESULT_ABORTED;
  const char* dispatcher_data = reinterpret_cast<const char*>(
      dispatcher_headers + header->num_dispatchers);
  size_t port_index = 0;
  size_t platform_handle_index = 0;
  std::vector<ScopedPlatformHandle> msg_handles =
      channel_message_->TakeHandles();
  for (size_t i = 0; i < header->num_dispatchers; ++i) {
    const DispatcherHeader& dh = dispatcher_headers[i];
    auto type = static_cast<Dispatcher::Type>(dh.type);

    base::CheckedNumeric<size_t> next_payload_index = data_payload_index;
    next_payload_index += dh.num_bytes;
    if (!next_payload_index.IsValid() ||
        header->header_size < next_payload_index.ValueOrDie()) {
      return MOJO_RESULT_ABORTED;
    }

    base::CheckedNumeric<size_t> next_port_index = port_index;
    next_port_index += dh.num_ports;
    if (!next_port_index.IsValid() ||
        message_event_->num_ports() < next_port_index.ValueOrDie()) {
      return MOJO_RESULT_ABORTED;
    }

    base::CheckedNumeric<size_t> next_platform_handle_index =
        platform_handle_index;
    next_platform_handle_index += dh.num_platform_handles;
    if (!next_platform_handle_index.IsValid() ||
        msg_handles.size() < next_platform_handle_index.ValueOrDie()) {
      return MOJO_RESULT_ABORTED;
    }

    ScopedPlatformHandle* out_handles =
        !msg_handles.empty() ? msg_handles.data() + platform_handle_index
                             : nullptr;
    dispatchers[i].dispatcher = Dispatcher::Deserialize(
        type, dispatcher_data, dh.num_bytes,
        message_event_->ports() + port_index, dh.num_ports, out_handles,
        dh.num_platform_handles);
    if (!dispatchers[i].dispatcher &&
        bad_handle_policy == ExtractBadHandlePolicy::kAbort) {
      return MOJO_RESULT_ABORTED;
    }

    dispatcher_data += dh.num_bytes;
    data_payload_index = next_payload_index.ValueOrDie();
    port_index = next_port_index.ValueOrDie();
    platform_handle_index = next_platform_handle_index.ValueOrDie();
  }

  if (!internal::g_core->AddDispatchersFromTransit(dispatchers, handles))
    return MOJO_RESULT_ABORTED;

  return MOJO_RESULT_OK;
}

// static
void UserMessageImpl::FailHandleSerializationForTesting(bool fail) {
  g_always_fail_handle_serialization = fail;
}

UserMessageImpl::UserMessageImpl(ports::UserMessageEvent* message_event)
    : ports::UserMessage(&kUserMessageTypeInfo),
      message_event_(message_event) {}

UserMessageImpl::UserMessageImpl(ports::UserMessageEvent* message_event,
                                 Channel::MessagePtr channel_message,
                                 void* header,
                                 size_t header_size,
                                 void* user_payload,
                                 size_t user_payload_size)
    : ports::UserMessage(&kUserMessageTypeInfo),
      message_event_(message_event),
      channel_message_(std::move(channel_message)),
      has_serialized_handles_(true),
      is_committed_(true),
      header_(header),
      header_size_(header_size),
      user_payload_(user_payload),
      user_payload_size_(user_payload_size) {}

bool UserMessageImpl::WillBeRoutedExternally() {
  MojoResult result = SerializeIfNecessary();
  return result == MOJO_RESULT_OK || result == MOJO_RESULT_FAILED_PRECONDITION;
}

}  // namespace edk
}  // namespace mojo
