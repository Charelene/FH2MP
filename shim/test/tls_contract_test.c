/*
 * Reproduces the checks azimuth.exe makes on its static-TLS helper (FUN_140092150):
 * LoadLibraryExW(path, 0, 0x900), both exports present, index != -1,
 * attached == current thread id. Usage: tls_contract_test.exe <path\to\azimuth-tls.dll>
 */
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>

int wmain(int argc, wchar_t **argv)
{
    HMODULE m;
    const int *index;
    const DWORD *attached;
    PIMAGE_DOS_HEADER dos;
    PIMAGE_NT_HEADERS64 nt;
    PIMAGE_DATA_DIRECTORY tlsdir;

    if (argc < 2) {
        fprintf(stderr, "usage: tls_contract_test <dll>\n");
        return 2;
    }
    m = LoadLibraryExW(argv[1], NULL, 0x900);
    if (!m) {
        fprintf(stderr, "FAIL load (%lu)\n", GetLastError());
        return 1;
    }
    index = (const int *)GetProcAddress(m, "azimuth_tls_index");
    attached = (const DWORD *)GetProcAddress(m, "azimuth_tls_attached");
    if (!index || !attached) {
        fprintf(stderr, "FAIL exports index=%p attached=%p\n", (void *)index, (void *)attached);
        return 1;
    }
    printf("tls_index=%d attached=%lu current=%lu\n", *index, (unsigned long)*attached,
           (unsigned long)GetCurrentThreadId());
    if (*index == -1 || *attached != GetCurrentThreadId()) {
        fprintf(stderr, "FAIL contract\n");
        return 1;
    }
    dos = (PIMAGE_DOS_HEADER)m;
    nt = (PIMAGE_NT_HEADERS64)((char *)m + dos->e_lfanew);
    tlsdir = &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS];
    if (tlsdir->VirtualAddress) {
        PIMAGE_TLS_DIRECTORY64 t = (PIMAGE_TLS_DIRECTORY64)((char *)m + tlsdir->VirtualAddress);
        unsigned long long size = t->EndAddressOfRawData - t->StartAddressOfRawData;
        printf("tls raw size=0x%llx zerofill=0x%lx characteristics=0x%lx\n", size,
               (unsigned long)t->SizeOfZeroFill, (unsigned long)t->Characteristics);
        if (size + t->SizeOfZeroFill < 0x150) {
            fprintf(stderr, "FAIL tls block smaller than 0x150\n");
            return 1;
        }
    } else {
        fprintf(stderr, "FAIL no TLS directory\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
