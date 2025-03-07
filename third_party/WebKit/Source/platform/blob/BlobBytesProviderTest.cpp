// Copyright 2017 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "platform/blob/BlobBytesProvider.h"

#include <algorithm>
#include <memory>
#include <utility>

#include "base/files/file.h"
#include "base/files/file_util.h"
#include "base/run_loop.h"
#include "base/test/scoped_task_environment.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/WebKit/public/platform/scheduler/test/renderer_scheduler_test_support.h"

namespace blink {

class BlobBytesProviderTest : public ::testing::Test {
 public:
  void SetUp() override {
    test_bytes1_.resize(128);
    for (size_t i = 0; i < test_bytes1_.size(); ++i)
      test_bytes1_[i] = i % 191;
    test_data1_ = RawData::Create();
    test_data1_->MutableData()->AppendVector(test_bytes1_);
    test_bytes2_.resize(64);
    for (size_t i = 0; i < test_bytes2_.size(); ++i)
      test_bytes2_[i] = i;
    test_data2_ = RawData::Create();
    test_data2_->MutableData()->AppendVector(test_bytes2_);
    test_bytes3_.resize(32);
    for (size_t i = 0; i < test_bytes3_.size(); ++i)
      test_bytes3_[i] = (i + 10) % 137;
    test_data3_ = RawData::Create();
    test_data3_->MutableData()->AppendVector(test_bytes3_);

    combined_bytes_.AppendVector(test_bytes1_);
    combined_bytes_.AppendVector(test_bytes2_);
    combined_bytes_.AppendVector(test_bytes3_);
  }

 protected:
  base::test::ScopedTaskEnvironment scoped_task_environment_;

  scoped_refptr<RawData> test_data1_;
  Vector<uint8_t> test_bytes1_;
  scoped_refptr<RawData> test_data2_;
  Vector<uint8_t> test_bytes2_;
  scoped_refptr<RawData> test_data3_;
  Vector<uint8_t> test_bytes3_;
  Vector<uint8_t> combined_bytes_;
};

TEST_F(BlobBytesProviderTest, Consolidation) {
  BlobBytesProvider data;
  data.AppendData(base::make_span("abc", 3));
  data.AppendData(base::make_span("def", 3));
  data.AppendData(base::make_span("ps1", 3));
  data.AppendData(base::make_span("ps2", 3));

  EXPECT_EQ(1u, data.data_.size());
  EXPECT_EQ(12u, data.data_[0]->length());
  EXPECT_EQ(0, memcmp(data.data_[0]->data(), "abcdefps1ps2", 12));

  auto large_data = std::make_unique<char[]>(
      BlobBytesProvider::kMaxConsolidatedItemSizeInBytes);
  data.AppendData(base::make_span(
      large_data.get(), BlobBytesProvider::kMaxConsolidatedItemSizeInBytes));

  EXPECT_EQ(2u, data.data_.size());
  EXPECT_EQ(12u, data.data_[0]->length());
  EXPECT_EQ(BlobBytesProvider::kMaxConsolidatedItemSizeInBytes,
            data.data_[1]->length());
}

TEST_F(BlobBytesProviderTest, RequestAsReply) {
  auto provider = std::make_unique<BlobBytesProvider>(test_data1_);
  Vector<uint8_t> received_bytes;
  provider->RequestAsReply(
      base::BindOnce([](Vector<uint8_t>* bytes_out,
                        const Vector<uint8_t>& bytes) { *bytes_out = bytes; },
                     &received_bytes));
  EXPECT_EQ(test_bytes1_, received_bytes);

  received_bytes.clear();
  provider = std::make_unique<BlobBytesProvider>(test_data1_);
  provider->AppendData(test_data2_);
  provider->AppendData(test_data3_);
  provider->RequestAsReply(
      base::BindOnce([](Vector<uint8_t>* bytes_out,
                        const Vector<uint8_t>& bytes) { *bytes_out = bytes; },
                     &received_bytes));
  EXPECT_EQ(combined_bytes_, received_bytes);
}

namespace {

struct FileTestData {
  uint64_t offset;
  uint64_t size;
};

void PrintTo(const FileTestData& test, std::ostream* os) {
  *os << "offset: " << test.offset << ", size: " << test.size;
}

class RequestAsFile : public BlobBytesProviderTest,
                      public ::testing::WithParamInterface<FileTestData> {
 public:
  void SetUp() override {
    BlobBytesProviderTest::SetUp();
    test_provider_ = std::make_unique<BlobBytesProvider>(test_data1_);
    test_provider_->AppendData(test_data2_);
    test_provider_->AppendData(test_data3_);

    sliced_data_.AppendRange(
        combined_bytes_.begin() + GetParam().offset,
        combined_bytes_.begin() + GetParam().offset + GetParam().size);
  }

  base::File DoRequestAsFile(uint64_t source_offset,
                             uint64_t source_length,
                             uint64_t file_offset) {
    base::FilePath path;
    base::CreateTemporaryFile(&path);
    WTF::Optional<WTF::Time> received_modified;
    test_provider_->RequestAsFile(
        source_offset, source_length,
        base::File(path, base::File::FLAG_OPEN | base::File::FLAG_WRITE),
        file_offset,
        base::BindOnce(
            [](WTF::Optional<WTF::Time>* received_modified,
               WTF::Optional<WTF::Time> modified) {
              *received_modified = modified;
            },
            &received_modified));
    base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ |
                              base::File::FLAG_DELETE_ON_CLOSE);
    base::File::Info info;
    EXPECT_TRUE(file.GetInfo(&info));
    EXPECT_EQ(info.last_modified, received_modified);
    return file;
  }

