#include "../host/ge_triangle_raster.hpp"
#include <iostream>
#include <limits>
#include <stdexcept>
static void require(bool r,const std::string &what){if(!r)throw std::runtime_error(what);}
struct Fixture {
    psprecomp::GuestMemory m;chili::GeClutCache clut;std::array<unsigned,256> r{};
    std::array<float,12> world{1,0,0,0,1,0,0,0,1,0,0,0},view=world;
    std::array<float,16> projection{1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    unsigned v=0x08804000,end=0xDEADBEEF,written=0xDEADF00D;std::string error;
    void regf(unsigned i,float value){r[i]=std::bit_cast<unsigned>(value)>>8;}
    void vertex(unsigned i,float x,float y,float z,unsigned color=0xFF0000FF,unsigned u=0,unsigned tv=0){const unsigned a=v+i*20;
        m.store8(a,u);m.store8(a+1,tv);m.store16(a+2,0xDEAD);m.store32(a+4,color);
        m.store32(a+8,std::bit_cast<unsigned>(x));m.store32(a+12,std::bit_cast<unsigned>(y));m.store32(a+16,std::bit_cast<unsigned>(z));}
    void reset(){for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x){m.store32(0x04000000+(y*512+x)*4,0xAA112233);m.store16(0x04080000+(y*512+x)*2,1000);}end=0xDEADBEEF;written=0xDEADF00D;}
    Fixture(){r[0x12]=0x19D;r[0xD2]=3;r[0x9D]=512;r[0x9E]=0x80000;r[0x9F]=512;r[0xD5]=r[0x16]=7|(7<<10);r[0xD7]=65535;
        regf(0x42,2);regf(0x43,2);regf(0x44,100);regf(0x45,2048);regf(0x46,2048);regf(0x47,100);
        r[0x4C]=r[0x4D]=2046*16;r[0x1C]=1;regf(0x48,1);regf(0x49,1);
        vertex(0,-1,-1,0);vertex(1,1,-1,0);vertex(2,-1,1,0);reset();}
    bool draw(unsigned count=3,unsigned primitive=3){return chili::render_ge_triangle_raster(m,r,world,view,projection,v,(primitive<<16)|count,end,error,clut,&written);}
    unsigned pixel(unsigned x,unsigned y){return m.load32(0x04000000+(y*512+x)*4);}
    unsigned depth(unsigned x,unsigned y){return m.load16(0x04080000+(y*512+x)*2);}
    void expected(unsigned color,bool (*inside)(unsigned,unsigned)){for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)
        require(pixel(x,y)==(inside(x,y)?color:0xAA112233),"triangle exact coverage pixel "+std::to_string(x)+","+std::to_string(y));}
};
static bool triangle(unsigned x,unsigned y){return x+y<=3;}
static bool quad(unsigned x,unsigned y){return x<4&&y<4;}
static void coverage_and_pixels(){
    Fixture f;require(f.draw()&&f.end==f.v+60&&f.written==10,"triangle stride/advance failed: "+f.error);f.expected(0xAA0000FF,triangle);
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)require(f.depth(x,y)==1000,"disabled depth test still wrote depth");
    // Two triangles form a square. Their shared diagonal must receive one
    // alpha blend, not two. Both winding directions draw when culling is off.
    f.reset();f.r[0x21]=1;f.r[0xDF]=0x32;
    f.vertex(0,-1,-1,0,0x80FFFFFF);f.vertex(1,1,-1,0,0x80FFFFFF);f.vertex(2,1,1,0,0x80FFFFFF);
    f.vertex(3,-1,-1,0,0x80FFFFFF);f.vertex(4,1,1,0,0x80FFFFFF);f.vertex(5,-1,1,0,0x80FFFFFF);
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)f.m.store32(0x04000000+(y*512+x)*4,0xAA000000);
    require(f.draw(6)&&f.written==16,"shared-edge quad failed: "+f.error);
    for(unsigned y=0;y<8;++y)for(unsigned x=0;x<8;++x)require(f.pixel(x,y)==(quad(x,y)?0xAA808080:0xAA000000),"top-left shared edge blended zero/twice");
    f.r[0x21]=0;f.vertex(0,-1,-1,0,0xFF0000FF);f.vertex(1,1,-1,0,0xFF00FF00);f.vertex(2,-1,1,0,0xFFFF0000);
    f.reset();f.r[0x50]=0;require(f.draw(),"flat triangle failed");f.expected(0xAAFF0000,triangle);
    f.reset();f.r[0x50]=1;require(f.draw(),"Gouraud triangle failed");
    require(f.pixel(0,0)==0xAA1B1BC7 && f.pixel(1,0)==0xAA1B5B87,"Gouraud 7/16 center/color interpolation mismatch");
    f.reset();f.r[0x1D]=1;f.r[0x9B]=0;require(f.draw(),"CW cull failed");
    require(f.pixel(0,0)==0xAA112233&&f.written==0,"CW front-face accepted opposite winding");
    f.r[0x9B]=1;require(f.draw()&&f.pixel(0,0)!=0xAA112233,"CCW front-face rejected winding");
    f.r[0x1D]=0;f.r[0x50]=0;f.vertex(0,-1,-1,0);f.vertex(1,1,-1,0);f.vertex(2,-1,1,0);
    f.reset();f.r[0x23]=1;f.r[0xDE]=4;f.r[0xE7]=0;require(f.draw(),"depth-test/write failed");
    require(f.pixel(0,0)==0xAA0000FF&&f.depth(0,0)==100,"triangle depth comparison/write mismatch");
    f.reset();f.r[0xDE]=6;require(f.draw()&&f.pixel(0,0)==0xAA112233&&f.depth(0,0)==1000,"failing depth test changed buffers");
    f.r[0x23]=0;f.r[0x22]=1;f.r[0xDB]=0xFF0000;require(f.draw()&&f.pixel(0,0)==0xAA112233,"alpha NEVER did not reject");
    f.r[0x22]=0;f.reset();f.r[0xE8]=0xFF;f.r[0xE9]=0;require(f.draw(),"RGB mask fixture failed");
    f.expected(0xAA000033,triangle); // Alpha/stencil stays AA even with PMSKA=0.
    f.r[0xE8]=0;f.r[0x17]=1;f.r[0x53]=1;f.r[0x5C]=0x7F7F7F;f.r[0x5D]=127;f.r[0x54]=0x010203;
    for(unsigned i=0;i<3;++i)f.m.store32(f.v+i*20+4,0x80808080);
    f.reset();require(f.draw(),"ambient-only lighting failed");f.expected(0xAA404142,triangle);
    f.r[0x53]=0;f.r[0x55]=0x202020;f.r[0x58]=255;f.reset();require(f.draw(),"material ambient failed");f.expected(0xAA111213,triangle);
}
static void clip_and_assembly(){
    Fixture f;f.vertex(0,-1,-1,-2);f.vertex(1,1,-1,0);f.vertex(2,-1,1,0);
    require(f.draw(),"near clipping failed: "+f.error);
    f.expected(0xAA0000FF,[](unsigned x,unsigned y){return x+y>=2&&x+y<=3;});
        f.vertex(0,-2000,-1,-2);f.reset();require(f.draw(),"behind-near guardband clipping failed");
    f.expected(0xAA0000FF,triangle);
    f.vertex(0,-1,-1,-2);
    f.r[0x1C]=0;f.reset();require(f.draw()&&f.pixel(2,0)==0xAA112233,"depth clamp off did not discard outside-Z primitive");
    f.r[0x1C]=1;f.vertex(0,-1,-1,0);f.vertex(1,1,-1,2);f.reset();require(f.draw(),"far-Z clamp fixture failed");f.expected(0xAA0000FF,triangle);
    for(unsigned i=0;i<3;++i)f.m.store32(f.v+i*20+16,std::bit_cast<unsigned>(2.0f));f.reset();
    require(f.draw()&&f.pixel(0,0)==0xAA112233,"all-far primitive was not discarded");
    f.vertex(0,-1,-1,0);f.vertex(1,1.5f,-1,0);f.vertex(2,-1,1,0);f.reset();
    require(f.draw()&&f.pixel(4,0)==0xAA0000FF,"X outside NDC incorrectly clipped");
    f.vertex(1,2000,-1,0);f.reset();require(f.draw()&&f.pixel(0,0)==0xAA112233,"guardband outside vertex did not discard primitive");
    f.vertex(0,-1,-1,0);f.vertex(1,1,-1,0);f.vertex(2,-1,1,0);f.vertex(3,1,1,0);f.reset();
    require(f.draw(4,4),"triangle strip failed");f.expected(0xAA0000FF,quad);
    f.vertex(0,-1,-1,0);f.vertex(1,1,-1,0);f.vertex(2,1,1,0);f.vertex(3,-1,1,0);f.reset();
    require(f.draw(4,5),"triangle fan failed");f.expected(0xAA0000FF,quad);
    f.r[0xD4]=1|(1<<10);f.r[0x16]=2|(2<<10);f.reset();require(f.draw(4,5),"scissor/REGION2 failed");
    f.expected(0xAA0000FF,[](unsigned x,unsigned y){return x>=1&&x<=2&&y>=1&&y<=2;});
}
static void texture_and_perspective(){
    Fixture f;f.r[0x1E]=1;f.r[0xC3]=3;f.r[0xC9]=3;f.r[0xC7]=0x101;f.r[0xC6]=0;
    f.r[0xA0]=0x810000;f.r[0xA8]=0x080004;f.r[0xB8]=2; // 4x1 linear RGBA
    const unsigned colors[]={0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFFFFFFFF};
    for(unsigned x=0;x<4;++x)f.m.store32(0x08810000+x*4,colors[x]);
    f.projection={1,0,0,0,0,1,0,0,0,0,0,1,0,0,0,0}; // W = input Z.
    f.vertex(0,-1,-1,1,0xFFFFFFFF,0);f.vertex(1,2,-2,2,0xFFFFFFFF,128);f.vertex(2,-1,1,1,0xFFFFFFFF,0);
    require(f.draw(),"perspective UV triangle failed: "+f.error);
    require(f.pixel(1,0)==0xAA0000FF&&f.pixel(2,0)==0xAA00FF00,"UV interpolation was affine instead of perspective correct");
    f.projection={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};f.r[0xC2]=2u<<16;f.r[0xC6]=0x107;f.r[0xC8]=0x080000;
    for(unsigned level=0;level<3;++level){const unsigned size=8u>>level,stride=std::max(size,4u),address=0x08810000+level*0x10000;
        f.r[0xA0+level]=address&0xFFFFFF;f.r[0xA8+level]=0x080000|stride;f.r[0xB8+level]=(3-level)|((3-level)<<8);
        for(unsigned y=0;y<size;++y)for(unsigned x=0;x<size;++x)f.m.store32(address+(y*stride+x)*4,colors[level]);}
    f.vertex(0,-1,-1,0,0xFFFFFFFF,0,0);f.vertex(1,1,-1,0,0xFFFFFFFF,128,0);f.vertex(2,-1,1,0,0xFFFFFFFF,0,128);
    f.reset();require(f.draw(),"triangle trilinear mip fixture failed: "+f.error);f.expected(0xAA7F7F00,triangle);
    f.r[0xC9]=0;f.r[0xC8]=0;for(unsigned i=0;i<3;++i)f.m.store32(f.v+i*20+4,0xFF808080);
    f.reset();require(f.draw(),"triangle MODULATE fixture failed");f.expected(0xAA008000,triangle);
}
static void atomic_invalid(){
    Fixture f;const auto initial=f.r;auto rejected=[&](const char *what,unsigned count=3){f.reset();
        const auto before=f.m.vram_bytes();
        require(!f.draw(count)&&f.end==0xDEADBEEF&&f.written==0xDEADF00D&&f.m.vram_bytes()==before,
            std::string("invalid triangle changed VRAM/outputs: ")+what);};
    f.r[0x12]|=0x800;rejected("unsupported normal layout");f.r=initial;f.r[0x1F]=1;f.r[0xCE]=0x7FC000;rejected("nonfinite fog");
    f.r=initial;f.r[0x17]=1;f.r[0x18]=1;f.r[0x5F]=3;rejected("unknown individual light");f.r=initial;f.r[0x24]=1;rejected("stencil");
    f.r=initial;f.r[0x21]=1;f.r[0xDF]=0x3AA;rejected("blend equation");f.r=initial;f.r[0x9C]=0x1FFFF0;rejected("wrapped target");
    f.r=initial;f.r[0x23]=1;f.r[0x9F]=0;rejected("depth pitch");f.r=initial;
    f.vertex(3,-1,-1,0);f.vertex(4,1,-1,0);f.vertex(5,-1,1,std::numeric_limits<float>::infinity());rejected("later nonfinite vertex",6);
    f.vertex(5,-1,1,0);f.projection[0]=std::numeric_limits<float>::quiet_NaN();rejected("projection matrix");f.projection[0]=1;
    f.r=initial;f.r[0x1E]=1;f.r[0xC3]=4;f.r[0xC9]=3;f.r[0xA0]=0x810000;f.r[0xA8]=0x080020;f.r[0xB8]=5;
    f.regf(0x48,1);f.regf(0x49,1);f.r[0xC5]=0xFF03;rejected("cold palette");
    f.r[0xC3]=3;f.r[0xA0]=0;f.r[0xA8]=0x040020;rejected("framebuffer texture feedback");
}
int main(){try{coverage_and_pixels();clip_and_assembly();texture_and_perspective();atomic_invalid();std::cout<<"Native visible triangle coverage, clipping, lighting, perspective/mip and atomic fixtures passed\n";}
catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}}
