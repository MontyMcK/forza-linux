#include "loader/pe_image.h"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>

#include "common/log.h"
#include "loader/pe_structs.h"

namespace fl::loader {
namespace {

PeImage g_main;

std::vector<uint8_t> read_file(const std::string& path) {
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) fatal("cannot open %s: %s", path.c_str(), strerror(errno));
    struct stat st;
    fstat(fd, &st);
    std::vector<uint8_t> buf(st.st_size);
    size_t off = 0;
    while (off < buf.size()) {
        ssize_t n = read(fd, buf.data() + off, buf.size() - off);
        if (n <= 0) fatal("read %s failed", path.c_str());
        off += n;
    }
    close(fd);
    return buf;
}

void apply_relocations(PeImage& img, const ImageNtHeaders64* nt) {
    int64_t delta = reinterpret_cast<int64_t>(img.base) - static_cast<int64_t>(img.preferred_base);
    if (delta == 0) return;
    const auto& dir = nt->OptionalHeader.DataDirectory[kDirBaseReloc];
    if (!dir.VirtualAddress) fatal("image needs relocation but has no .reloc");
    uint32_t off = 0;
    size_t count = 0;
    while (off < dir.Size) {
        auto* block = img.at<ImageBaseRelocation>(dir.VirtualAddress + off);
        if (block->SizeOfBlock == 0) break;
        auto* entries = reinterpret_cast<uint16_t*>(block + 1);
        size_t n = (block->SizeOfBlock - sizeof(ImageBaseRelocation)) / 2;
        for (size_t i = 0; i < n; i++) {
            uint16_t type = entries[i] >> 12, rel = entries[i] & 0xFFF;
            if (type == kRelBasedAbsolute) continue;
            if (type != kRelBasedDir64) fatal("unsupported relocation type %u", type);
            *img.at<int64_t>(block->VirtualAddress + rel) += delta;
            count++;
        }
        off += block->SizeOfBlock;
    }
    FL_LOG(loader, "applied %zu relocations (delta %+lld)", count, static_cast<long long>(delta));
}

int section_prot(uint32_t chr) {
    int prot = 0;
    if (chr & kScnMemRead) prot |= PROT_READ;
    if (chr & kScnMemWrite) prot |= PROT_WRITE;
    if (chr & kScnMemExecute) prot |= PROT_EXEC | PROT_READ;
    return prot ? prot : PROT_READ;
}

}  // namespace

PeImage& main_image() { return g_main; }

PeImage map_image(const std::string& path) {
    std::vector<uint8_t> file = read_file(path);
    auto* dos = reinterpret_cast<ImageDosHeader*>(file.data());
    if (file.size() < sizeof(ImageDosHeader) || dos->e_magic != 0x5A4D) fatal("%s: not an MZ file", path.c_str());
    auto* nt = reinterpret_cast<ImageNtHeaders64*>(file.data() + dos->e_lfanew);
    if (nt->Signature != 0x4550 || nt->OptionalHeader.Magic != 0x20B || nt->FileHeader.Machine != 0x8664)
        fatal("%s: not a PE32+ AMD64 image", path.c_str());

    const auto& oh = nt->OptionalHeader;
    PeImage img;
    img.path = path;
    img.preferred_base = oh.ImageBase;
    img.size = oh.SizeOfImage;
    img.entry_rva = oh.AddressOfEntryPoint;
    img.tls_dir_rva = oh.DataDirectory[kDirTls].VirtualAddress;
    img.import_dir_rva = oh.DataDirectory[kDirImport].VirtualAddress;
    img.import_dir_size = oh.DataDirectory[kDirImport].Size;
    img.exception_dir_rva = oh.DataDirectory[kDirException].VirtualAddress;
    img.exception_dir_size = oh.DataDirectory[kDirException].Size;
    img.stack_reserve = oh.SizeOfStackReserve;

    // Try the preferred base first; the game was linked for it.
    void* want = reinterpret_cast<void*>(oh.ImageBase);
    void* mem = mmap(want, img.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (mem == MAP_FAILED) {
        FL_ERROR("preferred base %p unavailable (%s), relocating", want, strerror(errno));
        mem = mmap(nullptr, img.size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem == MAP_FAILED) fatal("mmap of image failed: %s", strerror(errno));
    }
    img.base = static_cast<uint8_t*>(mem);

    memcpy(img.base, file.data(), oh.SizeOfHeaders);
    auto* sec = reinterpret_cast<ImageSectionHeader*>(reinterpret_cast<uint8_t*>(&nt->OptionalHeader) +
                                                      nt->FileHeader.SizeOfOptionalHeader);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t raw = std::min(sec[i].SizeOfRawData, sec[i].VirtualSize ? sec[i].VirtualSize : sec[i].SizeOfRawData);
        if (sec[i].PointerToRawData + raw > file.size()) fatal("section %d raw data out of file", i);
        memcpy(img.base + sec[i].VirtualAddress, file.data() + sec[i].PointerToRawData, raw);
        char name[9] = {};
        memcpy(name, sec[i].Name, 8);
        FL_LOG(loader, "section %-8s rva %08x vsize %08x chr %08x", name, sec[i].VirtualAddress, sec[i].VirtualSize,
               sec[i].Characteristics);
    }

    apply_relocations(img, nt);
    FL_LOG(loader, "mapped %s at %p (size 0x%x, entry rva 0x%x)", path.c_str(), img.base, img.size, img.entry_rva);
    return img;
}

void protect_image(PeImage& img) {
    auto* dos = img.at<ImageDosHeader>(0);
    auto* nt = img.at<ImageNtHeaders64>(dos->e_lfanew);
    auto* sec = reinterpret_cast<ImageSectionHeader*>(reinterpret_cast<uint8_t*>(&nt->OptionalHeader) +
                                                      nt->FileHeader.SizeOfOptionalHeader);
    mprotect(img.base, 0x1000, PROT_READ);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        uint32_t start = sec[i].VirtualAddress & ~0xFFFu;
        uint32_t end = (sec[i].VirtualAddress + std::max(sec[i].VirtualSize, sec[i].SizeOfRawData) + 0xFFF) & ~0xFFFu;
        if (mprotect(img.base + start, end - start, section_prot(sec[i].Characteristics)) != 0)
            fatal("mprotect section %d failed: %s", i, strerror(errno));
    }
}

}  // namespace fl::loader
