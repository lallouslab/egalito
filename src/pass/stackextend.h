#ifndef EGALITO_PASS_STACKEXTEND_H
#define EGALITO_PASS_STACKEXTEND_H

#include <vector>
#include <map>
#include <set>
#include "chunkpass.h"
#include "analysis/controlflow.h"
#include "elf/reloc.h"

#if defined(ARCH_AARCH64) || defined(ARCH_ARM)
class ControlFlowInstruction;

class FrameType {
private:
    size_t baseSize;    // local variable + callee-saved regs
    size_t outArgSize;
    Instruction *setBPInstr;
    std::vector<Instruction *> resetSPInstrs;
    std::vector<Instruction *> epilogueInstrs;
    std::vector<ControlFlowInstruction *> jumpToEpilogueInstrs;

public:
    FrameType(Function *function);
    Instruction *getSetBPInstr() const { return setBPInstr; }
    std::vector<Instruction *> getResetSPInstrs() const
        { return resetSPInstrs; }
    std::vector<Instruction *> getEpilogueInstrs() const
        { return epilogueInstrs; }
    void fixEpilogue(Instruction *oldInstr, Instruction *newInstr);
    void setSetBPInstr(Instruction *newInstr) { setBPInstr = newInstr; }
    void dump();

private:
    size_t getFrameSize(Function *function);
};

class StackExtendPass : public ChunkPass {
private:
    size_t extendSize;
    bool withSave;
    int reg1;
    int reg2;

public:
    StackExtendPass(size_t extendSize, bool withSave, int reg1=29, int reg2=30)
        : extendSize(extendSize), withSave(withSave), reg1(reg1), reg2(reg2) {}

    virtual void visit(Module *module);
    virtual void visit(Function *function);
    virtual void visit(Block *block) {}
    virtual void visit(Instruction *instruction) {}
private:
    virtual bool shouldApply(Function *function) { return true; }
    void addExtendStack(Function *function, FrameType *frame);
    void addShrinkStack(Function *function, FrameType *frame);
    virtual void useStack(Function *function, FrameType *frame) {};
};
#endif

#endif
