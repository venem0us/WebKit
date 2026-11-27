/*
 * Copyright (C) 2026 Glass. All rights reserved.
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
 * THIS SOFTWARE IS PROVIDED BY THE AUTHORS ``AS IS'' AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHORS OR
 * CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL,
 * EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO,
 * PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR
 * PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY
 * OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#if OS(WINDOWS)

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <dbghelp.h>
#include <exception>
#include <windows.h>

namespace WebKit {

// Glass: a crash of a child process (web content, network or GPU) leaves a small minidump, threads, thread state and
// the module list, no heap, as <process>-<kind>-<pid>-<tick>.dmp in the folder the embedder names in GLASS_CRASH_DIR
// (or, as before, next to GST_DEBUG_FILE while video stats are recorded). Without either, nothing is written. The
// embedder decides whether a dump ever leaves the PC.
namespace GlassCrashDumps {

inline wchar_t* directory()
{
    static wchar_t value[MAX_PATH];
    return value;
}

inline const wchar_t*& processName()
{
    static const wchar_t* value = L"process";
    return value;
}

inline void writeFile(EXCEPTION_POINTERS* exception, const wchar_t* kind)
{
    static LONG writing = 0;
    if (InterlockedExchange(&writing, 1))
        return; // one dump per process: a second thread crashing meanwhile adds nothing
    using MiniDumpWriteDumpFunction = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
    HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll");
    auto writeDump = dbghelp ? reinterpret_cast<MiniDumpWriteDumpFunction>(GetProcAddress(dbghelp, "MiniDumpWriteDump")) : nullptr;
    if (!writeDump)
        return;
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s%s-%s-%lu-%llu.dmp", directory(), processName(), kind, GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()));
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    MINIDUMP_EXCEPTION_INFORMATION information { GetCurrentThreadId(), exception, FALSE };
    writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, static_cast<MINIDUMP_TYPE>(MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules), &information, nullptr, nullptr);
    CloseHandle(file);
}

inline LONG WINAPI unhandledExceptionFilter(EXCEPTION_POINTERS* exception)
{
    writeFile(exception, L"crash");
    return EXCEPTION_CONTINUE_SEARCH;
}

// abort(), an invalid CRT parameter and std::terminate end the process with a fail-fast (C0000409), which skips the
// unhandled-exception filter: dump the calling thread from their handlers instead.
inline void writeCurrentThread(const wchar_t* kind)
{
    CONTEXT context { };
    RtlCaptureContext(&context);
    EXCEPTION_RECORD record { };
    record.ExceptionCode = 0xC0000409; // STATUS_STACK_BUFFER_OVERRUN, the fail-fast code Windows reports
    record.ExceptionAddress = reinterpret_cast<void*>(context.Rip);
    EXCEPTION_POINTERS pointers { &record, &context };
    writeFile(&pointers, kind);
}

inline void abortSignalHandler(int)
{
    writeCurrentThread(L"abort");
}

inline void invalidParameterHandler(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t)
{
    writeCurrentThread(L"invalid-parameter");
    std::abort();
}

inline void terminateHandler()
{
    writeCurrentThread(L"terminate");
    std::abort();
}

inline bool chooseDirectory()
{
    if (const wchar_t* folder = _wgetenv(L"GLASS_CRASH_DIR"); folder && *folder) {
        wcsncpy_s(directory(), MAX_PATH, folder, _TRUNCATE);
        size_t length = wcslen(directory());
        if (length && directory()[length - 1] != L'\\' && length + 1 < MAX_PATH)
            wcscat_s(directory(), MAX_PATH, L"\\");
        return true;
    }
    const wchar_t* debugFile = _wgetenv(L"GST_DEBUG_FILE");
    if (!debugFile || !*debugFile)
        return false;
    wcsncpy_s(directory(), MAX_PATH, debugFile, _TRUNCATE);
    wchar_t* slash = wcsrchr(directory(), L'\\');
    if (!slash)
        return false;
    slash[1] = L'\0';
    return true;
}

} // namespace GlassCrashDumps

// Call once, early in the process's main function. `name` is part of each dump's file name ("web-process").
inline void installGlassCrashDumpWriter(const wchar_t* name)
{
    if (!GlassCrashDumps::chooseDirectory())
        return;
    GlassCrashDumps::processName() = name;
    SetUnhandledExceptionFilter(GlassCrashDumps::unhandledExceptionFilter);
    std::signal(SIGABRT, GlassCrashDumps::abortSignalHandler);
    _set_invalid_parameter_handler(GlassCrashDumps::invalidParameterHandler);
    std::set_terminate(GlassCrashDumps::terminateHandler);
}

} // namespace WebKit

#endif // OS(WINDOWS)
