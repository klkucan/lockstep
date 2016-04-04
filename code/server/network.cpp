#include <stddef.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <netinet/in.h>
#include <stdio.h>
#include "lib/assert.h"
#include "lib/min_max.h"
#include "lib/chunk_ring_buffer.h"
#include "common/shared.h"
#include "client_set.h"
#include "network.h"

enum main_state {
  main_state_running,
  main_state_disconnecting,
  main_state_stopped
};

enum command_type {
  command_type_disconnect
};

struct base_command {
  command_type Type;
};

#define RECEIVE_BUFFER_SIZE 4096
static ui8 ReceiveBuffer[RECEIVE_BUFFER_SIZE];
static int WakeReadFD;
static int WakeWriteFD;
static int HostFD;
static int ReadFDMax;
static client_set ClientSet;
static void *CommandData;
static void *EventData;
static main_state MainState;
static chunk_ring_buffer CommandList;
static chunk_ring_buffer EventBuffer;

static void RequestWake() {
  ui8 X = 1;
  write(WakeWriteFD, &X, 1);
}

static void CheckNewReadFD(int NewFD) {
  ReadFDMax = MaxInt(ReadFDMax, NewFD);
}

static void RecalcReadFDMax() {
  ReadFDMax = 0;
  client_set_iterator Iterator = CreateClientSetIterator(&ClientSet);
  while(AdvanceClientSetIterator(&Iterator)) {
    CheckNewReadFD(Iterator.Client->FD);
  }
  CheckNewReadFD(WakeReadFD);
  CheckNewReadFD(HostFD);
}

void InitNetwork() {
  ReadFDMax = 0;

  int FDs[2];
  pipe(FDs);
  WakeReadFD = FDs[0];
  WakeWriteFD = FDs[1];
  CheckNewReadFD(WakeReadFD);

  memsize CommandDataLength = 1024*100;
  CommandData = malloc(CommandDataLength);
  InitChunkRingBuffer(&CommandList, 50, CommandData, CommandDataLength);

  memsize EventDataLength = 1024*100;
  EventData = malloc(EventDataLength);
  InitChunkRingBuffer(&EventBuffer, 50, EventData, EventDataLength);

  InitClientSet(&ClientSet);

  HostFD = socket(PF_INET, SOCK_STREAM, 0);
  Assert(HostFD != -1);
  CheckNewReadFD(HostFD);
  fcntl(HostFD, F_SETFL, O_NONBLOCK);

  struct sockaddr_in Address;
  memset(&Address, 0, sizeof(Address));
  Address.sin_len = sizeof(Address);
  Address.sin_family = AF_INET;
  Address.sin_port = htons(4321);
  Address.sin_addr.s_addr = INADDR_ANY;

  int BindResult = bind(HostFD, (struct sockaddr *)&Address, sizeof(Address));
  Assert(BindResult != -1);

  int ListenResult = listen(HostFD, 5);
  Assert(ListenResult == 0);
}

void TerminateNetwork() {
  close(WakeReadFD);
  close(WakeWriteFD);

  int Result = close(HostFD);
  Assert(Result == 0);

  TerminateClientSet(&ClientSet);

  TerminateChunkRingBuffer(&CommandList);
  free(CommandData);
  CommandData = NULL;

  TerminateChunkRingBuffer(&EventBuffer);
  free(EventData);
  EventData = NULL;
}

void DisconnectNetwork() {
  base_command Command;
  Command.Type = command_type_disconnect;
  ChunkRingBufferWrite(&CommandList, &Command, sizeof(Command));
  RequestWake();
}

static void ProcessCommands(main_state *MainState) {
  base_command *Command;
  memsize Length;
  while((Length = ChunkRingBufferRead(&CommandList, (void**)&Command))) {
    switch(Command->Type) {
      case command_type_disconnect: {
        client_set_iterator Iterator = CreateClientSetIterator(&ClientSet);
        while(AdvanceClientSetIterator(&Iterator)) {
          printf("Shutdown...\n");
          int Result = shutdown(Iterator.Client->FD, SHUT_RDWR);
          Assert(Result == 0);
        }
        *MainState = main_state_disconnecting;
      }
      break;
      default:
        InvalidCodePath;
    }
  }
}

memsize ReadNetworkEvent(network_base_event **Event) {
  return ChunkRingBufferRead(&EventBuffer, (void**)Event);
}

void* RunNetwork(void *Data) {
  MainState = main_state_running;

  while(MainState != main_state_stopped) {
    fd_set FDSet;
    FD_ZERO(&FDSet);
    {
      client_set_iterator Iterator = CreateClientSetIterator(&ClientSet);
      while(AdvanceClientSetIterator(&Iterator)) {
        FD_SET(Iterator.Client->FD, &FDSet);
      }
    }
    FD_SET(HostFD, &FDSet);
    FD_SET(WakeReadFD, &FDSet);

    int SelectResult = select(ReadFDMax+1, &FDSet, NULL, NULL, NULL);
    Assert(SelectResult != -1);

    {
      client_set_iterator Iterator = CreateClientSetIterator(&ClientSet);
      while(AdvanceClientSetIterator(&Iterator)) {
        client *Client = Iterator.Client;
        if(FD_ISSET(Client->FD, &FDSet)) {
          ssize_t Result = recv(Client->FD, ReceiveBuffer, RECEIVE_BUFFER_SIZE, 0);
          if(Result == 0) {
            int Result = close(Client->FD);
            Assert(Result != -1);
            DestroyClient(&Iterator);
            network_base_event Event;
            Event.Type = network_event_type_disconnect;
            ChunkRingBufferWrite(&EventBuffer, &Event, sizeof(Event));
            printf("A client disconnected.\n");
          }
          else {
            ByteRingBufferWrite(&Client->InBuffer, ReceiveBuffer, Result);
            printf("Got something\n");
          }
        }
        RecalcReadFDMax();
      }
    }

    if(FD_ISSET(WakeReadFD, &FDSet)) {
      ui8 X;
      int Result = read(WakeReadFD, &X, 1);
      Assert(Result != -1);
      ProcessCommands(&MainState);
    }

    if(FD_ISSET(HostFD, &FDSet) && ClientSet.Count != CLIENT_SET_MAX) {
      int ClientFD = accept(HostFD, NULL, NULL);
      Assert(ClientFD != -1);
      CreateClient(&ClientSet, ClientFD);
      CheckNewReadFD(ClientFD);
      network_base_event Event;
      Event.Type = network_event_type_connect;
      ChunkRingBufferWrite(&EventBuffer, &Event, sizeof(Event));
      printf("Someone connected!\n");
    }

    if(MainState == main_state_disconnecting) {
      if(ClientSet.Count == 0) {
        printf("No more clients. Stopping.\n");
        MainState = main_state_stopped;
      }
    }
  }

  return NULL;
}
