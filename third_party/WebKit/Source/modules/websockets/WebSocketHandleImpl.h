/*
 * Copyright (C) 2013 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *
 *     * Redistributions of source code must retain the above copyright
 * notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 * copyright notice, this list of conditions and the following disclaimer
 * in the documentation and/or other materials provided with the
 * distribution.
 *     * Neither the name of Google Inc. nor the names of its
 * contributors may be used to endorse or promote products derived from
 * this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef WebSocketHandleImpl_h
#define WebSocketHandleImpl_h

#include "modules/websockets/WebSocketHandle.h"
#include "mojo/public/cpp/bindings/binding.h"
#include "public/platform/modules/websockets/websocket.mojom-blink.h"

namespace blink {

class WebSocketHandleImpl : public WebSocketHandle,
                            public mojom::blink::WebSocketClient {
 public:
  WebSocketHandleImpl();
  ~WebSocketHandleImpl() override;

  void Initialize(mojom::blink::WebSocketPtr) override;
  void Connect(const KURL&,
               const Vector<String>& protocols,
               const KURL& site_for_cookies,
               const String& user_agent_override,
               WebSocketHandleClient*,
               base::SingleThreadTaskRunner*) override;
  void Send(bool fin, MessageType, const char* data, size_t) override;
  void FlowControl(int64_t quota) override;
  void Close(unsigned short code, const String& reason) override;

 private:
  void Disconnect();
  void OnConnectionError(uint32_t custom_reason,
                         const std::string& description);

  // mojom::blink::WebSocketClient methods:
  void OnFailChannel(const String& reason) override;
  void OnStartOpeningHandshake(
      mojom::blink::WebSocketHandshakeRequestPtr) override;
  void OnFinishOpeningHandshake(
      mojom::blink::WebSocketHandshakeResponsePtr) override;
  void OnAddChannelResponse(const String& selected_protocol,
                            const String& extensions) override;
  void OnDataFrame(bool fin,
                   mojom::blink::WebSocketMessageType,
                   const Vector<uint8_t>& data) override;
  void OnFlowControl(int64_t quota) override;
  void OnDropChannel(bool was_clean,
                     uint16_t code,
                     const String& reason) override;
  void OnClosingHandshake() override;

  WebSocketHandleClient* client_;

  mojom::blink::WebSocketPtr websocket_;
  mojo::Binding<mojom::blink::WebSocketClient> client_binding_;
};

}  // namespace blink

#endif  // WebSocketHandleImpl_h
