#include <sys/mman.h>
#include <cstring>
#include <algorithm>

#include "bingen.h"
#include "chunk/concrete.h"
#include "conductor/conductor.h"
#include "conductor/setup.h"
#include "elf/elfspace.h"
#include "instr/semantic.h"
#include "instr/writer.h"
#include "load/segmap.h"
#include "operation/mutator.h"
#include "pass/relocdata.h"
#include "pass/fixdataregions.h"
#include "pass/instrumentcalls.h"
#include "pass/switchcontext.h"

#include "log/log.h"
#include "chunk/dump.h"

#define ROUND_DOWN(x)       ((x) & ~0xfff)
#define ROUND_UP(x)         (((x) + 0xfff) & ~0xfff)
#define ROUND_UP_BY(x, y)   (((x) + (y) - 1) & ~((y) - 1))

BinGen::BinGen(ConductorSetup *setup, const char *filename)
    : setup(setup), mainModule(nullptr), addon(nullptr),
      fs(filename, std::ios::out | std::ios::binary) {

    mainModule = setup->getConductor()->getProgram()->getMain();
    for(auto module : CIter::children(setup->getConductor()->getProgram())) {
        if(module->getName() == "module-(addon)") {
            addon = module;
            break;
        }
    }
}

BinGen::~BinGen() {
    fs.close();
}

static std::vector<Function *> makeSortedFunctionList(Module *module) {
    std::vector<Function *> list;
    for(auto func : CIter::functions(module)) {
        list.push_back(func);
    }
    std::sort(list.begin(), list.end(),
        [](Function *a, Function *b) {
            return a->getAddress() < b->getAddress();
        });
    return list;
}

address_t BinGen::reassignFunctionAddress() {
    auto list = makeSortedFunctionList(mainModule);
    auto address = list.front()->getAddress();
    for(auto func : list) {
        LOG(1, func->getName() << " : "
            << func->getAddress() << " -> " << address);
        ChunkMutator(func).setPosition(address);
        address += func->getSize();
    }

    if(addon) {
        list = makeSortedFunctionList(addon);
        for(auto func : list) {
            LOG(1, func->getName() << " : "
                << func->getAddress() << " -> " << address);
            ChunkMutator(func).setPosition(address);
            address += func->getSize();
        }
    }

    return list.back()->getAddress() + list.back()->getSize();
}

void BinGen::dePLT(void) {
    if(addon) {
        for(auto func : CIter::functions(addon)) {
            for(auto block : CIter::children(func)) {
                for(auto instr : CIter::children(block)) {
                    auto s = instr->getSemantic();
                    auto cfi = dynamic_cast<ControlFlowInstruction *>(s);
                    if(!cfi) continue;
                    auto link = dynamic_cast<PLTLink *>(cfi->getLink());
                    if(!link) continue;
                    auto target = link->getPLTTrampoline()->getTarget();
                    if(!target) {
                        LOG(1, "target is supposed to be resolved!");
                        throw "dePLT: error";
                    }
                    auto newLink = new NormalLink(target);
                    cfi->setLink(newLink);
                    delete link;
                }
            }
        }
    }
}

void BinGen::addCallLogging() {
#if defined(ARCH_AARCH64)
    if(!addon) return;

    auto funcEntry = CIter::named(addon->getFunctionList())
        ->find("egalito_log_function");
    if(!funcEntry) return;

    auto funcExit = CIter::named(addon->getFunctionList())
        ->find("egalito_log_function_ret");
    if(!funcExit) return;

    auto prologue = CIter::named(addon->getFunctionList())
        ->find("egalito_dump_logs");
    if(!prologue) return;

    auto mainFunc = CIter::named(mainModule->getFunctionList())->find("main");
    if(!mainFunc) return;

    SwitchContextPass switcher;
    funcEntry->accept(&switcher);
    funcExit->accept(&switcher);
    prologue->accept(&switcher);

    InstrumentCallsPass instrument;
    instrument.setEntryAdvice(funcEntry);
    instrument.setExitAdvice(funcExit);
#if 0
    instrument.setPredicate([](Function *function) {
        return !function->hasName("egalito_log_function_transition");
    });
#endif
    mainModule->accept(&instrument);

    instrument.setEntryAdvice(nullptr);
    instrument.setExitAdvice(prologue);
    mainFunc->accept(&instrument);
#endif
}

