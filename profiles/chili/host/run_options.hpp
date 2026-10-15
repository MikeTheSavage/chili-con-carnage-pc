#pragma once
#include "psprecomp/common.hpp"
#include "state_hash.hpp"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <string>
#include <vector>
namespace chili {
// Test-mode switches. Everything is read from the environment once at startup;
// with nothing set the game runs exactly as an ordinary interactive launch.
//
//   CHILI_DETERMINISTIC=1        RTC and libc time follow the scheduler clock from a fixed epoch, audio goes to a
//                                guest-clocked null sink, dialogs answer themselves, no host input is read
//   CHILI_INPUT_RECORD=<file>    write the sampled pad state, keyed on the controller sample index
//   CHILI_INPUT_REPLAY=<file>    play such a file back
//   CHILI_FRAME_LOG=<csv>        one row per game frame
//   CHILI_STATE_HASH_EVERY=<n>   print "[state-hash] frame=<n> hash=<hex>" every n game frames
//   CHILI_STATE_HASH_REGIONS=1   also print one hash per MiB of main RAM
//   CHILI_STATE_HASH_EXCLUDE=<l> comma list of <guest address>:<bytes> main-RAM ranges left out of the hash
//   CHILI_STOP_AT_FRAME=<n>      stop cleanly at game frame n
//   CHILI_FRAME_PNG_EVERY=<n>    with CHILI_FRAME_PNG_DIR=<dir>: write frame_<n>.png every n game frames
//   CHILI_MS0_DIR=<dir>          memory stick root (same as the --ms0 argument); a deterministic, auto-dialog or
//                                replay run must name one, it never falls back to the real save folder
//   CHILI_NULL_AUDIO=1           null audio sink without the rest of deterministic mode
//   CHILI_AUTO_DIALOGS=1         scripted save and message dialogs without the rest of deterministic mode
//   CHILI_AUTO_SAVE_SLOT=<list>  comma list, one entry per save dialog, last repeats: focus | cancel | <slot index>
//                                (the load, save and delete lists alike; a pick also answers the list's question
//                                about a slot that holds a save: the save is written over, or deleted)
//   CHILI_AUTO_MESSAGE=<list>    comma list, one entry per message dialog, last repeats: accept | decline
//   CHILI_AUTO_DIALOG_DELAY_MS   guest milliseconds a scripted dialog stays open (default 500)
//   CHILI_LOG_FLUSH=1            flush stdout after every write, for watching a redirected log live
//   CHILI_UNPACED=1              outside deterministic mode: do not hold guest time back to real time
//   CHILI_CONFIG=<file>          ChiliNative.ini to use instead of the one beside the executable; in deterministic
//                                mode only its [Input] Scheme matters, and without it the scheme is psp
//   CHILI_AUDIO_WAV=<file>       record what reaches the audio device (16-bit stereo 44.1 kHz), with silence
//                                where the device ran dry
//   CHILI_AUDIO_TRACE=<csv>      one row per audio output call: guest and host time at the call and at its return
//   CHILI_WATCH=1                a scripted run opens an ordinary window, activated and in front; without it the
//                                window of a deterministic, replay, scripted-dialog or script-channel run is shown
//                                behind the others and never takes the keyboard or the mouse from the window in use
//   CHILI_INPUT_TRACE=<csv>      dual_analog scheme: one row per controller sample with the input and what the
//                                player script made of it (move vector, turn rate, look angles)
//   PSPRECOMP_PROBE_SECONDS=<n>  wall-clock deadline, 1 through 7200
inline std::wstring environment_text(const wchar_t *name){
    std::wstring value(256u,L'\0');
    for(;;){
        SetLastError(ERROR_SUCCESS);
        const auto count=GetEnvironmentVariableW(name,value.data(),static_cast<DWORD>(value.size()));
        if(!count){value.clear();return value;}
        if(count<value.size()){value.resize(count);return value;}
        value.resize(count+1u);
    }
}
inline std::string environment_ascii(const wchar_t *name){
    const auto wide=environment_text(name);std::string out;
    for(const auto c:wide){if(c>126 || c<32)throw psprecomp::Error("Environment setting must be printable ASCII");out.push_back(static_cast<char>(c));}
    return out;
}
inline std::uint64_t environment_number(const wchar_t *name,const char *label,std::uint64_t maximum=std::numeric_limits<std::uint64_t>::max()){
    const auto text=environment_ascii(name);
    if(text.empty())return 0u;
    std::uint64_t value{};
    for(const auto c:text){
        if(c<'0' || c>'9' || value>(maximum-static_cast<unsigned>(c-'0'))/10u)
            throw psprecomp::Error(std::string(label)+" must be an unsigned integer up to "+std::to_string(maximum));
        value=value*10u+static_cast<unsigned>(c-'0');
    }
    return value;
}
inline bool environment_flag(const wchar_t *name,const char *label){
    const auto text=environment_ascii(name);
    if(text.empty() || text=="0")return false;
    if(text!="1")throw psprecomp::Error(std::string(label)+" must be 1, 0 or unset");
    return true;
}
inline std::vector<std::string> environment_list(const wchar_t *name){
    const auto text=environment_ascii(name);std::vector<std::string> out;
    for(std::size_t start=0;start<text.size();){
        const auto comma=std::min(text.find(',',start),text.size());
        if(comma>start)out.push_back(text.substr(start,comma-start));
        start=comma+1u;
    }
    return out;
}
struct RunOptions {
    bool deterministic{},null_audio{},auto_dialogs{},state_hash_regions{},log_flush{},unpaced{},watch{};
    std::filesystem::path ms0_dir,input_record,input_replay,input_trace,frame_log,frame_png_dir,config_file,audio_wav,audio_trace;
    std::uint64_t state_hash_every{},stop_at_frame{},frame_png_every{},probe_seconds{},auto_dialog_delay_us{500000u};
    std::vector<std::string> auto_save_slot,auto_message;
    std::vector<HashExclusion> state_hash_exclude;
    // A run that answers the save dialogs itself or plays recorded input saves with
    // nobody watching, so it has to be told which memory stick folder to write.
    bool scripted() const {return deterministic || auto_dialogs || !input_replay.empty();}
    static RunOptions from_environment(){
        RunOptions o;
        o.deterministic=environment_flag(L"CHILI_DETERMINISTIC","CHILI_DETERMINISTIC");
        o.null_audio=o.deterministic || environment_flag(L"CHILI_NULL_AUDIO","CHILI_NULL_AUDIO");
        o.auto_dialogs=o.deterministic || environment_flag(L"CHILI_AUTO_DIALOGS","CHILI_AUTO_DIALOGS");
        o.state_hash_regions=environment_flag(L"CHILI_STATE_HASH_REGIONS","CHILI_STATE_HASH_REGIONS");
        o.log_flush=environment_flag(L"CHILI_LOG_FLUSH","CHILI_LOG_FLUSH");
        o.unpaced=environment_flag(L"CHILI_UNPACED","CHILI_UNPACED");
        o.watch=environment_flag(L"CHILI_WATCH","CHILI_WATCH");
        o.config_file=environment_text(L"CHILI_CONFIG");
        o.ms0_dir=environment_text(L"CHILI_MS0_DIR");
        o.input_record=environment_text(L"CHILI_INPUT_RECORD");
        o.input_replay=environment_text(L"CHILI_INPUT_REPLAY");
        o.input_trace=environment_text(L"CHILI_INPUT_TRACE");
        o.audio_wav=environment_text(L"CHILI_AUDIO_WAV");
        o.audio_trace=environment_text(L"CHILI_AUDIO_TRACE");
        o.frame_log=environment_text(L"CHILI_FRAME_LOG");
        o.frame_png_dir=environment_text(L"CHILI_FRAME_PNG_DIR");
        o.state_hash_every=environment_number(L"CHILI_STATE_HASH_EVERY","CHILI_STATE_HASH_EVERY");
        o.stop_at_frame=environment_number(L"CHILI_STOP_AT_FRAME","CHILI_STOP_AT_FRAME");
        o.frame_png_every=environment_number(L"CHILI_FRAME_PNG_EVERY","CHILI_FRAME_PNG_EVERY");
        if(o.frame_png_every && o.frame_png_dir.empty())throw psprecomp::Error("CHILI_FRAME_PNG_EVERY requires CHILI_FRAME_PNG_DIR");
        o.probe_seconds=environment_number(L"PSPRECOMP_PROBE_SECONDS","PSPRECOMP_PROBE_SECONDS",7200u);
        if(!o.probe_seconds && !environment_text(L"PSPRECOMP_PROBE_SECONDS").empty())
            throw psprecomp::Error("PSPRECOMP_PROBE_SECONDS must be an integer from 1 through 7200");
        if(const auto delay=environment_number(L"CHILI_AUTO_DIALOG_DELAY_MS","CHILI_AUTO_DIALOG_DELAY_MS",600000u))
            o.auto_dialog_delay_us=delay*1000u;
        for(const auto &entry:environment_list(L"CHILI_STATE_HASH_EXCLUDE")){
            const auto colon=entry.find(':');std::size_t used_address{},used_length{};unsigned long long address{},length{};
            try{
                if(colon!=std::string::npos){address=std::stoull(entry.substr(0u,colon),&used_address,0);length=std::stoull(entry.substr(colon+1u),&used_length,0);}
            }catch(const std::exception &){used_address=0u;}
            const auto offset=static_cast<unsigned long long>(psprecomp::GuestMemory::canonical(static_cast<std::uint32_t>(address)))-psprecomp::GuestMemory::kPhysicalBase;
            if(colon==std::string::npos || used_address!=colon || used_length!=entry.size()-colon-1u || address>0xFFFFFFFFull || !length ||
               length>0x100000ull || psprecomp::GuestMemory::canonical(static_cast<std::uint32_t>(address))<psprecomp::GuestMemory::kPhysicalBase ||
               offset+length>32ull*1024ull*1024ull)
                throw psprecomp::Error("CHILI_STATE_HASH_EXCLUDE entries are <main RAM address>:<bytes>, at most 1 MiB each");
            o.state_hash_exclude.push_back({static_cast<std::uint32_t>(address),static_cast<std::uint32_t>(length)});
        }
        std::sort(o.state_hash_exclude.begin(),o.state_hash_exclude.end(),[](const auto &a,const auto &b){
            return psprecomp::GuestMemory::canonical(a.address)<psprecomp::GuestMemory::canonical(b.address);});
        for(std::size_t i=1;i<o.state_hash_exclude.size();++i)
            if(psprecomp::GuestMemory::canonical(o.state_hash_exclude[i-1u].address)+o.state_hash_exclude[i-1u].length>
               psprecomp::GuestMemory::canonical(o.state_hash_exclude[i].address))
                throw psprecomp::Error("CHILI_STATE_HASH_EXCLUDE ranges overlap");
        o.auto_save_slot=environment_list(L"CHILI_AUTO_SAVE_SLOT");
        o.auto_message=environment_list(L"CHILI_AUTO_MESSAGE");
        if(!o.input_record.empty() && o.input_record==o.input_replay)
            throw psprecomp::Error("CHILI_INPUT_RECORD and CHILI_INPUT_REPLAY must be different files");
        if(!environment_text(L"PSPRECOMP_CONTROLLER_REPLAY").empty())
            throw psprecomp::Error("PSPRECOMP_CONTROLLER_REPLAY (host-millisecond replays) is gone; record a sample-indexed replay and use CHILI_INPUT_REPLAY");
        return o;
    }
};
// Fixed start of guest wall time in deterministic mode: 2026-01-01T00:00:00Z.
inline constexpr std::uint64_t deterministic_epoch_unix_us=1767225600000000ull;
// Return address of the frame-end function's first, unconditional
// sceDisplayWaitVblankStartCB call (guest 0x088256F8): one per game frame in
// menus, movies and missions.
inline constexpr std::uint32_t frame_end_vblank_return=0x08825700u;
// The engine's frame times: Scene::RealTimePassed is the measured frame time in seconds, a
// float clamped to 0.0001-0.1 and written by guest 0x08898750; Scene::TimePassed is that times the game's
// time multiplier (slow motion, pause) and is what the simulation reads. Their millisecond counters are
// Scene::RealTimeMs at 0x08DFAE9C and Scene::GameTimeMs at 0x08DFAEA4.
inline constexpr std::uint32_t real_delta_time_address=0x08DFAE90u;
inline constexpr std::uint32_t game_delta_time_address=0x08DFAE94u;
}
