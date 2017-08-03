//*   __                                      _    *
//*  / _|_ __ __ _  __ _ _ __ ___   ___ _ __ | |_  *
//* | |_| '__/ _` |/ _` | '_ ` _ \ / _ \ '_ \| __| *
//* |  _| | | (_| | (_| | | | | | |  __/ | | | |_  *
//* |_| |_|  \__,_|\__, |_| |_| |_|\___|_| |_|\__| *
//*                |___/                           *
#ifndef LLVM_MCREPOFRAGMENT_FRAGMENT_H
#define LLVM_MCREPOFRAGMENT_FRAGMENT_H

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <type_traits>
#include <vector>

#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/raw_ostream.h"

#include "MCRepoAligned.h"
#include "MCRepoSparseArray.h"

namespace llvm {
namespace repo {

//*  ___     _                     _ ___ _                *
//* |_ _|_ _| |_ ___ _ _ _ _  __ _| | __(_)_ ___  _ _ __  *
//*  | || ' \  _/ -_) '_| ' \/ _` | | _|| \ \ / || | '_ \ *
//* |___|_||_\__\___|_| |_||_\__,_|_|_| |_/_\_\\_,_| .__/ *
//*                                                |_|    *
struct InternalFixup {
  std::uint8_t Section;
  std::uint8_t Type;
  std::uint16_t Padding;
  std::uint32_t Offset;
  std::uint32_t Addend;
};

static_assert(std::is_standard_layout<InternalFixup>::value,
              "InternalFixup must satisfy StandardLayoutType");

static_assert(offsetof(InternalFixup, Section) == 0,
              "Section offset differs from expected value");
static_assert(offsetof(InternalFixup, Type) == 1,
              "Type offset differs from expected value");
static_assert(offsetof(InternalFixup, Padding) == 2,
              "Padding offset differs from expected value");
static_assert(offsetof(InternalFixup, Offset) == 4,
              "Offset offset differs from expected value");
static_assert(offsetof(InternalFixup, Addend) == 8,
              "Addend offset differs from expected value");
static_assert(sizeof(InternalFixup) == 12,
              "InternalFixup size does not match expected");

//*  ___     _                     _ ___ _                *
//* | __|_ _| |_ ___ _ _ _ _  __ _| | __(_)_ ___  _ _ __  *
//* | _|\ \ /  _/ -_) '_| ' \/ _` | | _|| \ \ / || | '_ \ *
//* |___/_\_\\__\___|_| |_||_\__,_|_|_| |_/_\_\\_,_| .__/ *
//*                                                |_|    *
struct ExternalFixup {
  char const *Name;
  std::uint8_t Type;
  // FIXME: much padding here.
  std::uint64_t Offset;
  std::uint64_t Addend;
};

static_assert(std::is_standard_layout<ExternalFixup>::value,
              "ExternalFixup must satisfy StandardLayoutType");
static_assert(offsetof(ExternalFixup, Name) == 0,
              "Name offset differs from expected value");
static_assert(offsetof(ExternalFixup, Type) == 8,
              "Type offset differs from expected value");
static_assert(offsetof(ExternalFixup, Offset) == 16,
              "Offset offset differs from expected value");
static_assert(offsetof(ExternalFixup, Addend) == 24,
              "Addend offset differs from expected value");
// static_assert (offsetof (external_fixup, padding3) == 20, "padding3 offset
// differs from expected value");
static_assert(sizeof(ExternalFixup) == 32,
              "ExternalFixup size does not match expected");

//*  ___         _   _           *
//* / __| ___ __| |_(_)___ _ _   *
//* \__ \/ -_) _|  _| / _ \ ' \  *
//* |___/\___\__|\__|_\___/_||_| *
//*                              *
class Section {
  friend struct SectionCheck;

public:
  /// Describes the three members of a Section as three pairs of iterators: one
  /// each for the data, internal fixups, and external fixups ranges.
  template <typename DataRangeType, typename IFixupRangeType,
            typename XFixupRangeType>
  struct Sources {
    DataRangeType DataRange;
    IFixupRangeType IfixupsRange;
    XFixupRangeType XfixupsRange;
  };

