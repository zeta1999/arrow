// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//   http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing,
// software distributed under the License is distributed on an
// "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
// KIND, either express or implied.  See the License for the
// specific language governing permissions and limitations
// under the License.

#ifndef _WIN32
#include <fcntl.h>  // IWYU pragma: keep
#include <unistd.h>
#endif

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iterator>
#include <memory>
#include <random>
#include <string>
#include <utility>
#include <valarray>
#include <vector>

#include <gtest/gtest.h>

#include "arrow/io/buffered.h"
#include "arrow/io/file.h"
#include "arrow/io/interfaces.h"
#include "arrow/io/memory.h"
#include "arrow/io/test_common.h"
#include "arrow/status.h"
#include "arrow/testing/gtest_util.h"
#include "arrow/util/string_view.h"

namespace arrow {
namespace io {

static std::string GenerateRandomData(size_t nbytes) {
  // MSVC doesn't accept uint8_t for std::independent_bits_engine<>
  typedef unsigned long UInt;  // NOLINT
  std::independent_bits_engine<std::default_random_engine, 8 * sizeof(UInt), UInt> engine;

  std::vector<UInt> data(nbytes / sizeof(UInt) + 1);
  std::generate(begin(data), end(data), std::ref(engine));
  return std::string(reinterpret_cast<char*>(data.data()), nbytes);
}

template <typename FileType>
class FileTestFixture : public ::testing::Test {
 public:
  void SetUp() {
    path_ = "arrow-test-io-buffered-stream.txt";
    EnsureFileDeleted();
  }

  void TearDown() { EnsureFileDeleted(); }

  void EnsureFileDeleted() {
    if (FileExists(path_)) {
      ARROW_UNUSED(std::remove(path_.c_str()));
    }
  }

  void AssertTell(int64_t expected) {
    int64_t actual;
    ASSERT_OK(buffered_->Tell(&actual));
    ASSERT_EQ(expected, actual);
  }

 protected:
  int fd_;
  std::shared_ptr<FileType> buffered_;
  std::string path_;
};

// ----------------------------------------------------------------------
// Buffered output tests

constexpr int64_t kDefaultBufferSize = 4096;

class TestBufferedOutputStream : public FileTestFixture<BufferedOutputStream> {
 public:
  void OpenBuffered(int64_t buffer_size = kDefaultBufferSize, bool append = false) {
    // So that any open file is closed
    buffered_.reset();

    std::shared_ptr<FileOutputStream> file;
    ASSERT_OK(FileOutputStream::Open(path_, append, &file));
    fd_ = file->file_descriptor();
    if (append) {
      // Workaround for ARROW-2466 ("append" flag doesn't set file pos)
#if defined(_MSC_VER)
      _lseeki64(fd_, 0, SEEK_END);
#else
      lseek(fd_, 0, SEEK_END);
#endif
    }
    ASSERT_OK(BufferedOutputStream::Create(buffer_size, default_memory_pool(), file,
                                           &buffered_));
  }

