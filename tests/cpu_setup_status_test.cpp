#ifndef UNICODE
#define UNICODE
#endif
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#define WINVER 0x0A00
#include <windows.h>
#include <string>
static std::wstring notice;
static UINT noticeFlags=0;
int WINAPI CaptureMessage(HWND,LPCWSTR text,LPCWSTR,UINT flags) { notice=text; noticeFlags=flags; return IDOK; }
#define MessageBoxW CaptureMessage
#include "../src/main.cpp"
#undef MessageBoxW
#include <iostream>
int wmain(int argc,wchar_t** argv) {
    if(argc>1) return _wtoi(argv[1]);
    wchar_t path[32768]{}; GetModuleFileNameW(nullptr,path,32768);
    auto finishChild=[&](int code) {
        std::wstring command=L"\""+std::wstring(path)+L"\" "+std::to_wstring(code);
        STARTUPINFOW startup{}; startup.cb=sizeof(startup); PROCESS_INFORMATION process{};
        if(!CreateProcessW(nullptr,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process)) return false;
        CloseHandle(process.hThread);
        if(WaitForSingleObject(process.hProcess,5000)!=WAIT_OBJECT_0) { CloseHandle(process.hProcess); return false; }
        cpuSetupProcess=process.hProcess; notice.clear(); noticeFlags=0;
        PollCpuSetup(nullptr);
        return cpuSetupProcess==nullptr;
    };
    if(!finishChild(30) || notice.find(L"службу")==notice.npos || !(noticeFlags&MB_ICONERROR) || CpuSetupActive()) return 1;
    if(!finishChild(10) || notice.find(L"подпись")==notice.npos || CpuSetupActive()) return 2;
    if(!finishChild(3010) || notice.find(L"перезагрузка")==notice.npos || CpuSetupActive()) return 3;
    if(!finishChild(0)) return 4;
    // Verify against the installed components, without installing or modifying them.
    cpu::Snapshot current;
    bool ready=cpu::DriverInstalled() && cpu::ServiceInstalled() && CpuServiceRunning() && cpu::ReadShared(current) && current.status==0 && current.count;
    if(ready && (notice.find(L"Готово!")==notice.npos || CpuSetupActive())) return 5;
    if(CpuSetupActive()) { cpuSetupDeadline=GetTickCount64(); PollCpuSetup(nullptr); }
    std::cout << "Setup monitor tests: PASS (errors, restart, completion)\n";
    return 0;
}
