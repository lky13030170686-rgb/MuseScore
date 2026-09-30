/*
 * SPDX-License-Identifier: GPL-3.0-only
 * MuseScore-Studio-CLA-applies
 *
 * MuseScore Studio
 * Music Composition & Notation
 *
 * Copyright (C) 2021 MuseScore Limited and others
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <csignal>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <typeinfo>

#include <QApplication>
#include <QStyleHints>
#include <QQuickWindow>
#include <QSslSocket>

#include "appfactory.h"
#include "internal/commandlineparser.h"
#include "global/iapplication.h"

#include "muse_framework_config.h"
#include "app_config.h"

#include "log.h"

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

// C++20 check
// #include <concepts>
// #include <type_traits>
// consteval int square(int n) { return n * n; }
// static_assert(square(5) == 25);
// ========================

#ifndef MUSE_MODULE_DIAGNOSTICS_CRASHPAD_CLIENT

//! Prints a symbolized stack for the faulting thread.
//!
//! Why this exists: a bare "signal: [11]" tells you that something went wrong and nothing about
//! where. On Windows the minidumps land in %LOCALAPPDATA%\CrashDumps, but reading them needs a
//! debugger, which a development machine does not necessarily have. CaptureStackBackTrace plus
//! DbgHelp gets the same information into the log the crash is already reported in.
//!
//! DbgHelp is loaded dynamically rather than linked, so this stays a local change in the app
//! entry point and does not add a library to the build. Symbols come from the .pdb next to the
//! executable, or from the directory named by MUSE_SYMBOL_PATH (the build tree keeps the .pdb
//! next to the build output, not next to the installed binary).
#ifdef Q_OS_WIN
//! Crash lines also go to a file next to the executable.
//!
//! Why: a crash report is only useful if it survives the crash, and it does not always.
//! Started from the desktop shortcut the process has no stderr at all, and the log can be cut
//! off mid-write when the process dies. This uses plain kernel calls (no C++ streams, no
//! allocation, no logging) so it is safe to call from a signal handler, and it appends, so
//! several crashes accumulate in one place: <exe dir>\dsh-crash.txt, or %TEMP% when the install
//! directory is not writable.
static void appendCrashFile(const char* text)
{
    char path[MAX_PATH] = {};
    if (!GetModuleFileNameA(nullptr, path, MAX_PATH)) {
        return;
    }

    char file[MAX_PATH + 32] = {};
    const char* lastSlash = strrchr(path, '\\');
    if (lastSlash) {
        const size_t dirLen = static_cast<size_t>(lastSlash - path) + 1;
        if (dirLen + 16 < sizeof(file)) {
            memcpy(file, path, dirLen);
            strcpy(file + dirLen, "dsh-crash.txt");
        }
    }

    HANDLE handle = INVALID_HANDLE_VALUE;
    if (file[0]) {
        handle = CreateFileA(file, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    }
    if (handle == INVALID_HANDLE_VALUE) {
        char temp[MAX_PATH] = {};
        if (GetTempPathA(MAX_PATH, temp)) {
            strncat(temp, "dsh-crash.txt", sizeof(temp) - strlen(temp) - 1);
            handle = CreateFileA(temp, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        }
    }
    if (handle == INVALID_HANDLE_VALUE) {
        return;
    }

    DWORD written = 0;
    WriteFile(handle, text, static_cast<DWORD>(strlen(text)), &written, nullptr);
    CloseHandle(handle);
}

//! printf-style helper for the crash path: formats into a stack buffer, writes it to the crash
//! file and to stderr. No allocation, so it is safe inside a fault handler.
static void crashLogFmt(const char* format, ...)
{
    char buffer[2048] = {};

    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer) - 1, format, args);
    va_end(args);

    fputs(buffer, stderr);
    fflush(stderr);
    appendCrashFile(buffer);
}

//! DbgHelp, loaded on demand rather than linked, so this stays a local change in the app entry
//! point and does not add a library to the build. Symbols come from the .pdb next to the
//! executable, or from the directory named by MUSE_SYMBOL_PATH (the build tree keeps the .pdb
//! next to the build output, not next to the installed binary).
struct DbgHelpApi
{
    decltype(&SymSetOptions) setOptions = nullptr;
    decltype(&SymInitialize) initialize = nullptr;
    decltype(&SymFromAddr) fromAddr = nullptr;
    decltype(&SymGetLineFromAddr64) getLine = nullptr;
    decltype(&StackWalk64) stackWalk = nullptr;
    decltype(&SymFunctionTableAccess64) functionTableAccess = nullptr;
    decltype(&SymGetModuleBase64) getModuleBase = nullptr;

    bool valid() const
    {
        return setOptions && initialize && fromAddr && getLine && stackWalk && functionTableAccess && getModuleBase;
    }
};

static DbgHelpApi& dbghelpApi()
{
    static DbgHelpApi api = [] {
        DbgHelpApi a;
        HMODULE module = LoadLibraryA("dbghelp.dll");
        if (!module) {
            return a;
        }

        a.setOptions = reinterpret_cast<decltype(a.setOptions)>(GetProcAddress(module, "SymSetOptions"));
        a.initialize = reinterpret_cast<decltype(a.initialize)>(GetProcAddress(module, "SymInitialize"));
        a.fromAddr = reinterpret_cast<decltype(a.fromAddr)>(GetProcAddress(module, "SymFromAddr"));
        a.getLine = reinterpret_cast<decltype(a.getLine)>(GetProcAddress(module, "SymGetLineFromAddr64"));
        a.stackWalk = reinterpret_cast<decltype(a.stackWalk)>(GetProcAddress(module, "StackWalk64"));
        a.functionTableAccess = reinterpret_cast<decltype(a.functionTableAccess)>(GetProcAddress(module, "SymFunctionTableAccess64"));
        a.getModuleBase = reinterpret_cast<decltype(a.getModuleBase)>(GetProcAddress(module, "SymGetModuleBase64"));
        if (!a.valid()) {
            return DbgHelpApi();
        }

        char exePath[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, exePath, MAX_PATH);
        std::string searchPath(exePath);
        const size_t slash = searchPath.find_last_of("\\/");
        searchPath = slash == std::string::npos ? std::string(".") : searchPath.substr(0, slash);
        if (const char* extra = std::getenv("MUSE_SYMBOL_PATH")) {
            searchPath += ";";
            searchPath += extra;
        }

        a.setOptions(SYMOPT_UNDNAME | SYMOPT_LOAD_LINES | SYMOPT_DEFERRED_LOADS);
        a.initialize(GetCurrentProcess(), searchPath.c_str(), TRUE);
        return a;
    }();

    return api;
}

static void printFrame(int index, DWORD64 address)
{
    DbgHelpApi& api = dbghelpApi();
    if (!api.valid()) {
        fprintf(stderr, "  #%d 0x%llX\n", index, static_cast<unsigned long long>(address));
        return;
    }

    char symbolBuffer[sizeof(SYMBOL_INFO) + MAX_SYM_NAME * sizeof(char)] = {};
    SYMBOL_INFO* symbol = reinterpret_cast<SYMBOL_INFO*>(symbolBuffer);
    symbol->SizeOfStruct = sizeof(SYMBOL_INFO);
    symbol->MaxNameLen = MAX_SYM_NAME;

    DWORD64 displacement = 0;
    if (!api.fromAddr(GetCurrentProcess(), address, &displacement, symbol)) {
        crashLogFmt("  #%d 0x%llX (no symbol)\n", index, static_cast<unsigned long long>(address));
        return;
    }

    IMAGEHLP_LINE64 line = {};
    line.SizeOfStruct = sizeof(IMAGEHLP_LINE64);
    DWORD lineDisplacement = 0;
    if (api.getLine(GetCurrentProcess(), address, &lineDisplacement, &line)) {
        crashLogFmt("  #%d 0x%llX %s+0x%llX (%s:%lu)\n", index, static_cast<unsigned long long>(address),
                    symbol->Name, static_cast<unsigned long long>(displacement), line.FileName, line.LineNumber);
    } else {
        crashLogFmt("  #%d 0x%llX %s+0x%llX\n", index, static_cast<unsigned long long>(address),
                    symbol->Name, static_cast<unsigned long long>(displacement));
    }
}

//! Unwinds from the faulting context. CaptureStackBackTrace starts at the handler and loses the
//! frames that matter (the SEH dispatcher sits between), so a crash that has a real caller gets
//! its stack walked from the context the exception arrived with.
static void logStackFromContext(CONTEXT* context)
{
    DbgHelpApi& api = dbghelpApi();
    if (!api.valid()) {
        return;
    }

    STACKFRAME64 frame = {};
    frame.AddrPC.Offset = context->Rip;
    frame.AddrPC.Mode = AddrModeFlat;
    frame.AddrFrame.Offset = context->Rbp;
    frame.AddrFrame.Mode = AddrModeFlat;
    frame.AddrStack.Offset = context->Rsp;
    frame.AddrStack.Mode = AddrModeFlat;

    for (int i = 0; i < 64; ++i) {
        if (!api.stackWalk(IMAGE_FILE_MACHINE_AMD64, GetCurrentProcess(), GetCurrentThread(), &frame, context,
                           nullptr, api.functionTableAccess, api.getModuleBase, nullptr)) {
            break;
        }
        if (frame.AddrPC.Offset == 0) {
            break;
        }
        printFrame(i, frame.AddrPC.Offset);
    }
}

//! Logs the fault itself: the signal handler cannot say where the access violation happened,
//! this can. The exception keeps going afterwards, so the existing crash path is unchanged.
//!
//! Deliberately does no symbol lookup: this runs inside a fault on whatever thread faulted --
//! including realtime audio threads -- and loading a symbol file there (SymInitialize with
//! invadeProcess suspends every thread) is heavy enough to break the app. Addresses are logged
//! raw and symbolized afterwards, off the crash path.
static LONG WINAPI logFaultingException(EXCEPTION_POINTERS* info)
{
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_ILLEGAL_INSTRUCTION
        && code != EXCEPTION_INT_DIVIDE_BY_ZERO && code != EXCEPTION_STACK_OVERFLOW) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    crashLogFmt("fault: code=0x%08lX at 0x%llX rip=0x%llX rsp=0x%llX thread=%lu\n", code,
                static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info->ExceptionRecord->ExceptionAddress)),
                static_cast<unsigned long long>(info->ContextRecord->Rip),
                static_cast<unsigned long long>(info->ContextRecord->Rsp),
                GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        crashLogFmt("  %s of address 0x%llX\n",
                    info->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                    static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]));
    }

    void* frames[48] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, 48, frames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        crashLogFmt("  raw #%u 0x%llX\n", i, static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(frames[i])));
    }

    return EXCEPTION_CONTINUE_SEARCH;
}

//! Prints a symbolized stack for the faulting thread.
static void logCrashStack()
{
    void* frames[64] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, 64, frames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        printFrame(i, reinterpret_cast<DWORD64>(frames[i]));
    }
}
#else
static void crashLogFmt(const char* format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fflush(stderr);
}

static void logCrashStack()
{
}
#endif

static void crashCallback(int signum)
{
    const char* signame = "UNKNOWN SIGNAME";
    const char* sigdescript = "";
    switch (signum) {
    case SIGILL:
        signame = "SIGILL";
        sigdescript = "Illegal Instruction";
        break;
    case SIGSEGV:
        signame = "SIGSEGV";
        sigdescript =  "Invalid memory reference";
        break;
    }
    LOGE() << "Oops! Application crashed with signal: [" << signum << "] " << signame << "-" << sigdescript;
#ifdef Q_OS_WIN
    crashLogFmt("signal: [%d] %s - %s (thread %lu)\n", signum, signame, sigdescript, GetCurrentThreadId());
#else
    crashLogFmt("signal: [%d] %s - %s\n", signum, signame, sigdescript);
#endif
    logCrashStack();
    exit(EXIT_FAILURE);
}

//! An exception that nothing catches ends in std::terminate, and on Windows that leaves a
//! minidump whose exception code (0xC0000409, fast-fail 7) says "the app called abort" and
//! nothing about why. The type and message of the exception are still available here, and the
//! stack is still the one that threw -- terminate runs before unwinding -- so this is the one
//! place where that crash can explain itself.
static void terminateCallback()
{
    // To the crash file and stderr as well as the log: this runs while the process is being torn
    // down, and the log does not always survive that. The crash file is what makes a report from
    // a normal (shortcut) launch usable -- there is no stderr in that case.
    crashLogFmt("terminate: uncaught exception\n");

    if (std::current_exception()) {
        try {
            std::rethrow_exception(std::current_exception());
        } catch (const std::exception& e) {
            crashLogFmt("  type: %s\n  what(): %s\n", typeid(e).name(), e.what());
            LOGE() << "  type: " << typeid(e).name();
            LOGE() << "  what(): " << e.what();
        } catch (...) {
            crashLogFmt("  type: not derived from std::exception\n");
            LOGE() << "  type: not derived from std::exception";
        }
    } else {
        crashLogFmt("  no active exception\n");
        LOGE() << "  no active exception";
    }

    logCrashStack();
    abort();
}

#endif

static void app_init_qrc()
{
    Q_INIT_RESOURCE(app);

#ifdef Q_OS_WIN
    Q_INIT_RESOURCE(app_win);
#endif
}

int main(int argc, char** argv)
{
#ifndef MUSE_MODULE_DIAGNOSTICS_CRASHPAD_CLIENT
    signal(SIGSEGV, crashCallback);
    signal(SIGILL, crashCallback);
    signal(SIGFPE, crashCallback);
    std::set_terminate(terminateCallback);
#ifdef Q_OS_WIN
    AddVectoredExceptionHandler(1 /*first*/, logFaultingException);