void BinGen::applyAdditionalTransform() {
    addCallLogging();

    dePLT();
}

int BinGen::generate() {
    applyAdditionalTransform();

    auto endOfText = reassignFunctionAddress();
    changeMapAddress(mainModule, 0xa0000000);

    address_t pos = makeImageBox();

    SegMap::mapAllSegments(setup);

    //setup->getConductor()->fixDataSections();
    RelocDataPass relocData(setup->getConductor());
    setup->getConductor()->getProgram()->accept(&relocData);

    //changeMapAddress(mainModule, 0);
    mainModule->getElfSpace()->getElfMap()->setBaseAddress(0);
    interleaveData(endOfText);

    FixDataRegionsPass fixDataRegions;
    setup->getConductor()->getProgram()->accept(&fixDataRegions);

    writeOut(pos);

    setup->getConductor()->writeDebugElf("bin-symbols.elf");

    LOG(0, "entry point is located at 0x"
        << std::hex << setup->getEntryPoint());

    return 0;
}

void BinGen::changeMapAddress(Module *module, address_t address) {
    auto map = module->getElfSpace()->getElfMap();
    map->setBaseAddress(address);
    for(auto region : CIter::regions(module)) {
        if(region == module->getDataRegionList()->getTLS()) continue;

        region->updateAddressFor(map->getBaseAddress());
    }
}

address_t BinGen::makeImageBox() {
    auto mainMap = setup->getConductor()->getMainSpace()->getElfMap();

    address_t startAddr = 0;
    for(void *s : mainMap->getSegmentList()) {
        Elf64_Phdr *phdr = static_cast<Elf64_Phdr *>(s);
        if(phdr->p_type != PT_LOAD) continue;

        startAddr = phdr->p_vaddr;
        break;
    }

    size_t length = mainMap->getLength();
    if(addon) {
        length += addon->getElfSpace()->getElfMap()->getLength();
    }

    length = ROUND_UP(length);
    auto imageMap = mmap((void *)startAddr, length, PROT_READ | PROT_WRITE,
            MAP_ANONYMOUS | MAP_PRIVATE,
            -1, 0);
    if(imageMap == (void *)-1) {
        LOG(1, "failed to create image: no more memory");
        throw "mmap error";
    }
    if(imageMap != (void *)startAddr) {
        LOG(1, "failed to create image: overlapping");
        throw "mmap error";
    }

    LOG(1, "imageBox at " << startAddr << " length " << length);
    return startAddr;
}

void BinGen::interleaveData(address_t pos) {
    LOG(1, "rouding up pos " << pos << " to " << ROUND_UP_BY(pos, 8));
    pos = ROUND_UP_BY(pos, 8);
    LOG(1, "data should start at " << pos);
    LOG(1, "copying in main rodata");
    pos = copyInData(mainModule, pos, false);
    if(addon) {
        LOG(1, "rouding up pos " << pos << " to " << ROUND_UP_BY(pos, 8));
        pos = ROUND_UP_BY(pos, 8);
        LOG(1, "copying in addon rodata");
        pos = copyInData(addon, pos, false);
    }

    pos = ROUND_UP(pos);
    LOG(1, "copying in main rwdata to box");
    pos = copyInData(mainModule, pos, true);
    if(addon) {
        LOG(1, "rouding up pos " << pos << " to " << ROUND_UP_BY(pos, 8));
        pos = ROUND_UP_BY(pos, 8);
        LOG(1, "copying in addon rwdata to box");
        pos = copyInData(addon, pos, true);
    }
}

