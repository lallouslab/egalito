#include <cstdio>
#include <cstring>
#include <cassert>
#include <set>
#include <capstone/x86.h>
#include <capstone/arm64.h>
#include <capstone/arm.h>
#include "disassemble.h"
#include "dump.h"
#include "makesemantic.h"
#include "elf/symbol.h"
#include "chunk/chunk.h"
#include "chunk/size.h"
#include "operation/mutator.h"
#include "instr/concrete.h"
#include "util/intervaltree.h"
#include "log/log.h"
#include "log/temp.h"

#include "chunk/dump.h"

void Disassemble::init() {
    if(PositionFactory::getInstance()) return;

    PositionFactory *positionFactory = new PositionFactory(
        //PositionFactory::MODE_DEBUGGING_NO_CACHE);  // 9.30 s
        //PositionFactory::MODE_CACHED_SUBSEQUENT);   // ~6.04 s
        PositionFactory::MODE_OFFSET);              // 5.89 s
        //PositionFactory::MODE_CACHED_OFFSET);       // 6.98 s
        //PositionFactory::MODE_GENERATION_SUBSEQUENT); // ~7.50 s
        //PositionFactory::MODE_GENERATION_OFFSET);
    PositionFactory::setInstance(positionFactory);
}

Module *Disassemble::module(ElfMap *elfMap, SymbolList *symbolList,
    DwarfUnwindInfo *dwarfInfo) {

    if(symbolList) {
        LOG(1, "Creating module from symbol info");
        return makeModuleFromSymbols(elfMap, symbolList);
    }
    else if(dwarfInfo) {
        LOG(1, "Creating module from dwarf info");
        return makeModuleFromDwarfInfo(elfMap, dwarfInfo);
    }
    else {
        LOG(1, "Creating module without symbol info or dwarf info");
        return makeModuleFromDwarfInfo(elfMap, nullptr);
    }
}

Instruction *Disassemble::instruction(const std::vector<unsigned char> &bytes,
    bool details, address_t address) {

    DisasmHandle handle(true);
    return instruction(handle, bytes, details, address);
}
Instruction *Disassemble::instruction(DisasmHandle &handle,
    const std::vector<unsigned char> &bytes, bool details, address_t address) {

    return DisassembleInstruction(handle, details).instruction(bytes, address);
}
Instruction *Disassemble::instruction(cs_insn *ins, DisasmHandle &handle,
    bool details) {

    return DisassembleInstruction(handle, details).instruction(ins);
}
Assembly Disassemble::makeAssembly(const std::vector<unsigned char> &str,
    address_t address) {

    DisasmHandle handle(true);
    return DisassembleInstruction(handle, true).makeAssembly(str, address);
}

Module *Disassemble::makeModuleFromSymbols(ElfMap *elfMap,
    SymbolList *symbolList) {

    Module *module = new Module();
    FunctionList *functionList = new FunctionList();
    module->getChildren()->add(functionList);
    module->setFunctionList(functionList);
    functionList->setParent(module);

    if(!symbolList) return module;

    for(auto sym : *symbolList) {
        // skip Symbols that we don't think represent functions
        if(!sym->isFunction()) continue;

        Function *function = Disassemble::function(elfMap, sym, symbolList);
        functionList->getChildren()->add(function);
        function->setParent(functionList);
        LOG(10, "adding function " << function->getName()
            << " at " << std::hex << function->getAddress()
            << " size " << function->getSize());
    }
#if defined(ARCH_AARCH64) || defined(ARCH_ARM)
    for(auto sym : *symbolList) {
        if(!sym->isFunction()) {
            // this misses some cases where there are only mapping symbols
            // for literals (__multc3 in libm compiled with old gcc)
            if(sym->getSize() == 0) continue;
            if(sym->getAliasFor()) continue;
            auto sec = elfMap->findSection(sym->getSectionIndex());
            if(!sec) continue;  // ABS
            if(!(sec->getHeader()->sh_flags & SHF_EXECINSTR)) continue;
            if(sec->getName() == ".plt") continue;
            if(CIter::spatial(functionList)->findContaining(
                sym->getAddress())) {

                continue;
            }
            Function *function
                = Disassemble::function(elfMap, sym, symbolList);
            functionList->getChildren()->add(function);
            function->setParent(functionList);
            LOG(10, "adding literal only function " << function->getName()
                << " at " << std::hex << function->getAddress()
                << " size " << function->getSize());
        }
    }
#endif

    return module;
}

