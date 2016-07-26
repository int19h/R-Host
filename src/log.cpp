/* ****************************************************************************
 *
 * Copyright (c) Microsoft Corporation. All rights reserved. 
 *
 *
 * This file is part of Microsoft R Host.
 * 
 * Microsoft R Host is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * Microsoft R Host is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Microsoft R Host.  If not, see <http://www.gnu.org/licenses/>.
 *
 * ***************************************************************************/

#pragma once
#include "log.h"
#include "Rapi.h"

using namespace std::literals;


namespace rhost {
    namespace log {
        namespace {
#ifdef WIN32
            const MINIDUMP_TYPE fulldump_type = MINIDUMP_TYPE(
                MiniDumpWithFullMemory |
                MiniDumpWithDataSegs |
                MiniDumpWithHandleData |
                MiniDumpWithProcessThreadData |
                MiniDumpWithFullMemoryInfo |
                MiniDumpWithThreadInfo |
                MiniDumpIgnoreInaccessibleMemory |
                MiniDumpWithTokenInformation |
                MiniDumpWithModuleHeaders);

            const DWORD fatal_error_exception_code = 0xE0000001;
#endif

            std::mutex log_mutex, terminate_mutex;
            std::string log_filename, stackdump_filename, fulldump_filename;
            FILE* logfile;
            int indent;

            void log_flush_thread() {
                for (;;) {
                    std::this_thread::sleep_for(1s);
                    flush_log();
                }
            }
        }

#ifdef WIN32
        void create_minidump(_EXCEPTION_POINTERS* ei) {
            // Don't let another thread interrupt us by terminating while we're doing this.
            std::lock_guard<std::mutex> terminate_lock(terminate_mutex);

            MINIDUMP_EXCEPTION_INFORMATION mei;
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = ei;
            mei.ClientPointers = FALSE;

            // Create a regular minidump.
            HANDLE dump_file = CreateFileA(stackdump_filename.c_str(), GENERIC_ALL, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump_file, MiniDumpNormal, &mei, nullptr, nullptr)) {
                logf("Stack-only minidump written out to %s\n", stackdump_filename.c_str());
            } else {
                logf("Failed to write stack-only minidump to %s\n", stackdump_filename.c_str());
            }
            CloseHandle(dump_file);
            flush_log();

            // Create a full heap minidump with as much data as possible.
            dump_file = CreateFileA(fulldump_filename.c_str(), GENERIC_ALL, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), dump_file, fulldump_type, &mei, nullptr, nullptr)) {
                logf("Full minidump written out to %s\n", fulldump_filename.c_str());
            } else {
                logf("Failed to write full minidump to %s\n", fulldump_filename.c_str());
            }
            CloseHandle(dump_file);
            flush_log();
        }

        LONG WINAPI unhandled_exception_filter(_EXCEPTION_POINTERS* ei) {
            // Prevent recursion if an unhandled exception happens inside the filter itself.
            static bool in_unhandled_exception_filter;
            if (in_unhandled_exception_filter) {
                return EXCEPTION_CONTINUE_SEARCH;
            }
            in_unhandled_exception_filter = true;

            // Flush log, so that if anything below fails (e.g. if heap is corrupted too badly, or
            // if we're out of memory), at least the stuff that's already in the log gets written
            flush_log();

            logf("Terminating process due to unhandled Win32 exception 0x%x\n", ei->ExceptionRecord->ExceptionCode);
            flush_log();
            create_minidump(ei);

            in_unhandled_exception_filter = false;
            return EXCEPTION_CONTINUE_SEARCH;
        }
