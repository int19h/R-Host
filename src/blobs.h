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
#include "stdafx.h"
#include "util.h"
#include "Rapi.h"

namespace rhost {
    namespace blobs {
        // Converts a RAWSXP or NILSXP object to a vector of bytes. For NILSXP, the vector is empty, and
        // return value is false. Otherwise, the vector contains the same bytes, and return value is true.
        bool to_blob(SEXP sexp, std::vector<char>& blob);

        typedef std::vector<char> blob;

        void append_from_file(blob& blob, const char* path);

        inline void append_from_file(blob& blob, const std::string& path) {
            return append_from_file(blob, path.c_str());
        }
    }
}