/* Copyright (c) 2018 PaddlePaddle Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License. */

#include <cuda_runtime_api.h>
#include <glog/logging.h>
#include <gtest/gtest.h>

#include "NvInfer.h"
#include "paddle/fluid/inference/tensorrt/helper.h"
#include "paddle/phi/backends/dynload/tensorrt.h"

namespace dy = phi::dynload;

class Logger : public nvinfer1::ILogger {
 public:
  void log(nvinfer1::ILogger::Severity severity,
           const char* msg) TRT_NOEXCEPT override {
    switch (severity) {
      case Severity::kINFO:
        LOG(INFO) << msg;
        break;
      case Severity::kWARNING:
        LOG(WARNING) << msg;
        break;
      case Severity::kINTERNAL_ERROR:
      case Severity::kERROR:
        LOG(ERROR) << msg;
        break;
      default:
        break;
    }
  }
};

class ScopedWeights {
 public:
  explicit ScopedWeights(float value)
      : value_(value), w{nvinfer1::DataType::kFLOAT, &value_, 1} {}
  const nvinfer1::Weights& get() { return w; }

 private:
  float value_;
  nvinfer1::Weights w;
};

// The following two API are implemented in TensorRT's header file, cannot load
// from the dynamic library. So create our own implementation and directly
// trigger the method from the dynamic library.
nvinfer1::IBuilder* createInferBuilder(nvinfer1::ILogger* logger) {
  return static_cast<nvinfer1::IBuilder*>(
      dy::createInferBuilder_INTERNAL(logger, NV_TENSORRT_VERSION));
}
nvinfer1::IRuntime* createInferRuntime(nvinfer1::ILogger* logger) {
  return static_cast<nvinfer1::IRuntime*>(
      dy::createInferRuntime_INTERNAL(logger, NV_TENSORRT_VERSION));
}

const char* kInputTensor = "input";
const char* kOutputTensor = "output";

// Creates a network to compute y = 2x + 3
nvinfer1::IHostMemory* CreateNetwork() {
  Logger logger;
  // Create the engine.
  nvinfer1::IBuilder* builder = createInferBuilder(&logger);
  auto config = builder->createBuilderConfig();
  ScopedWeights weights(2.);
  ScopedWeights bias(3.);

  nvinfer1::INetworkDefinition* network = builder->createNetworkV2(
      1U << static_cast<uint32_t>(
          nvinfer1::NetworkDefinitionCreationFlag::kEXPLICIT_BATCH));
  // Add the input
  auto input = network->addInput(
      kInputTensor, nvinfer1::DataType::kFLOAT, nvinfer1::Dims3{1, 1, 1});
  EXPECT_NE(input, nullptr);
  // Add the constant layer for weight
  auto weight_tensor =
      network->addConstant(nvinfer1::Dims3{1, 1, 1}, weights.get())
          ->getOutput(0);
  // Add the constant layer for bias
  auto bias_tensor =
      network->addConstant(nvinfer1::Dims3{1, 1, 1}, bias.get())->getOutput(0);
  // Add the hidden layer.
  auto matmul_layer =
      network->addMatrixMultiply(*input,
                                 nvinfer1::MatrixOperation::kNONE,
                                 *weight_tensor,
                                 nvinfer1::MatrixOperation::kTRANSPOSE);
  auto add_layer =
      network->addElementWise(*matmul_layer->getOutput(0),
                              *bias_tensor,
                              nvinfer1::ElementWiseOperation::kSUM);
  EXPECT_NE(add_layer, nullptr);
  // Mark the output.
  auto output = add_layer->getOutput(0);
  output->setName(kOutputTensor);
  network->markOutput(*output);
#if IS_TRT_VERSION_GE(8300)
  config->setMemoryPoolLimit(nvinfer1::MemoryPoolType::kWORKSPACE, 1 << 10);
#else
  config->setMaxWorkspaceSize(1 << 10);
#endif
#if IS_TRT_VERSION_GE(8600)
  nvinfer1::IHostMemory* model =
      builder->buildSerializedNetwork(*network, *config);
  EXPECT_NE(model, nullptr);
#else
  auto* engine = builder->buildEngineWithConfig(*network, *config);
  EXPECT_NE(engine, nullptr);
  // Serialize the engine to create a model, then close.
  nvinfer1::IHostMemory* model = engine->serialize();
  delete engine;
#endif
  delete network;
  delete builder;
  return model;
}

