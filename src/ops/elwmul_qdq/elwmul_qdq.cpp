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
#include <any>
#include <iostream>
#include <map>
#include <sstream>
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

#include <ops/ops_common/coeffs.hpp>
#include <ops/ops_common/op_util.hpp>

#include <ops/elwmul_qdq/elwmul_qdq.hpp>
#include <ops/ops_common/ctrlpkt.hpp>
#include <utils/logging.hpp>
#include <utils/utils.hpp>

// AIE Driver header
#include "ops/ops_common/kernel_structs.hpp"
#include "ops/ops_common/matmul_matrix.hpp"
#include "utils/ctrl_pkt_utils.hpp"
#include "xaiengine.h"

using namespace matmul_matrix;
using namespace kernel_structs;

namespace ryzenai {

static std::tuple<size_t, size_t>
extract_MK(const std::vector<Tensor> &inputs) {
  size_t M = 0;
  size_t K = 0;
  if (inputs.at(0).shape.size() == 2) {
    M = inputs.at(0).shape.at(0);
    K = inputs.at(0).shape.at(1);
  } else if (inputs.at(0).shape.size() == 3) {
    M = inputs.at(0).shape.at(1);
    K = inputs.at(0).shape.at(2);
  } else if (inputs.at(0).shape.size() == 4) {
    if (inputs.at(0).shape.at(1) == inputs.at(0).shape.at(2)) { // NHWC
      M = inputs.at(0).shape.at(0) * inputs.at(0).shape.at(1) *
          inputs.at(0).shape.at(2);
      K = inputs.at(0).shape.at(3);
    } else { // NCHW
      M = inputs.at(0).shape.at(2) * inputs.at(0).shape.at(3);
      K = inputs.at(0).shape.at(1);
    }
  }
  return std::make_tuple(M, K);
}

template <typename InT, typename WtT, typename OutT>
std::once_flag elwmul_qdq<InT, WtT, OutT>::logger_flag_;

template <typename InT, typename WtT, typename OutT>
uint64_t elwmul_qdq<InT, WtT, OutT>::elwmul_qdq_count = 0;

template <typename InT, typename WtT, typename OutT>
std::once_flag elwmul_qdq<InT, WtT, OutT>::instr_reg_flag_;

template <typename InT, typename WtT, typename OutT>
void elwmul_qdq<InT, WtT, OutT>::debug(bool enable) {
  debug_ = enable;
}

template <typename InT, typename WtT, typename OutT>
std::tuple<size_t, size_t>
elwmul_qdq<InT, WtT, OutT>::map_padded_shape(size_t M, size_t N) const {
  auto iter = raw_shapes_.find(txn_fname_prefix_);
  const std::vector<std::tuple<int, int>> &supported_shapes = iter->second;
  size_t Mo = M;
  size_t No = N;
  size_t fidx = 0;
  bool f_found = false;
  for (size_t i = 0; i < supported_shapes.size(); i++) {
    auto mat = supported_shapes[i];
    if (M == std::get<0>(mat) && N == std::get<1>(mat)) {
      fidx = i;
      f_found = true;
      break;
    }
  }
  if (f_found) {
    iter = default_shapes_.find(txn_fname_prefix_);
    const std::vector<std::tuple<int, int>> &actual_shapes = iter->second;
    auto mat = actual_shapes[fidx];
    Mo = std::get<0>(mat);
    No = std::get<1>(mat);
  } else {
    throw std::runtime_error("Cannot find the shape");
  }
  return std::make_tuple(Mo, No);
}

template <typename InT, typename WtT, typename OutT>
std::string elwmul_qdq<InT, WtT, OutT>::get_instr_key(std::string prefix,
                                                      size_t m,
                                                      size_t k) const {
  return "elwmul_qdq_" + prefix + "_" + std::to_string(m) + "_" +
         std::to_string(k);
}

template <typename InT, typename WtT, typename OutT>
void elwmul_qdq<InT, WtT, OutT>::setup_instr_registry() {

  std::vector<std::pair<std::string, bool>> instructions;
  std::vector<std::pair<std::string, bool>> layer_params;
  for (const auto &[mkey, value] : default_shapes_) {
    auto iter = default_shapes_.find(mkey);
    std::vector<std::tuple<int, int>> &supported_shapes = iter->second;
    for (size_t i = 0; i < supported_shapes.size(); i++) {
      auto mat = supported_shapes[i];
      auto key = get_instr_key(mkey, std::get<0>(mat), std::get<1>(mat));
      auto param_key =
          get_instr_key(mkey, std::get<0>(mat), std::get<1>(mat)) + "_param";
      instructions.push_back(std::make_pair(key, false));
      layer_params.push_back(std::make_pair(param_key, false));
    }
  }
  xrt_ctx_->get_registry().add_instructions(instructions);
  xrt_ctx_->get_registry().add_layer_params(layer_params);
}

template <typename InT, typename WtT, typename OutT>
elwmul_qdq<InT, WtT, OutT>::elwmul_qdq(
    const std::string &a_dtype, const std::string &b_dtype,
    const std::string &c_dtype, bool load_xrt,
    const std::map<std::string, std::any> &attr) {

  txnbin_a_header = {{"bfloat16", "abf16"}, {"uint16", "a16"}};
  txnbin_b_header = {{"bfloat16", "abf16"}, {"uint16", "a16"}};
  txnbin_c_header = {{"bfloat16", "accbf16"}, {"uint16", "acc16"}};

  // default shape is the padded shaped used in AIE for BO allocation
  default_shapes_["elwmul_qdq_4x4_abf16a16acc16"] =
      std::vector<std::tuple<int, int>>();
  default_shapes_["elwmul_qdq_4x2_abf16a16accbf16"] =
      std::vector<std::tuple<int, int>>();
  default_shapes_["elwmul_qdq_4x4_a16a16acc16"] =
      std::vector<std::tuple<int, int>>();
  default_shapes_["elwmul_qdq_8x4_abf16a16acc16"] =
      std::vector<std::tuple<int, int>>();
  default_shapes_["elwmul_qdq_8x4_a16a16acc16"] =
      std::vector<std::tuple<int, int>>();

  default_shapes_["elwmul_qdq_4x4_abf16a16acc16"].push_back(
      std::make_tuple(64, 5120));
  default_shapes_["elwmul_qdq_4x4_abf16a16acc16"].push_back(
      std::make_tuple(256, 5120));
  default_shapes_["elwmul_qdq_4x4_abf16a16acc16"].push_back(
      std::make_tuple(1024, 2560));
  default_shapes_["elwmul_qdq_4x4_abf16a16acc16"].push_back(
      std::make_tuple(4096, 1280));

  default_shapes_["elwmul_qdq_4x2_abf16a16accbf16"].push_back(
      std::make_tuple(512, 768));

  default_shapes_["elwmul_qdq_4x4_a16a16acc16"].push_back(
      std::make_tuple(1, 8192));
  default_shapes_["elwmul_qdq_4x4_a16a16acc16"].push_back(
      std::make_tuple(64 * 8192, 1));
  default_shapes_["elwmul_qdq_4x4_abf16a16acc16"].push_back(
      std::make_tuple(1, 8192));
  default_shapes_["elwmul_qdq_4x4_abf16a16acc16"].push_back(
      std::make_tuple(64 * 8192, 1));

  default_shapes_["elwmul_qdq_8x4_a16a16acc16"].push_back(
      std::make_tuple(1, 8192));
  default_shapes_["elwmul_qdq_8x4_a16a16acc16"].push_back(
      std::make_tuple(64 * 8192, 1));
  default_shapes_["elwmul_qdq_8x4_abf16a16acc16"].push_back(
      std::make_tuple(1, 8192));
  default_shapes_["elwmul_qdq_8x4_abf16a16acc16"].push_back(
      std::make_tuple(64 * 8192, 1));

  default_shapes_["elwmul_qdq_8x4_a16a16acc16"].push_back(
      std::make_tuple(1, 12288));
  default_shapes_["elwmul_qdq_8x4_a16a16acc16"].push_back(
      std::make_tuple(64 * 8960, 1));
  default_shapes_["elwmul_qdq_8x4_a16a16acc16"].push_back(
      std::make_tuple(64, 8960));
  default_shapes_["elwmul_qdq_8x4_abf16a16acc16"].push_back(
      std::make_tuple(1, 12288));
  default_shapes_["elwmul_qdq_8x4_abf16a16acc16"].push_back(
      std::make_tuple(64 * 8960, 1));
  default_shapes_["elwmul_qdq_8x4_abf16a16acc16"].push_back(
      std::make_tuple(64, 8960));
  // After enabling ort optimizations in PSU, M,K will be 64,8192 (64*8192,1)
  // As extract_MK function should not be disturbed, modified existing
  // transaction binaries to have this shape Older shape is also supported as we
  // may run without optimizations sometimes
  default_shapes_["elwmul_qdq_4x4_abf16a16acc16"].push_back(
      std::make_tuple(64, 8192));

  default_shapes_["elwmul_qdq_8x4_abf16a16acc16"].push_back(
      std::make_tuple(64, 8192));

  // raw shape is the actual shape from ONNX
  raw_shapes_["elwmul_qdq_4x4_abf16a16acc16"] =
      std::vector<std::tuple<int, int>>();
  raw_shapes_["elwmul_qdq_4x2_abf16a16accbf16"] =
      std::vector<std::tuple<int, int>>();
  raw_shapes_["elwmul_qdq_4x4_a16a16acc16"] =
      std::vector<std::tuple<int, int>>();
  raw_shapes_["elwmul_qdq_8x4_abf16a16acc16"] =
      std::vector<std::tuple<int, int>>();
  raw_shapes_["elwmul_qdq_8x4_a16a16acc16"] =
      std::vector<std::tuple<int, int>>();

  raw_shapes_["elwmul_qdq_4x4_abf16a16acc16"].push_back(
      std::make_tuple(64, 5120));
  raw_shapes_["elwmul_qdq_4x4_abf16a16acc16"].push_back(
      std::make_tuple(256, 5120));
  raw_shapes_["elwmul_qdq_4x4_abf16a16acc16"].push_back(
      std::make_tuple(1024, 2560));
  raw_shapes_["elwmul_qdq_4x4_abf16a16acc16"].push_back(
      std::make_tuple(4096, 1280));

  raw_shapes_["elwmul_qdq_4x2_abf16a16accbf16"].push_back(
      std::make_tuple(512, 768));

  raw_shapes_["elwmul_qdq_4x4_a16a16acc16"].push_back(std::make_tuple(1, 8192));
  raw_shapes_["elwmul_qdq_4x4_a16a16acc16"].push_back(
      std::make_tuple(64 * 8192, 1));
  raw_shapes_["elwmul_qdq_4x4_abf16a16acc16"].push_back(
      std::make_tuple(1, 8192));
  raw_shapes_["elwmul_qdq_4x4_abf16a16acc16"].push_back(
      std::make_tuple(64 * 8192, 1));

  raw_shapes_["elwmul_qdq_8x4_a16a16acc16"].push_back(std::make_tuple(1, 8192));
  raw_shapes_["elwmul_qdq_8x4_a16a16acc16"].push_back(
      std::make_tuple(64 * 8192, 1));
  raw_shapes_["elwmul_qdq_8x4_abf16a16acc16"].push_back(
      std::make_tuple(1, 8192));
  raw_shapes_["elwmul_qdq_8x4_abf16a16acc16"].push_back(
      std::make_tuple(64 * 8192, 1));

  raw_shapes_["elwmul_qdq_8x4_a16a16acc16"].push_back(std::make_tuple(1, 8960));
  raw_shapes_["elwmul_qdq_8x4_a16a16acc16"].push_back(
      std::make_tuple(64 * 8960, 1));
  raw_shapes_["elwmul_qdq_8x4_a16a16acc16"].push_back(
      std::make_tuple(64, 8960));
  raw_shapes_["elwmul_qdq_8x4_abf16a16acc16"].push_back(
      std::make_tuple(1, 8960));
  raw_shapes_["elwmul_qdq_8x4_abf16a16acc16"].push_back(
      std::make_tuple(64 * 8960, 1));
  raw_shapes_["elwmul_qdq_8x4_abf16a16acc16"].push_back(
      std::make_tuple(64, 8960));
  // After enabling ort optimizations in PSU, M,K will be 64,8192 instead of
  // (64*8192,1) As extract_MK function should not be disturbed, modified
  // existing transaction binaries to have this shape Older shape is also
  // supported as we may run without optimizations sometimes After enabling ort
  // optimizations in PSU, M,K will be 64,8192 instead of (64*8192,1) As
  // extract_MK function should not be disturbed, modified existing transaction
  // binaries to have this shape Older shape is also supported as we may run
  // without optimizations sometimes
  raw_shapes_["elwmul_qdq_4x4_abf16a16acc16"].push_back(
      std::make_tuple(64, 8192));
  raw_shapes_["elwmul_qdq_8x4_abf16a16acc16"].push_back(
      std::make_tuple(64, 8192));

  a_dtype_ = a_dtype;
  b_dtype_ = b_dtype;
  c_dtype_ = c_dtype;

  a_dtype_size_ = sizeof(InT);
  b_dtype_size_ = sizeof(WtT);
  c_dtype_size_ = sizeof(OutT);
  is_generic_fusion = OpsFusion::check_generic_fusion(attr);

  elwmul_qdq_id_ = elwmul_qdq_count++;

  /*select xclbin based on the input/output types*/
  std::string XCLBIN_FNAME =
      OpInterface::get_dd_base_dir() + ryzenai::mdsqr_A8W8_QDQ_XCLBIN_PATH;

  if (a_dtype_ == "uint16") {
    XCLBIN_FNAME =
        OpInterface::get_dd_base_dir() + ryzenai::mxpzi_A16W8_QDQ_XCLBIN_PATH;
  }

  design_param_ = "";
  if (attr.count("design_param") &&
      attr.at("design_param").type() == typeid(std::vector<std::string>)) {
    const auto &design_param_vector =
        std::any_cast<const std::vector<std::string> &>(
            attr.at("design_param"));

    if (design_param_vector.size() == 1) {
      design_param_ = design_param_vector[0];
    } else {
      std::cout
          << "Design Format attribute does not have the expected number of "
             "elements.Number of passed : design_param_vector.size(), "
             "Expected:1"
          << std::endl;
    }
    RYZENAI_LOG_TRACE("iConv: DesignFormat: " + design_param_);
  }

  txn_fname_prefix_ = "elwmul_qdq_4x2_" + txnbin_a_header.at(a_dtype_) +
                      txnbin_b_header.at(b_dtype_) +
                      txnbin_c_header.at(c_dtype_);

  param_fname_prefix_ = "elwmul_qdq_4x2_" + txnbin_a_header.at(a_dtype_) +
                        txnbin_b_header.at(b_dtype_) +
                        txnbin_c_header.at(c_dtype_);

  if (design_param_.find("4x4") != std::string::npos) { // 4x4 design
    txn_fname_prefix_ = "elwmul_qdq_4x4_" + txnbin_a_header.at(a_dtype_) +
                        txnbin_b_header.at(b_dtype_) +
                        txnbin_c_header.at(c_dtype_);

    param_fname_prefix_ = "elwmul_qdq_4x4_" + txnbin_a_header.at(a_dtype_) +
                          txnbin_b_header.at(b_dtype_) +
                          txnbin_c_header.at(c_dtype_);
  } else if (design_param_.find("8x4") != std::string::npos) { // 8x4 design
    txn_fname_prefix_ = "elwmul_qdq_8x4_" + txnbin_a_header.at(a_dtype_) +
                        txnbin_b_header.at(b_dtype_) +
                        txnbin_c_header.at(c_dtype_);

    param_fname_prefix_ = "elwmul_qdq_8x4_" + txnbin_a_header.at(a_dtype_) +
                          txnbin_b_header.at(b_dtype_) +
                          txnbin_c_header.at(c_dtype_);
  }

  KERNEL_M_MAX = 512;

  w_shape_[0] = KERNEL_M_MAX;
  w_shape_[1] = 768;

  if (load_xrt) {
    xrt_ctx_ = dynamic_dispatch::xrt_context::get_instance(XCLBIN_FNAME);
    std::call_once(instr_reg_flag_, [this]() { setup_instr_registry(); });
  }

  a_copy_time_ = 0;
  a_sync_time_ = 0;
  b_copy_time_ = 0;
  b_format_time_ = 0;
  b_sync_time_ = 0;
  c_copy_time_ = 0;
  c_sync_time_ = 0;
  run_aie_time_ = 0;
  cpu_acc_time_ = 0;
  num_run_aie_ = 0;
  is_ctrl_pkt_ = 0;

  std::call_once(logger_flag_, []() {
    std::string header =
        "elwmul_qdq_id M K N kernel_m kernel_k kernel_n Execute"
        "time(us) num_aie_runs run_aie_time(ns) "
        "A_copy_time(ns) A_sync_time(ns) "
        "C_copy_time(ns) C_sync_time(ns) "
        "Avg_time_per_aie_run(ns)\n";
    RYZENAI_LOG_INFO(header);
  });

  RYZENAI_LOG_TRACE("[Elwmul_qdq] ID: " + std::to_string(elwmul_qdq_id_) +
                    ", XCLBIN: " + XCLBIN_FNAME +
                    ", (a_dtype, b_dtype, c_dtype): (" + a_dtype_ + ", " +
                    b_dtype_ + ", " + c_dtype_ + ")");
}

template <typename InT, typename WtT, typename OutT>
void elwmul_qdq<InT, WtT, OutT>::set_params(const std::string &model_name,
                                            std::vector<size_t> input_shape) {
  std::string XCLBIN_FNAME;
  if (model_name == "mzdk5") {
    XCLBIN_FNAME =
        OpInterface::get_dd_base_dir() + ryzenai::mzdk5_A16W8_QDQ_XCLBIN_PATH;
  } else if (model_name == "4x4mzdk5") {
    is_ctrl_pkt_ = 1;
    XCLBIN_FNAME = OpInterface::get_dd_base_dir() +
                   ryzenai::mzdk54x4_A16W8_QDQ_XCLBIN_PATH;
  } else if (model_name == "START_TAIL_PS") {
    XCLBIN_FNAME = OpInterface::get_dd_base_dir() +
                   ryzenai::START_TAIL_4x2_MS_SHELL_QDQ_XCLBIN_PATH;
  } else if (model_name == "4x4PSU") {
    is_ctrl_pkt_ = 1;
    XCLBIN_FNAME =
        OpInterface::get_dd_base_dir() + ryzenai::PSU_4x4_A16W8_QDQ_XCLBIN_PATH;
  } else if (model_name == "8x4PSU") {
    is_ctrl_pkt_ = 1;
    XCLBIN_FNAME =
        OpInterface::get_dd_base_dir() + ryzenai::PSU_8x4_A16W8_QDQ_XCLBIN_PATH;
  } else if (model_name == "8x4HFDS") {
    is_ctrl_pkt_ = 1;
    XCLBIN_FNAME = OpInterface::get_dd_base_dir() +
                   ryzenai::HFDS_8x4_A16W8_QDQ_XCLBIN_PATH;
  } else {
    throw std::invalid_argument("model_name is not supported");
  }

  auto [M, K] = map_padded_shape(input_shape.at(0), input_shape.at(1));
  w_shape_[0] = M;
  w_shape_[1] = K;

  xrt_ctx_ = dynamic_dispatch::xrt_context::get_instance(XCLBIN_FNAME);
  std::call_once(instr_reg_flag_, [this]() { setup_instr_registry(); });
}

void calculate_qdq(ConstBufferIO &io, const std::vector<Tensor> &const_params,
                   const std::map<std::string, std::any> &attr,
                   std::vector<int32_t> &elwmul_coeffs) {

  float in_1_scale = *((float *)const_params.at(0).data);
  uint32_t in_1_zp = *((uint32_t *)const_params.at(1).data);
  float in_0_scale = *((float *)const_params.at(2).data);
  uint32_t in_0_zp = *((uint32_t *)const_params.at(3).data);
  float out_scale = *((float *)const_params.at(4).data);
  uint32_t out_zp = *((uint32_t *)const_params.at(5).data);

  float slice_q_scale =
      std::any_cast<std::vector<float>>(attr.at("input_q_params"))[0];
  float slice_q_zp =
      std::any_cast<std::vector<float>>(attr.at("input_q_params"))[1];

  in_0_scale = (float)slice_q_scale;
  in_0_zp = (int)slice_q_zp;

  auto [a_scale, a_zp] = OpsFusion::coeffs::calc_lrn_coeff(in_0_scale, in_0_zp);
  auto [b_scale, b_zp] = OpsFusion::coeffs::calc_lrn_coeff(in_1_scale, in_1_zp);
  auto [final_out_scale, final_out_zp] =
      OpsFusion::coeffs::calc_lrn_coeff(1 / out_scale, out_zp);

  elwmul_coeffs[0] = b_scale;
  elwmul_coeffs[1] = b_zp;
  elwmul_coeffs[2] = a_scale;
  elwmul_coeffs[3] = a_zp;
  elwmul_coeffs[4] = final_out_scale;
  elwmul_coeffs[5] = final_out_zp;
  elwmul_coeffs[6] = 0;
  elwmul_coeffs[7] = 1; // elwmul_coeffs
}

template <typename InT, typename WtT, typename OutT>
void elwmul_qdq<InT, WtT, OutT>::initialize_const_params(
    ConstBufferIO &io, const std::vector<Tensor> &const_params,
    const std::map<std::string, std::any> &attr) {
  RYZENAI_LOG_TRACE("Elwmul_qdq initialize_const_params(ptr) ...");

  int32_t *qdq_params;
  elwmul_qdq_coeffs = std::vector<int32_t>(16, 0);
  if (is_generic_fusion) {
    DD_THROW_IF(
        (const_params.size() != 6),
        OpsFusion::dd_format("Unsupported const spec for elwmul_qdq\n") +
            OpsFusion::dd_format("(Details : #const params == 6 ({})",
                                 const_params.size()));
    calculate_qdq(io, const_params, attr, elwmul_qdq_coeffs);
    qdq_params = elwmul_qdq_coeffs.data();
  } else {
    DD_THROW_IF(
        (const_params.size() != 1),
        OpsFusion::dd_format("Unsupported const spec for elwmul_qdq\n") +
            OpsFusion::dd_format("(Details : #const params == 1 ({})",
                                 const_params.size()));
    qdq_params = (int32_t *)const_params.at(0).data;
  }

  if (design_param_ == "4x4PSU" || design_param_ == "8x4PSU" ||
      design_param_ == "8x4HFDS") {
    std::vector<int32_t> qdq_data(QDQparam_size,
                                  0); // temp buffer to create qdq params
                                      // required by kernel
    elwddd_QdqParams *p_qdq_struct =
        (elwddd_QdqParams *)
            qdq_data.data(); // casting the temp memory created to
                             // struct so as to fill the elments

    p_qdq_struct->matA_zero_point = (uint16_t)qdq_params[1];
    p_qdq_struct->matA_scale = (uint16_t)qdq_params[0];
    p_qdq_struct->matB_zero_point = (uint16_t)qdq_params[3];
    p_qdq_struct->matB_scale = (uint16_t)qdq_params[2];
    p_qdq_struct->out_zero_point = (uint16_t)qdq_params[5];
    p_qdq_struct->out_scale = (uint16_t)qdq_params[4];
    p_qdq_struct->dq_A_enable = (uint16_t)qdq_params[6];
    p_qdq_struct->q_out_enable = (uint16_t)qdq_params[7];
    p_qdq_struct->dq_B_enable =
        1; // sw as of now doesn't have this flag and dQ B
           // is by default enable for all win24 models
    auto qdq_params_size = matmul_matrix::QDQparam_size * sizeof(int32_t);
    io.write(0, (void *)qdq_data.data(), qdq_params_size);
  } else {
    auto temp = qdq_params[0];
    qdq_params[0] = qdq_params[1];
    qdq_params[1] = temp;
    temp = qdq_params[2];
    qdq_params[2] = qdq_params[3];
    qdq_params[3] = temp;
    temp = qdq_params[4];
    qdq_params[4] = qdq_params[5];
    qdq_params[5] = temp;
    auto qdq_params_size = matmul_matrix::QDQparam_size * sizeof(int32_t);
    io.write(0, (void *)qdq_params, qdq_params_size);
  }
  RYZENAI_LOG_TRACE("elwmul_qdq initialize_const_params(ptr) ... DONE");
}

template <typename InT, typename WtT, typename OutT>
void elwmul_qdq<InT, WtT, OutT>::initialize_const_params(
    const std::vector<Tensor> &const_params,
    const std::map<std::string, std::any> &attr) {

  if (const_params.size() != 1) {
    throw std::runtime_error(
        "elwmul_qdq IPU Wrapper expect to have one constant.");
  }

  kernel_x_shape_[0] = w_shape_[0];
  kernel_x_shape_[1] = w_shape_[1];

  kernel_y_shape_[0] = 0;
  kernel_y_shape_[1] = 0;

  kernel_z_shape_[0] = w_shape_[0];
  kernel_z_shape_[1] = w_shape_[1];

  b_copy_time_ = 0;
  b_format_time_ = 0;
  b_sync_time_ = 0;
  /* Create input/output BOs */
  const size_t B_BO_SIZE = matmul_matrix::QDQparam_size * sizeof(int32_t);
  const size_t A_BO_SIZE =
      2 * (kernel_x_shape_[0] * kernel_x_shape_[1] * a_dtype_size_);
  const size_t C_BO_SIZE =
      (kernel_z_shape_[0] * kernel_z_shape_[1] * c_dtype_size_);
  RYZENAI_LOG_TRACE("elwmul_qdq: A_BO_SIZE:" + std::to_string(A_BO_SIZE) +
                    " B_BO_SIZE:" + std::to_string(B_BO_SIZE) +
                    " C_BO_SIZE:" + std::to_string(C_BO_SIZE));
  b_bo_ = xrt::bo(xrt_ctx_->get_device(), B_BO_SIZE, XRT_BO_FLAGS_HOST_ONLY,
                  xrt_ctx_->get_kernel().group_id(8));

  a_bo_ = xrt::bo(xrt_ctx_->get_device(), A_BO_SIZE, XRT_BO_FLAGS_HOST_ONLY,
                  xrt_ctx_->get_kernel().group_id(8));

  c_bo_ = xrt::bo(xrt_ctx_->get_device(), C_BO_SIZE, XRT_BO_FLAGS_HOST_ONLY,
                  xrt_ctx_->get_kernel().group_id(8));

  // copy b_bo
  b_copy_time_ = 0;
  b_format_time_ = 0;
  b_sync_time_ = 0;

  auto b_copy_start = GET_ELAPSED_TIME_NS();
  auto b_format_start = GET_ELAPSED_TIME_NS();
  WtT *b_bo_map = b_bo_.map<WtT *>();
  auto bo_const = BoConst(b_bo_map);
  initialize_const_params(bo_const, const_params);
  auto b_format_stop = GET_ELAPSED_TIME_NS();
  auto b_copy_stop = GET_ELAPSED_TIME_NS();
  b_format_time_ += static_cast<int64_t>(b_format_stop - b_format_start);
  b_copy_time_ = static_cast<int64_t>(b_copy_stop - b_copy_start);

  // sync b_bo
  auto b_sync_start = GET_ELAPSED_TIME_NS();
  b_bo_.sync(XCL_BO_SYNC_BO_TO_DEVICE);
  auto b_sync_stop = GET_ELAPSED_TIME_NS();
  b_sync_time_ = static_cast<int64_t>(b_sync_stop - b_sync_start);

  std::vector<size_t> param_shape = {kernel_x_shape_[0], kernel_x_shape_[1],
                                     kernel_y_shape_[1]};
  if (is_ctrl_pkt_) {
    // Based on the mapped_shape to get the meta json file
    std::vector<uint8_t> json_data;
    try {
      auto json_key = get_instr_key(param_fname_prefix_, kernel_x_shape_[0],
                                    kernel_x_shape_[1]) +
                      "_ctrl_meta";
      Transaction &txn = Transaction::getInstance();
      json_data = txn.get_txn_bvec(json_key);
    } catch (...) {
      is_ctrl_pkt_ = 0;
    }

    if (is_ctrl_pkt_) {
      std::cout << "ctrlpkt patching" << std::endl;
      RYZENAI_LOG_TRACE("act_act_matmul patch ctrlpkt ... START");
      // get param_bo address
      auto param_bo_key = get_instr_key(param_fname_prefix_, kernel_x_shape_[0],
                                        kernel_x_shape_[1]) +
                          "_param";
      const xrt::bo &param_bo =
          xrt_ctx_->get_registry().get_param_bo(param_bo_key).second;

      // Get ctrl pkt patch info from json
      std::vector<CtrlPktPatchInfo> ctrlpkt_info;
      ctrlpkt_info = json_str_to_ctrlpkt_patch_info(json_data);

      // Get the ctrl pkt
      auto ctrl_bo_key = get_instr_key(param_fname_prefix_, kernel_x_shape_[0],
                                       kernel_x_shape_[1]) +
                         "_ctrl";
      std::string ctrl_params =
          Transaction::getInstance().get_txn_str(ctrl_bo_key);
      std::vector<char> ctrl_buffer(ctrl_params.begin(), ctrl_params.end());

      // ctrl pkt patch
      std::vector<char> ctrl_pkt_new;
      std::vector<uint64_t> buffer_addrs = {
          uint64_t(c_bo_.address() + DDR_AIE_ADDR_OFFSET),
          uint64_t(a_bo_.address() + DDR_AIE_ADDR_OFFSET),
          uint64_t(b_bo_.address() + DDR_AIE_ADDR_OFFSET),
          uint64_t(param_bo.address() + DDR_AIE_ADDR_OFFSET)};
      ctrl_pkt_new = patch_ctrl_bin(ctrl_buffer, ctrlpkt_info, buffer_addrs);

      size_t ctrl_bo_words = ctrl_pkt_new.size();
      ctrl_bo_ =
          xrt::bo(xrt_ctx_->get_device(), ctrl_bo_words, XRT_BO_FLAGS_HOST_ONLY,
                  xrt_ctx_->get_kernel().group_id(8));
      ctrl_bo_.write(ctrl_pkt_new.data());
      ctrl_bo_.sync(XCL_BO_SYNC_BO_TO_DEVICE);
      RYZENAI_LOG_TRACE("act_act_matmul patch ctrlpkt ... DONE");
    }
  }
  RYZENAI_LOG_TRACE("act_act_matmul initialize_const_params ... DONE");
}

template <typename InT, typename WtT, typename OutT>
void elwmul_qdq<InT, WtT, OutT>::execute(std::vector<Tensor> &input,
                                         std::vector<Tensor> &output) {
  // Check the number of inputs
  if (input.size() != 2) {
    throw std::runtime_error(
        "elwmul_qdq IPU Wrapper expect to have two inputs.");
  }
  const int a_idx = 0;
  // The first data is a and second data is b
  InT *a = (InT *)input.at(a_idx).data;
  InT *b = (InT *)input.at(a_idx + 1).data;

  a_copy_time_ = 0;
  a_sync_time_ = 0;
  b_copy_time_ = 0;
  b_format_time_ = 0;
  b_sync_time_ = 0;
  c_copy_time_ = 0;
  c_sync_time_ = 0;
  run_aie_time_ = 0;
  cpu_acc_time_ = 0;

  auto exec_start = GET_ELAPSED_TIME_NS();

  a_shape_[0] = input.at(a_idx).shape.at(0);
  a_shape_[1] = input.at(a_idx).shape.at(1);

  // each input needs to have M * K size
  auto [M, K] = map_padded_shape(a_shape_[0], a_shape_[1]);
  c_shape_[0] = a_shape_[0];
  c_shape_[1] = a_shape_[1];

  size_t a_size = a_shape_[0] * a_shape_[1] * sizeof(InT);
  RYZENAI_LOG_TRACE("elwmul_qdq: a_size:" + std::to_string(a_size));
  // a_bo copy
  auto a_copy_start = GET_ELAPSED_TIME_NS();
  InT *a_bo_map = a_bo_.map<InT *>();
  memcpy((void *)a_bo_map, (void *)a, a_size);
  memcpy((void *)(reinterpret_cast<int8_t *>(a_bo_map) + M * K * sizeof(InT)),
         (void *)b, a_size);
  // memcpy((void*)&a_bo_map[a_size], (void*)b, a_size);

  auto a_copy_stop = GET_ELAPSED_TIME_NS();

  // a_bo sync
  auto a_sync_start = GET_ELAPSED_TIME_NS();
  a_bo_.sync(XCL_BO_SYNC_BO_TO_DEVICE);
  auto a_sync_stop = GET_ELAPSED_TIME_NS();

  a_copy_time_ = static_cast<int64_t>(a_copy_stop - a_copy_start);
  a_sync_time_ = static_cast<int64_t>(a_sync_stop - a_sync_start);

  // prepare inst_bo and param_bo
  auto instr_bo_key = get_instr_key(txn_fname_prefix_, M, K);
  auto param_bo_key = get_instr_key(param_fname_prefix_, M, K) + "_param";
  auto instr_bo = xrt_ctx_->get_registry().get_instr_bo(instr_bo_key);
  const xrt::bo &param_bo =
      xrt_ctx_->get_registry().get_param_bo(param_bo_key).second;
  uint32_t instr_bo_words = uint32_t(instr_bo.size() / sizeof(int));

  auto kernel_ = xrt_ctx_->get_kernel();
  // launch the kernel
  auto run_aie_start = GET_ELAPSED_TIME_NS();
  c_bo_.sync(XCL_BO_SYNC_BO_TO_DEVICE);
  ryzenai::dynamic_dispatch::execute_kernel(
      kernel_, 2, instr_bo, instr_bo_words, c_bo_, a_bo_, b_bo_, param_bo,
      ctrl_bo_, true, is_ctrl_pkt_);
  auto run_aie_stop = GET_ELAPSED_TIME_NS();
  run_aie_time_ += static_cast<int64_t>(run_aie_stop - run_aie_start);
  num_run_aie_++;

  // sync output activation to host memory
  auto c_sync_start = GET_ELAPSED_TIME_NS();
  c_bo_.sync(XCL_BO_SYNC_BO_FROM_DEVICE);
  auto c_sync_stop = GET_ELAPSED_TIME_NS();
  c_sync_time_ += static_cast<int64_t>(c_sync_stop - c_sync_start);

  // copy c_bo to host memory
  auto aie_out = (OutT *)output.at(0).data;
  auto c_copy_start = GET_ELAPSED_TIME_NS();
  OutT *c_bo_map = c_bo_.map<OutT *>();
  memcpy((void *)aie_out, (void *)c_bo_map,
         c_shape_[0] * c_shape_[1] * sizeof(OutT));
  auto c_copy_stop = GET_ELAPSED_TIME_NS();
  c_copy_time_ = static_cast<int64_t>(c_copy_stop - c_copy_start);
  auto exec_end = GET_ELAPSED_TIME_NS();

  RYZENAI_LOG_INFO(
      std::to_string(elwmul_qdq_id_) + " " + std::to_string(a_shape_[0]) + " " +
      std::to_string(a_shape_[1]) + " " + std::to_string(w_shape_[1]) + " " +
      std::to_string(exec_end - exec_start) + " " +
      std::to_string(num_run_aie_) + " " + std::to_string(run_aie_time_) + " " +
      std::to_string(a_copy_time_) + " " + std::to_string(a_sync_time_) + " " +
      std::to_string(c_copy_time_) + " " + std::to_string(c_sync_time_) + " " +
      std::to_string((double)run_aie_time_ / num_run_aie_) + "\n");
}

template <typename InT, typename WtT, typename OutT>
const std::vector<uint8_t> elwmul_qdq<InT, WtT, OutT>::get_transaction_bin(
    std::vector<Tensor> &input, std::vector<Tensor> &output,
    const std::map<std::string, std::any> &attr) const {
  auto [M, K] = extract_MK(input);
  auto [Mo, Ko] = map_padded_shape(M, K);
  std::string txn_key = get_instr_key(txn_fname_prefix_, Mo, Ko);
  Transaction &txn = Transaction::getInstance();
  return txn.get_txn_bvec(txn_key);
}

template <typename InT, typename WtT, typename OutT>
const std::vector<uint8_t> elwmul_qdq<InT, WtT, OutT>::get_super_kernel_params(
    std::vector<Tensor> &input, std::vector<Tensor> &output,
    const std::map<std::string, std::any> &attr) const {
  auto [M, K] = extract_MK(input);
  auto [Mo, Ko] = map_padded_shape(M, K);
  // TODO: Add check to validate tensor shapes
  std::string param_key = get_instr_key(param_fname_prefix_, Mo, Ko) + "_param";
  // std::cout << "Super kernel params name : " << fname << std::endl;
  Transaction &txn = Transaction::getInstance();
  return txn.get_txn_bvec(param_key);
}

template <typename InT, typename WtT, typename OutT>
std::vector<uint8_t> elwmul_qdq<InT, WtT, OutT>::get_ctrl_pkts(
    std::vector<Tensor> &input, std::vector<Tensor> &output,
    const std::map<std::string, std::any> &attr) const {
  auto [M, K] = extract_MK(input);
  auto [Mo, Ko] = map_padded_shape(M, K);
  // TODO: Add check to validate tensor shapes
  std::string ctrl_key = get_instr_key(param_fname_prefix_, Mo, Ko) + "_ctrl";
  try {
    Transaction &txn = Transaction::getInstance();
    return txn.get_txn_bvec(ctrl_key);
  } catch (...) {
    return {};
  }
}

template <typename InT, typename WtT, typename OutT>
std::vector<CtrlPktPatchInfo>
elwmul_qdq<InT, WtT, OutT>::get_ctrl_pkt_patch_info(
    std::vector<Tensor> &input, std::vector<Tensor> &output,
    const std::map<std::string, std::any> &attr) const {
  auto [M, K] = extract_MK(input);
  auto [Mo, Ko] = map_padded_shape(M, K);
  // TODO: Add check to validate tensor shapes
  try {
    auto ctrl_pkt_meta =
        get_instr_key(param_fname_prefix_, Mo, Ko) + "_ctrl_meta";
    Transaction &txn = Transaction::getInstance();
    return json_str_to_ctrlpkt_patch_info(txn.get_txn_bvec(ctrl_pkt_meta));
  } catch (...) {
    // throw std::runtime_error(
    //     "elwmul_qdq : Can not file the ctrl_meta.json file");
    return {};
  }
}

template <typename InT, typename WtT, typename OutT>
std::vector<OpArgMap> elwmul_qdq<InT, WtT, OutT>::get_buffer_reqs(
    std::vector<Tensor> &input, std::vector<Tensor> &output,
    const std::map<std::string, std::any> &attr) const {
  auto [M, N] = extract_MK(input);

  auto [Mo, No] = map_padded_shape(M, N);

  size_t output_idx = is_generic_fusion ? 8 : 3;
  size_t const_params_bo_size = matmul_matrix::QDQparam_size * sizeof(int32_t);
  size_t input_1_bo_size = (Mo * No * sizeof(InT));
  size_t input_2_bo_size = (Mo * No * sizeof(WtT));
  size_t output_bo_size = (Mo * No * sizeof(OutT));
  size_t super_kernel_size = get_super_kernel_params(input, output).size();
  size_t ctrl_pkt_size = get_ctrl_pkts(input, output).size();

  std::vector<OpArgMap> arg_map{
      {OpArgMap::OpArgType::INPUT, 1, 0, 0, input_1_bo_size},
      {OpArgMap::OpArgType::INPUT, 1, 1, input_1_bo_size, input_2_bo_size},
      {OpArgMap::OpArgType::CONST_INPUT, 2, 2, 0, const_params_bo_size},
      {OpArgMap::OpArgType::OUTPUT, 0, output_idx, 0, output_bo_size},
      {OpArgMap::OpArgType::CONST_KERNEL_PARAM_INPUT, 3, 0, 0,
       super_kernel_size},
      {OpArgMap::OpArgType::CTRL_PKT_BIN, 4, 0, 0, ctrl_pkt_size}};
  return arg_map;
}

template class elwmul_qdq<uint16_t, uint16_t, uint16_t>;

} // namespace ryzenai