Module *Disassemble::makeModuleFromDwarfInfo(ElfMap *elfMap,
    DwarfUnwindInfo *dwarfInfo) {

    Module *module = new Module();

    FunctionList *functionList = linearDisassembly(elfMap, ".text", dwarfInfo);
    module->getChildren()->add(functionList);
    module->setFunctionList(functionList);
    functionList->setParent(module);

    return module;
}

FunctionList *Disassemble::linearDisassembly(ElfMap *elfMap,
    const char *sectionName, DwarfUnwindInfo *dwarfInfo) {

    DisasmHandle handle(true);
    DisassembleFunction disassembler(handle, elfMap);
    return disassembler.linearDisassembly(sectionName, dwarfInfo);
}

Function *Disassemble::function(ElfMap *elfMap, Symbol *symbol,
    SymbolList *symbolList) {

    DisasmHandle handle(true);
    DisassembleFunction disassembler(handle, elfMap);
    return disassembler.function(symbol, symbolList);
}

bool DisassembleAARCH64Function::processMappingSymbol(Symbol *symbol) {
    bool literal = false;
    switch(symbol->getName()[1]) {
    case 'a':
        cs_option(handle.raw(), CS_OPT_MODE, CS_MODE_ARM);
        break;
    case 't':
        cs_option(handle.raw(), CS_OPT_MODE, CS_MODE_THUMB);
        break;
    case 'x':
        break;
    case 'd':
    default:
        literal = true;
        break;
    }
    return literal;
}

Instruction *DisassembleInstruction::instruction(
    const std::vector<unsigned char> &bytes, address_t address) {

    cs_insn *ins;
    if(cs_disasm(handle.raw(), (const uint8_t *)bytes.data(), bytes.size(),
        address, 0, &ins) != 1) {

        throw "Invalid instruction opcode string provided\n";
    }

    return instruction(ins);
}

Instruction *DisassembleInstruction::instruction(cs_insn *ins) {
    auto instr = new Instruction();
    InstructionSemantic *semantic = nullptr;

    semantic = MakeSemantic::makeNormalSemantic(instr, ins);

    if(!semantic) {
        if(details) {
            semantic = new DisassembledInstruction(Assembly(*ins));
        }
        else {
            std::string raw;
            raw.assign(reinterpret_cast<char *>(ins->bytes), ins->size);
            semantic = new RawInstruction(raw);
        }
    }
    instr->setSemantic(semantic);

    return instr;
}

Assembly DisassembleInstruction::makeAssembly(
    const std::vector<unsigned char> &str, address_t address) {

    cs_insn *insn;
    if(cs_disasm(handle.raw(), (const uint8_t *)str.data(), str.size(),
        address, 0, &insn) != 1) {

        throw "Invalid instruction opcode string provided\n";
    }
    Assembly assembly(*insn);
    cs_free(insn, 1);
    return assembly;
}

// --- X86_64 disassembly code

Function *DisassembleX86Function::function(Symbol *symbol,
    SymbolList *symbolList) {

    auto sectionIndex = symbol->getSectionIndex();
    auto section = elfMap->findSection(sectionIndex);

    PositionFactory *positionFactory = PositionFactory::getInstance();
    Function *function = new FunctionFromSymbol(symbol);

    address_t symbolAddress = symbol->getAddress();

    function->setPosition(
        positionFactory->makeAbsolutePosition(symbolAddress));

    auto readAddress =
        section->getReadAddress() + section->convertVAToOffset(symbolAddress);
    auto readSize = symbol->getSize();
    auto virtualAddress = symbol->getAddress();

    disassembleBlocks(
        function, readAddress, readSize, virtualAddress);

    {
        ChunkMutator m(function);  // recalculate cached values if necessary
    }

    return function;
}

