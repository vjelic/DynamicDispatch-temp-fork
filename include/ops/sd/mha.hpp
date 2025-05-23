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

#pragma once

#include <ops/op_interface.hpp>
#include <ops/ops_common.hpp>

namespace ryzenai {
// stable diffusion 1.5
namespace sd {
template <typename InT, typename WtT, typename OutT>
class mha : public OpInterface {
private:
  std::map<std::string, std::string> txnbin_a_header;
  std::map<std::string, std::string> txnbin_b_header;
  std::map<std::string, std::string> txnbin_acc_header;
  std::map<std::string, std::vector<std::vector<size_t>>> default_shapes_;
  uint64_t B_; // batch, SD1.5 vae B = 1, unet B = 2
  // query shape: (B, M, K)
  // key shape:   (B, N, K)
  // value shape: (B, N, K)
  uint64_t M_;
  uint64_t K_; // K = heads * head_dim
  uint64_t N_;
  uint64_t H_; // heads, SD1.5 vae H = 1, unet H = 8
  size_t q_size_;
  size_t k_size_;
  size_t v_size_;
  size_t out_size_;
  size_t scratch_size_;
  const size_t min_mask_size_ = 128;
  size_t mask_size_;
  static std::once_flag instr_reg_flag_;
  xrt::bo qkv_bo_;
  xrt::bo const_mask_bo_;
  xrt::bo scratch_bo_;
  xrt::bo out_bo_;
  /* size for input activation dtype*/
  int a_dtype_size_;
  /* size for weights dtype*/
  int b_dtype_size_;
  /* size for output activation dtype*/
  int c_dtype_size_;
  /* variables to store profile data */
  int64_t run_aie_time_ = 0;
  int64_t cpu_acc_time_ = 0;
  int64_t num_run_aie_ = 0;
  uint64_t num_execute_ = 0;
  static std::once_flag logger_flag_;
  uint64_t mha_id_ = 0;
  static uint64_t mha_count;
  /* debug flag */
  bool debug_ = false;
  /*xclbin and mc_code selection variables*/
  std::string a_dtype_;
  std::string b_dtype_;
  std::string c_dtype_;
  std::string txn_fname_prefix_;
  std::string XCLBIN_FNAME_;
  std::string pdi_name_;

  void setup_instr_registry();
  std::string get_instr_key(std::string prefix,
                            const std::vector<size_t> &mat) const;

public:
  mha(const std::string &a_dtype, const std::string &b_dtype,
      const std::string &c_dtype, bool load_xrt,
      const std::map<std::string, std::any> &attr = {});
  void initialize_const_params(
      ConstBufferIO &io, const std::vector<Tensor> &const_params,
      const std::map<std::string, std::any> &attr = {}) override;
  void initialize_const_params(
      const std::vector<Tensor> &const_params,
      const std::map<std::string, std::any> &attr = {}) override;
  void execute(std::vector<Tensor> &input,
               std::vector<Tensor> &output) override;
  void debug(bool enable);
  void set_params(const std::string &xclbin, const std::string &pdi_name);
  const std::vector<uint8_t> get_transaction_bin(
      std::vector<Tensor> &input, std::vector<Tensor> &output,
      const std::map<std::string, std::any> &attr = {}) const override;
  const std::vector<uint8_t> get_transaction_bin() const;
  const std::vector<uint8_t> get_super_kernel_params(
      std::vector<Tensor> &input, std::vector<Tensor> &output,
      const std::map<std::string, std::any> &attr = {}) const override {
    return {};
  };
  std::vector<OpArgMap> get_buffer_reqs(
      std::vector<Tensor> &input, std::vector<Tensor> &output,
      const std::map<std::string, std::any> &attr = {}) const override;
};
} // namespace sd
} // namespace ryzenai
