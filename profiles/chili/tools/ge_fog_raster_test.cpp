#include "ge_runtime.hpp"
#include "ge_fog_fixture_data.hpp"
#include <iostream>
#include <limits>
using namespace chili;
static unsigned assertions{};
static void require(bool result,const std::string &why){++assertions;if(!result)throw std::runtime_error(why);}
struct Fixture {
    psprecomp::Runtime rt;GeClutCache clut;std::array<unsigned,256> r{};ge::DeformState deformation{};
    std::array<float,12> world{1,0,0,0,1,0,0,0,1,0,0,0},view=world;
    std::array<float,16> projection{1,0,0,0,0,1,0,0,0,0,0,0,0,0,0,1};
    unsigned vertices=0x08804000,indices=0x08807000,end=0xDEADBEEF,iend=0xBAADF00D,written=0xABCD;std::string error;
    Fixture(){r[0x12]=0x19D;r[0xD2]=3;r[0x9D]=512;r[0x9E]=0x80000;r[0x9F]=512;r[0xD5]=r[0x16]=7|(7<<10);r[0xD7]=65535;
        r[0x4C]=r[0x4D]=2046*16;r[0x1C]=1;r[0x1F]=1;r[0xCF]=0xFF33FF;r[0x58]=r[0x5D]=255;
        regf(0xCD,1);regf(0xCE,1);regf(0x42,2);regf(0x43,2);regf(0x44,100);regf(0x45,2048);regf(0x46,2048);regf(0x47,100);regf(0x48,1);regf(0x49,1);
        for(auto &bone:deformation.bones)bone=world;deformation.morph_weights[0]=1;source();reset();}
    void regf(unsigned index,float value){r[index]=std::bit_cast<unsigned>(value)>>8;}
    void encode(unsigned vertex,ge::Vec3 p,ge::Color color={0,17,136,128}){const auto layout=ge::make_layout(r[0x12]);const unsigned a=vertices+vertex*layout.vertex_stride;rt.memory().zero(a,layout.vertex_stride);
        if(layout.weight_type)rt.memory().store32(a+layout.weight_offset,std::bit_cast<unsigned>(1.f));
        unsigned packed=0;for(unsigned k=0;k<4;++k)packed|=unsigned(color[k])<<(k*8);if(layout.color_type)rt.memory().store32(a+layout.color_offset,packed);
        if(layout.normal_type)for(unsigned k=0;k<3;++k)rt.memory().store32(a+layout.normal_offset+k*4,std::bit_cast<unsigned>(k==2?1.f:0.f));
        for(unsigned k=0;k<3;++k)rt.memory().store32(a+layout.position_offset+k*4,std::bit_cast<unsigned>(p[k]));}
    void source(){for(unsigned v=0;v<3;++v)encode(v,{v==1?1.f:-1.f,v==2?1.f:-1.f,0});}
    void reset(){for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){rt.memory().store32(0x04000000+(y*512+x)*4,0xAA112233);rt.memory().store16(0x04080000+(y*512+x)*2,1000);}end=0xDEADBEEF;iend=0xBAADF00D;written=0xABCD;}
    unsigned pixel(unsigned x,unsigned y){return rt.memory().load32(0x04000000+(y*512+x)*4);}
    std::vector<std::uint8_t> vram(){std::vector<std::uint8_t> bytes(psprecomp::GuestMemory::kVramSize);rt.memory().copy_out(0x04000000,bytes);return bytes;}
    bool draw(unsigned count=3,unsigned primitive=3,const ge::DeformState *state=nullptr){return render_ge_triangle_raster(rt.memory(),r,world,view,projection,vertices,(primitive<<16)|count,end,error,clut,&written,indices,&iend,state);}
    template<class Expected>void exact(Expected expected,bool clipped=false){for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){const bool inside=x+y<=3&&(!clipped||x+y>=2);
        require(pixel(x,y)==(inside?expected(x,y):0xAA112233),"visible pixel "+std::to_string(x)+","+std::to_string(y));}}
    void runtime_draw(unsigned count=3,unsigned primitive=3){GeRuntime gpu(rt);unsigned cursor=0x08809000;auto emit=[&](unsigned command,unsigned value){rt.memory().store32(cursor,(command<<24)|(value&0xFFFFFF));cursor+=4;};
        for(unsigned command=0x12;command<256;++command)if(r[command])emit(command,r[command]);
        auto matrix=[&](unsigned command,const auto &values){emit(command,0);for(float value:values)emit(command+1,std::bit_cast<unsigned>(value)>>8);};matrix(0x3A,world);matrix(0x3C,view);matrix(0x3E,projection);
        if(ge::make_layout(r[0x12]).needs_skinning()){emit(0x2A,0);for(const auto &bone:deformation.bones)for(float value:bone)emit(0x2B,std::bit_cast<unsigned>(value)>>8);}
        emit(0x10,0x080000);emit(1,vertices&0xFFFFFF);emit(2,indices&0xFFFFFF);emit(4,(primitive<<16)|count);emit(0x0F,0xBEEF);emit(0x0C,0);
        rt.cpu().gpr[4]=0x08809000;rt.cpu().gpr[6]=0xFFFFFFFF;rt.invoke_import("sceGe_user",0xAB49E76Au,rt.cpu());auto layout=ge::make_layout(r[0x12]);
        require(!rt.stopped()&&gpu.completed.size()==1&&gpu.vertex_address==(layout.index_bytes?vertices:vertices+count*layout.vertex_stride)&&gpu.index_address==(layout.index_bytes?indices+count*layout.index_bytes:indices),"streamed fog runtime/cursors: "+rt.stop_reason());}
    void specular_texture(){r[0x12]=0x1FF;r[0x17]=r[0x18]=1;r[0x5F]=r[0x5E]=1;r[0x57]=0x800000;r[0x91]=0xFFFFFF;regf(0x65,1);regf(0x5B,1);
        r[0x1E]=1;r[0xC3]=3;r[0xC9]=0x100;r[0xC7]=0x101;r[0xA0]=0x820000;r[0xA8]=0x080001;r[0xB8]=0;rt.memory().store32(0x08820000,0xFF000000);regf(0xCD,0.5f);source();r[0xCF]=0;r[0x58]=128;}
};
static unsigned fog_recorded(int factor){return 0xAA000000|(chili::fog_fixture::recorded[std::clamp(factor,0,255)]&0xFFFFFF);}
static void camera_provenance(){
    {Fixture f;f.regf(0xCD,0.5f);f.runtime_draw();f.exact([](auto,auto){return fog_recorded(128);});}
    // Camera z changes with world and view translation while projected depth
    // remains100. Fog must not be derived from screen z or abs(camera z).
    {Fixture f;f.world[11]=-2;f.view[11]=1.5f;f.runtime_draw();f.exact([](auto,auto){return fog_recorded(128);});}
    {Fixture f;f.r[0x12]|=3<<9;f.source();f.deformation.bones[0][11]=-0.5f;f.runtime_draw();f.exact([](auto,auto){return fog_recorded(128);});}
    {Fixture f;f.r[0x12]|=0x1000;f.source();for(unsigned n=0;n<3;++n)f.rt.memory().store16(f.indices+n*2,n);f.regf(0xCD,0.5f);f.runtime_draw();f.exact([](auto,auto){return fog_recorded(128);});}
    std::cout<<"PASS signed camera Z before projection, world/view/bone translation and streamed index16 dual cursors\n";
}
static void interpolation(){
    for(unsigned flat:{0u,1u}){Fixture f;f.r[0x50]=!flat;f.encode(0,{-1,-1,-1});f.encode(1,{1,-1,-0.5f});f.encode(2,{-1,1,0});
        require(f.draw()&&f.written==10,"varying native fog: "+f.error);f.exact([](unsigned x,unsigned y){return fog_recorded(32*x+64*y+42);});}
    for(unsigned flat:{0u,1u}){Fixture f;f.r[0x50]=!flat;f.projection[11]=1;f.projection[15]=0;f.regf(0xCD,0);f.regf(0xCE,0.25f);
        f.encode(0,{-1,-1,1});f.encode(1,{2,-2,2});f.encode(2,{-4,4,4});f.runtime_draw();f.exact([](unsigned x,unsigned y){return fog_recorded(16*x+48*y+92);});}
    {Fixture f;f.regf(0xCD,0);f.encode(0,{-1,-1,-1});f.encode(1,{1,-1,0});f.encode(2,{-1,1,3});require(f.draw(),"raw factor span: "+f.error);
        f.exact([](unsigned x,unsigned y){return fog_recorded(-116+64*int(x)+256*int(y));});}
    // Near clipping intersects fog along the same source edge; flat colors do
    // not flatten fog. Expected factor bytes are112 and240 on the two rows.
    for(unsigned flat:{0u,1u}){Fixture f;f.r[0x50]=!flat;f.projection[10]=1;f.encode(0,{-1,-1,-2});
        require(f.draw()&&f.written==7,"near-clipped fog: "+f.error);f.exact([](unsigned x,unsigned y){return fog_recorded(128*int(x+y)-144);},true);}
    for(unsigned primitive:{4u,5u}){Fixture f;f.regf(0xCD,0.5f);const ge::Vec3 strip[]={{-1,-1,0},{1,-1,0},{-1,1,0},{1,1,0}},fan[]={{-1,-1,0},{1,-1,0},{1,1,0},{-1,1,0}};
        for(unsigned n=0;n<4;++n)f.encode(n,primitive==4?strip[n]:fan[n]);f.runtime_draw(4,primitive);unsigned coverage=0;
        for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){const bool inside=x<4&&y<4;require(f.pixel(x,y)==(inside?fog_recorded(128):0xAA112233),"fogged strip/fan");coverage+=inside;}require(coverage==16,"fogged strip/fan visible area");}
    std::cout<<"PASS raw affine fog independent of flat/Gouraud and varying W, late clamp, near clipping, strip/fan\n";
}
static void pipeline_and_bypass(){
    {Fixture f;f.specular_texture();f.runtime_draw();f.exact([](auto,auto){return 0xAA400000;});}
    {Fixture f;f.specular_texture();f.r[0x22]=1;f.r[0xDB]=0xFF8002;f.r[0x23]=1;f.r[0xDE]=4;require(f.draw()&&f.written==10,"fog alpha/depth");f.exact([](auto,auto){return 0xAA400000;});require(f.rt.memory().load16(0x04080000)==100,"fog depth100");
        f.reset();f.r[0xDB]=0xFF8102;require(f.draw()&&f.written==0,"fog alpha unchanged");f.exact([](auto,auto){return 0xAA112233;});}
    {Fixture f;f.specular_texture();f.r[0x21]=1;f.r[0xDF]=0x32;require(f.draw(),"fog before blend");f.exact([](auto,auto){return 0xAA281119;});}
    {Fixture f;f.specular_texture();f.r[0xE8]=0xFF0000;require(f.draw(),"fog before masks");f.exact([](auto,auto){return 0xAA110000;});}
    {Fixture f;f.specular_texture();f.r[0xC9]=0x103;f.rt.memory().store32(0x08820000,0xFF800000);require(f.draw(),"fog after REPLACE and specular saturation");f.exact([](auto,auto){return 0xAA800000;});}
    {Fixture f;f.r[0x1F]=0;f.r[0xCD]=0x7FC000;f.r[0xCE]=0x7F8000;require(f.draw(),"disabled fog unused coefficients");f.exact([](auto,auto){return 0xAA881100;});}
    {Fixture f;f.r[0x12]|=0x800000;f.r[0xCD]=0x7FC000;f.r[0xCE]=0x7F8000;f.encode(0,{0,0,100});f.encode(1,{4,0,100});f.encode(2,{0,4,100});require(f.draw(),"through fog bypass");f.exact([](auto,auto){return 0xAA881100;});}
    std::cout<<"PASS texture/specular before fog, alpha unchanged, native depth/blend/mask ordering and through/disabled bypass\n";
}
static void unsupported_atomic(){auto check=[](Fixture &f,const char *label){const auto before=f.vram();require(!f.draw(),label);require(f.vram()==before&&f.end==0xDEADBEEF&&f.iend==0xBAADF00D&&f.written==0xABCD,std::string(label)+" changed VRAM/cursors/count");};
    {Fixture f;f.r[0xCD]=0x7FC000;check(f,"nonfinite fog end");}{Fixture f;f.r[0xCE]=0x7F8000;check(f,"infinite fog slope");}
    {Fixture f;f.regf(0xCD,0);f.regf(0xCE,2);f.encode(2,{-1,1,std::numeric_limits<float>::max()});check(f,"late fog factor overflow");}
    {Fixture f;f.r[0x24]=1;check(f,"stencil remains strict with fog");}{Fixture f;f.specular_texture();f.r[0xC0]=2;check(f,"environment UV remains strict with fog");}
    {Fixture f;f.specular_texture();f.r[0xA0]=0xFFFFF0;f.r[0xA8]=0x090008;f.r[0xB8]=3;check(f,"fog invalid texture");}
    {Fixture f;f.r[0x12]|=0x1000;f.source();for(unsigned n=0;n<3;++n)f.rt.memory().store16(f.indices+n*2,n);const auto before=f.vram();GeRuntime gpu(f.rt);gpu.commands=f.r;gpu.commands[4]=0x04123456;gpu.world=f.world;gpu.view=f.view;gpu.projection=f.projection;gpu.vertex_address=f.vertices;gpu.index_address=f.indices;gpu.commands[0xCE]=0x7FC000;
        f.rt.memory().store32(0x08809000,0x04030003);f.rt.cpu().gpr[4]=0x08809000;f.rt.cpu().gpr[6]=0xFFFFFFFF;f.rt.invoke_import("sceGe_user",0xAB49E76Au,f.rt.cpu());
        require(f.rt.stopped()&&gpu.completed.empty()&&gpu.commands[4]==0x04123456&&gpu.vertex_address==f.vertices&&gpu.index_address==f.indices&&f.vram()==before,"failed runtime fog published draw or partial effects");}
    std::cout<<"PASS nonfinite/late overflow and existing unsupported states atomic over full2MiB VRAM, dual cursors and runtime completion\n";
}
int main(){try{camera_provenance();interpolation();pipeline_and_bypass();unsupported_atomic();std::cout<<"PASS "<<assertions<<" native fog assertions; exact intermediate GPU precision remains unverified\n";
}catch(const std::exception &failure){std::cerr<<"FAIL "<<failure.what()<<'\n';return 1;}}
