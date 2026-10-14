#ifndef UNICODE
#define UNICODE
#endif
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#define _UNICODE
#define NOMINMAX
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shlobj.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wbemidl.h>
#include <dxgi1_2.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include <array>
#include <deque>
#include "cpu_sensors.h"

// Minimal public D3DKMT ABI; dynamically resolved from the system GDI32.
struct OpenAdapter { LUID luid; UINT handle; };
struct CloseAdapter { UINT handle; };
struct QueryAdapter { UINT handle; UINT type; void* data; UINT size; };
struct AdapterPerf {
    UINT physicalIndex; alignas(8) UINT64 memoryFrequency, maxMemoryFrequency,
        maxMemoryFrequencyOC, memoryBandwidth, pcieBandwidth;
    ULONG fanRPM, power, temperature; UCHAR powerState;
};
static_assert(sizeof(AdapterPerf) == 64 && offsetof(AdapterPerf, temperature) == 56);
using OpenFn = LONG (WINAPI*)(OpenAdapter*);
using CloseFn = LONG (WINAPI*)(const CloseAdapter*);
using QueryFn = LONG (WINAPI*)(const QueryAdapter*);
constexpr UINT WM_SAMPLE = WM_APP + 1;
constexpr UINT WM_TRAY = WM_APP + 2, WM_RESTORE_OVERLAY = WM_APP + 3;
NOTIFYICONDATAW tray{};
bool trayVisible=false;
HICON appIcon=nullptr;
UINT taskbarCreated=0;
struct Settings { int x=80,y=80,opacity=85,mask=15; COLORREF color=RGB(22,26,34); std::wstring sources[4]; } cfg;
struct Choice { std::wstring id, name; int slot; double value; };
struct Sample { std::vector<Choice> choices; std::wstring cpuSource=L"Libre Hardware Monitor: максимум CPU"; std::wstring gpuSource=L"Windows: максимум среди GPU"; double cpu=-1,gpu=-1,cpuTemp=-1,gpuTemp=-1; ULONGLONG tick=0; } latest;
struct HistoryPoint { ULONGLONG tick; std::array<double,4> values; };
std::deque<HistoryPoint> history;
constexpr ULONGLONG HistoryDuration=180000;
std::array<double,4> SelectedValues(const Sample& s) {
    std::array<double,4> values{s.cpu,s.cpuTemp,s.gpu,s.gpuTemp};
    for(int i=0;i<4;i++) if(!cfg.sources[i].empty()) {
        values[i]=-1;
        for(const auto& c:s.choices) if(c.slot==i && c.id==cfg.sources[i]) { values[i]=c.value; break; }
    }
    return values;
}
std::mutex sampleMutex;
std::wstring settingsPath;
HWND mainWindow=nullptr;
HANDLE stopEvent=nullptr;
UINT dpi=96;
HANDLE cpuSetupProcess=nullptr;
HWND cpuSetupWindow=nullptr,cpuSetupLabel=nullptr;
ULONGLONG cpuSetupDeadline=0;
bool cpuSetupLaunching=false;
bool CpuSetupActive() { return cpuSetupLaunching || cpuSetupProcess || cpuSetupDeadline; }
bool saveFailed=false;
bool menuHover=false, minimizeHover=false;
int px(int v) { return MulDiv(v,static_cast<int>(dpi),96); }
int rowCount() { int n=0; for(int i=0;i<4;i++) if(cfg.mask&(1<<i)) ++n; return n; }
int cardRows(int group) { return ((cfg.mask>>(group*2))&1)+((cfg.mask>>(group*2+1))&1); }
int height() { return px(176+std::max(cardRows(0),cardRows(1))*26); }

