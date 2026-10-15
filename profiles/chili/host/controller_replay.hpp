#pragma once
#include "controller_runtime.hpp"
#include "psprecomp/common.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace chili {
// Input record and replay keyed on the controller sample index, which is the
// count of sceCtrl reads that returned a sample. Time never enters the format,
// so a replay lands on the same reads at any host or guest speed.
//
//   # comment
//   scheme <name>
//   <sample index> <buttons, C-style number> <analog x> <analog y> [<right x> <right y> [<turn> <pitch>]]
//   end <samples>
//
// A line holds the pad state from its index until the next line; the state
// before the first line is neutral. The right stick is 128 128 when left out,
// which is every line of a recording made before it existed. <turn> and <pitch>
// are the degrees the mouse moved the view in one sample (decimal, right and
// down positive, 0 when left out); like the rest of a line they repeat in every
// sample until the next line. "scheme" names the
// control scheme the input was made for (psp or dual_analog, see pc_input.hpp)
// and comes before the first sample line; a file without it leaves the choice
// to the run. "end" marks where the replay stops driving input (default: one
// past the last line).
inline bool same_input(ControllerInput a,ControllerInput b) noexcept {return a.buttons==b.buttons && a.x==b.x && a.y==b.y && a.rx==b.rx && a.ry==b.ry && a.turn==b.turn && a.pitch==b.pitch;}

class InputRecorder {
    std::ofstream file_;
    ControllerInput last_{};
    std::uint64_t samples_{};
public:
    explicit InputRecorder(const std::filesystem::path &path,const std::string &scheme={}){
        if(path.empty())return;
        file_.open(path,std::ios::binary|std::ios::trunc);
        if(!file_)throw psprecomp::Error("Cannot create input record file: "+path.string());
        file_<<"# chili input v3: sample buttons x y [rx ry [turn pitch]]\n";
        // Nine digits bring a float back exactly.
        file_.precision(9);
        if(!scheme.empty())file_<<"scheme "<<scheme<<'\n';
        file_.flush();
        std::cout<<"[input] recording to "<<path.string()<<'\n';
    }
    ~InputRecorder(){if(file_.is_open()){file_<<"end "<<samples_<<'\n';file_.flush();}}
    InputRecorder(const InputRecorder &)=delete;
    InputRecorder &operator=(const InputRecorder &)=delete;
    bool enabled() const {return file_.is_open();}
    // Writes only when the state changes; flushed so a killed run keeps its input.
    void observe(std::uint64_t index,ControllerInput input){
        samples_=index+1u;
        if(!file_.is_open() || same_input(input,last_))return;
        last_=input;
        file_<<index<<" 0x"<<std::hex<<input.buttons<<std::dec<<' '<<unsigned(input.x)<<' '<<unsigned(input.y);
        const bool look=input.turn!=0.0f || input.pitch!=0.0f;
        if(look || input.rx!=128u || input.ry!=128u)file_<<' '<<unsigned(input.rx)<<' '<<unsigned(input.ry);
        if(look)file_<<' '<<input.turn<<' '<<input.pitch;
        file_<<'\n';
        file_.flush();
    }
};

class InputReplay {
    struct Event {std::uint64_t index{};ControllerInput input;};
    std::vector<Event> events_;
    std::uint64_t end_{};
    std::size_t cursor_{};
    ControllerInput state_{};
    std::string scheme_;
    bool enabled_{};
    static float degrees(const std::string &text){
        std::size_t used{};float value{};
        try{value=std::stof(text,&used);}catch(const std::exception &){used=0u;}
        if(text.empty() || used!=text.size() || !(value>=-3600.0f && value<=3600.0f))throw psprecomp::Error("Invalid angle in input replay: "+text);
        return value;
    }
    static std::uint64_t number(const std::string &text){
        std::size_t used{};std::uint64_t value{};
        try{value=std::stoull(text,&used,0);}catch(const std::exception &){used=0u;}
        if(text.empty() || text[0]=='-' || text[0]=='+' || used!=text.size())throw psprecomp::Error("Invalid number in input replay: "+text);
        return value;
    }
public:
    explicit InputReplay(const std::filesystem::path &path){
        if(path.empty())return;
        std::ifstream file(path);
        if(!file)throw psprecomp::Error("Cannot open input replay file: "+path.string());
        bool explicit_end{};std::string line;
        while(std::getline(file,line)){
            std::istringstream stream(line);std::string a,b,c,d,e,f,g,h,extra;
            if(!(stream>>a) || a[0]=='#')continue;
            if(explicit_end)throw psprecomp::Error("Input replay has lines after end");
            if(a=="scheme"){
                if(!(stream>>b) || stream>>extra || !scheme_.empty() || !events_.empty())
                    throw psprecomp::Error("Input replay scheme line: one name, once, before the first sample line");
                scheme_=b;continue;
            }
            if(a=="end"){
                if(!(stream>>b) || stream>>extra)throw psprecomp::Error("Input replay end line needs one sample count");
                end_=number(b);explicit_end=true;
                if(!events_.empty() && end_<=events_.back().index)throw psprecomp::Error("Input replay ends before its last line");
                continue;
            }
            if(!(stream>>b>>c>>d))throw psprecomp::Error("Input replay lines are: sample buttons x y [rx ry [turn pitch]]");
            const bool right=static_cast<bool>(stream>>e);
            if(right && !(stream>>f))throw psprecomp::Error("Input replay lines are: sample buttons x y [rx ry [turn pitch]]");
            const bool look=right && static_cast<bool>(stream>>g);
            if((look && !(stream>>h)) || stream>>extra)throw psprecomp::Error("Input replay lines are: sample buttons x y [rx ry [turn pitch]]");
            const auto index=number(a),buttons=number(b),x=number(c),y=number(d),rx=right?number(e):128u,ry=right?number(f):128u;
            if((buttons&~0xF3F9ull) || x>255u || y>255u || rx>255u || ry>255u || (!events_.empty() && index<=events_.back().index))
                throw psprecomp::Error("Invalid or out-of-order input replay line: "+line);
            events_.push_back({index,{static_cast<std::uint32_t>(buttons),static_cast<std::uint8_t>(x),static_cast<std::uint8_t>(y),
                static_cast<std::uint8_t>(rx),static_cast<std::uint8_t>(ry),look?degrees(g):0.0f,look?degrees(h):0.0f}});
        }
        if(file.bad())throw psprecomp::Error("Cannot read input replay file: "+path.string());
        if(!explicit_end)end_=events_.empty()?0u:events_.back().index+1u;
        enabled_=true;
        std::cout<<"[input] replay "<<path.string()<<" lines="<<events_.size()<<" end="<<end_;
        if(!scheme_.empty())std::cout<<" scheme="<<scheme_;
        std::cout<<'\n';
    }
    bool enabled() const {return enabled_;}
    // The control scheme the file names; empty when it names none.
    const std::string &scheme() const {return scheme_;}
    // True while the replay, rather than a live source, owns this sample.
    bool active(std::uint64_t index) const {return index<end_;}
    // Indices must not decrease between calls.
    ControllerInput at(std::uint64_t index){
        while(cursor_<events_.size() && events_[cursor_].index<=index)state_=events_[cursor_++].input;
        return state_;
    }
};
}
