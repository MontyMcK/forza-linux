// Minimal PE/COFF on-disk structures (public Microsoft PE format spec).
#pragma once

#include <cstdint>

namespace fl::loader {

#pragma pack(push, 4)
struct ImageDosHeader {
    uint16_t e_magic;
    uint16_t pad[29];
    int32_t e_lfanew;
};

struct ImageFileHeader {
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
};

struct ImageDataDirectory {
    uint32_t VirtualAddress;
    uint32_t Size;
};

struct ImageOptionalHeader64 {
    uint16_t Magic;
    uint8_t MajorLinkerVersion, MinorLinkerVersion;
    uint32_t SizeOfCode, SizeOfInitializedData, SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment, FileAlignment;
    uint16_t MajorOperatingSystemVersion, MinorOperatingSystemVersion;
    uint16_t MajorImageVersion, MinorImageVersion;
    uint16_t MajorSubsystemVersion, MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve, SizeOfStackCommit;
    uint64_t SizeOfHeapReserve, SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
    ImageDataDirectory DataDirectory[16];
};

struct ImageNtHeaders64 {
    uint32_t Signature;
    ImageFileHeader FileHeader;
    ImageOptionalHeader64 OptionalHeader;
};

struct ImageSectionHeader {
    uint8_t Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations, PointerToLinenumbers;
    uint16_t NumberOfRelocations, NumberOfLinenumbers;
    uint32_t Characteristics;
};

struct ImageBaseRelocation {
    uint32_t VirtualAddress;
    uint32_t SizeOfBlock;
};

struct ImageImportDescriptor {
    uint32_t OriginalFirstThunk;  // INT rva
    uint32_t TimeDateStamp;
    uint32_t ForwarderChain;
    uint32_t Name;
    uint32_t FirstThunk;  // IAT rva
};
#pragma pack(pop)

struct ImageTlsDirectory64 {
    uint64_t StartAddressOfRawData;
    uint64_t EndAddressOfRawData;
    uint64_t AddressOfIndex;
    uint64_t AddressOfCallBacks;
    uint32_t SizeOfZeroFill;
    uint32_t Characteristics;
};

constexpr int kDirExport = 0;
constexpr int kDirImport = 1;
constexpr int kDirException = 3;
constexpr int kDirBaseReloc = 5;
constexpr int kDirTls = 9;

constexpr uint16_t kRelBasedAbsolute = 0;
constexpr uint16_t kRelBasedDir64 = 10;

constexpr uint32_t kScnMemExecute = 0x20000000;
constexpr uint32_t kScnMemRead = 0x40000000;
constexpr uint32_t kScnMemWrite = 0x80000000;

}  // namespace fl::loader