#endif
#endif

    // ====================================================
    // Setup global Qt application variables
    // ====================================================

    app_init_qrc();

    qputenv("QT_STYLE_OVERRIDE", "Fusion");
    qputenv("QML_DISABLE_DISK_CACHE", "true");

    // HACK: Workaround for crash #28840. This disables the incremental GC
    if (!qEnvironmentVariableIsSet("MU_QV4_GC_TIMELIMIT")) {
        qputenv("QV4_GC_TIMELIMIT", "0");
    }

    if (!qEnvironmentVariableIsSet("QT_QUICK_FLICKABLE_WHEEL_DECELERATION")) {
        qputenv("QT_QUICK_FLICKABLE_WHEEL_DECELERATION", "5000");
    }

#ifdef Q_OS_LINUX
    if (qEnvironmentVariable("MU_QT_QPA_PLATFORM") != "offscreen") {
        qputenv("QT_QPA_PLATFORMTHEME", "gtk3");
    }

    //! NOTE Forced X11, with Wayland there are a number of problems now
    if (qEnvironmentVariable("MU_QT_QPA_PLATFORM") == "") {
        qputenv("QT_QPA_PLATFORM", "xcb");
    }
#endif

#ifdef Q_OS_WIN
    // NOTE: There are some problems with rendering the application window on some integrated graphics processors
    //       see https://github.com/musescore/MuseScore/issues/8270
    if (!qEnvironmentVariableIsSet("QT_OPENGL_BUGLIST")) {
        qputenv("QT_OPENGL_BUGLIST", ":/resources/win_opengl_buglist.json");
    }
