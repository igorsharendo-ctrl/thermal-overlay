# PawnIO CPU support

The native C++ service dynamically uses the installed PawnIOLib. It never embeds or replaces the driver and never calls MSR write or tuning functions.

Bundled signed modules: IntelMSR.bin and AMDFamily17.bin from PawnIO.Modules 0.2.11 (LGPL-2.1-or-later). The original license is in modules/COPYING; the corresponding upstream source archive is in source/PawnIO.Modules-0.2.11.tar.gz. Modules are loaded as separate replaceable files.

Upstream: https://github.com/namazso/PawnIO.Modules/releases/tag/0.2.11
Module API: https://github.com/namazso/PawnIO.Modules/wiki/Using-PawnIO-Modules
Driver: https://pawnio.eu

The official, unmodified PawnIO 2.2.0 installer is distributed only with the local build, under cpu-support/PawnIO_setup.exe. Its redistribution is permitted by the installer notice. It is not committed to Git. No unrestricted/debug edition is used. Installer SHA-256:
1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032

AMD register interpretation follows the documented THM_TCON_CUR_TMP layout and the reference behavior in LibreHardwareMonitor Amd17Cpu.cs and Linux k10temp.c; support is limited to AMD families 17h, 19h and 1Ah. Intel DTS decoding follows IA32_TEMPERATURE_TARGET / IA32_THERM_STATUS / IA32_PACKAGE_THERM_STATUS. No third-party application code is linked.