  template <typename DataRange, typename IFixupRange, typename XFixupRange>
  static inline auto makeSources(DataRange const &D, IFixupRange const &I,
                                 XFixupRange const &X)
      -> Sources<DataRange, IFixupRange, XFixupRange> {
    return {D, I, X};
  }

  template <typename DataRange, typename IFixupRange, typename XFixupRange>
  Section(DataRange const &D, IFixupRange const &I, XFixupRange const &X);

  template <typename DataRange, typename IFixupRange, typename XFixupRange>
  Section(Sources<DataRange, IFixupRange, XFixupRange> const &Src)
      : Section(Src.DataRange, Src.IfixupsRange, Src.XfixupsRange) {}

  Section(Section const &) = delete;
  Section &operator=(Section const &) = delete;
  Section(Section &&) = delete;
  Section &operator=(Section &&) = delete;

  /// A simple wrapper around the elements of one of the three arrays that make
  /// up a section. Enables the use of standard algorithms as well as
  /// range-based for loops on these collections.
  template <typename ValueType> class Container {
  public:
    using value_type = ValueType const;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = ValueType const &;
    using const_reference = reference;
    using pointer = ValueType const *;
    using const_pointer = pointer;
    using iterator = const_pointer;
    using const_iterator = iterator;

    Container(const_pointer Begin, const_pointer End)
        : Begin_{Begin}, End_{End} {
      assert(End >= Begin);
    }
    iterator begin() const { return Begin_; }
    iterator end() const { return End_; }
    const_iterator cbegin() const { return begin(); }
    const_iterator cend() const { return end(); }

    size_type size() const { return End_ - Begin_; }

  private:
    const_pointer Begin_;
    const_pointer End_;
  };

  Container<std::uint8_t> data() const {
    auto Begin = alignedPtr<std::uint8_t>(this + 1);
    return {Begin, Begin + DataSize_};
  }
  Container<InternalFixup> ifixups() const {
    auto Begin = alignedPtr<InternalFixup>(data().end());
    return {Begin, Begin + NumIfixups_};
  }
  Container<ExternalFixup> xfixups() const {
    auto Begin = alignedPtr<ExternalFixup>(ifixups().end());
    return {Begin, Begin + NumXfixups_};
  }

  ///@{
  /// \brief A group of member functions which return the number of bytes
  /// occupied by a fragment instance.

  /// \returns The number of bytes occupied by this fragment section.
  std::size_t sizeBytes() const;

  /// \returns The number of bytes needed to accommodate a fragment section with
  /// the given number of data bytes and fixups.
  static std::size_t sizeBytes(std::size_t DataSize, std::size_t NumIfixups,
                               std::size_t NumXfixups);

  template <typename DataRange, typename IFixupRange, typename XFixupRange>
  static std::size_t sizeBytes(DataRange const &D, IFixupRange const &I,
                               XFixupRange const &X);

  template <typename DataRange, typename IFixupRange, typename XFixupRange>
  static std::size_t
  sizeBytes(Sources<DataRange, IFixupRange, XFixupRange> const &Src) {
    return sizeBytes(Src.DataRange, Src.IfixupsRange, Src.XfixupsRange);
  }
  ///@}

private:
  std::uint32_t NumIfixups_ = 0;
  std::uint32_t NumXfixups_ = 0;
  std::uint64_t DataSize_ = 0;

  /// A helper function which returns the distance between two iterators,
  /// clamped to the maximum range of IntType.
  template <typename IntType, typename Iterator>
  static IntType setSize(Iterator First, Iterator Last);

