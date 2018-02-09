//===- repo2obj - Convert a Repository ticket to an ELF object file ------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "llvm/ADT/ArrayRef.h"
#include "llvm/ADT/Optional.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/BinaryFormat/Repo.h"
#include "llvm/MC/StringTableBuilder.h"
#include "llvm/Object/Binary.h"
#include "llvm/Object/ELF.h"
#include "llvm/Object/ELFObjectFile.h"
#include "llvm/Object/ELFTypes.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/ToolOutputFile.h"
#include "llvm/Support/raw_ostream.h"

#include "pstore/database.hpp"
#include "pstore/hamt_map.hpp"
#include "pstore/hamt_set.hpp"
#include "pstore/index_types.hpp"
#include "pstore/sstring_view_archive.hpp"
#include "pstore_mcrepo/fragment.hpp"
#include "pstore_mcrepo/ticket.hpp"

#include "R2OELFOutputSection.h"
#include "WriteHelpers.h"

#include <cstdlib>
#include <functional>
#include <list>
#include <tuple>
#include <unordered_map>

#ifdef _WIN32
#include <io.h>
#endif

using namespace llvm;
using namespace llvm::object;

namespace {

cl::opt<std::string> RepoPath("repo", cl::Optional,
                              cl::desc("Program repository path"),
                              cl::init("./clang.db"));
cl::opt<std::string> TicketPath(cl::Positional, cl::desc("<ticket path>"));
static cl::opt<std::string> OutputFilename("o", cl::desc("Output filename"),
                                           cl::value_desc("filename"),
                                           cl::init("./a.out"));

} // end anonymous namespace

LLVM_ATTRIBUTE_NORETURN static void error(Twine Message) {
  errs() << Message << "\n";
  exit(EXIT_FAILURE);
}

static ErrorOr<pstore::uuid> getTicketFileUuid(StringRef TicketPath) {
  constexpr auto TicketFileSize =
      sizeof(std::uint64_t) + pstore::uuid::elements;
  int TicketFD = 0;
  if (std::error_code Err = sys::fs::openFileForRead(TicketPath, TicketFD)) {
    return Err;
  }

  sys::fs::file_status Status;
  if (std::error_code Err = sys::fs::status(TicketFD, Status)) {
    return Err;
  }
  uint64_t FileSize = Status.getSize();
  if (FileSize != TicketFileSize) {
    error("File \"" + OutputFilename + "\" was not a Repo ticket file");
  }

  ErrorOr<std::unique_ptr<MemoryBuffer>> MemberBufferOrErr =
      MemoryBuffer::getOpenFile(TicketFD, TicketPath, FileSize, false);
  if (!MemberBufferOrErr) {
    return MemberBufferOrErr.getError();
  }

  if (close(TicketFD) != 0) {
    return std::error_code(errno, std::generic_category());
  }

  StringRef Contents = MemberBufferOrErr.get()->getBuffer();
  assert(Contents.size() == TicketFileSize);

  StringRef Signature = Contents.slice(0, 8);
  if (Signature != "RepoUuid") {
    error("File \"" + OutputFilename + "\" was not a Repo ticket file");
  }

  pstore::uuid::container_type UuidBytes;
  StringRef Bytes = Contents.substr(8, pstore::uuid::elements);
  assert(Bytes.size() == pstore::uuid::elements);
  std::copy(std::begin(Bytes), std::end(Bytes), std::begin(UuidBytes));
  return {UuidBytes};
}