FunctionList *DisassembleX86Function::linearDisassembly(const char *sectionName,
    DwarfUnwindInfo *dwarfInfo) {

    auto section = elfMap->findSection(sectionName);
    if(!section) return nullptr;

    address_t virtualAddress = section->getVirtualAddress();
    address_t readAddress = section->getReadAddress()
        + section->convertVAToOffset(virtualAddress);
    size_t readSize = section->getSize();

#if 0
    std::set<address_t> splitPoints;
    std::map<address_t, size_t> nopByteCount;
    splitPoints.insert(elfMap->getEntryPoint());
    splitPoints.insert(section->getVirtualAddress());
    {
        cs_insn *insn;
        size_t count = cs_disasm(handle.raw(),
            (const uint8_t *)readAddress, readSize, virtualAddress, 0, &insn);
        size_t nopBytes = 0;
        for(size_t j = 0; j < count; j++) {
            auto ins = &insn[j];

            address_t target = 0;
            if(shouldSplitFunctionDueTo(ins, &target)) {
                splitPoints.insert(target);
            }

            if(ins->id == X86_INS_NOP) {
                nopBytes += ins->size;
                nopByteCount[ins->address + ins->size] = nopBytes;
            }
            else nopBytes = 0;
        }

        if(count > 0) {
            cs_free(insn, count);
        }
    }

    FunctionList *functionList = new FunctionList();

    std::vector<Range> fromScratchIntervalList;

    for(std::set<address_t>::iterator it = splitPoints.begin();
        it != splitPoints.end(); it ++) {

        address_t functionOffset = (*it) - virtualAddress;
        std::set<address_t>::iterator next = it;
        next ++;
        address_t functionSize;
        if(next != splitPoints.end()) {
            functionSize = (*next) - (*it);
        }
        else {
            functionSize = readSize - functionOffset;
        }

        auto nop = nopByteCount.find((*it) + functionSize);
        if(nop != nopByteCount.end()) {
            LOG(10, "Shrinking function by " << (*nop).second << " nop bytes");
            functionSize -= (*nop).second;
        }

        Range range(virtualAddress + functionOffset, functionSize);
        fromScratchIntervalList.push_back(range);
    }

    LOG(1, "Splitting code section into " << fromScratchIntervalList.size()
        << " fuzzy functions");

    for(const Range &range : fromScratchIntervalList) {
        address_t intervalVirtualAddress = range.getStart();
        address_t intervalOffset = intervalVirtualAddress - virtualAddress;
        address_t intervalSize = range.getSize();
        LOG(1, "Split into function [0x"
            << std::hex << intervalVirtualAddress << ",+"
            << intervalSize << ") at section offset 0x" << intervalOffset);

        Function *function = new FuzzyFunction(intervalVirtualAddress);

        PositionFactory *positionFactory = PositionFactory::getInstance();
        function->setPosition(
            positionFactory->makeAbsolutePosition(intervalVirtualAddress));

        disassembleBlocks(function, readAddress + intervalOffset,
            intervalSize, intervalVirtualAddress);

        {
            ChunkMutator m(function);  // recalculate cached values if necessary
        }

        functionList->getChildren()->add(function);
        function->setParent(functionList);
    }
#else
    IntervalTree intervals(Range(virtualAddress, readSize));

    std::set<address_t> splitPoints;
    std::map<address_t, size_t> nopByteCount;
    splitPoints.insert(elfMap->getEntryPoint());
    splitPoints.insert(section->getVirtualAddress());
    {
        cs_insn *insn;
        size_t count = cs_disasm(handle.raw(),
            (const uint8_t *)readAddress, readSize, virtualAddress, 0, &insn);
        size_t nopBytes = 0;
        for(size_t j = 0; j < count; j++) {
            auto ins = &insn[j];

            address_t target = 0;
            if(shouldSplitFunctionDueTo(ins, &target)) {
                splitPoints.insert(target);
            }

            if(ins->id == X86_INS_NOP) {
                nopBytes += ins->size;
                nopByteCount[ins->address + ins->size] = nopBytes;
            }
            else nopBytes = 0;
        }

        if(count > 0) {
            cs_free(insn, count);
        }
    }

    std::vector<Range> intervalList;
    for(auto it = dwarfInfo->fdeBegin(); it != dwarfInfo->fdeEnd(); it ++) {
        DwarfFDE *fde = *it;
        LOG(1, "looks like an FDE at [" << std::hex << fde->getPcBegin() << ",+"
            << fde->getPcRange() << "]");

        Range range(fde->getPcBegin(), fde->getPcRange());
        Range sectionRange(section->getVirtualAddress(), section->getSize());
        if(range.overlaps(sectionRange)) {
            intervalList.push_back(range);
        }
        else {
            LOG(1, "FDE is out of bounds of .text section, skipping");
        }
    }

    if(auto s = elfMap->findSection(".init")) {
        intervalList.push_back(Range(s->getVirtualAddress(), s->getSize()));
    }
    if(auto s = elfMap->findSection(".fini")) {
        intervalList.push_back(Range(s->getVirtualAddress(), s->getSize()));
    }

    address_t entryPoint = elfMap->getEntryPoint();
    LOG(1, "Disassembly pass assumes _start at entry point 0x"
        << std::hex << entryPoint);
    for(auto range : intervalList) {
        if(range.getStart() == entryPoint) {
            // Gentoo, with gcc 5.4.0
            if(range.getSize() == 0x2a) {
                address_t start = entryPoint + 0x2a;
                start = (start + 0x7) & (~0x7);

                LOG(1, "Adding deregister_tm_clones etc starting at 0x"
                    << std::hex << start);

                intervalList.push_back(Range(start, 0x40));  // deregister_tm_clones
                start += intervalList.back().getSize();
                intervalList.push_back(Range(start, 0x40));  // register_tm_clones
                start += intervalList.back().getSize();
                intervalList.push_back(Range(start, 0x20));  // __do_global_dtors_aux
                start += intervalList.back().getSize();
                intervalList.push_back(Range(start, 0x30));  // frame_dummy
                start += intervalList.back().getSize();
            }

            // Debian, with gcc 7.2.0
            if(range.getSize() == 0x2b) {
                address_t start = entryPoint + 0x2b;
                start = (start + 0x7) & (~0x7);

                LOG(1, "Adding deregister_tm_clones etc starting at 0x"
                    << std::hex << start);

                intervalList.push_back(Range(start, 0x40));  // deregister_tm_clones
                start += intervalList.back().getSize();
                intervalList.push_back(Range(start, 0x50));  // register_tm_clones
                start += intervalList.back().getSize();
                intervalList.push_back(Range(start, 0x40));  // __do_global_dtors_aux
                start += intervalList.back().getSize();
                intervalList.push_back(Range(start, 0x10));  // frame_dummy
                start += intervalList.back().getSize();
            }
            break;
        }
    }

    FunctionList *functionList = new FunctionList();
    LOG(1, "Splitting code section into " << intervalList.size()
        << " fuzzy functions");

    for(const Range &range : intervalList) {
        address_t intervalVirtualAddress = range.getStart();
        address_t intervalOffset = intervalVirtualAddress - virtualAddress;
        address_t intervalSize = range.getSize();
        LOG(1, "Split into function [0x"
            << std::hex << intervalVirtualAddress << ",+"
            << intervalSize << ") at section offset 0x" << intervalOffset);

        Function *function = new FuzzyFunction(intervalVirtualAddress);

        PositionFactory *positionFactory = PositionFactory::getInstance();
        function->setPosition(
            positionFactory->makeAbsolutePosition(intervalVirtualAddress));

        disassembleBlocks(function, readAddress + intervalOffset,
            intervalSize, intervalVirtualAddress);

        {
            ChunkMutator m(function);  // recalculate cached values if necessary
        }

        functionList->getChildren()->add(function);
        function->setParent(functionList);
    }
#endif

    return functionList;
}

