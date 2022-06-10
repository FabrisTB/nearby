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

#import "connections/clients/swift/NearbyCoreAdapter/Sources/Public/NearbyCoreAdapter/GNCCoreAdapter.h"

#import <Foundation/Foundation.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "connections/core.h"

#import "connections/clients/swift/NearbyCoreAdapter/Sources/GNCAdvertisingOptions+Internal.h"
#import "connections/clients/swift/NearbyCoreAdapter/Sources/GNCConnectionOptions+Internal.h"
#import "connections/clients/swift/NearbyCoreAdapter/Sources/GNCDiscoveryOptions+Internal.h"
#import "connections/clients/swift/NearbyCoreAdapter/Sources/GNCPayload+Internal.h"
#import "connections/clients/swift/NearbyCoreAdapter/Sources/Public/NearbyCoreAdapter/GNCAdvertisingOptions.h"
#import "connections/clients/swift/NearbyCoreAdapter/Sources/Public/NearbyCoreAdapter/GNCConnectionDelegate.h"
#import "connections/clients/swift/NearbyCoreAdapter/Sources/Public/NearbyCoreAdapter/GNCConnectionOptions.h"
#import "connections/clients/swift/NearbyCoreAdapter/Sources/Public/NearbyCoreAdapter/GNCDiscoveryDelegate.h"
#import "connections/clients/swift/NearbyCoreAdapter/Sources/Public/NearbyCoreAdapter/GNCDiscoveryOptions.h"
#import "connections/clients/swift/NearbyCoreAdapter/Sources/Public/NearbyCoreAdapter/GNCPayload.h"
#import "connections/clients/swift/NearbyCoreAdapter/Sources/Public/NearbyCoreAdapter/GNCPayloadDelegate.h"
#import "connections/clients/swift/NearbyCoreAdapter/Sources/Public/NearbyCoreAdapter/GNCStatus.h"

using ::location::nearby::ByteArray;
using ::location::nearby::connections::AdvertisingOptions;
using ::location::nearby::connections::ConnectionListener;
using ::location::nearby::connections::ConnectionOptions;
using ::location::nearby::connections::ConnectionRequestInfo;
using ::location::nearby::connections::ConnectionResponseInfo;
using ::location::nearby::connections::Core;
using ::location::nearby::connections::DiscoveryListener;
using ::location::nearby::connections::DiscoveryOptions;
using ::location::nearby::connections::Payload;
using ::location::nearby::connections::PayloadListener;
using ::location::nearby::connections::PayloadProgressInfo;
using ResultListener = ::location::nearby::connections::ResultCallback;
using ::location::nearby::connections::ServiceControllerRouter;
using ::location::nearby::connections::Status;

NSString *const GNCErrorDomain = @"com.google.ios.CoreAdapter.ErrorDomain";

// TODO(bourdakos@): Look into how to do singletons in objective-c

@interface GNCCoreAdapter () {
  std::unique_ptr<Core> _core;
  std::unique_ptr<ServiceControllerRouter> _serviceControllerRouter;
}

@end

@implementation GNCCoreAdapter

- (instancetype)init {
  self = [super init];
  if (self) {
    _serviceControllerRouter = std::make_unique<ServiceControllerRouter>();
    _core = std::make_unique<Core>(_serviceControllerRouter.get());
  }
  return self;
}

- (void)dealloc {
  _core.reset();
  _serviceControllerRouter.reset();
}

