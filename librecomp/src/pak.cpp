#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <filesystem>
#include <mutex>
#include <vector>

#include "ultramodern/ultra64.h"
#include "ultramodern/ultramodern.hpp"

#include "recomp.h"
#include "helpers.hpp"

extern std::filesystem::path config_path;

namespace {

constexpr s32 PFS_NO_ERROR       = 0;
constexpr s32 PFS_ERR_NOPACK     = 1;
constexpr s32 PFS_ERR_INVALID    = 5;
constexpr s32 PFS_ERR_DATA_FULL  = 7;

constexpr int      kMaxNotes   = 16;
constexpr uint32_t kPageSize   = 256;
constexpr uint32_t kTotalPages = 123;   // free pages on a real 32KB pak

struct VNote {
    bool     used = false;
    uint16_t company = 0;
    uint32_t game_code = 0;
    uint8_t  game_name[16] = {};
    uint8_t  ext_name[4] = {};
    uint64_t last_write = 0;   // unix time of last allocate/write (0 = unknown/legacy)
    std::vector<uint8_t> data;
};

struct VPak {
    VNote notes[kMaxNotes];
    bool  loaded = false;
    std::mutex mutex;

    uint32_t pages_used() const {
        uint32_t p = 0;
        for (const VNote& n : notes) {
            if (n.used) p += (uint32_t)((n.data.size() + kPageSize - 1) / kPageSize);
        }
        return p;
    }
};

VPak g_pak;

std::filesystem::path pak_path() {
    return config_path / "saves" / "controller_pak.bin";
}

constexpr char kMagicV1[8] = { 'T','P','A','K','0','1','\0','\0' };
constexpr char kMagicV2[8] = { 'T','P','A','K','0','2','\0','\0' };

void pak_load_locked() {
    if (g_pak.loaded) return;
    g_pak.loaded = true;
    std::ifstream f(pak_path(), std::ios::binary);
    if (!f.good()) return;
    char magic[8] = {};
    f.read(magic, sizeof(magic));
    if (!f.good()) return;
    const bool v2 = (std::memcmp(magic, kMagicV2, sizeof(kMagicV2)) == 0);
    if (!v2 && std::memcmp(magic, kMagicV1, sizeof(kMagicV1)) != 0) return;
    for (VNote& n : g_pak.notes) {
        uint8_t used = 0;
        uint32_t size = 0;
        f.read((char*)&used, 1);
        f.read((char*)&n.company, 2);
        f.read((char*)&n.game_code, 4);
        f.read((char*)n.game_name, 16);
        f.read((char*)n.ext_name, 4);
        if (v2) f.read((char*)&n.last_write, 8);
        f.read((char*)&size, 4);
        if (!f.good() || size > kTotalPages * kPageSize) { n = VNote{}; break; }
        n.used = (used != 0);
        n.data.resize(size);
        if (size) f.read((char*)n.data.data(), size);
        if (!f.good()) { n = VNote{}; break; }
    }
}

void pak_store_locked() {
    std::error_code ec;
    std::filesystem::create_directories(pak_path().parent_path(), ec);
    std::ofstream f(pak_path(), std::ios::binary | std::ios::trunc);
    if (!f.good()) {
        std::fprintf(stderr, "[SAVE] WARNING: cannot write %s\n", pak_path().string().c_str());
        return;
    }
    f.write(kMagicV2, sizeof(kMagicV2));
    for (const VNote& n : g_pak.notes) {
        const uint8_t used = n.used ? 1 : 0;
        const uint32_t size = (uint32_t)n.data.size();
        f.write((const char*)&used, 1);
        f.write((const char*)&n.company, 2);
        f.write((const char*)&n.game_code, 4);
        f.write((const char*)n.game_name, 16);
        f.write((const char*)n.ext_name, 4);
        f.write((const char*)&n.last_write, 8);
        f.write((const char*)&size, 4);
        if (size) f.write((const char*)n.data.data(), size);
    }
}

// Read a fixed-size byte array from (byteswapped) rdram. Null pointer reads
// as all zeroes (libultra allows NULL ext_name).
void read_rdram_bytes(uint8_t* rdram, gpr addr, uint8_t* out, size_t count) {
    if (addr == 0) { std::memset(out, 0, count); return; }
    for (size_t i = 0; i < count; i++) {
        out[i] = (uint8_t)MEM_B(i, addr);
    }
}

int find_note_locked(uint16_t company, uint32_t game_code, const uint8_t* game_name, const uint8_t* ext_name) {
    for (int i = 0; i < kMaxNotes; i++) {
        const VNote& n = g_pak.notes[i];
        if (!n.used) continue;
        if (n.company != company || n.game_code != game_code) continue;
        if (std::memcmp(n.game_name, game_name, 16) != 0) continue;
        if (std::memcmp(n.ext_name, ext_name, 4) != 0) continue;
        return i;
    }
    return -1;
}

// Fetch a pointer-typed argument as a sign-extended rdram address.
template <int N>
gpr arg_addr(uint8_t* rdram, recomp_context* ctx) {
    return (gpr)(int32_t)_arg<N, u32>(rdram, ctx);
}

} // namespace

