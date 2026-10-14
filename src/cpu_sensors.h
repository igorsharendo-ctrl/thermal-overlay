#pragma once
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <vector>
namespace cpu {
constexpr wchar_t ServiceName[]=L"ThermalOverlayCpu";
constexpr wchar_t MappingName[]=L"Global\\ThermalOverlayCpu.v1";
constexpr unsigned Magic=0x54435055, MaxSensors=128;
struct Reading { wchar_t id[64],name[80]; double temperature; };
struct Snapshot { unsigned magic=Magic,version=1,count=0,status=1; ULONGLONG tick=0; Reading sensors[MaxSensors]{}; };
struct Shared { volatile LONG sequence; Snapshot snapshot; };
inline std::wstring ExeDir() { std::vector<wchar_t> p(32768); DWORD n=GetModuleFileNameW(nullptr,p.data(),static_cast<DWORD>(p.size())); if(!n || n>=p.size()) return {}; std::wstring s(p.data(),n); return s.substr(0,s.find_last_of(L"\\/")); }
inline int Vendor() { int r[4]{}; __cpuid(r,0); char name[13]{}; memcpy(name,r+1,4); memcpy(name+4,r+3,4); memcpy(name+8,r+2,4); return strcmp(name,"GenuineIntel")==0?1:strcmp(name,"AuthenticAMD")==0?2:0; }
inline std::wstring DriverDir() {
    wchar_t path[32768]{}; DWORD bytes=sizeof(path);
    if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\PawnIO",L"InstallLocation",RRF_RT_REG_SZ|RRF_SUBKEY_WOW6464KEY,nullptr,path,&bytes)==ERROR_SUCCESS) return path;
    if(!GetEnvironmentVariableW(L"ProgramFiles",path,32768)) return {};
    return std::wstring(path)+L"\\PawnIO";
}
inline bool DriverInstalled() { auto path=DriverDir()+L"\\PawnIOLib.dll"; return GetFileAttributesW(path.c_str())!=INVALID_FILE_ATTRIBUTES; }
inline bool ServiceInstalled() { SC_HANDLE scm=OpenSCManagerW(nullptr,nullptr,SC_MANAGER_CONNECT); if(!scm) return false; SC_HANDLE svc=OpenServiceW(scm,ServiceName,SERVICE_QUERY_STATUS); bool exists=svc!=nullptr; if(svc) CloseServiceHandle(svc); CloseServiceHandle(scm); return exists; }
inline bool ReadShared(Snapshot& out) {
    HANDLE mapping=OpenFileMappingW(FILE_MAP_READ,FALSE,MappingName); if(!mapping) return false;
    auto shared=static_cast<const Shared*>(MapViewOfFile(mapping,FILE_MAP_READ,0,0,sizeof(Shared)));
    bool ok=false;
    if(shared) {
        for(int i=0;i<4;i++) {
            LONG before=shared->sequence; if(before&1) { Sleep(1); continue; }
            MemoryBarrier(); memcpy(&out,&shared->snapshot,sizeof(out)); MemoryBarrier();
            if(before==shared->sequence) { ok=out.magic==Magic && out.version==1 && out.count<=MaxSensors && out.tick<=GetTickCount64() && GetTickCount64()-out.tick<5000; break; }
        }
        UnmapViewOfFile(shared);
    }
    CloseHandle(mapping); return ok;
}
inline bool DecodeIntel(UINT64 target,UINT64 status,double& value) {
    unsigned tj=static_cast<unsigned>((target>>16)&255);
    if(!(status&(1ull<<31)) || tj<60 || tj>125) return false;
    value=tj-static_cast<double>((status>>16)&127); return value>0 && value<150;
}
inline bool DecodeAmd(UINT64 raw,double offset,double& value) {
    if(raw==0 || raw==0xffffffff) return false;
    value=((raw>>21)&2047)*0.125;
    if((raw&0x80000) || (raw&0x30000)==0x30000) value-=49;
    value-=offset; return value>0 && value<150;
}
class Reader {
    using Open=HRESULT (WINAPI*)(PHANDLE);
    using Load=HRESULT (WINAPI*)(HANDLE,const UCHAR*,SIZE_T);
    using Execute=HRESULT (WINAPI*)(HANDLE,PCSTR,const ULONG64*,SIZE_T,PULONG64,SIZE_T,PSIZE_T);
    using Close=HRESULT (WINAPI*)(HANDLE);
    HMODULE lib=nullptr; HANDLE executor=nullptr,pci=nullptr;
    Execute execute=nullptr; Close close=nullptr;
    int vendor=Vendor(); double amdOffset=0;
    std::vector<GROUP_AFFINITY> cores;
    bool Call(const char* name,UINT64 address,UINT64& value) { SIZE_T count=0; return executor && SUCCEEDED(execute(executor,name,&address,1,&value,1,&count)) && count==1; }
    void Add(Snapshot& s,const std::wstring& id,const std::wstring& name,double value) {
        if(s.count>=MaxSensors || value<=0 || value>=150) return;
        auto& reading=s.sensors[s.count++]; lstrcpynW(reading.id,id.c_str(),64); lstrcpynW(reading.name,name.c_str(),80); reading.temperature=value;
    }
public:
    Reader() {
        if(vendor==2) {
            int regs[4]{}; __cpuid(regs,1); unsigned family=(regs[0]>>8)&15; if(family==15) family+=(regs[0]>>20)&255;
            if(family!=0x17 && family!=0x19 && family!=0x1a) return;
            char brand[49]{}; __cpuid(regs,static_cast<int>(0x80000000u));
            if(static_cast<unsigned>(regs[0])>=0x80000004u) for(unsigned n=0;n<3;n++) { __cpuid(regs,static_cast<int>(0x80000002u+n)); memcpy(brand+n*16,regs,16); }
            std::string name=brand;
            if(name.find("1600X")!=name.npos || name.find("1700X")!=name.npos || name.find("1800X")!=name.npos) amdOffset=20;
            else if(name.find("Threadripper 19")!=name.npos || name.find("Threadripper 29")!=name.npos) amdOffset=27;
            else if(name.find("2700X")!=name.npos) amdOffset=10;
            pci=CreateMutexW(nullptr,FALSE,L"Global\\Access_PCI");
        } else if(vendor!=1) return;
        auto dll=DriverDir()+L"\\PawnIOLib.dll";
        lib=LoadLibraryExW(dll.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32); if(!lib) return;
        auto open=reinterpret_cast<Open>(GetProcAddress(lib,"pawnio_open")); auto load=reinterpret_cast<Load>(GetProcAddress(lib,"pawnio_load"));
        execute=reinterpret_cast<Execute>(GetProcAddress(lib,"pawnio_execute")); close=reinterpret_cast<Close>(GetProcAddress(lib,"pawnio_close"));
        if(!open || !load || !execute || !close) return;
        auto path=ExeDir()+L"\\modules\\"+(vendor==1?L"IntelMSR.bin":L"AMDFamily17.bin");
        HANDLE file=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr); if(file==INVALID_HANDLE_VALUE) return;
        DWORD size=GetFileSize(file,nullptr),read=0; std::vector<BYTE> bytes;
        if(size>0 && size<1024*1024) { bytes.resize(size); if(!ReadFile(file,bytes.data(),size,&read,nullptr) || read!=size) bytes.clear(); }
        CloseHandle(file); if(bytes.empty()) return;
        if(FAILED(open(&executor))) { executor=nullptr; return; }
        if(FAILED(load(executor,bytes.data(),bytes.size()))) { close(executor); executor=nullptr; return; }
        DWORD length=0; GetLogicalProcessorInformationEx(RelationProcessorCore,nullptr,&length);
        std::vector<BYTE> data(length);
        if(length && GetLogicalProcessorInformationEx(RelationProcessorCore,reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(data.data()),&length)) {
            for(DWORD offset=0;offset<length;) {
                auto info=reinterpret_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION_EX>(data.data()+offset); if(!info->Size) break;
                for(WORD g=0;g<info->Processor.GroupCount;g++) { auto affinity=info->Processor.GroupMask[g]; affinity.Mask&=(~affinity.Mask+1); if(affinity.Mask) cores.push_back(affinity); }
                offset+=info->Size;
            }
        }
    }
    ~Reader() { if(executor && close) close(executor); if(lib) FreeLibrary(lib); if(pci) CloseHandle(pci); }
    Snapshot Read() {
        Snapshot s; s.tick=GetTickCount64(); if(!executor) return s;
        if(vendor==1) {
            double package=-1;
            for(size_t i=0;i<cores.size() && i<MaxSensors-1;i++) {
                GROUP_AFFINITY previous{}; if(!SetThreadGroupAffinity(GetCurrentThread(),&cores[i],&previous)) continue;
                UINT64 target=0,status=0; double t=0;
                if(Call("ioctl_read_msr",0x1a2,target)) {
                    if(Call("ioctl_read_msr",0x1b1,status) && DecodeIntel(target,status,t)) package=std::max(package,t);
                    if(Call("ioctl_read_msr",0x19c,status) && DecodeIntel(target,status,t)) Add(s,L"intel/core/"+std::to_wstring(i),L"Intel Core "+std::to_wstring(i+1),t);
                }
                SetThreadGroupAffinity(GetCurrentThread(),&previous,nullptr);
            }
            if(package>0) {
                if(s.count<MaxSensors) { for(unsigned i=s.count;i>0;i--) s.sensors[i]=s.sensors[i-1]; ++s.count; }
                auto& r=s.sensors[0]; lstrcpynW(r.id,L"intel/package",64); lstrcpynW(r.name,L"Intel CPU Package",80); r.temperature=package;
            }
        } else if(vendor==2 && pci) {
            DWORD wait=WaitForSingleObject(pci,100);
            if(wait==WAIT_OBJECT_0 || wait==WAIT_ABANDONED) {
                UINT64 raw=0; bool ok=Call("ioctl_read_smn",0x59800,raw); ReleaseMutex(pci);
                double t=0;
                if(ok && DecodeAmd(raw,amdOffset,t)) Add(s,L"amd/tdie",L"AMD CPU Tdie",t);
                if(ok && amdOffset>0 && DecodeAmd(raw,0,t)) Add(s,L"amd/tctl",L"AMD CPU Tctl",t);
            }
        }
        s.status=s.count?0:2; return s;
    }
};
}
