#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "psprecomp/guest_memory.hpp"
#include "png_writer.hpp"
#include "background_window.hpp"
#include <vector>
#include <stdexcept>
#include <filesystem>
#include <fstream>
#include <array>
namespace chili {
// PSP packed little-endian RGBA becomes the BGRA byte order expected by a DIB.
inline bool decode_display_frame(const psprecomp::GuestMemory &memory,unsigned address,unsigned stride,unsigned format,std::vector<unsigned> &out){
    if(!address || stride<480 || format>3)return false;
    const unsigned bpp=format==3?4:2;
    if(!memory.contains(address,(271u*stride+480u)*bpp))return false;
    out.resize(480u*272u);
    auto expand=[](unsigned x,unsigned bits){return bits==5?((x<<3)|(x>>2)):bits==6?((x<<2)|(x>>4)):((x<<4)|x);};
    for(unsigned y=0;y<272;++y)for(unsigned x=0;x<480;++x){
        const auto p=address+(y*stride+x)*bpp;
        unsigned r,g,b;
        if(format==3){const auto c=memory.load32(p);r=c&255;g=(c>>8)&255;b=(c>>16)&255;}
        else{const auto c=memory.load16(p);
            if(format==0){r=expand(c&31,5);g=expand((c>>5)&63,6);b=expand(c>>11,5);}
            else if(format==1){r=expand(c&31,5);g=expand((c>>5)&31,5);b=expand((c>>10)&31,5);}
            else{r=expand(c&15,4);g=expand((c>>4)&15,4);b=expand((c>>8)&15,4);}
        }
        out[y*480+x]=0xFF000000u|(r<<16)|(g<<8)|b;
    }
    return true;
}
// Writes a decoded 480x272 frame. A ".png" destination gets a PNG; anything
// else gets the 32-bit top-down BMP this port has always written.
inline bool save_display_pixels(const std::filesystem::path &path,const std::vector<unsigned> &pixels){
    if(pixels.size()!=480u*272u)return false;
    auto extension=path.extension().wstring();
    for(auto &c:extension)if(c>=L'A' && c<=L'Z')c=static_cast<wchar_t>(c-L'A'+L'a');
    if(extension==L".png")return write_png(path,480u,272u,pixels);
    std::array<unsigned char,54> header{};
    auto word=[&](unsigned offset,unsigned value){for(unsigned byte=0;byte<4;++byte)header[offset+byte]=static_cast<unsigned char>(value>>(byte*8));};
    header[0]='B';header[1]='M';word(2,54u+480u*272u*4u);word(10,54);word(14,40);
    word(18,480);word(22,static_cast<unsigned>(-272));header[26]=1;header[28]=32;word(34,480u*272u*4u);
    std::ofstream file(path,std::ios::binary|std::ios::trunc);
    file.write(reinterpret_cast<const char*>(header.data()),header.size());
    file.write(reinterpret_cast<const char*>(pixels.data()),pixels.size()*sizeof(unsigned));
    file.close();return !file.fail();
}
// Decodes the given guest framebuffer and writes it, without going through a window.
inline bool save_display_frame(const psprecomp::GuestMemory &memory,unsigned address,unsigned stride,unsigned format,const std::filesystem::path &path){
    std::vector<unsigned> pixels;
    return decode_display_frame(memory,address,stride,format,pixels) && save_display_pixels(path,pixels);
}
class PcWindow {
public:
    // Optional input layer. `message` sees every window message first and returns
    // true (with the result) when it handled one; `created` runs once per window.
    struct InputHook {bool (*message)(HWND,UINT,WPARAM,LPARAM,LRESULT &);void (*created)(HWND);};
    static void set_input_hook(InputHook hook) noexcept {input_hook_=hook;}
    // A listener ahead of the input layer, for the few messages something else owns (a key no binding has, a
    // click on something the host draws); true with the result when it took one.
    using FirstHook = bool (*)(HWND,UINT,WPARAM,LPARAM,LRESULT &);
    static void set_first_hook(FirstHook hook) noexcept {first_hook_=hook;}
private:
    inline static InputHook input_hook_{};
    inline static FirstHook first_hook_{};
    HWND hwnd_{};std::vector<unsigned> frame_;bool closed_{},external_paint_{},fullscreen_{};
    LONG_PTR windowed_style_{};WINDOWPLACEMENT windowed_place_{};
    static LRESULT CALLBACK procedure(HWND hwnd,UINT message,WPARAM w,LPARAM l){
        auto *self=reinterpret_cast<PcWindow*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(message==WM_NCCREATE){self=static_cast<PcWindow*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
        // F11 or Alt+Enter: borderless full screen and back. Bit 30 is set on auto-repeat.
        if(self && ((message==WM_KEYDOWN && w==VK_F11) || (message==WM_SYSKEYDOWN && w==VK_RETURN)) ){
            if(!(l&(LPARAM(1)<<30)))self->set_fullscreen(!self->fullscreen_);
            return 0;
        }
        if(self && message==WM_SYSCHAR && w==VK_RETURN)return 0;
        if(self && first_hook_){LRESULT result{};if(first_hook_(hwnd,message,w,l,result))return result;}
        if(self && input_hook_.message){LRESULT result{};if(input_hook_.message(hwnd,message,w,l,result))return result;}
        if(self && message==WM_CLOSE){self->closed_=true;DestroyWindow(hwnd);return 0;}
        if(self && message==WM_DESTROY){self->hwnd_=nullptr;return 0;}
        // A swapchain owns the client area: validate it and paint nothing over it.
        if(self && message==WM_PAINT && self->external_paint_){PAINTSTRUCT paint;BeginPaint(hwnd,&paint);EndPaint(hwnd,&paint);return 0;}
        if(self && message==WM_PAINT){PAINTSTRUCT paint;HDC dc=BeginPaint(hwnd,&paint);RECT area;GetClientRect(hwnd,&area);
            FillRect(dc,&area,static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
            if(!self->frame_.empty()){
                const int width=area.right,height=area.bottom;
                int dw=width,dh=width*272/480;if(dh>height){dh=height;dw=height*480/272;}
                BITMAPINFO info{};info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);info.bmiHeader.biWidth=480;info.bmiHeader.biHeight=-272;
                info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
                SetStretchBltMode(dc,COLORONCOLOR);
                StretchDIBits(dc,(width-dw)/2,(height-dh)/2,dw,dh,0,0,480,272,self->frame_.data(),&info,DIB_RGB_COLORS,SRCCOPY);
            }
            EndPaint(hwnd,&paint);return 0;
        }
        return DefWindowProcW(hwnd,message,w,l);
    }
public:
    explicit PcWindow(bool visible=true){
        const auto instance=GetModuleHandleW(nullptr);WNDCLASSW wc{};wc.lpfnWndProc=&procedure;wc.hInstance=instance;wc.lpszClassName=L"ChiliNativeDisplay";wc.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));wc.hIcon=LoadIconW(instance,MAKEINTRESOURCEW(1));
        if(!RegisterClassW(&wc) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)throw std::runtime_error("Cannot register Chili native window");
        // The largest whole multiple of the PSP's 480x272 that leaves a fifth of the desktop free, at least
        // 960x544 where that fits: 960x544 on a 1366x768 desktop, 1440x816 at 1080p, 2880x1632 at 2160p. The
        // process is DPI aware, so these are real pixels. A scripted run keeps 960x544 at 100% scaling.
        const UINT dpi=GetDpiForSystem();
        int multiple=2;
        RECT work{};
        if(!background_windows && SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0)){
            const int wide=static_cast<int>((work.right-work.left)*4/5/480),high=static_cast<int>((work.bottom-work.top)*4/5/272);
            multiple=std::clamp(std::min(wide,high),2,8);
            if(960>work.right-work.left || 544+48>work.bottom-work.top)multiple=1;
        }
        RECT size{0,0,480*multiple,272*multiple};
        if(background_windows)size={0,0,MulDiv(960,static_cast<int>(dpi),96),MulDiv(544,static_cast<int>(dpi),96)};
        AdjustWindowRectExForDpi(&size,WS_OVERLAPPEDWINDOW,FALSE,0,dpi);
        hwnd_=CreateWindowExW(0,wc.lpszClassName,L"Chili Con Carnage - Native PC",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,size.right-size.left,size.bottom-size.top,nullptr,nullptr,instance,this);
        if(!hwnd_)throw std::runtime_error("Cannot create Chili native window");
        if(input_hook_.created)input_hook_.created(hwnd_);
        // A scripted run's window goes behind the others and is not activated (background_window.hpp).
        if(visible && background_windows)SetWindowPos(hwnd_,HWND_BOTTOM,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_SHOWWINDOW);
        else if(visible)ShowWindow(hwnd_,SW_SHOW);
    }
    ~PcWindow(){if(hwnd_)DestroyWindow(hwnd_);}
    bool pump(){MSG message;while(PeekMessageW(&message,nullptr,0,0,PM_REMOVE)){TranslateMessage(&message);DispatchMessageW(&message);}return !closed_;}
    bool present(const psprecomp::GuestMemory &m,unsigned address,unsigned stride,unsigned format){
        if(!hwnd_ || !decode_display_frame(m,address,stride,format,frame_))return false;
        InvalidateRect(hwnd_,nullptr,FALSE);UpdateWindow(hwnd_);return true;
    }
    bool save_frame(const std::filesystem::path &path) const {return save_display_pixels(path,frame_);}
    // Borderless over the whole monitor the window is on, or back to the window it was.
    void set_fullscreen(bool on){
        if(!hwnd_ || on==fullscreen_)return;
        if(on){
            MONITORINFO monitor{};monitor.cbSize=sizeof(monitor);
            windowed_place_.length=sizeof(windowed_place_);
            if(!GetMonitorInfoW(MonitorFromWindow(hwnd_,MONITOR_DEFAULTTONEAREST),&monitor) || !GetWindowPlacement(hwnd_,&windowed_place_))return;
            windowed_style_=GetWindowLongPtrW(hwnd_,GWL_STYLE);
            SetWindowLongPtrW(hwnd_,GWL_STYLE,windowed_style_&~static_cast<LONG_PTR>(WS_OVERLAPPEDWINDOW));
            // A scripted run's window stays behind the others and is not activated, in full screen too.
            SetWindowPos(hwnd_,background_windows?HWND_BOTTOM:HWND_TOP,monitor.rcMonitor.left,monitor.rcMonitor.top,monitor.rcMonitor.right-monitor.rcMonitor.left,
                monitor.rcMonitor.bottom-monitor.rcMonitor.top,SWP_NOOWNERZORDER|SWP_FRAMECHANGED|(background_windows?SWP_NOACTIVATE:0u));
        }else{
            SetWindowLongPtrW(hwnd_,GWL_STYLE,windowed_style_);
            if(background_windows)windowed_place_.showCmd=SW_SHOWNOACTIVATE;
            SetWindowPlacement(hwnd_,&windowed_place_);
            SetWindowPos(hwnd_,nullptr,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOZORDER|SWP_NOOWNERZORDER|SWP_FRAMECHANGED|(background_windows?SWP_NOACTIVATE:0u));
        }
        fullscreen_=on;
        InvalidateRect(hwnd_,nullptr,TRUE);
    }
    bool fullscreen() const noexcept {return fullscreen_;}
    // True while something else (the D3D12 swapchain) draws the client area.
    void set_external_paint(bool on) noexcept {external_paint_=on;}
    HWND handle() const{return hwnd_;}
};
}