extern "C" int twine_pak_latest_note(void) {
    std::lock_guard lock{ g_pak.mutex };
    pak_load_locked();
    int best = -1;
    uint64_t best_t = 0;
    for (int i = 0; i < kMaxNotes; i++) {
        const VNote& n = g_pak.notes[i];
        if (!n.used) continue;
        if (best < 0 || n.last_write >= best_t) { best = i; best_t = n.last_write; }
    }
    return best;
}

extern "C" int twine_pak_redirect_next_read = -1;

// Copies the 16-byte game_name of a note; returns 1 if the note is in use.
extern "C" int twine_pak_note_name(int note, unsigned char* out16) {
    std::lock_guard lock{ g_pak.mutex };
    pak_load_locked();
    if (note < 0 || note >= kMaxNotes || !g_pak.notes[note].used) return 0;
    std::memcpy(out16, g_pak.notes[note].game_name, 16);
    return 1;
}

extern "C" void osPfsInitPak_recomp(uint8_t* rdram, recomp_context* ctx) {
    const s32 channel = _arg<2, s32>(rdram, ctx);
    if (channel != 0) { _return<s32>(ctx, PFS_ERR_NOPACK); return; }
    std::lock_guard lock{ g_pak.mutex };
    pak_load_locked();
    _return<s32>(ctx, PFS_NO_ERROR);
}

extern "C" void osPfsChecker_recomp(uint8_t* rdram, recomp_context* ctx) {
    _return<s32>(ctx, PFS_NO_ERROR);
}

extern "C" void osPfsRepairId_recomp(uint8_t* rdram, recomp_context* ctx) {
    _return<s32>(ctx, PFS_NO_ERROR);
}

extern "C" void osPfsFreeBlocks_recomp(uint8_t* rdram, recomp_context* ctx) {
    gpr bytes_ptr = arg_addr<1>(rdram, ctx);
    std::lock_guard lock{ g_pak.mutex };
    pak_load_locked();
    const uint32_t free_bytes = (kTotalPages - g_pak.pages_used()) * kPageSize;
    if (bytes_ptr != 0) MEM_W(0, bytes_ptr) = (int32_t)free_bytes;
    _return<s32>(ctx, PFS_NO_ERROR);
}

extern "C" void osPfsNumFiles_recomp(uint8_t* rdram, recomp_context* ctx) {
    gpr max_files_ptr  = arg_addr<1>(rdram, ctx);
    gpr files_used_ptr = arg_addr<2>(rdram, ctx);
    std::lock_guard lock{ g_pak.mutex };
    pak_load_locked();
    int used = 0;
    for (const VNote& n : g_pak.notes) used += n.used ? 1 : 0;
    if (max_files_ptr  != 0) MEM_W(0, max_files_ptr)  = kMaxNotes;
    if (files_used_ptr != 0) MEM_W(0, files_used_ptr) = used;
    _return<s32>(ctx, PFS_NO_ERROR);
}

