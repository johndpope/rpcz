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

#include <iostream>
#include <boost/thread/thread.hpp>
#include <glog/logging.h>
#include <gtest/gtest.h>
#include <zmq.hpp>

#include "proto/search.pb.h"
#include "proto/search.zrpc.h"
#include "zrpc/callback.h"
#include "zrpc/connection_manager.h"
#include "zrpc/event_manager.h"
#include "zrpc/rpc_channel.h"
#include "zrpc/rpc.h"
#include "zrpc/server.h"
#include "zrpc/sync_event.h"

using namespace std;

namespace zrpc {

void SuperDone(RPC* newrpc, Closure* done) {
  delete newrpc;
  done->Run();
}

class SearchServiceImpl : public SearchService {
 public:
  SearchServiceImpl(SearchService_Stub* backend)
      : backend_(backend), delayed_closure_(NULL) {};

  virtual void Search(
      zrpc::RPC* rpc, const SearchRequest* request,
      SearchResponse* response, Closure* done) {
    if (request->query() == "foo") {
      rpc->SetFailed("I don't like foo.");
    } else if (request->query() == "bar") {
      rpc->SetFailed(17, "I don't like bar.");
    } else if (request->query() == "delegate") {
      RPC* newrpc = new RPC;
      backend_->Search(newrpc, request, response, NewCallback(SuperDone,
                                                              newrpc,
                                                              done));
      return;
    } else if (request->query() == "timeout") {
      // We lose the request. We are going to reply only when we get a request
      // for the query "delayed".
      delayed_closure_ = done;
      return;
    } else if (request->query() == "delayed") {
      CHECK_NOTNULL(delayed_closure_);
      delayed_closure_->Run();
    } else {
      response->add_results("The search for " + request->query());
      response->add_results("is great");
    }
    done->Run();
  }

 private:
  scoped_ptr<SearchService_Stub> backend_;
  Closure* delayed_closure_;
};

// For handling complex delegated queries.
class BackendSearchServiceImpl : public SearchService {
  virtual void Search(
      zrpc::RPC*, const SearchRequest*,
      SearchResponse* response, Closure* done) {
    response->add_results("42!");
    done->Run();
  }
};

void ServerThread(zmq::socket_t* socket,
                  SearchService *service,
                  EventManager* em) {
  VLOG(2) << "Server thread up.";
  Server server(socket, em);
  server.RegisterService(service);
  server.Start();
  VLOG(2) << "Deleting service.";
  delete service;
  VLOG(2) << "Server thread done.";
}

class ServerTest : public ::testing::Test {
 public:
  ServerTest() :
      context_(new zmq::context_t(1)),
      em_(new EventManager(context_.get(), 10)),
      cm_(new ConnectionManager(context_.get(), em_.get(), 1)),
      frontend_connection_(cm_->Connect("inproc://myserver.frontend")),
      backend_connection_(cm_->Connect("inproc://myserver.backend")) {
  }

  ~ServerTest() {
    // Terminate the context, which will cause the thread to quit.
    frontend_connection_.reset(NULL);
    backend_connection_.reset(NULL);
    em_.reset(NULL);
    cm_.reset(NULL);
    context_.reset(NULL);
    server_thread_.join();
    backend_thread_.join();
  }

  void StartServer() {
    zmq::socket_t *backend_socket = new zmq::socket_t(*context_, ZMQ_ROUTER);
    backend_socket->bind("inproc://myserver.backend");
    server_thread_ = boost::thread(
        boost::bind(ServerThread, backend_socket, new BackendSearchServiceImpl,
                    em_.get()));

    zmq::socket_t *frontend_socket = new zmq::socket_t(*context_, ZMQ_ROUTER);
    frontend_socket->bind("inproc://myserver.frontend");
    backend_thread_ = boost::thread(
        boost::bind(ServerThread,
                    frontend_socket,
                    new SearchServiceImpl(
                        new SearchService_Stub(
                            RpcChannel::Create(backend_connection_.get()),
                        true)),
                    em_.get()));
  }