  void WriteChunkwise(const std::string& datastr, const std::valarray<int64_t>& sizes) {
    const char* data = datastr.data();
    const int64_t data_size = static_cast<int64_t>(datastr.size());
    int64_t data_pos = 0;
    auto size_it = std::begin(sizes);

    // Write datastr, chunk by chunk, until exhausted
    while (true) {
      int64_t size = *size_it++;
      if (size_it == std::end(sizes)) {
        size_it = std::begin(sizes);
      }
      if (data_pos + size > data_size) {
        break;
      }
      ASSERT_OK(buffered_->Write(data + data_pos, size));
      data_pos += size;
    }
    ASSERT_OK(buffered_->Write(data + data_pos, data_size - data_pos));
  }
};

TEST_F(TestBufferedOutputStream, DestructorClosesFile) {
  OpenBuffered();
  ASSERT_FALSE(FileIsClosed(fd_));
  buffered_.reset();
  ASSERT_TRUE(FileIsClosed(fd_));
}

TEST_F(TestBufferedOutputStream, Detach) {
  OpenBuffered();
  const std::string datastr = "1234568790";

  ASSERT_OK(buffered_->Write(datastr.data(), 10));

  std::shared_ptr<OutputStream> detached_stream;
  ASSERT_OK(buffered_->Detach(&detached_stream));

  // Destroying the stream does not close the file because we have detached
  buffered_.reset();
  ASSERT_FALSE(FileIsClosed(fd_));

  ASSERT_OK(detached_stream->Close());
  ASSERT_TRUE(FileIsClosed(fd_));

  AssertFileContents(path_, datastr);
}

TEST_F(TestBufferedOutputStream, ExplicitCloseClosesFile) {
  OpenBuffered();
  ASSERT_FALSE(buffered_->closed());
  ASSERT_FALSE(FileIsClosed(fd_));
  ASSERT_OK(buffered_->Close());
  ASSERT_TRUE(buffered_->closed());
  ASSERT_TRUE(FileIsClosed(fd_));
  // Idempotency
  ASSERT_OK(buffered_->Close());
  ASSERT_TRUE(buffered_->closed());
  ASSERT_TRUE(FileIsClosed(fd_));
}

TEST_F(TestBufferedOutputStream, InvalidWrites) {
  OpenBuffered();

  const char* data = "";
  ASSERT_RAISES(Invalid, buffered_->Write(data, -1));
}

TEST_F(TestBufferedOutputStream, TinyWrites) {
  OpenBuffered();

  const std::string datastr = "1234568790";
  const char* data = datastr.data();

  ASSERT_OK(buffered_->Write(data, 2));
  ASSERT_OK(buffered_->Write(data + 2, 6));
  ASSERT_OK(buffered_->Close());

  AssertFileContents(path_, datastr.substr(0, 8));
}

TEST_F(TestBufferedOutputStream, SmallWrites) {
  OpenBuffered();

  // Data here should be larger than BufferedOutputStream's buffer size
  const std::string data = GenerateRandomData(200000);
  const std::valarray<int64_t> sizes = {1, 1, 2, 3, 5, 8, 13};

  WriteChunkwise(data, sizes);
  ASSERT_OK(buffered_->Close());

  AssertFileContents(path_, data);
}

TEST_F(TestBufferedOutputStream, MixedWrites) {
  OpenBuffered();

  const std::string data = GenerateRandomData(300000);
  const std::valarray<int64_t> sizes = {1, 1, 2, 3, 70000};

  WriteChunkwise(data, sizes);
  ASSERT_OK(buffered_->Close());

  AssertFileContents(path_, data);
}

TEST_F(TestBufferedOutputStream, LargeWrites) {
  OpenBuffered();

  const std::string data = GenerateRandomData(800000);
  const std::valarray<int64_t> sizes = {10000, 60000, 70000};

  WriteChunkwise(data, sizes);
  ASSERT_OK(buffered_->Close());

  AssertFileContents(path_, data);
}

TEST_F(TestBufferedOutputStream, Flush) {
  OpenBuffered();

  const std::string datastr = "1234568790";
  const char* data = datastr.data();

  ASSERT_OK(buffered_->Write(data, datastr.size()));
  ASSERT_OK(buffered_->Flush());

  AssertFileContents(path_, datastr);

  ASSERT_OK(buffered_->Close());
}

TEST_F(TestBufferedOutputStream, SetBufferSize) {
  OpenBuffered(20);

  ASSERT_EQ(20, buffered_->buffer_size());

  const std::string datastr = "1234568790abcdefghij";
  const char* data = datastr.data();

  // Write part of the data, then shrink buffer size to make sure it gets
  // flushed
  ASSERT_OK(buffered_->Write(data, 10));
  ASSERT_OK(buffered_->SetBufferSize(10));

  ASSERT_EQ(10, buffered_->buffer_size());

  ASSERT_OK(buffered_->Write(data + 10, 10));
  ASSERT_OK(buffered_->Flush());

  AssertFileContents(path_, datastr);
  ASSERT_OK(buffered_->Close());
}

TEST_F(TestBufferedOutputStream, Tell) {
  OpenBuffered();

  AssertTell(0);
  AssertTell(0);
  WriteChunkwise(std::string(100, 'x'), {1, 1, 2, 3, 5, 8});
  AssertTell(100);
  WriteChunkwise(std::string(100000, 'x'), {60000});
  AssertTell(100100);

  ASSERT_OK(buffered_->Close());

  OpenBuffered(kDefaultBufferSize, true /* append */);
  AssertTell(100100);
  WriteChunkwise(std::string(90, 'x'), {1, 1, 2, 3, 5, 8});
  AssertTell(100190);

  ASSERT_OK(buffered_->Close());

  OpenBuffered();
  AssertTell(0);
}

TEST_F(TestBufferedOutputStream, TruncatesFile) {
  OpenBuffered();

  const std::string datastr = "1234568790";
  ASSERT_OK(buffered_->Write(datastr.data(), datastr.size()));
  ASSERT_OK(buffered_->Close());

  AssertFileContents(path_, datastr);

  OpenBuffered();
  AssertFileContents(path_, "");
}

// ----------------------------------------------------------------------
// BufferedInputStream tests

const char kExample1[] = "informaticacrobaticsimmolation";

class TestBufferedInputStream : public FileTestFixture<BufferedInputStream> {
 public:
  void SetUp() {
    FileTestFixture<BufferedInputStream>::SetUp();
    local_pool_ = MemoryPool::CreateDefault();
  }