extern "C" void osPfsFindFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    const uint16_t company   = _arg<1, u16>(rdram, ctx);
    const uint32_t game_code = _arg<2, u32>(rdram, ctx);
    gpr game_name_ptr = arg_addr<3>(rdram, ctx);
    gpr ext_name_ptr  = arg_addr<4>(rdram, ctx);
    gpr file_no_ptr   = arg_addr<5>(rdram, ctx);

    uint8_t game_name[16], ext_name[4];
    read_rdram_bytes(rdram, game_name_ptr, game_name, 16);
    read_rdram_bytes(rdram, ext_name_ptr, ext_name, 4);

    std::lock_guard lock{ g_pak.mutex };
    pak_load_locked();
    const int idx = find_note_locked(company, game_code, game_name, ext_name);
    if (idx < 0) { _return<s32>(ctx, PFS_ERR_INVALID); return; }
    if (file_no_ptr != 0) MEM_W(0, file_no_ptr) = idx;
    _return<s32>(ctx, PFS_NO_ERROR);
}

extern "C" void osPfsAllocateFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    const uint16_t company   = _arg<1, u16>(rdram, ctx);
    const uint32_t game_code = _arg<2, u32>(rdram, ctx);
    gpr game_name_ptr = arg_addr<3>(rdram, ctx);
    gpr ext_name_ptr  = arg_addr<4>(rdram, ctx);
    const s32 file_size = _arg<5, s32>(rdram, ctx);
    gpr file_no_ptr   = arg_addr<6>(rdram, ctx);

    if (file_size < 0) { _return<s32>(ctx, PFS_ERR_INVALID); return; }

    uint8_t game_name[16], ext_name[4];
    read_rdram_bytes(rdram, game_name_ptr, game_name, 16);
    read_rdram_bytes(rdram, ext_name_ptr, ext_name, 4);

    std::lock_guard lock{ g_pak.mutex };
    pak_load_locked();

    // Replace an existing identical note (real paks error here, but being
    // lenient avoids stale-note edge cases).
    int idx = find_note_locked(company, game_code, game_name, ext_name);
    if (idx < 0) {
        for (int i = 0; i < kMaxNotes; i++) {
            if (!g_pak.notes[i].used) { idx = i; break; }
        }
    }
    if (idx < 0) { _return<s32>(ctx, PFS_ERR_DATA_FULL); return; }

    const uint32_t pages_needed = (uint32_t)((file_size + kPageSize - 1) / kPageSize);
    uint32_t pages_available = kTotalPages - g_pak.pages_used();
    if (g_pak.notes[idx].used) {
        pages_available += (uint32_t)((g_pak.notes[idx].data.size() + kPageSize - 1) / kPageSize);
    }
    if (pages_needed > pages_available) { _return<s32>(ctx, PFS_ERR_DATA_FULL); return; }

    VNote& n = g_pak.notes[idx];
    n.used = true;
    n.company = company;
    n.game_code = game_code;
    std::memcpy(n.game_name, game_name, 16);
    std::memcpy(n.ext_name, ext_name, 4);
    n.last_write = (uint64_t)std::time(nullptr);
    n.data.assign(pages_needed * kPageSize, 0);
    pak_store_locked();

    if (file_no_ptr != 0) MEM_W(0, file_no_ptr) = idx;
    _return<s32>(ctx, PFS_NO_ERROR);
}

