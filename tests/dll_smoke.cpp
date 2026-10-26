#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstring>
#include <string>

#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line %d: %s\n",__LINE__,#x); return 1; } } while(0)
int wmain(int argc,wchar_t** argv) {
    CHECK(argc==2);
    HMODULE module=LoadLibraryW(argv[1]);
    CHECK(module!=nullptr);
    auto* base=reinterpret_cast<BYTE*>(module);
    auto* dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto* nt=reinterpret_cast<IMAGE_NT_HEADERS*>(base+dos->e_lfanew);
    CHECK(nt->FileHeader.Machine==IMAGE_FILE_MACHINE_I386);
    auto* exports=reinterpret_cast<IMAGE_EXPORT_DIRECTORY*>(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT].VirtualAddress);
    CHECK(exports->NumberOfNames==8);
    const char* names[]={"Direct3DCreate9","D3DPERF_BeginEvent","D3DPERF_EndEvent",
        "D3DPERF_SetMarker","D3DPERF_SetRegion","D3DPERF_QueryRepeatFrame","D3DPERF_SetOptions","D3DPERF_GetStatus"};
    for (auto name:names) CHECK(GetProcAddress(module,name)!=nullptr);
    // No import of the proxy itself and no dynamically linked MSVC C++ runtime.
    auto* imports=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress);
    for (; imports->Name; ++imports) {
        const char* name=reinterpret_cast<const char*>(base+imports->Name);
        CHECK(_stricmp(name,"d3d9.dll")!=0);
        CHECK(_strnicmp(name,"VCRUNTIME",9)!=0 && _strnicmp(name,"MSVCP",5)!=0);
    }
    auto create=reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(module,"Direct3DCreate9"));
    CHECK(create(0)==nullptr); // failed creation must not poison lazy initialization
    auto* d3d=create(D3D_SDK_VERSION);
    CHECK(d3d!=nullptr);
    IDirect3D9* qi=nullptr;
    CHECK(SUCCEEDED(d3d->QueryInterface(IID_IDirect3D9,reinterpret_cast<void**>(&qi))));
    CHECK(qi==d3d); qi->Release();
    CHECK(d3d->Release()==0);
    auto status=reinterpret_cast<DWORD (WINAPI*)()>(GetProcAddress(module,"D3DPERF_GetStatus"));
    status();
    FreeLibrary(module);
    puts("x86 exports, static runtime imports, lazy DLL load and COM identity passed");
    return 0;
}