void LoadSettings() {
    PWSTR folder=nullptr;
    if(SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&folder))) {
        std::wstring dir=std::wstring(folder)+L"\\ThermalOverlay"; CoTaskMemFree(folder);
        CreateDirectoryW(dir.c_str(),nullptr); settingsPath=dir+L"\\settings.ini";
    }
    if(settingsPath.empty()) return;
    auto get=[](const wchar_t* key,int def) { return static_cast<int>(GetPrivateProfileIntW(L"Window",key,def,settingsPath.c_str())); };
    cfg.x=std::clamp(get(L"X",80),-100000,100000); cfg.y=std::clamp(get(L"Y",80),-100000,100000);
    cfg.opacity=std::clamp(get(L"Opacity",85),20,100);
    cfg.mask=get(L"Sensors",15)&15; if(!(cfg.mask&3)) cfg.mask|=3; if(!(cfg.mask&12)) cfg.mask|=12;
    cfg.color=static_cast<COLORREF>(get(L"Color",RGB(22,26,34)))&0xffffff;
    for(int i=0;i<4;i++) {
        wchar_t value[512]{};
        GetPrivateProfileStringW(L"Sources",std::to_wstring(i).c_str(),L"",value,512,settingsPath.c_str());
        cfg.sources[i]=value;
    }
}
void SaveSettings() {
    if(settingsPath.empty()) { saveFailed=true; return; }
    RECT r{}; if(mainWindow && GetWindowRect(mainWindow,&r)) { cfg.x=r.left; cfg.y=r.top; }
    std::wstring s=L"[Window]\r\nX="+std::to_wstring(cfg.x)+L"\r\nY="+std::to_wstring(cfg.y)
        +L"\r\nOpacity="+std::to_wstring(cfg.opacity)+L"\r\nSensors="+std::to_wstring(cfg.mask)
        +L"\r\nColor="+std::to_wstring(cfg.color)+L"\r\n";
    s+=L"[Sources]\r\n";
    for(int i=0;i<4;i++) s+=std::to_wstring(i)+L"="+cfg.sources[i]+L"\r\n";
    std::wstring tmp=settingsPath+L".tmp";
    HANDLE f=CreateFileW(tmp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    saveFailed=true;
    if(f!=INVALID_HANDLE_VALUE) {
        WORD bom=0xfeff; DWORD wrote=0;
        bool ok=WriteFile(f,&bom,sizeof(bom),&wrote,nullptr) && wrote==sizeof(bom);
        DWORD bytes=static_cast<DWORD>(s.size()*sizeof(wchar_t));
        ok=ok && WriteFile(f,s.data(),bytes,&wrote,nullptr) && wrote==bytes;
        ok=ok && FlushFileBuffers(f); CloseHandle(f);
        if(ok) saveFailed=!MoveFileExW(tmp.c_str(),settingsPath.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH);
    }
}
RECT VisibleRect(int x,int y,int w,int h) {
    RECT r{x,y,x+w,y+h}; MONITORINFO m{}; m.cbSize=sizeof(m);
    if(GetMonitorInfoW(MonitorFromRect(&r,MONITOR_DEFAULTTONEAREST),&m)) {
        x=std::clamp<LONG>(x,m.rcWork.left,std::max(m.rcWork.left,m.rcWork.right-w));
        y=std::clamp<LONG>(y,m.rcWork.top,std::max(m.rcWork.top,m.rcWork.bottom-h));
    }
    return {x,y,x+w,y+h};
}
void Layout() {
    RECT r{}; GetWindowRect(mainWindow,&r); r=VisibleRect(r.left,r.top,px(420),height());
    SetWindowPos(mainWindow,HWND_TOPMOST,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOACTIVATE);
    SetLayeredWindowAttributes(mainWindow,0,static_cast<BYTE>(cfg.opacity*255/100),LWA_ALPHA);
    HRGN shape=CreateRoundRectRgn(0,0,px(420)+1,height()+1,px(32),px(32));
    if(shape && !SetWindowRgn(mainWindow,shape,TRUE)) DeleteObject(shape);
    InvalidateRect(mainWindow,nullptr,FALSE);
}
UINT64 FileTimeValue(FILETIME t) { return (static_cast<UINT64>(t.dwHighDateTime)<<32)|t.dwLowDateTime; }

class NvidiaSensors {
    HMODULE module=nullptr;
    using Init=int (*)(); using Count=int (*)(unsigned*); using Handle=int (*)(unsigned,void**);
    struct Utilization { unsigned gpu, memory; };
    using Usage=int (*)(void*,Utilization*); using Temperature=int (*)(void*,unsigned,unsigned*);
    using Name=int (*)(void*,char*,unsigned);
    Init shutdown=nullptr; Count count=nullptr; Handle handle=nullptr;
    Usage usage=nullptr; Temperature temperature=nullptr; Name name=nullptr;
    bool ready=false;
public:
    NvidiaSensors() {
        module=LoadLibraryExW(L"nvml.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!module) return;
        auto init=reinterpret_cast<Init>(GetProcAddress(module,"nvmlInit_v2"));
        shutdown=reinterpret_cast<Init>(GetProcAddress(module,"nvmlShutdown"));
        count=reinterpret_cast<Count>(GetProcAddress(module,"nvmlDeviceGetCount_v2"));
        handle=reinterpret_cast<Handle>(GetProcAddress(module,"nvmlDeviceGetHandleByIndex_v2"));
        usage=reinterpret_cast<Usage>(GetProcAddress(module,"nvmlDeviceGetUtilizationRates"));
        temperature=reinterpret_cast<Temperature>(GetProcAddress(module,"nvmlDeviceGetTemperature"));
        name=reinterpret_cast<Name>(GetProcAddress(module,"nvmlDeviceGetName"));
        ready=init && shutdown && count && handle && usage && temperature && name && init()==0;
    }
    ~NvidiaSensors() { if(ready) shutdown(); if(module) FreeLibrary(module); }
    void Read(Sample& s) {
        if(!ready) return;
        unsigned devices=0; if(count(&devices)!=0) return;
        for(unsigned i=0;i<std::min(devices,64u);i++) {
            void* device=nullptr; if(handle(i,&device)!=0) continue;
            char label[128]{}; name(device,label,sizeof(label));
            wchar_t wide[128]{}; MultiByteToWideChar(CP_UTF8,0,label,-1,wide,128);
            std::wstring title=wide; if(title.empty()) title=L"NVIDIA GPU "+std::to_wstring(i);
            Utilization u{}; unsigned t=0;
            double load=usage(device,&u)==0 && u.gpu<=100?static_cast<double>(u.gpu):-1;
            double temp=temperature(device,0,&t)==0 && t>0 && t<150?static_cast<double>(t):-1;
            std::wstring id=L"nvml/gpu/"+std::to_wstring(i);
            s.choices.push_back({id+L"/load",title+L" · NVML",2,load});
            s.choices.push_back({id+L"/temperature",title+L" · NVML",3,temp});
        }
    }
};

class Sensors {
    NvidiaSensors nvidia;
    PDH_HQUERY query=nullptr; PDH_HCOUNTER counter=nullptr;
    UINT64 oldIdle=0,oldKernel=0,oldUser=0;
    IWbemServices* wmi=nullptr;
    HMODULE gdi=nullptr;
    OpenFn open=nullptr; CloseFn close=nullptr; QueryFn info=nullptr;
    int cycle=0;
public:
    Sensors() {
        if(PdhOpenQueryW(nullptr,0,&query)==ERROR_SUCCESS) {
            if(PdhAddEnglishCounterW(query,L"\\GPU Engine(*)\\Utilization Percentage",0,&counter)!=ERROR_SUCCESS) counter=nullptr;
            PdhCollectQueryData(query);
        }
        gdi=LoadLibraryExW(L"gdi32.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(gdi) {
            open=reinterpret_cast<OpenFn>(GetProcAddress(gdi,"D3DKMTOpenAdapterFromLuid"));
            close=reinterpret_cast<CloseFn>(GetProcAddress(gdi,"D3DKMTCloseAdapter"));
            info=reinterpret_cast<QueryFn>(GetProcAddress(gdi,"D3DKMTQueryAdapterInfo"));
        }
    }
    ~Sensors() { if(wmi) wmi->Release(); if(query) PdhCloseQuery(query); if(gdi) FreeLibrary(gdi); }
    double Cpu() {
        FILETIME idle{},kernel{},user{};
        if(!GetSystemTimes(&idle,&kernel,&user)) return -1;
        auto i=FileTimeValue(idle), k=FileTimeValue(kernel), u=FileTimeValue(user);
        double value=-1;
        if(oldKernel && k>=oldKernel && u>=oldUser && i>=oldIdle) {
            auto total=(k-oldKernel)+(u-oldUser), rest=i-oldIdle;
            if(total>0 && rest<=total) value=100.0*static_cast<double>(total-rest)/static_cast<double>(total);
        }
        oldIdle=i; oldKernel=k; oldUser=u; return value;
    }
    double Gpu(std::vector<Choice>& choices) {
        if(!query || !counter || PdhCollectQueryData(query)!=ERROR_SUCCESS) return -1;
        DWORD bytes=0,count=0;
        if(PdhGetFormattedCounterArrayW(counter,PDH_FMT_DOUBLE,&bytes,&count,nullptr)!=static_cast<PDH_STATUS>(PDH_MORE_DATA)) return -1;
        std::vector<BYTE> data(bytes);
        auto items=reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(data.data());
        if(PdhGetFormattedCounterArrayW(counter,PDH_FMT_DOUBLE,&bytes,&count,items)!=ERROR_SUCCESS) return -1;
        std::map<std::wstring,double> engines;
        for(DWORD n=0;n<count;n++) {
            auto& v=items[n].FmtValue;
            if(v.CStatus!=PDH_CSTATUS_VALID_DATA && v.CStatus!=PDH_CSTATUS_NEW_DATA) continue;
            std::wstring name=items[n].szName;
            auto pos=name.find(L"luid_"); if(pos==std::wstring::npos || !std::isfinite(v.doubleValue)) continue;
            // Sum processes for the same physical engine, then take the busiest engine.
            std::wstring engine=name.substr(pos);
            auto duplicate=engine.find(L'#'); if(duplicate!=std::wstring::npos) engine.resize(duplicate);
            engines[engine]+=std::max(0.0,v.doubleValue);
        }
        if(engines.empty()) return -1;
        double value=0;
        for(const auto& e:engines) {
            value=std::max(value,e.second);
            auto type=e.first.find(L"_engtype_");
            std::wstring title=type==std::wstring::npos?e.first:e.first.substr(type+9);
            choices.push_back({L"windows/engine/"+e.first,L"Windows: "+title,2,std::clamp(e.second,0.0,100.0)});
        }
        return std::clamp(value,0.0,100.0);
    }
    double GpuTemperature() {
        if(!open || !close || !info) return -1;
        IDXGIFactory1* factory=nullptr;
        if(FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1),reinterpret_cast<void**>(&factory)))) return -1;
        double value=-1; IDXGIAdapter1* adapter=nullptr;
        for(UINT n=0;factory->EnumAdapters1(n,&adapter)==S_OK;n++) {
            DXGI_ADAPTER_DESC1 desc{};
            if(SUCCEEDED(adapter->GetDesc1(&desc)) && !(desc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE)) {
                OpenAdapter a{desc.AdapterLuid,0};
                if(open(&a)>=0) {
                    UINT physicalCount=1; QueryAdapter countQ{a.handle,29,&physicalCount,sizeof(physicalCount)};
                    if(info(&countQ)<0 || physicalCount>64) physicalCount=1;
                    for(UINT p=0;p<physicalCount;p++) {
                        AdapterPerf perf{}; perf.physicalIndex=p;
                        QueryAdapter q{a.handle,62,&perf,sizeof(perf)};
                        if(info(&q)>=0 && perf.temperature>0 && perf.temperature<1500)
                            value=std::max(value,perf.temperature/10.0);
                    }
                    CloseAdapter c{a.handle}; close(&c);
                }
            }
            adapter->Release();
        }
        factory->Release(); return value;
    }
    double CpuTemperature(std::vector<Choice>& choices) {
        // Never mislabel an ACPI thermal-zone temperature as a CPU core temperature.
        if(!wmi && cycle++%15==0) {
            IWbemLocator* locator=nullptr;
            if(SUCCEEDED(CoCreateInstance(CLSID_WbemLocator,nullptr,CLSCTX_INPROC_SERVER,IID_IWbemLocator,reinterpret_cast<void**>(&locator)))) {
                BSTR ns=SysAllocString(L"ROOT\\LibreHardwareMonitor");
                HRESULT hr=locator->ConnectServer(ns,nullptr,nullptr,nullptr,WBEM_FLAG_CONNECT_USE_MAX_WAIT,nullptr,nullptr,&wmi);
                SysFreeString(ns); locator->Release();
                if(SUCCEEDED(hr)) {
                    hr=CoSetProxyBlanket(wmi,RPC_C_AUTHN_WINNT,RPC_C_AUTHZ_NONE,nullptr,RPC_C_AUTHN_LEVEL_CALL,RPC_C_IMP_LEVEL_IMPERSONATE,nullptr,EOAC_NONE);
                    if(FAILED(hr)) { wmi->Release(); wmi=nullptr; }
                }
            }
        }
        if(!wmi) return -1;
        BSTR lang=SysAllocString(L"WQL"), sql=SysAllocString(L"SELECT Identifier, Name, Value, SensorType FROM Sensor WHERE SensorType='Temperature' OR SensorType='Load'");
        IEnumWbemClassObject* items=nullptr;
        HRESULT hr=wmi->ExecQuery(lang,sql,WBEM_FLAG_FORWARD_ONLY|WBEM_FLAG_RETURN_IMMEDIATELY,nullptr,&items);
        SysFreeString(lang); SysFreeString(sql);
        if(FAILED(hr)) { wmi->Release(); wmi=nullptr; return -1; }
        double value=-1; auto deadline=GetTickCount64()+500;
        while(GetTickCount64()<deadline) {
            IWbemClassObject* obj=nullptr; ULONG count=0;
            hr=items->Next(100,1,&obj,&count);
            if(!count) { if(hr==WBEM_S_TIMEDOUT) continue; break; }
            VARIANT id{},v{},number{},label{},kind{};
            obj->Get(L"Identifier",0,&id,nullptr,nullptr); obj->Get(L"Value",0,&v,nullptr,nullptr);
            obj->Get(L"Name",0,&label,nullptr,nullptr); obj->Get(L"SensorType",0,&kind,nullptr,nullptr);
            if(id.vt==VT_BSTR && id.bstrVal) {
                std::wstring name=id.bstrVal;
                bool cpu=name.find(L"/intelcpu/")==0 || name.find(L"/amdcpu/")==0;
                bool gpu=name.find(L"/gpu-")==0;
                bool temp=kind.vt==VT_BSTR && kind.bstrVal && std::wstring(kind.bstrVal)==L"Temperature";
                if((cpu || gpu) && SUCCEEDED(VariantChangeType(&number,&v,0,VT_R8))) {
                    double t=number.dblVal;
                    if(std::isfinite(t) && (temp?(t>0 && t<150):(t>=0 && t<=100)) && name.find_first_of(L"\r\n=")==std::wstring::npos) {
                        if(cpu && temp) value=std::max(value,t);
                        std::wstring title=(label.vt==VT_BSTR && label.bstrVal)?label.bstrVal:name;
                        if(choices.size()<512) choices.push_back({name,title,(gpu?2:0)+(temp?1:0),t});
                    }
                }
            }
            VariantClear(&label); VariantClear(&kind);
            VariantClear(&number); VariantClear(&id); VariantClear(&v); obj->Release();
        }
        items->Release(); return value;
    }
    Sample Read() { Sample s; s.cpu=Cpu(); s.gpu=Gpu(s.choices); s.gpuTemp=GpuTemperature(); s.cpuTemp=CpuTemperature(s.choices);
        cpu::Snapshot cpuSnapshot;
        if(cpu::ReadShared(cpuSnapshot) && cpuSnapshot.status==0 && cpuSnapshot.count) {
            s.cpuTemp=cpuSnapshot.sensors[0].temperature;
            cpuSnapshot.sensors[0].name[79]=0; s.cpuSource=cpuSnapshot.sensors[0].name;
            for(unsigned i=0;i<cpuSnapshot.count;i++) {
                auto& reading=cpuSnapshot.sensors[i]; reading.id[63]=0; reading.name[79]=0;
                if(std::isfinite(reading.temperature) && reading.temperature>0 && reading.temperature<150)
                    s.choices.push_back({L"native/"+std::wstring(reading.id),std::wstring(reading.name)+L" · PawnIO",1,reading.temperature});
            }
        }
        s.choices.push_back({L"windows/gpu/load",L"Windows: самый загруженный движок среди GPU",2,s.gpu});
        s.choices.push_back({L"windows/gpu/temperature",L"Windows: максимум температуры среди GPU",3,s.gpuTemp});
        nvidia.Read(s); s.tick=GetTickCount64(); return s; }
};
DWORD WINAPI Worker(void*) {
    HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    {
        Sensors sensors;
        ULONGLONG next=GetTickCount64();
        do {
            next+=1000;
            Sample s=sensors.Read();
            { std::lock_guard<std::mutex> lock(sampleMutex); latest=s; }
            PostMessageW(mainWindow,WM_SAMPLE,0,0);
            auto now=GetTickCount64();
            if(next<now) next=now;
        } while(WaitForSingleObject(stopEvent,static_cast<DWORD>(std::clamp<LONGLONG>(static_cast<LONGLONG>(next)-static_cast<LONGLONG>(GetTickCount64()),0,1000)))==WAIT_TIMEOUT);
    }
    if(SUCCEEDED(com)) CoUninitialize(); return 0;
}
std::wstring Value(double v,bool temperature) {
    if(v<0 || !std::isfinite(v)) return L"--";
    std::wostringstream out; out<<std::fixed<<std::setprecision(0)<<v<<(temperature?L" °C":L" %"); return out.str();
}
void DrawThermometer(HDC dc,int x,int y,int size,COLORREF background,COLORREF outline,COLORREF mercury) {
    // A compact temperature instrument: flat gauge, scale and Celsius label.
    // No bulb silhouette; remains recognizable at notification-area sizes.
    auto at=[&](int v) { return MulDiv(v,size,32); };
    int saved=SaveDC(dc);
    HPEN border=CreatePen(PS_SOLID,std::max(1,at(1)),outline);
    HBRUSH face=CreateSolidBrush(background),liquid=CreateSolidBrush(mercury),track=CreateSolidBrush(outline);
    SelectObject(dc,border); SelectObject(dc,face);
    RoundRect(dc,x+at(2),y+at(1),x+at(30),y+at(31),at(7),at(7));
    RECT rail{x+at(7),y+at(7),x+at(11),y+at(26)};
    FillRect(dc,&rail,track);
    RECT level{x+at(7),y+at(16),x+at(11),y+at(26)};
    FillRect(dc,&level,liquid);
    for(int i=0;i<3;i++) {
        int yy=y+at(16+i*5);
        MoveToEx(dc,x+at(14),yy,nullptr); LineTo(dc,x+at(i==1?19:22),yy);
    }
    HFONT unit=CreateFontW(-std::max(7,at(12)),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,ANTIALIASED_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    SelectObject(dc,unit); SetBkMode(dc,TRANSPARENT); SetTextColor(dc,mercury);
    RECT label{x+at(13),y+at(3),x+at(28),y+at(15)};
    DrawTextW(dc,L"°C",-1,&label,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    RestoreDC(dc,saved);
    DeleteObject(unit); DeleteObject(border); DeleteObject(face); DeleteObject(liquid); DeleteObject(track);
}
HICON CreateAppIcon() {
    HDC screen=GetDC(nullptr),dc=CreateCompatibleDC(screen);
    HBITMAP color=CreateCompatibleBitmap(screen,32,32),mask=CreateBitmap(32,32,1,1,nullptr);
    HGDIOBJ old=SelectObject(dc,color);
    RECT rect{0,0,32,32}; HBRUSH bg=CreateSolidBrush(RGB(22,26,34)); FillRect(dc,&rect,bg); DeleteObject(bg);
    DrawThermometer(dc,0,0,32,RGB(22,26,34),RGB(239,244,250),RGB(124,211,238));
    SelectObject(dc,mask); PatBlt(dc,0,0,32,32,BLACKNESS); SelectObject(dc,old);
    ICONINFO ii{}; ii.fIcon=TRUE; ii.hbmColor=color; ii.hbmMask=mask;
    HICON result=CreateIconIndirect(&ii); DeleteObject(color); DeleteObject(mask); DeleteDC(dc); ReleaseDC(nullptr,screen);
    return result;
}
void RemoveTray() {
    if(trayVisible) { Shell_NotifyIconW(NIM_DELETE,&tray); trayVisible=false; }
}
bool AddTray(HWND hwnd) {
    tray={}; tray.cbSize=sizeof(tray); tray.hWnd=hwnd; tray.uID=1;
    tray.uFlags=NIF_MESSAGE|NIF_ICON|NIF_TIP; tray.uCallbackMessage=WM_TRAY;
    tray.hIcon=appIcon?appIcon:LoadIconW(nullptr,IDI_APPLICATION);
    lstrcpynW(tray.szTip,L"Thermal — нажмите, чтобы показать",128);
    trayVisible=Shell_NotifyIconW(NIM_ADD,&tray)!=FALSE;
    return trayVisible;
}
void RestoreOverlay(HWND hwnd) {
    ShowWindow(hwnd,SW_SHOWNORMAL); Layout(); RemoveTray(); SetForegroundWindow(hwnd);
}
void MinimizeToTray(HWND hwnd) {
    SaveSettings();
    if(trayVisible || AddTray(hwnd)) ShowWindow(hwnd,SW_HIDE);
    else MessageBoxW(hwnd,L"Не удалось добавить значок в область уведомлений. Окно осталось открытым.",L"Thermal",MB_OK|MB_ICONINFORMATION);
}
void TrayMenu(HWND hwnd) {
    HMENU menu=CreatePopupMenu();
    AppendMenuW(menu,MF_STRING,601,L"Показать"); AppendMenuW(menu,MF_SEPARATOR,0,nullptr); AppendMenuW(menu,MF_STRING,602,L"Выход");
    POINT p{}; GetCursorPos(&p); SetForegroundWindow(hwnd);
    int id=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,hwnd,nullptr); DestroyMenu(menu);
    PostMessageW(hwnd,WM_NULL,0,0);
    if(id==601) RestoreOverlay(hwnd); else if(id==602) DestroyWindow(hwnd);
}
COLORREF Blend(COLORREF a,COLORREF b,int weight) {
    return RGB((GetRValue(a)*(100-weight)+GetRValue(b)*weight)/100,
        (GetGValue(a)*(100-weight)+GetGValue(b)*weight)/100,
        (GetBValue(a)*(100-weight)+GetBValue(b)*weight)/100);
}
bool HitMinimize(int x,int y) { return x>=px(378) && x<px(408) && y>=px(10) && y<px(38); }
bool HitMenu(int x,int y) { return x>=px(306) && x<px(370) && y>=px(10) && y<px(38); }
COLORREF SensorColor(int kind,COLORREF background) {
    bool light=(GetRValue(background)*299+GetGValue(background)*587+GetBValue(background)*114)>145000;
    return kind==1?(light?RGB(165,78,13):RGB(242,158,77)):(light?RGB(0,111,151):RGB(78,183,218));
}
void DrawHistory(HDC dc,int left,int top,int group,COLORREF foreground,COLORREF background) {
    int saved=SaveDC(dc);
    HFONT small=CreateFontW(-px(10),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    SelectObject(dc,small); SetBkMode(dc,TRANSPARENT);
    RECT label{px(left),px(top),px(left+169),px(top+15)};
    for(int j=0;j<2;j++) if(cfg.mask&(1<<(group*2+j))) {
        SetTextColor(dc,SensorColor(j,background));
        DrawTextW(dc,j?L"0–150 °C":L"0–100 %",-1,&label,(j?DT_RIGHT:DT_LEFT)|DT_SINGLELINE|DT_VCENTER);
    }
    int x=px(left),y=px(top+17),w=px(169),h=px(30);
    HPEN grid=CreatePen(PS_SOLID,1,Blend(background,foreground,18)); SelectObject(dc,grid);
    for(int i=0;i<=2;i++) { MoveToEx(dc,x,y+i*h/2,nullptr); LineTo(dc,x+w,y+i*h/2); }
    const ULONGLONG now=GetTickCount64();
    bool any=false;
    for(int j=0;j<2;j++) {
        int slot=group*2+j; if(!(cfg.mask&(1<<slot))) continue;
        double maximum=j?150.0:100.0;
        COLORREF color=SensorColor(j,background);
        HPEN line=CreatePen(PS_SOLID,std::max(1,px(1)),color); SelectObject(dc,line);
        ULONGLONG previous=0; bool connected=false;
        for(const auto& point:history) {
            double value=point.values[slot];
            if(now<point.tick || now-point.tick>HistoryDuration || value<0 || !std::isfinite(value)) { connected=false; continue; }
            int xx=x+w-1-static_cast<int>((now-point.tick)*static_cast<ULONGLONG>(w-1)/HistoryDuration);
            int yy=y+h-1-static_cast<int>(std::clamp(value/maximum,0.0,1.0)*(h-1));
            if(connected && point.tick-previous<=2500) LineTo(dc,xx,yy);
            else { MoveToEx(dc,xx,yy,nullptr); SetPixelV(dc,xx,yy,color); }
            previous=point.tick; connected=true; any=true;
        }
        SelectObject(dc,grid); DeleteObject(line);
    }
    if(!any) {
        RECT empty{x,y,x+w,y+h}; SetTextColor(dc,foreground);
        DrawTextW(dc,L"Нет данных",-1,&empty,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
    }
    RestoreDC(dc,saved); DeleteObject(small); DeleteObject(grid);
}
void Paint(HWND hwnd) {
    PAINTSTRUCT ps{}; HDC target=BeginPaint(hwnd,&ps); RECT r{}; GetClientRect(hwnd,&r);
    HDC dc=CreateCompatibleDC(target); HBITMAP bitmap=CreateCompatibleBitmap(target,r.right,r.bottom);
    HGDIOBJ oldBitmap=SelectObject(dc,bitmap); HBRUSH brush=CreateSolidBrush(cfg.color); FillRect(dc,&r,brush); DeleteObject(brush);
    bool light=(GetRValue(cfg.color)*299+GetGValue(cfg.color)*587+GetBValue(cfg.color)*114)>145000;
    COLORREF fg=light?RGB(20,25,33):RGB(239,244,250), muted=light?RGB(55,65,78):RGB(164,180,199);
    SetBkMode(dc,TRANSPARENT);
    HFONT font=CreateFontW(-px(15),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    HGDIOBJ oldFont=SelectObject(dc,font);
    auto text=[&](const std::wstring& s,int x,int y,int w,UINT flags,COLORREF color) {
        RECT rr{px(x),px(y),px(x+w),px(y+26)}; SetTextColor(dc,color); DrawTextW(dc,s.c_str(),-1,&rr,DT_SINGLELINE|DT_VCENTER|flags);
    };
    COLORREF accent=light?RGB(20,103,140):RGB(124,211,238);
    auto rounded=[&](int x,int y,int w,int h,int radius,COLORREF fill,COLORREF outline) {
        HBRUSH b=CreateSolidBrush(fill); HPEN p=CreatePen(PS_SOLID,std::max(1,px(1)),outline);
        HGDIOBJ ob=SelectObject(dc,b),op=SelectObject(dc,p);
        RoundRect(dc,px(x),px(y),px(x+w),px(y+h),px(radius),px(radius));
        SelectObject(dc,ob); SelectObject(dc,op); DeleteObject(b); DeleteObject(p);
    };
    rounded(0,0,419,height()*96/static_cast<int>(dpi)-1,32,cfg.color,Blend(cfg.color,fg,22));
    rounded(12,10,132,28,12,Blend(cfg.color,accent,12),Blend(cfg.color,accent,28));
    HFONT brand=CreateFontW(-px(13),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    SelectObject(dc,brand);
    DrawThermometer(dc,px(22),px(13),px(21),Blend(cfg.color,accent,12),fg,accent);
    text(L"THERMAL",48,11,90,DT_LEFT,fg);
    rounded(306,10,64,28,12,Blend(cfg.color,fg,menuHover?18:7),Blend(cfg.color,fg,menuHover?40:18));
    text(L"Меню",306,11,64,DT_CENTER,fg);
    rounded(378,10,30,28,12,Blend(cfg.color,fg,minimizeHover?18:7),Blend(cfg.color,fg,minimizeHover?40:18));
    HPEN minimizePen=CreatePen(PS_SOLID,std::max(2,px(2)),minimizeHover?fg:muted);
    HGDIOBJ oldMinimizePen=SelectObject(dc,minimizePen);
    MoveToEx(dc,px(387),px(26),nullptr); LineTo(dc,px(399),px(26));
    SelectObject(dc,oldMinimizePen); DeleteObject(minimizePen);
    if(CpuSetupActive()) text(L"Установка…",151,11,115,DT_LEFT,accent);
    else if(saveFailed) text(L"Не сохранено",151,11,115,DT_LEFT,accent);
    SelectObject(dc,font); DeleteObject(brand);
    Sample s; { std::lock_guard<std::mutex> lock(sampleMutex); s=latest; }
    if(GetTickCount64()-s.tick>5000) s=Sample{};
    auto values=SelectedValues(s);
    int top=50;
    HFONT titleFont=CreateFontW(-px(14),0,0,0,FW_SEMIBOLD,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
    HPEN pen=CreatePen(PS_SOLID,std::max(1,px(1)),Blend(cfg.color,fg,45));
    HBRUSH cardBrush=CreateSolidBrush(Blend(cfg.color,fg,4));
    HGDIOBJ oldPen=SelectObject(dc,pen), oldBrush=SelectObject(dc,cardBrush);
    for(int g=0;g<2;g++) {
        int rows=cardRows(g); if(!rows) continue;
        int cardHeight=114+std::max(cardRows(0),cardRows(1))*26;
        int left=10+g*205;
        RoundRect(dc,px(left),px(top),px(left+195),px(top+cardHeight),px(18),px(18));
        SelectObject(dc,titleFont);
        text(g==0?L"CPU":L"GPU",left+13,top+3,190,DT_LEFT,fg);
        SelectObject(dc,font);
        int y=top+28;
        for(int j=0;j<2;j++) {
            int i=g*2+j; if(!(cfg.mask&(1<<i))) continue;
            text(j==0?L"Загрузка ▾":L"Температура ▾",left+13,y,116,DT_LEFT,SensorColor(j,cfg.color));
            text(Value(values[i],j==1),left+124,y,58,DT_RIGHT,fg);
            y+=26;
        }
        int graphTop=top+42+std::max(cardRows(0),cardRows(1))*26;
        DrawHistory(dc,left+13,graphTop,g,muted,cfg.color);
        HFONT timeline=CreateFontW(-px(10),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        SelectObject(dc,timeline); SetTextColor(dc,muted);
        RECT time{px(left+13),px(top+cardHeight-19),px(left+182),px(top+cardHeight-4)};
        DrawTextW(dc,L"−3 мин",-1,&time,DT_LEFT|DT_SINGLELINE|DT_VCENTER);
        DrawTextW(dc,L"сейчас",-1,&time,DT_RIGHT|DT_SINGLELINE|DT_VCENTER);
        SelectObject(dc,font); DeleteObject(timeline);
    }
    SelectObject(dc,oldPen); SelectObject(dc,oldBrush); DeleteObject(pen); DeleteObject(cardBrush); DeleteObject(titleFont);
    BitBlt(target,0,0,r.right,r.bottom,dc,0,0,SRCCOPY);
    SelectObject(dc,oldFont); DeleteObject(font); SelectObject(dc,oldBitmap); DeleteObject(bitmap); DeleteDC(dc); EndPaint(hwnd,&ps);
}
std::wstring StartupCommand() {
    std::vector<wchar_t> path(32768);
    DWORD length=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));
    if(!length || length>=path.size()) return L"";
    return L"\""+std::wstring(path.data(),length)+L"\"";
}
constexpr const wchar_t* StartupKey=L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
// Keep the existing Run value so Windows startup approval remains unchanged.
void MigrateAutostart() {
    wchar_t value[32768]{}; DWORD bytes=sizeof(value);
    std::wstring previous=L"\""+cpu::ExeDir()+L"\\ThermalOverlay.exe\"";
    std::wstring command=StartupCommand();
    if(command.empty() || RegGetValueW(HKEY_CURRENT_USER,StartupKey,L"ThermalOverlay",RRF_RT_REG_SZ,nullptr,value,&bytes)!=ERROR_SUCCESS || _wcsicmp(value,previous.c_str())!=0) return;
    HKEY key=nullptr;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,StartupKey,0,KEY_SET_VALUE,&key)==ERROR_SUCCESS) {
        RegSetValueExW(key,L"ThermalOverlay",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)));
        RegCloseKey(key);
    }
}
bool AutostartEnabled() {
    wchar_t value[32768]{}; DWORD bytes=sizeof(value);
    return RegGetValueW(HKEY_CURRENT_USER,StartupKey,L"ThermalOverlay",RRF_RT_REG_SZ,nullptr,value,&bytes)==ERROR_SUCCESS && std::wstring(value)==StartupCommand();
}
void ToggleAutostart(HWND hwnd) {
    bool enabled=AutostartEnabled(); HKEY key=nullptr;
    LSTATUS status=RegCreateKeyExW(HKEY_CURRENT_USER,StartupKey,0,nullptr,0,KEY_SET_VALUE,nullptr,&key,nullptr);
    if(status==ERROR_SUCCESS) {
        if(enabled) status=RegDeleteValueW(key,L"ThermalOverlay");
        else {
            std::wstring command=StartupCommand();
            status=command.empty()?ERROR_BAD_PATHNAME:RegSetValueExW(key,L"ThermalOverlay",0,REG_SZ,reinterpret_cast<const BYTE*>(command.c_str()),static_cast<DWORD>((command.size()+1)*sizeof(wchar_t)));
        }
        RegCloseKey(key);
    }
    if(status!=ERROR_SUCCESS) MessageBoxW(hwnd,L"Не удалось изменить автозагрузку для текущего пользователя.",L"Thermal",MB_OK|MB_ICONERROR);
}

bool SupportedCpu() {
    int vendor=cpu::Vendor(); int regs[4]{};
    if(vendor==1) { __cpuid(regs,6); return (regs[0]&1)!=0; }
    if(vendor==2) { __cpuid(regs,1); unsigned family=(regs[0]>>8)&15; if(family==15) family+=(regs[0]>>20)&255; return family==0x17 || family==0x19 || family==0x1a; }
    return false;
}
bool CpuServiceRunning() {
    SC_HANDLE manager=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT); if(!manager) return false;
    SC_HANDLE service=OpenServiceW(manager,cpu::ServiceName,SERVICE_QUERY_STATUS);
    SERVICE_STATUS state{}; bool running=service && QueryServiceStatus(service,&state) && state.dwCurrentState==SERVICE_RUNNING;
    if(service) CloseServiceHandle(service); CloseServiceHandle(manager); return running;
}
void ShowCpuSetupStatus(HWND owner,const wchar_t* status) {
    if(!cpuSetupWindow) {
        WNDCLASSW wc{}; wc.lpfnWndProc=DefWindowProcW; wc.hInstance=GetModuleHandleW(nullptr);
        wc.lpszClassName=L"ThermalOverlaySetupStatus"; wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hbrBackground=reinterpret_cast<HBRUSH>(COLOR_WINDOW+1);
        RegisterClassW(&wc);
        RECT ownerRect{}; GetWindowRect(owner,&ownerRect);
        RECT r=VisibleRect(ownerRect.left,ownerRect.top,px(390),px(130));
        cpuSetupWindow=CreateWindowExW(WS_EX_TOPMOST|WS_EX_DLGMODALFRAME,wc.lpszClassName,L"Настройка температуры CPU",WS_POPUP|WS_CAPTION,
            r.left,r.top,px(390),px(130),owner,nullptr,wc.hInstance,nullptr);
        cpuSetupLabel=CreateWindowExW(0,L"STATIC",L"",WS_CHILD|WS_VISIBLE,px(16),px(16),px(352),px(72),cpuSetupWindow,nullptr,wc.hInstance,nullptr);
        SendMessageW(cpuSetupLabel,WM_SETFONT,reinterpret_cast<WPARAM>(GetStockObject(DEFAULT_GUI_FONT)),TRUE);
    }
    SetWindowTextW(cpuSetupLabel,status); ShowWindow(cpuSetupWindow,SW_SHOWNOACTIVATE); UpdateWindow(cpuSetupWindow);
    InvalidateRect(owner,nullptr,FALSE); UpdateWindow(owner);
}
void FinishCpuSetup(HWND hwnd,const wchar_t* message,UINT icon) {
    KillTimer(hwnd,3);
    if(cpuSetupProcess) { CloseHandle(cpuSetupProcess); cpuSetupProcess=nullptr; }
    cpuSetupDeadline=0; cpuSetupLaunching=false;
    if(cpuSetupWindow) DestroyWindow(cpuSetupWindow);
    cpuSetupWindow=nullptr; cpuSetupLabel=nullptr;
    InvalidateRect(hwnd,nullptr,FALSE);
    MessageBoxW(hwnd,message,L"Настройка температуры CPU",MB_OK|icon);
}
void PollCpuSetup(HWND hwnd) {
    if(cpuSetupProcess) {
        DWORD wait=WaitForSingleObject(cpuSetupProcess,0);
        if(wait==WAIT_TIMEOUT) {
            ShowCpuSetupStatus(hwnd,cpu::DriverInstalled()?L"Драйвер установлен. Настраиваем и запускаем службу…\n\nПожалуйста, дождитесь завершения.":L"Проверяем и устанавливаем драйвер PawnIO…\n\nПожалуйста, дождитесь завершения.");
            return;
        }
        DWORD code=1;
        if(wait!=WAIT_OBJECT_0 || !GetExitCodeProcess(cpuSetupProcess,&code)) { FinishCpuSetup(hwnd,L"Не удалось получить результат установки. Проверьте состояние компонентов через меню настройки CPU.",MB_ICONERROR); return; }
        CloseHandle(cpuSetupProcess); cpuSetupProcess=nullptr;
        if(code==3010) { FinishCpuSetup(hwnd,L"Драйвер установлен. Для продолжения требуется перезагрузка Windows. После неё запустите программу и завершите настройку службы.",MB_ICONINFORMATION); return; }
        if(code!=0) {
            std::wstring error=code==10?L"Не удалось проверить комплект установки или подпись PawnIO. Используйте полную папку build.":code==20?L"Установщик PawnIO завершился с ошибкой. Драйвер не удалось установить.":code==30?L"Не удалось установить или запустить службу ThermalOverlay. Проверьте её состояние в службах Windows.":L"Установка завершилась с ошибкой. Убедитесь, что доступны все файлы из папки build и были предоставлены права администратора.";
            error+=L"\n\nКод: "+std::to_wstring(code)+L". Повторить настройку можно через меню.";
            FinishCpuSetup(hwnd,error.c_str(),MB_ICONERROR); return;
        }
        if(!cpu::DriverInstalled() || !cpu::ServiceInstalled() || !CpuServiceRunning()) {
            FinishCpuSetup(hwnd,L"Установщик завершил работу, но драйвер или запущенная служба не обнаружены. Настройка не подтверждена. Проверьте компоненты через меню CPU.",MB_ICONWARNING); return;
        }
        cpuSetupDeadline=GetTickCount64()+10000;
        ShowCpuSetupStatus(hwnd,L"Драйвер и служба установлены.\n\nОжидаем первые показания температуры CPU…");
    }
    if(cpuSetupDeadline) {
        cpu::Snapshot snapshot;
        if(cpu::ReadShared(snapshot) && snapshot.status==0 && snapshot.count) {
            FinishCpuSetup(hwnd,L"Готово! Драйвер PawnIO установлен, служба ThermalOverlay запущена.\n\nПоказания температуры CPU получены. При следующих запусках повторная установка не потребуется.",MB_ICONINFORMATION);
        } else if(GetTickCount64()>=cpuSetupDeadline) {
            FinishCpuSetup(hwnd,L"Драйвер установлен, служба запущена.\n\nПоказания температуры пока не получены. Виджет продолжит ожидать данные; если остаётся --, нужно проверить доступ к датчикам CPU.",MB_ICONWARNING);
        }
    }
}
void SetupCpu(HWND hwnd,bool automatic) {
    if(CpuSetupActive()) { if(cpuSetupWindow) SetForegroundWindow(cpuSetupWindow); return; }
    if(!SupportedCpu()) {
        if(!automatic) MessageBoxW(hwnd,L"Встроенный доступ поддерживает Intel с DTS и AMD Ryzen семейств 17h, 19h, 1Ah. Для остальных CPU доступен Libre Hardware Monitor.",L"Температура CPU",MB_OK|MB_ICONINFORMATION);
        return;
    }
    bool driver=cpu::DriverInstalled(),service=cpu::ServiceInstalled();
    if(driver && service) {
        if(!automatic) MessageBoxW(hwnd,L"Драйвер PawnIO и служба ThermalOverlay уже установлены. Повторная установка не требуется. Если температура недоступна, проверьте состояние службы ThermalOverlayCpu в Windows.",L"Температура CPU",MB_OK|MB_ICONINFORMATION);
        return;
    }
    std::wstring message=L"Обнаружен процессор "+std::wstring(cpu::Vendor()==1?L"Intel":L"AMD")+L".\n\nДля чтения температуры CPU нужны:\n";
    message+=driver?L"• Драйвер PawnIO — уже установлен.\n":L"• Подписанный драйвер PawnIO — требуется установка.\n";
    message+=service?L"• Служба ThermalOverlay — уже установлена.\n":L"• Служба ThermalOverlay — требуется установка.\n";
    message+=L"\nДрайвер даёт доступ к аппаратным датчикам, служба читает температуру в фоне, чтобы виджет не запрашивал права администратора при каждом запуске. Частоты, напряжения и лимиты питания не меняются.\n\nУстановить недостающие компоненты сейчас? Windows запросит права администратора. При отказе программа продолжит работу, установку можно вызвать из меню.";
    if(MessageBoxW(hwnd,message.c_str(),L"Настройка температуры CPU",MB_YESNO|MB_ICONINFORMATION)!=IDYES) return;
    auto script=cpu::ExeDir()+L"\\support\\setup-cpu.ps1";
    if(GetFileAttributesW(script.c_str())==INVALID_FILE_ATTRIBUTES) { MessageBoxW(hwnd,L"В папке support отсутствует setup-cpu.ps1. Используйте полную папку build с компонентами CPU.",L"Температура CPU",MB_OK|MB_ICONERROR); return; }
    wchar_t system[MAX_PATH]{}; GetSystemDirectoryW(system,MAX_PATH);
    std::wstring powershell=std::wstring(system)+L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    std::wstring args=L"-NoProfile -ExecutionPolicy Bypass -File \""+script+L"\" -Managed";
    cpuSetupLaunching=true;
    ShowCpuSetupStatus(hwnd,L"Ожидаем разрешение Windows…\n\nПодтвердите запрос администратора, чтобы начать установку.");
    SHELLEXECUTEINFOW execute{}; execute.cbSize=sizeof(execute); execute.fMask=SEE_MASK_NOCLOSEPROCESS|SEE_MASK_NOASYNC; execute.hwnd=hwnd; execute.lpVerb=L"runas"; execute.lpFile=powershell.c_str(); execute.lpParameters=args.c_str(); execute.nShow=SW_HIDE;
    if(!ShellExecuteExW(&execute)) {
        DWORD error=GetLastError();
        FinishCpuSetup(hwnd,error==ERROR_CANCELLED?L"Установка отменена. Её можно запустить позже через меню настройки CPU.":L"Не удалось запустить установку. Проверьте наличие support\\setup-cpu.ps1 в папке программы.",error==ERROR_CANCELLED?MB_ICONINFORMATION:MB_ICONERROR);
        return;
    }
    cpuSetupLaunching=false; cpuSetupProcess=execute.hProcess;
    if(!cpuSetupProcess) { FinishCpuSetup(hwnd,L"Установщик запущен, но Windows не предоставила доступ к его статусу. Проверьте компоненты через меню после завершения установки.",MB_ICONWARNING); return; }
    ShowCpuSetupStatus(hwnd,L"Идёт установка драйвера и службы…\n\nПожалуйста, дождитесь сообщения о результате.");
    SetTimer(hwnd,3,250,nullptr);
}