- (void)startAdvertisingAsService:(NSString *)serviceID
                     endpointInfo:(NSData *)endpointInfo
                          options:(GNCAdvertisingOptions *)advertisingOptions
                         delegate:(id<GNCConnectionDelegate>)delegate
            withCompletionHandler:(void (^)(NSError *error))completionHandler {
  std::string service_id = [serviceID cStringUsingEncoding:[NSString defaultCStringEncoding]];

  AdvertisingOptions advertising_options = [advertisingOptions toCpp];

  ConnectionListener listener;
  listener.initiated_cb = ^(const std::string &endpoint_id, const ConnectionResponseInfo &info) {
    NSString *endpointID = @(endpoint_id.c_str());
    NSData *endpointInfo = [NSData dataWithBytes:info.remote_endpoint_info.data()
                                          length:info.remote_endpoint_info.size()];
    NSString *authenticationToken = @(info.authentication_token.c_str());
    [delegate connectedToEndpoint:endpointID
                 withEndpointInfo:endpointInfo
              authenticationToken:authenticationToken];
  };
  listener.accepted_cb = ^(const std::string &endpoint_id) {
    NSString *endpointID = @(endpoint_id.c_str());
    [delegate acceptedConnectionToEndpoint:endpointID];
  };
  listener.rejected_cb = ^(const std::string &endpoint_id, Status status) {
    NSString *endpointID = @(endpoint_id.c_str());
    // TODO(bourdakos@): map c++ status to GNCStatus
    [delegate rejectedConnectionToEndpoint:endpointID withStatus:GNCStatusError];
  };
  listener.disconnected_cb = ^(const std::string &endpoint_id) {
    NSString *endpointID = @(endpoint_id.c_str());
    [delegate disconnectedFromEndpoint:endpointID];
  };

  ConnectionRequestInfo connection_request_info;
  connection_request_info.endpoint_info =
      ByteArray((const char *)endpointInfo.bytes, endpointInfo.length);
  connection_request_info.listener = std::move(listener);

  ResultListener result;
  result.result_cb = ^(Status status) {
    NSError *err = nil;
    if (!status.Ok()) {
      // TODO(bourdakos@): handle errors better
      err = [NSError errorWithDomain:GNCErrorDomain code:0 userInfo:nil];
    }
    completionHandler(err);
  };

  _core->StartAdvertising(service_id, advertising_options, connection_request_info, result);
}

- (void)stopAdvertisingWithCompletionHandler:(void (^)(NSError *error))completionHandler {
  ResultListener result;
  result.result_cb = ^(Status status) {
    NSError *err = nil;
    if (!status.Ok()) {
      // TODO(bourdakos@): handle errors better
      err = [NSError errorWithDomain:GNCErrorDomain code:0 userInfo:nil];
    }
    completionHandler(err);
  };

  _core->StopAdvertising(result);
}

- (void)startDiscoveryAsService:(NSString *)serviceID
                        options:(GNCDiscoveryOptions *)discoveryOptions
                       delegate:(id<GNCDiscoveryDelegate>)delegate
          withCompletionHandler:(void (^)(NSError *error))completionHandler {
  std::string service_id = [serviceID cStringUsingEncoding:[NSString defaultCStringEncoding]];

  DiscoveryOptions discovery_options = [discoveryOptions toCpp];

  DiscoveryListener listener;
  listener.endpoint_found_cb = ^(const std::string &endpoint_id, const ByteArray &endpoint_info,
                                 const std::string &service_id) {
    NSString *endpointID = @(endpoint_id.c_str());
    NSData *info = [NSData dataWithBytes:endpoint_info.data() length:endpoint_info.size()];
    [delegate foundEndpoint:endpointID withEndpointInfo:info];
  };
  listener.endpoint_lost_cb = ^(const std::string &endpoint_id) {
    NSString *endpointID = @(endpoint_id.c_str());
    [delegate lostEndpoint:endpointID];
  };

  ResultListener result;
  result.result_cb = ^(Status status) {
    NSError *err = nil;
    if (!status.Ok()) {
      // TODO(bourdakos@): handle errors better
      err = [NSError errorWithDomain:GNCErrorDomain code:0 userInfo:nil];
    }
    completionHandler(err);
  };

  _core->StartDiscovery(service_id, discovery_options, std::move(listener), result);
}