#endif

    QGuiApplication::styleHints()->setMousePressAndHoldInterval(250);

#ifdef MUSE_APP_UNSTABLE
    QCoreApplication::setApplicationName(MUSE_APP_NAME_MACHINE_READABLE MUSE_APP_VERSION_MAJOR "Development");
#else
    QCoreApplication::setApplicationName(MUSE_APP_NAME_MACHINE_READABLE MUSE_APP_VERSION_MAJOR);
#endif
    QCoreApplication::setOrganizationName("MuseScore");
    QCoreApplication::setOrganizationDomain("musescore.org");
    QCoreApplication::setApplicationVersion(MUSE_APP_VERSION);

#if !defined(Q_OS_WIN) && !defined(Q_OS_DARWIN) && !defined(Q_OS_WASM)
    // Any OS that uses Freedesktop.org Desktop Entry Specification (e.g. Linux, BSD)
#ifndef MUSE_APP_INSTALL_SUFFIX
#define MUSE_APP_INSTALL_SUFFIX ""
#endif
    QGuiApplication::setDesktopFileName("org.musescore.MuseScore" MUSE_APP_INSTALL_SUFFIX);
#endif

    using namespace muse;
    using namespace mu::app;

    auto fixSslBackend = []() {
#ifdef Q_OS_WIN
        // NOTE: Force schannel backend. Qt prefers OpenSSL when qopensslbackend.dll
        //       is present, which can crash on ABI mismatch with bundled OpenSSL.
        //       see https://github.com/musescore/MuseScore/issues/33401
        QSslSocket::setActiveBackend("schannel");
#endif
    };

    // ====================================================
    // Parse command line options
    // ====================================================
