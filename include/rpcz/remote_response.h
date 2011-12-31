// Copyright 2011 Google Inc. All Rights Reserved.
// 
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// 
//     http://www.apache.org/licenses/LICENSE-2.0
// 
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Author: nadavs@google.com <Nadav Samet>

#ifndef RPCZ_REMOTE_RESPONSE_H
#define RPCZ_REMOTE_RESPONSE_H

#include "rpcz/zmq_utils.h"

namespace rpcz {
class RemoteResponse {
 public:
  RemoteResponse();

  ~RemoteResponse();

  enum Status {
    INACTIVE = 0,
    ACTIVE = 1,
    DONE = 2,
    DEADLINE_EXCEEDED = 3,
  };
  Status status;
  MessageVector reply;
};
}  // namespace rpcz
#endif