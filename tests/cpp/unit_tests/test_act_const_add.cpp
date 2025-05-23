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

#include <fstream>
#include <gtest/gtest.h>
#include <iostream>

#include "enable_perf.hpp"
#include "ops/ops_common/help_file.hpp"
#include "ops/ops_common/matmul_matrix.hpp"
#include <ops/act_const_add/act_const_add.hpp>

#include "test_common.hpp"
#define RANDOM_DATA
using namespace matmul_matrix;
using namespace std;

template <typename InT = int8_t, typename WgT = int8_t, typename OutT = int8_t>
int test_act_const_add(int M, int N, bool debug = false,
                       const std::string &a_dtype = "int16",
                       const std::string &w_dtype = "int16",
                       const std::string &c_dtype = "int16",
                       const std::string &model_name = "mdsqr") {

  int err_count = 0;
  size_t Ms = static_cast<size_t>(M);
  size_t Ns = static_cast<size_t>(N);
  std::vector<size_t> a_shape = {1, Ms, Ns};
  std::vector<size_t> w_shape = {1, Ms, Ns};
  std::vector<size_t> aie_out_shape = {1, Ms, Ns};

  std::vector<InT> a(M * N);
  std::vector<InT> w(M * N);
  std::vector<OutT> cpu_out(M * N);
  std::vector<OutT> aie_out(M * N);

  RowMajorMatrix<OutT> cpu_Y(M, N, cpu_out.data());
  RowMajorMatrix<OutT> aie_Y(M, N, aie_out.data());

#ifdef RANDOM_DATA
  srand(0xABCD);
  initialize_random_bfloat16(a, M * N, -2, 2);
  initialize_random_bfloat16(w, M * N, -2, 2);

  for (int r = 0; r < M; ++r) {
    for (int c = 0; c < N; ++c) {
      cpu_Y.at(r, c) = float_to_bfloat16(bfloat16_to_float(a[r * N + c]) +
                                         bfloat16_to_float(w[r * N + c]));
    }
  }

#else
  std::string fld_name;
  fld_name = "//bins//ConstMatAdd//";

  if (model_name == "mtea0a" && M == 77 && N == 1024) {
    fld_name = "//bins//ConstMatAdd//77_1024//";
  }

  read_bin_file(OpInterface::get_dd_base_dir() + fld_name + "ifm.bin",
                reinterpret_cast<char *>(a.data()));

  read_bin_file(OpInterface::get_dd_base_dir() + fld_name + "wgt.bin",
                reinterpret_cast<char *>(w.data()));

  read_bin_file(OpInterface::get_dd_base_dir() + fld_name + "ofm.bin",
                reinterpret_cast<char *>(cpu_out.data()));
#endif

  // run aie
  std::map<std::string, std::any> attr;

  ryzenai::act_const_add act_const_add_ =
      ryzenai::act_const_add<InT, WgT, OutT>(a_dtype, w_dtype, c_dtype, false,
                                             attr);

  act_const_add_.debug(debug);
  act_const_add_.set_params(model_name, a_shape);

  std::vector<Tensor> const_Tensor;

  const_Tensor = {{w.data(), w_shape, w_dtype}};

  act_const_add_.initialize_const_params(const_Tensor);

  std::vector<Tensor> input_Tensor;
  input_Tensor = {{a.data(), a_shape, a_dtype}};

  std::vector<Tensor> output_Tensor;
  output_Tensor = {{aie_out.data(), aie_out_shape, c_dtype}};

#ifdef UNIT_TEST_PERF
  LOG_THIS("M = " << M << ", N = " << N);
  PROFILE_THIS(act_const_add_.execute(input_Tensor, output_Tensor));
#else
  act_const_add_.execute(input_Tensor, output_Tensor);
#endif

  // compare results
  err_count = check_result_bfloat(cpu_Y, aie_Y, 0.01);

  return err_count;
}

TEST(START_TAIL_mxpzi_act_const_add_Testa16, Kernel1) {
  int err_count = test_act_const_add<uint16_t, uint16_t, uint16_t>(
      77, 768, false, "bfloat16", "bfloat16", "bfloat16", "START_TAIL_PS");
  EXPECT_TRUE(err_count == 0) << "Error Count = " << err_count;
}

TEST(mtea0a_act_const_add_Testabf16wbf16accbf16, Kernel_128_1024) {
  int err_count = test_act_const_add<uint16_t, uint16_t, uint16_t>(
      77, 1024, false, "bfloat16", "bfloat16", "bfloat16", "mtea0a");
  EXPECT_TRUE(err_count == 0) << "Error Count = " << err_count;
}
