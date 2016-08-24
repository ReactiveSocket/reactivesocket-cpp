// Copyright 2004-present Facebook. All Rights Reserved.

#pragma once

#include <string>
#include "Payload.h"

namespace reactivesocket {
class ConnectionSetupPayload {
 public:
  ConnectionSetupPayload(
      std::string _metadataMimeType,
      std::string _dataMimeType,
      Payload _payload)
      : metadataMimeType(_metadataMimeType),
        dataMimeType(_dataMimeType),
        payload(std::move(_payload)){};

  std::string metadataMimeType;
  std::string dataMimeType;
  Payload payload;
};
}
