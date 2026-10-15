#include "../host/ge_mip_texture.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
using chili::GeMipTexture;
using chili::GeMipSelection;
static void require(bool result,const char *message){if(!result)throw std::runtime_error(message);}
static void mip(psprecomp::GuestMemory &m,std::array<unsigned,256> &r,unsigned level,unsigned address,unsigned width,unsigned height,unsigned stride,unsigned format,bool swizzle,unsigned fill){
    r[0xA0+level]=address&0xFFFFFF;r[0xA8+level]=((address>>8)&0xF0000)|stride;
    r[0xB8+level]=unsigned(std::countr_zero(width))|(unsigned(std::countr_zero(height))<<8);
    const unsigned pitch=format==3?stride*4:format==4?stride/2:stride;
    const unsigned rows=swizzle?((height+7)&~7u):height;
    for(unsigned offset=0;offset<pitch*rows;++offset)m.store8(address+offset,0);
    for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
        const unsigned bx=format==3?x*4:format==4?x/2:x;
        const unsigned off=swizzle?(y/8)*pitch*8+(bx/16)*128+(y%8)*16+bx%16:y*pitch+bx;
        if(format==3)m.store32(address+off,fill);
        else if(format==4){const unsigned mask=15u<<((x&1)*4);m.store8(address+off,(m.load8(address+off)&~mask)|((fill&15)<<((x&1)*4)));}
        else m.store8(address+off,fill);
    }
}
static void hardware_lod_fixture(){
    psprecomp::GuestMemory m;std::array<unsigned,256> r{};std::string error;GeMipTexture view;GeMipSelection pick;unsigned actual{};
    r[0xC3]=3;r[0xC6]=6;r[0xC2]=7u<<16;
    for(unsigned level=0;level<8;++level){const unsigned size=256u>>level,c=level*16;
        mip(m,r,level,0x08810000+level*0x40000,size,size,std::max(size,4u),3,false,0xFF000000|c|(c<<8)|(c<<16));}
    // Primary reference: pspautotests/tests/gpu/textures/mipmap.expected.
    struct Case{float footprint;unsigned bias,color;};
    const Case cases[]={
        {1,0,0x000000},{0.25f,0,0x000000},{2,0,0x101010},{4,0,0x202020},{64,0,0x606060},{256,0,0x707070},
        {1,7,0x070707},{2,7,0x171717},{64,7,0x676767},{256,7,0x707070},
        {1,8,0x080808},{4,8,0x282828},{1,0x10,0x101010},{0.25f,0x10,0x000000},
        {0.25f,0x77,0x575757},{256,0x87,0x070707},{256,0x88,0x080808},
        {1,0xF0,0x000000},{4,0xF0,0x101010},{64,0xF0,0x505050},{256,0xF0,0x707070},
    };
    for(const auto &c:cases){r[0xC8]=c.bias<<16;
        require(view.configure(m,r,nullptr,error),"hardware AUTO fixture configure failed");
        require(view.select(c.footprint,1,pick,error)&&view.sample(m,0.5f,0.5f,pick,actual,error),"hardware AUTO fixture sample failed");
        require((actual&0xFFFFFF)==c.color,"hardware AUTO power/bias/clamp output mismatch");}
    // Current game's signed C8 bias is -1.5, not unsigned 14.5.
    r[0xC8]=0xE80000;require(view.configure(m,r,nullptr,error),"game bias configure failed");
    require(view.select(4,1,pick,error)&&pick.detail16==8&&pick.lower==0&&pick.upper==1&&pick.fraction16==8,"game negative LOD bias mismatch");
    // C8 reserved mode 3 acts as CONST in the PSP's expected output.
    for(unsigned mode:{1u,3u}){r[0xC8]=(24u<<16)|mode;require(view.configure(m,r,nullptr,error),"CONST configure failed");
        require(view.select(0,0,pick,error)&&pick.lower==1&&pick.upper==2&&pick.fraction16==8,"CONST/reserved mode selection mismatch");}
    // All valid C6 filters: levels are selected only with bit2; bit1 chooses
    // adjacent-mip blending; bit0 selects in-level nearest versus bilinear.
    for(unsigned filter:{0u,1u,4u,5u,6u,7u}){r[0xC6]=filter|(1u<<8);r[0xC8]=24u<<16|1u;
        require(view.configure(m,r,nullptr,error)&&view.select(0,0,pick,error),"C6 filter configure/select failed");
        require(pick.linear==bool(filter&1),"C6 in-level min filter mismatch");
        if(!(filter&4))require(pick.lower==0&&pick.upper==0&&view.max_level()==0,"nonmip filter selected other level");
        else if(filter&2)require(pick.lower==1&&pick.upper==2&&pick.fraction16==8,"mip-linear filter selection mismatch");
        else require(pick.lower==2&&pick.upper==2&&pick.fraction16==0,"nearest-mip tie selection mismatch");
        r[0xC8]=0xF00001;require(view.configure(m,r,nullptr,error)&&view.select(0,0,pick,error)&&pick.linear&&pick.lower==0,"magnification filter not selected at negative LOD");}
    // The SLOPE fixture uses identity matrices (clip W=1), two constant-color
    // levels, and slope=.5..1 in 1/32 steps. Its expected RGB values establish
    // exponent/mantissa-linear LOD and floor channel divisions at 1/16 steps.
    r[0xC6]=6;r[0xC2]=1u<<16;r[0xC8]=2;
    mip(m,r,0,0x08810000,2,2,4,3,false,0xFFFDFEFF);mip(m,r,1,0x08850000,1,1,4,3,false,0);
    const unsigned expected[]={0xFDFEFF,0xEDEEEF,0xDDDEDF,0xCDCECF,0xBDBEBF,0xADAEAF,0x9E9E9F,0x8E8E8F,
                               0x7E7F7F,0x6E6F6F,0x5E5F5F,0x4F4F4F,0x3F3F3F,0x2F2F2F,0x1F1F1F,0x0F0F0F};
    for(unsigned step=0;step<16;++step){const float slope=0.5f+float(step)/32.0f;r[0xD0]=std::bit_cast<unsigned>(slope)>>8;
        require(view.configure(m,r,nullptr,error)&&view.select(0,1,pick,error)&&view.sample(m,0.5f,0.5f,pick,actual,error),"hardware SLOPE fixture failed");
        require((actual&0xFFFFFF)==expected[step],"hardware SLOPE fraction/floor output mismatch");}
    r[0xD0]=std::bit_cast<unsigned>(0.5f)>>8;require(view.configure(m,r,nullptr,error)&&view.select(0,2,pick,error)&&pick.lower==1,"SLOPE did not use clip W");
    r[0xD0]=0x7F8000;require(view.configure(m,r,nullptr,error)&&!view.select(0,1,pick,error),"nonfinite slope accepted");
}
static void texture_fixture(){
    psprecomp::GuestMemory m;chili::GeClutCache palette;std::string error;GeMipTexture view;GeMipSelection pick;unsigned actual{};
    const unsigned pal=0x08808000;
    auto color=[](unsigned i){return 0xC0000000u|i|((255-i)<<8)|((i^0x55)<<16);};
    for(unsigned i=0;i<256;++i)m.store32(pal+i*4,color(i));
    require(palette.load(m,pal&0xFFFFFF,(pal>>8)&0xF0000,32,error),"mip fixture palette load failed");
    for(unsigned format:{3u,4u,5u})for(bool swizzle:{false,true})for(bool multi:{false,true}){
        std::array<unsigned,256> r{};r[0xC3]=format;r[0xC5]=0xFF03;r[0xC6]=6;r[0xC2]=(2u<<16)|(multi?0x100u:0u)|(swizzle?1u:0u);r[0xC8]=1;
        auto raw=[&](unsigned level,unsigned x,unsigned y,unsigned width){return format==4?(x+2*y+level)&15u:(x+y*width+level*17)&255u;};
        auto value=[&](unsigned level,unsigned x,unsigned y,unsigned width){
            if(format==3)return 0xFF000000u|(x+level*32)|((y*4+level)<<8)|((x^y)<<16);
            return color(raw(level,x,y,width)+(format==4&&multi?level*16u:0u));};
        for(unsigned level=0;level<3;++level){const unsigned width=(format==4?64u:format==5?32u:16u)>>level,height=16u>>level;
            const unsigned stride=std::max(width,format==4?32u:format==5?16u:4u),address=0x08810000+level*0x10000;
            mip(m,r,level,address,width,height,stride,format,swizzle,0);
            // Write physical storage independently in block order for swizzle.
            const unsigned pitch=format==3?stride*4:format==4?stride/2:stride;
            if(swizzle){unsigned cursor=address;
                for(unsigned by=0;by<(height+7)/8;++by)for(unsigned bx=0;bx<pitch/16;++bx)for(unsigned y=0;y<8;++y)for(unsigned b=0;b<16;++b){
                    const unsigned byte_x=bx*16+b,gy=by*8+y;
                    unsigned packed=0;
                    if(gy<height){
                        if(format==3){const unsigned x=byte_x/4;if(x<width)packed=value(level,x,gy,width)>>((byte_x%4)*8)&255;}
                        else if(format==4){const unsigned x=byte_x*2;if(x<width)packed=raw(level,x,gy,width)|(raw(level,x+1,gy,width)<<4);}
                        else if(byte_x<width)packed=raw(level,byte_x,gy,width);}
                    m.store8(cursor++,packed);}
            }else for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
                if(format==3)m.store32(address+(y*stride+x)*4,value(level,x,y,width));
                else if(format==4){if(!(x&1))m.store8(address+y*pitch+x/2,raw(level,x,y,width)|(raw(level,x+1,y,width)<<4));}
                else m.store8(address+y*pitch+x,raw(level,x,y,width));}
        }
        require(view.configure(m,r,format==3?nullptr:&palette,error),"mip indexed/direct prepare failed");
        for(unsigned level=0;level<3;++level){r[0xC8]=(level*16u<<16)|1;
            require(view.configure(m,r,format==3?nullptr:&palette,error)&&view.select(0,1,pick,error),"explicit level selection failed");
            const unsigned width=view.level(level).width,height=view.level(level).height;
            for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){
                require(view.sample(m,(x+0.5f)/width,(y+0.5f)/height,pick,actual,error),"mip grid sample failed");
                require(actual==value(level,x,y,width),"mip level register/stride/swizzle/palette sharing mismatch");}
        }
        // Bilinear crosses both tile axes at level0; then mip-linear blends
        // that independently filtered RGBA with level1's filtered value.
        r[0xC6]=7;r[0xC8]=(8u<<16)|1;r[0xC7]=0x101;
        require(view.configure(m,r,format==3?nullptr:&palette,error)&&view.select(0,1,pick,error)&&pick.fraction16==8&&pick.linear,"trilinear selection failed");
        const float s=format==3?0.25f:0.5f,t=0.5f;
        unsigned per_level[2]{};
        for(unsigned level=0;level<2;++level){const unsigned width=view.level(level).width,height=view.level(level).height;
            const unsigned x=unsigned(s*width)-1,y=unsigned(t*height)-1;
            const unsigned samples[]={value(level,x,y,width),value(level,x+1,y,width),value(level,x,y+1,width),value(level,x+1,y+1,width)};
            for(unsigned ch=0;ch<4;++ch){unsigned sum=0;for(unsigned c:samples)sum+=(c>>(ch*8))&255;per_level[level]|=(sum/4)<<(ch*8);}}
        unsigned expected=0;for(unsigned ch=0;ch<4;++ch)expected|=((((per_level[0]>>(ch*8))&255)+((per_level[1]>>(ch*8))&255))/2)<<(ch*8);
        require(view.sample(m,s,t,pick,actual,error)&&actual==expected,"trilinear decoded color/tile corner/floor mismatch");
        // Failure leaves the prepared view intact and cannot trigger a commit.
        auto broken=r;broken[0xA1]=0xFFFFF0;broken[0xA9]=0x090000|view.level(1).stride;
        m.store32(0x04000000,0xAABBCCDD);
        if(view.configure(m,broken,format==3?nullptr:&palette,error))m.store32(0x04000000,0);
        require(m.load32(0x04000000)==0xAABBCCDD,"missing mip data reached framebuffer commit");
        require(view.sample(m,s,t,pick,actual,error)&&actual==expected,"failed prepare replaced previous view");
    }
    // Multi-CLUT requires loaded entries for every reachable T4 level, while
    // shared mode legitimately reuses a sixteen-entry palette for all levels.
    std::array<unsigned,256> r{};r[0xC3]=4;r[0xC5]=0xFF03;r[0xC6]=6;r[0xC2]=(1u<<16)|0x101;r[0xC8]=1;
    mip(m,r,0,0x08810000,32,8,32,4,true,0);mip(m,r,1,0x08820000,16,4,32,4,true,0);
    chili::GeClutCache small;require(small.load(m,pal&0xFFFFFF,(pal>>8)&0xF0000,2,error),"small mip CLUT load failed");
    require(!view.configure(m,r,&small,error),"multiclut missing per-level palette accepted");r[0xC2]&=~0x100;
    require(view.configure(m,r,&small,error),"shared mip palette incorrectly required separate colors");
    r[0xC2]|=0x100;r[0xC5]=0x0FFF03;
    require(!view.configure(m,r,&palette,error),"unverified multiclut cache overflow accepted");
    r[0xC5]=0xFF03;r[0xC2]&=~0x100;r[0xC6]=2;
    require(!view.configure(m,r,&palette,error),"reserved mip filter accepted");
    r[0xC6]=7;r[0xC8]=0;require(view.configure(m,r,&small,error),"invalid-input fixture setup failed");
    pick.lower=7;actual=0xDEADBEEF;require(!view.sample(m,0,0,pick,actual,error)&&actual==0xDEADBEEF,"invalid mip selection changed output");
    require(!view.select(std::numeric_limits<float>::quiet_NaN(),1,pick,error),"nonfinite mip footprint accepted");
    require(view.select(1,1,pick,error)&&!view.sample(m,std::numeric_limits<float>::infinity(),0,pick,actual,error),"nonfinite texture coordinate accepted");
}
int main(){try{hardware_lod_fixture();texture_fixture();std::cout<<"Native mip sampler hardware-reference LOD and storage/palette fixtures passed\n";}
catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
