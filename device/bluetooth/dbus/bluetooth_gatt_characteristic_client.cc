// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "device/bluetooth/dbus/bluetooth_gatt_characteristic_client.h"

#include <stddef.h>

#include "base/bind.h"
#include "base/macros.h"
#include "base/memory/weak_ptr.h"
#include "base/observer_list.h"
#include "base/values.h"
#include "dbus/bus.h"
#include "dbus/object_manager.h"
#include "dbus/values_util.h"
#include "third_party/cros_system_api/dbus/service_constants.h"

namespace bluez {

// static
const char BluetoothGattCharacteristicClient::kNoResponseError[] =
    "org.chromium.Error.NoResponse";
// static
const char BluetoothGattCharacteristicClient::kUnknownCharacteristicError[] =
    "org.chromium.Error.UnknownCharacteristic";

BluetoothGattCharacteristicClient::Properties::Properties(
    dbus::ObjectProxy* object_proxy,
    const std::string& interface_name,
    const PropertyChangedCallback& callback)
    : dbus::PropertySet(object_proxy, interface_name, callback) {
  RegisterProperty(bluetooth_gatt_characteristic::kUUIDProperty, &uuid);
  RegisterProperty(bluetooth_gatt_characteristic::kServiceProperty, &service);
  RegisterProperty(bluetooth_gatt_characteristic::kValueProperty, &value);
  RegisterProperty(bluetooth_gatt_characteristic::kNotifyingProperty,
                   &notifying);
  RegisterProperty(bluetooth_gatt_characteristic::kFlagsProperty, &flags);
}

BluetoothGattCharacteristicClient::Properties::~Properties() = default;

// The BluetoothGattCharacteristicClient implementation used in production.
class BluetoothGattCharacteristicClientImpl
    : public BluetoothGattCharacteristicClient,
      public dbus::ObjectManager::Interface {
 public:
  BluetoothGattCharacteristicClientImpl()
      : object_manager_(NULL), weak_ptr_factory_(this) {}

  ~BluetoothGattCharacteristicClientImpl() override {
    object_manager_->UnregisterInterface(
        bluetooth_gatt_characteristic::kBluetoothGattCharacteristicInterface);
  }

  // BluetoothGattCharacteristicClient override.
  void AddObserver(
      BluetoothGattCharacteristicClient::Observer* observer) override {
    DCHECK(observer);
    observers_.AddObserver(observer);
  }

  // BluetoothGattCharacteristicClient override.
  void RemoveObserver(
      BluetoothGattCharacteristicClient::Observer* observer) override {
    DCHECK(observer);
    observers_.RemoveObserver(observer);
  }

  // BluetoothGattCharacteristicClient override.
  std::vector<dbus::ObjectPath> GetCharacteristics() override {
    DCHECK(object_manager_);
    return object_manager_->GetObjectsWithInterface(
        bluetooth_gatt_characteristic::kBluetoothGattCharacteristicInterface);
  }

  // BluetoothGattCharacteristicClient override.
  Properties* GetProperties(const dbus::ObjectPath& object_path) override {
    DCHECK(object_manager_);
    return static_cast<Properties*>(object_manager_->GetProperties(
        object_path,
        bluetooth_gatt_characteristic::kBluetoothGattCharacteristicInterface));
  }

  // BluetoothGattCharacteristicClient override.
  void ReadValue(const dbus::ObjectPath& object_path,
                 const ValueCallback& callback,
                 const ErrorCallback& error_callback) override {
    dbus::ObjectProxy* object_proxy =
        object_manager_->GetObjectProxy(object_path);
    if (!object_proxy) {
      error_callback.Run(kUnknownCharacteristicError, "");
      return;
    }

    dbus::MethodCall method_call(
        bluetooth_gatt_characteristic::kBluetoothGattCharacteristicInterface,
        bluetooth_gatt_characteristic::kReadValue);

    // Append empty option dict
    dbus::MessageWriter writer(&method_call);
    base::DictionaryValue dict;
    dbus::AppendValueData(&writer, dict);

    object_proxy->CallMethodWithErrorCallback(
        &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
        base::BindOnce(&BluetoothGattCharacteristicClientImpl::OnValueSuccess,
                       weak_ptr_factory_.GetWeakPtr(), callback),
        base::BindOnce(&BluetoothGattCharacteristicClientImpl::OnError,
                       weak_ptr_factory_.GetWeakPtr(), error_callback));
  }

  // BluetoothGattCharacteristicClient override.
  void WriteValue(const dbus::ObjectPath& object_path,
                  const std::vector<uint8_t>& value,
                  const base::Closure& callback,
                  const ErrorCallback& error_callback) override {
    dbus::ObjectProxy* object_proxy =
        object_manager_->GetObjectProxy(object_path);
    if (!object_proxy) {
      error_callback.Run(kUnknownCharacteristicError, "");
      return;
    }

    dbus::MethodCall method_call(
        bluetooth_gatt_characteristic::kBluetoothGattCharacteristicInterface,
        bluetooth_gatt_characteristic::kWriteValue);
    dbus::MessageWriter writer(&method_call);
    writer.AppendArrayOfBytes(value.data(), value.size());

    // Append empty option dict
    base::DictionaryValue dict;
    dbus::AppendValueData(&writer, dict);

    object_proxy->CallMethodWithErrorCallback(
        &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
        base::BindOnce(&BluetoothGattCharacteristicClientImpl::OnSuccess,
                       weak_ptr_factory_.GetWeakPtr(), callback),
        base::BindOnce(&BluetoothGattCharacteristicClientImpl::OnError,
                       weak_ptr_factory_.GetWeakPtr(), error_callback));
  }

  // BluetoothGattCharacteristicClient override.
  void StartNotify(const dbus::ObjectPath& object_path,
                   const base::Closure& callback,
                   const ErrorCallback& error_callback) override {
    dbus::ObjectProxy* object_proxy =
        object_manager_->GetObjectProxy(object_path);
    if (!object_proxy) {
      error_callback.Run(kUnknownCharacteristicError, "");
      return;
    }

    dbus::MethodCall method_call(
        bluetooth_gatt_characteristic::kBluetoothGattCharacteristicInterface,
        bluetooth_gatt_characteristic::kStartNotify);

    object_proxy->CallMethodWithErrorCallback(
        &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
        base::BindOnce(&BluetoothGattCharacteristicClientImpl::OnSuccess,
                       weak_ptr_factory_.GetWeakPtr(), callback),
        base::BindOnce(&BluetoothGattCharacteristicClientImpl::OnError,
                       weak_ptr_factory_.GetWeakPtr(), error_callback));
  }

  // BluetoothGattCharacteristicClient override.
  void StopNotify(const dbus::ObjectPath& object_path,
                  const base::Closure& callback,
                  const ErrorCallback& error_callback) override {
    dbus::ObjectProxy* object_proxy =
        object_manager_->GetObjectProxy(object_path);
    if (!object_proxy) {
      error_callback.Run(kUnknownCharacteristicError, "");
      return;
    }

    dbus::MethodCall method_call(
        bluetooth_gatt_characteristic::kBluetoothGattCharacteristicInterface,
        bluetooth_gatt_characteristic::kStopNotify);

    object_proxy->CallMethodWithErrorCallback(
        &method_call, dbus::ObjectProxy::TIMEOUT_USE_DEFAULT,
        base::BindOnce(&BluetoothGattCharacteristicClientImpl::OnSuccess,
                       weak_ptr_factory_.GetWeakPtr(), callback),
        base::BindOnce(&BluetoothGattCharacteristicClientImpl::OnError,
                       weak_ptr_factory_.GetWeakPtr(), error_callback));
  }

  // dbus::ObjectManager::Interface override.
  dbus::PropertySet* CreateProperties(
      dbus::ObjectProxy* object_proxy,
      const dbus::ObjectPath& object_path,
      const std::string& interface_name) override {
    Properties* properties = new Properties(
        object_proxy, interface_name,
        base::Bind(&BluetoothGattCharacteristicClientImpl::OnPropertyChanged,
                   weak_ptr_factory_.GetWeakPtr(), object_path));
    return static_cast<dbus::PropertySet*>(properties);
  }

  // dbus::ObjectManager::Interface override.
  void ObjectAdded(const dbus::ObjectPath& object_path,
                   const std::string& interface_name) override {
    VLOG(2) << "Remote GATT characteristic added: " << object_path.value();
    for (auto& observer : observers_)
      observer.GattCharacteristicAdded(object_path);
  }

  // dbus::ObjectManager::Interface override.
  void ObjectRemoved(const dbus::ObjectPath& object_path,
                     const std::string& interface_name) override {
    VLOG(2) << "Remote GATT characteristic removed: " << object_path.value();
    for (auto& observer : observers_)
      observer.GattCharacteristicRemoved(object_path);
  }

 protected:
  // bluez::DBusClient override.
  void Init(dbus::Bus* bus) override {
    object_manager_ = bus->GetObjectManager(
        bluetooth_object_manager::kBluetoothObjectManagerServiceName,
        dbus::ObjectPath(
            bluetooth_object_manager::kBluetoothObjectManagerServicePath));
    object_manager_->RegisterInterface(
        bluetooth_gatt_characteristic::kBluetoothGattCharacteristicInterface,
        this);
  }

 private:
  // Called by dbus::PropertySet when a property value is changed, either by
  // result of a signal or response to a GetAll() or Get() call. Informs
  // observers.
  virtual void OnPropertyChanged(const dbus::ObjectPath& object_path,
                                 const std::string& property_name) {
    VLOG(2) << "Remote GATT characteristic property changed: "
            << object_path.value() << ": " << property_name;
    for (auto& observer : observers_)
      observer.GattCharacteristicPropertyChanged(object_path, property_name);
  }

  // Called when a response for successful method call is received.
  void OnSuccess(const base::Closure& callback, dbus::Response* response) {
    DCHECK(response);
    callback.Run();
  }

  // Called when a characteristic value response for a successful method call
  // is received.
  void OnValueSuccess(const ValueCallback& callback, dbus::Response* response) {
    DCHECK(response);
    dbus::MessageReader reader(response);

    const uint8_t* bytes = NULL;
    size_t length = 0;

    if (!reader.PopArrayOfBytes(&bytes, &length))
      VLOG(2) << "Error reading array of bytes in ValueCallback";

    std::vector<uint8_t> value;

    if (bytes)
      value.assign(bytes, bytes + length);

    callback.Run(value);
  }

  // Called when a response for a failed method call is received.
  void OnError(const ErrorCallback& error_callback,
               dbus::ErrorResponse* response) {
    // Error response has optional error message argument.
    std::string error_name;
    std::string error_message;
    if (response) {
      dbus::MessageReader reader(response);
      error_name = response->GetErrorName();
      reader.PopString(&error_message);
    } else {
      error_name = kNoResponseError;
      error_message = "";
    }
    error_callback.Run(error_name, error_message);
  }

  dbus::ObjectManager* object_manager_;

  // List of observers interested in event notifications from us.
  base::ObserverList<BluetoothGattCharacteristicClient::Observer> observers_;

  // Weak pointer factory for generating 'this' pointers that might live longer
  // than we do.
  // Note: This should remain the last member so it'll be destroyed and
  // invalidate its weak pointers before any other members are destroyed.
  base::WeakPtrFactory<BluetoothGattCharacteristicClientImpl> weak_ptr_factory_;

  DISALLOW_COPY_AND_ASSIGN(BluetoothGattCharacteristicClientImpl);
};

BluetoothGattCharacteristicClient::BluetoothGattCharacteristicClient() =
    default;

BluetoothGattCharacteristicClient::~BluetoothGattCharacteristicClient() =
    default;

// static
BluetoothGattCharacteristicClient* BluetoothGattCharacteristicClient::Create() {
  return new BluetoothGattCharacteristicClientImpl();
}

}  // namespace bluez
