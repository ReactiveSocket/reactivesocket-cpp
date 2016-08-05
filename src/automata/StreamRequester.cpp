// Copyright 2004-present Facebook. All Rights Reserved.

#include "StreamRequester.h"

#include <algorithm>
#include <iostream>

namespace reactivesocket {

void StreamRequesterBase::sendRequestFrame(
    FrameFlags flags,
    size_t initialN,
    Payload&& request) {
  Frame_REQUEST_STREAM frame(
      streamId_,
      flags,
      static_cast<uint32_t>(initialN),
      FrameMetadata::empty(),
      std::move(request));
  // We must inform ConsumerMixin about an implicit allowance we have
  // requested from the remote end.
  addImplicitAllowance(initialN);
  connection_->onNextFrame(frame);
}

std::ostream& StreamRequesterBase::logPrefix(std::ostream& os) {
  return os << "StreamRequester(" << &connection_ << ", " << streamId_ << "): ";
}
}
