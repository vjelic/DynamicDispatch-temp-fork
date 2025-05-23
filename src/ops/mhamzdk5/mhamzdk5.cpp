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
#include <array>
#include <fstream>
#include <iostream>
#include <map>
#include <numeric>
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

#include "utils/ctrl_pkt_utils.hpp"
#include <ops/mhamzdk5/mhamzdk5.hpp>
#include <ops/op_interface.hpp>
#include <ops/ops_common/coeffs.hpp>
#include <ops/ops_common/ctrlpkt.hpp>
#include <ops/ops_common/op_util.hpp>
#include <utils/logging.hpp>
#include <utils/utils.hpp>
// AIE Driver header
#include "xaiengine.h"

#include "ops/ops_common/mhagprb_matrix.hpp"

namespace ryzenai {

static std::array<size_t, 2> extract_shape(const Tensor &tensor) {
  std::array<size_t, 2> res;
  if (tensor.shape.size() == 4) {
    // if (tensor.shape.at(1) == tensor.shape.at(2)) // assume NHWC
    res = {tensor.shape.at(1) * tensor.shape.at(2), tensor.shape.at(3)};
    // else // NCHW
    //  res = {tensor.shape.at(2) * tensor.shape.at(3), tensor.shape.at(1)};
  } else if (tensor.shape.size() == 3) {
    res = {tensor.shape.at(0) * tensor.shape.at(1), tensor.shape.at(2)};
  } else if (tensor.shape.size() == 2) {
    res = {tensor.shape.at(0), tensor.shape.at(1)};
  } else if (tensor.shape.size() == 1) {
    res = {tensor.shape.at(0)};
  } else {
    throw std::runtime_error("MHA : Invalid shape received for Matrix");
  }
  return res;
}

template <typename InT, typename WtT, typename OutT>
std::once_flag mhamzdk5<InT, WtT, OutT>::instr_reg_flag_;

template <typename InT, typename WtT, typename OutT>
void mhamzdk5<InT, WtT, OutT>::debug(bool enable) {
  debug_ = enable;
}

template <typename InT, typename WtT, typename OutT>
std::string
mhamzdk5<InT, WtT, OutT>::get_instr_key(std::string prefix,
                                        std::vector<size_t> &mat) const {
  std::string out_str = "mhamzdk5_" + prefix;
  for (size_t i = 0; i < mat.size(); i++) {
    out_str += "_" + std::to_string(mat[i]);
  }
  return out_str;
}

template <typename InT, typename WtT, typename OutT>
std::once_flag mhamzdk5<InT, WtT, OutT>::logger_flag_;

template <typename InT, typename WtT, typename OutT>
uint64_t mhamzdk5<InT, WtT, OutT>::mhamzdk5_count = 0;

template <typename InT, typename WtT, typename OutT>
void mhamzdk5<InT, WtT, OutT>::setup_instr_registry() {
  std::vector<std::pair<std::string, bool>> instructions;
  std::vector<std::pair<std::string, bool>> layer_params;
  // mhamzdk5
  std::vector<std::vector<size_t>> supported_shapes =
      default_shapes_.find(txn_fname_prefix_)->second;
  for (size_t i = 0; i < supported_shapes.size(); i++) {
    auto mat = supported_shapes.at(i);
    auto key = get_instr_key(txn_fname_prefix_, mat);
    auto param_key = get_instr_key(param_fname_prefix_, mat) + "_param";
    instructions.push_back(std::make_pair(key, false));
    layer_params.push_back(std::make_pair(param_key, false));
  }

  xrt_ctx_->get_registry().add_instructions(instructions);
  xrt_ctx_->get_registry().add_layer_params(layer_params);
}

template <typename InT, typename WtT, typename OutT>
mhamzdk5<InT, WtT, OutT>::mhamzdk5(
    const std::string &a_dtype, const std::string &b_dtype,
    const std::string &c_dtype, bool load_xrt,
    const std::map<std::string, std::any> &attr) {

  is_generic_pass_in_onnx = OpsFusion::check_generic_fusion(attr);

  if (is_generic_pass_in_onnx) {
    q_shape_back = std::any_cast<std::vector<int>>(attr.at("q_shape_back"))[0];
    k_shape_back = std::any_cast<std::vector<int>>(attr.at("k_shape_back"))[0];
  }

  txnbin_a_header = {{"uint16", "a16"}, {"uint8", "a8"}};

  txnbin_b_header = {{"uint16", "w16"}, {"uint8", "w8"}};

  txnbin_acc_header = {{"uint16", "acc16"}, {"uint8", "acc8"}};

  // corss mha
  default_shapes_["mhamzdk5_4x4_a16w8acc16"] =
      std::vector<std::vector<size_t>>();
  default_shapes_["mhamzdk5_4x4_a16w8acc16"].push_back(
      std::vector<size_t>{64, 77, 64});
  default_shapes_["mhamzdk5_4x4_a16w8acc16"].push_back(
      std::vector<size_t>{256, 77, 64});
  default_shapes_["mhamzdk5_4x4_a16w8acc16"].push_back(
      std::vector<size_t>{1024, 77, 64});
  default_shapes_["mhamzdk5_4x4_a16w8acc16"].push_back(
      std::vector<size_t>{4096, 77, 64});

  // self mha
  default_shapes_["mhamzdk5_4x4_a16w8acc16"].push_back(
      std::vector<size_t>{64, 64, 64});

  a_dtype_ = a_dtype;
  b_dtype_ = b_dtype;
  c_dtype_ = c_dtype;

  a_dtype_size_ = sizeof(InT);
  b_dtype_size_ = sizeof(WtT);
  c_dtype_size_ = sizeof(OutT);

  mhamzdk5_id_ = mhamzdk5_count++;

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

  txn_fname_prefix_ = "mhamzdk5_4x2_" + txnbin_a_header.at(a_dtype_) +
                      txnbin_b_header.at(b_dtype_) +
                      txnbin_acc_header.at(c_dtype_);

  param_fname_prefix_ = "mhamzdk5_4x2_" + txnbin_a_header.at(a_dtype_) +
                        txnbin_b_header.at(b_dtype_) +
                        txnbin_acc_header.at(c_dtype_);

  if (design_param_.find("4x4") != std::string::npos) { // 4x4 design
    txn_fname_prefix_ = "mhamzdk5_4x4_" + txnbin_a_header.at(a_dtype_) +
                        txnbin_b_header.at(b_dtype_) +
                        txnbin_acc_header.at(c_dtype_);

    param_fname_prefix_ = "mhamzdk5_4x4_" + txnbin_a_header.at(a_dtype_) +
                          txnbin_b_header.at(b_dtype_) +
                          txnbin_acc_header.at(c_dtype_);
  }
  KERNEL_M_MAX = 512;

  if (load_xrt == true) {
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
        "ipu_wrapper_id M K N kernel_m kernel_k kernel_n Execute"
        "time(us) num_aie_runs run_aie_time(ns) "
        "A_copy_time(ns) A_sync_time(ns) "
        "C_copy_time(ns) C_sync_time(ns) "
        "Avg_time_per_aie_run(ns)\n";
    RYZENAI_LOG_INFO(header);
  });