address_t BinGen::copyInData(Module *module, address_t pos, bool writable) {
    for(auto region : CIter::children(module->getDataRegionList())) {
        if(region->writable() != writable) continue;
        if(region->bss()) continue;

        LOG(1, "copying in to " << pos
            << " from " << (region->getAddress() + region->getStartOffset()));
        std::memcpy((void *)pos,
            (void *)(region->getAddress() + region->getStartOffset()),
            region->getSize() - region->getStartOffset());
        //ChunkMutator(region).setPosition(pos - region->getStartOffset());
        LOG(1, "offset is " << region->getStartOffset());
        ChunkMutator(region).setPosition(pos - region->getStartOffset());

        LOG(1, "base for " << region->getName() << " set to " << region->getAddress());
        pos += region->getSize() - region->getStartOffset();
    }

    return pos;
}

void BinGen::writeOut(address_t pos) {
    pos = writeOutCode(mainModule, pos);
    if(addon) {
        pos = writeOutCode(addon, pos);
    }

    LOG(1, "writing out main rodata");
    pos = writeOutRoData(mainModule, pos);
    if(addon) {
        LOG(1, "writing out addon rodata");
        pos = writeOutRoData(addon, pos);
    }

    LOG(1, "writing out main data");
    pos = writeOutRwData(mainModule, pos);
    if(addon) {
        LOG(1, "writing out addon data");
        pos = writeOutRwData(addon, pos);
    }
    LOG(1, "final pos = " << pos);
}

// this needs to write out PLT too (if addon is a library)
address_t BinGen::writeOutCode(Module *module, address_t pos) {
    const int ll = 1;
    std::vector<Function *> list;
    for(auto func : CIter::functions(module)) {
        list.push_back(func);
    }
    std::sort(list.begin(), list.end(),
        [](Function *a, Function *b) {
            return a->getAddress() < b->getAddress();
        });

    for(auto func : list) {
        LOG(ll, "writing out " << func->getName()
            << ": pos " << pos << " vs function " << func->getAddress());
        std::cout.flush();
        if(pos != func->getAddress()) {
            LOG(ll, "adding padding of size " << (func->getAddress() - pos));
            std::string zero(func->getAddress() - pos, 0);
            fs << zero;
        }

        for(auto block : CIter::children(func)) {
            for(auto instr : CIter::children(block)) {
#if 0
                ChunkDumper dumper;
                instr->accept(&dumper);
#endif

                std::string output;
                InstrWriterCppString writer(output);
                instr->getSemantic()->accept(&writer);
                fs << output;
            }
        }
        pos = func->getAddress() + func->getSize();
        LOG(ll, " to " << pos);
    }
    return pos;
}

address_t BinGen::writeOutRoData(Module *module, address_t pos) {
    return writeOutData(module, pos, false);
}

address_t BinGen::writeOutRwData(Module *module, address_t pos) {
    return writeOutData(module, pos, true);
}

address_t BinGen::writeOutData(Module *module, address_t pos, bool writable) {
    for(auto region : CIter::children(module->getDataRegionList())) {
        if(region->writable() != writable) continue;

        LOG(1, "region at " << region->getAddress());
        LOG(1, "  offset " << region->getStartOffset());
        LOG(1, "  size   " << region->getSize());
        LOG(1, "pos at " << pos);
        auto start = region->getAddress() + region->getStartOffset();
        if(pos != start) {
            LOG(1, "adding padding of size " << (start - pos));
            std::string zero(start - pos, 0);
            fs << zero;
            pos += start - pos;
        }
        auto size = region->getSize() - region->getStartOffset();
        LOG(1, "writing out data: " << region->getName()
            << " at " << start << " to " << (start + size));
        fs.write(reinterpret_cast<char *>(start), size);
        pos += size;
    }
    return pos;
}