  /// Calculates the size of a region in the section including any necessary
  /// preceeding alignment bytes.
  /// \param Pos The starting offset within the section.
  /// \param Num The number of instance of type Ty.
  /// \returns Number of bytes occupied by the elements.
  template <typename Ty>
  static inline std::size_t partSizeBytes(std::size_t Pos, std::size_t Num) {
    if (Num > 0) {
      Pos = aligned<Ty>(Pos) + Num * sizeof(Ty);
    }
    return Pos;
  }
};

static_assert(std::is_standard_layout<Section>::value,
              "Section must satisfy StandardLayoutType");

/// A trivial class which exists solely to verify the layout of the Section type
/// (including its private member variables and is declared as a friend of it.
struct SectionCheck {
  static_assert(offsetof(Section, NumIfixups_) == 0,
                "NumIfixups_ offset differs from expected value");
  static_assert(offsetof(Section, NumXfixups_) == 4,
                "NumXfixups_ offset differs from expected value");
  static_assert(offsetof(Section, DataSize_) == 8,
                "DataSize_ offset differs from expected value");
  static_assert(sizeof(Section) == 16, "Section size does not match expected");
};

// (ctor)
// ~~~~~~
template <typename DataRange, typename IFixupRange, typename XFixupRange>
Section::Section(DataRange const &D, IFixupRange const &I,
                 XFixupRange const &X) {
  auto const Start = reinterpret_cast<std::uint8_t *>(this);
  auto P = reinterpret_cast<std::uint8_t *>(this + 1);

  if (D.first != D.second) {
    P = std::copy(D.first, D.second, alignedPtr<std::uint8_t>(P));
    DataSize_ = Section::setSize<decltype(DataSize_)>(D.first, D.second);
  }
  if (I.first != I.second) {
    P = reinterpret_cast<std::uint8_t *>(
        std::copy(I.first, I.second, alignedPtr<InternalFixup>(P)));
    NumIfixups_ = Section::setSize<decltype(NumIfixups_)>(I.first, I.second);
  }
  if (X.first != X.second) {
    P = reinterpret_cast<std::uint8_t *>(
        std::copy(X.first, X.second, alignedPtr<ExternalFixup>(P)));
    NumXfixups_ = Section::setSize<decltype(NumXfixups_)>(X.first, X.second);
  }
  assert(P >= Start &&
         static_cast<std::size_t>(P - Start) == sizeBytes(D, I, X));
}

// setSize
// ~~~~~~~
template <typename IntType, typename Iterator>
inline IntType Section::setSize(Iterator First, Iterator Last) {
  static_assert(std::is_unsigned<IntType>::value, "IntType must be unsigned");
  auto const Size = std::distance(First, Last);
  assert(Size >= 0);

  auto const USize =
      static_cast<typename std::make_unsigned<decltype(Size)>::type>(Size);
  assert(USize >= std::numeric_limits<IntType>::min());
  assert(USize <= std::numeric_limits<IntType>::max());

  return static_cast<IntType>(Size);
}

// SizeBytes
// ~~~~~~~~~
template <typename DataRange, typename IFixupRange, typename XFixupRange>
std::size_t Section::sizeBytes(DataRange const &D, IFixupRange const &I,
                               XFixupRange const &X) {
  auto const DataSize = std::distance(D.first, D.second);
  auto const NumIfixups = std::distance(I.first, I.second);
  auto const NumXfixups = std::distance(X.first, X.second);
  assert(DataSize >= 0 && NumIfixups >= 0 && NumXfixups >= 0);
  return sizeBytes(static_cast<std::size_t>(DataSize),
                   static_cast<std::size_t>(NumIfixups),
                   static_cast<std::size_t>(NumXfixups));
}

// FIXME: the members of this collection are drawn from
// RepoObjectWriter::writeRepoSectionData(). Not sure it's correct.
#define LLVM_REPO_SECTION_TYPES                                                \
  X(BSS)                                                                       \
  X(Common)                                                                    \
  X(Data)                                                                      \
  X(RelRo)                                                                     \
  X(Text)                                                                      \
  X(Mergeable1ByteCString)                                                     \
  X(Mergeable2ByteCString)                                                     \
  X(Mergeable4ByteCString)                                                     \
  X(MergeableConst4)                                                           \
  X(MergeableConst8)                                                           \
  X(MergeableConst16)                                                          \
  X(MergeableConst32)                                                          \
  X(MergeableConst)                                                            \
  X(ReadOnly)                                                                  \
  X(ThreadBSS)                                                                 \
  X(ThreadData)                                                                \
  X(ThreadLocal)                                                               \
  X(Metadata)

#define X(a) a,
enum class SectionType : std::uint8_t { LLVM_REPO_SECTION_TYPES };
#undef X

struct SectionContent {
  template <typename Iterator> using Range = std::pair<Iterator, Iterator>;