extern "C" void osPfsDeleteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    const uint16_t company   = _arg<1, u16>(rdram, ctx);
    const uint32_t game_code = _arg<2, u32>(rdram, ctx);
    gpr game_name_ptr = arg_addr<3>(rdram, ctx);
    gpr ext_name_ptr  = arg_addr<4>(rdram, ctx);

    uint8_t game_name[16], ext_name[4];
    read_rdram_bytes(rdram, game_name_ptr, game_name, 16);
    read_rdram_bytes(rdram, ext_name_ptr, ext_name, 4);

    std::lock_guard lock{ g_pak.mutex };
    pak_load_locked();
    const int idx = find_note_locked(company, game_code, game_name, ext_name);
    if (idx < 0) { _return<s32>(ctx, PFS_ERR_INVALID); return; }
    g_pak.notes[idx] = VNote{};
    pak_store_locked();
    _return<s32>(ctx, PFS_NO_ERROR);
}

extern "C" void osPfsFileState_recomp(uint8_t* rdram, recomp_context* ctx) {
    const s32 file_no = _arg<1, s32>(rdram, ctx);
    gpr state_ptr = arg_addr<2>(rdram, ctx);

    std::lock_guard lock{ g_pak.mutex };
    pak_load_locked();
    if (file_no < 0 || file_no >= kMaxNotes || !g_pak.notes[file_no].used || state_ptr == 0) {
        _return<s32>(ctx, PFS_ERR_INVALID);
        return;
    }
    const VNote& n = g_pak.notes[file_no];
    // OSPfsState layout: u32 file_size; u32 game_code; u16 company_code;
    //                    char ext_name[4]; char game_name[16];
    MEM_W(0x0, state_ptr) = (int32_t)n.data.size();
    MEM_W(0x4, state_ptr) = (int32_t)n.game_code;
    MEM_H(0x8, state_ptr) = (int16_t)n.company;
    for (int i = 0; i < 4; i++)  MEM_B(0xA + i, state_ptr) = (int8_t)n.ext_name[i];
    for (int i = 0; i < 16; i++) MEM_B(0xE + i, state_ptr) = (int8_t)n.game_name[i];
    _return<s32>(ctx, PFS_NO_ERROR);
}

extern "C" void osPfsReadWriteFile_recomp(uint8_t* rdram, recomp_context* ctx) {
    const s32 file_no = _arg<1, s32>(rdram, ctx);
    const u8  flag    = _arg<2, u8>(rdram, ctx);
    const s32 offset  = _arg<3, s32>(rdram, ctx);
    const s32 nbytes  = _arg<4, s32>(rdram, ctx);
    gpr data_ptr = arg_addr<5>(rdram, ctx);

    std::lock_guard lock{ g_pak.mutex };
    pak_load_locked();
    s32 eff_file = file_no;
    if (flag == 0 && twine_pak_redirect_next_read >= 0) {
        eff_file = twine_pak_redirect_next_read;
        twine_pak_redirect_next_read = -1;
    }
    if (eff_file < 0 || eff_file >= kMaxNotes || !g_pak.notes[eff_file].used ||
        offset < 0 || nbytes < 0 || data_ptr == 0) {
        _return<s32>(ctx, PFS_ERR_INVALID);
        return;
    }
    VNote& n = g_pak.notes[eff_file];
    if ((size_t)offset + (size_t)nbytes > n.data.size()) {
        _return<s32>(ctx, PFS_ERR_INVALID);
        return;
    }
    if (flag == 0) { // PFS_READ: pak -> rdram
        for (s32 i = 0; i < nbytes; i++) {
            MEM_B(i, data_ptr) = (int8_t)n.data[(size_t)offset + i];
        }
    } else {         // PFS_WRITE: rdram -> pak
        for (s32 i = 0; i < nbytes; i++) {
            n.data[(size_t)offset + i] = (uint8_t)MEM_B(i, data_ptr);
        }
        n.last_write = (uint64_t)std::time(nullptr);
        pak_store_locked();
    }
    _return<s32>(ctx, PFS_NO_ERROR);
}