template <class ELFT> struct ELFState {
  using Elf_Word = typename object::ELFFile<ELFT>::Elf_Word;
  using Elf_Ehdr = typename object::ELFFile<ELFT>::Elf_Ehdr;
  using Elf_Phdr = typename object::ELFFile<ELFT>::Elf_Phdr;
  using Elf_Shdr = typename object::ELFFile<ELFT>::Elf_Shdr;
  using Elf_Sym = typename object::ELFFile<ELFT>::Elf_Sym;
  using Elf_Rel = typename object::ELFFile<ELFT>::Elf_Rel;
  using Elf_Rela = typename object::ELFFile<ELFT>::Elf_Rela;

  using SectionHeaderTable = std::vector<Elf_Shdr>;
  SectionHeaderTable SectionHeaders;
  std::map<SectionId, OutputSection<ELFT>> Sections;
  std::map<pstore::address, GroupInfo<ELFT>> Groups;

  StringTable Strings;
  SymbolTable<ELFT> Symbols;

  explicit ELFState(pstore::database &Db) : Symbols{Strings} {}

  void initELFHeader(Elf_Ehdr &Header);
  void initStandardSections();
  uint64_t writeSectionHeaders(raw_ostream &OS);

  void buildGroupSection(pstore::database &Db, GroupInfo<ELFT> &GI);

  /// Writes the group section data that was recorded by earlier calls to
  /// buildGroupSection(). The group section headers are updated to record the
  /// location and and size of this data.
  void writeGroupSections(raw_ostream &OS);
};

template <class ELFT> void ELFState<ELFT>::initELFHeader(Elf_Ehdr &Header) {
  using namespace llvm::ELF;
  Header.e_ident[EI_MAG0] = 0x7f;
  Header.e_ident[EI_MAG1] = 'E';
  Header.e_ident[EI_MAG2] = 'L';
  Header.e_ident[EI_MAG3] = 'F';
  Header.e_ident[EI_CLASS] = ELFT::Is64Bits ? ELFCLASS64 : ELFCLASS32;
  Header.e_ident[EI_DATA] =
      (ELFT::TargetEndianness == support::little) ? ELFDATA2LSB : ELFDATA2MSB;
  Header.e_ident[EI_VERSION] = EV_CURRENT;
  Header.e_ident[EI_OSABI] = ELFOSABI_NONE;
  Header.e_ident[EI_ABIVERSION] = 0;
  Header.e_type = ET_REL;
  Header.e_machine =
      EM_X86_64; // FIXME: where do we represent the machine type?
  Header.e_version = EV_CURRENT;
  Header.e_entry = 0;
  Header.e_phoff = 0;
  Header.e_shoff = 0; // patched up later
  Header.e_flags = 0;
  Header.e_ehsize = sizeof(Elf_Ehdr);
  Header.e_phentsize = sizeof(Elf_Phdr);
  Header.e_phnum = 0;
  Header.e_shentsize = sizeof(Elf_Shdr);
  Header.e_shnum = 0; // patched up later.
  Header.e_shstrndx = SectionIndices::StringTab;
}

template <typename ELFT> void ELFState<ELFT>::initStandardSections() {
  // null section
  Elf_Shdr SH;
  zero(SH);
  assert(SectionHeaders.size() == SectionIndices::Null);
  SectionHeaders.push_back(SH);

  // string table
  zero(SH);
  SH.sh_name = Strings.insert(stringToSStringView(".strtab"));
  SH.sh_type = ELF::SHT_STRTAB;
  assert(SectionHeaders.size() == SectionIndices::StringTab);
  SectionHeaders.push_back(SH);

  // Symbol table
  zero(SH);
  SH.sh_name = Strings.insert(stringToSStringView(".symtab"));
  SH.sh_type = ELF::SHT_SYMTAB;
  SH.sh_link = SectionIndices::StringTab;
  SH.sh_entsize = sizeof(ELFState<ELFT>::Elf_Sym);
  SH.sh_addralign = alignof(ELFState<ELFT>::Elf_Sym);
  assert(SectionHeaders.size() == SectionIndices::SymTab);
  SectionHeaders.push_back(SH);
}

template <typename ELFT>
uint64_t ELFState<ELFT>::writeSectionHeaders(raw_ostream &OS) {
  writeAlignmentPadding<Elf_Shdr>(OS);
  auto const Offset = OS.tell();
  OS.write(reinterpret_cast<char const *>(SectionHeaders.data()),
           SectionHeaders.size() * sizeof(Elf_Shdr));
  return Offset;
}

