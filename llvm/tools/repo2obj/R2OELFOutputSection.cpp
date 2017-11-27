//===-- R2OELFOutputSection.cpp -------------------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "R2OELFOutputSection.h"

using namespace llvm;
using namespace pstore::repo;

namespace details {

    SectionMap const SectionAttributes{
        {ELFSectionType::BSS, {".bss", ELF::SHT_NOBITS, ELF::SHF_ALLOC | ELF::SHF_WRITE}},
        // X (Common)
        {ELFSectionType::Data, {".data", ELF::SHT_PROGBITS, ELF::SHF_ALLOC | ELF::SHF_WRITE}},
        // X (RelRo)
        {ELFSectionType::Text, {".text", ELF::SHT_PROGBITS, ELF::SHF_ALLOC | ELF::SHF_EXECINSTR}},
        // X (Mergeable1ByteCString)
        // X (Mergeable2ByteCString)
        // X (Mergeable4ByteCString)
        // X (MergeableConst4)
        // X (MergeableConst8)
        // X (MergeableConst16)
        // X (MergeableConst32)
        // X (MergeableConst)
        {ELFSectionType::ReadOnly, {".rodata", ELF::SHT_PROGBITS, ELF::SHF_ALLOC}},
        {ELFSectionType::ThreadBSS,
         {".tbss", ELF::SHT_NOBITS, ELF::SHF_ALLOC | ELF::SHF_WRITE | ELF::SHF_TLS}},
        {ELFSectionType::ThreadData,
         {".tdata", ELF::SHT_PROGBITS, ELF::SHF_ALLOC | ELF::SHF_WRITE | ELF::SHF_TLS}},
        // X (ThreadLocal)
        // X (Metadata)
        {ELFSectionType::InitArray,
         {".init_array", ELF::SHT_INIT_ARRAY, ELF::SHF_ALLOC | ELF::SHF_WRITE }},
        {ELFSectionType::FiniArray,
         {".fini_array", ELF::SHT_FINI_ARRAY, ELF::SHF_ALLOC | ELF::SHF_WRITE }},
    };

} // namespace details
