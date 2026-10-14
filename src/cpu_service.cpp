#ifndef UNICODE
#define UNICODE
#endif
#define _UNICODE
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include "cpu_sensors.h"
#include <sddl.h>
#include <memory>
SERVICE_STATUS_HANDLE serviceHandle=nullptr;
SERVICE_STATUS serviceStatus{};
HANDLE stopped=nullptr;
void Report(DWORD state,DWORD code=0) {
    serviceStatus.dwServiceType=SERVICE_WIN32_OWN_PROCESS;
    serviceStatus.dwCurrentState=state; serviceStatus.dwWin32ExitCode=code;
    serviceStatus.dwControlsAccepted=state==SERVICE_RUNNING?SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN:0;
    SetServiceStatus(serviceHandle,&serviceStatus);
}
void WINAPI Control(DWORD control) { if(control==SERVICE_CONTROL_STOP || control==SERVICE_CONTROL_SHUTDOWN) { Report(SERVICE_STOP_PENDING); SetEvent(stopped); } }
void WINAPI ServiceMain(DWORD,LPWSTR*) {
    stopped=CreateEventW(nullptr,TRUE,FALSE,nullptr);
    serviceHandle=RegisterServiceCtrlHandlerW(cpu::ServiceName,Control); if(!serviceHandle) { if(stopped) CloseHandle(stopped); return; }
    Report(SERVICE_START_PENDING);
    PSECURITY_DESCRIPTOR descriptor=nullptr;
    if(!stopped || !ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GR;;;AU)",SDDL_REVISION_1,&descriptor,nullptr)) { Report(SERVICE_STOPPED,GetLastError()); return; }
    SECURITY_ATTRIBUTES attributes{sizeof(attributes),descriptor,FALSE};
    HANDLE mapping=CreateFileMappingW(INVALID_HANDLE_VALUE,&attributes,PAGE_READWRITE,0,sizeof(cpu::Shared),cpu::MappingName);
    DWORD created=GetLastError(); LocalFree(descriptor);
    // Refuse to publish through an object pre-created by another process.
    if(!mapping || created==ERROR_ALREADY_EXISTS) { if(mapping) CloseHandle(mapping); Report(SERVICE_STOPPED,ERROR_ALREADY_EXISTS); CloseHandle(stopped); return; }
    auto data=static_cast<cpu::Shared*>(MapViewOfFile(mapping,FILE_MAP_WRITE,0,0,sizeof(cpu::Shared)));
    if(!data) { CloseHandle(mapping); Report(SERVICE_STOPPED,GetLastError()); CloseHandle(stopped); return; }
    auto reader=std::make_unique<cpu::Reader>(); unsigned failed=0; Report(SERVICE_RUNNING);
    do {
        cpu::Snapshot snapshot=reader->Read();
        if(snapshot.status==1 && ++failed>=15) { reader.reset(); reader=std::make_unique<cpu::Reader>(); failed=0; }
        else if(snapshot.status==0) failed=0;
        InterlockedIncrement(&data->sequence); MemoryBarrier(); memcpy(&data->snapshot,&snapshot,sizeof(snapshot)); MemoryBarrier(); InterlockedIncrement(&data->sequence);
    } while(WaitForSingleObject(stopped,1000)==WAIT_TIMEOUT);
    UnmapViewOfFile(data); CloseHandle(mapping); CloseHandle(stopped); Report(SERVICE_STOPPED);
}
int WINAPI wWinMain(HINSTANCE,HINSTANCE,LPWSTR,int) {
    SERVICE_TABLE_ENTRYW services[]={{const_cast<LPWSTR>(cpu::ServiceName),ServiceMain},{nullptr,nullptr}};
    return StartServiceCtrlDispatcherW(services)?0:static_cast<int>(GetLastError());
}