#endif

        void init_log(const std::string& log_suffix) {
            {
                std::string filename;
                filename.resize(MAX_PATH);
                GetTempPathA(static_cast<DWORD>(filename.size()), &filename[0]);
                filename.resize(strlen(filename.c_str()));
                filename += "/Microsoft.R.Host_";

                if (!log_suffix.empty()) {
                    filename += log_suffix + "_";
                }

                time_t t;
                time(&t);

                tm tm;
                localtime_s(&tm, &t);

                size_t len = filename.size();
                filename.resize(len + 1 + MAX_PATH);
                auto it = filename.begin() + len;
                strftime(&*it, filename.end() - it, "%Y%m%d_%H%M%S", &tm);
                filename.resize(strlen(filename.c_str()));

                // Add PID to prevent conflicts in case two hosts with the same suffix
                // get started at the same time.
                filename += "_pid" + std::to_string(getpid());

                log_filename = filename + ".log";
                stackdump_filename = filename + ".stack.dmp";
                fulldump_filename = filename + ".full.dmp";
            }
        
            logfile = _fsopen(log_filename.c_str(), "wc", _SH_DENYWR);
            if (logfile) {
                // Logging happens often, so use a large buffer to avoid hitting the disk all the time.
                setvbuf(logfile, nullptr, _IOFBF, 0x100000);

                // Start a thread that will flush the buffer periodically.
                std::thread(log_flush_thread).detach();
            } else {
                std::string error = "Error creating logfile: " + std::string(log_filename) + "\r\n";
                fputs(error.c_str(), stderr);
                MessageBoxA(HWND_DESKTOP, error.c_str(), "Microsoft R Host", MB_OK | MB_ICONWARNING);
            }

#ifdef WIN32
            SetUnhandledExceptionFilter(unhandled_exception_filter);
#endif
        }

        void vlogf(const char* format, va_list va) {
            std::lock_guard<std::mutex> lock(log_mutex);

#ifndef NDEBUG
            va_list va2;
            va_copy(va2, va);
#endif

            if (logfile) {
                for (int i = 0; i < indent; ++i) {
                    fputc('\t', logfile);
                }
                vfprintf(logfile, format, va);

#ifndef NDEBUG
                // In Debug builds, flush on every write so that log is always up-to-date.
                // In Release builds, we rely on flush_log being called on process shutdown.
                fflush(logfile);
#endif
            }

#ifndef NDEBUG
            for (int i = 0; i < indent; ++i) {
                fputc('\t', stderr);
            }
            vfprintf(stderr, format, va2);
            va_end(va2);
#endif
        }

        void logf(const char* format, ...) {
            va_list va;
            va_start(va, format);
            vlogf(format, va);
            va_end(format);
        }

        void indent_log(int n) {
            indent += n;
            if (indent < 0) {
                indent = 0;
            }
        }

        void flush_log() {
            std::lock_guard<std::mutex> lock(log_mutex);
            if (logfile) {
                fflush(logfile);
            }
        }


        void terminate(bool unexpected, const char* format, va_list va) {
            std::lock_guard<std::mutex> terminate_lock(terminate_mutex);

            char message[0xFFFF];
            vsprintf_s(message, format, va);

            if (unexpected) {
                logf("Fatal error: ");
            }
            logf("%s\n", message);
            flush_log();

            if (unexpected) {
                std::string msgbox_text;
                for (int i = 0; i < strlen(message); ++i) {
                    char c = message[i];
                    if (c == '\n') {
                        msgbox_text += '\r';
                    } 
                    msgbox_text += c;
                }
                
                //MessageBoxA(HWND_DESKTOP, msgbox_text.c_str(), "Microsoft R Host Process fatal error", MB_OK | MB_ICONERROR);

                // Raise and catch an exception so that minidump with a stacktrace can be produced from it.
                [&] {
                    terminate_mutex.unlock();
                    __try {
                        RaiseException(fatal_error_exception_code, 0, 0, nullptr);
                    } __except(unhandled_exception_filter(GetExceptionInformation()), EXCEPTION_CONTINUE_EXECUTION) {
                    }
                    terminate_mutex.lock();
                }();
            }
            
            R_Suicide(message);
        }

        void terminate(const char* format, ...) {
            va_list va;
            va_start(va, format);
            terminate(false, format, va);
            va_end(format);
        }

        void fatal_error(const char* format, ...) {
            va_list va;
            va_start(va, format);
            terminate(true, format, va);
            va_end(format);
        }
    }
}
