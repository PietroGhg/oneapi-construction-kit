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

/// @file
///
/// Replaces calls to sub-group builtins with their analagous work-group
/// builtin.

#include <compiler/utils/attributes.h>
#include <compiler/utils/builtin_info.h>
#include <compiler/utils/degenerate_sub_group_pass.h>
#include <compiler/utils/device_info.h>
#include <compiler/utils/group_collective_helpers.h>
#include <compiler/utils/metadata.h>
#include <compiler/utils/pass_functions.h>
#include <llvm/ADT/SmallVector.h>
#include <llvm/ADT/StringMap.h>
#include <llvm/ADT/StringRef.h>
#include <llvm/IR/Function.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/Instructions.h>
#include <llvm/IR/Module.h>
#include <llvm/Support/ErrorHandling.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include <set>

using namespace llvm;

#define DEBUG_TYPE "degenerate-sub-groups"

namespace {
/// @brief Helper for determining if a call instruction calls a sub-group
/// builtin function.
///
/// @param[in] CI Call instruction to query.
///
/// @return True if CI is a call to sub-group builtin, false otherwise.
std::optional<compiler::utils::Builtin> isSubGroupFunction(
    CallInst *CI, compiler::utils::BuiltinInfo &BI) {
  auto *Fcn = CI->getCalledFunction();
  assert(Fcn && "virtual calls are not supported");
  auto SGBuiltin = BI.analyzeBuiltin(*Fcn);

  if (SGBuiltin.ID == compiler::utils::eMuxBuiltinSubGroupBarrier) {
    return SGBuiltin;
  }
  if (auto GroupOp = BI.isMuxGroupCollective(SGBuiltin.ID);
      GroupOp && GroupOp->isSubGroupScope()) {
    return SGBuiltin;
  }

  return std::nullopt;
}

/// @return The work-group equivalent of the given builtin.
Function *lookupWGBuiltin(const compiler::utils::Builtin &SGBuiltin,
                          compiler::utils::BuiltinInfo &BI, Module &M) {
  compiler::utils::BuiltinID WGBuiltinID = compiler::utils::eBuiltinInvalid;
  if (SGBuiltin.ID == compiler::utils::eMuxBuiltinSubGroupBarrier) {
    WGBuiltinID = compiler::utils::eMuxBuiltinWorkGroupBarrier;
  } else {
    auto SGCollective = BI.isMuxGroupCollective(SGBuiltin.ID);
    assert(SGCollective.has_value() && "Not a sub-group builtin");
    auto WGCollective = *SGCollective;
    WGCollective.Scope = compiler::utils::GroupCollective::ScopeKind::WorkGroup;
    WGBuiltinID = BI.getMuxGroupCollective(WGCollective);
  }
  // Not all sub-group builtins have a work-group equivalent.
  if (WGBuiltinID == compiler::utils::eBuiltinInvalid) {
    return nullptr;
  }
  auto *WGBuiltin =
      BI.getOrDeclareMuxBuiltin(WGBuiltinID, M, SGBuiltin.mux_overload_info);
  assert(WGBuiltin && "Missing work-group builtin");

  return WGBuiltin;
}

/// @brief Helper for determining if a call instruction calls a sub-group
/// work-item builtin function.
///
/// @param[in] CI Call instruction to query.
///
/// @return True if CI is a call to sub-group work-item builtin, false
/// otherwise.
bool isSubGroupWorkItemFunction(CallInst *CI,
                                compiler::utils::BuiltinInfo &BI) {
  auto *Fcn = CI->getCalledFunction();
  assert(Fcn && "virtual calls are not supported");
  auto SGBuiltin = BI.analyzeBuiltin(*Fcn);

  switch (SGBuiltin.ID) {
    default:
      return false;
    case compiler::utils::eMuxBuiltinGetSubGroupSize:
    case compiler::utils::eMuxBuiltinGetMaxSubGroupSize:
    case compiler::utils::eMuxBuiltinGetNumSubGroups:
    case compiler::utils::eMuxBuiltinGetSubGroupId:
    case compiler::utils::eMuxBuiltinGetSubGroupLocalId:
      return true;
  }
}

/// @brief Replaces sub-group builtin calls with their work-group equivalents.
///
/// @param[in] CI Builtin call to replace.
/// @param[in] SGBuiltin Builtin to replace
/// @param[in] BI BuiltinInfo
void replaceSubGroupBuiltinCall(CallInst *CI,
                                compiler::utils::Builtin SGBuiltin,
                                compiler::utils::BuiltinInfo &BI) {
  auto *const M = CI->getModule();
  if (SGBuiltin.ID != compiler::utils::eMuxBuiltinSubgroupBroadcast) {
    // We can just forward the argument directly to the
    // work-group builtin for everything except broadcasts.
    SmallVector<Value *, 4> Args;
    if (SGBuiltin.ID != compiler::utils::eMuxBuiltinSubGroupBarrier) {
      // Barrier ID
      Args.push_back(
          ConstantInt::get(IntegerType::get(M->getContext(), 32), 0));
    }
    for (auto &arg : CI->args()) {
      Args.push_back(arg);
    }
    auto *const WorkGroupBuiltinFcn = lookupWGBuiltin(SGBuiltin, BI, *M);
    assert(WorkGroupBuiltinFcn && "Must have work-group equivalent");
    WorkGroupBuiltinFcn->setCallingConv(CI->getCallingConv());
    auto *WGCI = CallInst::Create(WorkGroupBuiltinFcn, Args, "", CI);
    WGCI->setCallingConv(CI->getCallingConv());
    CI->replaceAllUsesWith(WGCI);
    return;
  }
  // Broadcasts don't map particularly well from sub-groups to work-groups.
  // This is because the sub-group broadcast expects an index in the half
  // closed interval [0, get_sub_group_size()), where as the work-group
  // broadcasts expect the index arguments to be in the ranges [0,
  // get_local_size(0)), [0, get_local_size(1)), [0, get_local_size(2)) for
  // the 1D, 2D and 3D overloads respectively. This means that we need to
  // invert the mapping of sub-group local id to the local (x, y, z)
  // coordinates of the enqueue. This amounts to solving get_local_linear_id
  // (since this is the sub-group local id) for x, y and z given ID of a
  // sub-group element: x = ID % get_local_size(0) y = (ID - x) /
  // get_local_size(0) % get_local_size(1) z = (ID - x - y *
  // get_local_size(0) / (get_local_size(0) * get_local_size(1)
  IRBuilder<> Builder{CI};
  auto *const Value = CI->getArgOperand(0);
  auto *const SubGroupElementID = CI->getArgOperand(1);

  auto *const GetLocalSize =
      BI.getOrDeclareMuxBuiltin(compiler::utils::eMuxBuiltinGetLocalSize, *M);
  auto *const LocalSizeX = Builder.CreateIntCast(
      Builder.CreateCall(
          GetLocalSize, ConstantInt::get(Type::getInt32Ty(M->getContext()), 0)),
      SubGroupElementID->getType(), /* isSigned */ false);
  auto *const LocalSizeY = Builder.CreateIntCast(
      Builder.CreateCall(
          GetLocalSize, ConstantInt::get(Type::getInt32Ty(M->getContext()), 1)),
      SubGroupElementID->getType(), /* isSigned */ false);

  auto *X = Builder.CreateURem(SubGroupElementID, LocalSizeX, "x");
  auto *Y = Builder.CreateURem(
      Builder.CreateUDiv(Builder.CreateSub(SubGroupElementID, X), LocalSizeX),
      LocalSizeY, "y");
  auto *Z = Builder.CreateUDiv(
      Builder.CreateSub(SubGroupElementID,
                        Builder.CreateAdd(X, Builder.CreateMul(Y, LocalSizeX))),
      Builder.CreateMul(LocalSizeX, LocalSizeY), "z");

  auto *const SizeType = compiler::utils::getSizeType(*M);
  auto *const WorkGroupBroadcastFcn = lookupWGBuiltin(SGBuiltin, BI, *M);
  assert(WorkGroupBroadcastFcn && "Must have work-group equivalent");
  WorkGroupBroadcastFcn->setCallingConv(CI->getCallingConv());
  // Because sub_group_broadcast takes uint as its index argument but
  // work_group_broadcast takes size_t we potentially need cast here to the
  // native size_t.
  auto *ID = Builder.getInt32(0);
  X = Builder.CreateIntCast(X, SizeType, /* isSigned */ false);
  Y = Builder.CreateIntCast(Y, SizeType, /* isSigned */ false);
  Z = Builder.CreateIntCast(Z, SizeType, /* isSigned */ false);
  auto *const WGCI =
      Builder.CreateCall(WorkGroupBroadcastFcn, {ID, Value, X, Y, Z});
  CI->replaceAllUsesWith(WGCI);
}

/// @brief Replace sub-group work-item builtin calls with suitable values for
/// the degenerate sub-group case.
///
/// @param[in] CI Builtin call to replace
/// @param[in] BI BuiltinInfo
void replaceSubGroupWorkItemBuiltinCall(CallInst *CI,
                                        compiler::utils::BuiltinInfo &BI) {
  const auto CalledFunctionName = CI->getCalledFunction()->getName();
  // Handle __mux_get_sub_group_size, get_sub_group_size &
  // get_max_sub_group_size. The sub-group is the work-group, meaning the
  // sub-group size is the total local size.
  if (CalledFunctionName.contains("sub_group_size")) {
    auto *const M = CI->getModule();
    IRBuilder<> Builder{CI};
    auto *const GetLocalSize =
        BI.getOrDeclareMuxBuiltin(compiler::utils::eMuxBuiltinGetLocalSize, *M);
    GetLocalSize->setCallingConv(CI->getCallingConv());

    Value *TotalLocalSize =
        ConstantInt::get(compiler::utils::getSizeType(*M), 1);
    for (unsigned i = 0; i < 3; ++i) {
      auto *const LocalSize = Builder.CreateCall(
          GetLocalSize, ConstantInt::get(Type::getInt32Ty(M->getContext()), i));
      LocalSize->setCallingConv(CI->getCallingConv());
      TotalLocalSize = Builder.CreateMul(LocalSize, TotalLocalSize);
    }
    TotalLocalSize = Builder.CreateIntCast(TotalLocalSize, CI->getType(),
                                           /* isSigned */ false);
    CI->replaceAllUsesWith(TotalLocalSize);
  } else if (CalledFunctionName.contains("num_sub_groups")) {
    // Handle get_num_sub_groups & get_enqueued_num_sub_groups.
    // The sub-group is the work-group, meaning there is exactly 1 sub-group.
    auto *const One = ConstantInt::get(CI->getType(), 1);
    CI->replaceAllUsesWith(One);
  } else if (CalledFunctionName.contains("get_sub_group_id")) {
    // Handle get_sub_group_id. The sub-group is the work-group, meaning the
    // sub-group id is 0.
    auto *const Zero = ConstantInt::get(CI->getType(), 0);
    CI->replaceAllUsesWith(Zero);
  } else if (CalledFunctionName.contains("get_sub_group_local_id")) {
    // Handle __mux_get_sub_group_local_id and get_sub_group_local_id. The
    // sub-group local id is a unique local id of the work item, here we use
    // get_local_linear_id.
    auto *const M = CI->getModule();
    auto *const GetLocalLinearID = BI.getOrDeclareMuxBuiltin(
        compiler::utils::eMuxBuiltinGetLocalLinearId, *M);
    GetLocalLinearID->setCallingConv(CI->getCallingConv());
    auto *const LocalLinearIDCall =
        CallInst::Create(GetLocalLinearID, {}, "", CI);
    LocalLinearIDCall->setCallingConv(CI->getCallingConv());
    auto *const LocalLinearID = CastInst::CreateIntegerCast(
        LocalLinearIDCall, Type::getInt32Ty(M->getContext()),
        /* isSigned */ false, "", CI);
    CI->replaceAllUsesWith(LocalLinearID);
  } else {
    llvm_unreachable("unhandled sub-group builtin function");
  }
}
}  // namespace

