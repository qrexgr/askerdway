#include "world_frame_renderer.h"
#include "creep_badge.h"
#include "creep_emoticon_pixels.h"
#include "camp_icon_pixels.h"
#include <d3d11_1.h>
#include <dxgi1_5.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <atomic>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <MinHook.h>

#ifndef ASKERDWAY_DIAGNOSTICS
#define ASKERDWAY_DIAGNOSTICS 1
#endif

namespace world_frame {
namespace {
using Microsoft::WRL::ComPtr;
using present_fn=HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
using resize_fn=HRESULT (STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT,UINT,DXGI_FORMAT,UINT);
present_fn original_present=nullptr;
void* present_entry=nullptr;
std::array<std::uint8_t,16> present_patch{};
IDXGISwapChain* hooked_game_chain=nullptr; // Observation only; never owned/dereferenced during teardown.
thread_local unsigned present_depth=0;
std::atomic<std::uint64_t> rejected_recursion{0};
std::atomic<bool> object_attached{false};
HWND game_window=nullptr;
std::atomic<HWND> interactive_window{nullptr};
std::atomic<menu_draw_fn> menu_draw{nullptr};
std::atomic<menu_resize_fn> menu_resize{nullptr};
std::atomic<world_view_reader> view_reader{nullptr};
std::atomic<bool> enabled{false};
std::atomic<unsigned> calls{0},visible_count{0},render_us{0};
std::atomic<std::uint64_t> presents{0},drawn{0};
std::atomic<std::uint64_t> gpu_builds{0},tile_uploads{0};
std::atomic<bool> gpu_ready{false};
std::atomic<bool> draw_bypassed{false};
std::array<std::atomic<unsigned>,1024> sampled_intervals{};
std::atomic<unsigned> sample_remaining{0},sample_cursor{0};
std::atomic<LONGLONG> sample_previous{0};
void sample_present_interval() {
    if(!sample_remaining.load(std::memory_order_relaxed))return;
    LARGE_INTEGER tick{};QueryPerformanceCounter(&tick);
    static const LONGLONG frequency=[](){LARGE_INTEGER f{};QueryPerformanceFrequency(&f);return f.QuadPart;}();
    const auto previous=sample_previous.exchange(tick.QuadPart,std::memory_order_relaxed);
    if(previous&&frequency>0){
        const auto interval=static_cast<unsigned>(std::min<LONGLONG>(UINT_MAX,(tick.QuadPart-previous)*1000000/frequency));
        sampled_intervals[sample_cursor.fetch_add(1,std::memory_order_relaxed)%sampled_intervals.size()].store(interval,std::memory_order_relaxed);
        auto remaining=sample_remaining.load(std::memory_order_relaxed);
        while(remaining&&!sample_remaining.compare_exchange_weak(remaining,remaining-1,std::memory_order_relaxed)){}
    }
}
std::atomic<unsigned> game_present_us{0},game_present_peak_us{0};
std::atomic<std::uint64_t> game_present_peak_reset_at{0};
void record_present_duration(unsigned elapsed) noexcept {
    game_present_us.store(elapsed, std::memory_order_relaxed);
    const auto now = GetTickCount64();
    auto reset_time = game_present_peak_reset_at.load(std::memory_order_relaxed);
    if (now >= reset_time) {
        game_present_peak_reset_at.store(now + 1000, std::memory_order_relaxed);
        game_present_peak_us.store(elapsed, std::memory_order_relaxed);
    } else {
        auto current_peak = game_present_peak_us.load(std::memory_order_relaxed);
        while (elapsed > current_peak &&
               !game_present_peak_us.compare_exchange_weak(current_peak, elapsed,
                   std::memory_order_relaxed)) {}
    }
}
SRWLOCK scene_lock=SRWLOCK_INIT,gpu_lock=SRWLOCK_INIT;
scene pending{},current{};
std::array<tile_pixels,capacity> pending_pixels{};
std::array<tile_pixels,capacity> upload_pixels{}; // Owned by gpu_lock.
std::array<std::uint64_t,capacity> uploaded{};
constexpr unsigned columns=8,rows=(capacity+columns-1)/columns+1,atlas_width=columns*tile_width,atlas_height=rows*tile_height;
std::array<std::uint8_t,atlas_width*atlas_height*4> atlas_pixels{};
struct vertex { float x,y,u,v; std::uint32_t color; };
constexpr unsigned max_vertices=capacity*(creep_badge::vertices+24*6+6)+16*48+32*96+16*24*6;
ComPtr<ID3D11Device> device;
ComPtr<ID3D11DeviceContext1> context;
#if defined(ASKERDWAY_RENDER_DEFERRED)
ComPtr<ID3D11DeviceContext1> recording_context;
#endif
ComPtr<ID3D11RenderTargetView> target;
ComPtr<ID3D11Texture2D> atlas;
ComPtr<ID3D11ShaderResourceView> texture;
ComPtr<ID3D11VertexShader> vs;
ComPtr<ID3D11PixelShader> ps;
ComPtr<ID3D11InputLayout> layout;
ComPtr<ID3D11Buffer> vertices;
ComPtr<ID3D11SamplerState> sampler;
ComPtr<ID3D11BlendState> blend;
ComPtr<ID3D11DepthStencilState> depth;
ComPtr<ID3D11RasterizerState> raster;
ComPtr<ID3DBlob> vertex_bytecode,pixel_bytecode;
IDXGISwapChain* target_chain=nullptr; // Non-owning: no reference that prevents game teardown.
unsigned width=0,height=0;
ULONGLONG retry_gpu_at=0;
ULONGLONG alt_clock=0;
float alt_alpha=0;
struct call_guard { call_guard(){calls.fetch_add(1);} ~call_guard(){calls.fetch_sub(1);} };

struct ScopedD3D11State {
    ID3D11DeviceContext* ctx=nullptr;
    ID3D11RenderTargetView* rtvs[D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT]{};
    ID3D11DepthStencilView* dsv=nullptr;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE]{};
    UINT viewport_count=D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    ID3D11RasterizerState* raster_state=nullptr;
    ID3D11BlendState* blend_state=nullptr;
    FLOAT blend_factor[4]{};
    UINT sample_mask=0xffffffffu;
    ID3D11DepthStencilState* depth_state=nullptr;
    UINT stencil_ref=0;
    ID3D11Buffer* vb=nullptr;
    UINT vb_stride=0,vb_offset=0;
    ID3D11InputLayout* input_layout=nullptr;
    D3D11_PRIMITIVE_TOPOLOGY topology=D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ID3D11VertexShader* vertex_shader=nullptr;
    ID3D11ClassInstance* vs_instances[256]{};
    UINT vs_instance_count=256;
    ID3D11PixelShader* pixel_shader=nullptr;
    ID3D11ClassInstance* ps_instances[256]{};
    UINT ps_instance_count=256;
    ID3D11GeometryShader* geometry_shader=nullptr;
    ID3D11ClassInstance* gs_instances[256]{};
    UINT gs_instance_count=256;
    ID3D11HullShader* hull_shader=nullptr;
    ID3D11ClassInstance* hs_instances[256]{};
    UINT hs_instance_count=256;
    ID3D11DomainShader* domain_shader=nullptr;
    ID3D11ClassInstance* ds_instances[256]{};
    UINT ds_instance_count=256;
    ID3D11ShaderResourceView* ps_srv=nullptr;
    ID3D11SamplerState* ps_sampler=nullptr;

