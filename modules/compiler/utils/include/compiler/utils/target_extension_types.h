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

#ifndef COMPILER_UTILS_TARGET_EXTENSION_TYPES_H_INCLUDED
#define COMPILER_UTILS_TARGET_EXTENSION_TYPES_H_INCLUDED

namespace compiler {
namespace utils {
namespace tgtext {

/// @brief The indices of the *integer* parameters of a "spirv.Image" type.
enum ImageTyIntParamIdx {
  ImageTyDimensionalityIdx = 0,
  ImageTyDepthIdx,
  ImageTyArrayedIdx,
  ImageTyMSIdx,
  ImageTySampledIdx,
  ImageTyFormatIdx,
  ImageTyAccessQualIdx,
};

/// @brief Values the 'dimensionality' parameter of a "spirv.Image" type may
/// hold.
///
/// Note that not all of these are supported by the compiler.
enum ImageTyDimensionalityParam {
  ImageDim1D = 0,
  ImageDim2D,
  ImageDim3D,
  ImageDimCube,
  ImageDimRect,
  ImageDimBuffer,
  ImageDimSubpassData,
};

/// @brief Values the 'depth' parameter of a "spirv.Image" type may hold.
enum ImageTyDepthParam {
  ImageDepthNone = 0,  // Not a depth image
  ImageDepth,          // A depth image
  ImageDepthUnknown,   // No indication as to whether this is a depth or
                       // non-depth image
};

/// @brief Values the 'arrayed' parameter of a "spirv.Image" type may hold.
enum ImageTyArrayedParam {
  ImageNonArrayed = 0,
  ImageArrayed,
};

/// @brief Values the 'MS' parameter of a "spirv.Image" type may hold.
enum ImageTyMSParam {
  ImageMSSingleSampled = 0,
  ImageMSMultiSampled,
};

/// @brief Values the 'Sampled' parameter of a "spirv.Image" type may hold.
enum ImageTySampledParam {
  ImageSampledRuntime = 0,      // only known at run time
  ImageSampledCompat,           // compatible with sampling operations
  ImageSampledReadWriteCompat,  // compatiable with read/write operations (a
                                // storage or subpass data image)
};

enum ImageTyAccessQualParam {
  ImageAccessQualReadOnly = 0,
  ImageAccessQualWriteOnly,
  ImageAccessQualReadWrite,
};

}  // namespace tgtext
}  // namespace utils
}  // namespace compiler

#endif  // COMPILER_UTILS_TARGET_EXTENSION_TYPES_H_INCLUDED