void Menu(HWND hwnd,POINT p) {
    HMENU menu=CreatePopupMenu(), sensors=CreatePopupMenu(), opacity=CreatePopupMenu(), colors=CreatePopupMenu();
    const wchar_t* labels[]={L"Загрузка CPU",L"Температура CPU (максимум)",L"Загрузка GPU (максимум)",L"Температура GPU (максимум)"};
    for(int i=0;i<4;i++) AppendMenuW(sensors,MF_STRING|((cfg.mask&(1<<i))?MF_CHECKED:0),100+i,labels[i]);
    for(int n=20;n<=100;n+=5) {
        std::wstring label=std::to_wstring(100-n)+L" % прозрачности";
        AppendMenuW(opacity,MF_STRING|(cfg.opacity==n?MF_CHECKED:0),200+n,label.c_str());
    }
    const wchar_t* colorNames[]={L"Графит",L"Синий",L"Зелёный",L"Бордовый",L"Светлый",L"Свой цвет…"};
    COLORREF presets[]={RGB(22,26,34),RGB(17,38,65),RGB(17,48,38),RGB(62,25,39),RGB(233,239,247)};
    for(int i=0;i<6;i++) AppendMenuW(colors,MF_STRING|((i<5 && cfg.color==presets[i])?MF_CHECKED:0),400+i,colorNames[i]);
    AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(sensors),L"Показатели");
    AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(opacity),L"Прозрачность");
    AppendMenuW(menu,MF_POPUP,reinterpret_cast<UINT_PTR>(colors),L"Цвет фона");
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    AppendMenuW(menu,MF_STRING|(AutostartEnabled()?MF_CHECKED:0),503,L"Запускать при входе в Windows");
    AppendMenuW(menu,MF_STRING|(CpuSetupActive()?MF_GRAYED:0),504,CpuSetupActive()?L"Установка CPU выполняется…":L"Настроить температуру CPU…");
    AppendMenuW(menu,MF_STRING,501,L"О датчиках");
    SetForegroundWindow(hwnd);
    int id=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,hwnd,nullptr); DestroyMenu(menu);
    if(id>=100 && id<104) { int next=cfg.mask^(1<<(id-100)); if((next&3) && (next&12)) cfg.mask=next; }
    else if(id>=220 && id<=300) cfg.opacity=id-200;
    else if(id>=400 && id<405) cfg.color=presets[id-400];
    else if(id==405) {
        static COLORREF custom[16]{}; CHOOSECOLORW cc{}; cc.lStructSize=sizeof(cc);
        cc.hwndOwner=hwnd; cc.rgbResult=cfg.color; cc.lpCustColors=custom; cc.Flags=CC_FULLOPEN|CC_RGBINIT;
        if(ChooseColorW(&cc)) cfg.color=cc.rgbResult;
    } else if(id==501) MessageBoxW(hwnd,
        L"CPU: общая загрузка Windows. На системах с более чем 64 логическими процессорами учитывается основная группа.\n\n"
        L"GPU: по умолчанию Windows, самый загруженный аппаратный движок (принцип Диспетчера задач). Температура округлена до целых градусов. При нескольких GPU максимумы могут относиться к разным адаптерам. В меню строки можно выбрать отдельный движок Windows или датчик NVIDIA NVML. NVML считает загрузку иначе.\n\n"
        L"Температура CPU: встроенная служба (Intel CPU Package / AMD Tdie), резервный источник — Libre Hardware Monitor. Для подключения используйте пункт меню «Настроить температуру CPU».\n\n"
        L"-- означает отсутствие данных, а не нулевую температуру. Обновление примерно раз в секунду.\n\n"
        L"Положение, показатели, цвет и прозрачность сохраняются в %LOCALAPPDATA%\\ThermalOverlay\\settings.ini.\n"
        L"Программа не отправляет данные в сеть.",L"Thermal",MB_OK|MB_ICONINFORMATION);
    else if(id==504) SetupCpu(hwnd,false);
    else if(id==503) ToggleAutostart(hwnd);
    if(id) { Layout(); SaveSettings(); }
}