  void MakeExample1(int64_t buffer_size, MemoryPool* pool = default_memory_pool()) {
    test_data_ = kExample1;

    std::shared_ptr<FileOutputStream> file_out;
    ASSERT_OK(FileOutputStream::Open(path_, &file_out));
    ASSERT_OK(file_out->Write(test_data_));
    ASSERT_OK(file_out->Close());

    std::shared_ptr<ReadableFile> file_in;
    ASSERT_OK(ReadableFile::Open(path_, &file_in));
    raw_ = file_in;
    ASSERT_OK(BufferedInputStream::Create(buffer_size, pool, raw_, &buffered_));
  }

 protected:
  std::unique_ptr<MemoryPool> local_pool_;
  std::string test_data_;
  std::shared_ptr<InputStream> raw_;
};

TEST_F(TestBufferedInputStream, BasicOperation) {
  const int64_t kBufferSize = 10;
  MakeExample1(kBufferSize);
  ASSERT_EQ(kBufferSize, buffered_->buffer_size());

  int64_t stream_position = -1;
  ASSERT_OK(buffered_->Tell(&stream_position));
  ASSERT_EQ(0, stream_position);

  // Nothing in the buffer
  ASSERT_EQ(0, buffered_->bytes_buffered());

  std::vector<char> buf(test_data_.size());
  int64_t bytes_read;
  ASSERT_OK(buffered_->Read(4, &bytes_read, buf.data()));
  ASSERT_EQ(4, bytes_read);
  ASSERT_EQ(0, memcmp(buf.data(), test_data_.data(), 4));

  // 6 bytes remaining in buffer
  ASSERT_EQ(6, buffered_->bytes_buffered());

  util::string_view peek;
  ASSERT_OK(buffered_->Peek(6, &peek));
  ASSERT_EQ(6, peek.size());

  // Buffered position is 4
  ASSERT_OK(buffered_->Tell(&stream_position));
  ASSERT_EQ(4, stream_position);

  // Raw position actually 10
  ASSERT_OK(raw_->Tell(&stream_position));
  ASSERT_EQ(10, stream_position);

  // Reading to end of buffered bytes does not cause any more data to be
  // buffered
  ASSERT_OK(buffered_->Read(6, &bytes_read, buf.data()));
  ASSERT_EQ(6, bytes_read);
  ASSERT_EQ(0, memcmp(buf.data(), test_data_.data() + 4, 6));

  ASSERT_EQ(0, buffered_->bytes_buffered());

  // Read to EOF, exceeding buffer size
  ASSERT_OK(buffered_->Read(20, &bytes_read, buf.data()));
  ASSERT_EQ(20, bytes_read);
  ASSERT_EQ(0, memcmp(buf.data(), test_data_.data() + 10, 20));
  ASSERT_EQ(0, buffered_->bytes_buffered());

  // Read to EOF
  ASSERT_OK(buffered_->Read(1, &bytes_read, buf.data()));
  ASSERT_EQ(0, bytes_read);
  ASSERT_OK(buffered_->Tell(&stream_position));
  ASSERT_EQ(test_data_.size(), stream_position);

  // Peek at EOF
  ASSERT_OK(buffered_->Peek(10, &peek));
  ASSERT_EQ(0, peek.size());

  // Calling Close closes raw_
  ASSERT_OK(buffered_->Close());
  ASSERT_TRUE(buffered_->raw()->closed());
}

TEST_F(TestBufferedInputStream, Detach) {
  MakeExample1(10);
  auto raw = buffered_->Detach();
  ASSERT_OK(buffered_->Close());
  ASSERT_FALSE(raw->closed());
}

TEST_F(TestBufferedInputStream, ReadBuffer) {
  const int64_t kBufferSize = 10;
  MakeExample1(kBufferSize);

  std::shared_ptr<Buffer> buf;

  // Read exceeding buffer size
  ASSERT_OK(buffered_->Read(15, &buf));
  ASSERT_EQ(15, buf->size());
  ASSERT_EQ(0, memcmp(buf->data(), test_data_.data(), 15));
  ASSERT_EQ(0, buffered_->bytes_buffered());

  // Buffered reads
  ASSERT_OK(buffered_->Read(6, &buf));
  ASSERT_EQ(6, buf->size());
  ASSERT_EQ(0, memcmp(buf->data(), test_data_.data() + 15, 6));
  ASSERT_EQ(4, buffered_->bytes_buffered());

  ASSERT_OK(buffered_->Read(4, &buf));
  ASSERT_EQ(4, buf->size());
  ASSERT_EQ(0, memcmp(buf->data(), test_data_.data() + 21, 4));
  ASSERT_EQ(0, buffered_->bytes_buffered());
}

TEST_F(TestBufferedInputStream, SetBufferSize) {
  MakeExample1(5);

  std::shared_ptr<Buffer> buf;
  ASSERT_OK(buffered_->Read(5, &buf));
  ASSERT_EQ(5, buf->size());

  // Increase buffer size
  ASSERT_OK(buffered_->SetBufferSize(10));
  ASSERT_EQ(10, buffered_->buffer_size());
  ASSERT_OK(buffered_->Read(6, &buf));
  ASSERT_EQ(4, buffered_->bytes_buffered());

  // Consume until 5 byte left
  ASSERT_OK(buffered_->Read(15, &buf));

  // Read at EOF so there will be only 5 bytes in the buffer
  ASSERT_OK(buffered_->Read(2, &buf));

  // Cannot shrink buffer if it would destroy data
  ASSERT_RAISES(Invalid, buffered_->SetBufferSize(4));

  // Shrinking to exactly number of buffered bytes is ok
  ASSERT_OK(buffered_->SetBufferSize(5));
}

class TestBufferedInputStreamBound : public ::testing::Test {
 public:
  void SetUp() { CreateExample(/*bounded=*/true); }

