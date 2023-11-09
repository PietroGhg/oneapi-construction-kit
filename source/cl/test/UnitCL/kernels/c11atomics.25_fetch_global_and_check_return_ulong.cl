// Copyright (C) Codeplay Software Limited
//
// Licensed under the Apache License, Version 2.0 (the "License") with LLVM
// Exceptions; you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://github.com/codeplaysoftware/oneapi-construction-kit/blob/main/LICENSE.txt
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
// WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
// License for the specific language governing permissions and limitations
// under the License.
//
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
// CL_STD: 3.0
__kernel void fetch_global_and_check_return_ulong(
    volatile __global atomic_ulong *input_buffer,
    __global ulong *output_buffer) {
  uint gid = get_global_id(0);

  output_buffer[gid] = atomic_fetch_and_explicit(
      input_buffer + gid, 0, memory_order_relaxed, memory_scope_work_item);
}
