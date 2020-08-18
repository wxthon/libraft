/*
 * Copyright (C) codedump
 */

#ifndef __LIBRAFT_BASE_MESSAGE_H__
#define __LIBRAFT_BASE_MESSAGE_H__

#include "base/define.h"
#include "base/error.h"
#include "base/typedef.h"

BEGIN_NAMESPACE

class MessageHandler;

class Message {
  friend class MessageHandler;
public:
  virtual ~Message() {}

  const MessageType Type() const { return type_; }
  
protected:
  MessageId id_;
  EntityId srcId_;
  EntityId dstId_;
  Error error_;
  MessageType type_;
  bool isResponse_;
};

class MessageHandler {
public:

  virtual ~MessageHandler() {}
};

END_NAMESPACE

#endif // __LIBRAFT_BASE_MESSAGE_H__