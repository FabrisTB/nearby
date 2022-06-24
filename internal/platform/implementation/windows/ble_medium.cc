// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "internal/platform/implementation/windows/ble_medium.h"

#include <chrono>  // NOLINT
#include <future>  // NOLINT
#include <memory>
#include <string>
#include <utility>

#include "absl/strings/escaping.h"
#include "absl/synchronization/mutex.h"
#include "internal/platform/implementation/windows/ble_peripheral.h"
#include "internal/platform/implementation/windows/bluetooth_adapter.h"
#include "internal/platform/implementation/windows/utils.h"
#include "internal/platform/logging.h"
#include "winrt/Windows.Devices.Bluetooth.Advertisement.h"
#include "winrt/Windows.Devices.Bluetooth.h"
#include "winrt/Windows.Foundation.Collections.h"
#include "winrt/Windows.Storage.Streams.h"
#include "winrt/base.h"

namespace location {
namespace nearby {
namespace windows {
namespace {

// Specifies common Bluetooth error cases.
// https://docs.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.bluetootherror?view=winrt-22621
using ::winrt::Windows::Devices::Bluetooth::BluetoothError;

// Represents a Bluetooth LE advertisement payload.
// https://docs.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisement?view=winrt-22621
using ::winrt::Windows::Devices::Bluetooth::Advertisement::
    BluetoothLEAdvertisement;

// Represents a Bluetooth LE advertisement section. A Bluetooth LE advertisement
// packet can contain multiple instances of these
// BluetoothLEAdvertisementDataSection objects.
// https://docs.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisementdatasection?view=winrt-22621
using ::winrt::Windows::Devices::Bluetooth::Advertisement::
    BluetoothLEAdvertisementDataSection;

// Represents the Bluetooth LE advertisement types defined in the Generic Access
// Profile (GAP) by the Bluetooth Special Interest Group (SIG).
// https://docs.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisementdatatypes?view=winrt-22621
using ::winrt::Windows::Devices::Bluetooth::Advertisement::
    BluetoothLEAdvertisementDataTypes;

// Represents an object to send Bluetooth Low Energy (LE) advertisements.
// https://docs.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisementpublisher?view=winrt-22621
using ::winrt::Windows::Devices::Bluetooth::Advertisement::
    BluetoothLEAdvertisementPublisher;

// Represents the possible states of the BluetoothLEAdvertisementPublisher.
// https://docs.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisementpublisherstatus?view=winrt-22621
using ::winrt::Windows::Devices::Bluetooth::Advertisement::
    BluetoothLEAdvertisementPublisherStatus;

// Provides data for a StatusChanged event on a
// BluetoothLEAdvertisementPublisher.
// https://docs.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisementpublisherstatuschangedeventargs?view=winrt-22621
using ::winrt::Windows::Devices::Bluetooth::Advertisement::
    BluetoothLEAdvertisementPublisherStatusChangedEventArgs;

// Provides data for a Received event on a BluetoothLEAdvertisementWatcher. A
// BluetoothLEAdvertisementReceivedEventArgs instance is created when the
// Received event occurs on a BluetoothLEAdvertisementWatcher object.
// https://docs.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisementreceivedeventargs?view=winrt-22621
using ::winrt::Windows::Devices::Bluetooth::Advertisement::
    BluetoothLEAdvertisementReceivedEventArgs;

// Represents an object to receive Bluetooth Low Energy (LE) advertisements.
// https://docs.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisementwatcher?view=winrt-22621
using ::winrt::Windows::Devices::Bluetooth::Advertisement::
    BluetoothLEAdvertisementWatcher;

// Represents the possible states of the BluetoothLEAdvertisementWatcher.
// https://docs.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisementwatcherstatus?view=winrt-22621
using ::winrt::Windows::Devices::Bluetooth::Advertisement::
    BluetoothLEAdvertisementWatcherStatus;

// Provides data for a Stopped event on a BluetoothLEAdvertisementWatcher. A
// BluetoothLEAdvertisementWatcherStoppedEventArgs instance is created when the
// Stopped event occurs on a BluetoothLEAdvertisementWatcher object.
// https://docs.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothleadvertisementwatcherstoppedeventargs?view=winrt-22621
using ::winrt::Windows::Devices::Bluetooth::Advertisement::
    BluetoothLEAdvertisementWatcherStoppedEventArgs;

// Defines constants that specify a Bluetooth LE scanning mode.
// https://docs.microsoft.com/en-us/uwp/api/windows.devices.bluetooth.advertisement.bluetoothlescanningmode?view=winrt-22621
using ::winrt::Windows::Devices::Bluetooth::Advertisement::
    BluetoothLEScanningMode;

// Reads data from an input stream.
// https://docs.microsoft.com/en-us/uwp/api/windows.storage.streams.datareader?view=winrt-22621
using ::winrt::Windows::Storage::Streams::DataReader;

// Writes data to an output stream.
// https://docs.microsoft.com/en-us/uwp/api/windows.storage.streams.datawriter?view=winrt-22621
using ::winrt::Windows::Storage::Streams::DataWriter;

// Represents a time interval as a signed 64-bit integer value.
// https://docs.microsoft.com/en-us/uwp/api/windows.foundation.timespan?view=winrt-22621
using ::winrt::Windows::Foundation::TimeSpan;

template <typename T>
using IVector = winrt::Windows::Foundation::Collections::IVector<T>;

// Copresence Service UUID 0xfef3 (little-endian)
constexpr uint16_t kCopresenceServiceUuid = 0xf3fe;
}  // namespace

BleMedium::BleMedium(api::BluetoothAdapter& adapter)
    : adapter_(static_cast<BluetoothAdapter*>(&adapter)) {}

bool BleMedium::StartAdvertising(
    const std::string& service_id, const ByteArray& advertisement_bytes,
    const std::string& fast_advertisement_service_uuid) {
  absl::MutexLock lock(&mutex_);

  NEARBY_LOGS(INFO) << "Windows Ble StartAdvertising: service_id=" << service_id
                    << ", advertisement bytes= 0x"
                    << absl::BytesToHexString(advertisement_bytes.data()) << "("
                    << advertisement_bytes.size() << "),"
                    << " fast advertisement service uuid= 0x"
                    << absl::BytesToHexString(fast_advertisement_service_uuid);

  DataWriter data_writer;

  // TODO(b/234229562): Add parsing logic for fast_advertisement_service_uuid
  // and insert into the 0x16 Service Data field in the BLE advertisement when
  // Fast Advertisement is enabled. For Extended Advertising, use the same
  // hardcoded Copresence service uuid 0xFEF3.

  // Copresence Service UUID 0xfef3 (little-endian)
  data_writer.WriteUInt16(kCopresenceServiceUuid);

  for (int i = 0; i < advertisement_bytes.size(); ++i) {
    data_writer.WriteByte(
        static_cast<uint8_t>(*(advertisement_bytes.data() + i)));
  }

  BluetoothLEAdvertisementDataSection service_data =
      BluetoothLEAdvertisementDataSection(0x16, data_writer.DetachBuffer());

  IVector<BluetoothLEAdvertisementDataSection> data_sections =
      advertisement_.DataSections();
  data_sections.Append(service_data);
  advertisement_.DataSections() = data_sections;

  // Use Extended Advertising if Fast Advertisement Service Uuid is empty string
  // because the long format advertisement will be used
  if (fast_advertisement_service_uuid.empty()) {
    publisher_ = BluetoothLEAdvertisementPublisher(advertisement_);
    publisher_.UseExtendedAdvertisement(true);

    publisher_token_ =
        publisher_.StatusChanged({this, &BleMedium::PublisherHandler});

    publisher_started_promise_ = std::promise<PublisherState>();

    std::future<PublisherState> publisher_state_future =
        publisher_started_promise_.get_future();

    publisher_.Start();

    return publisher_state_future.get() == PublisherState::kStarted;
  } else {
    // Extended Advertisement not supported, must make sure advertisement_bytes
    // is less than 27 bytes
    if (advertisement_bytes.size() <= 27) {
      publisher_ = BluetoothLEAdvertisementPublisher(advertisement_);
      publisher_.UseExtendedAdvertisement(false);

      publisher_token_ =
          publisher_.StatusChanged({this, &BleMedium::PublisherHandler});

      publisher_started_promise_ = std::promise<PublisherState>();

      std::future<PublisherState> publisher_state_future =
          publisher_started_promise_.get_future();

      publisher_.Start();

      return publisher_state_future.get() == PublisherState::kStarted;
    } else {
      // otherwise no-op
      NEARBY_LOGS(INFO) << "Everyone Mode unavailable for hardware that does "
                           "not support Extended Advertising.";
      return false;
    }
  }
}

bool BleMedium::StopAdvertising(const std::string& service_id) {
  absl::MutexLock lock(&mutex_);

  NEARBY_LOGS(INFO) << "Windows Ble StopAdvertising: service_id=" << service_id;

  publisher_stopped_promise_ = std::promise<PublisherState>();

  std::future<PublisherState> publisher_state_future =
      publisher_stopped_promise_.get_future();

  publisher_.Stop();

  return publisher_state_future.get() == PublisherState::kStopped;
}

bool BleMedium::StartScanning(
    const std::string& service_id,
    const std::string& fast_advertisement_service_uuid,
    DiscoveredPeripheralCallback callback) {
  absl::MutexLock lock(&mutex_);

  NEARBY_LOGS(INFO) << "Windows Ble StartScanning: service_id=" << service_id;

  if (is_watcher_started_) {
    NEARBY_LOGS(WARNING)
        << "BLE cannot start to scan again when it is running.";
    return false;
  }

  service_id_ = service_id;
  advertisement_received_callback_ = std::move(callback);

  watcher_ = BluetoothLEAdvertisementWatcher();
  watcher_token_ = watcher_.Stopped({this, &BleMedium::WatcherHandler});
  advertisement_received_token_ =
      watcher_.Received({this, &BleMedium::AdvertisementReceivedHandler});

  watcher_started_promise_ = std::promise<WatcherState>();

  std::future<WatcherState> watcher_state_future =
      watcher_started_promise_.get_future();

  if (adapter_->IsExtendedAdvertisingSupported()) {
    watcher_.AllowExtendedAdvertisements(true);
  }
  // Active mode indicates that scan request packets will be sent to query for
  // Scan Response
  watcher_.ScanningMode(BluetoothLEScanningMode::Active);
  ::winrt::Windows::Devices::Bluetooth::BluetoothSignalStrengthFilter filter;
  filter.InRangeThresholdInDBm(nullptr);
  filter.OutOfRangeThresholdInDBm(nullptr);
  filter.OutOfRangeTimeout(TimeSpan(std::chrono::seconds(10)));
  filter.SamplingInterval(TimeSpan(std::chrono::seconds(2)));
  watcher_.SignalStrengthFilter(filter);
  watcher_.Start();

  while (!is_watcher_started_) {
    if (watcher_.Status() == BluetoothLEAdvertisementWatcherStatus::Created) {
      watcher_started_promise_.set_value(WatcherState::kStarted);
      NEARBY_LOGS(INFO) << "Windows Ble StartScanning started.";
      is_watcher_started_ = true;
      return true;
    }
  }

  watcher_ = nullptr;
  NEARBY_LOGS(ERROR) << "Windows Ble StartScanning: failed to start service_id="
                     << service_id;
  return false;
}

bool BleMedium::StopScanning(const std::string& service_id) {
  absl::MutexLock lock(&mutex_);
  NEARBY_LOGS(INFO) << "Windows Ble StopScanning: service_id=" << service_id;

  if (!is_watcher_started_) {
    NEARBY_LOGS(WARNING) << "BLE scanning is not running.";
    return true;
  }

  watcher_stopped_promise_ = std::promise<WatcherState>();

  std::future<WatcherState> watcher_state_future =
      watcher_stopped_promise_.get_future();

  watcher_.Stop();

  while (is_watcher_started_) {
    if (watcher_.Status() == BluetoothLEAdvertisementWatcherStatus::Stopped) {
      watcher_stopped_promise_.set_value(WatcherState::kStopped);
      watcher_.Stopped(watcher_token_);
      watcher_.Received(advertisement_received_token_);
      watcher_ = nullptr;
      is_watcher_started_ = false;
      return true;
    }
  }

  NEARBY_LOGS(ERROR) << "Windows Ble StopScanning: failed to stop service_id="
                     << service_id;
  return false;
}

bool BleMedium::StartAcceptingConnections(const std::string& service_id,
                                          AcceptedConnectionCallback callback) {
  NEARBY_LOGS(INFO) << "Windows Ble StartAcceptingConnections: service_id="
                    << service_id;
  return true;
}

bool BleMedium::StopAcceptingConnections(const std::string& service_id) {
  NEARBY_LOGS(INFO) << "Windows Ble StopAcceptingConnections: service_id="
                    << service_id;
  return true;
}

std::unique_ptr<api::BleSocket> BleMedium::Connect(
    api::BlePeripheral& remote_peripheral, const std::string& service_id,
    CancellationFlag* cancellation_flag) {
  if (cancellation_flag->Cancelled()) {
    NEARBY_LOGS(ERROR) << "Windows BLE Connect: Has been cancelled: "
                          "service_id="
                       << service_id;
    return {};
  }

  NEARBY_LOGS(ERROR) << "Windows Ble Connect: Cannot connect over BLE socket. "
                        "service_id="
                     << service_id;
  return {};
}

void BleMedium::PublisherHandler(
    BluetoothLEAdvertisementPublisher publisher,
    BluetoothLEAdvertisementPublisherStatusChangedEventArgs args) {
  switch (args.Status()) {
    case BluetoothLEAdvertisementPublisherStatus::Started:
      publisher_started_promise_.set_value(PublisherState::kStarted);
      break;
    case BluetoothLEAdvertisementPublisherStatus::Stopped:
      publisher_stopped_promise_.set_value(PublisherState::kStopped);
      publisher_.StatusChanged(publisher_token_);
      break;
    case BluetoothLEAdvertisementPublisherStatus::Aborted:
      switch (args.Error()) {
        case BluetoothError::Success:
          if (publisher_.Status() ==
              BluetoothLEAdvertisementPublisherStatus::Started) {
            NEARBY_LOGS(ERROR)
                << "Nearby BLE Medium start advertising operation was "
                   "successfully completed or serviced.";
            publisher_started_promise_.set_value(PublisherState::kStarted);
          }
          if (publisher_.Status() ==
              BluetoothLEAdvertisementPublisherStatus::Stopped) {
            NEARBY_LOGS(ERROR)
                << "Nearby BLE Medium stop advertising operation was "
                   "successfully completed or serviced.";
            publisher_stopped_promise_.set_value(PublisherState::kStopped);
            publisher_.StatusChanged(publisher_token_);
          } else {
            NEARBY_LOGS(ERROR) << "Nearby BLE Medium advertising failed due to "
                                  "unknown errors.";
            publisher_started_promise_.set_value(PublisherState::kError);
            publisher_stopped_promise_.set_value(PublisherState::kError);
          }
          break;
        case BluetoothError::RadioNotAvailable:
          NEARBY_LOGS(ERROR) << "Nearby BLE Medium advertising failed due to "
                                "radio not available.";
          publisher_started_promise_.set_value(PublisherState::kError);
          publisher_stopped_promise_.set_value(PublisherState::kError);
          break;
        case BluetoothError::ResourceInUse:
          NEARBY_LOGS(ERROR)
              << "Nearby BLE Medium advertising failed due to resource in use.";
          publisher_started_promise_.set_value(PublisherState::kError);
          publisher_stopped_promise_.set_value(PublisherState::kError);
          break;
        case BluetoothError::DeviceNotConnected:
          NEARBY_LOGS(ERROR) << "Nearby BLE Medium advertising failed due to "
                                "remote device is not connected.";
          publisher_started_promise_.set_value(PublisherState::kError);
          publisher_stopped_promise_.set_value(PublisherState::kError);
          break;
        case BluetoothError::DisabledByPolicy:
          NEARBY_LOGS(ERROR) << "Nearby BLE Medium advertising failed due to "
                                "disabled by policy.";
          publisher_started_promise_.set_value(PublisherState::kError);
          publisher_stopped_promise_.set_value(PublisherState::kError);
          break;
        case BluetoothError::DisabledByUser:
          NEARBY_LOGS(ERROR) << "Nearby BLE Medium advertising failed due to "
                                "disabled by user.";
          publisher_started_promise_.set_value(PublisherState::kError);
          publisher_stopped_promise_.set_value(PublisherState::kError);
          break;
        case BluetoothError::NotSupported:
          NEARBY_LOGS(ERROR) << "Nearby BLE Medium advertising failed due to "
                                "hardware not supported.";
          publisher_started_promise_.set_value(PublisherState::kError);
          publisher_stopped_promise_.set_value(PublisherState::kError);
          break;
        case BluetoothError::TransportNotSupported:
          NEARBY_LOGS(ERROR) << "Nearby BLE Medium advertising failed due to "
                                "transport not supported.";
          publisher_started_promise_.set_value(PublisherState::kError);
          publisher_stopped_promise_.set_value(PublisherState::kError);
          break;
        case BluetoothError::ConsentRequired:
          NEARBY_LOGS(ERROR) << "Nearby BLE Medium advertising failed due to "
                                "consent required.";
          publisher_started_promise_.set_value(PublisherState::kError);
          publisher_stopped_promise_.set_value(PublisherState::kError);
          break;
        case BluetoothError::OtherError:
        default:
          NEARBY_LOGS(ERROR)
              << "Nearby BLE Medium advertising failed due to unknown errors.";
          publisher_started_promise_.set_value(PublisherState::kError);
          publisher_stopped_promise_.set_value(PublisherState::kError);
          break;
      }
      break;
    default:
      break;
  }
}

void BleMedium::WatcherHandler(
    BluetoothLEAdvertisementWatcher watcher,
    BluetoothLEAdvertisementWatcherStoppedEventArgs args) {
  switch (args.Error()) {
    case BluetoothError::Success:
      if (watcher_.Status() == BluetoothLEAdvertisementWatcherStatus::Started) {
        NEARBY_LOGS(ERROR) << "Nearby BLE Medium start scanning operation was "
                              "successfully completed or serviced.";
        watcher_started_promise_.set_value(WatcherState::kStarted);
      }
      if (watcher_.Status() == BluetoothLEAdvertisementWatcherStatus::Stopped) {
        NEARBY_LOGS(ERROR) << "Nearby BLE Medium stop scanning operation was "
                              "successfully completed or serviced.";
        watcher_stopped_promise_.set_value(WatcherState::kStopped);
        watcher_.Stopped(watcher_token_);
        watcher_.Received(advertisement_received_token_);
      } else {
        NEARBY_LOGS(ERROR)
            << "Nearby BLE Medium scanning failed due to unknown errors.";
        watcher_started_promise_.set_value(WatcherState::kError);
        watcher_stopped_promise_.set_value(WatcherState::kError);
      }
      break;
    case BluetoothError::RadioNotAvailable:
      NEARBY_LOGS(ERROR)
          << "Nearby BLE Medium scanning failed due to radio not available.";
      watcher_started_promise_.set_value(WatcherState::kError);
      watcher_stopped_promise_.set_value(WatcherState::kError);
      break;
    case BluetoothError::ResourceInUse:
      NEARBY_LOGS(ERROR)
          << "Nearby BLE Medium scanning failed due to resource in use.";
      watcher_started_promise_.set_value(WatcherState::kError);
      watcher_stopped_promise_.set_value(WatcherState::kError);
      break;
    case BluetoothError::DeviceNotConnected:
      NEARBY_LOGS(ERROR) << "Nearby BLE Medium scanning failed due to "
                            "remote device is not connected.";
      watcher_started_promise_.set_value(WatcherState::kError);
      watcher_stopped_promise_.set_value(WatcherState::kError);
      break;
    case BluetoothError::DisabledByPolicy:
      NEARBY_LOGS(ERROR)
          << "Nearby BLE Medium scanning failed due to disabled by policy.";
      watcher_started_promise_.set_value(WatcherState::kError);
      watcher_stopped_promise_.set_value(WatcherState::kError);
      break;
    case BluetoothError::DisabledByUser:
      NEARBY_LOGS(ERROR)
          << "Nearby BLE Medium scanning failed due to disabled by user.";
      watcher_started_promise_.set_value(WatcherState::kError);
      watcher_stopped_promise_.set_value(WatcherState::kError);
      break;
    case BluetoothError::NotSupported:
      NEARBY_LOGS(ERROR)
          << "Nearby BLE Medium scanning failed due to hardware not supported.";
      watcher_started_promise_.set_value(WatcherState::kError);
      watcher_stopped_promise_.set_value(WatcherState::kError);
      break;
    case BluetoothError::TransportNotSupported:
      NEARBY_LOGS(ERROR) << "Nearby BLE Medium scanning failed due to "
                            "transport not supported.";
      watcher_started_promise_.set_value(WatcherState::kError);
      watcher_stopped_promise_.set_value(WatcherState::kError);
      break;
    case BluetoothError::ConsentRequired:
      NEARBY_LOGS(ERROR)
          << "Nearby BLE Medium scanning failed due to consent required.";
      watcher_started_promise_.set_value(WatcherState::kError);
      watcher_stopped_promise_.set_value(WatcherState::kError);
      break;
    case BluetoothError::OtherError:
      NEARBY_LOGS(ERROR)
          << "Nearby BLE Medium scanning failed due to unknown errors.";
      watcher_started_promise_.set_value(WatcherState::kError);
      watcher_stopped_promise_.set_value(WatcherState::kError);
      break;
    default:
      if (watcher_.Status() == BluetoothLEAdvertisementWatcherStatus::Started) {
        watcher_started_promise_.set_value(WatcherState::kStarted);
      }
      if (watcher_.Status() == BluetoothLEAdvertisementWatcherStatus::Stopped) {
        watcher_stopped_promise_.set_value(WatcherState::kStopped);
        watcher_.Stopped(watcher_token_);
        watcher_.Received(advertisement_received_token_);
      } else {
        NEARBY_LOGS(ERROR)
            << "Nearby BLE Medium scanning failed due to unknown errors.";
        watcher_started_promise_.set_value(WatcherState::kError);
        watcher_stopped_promise_.set_value(WatcherState::kError);
      }
      break;
  }
}

void BleMedium::AdvertisementReceivedHandler(
    BluetoothLEAdvertisementWatcher watcher,
    BluetoothLEAdvertisementReceivedEventArgs args) {
  // Handle all BLE advertisements and determine whether the BLE Medium
  // Advertisement Scan Response packet (containing Copresence UUID 0xFEF3 in
  // 0x16 Service Data) has been received in the handler
  BluetoothLEAdvertisement advertisement = args.Advertisement();

  for (BluetoothLEAdvertisementDataSection service_data :
       advertisement.GetSectionsByType(0x16)) {
    // Parse Advertisement Data for Section 0x16 (Service Data)
    DataReader data_reader = DataReader::FromBuffer(service_data.Data());

    // Discard the first 2 bytes of Service Uuid in Service Data
    uint8_t first_byte = data_reader.ReadByte();   // 0xf3
    uint8_t second_byte = data_reader.ReadByte();  // 0xfe

    if (first_byte == 0xf3 && second_byte == 0xfe) {
      std::string data;

      uint8_t unconsumed_buffer_length = data_reader.UnconsumedBufferLength();
      for (int i = 0; i < unconsumed_buffer_length; i++) {
        data.append(1, static_cast<unsigned char>(data_reader.ReadByte()));
      }

      ByteArray advertisement_data(data);

      NEARBY_LOGS(VERBOSE)
          << "Nearby BLE Medium 0xFEF3 Advertisement discovered. "
             "0x16 Service data: advertisement bytes= 0x"
          << absl::BytesToHexString(advertisement_data.data()) << "("
          << advertisement_data.size() << ")";

      std::string peripheral_name =
          uint64_to_mac_address_string(args.BluetoothAddress());

      std::unique_ptr<BlePeripheral> peripheral =
          std::make_unique<BlePeripheral>();
      peripheral->SetName(peripheral_name);
      peripheral->SetAdvertisementBytes(advertisement_data);
      BlePeripheral* peripheral_ptr = peripheral.get();

      {
        absl::MutexLock lock(&peripheral_map_mutex_);
        peripheral_map_[peripheral_name] = std::move(peripheral);
      }

      // Received Fast Advertisement packet
      if (unconsumed_buffer_length <= 27) {
        NEARBY_LOGS(INFO)
            << "Sending Fast Advertisement packet for processing.";
        advertisement_received_callback_.peripheral_discovered_cb(
            /*ble_peripheral*/ *peripheral_ptr, /*service_id*/ service_id_,
            /*is_fast_advertisement*/ true);
      } else {
        // Received Extended Advertising packet
        NEARBY_LOGS(INFO)
            << "Sending Extended Advertising packet for processing.";
        advertisement_received_callback_.peripheral_discovered_cb(
            /*ble_peripheral*/ *peripheral_ptr, /*service_id*/ service_id_,
            /*is_fast_advertisement*/ false);
      }
    }
  }
}

}  // namespace windows
}  // namespace nearby
}  // namespace location
