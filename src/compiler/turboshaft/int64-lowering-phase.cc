// Copyright 2023 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/compiler/turboshaft/int64-lowering-phase.h"

#if V8_TARGET_ARCH_32_BIT
#include "src/compiler/turboshaft/int64-lowering-reducer.h"
#include "src/compiler/turboshaft/optimization-phase.h"
#endif

namespace v8::internal::compiler::turboshaft {

void Int64LoweringPhase::Run(Zone* temp_zone) {
#if V8_TARGET_ARCH_32_BIT
  turboshaft::OptimizationPhase<turboshaft::Int64LoweringReducer>::Run(
      temp_zone);
#else
  UNREACHABLE();
#endif
}

}  // namespace v8::internal::compiler::turboshaft
