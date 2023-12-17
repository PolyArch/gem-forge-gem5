#include "arch/x86/insts/microldstop.hh"

#include <string>

#include "arch/x86/regs/float.hh"
#include "arch/x86/regs/int.hh"
#include "arch/x86/regs/misc.hh"
#include "arch/x86/isa.hh"
#include "base/logging.hh"
#include "cpu/exec_context.hh"
#include "debug/X86AMX.hh"

namespace gem5
{

namespace X86ISA
{


int
LdStFpOp::getAMXSubTileBytes(ExecContext *xc,
                             int tileId, int row, int col) const
{
    auto isa = dynamic_cast<X86ISA::ISA *>(
        xc->tcBase()->getIsaPtr());
    assert(isa != nullptr);

    const auto &cfg = isa->getAMXTileCfg();

    auto bytes = cfg.getSubTileSize(tileId, row, col);

    DPRINTF(X86AMX, "AMXSubTileBytes %d-%d-%d %d\n",
        tileId, row, col, bytes);
    return bytes;
}

} // namespace X86ISA
} // namespace gem5
