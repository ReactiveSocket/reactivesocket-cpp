// Copyright 2004-present Facebook. All Rights Reserved.

#include <utility>

#include <folly/Singleton.h>
#include <folly/io/IOBuf.h>
#include <gmock/gmock.h>

#include "src/Frame.h"
#include "src/Payload.h"

using namespace ::testing;
using namespace ::reactivesocket;

class FrameTest : public ::testing::Test {
  void SetUp() override {
    EXPECT_CALL(allocator, allocateBuffer_(_))
        .WillRepeatedly(Invoke([](size_t size) {
          auto buf = folly::IOBuf::createCombined(size + sizeof(int32_t));
          // create some wasted headroom
          buf->advance(sizeof(int32_t));
          return buf.release();
        }));

    folly::Singleton<reactivesocket::FrameBufferAllocator>::make_mock(
        [&] { return &allocator; }, /* no tear down */ [](void*) {});
  }

  void TearDown() override {
    folly::SingletonVault::singleton()->destroyInstances();
    // Bring the default allocator.
    folly::Singleton<reactivesocket::FrameBufferAllocator>::make_mock(nullptr);
    folly::SingletonVault::singleton()->reenableInstances();
  }

  class MockBufferAllocator : public FrameBufferAllocator {
   public:
    MOCK_METHOD1(allocateBuffer_, folly::IOBuf*(size_t size));

    std::unique_ptr<folly::IOBuf> allocateBuffer(size_t size) override {
      return std::unique_ptr<folly::IOBuf>(allocateBuffer_(size));
    }
  } allocator;
};

// TODO(stupaq): tests with malformed frames

template <typename Frame, typename... Args>
Frame reserialize(Args... args) {
  Frame frame;
  EXPECT_TRUE(
      frame.deserializeFrom(Frame(std::forward<Args>(args)...).serializeOut()));
  return frame;
}

template <typename Frame>
void expectHeader(
    FrameType type,
    FrameFlags flags,
    StreamId streamId,
    const Frame& frame) {
  EXPECT_EQ(type, frame.header_.type_);
  EXPECT_EQ(streamId, frame.header_.streamId_);
  EXPECT_EQ(flags, frame.header_.flags_);
}

TEST_F(FrameTest, Frame_REQUEST_CHANNEL) {
  uint32_t streamId = 42;
  FrameFlags flags =
      FrameFlags_COMPLETE | FrameFlags_REQN_PRESENT | FrameFlags_METADATA;
  uint32_t requestN = 3;
  auto metadata = folly::IOBuf::copyBuffer("i'm so meta even this acronym");
  auto data = folly::IOBuf::copyBuffer("424242");
  auto frame = reserialize<Frame_REQUEST_CHANNEL>(
      streamId,
      flags,
      requestN,
      FrameMetadata(metadata->clone()),
      data->clone());

  expectHeader(FrameType::REQUEST_CHANNEL, flags, streamId, frame);
  EXPECT_EQ(requestN, frame.requestN_);
  EXPECT_TRUE(
      folly::IOBufEqual()(*metadata, *frame.metadata_.metadataPayload_));
  EXPECT_TRUE(folly::IOBufEqual()(*data, *frame.data_));
}

TEST_F(FrameTest, Frame_REQUEST_N) {
  uint32_t streamId = 42;
  uint32_t requestN = 24;
  auto frame = reserialize<Frame_REQUEST_N>(streamId, requestN);

  expectHeader(FrameType::REQUEST_N, FrameFlags_EMPTY, streamId, frame);
  EXPECT_EQ(requestN, frame.requestN_);
}

TEST_F(FrameTest, Frame_CANCEL) {
  uint32_t streamId = 42;
  FrameFlags flags = FrameFlags_METADATA;
  auto metadata = folly::IOBuf::copyBuffer("i'm so meta even this acronym");
  auto frame = reserialize<Frame_CANCEL>(
      streamId, flags, FrameMetadata(metadata->clone()));

  expectHeader(FrameType::CANCEL, flags, streamId, frame);
  EXPECT_TRUE(
      folly::IOBufEqual()(*metadata, *frame.metadata_.metadataPayload_));
}

