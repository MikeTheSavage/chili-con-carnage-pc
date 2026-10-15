#include "../host/ge_mip_texture.hpp"
#include <iostream>
#include <stdexcept>
static void require(bool r,const std::string &message){if(!r)throw std::runtime_error(message);}
static unsigned mixed(unsigned a,unsigned b){unsigned out=0;for(unsigned channel=0;channel<4;++channel)out|=((((a>>(channel*8))&255)+((b>>(channel*8))&255))/2)<<(channel*8);return out;}
struct Fixture{
    psprecomp::GuestMemory m;std::array<unsigned,256> regs{};chili::GeMipTexture sampler;std::string error;
    unsigned format;bool swizzle;std::array<unsigned,5> expected{};
    Fixture(unsigned f,bool s):format(f),swizzle(s){regs[0xC3]=f;regs[0xC2]=(4u<<16)|(s?1u:0u);regs[0xC6]=6;regs[0xC7]=0x101;regs[0xC8]=1;
        const unsigned direct_words[4][5]={{0x001F,0x07E0,0xF800,0xFFFF,0},{0x801F,0x83E0,0xFC00,0xFFFF,0x8000},{0xF00F,0xF0F0,0xFF00,0xFFFF,0xF000},{0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFFFFFFFF,0xFF000000}};
        const unsigned dxt_words[]={0xF800,0x07E0,0x001F,0xFFFF,0};const unsigned dxt_colors[]={0x0000F8,0x00FC00,0xF80000,0xF8FCF8,0};
        for(unsigned level=0;level<5;++level){const unsigned size=16u>>level,address=0x08810000+level*0x10000;
            // Uncompressed raw buffer widths use16-byte row quanta even in
            // linear mode. Keep this format-decoding fixture's written storage
            // equal to its declared effective layout; raw lowbits/zero have a
            // separate unique-texel pitch fixture.
            const unsigned stride=f>=8?std::max(size,4u):std::max(size,f==3?4u:8u);
            regs[0xA0+level]=address&0xFFFFFF;regs[0xA8+level]=0x080000|stride;regs[0xB8+level]=(4-level)|((4-level)<<8);
            if(f>=8){const unsigned block=f==8?8u:16u,rows=(size+3)/4;
                expected[level]=dxt_colors[level]|((f==8?255:f==9?(level+1)*16:128+level*16)<<24);
                for(unsigned by=0;by<rows;++by)for(unsigned bx=0;bx<stride/4;++bx){const unsigned at=address+(by*(stride/4)+bx)*block;
                    for(unsigned row=0;row<4;++row)m.store8(at+row,0);m.store16(at+4,dxt_words[level]);m.store16(at+6,0);
                    if(f==9)for(unsigned row=0;row<4;++row)m.store16(at+8+row*2,(level+1)*0x1111u);
                    else if(f==10){m.store32(at+8,0);m.store16(at+12,0);m.store8(at+14,128+level*16);m.store8(at+15,0);}}
            }else{expected[level]=direct_words[3][level];const unsigned pixel=f==3?4u:2u,pitch=stride*pixel;
                for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x){const unsigned bx=x*pixel;
                    const unsigned offset=s?(y/8u)*pitch*8u+(bx/16u)*128u+(y%8u)*16u+bx%16u:y*pitch+bx;
                    if(f==3)m.store32(address+offset,direct_words[f][level]);else m.store16(address+offset,direct_words[f][level]);}}
        }
    }
    void prepare(){require(sampler.configure(m,regs,nullptr,error),error);}
    chili::GeMipSelection pick(unsigned lod16){regs[0xC8]=(lod16<<16)|1;prepare();chili::GeMipSelection out;require(sampler.select(0,1,out,error),error);return out;}
    unsigned sample(float u,float v,const chili::GeMipSelection &pick){unsigned out=0xDEADBEEF;require(sampler.sample(m,u,v,pick,out,error),error);return out;}
};
static void levels_filtering_and_preflight(){
    for(unsigned format:{0u,1u,2u,3u,8u,9u,10u})for(bool swizzle:{false,true}){
        if(format>=8&&swizzle)continue;Fixture f(format,swizzle);f.prepare();
        for(unsigned level=0;level<5;++level){const auto pick=f.pick(level*16);const unsigned size=16u>>level;
            for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x)
                require(f.sample((x+.5f)/size,(y+.5f)/size,pick)==f.expected[level],"direct/DXT mip level register or tiny block allocation mismatch");
            if(format>=8&&size<4)require(f.sampler.level(level).storage_bytes==(format==8?8u:16u),"tiny DXT mip storage not rounded to full block");}
        f.regs[0xC6]=7;const auto blend=f.pick(24);
        require(blend.lower==1&&blend.upper==2&&blend.linear&&blend.fraction16==8,"new texture format altered mip selection");
        const unsigned expected=mixed(f.expected[1],f.expected[2]);
        require(f.sample(.375f,.625f,blend)==expected,"decoded DXT/direct trilinear color/alpha floor mismatch");
        const auto valid=f.regs;f.regs[0xA4]=0xFFF000;f.regs[0xAC]=0x0A0000|f.sampler.level(4).stride;
        f.m.store32(0x04000000,0xAABBCCDD);
        if(f.sampler.configure(f.m,f.regs,nullptr,f.error))f.m.store32(0x04000000,0);
        require(f.m.load32(0x04000000)==0xAABBCCDD&&f.sample(.375f,.625f,blend)==expected,"missing last mip reached commit or replaced prepared view");
        f.regs[0xC6]=0;require(f.sampler.configure(f.m,f.regs,nullptr,f.error)&&f.sampler.max_level()==0,"nonmipmap filter unnecessarily required unused DXT/direct levels");
        f.regs=valid;f.prepare();
        if(format>=8){f.regs[0xC2]|=1;require(!f.sampler.configure(f.m,f.regs,nullptr,f.error)&&f.sample(.375f,.625f,blend)==expected,"unverified compressed swizzle changed mip view");}
    }
}
static void compressed_bilinear_cross_block(){
    for(unsigned format:{8u,9u,10u}){Fixture f(format,false);const unsigned block=format==8?8u:16u,address=0x08810000;
        const unsigned endpoints[]={0xF800,0x07E0,0x001F,0xFFFF};
        for(unsigned by=0;by<2;++by)for(unsigned bx=0;bx<2;++bx){const unsigned at=address+(by*4+bx)*block,id=by*2+bx;
            f.m.store16(at+4,endpoints[id]);if(format==9)for(unsigned row=0;row<4;++row)f.m.store16(at+8+row*2,0xFFFF);
            if(format==10)f.m.store8(at+14,240);}
        f.regs[0xC6]=0x107;const auto base=f.pick(0);require(base.linear,"DXT crossing fixture did not select linear magnification");const unsigned alpha=format==8?255:240;
        require(f.sample(.25f,.25f,base)==(0x7C7E7C|(alpha<<24)),"DXT bilinear failed crossing both compressed block axes");
        const auto blend=f.pick(8);const unsigned expected=mixed(0x7C7E7C|(alpha<<24),f.expected[1]);
        require(f.sample(.25f,.25f,blend)==expected,"DXT bilinear plus inter-level alpha interpolation mismatch");
    }
}
int main(){try{levels_filtering_and_preflight();compressed_bilinear_cross_block();
    std::cout<<"Native direct16/DXT five-level selection, bilinear/trilinear and strict preflight fixtures passed\n";
}catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
