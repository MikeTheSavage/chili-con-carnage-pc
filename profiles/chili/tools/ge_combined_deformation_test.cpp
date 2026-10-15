#include "ge_combined_fixture.hpp"
#include <cstring>
#include <fstream>
#include <sstream>

static bool cpu_equal(const psprecomp::AllegrexContext &a,const psprecomp::AllegrexContext &b){
    return a.gpr==b.gpr&&a.pc==b.pc&&a.hi==b.hi&&a.lo==b.lo&&a.fcr31==b.fcr31&&a.vfpu_ctrl==b.vfpu_ctrl&&
        std::memcmp(a.fpr.data(),b.fpr.data(),a.fpr.size()*sizeof(float))==0&&
        std::memcmp(a.vfpu.data(),b.vfpu.data(),a.vfpu.size()*sizeof(float))==0;
}
static void seed_cpu(Fixture &f){auto &c=f.rt.cpu();
    for(unsigned n=1;n<c.gpr.size();++n)c.gpr[n]=0x90000000u+n;
    c.gpr[28]=0x08D9ABCD;c.gpr[29]=0x09FE8810;c.gpr[31]=0x08844364;c.pc=0x08D98A58;
    c.hi=0x01234567;c.lo=0x89ABCDEF;c.fcr31=0x13579BDF;
    for(unsigned n=0;n<c.fpr.size();++n)c.fpr[n]=std::bit_cast<float>(0x7FC10000u+n);
    for(unsigned n=0;n<c.vfpu.size();++n)c.vfpu[n]=std::bit_cast<float>(0xFFC10000u+n);
    for(unsigned n=0;n<c.vfpu_ctrl.size();++n)c.vfpu_ctrl[n]=0xE4000000u+n;
}
static ge::Draw decoded(Fixture &f,unsigned count=3){
    return ge::prepare_draw([&](unsigned a,std::span<std::uint8_t> b){if(!f.rt.memory().contains(a,b.size()))return false;f.rt.memory().copy_out(a,b);return true;},f.r[0x12]&0xFFFFFF,f.v,f.idx,count);
}
static void exact_attributes(const ge::DeformedDraw &a,const ge::DeformedDraw &b){
    require(a.vertices.size()==b.vertices.size(),"deformation vertex count changed");
    for(unsigned n=0;n<a.vertices.size();++n){const auto &x=a.vertices[n],&y=b.vertices[n];
        require(x.source_index==y.source_index,"deformation lost original source index");
        require(x.attributes.position==y.attributes.position&&x.attributes.normal==y.attributes.normal&&x.attributes.uv==y.attributes.uv&&x.attributes.color==y.attributes.color&&x.attributes.weights==y.attributes.weights,
            "canonical combined differs from independent single-record skin attributes");}
}
static void stream(Fixture &f,unsigned count,unsigned primitive);
static void canonical_equivalence(){
    for(unsigned morphs:{2u,8u})for(unsigned type:{1u,2u,3u})for(unsigned weights:{1u,4u,8u})for(unsigned indices:{0u,1u,2u}){
        Fixture combined,reference;combined.source(morphs,type,weights,indices);reference.source(1,type,weights,indices);
        combined.deformation.bones[weights-1][9]=reference.deformation.bones[weights-1][9]=0.5f*weights;
        // Inactive records deliberately disagree in geometry and bone weights.
        // They must not contaminate the canonical base, yet remain validated.
        for(unsigned v=0;v<3;++v)for(unsigned m=1;m<morphs;++m){ge::Sample s;
            s.position={20.f+v,30.f+m,40.f};s.normal=ge::Vec3{4,5,6};s.uv=ge::Vec2{1,1};s.color=ge::Color{0,255,0,255};
            for(unsigned b=0;b<weights;++b)s.weights[b]=1.5f;combined.encode(v,m,s);}
        seed_cpu(combined);const auto cpu=combined.rt.cpu();const auto ram=combined.rt.memory().bytes();
        const auto uid=psprecomp::runtime_thread_uid();const auto generation=psprecomp::runtime_thread_switch_generation();
        exact_attributes(ge::prepare_deformed_draw(decoded(combined),combined.deformation),ge::prepare_deformed_draw(decoded(reference),reference.deformation));
        require(reference.draw(&reference.deformation),"single-record skin reference failed: "+reference.error);
        require(combined.draw(&combined.deformation)&&combined.written==10&&combined.written==reference.written,"canonical combined native coverage failed: "+combined.error);
        require(combined.vram()==reference.vram(),"combined full color/depth VRAM differs from skin-only reference");
        const auto l=ge::make_layout(combined.r[0x12]);
        require(combined.end==(indices?combined.v:combined.v+3*l.vertex_stride)&&combined.iend==(indices?combined.idx+3*indices:combined.idx),"combined original storage/index cursors lost");
        require(combined.rt.memory().bytes()==ram&&cpu_equal(combined.rt.cpu(),cpu)&&psprecomp::runtime_thread_uid()==uid&&psprecomp::runtime_thread_switch_generation()==generation,"combined changed RAM/full NaN CPU/UID/generation");
    }
    std::cout<<"PASS54 canonical combined variants: morph2/8, weight U8/U16/float, bone1/4/8, index0/8/16; exact skin-only attributes/pixels/cursors/context\n";
}
static void linear_weight_cases(){
    for(unsigned morphs:{2u,8u})for(unsigned type:{1u,2u,3u})for(unsigned indices:{0u,1u,2u}){
        Fixture f,reference;f.source(morphs,type,2,indices);reference.source(1,0,0,indices);
        const float mean=float(morphs-1)*0.5f,first=1.f/morphs,second=1-first,scale=1-0.5f*first;
        for(unsigned m=0;m<morphs;++m)f.deformation.morph_weights[m]=first;
        f.deformation.bones[0]={0.5f,0,0,0,0.5f,0,0,0,0.5f,0.25f,0,0};
        f.deformation.bones[1]={1,0,0,0,1,0,0,0,1,0,0.25f,0};
        for(unsigned v=0;v<3;++v){const float x=v==1?1.f:-1.f,y=v==2?1.f:-1.f;
            for(unsigned m=0;m<morphs;++m){ge::Sample a;a.position={x+0.25f*m,y,0};a.normal=ge::Vec3{1.f+m/8.f,2,3};a.uv=ge::Vec2{m/8.f,0};a.color=ge::Color{std::uint8_t(16+16*m),32,48,255};a.weights[m?1:0]=1;f.encode(v,m,a);}
            // Independent exact dyadic closed form: mean target displacement,
            // then matrix from [1/M,(M-1)/M]. Per-target skin-then-morph and
            // first-record-only weights give different answers.
            ge::Sample expected;expected.position={(x+0.25f*mean)*scale+0.25f*first,y*scale+0.25f*second,0};
            expected.normal=ge::Vec3{(1.f+mean/8.f)*scale,2*scale,3*scale};expected.uv=ge::Vec2{mean/8.f,0};expected.color=ge::Color{std::uint8_t(16+16*mean),32,48,255};reference.encode(v,0,expected);
        }
        const auto source=decoded(f),plain=decoded(reference);const auto output=ge::prepare_deformed_draw(source,f.deformation);
        for(unsigned n=0;n<3;++n){const auto &a=output.vertices[n].attributes,&b=plain.vertices[n].samples[0];
            require(a.position==b.position&&a.normal==b.normal&&a.uv==b.uv&&a.color==b.color,"independent morph-before-skin arithmetic differs");
            require(a.weights[0]==first&&a.weights[1]==second&&a.weights[2]==0,"declared differing target weights not morphed linearly");}
        require(reference.draw(nullptr)&&f.draw(&f.deformation)&&f.written>0&&f.written==reference.written&&f.vram()==reference.vram(),"combined differing-weight raster differs from unweighted closed-form reference: "+f.error);
        const auto l=source.layout;require(f.end==(indices?f.v:f.v+3*l.vertex_stride)&&f.iend==(indices?f.idx+3*indices:f.idx),"weighted blend cursor changed");
        // Exercise streamed animation state as well, without synthetic pixels.
        f.reset();stream(f,3,3);
    }
    // Arithmetic contract, not a PSP negative/nonunit hardware oracle: preserve
    // the declared sum, including non-unit float weights and signed morph weights.
    Fixture f;f.source(2,3,2);f.deformation.morph_weights[0]=1.5f;f.deformation.morph_weights[1]=-0.5f;
    for(unsigned v=0;v<3;++v)for(unsigned m=0;m<2;++m){ge::Sample a;a.position={1,2,3};a.normal=ge::Vec3{1,0,0};a.uv=ge::Vec2{0,0};a.color=ge::Color{16,32,48,255};a.weights[0]=m?0.5f:2;a.weights[1]=m?1:0;f.encode(v,m,a);}
    const auto out=ge::prepare_deformed_draw(decoded(f),f.deformation);
    require(out.vertices[0].attributes.weights[0]==2.75f&&out.vertices[0].attributes.weights[1]==-0.5f&&out.vertices[0].attributes.position==ge::Vec3{2.25f,4.5f,6.75f},"extra normalization/clamping altered linear bone weight sum");
    std::cout<<"PASS18 differing-weight morph2/8 blends: independent exact dyadic position/normal/UV/color/weights, native raster and streamed animation; no-normalization arithmetic contract\n";
}
static void independent_vectors(){
    for(unsigned nonunit:{0u,1u}){Fixture f;f.source(2,3,2);f.deformation.morph_weights[0]=0.25f;f.deformation.morph_weights[1]=0.75f;
        f.deformation.bones[0]={1,0,0,0,1,0,0,0,1,8,0,0};f.deformation.bones[1]={2,0,0,0,2,0,0,0,2,0,12,-4};
        for(unsigned v=0;v<3;++v)for(unsigned m=0;m<2;++m){ge::Sample a;a.position=m?ge::Vec3{5,6,7}:ge::Vec3{1,2,3};a.normal=m?ge::Vec3{1,0,0}:ge::Vec3{0,1,0};a.uv=m?ge::Vec2{1,0}:ge::Vec2{0,0.5f};a.color=ge::Color{16,32,48,255};a.weights[m]=nonunit?(m?0.75f:0.5f):1;f.encode(v,m,a);}
        const auto d=ge::prepare_deformed_draw(decoded(f),f.deformation);const auto &a=d.vertices[0].attributes;
        require(a.position==(nonunit?ge::Vec3{6,13,5.25f}:ge::Vec3{9,17.75f,7.5f}),"independent Frame scalar position vector mismatch");
        require(a.normal==(nonunit?ge::Vec3{0.9375f,0.3125f,0}:ge::Vec3{1.3125f,0.4375f,0})&&a.uv==ge::Vec2{0.75f,0.125f},"independent Frame scalar normal/UV mismatch");
        require(a.weights[0]==(nonunit?0.125f:0.25f)&&a.weights[1]==(nonunit?0.5625f:0.75f),"independent Frame scalar weights mismatch");
    }
    std::cout<<"PASS Independent Frame-worker differing/nonunit weight vectors reject first-record-only and skin-before-morph formulas\n";
}
static void failures(){
    auto failure=[](Fixture &f,const char *why){const auto before=f.vram(),ram=f.rt.memory().bytes();seed_cpu(f);auto cpu=f.rt.cpu();
        require(!f.draw(&f.deformation),std::string(why)+" unexpectedly accepted");
        require(f.vram()==before&&f.rt.memory().bytes()==ram&&cpu_equal(f.rt.cpu(),cpu)&&f.end==0xDEADBEEF&&f.iend==0xBAADF00D&&f.written==0xABCD,std::string(why)+" published partial output");
        ge::DeformedDraw sentinel;sentinel.next_vertex_address=0xABCD;std::string error;
        require(!ge::try_prepare_deformed_draw(decoded(f),f.deformation,sentinel,error)&&sentinel.next_vertex_address==0xABCD,std::string(why)+" assigned prepared result on error");};
    for(unsigned which=2;which<7;++which){Fixture f;f.source(8,3,4,2);
        if(which==2)f.deformation.morph_weights[1]=std::numeric_limits<float>::quiet_NaN();
        if(which==3)f.deformation.morph_weights[0]=std::numeric_limits<float>::infinity();
        if(which==4)f.deformation.bones[3][7]=std::numeric_limits<float>::quiet_NaN();
        if(which==5){auto l=ge::make_layout(f.r[0x12]);f.rt.memory().store32(f.v+2*l.vertex_stride+7*l.sample_stride+l.weight_offset,std::bit_cast<unsigned>(std::numeric_limits<float>::infinity()));}
        if(which==6){auto l=ge::make_layout(f.r[0x12]);f.deformation.morph_weights[0]=2;f.rt.memory().store32(f.v+l.weight_offset,std::bit_cast<unsigned>(std::numeric_limits<float>::max()));}
        if(which==5){const auto before=f.vram(),ram=f.rt.memory().bytes();require(!f.draw(&f.deformation)&&f.vram()==before&&f.rt.memory().bytes()==ram&&f.end==0xDEADBEEF&&f.written==0xABCD,"late inactive nonfinite source failed atomicity");}
        else failure(f,"malformed combined");
    }
    // Strict through+morph and morph565 boundaries remain in existing regressions.
    Fixture f;f.source(8,1,4,2);GeRuntime gpu(f.rt);gpu.commands=f.r;gpu.commands[4]=0x04123456;
    gpu.world=f.world;gpu.view=f.view;gpu.projection=f.projection;gpu.vertex_address=f.v;gpu.index_address=f.idx;
    for(unsigned b=0;b<8;++b)std::copy(f.deformation.bones[b].begin(),f.deformation.bones[b].end(),gpu.bones.begin()+b*12);
    gpu.morph=f.deformation.morph_weights;gpu.morph[7]=std::numeric_limits<float>::quiet_NaN();
    const unsigned list=0x08809000;f.rt.memory().store32(list,0x04030003);f.rt.memory().store32(list+4,0x0F000000);f.rt.memory().store32(list+8,0x0C000000);
    const auto before=f.vram(),ram=f.rt.memory().bytes();f.rt.cpu().gpr[4]=list;f.rt.cpu().gpr[6]=0xFFFFFFFF;f.rt.invoke_import("sceGe_user",0xAB49E76Au,f.rt.cpu());
    require(f.rt.stopped()&&gpu.completed.empty()&&gpu.commands[4]==0x04123456&&gpu.vertex_address==f.v&&gpu.index_address==f.idx&&f.vram()==before&&f.rt.memory().bytes()==ram,"runtime nonfinite stop published PRIM/cursor/completion/output");
    std::cout<<"PASS NaN/inactive malformed records remain strict and whole-draw atomic; runtime failure commits no PRIM/cursors/completion\n";
}
static void stream(Fixture &f,unsigned count,unsigned primitive){
    ThreadRuntime scheduler(f.rt);GeRuntime gpu(f.rt,&scheduler);gpu.commands=f.r;gpu.clut=f.clut;
    const unsigned list=0x09F00000;unsigned cursor=list;
    auto emit=[&](unsigned c,unsigned d){f.rt.memory().store32(cursor,(c<<24)|(d&0xFFFFFF));cursor+=4;};
    auto emitf=[&](unsigned c,float value){emit(c,std::bit_cast<unsigned>(value)>>8);};
    emit(0x2A,0);for(const auto &b:f.deformation.bones)for(float value:b)emitf(0x2B,value);
    for(unsigned n=0;n<8;++n)emitf(0x2C+n,f.deformation.morph_weights[n]);
    emit(0x3A,0);for(float value:f.world)emitf(0x3B,value);
    emit(0x3C,0);for(float value:f.view)emitf(0x3D,value);
    emit(0x3E,0);for(float value:f.projection)emitf(0x3F,value);
    emit(0x12,f.r[0x12]);emit(0x10,0x080000);emit(1,f.v&0xFFFFFF);if(f.idx)emit(2,f.idx&0xFFFFFF);
    emit(4,(primitive<<16)|count);emit(0x0F,0xBEEF);emit(0x0C,0);
    const auto before=f.vram();require(f.draw(&f.deformation,count,primitive),"expected direct combined draw before stream failed: "+f.error);const auto expected=f.vram();
    f.rt.memory().copy_in(0x04000000,before);seed_cpu(f);f.rt.cpu().gpr[4]=list;f.rt.cpu().gpr[5]=0;f.rt.cpu().gpr[6]=0xFFFFFFFF;f.rt.cpu().gpr[7]=0;
    auto cpu=f.rt.cpu();cpu.gpr[2]=1;const auto ram=f.rt.memory().bytes();const auto uid=psprecomp::runtime_thread_uid();const auto generation=psprecomp::runtime_thread_switch_generation();const auto time=scheduler.now_us();
    f.rt.invoke_import("sceGe_user",0xAB49E76Au,f.rt.cpu());const auto l=ge::make_layout(f.r[0x12]&0xFFFFFF);
    require(!f.rt.stopped()&&gpu.completed.size()==1&&gpu.completed.contains(1)&&gpu.commands[4]==((4u<<24)|(primitive<<16)|count),"streamed canonical PRIM did not genuinely complete: "+f.rt.stop_reason());
    require(gpu.vertex_address==(l.index_bytes?f.v:f.v+count*l.vertex_stride)&&gpu.index_address==(l.index_bytes?f.idx+count*l.index_bytes:f.idx),"runtime original cursor advancement incorrect");
    require(gpu.bone_cursor==96&&gpu.morph==f.deformation.morph_weights,"runtime GE bone/morph stream not preserved");
    require(f.vram()==expected&&f.rt.memory().bytes()==ram&&cpu_equal(f.rt.cpu(),cpu)&&scheduler.now_us()==time&&psprecomp::runtime_thread_uid()==uid&&psprecomp::runtime_thread_switch_generation()==generation,"streamed raster differs or changes memory/CPU/UID/time/generation");
}
static std::vector<std::uint8_t> read(const std::filesystem::path &path,std::size_t size){std::ifstream file(path,std::ios::binary);require(bool(file),"capture file absent");file.seekg(0,std::ios::end);require(file.tellg()==static_cast<std::streamoff>(size),"capture byte size mismatch");file.seekg(0);std::vector<std::uint8_t> b(size);file.read(reinterpret_cast<char*>(b.data()),static_cast<std::streamsize>(b.size()));require(bool(file),"capture read failed");return b;}
static void captured(const std::filesystem::path &prefix){Fixture actual,reference;std::ifstream file(prefix.string()+".txt");require(bool(file),"capture metadata absent");unsigned data{};std::string line;
    while(std::getline(file,line)){std::istringstream s(line);std::string kind;s>>kind;
        auto parse=[&](auto &values){for(auto &v:values){s>>v;require(bool(s)&&std::isfinite(v),"invalid capture matrix");}};
        if(kind=="reg"){unsigned n{},v{};s>>std::hex>>n>>v;require(n<256&&bool(s),"invalid captured register");actual.r[n]=v;}
        else if(kind=="world")parse(actual.world);else if(kind=="view")parse(actual.view);else if(kind=="projection")parse(actual.projection);
        else if(kind=="morph")parse(actual.deformation.morph_weights);
        else if(kind=="bones"){for(auto &b:actual.deformation.bones)parse(b);}
        else if(kind.starts_with("primitive=")){data=unsigned(std::stoul(kind.substr(10),nullptr,16));std::string address;s>>address;require(address.starts_with("vertices="),"captured VADDR absent");actual.v=unsigned(std::stoul(address.substr(9),nullptr,16));}}
    require(data==0x40004&&actual.v==0x08492088&&(actual.r[0x12]&0xFFFFFF)==0x1CC3A1,"actual combined capture shape changed");actual.idx=0;
    const auto raw=read(prefix.string()+"_vertices.bin",768),vram=read(prefix.string()+"_vram.bin",psprecomp::GuestMemory::kVramSize);
    const auto texture=(actual.r[0xA0]&0xFFFFF0)|((actual.r[0xA8]<<8)&0x0F000000),palette=(actual.r[0xB0]&0xFFFFF0)|((actual.r[0xB1]<<8)&0x0F000000);
    actual.rt.memory().copy_in(actual.v,raw);actual.rt.memory().copy_in(0x04000000,vram);actual.rt.memory().copy_in(texture,read(prefix.string()+"_texture.bin",16384));actual.rt.memory().copy_in(palette,read(prefix.string()+"_palette_source.bin",1024));
    require(actual.clut.load(actual.rt.memory(),actual.r[0xB0],actual.r[0xB1],actual.r[0xC4],actual.error),"capture CLUT load failed");
    reference.r=actual.r;reference.r[0x12]&=~(7u<<18);reference.v=actual.v;reference.idx=0;reference.world=actual.world;reference.view=actual.view;reference.projection=actual.projection;reference.deformation=actual.deformation;reference.clut=actual.clut;
    reference.rt.memory().copy_in(0x08000000,actual.rt.memory().bytes());reference.rt.memory().copy_in(0x04000000,vram);
    for(unsigned n=0;n<4;++n)reference.rt.memory().copy_in(reference.v+n*24,std::span<const std::uint8_t>(raw).subspan(n*192,24));
    const auto source=decoded(actual,4),single=decoded(reference,4);const auto l=source.layout;
    require(l.morph_count==8&&l.weight_count==4&&l.sample_stride==24&&l.vertex_stride==192,"actual decoded layout differs");
    exact_attributes(ge::prepare_deformed_draw(source,actual.deformation),ge::prepare_deformed_draw(single,reference.deformation));
    seed_cpu(actual);const auto cpu=actual.rt.cpu();const auto ram=actual.rt.memory().bytes();
    require(reference.draw(&reference.deformation,4,4),"capture single-record reference draw failed: "+reference.error);
    require(actual.draw(&actual.deformation,4,4),"captured actual combined strip failed: "+actual.error);
    require(actual.written==reference.written&&actual.written>0&&actual.vram()==reference.vram(),"actual combined raster differs from single-record reduction or has no visible effect");
    require(actual.end==actual.v+768&&reference.end==reference.v+96&&actual.iend==0,"actual source stride cursor incorrect");
    require(actual.rt.memory().bytes()==ram&&cpu_equal(actual.rt.cpu(),cpu),"capture direct render changed source/full CPU");
    const unsigned fb=actual.r[0x9C]&0x1FFFF0,db=actual.r[0x9E]&0x1FFFF0,stride=actual.r[0x9D]&0x7FC,ds=actual.r[0x9F]&0x7FC;
    const unsigned sx0=actual.r[0xD4]&1023,sy0=(actual.r[0xD4]>>10)&1023,sx1=std::min(actual.r[0xD5]&1023,actual.r[0x16]&1023),sy1=std::min((actual.r[0xD5]>>10)&1023,(actual.r[0x16]>>10)&1023);unsigned changed{};
    const auto after=actual.vram();for(std::size_t n=0;n<vram.size();++n)if(vram[n]!=after[n]){auto inside=[&](unsigned base,unsigned pitch,unsigned bytes){if(n<base)return false;const auto pixel=(n-base)/bytes,y=pixel/pitch,x=pixel%pitch;return x>=sx0&&x<=sx1&&y>=sy0&&y<=sy1;};require(inside(fb,stride,4)||inside(db,ds,2),"capture write escaped bounded color/depth scissor");++changed;}
    require(changed>0,"capture published no real color/depth bytes");
    actual.rt.memory().copy_in(0x04000000,vram);stream(actual,4,4);
    std::cout<<"PASS Actual captured1CC3A1 strip4: committed pixels="<<actual.written<<", changed color/depth bytes="<<changed<<"; independent single-record attributes/full VRAM and actual streamed GE HLE match\n";
}
int main(int argc,char **argv){try{require(argc<=2,"Usage: ge_combined_deformation_test [captured failure prefix]");canonical_equivalence();linear_weight_cases();independent_vectors();failures();
    for(unsigned indices:{0u,1u,2u}){Fixture f;f.source(8,1,4,indices);f.deformation.bones[3][9]=2;stream(f,3,3);}
    std::cout<<"PASS Streamed canonical combined state, PRIM/index cursors and scheduler/NaN CPU invariants\n";
    if(argc==2)captured(argv[1]);std::cout<<"PASS "<<assertions<<" combined deformation assertions; PSP float24 intermediates and negative/nonunit hardware precision remain unverified\n";
}catch(const std::exception &e){std::cerr<<"FAIL "<<e.what()<<'\n';return 1;}}
