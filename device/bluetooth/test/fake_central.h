// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef DEVICE_BLUETOOTH_TEST_FAKE_CENTRAL_H_
#define DEVICE_BLUETOOTH_TEST_FAKE_CENTRAL_H_

#include <memory>
#include <string>
#include <vector>

#include "base/compiler_specific.h"
#include "device/bluetooth/bluetooth_adapter.h"
#include "device/bluetooth/public/mojom/test/fake_bluetooth.mojom.h"
#include "mojo/public/cpp/bindings/binding.h"

namespace bluetooth {

class FakePeripheral;
class FakeRemoteGattCharacteristic;
class FakeRemoteGattDescriptor;
class FakeRemoteGattService;

// Implementation of FakeCentral in
// src/device/bluetooth/public/mojom/test/fake_bluetooth.mojom.
// Implemented on top of the C++ device/bluetooth API, mainly
// device/bluetooth/bluetooth_adapter.h.
//
// Not intended for direct use by clients.  See README.md.
class FakeCentral : public mojom::FakeCentral, public device::BluetoothAdapter {
 public:
  FakeCentral(mojom::CentralState state, mojom::FakeCentralRequest request);

  // FakeCentral overrides:
  void SimulatePreconnectedPeripheral(
      const std::string& address,
      const std::string& name,
      const std::vector<device::BluetoothUUID>& known_service_uuids,
      SimulatePreconnectedPeripheralCallback callback) override;
  void SimulateAdvertisementReceived(
      mojom::ScanResultPtr scan_result_ptr,
      SimulateAdvertisementReceivedCallback callback) override;
  void SetNextGATTConnectionResponse(
      const std::string& address,
      uint16_t code,
      SetNextGATTConnectionResponseCallback) override;
  void SetNextGATTDiscoveryResponse(
      const std::string& address,
      uint16_t code,
      SetNextGATTDiscoveryResponseCallback callback) override;
  bool AllResponsesConsumed();
  void SimulateGATTDisconnection(
      const std::string& address,
      SimulateGATTDisconnectionCallback callback) override;
  void SimulateGATTServicesChanged(
      const std::string& address,
      SimulateGATTServicesChangedCallback callback) override;
  void AddFakeService(const std::string& peripheral_address,
                      const device::BluetoothUUID& service_uuid,
                      AddFakeServiceCallback callback) override;
  void RemoveFakeService(const std::string& identifier,
                         const std::string& peripheral_address,
                         RemoveFakeServiceCallback callback) override;
  void AddFakeCharacteristic(const device::BluetoothUUID& characteristic_uuid,
                             mojom::CharacteristicPropertiesPtr properties,
                             const std::string& service_id,
                             const std::string& peripheral_address,
                             AddFakeCharacteristicCallback callback) override;
  void RemoveFakeCharacteristic(
      const std::string& identifier,
      const std::string& service_id,
      const std::string& peripheral_address,
      RemoveFakeCharacteristicCallback callback) override;
  void AddFakeDescriptor(const device::BluetoothUUID& characteristic_uuid,
                         const std::string& characteristic_id,
                         const std::string& service_id,
                         const std::string& peripheral_address,
                         AddFakeDescriptorCallback callback) override;
  void SetNextReadCharacteristicResponse(
      uint16_t gatt_code,
      const base::Optional<std::vector<uint8_t>>& value,
      const std::string& characteristic_id,
      const std::string& service_id,
      const std::string& peripheral_address,
      SetNextReadCharacteristicResponseCallback callback) override;
  void SetNextWriteCharacteristicResponse(
      uint16_t gatt_code,
      const std::string& characteristic_id,
      const std::string& service_id,
      const std::string& peripheral_address,
      SetNextWriteCharacteristicResponseCallback callback) override;
  void SetNextSubscribeToNotificationsResponse(
      uint16_t gatt_code,
      const std::string& characteristic_id,
      const std::string& service_id,
      const std::string& peripheral_address,
      SetNextWriteCharacteristicResponseCallback callback) override;
  void GetLastWrittenValue(const std::string& characteristic_id,
                           const std::string& service_id,
                           const std::string& peripheral_address,
                           GetLastWrittenValueCallback callback) override;
  void SetNextReadDescriptorResponse(
      uint16_t gatt_code,
      const base::Optional<std::vector<uint8_t>>& value,
      const std::string& descriptor_id,
      const std::string& characteristic_id,
      const std::string& service_id,
      const std::string& peripheral_address,
      SetNextReadDescriptorResponseCallback callback) override;