 protected:
  std::unique_ptr<BlobBytesProvider> test_provider_;
  Vector<uint8_t> sliced_data_;
};

TEST_P(RequestAsFile, AtStartOfEmptyFile) {
  FileTestData test = GetParam();
  base::File file = DoRequestAsFile(test.offset, test.size, 0);

  base::File::Info info;
  EXPECT_TRUE(file.GetInfo(&info));
  EXPECT_EQ(static_cast<int64_t>(test.size), info.size);

  Vector<uint8_t> read_data(test.size);
  EXPECT_EQ(static_cast<int>(test.size),
            file.Read(0, reinterpret_cast<char*>(read_data.data()), test.size));
  EXPECT_EQ(sliced_data_, read_data);
}

TEST_P(RequestAsFile, OffsetInEmptyFile) {
  FileTestData test = GetParam();
  int file_offset = 32;
  sliced_data_.InsertVector(0, Vector<uint8_t>(file_offset));

  base::File file = DoRequestAsFile(test.offset, test.size, file_offset);

  base::File::Info info;
  EXPECT_TRUE(file.GetInfo(&info));
  if (test.size == 0) {
    EXPECT_EQ(0, info.size);
  } else {
    EXPECT_EQ(static_cast<int64_t>(test.size) + 32, info.size);

    Vector<uint8_t> read_data(sliced_data_.size());
    EXPECT_EQ(static_cast<int>(sliced_data_.size()),
              file.Read(0, reinterpret_cast<char*>(read_data.data()),
                        sliced_data_.size()));
    EXPECT_EQ(sliced_data_, read_data);
  }
}

TEST_P(RequestAsFile, OffsetInNonEmptyFile) {
  FileTestData test = GetParam();
  int file_offset = 23;

  Vector<uint8_t> expected_data(1024, 42);

  base::FilePath path;
  base::CreateTemporaryFile(&path);
  {
    base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_WRITE);
    EXPECT_EQ(static_cast<int>(expected_data.size()),
              file.WriteAtCurrentPos(
                  reinterpret_cast<const char*>(expected_data.data()),
                  expected_data.size()));
  }

  std::copy(sliced_data_.begin(), sliced_data_.end(),
            expected_data.begin() + file_offset);

  test_provider_->RequestAsFile(
      test.offset, test.size,
      base::File(path, base::File::FLAG_OPEN | base::File::FLAG_WRITE),
      file_offset, base::BindOnce([](WTF::Optional<WTF::Time> last_modified) {
        EXPECT_TRUE(last_modified);
      }));

  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ |
                            base::File::FLAG_DELETE_ON_CLOSE);
  base::File::Info info;
  EXPECT_TRUE(file.GetInfo(&info));
  EXPECT_EQ(static_cast<int64_t>(expected_data.size()), info.size);

  Vector<uint8_t> read_data(expected_data.size());
  EXPECT_EQ(static_cast<int>(expected_data.size()),
            file.Read(0, reinterpret_cast<char*>(read_data.data()),
                      expected_data.size()));
  EXPECT_EQ(expected_data, read_data);
}

const FileTestData file_tests[] = {
    {0, 128 + 64 + 32},  // The full amount of data.
    {0, 128 + 64},       // First two chunks.
    {10, 13},            // Just a subset of the first chunk.
    {10, 128},           // Parts of both the first and second chunk.
    {128, 64},           // The entire second chunk.
    {0, 0},              // Zero bytes from the beginning.
    {130, 10},           // Just a subset of the second chunk.
    {140, 0},            // Zero bytes from the middle of the second chunk.
    {10, 128 + 64},      // Parts of all three chunks.
};

