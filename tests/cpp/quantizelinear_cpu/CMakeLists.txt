# Copyright © 2024 Advanced Micro Devices, Inc. All rights reserved.

add_executable(test_quantizedlinear_cpu test_fusion.cpp)
dd_configure_test(test_quantizedlinear_cpu OFF)