// --- AARCH64 disassembly code

// We do not handle binaries that contain embedded literals in code without
// mapping symbols.

Function *DisassembleAARCH64Function::function(Symbol *symbol,
    SymbolList *symbolList) {

    auto sectionIndex = symbol->getSectionIndex();
    auto section = elfMap->findSection(sectionIndex);

    PositionFactory *positionFactory = PositionFactory::getInstance();
    Function *function = new FunctionFromSymbol(symbol);

    address_t symbolAddress = symbol->getAddress();
#ifdef ARCH_ARM
    symbolAddress &= ~1;
#endif

    function->setPosition(
        positionFactory->makeAbsolutePosition(symbolAddress));

    auto readAddress =
        section->getReadAddress() + section->convertVAToOffset(symbolAddress);
    auto virtualAddress = symbol->getAddress();

    if(knownLinkerBytes(symbol)) {
        LOG(1, "treating " << symbol->getName() << " as a special case");
        disassembleBlocks(true, function, readAddress, symbol->getSize(),
            virtualAddress);
        ChunkMutator m(function);  // recalculate cached values if necessary
        return function;
    }

    auto mapping = symbolList->findMappingBelowOrAt(symbol);
    if(!mapping) {
        LOG(1, "NO mapping symbol below " << symbol->getName()
            << " at " << std::hex << symbol->getAddress()
            << " - " << (symbol->getAddress() + symbol->getSize()));
        throw "mapping symbol decode error";
    }
    LOG(10, "mapping symbol below " << symbol->getName()
        << " at " << std::hex << symbol->getAddress()
        << " - " << (symbol->getAddress() + symbol->getSize())
        << " is " << mapping->getName()
        << " #" << std::dec << mapping->getIndex());

    address_t end = symbol->getAddress() + symbol->getSize();
    size_t offset = 0;

    bool literal = processMappingSymbol(mapping);
    while((mapping = symbolList->findMappingAbove(mapping))) {
        LOG(10, "    next mapping symbol is #"
            << std::dec << mapping->getIndex());
        if(end <= mapping->getAddress()) {
            auto size = symbol->getSize() - offset;
            disassembleBlocks(literal, function, readAddress + offset,
                size, virtualAddress + offset);
            offset += size;
            break;
        }
        auto size = mapping->getAddress() - (symbol->getAddress() + offset);
        disassembleBlocks(literal, function, readAddress + offset,
            size, symbol->getAddress() + offset);
        offset += size;
        literal = processMappingSymbol(mapping);
    }
    if(offset < symbol->getSize()) {
        disassembleBlocks(literal, function, readAddress + offset,
            symbol->getSize() - offset, virtualAddress + offset);
    }

    {
        ChunkMutator m(function);  // recalculate cached values if necessary
    }

    return function;
}

