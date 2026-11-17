/*
 *  nextpnr -- Next Generation Place and Route
 *
 *  Copyright (C) 2026  nextpnr contributors
 *
 *  Permission to use, copy, modify, and/or distribute this software for any
 *  purpose with or without fee is hereby granted, provided that the above
 *  copyright notice and this permission notice appear in all copies.
 *
 *  THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 *  WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 *  MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 *  ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 *  WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 *  ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 *  OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 */

#include <cstdio>
#include <fstream>
#include <iterator>
#include "gtest/gtest.h"
#include "log.h"
#include "nextpnr.h"
#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

USING_NEXTPNR_NAMESPACE

static int process_id()
{
#ifdef _WIN32
    return _getpid();
#else
    return getpid();
#endif
}

// Every output file of nextpnr is opened with open_ofstream_and_log_error() and has to be closed with
// close_ofstream_and_log_error(): a failed write only sets the stream's state, which the destructor ignores.
TEST(OutputFileTest, a_write_that_fails_is_an_error_when_the_file_is_closed)
{
    // /dev/full accepts the open and fails every write with ENOSPC
    if (!std::ofstream("/dev/full"))
        GTEST_SKIP() << "there is no /dev/full";
    auto out = open_ofstream_and_log_error("/dev/full", "test file");
    out << "contents that cannot be written";
    EXPECT_THROW(close_ofstream_and_log_error(out, "/dev/full", "test file"), log_execution_error_exception);
}

TEST(OutputFileTest, a_file_that_is_written_closes_without_an_error)
{
    // one file per process: the test binaries of several microarchitectures may run at the same time
    const std::string filename =
            ::testing::TempDir() + "nextpnr_output_file_test_" + std::to_string(process_id()) + ".txt";
    {
        auto out = open_ofstream_and_log_error(filename, "test file");
        out << "contents";
        EXPECT_NO_THROW(close_ofstream_and_log_error(out, filename, "test file"));
    }
    std::ifstream in(filename);
    EXPECT_EQ(std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()), "contents");
    in.close();
    std::remove(filename.c_str());
}