#ifdef MUE_ENABLE_CONSOLEAPP
    CommandLineParser commandLineParser;
    commandLineParser.init();
    commandLineParser.parse(argc, argv);

    IApplication::RunMode runMode = commandLineParser.runMode();
    QCoreApplication* qapp = nullptr;

    if (runMode == IApplication::RunMode::AudioPluginRegistration) {
        qapp = new QCoreApplication(argc, argv);
    } else {
        qapp = new QApplication(argc, argv);
    }

    fixSslBackend();

    commandLineParser.processBuiltinArgs(*qapp);
    std::shared_ptr<MuseScoreCmdOptions> opt = commandLineParser.options();

#else
    QCoreApplication* qapp = new QApplication(argc, argv);

    fixSslBackend();

    std::shared_ptr<MuseScoreCmdOptions> opt = std::make_shared<MuseScoreCmdOptions>();
    opt->runMode = IApplication::RunMode::GuiApp;
#endif

    // ====================================================
    // Setup application
    // ====================================================

    //! NOTE: We immediately launch the application's event loop
    // to be able to show a splash screen (on Linux, splash screen won't show without event loop).
    // All subsequent initialization steps will be executed as events in the event loop.

    std::shared_ptr<muse::IApplication> app;
    QMetaObject::invokeMethod(qapp, [qapp, &app, &opt]() {
        AppFactory f;
        app = f.newApp(opt);
        IF_ASSERT_FAILED(app) {
            return;
        }
        app->showSplash();
        QMetaObject::invokeMethod(qapp, [qapp, &app]() {
            app->setup();
            QMetaObject::invokeMethod(qapp, [&app]() {
                app->setupNewContext();

                LOGI() << QString("SSL Info: supported: %1, build: %2, runtime: %3, active backend: %4, available backends: %5")
                    .arg(QSslSocket::supportsSsl())
                    .arg(QSslSocket::sslLibraryBuildVersionString())
                    .arg(QSslSocket::sslLibraryVersionString())
                    .arg(QSslSocket::activeBackend())
                    .arg(QSslSocket::availableBackends().join(", "));
            }, Qt::QueuedConnection);
        }, Qt::QueuedConnection);
    }, Qt::QueuedConnection);

    // ====================================================
    // Run main loop
    // ====================================================
    int code = qapp->exec();

    // ====================================================
    // Quit
    // ====================================================

    if (app) {
        app->finish();
    }

    delete qapp;

    LOGI() << "Goodbye!! code: " << code;
    return code;
}
