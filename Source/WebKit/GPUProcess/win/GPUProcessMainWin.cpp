/*
 * Copyright (C) 2020 Sony Interactive Entertainment Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "config.h"
#include "GPUProcessMain.h"

#if ENABLE(GPU_PROCESS)

#include "AuxiliaryProcessMain.h"
#include "GPUProcess.h"
#include <objbase.h>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <dbghelp.h>
#include <exception>

#if USE(SKIA_OPENTYPE_SVG)
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_BEGIN
#include <skia/core/SkGraphics.h>
#include <skia/modules/svg/SkSVGOpenTypeSVGDecoder.h>
WTF_IGNORE_WARNINGS_IN_THIRD_PARTY_CODE_END
#endif

namespace WebKit {

// Glass: with video stats recording on, a crash of this process (in WebKit, ANGLE or a graphics driver) leaves a small
// minidump next to the stats, gpu-process-crash-<pid>-<tick>.dmp, for its stack: Windows' own reports keep the dump only
// briefly. Stacks and thread state only, no heap.
static wchar_t s_crashDumpDirectory[MAX_PATH];

static void writeCrashDumpFile(EXCEPTION_POINTERS* exception, const wchar_t* kind)
{
    using MiniDumpWriteDumpFunction = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    auto writeDump = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFunction>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    if (!writeDump)
        return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%sgpu-process-%s-%lu-%llu.dmp", s_crashDumpDirectory, kind, GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()));
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION information { GetCurrentThreadId(), exception, FALSE };
        writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules), &information, nullptr, nullptr);
        CloseHandle(file);
    }
}

static LONG WINAPI writeCrashDump(EXCEPTION_POINTERS* exception)
{
    writeCrashDumpFile(exception, L"crash");
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort(), an invalid CRT parameter and std::terminate end the process with a fail-fast (C0000409), which skips the
// unhandled-exception filter: dump the calling thread from their handlers instead.
static void writeCurrentThreadDump(const wchar_t* kind)
{
    CONTEXT context { };
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record { };
    record.ExceptionCode = 0xC0000409; // STATUS_STACK_BUFFER_OVERRUN, the fail-fast code Windows reports
    record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
    EXCEPTION_POINTERS pointers { &record, &context };
    writeCrashDumpFile(&pointers, kind);
}

static void abortSignalHandler(int)
{
    writeCurrentThreadDump(L"abort");
}

static void invalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t)
{
    writeCurrentThreadDump(L"invalid-parameter");
    std::abort();
}

static void terminateHandler()
{
    writeCurrentThreadDump(L"terminate");
    std::abort();
}

static void installCrashDumpWriter()
{
    const wchar_t* debugFile = _wgetenv(L"GST_DEBUG_FILE");
    if (!debugFile || !*debugFile)
        return;
    wcsncpy_s(s_crashDumpDirectory, debugFile, _TRUNCATE);
    wchar_t* slash = wcsrchr(s_crashDumpDirectory, L'\\');
    if (!slash)
        return;
    slash[1] = L'\0';
    SetUnhandledExceptionFilter(writeCrashDump);
    std::signal(SIGABRT, abortSignalHandler);
    _set_invalid_parameter_handler(invalidParameterHandler);
    std::set_terminate(terminateHandler);
}

class GPUProcessMainWin final: public AuxiliaryProcessMainBase<GPUProcess> {
public:
    bool platformInitialize() override
    {
#if USE(SKIA_OPENTYPE_SVG)
        SkGraphics::SetOpenTypeSVGDecoderFactory(SkSVGOpenTypeSVGDecoder::Make);
#endif
        return true;
    }
};

int GPUProcessMain(int argc, char** argv)
{
    // for DirectX
    HRESULT hr = ::CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    RELEASE_ASSERT(SUCCEEDED(hr));
    installCrashDumpWriter();
    return AuxiliaryProcessMain<GPUProcessMainWin>(argc, argv);
}

} // namespace WebKit

#endif // ENABLE(GPU_PROCESS)
