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

#import "connections/clients/swift/NearbyCoreAdapter/Sources/Public/NearbyCoreAdapter/GNCDiscoveryOptions.h"

#import <Foundation/Foundation.h>

#include "connections/discovery_options.h"

#import "connections/clients/swift/NearbyCoreAdapter/Sources/GNCDiscoveryOptions+Internal.h"
#import "connections/clients/swift/NearbyCoreAdapter/Sources/GNCStrategy+Internal.h"

using ::location::nearby::connections::CppStrategyFromGNCStrategy;
using ::location::nearby::connections::DiscoveryOptions;

@implementation GNCDiscoveryOptions

- (instancetype)initWithStrategy:(GNCStrategy)strategy {
  self = [super init];
  if (self) {
    _strategy = strategy;
  }
  return self;
}

- (DiscoveryOptions)toCpp {
  DiscoveryOptions discovery_options;
  discovery_options.strategy = CppStrategyFromGNCStrategy(_strategy);
  return discovery_options;
}

@end
