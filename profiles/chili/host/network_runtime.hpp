#pragma once
#include "partition_memory.hpp"
#include <array>
namespace chili {
class NetworkRuntime {
public:
    NetworkRuntime(psprecomp::Runtime &,PartitionMemory &);
    ~NetworkRuntime();
    bool common_loaded() const {return common_;}
    bool adhoc_loaded() const {return adhoc_;}
    bool initialized() const {return pool_!=0u;}
private:
    void module(psprecomp::AllegrexContext &,bool load);
    void initialize(psprecomp::AllegrexContext &);
    void terminate();
    void mac(psprecomp::AllegrexContext &,bool to_string);
    psprecomp::Runtime &runtime_;PartitionMemory &memory_;
    bool common_{},adhoc_{},adhoc_initialized_{},winsock_{};
    std::uint32_t pool_{},callout_stack_{},interrupt_stack_{};
    std::array<std::uint8_t,6> local_mac_{};
};
}