void SourceMenu(HWND hwnd,int slot,POINT p) {
    Sample s; { std::lock_guard<std::mutex> lock(sampleMutex); s=latest; }
    HMENU menu=CreatePopupMenu();
    const wchar_t* defaults[]={L"Windows: общая загрузка CPU",L"Libre Hardware Monitor: максимум CPU",L"Windows: самый загруженный движок GPU",L"Windows: максимум температуры GPU"};
    std::wstring automatic=slot>=2?L"Авто: "+s.gpuSource:slot==1?L"Авто: "+s.cpuSource:defaults[slot];
    AppendMenuW(menu,MF_STRING|(cfg.sources[slot].empty()?MF_CHECKED:0),1,automatic.c_str());
    AppendMenuW(menu,MF_SEPARATOR,0,nullptr);
    std::vector<std::wstring> ids;
    for(const auto& c:s.choices) if(c.slot==slot) {
        ids.push_back(c.id);
        std::wstring title=c.name+L"  ["+c.id+L"]";
        size_t pos=0; while((pos=title.find(L'&',pos))!=std::wstring::npos) { title.insert(pos,1,L'&'); pos+=2; }
        AppendMenuW(menu,MF_STRING|(cfg.sources[slot]==c.id?MF_CHECKED:0),100+ids.size(),title.c_str());
    }
    if(ids.empty()) AppendMenuW(menu,MF_STRING|MF_GRAYED,0,L"Для отдельных датчиков запустите Libre Hardware Monitor");
    SetForegroundWindow(hwnd);
    int id=TrackPopupMenu(menu,TPM_RETURNCMD|TPM_RIGHTBUTTON,p.x,p.y,0,hwnd,nullptr); DestroyMenu(menu);
    std::wstring previousSource=cfg.sources[slot];
    if(id==1) cfg.sources[slot].clear();
    else if(id>=101 && static_cast<size_t>(id-101)<ids.size()) cfg.sources[slot]=ids[id-101];
    if(previousSource!=cfg.sources[slot]) for(auto& point:history) point.values[slot]=-1;
    if(id) { SaveSettings(); InvalidateRect(hwnd,nullptr,FALSE); }
}
int HitSensor(int x,int y) {
    for(int g=0;g<2;g++) {
        int left=10+g*205;
        if(x<px(left+10) || x>=px(left+185)) continue;
        int rowY=78;
        for(int j=0;j<2;j++) if(cfg.mask&(1<<(g*2+j))) {
            if(y>=px(rowY) && y<px(rowY+26)) return g*2+j;
            rowY+=26;
        }
    }
    return -1;
}

