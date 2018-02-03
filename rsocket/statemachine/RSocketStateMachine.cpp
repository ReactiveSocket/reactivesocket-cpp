// Copyright 2004-present Facebook. All Rights Reserved.

#include "rsocket/statemachine/RSocketStateMachine.h"

#include <folly/ExceptionWrapper.h>
#include <folly/Format.h>
#include <folly/Optional.h>
#include <folly/String.h>
#include <folly/io/async/EventBaseManager.h>

#include "rsocket/DuplexConnection.h"
#include "rsocket/RSocketConnectionEvents.h"
#include "rsocket/RSocketParameters.h"
#include "rsocket/RSocketResponder.h"
#include "rsocket/RSocketStats.h"
#include "rsocket/framing/Frame.h"
#include "rsocket/framing/FrameSerializer.h"
#include "rsocket/framing/FrameTransportImpl.h"
#include "rsocket/internal/ClientResumeStatusCallback.h"
#include "rsocket/internal/ConnectionSet.h"
#include "rsocket/internal/ScheduledSubscriber.h"
#include "rsocket/internal/WarmResumeManager.h"
#include "rsocket/statemachine/ChannelResponder.h"
#include "rsocket/statemachine/StreamState.h"
#include "rsocket/statemachine/StreamStateMachineBase.h"

