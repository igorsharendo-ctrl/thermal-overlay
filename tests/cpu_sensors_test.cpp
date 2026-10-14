#define UNICODE
#define _UNICODE
#define NOMINMAX
#define _WIN32_WINNT 0x0A00
#include "../src/cpu_sensors.h"
#include <iostream>
int main() {
    double t=0;
    if(!cpu::DecodeIntel(100ull<<16,(1ull<<31)|(42ull<<16),t) || t!=58) return 1;
    if(cpu::DecodeIntel(100ull<<16,42ull<<16,t)) return 2; // Invalid DTS reading must be rejected.
    if(cpu::DecodeIntel(0,(1ull<<31)|(42ull<<16),t)) return 3; // Do not guess TjMax.
    if(!cpu::DecodeAmd(480ull<<21,0,t) || t!=60) return 4;
    if(!cpu::DecodeAmd((872ull<<21)|0x80000,0,t) || t!=60) return 5;
    if(!cpu::DecodeAmd((872ull<<21)|0x30000,0,t) || t!=60) return 6;
    if(!cpu::DecodeAmd(640ull<<21,20,t) || t!=60) return 7; // Ryzen 1000X Tctl offset.
    if(cpu::DecodeAmd(0,0,t) || cpu::DecodeAmd(0xffffffff,0,t)) return 8;
    std::cout << "CPU conversion tests: PASS\n";
    std::cout << "Driver installed: " << cpu::DriverInstalled() << "\nService installed: " << cpu::ServiceInstalled() << "\n";
    return 0;
}
