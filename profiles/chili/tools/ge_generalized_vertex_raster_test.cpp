#include "ge_runtime.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
using namespace chili;
void require(bool v,const std::string &why){if(!v)throw std::runtime_error(why);}
struct Fixture {
    psprecomp::Runtime rt;GeClutCache clut;std::array<unsigned,256> r{};
    std::array<float,12> world{1,0,0,0,1,0,0,0,1,0,0,0},view=world;
    std::array<float,16> projection{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    unsigned v=0x08804000,idx=0x08807000,end=0xDEADBEEF,iend=0xBAADF00D,written=0xABCD;
    std::string error;
    void regf(unsigned at,float f){r[at]=std::bit_cast<unsigned>(f)>>8;}
    void reset(){for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){rt.memory().store32(0x04000000+(y*512+x)*4,0xAA112233);rt.memory().store16(0x04080000+(y*512+x)*2,1000);}end=0xDEADBEEF;iend=0xBAADF00D;written=0xABCD;}
    Fixture(){r[0x12]=0x180;r[0xD2]=3;r[0x9D]=512;r[0x9E]=0x80000;r[0x9F]=512;r[0xD5]=r[0x16]=7|(7<<10);r[0xD7]=65535;
      r[0x55]=0x223311;r[0x58]=0x66;regf(0x42,2);regf(0x43,2);regf(0x44,100);regf(0x45,2048);regf(0x46,2048);regf(0x47,100);
      r[0x4C]=r[0x4D]=2046*16;r[0x1C]=1;regf(0x48,1);regf(0x49,1);reset();}
    unsigned pixel(unsigned x,unsigned y){return rt.memory().load32(0x04000000+(y*512+x)*4);}
    bool draw(unsigned count=3,unsigned primitive=3){return render_ge_triangle_raster(rt.memory(),r,world,view,projection,v,(primitive<<16)|count,end,error,clut,&written,idx,&iend);}
    void fields(unsigned n,float x,float y,float z,float u=0,float tv=0){
      auto l=ge::make_layout(r[0x12]);std::vector<std::uint8_t>b(l.vertex_stride,0xCC);
      auto put=[&](unsigned at,unsigned size,unsigned value){for(unsigned i=0;i<size;++i)b[at+i]=std::uint8_t(value>>(i*8));};
      auto component=[&](unsigned at,unsigned type,float value,bool norm){unsigned size=1u<<(type-1);unsigned bits=type==3?std::bit_cast<unsigned>(value):unsigned(int(value*(norm?(type==1?128:32768):1)));put(at,size,bits);};
      constexpr unsigned size[]={0,1,2,4};
      if(l.uv_type){component(l.uv_offset,l.uv_type,u,!l.through);component(l.uv_offset+size[l.uv_type],l.uv_type,tv,!l.through);}
      if(l.color_type)put(l.color_offset,l.color_type==7?4:2,l.color_type==4?31:l.color_type==5?0x801F:l.color_type==6?0xF00F:0xFF0000FF);
      if(l.normal_type)for(unsigned i=0;i<3;++i)component(l.normal_offset+size[l.normal_type]*i,l.normal_type,0,true);
      component(l.position_offset,l.position_type,x,!l.through);component(l.position_offset+size[l.position_type],l.position_type,y,!l.through);component(l.position_offset+2*size[l.position_type],l.position_type,z,!l.through);
      for(unsigned i=0;i<b.size();++i)rt.memory().store8(v+n*l.vertex_stride+i,b[i]);
    }
    void triangle(){fields(0,-0.5f,-0.5f,0);fields(1,0.5f,-0.5f,0);fields(2,-0.5f,0.5f,0);world[0]=world[4]=2;}
    void exact(unsigned color,unsigned shifted=0){for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){bool inside=shifted?(x>=1&&y>=1&&x+y<=4):(x+y<=3);require(pixel(x,y)==(inside?color:0xAA112233),"Exact coverage/color mismatch at "+std::to_string(x)+","+std::to_string(y));}}
};
void format_matrix(){
    Fixture f;unsigned cases=0;
    for(unsigned uv=0;uv<4;++uv)for(unsigned col : {0u,4u,5u,6u,7u})for(unsigned normal=0;normal<4;++normal)for(unsigned pos=1;pos<4;++pos){
      f.r[0x12]=uv|(col<<2)|(normal<<5)|(pos<<7);f.reset();f.triangle();auto l=ge::make_layout(f.r[0x12]);
      require(f.draw()&&f.written==10&&f.end==f.v+3*l.vertex_stride&&f.iend==f.idx,"General source layout draw failed: "+f.error);
      f.exact(col?0xAA0000FF:0xAA223311);++cases;
    }
    require(cases==240,"Incomplete renderer format matrix");
    std::cout<<"PASS:240 no-skin/morph1 source format combinations preserve exact coverage, natural strides, absent-color material ambient, optional unused normals\n";
}
void texture_uv(){
    Fixture f;auto &m=f.rt.memory();const unsigned tex=0x08820000;
    f.r[0x1E]=1;f.r[0xA0]=tex&0xFFFFFF;f.r[0xA8]=((tex>>8)&0xF0000)|2;f.r[0xB8]=0x101;f.r[0xC3]=3;f.r[0xC9]=0x103;
    m.store32(tex,0xFF000000);m.store32(tex+4,0xFFABCDEF);m.store32(tex+8,0);m.store32(tex+12,0);
    for(unsigned uv=1;uv<=3;++uv){f.r[0x12]=0x180|(7u<<2)|uv;f.reset();f.triangle();for(unsigned i=0;i<3;++i)f.fields(i,i==1?0.5f:-0.5f,i==2?0.5f:-0.5f,0,0.5f,0);
      require(f.draw(),"3D UV type failed: "+f.error);f.exact(0xAAABCDEF);}
    for(unsigned uv=1;uv<=3;++uv){f.r[0x12]=0x800100|uv;f.reset();f.fields(0,0,0,65535,1,0);f.fields(1,4,0,65535,1,0);f.fields(2,0,4,65535,1,0);require(f.draw(),"Through UV type failed: "+f.error);f.exact(0xAAABCDEF);}
    f.r[0x12]=0x800100;f.reset();f.fields(0,0,0,1);f.fields(1,4,0,1);f.fields(2,0,4,1);
    require(!f.draw()&&f.end==0xDEADBEEF&&f.iend==0xBAADF00D&&f.written==0xABCD&&f.pixel(0,0)==0xAA112233,"Missing textured UV did not fail atomically");
    std::cout<<"PASS: UV8/16/float 3D normalization and raw through texel units sample exact same texel; absent textured UV rejects atomically\n";
}
void through_float(){
    Fixture f;f.r[0x12]=0x800180;f.world.fill(std::numeric_limits<float>::quiet_NaN());f.view=f.world;f.projection.fill(std::numeric_limits<float>::quiet_NaN());
    for(float shift : {-0.00001f,0.49999f,0.5f}){f.reset();f.fields(0,shift,shift,100.9f);f.fields(1,4+shift,shift,100.9f);f.fields(2,shift,4+shift,100.9f);
      require(f.draw()&&f.end==f.v+36,"Finite throughfloat draw failed: "+f.error);f.exact(0xAA223311,shift==0.5f);require(f.written==(shift==0.5f?6u:10u),"Throughfloat subpixel truncation boundary mismatch");}
    f.reset();f.fields(0,32768,0,0);require(!f.draw()&&f.end==0xDEADBEEF&&f.iend==0xBAADF00D&&f.pixel(0,0)==0xAA112233,"Throughfloat range gate was not atomic");
    std::cout<<"PASS: throughfloat XY truncation at 1/16 pixel, negative nearzero, halfpixel coverage boundary, unused matrices, explicit safe-range gate\n";
}
void indexed_raster_and_runtime(){
    for(unsigned type : {1u,2u}){
      Fixture f;auto &m=f.rt.memory();f.r[0x12]=0x180|(type<<11);f.triangle();for(unsigned i=0;i<3;++i){if(type==1)m.store8(f.idx+i,(i+2)%3);else m.store16(f.idx+i*2,(i+2)%3);}
      require(f.draw()&&f.end==f.v&&f.iend==f.idx+3*type,"Indexed source cursor failed: "+f.error);f.exact(0xAA223311);
      f.reset();unsigned old_iend=f.iend;require(!render_ge_triangle_raster(m,f.r,f.world,f.view,f.projection,f.v,0x30003,f.end,f.error,f.clut,&f.written,f.idx)&&f.iend==old_iend&&f.end==0xDEADBEEF,"Legacy indexed call accepted missing IADDR output");
      GeRuntime ge(f.rt);ge.commands=f.r;ge.world=f.world;ge.view=f.view;ge.projection=f.projection;ge.vertex_address=f.v;ge.index_address=f.idx;
      const unsigned list=0x08809000;m.store32(list,0x04030003);m.store32(list+4,0x0F000000);m.store32(list+8,0x0C000000);
      f.rt.cpu().gpr[4]=list;f.rt.cpu().gpr[6]=0xFFFFFFFF;f.rt.invoke_import("sceGe_user",0xAB49E76Au,f.rt.cpu());
      require(ge.completed.size()==1&&ge.vertex_address==f.v&&ge.index_address==f.idx+3*type&&ge.commands[4]==0x04030003,"Runtime did not commit indexed cursor only on success");f.exact(0xAA223311);
    }
    // A failing indexed list must preserve both cursors, command, framebuffer, completion state.
    Fixture f;auto &m=f.rt.memory();f.v=0x09F80000;f.r[0x12]=0x1180;f.triangle();m.store16(f.idx,0);m.store16(f.idx+2,1);m.store16(f.idx+4,65535);
    require(!f.draw()&&f.end==0xDEADBEEF&&f.iend==0xBAADF00D&&f.written==0xABCD&&f.pixel(0,0)==0xAA112233,"Bad final index published partial raster/cursors");
    GeRuntime ge(f.rt);ge.commands=f.r;ge.commands[4]=0xDEAD1234;ge.world=f.world;ge.view=f.view;ge.projection=f.projection;ge.vertex_address=f.v;ge.index_address=f.idx;
    const unsigned list=0x08809000;m.store32(list,0x04030003);m.store32(list+4,0x0F000000);m.store32(list+8,0x0C000000);
    f.rt.cpu().gpr[4]=list;f.rt.cpu().gpr[6]=0xFFFFFFFF;f.rt.invoke_import("sceGe_user",0xAB49E76Au,f.rt.cpu());
    require(ge.completed.empty()&&ge.vertex_address==f.v&&ge.index_address==f.idx&&ge.commands[4]==0xDEAD1234&&f.pixel(0,0)==0xAA112233,"Failed runtime draw changed authoritative state");
    f.r[0x12]=0x380;f.reset();require(!f.draw()&&f.end==0xDEADBEEF&&f.pixel(0,0)==0xAA112233,"Skinning was silently bypassed");
    f.r[0x12]=0x40180;require(!f.draw()&&f.end==0xDEADBEEF&&f.pixel(0,0)==0xAA112233,"Morphing was silently bypassed");
    std::cout<<"PASS: indexed8/16 raster/runtime ordering and dual cursor commits; late bad index failure atomic; missing IADDR output and transform prerequisites reject\n";
}
int main(){try{format_matrix();texture_uv();through_float();indexed_raster_and_runtime();}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 1;}}