  // BluetoothAdapter overrides:
  std::string GetAddress() const override;
  std::string GetName() const override;
  void SetName(const std::string& name,
               const base::Closure& callback,
               const ErrorCallback& error_callback) override;
  bool IsInitialized() const override;
  bool IsPresent() const override;
  bool IsPowered() const override;
  void SetPowered(bool powered,
                  const base::Closure& callback,
                  const ErrorCallback& error_callback) override;
  bool IsDiscoverable() const override;
  void SetDiscoverable(bool discoverable,
                       const base::Closure& callback,
                       const ErrorCallback& error_callback) override;
  bool IsDiscovering() const override;
  UUIDList GetUUIDs() const override;
  void CreateRfcommService(
      const device::BluetoothUUID& uuid,
      const ServiceOptions& options,
      const CreateServiceCallback& callback,
      const CreateServiceErrorCallback& error_callback) override;
  void CreateL2capService(
      const device::BluetoothUUID& uuid,
      const ServiceOptions& options,
      const CreateServiceCallback& callback,
      const CreateServiceErrorCallback& error_callback) override;
  void RegisterAdvertisement(
      std::unique_ptr<device::BluetoothAdvertisement::Data> advertisement_data,
      const CreateAdvertisementCallback& callback,
      const AdvertisementErrorCallback& error_callback) override;
#if defined(OS_CHROMEOS) || defined(OS_LINUX)
  void SetAdvertisingInterval(
      const base::TimeDelta& min,
      const base::TimeDelta& max,
      const base::Closure& callback,
      const AdvertisementErrorCallback& error_callback) override;
  void ResetAdvertising(
      const base::Closure& callback,
      const AdvertisementErrorCallback& error_callback) override;
#endif
  device::BluetoothLocalGattService* GetGattService(
      const std::string& identifier) const override;
  bool SetPoweredImpl(bool powered) override;
  void AddDiscoverySession(
      device::BluetoothDiscoveryFilter* discovery_filter,
      const base::Closure& callback,
      const DiscoverySessionErrorCallback& error_callback) override;
  void RemoveDiscoverySession(
      device::BluetoothDiscoveryFilter* discovery_filter,
      const base::Closure& callback,
      const DiscoverySessionErrorCallback& error_callback) override;
  void SetDiscoveryFilter(
      std::unique_ptr<device::BluetoothDiscoveryFilter> discovery_filter,
      const base::Closure& callback,
      const DiscoverySessionErrorCallback& error_callback) override;
  void RemovePairingDelegateInternal(
      device::BluetoothDevice::PairingDelegate* pairing_delegate) override;

 private:
  ~FakeCentral() override;

  FakePeripheral* GetFakePeripheral(
      const std::string& peripheral_address) const;
  FakeRemoteGattService* GetFakeRemoteGattService(
      const std::string& peripheral_address,
      const std::string& service_id) const;
  FakeRemoteGattCharacteristic* GetFakeRemoteGattCharacteristic(
      const std::string& peripheral_address,
      const std::string& service_id,
      const std::string& characteristic_id) const;
  FakeRemoteGattDescriptor* GetFakeRemoteGattDescriptor(
      const std::string& peripheral_address,
      const std::string& service_id,
      const std::string& characteristic_id,
      const std::string& descriptor_id) const;

  bool has_pending_or_active_discovery_session_;

  mojom::CentralState state_;
  mojo::Binding<mojom::FakeCentral> binding_;
};

}  // namespace bluetooth

#endif  // DEVICE_BLUETOOTH_TEST_FAKE_CENTRAL_H_
