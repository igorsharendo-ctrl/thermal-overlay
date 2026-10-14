#include "../src/main.cpp"
#include <iostream>
int wmain() {
    if(_wcsnicmp(L"luid_0x00000000_0x00011A88_phys_0",L"luid_0x00000000_0x00011a88_phys_",30)!=0) return 6;
    CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    cfg.sources[1]=L"";
    {
        Sensors sensors; sensors.Read(); Sleep(1200); auto sample=sensors.Read();
        if(sample.ramTotal<=0 || sample.ramUsed<0 || sample.ramUsed>sample.ramTotal) return 1;
        auto values=SelectedValues(sample);
        if(std::abs(values[4]-100*sample.ramUsed/sample.ramTotal)>0.001) return 2;
        std::wcout<<L"RAM "<<MemoryValue(sample.ramUsed,sample.ramTotal)<<L"; VRAM "<<MemoryValue(sample.gpuUsed,sample.gpuTotal)<<L"\n";
        if(sample.gpuUsed>=0 && (sample.gpuTotal<=0 || values[6]<0)) return 3;
        Sample synthetic; synthetic.gpuUsed=4; synthetic.gpuTotal=8; synthetic.ram=25; synthetic.ramTemp=40;
        auto v=SelectedValues(synthetic); if(v[6]!=50 || v[4]!=25 || v[5]!=40) return 4;
        synthetic.gpuUsed=-1; if(SelectedValues(synthetic)[6]!=-1) return 5;
    }
    CoUninitialize(); return 0;
}