namespace rsocket {

RSocketStateMachine::RSocketStateMachine(
    std::shared_ptr<RSocketResponder> requestResponder,
    std::unique_ptr<KeepaliveTimer> keepaliveTimer,
    RSocketMode mode,
    std::shared_ptr<RSocketStats> stats,
    std::shared_ptr<RSocketConnectionEvents> connectionEvents,
    std::shared_ptr<ResumeManager> resumeManager,
    std::shared_ptr<ColdResumeHandler> coldResumeHandler)
    : mode_{mode},
      stats_{stats ? stats : RSocketStats::noop()},
      streamState_{*stats_},
      resumeManager_{resumeManager
                         ? resumeManager
                         : std::make_shared<WarmResumeManager>(stats_)},
      requestResponder_{std::move(requestResponder)},
      keepaliveTimer_{std::move(keepaliveTimer)},
      coldResumeHandler_{std::move(coldResumeHandler)},
      streamsFactory_{*this, mode},
      connectionEvents_{connectionEvents} {
  // We deliberately do not "open" input or output to avoid having c'tor on the
  // stack when processing any signals from the connection. See ::connect and
  // ::onSubscribe.

  CHECK(requestResponder_);

  stats_->socketCreated();
  VLOG(2) << "Creating RSocketStateMachine";
}

RSocketStateMachine::~RSocketStateMachine() {
  // this destructor can be called from a different thread because the stream
  // automatons destroyed on different threads can be the last ones referencing
  // this.

  VLOG(3) << "~RSocketStateMachine";
  // We rely on SubscriptionPtr and SubscriberPtr to dispatch appropriate
  // terminal signals.
  DCHECK(!resumeCallback_);
  DCHECK(isDisconnected()); // the instance should be closed by via
  // close method
}

void RSocketStateMachine::setResumable(bool resumable) {
  // We should set this flag before we are connected
  DCHECK(isDisconnected());
  isResumable_ = resumable;
}

void RSocketStateMachine::connectServer(
    std::shared_ptr<FrameTransport> frameTransport,
    const SetupParameters& setupParams) {
  setResumable(setupParams.resumable);
  setProtocolVersionOrThrow(setupParams.protocolVersion, frameTransport);
  connect(std::move(frameTransport));
  sendPendingFrames();
}

bool RSocketStateMachine::resumeServer(
    std::shared_ptr<FrameTransport> frameTransport,
    const ResumeParameters& resumeParams) {
  folly::Optional<int64_t> clientAvailable =
      (resumeParams.clientPosition == kUnspecifiedResumePosition)
      ? folly::none
      : folly::make_optional(
            resumeManager_->impliedPosition() - resumeParams.clientPosition);

  int64_t serverAvailable =
      resumeManager_->lastSentPosition() - resumeManager_->firstSentPosition();
  int64_t serverDelta =
      resumeManager_->lastSentPosition() - resumeParams.serverPosition;

  std::runtime_error exn{"Connection being resumed, dropping old connection"};
  disconnect(std::move(exn));
  setProtocolVersionOrThrow(resumeParams.protocolVersion, frameTransport);
  connect(std::move(frameTransport));

  auto result = resumeFromPositionOrClose(
      resumeParams.serverPosition, resumeParams.clientPosition);

  stats_->serverResume(
      clientAvailable,
      serverAvailable,
      serverDelta,
      result ? RSocketStats::ResumeOutcome::SUCCESS
             : RSocketStats::ResumeOutcome::FAILURE);

  return result;
}

void RSocketStateMachine::connectClient(
    std::shared_ptr<FrameTransport> transport,
    SetupParameters params) {
  auto const version = params.protocolVersion == ProtocolVersion::Unknown
      ? ProtocolVersion::Current()
      : params.protocolVersion;

  setProtocolVersionOrThrow(version, transport);
  setResumable(params.resumable);

  Frame_SETUP frame(
      params.resumable ? FrameFlags::RESUME_ENABLE : FrameFlags::EMPTY,
      version.major,
      version.minor,
      getKeepaliveTime(),
      Frame_SETUP::kMaxLifetime,
      std::move(params.token),
      std::move(params.metadataMimeType),
      std::move(params.dataMimeType),
      std::move(params.payload));

  // TODO: when the server returns back that it doesn't support resumability, we
  // should retry without resumability

  VLOG(3) << "Out: " << frame;

  connect(std::move(transport));
  // making sure we send setup frame first
  outputFrame(frameSerializer_->serializeOut(std::move(frame)));
  // then the rest of the cached frames will be sent
  sendPendingFrames();
}

void RSocketStateMachine::resumeClient(
    ResumeIdentificationToken token,
    std::shared_ptr<FrameTransport> transport,
    std::unique_ptr<ClientResumeStatusCallback> resumeCallback,
    ProtocolVersion version) {
  // Cold-resumption.  Set the serializer.
  if (!frameSerializer_) {
    CHECK(coldResumeHandler_);
    coldResumeInProgress_ = true;
  }

  setProtocolVersionOrThrow(
      version == ProtocolVersion::Unknown ? ProtocolVersion::Current()
                                          : version,
      transport);

  Frame_RESUME resumeFrame(
      std::move(token),
      resumeManager_->impliedPosition(),
      resumeManager_->firstSentPosition(),
      frameSerializer_->protocolVersion());
  VLOG(3) << "Out: " << resumeFrame;

  // Disconnect a previous client if there is one.
  disconnect(std::runtime_error{"Resuming client on a different connection"});

  setResumable(true);
  reconnect(std::move(transport), std::move(resumeCallback));
  outputFrame(frameSerializer_->serializeOut(std::move(resumeFrame)));
}

void RSocketStateMachine::connect(
    std::shared_ptr<FrameTransport> transport) {
  VLOG(2) << "Connecting to transport " << transport.get();

  CHECK(isDisconnected());
  CHECK(transport);

  // Keep a reference to the argument, make sure the instance survives until
  // setFrameProcessor() returns.  There can be terminating signals processed in
  // that call which will nullify frameTransport_.
  frameTransport_ = transport;

  CHECK(frameSerializer_);
  frameSerializer_->preallocateFrameSizeField() =
      transport->isConnectionFramed();

  if (connectionEvents_) {
    connectionEvents_->onConnected();
  }

  // Keep a reference to this, as processing frames might close this
  // instance.
  auto copyThis = shared_from_this();
  frameTransport_->setFrameProcessor(copyThis);
  stats_->socketConnected();
}

void RSocketStateMachine::sendPendingFrames() {
  DCHECK(!resumeCallback_);

  // We are free to try to send frames again.  Not all frames might be sent if
  // the connection breaks, the rest of them will queue up again.
  auto outputFrames = streamState_.moveOutputPendingFrames();
  for (auto& frame : outputFrames) {
    outputFrameOrEnqueue(std::move(frame));
  }

  // TODO: turn on only after setup frame was received
  if (keepaliveTimer_) {
    keepaliveTimer_->start(shared_from_this());
  }
}

void RSocketStateMachine::disconnect(folly::exception_wrapper ex) {
  VLOG(2) << "Disconnecting transport";
  if (isDisconnected()) {
    return;
  }

  if (connectionEvents_) {
    connectionEvents_->onDisconnected(ex);
  }

  closeFrameTransport(std::move(ex));

  if (connectionEvents_) {
    connectionEvents_->onStreamsPaused();
  }

  stats_->socketDisconnected();
}

void RSocketStateMachine::close(
    folly::exception_wrapper ex,
    StreamCompletionSignal signal) {
  if (isClosed()) {
    return;
  }

  isClosed_ = true;
  stats_->socketClosed(signal);

  VLOG(6) << "close";

  if (auto resumeCallback = std::move(resumeCallback_)) {
    resumeCallback->onResumeError(
        ConnectionException(ex ? ex.get_exception()->what() : "RS closing"));
  }

  closeStreams(signal);
  closeFrameTransport(ex);

  if (auto connectionEvents = std::move(connectionEvents_)) {
    connectionEvents->onClosed(std::move(ex));
  }

  if (connectionSet_) {
    connectionSet_->remove(shared_from_this());
  }
}

void RSocketStateMachine::closeFrameTransport(folly::exception_wrapper ex) {
  if (isDisconnected()) {
    DCHECK(!resumeCallback_);
    return;
  }

  // Stop scheduling keepalives since the socket is now disconnected
  if (keepaliveTimer_) {
    keepaliveTimer_->stop();
  }

  if (auto resumeCallback = std::move(resumeCallback_)) {
    resumeCallback->onResumeError(ConnectionException(
        ex ? ex.get_exception()->what() : "connection closing"));
  }

  // Echo the exception to the frameTransport only if the frameTransport started
  // closing with error.  Otherwise we sent some error frame over the wire and
  // we are closing the transport cleanly.
  if (frameTransport_) {
    frameTransport_->close();
    frameTransport_ = nullptr;
  }
}

void RSocketStateMachine::disconnectOrCloseWithError(Frame_ERROR&& errorFrame) {
  if (isResumable_) {
    std::runtime_error exn{errorFrame.payload_.moveDataToString()};
    disconnect(std::move(exn));
  } else {
    closeWithError(std::move(errorFrame));
  }
}

void RSocketStateMachine::closeWithError(Frame_ERROR&& error) {
  VLOG(3) << "closeWithError "
          << error.payload_.data->cloneAsValue().moveToFbString();

  StreamCompletionSignal signal;
  switch (error.errorCode_) {
    case ErrorCode::INVALID_SETUP:
      signal = StreamCompletionSignal::INVALID_SETUP;
      break;
    case ErrorCode::UNSUPPORTED_SETUP:
      signal = StreamCompletionSignal::UNSUPPORTED_SETUP;
      break;
    case ErrorCode::REJECTED_SETUP:
      signal = StreamCompletionSignal::REJECTED_SETUP;
      break;

    case ErrorCode::CONNECTION_ERROR:
    // StreamCompletionSignal::CONNECTION_ERROR is reserved for
    // frameTransport errors
    // ErrorCode::CONNECTION_ERROR is a normal Frame_ERROR error code which has
    // nothing to do with frameTransport
    case ErrorCode::APPLICATION_ERROR:
    case ErrorCode::REJECTED:
    case ErrorCode::RESERVED:
    case ErrorCode::CANCELED:
    case ErrorCode::INVALID:
    default:
      signal = StreamCompletionSignal::ERROR;
  }

  std::runtime_error exn{error.payload_.cloneDataToString()};
  if (frameSerializer_) {
    outputFrameOrEnqueue(std::move(error));
  }
  close(std::move(exn), signal);
}

void RSocketStateMachine::reconnect(
    std::shared_ptr<FrameTransport> newFrameTransport,
    std::unique_ptr<ClientResumeStatusCallback> resumeCallback) {
  CHECK(newFrameTransport);
  CHECK(resumeCallback);

  CHECK(!resumeCallback_);
  CHECK(isResumable_);
  CHECK(mode_ == RSocketMode::CLIENT);

  // TODO: output frame buffer should not be written to the new connection until
  // we receive resume ok
  resumeCallback_ = std::move(resumeCallback);
  connect(std::move(newFrameTransport));
}

void RSocketStateMachine::addStream(
    StreamId streamId,
    std::shared_ptr<StreamStateMachineBase> stateMachine) {
  auto result =
      streamState_.streams_.emplace(streamId, std::move(stateMachine));
  DCHECK(result.second);
}

void RSocketStateMachine::endStream(
    StreamId streamId,
    StreamCompletionSignal signal) {
  VLOG(6) << "endStream";
  // The signal must be idempotent.
  if (!endStreamInternal(streamId, signal)) {
    return;
  }
  resumeManager_->onStreamClosed(streamId);
  DCHECK(
      signal == StreamCompletionSignal::CANCEL ||
      signal == StreamCompletionSignal::COMPLETE ||
      signal == StreamCompletionSignal::APPLICATION_ERROR ||
      signal == StreamCompletionSignal::ERROR);
}

bool RSocketStateMachine::endStreamInternal(
    StreamId streamId,
    StreamCompletionSignal signal) {
  VLOG(6) << "endStreamInternal";
  auto it = streamState_.streams_.find(streamId);
  if (it == streamState_.streams_.end()) {
    // Unsubscribe handshake initiated by the connection, we're done.
    return false;
  }

  // Remove from the map before notifying the stateMachine.
  auto streamElem = std::move(it->second);
  streamState_.streams_.erase(it);
  streamElem.stateMachine->endStream(signal);
  return true;
}

void RSocketStateMachine::closeStreams(StreamCompletionSignal signal) {
  // Close all streams.
  while (!streamState_.streams_.empty()) {
    auto oldSize = streamState_.streams_.size();
    auto result =
        endStreamInternal(streamState_.streams_.begin()->first, signal);
    // TODO(stupaq): what kind of a user action could violate these
    // assertions?
    DCHECK(result);
    DCHECK_EQ(streamState_.streams_.size(), oldSize - 1);
  }
}

void RSocketStateMachine::processFrame(std::unique_ptr<folly::IOBuf> frame) {
  if (isClosed()) {
    VLOG(4) << "StateMachine has been closed.  Discarding incoming frame";
    return;
  }

  // Necessary in case the only stream state machine closes itself, and takes
  // the RSocketStateMachine with it.
  auto self = shared_from_this();

  if (!ensureOrAutodetectFrameSerializer(*frame)) {
    constexpr folly::StringPiece message{"Cannot detect protocol version"};
    closeWithError(Frame_ERROR::connectionError(message.str()));
    return;
  }

  auto frameType = frameSerializer_->peekFrameType(*frame);
  stats_->frameRead(frameType);

  auto optStreamId = frameSerializer_->peekStreamId(*frame);
  if (!optStreamId) {
    constexpr folly::StringPiece message{"Cannot decode stream ID"};
    closeWithError(Frame_ERROR::connectionError(message.str()));
    return;
  }

  auto frameLength = frame->computeChainDataLength();
  auto streamId = *optStreamId;
  if (streamId == 0) {
    handleConnectionFrame(frameType, std::move(frame));
  } else if (resumeCallback_) {
    // during the time when we are resuming we are can't receive any other
    // than connection level frames which drives the resumption
    // TODO(lehecka): this assertion should be handled more elegantly using
    // different state machine
    constexpr folly::StringPiece message{
        "Received stream frame while resuming"};
    LOG(ERROR) << message;
    closeWithError(Frame_ERROR::connectionError(message.str()));
    return;
  } else {
    handleStreamFrame(streamId, frameType, std::move(frame));
  }
  resumeManager_->trackReceivedFrame(
      frameLength, frameType, streamId, getConsumerAllowance(streamId));
}

void RSocketStateMachine::onTerminal(folly::exception_wrapper ex) {
  if (isResumable_) {
    disconnect(std::move(ex));
    return;
  }
  auto termSignal = ex ? StreamCompletionSignal::CONNECTION_ERROR
                       : StreamCompletionSignal::CONNECTION_END;
  close(std::move(ex), termSignal);
}

void RSocketStateMachine::handleConnectionFrame(
    FrameType frameType,
    std::unique_ptr<folly::IOBuf> payload) {
  switch (frameType) {
    case FrameType::KEEPALIVE: {
      Frame_KEEPALIVE frame;
      if (!deserializeFrameOrError(isResumable_, frame, std::move(payload))) {
        return;
      }
      VLOG(3) << mode_ << " In: " << frame;
      resumeManager_->resetUpToPosition(frame.position_);
      if (mode_ == RSocketMode::SERVER) {
        if (!!(frame.header_.flags & FrameFlags::KEEPALIVE_RESPOND)) {
          sendKeepalive(FrameFlags::EMPTY, std::move(frame.data_));
        } else {
          closeWithError(
              Frame_ERROR::connectionError("keepalive without flag"));
        }
      } else {
        if (!!(frame.header_.flags & FrameFlags::KEEPALIVE_RESPOND)) {
          closeWithError(Frame_ERROR::connectionError(
              "client received keepalive with respond flag"));
        } else if (keepaliveTimer_) {
          keepaliveTimer_->keepaliveReceived();
        }
        stats_->keepaliveReceived();
      }
      return;
    }
    case FrameType::METADATA_PUSH: {
      Frame_METADATA_PUSH frame;
      if (deserializeFrameOrError(frame, std::move(payload))) {
        VLOG(3) << mode_ << " In: " << frame;
        requestResponder_->handleMetadataPush(std::move(frame.metadata_));
      }
      return;
    }
    case FrameType::RESUME_OK: {
      Frame_RESUME_OK frame;
      if (!deserializeFrameOrError(frame, std::move(payload))) {
        return;
      }
      VLOG(3) << mode_ << " In: " << frame;

      if (!resumeCallback_) {
        constexpr folly::StringPiece message{
            "Received RESUME_OK while not resuming"};
        closeWithError(Frame_ERROR::connectionError(message.str()));
        return;
      }

      if (!resumeManager_->isPositionAvailable(frame.position_)) {
        auto message = folly::sformat(
            "Client cannot resume, server position {} is not available",
            frame.position_);
        closeWithError(Frame_ERROR::connectionError(std::move(message)));
        return;
      }

      if (coldResumeInProgress_) {
        streamsFactory().setNextStreamId(
            resumeManager_->getLargestUsedStreamId());
        for (const auto& it : resumeManager_->getStreamResumeInfos()) {
          auto streamId = it.first;
          const StreamResumeInfo& streamResumeInfo = it.second;
          if (streamResumeInfo.requester == RequestOriginator::LOCAL &&
              streamResumeInfo.streamType == StreamType::STREAM) {
            auto subscriber = coldResumeHandler_->handleRequesterResumeStream(
                streamResumeInfo.streamToken,
                streamResumeInfo.consumerAllowance);
            streamsFactory().createStreamRequester(
                std::make_shared<ScheduledSubscriptionSubscriber<Payload>>(
                    std::move(subscriber),
                    *folly::EventBaseManager::get()->getEventBase()),
                streamId,
                streamResumeInfo.consumerAllowance);
          }
        }
        coldResumeInProgress_ = false;
      }

      auto resumeCallback = std::move(resumeCallback_);
      resumeCallback->onResumeOk();
      resumeFromPosition(frame.position_);
      return;
    }
    case FrameType::ERROR: {
      Frame_ERROR frame;
      if (!deserializeFrameOrError(frame, std::move(payload))) {
        return;
      }
      VLOG(3) << mode_ << " In: " << frame;

      // TODO: handle INVALID_SETUP, UNSUPPORTED_SETUP, REJECTED_SETUP

      if ((frame.errorCode_ == ErrorCode::CONNECTION_ERROR ||
           frame.errorCode_ == ErrorCode::REJECTED_RESUME) &&
          resumeCallback_) {
        auto resumeCallback = std::move(resumeCallback_);
        resumeCallback->onResumeError(
            ResumptionException(frame.payload_.cloneDataToString()));
        // fall through
      }

      close(
          std::runtime_error(frame.payload_.moveDataToString()),
          StreamCompletionSignal::ERROR);
      return;
    }
    case FrameType::SETUP: // this should be processed in SetupResumeAcceptor
    case FrameType::RESUME: // this should be processed in SetupResumeAcceptor
    case FrameType::RESERVED:
    case FrameType::LEASE:
    case FrameType::REQUEST_RESPONSE:
    case FrameType::REQUEST_FNF:
    case FrameType::REQUEST_STREAM:
    case FrameType::REQUEST_CHANNEL:
    case FrameType::REQUEST_N:
    case FrameType::CANCEL:
    case FrameType::PAYLOAD:
    case FrameType::EXT:
    default: {
      auto msg = folly::sformat(
          "Unexpected {} frame for stream 0", toString(frameType));
      closeWithError(Frame_ERROR::connectionError(std::move(msg)));
      return;
    }
  }
}

void RSocketStateMachine::handleStreamFrame(
    StreamId streamId,
    FrameType frameType,
    std::unique_ptr<folly::IOBuf> serializedFrame) {
  auto it = streamState_.streams_.find(streamId);
  if (it == streamState_.streams_.end()) {
    handleUnknownStream(streamId, frameType, std::move(serializedFrame));
    return;
  }

  // we are purposely making a copy of the reference here to avoid problems with
  // lifetime of the stateMachine when a terminating signal is delivered which
  // will cause the stateMachine to be destroyed while in one of its methods
  auto& stateElem = it->second;
  auto stateMachine = stateElem.stateMachine;

  switch (frameType) {
    case FrameType::REQUEST_N: {
      Frame_REQUEST_N frameRequestN;
      if (!deserializeFrameOrError(frameRequestN, std::move(serializedFrame))) {
        return;
      }
      VLOG(3) << mode_ << " In: " << frameRequestN;
      stateMachine->handleRequestN(frameRequestN.requestN_);
      break;
    }
    case FrameType::CANCEL: {
      VLOG(3) << mode_ << " In: " << Frame_CANCEL(streamId);
      stateMachine->handleCancel();
      break;
    }
    case FrameType::PAYLOAD: {
      Frame_PAYLOAD framePayload;
      if (!deserializeFrameOrError(framePayload, std::move(serializedFrame))) {
        return;
      }
      VLOG(3) << mode_ << " In: " << framePayload;

      if (!!(framePayload.header_.flags & FrameFlags::FOLLOWS) &&
          !stateElem.fragmentAccumulator.anyFragments()) {
        // first fragment seen for the frame; copy headers
        stateElem.fragmentAccumulator.header = framePayload.header_;
      }

      bool const hasFollowsFlag =
          !!(framePayload.header_.flags & FrameFlags::FOLLOWS);

      if (hasFollowsFlag || stateElem.fragmentAccumulator.anyFragments()) {
        stateElem.fragmentAccumulator.addPayload(
            std::move(framePayload.payload_));

        // final fragment in the payload, consume the payload
        if (!hasFollowsFlag) {
          stateMachine->handlePayload(
              stateElem.fragmentAccumulator.consumePayload(),
              stateElem.fragmentAccumulator.header.flagsComplete(),
              stateElem.fragmentAccumulator.header.flagsNext());
        }
      } else {
        // not a fragmented payload
        stateMachine->handlePayload(
            std::move(framePayload.payload_),
            framePayload.header_.flagsComplete(),
            framePayload.header_.flagsNext());
      }
      break;
    }
    case FrameType::ERROR: {
      Frame_ERROR frameError;
      if (!deserializeFrameOrError(frameError, std::move(serializedFrame))) {
        return;
      }
      VLOG(3) << mode_ << " In: " << frameError;
      stateMachine->handleError(
          std::runtime_error(frameError.payload_.moveDataToString()));
      break;
    }
    case FrameType::REQUEST_CHANNEL:
    case FrameType::REQUEST_RESPONSE:
    case FrameType::RESERVED:
    case FrameType::SETUP:
    case FrameType::LEASE:
    case FrameType::KEEPALIVE:
    case FrameType::REQUEST_FNF:
    case FrameType::REQUEST_STREAM:
    case FrameType::METADATA_PUSH:
    case FrameType::RESUME:
    case FrameType::RESUME_OK:
    case FrameType::EXT: {
      auto msg = folly::sformat(
          "Unexpected {} frame for stream {}", toString(frameType), streamId);
      closeWithError(Frame_ERROR::connectionError(std::move(msg)));
      break;
    }
    default:
      // Ignore unknown frames for compatibility with future frame types.
      break;
  }
}

void RSocketStateMachine::handleUnknownStream(
    StreamId streamId,
    FrameType frameType,
    std::unique_ptr<folly::IOBuf> serializedFrame) {
  DCHECK(streamId != 0);
  // TODO: comparing string versions is odd because from version
  // 10.0 the lexicographic comparison doesn't work
  // we should change the version to struct
  if (frameType != FrameType::PAYLOAD) {
    // don't check registerNewPeerStreamId if it's a payload - it may be an
    // additional fragment
    if (frameSerializer_->protocolVersion() > ProtocolVersion{0, 0} &&
        !streamsFactory_.registerNewPeerStreamId(streamId)) {
      return;
    }
  }

  if (!isNewStreamFrame(frameType) && (frameType != FrameType::PAYLOAD)) {
    auto msg = folly::sformat(
        "Unexpected frame {} for stream {}", toString(frameType), streamId);
    closeWithError(Frame_ERROR::connectionError(std::move(msg)));
    return;
  }

  auto saveStreamToken = [&](const Payload& payload) {
    if (coldResumeHandler_) {
      auto streamType = getStreamType(frameType);
      CHECK(streamType != StreamType::FNF);
      auto streamToken = coldResumeHandler_->generateStreamToken(
          payload, streamId, streamType);
      resumeManager_->onStreamOpen(
          streamId, RequestOriginator::REMOTE, streamToken, streamType);
    }
  };

  // called when 'frame' is the first frame for a stream, and the stream
  // is fragmented
  auto handleInitialFollowsFrame = [&](auto& frame) {
    auto it = streamFragments_.find(streamId);
    if (it == streamFragments_.end()) {
      streamFragments_.insert(std::make_pair(
          streamId,
          StreamFragmentAccumulator{frame, std::move(frame.payload_)}));
    } else {
      auto msg = folly::sformat(
          "Expected stream {} to already be in fragment cache", streamId);
      closeWithError(Frame_ERROR::connectionError(std::move(msg)));
    }
  };

  if (frameType == FrameType::PAYLOAD) {
    Frame_PAYLOAD frame;
    if (!deserializeFrameOrError(frame, std::move(serializedFrame))) {
      return;
    }

    auto it = streamFragments_.find(streamId);
    if (it == streamFragments_.end()) {
      auto msg = folly::sformat(
          "Expected payload frame in stream {} to be in fragment cache",
          streamId);
      closeWithError(Frame_ERROR::connectionError(std::move(msg)));
      return;
    }

    auto& fragRef = it->second;
    CHECK_EQ(fragRef.header.streamId, streamId);
    fragRef.addPayload(std::move(frame.payload_));

    if (!(frame.header_.flags & FrameFlags::FOLLOWS)) {
      // last fragment in the stream, trigger stream/request creation
      auto frag = std::move(it->second);
      streamFragments_.erase(it);
      auto payload = frag.consumePayload();

      if (frag.header.type == FrameType::REQUEST_CHANNEL) {
        saveStreamToken(payload);
        setupRequestChannel(
            frag.header.streamId, frag.requestN, std::move(payload));
      } else if (frag.header.type == FrameType::REQUEST_STREAM) {
        saveStreamToken(payload);
        setupRequestStream(
            frag.header.streamId, frag.requestN, std::move(payload));
      } else if (frag.header.type == FrameType::REQUEST_RESPONSE) {
        saveStreamToken(payload);
        setupRequestResponse(frag.header.streamId, std::move(payload));
      } else {
        CHECK(false) << "Stream cache had invalid stream type "
                     << fragRef.header.type << " for stream id "
                     << fragRef.header.streamId;
      }
    }
  } else if (frameType == FrameType::REQUEST_CHANNEL) {
    Frame_REQUEST_CHANNEL frame;
    if (!deserializeFrameOrError(frame, std::move(serializedFrame))) {
      return;
    }

    if (!!(frame.header_.flags & FrameFlags::FOLLOWS)) {
      handleInitialFollowsFrame(frame);
    } else {
      saveStreamToken(frame.payload_);
      setupRequestChannel(streamId, frame.requestN_, std::move(frame.payload_));
    }

  } else if (frameType == FrameType::REQUEST_STREAM) {
    Frame_REQUEST_STREAM frame;
    if (!deserializeFrameOrError(frame, std::move(serializedFrame))) {
      return;
    }

    // see if the follows flag is set, if it is, then enqueue the state machine
    // stream creation
    // if it's not set, create the stream like normal

    VLOG(3) << mode_ << " In: " << frame;

    if (!!(frame.header_.flags & FrameFlags::FOLLOWS)) {
      handleInitialFollowsFrame(frame);
    } else {
      saveStreamToken(frame.payload_);
      setupRequestStream(streamId, frame.requestN_, std::move(frame.payload_));
    }

  } else if (frameType == FrameType::REQUEST_RESPONSE) {
    Frame_REQUEST_RESPONSE frame;
    if (!deserializeFrameOrError(frame, std::move(serializedFrame))) {
      return;
    }
    VLOG(3) << mode_ << " In: " << frame;

    if (!!(frame.header_.flags & FrameFlags::FOLLOWS)) {
      handleInitialFollowsFrame(frame);
    } else {
      saveStreamToken(frame.payload_);
      setupRequestResponse(streamId, std::move(frame.payload_));
    }

  } else if (frameType == FrameType::REQUEST_FNF) {
    Frame_REQUEST_FNF frame;
    if (!deserializeFrameOrError(frame, std::move(serializedFrame))) {
      return;
    }
    VLOG(3) << mode_ << " In: " << frame;
    // no stream tracking is necessary
    requestResponder_->handleFireAndForget(std::move(frame.payload_), streamId);
  }
}

void RSocketStateMachine::setupRequestChannel(
    StreamId streamId,
    uint32_t requestN,
    Payload payload) {
  auto stateMachine =
      streamsFactory_.createChannelResponder(requestN, streamId);
  auto requestSink = requestResponder_->handleRequestChannelCore(
      std::move(payload), streamId, stateMachine);
  stateMachine->subscribe(requestSink);
}

void RSocketStateMachine::setupRequestStream(
    StreamId streamId,
    uint32_t requestN,
    Payload payload) {
  auto stateMachine = streamsFactory_.createStreamResponder(requestN, streamId);
  requestResponder_->handleRequestStreamCore(
      std::move(payload), streamId, stateMachine);
}

void RSocketStateMachine::setupRequestResponse(
    StreamId streamId,
    Payload payload) {
  auto stateMachine = streamsFactory_.createRequestResponseResponder(streamId);
  requestResponder_->handleRequestResponseCore(
      std::move(payload), streamId, stateMachine);
}

void RSocketStateMachine::sendKeepalive(std::unique_ptr<folly::IOBuf> data) {
  sendKeepalive(FrameFlags::KEEPALIVE_RESPOND, std::move(data));
}

void RSocketStateMachine::sendKeepalive(
    FrameFlags flags,
    std::unique_ptr<folly::IOBuf> data) {
  Frame_KEEPALIVE pingFrame(
      flags, resumeManager_->impliedPosition(), std::move(data));
  VLOG(3) << "Out: " << pingFrame;
  outputFrameOrEnqueue(
      frameSerializer_->serializeOut(std::move(pingFrame), isResumable_));
  stats_->keepaliveSent();
}

bool RSocketStateMachine::isPositionAvailable(ResumePosition position) const {
  return resumeManager_->isPositionAvailable(position);
}

bool RSocketStateMachine::resumeFromPositionOrClose(
    ResumePosition serverPosition,
    ResumePosition clientPosition) {
  DCHECK(!resumeCallback_);
  DCHECK(!isDisconnected());
  DCHECK(mode_ == RSocketMode::SERVER);

  bool clientPositionExist = (clientPosition == kUnspecifiedResumePosition) ||
      clientPosition <= resumeManager_->impliedPosition();

  if (clientPositionExist &&
      resumeManager_->isPositionAvailable(serverPosition)) {
    Frame_RESUME_OK resumeOkFrame(resumeManager_->impliedPosition());
    VLOG(3) << "Out: " << resumeOkFrame;
    frameTransport_->outputFrameOrDrop(
        frameSerializer_->serializeOut(std::move(resumeOkFrame)));
    resumeFromPosition(serverPosition);
    return true;
  }

  auto message = folly::to<std::string>(
      "Cannot resume server, client lastServerPosition=",
      serverPosition,
      " firstClientPosition=",
      clientPosition,
      " is not available. Last reset position is ",
      resumeManager_->firstSentPosition());

  closeWithError(Frame_ERROR::connectionError(std::move(message)));
  return false;
}

void RSocketStateMachine::resumeFromPosition(ResumePosition position) {
  DCHECK(!resumeCallback_);
  DCHECK(!isDisconnected());
  DCHECK(resumeManager_->isPositionAvailable(position));

  if (connectionEvents_) {
    connectionEvents_->onStreamsResumed();
  }
  resumeManager_->sendFramesFromPosition(position, *frameTransport_);

  for (auto& frame : streamState_.moveOutputPendingFrames()) {
    outputFrameOrEnqueue(std::move(frame));
  }

  if (!isDisconnected() && keepaliveTimer_) {
    keepaliveTimer_->start(shared_from_this());
  }
}

void RSocketStateMachine::outputFrameOrEnqueue(
    std::unique_ptr<folly::IOBuf> frame) {
  // if we are resuming we cant send any frames until we receive RESUME_OK
  if (!isDisconnected() && !resumeCallback_) {
    outputFrame(std::move(frame));
  } else {
    streamState_.enqueueOutputPendingFrame(std::move(frame));
  }
}

void RSocketStateMachine::fireAndForget(Payload request) {
  auto const streamId = streamsFactory().getNextStreamId();
  Frame_REQUEST_FNF frame{streamId, FrameFlags::EMPTY, std::move(request)};
  outputFrameOrEnqueue(std::move(frame));
}

void RSocketStateMachine::metadataPush(std::unique_ptr<folly::IOBuf> metadata) {
  Frame_METADATA_PUSH metadataPushFrame{std::move(metadata)};
  outputFrameOrEnqueue(std::move(metadataPushFrame));
}

void RSocketStateMachine::outputFrame(std::unique_ptr<folly::IOBuf> frame) {
  DCHECK(!isDisconnected());

  auto frameType = frameSerializer_->peekFrameType(*frame);
  stats_->frameWritten(frameType);

  if (isResumable_) {
    auto streamIdPtr = frameSerializer_->peekStreamId(*frame);
    CHECK(streamIdPtr) << "Error in serialized frame.";
    resumeManager_->trackSentFrame(
        *frame, frameType, *streamIdPtr, getConsumerAllowance(*streamIdPtr));
  }
  frameTransport_->outputFrameOrDrop(std::move(frame));
}

uint32_t RSocketStateMachine::getKeepaliveTime() const {
  return keepaliveTimer_
      ? static_cast<uint32_t>(keepaliveTimer_->keepaliveTime().count())
      : Frame_SETUP::kMaxKeepaliveTime;
}

bool RSocketStateMachine::isDisconnected() const {
  return !frameTransport_;
}

bool RSocketStateMachine::isClosed() const {
  return isClosed_;
}

// The max amount of user data transmitted per frame - eg the size
// of the data and metadata combined, plus the size of the frame header.
// This assumes that the frame header will never be more than 512 bytes in
// size. A CHECK in FrameTransportImpl enforces this. The idea is that
// 16M is so much larger than the ~500 bytes possibly wasted that it won't
// be noticable (0.003% wasted at most)
constexpr size_t GENEROUS_MAX_FRAME_SIZE = 0xFFFFFF - 512;

// writeFragmented takes a `payload` and splits it up into chunks which
// are sent as fragmented requests. The first fragmented payload is
// given to writeInitialFrame, which is expected to write the initial
// "REQUEST_" or "PAYLOAD" frame of a stream or response. writeFragmented
// then writes the rest of the frames as payloads.
//
// writeInitialFrame
//  - called with the payload of the first frame to send, and any additional
//    flags (eg, addFlags with FOLLOWS, if there are more frames to write)
// streamId
//  - The stream ID to write additional fragments with
// addFlags
//  - All flags that writeInitialFrame wants to write the first frame with,
//    and all flags that subsequent fragmented payloads will be sent with
// payload
//  - The unsplit payload to send, possibly in multiple fragments
template <typename WriteInitialFrame>
void RSocketStateMachine::writeFragmented(
    WriteInitialFrame writeInitialFrame,
    StreamId const streamId,
    FrameFlags const addFlags,
    Payload payload) {
  folly::IOBufQueue metaQueue{folly::IOBufQueue::cacheChainLength()};
  folly::IOBufQueue dataQueue{folly::IOBufQueue::cacheChainLength()};

  // have to keep track of "did the full payload even have a metadata", because
  // the rsocket protocol makes a distinction between a zero-length metadata
  // and a null metadata.
  bool const haveNonNullMeta = !!payload.metadata;
  metaQueue.append(std::move(payload.metadata));
  dataQueue.append(std::move(payload.data));

  bool isFirstFrame = true;

  while (true) {
    Payload sendme;

    // chew off some metadata (splitAtMost will never return a null pointer,
    // safe to compute length on it always)
    if (haveNonNullMeta) {
      sendme.metadata = metaQueue.splitAtMost(GENEROUS_MAX_FRAME_SIZE);
      DCHECK_GE(
          GENEROUS_MAX_FRAME_SIZE, sendme.metadata->computeChainDataLength());
    }
    sendme.data = dataQueue.splitAtMost(
        GENEROUS_MAX_FRAME_SIZE - (haveNonNullMeta ? sendme.metadata->computeChainDataLength() : 0));


    auto const metaLeft = metaQueue.chainLength();
    auto const dataLeft = dataQueue.chainLength();
    auto const moreFragments = metaLeft || dataLeft;
    auto const flags = (moreFragments ? FrameFlags::FOLLOWS : FrameFlags::EMPTY) | addFlags;

    if (isFirstFrame) {
      isFirstFrame = false;
      writeInitialFrame(std::move(sendme), flags);
    } else {
      outputFrameOrEnqueue(Frame_PAYLOAD(streamId, flags, std::move(sendme)));
    }

    if (!moreFragments) {
      break;
    }
  }
}

void RSocketStateMachine::writeNewStream(
    StreamId streamId,
    StreamType streamType,
    uint32_t initialRequestN,
    Payload payload) {
  if (coldResumeHandler_ && streamType != StreamType::FNF) {
    auto streamToken =
        coldResumeHandler_->generateStreamToken(payload, streamId, streamType);
    resumeManager_->onStreamOpen(
        streamId, RequestOriginator::LOCAL, streamToken, streamType);
  }

  // for simplicity, require that sent buffers don't consist of chains
  writeFragmented(
    [&](Payload p, FrameFlags flags) {
      switch (streamType) {
        case StreamType::CHANNEL:
          outputFrameOrEnqueue(Frame_REQUEST_CHANNEL(
              streamId, flags, initialRequestN, std::move(p)));
          break;
        case StreamType::STREAM:
          outputFrameOrEnqueue(Frame_REQUEST_STREAM(
              streamId, flags, initialRequestN, std::move(p)));
          break;
        case StreamType::REQUEST_RESPONSE:
          outputFrameOrEnqueue(Frame_REQUEST_RESPONSE(
              streamId, flags, std::move(p)));
          break;
        case StreamType::FNF:
          outputFrameOrEnqueue(
              Frame_REQUEST_FNF(streamId, flags, std::move(p)));
          break;

        default:
          CHECK(false) << "invalid stream type " << toString(streamType);
      }
    },
    streamId,
    FrameFlags::EMPTY,
    std::move(payload));
}

void RSocketStateMachine::writeRequestN(Frame_REQUEST_N&& frame) {
  outputFrameOrEnqueue(std::move(frame));
}

void RSocketStateMachine::writeCancel(Frame_CANCEL&& frame) {
  outputFrameOrEnqueue(std::move(frame));
}

void RSocketStateMachine::writePayload(Frame_PAYLOAD&& f) {
  Frame_PAYLOAD frame = std::move(f);
  auto const streamId = frame.header_.streamId;
  auto const initialFlags = frame.header_.flags;

  writeFragmented(
      [this, streamId](Payload p, FrameFlags flags) {
        outputFrameOrEnqueue(Frame_PAYLOAD(streamId, flags, std::move(p)));
      },
      streamId,
      initialFlags,
      std::move(frame.payload_));
}

void RSocketStateMachine::writeError(Frame_ERROR&& frame) {
  // TODO: implement fragmentation for writeError as well
  outputFrameOrEnqueue(std::move(frame));
}

void RSocketStateMachine::onStreamClosed(
    StreamId streamId,
    StreamCompletionSignal signal) {
  endStream(streamId, signal);
}

bool RSocketStateMachine::ensureOrAutodetectFrameSerializer(
    const folly::IOBuf& firstFrame) {
  if (frameSerializer_) {
    return true;
  }

  if (mode_ != RSocketMode::SERVER) {
    // this should never happen as clients are initized with FrameSerializer
    // instance
    DCHECK(false);
    return false;
  }

  auto serializer = FrameSerializer::createAutodetectedSerializer(firstFrame);
  if (!serializer) {
    LOG(ERROR) << "unable to detect protocol version";
    return false;
  }

  VLOG(2) << "detected protocol version" << serializer->protocolVersion();
  frameSerializer_ = std::move(serializer);
  frameSerializer_->preallocateFrameSizeField() =
      frameTransport_ && frameTransport_->isConnectionFramed();

  return true;
}

size_t RSocketStateMachine::getConsumerAllowance(StreamId streamId) const {
  size_t consumerAllowance = 0;
  auto it = streamState_.streams_.find(streamId);
  if (it != streamState_.streams_.end()) {
    consumerAllowance = it->second.stateMachine->getConsumerAllowance();
  }
  return consumerAllowance;
}

void RSocketStateMachine::registerSet(ConnectionSet* set) {
  connectionSet_ = set;
}

DuplexConnection* RSocketStateMachine::getConnection() {
  return frameTransport_ ? frameTransport_->getConnection() : nullptr;
}

void RSocketStateMachine::setProtocolVersionOrThrow(
    ProtocolVersion version,
    const std::shared_ptr<FrameTransport>& transport) {
  CHECK(version != ProtocolVersion::Unknown);

  // TODO(lehecka): this is a temporary guard to make sure the transport is
  // explicitly closed when exceptions are thrown. The right solution is to
  // automatically close duplex connection in the destructor when unique_ptr
  // is released
  auto transportGuard = folly::makeGuard([&] { transport->close(); });

  if (frameSerializer_) {
    if (frameSerializer_->protocolVersion() != version) {
      // serializer is not interchangeable, it would screw up resumability
      throw std::runtime_error{"Protocol version mismatch"};
    }
  } else {
    auto frameSerializer = FrameSerializer::createFrameSerializer(version);
    if (!frameSerializer) {
      throw std::runtime_error{"Invalid protocol version"};
    }

    frameSerializer_ = std::move(frameSerializer);
    frameSerializer_->preallocateFrameSizeField() =
        frameTransport_ && frameTransport_->isConnectionFramed();
  }

  transportGuard.dismiss();
}

} // namespace rsocket