  RYZENAI_LOG_TRACE("[mhamzdk5] ID: " + std::to_string(mhamzdk5_id_) +
                    ", XCLBIN: " + XCLBIN_FNAME +
                    ", (a_dtype, b_dtype, c_dtype): (" + a_dtype + ", " +
                    b_dtype + ", " + c_dtype + ")");
}

template <typename InT, typename WtT, typename OutT>
void mhamzdk5<InT, WtT, OutT>::set_params(const std::string &model_name,
                                          std::vector<size_t> input_shape) {
  std::string XCLBIN_FNAME;

  if (model_name == "mzdk5") {
    XCLBIN_FNAME =
        OpInterface::get_dd_base_dir() + ryzenai::mzdk5_A16W8_QDQ_XCLBIN_PATH;
  } else if (model_name == "4x4mzdk5") {
    is_ctrl_pkt_ = 1;
    XCLBIN_FNAME = OpInterface::get_dd_base_dir() +
                   ryzenai::mzdk54x4_A16W8_QDQ_XCLBIN_PATH;
  } else {
    throw std::invalid_argument("model_name is not supported");
  }

  kernel_x_shape_[0] = input_shape.at(0);
  kernel_x_shape_[1] = input_shape.at(1);
  kernel_x_shape_[2] = input_shape.at(2);

  xrt_ctx_ = dynamic_dispatch::xrt_context::get_instance(XCLBIN_FNAME);
  std::call_once(instr_reg_flag_, [this]() { setup_instr_registry(); });
}

