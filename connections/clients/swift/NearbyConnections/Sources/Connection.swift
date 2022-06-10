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

public enum ConnectionState: Int {
  case notConnected = 0
  case connecting = 1
  case connected = 2
}

public class Connection {

  public weak var delegate: ConnectionDelegate?

  let adapter: GNCCoreAdapter

  public init() {
    self.adapter = GNCCoreAdapter()
  }

  public func send(_ data: Data) {
    adapter.send(GNCBytesPayload2(data: data, identifier: 0), toEndpoints: []) { _ in
      // done
    }
  }

  public func sendResource(
    at resourceURL: URL, withID id: Int64,
    withCompletionHandler completionHandler: ((Error?) -> Void)? = nil
  ) -> Progress? {
    // let payload = GNCFilePayload(fileURL: resourceURL, identifier: id)
    // return connection?.send(payload) { result in
    //     completionHandler?(nil)
    // }
    return nil
  }
}

public protocol ConnectionDelegate: AnyObject {

  func connection(_ connection: Connection, didReceive data: Data, fromEndpoint endpointID: String)

  func connection(
    _ connection: Connection, didStartReceivingResourceWithName resourceName: String,
    fromEndpoint endpointID: String, with progress: Progress)

  func connection(
    _ connection: Connection, didFinishReceivingResourceWithName resourceName: String,
    fromEndpoint endpointID: String, at localURL: URL?, withError error: Error?)

  func connection(
    _ connection: Connection, didReceive stream: InputStream, withName streamName: String,
    fromEndpoint endpointID: String)

  func connection(
    _ connection: Connection, endpoint fromEndpoint: String, didChange state: ConnectionState)
}