  void CreateExample(bool bounded = true) {
    // Create a buffer larger than source size, to check that the
    // stream end is respected
    std::shared_ptr<ResizableBuffer> buf;
    ASSERT_OK(AllocateResizableBuffer(default_memory_pool(), source_size_ + 10, &buf));
    ASSERT_LT(source_size_, buf->size());
    for (int i = 0; i < source_size_; i++) {
      buf->mutable_data()[i] = static_cast<uint8_t>(i);
    }
    source_ = std::make_shared<BufferReader>(buf);
    ASSERT_OK(source_->Advance(stream_offset_));
    ASSERT_OK(BufferedInputStream::Create(chunk_size_, default_memory_pool(), source_,
                                          &stream_, bounded ? stream_size_ : -1));
  }

 protected:
  int64_t source_size_ = 256;
  int64_t stream_offset_ = 10;
  int64_t stream_size_ = source_size_ - stream_offset_;
  int64_t chunk_size_ = 50;
  std::shared_ptr<InputStream> source_;
  std::shared_ptr<BufferedInputStream> stream_;
};

TEST_F(TestBufferedInputStreamBound, Basics) {
  std::shared_ptr<Buffer> buffer;
  util::string_view view;

  // source is at offset 10
  ASSERT_OK(stream_->Peek(10, &view));
  ASSERT_EQ(10, view.size());
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(10 + i, view[i]) << i;
  }

  ASSERT_OK(stream_->Read(10, &buffer));
  ASSERT_EQ(10, buffer->size());
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(10 + i, (*buffer)[i]) << i;
  }

  ASSERT_OK(stream_->Read(10, &buffer));
  ASSERT_EQ(10, buffer->size());
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(20 + i, (*buffer)[i]) << i;
  }
  ASSERT_OK(stream_->Advance(5));
  ASSERT_OK(stream_->Advance(5));

  // source is at offset 40
  // read across buffer boundary. buffer size is 50
  ASSERT_OK(stream_->Read(20, &buffer));
  ASSERT_EQ(20, buffer->size());
  for (int i = 0; i < 20; i++) {
    ASSERT_EQ(40 + i, (*buffer)[i]) << i;
  }

  // read more than original chunk size
  ASSERT_OK(stream_->Read(60, &buffer));
  ASSERT_EQ(60, buffer->size());
  for (int i = 0; i < 60; i++) {
    ASSERT_EQ(60 + i, (*buffer)[i]) << i;
  }

  ASSERT_OK(stream_->Advance(120));

  // source is at offset 240
  // read outside of source boundary. source size is 256
  ASSERT_OK(stream_->Read(30, &buffer));

  ASSERT_EQ(16, buffer->size());
  for (int i = 0; i < 16; i++) {
    ASSERT_EQ(240 + i, (*buffer)[i]) << i;
  }
  // Stream exhausted
  ASSERT_OK(stream_->Read(1, &buffer));
  ASSERT_EQ(0, buffer->size());
}