std::vector<int32_t>
calculate_qdq_param(const std::vector<Tensor> &const_params,
                    int64_t q_shape_back, int64_t k_shape_back) {
  float q_sc = OpsFusion::get_tensor_as_float_vec(const_params.at(0))[0];
  uint16_t q_zp = OpsFusion::get_tensor_as_uint16_t_vec(const_params.at(1))[0];

  float k_sc = OpsFusion::get_tensor_as_float_vec(const_params.at(2))[0];
  uint16_t k_zp = OpsFusion::get_tensor_as_uint16_t_vec(const_params.at(3))[0];

  float v_sc = OpsFusion::get_tensor_as_float_vec(const_params.at(4))[0];
  uint16_t v_zp = OpsFusion::get_tensor_as_uint16_t_vec(const_params.at(5))[0];

  float qkt_sc = OpsFusion::get_tensor_as_float_vec(const_params.at(6))[0];
  uint16_t qkt_zp =
      OpsFusion::get_tensor_as_uint16_t_vec(const_params.at(7))[0];

  float sm_sc = OpsFusion::get_tensor_as_float_vec(const_params.at(8))[0];
  uint16_t sm_zp = OpsFusion::get_tensor_as_uint16_t_vec(const_params.at(9))[0];

  float vsm_sc = OpsFusion::get_tensor_as_float_vec(const_params.at(10))[0];
  uint16_t vsm_zp =
      OpsFusion::get_tensor_as_uint16_t_vec(const_params.at(11))[0];

  float mul_sc = OpsFusion::get_tensor_as_float_vec(const_params.at(13))[0];
  uint16_t mul_zp =
      OpsFusion::get_tensor_as_uint16_t_vec(const_params.at(14))[0];

  float mul_w = (static_cast<float>(OpsFusion::get_tensor_as_int64_t_vec(
                     const_params.at(12))[0]) -
                 mul_zp) *
                mul_sc;

  auto coeff_qkt = OpsFusion::coeffs::qdq_act_matmul_uint16_uint16_cstm(
      q_sc, q_zp, q_shape_back, k_sc, k_zp, qkt_sc, qkt_zp);

  auto coeff_smv = OpsFusion::coeffs::qdq_act_matmul_uint16_uint16_cstm(
      sm_sc, sm_zp, k_shape_back, v_sc, v_zp, vsm_sc, vsm_zp);

  std::vector<int32_t> qdq_params =
      OpsFusion::coeffs::mha_channel_qdq_params_fill( // in32_t * 96
          coeff_qkt, coeff_smv,
          std::make_tuple(OpsFusion::coeffs::float_to_bfloat16(qkt_sc * mul_w *
                                                               1.442695041f),
                          (int)qkt_zp),
          std::make_tuple(OpsFusion::coeffs::float_to_bfloat16(1.0f / sm_sc),
                          (int)sm_zp),
          std::make_tuple(0, 0), std::make_tuple(0, 0), 1, 0);

  return qdq_params;
}

template <typename InT, typename WtT, typename OutT>
void mhamzdk5<InT, WtT, OutT>::initialize_const_params(
    ConstBufferIO &io, const std::vector<Tensor> &const_params,
    const std::map<std::string, std::any> &attr) {
  RYZENAI_LOG_TRACE("mhamzdk5 initialize_const_params(ptr) ...");
  DD_THROW_IF(
      !is_generic_pass_in_onnx && ((const_params.size() != 1) ||
                                   (const_params.at(0).shape.size() != 2)),
      OpsFusion::dd_format(
          "Unsupported const spec for mhamzdk5\n"
          "(Details : #const params == 1 ({}), Const param1 dim == 2 ({})",
          const_params.size(), const_params.at(0).shape.size()));
  const int qdq_idx = 0;

  int32_t *qdq_param = nullptr;
  std::vector<int32_t> qdq_param_vec;
  if (is_generic_pass_in_onnx) {
    qdq_param_vec =
        calculate_qdq_param(const_params, q_shape_back, k_shape_back);
    qdq_param = (int32_t *)qdq_param_vec.data();
  } else {
    qdq_param = (int32_t *)const_params.at(qdq_idx).data;
  }

  int size_qdqparam = QDQparam_size * num_qdq_nodes * sizeof(int32_t);

  qdq_param[(16 * 2) + qdq_Mv_idx] = mha_mzdk5_sq;
  qdq_param[(16 * 2) + qdq_Nv_idx] = mha_mzdk5_st_pad;

  qdq_param[(16 * 3) + qdq_Mv_idx] = mha_mzdk5_sq;
  qdq_param[(16 * 3) + qdq_Nv_idx] = mha_mzdk5_val_subv_cols;

  io.write(0, (void *)qdq_param, size_qdqparam);

  RYZENAI_LOG_TRACE("mhamzdk5 initialize_const_params(ptr) ... DONE");
}

