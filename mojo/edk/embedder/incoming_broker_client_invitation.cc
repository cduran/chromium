// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mojo/edk/embedder/incoming_broker_client_invitation.h"

#include "base/command_line.h"
#include "base/logging.h"
#include "base/memory/ptr_util.h"
#include "mojo/edk/embedder/embedder_internal.h"
#include "mojo/edk/embedder/platform_channel_pair.h"
#include "mojo/edk/embedder/scoped_platform_handle.h"
#include "mojo/edk/system/core.h"

namespace mojo {
namespace edk {

IncomingBrokerClientInvitation::~IncomingBrokerClientInvitation() = default;

// static
std::unique_ptr<IncomingBrokerClientInvitation>
IncomingBrokerClientInvitation::Accept(ConnectionParams params) {
  return base::WrapUnique(
      new IncomingBrokerClientInvitation(std::move(params)));
}

// static
std::unique_ptr<IncomingBrokerClientInvitation>
IncomingBrokerClientInvitation::AcceptFromCommandLine(
    TransportProtocol protocol) {
  ScopedPlatformHandle platform_channel =
      PlatformChannelPair::PassClientHandleFromParentProcess(
          *base::CommandLine::ForCurrentProcess());
  DCHECK(platform_channel.is_valid());
  return base::WrapUnique(new IncomingBrokerClientInvitation(
      ConnectionParams(protocol, std::move(platform_channel))));
}

ScopedMessagePipeHandle IncomingBrokerClientInvitation::ExtractMessagePipe(
    const std::string& name) {
  return ScopedMessagePipeHandle(MessagePipeHandle(
      internal::g_core->ExtractMessagePipeFromInvitation(name)));
}

IncomingBrokerClientInvitation::IncomingBrokerClientInvitation(
    ConnectionParams params) {
  DCHECK(internal::g_core);
  internal::g_core->AcceptBrokerClientInvitation(std::move(params));
}

}  // namespace edk
}  // namespace mojo
