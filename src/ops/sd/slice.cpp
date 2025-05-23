// Copyright (c) 2025 Advanced Micro Devices, Inc
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#include <iostream>
#include <map>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <tuple>
#include <utility>

// XRT headers
#include "xrt/xrt_bo.h"
#include "xrt/xrt_device.h"
#include "xrt/xrt_kernel.h"

// dpu kernel metadata
#include <utils/dpu_mdata.hpp>

#include <txn_container.hpp>
#include <utils/instruction_registry.hpp>
#include <xrt_context/xrt_context.hpp>

#include <ops/op_interface.hpp>
#include <ops/sd/slice.hpp>
#include <utils/logging.hpp>
#include <utils/utils.hpp>

namespace ryzenai {

namespace sd {

template <typename InT, typename OutT>
void slice<InT, OutT>::setup_instr_registry() {
  std::vector<std::pair<std::string, bool>> instructions;
  std::vector<std::vector<size_t>> supported_shapes =
      default_shapes_.find(txn_fname_prefix_)->second;
  for (size_t i = 0; i < supported_shapes.size(); i++) {
    auto shape = supported_shapes[i];
    auto key = get_instr_key(txn_fname_prefix_, shape);
    instructions.push_back(std::make_pair(key, false));
  }
  xrt_ctx_->get_registry().add_instructions(instructions);
}
template <typename InT, typename OutT>
std::string
slice<InT, OutT>::get_instr_key(std::string prefix,
                                const std::vector<size_t> &dimensions) const {
  std::ostringstream oss;
  oss << prefix;
  for (const auto &dim : dimensions) {
    oss << "_" << dim;
  }
  return oss.str();
}

template <typename InT, typename OutT>
void slice<InT, OutT>::set_params(const std::string &xclbin,
                                  const std::string &pdi_name) {
  if (!xclbin.empty()) {
    XCLBIN_FNAME_ = OpInterface::get_dd_base_dir() + "\\xclbin\\stx\\" + xclbin;
  }
  pdi_name_ = pdi_name;
  xrt_ctx_ = dynamic_dispatch::xrt_context::get_instance(XCLBIN_FNAME_);
  std::call_once(instr_reg_flag_, [this]() { setup_instr_registry(); });
}

template <typename InT, typename OutT>
void slice<InT, OutT>::initialize_const_params(
    ConstBufferIO &io, const std::vector<Tensor> &const_params,
    const std::map<std::string, std::any> &attr) {
  RYZENAI_LOG_TRACE("SD slice initialize_const_params(ptr) ...");
  io.write(0, const_params.at(0).data, b_bo_size_);
  RYZENAI_LOG_TRACE("SD slice initialize_const_params(ptr) ... DONE");
}

template <typename InT, typename OutT>
void slice<InT, OutT>::initialize_const_params(
    const std::vector<Tensor> &const_params,
    const std::map<std::string, std::any> &attr) {
  RYZENAI_LOG_TRACE("SD slice initialize_const_params ...");
  size_t input_bo_size =
      std::accumulate(input_shape_.begin(), input_shape_.end(), size_t(1),
                      std::multiplies<size_t>()) *
      sizeof(InT);
  size_t output_bo_size =
      std::accumulate(output_shape_.begin(), output_shape_.end(), size_t(1),
                      std::multiplies<size_t>()) *
      sizeof(OutT);
  b_bo_ = xrt::bo(xrt_ctx_->get_device(), b_bo_size_, XRT_BO_FLAGS_HOST_ONLY,
                  xrt_ctx_->get_kernel(pdi_name_).group_id(0));
  a_bo_ = xrt::bo(xrt_ctx_->get_device(), input_bo_size, XRT_BO_FLAGS_HOST_ONLY,
                  xrt_ctx_->get_kernel(pdi_name_).group_id(0));
  c_bo_ =
      xrt::bo(xrt_ctx_->get_device(), output_bo_size, XRT_BO_FLAGS_HOST_ONLY,
              xrt_ctx_->get_kernel(pdi_name_).group_id(0));
  uint16_t *b_bo_map = b_bo_.map<uint16_t *>();
  auto bo_const = BoConst(b_bo_map);
  initialize_const_params(bo_const, const_params);
  b_bo_.sync(XCL_BO_SYNC_BO_TO_DEVICE);
}

template <typename InT, typename OutT>
slice<InT, OutT>::slice(const std::string &ifm_dtype,
                        const std::string &out_dtype, bool load_xrt,
                        const std::map<std::string, std::any> &attr) {
  // operand_dtype_ = operand_dtype;
  ifm_dtype_ = ifm_dtype;
  ofm_dtype_ = out_dtype;
  ifm_dtype_size_ = sizeof(InT);
  ofm_dtype_size_ = sizeof(OutT);

  txnbin_a_header = {{"bfloat16", "a16bf"}};
  txnbin_acc_header = {{"bfloat16", "acc16bf"}};

  slice_id_ = slice_count++;

  XCLBIN_FNAME_ =
      OpInterface::get_dd_base_dir() + "\\xclbin\\stx\\SDSlice.xclbin";
  txn_fname_prefix_ = sd_slice_key_ + txnbin_a_header.at(ifm_dtype_) +
                      txnbin_acc_header.at(ofm_dtype_);

  if (attr.count("input_shape") &&
      attr.at("input_shape").type() == typeid(std::vector<int>)) {
    const auto &input_shape_vector =
        std::any_cast<const std::vector<int> &>(attr.at("input_shape"));
    input_shape_.assign(input_shape_vector.begin(), input_shape_vector.end());
  } else {
    std::cout << "Input Shape attribute not found or not of correct type."
              << std::endl;
  }

  if (attr.count("output_shape") &&
      attr.at("output_shape").type() == typeid(std::vector<int>)) {
    const auto &output_shape_vector =
        std::any_cast<const std::vector<int> &>(attr.at("output_shape"));

    output_shape_.assign(output_shape_vector.begin(),
                         output_shape_vector.end());
  } else {
    std::cout << "Output Shape attribute not found or not of correct type."
              << std::endl;
  }
  dimensions_.insert(dimensions_.end(), input_shape_.begin(),
                     input_shape_.end());
  dimensions_.insert(dimensions_.end(), output_shape_.begin(),
                     output_shape_.end());
  default_shapes_["sd_slice_a16bfacc16bf"] = std::vector<std::vector<size_t>>();
  default_shapes_["sd_slice_a16bfacc16bf"].push_back(
      {2, 1178, 1536, 2, 154, 1536});
  default_shapes_["sd_slice_a16bfacc16bf"].push_back(
      {2, 1178, 1536, 2, 1024, 1536});
  default_shapes_["sd_slice_a16bfacc16bf"].push_back(
      {2, 4250, 1536, 2, 4096, 1536});
  default_shapes_["sd_slice_a16bfacc16bf"].push_back(
      {2, 4250, 1536, 2, 154, 1536});

  default_shapes_["sd_slice_a16bfacc16bf"].push_back(
      {2, 1184, 1536, 2, 160, 1536});
  default_shapes_["sd_slice_a16bfacc16bf"].push_back(
      {2, 4256, 1536, 2, 160, 1536});
  default_shapes_["sd_slice_a16bfacc16bf"].push_back(
      {2, 1184, 1536, 2, 1024, 1536});
  default_shapes_["sd_slice_a16bfacc16bf"].push_back(
      {2, 4256, 1536, 2, 4096, 1536});

  if (load_xrt) {
    xrt_ctx_ = dynamic_dispatch::xrt_context::get_instance(XCLBIN_FNAME_);
    std::call_once(instr_reg_flag_, [this]() { setup_instr_registry(); });
  }

  a_copy_time_ = 0;
  a_sync_time_ = 0;
  b_copy_time_ = 0;
  b_sync_time_ = 0;
  c_copy_time_ = 0;
  c_sync_time_ = 0;

  run_aie_time_ = 0;
  num_run_aie_ = 0;

  std::call_once(logger_flag_, []() {
    std::string header =
        "sd_slice_id | Execute time | total time | Avg_time_per_aie_run\n";
    RYZENAI_LOG_INFO(header);
  });

  RYZENAI_LOG_TRACE(
      "[Gelu] ID: " + std::to_string(slice_id_) + ", XCLBIN: " + XCLBIN_FNAME_ +
      ", (ifm_dtype, out_dtype): (" + ifm_dtype + ", " + out_dtype + ")");
}

template <typename InT, typename OutT>
void slice<InT, OutT>::execute(std::vector<Tensor> &input,
                               std::vector<Tensor> &output) {
  a_bo_.write(input.at(0).data);
  a_bo_.sync(XCL_BO_SYNC_BO_TO_DEVICE);

  auto instr_bo_key = get_instr_key(txn_fname_prefix_, dimensions_);
  auto instr_bo = xrt_ctx_->get_registry().get_instr_bo(instr_bo_key);
  size_t instr_bo_words = instr_bo.size() / sizeof(int);

  // launch the kernel
  auto kernel_ = xrt_ctx_->get_kernel(pdi_name_);

  ryzenai::dynamic_dispatch::execute_kernel(kernel_, 2, instr_bo,
                                            instr_bo_words, a_bo_, b_bo_, c_bo_,
                                            0, 0, true, false);
  // sync output activation to host memory
  c_bo_.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
  c_bo_.read(output.at(0).data);
}

template <typename InT, typename OutT>
void slice<InT, OutT>::debug(bool enable) {
  debug_ = enable;
}

template <typename InT, typename OutT>
const std::vector<uint8_t> slice<InT, OutT>::get_transaction_bin(
    std::vector<Tensor> &input, std::vector<Tensor> &output,
    const std::map<std::string, std::any> &attr) const {
  std::string txn_key = get_instr_key(txn_fname_prefix_, dimensions_);
  Transaction &txn = Transaction::getInstance();
  return txn.get_txn_bvec(txn_key);
}

template <typename InT, typename OutT>
std::vector<OpArgMap> slice<InT, OutT>::get_buffer_reqs(
    std::vector<Tensor> &input, std::vector<Tensor> &output,
    const std::map<std::string, std::any> &attr) const {
  size_t input_bo_size =
      std::accumulate(input_shape_.begin(), input_shape_.end(), size_t(1),
                      std::multiplies<size_t>()) *
      sizeof(InT);
  size_t output_bo_size =
      std::accumulate(output_shape_.begin(), output_shape_.end(), size_t(1),
                      std::multiplies<size_t>()) *
      sizeof(OutT);
  std::vector<OpArgMap> arg_map{
      {OpArgMap::OpArgType::INPUT, 0, 0, 0, input_bo_size},
      {OpArgMap::OpArgType::CONST_INPUT, 1, 1, 0, b_bo_size_},
      {OpArgMap::OpArgType::OUTPUT, 2, 2, 0, output_bo_size}};
  return arg_map;
}

template <typename InT, typename OutT>
std::once_flag slice<InT, OutT>::logger_flag_;

template <typename InT, typename OutT>
uint64_t slice<InT, OutT>::slice_count = 0;

template <typename InT, typename OutT>
std::once_flag slice<InT, OutT>::instr_reg_flag_;

template class slice<std::uint16_t, std::uint16_t>;
} // namespace sd
} // namespace ryzenai
