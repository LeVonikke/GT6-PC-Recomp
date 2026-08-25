@echo off
echo Configurando variaveis de ambiente...

set GT6_PDI_INJECT=1
set GT6_PDI_DMA_TRACE=1
set GT6_PDI_REAL_KERNEL1=1
set YDKJ_SCTRACE=1
set YDKJ_LIBTRACE=1

echo Iniciando GT6MainRecompHook.exe...
"E:\Ferramentas\Projetos Codex\GT6HACKFIX\gt6\emain_project\build\Release\GT6MainRecompHook.exe" "E:\Emulation\storage\rpcs3\dev_hdd0\game\NPUA81049\USRDIR\emain.elf"
pause
