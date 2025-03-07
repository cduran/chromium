// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/network/url_request_context_builder_mojo.h"

#include "base/logging.h"
#include "build/build_config.h"
#include "net/proxy_resolution/pac_file_fetcher_impl.h"
#include "net/proxy_resolution/proxy_config_service.h"
#include "services/network/network_context.h"

#if !defined(OS_IOS)
#include "services/network/proxy_service_mojo.h"
#endif

namespace network {

URLRequestContextBuilderMojo::URLRequestContextBuilderMojo()
    : dhcp_fetcher_factory_(new net::DhcpPacFileFetcherFactory()) {}

URLRequestContextBuilderMojo::~URLRequestContextBuilderMojo() = default;

void URLRequestContextBuilderMojo::SetDhcpFetcherFactory(
    std::unique_ptr<net::DhcpPacFileFetcherFactory> dhcp_fetcher_factory) {
  dhcp_fetcher_factory_ = std::move(dhcp_fetcher_factory);
}

void URLRequestContextBuilderMojo::SetMojoProxyResolverFactory(
    proxy_resolver::mojom::ProxyResolverFactoryPtr
        mojo_proxy_resolver_factory) {
  mojo_proxy_resolver_factory_ = std::move(mojo_proxy_resolver_factory);
}

URLRequestContextOwner URLRequestContextBuilderMojo::Create(
    mojom::NetworkContextParams* params,
    bool quic_disabled,
    net::NetLog* net_log) {
  return NetworkContext::ApplyContextParamsToBuilder(
      this, params, quic_disabled, net_log,
      nullptr /* out_static_user_agent_settings */);
}

std::unique_ptr<net::ProxyResolutionService>
URLRequestContextBuilderMojo::CreateProxyService(
    std::unique_ptr<net::ProxyConfigService> proxy_config_service,
    net::URLRequestContext* url_request_context,
    net::HostResolver* host_resolver,
    net::NetworkDelegate* network_delegate,
    net::NetLog* net_log) {
  DCHECK(url_request_context);
  DCHECK(host_resolver);

#if !defined(OS_IOS)
  if (mojo_proxy_resolver_factory_) {
    std::unique_ptr<net::DhcpPacFileFetcher> dhcp_pac_file_fetcher =
        dhcp_fetcher_factory_->Create(url_request_context);
    std::unique_ptr<net::PacFileFetcher> pac_file_fetcher =
        std::make_unique<net::PacFileFetcherImpl>(url_request_context);
    return CreateProxyServiceUsingMojoFactory(
        std::move(mojo_proxy_resolver_factory_),
        std::move(proxy_config_service), std::move(pac_file_fetcher),
        std::move(dhcp_pac_file_fetcher), host_resolver, net_log,
        network_delegate);
  }
#endif

  return net::URLRequestContextBuilder::CreateProxyService(
      std::move(proxy_config_service), url_request_context, host_resolver,
      network_delegate, net_log);
}

}  // namespace network