// For mhamzdk5
template <typename InT, typename WtT, typename OutT>
void mhamzdk5<InT, WtT, OutT>::initialize_const_params(
    const std::vector<Tensor> &const_params,
    const std::map<std::string, std::any> &attr) {
  // Check the number of inputs
  size_t expected_const_params_size = is_generic_pass_in_onnx ? 15 : 1;
  DD_ASSERT(
      (!is_generic_pass_in_onnx &&
       const_params.size() == expected_const_params_size),
      OpsFusion::dd_format("mhamzdk5 got unexpected number of const params {}",
                           const_params.size()));

  int size_qdqparam = QDQparam_size * num_qdq_nodes * sizeof(int32_t);

  // Create input/output BOs
  const size_t A_BO_SIZE = ((kernel_x_shape_[0] * kernel_x_shape_[2] +
                             2 * kernel_x_shape_[1] * kernel_x_shape_[2]) *
                            a_dtype_size_);
  const size_t B_BO_SIZE = size_qdqparam;
  const size_t C_BO_SIZE =
      (kernel_x_shape_[0] * kernel_x_shape_[2] * c_dtype_size_);

  RYZENAI_LOG_TRACE("mhamzdk5: A_BO_SIZE:" + std::to_string(A_BO_SIZE) +
                    " B_BO_SIZE:" + std::to_string(B_BO_SIZE) +
                    " C_BO_SIZE size:" + std::to_string(C_BO_SIZE));

  a_bo_ = xrt::bo(xrt_ctx_->get_device(), A_BO_SIZE, XRT_BO_FLAGS_HOST_ONLY,
                  xrt_ctx_->get_kernel().group_id(8));
  b_bo_ = xrt::bo(xrt_ctx_->get_device(), B_BO_SIZE, XRT_BO_FLAGS_HOST_ONLY,
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
                                     kernel_x_shape_[2]};
  if (is_ctrl_pkt_) {
    // Based on the mapped_shape to get the meta json file
    std::vector<uint8_t> json_data;
    try {
      auto json_key =
          get_instr_key(param_fname_prefix_, param_shape) + "_ctrl_meta";
      Transaction &txn = Transaction::getInstance();
      json_data = txn.get_txn_bvec(json_key);
    } catch (...) {
      is_ctrl_pkt_ = 0;
    }

    if (is_ctrl_pkt_) {
      std::cout << "ctrlpkt patching" << std::endl;
      RYZENAI_LOG_TRACE("mhamzdk5 patch ctrlpkt ... START");
      // get param_bo address
      auto param_bo_key =
          get_instr_key(param_fname_prefix_, param_shape) + "_param";
      const xrt::bo &param_bo =
          xrt_ctx_->get_registry().get_param_bo(param_bo_key).second;

      // Get ctrl pkt patch info from json
      std::vector<CtrlPktPatchInfo> ctrlpkt_info;
      ctrlpkt_info = json_str_to_ctrlpkt_patch_info(json_data);

      // Get the ctrl pkt
      auto ctrl_bo_key =
          get_instr_key(param_fname_prefix_, param_shape) + "_ctrl";
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
      RYZENAI_LOG_TRACE("mhamzdk5 patch ctrlpkt ... DONE");
    }
  }
  RYZENAI_LOG_TRACE("mhamzdk5 initialize_const_params ... DONE");
}

