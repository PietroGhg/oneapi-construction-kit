; Copyright (C) Codeplay Software Limited
;
; Licensed under the Apache License, Version 2.0 (the "License") with LLVM
; Exceptions; you may not use this file except in compliance with the License.
; You may obtain a copy of the License at
;
;     https://github.com/codeplaysoftware/oneapi-construction-kit/blob/main/LICENSE.txt
;
; Unless required by applicable law or agreed to in writing, software
; distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
; WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. See the
; License for the specific language governing permissions and limitations
; under the License.
;
; SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

; RUN: muxc --passes work-item-loops,verify -S %s | FileCheck %s --check-prefix=CHECK1
; RUN: muxc --passes work-item-loops,verify -S %s | FileCheck %s --check-prefix=CHECK2

target triple = "spir64-unknown-unknown"
target datalayout = "e-i64:64-v16:16-v24:32-v32:32-v48:64-v96:128-v192:256-v256:256-v512:512-v1024:1024"

; Check that we handle correctly the reqd_work_group_size metadata node
; even when it has less than three operands

define void @foo1() #0 !reqd_work_group_size !1 {
entry:
  %call1 = tail call i64 @__mux_get_local_id(i32 0)
  ret void
}

; we expect to see a comparison to 30 (from the reqd_work_group_size), and two comparisons
; to 1 (default value used when less than three operands are provided)
; CHECK1: %{{.*}} = icmp ult i64 %{{.*}}, 30
; CHECK1: %{{.*}} = icmp ult i64 %{{.*}}, 1
; CHECK1: %{{.*}} = icmp ult i64 %{{.*}}, 1

define void @foo2() #0 !reqd_work_group_size !2 {
entry:
  %call1 = tail call i64 @__mux_get_local_id(i32 0)
  %call2 = tail call i64 @__mux_get_local_id(i32 1)
  ret void
}

; CHECK2: %{{.*}} = icmp ult i64 %{{.*}}, 20
; CHECK2: %{{.*}} = icmp ult i64 %{{.*}}, 10
; CHECK2: %{{.*}} = icmp ult i64 %{{.*}}, 1


declare i64 @__mux_get_local_id(i32)

attributes #0 = { "mux-kernel"="entry-point" }
!1 = !{i32 30}
!2 = !{i32 20, i32 10}
