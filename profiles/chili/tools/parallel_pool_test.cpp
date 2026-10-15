#include "parallel_shade_pool.hpp"
#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>

int main(){try {
    unsigned checks=0;
    auto check=[&](bool value){++checks;if(!value)throw std::runtime_error("pool lifetime/order check");};
    for(unsigned lifecycle=0;lifecycle<16;++lifecycle){
        chili::ParallelShadePool pool(3);
        for(unsigned generation=0;generation<128;++generation){
            struct Context{std::array<std::atomic<unsigned>,257> hits{};std::array<unsigned,257> output{};unsigned salt{};} context;
            context.salt=lifecycle*1024+generation;
            const auto count=std::size_t((generation+lifecycle*17)%258);
            pool.run(count,[](void *opaque,std::size_t index) noexcept {
                auto &c=*static_cast<Context*>(opaque);
                c.hits[index].fetch_add(1,std::memory_order_relaxed);
                c.output[index]=c.salt+unsigned(index);
            },&context);
            for(std::size_t n=0;n<context.hits.size();++n){
                check(context.hits[n].load(std::memory_order_relaxed)==(n<count?1u:0u));
                check(context.output[n]==(n<count?context.salt+unsigned(n):0u));
            }
        }
    }
    std::cout<<"PASS "<<checks<<" synchronous pool lifetime/publication checks\n";
    return 0;
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
