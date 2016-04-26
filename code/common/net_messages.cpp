#include "lib/assert.h"
#include "lib/serialization.h"
#include "common/conversion.h"
#include "net_messages.h"

const memsize MinMessageSize = 1;
const memsize ReplyNetMessageSize = 1;
const memsize OrderListNetMessageSize = 1;
const memsize StartNetMessageSize = 3;

const memsize OrderMessageMinSize = 9;
const memsize OrderMessageHeaderSize = 9;

void WriteType(serializer *S, net_message_type Type) {
  ui8 TypeUI8 = SafeCastIntToUI8(Type);
  SerializerWriteUI8(S, TypeUI8);
}

static order_net_message UnserializeOrderHeader(serializer *S) {
  net_message_type Type = (net_message_type)SerializerReadUI8(S);
  Assert(Type == net_message_type_order);

  order_net_message Message;
  Message.UnitCount = SerializerReadUI16(S);
  Message.Target.X = SerializerReadSI16(S);
  Message.Target.Y = SerializerReadSI16(S);
  Message.UnitIDs = NULL;

  return Message;
}

memsize SerializeStartNetMessage(memsize PlayerCount, memsize PlayerIndex, buffer Buffer) {
  serializer Writer = CreateSerializer(Buffer);

  ui8 TypeUI8 = SafeCastIntToUI8(net_message_type_start);
  SerializerWriteUI8(&Writer, TypeUI8);

  ui8 PlayerCountUI8 = SafeCastIntToUI8(PlayerCount);
  SerializerWriteUI8(&Writer, PlayerCountUI8);

  ui8 PlayerIndexUI8 = SafeCastIntToUI8(PlayerIndex);
  SerializerWriteUI8(&Writer, PlayerIndexUI8);

  Assert(Writer.Position == StartNetMessageSize);

  return Writer.Position;
}

bool ValidateMessageLength(buffer Buffer, net_message_type Type) {
  memsize RequiredLength = 0;
  switch(Type) {
    case net_message_type_start:
      RequiredLength = StartNetMessageSize;
      break;
    case net_message_type_reply:
      RequiredLength = ReplyNetMessageSize;
      break;
    case net_message_type_order_list:
      RequiredLength = OrderListNetMessageSize;
      break;
    case net_message_type_order:
      if(Buffer.Length < OrderMessageHeaderSize) {
        return false;
      }
      else {
        serializer S = CreateSerializer(Buffer);
        order_net_message Message = UnserializeOrderHeader(&S);
        RequiredLength = 7 + Message.UnitCount * 2;
      }
      break;
    default:
      InvalidCodePath;
  }

  return RequiredLength <= Buffer.Length;
}

memsize SerializeReplyNetMessage(buffer Buffer) {
  ui8 TypeInt = SafeCastIntToUI8(net_message_type_reply);
  serializer Writer = CreateSerializer(Buffer);
  SerializerWriteUI8(&Writer, TypeInt);
  Assert(Writer.Position == ReplyNetMessageSize);
  return Writer.Position;
}

memsize SerializeOrderListNetMessage(buffer Out) {
  serializer W = CreateSerializer(Out);
  WriteType(&W, net_message_type_order_list);
  Assert(W.Position == OrderListNetMessageSize);
  return W.Position;
}

memsize SerializeOrderNetMessage(simulation_unit_id *UnitIDs, memsize UnitCount, ivec2 Target, buffer Out) {
  serializer W = CreateSerializer(Out);
  WriteType(&W, net_message_type_order);
  ui16 UnitCountUI16 = SafeCastIntToUI16(UnitCount);
  SerializerWriteUI16(&W, UnitCountUI16);

  SerializerWriteSI16(&W, Target.X);
  SerializerWriteSI16(&W, Target.Y);

  for(memsize I=0; I<UnitCount; ++I) {
    ui16 IDUI16 = SafeCastIntToUI16(UnitIDs[I]);
    SerializerWriteUI16(&W, IDUI16);
  }

  return W.Position;
}

net_message_type UnserializeNetMessageType(buffer Input) {
  serializer S = CreateSerializer(Input);
  net_message_type Type = (net_message_type)SerializerReadUI8(&S);
  return Type;
}

order_net_message UnserializeOrderNetMessage(buffer Input, linear_allocator Allocator) {
  serializer S = CreateSerializer(Input);
  order_net_message Message = UnserializeOrderHeader(&S);

  memsize IDsSize = sizeof(simulation_unit_id) * Message.UnitCount;
  Message.UnitIDs = (simulation_unit_id*)LinearAllocate(&Allocator, IDsSize);
  for(memsize I=0; I<Message.UnitCount; ++I) {
    Message.UnitIDs[I] = SerializerReadUI16(&S);
  }

  return Message;
}

start_net_message UnserializeStartNetMessage(buffer Buffer) {
  serializer S = CreateSerializer(Buffer);
  net_message_type Type = (net_message_type)SerializerReadUI8(&S);
  Assert(Type == net_message_type_start);

  start_net_message Message;
  Message.PlayerCount = SerializerReadUI8(&S);
  Message.PlayerIndex = SerializerReadUI8(&S);

  return Message;
}

order_list_net_message UnserializeOrderListNetMessage(buffer Input) {
  serializer S = CreateSerializer(Input);
  net_message_type Type = (net_message_type)SerializerReadUI8(&S);
  Assert(Type == net_message_type_order_list);

  order_list_net_message Message;
  Message.Count = 0;

  return Message;
}

bool ValidateNetMessageType(net_message_type Type) {
  return Type < net_message_type_count;
}

bool ValidateStartNetMessage(start_net_message Message) {
  // TODO: Check properties of message
  return true;
}

bool ValidateOrderListNetMessage(order_list_net_message Message) {
  // TODO: Check properties of message
  return true;
}