- (void)stopDiscoveryWithCompletionHandler:(void (^)(NSError *error))completionHandler {
  ResultListener result;
  result.result_cb = ^(Status status) {
    NSError *err = nil;
    if (!status.Ok()) {
      // TODO(bourdakos@): handle errors better
      err = [NSError errorWithDomain:GNCErrorDomain code:0 userInfo:nil];
    }
    completionHandler(err);
  };

  _core->StopDiscovery(result);
}

- (void)requestConnectionToEndpoint:(NSString *)endpointID
                       endpointInfo:(NSData *)endpointInfo
                            options:(GNCConnectionOptions *)connectionOptions
                           delegate:(id<GNCConnectionDelegate>)delegate
              withCompletionHandler:(void (^)(NSError *error))completionHandler {
  std::string endpoint_id = [endpointID cStringUsingEncoding:[NSString defaultCStringEncoding]];

  ConnectionListener listener;
  listener.initiated_cb = ^(const std::string &endpoint_id, const ConnectionResponseInfo &info) {
    NSString *endpointID = @(endpoint_id.c_str());
    NSData *endpointInfo = [NSData dataWithBytes:info.remote_endpoint_info.data()
                                          length:info.remote_endpoint_info.size()];
    NSString *authenticationToken = @(info.authentication_token.c_str());
    [delegate connectedToEndpoint:endpointID
                 withEndpointInfo:endpointInfo
              authenticationToken:authenticationToken];
  };
  listener.accepted_cb = ^(const std::string &endpoint_id) {
    NSString *endpointID = @(endpoint_id.c_str());
    [delegate acceptedConnectionToEndpoint:endpointID];
  };
  listener.rejected_cb = ^(const std::string &endpoint_id, Status status) {
    NSString *endpointID = @(endpoint_id.c_str());
    // TODO(bourdakos@): map c++ status to GNCStatus
    [delegate rejectedConnectionToEndpoint:endpointID withStatus:GNCStatusError];
  };
  listener.disconnected_cb = ^(const std::string &endpoint_id) {
    NSString *endpointID = @(endpoint_id.c_str());
    [delegate disconnectedFromEndpoint:endpointID];
  };

  ConnectionRequestInfo connection_request_info;
  connection_request_info.endpoint_info =
      ByteArray((const char *)endpointInfo.bytes, endpointInfo.length);
  connection_request_info.listener = std::move(listener);

  ConnectionOptions connection_options = [connectionOptions toCpp];

  ResultListener result;
  result.result_cb = ^(Status status) {
    NSError *err = nil;
    if (!status.Ok()) {
      // TODO(bourdakos@): handle errors better
      err = [NSError errorWithDomain:GNCErrorDomain code:0 userInfo:nil];
    }
    completionHandler(err);
  };

  _core->RequestConnection(endpoint_id, connection_request_info, connection_options, result);
}

- (void)acceptConnectionRequestFromEndpoint:(NSString *)endpointID
                                   delegate:(id<GNCPayloadDelegate>)delegate
                      withCompletionHandler:(void (^)(NSError *error))completionHandler {
  std::string endpoint_id = [endpointID cStringUsingEncoding:[NSString defaultCStringEncoding]];

  PayloadListener listener;
  listener.payload_cb = ^(const std::string &endpoint_id, Payload payload) {
    NSString *endpointID = @(endpoint_id.c_str());
    GNCPayload2 *payload2 = [GNCPayload2 fromCpp:std::move(payload)];
    [delegate receivedPayload:payload2 fromEndpoint:endpointID];
  };
  listener.payload_progress_cb =
      ^(const std::string &endpoint_id, const PayloadProgressInfo &info) {
        NSString *endpointID = @(endpoint_id.c_str());
        // TODO(bourdakos@): Implement
      };

  ResultListener result;
  result.result_cb = ^(Status status) {
    NSError *err = nil;
    if (!status.Ok()) {
      // TODO(bourdakos@): handle errors better
      err = [NSError errorWithDomain:GNCErrorDomain code:0 userInfo:nil];
    }
    completionHandler(err);
  };

  _core->AcceptConnection(endpoint_id, std::move(listener), result);
}