void Execute(nvinfer1::IExecutionContext* context,
             const float* input,
             float* output) {
  const nvinfer1::ICudaEngine& engine = context->getEngine();
  // Two binds, input and output
  cudaStream_t stream;
  ASSERT_EQ(0, cudaStreamCreate(&stream));
#if IS_TRT_VERSION_GE(8600)
  ASSERT_EQ(engine.getNbIOTensors(), 2);
  void* buffers[2];
  for (int i = 0; i < 2; ++i) {
    ASSERT_EQ(0, cudaMalloc(&buffers[i], sizeof(float)));
    auto tensor_name = engine.getIOTensorName(i);
    context->setTensorAddress(tensor_name, buffers[i]);
  }
  ASSERT_EQ(
      0,
      cudaMemcpyAsync(
          buffers[0], input, sizeof(float), cudaMemcpyHostToDevice, stream));
  context->enqueueV3(stream);
  ASSERT_EQ(
      0,
      cudaMemcpyAsync(
          output, buffers[1], sizeof(float), cudaMemcpyDeviceToHost, stream));
  cudaStreamSynchronize(stream);
  cudaStreamDestroy(stream);
  ASSERT_EQ(0, cudaFree(buffers[0]));
  ASSERT_EQ(0, cudaFree(buffers[1]));
#else
  ASSERT_EQ(engine.getNbBindings(), 2);
  const int input_index = engine.getBindingIndex(kInputTensor);
  const int output_index = engine.getBindingIndex(kOutputTensor);
  // Create GPU buffers and a stream
  std::vector<void*> buffers(2);
  ASSERT_EQ(0, cudaMalloc(&buffers[input_index], sizeof(float)));
  ASSERT_EQ(0, cudaMalloc(&buffers[output_index], sizeof(float)));
  ASSERT_EQ(0, cudaStreamCreate(&stream));
  // Copy the input to the GPU, execute the network, and copy the output back.
  ASSERT_EQ(0,
            cudaMemcpyAsync(buffers[input_index],
                            input,
                            sizeof(float),
                            cudaMemcpyHostToDevice,
                            stream));
  context->enqueue(1, buffers.data(), stream, nullptr);
  ASSERT_EQ(0,
            cudaMemcpyAsync(output,
                            buffers[output_index],
                            sizeof(float),
                            cudaMemcpyDeviceToHost,
                            stream));
  cudaStreamSynchronize(stream);

  // Release the stream and the buffers
  cudaStreamDestroy(stream);
  ASSERT_EQ(0, cudaFree(buffers[input_index]));
  ASSERT_EQ(0, cudaFree(buffers[output_index]));
#endif
}

TEST(TensorrtTest, BasicFunction) {
  // Create the network serialized model.
  nvinfer1::IHostMemory* model = CreateNetwork();

  // Use the model to create an engine and an execution context.
  Logger logger;
  nvinfer1::IRuntime* runtime = createInferRuntime(&logger);
  nvinfer1::ICudaEngine* engine =
      runtime->deserializeCudaEngine(model->data(), model->size());
  delete model;
  nvinfer1::IExecutionContext* context = engine->createExecutionContext();

  // Execute the network.
  float input = 1234;
  float output;
  Execute(context, &input, &output);
  EXPECT_EQ(output, input * 2 + 3);

  // Destroy the engine.
  delete context;
  delete engine;
  delete runtime;
}
