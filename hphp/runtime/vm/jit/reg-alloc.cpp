/*
   +----------------------------------------------------------------------+
   | HipHop for PHP                                                       |
   +----------------------------------------------------------------------+
   | Copyright (c) 2010-2014 Facebook, Inc. (http://www.facebook.com)     |
   +----------------------------------------------------------------------+
   | This source file is subject to version 3.01 of the PHP license,      |
   | that is bundled with this package in the file LICENSE, and is        |
   | available through the world-wide-web at the following url:           |
   | http://www.php.net/license/3_01.txt                                  |
   | If you did not receive a copy of the PHP license and are unable to   |
   | obtain it through the world-wide-web, please send a note to          |
   | license@php.net so we can mail you a copy immediately.               |
   +----------------------------------------------------------------------+
*/

#include "hphp/runtime/vm/jit/reg-alloc.h"
#include "hphp/runtime/vm/jit/native-calls.h"
#include "hphp/runtime/vm/jit/mc-generator.h"

namespace HPHP { namespace JIT {

using namespace JIT::reg;
using NativeCalls::CallMap;

TRACE_SET_MOD(hhir);

PhysReg forceAlloc(const SSATmp& tmp) {
  auto inst = tmp.inst();
  auto opc = inst->op();

  // Note that the point of StashResumableSP is to save a StkPtr
  // somewhere other than rVmSp.  (TODO(#2288359): make rbx not
  // special.)
  if (opc != StashResumableSP && tmp.isA(Type::StkPtr)) {
    assert(opc == DefSP ||
           opc == ReDefSP ||
           opc == ReDefResumableSP ||
           opc == PassSP ||
           opc == DefInlineSP ||
           opc == Call ||
           opc == CallArray ||
           opc == SpillStack ||
           opc == SpillFrame ||
           opc == CufIterSpillFrame ||
           opc == ExceptionBarrier ||
           opc == RetAdjustStack ||
           opc == InterpOne ||
           opc == InterpOneCF ||
           opc == CheckStk ||
           opc == GuardStk ||
           opc == AssertStk ||
           opc == CastStk ||
           opc == CoerceStk ||
           opc == SideExitGuardStk  ||
           MInstrEffects::supported(opc));
    return mcg->backEnd().rVmSp();
  }

  // LdContActRec and LdAFWHActRec, loading a generator's AR, is the only time
  // we have a pointer to an AR that is not in rVmFp.
  if (opc != LdContActRec && opc != LdAFWHActRec && tmp.isA(Type::FramePtr)) {
    return mcg->backEnd().rVmFp();
  }

  if (opc == DefMIStateBase) {
    assert(tmp.isA(Type::PtrToCell));
    return mcg->backEnd().rSp();
  }
  return InvalidReg;
}

namespace {
// This implements an array of arrays of bools, one for each declared
// source operand of each instruction.  True means the operand must
// be a const; i.e. it was declared with C(T) instead of S(T).
struct ConstSrcTable {
  auto static constexpr MaxSrc = 8;
  bool table[kNumOpcodes][MaxSrc];
  ConstSrcTable() {
    int op = 0;
    int i;

#define NA
#define S(...)   i++;
#define C(type)  table[op][i++] = true;
#define CStr     table[op][i++] = true;
#define SNumInt  i++;
#define SNum     i++;
#define SUnk     i++;
#define SSpills
#define O(opcode, dstinfo, srcinfo, flags) \
    i = 0; \
    srcinfo \
    op++;

    IR_OPCODES

#undef O
#undef NA
#undef SAny
#undef S
#undef C
#undef CStr
#undef SNum
#undef SUnk
#undef SSpills

  }
  bool mustBeConst(int op, int i) const {
    return i < MaxSrc ? table[op][i] : false;
  }
};
const ConstSrcTable g_const_table;

// Return true if the ith source operand must be a constant.  Most
// of this information comes from the table above, but a few instructions
// have complex signatures, so we handle them individually.
bool mustUseConst(const IRInstruction& inst, int i) {
  auto check = [&](bool b) {
    assert(!b || inst.src(i)->isConst());
    return b;
  };
  // handle special cases we can't derive from IR_OPCODES macro
  switch (inst.op()) {
  case LdAddr: return check(i == 1); // offset
  case Call: return check(i == 2); // returnBcOffset
  case CallBuiltin: return check(i == 0); // f
  default: break;
  }
  return check(g_const_table.mustBeConst(int(inst.op()), i));
}
}

Constraint srcConstraint(const IRInstruction& inst, unsigned i) {
  auto r = forceAlloc(*inst.src(i));
  if (r != InvalidReg) return r;
  if (mustUseConst(inst, i)) return Constraint::IMM;
  return mcg->backEnd().srcConstraint(inst, i);
}

Constraint dstConstraint(const IRInstruction& inst, unsigned i) {
  auto r = forceAlloc(*inst.dst(i));
  if (r != InvalidReg) return r;
  return mcg->backEnd().dstConstraint(inst, i);
}

}}
