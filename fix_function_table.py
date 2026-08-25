content = open(r'e:\Ferramentas\GT6HACKFIX\gt6\emain_recompiled\ppu_recomp_009.cpp', 'r', encoding='utf-8').read()
old_line = '0x0001022CULL, func_00010230, "func_0001022C" },`n    { 0x00010230ULL, func_00010230,'
new_line = '0x0001022CULL, func_00010230, "func_0001022C" },\n    { 0x00010230ULL, func_00010230,'
content = content.replace(old_line, new_line)
open(r'e:\Ferramentas\GT6HACKFIX\gt6\emain_recompiled\ppu_recomp_009.cpp', 'w', encoding='utf-8').write(content)
print("Fixed function_table")