template <typename InT, typename WtT, typename OutT>
void mhamzdk5<InT, WtT, OutT>::execute(std::vector<Tensor> &input,
                                       std::vector<Tensor> &output) {
  // Check the number of inputs
  if (input.size() != 3) {
    throw std::runtime_error("mhamzdk5 IPU Wrapper expect to have one input.");
  }

  const int q_idx = 0, k_idx = 1, v_idx = 2;
  // The first data is Query
  InT *a = (InT *)input.at(q_idx).data;
  // The second data is Key
  InT *key = (InT *)input.at(k_idx).data;
  // The third data is Val
  InT *val = (InT *)input.at(v_idx).data;

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
  size_t M, K, N;

  M = input.at(q_idx).shape.at(0);
  K = input.at(k_idx).shape.at(0);
  N = input.at(v_idx).shape.at(1);

  a_shape_[0] = input.at(q_idx).shape.at(0);
  a_shape_[1] = input.at(q_idx).shape.at(1);

  int64_t key_shape_[2], val_shape_[2];
  key_shape_[0] = input.at(k_idx).shape.at(0);
  key_shape_[1] = input.at(k_idx).shape.at(1);
  val_shape_[0] = input.at(v_idx).shape.at(0);
  val_shape_[1] = input.at(v_idx).shape.at(1);
  c_shape_[0] = a_shape_[0];
  c_shape_[1] = a_shape_[1];

  //  a_bo copy
  auto a_copy_start = GET_ELAPSED_TIME_NS();
  InT *a_bo_map = a_bo_.map<InT *>();
  size_t a_size = a_shape_[0] * a_shape_[1] * sizeof(InT);
  memcpy((void *)a_bo_map, (void *)a, a_size);
  size_t key_size = key_shape_[0] * key_shape_[1] * sizeof(InT);
  size_t val_size = val_shape_[0] * val_shape_[1] * sizeof(InT);
  void *aie_key =
      static_cast<void *>((reinterpret_cast<int8_t *>(a_bo_map)) + a_size);
  void *aie_val =
      static_cast<void *>((reinterpret_cast<int8_t *>(aie_key)) + key_size);
  memcpy((void *)aie_key, (void *)key, key_size);
  memcpy((void *)aie_val, (void *)val, val_size);

  auto a_copy_stop = GET_ELAPSED_TIME_NS();

  // a_bo sync
  auto a_sync_start = GET_ELAPSED_TIME_NS();
  a_bo_.sync(XCL_BO_SYNC_BO_TO_DEVICE);
  auto a_sync_stop = GET_ELAPSED_TIME_NS();

  a_copy_time_ = static_cast<int64_t>(a_copy_stop - a_copy_start);
  a_sync_time_ = static_cast<int64_t>(a_sync_stop - a_sync_start);

  w_shape_[0] = a_shape_[0];
  w_shape_[1] = a_shape_[1];
  // prepare inst_bo and param_bo
  std::vector<size_t> param_shape = {M, K, N};
  auto instr_bo_key = get_instr_key(txn_fname_prefix_, param_shape);
  auto param_bo_key =
      get_instr_key(param_fname_prefix_, param_shape) + "_param";

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
      std::to_string(mhamzdk5_id_) + " " + std::to_string(a_shape_[0]) + " " +
      std::to_string(a_shape_[1]) + " " + std::to_string(w_shape_[1]) + " " +
      std::to_string(kernel_x_rows) + " " + std::to_string(kernel_x_shape_[1]) +
      " " + std::to_string(kernel_y_shape_[1]) + " " +
      std::to_string(exec_end - exec_start) + " " +
      std::to_string(num_run_aie_) + " " + std::to_string(run_aie_time_) + " " +
      std::to_string(a_copy_time_) + " " + std::to_string(a_sync_time_) + " " +
      std::to_string(c_copy_time_) + " " + std::to_string(c_sync_time_) + " " +
      std::to_string((double)run_aie_time_ / num_run_aie_) + "\n");
}

template <typename InT, typename WtT, typename OutT>
const std::vector<uint8_t> mhamzdk5<InT, WtT, OutT>::get_transaction_bin(
    std::vector<Tensor> &input, std::vector<Tensor> &output,
    const std::map<std::string, std::any> &attr) const {

  auto Q_shape = extract_shape(input.at(0));
  auto K_shape = extract_shape(input.at(1));
  auto V_shape = extract_shape(input.at(2));

  std::vector<size_t> param_shape = {Q_shape[0], K_shape[0], V_shape[1]};
  std::string txn_key = get_instr_key(txn_fname_prefix_, param_shape);
  Transaction &txn = Transaction::getInstance();
  return txn.get_txn_bvec(txn_key);
}

template <typename InT, typename WtT, typename OutT>
const std::vector<uint8_t> mhamzdk5<InT, WtT, OutT>::get_super_kernel_params(
    std::vector<Tensor> &input, std::vector<Tensor> &output,
    const std::map<std::string, std::any> &attr) const {

  auto Q_shape = extract_shape(input.at(0));
  auto K_shape = extract_shape(input.at(1));
  auto V_shape = extract_shape(input.at(2));

  std::vector<size_t> param_shape = {Q_shape[0], K_shape[0], V_shape[1]};
  std::string param_key =
      get_instr_key(param_fname_prefix_, param_shape) + "_param";
  // std::cout << "Super kernel params name : " << fname << std::endl;

  Transaction &txn = Transaction::getInstance();
  return txn.get_txn_bvec(param_key);
}