    explicit ScopedD3D11State(ID3D11DeviceContext* c) noexcept : ctx(c) {
        ctx->OMGetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,rtvs,&dsv);
        ctx->RSGetViewports(&viewport_count,viewports);
        ctx->RSGetState(&raster_state);
        ctx->OMGetBlendState(&blend_state,blend_factor,&sample_mask);
        ctx->OMGetDepthStencilState(&depth_state,&stencil_ref);
        ctx->IAGetVertexBuffers(0,1,&vb,&vb_stride,&vb_offset);
        ctx->IAGetInputLayout(&input_layout);
        ctx->IAGetPrimitiveTopology(&topology);
        ctx->VSGetShader(&vertex_shader,vs_instances,&vs_instance_count);
        ctx->PSGetShader(&pixel_shader,ps_instances,&ps_instance_count);
        ctx->GSGetShader(&geometry_shader,gs_instances,&gs_instance_count);
        ctx->HSGetShader(&hull_shader,hs_instances,&hs_instance_count);
        ctx->DSGetShader(&domain_shader,ds_instances,&ds_instance_count);
        ctx->PSGetShaderResources(0,1,&ps_srv);
        ctx->PSGetSamplers(0,1,&ps_sampler);
    }

    ScopedD3D11State(const ScopedD3D11State&)=delete;
    ScopedD3D11State& operator=(const ScopedD3D11State&)=delete;

    ~ScopedD3D11State() noexcept {
        ctx->OMSetRenderTargets(D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT,rtvs,dsv);
        ctx->RSSetViewports(viewport_count,viewports);
        ctx->RSSetState(raster_state);
        ctx->OMSetBlendState(blend_state,blend_factor,sample_mask);
        ctx->OMSetDepthStencilState(depth_state,stencil_ref);
        ctx->IASetVertexBuffers(0,1,&vb,&vb_stride,&vb_offset);
        ctx->IASetInputLayout(input_layout);
        ctx->IASetPrimitiveTopology(topology);
        ctx->VSSetShader(vertex_shader,vs_instances,vs_instance_count);
        ctx->PSSetShader(pixel_shader,ps_instances,ps_instance_count);
        ctx->GSSetShader(geometry_shader,gs_instances,gs_instance_count);
        ctx->HSSetShader(hull_shader,hs_instances,hs_instance_count);
        ctx->DSSetShader(domain_shader,ds_instances,ds_instance_count);
        ctx->PSSetShaderResources(0,1,&ps_srv);
        ctx->PSSetSamplers(0,1,&ps_sampler);

        for(auto* rtv : rtvs) if(rtv) rtv->Release();
        if(dsv) dsv->Release();
        if(raster_state) raster_state->Release();
        if(blend_state) blend_state->Release();
        if(depth_state) depth_state->Release();
        if(vb) vb->Release();
        if(input_layout) input_layout->Release();
        for(UINT i=0;i<vs_instance_count;++i) if(vs_instances[i]) vs_instances[i]->Release();
        if(vertex_shader) vertex_shader->Release();
        for(UINT i=0;i<ps_instance_count;++i) if(ps_instances[i]) ps_instances[i]->Release();
        if(pixel_shader) pixel_shader->Release();
        for(UINT i=0;i<gs_instance_count;++i) if(gs_instances[i]) gs_instances[i]->Release();
        if(geometry_shader) geometry_shader->Release();
        for(UINT i=0;i<hs_instance_count;++i) if(hs_instances[i]) hs_instances[i]->Release();
        if(hull_shader) hull_shader->Release();
        for(UINT i=0;i<ds_instance_count;++i) if(ds_instances[i]) ds_instances[i]->Release();
        if(domain_shader) domain_shader->Release();
        if(ps_srv) ps_srv->Release();
        if(ps_sampler) ps_sampler->Release();
    }
};

HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain*,UINT,UINT);
void reset_gpu() {
    gpu_ready.store(false,std::memory_order_release);
#if defined(ASKERDWAY_RENDER_DEFERRED)
    recording_context.Reset();
#endif
    target.Reset(); texture.Reset(); atlas.Reset(); vs.Reset(); ps.Reset();
    layout.Reset(); vertices.Reset(); sampler.Reset(); blend.Reset(); depth.Reset(); raster.Reset();
    context.Reset(); device.Reset(); target_chain=nullptr; uploaded.fill(0); width=height=0;
#if defined(ASKERDWAY_RENDER_STREAMING)
    atlas_pixels.fill(0);
#endif
}
bool prepare_shaders() {
    if(vertex_bytecode&&pixel_bytecode)return true;
    constexpr char shader[]=R"(
struct V { float2 p:POSITION; float2 uv:TEXCOORD; float4 c:COLOR; };
struct P { float4 p:SV_POSITION; float2 uv:TEXCOORD; float4 c:COLOR; };
P VS(V v) { P p; p.p=float4(v.p,0,1);p.uv=v.uv;p.c=v.c;return p; }
Texture2D tex:register(t0); SamplerState smp:register(s0);
float4 PS(P p):SV_Target {
    // Complete analytic circles, not separately rasterized short segments.
    // Their coverage follows subpixel camera motion continuously.
    if (p.uv.x < -6.5) {
        bool badge = p.uv.x < -10;
        float2 q = float2(p.uv.x + (badge ? 12 : 8), p.uv.y);
        float r = length(q);
        float pixel = max(length(float2(ddx(q.x), ddy(q.x))), 0.00001);
        if (!badge) {
            float inner = max(0, 1-max(.30,2*pixel));
            p.c.a *= saturate((1-r)/pixel+.5) * saturate((r-inner)/pixel+.5);
            return p.c;
        }
        float4 rim = float4(233,66,66,255)/255.;
        float4 face = float4(35,21,20,255)/255.;
        float4 c = lerp(float4(13,8,8,208)/255., rim, saturate((.92-r)/pixel+.5));
        c = lerp(c, face, saturate((.736-r)/pixel+.5));
        c.a *= saturate((1-r)/pixel+.5);
        return c;
    }
    // Lines reserve negative UVs in the existing vertex format. A one-pixel
    // coverage ramp needs no MSAA or extra pass. Joined ring segments use
    // [-5,-4] and omit end-cap fading to keep their circumference continuous.
    if (p.uv.x < -1.5) {
        float2 edge = float2(p.uv.x < -3.5 ? 1 : min(p.uv.x+3, -2-p.uv.x), 1-abs(p.uv.y));
        // Euclidean derivatives keep a diagonal stroke as crisp as a horizontal
        // one; the L1 fwidth over-blurs thin diagonals by up to sqrt(2).
        float2 dx = ddx(p.uv), dy = ddy(p.uv);
        edge /= max(sqrt(dx*dx+dy*dy), float2(0.00001,0.00001));
        p.c.a *= saturate(min(edge.x,edge.y));
        return p.c;
    }
    return p.uv.x<0 ? p.c : tex.Sample(smp,p.uv)*p.c;
}
)";
    return SUCCEEDED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"VS","vs_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&vertex_bytecode,nullptr))&&
        SUCCEEDED(D3DCompile(shader,sizeof(shader),nullptr,nullptr,nullptr,"PS","ps_4_0",
        D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&pixel_bytecode,nullptr));
}
bool create_gpu(IDXGISwapChain* chain) {
    ComPtr<ID3D11Device> found;
    if(FAILED(chain->GetDevice(IID_PPV_ARGS(&found)))) return false;
    if(device.Get()!=found.Get()) reset_gpu();
    if(!device) {
        device=found;
        ComPtr<ID3D11Device1> d1;
        ComPtr<ID3D11DeviceContext> base;
        device->GetImmediateContext(&base);
        if(FAILED(device.As(&d1))||FAILED(base.As(&context))) {reset_gpu();return false;}
#if defined(ASKERDWAY_RENDER_DEFERRED)
        if(FAILED(d1->CreateDeferredContext1(0,&recording_context))) {reset_gpu();return false;}
#endif
        if(!vertex_bytecode||!pixel_bytecode||
           FAILED(device->CreateVertexShader(vertex_bytecode->GetBufferPointer(),vertex_bytecode->GetBufferSize(),nullptr,&vs))||
           FAILED(device->CreatePixelShader(pixel_bytecode->GetBufferPointer(),pixel_bytecode->GetBufferSize(),nullptr,&ps))) {reset_gpu();return false;}
        const D3D11_INPUT_ELEMENT_DESC elements[]={
            {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA,0},
            {"COLOR",0,DXGI_FORMAT_R8G8B8A8_UNORM,0,16,D3D11_INPUT_PER_VERTEX_DATA,0}};
        D3D11_BUFFER_DESC buffer{};buffer.ByteWidth=max_vertices*sizeof(vertex);buffer.Usage=D3D11_USAGE_DYNAMIC;
        buffer.BindFlags=D3D11_BIND_VERTEX_BUFFER;buffer.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        D3D11_TEXTURE2D_DESC td{};td.Width=atlas_width;td.Height=atlas_height;td.MipLevels=td.ArraySize=1;
        td.Format=DXGI_FORMAT_B8G8R8A8_UNORM;td.SampleDesc.Count=1;td.Usage=D3D11_USAGE_DEFAULT;td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
#if defined(ASKERDWAY_RENDER_STREAMING)
        td.Usage=D3D11_USAGE_DYNAMIC;td.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
#endif
        atlas_pixels.fill(0);
        constexpr unsigned creep_y=(rows-1)*tile_height+2;
        for(unsigned y=0;y<creep_emoticon::size;++y)
            std::memcpy(atlas_pixels.data()+((creep_y+y)*atlas_width+2)*4,
                creep_emoticon::bgra.data()+y*creep_emoticon::size,creep_emoticon::size*4);
        for(unsigned variant=0;variant<2;++variant){
            const auto& sprite=variant?camp_icons::uncertain:camp_icons::cleared;
            for(unsigned y=0;y<camp_icons::size;++y)
                std::memcpy(atlas_pixels.data()+((creep_y+y)*atlas_width+72+variant*68)*4,
                    sprite.data()+y*camp_icons::size,camp_icons::size*4);
        }
        const D3D11_SUBRESOURCE_DATA initial{atlas_pixels.data(),atlas_width*4,0};
        D3D11_SAMPLER_DESC sd{};sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;sd.MaxLOD=D3D11_FLOAT32_MAX;
        D3D11_BLEND_DESC bd{};auto& b=bd.RenderTarget[0];b.BlendEnable=TRUE;
        b.SrcBlend=D3D11_BLEND_SRC_ALPHA;b.DestBlend=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOp=D3D11_BLEND_OP_ADD;
        b.SrcBlendAlpha=D3D11_BLEND_ONE;b.DestBlendAlpha=D3D11_BLEND_INV_SRC_ALPHA;b.BlendOpAlpha=D3D11_BLEND_OP_ADD;b.RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
        D3D11_DEPTH_STENCIL_DESC dd{};dd.DepthEnable=FALSE;dd.DepthWriteMask=D3D11_DEPTH_WRITE_MASK_ZERO;dd.DepthFunc=D3D11_COMPARISON_ALWAYS;
        D3D11_RASTERIZER_DESC rd{};rd.FillMode=D3D11_FILL_SOLID;rd.CullMode=D3D11_CULL_NONE;rd.DepthClipEnable=TRUE;
        if(FAILED(device->CreateInputLayout(elements,3,vertex_bytecode->GetBufferPointer(),vertex_bytecode->GetBufferSize(),&layout))||
           FAILED(device->CreateBuffer(&buffer,nullptr,&vertices))||FAILED(device->CreateTexture2D(&td,&initial,&atlas))||
           FAILED(device->CreateShaderResourceView(atlas.Get(),nullptr,&texture))||FAILED(device->CreateSamplerState(&sd,&sampler))||
           FAILED(device->CreateBlendState(&bd,&blend))||FAILED(device->CreateDepthStencilState(&dd,&depth))||FAILED(device->CreateRasterizerState(&rd,&raster))) {reset_gpu();return false;}
        gpu_builds.fetch_add(1,std::memory_order_relaxed);
    }
    if(target_chain!=chain) {target.Reset();target_chain=chain;}
    return true;
}
bool acquire_frame_target(IDXGISwapChain* chain) {
    // A view owns the swapchain's buffer. Never retain it across Present or
    // video settings changes, including paths that do not call ResizeBuffers.
    target.Reset();
    {
        ComPtr<ID3D11Texture2D> back;
        if(FAILED(chain->GetBuffer(0,IID_PPV_ARGS(&back)))) return false;
        D3D11_TEXTURE2D_DESC desc{};back->GetDesc(&desc);width=desc.Width;height=desc.Height;
        // Reuse the game's current output view when it is this exact buffer.
        context->OMGetRenderTargets(1,&target,nullptr);
        if(target) {
            ComPtr<ID3D11Resource> resource;target->GetResource(&resource);
            if(resource.Get()!=back.Get())target.Reset();
        }
        if(!target&&FAILED(device->CreateRenderTargetView(back.Get(),nullptr,&target)))return false;
    }
    gpu_ready.store(width&&height,std::memory_order_release);
    return width&&height;
}
bool read_matrix(std::uintptr_t address,float (&matrix)[4][4]) {
    if(!address)return false;
#if defined(_MSC_VER)
    __try {std::memcpy(matrix,reinterpret_cast<const void*>(address),sizeof(matrix));return true;}
    __except(EXCEPTION_EXECUTE_HANDLER) {return false;}
#else
    MEMORY_BASIC_INFORMATION info{};
    if(!address||!VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof(info))||info.State!=MEM_COMMIT||
        (info.Protect&(PAGE_NOACCESS|PAGE_GUARD))||address+sizeof(matrix)>reinterpret_cast<std::uintptr_t>(info.BaseAddress)+info.RegionSize) return false;
    std::memcpy(matrix,reinterpret_cast<void*>(address),sizeof(matrix));return true;