TEST_F(TestBufferedInputStreamBound, LargeFirstPeek) {
  // Test a first peek larger than chunk size
  std::shared_ptr<Buffer> buffer;
  util::string_view view;
  int64_t n = 70;
  ASSERT_GT(n, chunk_size_);

  // source is at offset 10
  ASSERT_OK(stream_->Peek(n, &view));
  ASSERT_EQ(n, static_cast<int>(view.size()));
  for (int i = 0; i < n; i++) {
    ASSERT_EQ(10 + i, view[i]) << i;
  }

  ASSERT_OK(stream_->Peek(n, &view));
  ASSERT_EQ(n, static_cast<int>(view.size()));
  for (int i = 0; i < n; i++) {
    ASSERT_EQ(10 + i, view[i]) << i;
  }

  ASSERT_OK(stream_->Read(n, &buffer));
  ASSERT_EQ(n, buffer->size());
  for (int i = 0; i < n; i++) {
    ASSERT_EQ(10 + i, (*buffer)[i]) << i;
  }
  // source is at offset 10 + n
  ASSERT_OK(stream_->Read(20, &buffer));
  ASSERT_EQ(20, buffer->size());
  for (int i = 0; i < 20; i++) {
    ASSERT_EQ(10 + n + i, (*buffer)[i]) << i;
  }
}

TEST_F(TestBufferedInputStreamBound, UnboundedPeek) {
  CreateExample(/*bounded=*/false);

  util::string_view view;
  ASSERT_OK(stream_->Peek(10, &view));
  ASSERT_EQ(10, view.size());
  ASSERT_EQ(50, stream_->bytes_buffered());

  std::shared_ptr<Buffer> buf;
  ASSERT_OK(stream_->Read(10, &buf));

  // Peek into buffered bytes
  ASSERT_OK(stream_->Peek(40, &view));
  ASSERT_EQ(40, view.size());
  ASSERT_EQ(40, stream_->bytes_buffered());
  ASSERT_EQ(50, stream_->buffer_size());

  // Peek past buffered bytes
  ASSERT_OK(stream_->Peek(41, &view));
  ASSERT_EQ(41, view.size());
  ASSERT_EQ(41, stream_->bytes_buffered());
  ASSERT_EQ(51, stream_->buffer_size());

  // Peek to the end of the buffer
  ASSERT_OK(stream_->Peek(246, &view));
  ASSERT_EQ(246, view.size());
  ASSERT_EQ(246, stream_->bytes_buffered());
  ASSERT_EQ(246, stream_->buffer_size());

  // Larger peek returns the same, expands the buffer, but there is no
  // more data to buffer
  ASSERT_OK(stream_->Peek(300, &view));
  ASSERT_EQ(246, view.size());
  ASSERT_EQ(246, stream_->bytes_buffered());
  ASSERT_EQ(300, stream_->buffer_size());
}

TEST_F(TestBufferedInputStreamBound, OneByteReads) {
  std::shared_ptr<Buffer> buffer;
  for (int i = 0; i < stream_size_; ++i) {
    ASSERT_OK(stream_->Read(1, &buffer));
    ASSERT_EQ(1, buffer->size());
    ASSERT_EQ(10 + i, (*buffer)[0]) << i;
  }
  // Stream exhausted
  ASSERT_OK(stream_->Read(1, &buffer));
  ASSERT_EQ(0, buffer->size());
}

TEST_F(TestBufferedInputStreamBound, BufferExactlyExhausted) {
  // Test exhausting the buffer exactly then issuing further reads (PARQUET-1571).
  std::shared_ptr<Buffer> buffer;

  // source is at offset 10
  int64_t n = 10;
  ASSERT_OK(stream_->Read(n, &buffer));
  ASSERT_EQ(n, buffer->size());
  for (int i = 0; i < n; i++) {
    ASSERT_EQ(10 + i, (*buffer)[i]) << i;
  }
  // source is at offset 20
  // Exhaust buffer exactly
  n = stream_->bytes_buffered();
  ASSERT_OK(stream_->Read(n, &buffer));
  ASSERT_EQ(n, buffer->size());
  for (int i = 0; i < n; i++) {
    ASSERT_EQ(20 + i, (*buffer)[i]) << i;
  }

  // source is at offset 20 + n
  // Read new buffer
  ASSERT_OK(stream_->Read(10, &buffer));
  ASSERT_EQ(10, buffer->size());
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(20 + n + i, (*buffer)[i]) << i;
  }

  // source is at offset 30 + n
  ASSERT_OK(stream_->Read(10, &buffer));
  ASSERT_EQ(10, buffer->size());
  for (int i = 0; i < 10; i++) {
    ASSERT_EQ(30 + n + i, (*buffer)[i]) << i;
  }
}

}  // namespace io
}  // namespace arrow