FunctionList *DisassembleAARCH64Function::linearDisassembly(
    const char *sectionName, DwarfUnwindInfo *dwarfInfo) {

    auto section = elfMap->findSection(sectionName);
    if(!section) return nullptr;

    address_t virtualAddress = section->getVirtualAddress();
    address_t readAddress = section->getReadAddress()
        + section->convertVAToOffset(virtualAddress);
    size_t readSize = section->getSize();

    std::set<address_t> splitPoints;
    splitPoints.insert(elfMap->getEntryPoint());
    for(size_t size = 0; size < readSize; ) {
        cs_insn *insn;
        size_t count = cs_disasm(handle.raw(),
            (const uint8_t *)readAddress + size,
            readSize - size,
            virtualAddress + size,
            0, &insn);
        for(size_t j = 0; j < count; j++) {
            auto ins = &insn[j];

            address_t target = 0;
            if(shouldSplitFunctionDueTo(ins, &target)) {
                splitPoints.insert(target);
            }
        }

        if(count > 0) {
            cs_free(insn, count);
            size += count * 4;
        }
        else {
            size += 4;
        }
    }

    FunctionList *functionList = new FunctionList();

    LOG(1, "Splitting code section into " << splitPoints.size()
        << " fuzzy functions");

    for(std::set<address_t>::iterator it = splitPoints.begin();
        it != splitPoints.end(); it ++) {

        address_t functionOffset = (*it) - virtualAddress;
        std::set<address_t>::iterator next = it;
        next ++;
        address_t functionSize;
        if(next != splitPoints.end()) {
            functionSize = (*next) - (*it);
        }
        else {
            functionSize = readSize - functionOffset;
        }

        LOG(10, "Split into function [0x" << std::hex << (*it) << ",+"
            << functionSize << ")");

        Function *function = new FuzzyFunction(virtualAddress + functionOffset);

        PositionFactory *positionFactory = PositionFactory::getInstance();
        function->setPosition(
            positionFactory->makeAbsolutePosition(
                virtualAddress + functionOffset));

        // We don't handle literals for now. We need the section index to
        // find the corresponding mapping symbol.
        disassembleBlocks(false, function, readAddress + functionOffset,
            functionSize, virtualAddress + functionOffset);

        {
            ChunkMutator m(function);  // recalculate cached values if necessary
        }

        functionList->getChildren()->add(function);
        function->setParent(functionList);
    }

    return functionList;
}

