// Reuse the frozen required-level fixture's deterministic memory/state helpers
// and its explicitly original F60/AC86 reference; the renamed entry is unused.
#define main required_mip_fixture_entry
#include "ge_required_mip_levels_test.cpp"
#undef main

namespace {
unsigned quantum(unsigned format){return format==3?4u:format==4?32u:format==5?16u:8u;}
unsigned bits(unsigned format){return format==3?32u:format==4?4u:format==5?8u:16u;}
unsigned palette_word(unsigned index){return 0xFF000000u|((index*13u)&255u)|(((index*29u+7u)&255u)<<8)|(((index*47u+19u)&255u)<<16);}
unsigned source_word(unsigned format,unsigned x,unsigned y){const unsigned n=x+13u*y;
    if(format==0)return ((n*3u)&31u)|(((n*5u)&63u)<<5)|(((n*7u)&31u)<<11);
    if(format==1)return ((n*3u)&31u)|(((n*5u)&31u)<<5)|(((n*7u)&31u)<<10)|((n&1u)<<15);
    if(format==2)return ((n*3u)&15u)|(((n*5u)&15u)<<4)|(((n*7u)&15u)<<8)|(((n+9u)&15u)<<12);
    if(format==3)return 0xA5000000u|((n*3u)&255u)|(((n*5u+7u)&255u)<<8)|(((n*7u+19u)&255u)<<16);
    return n&(format==4?15u:255u);}
unsigned expected_word(unsigned format,unsigned x,unsigned y){const unsigned raw=source_word(format,x,y);
    const auto five=[](unsigned v){return (v<<3)|(v>>2);};
    if(format==0){const unsigned g=(raw>>5)&63;return 0xFF000000u|five(raw&31)|(((g<<2)|(g>>4))<<8)|(five((raw>>11)&31)<<16);}
    if(format==1)return ((raw&0x8000)?0xFF000000u:0u)|five(raw&31)|(five((raw>>5)&31)<<8)|(five((raw>>10)&31)<<16);
    if(format==2)return (raw&15)*17u|(((raw>>4)&15)*17u<<8)|(((raw>>8)&15)*17u<<16)|(((raw>>12)&15)*17u<<24);
    return format==3?raw:palette_word(raw);}
// Write storage in physical128-byte block order, without using the sampler's
// logical-texel address equation. Distinct texels expose row/tile mistakes.
void write_storage(psprecomp::GuestMemory &m,unsigned address,unsigned format,unsigned pitch,unsigned height,bool swizzle){
    unsigned cursor=0;const unsigned rows=swizzle?((height+7u)&~7u):height;
    for(unsigned by=0;by<(rows+7u)/8u;++by)for(unsigned bx=0;bx<pitch/16u;++bx)
        for(unsigned ly=0;ly<8u && by*8u+ly<rows;++ly)for(unsigned b=0;b<16;++b){
            const unsigned y=by*8u+ly,byte_x=bx*16u+b;
            unsigned packed{};
            if(format==4)packed=source_word(format,byte_x*2u,y)|(source_word(format,byte_x*2u+1u,y)<<4);
            else if(format==5)packed=source_word(format,byte_x,y);
            else{const unsigned bytes=bits(format)/8u;packed=(source_word(format,byte_x/bytes,y)>>((byte_x%bytes)*8u))&255u;}
            m.store8(address+(swizzle?cursor:y*pitch+byte_x),packed);++cursor;
        }
}
void prepare_palette(psprecomp::GuestMemory &m,chili::GeClutCache &cache,std::string &error){
    for(unsigned n=0;n<256;++n)m.store32(palette_address+n*4,palette_word(n));
    require(cache.load(m,palette_address&0xFFFFFF,(palette_address>>8)&0xF0000,32,error),error);
}
void unique_layouts(){constexpr unsigned address=0x08830000;std::string error;
    for(unsigned format=0;format<6;++format)for(bool swizzle:{false,true}){psprecomp::Runtime rt;auto &m=rt.memory();chili::GeClutCache cache;prepare_palette(m,cache,error);
        const unsigned q=quantum(format);
        for(unsigned raw:{0u,1u,q-1u,q,q+1u,q*2u-1u,q*2u,q*2u+1u,2047u}){
            Regs r{};r[0xC3]=format;r[0xC2]=swizzle?1u:0u;r[0xC5]=0xFF03;
            const unsigned effective=std::max(q,raw&~(q-1u)),width=raw>=q*2u?q*2u:q,height=16,pitch=effective*bits(format)/8;
            r[0xA0]=address&0xFFFFFF;r[0xA8]=((address>>8)&0xF0000)|raw;r[0xB8]=unsigned(std::countr_zero(width))|(4u<<8);
            write_storage(m,address,format,pitch,height,swizzle);const auto registers=r;const MemoryState before(m);const auto cpu=rt.cpu();
            chili::GeMipTexture sampler;require(sampler.configure(m,r,format<4?nullptr:&cache,error),error);
            require(r==registers&&sampler.level(0).stride==effective&&sampler.level(0).byte_pitch==pitch&&sampler.level(0).storage_bytes==pitch*height,"effective layout/register preservation mismatch");
            chili::GeMipSelection selection;require(sampler.select(1,1,selection,error)&&!selection.linear&&selection.lower==0,"nearest selector changed");
            for(unsigned y=0;y<height;++y)for(unsigned x=0;x<width;++x){unsigned out=0xDEADBEEF;
                require(sampler.sample(m,(float(x)+.5f)/float(width),(float(y)+.5f)/float(height),selection,out,error),error);
                require(out==expected_word(format,x,y),"unique texel mismatch format="+std::to_string(format)+" raw="+std::to_string(raw)+" swizzle="+std::to_string(swizzle)+" xy="+std::to_string(x)+","+std::to_string(y));}
            require(before.same(m)&&same_cpu(rt.cpu(),cpu),"pure configure/sample changed guest state");
            chili_original_mip::GeMipTexture old;std::string old_error;const bool old_ok=old.configure(m,r,format<4?nullptr:&cache,old_error);
            if(raw==effective){require(old_ok,"historically valid aligned pitch rejected by original");chili_original_mip::GeMipSelection old_pick;require(old.select(1,1,old_pick,old_error),old_error);
                unsigned a{},b{};require(old.sample(m,.3125f,.6875f,old_pick,a,old_error)&&sampler.sample(m,.3125f,.6875f,selection,b,error)&&a==b,"old valid aligned layout changed");}
            if(raw==0)require(!old_ok&&old_error=="invalid mip texture stride","historical F60 zero rejection lost");
            r[0xA8]=((address>>8)&0xF0000);r[0xB8]=unsigned(std::countr_zero(q*2u))|(4u<<8);
            const unsigned prepared=sampler.prepared_levels();require(!sampler.configure(m,r,format<4?nullptr:&cache,error)&&error=="invalid mip texture stride"&&sampler.prepared_levels()==prepared,"effective narrow width accepted or old view lost");
        }
    }
    std::cout<<"PASS all6 uncompressed formats/zero+lowbits/max11bit linear+swizzle unique physical layouts; historicalF60 rejection separate\n";
}
void visible_setup(Synthetic &f){auto &r=f.regs;auto &m=f.rt.memory();prepare_palette(m,f.clut,f.error);
    r[0xC2]=(5u<<16)|1u;r[0xC3]=4;r[0xC5]=0xFF03;r[0xC6]=0x107;r[0xC8]=0xE80000;r[0xC9]=0x103;
    r[0xD5]=r[0x16]=511u|(319u<<10);
    const unsigned widths[]={16,8,4,2,1,4},heights[]={256,128,64,32,16,8};
    for(unsigned l=0;l<6;++l){const unsigned a=0x08820000+l*0x10000;r[0xA0+l]=a&0xFFFFFF;r[0xA8+l]=0x80000|(l==5?0u:32u);r[0xB8+l]=unsigned(std::countr_zero(widths[l]))|(unsigned(std::countr_zero(heights[l]))<<8);
        write_storage(m,a,4,16,heights[l],true);}
    f.vertex_at(0,0,1459,448,245);f.vertex_at(1,0,1715,448,249);f.vertex_at(2,16,1715,460,249);f.vertex_at(3,16,1459,460,245);
}
void synthetic_visible_native(){Synthetic old,f;visible_setup(old);visible_setup(f);old.regs[0xAD]|=32;
    const MemoryState before(f.rt.memory());const auto cpu=f.rt.cpu();
    require(chili_required_baseline::render_ge_triangle_raster(old.rt.memory(),old.regs,old.world,old.view,old.projection,vertex,0x50004,old.cursor,old.error,old.clut,&old.written,0,&old.index),old.error);
    require(chili::render_ge_triangle_raster(f.rt.memory(),f.regs,f.world,f.view,f.projection,vertex,0x50004,f.cursor,f.error,f.clut,&f.written,0,&f.index),f.error);
    require(f.written==48&&old.written==48&&f.cursor==vertex+64&&old.cursor==f.cursor&&f.index==0&&old.index==0,"visible strip count/cursors changed");
    require(f.rt.memory().vram_bytes()==old.rt.memory().vram_bytes()&&before.source_same(f.rt.memory())&&same_cpu(f.rt.cpu(),cpu),"visible whole2MiB/state differs from originalF60 explicitstride32 oracle");
    const Bytes expected=old.rt.memory().vram_bytes();
    for(bool enabled:{false,true}){Synthetic run;visible_setup(run);chili::ThreadRuntime scheduler(run.rt);std::ostringstream output;chili::PerformanceTelemetry telemetry(output);
        chili::GeRuntime ge(run.rt,&scheduler,0,nullptr,enabled?&telemetry:nullptr);ge.commands=run.regs;ge.clut=run.clut;ge.world=run.world;ge.view=run.view;ge.projection=run.projection;ge.vertex_address=vertex;
        constexpr unsigned list=0x09F00000;run.rt.memory().store32(list,0x04050004);run.rt.memory().store32(list+4,0x0F000000);run.rt.memory().store32(list+8,0x0C000000);
        run.rt.cpu().gpr[4]=list;run.rt.cpu().gpr[5]=0;run.rt.cpu().gpr[6]=0xFFFFFFFF;run.rt.cpu().gpr[7]=0;const MemoryState source(run.rt.memory());auto wanted=run.rt.cpu();wanted.gpr[2]=1;
        const auto uid=psprecomp::runtime_thread_uid();const auto generation=psprecomp::runtime_thread_switch_generation();const auto time=scheduler.now_us();
        run.rt.invoke_import("sceGe_user",0xAB49E76A,run.rt.cpu());auto commands=run.regs;commands[4]=0x04050004;commands[0x0F]=0x0F000000;
        require(!run.rt.stopped()&&ge.completed.contains(1)&&ge.vertex_address==vertex+64&&ge.index_address==0&&ge.commands==commands&&ge.world==run.world&&ge.view==run.view&&ge.projection==run.projection,"visible nativeHLE completion/GE state mismatch");
        require(run.rt.memory().vram_bytes()==expected&&source.source_same(run.rt.memory())&&same_cpu(run.rt.cpu(),wanted)&&scheduler.now_us()==time&&psprecomp::runtime_thread_uid()==uid&&psprecomp::runtime_thread_switch_generation()==generation,"visible nativeHLE fullmemory/CPU/thread/time mismatch");
        chili::PerformanceTelemetry::Sample sample;require(telemetry.sample(chili::PerformanceTelemetry::Clock::now(),time,sample,true),"telemetry sample failed");
        if(enabled)require(sample.total[chili::PerformanceTelemetry::DrawSuccesses]==1&&sample.total[chili::PerformanceTelemetry::TriangleCommittedFragments]==48,"nativeHLE counters changed");else for(auto counter:sample.total)require(!counter,"default null counters changed");
    }
    for(unsigned kind=0;kind<4;++kind){Synthetic run;visible_setup(run);
        if(kind==0){run.regs[0xA5]=0xFFFFC0;run.regs[0xAD]=0x90000;}
        if(kind==1)run.regs[0xBD]=0x306; //64wide exceeds effective32
        if(kind==2)run.clut=chili::GeClutCache{};
        if(kind==3){run.regs[0xA5]=0x7AF00;run.regs[0xAD]=0x40000;}
        const MemoryState original(run.rt.memory());const auto original_cpu=run.rt.cpu();
        require(!chili::render_ge_triangle_raster(run.rt.memory(),run.regs,run.world,run.view,run.projection,vertex,0x50004,run.cursor,run.error,run.clut,&run.written,0,&run.index),"selected storage/width/cache/feedback failure accepted kind="+std::to_string(kind));
        require(original.same(run.rt.memory())&&same_cpu(run.rt.cpu(),original_cpu)&&run.cursor==0xFEEDCAFE&&run.index==0xBAADF00D&&run.written==0xDEADF00D,"selected failure not atomic");
    }
    std::cout<<"PASS synthetic visible48pixel L4/L5half originalF60 explicitpitch32 fullVRAM oracle/nativeHLE null+enabled/selected atomic failures\n";
}
void synthetic_mip_sprite(){Synthetic reference;visible_setup(reference);reference.regs[0xAD]|=32;
    chili_original_mip::GeMipTexture old;chili_original_mip::GeMipSelection selection;std::string error;
    require(old.configure(reference.rt.memory(),reference.regs,&reference.clut,error)&&old.select(64,1,selection,error),error);
    require(selection.lower==4&&selection.upper==5&&selection.fraction16==8&&selection.linear,"sprite independent reference selection wrong");
    Bytes expected=reference.rt.memory().vram_bytes();
    for(unsigned y=245;y<249;++y)for(unsigned x=448;x<460;++x){
        const float u=float((double(x)+.5-448)*16.0/12.0)/16.0f;
        const float v=float(1459+(double(y)+.5-245)*64.0)/256.0f;unsigned color{};
        require(old.sample(reference.rt.memory(),u,v,selection,color,error),error);
        const unsigned at=(y*512+x)*4;expected[at]=std::uint8_t(color);expected[at+1]=std::uint8_t(color>>8);expected[at+2]=std::uint8_t(color>>16);
        // Sprite stencil/alpha retention is existing caller behavior.
    }
    for(bool enabled:{false,true}){Synthetic run;visible_setup(run);run.vertex_at(1,16,1715,460,249);
        const MemoryState before(run.rt.memory());const auto cpu=run.rt.cpu();std::ostringstream output;chili::PerformanceTelemetry telemetry(output);
        require(chili::render_ge_sprites(run.rt.memory(),run.regs,vertex,0x60002,run.cursor,run.error,&run.clut,enabled?&telemetry:nullptr),run.error);
        require(run.cursor==vertex+32&&run.rt.memory().vram_bytes()==expected&&before.source_same(run.rt.memory())&&same_cpu(run.rt.cpu(),cpu),"mip sprite raw0 fullVRAM/state/cursor differs from originalF60 sample oracle");
    }
    for(bool enabled:{false,true}){Synthetic run;visible_setup(run);run.vertex_at(0,0,0,0,0);run.vertex_at(1,16,256,16,256);run.vertex_at(2,0,1459,448,245);run.vertex_at(3,16,1715,460,249);
        // First rectangle genuinely usesL0; second needsL4/L5. SelectedL5's
        // effective128-byte span extends past valid RAM, despite raw pitch0.
        run.regs[0xA5]=0xFFFFC0;run.regs[0xAD]=0x90000;
        const MemoryState before(run.rt.memory());const auto cpu=run.rt.cpu();std::ostringstream output;chili::PerformanceTelemetry telemetry(output);
        require(!chili::render_ge_sprites(run.rt.memory(),run.regs,vertex,0x60004,run.cursor,run.error,&run.clut,enabled?&telemetry:nullptr),"late mip sprite selected span accepted");
        require(before.same(run.rt.memory())&&same_cpu(run.rt.cpu(),cpu)&&run.cursor==0xFEEDCAFE,"late mip sprite failure published pixels/cursor/state");
    }
    std::cout<<"PASS visible mip sprite L4/L5half originalF60 per-pixel sample oracle/full2MiB/null+enabled and late-selected-upper atomicity\n";
}
void captured_descriptors(const std::filesystem::path &prefix){Regs r{};std::ifstream input(prefix.string()+".txt");require(bool(input),"capture metadata missing");std::string line;
    while(std::getline(input,line)){std::istringstream words(line);std::string tag;words>>tag;if(tag=="reg"){unsigned i{},v{};words>>std::hex>>i>>v;require(bool(words)&&i<256,"register parse failed");r[i]=v;}}
    require((r[0xC2]&0xFFFFFF)==0x50001&&(r[0xC3]&0xFFFFFF)==4&&(r[0xAD]&0x7FF)==0,"Run10 descriptor identity changed");
    psprecomp::Runtime rt;chili::GeClutCache cache;std::string error;const Bytes captured_palette=read(prefix.string()+"_palette_source.bin",64);
    rt.memory().copy_in(0x0924EF20,captured_palette);require(cache.load(rt.memory(),r[0xB0],r[0xB1],r[0xC4],error),error);
    chili::GeMipTexture sampler;require(sampler.configure(rt.memory(),r,&cache,error,0),error);chili::GeMipSelection pick;
    require(sampler.select(64,1,pick,error)&&pick.lower==4&&pick.upper==5&&pick.fraction16==8&&pick.detail16==72&&pick.linear,"captured selector differs from independent numeric proof");
    require(sampler.configure(rt.memory(),r,&cache,error,0x30),error);
    require(sampler.level(4).address==0x0924EE20&&sampler.level(4).stride==32&&sampler.level(4).storage_bytes==256&&sampler.level(5).address==0x0924EF20&&sampler.level(5).stride==32&&sampler.level(5).byte_pitch==16&&sampler.level(5).storage_bytes==128,"captured effective descriptor mismatch");
    // Do not sample or render: captured L4 and L5 tail bytes are absent.
    std::cout<<"PASS captured selection/effective descriptors only; no fabricated missingL4/L5 bytes or actual pixel claim\n";
}
}
int main(int argc,char **argv){try{require(argc==2,"Usage: ge_effective_texture_pitch_test actual-prefix");unique_layouts();synthetic_visible_native();synthetic_mip_sprite();captured_descriptors(argv[1]);std::cout<<"PASS "<<checks<<" effective pitch checks; reference-supported addressing, not new PSP hardware goldens\n";return 0;}catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