// The ELF spec. requires the groups sections to appear before the sections that
// they contain in the section header table. For this reason, we have to create
// them in two passes: the first creates the headers; later, once we know the
// collections of the section indices that they'll contain, we can write the
// group section bodies.
//
// A further wrinkle is that the entry in the section header table contains the
// index of the group's "signature" symbol. We therefore must have already
// generated and sorted the symbol table to assign indices.
template <typename ELFT>
void ELFState<ELFT>::buildGroupSection(pstore::database &Db,
                                       GroupInfo<ELFT> &GI) {
  // If we haven't yet recorded a section index for this group, then build one
  // now.
  if (GI.SectionIndex == 0U) {
    auto SignatureSymbol =
        Symbols.findSymbol(getString(Db, GI.IdentifyingSymbol));
    assert(SignatureSymbol != nullptr &&
           SignatureSymbol->Index != llvm::ELF::STN_UNDEF);
    static auto const GroupString = stringToSStringView(".group");
    ELFState<ELFT>::Elf_Shdr SH;
    zero(SH);
    SH.sh_name = Strings.insert(GroupString);
    SH.sh_type = ELF::SHT_GROUP;
    SH.sh_link = SectionIndices::SymTab;
    SH.sh_info = SignatureSymbol->Index; // The group's signature symbol entry.
    SH.sh_entsize = sizeof(ELF::Elf32_Word);
    SH.sh_addralign = alignof(ELF::Elf32_Word);

    GI.SectionIndex = SectionHeaders.size();
    SectionHeaders.push_back(SH);
  }
}

template <typename ELFT>
void ELFState<ELFT>::writeGroupSections(raw_ostream &OS) {
  for (auto const &G : Groups) {
    writeAlignmentPadding<ELF::Elf32_Word>(OS);
    auto const StartPos = OS.tell();
    auto NumWords = 1U;
    writeRaw(OS, ELF::Elf32_Word{ELF::GRP_COMDAT});

    for (OutputSection<ELFT> const *GroupMember : G.second.Members) {
      auto SectionIndex = GroupMember->getIndex();
      writeRaw(OS, ELF::Elf32_Word{SectionIndex});
      ++NumWords;

      if (GroupMember->numRelocations() > 0) {
        writeRaw(OS, ELF::Elf32_Word{SectionIndex + 1});
        ++NumWords;
      }
    }

    auto const SectionSize = NumWords * sizeof(ELF::Elf32_Word);
    assert(OS.tell() - StartPos == SectionSize);

    assert(G.second.SectionIndex < SectionHeaders.size());
    Elf_Shdr &SH = SectionHeaders[G.second.SectionIndex];
    assert(SH.sh_type == ELF::SHT_GROUP);
    SH.sh_offset = StartPos;
    SH.sh_size = SectionSize;
  }
}

namespace {

class SpecialNames {
public:
  void initialize(pstore::database &Db);

  pstore::address CtorName = pstore::address::null();
  pstore::address DtorName = pstore::address::null();

private:
  static pstore::address findString(pstore::index::name_index const &NameIndex,
                                    std::string str);
};

void SpecialNames::initialize(pstore::database &Db) {
  pstore::index::name_index const *const NameIndex =
      pstore::index::get_name_index(Db);
  if (!NameIndex) {
    errs() << "Warning: name index was not found.\n";
  } else {
    // Get the address of the global tors names from the names set. If the
    // string is missing, use null since we know that can't appear as a ticket's
    // name.
    CtorName = findString(*NameIndex, "llvm.global_ctors");
    DtorName = findString(*NameIndex, "llvm.global_dtors");
  }
}

pstore::address
SpecialNames::findString(pstore::index::name_index const &NameIndex,
                         std::string str) {
  auto Pos = NameIndex.find(
      pstore::sstring_view<char const *>{str.data(), str.length()});
  return (Pos != NameIndex.end()) ? Pos.get_address() : pstore::address::null();
}

} // anonymous namespace

static ELFSectionType getELFSectionType(pstore::repo::section_type T,
                                        pstore::address Name,
                                        SpecialNames const &Magics) {
  if (Name == Magics.CtorName) {
    return ELFSectionType::init_array;
  } else if (Name == Magics.DtorName) {
    return ELFSectionType::fini_array;
  }

#define X(a)                                                                   \
  case (pstore::repo::section_type::a):                                        \
    return (ELFSectionType::a);
  switch (T) { PSTORE_REPO_SECTION_TYPES }
#undef X
  llvm_unreachable("getELFSectionType: unknown repository section kind.");
}