LRESULT CALLBACK WindowProc(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
    if(taskbarCreated && msg==taskbarCreated && trayVisible) {
        trayVisible=false; if(!AddTray(hwnd)) RestoreOverlay(hwnd); return 0;
    }
    switch(msg) {
    case WM_APP+4: SetupCpu(hwnd,true); return 0;
    case WM_RESTORE_OVERLAY: RestoreOverlay(hwnd); return 0;
    case WM_TRAY:
        if(lp==WM_LBUTTONUP || lp==WM_LBUTTONDBLCLK) RestoreOverlay(hwnd);
        else if(lp==WM_RBUTTONUP || lp==WM_CONTEXTMENU) TrayMenu(hwnd);
        return 0;
    case WM_SYSCOMMAND:
        if((wp&0xfff0)==SC_MINIMIZE) { MinimizeToTray(hwnd); return 0; }
        break;
    case WM_SETCURSOR: {
        POINT p{}; GetCursorPos(&p); ScreenToClient(hwnd,&p);
        if(LOWORD(lp)==HTCLIENT && (HitSensor(p.x,p.y)>=0 || HitMenu(p.x,p.y) || HitMinimize(p.x,p.y))) { SetCursor(LoadCursorW(nullptr,IDC_HAND)); return TRUE; }
        break;
    }
    case WM_MOUSEMOVE: {
        bool hover=HitMenu(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));
        bool minimize=HitMinimize(GET_X_LPARAM(lp),GET_Y_LPARAM(lp));
        if(hover!=menuHover || minimize!=minimizeHover) { menuHover=hover; minimizeHover=minimize; InvalidateRect(hwnd,nullptr,FALSE); }
        TRACKMOUSEEVENT track{}; track.cbSize=sizeof(track); track.dwFlags=TME_LEAVE; track.hwndTrack=hwnd;
        TrackMouseEvent(&track); return 0;
    }
    case WM_MOUSELEAVE: menuHover=false; minimizeHover=false; InvalidateRect(hwnd,nullptr,FALSE); return 0;
    case WM_PAINT: Paint(hwnd); return 0;
    case WM_ERASEBKGND: return 1;
    case WM_SAMPLE: {
        Sample current; { std::lock_guard<std::mutex> lock(sampleMutex); current=latest; }
        static std::wstring previousCpuSource;
        if(cfg.sources[1].empty() && !previousCpuSource.empty() && previousCpuSource!=current.cpuSource)
            for(auto& point:history) point.values[1]=-1;
        previousCpuSource=current.cpuSource;
        if(current.tick && (history.empty() || history.back().tick!=current.tick)) history.push_back({current.tick,SelectedValues(current)});
        ULONGLONG now=GetTickCount64();
        while(!history.empty() && now-history.front().tick>HistoryDuration) history.pop_front();
        InvalidateRect(hwnd,nullptr,FALSE); return 0;
    }
    case WM_TIMER: if(wp==3) PollCpuSetup(hwnd); InvalidateRect(hwnd,nullptr,FALSE); return 0;
    case WM_LBUTTONDOWN:
        if(HitMinimize(GET_X_LPARAM(lp),GET_Y_LPARAM(lp))) { MinimizeToTray(hwnd); return 0; }
        if(int slot=HitSensor(GET_X_LPARAM(lp),GET_Y_LPARAM(lp)); slot>=0) { POINT p{}; GetCursorPos(&p); SourceMenu(hwnd,slot,p); return 0; }
        if(HitMenu(GET_X_LPARAM(lp),GET_Y_LPARAM(lp))) { POINT p{}; GetCursorPos(&p); Menu(hwnd,p); }
        else { ReleaseCapture(); SendMessageW(hwnd,WM_NCLBUTTONDOWN,HTCAPTION,lp); } return 0;
    case WM_CONTEXTMENU: {
        POINT p{GET_X_LPARAM(lp),GET_Y_LPARAM(lp)}; if(p.x==-1 && p.y==-1) GetCursorPos(&p); Menu(hwnd,p); return 0;
    }
    case WM_EXITSIZEMOVE: Layout(); SaveSettings(); return 0;
    case WM_DPICHANGED: {
        dpi=HIWORD(wp); auto r=reinterpret_cast<RECT*>(lp);
        SetWindowPos(hwnd,nullptr,r->left,r->top,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE); Layout(); SaveSettings(); return 0;
    }
    case WM_DISPLAYCHANGE: Layout(); SaveSettings(); return 0;
    case WM_QUERYENDSESSION: SaveSettings(); return TRUE;
    case WM_CLOSE: MinimizeToTray(hwnd); return 0;
    case WM_DESTROY: if(cpuSetupProcess) { CloseHandle(cpuSetupProcess); cpuSetupProcess=nullptr; } RemoveTray(); SaveSettings(); SetEvent(stopEvent); PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd,msg,wp,lp);
}
int WINAPI wWinMain(HINSTANCE instance,HINSTANCE,LPWSTR command,int) {
    bool smoke=std::wstring(command).find(L"--smoke-test")!=std::wstring::npos;
    if(!SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2))
        SetProcessDPIAware();
    HANDLE singleton=CreateMutexW(nullptr,FALSE,smoke?L"Local\\ThermalOverlaySmoke":L"Local\\ThermalOverlay");
    if(!singleton || GetLastError()==ERROR_ALREADY_EXISTS) {
        if(singleton) {
            if(!smoke) { HWND existing=FindWindowW(L"ThermalOverlayWindow",nullptr); if(existing) PostMessageW(existing,WM_RESTORE_OVERLAY,0,0); }
            CloseHandle(singleton);
        }
        return 0;
    }
    CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    LoadSettings();
    if(!smoke) MigrateAutostart();
    if(smoke) {
        wchar_t temp[MAX_PATH]{};
        if(!GetTempPathW(MAX_PATH,temp)) return 10;
        settingsPath=std::wstring(temp)+L"ThermalOverlay-test-"+std::to_wstring(GetCurrentProcessId())+L".ini";
        cfg.x=80; cfg.y=80; cfg.opacity=65; cfg.mask=15; cfg.color=RGB(31,57,81); cfg.sources[1]=L"/test/temperature/0";
        SaveSettings();
        if(saveFailed || GetPrivateProfileIntW(L"Window",L"Opacity",0,settingsPath.c_str())!=65 ||
           GetPrivateProfileIntW(L"Window",L"Sensors",0,settingsPath.c_str())!=15 ||
           GetPrivateProfileIntW(L"Window",L"Color",0,settingsPath.c_str())!=cfg.color) return 11;
        wchar_t source[128]{};
        GetPrivateProfileStringW(L"Sources",L"1",L"",source,128,settingsPath.c_str());
        if(std::wstring(source)!=cfg.sources[1]) return 13;
    }
    stopEvent=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    appIcon=CreateAppIcon(); taskbarCreated=RegisterWindowMessageW(L"TaskbarCreated");
    WNDCLASSW wc{}; wc.lpfnWndProc=WindowProc; wc.hInstance=instance; wc.lpszClassName=L"ThermalOverlayWindow";
    wc.hCursor=LoadCursorW(nullptr,IDC_ARROW); wc.hIcon=appIcon?appIcon:LoadIconW(nullptr,IDI_APPLICATION);
    if(!RegisterClassW(&wc) || !stopEvent) return 1;
    RECT r=VisibleRect(cfg.x,cfg.y,px(420),height());
    mainWindow=CreateWindowExW(WS_EX_TOPMOST|WS_EX_LAYERED|WS_EX_TOOLWINDOW,wc.lpszClassName,L"Thermal",WS_POPUP,
        r.left,r.top,r.right-r.left,r.bottom-r.top,nullptr,nullptr,instance,nullptr);
    if(!mainWindow) return 2;
    dpi=GetDpiForWindow(mainWindow); Layout(); ShowWindow(mainWindow,SW_SHOWNOACTIVATE); SetTimer(mainWindow,1,1000,nullptr);
    HANDLE worker=CreateThread(nullptr,0,Worker,nullptr,0,nullptr);
    if(!worker) return 3;
    if(smoke) {
        BYTE alpha=0; DWORD flags=0;
        if(!GetLayeredWindowAttributes(mainWindow,nullptr,&alpha,&flags) || alpha!=cfg.opacity*255/100 ||
           !(GetWindowLongPtrW(mainWindow,GWL_EXSTYLE)&WS_EX_TOPMOST)) ExitProcess(15);
        SetWindowPos(mainWindow,nullptr,120,130,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
        SendMessageW(mainWindow,WM_EXITSIZEMOVE,0,0);
        RECT actual{}; GetWindowRect(mainWindow,&actual);
        if(GetPrivateProfileIntW(L"Window",L"X",0,settingsPath.c_str())!=static_cast<UINT>(actual.left) ||
           GetPrivateProfileIntW(L"Window",L"Y",0,settingsPath.c_str())!=static_cast<UINT>(actual.top)) ExitProcess(16);
        if(HitSensor(px(30),px(82))!=0 || HitSensor(px(260),px(82))!=2 || cardRows(0)!=2 || cardRows(1)!=2) ExitProcess(17);
        SendMessageW(mainWindow,WM_CLOSE,0,0);
        if(!IsWindow(mainWindow)) ExitProcess(21);
        if(!trayVisible || IsWindowVisible(mainWindow)) ExitProcess(18);
        RestoreOverlay(mainWindow);
        if(trayVisible || !IsWindowVisible(mainWindow)) ExitProcess(19);
        SetTimer(mainWindow,2,4500,[](HWND w,UINT,UINT_PTR, DWORD) { DestroyWindow(w); });
    }
    if(!smoke) PostMessageW(mainWindow,WM_APP+4,0,0);
    MSG msg{}; while(GetMessageW(&msg,nullptr,0,0)>0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    SetEvent(stopEvent);
    // Local WMI providers can stall. Never leave an invisible process on exit.
    if(WaitForSingleObject(worker,1500)==WAIT_TIMEOUT) ExitProcess(smoke?14:0);
    int result=0;
    if(smoke) {
        std::lock_guard<std::mutex> lock(sampleMutex);
        if(latest.cpu<0 || latest.cpu>100 || !latest.tick) result=12;
        if(history.size()<2 || history.back().tick-history.front().tick>HistoryDuration) result=20;
        DeleteFileW(settingsPath.c_str());
        DeleteFileW((settingsPath+L".tmp").c_str());
    }
    CloseHandle(worker); CloseHandle(stopEvent); CloseHandle(singleton); if(appIcon) DestroyIcon(appIcon); CoUninitialize(); return result;
}