template <typename InT, typename WtT, typename OutT>
std::vector<uint8_t> mhamzdk5<InT, WtT, OutT>::get_ctrl_pkts(
    std::vector<Tensor> &input, std::vector<Tensor> &output,
    const std::map<std::string, std::any> &attr) const {
  auto Q_shape = extract_shape(input.at(0));
  auto K_shape = extract_shape(input.at(1));
  auto V_shape = extract_shape(input.at(2));

  std::vector<size_t> param_shape = {Q_shape[0], K_shape[0], V_shape[1]};
  // TODO: Add check to validate tensor shapes
  std::string ctrl_key =
      get_instr_key(param_fname_prefix_, param_shape) + "_ctrl";
  try {
    Transaction &txn = Transaction::getInstance();
    return txn.get_txn_bvec(ctrl_key);
  } catch (...) {
    return {};
  }
}

template <typename InT, typename WtT, typename OutT>
std::vector<CtrlPktPatchInfo> mhamzdk5<InT, WtT, OutT>::get_ctrl_pkt_patch_info(
    std::vector<Tensor> &input, std::vector<Tensor> &output,
    const std::map<std::string, std::any> &attr) const {
  auto Q_shape = extract_shape(input.at(0));
  auto K_shape = extract_shape(input.at(1));
  auto V_shape = extract_shape(input.at(2));

  std::vector<size_t> param_shape = {Q_shape[0], K_shape[0], V_shape[1]};
  // TODO: Add check to validate tensor shapes
  try {
    auto ctrl_pkt_meta =
        get_instr_key(param_fname_prefix_, param_shape) + "_ctrl_meta";
    Transaction &txn = Transaction::getInstance();
    return json_str_to_ctrlpkt_patch_info(txn.get_txn_bvec(ctrl_pkt_meta));
  } catch (...) {
    // throw std::runtime_error("mha : Can not file the ctrl_meta.json file");
    return {};
  }
}

template <typename InT, typename WtT, typename OutT>
std::vector<OpArgMap> mhamzdk5<InT, WtT, OutT>::get_buffer_reqs(
    std::vector<Tensor> &input, std::vector<Tensor> &output,
    const std::map<std::string, std::any> &attr) const {
  // [QKV, qdq_params]
  if (!is_generic_pass_in_onnx && input.size() != 5) {
    throw std::runtime_error("mhamzdk5: Incorrect number of tensors received");
  } else if (is_generic_pass_in_onnx && input.size() != 19) {
    throw std::runtime_error("mhamzdk5: Incorrect number of tensors received");
  }

  size_t out_idx = is_generic_pass_in_onnx ? 18 : 4;

  size_t size_qdqparam = QDQparam_size * num_qdq_nodes * sizeof(int32_t);
  auto Q_shape = extract_shape(input.at(0));
  auto K_shape = extract_shape(input.at(1));
  auto V_shape = extract_shape(input.at(2));
  auto out_shape = extract_shape(input.at(out_idx));

  size_t Q_size = (Q_shape[0] * Q_shape[1] * sizeof(InT));
  size_t K_size = (K_shape[0] * K_shape[1] * sizeof(InT));
  size_t V_size = (V_shape[0] * V_shape[1] * sizeof(InT));

  size_t out_size = (out_shape[0] * out_shape[1] * sizeof(OutT));

  size_t super_kernel_size = get_super_kernel_params(input, output).size();
  size_t ctrl_pkt_size = get_ctrl_pkts(input, output).size();

  std::vector<OpArgMap> arg_map{
      {OpArgMap::OpArgType::INPUT, 1, 0, 0, Q_size},
      {OpArgMap::OpArgType::INPUT, 1, 1, Q_size, K_size},
      {OpArgMap::OpArgType::INPUT, 1, 2, Q_size + K_size, V_size},
      {OpArgMap::OpArgType::CONST_INPUT, 2, 3, 0, size_qdqparam},
      {OpArgMap::OpArgType::OUTPUT, 0, out_idx, 0, out_size},
      {OpArgMap::OpArgType::CONST_KERNEL_PARAM_INPUT, 3, 0, 0,
       super_kernel_size},
      {OpArgMap::OpArgType::CTRL_PKT_BIN, 4, 0, 0, ctrl_pkt_size}};

  return arg_map;
}

template class mhamzdk5<uint16_t, uint8_t, uint16_t>;
} // namespace ryzenai
