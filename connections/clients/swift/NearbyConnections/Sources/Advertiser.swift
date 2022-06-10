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

import NearbyCoreAdapter

public class Advertiser {

  public weak var delegate: AdvertiserDelegate?

  let info: Data
  let serviceId: String
  let adapter: GNCCoreAdapter

  public init(discoveryInfo info: Data, serviceId: String) {
    self.info = info
    self.serviceId = serviceId
    self.adapter = GNCCoreAdapter()
  }

  public func start(strategy: Strategy) {
    let options = GNCAdvertisingOptions(strategy: strategy)
    adapter.startAdvertising(
      asService: serviceId, endpointInfo: info, options: options, delegate: self
    ) { _ in
      // done
    }
  }

  public func stop() {
    adapter.stopAdvertising { _ in
      // done
    }
  }

  deinit {
    stop()
  }
}

extension Advertiser: GNCConnectionDelegate {
  public func connected(
    toEndpoint endpointID: String, withEndpointInfo info: Data, authenticationToken: String
  ) {
    // adapter.acceptConnectionRequest(fromEndpoint)
  }

  public func acceptedConnection(toEndpoint endpointID: String) {

  }

  public func rejectedConnection(toEndpoint endpointID: String, with status: GNCStatus) {

  }

  public func disconnected(fromEndpoint endpointID: String) {

  }
}

public protocol AdvertiserDelegate: AnyObject {

  func advertiser(
    _ advertiser: Advertiser, didReceiveInvitationFromEndpoint endpointID: String,
    withContext context: Data?, invitationHandler: @escaping (Connection?) -> Void)
}
