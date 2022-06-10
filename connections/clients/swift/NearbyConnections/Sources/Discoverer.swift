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

public class Discoverer {

  public weak var delegate: DiscovererDelegate?

  let serviceId: String
  let adapter: GNCCoreAdapter

  public init(serviceId: String) {
    self.serviceId = serviceId
    self.adapter = GNCCoreAdapter()
  }

  public func start(strategy: Strategy) {
    let options = GNCDiscoveryOptions(strategy: strategy)
    adapter.startDiscovery(asService: serviceId, options: options, delegate: self) { _ in
      // done
    }
  }

  public func invite(_ endpointID: String, to connection: Connection, withEndpointInfo info: Data) {
    // TODO: temporarilly add connection object to a map for the endpoint
    // this will be used give the delegate to the accept connection request function
    let options = GNCConnectionOptions()
    adapter.requestConnection(
      toEndpoint: endpointID, endpointInfo: info, options: options, delegate: nil
    ) { _ in
      // done
    }
  }

  public func stop() {
    adapter.stopDiscovery { _ in
      // done
    }
  }

  deinit {
    stop()
  }
}

extension Discoverer: GNCDiscoveryDelegate {

  public func foundEndpoint(_ endpointID: String, withEndpointInfo info: Data) {
    delegate?.discoverer(self, foundEndpoint: endpointID, withEndpointInfo: info)
  }

  public func lostEndpoint(_ endpointID: String) {
    delegate?.discoverer(self, lostEndpoint: endpointID)
  }
}

// extension Advertiser: GNCConnectionDelegate {
//   public func connected(
//     toEndpoint endpointID: String, withEndpointInfo info: Data, authenticationToken: String
//   ) {
//     // adapter.acceptConnectionRequest(fromEndpoint)
//   }

//   public func acceptedConnection(toEndpoint endpointID: String) {

//   }

//   public func rejectedConnection(toEndpoint endpointID: String, with status: GNCStatus) {

//   }

//   public func disconnected(fromEndpoint endpointID: String) {

//   }
// }

public protocol DiscovererDelegate: AnyObject {

  func discoverer(
    _ discoverer: Discoverer, foundEndpoint endpointID: String, withEndpointInfo info: Data)

  func discoverer(_ discoverer: Discoverer, lostEndpoint endpointID: String)
}