- (void)rejectConnectionRequestFromEndpoint:(NSString *)endpointID
                      withCompletionHandler:(void (^)(NSError *error))completionHandler {
  std::string endpoint_id = [endpointID cStringUsingEncoding:[NSString defaultCStringEncoding]];

  ResultListener result;
  result.result_cb = ^(Status status) {
    NSError *err = nil;
    if (!status.Ok()) {
      // TODO(bourdakos@): handle errors better
      err = [NSError errorWithDomain:GNCErrorDomain code:0 userInfo:nil];
    }
    completionHandler(err);
  };

  _core->RejectConnection(endpoint_id, result);
}

- (void)sendPayload:(GNCPayload2 *)payload
              toEndpoints:(NSArray<NSString *> *)endpointIDs
    withCompletionHandler:(void (^)(NSError *error))completionHandler {
  std::vector<std::string> endpoint_ids;
  endpoint_ids.reserve([endpointIDs count]);
  for (NSString *endpointID in endpointIDs) {
    std::string endpoint_id = [endpointID cStringUsingEncoding:[NSString defaultCStringEncoding]];
    endpoint_ids.push_back(endpoint_id);
  }

  ResultListener result;
  result.result_cb = ^(Status status) {
    NSError *err = nil;
    if (!status.Ok()) {
      // TODO(bourdakos@): handle errors better
      err = [NSError errorWithDomain:GNCErrorDomain code:0 userInfo:nil];
    }
    completionHandler(err);
  };

  _core->SendPayload(endpoint_ids, [payload toCpp], result);
}

- (void)cancelPayload:(int64_t)payloadID
    withCompletionHandler:(void (^)(NSError *error))completionHandler {
  ResultListener result;
  result.result_cb = ^(Status status) {
    NSError *err = nil;
    if (!status.Ok()) {
      // TODO(bourdakos@): handle errors better
      err = [NSError errorWithDomain:GNCErrorDomain code:0 userInfo:nil];
    }
    completionHandler(err);
  };

  _core->CancelPayload(payloadID, result);
}

- (void)disconnectFromEndpoint:(NSString *)endpointID
         withCompletionHandler:(void (^)(NSError *error))completionHandler {
  std::string endpoint_id = [endpointID cStringUsingEncoding:[NSString defaultCStringEncoding]];

  ResultListener result;
  result.result_cb = ^(Status status) {
    NSError *err = nil;
    if (!status.Ok()) {
      // TODO(bourdakos@): handle errors better
      err = [NSError errorWithDomain:GNCErrorDomain code:0 userInfo:nil];
    }
    completionHandler(err);
  };

  _core->DisconnectFromEndpoint(endpoint_id, result);
}

- (void)stopAllEndpointsWithCompletionHandler:(void (^)(NSError *error))completionHandler {
  ResultListener result;
  result.result_cb = ^(Status status) {
    NSError *err = nil;
    if (!status.Ok()) {
      // TODO(bourdakos@): handle errors better
      err = [NSError errorWithDomain:GNCErrorDomain code:0 userInfo:nil];
    }
    completionHandler(err);
  };
  _core->StopAllEndpoints(result);
}

- (void)initiateBandwidthUpgrade:(NSString *)endpointID
           withCompletionHandler:(void (^)(NSError *error))completionHandler {
  std::string endpoint_id = [endpointID cStringUsingEncoding:[NSString defaultCStringEncoding]];

  ResultListener result;
  result.result_cb = ^(Status status) {
    NSError *err = nil;
    if (!status.Ok()) {
      // TODO(bourdakos@): handle errors better
      err = [NSError errorWithDomain:GNCErrorDomain code:0 userInfo:nil];
    }
    completionHandler(err);
  };

  _core->InitiateBandwidthUpgrade(endpoint_id, result);
}

- (NSString *)localEndpointID {
  std::string endpoint_id = _core->GetLocalEndpointId();
  return @(endpoint_id.c_str());
}

@end
