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
#pragma warning(disable: 4996)

#define NOMINMAX

#include <atomic>
#include <cstdarg>
#include <cinttypes>
#include <codecvt>
#include <chrono>
#include <condition_variable>
#include <csetjmp>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <future>
#include <iostream>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <tuple>
#include <utility>
#include <unordered_map>
#include <vector>
#include <stdexcept>

#include "boost/algorithm/string.hpp"
#include "boost/date_time/posix_time/posix_time.hpp"
#include "boost/endian/buffers.hpp"
#include "boost/format.hpp"
#include "boost/program_options/cmdline.hpp"
#include "boost/program_options/options_description.hpp"
#include "boost/program_options/value_semantic.hpp"
#include "boost/program_options/variables_map.hpp"
#include "boost/program_options/parsers.hpp"
#include "boost/optional.hpp"
#include "boost/locale.hpp"
#include "boost/signals2/signal.hpp"
#include "boost/uuid/uuid.hpp"
#include "boost/uuid/uuid_io.hpp"
#include "boost/uuid/uuid_generators.hpp"
#include "boost/filesystem.hpp"

#include "picojson.h"
#include "zip.h"

#ifdef _WIN32
#include <io.h>
#include <fcntl.h>
#include <process.h>
#include <windows.h>

#define STRSAFE_NO_DEPRECATE
#include <strsafe.h>

#pragma warning(push)
#pragma warning(disable:4091)
#include <dbghelp.h>
#pragma warning(pop)

#include "minhook.h"
#else
#include <unistd.h>
#endif

#if defined(_MSC_VER)
#define RHOST_EXPORT __declspec(dllexport)
#define RHOST_IMPORT __declspec(dllimport)
#define RHOST_NORETURN __declspec(noreturn)
#elif defined(__GNUC__)
#define RHOST_EXPORT __attribute__((visibility("default")))
#define RHOST_IMPORT
#define RHOST_NORETURN
#else
#define RHOST_EXPORT
#define RHOST_IMPORT
#define RHOST_NORETURN
#pragma warning Unknown DLL import/export.
#endif

#ifdef _WIN32
#define RHOST_MAX_PATH MAX_PATH
#define RHOST_mbstowcs msvcrt::mbstowcs
#define RHOST_wctomb msvcrt::wctomb
#define RHOST_mbtowc msvcrt::mbtowc
#define RHOST_calloc msvcrt::calloc
#define RHOST_vsnprintf msvcrt::vsnprintf
#else

#define RHOST_MAX_PATH PATH_MAX
#define RHOST_mbstowcs std::mbstowcs
#define RHOST_wctomb std::wctomb
#define RHOST_mbtowc std::mbtowc
#define RHOST_calloc calloc
#define RHOST_vsnprintf vsnprintf

#define vsprintf_s vsprintf
void strcpy_s(char* dest, size_t n, char const* source) {
    strcpy(dest, source);
}

void memcpy_s(void* const dest, size_t const destSize, void const* const source, size_t const sourceSize) {
    memcpy(dest, source, sourceSize);
}

#endif

namespace fs = boost::filesystem;

#define RHOST_BITMASK_OPS(Ty) \
inline Ty& operator&=(Ty& _Left, Ty _Right) { _Left = (Ty)((int)_Left & (int)_Right); return (_Left); } \
inline Ty& operator|=(Ty& _Left, Ty _Right) { _Left = (Ty)((int)_Left | (int)_Right); return (_Left); } \
inline Ty& operator^=(Ty& _Left, Ty _Right) { _Left = (Ty)((int)_Left ^ (int)_Right); return (_Left); } \
inline constexpr Ty operator&(Ty _Left, Ty _Right) { return ((Ty)((int)_Left & (int)_Right)); } \
inline constexpr Ty operator|(Ty _Left, Ty _Right) { return ((Ty)((int)_Left | (int)_Right)); } \
inline constexpr Ty operator^(Ty _Left, Ty _Right) { return ((Ty)((int)_Left ^ (int)_Right)); } \
inline constexpr Ty operator~(Ty _Left) { return ((Ty)~(int)_Left); }