INSTANTIATE_TEST_CASE_P(BlobBytesProviderTest,
                        RequestAsFile,
                        ::testing::ValuesIn(file_tests));

TEST_F(BlobBytesProviderTest, RequestAsFile_MultipleChunks) {
  auto provider = std::make_unique<BlobBytesProvider>(test_data1_);
  provider->AppendData(test_data2_);
  provider->AppendData(test_data3_);

  base::FilePath path;
  base::CreateTemporaryFile(&path);

  Vector<uint8_t> expected_data;
  for (size_t i = 0; i < combined_bytes_.size(); i += 16) {
    provider->RequestAsFile(
        i, 16, base::File(path, base::File::FLAG_OPEN | base::File::FLAG_WRITE),
        combined_bytes_.size() - i - 16,
        base::BindOnce([](WTF::Optional<WTF::Time> last_modified) {
          EXPECT_TRUE(last_modified);
        }));
    expected_data.insert(0, combined_bytes_.data() + i, 16);
  }

  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ |
                            base::File::FLAG_DELETE_ON_CLOSE);
  base::File::Info info;
  EXPECT_TRUE(file.GetInfo(&info));
  EXPECT_EQ(static_cast<int64_t>(combined_bytes_.size()), info.size);

  Vector<uint8_t> read_data(expected_data.size());
  EXPECT_EQ(static_cast<int>(expected_data.size()),
            file.Read(0, reinterpret_cast<char*>(read_data.data()),
                      expected_data.size()));
  EXPECT_EQ(expected_data, read_data);
}

TEST_F(BlobBytesProviderTest, RequestAsFile_InvaldFile) {
  auto provider = std::make_unique<BlobBytesProvider>(test_data1_);

  provider->RequestAsFile(
      0, 16, base::File(), 0,
      base::BindOnce([](WTF::Optional<WTF::Time> last_modified) {
        EXPECT_FALSE(last_modified);
      }));
}

TEST_F(BlobBytesProviderTest, RequestAsFile_UnwritableFile) {
  auto provider = std::make_unique<BlobBytesProvider>(test_data1_);

  base::FilePath path;
  base::CreateTemporaryFile(&path);
  provider->RequestAsFile(
      0, 16, base::File(path, base::File::FLAG_OPEN | base::File::FLAG_READ), 0,
      base::BindOnce([](WTF::Optional<WTF::Time> last_modified) {
        EXPECT_FALSE(last_modified);
      }));

  base::File file(path, base::File::FLAG_OPEN | base::File::FLAG_READ |
                            base::File::FLAG_DELETE_ON_CLOSE);
  base::File::Info info;
  EXPECT_TRUE(file.GetInfo(&info));
  EXPECT_EQ(0, info.size);
}

TEST_F(BlobBytesProviderTest, RequestAsStream) {
  auto provider = std::make_unique<BlobBytesProvider>(test_data1_);
  provider->AppendData(test_data2_);
  provider->AppendData(test_data3_);

  mojo::DataPipe pipe(7);
  provider->RequestAsStream(std::move(pipe.producer_handle));

  Vector<uint8_t> received_data;
  base::RunLoop loop;
  mojo::SimpleWatcher watcher(
      FROM_HERE, mojo::SimpleWatcher::ArmingPolicy::AUTOMATIC,
      blink::scheduler::GetSequencedTaskRunnerForTesting());
  watcher.Watch(
      pipe.consumer_handle.get(), MOJO_HANDLE_SIGNAL_READABLE,
      base::BindRepeating(
          [](mojo::DataPipeConsumerHandle pipe, base::Closure quit_closure,
             Vector<uint8_t>* bytes_out, MojoResult result) {
            if (result == MOJO_RESULT_CANCELLED ||
                result == MOJO_RESULT_FAILED_PRECONDITION) {
              quit_closure.Run();
              return;
            }

            uint32_t num_bytes = 0;
            MojoResult query_result =
                pipe.ReadData(nullptr, &num_bytes, MOJO_READ_DATA_FLAG_QUERY);
            if (query_result == MOJO_RESULT_SHOULD_WAIT)
              return;
            EXPECT_EQ(MOJO_RESULT_OK, query_result);

            Vector<uint8_t> bytes(num_bytes);
            EXPECT_EQ(MOJO_RESULT_OK,
                      pipe.ReadData(bytes.data(), &num_bytes,
                                    MOJO_READ_DATA_FLAG_ALL_OR_NONE));
            bytes_out->AppendVector(bytes);
          },
          pipe.consumer_handle.get(), loop.QuitClosure(), &received_data));
  loop.Run();

  EXPECT_EQ(combined_bytes_, received_data);
}

}  // namespace

}  // namespace blink