#endif
}
bool project(const float* p,const float (&m)[4][4],float& x,float& y,bool limit=true) {
    const float cx=m[0][0]*p[0]+m[0][1]*p[1]+m[0][2]*p[2]+m[0][3];
    const float cy=m[1][0]*p[0]+m[1][1]*p[1]+m[1][2]*p[2]+m[1][3];
    const float cw=m[3][0]*p[0]+m[3][1]*p[1]+m[3][2]*p[2]+m[3][3];
    if(!std::isfinite(cx)||!std::isfinite(cy)||!std::isfinite(cw)||cw<=.001f)return false;
    x=cx/cw;y=cy/cw;
    if(!std::isfinite(x)||!std::isfinite(y)||(limit&&(std::abs(x)>1.12f||std::abs(y)>1.12f))) return false;
    x=(x+1)*.5f*width;y=(1-y)*.5f*height;return true;
}
void render(IDXGISwapChain* chain,bool alt_down=(GetAsyncKeyState(VK_MENU)&0x8000)!=0) {
    // Every world primitive shares this admission check. Dashboard visibility
    // changes faster than worker publication and does not end the match/model.
    if(const auto reader=view_reader.load(std::memory_order_acquire);reader&&!reader()) {
        visible_count.store(0,std::memory_order_relaxed);render_us.store(0,std::memory_order_relaxed);
        alt_alpha=0;alt_clock=0;return;
    }
    if(draw_bypassed.load(std::memory_order_relaxed)) {
        visible_count.store(0, std::memory_order_relaxed);
        render_us.store(0, std::memory_order_relaxed);
        return;
    }
    LARGE_INTEGER start{},finish{},frequency{};
    if constexpr(ASKERDWAY_DIAGNOSTICS)QueryPerformanceCounter(&start);
    if(!TryAcquireSRWLockExclusive(&gpu_lock))return;
    const auto unlock=[](){target.Reset();ReleaseSRWLockExclusive(&gpu_lock);};
    if(TryAcquireSRWLockShared(&scene_lock)) {
        current=pending;
        ReleaseSRWLockShared(&scene_lock);
    }
    const auto frame_time=GetTickCount64();
    const float alt_step=alt_clock&&frame_time>=alt_clock?
        std::min(float(frame_time-alt_clock)/45.f,1.f):0.f;
    alt_clock=frame_time;
    alt_alpha=std::clamp(alt_alpha+(alt_down?alt_step:-alt_step),0.f,1.f);
    bool has_content=false;
    for(unsigned i=0;i<current.direction_count&&!has_content;++i)
        has_content=current.directions[i].expires>frame_time;
    for(unsigned i=0;i<current.area_count&&!has_content;++i)
        has_content=current.areas[i].expires>frame_time;
    for(unsigned i=0;i<current.pulse_count&&!has_content;++i)
        has_content=current.pulses[i].expires>frame_time;
    for(unsigned i=0;i<current.count&&!has_content;++i)
        has_content=(!current.markers[i].alt_only||alt_alpha>0)&&current.markers[i].expires>frame_time;
    if(!enabled.load()||!current.enabled||(current.count==0&&current.direction_count==0&&current.area_count==0&&current.pulse_count==0)) {
        visible_count.store(0);render_us.store(0);unlock();return;
    }
    const auto clock=GetTickCount64();
    if(clock<retry_gpu_at){unlock();return;}
    // Prepare device resources and cached ALT tiles while ALT is still up.
    // The key press must not trigger CreateTexture/Shader or a full upload.
    // COM device discovery is a lifecycle operation, not per-frame work.
    if(!create_gpu(chain)) {
        retry_gpu_at=clock+1000;unlock();return;
    }
    if(!acquire_frame_target(chain)){unlock();return;}
    // A try-lock prevents the game frame waiting on entity/inventory polling.
    std::array<unsigned,8> upload_indices{};
    unsigned upload_count=0;
    if(TryAcquireSRWLockShared(&scene_lock)) {
        current=pending;
        for(unsigned i=0;i<current.count&&upload_count<upload_indices.size();++i)
            if(!current.markers[i].wave&&current.markers[i].version!=uploaded[i]) {
            upload_pixels[i]=pending_pixels[i];
            upload_indices[upload_count++]=i;
        }
        ReleaseSRWLockShared(&scene_lock);
    }
    // Driver calls never hold the publication lock. Bound cold-scene uploads
    // per frame; ordinary five-player ALT content fits in the first batch.
#if defined(ASKERDWAY_RENDER_STREAMING)
    if(upload_count) {
        for(unsigned j=0;j<upload_count;++j) {
            const auto i=upload_indices[j];
            const unsigned x=(i%columns)*tile_width,y=(i/columns)*tile_height;
            for(unsigned row=0;row<tile_height;++row)
                std::memcpy(atlas_pixels.data()+((y+row)*atlas_width+x)*4,
                    upload_pixels[i].data()+row*tile_width*4,tile_width*4);
        }
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(SUCCEEDED(context->Map(atlas.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) {
            for(unsigned row=0;row<atlas_height;++row)
                std::memcpy(static_cast<std::uint8_t*>(mapped.pData)+row*mapped.RowPitch,
                    atlas_pixels.data()+row*atlas_width*4,atlas_width*4);
            context->Unmap(atlas.Get(),0);
            for(unsigned j=0;j<upload_count;++j)uploaded[upload_indices[j]]=current.markers[upload_indices[j]].version;
        } else upload_count=0;
    }
#else
    for(unsigned j=0;j<upload_count;++j) {
        const auto i=upload_indices[j];
        const unsigned x=(i%columns)*tile_width,y=(i/columns)*tile_height;
        const D3D11_BOX box{x,y,0,x+tile_width,y+tile_height,1};
        context->UpdateSubresource(atlas.Get(),0,&box,upload_pixels[i].data(),tile_width*4,0);
        uploaded[i]=current.markers[i].version;
    }
#endif
    if(upload_count)tile_uploads.fetch_add(upload_count,std::memory_order_relaxed);
    if(!has_content){visible_count.store(0);render_us.store(0);unlock();return;}
#if defined(ASKERDWAY_RENDER_DEFERRED)
    auto* drawing=recording_context.Get();
#else
    auto* drawing=context.Get();
#endif
    unsigned count=0,visible=0;
    static thread_local std::array<vertex,max_vertices> verts;
    float matrix[4][4]{};
    const auto now=GetTickCount64();
    auto append=[&](float x,float y,float u,float v,std::uint32_t color) {
        if(count<verts.size())verts[count++]={x*2/width-1,1-y*2/height,u,v,color};
    };
    auto line=[&](float x,float y,float ex,float ey,float thickness,std::uint32_t color,bool joined=false) {
        const float len=std::hypot(ex-x,ey-y);if(len<1)return;
        if(count+6>verts.size())return; // Never submit a partial quad.
        const float dx=(ex-x)/len,dy=(ey-y)/len;
        const float nx=-dy*(thickness+1)*.5f,ny=dx*(thickness+1)*.5f;
        if(!joined){x-=dx*.5f;y-=dy*.5f;ex+=dx*.5f;ey+=dy*.5f;}
        const float u=joined?-5.f:-3.f,v=joined?-4.f:-2.f;
        append(x+nx,y+ny,u,1,color);append(ex+nx,ey+ny,v,1,color);append(ex-nx,ey-ny,v,-1,color);
        append(x+nx,y+ny,u,1,color);append(ex-nx,ey-ny,v,-1,color);append(x-nx,y-ny,u,-1,color);
    };
    auto circle=[&](float x,float y,float radius,float tag,std::uint32_t color) {
        // Include half a pixel beyond the mathematical edge for full coverage.
        const float extent=radius+.5f, uv=extent/radius;
        append(x-extent,y-extent,tag-uv,-uv,color);append(x+extent,y-extent,tag+uv,-uv,color);append(x+extent,y+extent,tag+uv,uv,color);
        append(x-extent,y-extent,tag-uv,-uv,color);append(x+extent,y+extent,tag+uv,uv,color);append(x-extent,y+extent,tag-uv,uv,color);
    };
    bool needs_matrix=false;
    for(unsigned i=0;i<current.direction_count&&!needs_matrix;++i)
        needs_matrix=current.directions[i].expires>now;
    for(unsigned i=0;i<current.area_count&&!needs_matrix;++i)
        needs_matrix=current.areas[i].expires>now;
    for(unsigned i=0;i<current.pulse_count&&!needs_matrix;++i)
        needs_matrix=current.pulses[i].expires>now;
    for(unsigned i=0;i<current.count&&!needs_matrix;++i)
        needs_matrix=!current.markers[i].screen_space&&current.markers[i].expires>now&&
            (!current.markers[i].alt_only||alt_alpha>0);
    const bool matrix_ready=needs_matrix&&read_matrix(current.matrix_address,matrix);
    const auto& map=current.minimap;
    const float map_max=static_cast<float>(std::min(520u,std::min(height,width/2)));
    const float correction=std::clamp(map.size_percent,60,150)/100.f;
    const bool automatic=map.automatic&&map.viewport_width>0&&map.viewport_height>0&&map.width>0&&map.height>0;
    const float map_size=std::clamp(automatic?map.width*width/map.viewport_width*correction:
        std::round(320.f*height/1080.f*correction),std::min(120.f,map_max),map_max);
    const float base_x=automatic?map.x*width/map.viewport_width+(map.width*width/map.viewport_width-map_size)*.5f:
        (map.on_right?width-map_size:0.f);
    const float base_y=automatic?map.y*height/map.viewport_height+map.height*height/map.viewport_height-map_size:height-map_size;
    const float map_x=std::clamp(base_x+std::clamp(map.offset_x,-120,120),0.f,width-map_size);
    const float map_y=std::clamp(base_y+std::clamp(map.offset_y,-120,120),0.f,height-map_size);
    const auto map_point=[&](const float* position,float& x,float& y) {
        const float dx=map.maximum[0]-map.minimum[0],dy=map.maximum[1]-map.minimum[1];
        if(!map.enabled||!std::isfinite(dx)||!std::isfinite(dy)||dx<=0||dy<=0||
            !std::isfinite(position[0])||!std::isfinite(position[1]))return false;
        x=map_x+std::clamp((position[0]-map.minimum[0])/dx,0.f,1.f)*map_size;
        y=map_y+(1.f-std::clamp((position[1]-map.minimum[1])/dy,0.f,1.f))*map_size;return true;
    };
    const auto tile_quad=[&](unsigned i,float x,float y,float size,float crop_x,float crop_y,float crop_size) {
        const float l=x-size*.5f,r=x+size*.5f,t=y-size*.5f,b=y+size*.5f;
        const float u=((i%columns)*tile_width+crop_x)/atlas_width,v=((i/columns)*tile_height+crop_y)/atlas_height;
        const float eu=u+crop_size/atlas_width,ev=v+crop_size/atlas_height;
        append(l,t,u,v,0xffffffff);append(r,t,eu,v,0xffffffff);append(r,b,eu,ev,0xffffffff);
        append(l,t,u,v,0xffffffff);append(r,b,eu,ev,0xffffffff);append(l,b,u,ev,0xffffffff);
    };
    if(current.enabled) {
        // At most 32 areas. Cull against the camera's viewport expanded by
        // 500 world units before emitting any geometry; no map-wide particles.
        for(unsigned i=0;i<current.area_count;++i) {
            const auto& area=current.areas[i];
            if(area.expires<=now|| (area.alt_only&&alt_alpha<=.01f))continue;
            if(area.cleared){
                const auto sprite=[&](float x,float y,float half){
                    const float u=float(72+(area.uncertain?68:0))/atlas_width,v=float((rows-1)*tile_height+2)/atlas_height;
                    const float eu=u+float(camp_icons::size)/atlas_width,ev=v+float(camp_icons::size)/atlas_height;
                    append(x-half,y-half,u,v,0xffffffff);append(x+half,y-half,eu,v,0xffffffff);append(x+half,y+half,eu,ev,0xffffffff);
                    append(x-half,y-half,u,v,0xffffffff);append(x+half,y+half,eu,ev,0xffffffff);append(x-half,y+half,u,ev,0xffffffff);
                };
                float x=0,y=0;
                if(matrix_ready&&project(area.center,matrix,x,y))sprite(x,y,16.f);
                continue;
            }
            float low[3]{area.center[0]-area.half[0],area.center[1]-area.half[1],area.center[2]};
            float high[3]{area.center[0]+area.half[0],area.center[1]+area.half[1],area.center[2]};
            float mx1,my1,mx2,my2;
            if(area.blocked&&map_point(low,mx1,my1)&&map_point(high,mx2,my2)) {
                line(mx1,my1,mx2,my1,1.5f,0xc05050ee);line(mx2,my1,mx2,my2,1.5f,0xc05050ee);
                line(mx2,my2,mx1,my2,1.5f,0xc05050ee);line(mx1,my2,mx1,my1,1.5f,0xc05050ee);
            }
            if(!matrix_ready)continue;
            float x[4]{},y[4]{};bool valid=true;
            float left=INFINITY,right=-INFINITY,top=INFINITY,bottom=-INFINITY;
            for(int c=0;c<4;++c) {
                const float sx=(c==0||c==3)?-1.f:1.f,sy=c<2?-1.f:1.f;
                float p[3]{area.center[0]+sx*(area.half[0]+500),
                    area.center[1]+sy*(area.half[1]+500),area.center[2]};
                float px,py;
                if(!project(p,matrix,px,py,false)){valid=false;break;}
                left=std::min(left,px);right=std::max(right,px);
                top=std::min(top,py);bottom=std::max(bottom,py);
                p[0]=area.center[0]+sx*area.half[0];p[1]=area.center[1]+sy*area.half[1];
                if(!project(p,matrix,x[c],y[c],false)){valid=false;break;}
            }
            if(!valid||right<0||left>width||bottom<0||top>height)continue;
            const std::uint32_t color=area.blocked?0xc05050ee:0x8064b6a0;
            // Broken outline communicates inferred geometry/body position.
            for(int c=0;c<4;++c) {
                const int n=(c+1)%4;
                if(area.estimated) {
                    line(x[c],y[c],x[c]+(x[n]-x[c])*.42f,y[c]+(y[n]-y[c])*.42f,1.5f,color);
                    line(x[c]+(x[n]-x[c])*.58f,y[c]+(y[n]-y[c])*.58f,x[n],y[n],1.5f,color);
                } else line(x[c],y[c],x[n],y[n],1.5f,color);
            }
            float cx,cy;
            if(area.blocked&&project(area.center,matrix,cx,cy)) {
                line(cx-5,cy-5,cx+5,cy+5,2,color);
                line(cx-5,cy+5,cx+5,cy-5,2,color);
            }
        }
        for(unsigned i=0;i<current.pulse_count&&matrix_ready;++i){
            const auto& pulse=current.pulses[i];if(pulse.expires<=now)continue;
            const float age=std::clamp(float(now-pulse.started)/1200.f,0.f,1.f);
            const float radius=100.f+150.f*age;
            const auto color=(static_cast<std::uint32_t>(200*(1-age))<<24)|0x006088ff;
            for(int segment=0;segment<24;++segment){
                const float a=segment*6.2831853f/24,b=(segment+1)*6.2831853f/24;
                const float p[3]{pulse.center[0]+std::cos(a)*radius,pulse.center[1]+std::sin(a)*radius,pulse.center[2]};
                const float q[3]{pulse.center[0]+std::cos(b)*radius,pulse.center[1]+std::sin(b)*radius,pulse.center[2]};
                float x,y,ex,ey;if(project(p,matrix,x,y)&&project(q,matrix,ex,ey))line(x,y,ex,ey,2.f,color,true);
            }
        }
        for(unsigned i=0;i<current.count;++i) {
            const auto& m=current.markers[i];float x,y;
            if((!m.wave&&m.version!=uploaded[i])||m.expires<=now||(m.alt_only&&alt_alpha<=0))continue;
            float displayed[3]{m.position[0],m.position[1],m.position[2]};
            if(m.motion_key) {
                struct motion {std::uint64_t key=0,at=0;float p[3]{};};
                static std::array<motion,capacity> smooth{};
                auto* value=&smooth[0];
                for(auto& candidate:smooth) {
                    if(candidate.key==m.motion_key){value=&candidate;break;}
                    if(candidate.at<value->at)value=&candidate;
                }
                const bool reset=value->key!=m.motion_key||now<value->at||now-value->at>2000||
                    std::hypot(value->p[0]-m.position[0],value->p[1]-m.position[1])>4096;
                const float step=reset?1.f:1.f-std::exp(-float(now-value->at)/60.f);
                for(int axis=0;axis<3;++axis)displayed[axis]=value->p[axis]+(m.position[axis]-value->p[axis])*step;
                value->key=m.motion_key;value->at=now;std::copy_n(displayed,3,value->p);
            }
            if(m.wave) {
                const float scale=std::clamp(float(height)/1080.f,.65f,2.f);
                if(m.minimap_icon&&map_point(displayed,x,y)) {
                    circle(x,y,7.f*scale,-8.f,0xff49dce8);
                    ++visible;
                }
                if(matrix_ready&&project(displayed,matrix,x,y)) {
                    circle(x,y,19.5f*scale,-12.f,0xffffffff);
                    const float half=12.5f*scale;
                    const float u=2.f/atlas_width,v=float((rows-1)*tile_height+2)/atlas_height;
                    const float eu=(2.f+creep_emoticon::size)/atlas_width,ev=v+float(creep_emoticon::size)/atlas_height;
                    append(x-half,y-half,u,v,0xffffffff);append(x+half,y-half,eu,v,0xffffffff);append(x+half,y+half,eu,ev,0xffffffff);
                    append(x-half,y-half,u,v,0xffffffff);append(x+half,y+half,eu,ev,0xffffffff);append(x-half,y+half,u,ev,0xffffffff);
                    ++visible;
                }
                continue;
            }
            if(m.minimap_icon&&map_point(displayed,x,y)) {
                const float diameter=std::clamp(m.icon_diameter,16.f,48.f);
                const float size=std::clamp(28.f*map_size/320.f,12.f,42.f);
                tile_quad(i,x,y,size,tile_width*.5f-diameter*.5f,26-diameter*.5f,diameter);
                if(m.minimap_source_icon)tile_quad(i,x+size*.32f,y+size*.32f,size*.5f,
                    tile_width*.5f+diameter*.5f-9,26+diameter*.5f-20,24);
                ++visible;
            }
            const float scale=m.screen_space?float(height)/1080.f:1.f;
            if(m.screen_space){x=width*.5f+m.position[0]*scale;y=m.position[1]*scale;}
            else if(!matrix_ready||!project(displayed,matrix,x,y))continue;
            const float l=x-tile_width*.5f*scale,t=y-26*scale,r=l+tile_width*scale,b=t+tile_height*scale;
            const float u=float((i%columns)*tile_width)/atlas_width,v=float((i/columns)*tile_height)/atlas_height;
            const float eu=u+float(tile_width)/atlas_width,ev=v+float(tile_height)/atlas_height;
            const auto color=m.alt_only?(static_cast<std::uint32_t>(std::lround(255*alt_alpha))<<24)|0x00ffffff:0xffffffff;
            append(l,t,u,v,color);append(r,t,eu,v,color);append(r,b,eu,ev,color);
            append(l,t,u,v,color);append(r,b,eu,ev,color);append(l,b,u,ev,color);++visible;
        }
        for(unsigned i=0;i<current.direction_count;++i) {
            const auto& d=current.directions[i];float x,y,ex,ey;
            if(d.expires>now&&map_point(d.origin,x,y)&&map_point(d.destination,ex,ey)) {
                line(x,y,ex,ey,1.5f,d.color);
                const float length=std::hypot(ex-x,ey-y);
                if(length>24) {
                    const float dx=(ex-x)/length,dy=(ey-y)/length;
                    ex-=dx*15;ey-=dy*15;
                    line(ex,ey,ex-dx*7-dy*4,ey-dy*7+dx*4,1.5f,d.color);
                    line(ex,ey,ex-dx*7+dy*4,ey-dy*7-dx*4,1.5f,d.color);
                }
            }
            if(!matrix_ready)continue;
            const float world_length=std::hypot(d.destination[0]-d.origin[0],d.destination[1]-d.origin[1]);
            if(d.expires<=now||!std::isfinite(world_length)||world_length<1)continue;
            const float probe[3]{d.origin[0]+(d.destination[0]-d.origin[0])*128/world_length,
                d.origin[1]+(d.destination[1]-d.origin[1])*128/world_length,d.origin[2]};
            const bool at_origin=project(d.origin,matrix,x,y);
            if(at_origin) {
                if(!project(probe,matrix,ex,ey,false))continue;
            } else {
                // The source may be outside the camera. Show an arrival arrow
                // at the visible destination, still pointing from source to target.
                const float before[3]{d.destination[0]-(d.destination[0]-d.origin[0])*128/world_length,
                    d.destination[1]-(d.destination[1]-d.origin[1])*128/world_length,d.destination[2]};
                if(!project(d.destination,matrix,ex,ey)||!project(before,matrix,x,y,false))continue;
            }
            const float len=std::hypot(ex-x,ey-y);if(len<1)continue;
            const float dx=(ex-x)/len,dy=(ey-y)/len;
            // Compact indication toward even an off-screen endpoint; no line across terrain.
            if(at_origin){x+=dx*26;y+=dy*26;}
            else{x=ex-dx*66;y=ey-dy*66;}
            ex=x+dx*40;ey=y+dy*40;
            const std::uint32_t color=d.color;
            line(x,y,ex,ey,4.5f,0x8017130e);
            line(x,y,ex,ey,2.5f,color);
            line(ex,ey,ex-dx*8-dy*5,ey-dy*8+dx*5,2,color);
            line(ex,ey,ex-dx*8+dy*5,ey-dy*8-dx*5,2,color);
        }
    }
    bool submitted=false;
    if(count) {
#if !defined(ASKERDWAY_RENDER_DEFERRED)
        ScopedD3D11State state_guard(drawing);
#endif
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if(SUCCEEDED(drawing->Map(vertices.Get(),0,D3D11_MAP_WRITE_DISCARD,0,&mapped))) {
            std::memcpy(mapped.pData,verts.data(),count*sizeof(vertex));drawing->Unmap(vertices.Get(),0);
            const D3D11_VIEWPORT viewport{0,0,float(width),float(height),0,1};drawing->RSSetViewports(1,&viewport);
            drawing->RSSetState(raster.Get());drawing->OMSetBlendState(blend.Get(),nullptr,0xffffffff);
            drawing->OMSetDepthStencilState(depth.Get(),0);auto* rtv=target.Get();drawing->OMSetRenderTargets(1,&rtv,nullptr);
            auto* vb=vertices.Get();const UINT stride=sizeof(vertex),offset=0;drawing->IASetVertexBuffers(0,1,&vb,&stride,&offset);
            drawing->IASetInputLayout(layout.Get());drawing->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            drawing->GSSetShader(nullptr,nullptr,0);drawing->HSSetShader(nullptr,nullptr,0);drawing->DSSetShader(nullptr,nullptr,0);
            drawing->VSSetShader(vs.Get(),nullptr,0);drawing->PSSetShader(ps.Get(),nullptr,0);
            auto* srv=texture.Get();auto* smp=sampler.Get();drawing->PSSetShaderResources(0,1,&srv);drawing->PSSetSamplers(0,1,&smp);
            drawing->Draw(count,0);submitted=true;
        }
#if defined(ASKERDWAY_RENDER_DEFERRED)
        drawing->OMSetRenderTargets(0,nullptr,nullptr);
        ComPtr<ID3D11CommandList> commands;
        if(SUCCEEDED(drawing->FinishCommandList(FALSE,&commands)))context->ExecuteCommandList(commands.Get(),TRUE);
        else submitted=false;
#endif
    }
    // Keep basic liveness truthful in release builds too. Expensive timing
    // probes remain opt-in; these counters neither allocate nor synchronize GPU.
    if(submitted)drawn.fetch_add(1,std::memory_order_relaxed);
    visible_count.store(visible,std::memory_order_relaxed);
    if constexpr(ASKERDWAY_DIAGNOSTICS) {
        QueryPerformanceCounter(&finish);QueryPerformanceFrequency(&frequency);
        render_us.store(unsigned((finish.QuadPart-start.QuadPart)*1000000/frequency.QuadPart));
    }
    unlock();
}
HRESULT STDMETHODCALLTYPE hooked_present(IDXGISwapChain* chain,UINT interval,UINT flags) {
    call_guard guard;
    if(present_depth) {
        rejected_recursion.fetch_add(1, std::memory_order_relaxed);
        return S_OK;
    }
    struct depth_guard { depth_guard(){++present_depth;} ~depth_guard(){--present_depth;} } depth_scope;
    if(enabled.load(std::memory_order_acquire)&&!(flags&DXGI_PRESENT_TEST)) {
        DXGI_SWAP_CHAIN_DESC desc{};
        const bool is_game=SUCCEEDED(chain->GetDesc(&desc))&&desc.OutputWindow==game_window;
        if(is_game) {
            sample_present_interval();
            hooked_game_chain=chain;object_attached.store(true,std::memory_order_release);
            presents.fetch_add(1,std::memory_order_relaxed);
            // This draws into Dota's own backbuffer, like the native menu.
            // Focus controls input, not presentation: an occluded game still
            // needs complete frames for window previews/capture and Alt+Tab.
            if(IsWindowVisible(game_window)&&!IsIconic(game_window))render(chain);
            else {visible_count.store(0);alt_alpha=0;alt_clock=0;}
            // Native UI remains part of Dota's frame when another application
            // covers the game. It cannot escape onto the desktop as a HWND.
            if(const auto callback=menu_draw.load(std::memory_order_acquire))callback(chain);
        }
    }
    LARGE_INTEGER p_start{},p_finish{},p_freq{};
    if constexpr(ASKERDWAY_DIAGNOSTICS)QueryPerformanceCounter(&p_start);
    const HRESULT result=original_present?original_present(chain,interval,flags):S_OK;
    if constexpr(ASKERDWAY_DIAGNOSTICS) {
      QueryPerformanceCounter(&p_finish);
      QueryPerformanceFrequency(&p_freq);
      if(p_freq.QuadPart>0) {
        const unsigned elapsed=unsigned((p_finish.QuadPart-p_start.QuadPart)*1000000/p_freq.QuadPart);
        record_present_duration(elapsed);
      }
    }
    return result;
}
// Keep the COM dispatch table native. Steam can reconfigure its Present path
// during VSync changes; an object vtable clone lets it save our callback as its
// own downstream call and creates a cycle. The trampoline bypasses that cycle.
}
bool install(IDXGISwapChain* discovery,HWND game) {
    if(present_entry)return enabled.load();
    if(!discovery||!game||!prepare_shaders())return false;
    const auto initialized=MH_Initialize();
    if(initialized!=MH_OK&&initialized!=MH_ERROR_ALREADY_INITIALIZED)return false;
    present_entry=(*reinterpret_cast<void***>(discovery))[8];game_window=game;
    if(MH_CreateHook(present_entry,reinterpret_cast<void*>(&hooked_present),
            reinterpret_cast<void**>(&original_present))!=MH_OK) {
        present_entry=nullptr;MH_Uninitialize();return false;
    }
    enabled.store(true,std::memory_order_release);
    if(MH_EnableHook(present_entry)!=MH_OK) {
        enabled.store(false);MH_RemoveHook(present_entry);MH_Uninitialize();present_entry=nullptr;return false;
    }
    std::memcpy(present_patch.data(),present_entry,present_patch.size());
    return true;
}
bool shutdown() {
    interactive_window.store(nullptr,std::memory_order_release);
    enabled.store(false,std::memory_order_release);
    if(present_entry) {
        // Never overwrite a newer third-party detour or unload a trampoline
        // which another overlay could still have saved as its downstream call.
        if(std::memcmp(present_entry,present_patch.data(),present_patch.size())!=0)return false;
        if(MH_DisableHook(present_entry)!=MH_OK)return false;
    }
    const auto end=GetTickCount64()+2000;
    do {Sleep(10);}while(calls.load()&&GetTickCount64()<end);
    if(calls.load())return false;
    view_reader.store(nullptr,std::memory_order_release);
    if(present_entry) {
        if(MH_RemoveHook(present_entry)!=MH_OK)return false;
        MH_Uninitialize();present_entry=nullptr;original_present=nullptr;
    }
    hooked_game_chain=nullptr;object_attached.store(false);
    AcquireSRWLockExclusive(&gpu_lock);reset_gpu();ReleaseSRWLockExclusive(&gpu_lock);
    draw_bypassed.store(false,std::memory_order_release);
    return true;
}
void publish(const scene& value,const std::array<tile_pixels,capacity>& pixels) {
    AcquireSRWLockExclusive(&scene_lock);
    for(unsigned i=0;i<value.count;++i) if(pending.markers[i].version!=value.markers[i].version)pending_pixels[i]=pixels[i];
    pending=value;ReleaseSRWLockExclusive(&scene_lock);
}
void set_interactive_window(HWND menu){interactive_window.store(menu,std::memory_order_release);}
void set_menu_callbacks(menu_draw_fn draw,menu_resize_fn resize){
    menu_resize.store(resize,std::memory_order_release);
    menu_draw.store(draw,std::memory_order_release);
}
void set_world_view_reader(world_view_reader reader){view_reader.store(reader,std::memory_order_release);}
HWND active_menu_window(){return interactive_window.load(std::memory_order_acquire);}
const char* backend(){
#if defined(ASKERDWAY_RENDER_DEFERRED)
    return "deferred commands";
#elif defined(ASKERDWAY_RENDER_STREAMING)
    return "streaming atlas";
#else
    return "context state";
#endif
}
metrics status(){return {presents.load(),drawn.load(),visible_count.load(),render_us.load(),enabled.load(),
    rejected_recursion.load(),object_attached.load(),gpu_builds.load(),tile_uploads.load(),gpu_ready.load(std::memory_order_acquire),
    game_present_us.load(std::memory_order_relaxed),game_present_peak_us.load(std::memory_order_relaxed),
    draw_bypassed.load(std::memory_order_relaxed)};}
void set_draw_bypass(bool bypass){
    draw_bypassed.store(bypass,std::memory_order_relaxed);
    sample_remaining.store(0,std::memory_order_relaxed);
    sample_previous.store(0,std::memory_order_relaxed);sample_cursor.store(0,std::memory_order_relaxed);
    sample_remaining.store(2048,std::memory_order_relaxed);
}
bool is_draw_bypassed(){return draw_bypassed.load(std::memory_order_relaxed);}
frame_sample read_frame_sample(){
    frame_sample result{};std::array<unsigned,1024> values{};
    result.count=std::min<unsigned>(sample_cursor.load(std::memory_order_relaxed),unsigned(values.size()));
    for(unsigned i=0;i<result.count;++i)values[i]=sampled_intervals[i].load(std::memory_order_relaxed);
    if(result.count){
        std::sort(values.begin(),values.begin()+result.count);
        result.p50_us=values[(result.count-1)*50/100];result.p95_us=values[(result.count-1)*95/100];
        result.p99_us=values[(result.count-1)*99/100];result.maximum_us=values[result.count-1];
    }
    return result;
}
}