  template <typename Iterator>
  static inline auto makeRange(Iterator Begin, Iterator End)
      -> Range<Iterator> {
    return {Begin, End};
  }

  explicit SectionContent(SectionType St) : Type{St} {}

  SectionType Type;
  SmallVector<char, 128> Data;
  std::vector<InternalFixup> Ifixups;
  std::vector<ExternalFixup> Xfixups;

  auto makeSources() const
      -> Section::Sources<Range<decltype(Data)::const_iterator>,
                          Range<decltype(Ifixups)::const_iterator>,
                          Range<decltype(Xfixups)::const_iterator>> {

    return Section::makeSources(
        makeRange(std::begin(Data), std::end(Data)),
        makeRange(std::begin(Ifixups), std::end(Ifixups)),
        makeRange(std::begin(Xfixups), std::end(Xfixups)));
  }
};

namespace details {

/// An iterator adaptor which produces a value_type of 'SectionType const' from
/// values deferences from the supplied underlying iterator.
template <typename Iterator> class ContentTypeIterator {
public:
  using value_type = SectionType const;
  using difference_type =
      typename std::iterator_traits<Iterator>::difference_type;
  using pointer = value_type *;
  using reference = value_type &;
  using iterator_category = std::input_iterator_tag;

  ContentTypeIterator() : It_{} {}
  explicit ContentTypeIterator(Iterator It) : It_{It} {}
  ContentTypeIterator(ContentTypeIterator const &Rhs) : It_{Rhs.It_} {}
  ContentTypeIterator &operator=(ContentTypeIterator const &Rhs) {
    It_ = Rhs.It_;
    return *this;
  }
  bool operator==(ContentTypeIterator const &Rhs) const {
    return It_ == Rhs.It_;
  }
  bool operator!=(ContentTypeIterator const &Rhs) const {
    return !(operator==(Rhs));
  }
  ContentTypeIterator &operator++() {
    ++It_;
    return *this;
  }
  ContentTypeIterator operator++(int) {
    ContentTypeIterator Old{*this};
    It_++;
    return Old;
  }

  reference operator*() const { return (*It_).Type; }
  pointer operator->() const { return &(*It_).Type; }
  reference operator[](difference_type n) const { return It_[n].Type; }

private:
  Iterator It_;
};

template <typename Iterator>
inline ContentTypeIterator<Iterator> makeContentTypeIterator(Iterator It) {
  return ContentTypeIterator<Iterator>(It);
}

/// An iterator adaptor which produces a value_type of dereferences the
/// value_type of the wrapped iterator.
template <typename Iterator> class SectionContentIterator {
public:
  using value_type = typename std::pointer_traits<
      typename std::pointer_traits<Iterator>::element_type>::element_type;
  using difference_type =
      typename std::iterator_traits<Iterator>::difference_type;
  using pointer = value_type *;
  using reference = value_type &;
  using iterator_category = std::input_iterator_tag;

  SectionContentIterator() : It_{} {}
  explicit SectionContentIterator(Iterator It) : It_{It} {}
  SectionContentIterator(SectionContentIterator const &Rhs) : It_{Rhs.It_} {}
  SectionContentIterator &operator=(SectionContentIterator const &Rhs) {
    It_ = Rhs.It_;
    return *this;
  }
  bool operator==(SectionContentIterator const &Rhs) const {
    return It_ == Rhs.It_;
  }
  bool operator!=(SectionContentIterator const &Rhs) const {
    return !(operator==(Rhs));
  }
  SectionContentIterator &operator++() {
    ++It_;
    return *this;
  }
  SectionContentIterator operator++(int) {
    SectionContentIterator Old{*this};
    It_++;
    return Old;
  }

  reference operator*() const { return **It_; }
  pointer operator->() const { return &(**It_); }
  reference operator[](difference_type n) const { return *(It_[n]); }

private:
  Iterator It_;
};

template <typename Iterator>
inline SectionContentIterator<Iterator>
makeSectionContentIterator(Iterator It) {
  return SectionContentIterator<Iterator>(It);
}

} // end namespace details

//*  ___                            _    *
//* | __| _ __ _ __ _ _ __  ___ _ _| |_  *
//* | _| '_/ _` / _` | '  \/ -_) ' \  _| *
//* |_||_| \__,_\__, |_|_|_\___|_||_\__| *
//*             |___/                    *
class Fragment {
public:
  struct Deleter {
    void operator()(void *P);
  };

