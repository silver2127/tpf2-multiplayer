#!/usr/bin/env python3
"""Run the upstream GL pixel/state tests using an SDL Linux context."""
from pathlib import Path
import subprocess
import ast
root=Path(__file__).resolve().parents[2]
out=root/'.local-test/tests/gl-overlay-linux'
out.mkdir(parents=True,exist_ok=True)
source=(root/'tools/test_gl_overlay.py').read_text()
code=next(ast.literal_eval(n.value) for n in ast.parse(source).body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='code' for t in n.targets))
code=code.replace('#include <windows.h>', '#include <dlfcn.h>')
a=code.index('    WNDCLASSW'); b=code.index('    auto clearColor',a)
code=code[:a]+r'''    void* lib=dlopen("libSDL2-2.0.so.0",RTLD_NOW|RTLD_GLOBAL);
    if(!lib) return 2;
    auto init=(int(*)(unsigned))dlsym(lib,"SDL_Init");
    auto create=(void*(*)(const char*,int,int,int,int,unsigned))dlsym(lib,"SDL_CreateWindow");
    auto make=(void*(*)(void*))dlsym(lib,"SDL_GL_CreateContext");
    if(init(32))return 2;
    void* window=create("GL test",0,0,64,64,2|8);
    if(!window || !make(window))return 2;
'''+code[b:]
code=code.replace('GetProcAddress(gl, ', 'WglProc(')
a=code.index('    wglMakeCurrent');b=code.index('    if (fails)',a)
code=code[:a]+code[b:]
code=code.replace('GL_OVERLAY',str(root/'native/src/gl_overlay.h'))
(out/'test.cpp').write_text(code)
subprocess.run(['g++','-std=c++17',str(out/'test.cpp'),'-ldl','-o',str(out/'test')],check=True)
subprocess.run([str(out/'test')],check=True,timeout=30)
