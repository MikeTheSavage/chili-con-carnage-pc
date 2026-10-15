#pragma once
#include "psprecomp/runtime.hpp"
#include <array>
#include <stdexcept>
#include <string>
#include <vector>

namespace chili::save_request {
enum class Failure { Address, Size, Parameter, Unsupported };
struct Error: std::runtime_error {
    Failure kind;
    Error(Failure k,const char *message):std::runtime_error(message),kind(k){}
};
struct Buffer {std::uint32_t address{},capacity{},size{};};
struct Request {
    std::uint32_t params{},mode{},language{},button_swap{},focus{},secure_version{};
    // The word at offset 56 as the game wrote it. It is not a permission to
    // write over a save: the game leaves it 0 in its automatic save (mode 1),
    // which a PSP writes over existing data without asking, and sets it to 1 only
    // in its list save (mode 5). Nothing decides by it; it is kept for the log.
    std::uint32_t overwrite_word{};
    std::string game,slot,file,title,savedata_title,detail,new_title;
    std::uint8_t parental{};
    Buffer data;
    std::array<Buffer,4> media;
    std::array<std::uint8_t,16> key{};
    std::vector<std::string> slots;
    std::array<std::uint8_t,1536> original{};
    bool is_save()const{return mode==1u||mode==5u;}
    // Mode 7: the player picks a save from a list and it is deleted. The request
    // carries the list and no payload or media.
    bool is_delete()const{return mode==7u;}
    bool interactive()const{return mode==4u||mode==5u||mode==7u;}
};
// Independent guest32 wire reader; it never writes output or chooses a slot.
// Resource caps are native host limits rather than asserted firmware limits.
Request parse(const psprecomp::GuestMemory &,std::uint32_t params);
std::vector<std::uint8_t> bytes(const psprecomp::GuestMemory &,Buffer);
// Header-only mutation guard. Slot/title strings are retained at Init; source
// payload/media bytes are intentionally captured at the eventual save action.
bool header_unchanged(const psprecomp::GuestMemory &,const Request &);
}