PreservedAnalyses compiler::utils::DegenerateSubGroupPass::run(
    Module &M, ModuleAnalysisManager &AM) {
  SmallVector<Function *, 8> kernels;
  SmallPtrSet<Function *, 8> degenerateKernels;
  SmallPtrSet<Function *, 8> kernelsToClone;
  auto &BI = AM.getResult<BuiltinInfoAnalysis>(M);

  for (auto &F : M) {
    if (isKernelEntryPt(F)) {
      kernels.push_back(&F);

      auto const local_sizes = compiler::utils::getLocalSizeMetadata(F);
      if (!local_sizes) {
        // If we don't know the local size at compile time, we can't guarantee
        // safety of non-degenerate subgroups, so we clone the kernel and defer
        // the decision to the runtime.
        kernelsToClone.insert(&F);
      } else {
        // Otherwise we can check for compatibility with the work group size.
        // If the local size is a power of two, OR a multiple of the maximum
        // vectorization width, we don't need degenerate subgroups. Otherwise,
        // we probably do.
        //
        // Note that this is a conservative approach that doesn't take into
        // account vectorization failures or more involved SIMD width decisions.
        // Degenerate subgroups are ALWAYS safe, so we only want to choose
        // non-degenerate sub-groups when we KNOW they will be safe. Thus it
        // may be the case that the vectorizer can choose a narrower width to
        // avoid the need for degenerate sub-groups, but we can't rely on it,
        // therefore if the local size is not a power of two, we only go by the
        // maximum width supported by the device. TODO DDK-75
        uint32_t const local_size = local_sizes ? (*local_sizes)[0] : 0;
        if (!isPowerOf2_32(local_size)) {
          auto const &DI =
              AM.getResult<compiler::utils::DeviceInfoAnalysis>(*F.getParent());
          auto const max_work_width = DI.max_work_width;
          if (local_size % max_work_width != 0) {
            // Flag the presence of degenerate sub-groups in this kernel.
            // There might not be any sub-group builtins, in which case it's
            // academic.
            setHasDegenerateSubgroups(F);
            degenerateKernels.insert(&F);
          }
        }
      }
    }
  }

  // In order to handle multiple kernels, some of which may require degenerate
  // subgroups, and some which may not, we traverse the Call Graph in both
  // directions:
  //
  //  * We need to know which kernels and functions, directly or indirectly,
  //    make use of subgroup functions, so we start at the subgroup calls and
  //    trace through call instructions down to the kernels.
  //  * We need to know which functions, directly or indirectly, are used by
  //    kernels that do and do not use degenerate subgroups, so we trace through
  //    call instructions from the kernels up to the leaves.
  //
  // We need to clone all functions that are used by both degenerate and
  // non-degenerate subgroup kernels, but only where those functions directly
  // or indirectly make use of subgroups; otherwise, they can be shared by both
  // kinds of kernel.
  SmallVector<Function *, 8> worklist;
  SmallPtrSet<Function *, 8> usesSubgroups;
  // Some sub-group functions have no work-group equivalent (e.g., shuffles).
  // We mark these as 'poisonous' as they poison the call-graph and halt the
  // process of converting any of their transitive users to degenerate
  // sub-groups.
  SmallPtrSet<Function *, 8> poisonList;
  for (auto &F : M) {
    for (auto &BB : F) {
      for (auto &I : BB) {
        if (auto *CI = dyn_cast<CallInst>(&I)) {
          if (auto SGBuiltin = isSubGroupFunction(CI, BI);
              SGBuiltin || isSubGroupWorkItemFunction(CI, BI)) {
            // Only add each function to the worklist once
            if (usesSubgroups.insert(&F).second) {
              worklist.push_back(&F);
            }
            if (SGBuiltin && !lookupWGBuiltin(*SGBuiltin, BI, M)) {
              poisonList.insert(&F);
            }
          }
        }
      }
    }
  }

  // If there were no sub-group builtin calls we are done, exit early and
  // preserve all analysis since we didn't touch the module.
  if (worklist.empty()) {
    for (auto *const K : kernels) {
      // Set the attribute on every kernel that doesn't use any subgroups at
      // all, so the vectorizer knows it can vectorize them however it likes.
      setHasDegenerateSubgroups(*K);
    }
    return PreservedAnalyses::all();
  }

  // Collect all functions that contain subgroup calls, including calls to
  // other functions in the module that contain subgroup calls. Also propagate
  // the poison through the call graph.
  while (!worklist.empty()) {
    auto *const work = worklist.pop_back_val();
    for (auto *const U : work->users()) {
      if (auto *const CI = dyn_cast<CallInst>(U)) {
        auto *const P = CI->getFunction();
        if (usesSubgroups.insert(P).second) {
          worklist.push_back(P);
        }
        if (poisonList.contains(work)) {
          poisonList.insert(P);
        }
      }
    }
  }

  // Categorise the kernels as users of degenerate and/or non-degenerate
  // sub-groups. These are the roots of the call graph traversal that is done
  // afterwards.
  //
  // Note that kernels marked as using degenerate subgroups that don't actually
  // call any subgroup functions (directly or indirectly) don't need to be
  // collected here.
  SmallVector<Function *, 8> nonDegenerateUsers;
  for (auto *const K : kernels) {
    bool const subgroups = usesSubgroups.contains(K);
    if (!subgroups) {
      // Set the attribute on every kernel that doesn't use any subgroups at
      // all, so the vectorizer knows it can vectorize them however it likes.
      setHasDegenerateSubgroups(*K);

      // No need to clone kernels that don't use any subgroup functions.
      kernelsToClone.erase(K);
    }

    // If the kernel transitively uses a sub-group function for which there is
    // no work-group equivalent, we can't clone it and can't mark it as having
    // degenerate sub-groups.
    if (poisonList.contains(K)) {
      LLVM_DEBUG(dbgs() << "Kernel '" << K->getName()
                        << "' uses sub-group builtin with no work-group "
                           "equivalent - skipping\n");
      kernelsToClone.erase(K);
      nonDegenerateUsers.push_back(K);
      continue;
    }

    if (kernelsToClone.contains(K)) {
      // Kernels that are to be cloned count as both degenerate and
      // non-degenerate subgroup users.
      worklist.push_back(K);
      nonDegenerateUsers.push_back(K);
      degenerateKernels.insert(K);
    } else if (!subgroups || degenerateKernels.contains(K)) {
      worklist.push_back(K);
    } else {
      nonDegenerateUsers.push_back(K);
    }
  }

  // Traverse the call graph to collect all functions that get called (directly
  // or indirectly) by degenerate-subgroup using kernels.
  SmallPtrSet<Function *, 8> usedByDegenerate;
  while (!worklist.empty()) {
    auto *const work = worklist.pop_back_val();
    for (auto &BB : *work) {
      for (auto &I : BB) {
        if (auto *const CI = dyn_cast<CallInst>(&I)) {
          auto *const callee = CI->getCalledFunction();
          if (callee && !callee->empty() && usesSubgroups.contains(callee) &&
              usedByDegenerate.insert(callee).second) {
            worklist.push_back(callee);
          }
        }
      }
    }
  }

  // Traverse the call graph to collect all functions that get called (directly
  // or indirectly) by non-degenerate-subgroup using kernels.
  worklist.assign(nonDegenerateUsers.begin(), nonDegenerateUsers.end());
  SmallPtrSet<Function *, 8> usedByNonDegenerate;
  while (!worklist.empty()) {
    auto *const work = worklist.pop_back_val();
    for (auto &BB : *work) {
      for (auto &I : BB) {
        if (auto *const CI = dyn_cast<CallInst>(&I)) {
          auto *const callee = CI->getCalledFunction();
          if (callee && !callee->empty() && usesSubgroups.contains(callee) &&
              usedByNonDegenerate.insert(callee).second) {
            worklist.push_back(callee);
          }
        }
      }
    }
  }

  // Clone all functions used by both degenerate and non-degenerate subgroup
  // kernels
  SmallVector<Function *, 8> functionsToClone(kernelsToClone.begin(),
                                              kernelsToClone.end());
  for (auto &F : M) {
    if (!F.empty() && usedByDegenerate.contains(&F) &&
        usedByNonDegenerate.contains(&F)) {
      functionsToClone.push_back(&F);
    }
  }

  // First clone all the function declarations and insert them into the VMap.
  // This allows us to automatically update all non-degenerate function calls
  // to degenerate function calls while we clone.
  ValueToValueMapTy VMap;
  for (auto *const F : functionsToClone) {
    // Create our new function, using the linkage from the old one
    // Note - we don't have to copy attributes or metadata over, as
    // CloneFunctionInto does that for us.
    auto *const NewF =
        Function::Create(F->getFunctionType(), F->getLinkage(), "", &M);
    NewF->setCallingConv(F->getCallingConv());

    auto baseName = getOrSetBaseFnName(*NewF, *F);
    NewF->setName(baseName + ".degenerate-subgroups");
    VMap[F] = NewF;
  }

  // Clone the function bodies
  for (auto *const F : functionsToClone) {
    auto Mapped = VMap.find(F);
    assert(Mapped != VMap.end());
    Function *const NewF = cast<Function>(Mapped->second);
    assert(NewF && "Missing cloned function");
    // Scrub any old subprogram - CloneFunctionInto will create a new one for us
    if (auto *const SP = F->getSubprogram()) {
      NewF->setSubprogram(nullptr);
    }

    // Map all original function arguments to the new function arguments
    for (auto it : zip(F->args(), NewF->args())) {
      auto *const OldA = &std::get<0>(it);
      auto *const NewA = &std::get<1>(it);
      VMap[OldA] = NewA;
      NewA->setName(OldA->getName());
    }

    StringRef BaseName = getBaseFnNameOrFnName(*F);

    auto const ChangeType = CloneFunctionChangeType::LocalChangesOnly;
    SmallVector<ReturnInst *, 1> Returns;
    CloneFunctionInto(NewF, F, VMap, ChangeType, Returns);

    // Set the base name on the new cloned kernel to preserve its lineage.
    if (!BaseName.empty()) {
      setBaseFnName(*NewF, BaseName);
    }

    // If we just cloned a kernel, the original now has degenerate subgroups.
    if (isKernel(*F)) {
      setHasDegenerateSubgroups(*NewF);
    }
  }

  // The degenerate functions/kernels are still using non-degenerate subgroup
  // functions, so we must collect subgroup builtin calls and replace them. Not
  // all degenerate functions were cloned - some were updated in-place, so we
  // must be careful about which functions we're updating.
  SmallVector<Instruction *> toDelete;
  worklist.assign(degenerateKernels.begin(), degenerateKernels.end());
  worklist.append(usedByDegenerate.begin(), usedByDegenerate.end());
  for (auto *const F : worklist) {
    // Assume we'll update this function in place. If it's in the VMap then the
    // degenerate version is the cloned version.
    auto *ReplaceF = F;
    if (auto Mapped = VMap.find(F); Mapped != VMap.end()) {
      ReplaceF = cast<Function>(Mapped->second);
    }
    assert(ReplaceF && "Missing function");
    for (auto &BB : *ReplaceF) {
      for (auto &I : BB) {
        if (auto *CI = dyn_cast<CallInst>(&I)) {
          if (auto SGBuiltin = isSubGroupFunction(CI, BI)) {
            // Replace the sub-group function builtin calls with work-group
            // builtin calls inside the degenerately cloned functions.
            replaceSubGroupBuiltinCall(CI, *SGBuiltin, BI);
            toDelete.push_back(CI);
          } else if (isSubGroupWorkItemFunction(CI, BI)) {
            // Replace the sub-group work-item builtin calls with work-group
            // work-item builtin calls inside the degenerately cloned functions.
            replaceSubGroupWorkItemBuiltinCall(CI, BI);
            toDelete.push_back(CI);
          }
        }
      }
    }
  }

  // Remove the old instructions from the module.
  for (auto *I : toDelete) {
    I->eraseFromParent();
  }

  // If we got this far then we changed something, maybe this is too
  // conservative, but assume we invalidated all analyses.
  return PreservedAnalyses::none();
}