void DisassembleAARCH64Function::disassembleBlocks(bool literal,
    Function *function, address_t readAddress, size_t readSize,
    address_t virtualAddress) {

    if(literal) {
        processLiterals(function, readAddress, readSize, virtualAddress);
    }
    else {
        DisassembleFunctionBase::disassembleBlocks(
            function, readAddress, readSize, virtualAddress);
    }
}

void DisassembleAARCH64Function::processLiterals(Function *function,
    address_t readAddress, size_t readSize, address_t virtualAddress) {

    LOG(10, "literals embedded in " << function->getName()
        << " at address 0x" << std::hex << virtualAddress);

    PositionFactory *positionFactory = PositionFactory::getInstance();

    Block *block = makeBlock(function, nullptr);

    Chunk *prevChunk = nullptr;
    if(function->getChildren()->getIterable()->getCount() > 0) {
        prevChunk = function->getChildren()->getIterable()->getLast();
    }

    for(size_t sz = 0; sz < readSize; sz += 4) {
        auto instr = new Instruction();
        std::string raw;
        raw.assign(reinterpret_cast<char *>(readAddress + sz), 4);
        auto li = new LiteralInstruction(raw);
        instr->setSemantic(li);
        instr->setPosition(
            positionFactory->makePosition(prevChunk, instr, block->getSize()));
        prevChunk = instr;
        ChunkMutator(block, false).append(instr);
    }
    if(block->getSize() > 0) {
        ChunkMutator(function, false).append(block);
    }
    else {
        delete block;
    }
}

bool DisassembleAARCH64Function::knownLinkerBytes(Symbol *symbol) {
    if(!strcmp(symbol->getName(), "buildsig")) return true;
    return false;
}

