/* Copyright (c) 2021 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#if (defined PADDLE_WITH_CUDA) && (defined PADDLE_WITH_PSCORE)

#include <cstdlib>

#include <memory>
#include <random>
#include <sstream>
#include <string>
#include <thread>  // NOLINT

#include "gtest/gtest.h"
#include "paddle/fluid/distributed/ps/service/heter_client.h"
#include "paddle/fluid/distributed/ps/service/heter_server.h"
#include "paddle/fluid/framework/op_registry.h"
#include "paddle/fluid/framework/op_version_registry.h"
#include "paddle/fluid/memory/memcpy.h"
#include "paddle/fluid/platform/device_context.h"

namespace framework = paddle::framework;
namespace platform = paddle::platform;
namespace distributed = paddle::distributed;
namespace memory = paddle::memory;

using MultiVarMsg = ::paddle::distributed::MultiVariableMessage;
using VarMsg = ::paddle::distributed::VariableMessage;

USE_OP_ITSELF(scale);
PD_DECLARE_KERNEL(send_and_recv, GPU, ALL_LAYOUT);

std::shared_ptr<distributed::HeterServer> b_rpc_service2;

std::string get_ip_port() {
  std::mt19937 rng;
  rng.seed(std::random_device()());
  std::uniform_int_distribution<std::mt19937::result_type> dist(4444, 25000);
  int port = dist(rng);
  std::string ip_port;
  std::stringstream temp_str;
  temp_str << "127.0.0.1:";
  temp_str << port;
  temp_str >> ip_port;
  return ip_port;
}

framework::BlockDesc* AppendSendAndRecvBlock(framework::ProgramDesc* program) {
  auto root_block = program->MutableBlock(0);
  auto* block = program->AppendBlock(*root_block);
  framework::OpDesc* op = block->AppendOp();
  op->SetType("scale");
  op->SetInput("X", {"x"});
  op->SetOutput("Out", {"res"});
  op->SetAttr("scale", 0.5f);
  auto& out = *root_block->Var("res");
  out.SetType(framework::proto::VarType::LOD_TENSOR);
  out.SetShape({1, 10});
  return block;
}

void CreateVarsOnScope(framework::Scope* scope) {
  auto w_var = scope->Var("w");
  w_var->GetMutable<phi::SelectedRows>();

  auto out_var = scope->Var("out");
  out_var->GetMutable<phi::DenseTensor>();

  auto micro_var = scope->Var("microbatch_id");
  micro_var->GetMutable<phi::DenseTensor>();

  auto ids_var = scope->Var("ids");
  ids_var->GetMutable<phi::DenseTensor>();

  auto x_var = scope->Var("x");
  x_var->GetMutable<phi::DenseTensor>();

  auto res_var = scope->Var("res");
  res_var->GetMutable<phi::DenseTensor>();
}

void InitTensorsOnClient(framework::Scope* scope,
                         int64_t rows_numel,
                         const phi::DeviceContext& ctx) {
  CreateVarsOnScope(scope);
  const auto place = ctx.GetPlace();
  // auto ids_var = scope->Var("ids")->GetMutable<phi::DenseTensor>();
  // int64_t* ids_ptr =
  //    ids_var->mutable_data<int64_t>(phi::DDim({rows_numel, 1}),
  //    *place);
  // for (int64_t i = 0; i < rows_numel; ++i) ids_ptr[i] = i * 2;
  auto stream = reinterpret_cast<const phi::GPUContext&>(ctx).stream();

  auto micro_id_var =
      scope->Var("microbatch_id")->GetMutable<phi::DenseTensor>();
  float* micro_id_ptr =
      micro_id_var->mutable_data<float>(phi::DDim({1}), place);
  std::vector<float> temp_vec{0};
  float* temp_ptr = temp_vec.data();

  memory::Copy(place,
               reinterpret_cast<void*>(micro_id_ptr),
               phi::CPUPlace(),
               reinterpret_cast<void*>(temp_ptr),
               micro_id_var->numel() *
                   framework::SizeOfType(
                       framework::TransToProtoVarType(micro_id_var->dtype())),
               stream);

  auto x_var = scope->Var("x")->GetMutable<phi::DenseTensor>();
  float* x_ptr = x_var->mutable_data<float>(phi::DDim({1, rows_numel}), place);
  std::vector<float> x_vec;
  for (int64_t i = 0; i < rows_numel; ++i) x_vec.push_back(1.0);
  float* x_vec_ptr = x_vec.data();
  memory::Copy(place,
               reinterpret_cast<void*>(x_ptr),
               phi::CPUPlace(),
               reinterpret_cast<void*>(x_vec_ptr),
               x_var->numel() * phi::SizeOf(x_var->dtype()),
               stream);

  // auto res_var = scope->Var("res")->GetMutable<phi::DenseTensor>();
  // float* res_ptr =
  //    res_var->mutable_data<float>(phi::DDim({1, rows_numel}), place);
  // for (int64_t i = 0; i < rows_numel; ++i) res_ptr[i] = 1.0;
}

void InitTensorsOnServer(framework::Scope* scope,
                         phi::CPUPlace* place,
                         int64_t rows_numel) {
  CreateVarsOnScope(scope);
  auto w = scope->Var("w")->GetMutable<phi::SelectedRows>();
  auto w_value = w->mutable_value();
  w_value->Resize({rows_numel, 10});
  for (int64_t i = 0; i < rows_numel; ++i) w->AutoGrownIndex(i, true);

  auto ptr = w_value->mutable_data<float>(*place);

  for (int64_t i = 0; i < w_value->numel(); ++i) {
    ptr[i] = static_cast<float>(i) / 10.0;
  }
}

void RunServer(std::shared_ptr<paddle::distributed::HeterServer> service) {
  service->StartHeterService();
}

void StartSendAndRecvServer(std::string endpoint) {
  framework::ProgramDesc program;
  framework::Scope scope;
  phi::CPUPlace place;
  framework::Executor exe(place);
  phi::CPUContext ctx(place);
  LOG(INFO) << "before AppendSendAndRecvBlock";
  auto block = AppendSendAndRecvBlock(&program);
  std::string in_var_name("x");
  std::vector<int> prefetch_block_ids{block->ID()};

  LOG(INFO) << "before InitTensorsOnServer";
  InitTensorsOnServer(&scope, &place, 10);
  LOG(INFO) << "end InitTensorsOnServer";

  std::shared_ptr<distributed::SendAndRecvVariableHandler> b_req_handler;
  b_req_handler.reset(new distributed::SendAndRecvVariableHandler());
  LOG(INFO) << "before SetDevCtx";
  b_req_handler->SetDevCtx(&ctx);
  LOG(INFO) << "before SetScope";
  b_req_handler->SetScope(&scope);
  LOG(INFO) << "before HeterServer::GetInstance";
  b_rpc_service2 = distributed::HeterServer::GetInstance();
  b_rpc_service2->SetEndPoint(endpoint);
  LOG(INFO) << "before HeterServer::RegisterServiceHandler";
  b_rpc_service2->RegisterServiceHandler(in_var_name,
                                         [&](const MultiVarMsg* request,
                                             MultiVarMsg* response,
                                             brpc::Controller* cntl) -> int {
                                           return b_req_handler->Handle(
                                               request, response, cntl);
                                         });

  b_rpc_service2->SetServiceHandler(b_req_handler);
  LOG(INFO) << "before HeterServer::RunServer";

  RunServer(b_rpc_service2);
  // std::thread server_thread(std::bind(RunServer, b_rpc_service2));
  // server_thread.join();
}

TEST(SENDANDRECV, GPU) {
  setenv("http_proxy", "", 1);
  setenv("https_proxy", "", 1);
  std::string endpoint = get_ip_port();
  std::string previous_endpoint = endpoint;
  LOG(INFO) << "before StartSendAndRecvServer";
  b_rpc_service2 = distributed::HeterServer::GetInstance();
  std::thread server_thread(StartSendAndRecvServer, endpoint);
  b_rpc_service2->WaitServerReady();
  using MicroScope =
      std::unordered_map<int, std::shared_ptr<std::vector<framework::Scope*>>>;
  using MiniScope = std::unordered_map<int, framework::Scope*>;
  std::shared_ptr<MiniScope> mini_scopes(new MiniScope{});
  std::shared_ptr<MicroScope> micro_scopes(new MicroScope{});
  auto* mini_scope = new framework::Scope();
  (*mini_scopes)[0] = mini_scope;
  std::shared_ptr<std::vector<framework::Scope*>> micro_scope(
      new std::vector<framework::Scope*>{});
  auto* micro_scope_0 = &(mini_scope->NewScope());
  (*micro_scope).push_back(micro_scope_0);
  (*micro_scopes)[0] = micro_scope;
  b_rpc_service2->SetMicroBatchScopes(micro_scopes);
  b_rpc_service2->SetMiniBatchScopes(mini_scopes);

  using TaskQueue =
      std::unordered_map<int,
                         std::shared_ptr<::paddle::framework::BlockingQueue<
                             std::pair<std::string, int>>>>;
  using SharedTaskQueue = std::shared_ptr<
      std::unordered_map<int,
                         std::shared_ptr<::paddle::framework::BlockingQueue<
                             std::pair<std::string, int>>>>>;
  SharedTaskQueue task_queue_(new TaskQueue{});
  (*task_queue_)[0] = std::make_shared<
      ::paddle::framework::BlockingQueue<std::pair<std::string, int>>>();
  b_rpc_service2->SetTaskQueue(task_queue_);

  LOG(INFO) << "before HeterClient::GetInstance";
  std::shared_ptr<distributed::HeterClient> heter_client_ptr_ =
      distributed::HeterClient::GetInstance({endpoint}, {previous_endpoint}, 0);
  if (heter_client_ptr_ == nullptr) {
    LOG(ERROR) << "heter_client_ptr_ is null";
  }

  framework::Scope* scope = (*micro_scope)[0];
  phi::GPUPlace place;
  phi::GPUContext ctx(place);
  ctx.SetAllocator(paddle::memory::allocation::AllocatorFacade::Instance()
                       .GetAllocator(place, ctx.stream())
                       .get());
  ctx.PartialInitWithAllocator();

  framework::Executor exe(place);
  // create var on local scope
  int64_t rows_numel = 10;
  LOG(INFO) << "before InitTensorsOnClient";
  InitTensorsOnClient(scope, rows_numel, ctx);
  LOG(INFO) << "after InitTensorsOnClient2";
  std::string in_var_name("x");
  std::string micro_var_name("microbatch_id");
  // std::string out_var_name("res");
  std::vector<std::string> send_var{in_var_name, micro_var_name};
  std::vector<std::string> recv_var{};
  std::string mode_str("forward");

  LOG(INFO) << "add block & op1";
  framework::ProgramDesc program;
  auto root_block = program.MutableBlock(0);
  // op for forward
  framework::OpDesc* op = root_block->AppendOp();
  op->SetType("send_and_recv");
  LOG(INFO) << "op1 set input";
  op->SetInput("X", std::vector<std::string>({in_var_name}));
  op->SetOutput("Out", {});
  op->SetAttr("next_endpoints", std::vector<std::string>({endpoint}));
  op->SetAttr("previous_endpoints",
              std::vector<std::string>({previous_endpoint}));
  op->SetAttr("trainer_id", 0);
  op->SetAttr("mode", mode_str);
  op->SetAttr("message_name", in_var_name);
  op->SetAttr("send_var_name", send_var);
  op->SetAttr("recv_var_name", recv_var);
  op->SetAttr("op_device", std::string("gpu"));

  std::string mode_str2("backward");
  // op for backward
  LOG(INFO) << "add op2";
  framework::OpDesc* op2 = root_block->AppendOp();

  op2->SetType("send_and_recv");
  LOG(INFO) << "op2 set input";
  op2->SetInput("X", std::vector<std::string>({in_var_name}));
  op2->SetOutput("Out", {});
  op2->SetAttr("next_endpoints", std::vector<std::string>({endpoint}));
  op2->SetAttr("previous_endpoints",
               std::vector<std::string>({previous_endpoint}));
  op2->SetAttr("trainer_id", 0);
  op2->SetAttr("mode", mode_str2);
  op2->SetAttr("message_name", in_var_name);
  op2->SetAttr("send_var_name", send_var);
  op2->SetAttr("recv_var_name", recv_var);
  op2->SetAttr("op_device", std::string("gpu"));

  LOG(INFO) << "exe before prepare";
  auto prepared = exe.Prepare(program, 0);
  LOG(INFO) << "exe after prepare";

  LOG(INFO) << "before RunPreparedContext";
  exe.RunPreparedContext(prepared.get(), scope, false);

  LOG(INFO) << "client wait for Pop";
  auto task = (*task_queue_)[0]->Pop();
  LOG(INFO) << "client get from task queue";
  PADDLE_ENFORCE_EQ(
      task.first,
      "x",
      common::errors::InvalidArgument(
          "Recv message and Send message name not match, Check your Code"));

  auto task2 = (*task_queue_)[0]->Pop();
  PADDLE_ENFORCE_EQ(
      task2.first,
      "x",
      common::errors::InvalidArgument(
          "Recv message and Send message name not match, Check your Code"));

  b_rpc_service2->Stop();
  LOG(INFO) << "end server Stop";
  server_thread.join();
  LOG(INFO) << "end server thread join";
}
#endif
