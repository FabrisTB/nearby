// Copyright 2021 Google LLC
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

#include "connections/implementation/service_controller_router.h"

#include <cstddef>
#include <memory>
#include <string>
#include <utility>

#include "absl/memory/memory.h"
#include "connections/implementation/client_proxy.h"
#include "connections/implementation/offline_service_controller.h"
#include "connections/listeners.h"
#include "connections/params.h"
#include "connections/payload.h"
#include "connections/v3/bandwidth_info.h"
#include "connections/v3/connection_result.h"
#include "connections/v3/connections_device.h"
#include "internal/platform/logging.h"

// TODO(b/285657711): Add tests for uncovered logic, even if trivial.
namespace nearby {
namespace connections {
namespace {
// Length of a MAC address, which consists of 6 bytes uniquely identifying a
// hardware interface.
const std::size_t kMacAddressLength = 6u;

// Length used for an endpoint ID, which identifies a device discovery and
// associated connection request.
const std::size_t kEndpointIdLength = 4u;

// Maximum length for information describing an endpoint; this information is
// advertised by one device and can be used by the other device to identify the
// advertiser.
const std::size_t kMaxEndpointInfoLength = 131u;

bool ClientHasConnectionToAtLeastOneEndpoint(
    ClientProxy* client, const std::vector<std::string>& remote_endpoint_ids) {
  for (auto& endpoint_id : remote_endpoint_ids) {
    if (client->IsConnectedToEndpoint(endpoint_id)) {
      return true;
    }
  }
  return false;
}
}  // namespace

v3::Quality ServiceControllerRouter::GetMediumQuality(Medium medium) {
  switch (medium) {
    case location::nearby::proto::connections::USB:
    case location::nearby::proto::connections::UNKNOWN_MEDIUM:
      return v3::Quality::kUnknown;
    case location::nearby::proto::connections::BLE:
    case location::nearby::proto::connections::NFC:
      return v3::Quality::kLow;
    case location::nearby::proto::connections::BLUETOOTH:
    case location::nearby::proto::connections::BLE_L2CAP:
      return v3::Quality::kMedium;
    case location::nearby::proto::connections::WIFI_HOTSPOT:
    case location::nearby::proto::connections::WIFI_LAN:
    case location::nearby::proto::connections::WIFI_AWARE:
    case location::nearby::proto::connections::WIFI_DIRECT:
    case location::nearby::proto::connections::WEB_RTC:
      return v3::Quality::kHigh;
    default:
      return v3::Quality::kUnknown;
  }
}

ServiceControllerRouter::ServiceControllerRouter() {
  NEARBY_LOGS(INFO) << "ServiceControllerRouter going up.";
}

ServiceControllerRouter::~ServiceControllerRouter() {
  NEARBY_LOGS(INFO) << "ServiceControllerRouter going down.";

  if (service_controller_) {
    service_controller_->Stop();
  }
  // And make sure that cleanup is the last thing we do.
  serializer_.Shutdown();
}

void ServiceControllerRouter::StartAdvertising(
    ClientProxy* client, absl::string_view service_id,
    const AdvertisingOptions& advertising_options,
    const ConnectionRequestInfo& info, const ResultCallback& callback) {
  RouteToServiceController(
      "scr-start-advertising",
      [this, client, service_id = std::string(service_id), advertising_options,
       info, callback]() {
        if (client->IsAdvertising()) {
          callback.result_cb({Status::kAlreadyAdvertising});
          return;
        }

        callback.result_cb(GetServiceController()->StartAdvertising(
            client, service_id, advertising_options, info));
      });
}

void ServiceControllerRouter::StopAdvertising(ClientProxy* client,
                                              const ResultCallback& callback) {
  RouteToServiceController("scr-stop-advertising", [this, client, callback]() {
    if (client->IsAdvertising()) {
      GetServiceController()->StopAdvertising(client);
    }
    callback.result_cb({Status::kSuccess});
  });
}

void ServiceControllerRouter::StartDiscovery(
    ClientProxy* client, absl::string_view service_id,
    const DiscoveryOptions& discovery_options,
    const DiscoveryListener& listener, const ResultCallback& callback) {
  RouteToServiceController(
      "scr-start-discovery",
      [this, client, service_id = std::string(service_id), discovery_options,
       listener, callback]() {
        if (client->IsDiscovering()) {
          callback.result_cb({Status::kAlreadyDiscovering});
          return;
        }

        callback.result_cb(GetServiceController()->StartDiscovery(
            client, service_id, discovery_options, listener));
      });
}

void ServiceControllerRouter::StopDiscovery(ClientProxy* client,
                                            const ResultCallback& callback) {
  RouteToServiceController("scr-stop-discovery", [this, client, callback]() {
    if (client->IsDiscovering()) {
      GetServiceController()->StopDiscovery(client);
    }
    callback.result_cb({Status::kSuccess});
  });
}

void ServiceControllerRouter::InjectEndpoint(
    ClientProxy* client, absl::string_view service_id,
    const OutOfBandConnectionMetadata& metadata,
    const ResultCallback& callback) {
  RouteToServiceController(
      "scr-inject-endpoint",
      [this, client, service_id = std::string(service_id), metadata,
       callback]() {
        // Currently, Bluetooth is the only supported medium for endpoint
        // injection.
        if (metadata.medium != Medium::BLUETOOTH ||
            metadata.remote_bluetooth_mac_address.size() != kMacAddressLength) {
          callback.result_cb({Status::kError});
          return;
        }

        if (metadata.endpoint_id.size() != kEndpointIdLength) {
          callback.result_cb({Status::kError});
          return;
        }

        if (metadata.endpoint_info.Empty() ||
            metadata.endpoint_info.size() > kMaxEndpointInfoLength) {
          callback.result_cb({Status::kError});
          return;
        }

        if (!client->IsDiscovering()) {
          callback.result_cb({Status::kOutOfOrderApiCall});
          return;
        }

        GetServiceController()->InjectEndpoint(client, service_id, metadata);
        callback.result_cb({Status::kSuccess});
      });
}

void ServiceControllerRouter::RequestConnection(
    ClientProxy* client, absl::string_view endpoint_id,
    const ConnectionRequestInfo& info,
    const ConnectionOptions& connection_options,
    const ResultCallback& callback) {
  // Cancellations can be fired from clients anytime, need to add the
  // CancellationListener as soon as possible.
  client->AddCancellationFlag(std::string(endpoint_id));

  RouteToServiceController(
      "scr-request-connection",
      [this, client, endpoint_id = std::string(endpoint_id), info,
       connection_options, callback]() {
        if (client->HasPendingConnectionToEndpoint(endpoint_id) ||
            client->IsConnectedToEndpoint(endpoint_id)) {
          callback.result_cb({Status::kAlreadyConnectedToEndpoint});
          return;
        }

        Status status = GetServiceController()->RequestConnection(
            client, endpoint_id, info, connection_options);
        if (!status.Ok()) {
          client->CancelEndpoint(endpoint_id);
        }
        callback.result_cb(status);
      });
}

void ServiceControllerRouter::AcceptConnection(ClientProxy* client,
                                               absl::string_view endpoint_id,
                                               PayloadListener listener,
                                               const ResultCallback& callback) {
  RouteToServiceController(
      "scr-accept-connection",
      [this, client, endpoint_id = std::string(endpoint_id),
       listener = std::move(listener), callback]() mutable {
        if (client->IsConnectedToEndpoint(endpoint_id)) {
          callback.result_cb({Status::kAlreadyConnectedToEndpoint});
          return;
        }

        if (client->HasLocalEndpointResponded(endpoint_id)) {
          NEARBY_LOGS(WARNING)
              << "Client " << client->GetClientId()
              << " invoked acceptConnectionRequest() after having already "
                 "accepted/rejected the connection to endpoint(id="
              << endpoint_id << ")";
          callback.result_cb({Status::kOutOfOrderApiCall});
          return;
        }

        callback.result_cb(GetServiceController()->AcceptConnection(
            client, endpoint_id, std::move(listener)));
      });
}

void ServiceControllerRouter::RejectConnection(ClientProxy* client,
                                               absl::string_view endpoint_id,
                                               const ResultCallback& callback) {
  client->CancelEndpoint(std::string(endpoint_id));

  RouteToServiceController(
      "scr-reject-connection",
      [this, client, endpoint_id = std::string(endpoint_id), callback]() {
        if (client->IsConnectedToEndpoint(endpoint_id)) {
          callback.result_cb({Status::kAlreadyConnectedToEndpoint});
          return;
        }

        if (client->HasLocalEndpointResponded(endpoint_id)) {
          NEARBY_LOGS(WARNING)
              << "Client " << client->GetClientId()
              << " invoked rejectConnectionRequest() after having already "
                 "accepted/rejected the connection to endpoint(id="
              << endpoint_id << ")";
          callback.result_cb({Status::kOutOfOrderApiCall});
          return;
        }

        callback.result_cb(
            GetServiceController()->RejectConnection(client, endpoint_id));
      });
}

void ServiceControllerRouter::InitiateBandwidthUpgrade(
    ClientProxy* client, absl::string_view endpoint_id,
    const ResultCallback& callback) {
  RouteToServiceController(
      "scr-init-bwu",
      [this, client, endpoint_id = std::string(endpoint_id), callback]() {
        if (!client->IsConnectedToEndpoint(endpoint_id)) {
          callback.result_cb({Status::kOutOfOrderApiCall});
          return;
        }

        GetServiceController()->InitiateBandwidthUpgrade(client, endpoint_id);

        // Operation is triggered; the caller can listen to
        // ConnectionListener::OnBandwidthChanged() to determine its success.
        callback.result_cb({Status::kSuccess});
      });
}

void ServiceControllerRouter::SendPayload(
    ClientProxy* client, absl::Span<const std::string> endpoint_ids,
    Payload payload, const ResultCallback& callback) {
  // Payload is a move-only type.
  // We have to capture it by value inside the lambda, and pass it over to
  // the executor as an std::function<void()> instance.
  // Lambda must be copyable, in order ot satisfy std::function<> requirements.
  // To make it so, we need Payload wrapped by a copyable wrapper.
  // std::shared_ptr<> is used, because it is copyable.
  auto shared_payload = std::make_shared<Payload>(std::move(payload));
  const std::vector<std::string> endpoints =
      std::vector<std::string>(endpoint_ids.begin(), endpoint_ids.end());

  RouteToServiceController("scr-send-payload", [this, client, shared_payload,
                                                endpoints, callback]() {
    if (!ClientHasConnectionToAtLeastOneEndpoint(client, endpoints)) {
      callback.result_cb({Status::kEndpointUnknown});
      return;
    }

    GetServiceController()->SendPayload(client, endpoints,
                                        std::move(*shared_payload));

    // At this point, we've queued up the send Payload request with the
    // ServiceController; any further failures (e.g. one of the endpoints is
    // unknown, goes away, or otherwise fails) will be returned to the
    // client as a PayloadTransferUpdate.
    callback.result_cb({Status::kSuccess});
  });
}

void ServiceControllerRouter::CancelPayload(ClientProxy* client,
                                            std::uint64_t payload_id,
                                            const ResultCallback& callback) {
  RouteToServiceController(
      "scr-cancel-payload", [this, client, payload_id, callback]() {
        callback.result_cb(
            GetServiceController()->CancelPayload(client, payload_id));
      });
}

void ServiceControllerRouter::DisconnectFromEndpoint(
    ClientProxy* client, absl::string_view endpoint_id,
    const ResultCallback& callback) {
  // Client can emit the cancellation at anytime, we need to execute the request
  // without further posting it.
  client->CancelEndpoint(std::string(endpoint_id));

  RouteToServiceController(
      "scr-disconnect-endpoint",
      [this, client, endpoint_id = std::string(endpoint_id), callback]() {
        if (!client->IsConnectedToEndpoint(endpoint_id) &&
            !client->HasPendingConnectionToEndpoint(endpoint_id)) {
          callback.result_cb({Status::kOutOfOrderApiCall});
          return;
        }

        GetServiceController()->DisconnectFromEndpoint(client, endpoint_id);
        callback.result_cb({Status::kSuccess});
      });
}

Status ServiceControllerRouter::StartListeningForIncomingConnectionsV3(
    ClientProxy* client, absl::string_view service_id,
    v3::ConnectionListener listener,
    const v3::ConnectionListeningOptions& options) {
  return GetServiceController()->StartListeningForIncomingConnections(
      client, service_id, std::move(listener), options);
}

void ServiceControllerRouter::StopListeningForIncomingConnectionsV3(
    ClientProxy* client) {
  GetServiceController()->StopListeningForIncomingConnections(client);
}

void ServiceControllerRouter::RequestConnectionV3(
    ClientProxy* client, const NearbyDevice& remote_device,
    v3::ConnectionRequestInfo info, const ConnectionOptions& connection_options,
    const ResultCallback& callback) {
  // Cancellations can be fired from clients anytime, need to add the
  // CancellationListener as soon as possible.
  client->AddCancellationFlag(remote_device.GetEndpointId());

  RouteToServiceController(
      "scr-request-connection",
      [this, client, endpoint_id = remote_device.GetEndpointId(),
       v3_info = std::move(info), connection_options, callback]() mutable {
        if (client->HasPendingConnectionToEndpoint(endpoint_id) ||
            client->IsConnectedToEndpoint(endpoint_id)) {
          callback.result_cb({Status::kAlreadyConnectedToEndpoint});
          return;
        }

        std::string endpoint_info;
        if (v3_info.local_device.GetType() ==
            NearbyDevice::Type::kConnectionsDevice) {
          endpoint_info =
              reinterpret_cast<v3::ConnectionsDevice&>(v3_info.local_device)
                  .GetEndpointInfo();
        }

        ConnectionListener listener = {
            .initiated_cb =
                [&v3_info](
                    const std::string& endpoint_id,
                    const ConnectionResponseInfo& response_info) mutable {
                  v3::InitialConnectionInfo new_info = {
                      .authentication_digits =
                          response_info.authentication_token,
                      .raw_authentication_token =
                          response_info.raw_authentication_token.string_data(),
                      .is_incoming_connection =
                          response_info.is_incoming_connection,
                  };
                  v3::ConnectionsDevice device(
                      endpoint_id,
                      response_info.remote_endpoint_info.AsStringView(), {});
                  v3_info.listener.initiated_cb(device, new_info);
                },
            .accepted_cb =
                [result_cb = v3_info.listener.result_cb](
                    const std::string& endpoint_id) {
                  v3::ConnectionResult result = {
                      .status = {Status::kSuccess},
                  };
                  result_cb(v3::ConnectionsDevice(endpoint_id, "", {}), result);
                },
            .rejected_cb =
                [result_cb = v3_info.listener.result_cb](
                    const std::string& endpoint_id, Status status) {
                  v3::ConnectionResult result = {
                      .status = status,
                  };
                  result_cb(v3::ConnectionsDevice(endpoint_id, "", {}), result);
                },
            .disconnected_cb =
                [&v3_info](const std::string& endpoint_id) mutable {
                  auto device = v3::ConnectionsDevice(endpoint_id, "", {});
                  v3_info.listener.disconnected_cb(device);
                },
            .bandwidth_changed_cb =
                [this, &v3_info](const std::string& endpoint_id,
                                 Medium medium) mutable {
                  v3::BandwidthInfo bandwidth_info = {
                      .quality = GetMediumQuality(medium),
                      .medium = medium,
                  };
                  v3_info.listener.bandwidth_changed_cb(
                      v3::ConnectionsDevice(endpoint_id, "", {}),
                      bandwidth_info);
                },
        };
        ConnectionRequestInfo old_info = {
            .endpoint_info = ByteArray(endpoint_info),
            .listener = std::move(listener),
        };
        Status status = GetServiceController()->RequestConnection(
            client, endpoint_id, std::move(old_info), connection_options);
        if (!status.Ok()) {
          NEARBY_LOGS(WARNING) << "Unable to request connection to endpoint "
                               << endpoint_id << ": " << status.ToString();
          client->CancelEndpoint(endpoint_id);
        }
        callback.result_cb(status);
      });
}

void ServiceControllerRouter::AcceptConnectionV3(
    ClientProxy* client, const NearbyDevice& remote_device,
    v3::PayloadListener listener, const ResultCallback& callback) {
  RouteToServiceController(
      "scr-accept-connection",
      [this, client, endpoint_id = remote_device.GetEndpointId(),
       v3_listener = std::move(listener), callback]() mutable {
        if (client->IsConnectedToEndpoint(endpoint_id)) {
          callback.result_cb({Status::kAlreadyConnectedToEndpoint});
          return;
        }

        if (client->HasLocalEndpointResponded(endpoint_id)) {
          NEARBY_LOGS(WARNING)
              << "Client " << client->GetClientId()
              << " invoked acceptConnectionRequest() after having already "
                 "accepted/rejected the connection to endpoint(id="
              << endpoint_id << ")";
          callback.result_cb({Status::kOutOfOrderApiCall});
          return;
        }

        PayloadListener old_listener = {
            .payload_cb =
                [v3_received = std::move(v3_listener.payload_received_cb)](
                    absl::string_view endpoint_id, Payload payload) {
                  v3_received(v3::ConnectionsDevice(endpoint_id, "", {}),
                              std::move(payload));
                },
            .payload_progress_cb =
                [v3_cb = std::move(v3_listener.payload_progress_cb)](
                    absl::string_view endpoint_id,
                    const PayloadProgressInfo& info) mutable {
                  v3_cb(v3::ConnectionsDevice(endpoint_id, "", {}), info);
                }};

        callback.result_cb(GetServiceController()->AcceptConnection(
            client, endpoint_id, std::move(old_listener)));
      });
}

void ServiceControllerRouter::RejectConnectionV3(
    ClientProxy* client, const NearbyDevice& remote_device,
    const ResultCallback& callback) {
  client->CancelEndpoint(remote_device.GetEndpointId());

  RouteToServiceController(
      "scr-reject-connection",
      [this, client, endpoint_id = remote_device.GetEndpointId(), callback]() {
        if (client->IsConnectedToEndpoint(endpoint_id)) {
          callback.result_cb({Status::kAlreadyConnectedToEndpoint});
          return;
        }

        if (client->HasLocalEndpointResponded(endpoint_id)) {
          NEARBY_LOGS(WARNING)
              << "Client " << client->GetClientId()
              << " invoked rejectConnectionRequest() after having already "
                 "accepted/rejected the connection to endpoint(id="
              << endpoint_id << ")";
          callback.result_cb({Status::kOutOfOrderApiCall});
          return;
        }

        callback.result_cb(
            GetServiceController()->RejectConnection(client, endpoint_id));
      });
}

void ServiceControllerRouter::InitiateBandwidthUpgradeV3(
    ClientProxy* client, const NearbyDevice& remote_device,
    const ResultCallback& callback) {
  RouteToServiceController(
      "scr-init-bwu",
      [this, client, endpoint_id = remote_device.GetEndpointId(), callback]() {
        if (!client->IsConnectedToEndpoint(endpoint_id)) {
          callback.result_cb({Status::kOutOfOrderApiCall});
          return;
        }

        GetServiceController()->InitiateBandwidthUpgrade(client, endpoint_id);

        // Operation is triggered; the caller can listen to
        // ConnectionListener::OnBandwidthChanged() to determine its success.
        callback.result_cb({Status::kSuccess});
      });
}

void ServiceControllerRouter::SendPayloadV3(
    ClientProxy* client, const NearbyDevice& recipient_device, Payload payload,
    const ResultCallback& callback) {
  // Payload is a move-only type.
  // We have to capture it by value inside the lambda, and pass it over to
  // the executor as an std::function<void()> instance.
  // Lambda must be copyable, in order ot satisfy std::function<> requirements.
  // To make it so, we need Payload wrapped by a copyable wrapper.
  // std::shared_ptr<> is used, because it is copyable.
  auto shared_payload = std::make_shared<Payload>(std::move(payload));

  RouteToServiceController(
      "scr-send-payload",
      [this, client, shared_payload,
       endpoint_id = recipient_device.GetEndpointId(), callback]() {
        if (!client->IsConnectedToEndpoint(endpoint_id)) {
          callback.result_cb({Status::kEndpointUnknown});
          return;
        }

        GetServiceController()->SendPayload(
            client, std::vector<std::string>{endpoint_id},
            std::move(*shared_payload));

        // At this point, we've queued up the send Payload request with the
        // ServiceController; any further failures (e.g. one of the endpoints is
        // unknown, goes away, or otherwise fails) will be returned to the
        // client as a PayloadTransferUpdate.
        callback.result_cb({Status::kSuccess});
      });
}

void ServiceControllerRouter::CancelPayloadV3(
    ClientProxy* client, const NearbyDevice& recipient_device,
    uint64_t payload_id, const ResultCallback& callback) {
  RouteToServiceController(
      "scr-cancel-payload", [this, client, payload_id, callback]() {
        callback.result_cb(
            GetServiceController()->CancelPayload(client, payload_id));
      });
}

void ServiceControllerRouter::DisconnectFromDeviceV3(
    ClientProxy* client, const NearbyDevice& remote_device,
    const ResultCallback& callback) {
  // Client can emit the cancellation at anytime, we need to execute the request
  // without further posting it.
  client->CancelEndpoint(remote_device.GetEndpointId());

  RouteToServiceController(
      "scr-disconnect-endpoint",
      [this, client, endpoint_id = remote_device.GetEndpointId(), callback]() {
        if (!client->IsConnectedToEndpoint(endpoint_id) &&
            !client->HasPendingConnectionToEndpoint(endpoint_id)) {
          callback.result_cb({Status::kOutOfOrderApiCall});
          return;
        }

        GetServiceController()->DisconnectFromEndpoint(client, endpoint_id);
        callback.result_cb({Status::kSuccess});
      });
}

void ServiceControllerRouter::UpdateAdvertisingOptionsV3(
    ClientProxy* client, absl::string_view service_id,
    const AdvertisingOptions& options, const ResultCallback& callback) {
  RouteToServiceController(
      "scr-update-advertising-options",
      [this, client, options, callback, service_id]() {
        callback.result_cb(GetServiceController()->UpdateAdvertisingOptions(
            client, service_id, options));
      });
}

void ServiceControllerRouter::UpdateDiscoveryOptionsV3(
    ClientProxy* client, absl::string_view service_id,
    const DiscoveryOptions& options, const ResultCallback& callback) {
  RouteToServiceController(
      "scr-update-discovery-options",
      [this, client, options, callback, service_id]() {
        callback.result_cb(GetServiceController()->UpdateDiscoveryOptions(
            client, service_id, options));
      });
}

void ServiceControllerRouter::StopAllEndpoints(ClientProxy* client,
                                               const ResultCallback& callback) {
  // Client can emit the cancellation at anytime, we need to execute the request
  // without further posting it.
  client->CancelAllEndpoints();

  RouteToServiceController(
      "scr-stop-all-endpoints", [this, client, callback]() {
        NEARBY_LOGS(INFO) << "Client " << client->GetClientId()
                          << " has requested us to stop all endpoints. We will "
                             "now reset the client.";
        FinishClientSession(client);
        callback.result_cb({Status::kSuccess});
      });
}

void ServiceControllerRouter::SetCustomSavePath(
    ClientProxy* client, absl::string_view path,
    const ResultCallback& callback) {
  RouteToServiceController(
      "scr-set-custom-save-path",
      [this, client, path = std::string(path), callback]() {
        NEARBY_LOGS(INFO) << "Client " << client->GetClientId()
                          << " has requested us to set custom save path to "
                          << path;
        GetServiceController()->SetCustomSavePath(client, path);
        callback.result_cb({Status::kSuccess});
      });
}

void ServiceControllerRouter::SetServiceControllerForTesting(
    std::unique_ptr<ServiceController> service_controller) {
  service_controller_ = std::move(service_controller);
}

ServiceController* ServiceControllerRouter::GetServiceController() {
  if (!service_controller_) {
    service_controller_ = std::make_unique<OfflineServiceController>();
  }
  return service_controller_.get();
}

void ServiceControllerRouter::FinishClientSession(ClientProxy* client) {
  // Disconnect from all the connected endpoints tied to this clientProxy.
  for (auto& endpoint_id : client->GetPendingConnectedEndpoints()) {
    GetServiceController()->DisconnectFromEndpoint(client, endpoint_id);
  }

  for (auto& endpoint_id : client->GetConnectedEndpoints()) {
    GetServiceController()->DisconnectFromEndpoint(client, endpoint_id);
  }

  // Stop any advertising and discovery that may be underway due to this client.
  GetServiceController()->StopAdvertising(client);
  GetServiceController()->StopDiscovery(client);
  GetServiceController()->ShutdownBwuManagerExecutors();

  // Finally, clear all state maintained by this client.
  client->Reset();
}

void ServiceControllerRouter::RouteToServiceController(const std::string& name,
                                                       Runnable runnable) {
  serializer_.Execute(name, std::move(runnable));
}

}  // namespace connections
}  // namespace nearby
