#include "psprecomp/runtime.hpp"
#include <stdexcept>
#include <iostream>
int main(){try{
for(unsigned low=0;low<16;++low){psprecomp::Runtime rt;psprecomp::register_generated_functions(rt);auto &c=rt.cpu();
const float col[]={1,2,3,4},row[]={1,5,6,7};c.write_vfpu_vector(col,0,4);c.write_vfpu_vector(row,32,4);
c.vfpu_ctrl[0]=0x12345;c.vfpu_ctrl[1]=0x98765;c.vfpu_ctrl[2]=0xFFF;
for(unsigned b=0;b<4;++b){c.gpr[4+b]=0x08810000+b*32+low;for(unsigned j=0;j<8;++j)rt.memory().store32(0x08810000+b*32+j*4,0xDEADBEEF);}
c.gpr[31]=0x08820000;rt.run(0x08804000,1);
for(unsigned b=0;b<4;++b)for(unsigned j=0;j<8;++j){unsigned expected=0xDEADBEEF,k=low/4;
if((b&1)==0 && j<=k)expected=std::bit_cast<unsigned>((b<2?col:row)[3-k+j]);
if((b&1)==1 && j>=k && j<4)expected=std::bit_cast<unsigned>((b<2?col:row)[j-k]);
if(rt.memory().load32(0x08810000+b*32+j*4)!=expected)throw std::runtime_error("VFPU partial store lane/sentinel mismatch");}
if(c.vfpu_ctrl[0]!=0x12345 || c.vfpu_ctrl[1]!=0x98765 || c.vfpu_ctrl[2]!=0xFFF)throw std::runtime_error("VFPU store consumed prefixes");
}
std::cout<<"PASS: generated SVL/SVR lane boundaries, row/column registers, ignored low address bits, preserved memory and prefixes\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
