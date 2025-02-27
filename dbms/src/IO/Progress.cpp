// Copyright 2023 PingCAP, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "Progress.h"

#include <IO/ReadBuffer.h>
#include <IO/ReadHelpers.h>
#include <IO/WriteBuffer.h>
#include <IO/WriteHelpers.h>


namespace DB
{
void ProgressValues::read(ReadBuffer & in)
{
    size_t new_rows = 0;
    size_t new_bytes = 0;
    size_t new_total_rows = 0;

    readVarUInt(new_rows, in);
    readVarUInt(new_bytes, in);
    readVarUInt(new_total_rows, in);

    rows = new_rows;
    bytes = new_bytes;
    total_rows = new_total_rows;
}


void ProgressValues::write(WriteBuffer & out) const
{
    writeVarUInt(rows, out);
    writeVarUInt(bytes, out);
    writeVarUInt(total_rows, out);
}


void Progress::read(ReadBuffer & in)
{
    ProgressValues values{};
    values.read(in);

    rows.store(values.rows, std::memory_order_relaxed);
    bytes.store(values.bytes, std::memory_order_relaxed);
    total_rows.store(values.total_rows, std::memory_order_relaxed);
}


void Progress::write(WriteBuffer & out) const
{
    getValues().write(out);
}

} // namespace DB
