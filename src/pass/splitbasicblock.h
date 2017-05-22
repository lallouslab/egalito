#ifndef EGALITO_PASS_SPLITBASICBLOCK_CALLS_H
#define EGALITO_PASS_SPLITBASICBLOCK_CALLS_H

#include "chunkpass.h"
#include "elf/reloc.h"

class SplitBasicBlock : public ChunkPass {
public:
    SplitBasicBlock() {}
    virtual void visit(Function *function);
};

#endif