void DisassembleFunctionBase::disassembleBlocks(Function *function,
    address_t readAddress, size_t readSize, address_t virtualAddress) {

    PositionFactory *positionFactory = PositionFactory::getInstance();

    cs_insn *insn;
    LOG(19, "disassemble 0x" << std::hex << readAddress << " size " << readSize
        << ", virtual address " << virtualAddress);
    size_t count = cs_disasm(handle.raw(),
        (const uint8_t *)readAddress, readSize, virtualAddress, 0, &insn);

    Block *block = makeBlock(function, nullptr);

    for(size_t j = 0; j < count; j++) {
        auto ins = &insn[j];

        // check if this instruction ends the current basic block
        bool split = shouldSplitBlockAt(ins);

        // Create Instruction from cs_insn
        auto instr = Disassemble::instruction(ins, handle, true);

        Chunk *prevChunk = nullptr;
        if(block->getChildren()->getIterable()->getCount() > 0) {
            prevChunk = block->getChildren()->getIterable()->getLast();
        }
        else if(function->getChildren()->getIterable()->getCount() > 0) {
            prevChunk = function->getChildren()->getIterable()->getLast();
        }
        else {
            prevChunk = nullptr;
        }
        instr->setPosition(
            positionFactory->makePosition(prevChunk, instr, block->getSize()));

        ChunkMutator(block, false).append(instr);
        if(split) {
            LOG(11, "split-instr in block: " << j+1);
            ChunkMutator(function, false).append(block);

            block = makeBlock(function, block);
        }
    }

    if(block->getSize() > 0) {
        CLOG0(9, "fall-through function [%s]... "
            "adding basic block\n", function->getName().c_str());
        ChunkMutator(function, false).append(block);
    }
    if(block->getSize() == 0) {
        delete block;
    }

    cs_free(insn, count);
}

Block *DisassembleFunctionBase::makeBlock(Function *function, Block *prev) {
    PositionFactory *positionFactory = PositionFactory::getInstance();

    if(prev == nullptr) {
        if(function->getChildren()->getIterable()->getCount() > 0) {
            prev = function->getChildren()->getIterable()->getLast();
        }
    }
    Block *block = new Block();
    block->setPosition(
        positionFactory->makePosition(prev, block,
                                      function->getSize()));
    return block;
}

bool DisassembleFunctionBase::shouldSplitBlockAt(cs_insn *ins) {
    // Note: we split on all explicit control flow changes like jumps, rets,
    // etc, but not on conditional moves or instructions that generate OS
    // interrupts/exceptions/traps.
    bool split = false;
#ifdef ARCH_X86_64
    if(cs_insn_group(handle.raw(), ins, X86_GRP_JUMP)) {
        split = true;
    }
    else if(cs_insn_group(handle.raw(), ins, X86_GRP_CALL)) {
        split = true;
    }
    else if(cs_insn_group(handle.raw(), ins, X86_GRP_RET)) {
        split = true;
    }
#elif defined(ARCH_AARCH64)
    if(cs_insn_group(handle.raw(), ins, ARM64_GRP_JUMP)) {  // only branches
        split = true;
    }
    else if(ins->id == ARM64_INS_BL
        || ins->id == ARM64_INS_BLR
        || ins->id == ARM64_INS_RET) {

        split = true;
    }
#elif defined(ARCH_ARM)
    if(cs_insn_group(handle.raw(), ins, ARM_GRP_JUMP)) {
        split = true;
    }
    else if(ins->id == ARM_INS_B
        || ins->id == ARM_INS_BX
        || ins->id == ARM_INS_BL
        || ins->id == ARM_INS_BLX
        || ins->id == ARM_INS_BXJ
        || ins->id == ARM_INS_CBZ
        || ins->id == ARM_INS_CBNZ) {

        split = true;
    }
#endif
    return split;
}

bool DisassembleFunctionBase::shouldSplitFunctionDueTo(cs_insn *ins,
    address_t *target) {

#ifdef ARCH_X86_64
    if(cs_insn_group(handle.raw(), ins, X86_GRP_CALL)) {
        cs_x86 *x = &ins->detail->x86;
        cs_x86_op *op = &x->operands[0];
        if(x->op_count > 0 && op->type == X86_OP_IMM) {
            *target = op->imm;
            return true;
        }
    }
#elif defined(ARCH_AARCH64)
    if(ins->id == ARM64_INS_BL) {
        cs_arm64 *x = &ins->detail->arm64;
        cs_arm64_op *op = &x->operands[0];
        if(x->op_count > 0 && op->type == ARM64_OP_IMM) {
            *target = op->imm;
            return true;
        }
    }
#elif defined(ARCH_ARM)
    #error "Not yet implemented"
#endif
    return false;
}