 protected:
  scoped_ptr<zmq::context_t> context_;
  scoped_ptr<EventManager> em_;
  scoped_ptr<ConnectionManager> cm_;
  scoped_ptr<Connection> frontend_connection_;
  scoped_ptr<Connection> backend_connection_;
  boost::thread server_thread_;
  boost::thread backend_thread_;
};

TEST_F(ServerTest, SimpleRequest) {
  StartServer();
  SearchService_Stub stub(RpcChannel::Create(frontend_connection_.get()), true);
  SearchRequest request;
  SearchResponse response;
  RPC rpc;
  request.set_query("happiness");
  stub.Search(&rpc, &request, &response, NULL);
  rpc.Wait();
  CHECK(rpc.OK());
  CHECK_EQ(2, response.results_size());
  CHECK_EQ("The search for happiness", response.results(0));
}

TEST_F(ServerTest, SimpleRequestAsync) {
  StartServer();
  SearchService_Stub stub(RpcChannel::Create(frontend_connection_.get()), true);
  SearchRequest request;
  SearchResponse response;
  RPC rpc;
  request.set_query("happiness");
  SyncEvent sync;
  stub.Search(&rpc, &request, &response, NewCallback(
          &sync, &SyncEvent::Signal));
  sync.Wait();
  CHECK(rpc.OK());
  CHECK_EQ(2, response.results_size());
  CHECK_EQ("The search for happiness", response.results(0));
}

TEST_F(ServerTest, SimpleRequestWithError) {
  StartServer();
  SearchService_Stub stub(RpcChannel::Create(frontend_connection_.get()), true);
  SearchRequest request;
  SearchResponse response;
  RPC rpc;
  request.set_query("foo");
  stub.Search(&rpc, &request, &response, NULL);
  rpc.Wait();
  CHECK_EQ(GenericRPCResponse::APPLICATION_ERROR, rpc.GetStatus());
  CHECK_EQ("I don't like foo.", rpc.GetErrorMessage());
}

TEST_F(ServerTest, SimpleRequestWithTimeout) {
  StartServer();
  SearchService_Stub stub(RpcChannel::Create(frontend_connection_.get()), true);
  SearchRequest request;
  SearchResponse response;
  RPC rpc;
  request.set_query("timeout");
  rpc.SetDeadlineMs(1);
  stub.Search(&rpc, &request, &response, NULL);
  rpc.Wait();
  CHECK_EQ(GenericRPCResponse::DEADLINE_EXCEEDED, rpc.GetStatus());
}

TEST_F(ServerTest, SimpleRequestWithTimeoutAsync) {
  StartServer();
  SearchService_Stub stub(RpcChannel::Create(frontend_connection_.get()), true);
  SearchRequest request;
  SearchResponse response;
  {
    RPC rpc;
    request.set_query("timeout");
    rpc.SetDeadlineMs(1);
    SyncEvent event;
    stub.Search(&rpc, &request, &response,
                NewCallback(&event, &SyncEvent::Signal));
    event.Wait();
    CHECK_EQ(GenericRPCResponse::DEADLINE_EXCEEDED, rpc.GetStatus());
  }
  LOG(INFO) << "Sending a request that will fire the server-side closure.";
  // Now we clean up the closure we kept aside.
  {
    RPC rpc;
    request.set_query("delayed");
    stub.Search(&rpc, &request, &response, NULL);
    rpc.Wait();
    CHECK(rpc.OK());
  }
}

TEST_F(ServerTest, DelegatedRequest) {
  StartServer();
  SearchService_Stub stub(RpcChannel::Create(frontend_connection_.get()), true);
  SearchRequest request;
  SearchResponse response;
  RPC rpc;
  request.set_query("delegate");
  stub.Search(&rpc, &request, &response, NULL);
  rpc.Wait();
  CHECK_EQ(GenericRPCResponse::OK, rpc.GetStatus());
  CHECK_EQ("42!", response.results(0));
}
}  // namespace