  template <typename Iterator>
  static auto make_unique(Iterator First, Iterator Last)
      -> std::unique_ptr<Fragment, Deleter>;

  using MemberArray = SparseArray<std::uint64_t>;

  Section const &operator[](SectionType Key) const;
  std::size_t numSections() const { return Arr_.size(); }
  MemberArray const &sections() const { return Arr_; }

private:
  template <typename IteratorIdx>
  Fragment(IteratorIdx FirstIndex, IteratorIdx LastIndex)
      : Arr_(FirstIndex, LastIndex) {}

  MemberArray Arr_;
};

// make_unique
// ~~~~~~~~~~~
template <typename Iterator>
auto Fragment::make_unique(Iterator First, Iterator Last)
    -> std::unique_ptr<Fragment, Deleter> {
  static_assert(
      (std::is_same<typename std::iterator_traits<Iterator>::value_type,
                    SectionContent>::value),
      "Iterator value_type should be SectionContent");

  auto const NumSections = std::distance(First, Last);
  assert(NumSections >= 0);

  // Compute the number of bytes of storage that we'll need for this fragment.
  auto Size = std::size_t{0};
  Size += decltype(Fragment::Arr_)::size_bytes(NumSections);
  std::for_each(First, Last, [&Size](SectionContent const &C) {
    Size = aligned<Section>(Size);
    Size += Section::sizeBytes(C.makeSources());
  });

  // Allocate sufficient memory for the fragment including its three arrays.
  auto Ptr = std::unique_ptr<std::uint8_t[]>(new std::uint8_t[Size]);
  std::fill(Ptr.get(), Ptr.get() + Size, std::uint8_t{0xFF});

  auto FragmentPtr =
      new (Ptr.get()) Fragment(details::makeContentTypeIterator(First),
                               details::makeContentTypeIterator(Last));
  // Point past the end of the sparse array.
  auto Out = reinterpret_cast<std::uint8_t *>(FragmentPtr) +
             FragmentPtr->Arr_.size_bytes();

  // Copy the contents of each of the segments to the fragment.
  std::for_each(First, Last, [&Out, FragmentPtr](SectionContent const &C) {
    Out = reinterpret_cast<std::uint8_t *>(alignedPtr<Section>(Out));
    auto Scn = new (Out) Section(C.makeSources());
    auto Offset = reinterpret_cast<std::uintptr_t>(Scn) -
                  reinterpret_cast<std::uintptr_t>(FragmentPtr);
    FragmentPtr->Arr_[static_cast<unsigned>(C.Type)] = Offset;
    Out += Scn->sizeBytes();
  });

  assert(Out >= Ptr.get() && static_cast<std::size_t>(Out - Ptr.get()) == Size);
  return {reinterpret_cast<Fragment *>(Ptr.release()), Deleter()};
}

raw_ostream &operator<<(raw_ostream &OS, llvm::repo::SectionType T);
raw_ostream &operator<<(raw_ostream &OS, llvm::repo::InternalFixup const &ifx);
raw_ostream &operator<<(raw_ostream &OS, llvm::repo::ExternalFixup const &Xfx);
raw_ostream &operator<<(raw_ostream &OS, llvm::repo::Section const &Src);
raw_ostream &operator<<(raw_ostream &OS, llvm::repo::Fragment const &F);

} // end namespace repo
} // end namespace llvm

#endif // LLVM_MCREPOFRAGMENT_FRAGMENT_H
// eof:fragment.hpp
