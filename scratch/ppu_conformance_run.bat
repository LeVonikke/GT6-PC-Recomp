@echo off
call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
cd /d "C:\Users\nedch\AppData\Local\Temp\claude\G--recomp-ps3\01e23e74-74ee-48e8-a174-37d9ae59237b\scratchpad\integrate"
cl /nologo /O1 /W3 /I runtime\ppu /Fe:"C:\Users\nedch\AppData\Local\Temp\claude\G--recomp-ps3\01e23e74-74ee-48e8-a174-37d9ae59237b\scratchpad\integrate\scratch\ppu_conformance.exe" "C:\Users\nedch\AppData\Local\Temp\claude\G--recomp-ps3\01e23e74-74ee-48e8-a174-37d9ae59237b\scratchpad\integrate\scratch\ppu_conformance.cpp" > "C:\Users\nedch\AppData\Local\Temp\claude\G--recomp-ps3\01e23e74-74ee-48e8-a174-37d9ae59237b\scratchpad\integrate\scratch\ppu_conformance.log" 2>&1
if errorlevel 1 exit /b 2
"C:\Users\nedch\AppData\Local\Temp\claude\G--recomp-ps3\01e23e74-74ee-48e8-a174-37d9ae59237b\scratchpad\integrate\scratch\ppu_conformance.exe" >> "C:\Users\nedch\AppData\Local\Temp\claude\G--recomp-ps3\01e23e74-74ee-48e8-a174-37d9ae59237b\scratchpad\integrate\scratch\ppu_conformance.log" 2>&1