TEST_F(FrameTest, Frame_RESPONSE) {
  uint32_t streamId = 42;
  FrameFlags flags = FrameFlags_COMPLETE | FrameFlags_METADATA;
  auto metadata = folly::IOBuf::copyBuffer("i'm so meta even this acronym");
  auto data = folly::IOBuf::copyBuffer("424242");
  auto frame = reserialize<Frame_RESPONSE>(
      streamId, flags, FrameMetadata(metadata->clone()), data->clone());

  expectHeader(FrameType::RESPONSE, flags, streamId, frame);
  EXPECT_TRUE(
      folly::IOBufEqual()(*metadata, *frame.metadata_.metadataPayload_));
  EXPECT_TRUE(folly::IOBufEqual()(*data, *frame.data_));
}

TEST_F(FrameTest, Frame_RESPONSE_NoMeta) {
  uint32_t streamId = 42;
  FrameFlags flags = FrameFlags_COMPLETE;
  auto data = folly::IOBuf::copyBuffer("424242");
  auto frame = reserialize<Frame_RESPONSE>(
      streamId, flags, FrameMetadata::empty(), data->clone());

  expectHeader(FrameType::RESPONSE, flags, streamId, frame);
  EXPECT_FALSE(frame.metadata_.metadataPayload_);
  EXPECT_TRUE(folly::IOBufEqual()(*data, *frame.data_));
}

TEST_F(FrameTest, Frame_ERROR) {
  uint32_t streamId = 42;
  FrameFlags flags = FrameFlags_METADATA;
  auto errorCode = ErrorCode::REJECTED;
  auto metadata = folly::IOBuf::copyBuffer("i'm so meta even this acronym");
  auto frame = reserialize<Frame_ERROR>(
      streamId, flags, errorCode, FrameMetadata(metadata->clone()));

  expectHeader(FrameType::ERROR, flags, streamId, frame);
  EXPECT_EQ(errorCode, frame.errorCode_);
  EXPECT_TRUE(
      folly::IOBufEqual()(*metadata, *frame.metadata_.metadataPayload_));
}

TEST_F(FrameTest, Frame_KEEPALIVE) {
  uint32_t streamId = 0;
  auto flags = FrameFlags_KEEPALIVE_RESPOND;
  auto data = folly::IOBuf::copyBuffer("424242");
  auto frame = reserialize<Frame_KEEPALIVE>(streamId, flags, data->clone());

  expectHeader(
      FrameType::KEEPALIVE, FrameFlags_KEEPALIVE_RESPOND, streamId, frame);
  EXPECT_TRUE(folly::IOBufEqual()(*data, *frame.data_));
}

TEST_F(FrameTest, Frame_SETUP) {
  uint32_t streamId = 0;
  FrameFlags flags = FrameFlags_EMPTY;
  uint32_t version = 0;
  uint32_t keepaliveTime = std::numeric_limits<uint32_t>::max();
  uint32_t maxLifetime = std::numeric_limits<uint32_t>::max();
  auto data = folly::IOBuf::copyBuffer("424242");
  auto frame = reserialize<Frame_SETUP>(
      streamId,
      flags,
      version,
      keepaliveTime,
      maxLifetime,
      FrameMetadata::empty(),
      data->clone());

  expectHeader(FrameType::SETUP, flags, streamId, frame);
  EXPECT_EQ(version, frame.version_);
  EXPECT_EQ(keepaliveTime, frame.keepaliveTime_);
  EXPECT_EQ(maxLifetime, frame.maxLifetime_);
  EXPECT_TRUE(folly::IOBufEqual()(*data, *frame.data_));
}

TEST_F(FrameTest, Frame_REQUEST_FNF) {
  uint32_t streamId = 42;
  FrameFlags flags = FrameFlags_METADATA;
  auto metadata = folly::IOBuf::copyBuffer("i'm so meta even this acronym");
  auto data = folly::IOBuf::copyBuffer("424242");
  auto frame = reserialize<Frame_REQUEST_FNF>(
      streamId, flags, FrameMetadata(metadata->clone()), data->clone());

  expectHeader(FrameType::REQUEST_FNF, flags, streamId, frame);
  EXPECT_TRUE(
      folly::IOBufEqual()(*metadata, *frame.metadata_.metadataPayload_));
  EXPECT_TRUE(folly::IOBufEqual()(*data, *frame.data_));
}

TEST_F(FrameTest, Frame_METADATA_PUSH) {
  FrameFlags flags = FrameFlags_METADATA;
  auto metadata = folly::IOBuf::copyBuffer("i'm so meta even this acronym");
  auto frame = reserialize<Frame_METADATA_PUSH>(
      FrameMetadata(metadata->clone()));

  expectHeader(FrameType::METADATA_PUSH, flags, 0, frame);
  EXPECT_TRUE(
      folly::IOBufEqual()(*metadata, *frame.metadata_.metadataPayload_));
}