static std::string getRepoPath() {
  if (RepoPath.getNumOccurrences() == 0) {
    // TODO: remove this envrionment variable once the matching behavior is
    // removed from the compiler.
    if (auto File = getenv("REPOFILE")) {
      return {File};
    }
  }
  return RepoPath;
}

raw_ostream &operator<<(raw_ostream &OS, pstore::index::digest const &Digest) {
  return OS << Digest.to_hex_string();
}

int main(int argc, char *argv[]) {
  cl::ParseCommandLineOptions(argc, argv);

  std::error_code EC;
  std::unique_ptr<ToolOutputFile> Out(
      new ToolOutputFile(OutputFilename, EC, sys::fs::F_None));
  if (EC) {
    error("repo2obj: Error opening '" + OutputFilename + "': " + EC.message());
  }

  ErrorOr<pstore::uuid> UuidOrError = getTicketFileUuid(TicketPath);
  if (!UuidOrError) {
    errs() << "Error: '" << TicketPath << "' ("
           << UuidOrError.getError().message() << ")\n";
    return EXIT_FAILURE;
  }

  pstore::uuid const &Uuid = UuidOrError.get();
  DEBUG(dbgs() << "'" << TicketPath << "' : " << Uuid.str() << '\n');

  pstore::database Db(getRepoPath(), pstore::database::access_mode::read_only);
  pstore::index::ticket_index const *const TicketIndex =
      pstore::index::get_ticket_index(Db);
  if (!TicketIndex) {
    errs() << "Error: ticket index was not found.\n";
    return EXIT_FAILURE;
  }
  pstore::index::digest_index const *const FragmentIndex =
      pstore::index::get_digest_index(Db);
  if (!FragmentIndex) {
    errs() << "Error: fragment index was not found.\n";
    return EXIT_FAILURE;
  }

  auto TicketPos = TicketIndex->find(UuidOrError.get());
  if (TicketPos == TicketIndex->end()) {
    errs() << "Error: ticket " << Uuid.str() << " was not found.\n";
    return EXIT_FAILURE;
  }

  SpecialNames Magics;
  Magics.initialize(Db);

  using ELFT = ELF64LE;
  ELFState<ELF64LE> State(Db);

  {
    std::vector<OutputSection<ELFT>::SectionInfo> OutputSections;
    OutputSections.resize(::pstore::repo::fragment::member_array::max_size());

    auto Ticket = pstore::repo::ticket::load(Db, TicketPos->second);
    for (auto const &TM : *Ticket) {
      assert(TM.name != pstore::address::null());
      DEBUG(dbgs() << "Processing: " << getString(Db, TM.name) << '\n');

      auto const FragmentPos = FragmentIndex->find(TM.digest);
      if (FragmentPos == FragmentIndex->end()) {
        errs() << "Error: fragment " << TM.digest << " was not found.\n";
        return EXIT_FAILURE;
      }

      std::fill(std::begin(OutputSections), std::end(OutputSections),
                OutputSection<ELFT>::SectionInfo{});

      auto const IsLinkOnce =
          TM.linkage == pstore::repo::linkage_type::linkonce;
      // TODO: enable the name discriminator if "function/data sections mode" is
      // enabled.
      auto const Discriminator = IsLinkOnce ? TM.name : pstore::address::null();
      auto const Fragment =
          pstore::repo::fragment::load(Db, FragmentPos->second);

      if (TM.linkage == pstore::repo::linkage_type::common) {
        SString Name = getString(Db, TM.name);

        if (Fragment->num_sections() != 1U ||
            !Fragment->has_section(pstore::repo::section_type::bss)) {
          error("Fragment for common symbol \"" + std::string{Name} +
                "\" did not contain a sole BSS section");
        }
        pstore::repo::section const &S =
            (*Fragment)[pstore::repo::section_type::bss];
        State.Symbols.insertSymbol(Name, nullptr /*no output section*/,
                                   0 /*offset*/, S.data().size(), TM.linkage);
        continue;
      }
      // Go through the sections that this fragment contains create the
      // corresponding ELF section(s) as necessary.
      for (auto const Key : Fragment->sections().get_indices()) {
        // The section type and "discriminator" together identify the ELF output
        // section to which this fragment's section data will be appended.
        auto const SectionType = static_cast<pstore::repo::section_type>(Key);
        auto const Id = std::make_tuple(
            getELFSectionType(SectionType, TM.name, Magics), Discriminator);

        decltype(State.Sections)::iterator Pos;
        bool DidInsert;
        std::tie(Pos, DidInsert) =
            State.Sections.emplace(Id, OutputSection<ELFT>(Db, Id));

        OutputSection<ELFT> *const OSection = &Pos->second;

        // If this is the first time that we've wanted to append to the ELF
        // section described by 'Id' and the ticket-members has linkonce
        // linkage, then we need to make the section a member of a group
        // section.
        if (DidInsert && IsLinkOnce) {
          decltype(State.Groups)::iterator GroupPos =
              State.Groups.find(TM.name);
          if (GroupPos == State.Groups.end()) {
            bool _;
            std::tie(GroupPos, _) =
                State.Groups.emplace(TM.name, GroupInfo<ELFT>(TM.name));
          }

          GroupPos->second.Members.push_back(OSection);

          // Tell the output section about the group of which it's a member.
          OSection->attachToGroup(&GroupPos->second);
        }

        // Record the location that the later call to section()->append() will
        // assign to this data. We need to account for any alignment padding
        // that that function may place before the data itself (hence the call
        // to alignedContributionSize()).
        OutputSections[Key] = OutputSection<ELFT>::SectionInfo(
            OSection, OSection->alignedContributionSize(
                          (*Fragment)[SectionType].align()));
      }

      // This can't currently be folded into the first loop because the need the
      // OutputSections array to be built.
      for (auto const Key : Fragment->sections().get_indices()) {
        pstore::repo::section const &Section =
            (*Fragment)[static_cast<pstore::repo::section_type>(Key)];
        OutputSections[Key].section()->append(
            TM, SectionPtr{std::static_pointer_cast<void const>(Fragment),
                           &Section},
            State.Symbols, OutputSections);
      }
    }
  }

  DEBUG(dbgs() << "There are " << State.Groups.size() << " groups\n");

  std::vector<SymbolTable<ELFT>::Value *> OrderedSymbols = State.Symbols.sort();

  decltype(State)::Elf_Ehdr Header;
  State.initELFHeader(Header);
  State.initStandardSections();

  auto &OS = Out->os();
  writeRaw(OS, Header);

  for (auto &S : State.Sections) {
    OutputSection<ELFT> &Section = S.second;
    if (GroupInfo<ELFT> *const Group = Section.group()) {
      State.buildGroupSection(Db, *Group);
    }
    Section.setIndex(State.SectionHeaders.size());
    Section.write(OS, State.Strings, std::back_inserter(State.SectionHeaders));
  }

  State.writeGroupSections(OS);

  // Write the string table (and patch its section header)
  {
    auto &S = State.SectionHeaders[SectionIndices::StringTab];
    std::tie(S.sh_offset, S.sh_size) = State.Strings.write(OS);
  }
  // Now do the same for the symbol table.
  {
    auto &S = State.SectionHeaders[SectionIndices::SymTab];
    // st_info should be one greater than the symbol table index of the last
    // local symbol (binding STB_LOCAL).
    S.sh_info = SymbolTable<ELFT>::firstNonLocal(OrderedSymbols);
    std::tie(S.sh_offset, S.sh_size) = State.Symbols.write(OS, OrderedSymbols);
  }

  uint64_t SectionHeadersOffset = State.writeSectionHeaders(OS);
  Header.e_shoff = SectionHeadersOffset;
  Header.e_shnum = State.SectionHeaders.size();
  Header.e_shstrndx = SectionIndices::StringTab;

  OS.seek(0);
  writeRaw(OS, Header);

  //  if (Res == 0)
  Out->keep();

  Out->os().flush();

  return EXIT_SUCCESS;
}
