// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef DEVICE_FIDO_MOCK_FIDO_DEVICE_H_
#define DEVICE_FIDO_MOCK_FIDO_DEVICE_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "base/component_export.h"
#include "base/macros.h"
#include "device/fido/fido_device.h"
#include "testing/gmock/include/gmock/gmock.h"

namespace device {

class MockFidoDevice : public FidoDevice {
 public:
  MockFidoDevice();
  ~MockFidoDevice() override;

  // GMock cannot mock a method taking a move-only type.
  // TODO(crbug.com/729950): Remove these workarounds once support for move-only
  // types is added to GMock.
  MOCK_METHOD1(TryWinkRef, void(WinkCallback& cb));
  void TryWink(WinkCallback cb) override;

  MOCK_CONST_METHOD0(GetId, std::string(void));
  // GMock cannot mock a method taking a move-only type.
  // TODO(crbug.com/729950): Remove these workarounds once support for move-only
  // types is added to GMock.
  MOCK_METHOD2(DeviceTransactPtr,
               void(const std::vector<uint8_t>& command, DeviceCallback& cb));
  void DeviceTransact(std::vector<uint8_t> command, DeviceCallback cb) override;

  base::WeakPtr<FidoDevice> GetWeakPtr() override;
  static void NotSatisfied(const std::vector<uint8_t>& command,
                           DeviceCallback& cb);
  static void WrongData(const std::vector<uint8_t>& command,
                        DeviceCallback& cb);
  static void NoErrorSign(const std::vector<uint8_t>& command,
                          DeviceCallback& cb);
  static void NoErrorRegister(const std::vector<uint8_t>& command,
                              DeviceCallback& cb);
  static void NoErrorVersion(const std::vector<uint8_t>& command,
                             DeviceCallback& cb);
  static void SignWithCorruptedResponse(const std::vector<uint8_t>& command,
                                        DeviceCallback& cb);
  static void WinkDoNothing(WinkCallback& cb);

 private:
  base::WeakPtrFactory<FidoDevice> weak_factory_;

  DISALLOW_COPY_AND_ASSIGN(MockFidoDevice);
};

}  // namespace device

#endif  // DEVICE_FIDO_MOCK_FIDO_DEVICE_H_
