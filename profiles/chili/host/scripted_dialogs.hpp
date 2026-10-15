#pragma once
#include "message_dialog_runtime.hpp"
#include "save_slot_presenter.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <cstdint>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <utility>
#include <vector>
namespace chili {
// Dialog presenters for unattended runs. They show nothing and read no host
// input: each dialog is answered a fixed stretch of guest time after it opens,
// from a script with one entry per dialog (the last entry repeats).
class ScriptedDialogClock {
public:
    using Clock=std::function<std::uint64_t()>;
    ScriptedDialogClock(Clock clock,std::uint64_t delay_us):clock_(std::move(clock)),delay_us_(delay_us){}
    void open(){opened_=clock_();}
    bool due() const {return clock_()-opened_>=delay_us_;}
    std::uint64_t now() const {return clock_();}
private:
    Clock clock_;std::uint64_t delay_us_,opened_{};
};
inline const std::string &scripted_entry(const std::vector<std::string> &script,std::size_t index,const std::string &fallback){
    return script.empty()?fallback:script[std::min(index,script.size()-1u)];
}
// Script entries: "focus" picks the slot the game put the cursor on (for a load
// or a delete, the first slot holding data when that one is empty), "cancel"
// backs out, a number picks that slot index. A pick stands for the player's
// answer to the question about an existing save as well: a scripted save writes
// over it and a scripted delete removes it.
class ScriptedSaveSlotPresenter final:public SaveSlotPresenter {
public:
    ScriptedSaveSlotPresenter(ScriptedDialogClock::Clock clock,std::uint64_t delay_us,std::vector<std::string> script)
        :clock_(std::move(clock),delay_us),script_(std::move(script)){}
    void open(const SaveSlotRequest &request) override {request_=request;open_=true;clock_.open();}
    std::optional<SaveSlotSelection> poll(std::uint64_t generation) override {
        if(!open_ || generation!=request_.generation || !clock_.due())return {};
        static const std::string focus="focus";
        const auto &entry=scripted_entry(script_,answered_++,focus);
        SaveSlotSelection selection{generation,SaveSlotChoice::Cancel,0u};
        // Load and delete alike take only a slot that holds a save.
        const bool load=request_.operation!=SaveSlotOperation::Save;
        const char *const what=request_.operation==SaveSlotOperation::Load?"load":request_.operation==SaveSlotOperation::Delete?"delete":"save";
        if(entry!="cancel"){
            std::size_t index=request_.initial_index;
            if(entry!="focus"){
                index=0u;
                for(const auto c:entry){
                    if(c<'0' || c>'9' || index>1000u)throw psprecomp::Error("CHILI_AUTO_SAVE_SLOT entries are focus, cancel or a slot index");
                    index=index*10u+static_cast<unsigned>(c-'0');
                }
            }else if(load && (index>=request_.slots.size() || !request_.slots[index].exists)){
                index=request_.slots.size();
                for(std::size_t i=0;i<request_.slots.size();++i)if(request_.slots[i].exists){index=i;break;}
            }
            if(index<request_.slots.size() && (!load || request_.slots[index].exists)){
                selection.choice=SaveSlotChoice::Choose;selection.index=index;
                selection.confirmed=request_.operation!=SaveSlotOperation::Load && request_.slots[index].exists;
            }
        }
        std::cout<<"[dialog] scripted "<<what<<" slots="<<request_.slots.size()<<" answer="
                 <<(selection.choice==SaveSlotChoice::Cancel?std::string("cancel"):"slot "+std::to_string(selection.index)+" "+request_.slots[selection.index].name)
                 <<" guest_us="<<clock_.now()<<std::endl;
        open_=false;
        return selection;
    }
    void close(std::uint64_t) noexcept override {open_=false;}
private:
    ScriptedDialogClock clock_;std::vector<std::string> script_;
    SaveSlotRequest request_;bool open_{};std::size_t answered_{};
};
// Script entries: "accept" answers Yes or OK, "decline" answers No or Back. A
// dialog that offers only Back gets Back either way.
class ScriptedMessageDialogPresenter final:public MessageDialogPresenter {
public:
    ScriptedMessageDialogPresenter(ScriptedDialogClock::Clock clock,std::uint64_t delay_us,std::vector<std::string> script)
        :clock_(std::move(clock),delay_us),script_(std::move(script)){}
    void open(const MessageDialogRequest &request) override {request_=request;open_=true;clock_.open();}
    std::optional<MessageDialogSelection> poll(std::uint64_t generation) override {
        if(!open_ || generation!=request_.generation || !clock_.due())return {};
        static const std::string accept="accept";
        const auto &entry=scripted_entry(script_,answered_++,accept);
        if(entry!="accept" && entry!="decline")throw psprecomp::Error("CHILI_AUTO_MESSAGE entries are accept or decline");
        const bool yes=entry=="accept";
        MessageDialogChoice choice;const char *name;
        if(request_.yes_no){choice=yes?MessageDialogChoice::Yes:MessageDialogChoice::No;name=yes?"yes":"no";}
        else if(request_.okay && (yes || !request_.allow_back)){choice=MessageDialogChoice::Okay;name="ok";}
        else{choice=MessageDialogChoice::Back;name="back";}
        auto text=request_.text.substr(0u,96u);
        for(auto &c:text)if(static_cast<unsigned char>(c)<32u)c=' ';
        std::cout<<"[dialog] scripted message answer="<<name<<" guest_us="<<clock_.now()<<" text=\""<<text<<"\""<<std::endl;
        open_=false;
        return MessageDialogSelection{generation,choice};
    }
    void close(std::uint64_t) noexcept override {open_=false;}
private:
    ScriptedDialogClock clock_;std::vector<std::string> script_;
    MessageDialogRequest request_;bool open_{};std::size_t answered_{};
};
}
