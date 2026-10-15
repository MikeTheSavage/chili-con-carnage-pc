#include "../host/file_runtime.hpp"
#include "../host/fat_names.hpp"
#include <chrono>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace {
constexpr auto text = 0x09000000u, buffer = text + 2048u, status = buffer + 1024u;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void string(psprecomp::Runtime &rt, const std::string &value, std::uint32_t address = text) {
    for (std::size_t i = 0; i <= value.size(); ++i)
        rt.memory().store8(address + static_cast<std::uint32_t>(i), i == value.size() ? 0u : value[i]);
}
std::uint32_t call(psprecomp::Runtime &rt, std::uint32_t nid, std::uint32_t a0, std::uint32_t a1 = 0u, std::uint32_t a2 = 0u) {
    auto &ctx=rt.cpu(); ctx.gpr[4]=a0; ctx.gpr[5]=a1; ctx.gpr[6]=a2;
    rt.invoke_import("IoFileMgrForUser", nid, ctx); return ctx.gpr[2];
}
std::uint32_t open(psprecomp::Runtime &rt, const std::string &path, std::uint32_t flags) {
    string(rt, path); return call(rt, 0x109F50BCu, text, flags, 0777u);
}
std::uint64_t seek(psprecomp::Runtime &rt, std::uint32_t uid, std::int64_t offset, unsigned whence) {
    auto &ctx=rt.cpu(); ctx.gpr[4]=uid; ctx.gpr[5]=0xDEADBEEFu;
    ctx.gpr[6]=static_cast<std::uint32_t>(offset); ctx.gpr[7]=static_cast<std::uint32_t>(static_cast<std::uint64_t>(offset) >> 32u);
    ctx.gpr[8]=whence; rt.invoke_import("IoFileMgrForUser", 0x27EB27B8u, ctx);
    return ctx.gpr[2] | (static_cast<std::uint64_t>(ctx.gpr[3]) << 32u);
}
void fixture(const std::filesystem::path &root) {
    const auto disc=root/"disc0", ms=root/"ms0";
    std::filesystem::create_directories(disc/"PSP_GAME"/"USRDIR");
    const char bytes[]={'A', 0, 'B', static_cast<char>(0xFF), 'C'};
    { std::ofstream data(disc/"PSP_GAME"/"USRDIR"/"data.bin", std::ios::binary); data.write(bytes, sizeof(bytes)); }
    psprecomp::Runtime rt; rt.set_game_root(disc); chili::FileRuntime files(rt, ms);
    const auto uid=open(rt, "data.bin", 1u); require(static_cast<std::int32_t>(uid)>2, "Relative disc open failed");
    rt.memory().zero(buffer, 32u);
    require(call(rt, 0x6A638D83u, uid, buffer, 32u)==5u, "Partial read size mismatch");
    for (unsigned i=0; i<5u; ++i) require(rt.memory().load8(buffer+i)==static_cast<std::uint8_t>(bytes[i]), "Binary read changed bytes");
    require(call(rt, 0x6A638D83u, uid, buffer, 1u)==0u, "EOF read failed");
    require(seek(rt, uid, -2, 2u)==3u, "Signed seek ABI mismatch");
    require(call(rt, 0x6A638D83u, uid, 0u, 1u)==0x8001000Eu, "Invalid read pointer accepted");
    require(call(rt, 0x6A638D83u, uid, buffer, 2u)==2u && rt.memory().load8(buffer)==0xFFu, "Failed read moved position");
    require(seek(rt, uid, 0x100000010ll, 0u)==0x100000010ull, "64-bit seek return truncated");
    require(call(rt, 0x810C4BC3u, uid)==0u && call(rt, 0x810C4BC3u, uid)==0x80010009u, "Close/invalid handle mismatch");
    string(rt, "disc0:/PSP_GAME/USRDIR/data.bin");
    require(call(rt, 0xACE946E8u, text, status)==0u && rt.memory().load32(status+8u)==5u &&
        rt.memory().load32(status+12u)==0u && (rt.memory().load32(status)&0xF000u)==0x2000u, "Stat layout mismatch");
    require(open(rt, "disc0:/PSP_GAME/USRDIR/data.bin", 0x402u)==0x8001001Eu, "Disc truncation was permitted");
    require(open(rt, "ms0:/../disc0/PSP_GAME/USRDIR/data.bin", 1u)==0x8001000Du, "Traversal was permitted");
    require(open(rt, "flash0:/data.bin", 1u)==0x80010013u, "Unknown device was accepted");
    string(rt, "ms0:/missing-dir"); require(call(rt, 0xB29DDF9Cu, text)==0x80010002u, "Missing directory error mismatch");
    string(rt, "ms0:/saves"); require(call(rt, 0x06A70004u, text, 0777u)==0u, "Mkdir failed");
    const auto saved=open(rt, "ms0:/saves/test.bin", 0x602u); require(static_cast<std::int32_t>(saved)>2, "Save create failed");
    string(rt, "SAVE", buffer); require(call(rt, 0x42EC03ACu, saved, buffer, 4u)==4u, "Save write failed");
    require(call(rt, 0x42EC03ACu, saved, 0u, 0u)==0u, "Zero-length write rejected pointer");
    require(call(rt, 0x810C4BC3u, saved)==0u, "Save close failed");
    require(open(rt, "ms0:/saves/test.bin", 0xA02u)==0x80010011u, "Exclusive create replaced existing save");
    const auto append=open(rt, "ms0:/saves/test.bin", 0x102u);
    string(rt, "++", buffer); require(call(rt, 0x42EC03ACu, append, buffer, 2u)==2u, "Append failed");
    call(rt, 0x810C4BC3u, append);
    const auto reread=open(rt, "ms0:/saves/test.bin", 1u);
    require(call(rt, 0x6A638D83u, reread, buffer, 16u)==6u && rt.memory().load32(buffer)==0x45564153u &&
        rt.memory().load16(buffer+4u)==0x2B2Bu, "Save round trip mismatch");
    call(rt, 0x810C4BC3u, reread);
    string(rt, "ms0:/saves"); const auto dir=call(rt, 0xB29DDF9Cu, text);
    rt.memory().zero(status, 352u);
    require(call(rt, 0xE3EB004Cu, dir, status)==1u && rt.memory().read_c_string(status+88u)=="test.bin" &&
        rt.memory().load32(status+8u)==6u && call(rt, 0xE3EB004Cu, dir, status)==0u, "Directory entry layout/EOF mismatch");
    require(call(rt, 0xEB092469u, dir)==0u, "Directory close failed");
    const auto private_buffer=status+512u;
    auto private_read = [&](std::uint32_t sdk, std::uint32_t private_size) {
        files.set_compiled_sdk(sdk);
        string(rt, "ms0:/saves"); const auto handle=call(rt, 0xB29DDF9Cu, text);
        rt.memory().zero(status, 352u); rt.memory().zero(private_buffer, 1044u);
        rt.memory().store32(status+344u, private_buffer);
        rt.memory().store32(private_buffer, private_size);
        rt.memory().store32(private_buffer+4u, 0xAABBCCDDu);
        require(call(rt, 0xE3EB004Cu, handle, status)==1u && rt.memory().load32(status+344u)==private_buffer,
            "FAT private read failed or changed pointer");
        call(rt, 0xEB092469u, handle);
    };
    private_read(0x02080010u, 0u);
    require(rt.memory().read_c_string(private_buffer)=="TEST.BIN" && rt.memory().read_c_string(private_buffer+13u)=="test.bin",
        "Legacy FAT layout mismatch");
    private_read(0x03080010u, 1044u);
    require(rt.memory().load32(private_buffer)==1044u && rt.memory().read_c_string(private_buffer+4u)=="TEST.BIN" &&
        rt.memory().read_c_string(private_buffer+20u)=="test.bin", "Modern FAT layout mismatch");
    private_read(0x03080010u, 4u);
    require(rt.memory().load32(private_buffer+4u)==0xAABBCCDDu, "Undersized FAT private area was written");
    const auto long_file=open(rt, "ms0:/saves/a_very_long_filename.bin", 0x602u);
    require(static_cast<std::int32_t>(long_file)>2, "Long filename create failed");
    string(rt, "LONG", buffer); call(rt, 0x42EC03ACu, long_file, buffer, 4u); call(rt, 0x810C4BC3u, long_file);
    const auto short_file=open(rt, "ms0:/saves/A_VERY~1.BIN", 1u);
    require(static_cast<std::int32_t>(short_file)>2 && call(rt, 0x6A638D83u, short_file, buffer, 4u)==4u &&
        rt.memory().load32(buffer)==0x474E4F4Cu, "Short alias did not open long file");
    call(rt, 0x810C4BC3u, short_file);
    string(rt, "ms0:/saves/A_VERY~1.BIN"); require(call(rt, 0xF27A9C51u, text)==0u, "Short alias deletion failed");
    auto create_marker = [&](const std::string &filename, const std::string &marker) {
        const auto handle=open(rt, "ms0:/saves/"+filename, 0x602u);
        require(static_cast<std::int32_t>(handle)>2, "Collision file creation failed");
        string(rt, marker, buffer); call(rt, 0x42EC03ACu, handle, buffer, 4u); call(rt, 0x810C4BC3u, handle);
    };
    create_marker("collision_zeta.bin", "ZETA");
    create_marker("collision_alpha.bin", "ALPH");
    const auto collision=open(rt, "ms0:/saves/COLLIS~1.BIN", 1u);
    require(call(rt, 0x6A638D83u, collision, buffer, 4u)==4u && rt.memory().load32(buffer)==0x48504C41u,
        "Host short name overrode PSP-facing alias");
    call(rt, 0x810C4BC3u, collision);
    string(rt, "ms0:/saves/collision_alpha.bin"); call(rt, 0xF27A9C51u, text);
    const auto stable=open(rt, "ms0:/saves/COLLIS~2.BIN", 1u);
    require(call(rt, 0x6A638D83u, stable, buffer, 4u)==4u && rt.memory().load32(buffer)==0x4154455Au,
        "Issued short alias changed after deletion");
    call(rt, 0x810C4BC3u, stable);
    string(rt, "ms0:/saves/collision_zeta.bin"); call(rt, 0xF27A9C51u, text);
    string(rt, "ms0:/saves/test.bin"); string(rt, "ms0:/saves/moved.bin", text+1024u);
    require(call(rt, 0x779103A0u, text, text+1024u)==0u, "Rename failed");
    string(rt, "ms0:/saves/moved.bin"); require(call(rt, 0xF27A9C51u, text)==0u, "Remove failed");
    string(rt, "ms0:/saves"); require(call(rt, 0x1117C65Fu, text)==0u, "Rmdir failed");
    require(std::filesystem::file_size(disc/"PSP_GAME"/"USRDIR"/"data.bin")==5u, "Disc source changed");
}
void actual_disc(const std::filesystem::path &disc, const std::filesystem::path &stick) {
    psprecomp::Runtime rt; rt.set_game_root(disc); chili::FileRuntime files(rt, stick);
    const auto uid=open(rt, "disc0:/PSP_GAME/USRDIR/assets00.naz", 1u);
    require(static_cast<std::int32_t>(uid)>2, "Actual asset archive open failed");
    require(call(rt, 0x6A638D83u, uid, buffer, 16u)==16u, "Actual archive read failed");
    const auto expected=std::filesystem::file_size(disc/"PSP_GAME"/"USRDIR"/"assets00.naz");
    require(seek(rt, uid, 0, 2u)==expected, "Actual archive size/seek mismatch");
    call(rt, 0x810C4BC3u, uid);
    std::cout << "PASS: native file runtime reads actual asset archive (" << expected << " bytes)\n";
}
}
int main(int argc, char **argv) {
    try {
        const auto aliases=chili::fat_short_names({"foo.txt", "FOOBAR~1.TXT", "foobar_longname.txt", "foobar_othername.txt", "a b+c.bin", "long.dir.name"});
        require(aliases==std::vector<std::string>{"FOO.TXT", "FOOBAR~1.TXT", "FOOBAR~2.TXT", "FOOBAR~3.TXT", "AB_C~1.BIN", "LONGDI~1.NAM"},
            "FAT short-name reserved/collision/sanitization mismatch");
        if (argc<2 || argc>3) throw std::runtime_error("Usage: chili_file_test <fixture directory> [actual disc0]");
        const auto root=std::filesystem::absolute(argv[1])/std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        fixture(root);
        if (argc==3) actual_disc(argv[2], root/"actual-ms0");
        std::cout << "PASS: binary IO, seek ABI, stat/dir layouts, legacy/modern FAT data and short aliases, save round trip and read-only disc\n";
    } catch (const std::exception &e) { std::cerr << e.what() << '\n'; return 1; }
}
