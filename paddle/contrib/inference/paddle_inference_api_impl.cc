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

#include <sys/time.h>
#include <algorithm>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "paddle/contrib/inference/paddle_inference_api_impl.h"

namespace paddle {
namespace {

// Timer for timer
class Timer {
 public:
  double start;
  double startu;
  void tic() {
    struct timeval tp;
    gettimeofday(&tp, NULL);
    start = tp.tv_sec;
    startu = tp.tv_usec;
  }
  double toc() {
    struct timeval tp;
    gettimeofday(&tp, NULL);
    double used_time_ms =
        (tp.tv_sec - start) * 1000.0 + (tp.tv_usec - startu) / 1000.0;
    return used_time_ms;
  }
};

template <class T>
std::string num2str(T a) {
  std::stringstream istr;
  istr << a;
  return istr.str();
}
}  // namespace

bool PaddlePredictorImpl::Init() {
  VLOG(3) << "Predictor::init()";

  // TODO(panyx0718): Should CPU vs GPU device be decided by id?
  if (config_.device >= 0) {
    place_ = paddle::platform::CUDAPlace(config_.device);
  } else {
    place_ = paddle::platform::CPUPlace();
  }
  paddle::framework::InitDevices(false);
  executor_.reset(new paddle::framework::Executor(place_));
  scope_.reset(new paddle::framework::Scope());

  // Initialize the inference program
  if (!config_.model_dir.empty()) {
    // Parameters are saved in separate files sited in
    // the specified `dirname`.
    inference_program_ = paddle::inference::Load(
        executor_.get(), scope_.get(), config_.model_dir);
  } else if (!config_.prog_file.empty() && !config_.param_file.empty()) {
    // All parameters are saved in a single file.
    // The file names should be consistent with that used
    // in Python API `fluid.io.save_inference_model`.
    inference_program_ = paddle::inference::Load(
        executor_.get(), scope_.get(), config_.prog_file, config_.param_file);
  } else {
    LOG(ERROR) << "fail to load inference model.";
    return false;
  }
  ctx_ = executor_->Prepare(*inference_program_, 0);

  // Create variables
  // TODO(panyx0718): Why need to test share_variables here?
  if (config_.share_variables) {
    executor_->CreateVariables(*inference_program_, scope_.get(), 0);
  }
  // Get the feed_target_names and fetch_target_names
  feed_target_names_ = inference_program_->GetFeedTargetNames();
  fetch_target_names_ = inference_program_->GetFetchTargetNames();
  return true;
}

bool PaddlePredictorImpl::Run(const std::vector<PaddleTensor> &inputs,
                              std::vector<PaddleTensor> *output_data) {
  VLOG(3) << "Predictor::predict";
  Timer timer;
  timer.tic();
  // set feed variable
  std::map<std::string, const paddle::framework::LoDTensor *> feed_targets;
  std::vector<paddle::framework::LoDTensor> feeds;
  if (!SetFeed(inputs, &feeds)) {
    LOG(ERROR) << "fail to set feed";
    return false;
  }
  for (size_t i = 0; i < feed_target_names_.size(); ++i) {
    feed_targets[feed_target_names_[i]] = &feeds[i];
  }
  // get fetch variable
  std::map<std::string, paddle::framework::LoDTensor *> fetch_targets;
  std::vector<paddle::framework::LoDTensor> fetchs;
  fetchs.resize(fetch_target_names_.size());
  for (size_t i = 0; i < fetch_target_names_.size(); ++i) {
    fetch_targets[fetch_target_names_[i]] = &fetchs[i];
  }
  // Run the inference program
  // if share variables, we need not create variables
  executor_->RunPreparedContext(ctx_.get(),
                                scope_.get(),
                                &feed_targets,
                                &fetch_targets,
                                !config_.share_variables);
  if (!GetFetch(fetchs, output_data)) {
    LOG(ERROR) << "fail to get fetchs";
    return false;
  }
  VLOG(3) << "predict cost: " << timer.toc() << "ms";
  return true;
}

std::unique_ptr<PaddlePredictor> PaddlePredictorImpl::Clone() {
  VLOG(3) << "Predictor::clone";
  std::unique_ptr<PaddlePredictor> cls(new PaddlePredictorImpl(config_));
  if (!cls->InitShared()) {
    LOG(ERROR) << "fail to call InitShared";
    return nullptr;
  }
  return cls;
}

// TODO(panyx0718): Consider merge with Init()?
bool PaddlePredictorImpl::InitShared() {
  VLOG(3) << "Predictor::init_shared";
  // 1. Define place, executor, scope
  if (this->config_.device >= 0) {
    place_ = paddle::platform::CUDAPlace();
  } else {
    place_ = paddle::platform::CPUPlace();
  }
  this->executor_.reset(new paddle::framework::Executor(this->place_));
  this->scope_.reset(new paddle::framework::Scope());
  // Initialize the inference program
  if (!this->config_.model_dir.empty()) {
    // Parameters are saved in separate files sited in
    // the specified `dirname`.
    this->inference_program_ = paddle::inference::Load(
        this->executor_.get(), this->scope_.get(), this->config_.model_dir);
  } else if (!this->config_.prog_file.empty() &&
             !this->config_.param_file.empty()) {
    // All parameters are saved in a single file.
    // The file names should be consistent with that used
    // in Python API `fluid.io.save_inference_model`.
    this->inference_program_ =
        paddle::inference::Load(this->executor_.get(),
                                this->scope_.get(),
                                this->config_.prog_file,
                                this->config_.param_file);
  }
  this->ctx_ = this->executor_->Prepare(*this->inference_program_, 0);
  // 3. create variables
  // TODO(panyx0718): why test share_variables.
  if (config_.share_variables) {
    this->executor_->CreateVariables(
        *this->inference_program_, this->scope_.get(), 0);
  }
  // 4. Get the feed_target_names and fetch_target_names
  this->feed_target_names_ = this->inference_program_->GetFeedTargetNames();
  this->fetch_target_names_ = this->inference_program_->GetFetchTargetNames();
  return true;
}

bool PaddlePredictorImpl::SetFeed(
    const std::vector<PaddleTensor> &inputs,
    std::vector<paddle::framework::LoDTensor> *feeds) {
  VLOG(3) << "Predictor::set_feed";
  if (inputs.size() != feed_target_names_.size()) {
    LOG(ERROR) << "wrong feed input size.";
    return false;
  }
  for (size_t i = 0; i < feed_target_names_.size(); ++i) {
    paddle::framework::LoDTensor input;
    paddle::framework::DDim ddim =
        paddle::framework::make_ddim(inputs[i].shape);
    void *input_ptr;
    if (inputs[i].dtype == PaddleDType::INT64) {
      input_ptr =
          input.mutable_data<int64_t>(ddim, paddle::platform::CPUPlace());
    } else if (inputs[i].dtype == PaddleDType::FLOAT32) {
      input_ptr = input.mutable_data<float>(ddim, paddle::platform::CPUPlace());
    } else {
      LOG(ERROR) << "unsupported feed type " << inputs[i].dtype;
      return false;
    }

    // TODO(panyx0718): Init LoDTensor from existing memcpy to save a copy.
    std::memcpy(static_cast<void *>(input_ptr),
                inputs[i].data.data,
                inputs[i].data.length);
    feeds->push_back(input);
    LOG(ERROR) << "Actual feed type " << feeds->back().type().name();
  }
  return true;
}

bool PaddlePredictorImpl::GetFetch(
    const std::vector<paddle::framework::LoDTensor> &fetchs,
    std::vector<PaddleTensor> *outputs) {
  VLOG(3) << "Predictor::get_fetch";
  outputs->resize(fetchs.size());
  for (size_t i = 0; i < fetchs.size(); ++i) {
    // TODO(panyx0718): Support fetch of other types.
    if (fetchs[i].type() != typeid(float)) {
      LOG(ERROR) << "only support fetching float now.";
      return false;
    }
    std::vector<int> shape;
    auto dims_i = fetchs[i].dims();
    auto lod = fetchs[i].lod();
    const float *output_ptr = fetchs[i].data<float>();
    // const int64_t* output_ptr = fetchs[i].data<int64_t>();
    auto num = fetchs[i].numel();
    std::vector<float> data;
    if (0 == lod.size()) {
      std::copy(output_ptr, output_ptr + num, std::back_inserter(data));
      for (int j = 0; j < dims_i.size(); ++j) {
        shape.push_back(dims_i[j]);
      }
    } else {
      // for batch detection
      // image[0] -> output[0] shape {145, 6}
      // image[1] -> output[1] shape {176, 6}
      // then,
      // the batch output shape {321, 6}
      // the lod {{0, 145, 321}}
      // so we should append output[0] to {176, 6}
      size_t max_dim = 0;
      for (size_t j = 1; j < lod[0].size(); j++) {
        max_dim = std::max(max_dim, lod[0][j] - lod[0][j - 1]);
      }
      size_t common_dim = lod[0].back() == 0 ? 0 : num / lod[0].back();
      if (max_dim > 0) {
        data.resize((lod[0].size() - 1) * max_dim * common_dim, 0);
      }
      for (size_t j = 1; j < lod[0].size(); j++) {
        size_t start = lod[0][j - 1] * common_dim;
        size_t end = lod[0][j] * common_dim;
        if (end > start) {
          std::copy(output_ptr + start,
                    output_ptr + end,
                    data.begin() + (j - 1) * max_dim * common_dim);
        }
      }
      shape.push_back(lod[0].size() - 1);
      shape.push_back(max_dim);
      for (int j = 1; j < dims_i.size(); ++j) {
        shape.push_back(dims_i[j]);
      }
    }

    outputs->at(i).shape = shape;
    outputs->at(i).data.length = sizeof(float) * data.size();
    outputs->at(i).data.data = malloc(outputs->at(i).data.length);
    std::memcpy(
        outputs->at(i).data.data, data.data(), outputs->at(i).data.length);
    outputs->at(i).dtype = PaddleDType::FLOAT32;
    // TODO(panyx0718): support other types? fill tensor name? avoid a copy.
  }
  return true;
}

std::unique_ptr<PaddlePredictorImpl> CreatePaddlePredictorImpl(
    const VisConfig &config) {
  VLOG(3) << "create PaddlePredictorImpl";
  // 1. GPU memeroy
  std::vector<std::string> flags;
  if (config.fraction_of_gpu_memory >= 0.0f ||
      config.fraction_of_gpu_memory <= 0.95f) {
    flags.push_back("dummpy");
    std::string flag = "--fraction_of_gpu_memory_to_use=" +
                       num2str<float>(config.fraction_of_gpu_memory);
    flags.push_back(flag);
    VLOG(3) << "set flag: " << flag;
    framework::InitGflags(flags);
  }

  std::unique_ptr<PaddlePredictorImpl> predictor(
      new PaddlePredictorImpl(config));
  if (!predictor->Init()) {
    return nullptr;
  }
  return predictor;
}

}  // namespace paddle
