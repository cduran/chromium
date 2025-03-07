// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/smb_client/discovery/network_scanner.h"

#include <map>
#include <string>
#include <utility>

#include "base/bind.h"
#include "chrome/browser/chromeos/smb_client/discovery/in_memory_host_locator.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace chromeos {
namespace smb_client {

namespace {

// Expects |actual_hosts| to equal |expected_hosts|.
void ExpectMapEntriesEqual(const HostMap& expected_hosts,
                           const HostMap& actual_hosts) {
  EXPECT_EQ(expected_hosts, actual_hosts);
}

}  // namespace

class NetworkScannerTest : public testing::Test {
 public:
  NetworkScannerTest() = default;
  ~NetworkScannerTest() override = default;

 protected:
  void RegisterHostLocatorWithHosts(const HostMap& hosts) {
    std::unique_ptr<InMemoryHostLocator> host_locator =
        std::make_unique<InMemoryHostLocator>();
    host_locator->AddHosts(hosts);
    scanner_.RegisterHostLocator(std::move(host_locator));
  }

  void ExpectHostMapEqual(const HostMap& expected_hosts) {
    scanner_.FindHostsInNetwork(
        base::BindOnce(&ExpectMapEntriesEqual, expected_hosts));
  }

 private:
  NetworkScanner scanner_;

  DISALLOW_COPY_AND_ASSIGN(NetworkScannerTest);
};

TEST_F(NetworkScannerTest, ShouldFindNoHostsWithNoLocator) {
  ExpectHostMapEqual(HostMap());
}

TEST_F(NetworkScannerTest, ShouldFindNoHostsWithOneLocator) {
  RegisterHostLocatorWithHosts(HostMap());

  ExpectHostMapEqual(HostMap());
}

TEST_F(NetworkScannerTest, ShouldFindNoHostsWithMultipleLocators) {
  RegisterHostLocatorWithHosts(HostMap());
  RegisterHostLocatorWithHosts(HostMap());

  ExpectHostMapEqual(HostMap());
}

TEST_F(NetworkScannerTest, ShouldFindOneHostWithOneLocator) {
  HostMap hosts;
  hosts["share1"] = "1.2.3.4";
  RegisterHostLocatorWithHosts(hosts);

  ExpectHostMapEqual(hosts);
}

TEST_F(NetworkScannerTest, ShouldFindMultipleHostsWithOneLocator) {
  HostMap hosts;
  hosts["share1"] = "1.2.3.4";
  hosts["share2"] = "5.6.7.8";
  RegisterHostLocatorWithHosts(hosts);

  ExpectHostMapEqual(hosts);
}

TEST_F(NetworkScannerTest, ShouldFindMultipleHostsWithMultipleLocators) {
  HostMap hosts1;
  hosts1["share1"] = "1.2.3.4";
  hosts1["share2"] = "5.6.7.8";
  RegisterHostLocatorWithHosts(hosts1);

  HostMap hosts2;
  hosts2["share3"] = "11.12.13.14";
  hosts2["share4"] = "15.16.17.18";
  RegisterHostLocatorWithHosts(hosts2);

  HostMap expected;
  expected["share1"] = "1.2.3.4";
  expected["share2"] = "5.6.7.8";
  expected["share3"] = "11.12.13.14";
  expected["share4"] = "15.16.17.18";

  ExpectHostMapEqual(expected);
}

TEST_F(NetworkScannerTest, ShouldResolveMultipleHostsWithSameAddress) {
  HostMap hosts1;
  hosts1["share1"] = "1.2.3.4";
  hosts1["share2"] = "5.6.7.8";
  RegisterHostLocatorWithHosts(hosts1);

  HostMap hosts2;
  hosts2["share2"] = "11.12.13.14";
  hosts2["share3"] = "15.16.17.18";
  RegisterHostLocatorWithHosts(hosts2);

  // share2 should have the value from host1 since it is found first.
  HostMap expected;
  expected["share1"] = "1.2.3.4";
  expected["share2"] = "5.6.7.8";
  expected["share3"] = "15.16.17.18";

  ExpectHostMapEqual(expected);
}

}  // namespace smb_client
}  // namespace chromeos
