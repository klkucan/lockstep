#include <stdio.h>
#include <math.h>
#include <signal.h>
#include <pthread.h>
#include <Foundation/Foundation.h>
#include <AppKit/AppKit.h>
#include "lib/assert.h"
#include "common/net_messages.h"
#include "common/memory.h"
#include "common/posix_time.h"
#include "net_commands.h"
#include "net_events.h"
#include "game.h"
#include "posix_net.h"
#include "opengl.h"

static bool TerminationRequested;

struct osx_state {
  bool Running;
  void *Memory;
  NSWindow *Window;
  NSOpenGLContext *OGLContext;
  linear_allocator Allocator;
  buffer ClientMemory;
  chunk_list NetCommandList;
  chunk_list NetEventList;
  chunk_list RenderCommandList;
  ivec2 Resolution;
  pthread_t NetThread;
  posix_net_context NetContext;
  game_mouse Mouse;
};

@interface ClientAppDelegate : NSObject <NSApplicationDelegate>
@end

@implementation ClientAppDelegate
- (NSApplicationTerminateReply)applicationShouldTerminate:(NSApplication*)sender {
  TerminationRequested = true;
  return NSTerminateCancel;
}
@end

@interface ClientWindowDelegate : NSObject <NSWindowDelegate>
@end

@implementation ClientWindowDelegate
- (BOOL)windowShouldClose:(id)sender {
    TerminationRequested = true;
    return NO;
}
@end

static void HandleSigint(int signum) {
  TerminationRequested = true;
}

static void InitMemory(osx_state *State) {
  memsize MemorySize = 1024*1024;
  State->Memory = malloc(MemorySize);
  InitLinearAllocator(&State->Allocator, State->Memory, MemorySize);
}

static void TerminateMemory(osx_state *State) {
  TerminateLinearAllocator(&State->Allocator);
  free(State->Memory);
  State->Memory = NULL;
}

static void ExecuteNetCommands(posix_net_context *Context, chunk_list *Cmds) {
  for(;;) {
    buffer Command = ChunkListRead(Cmds);
    if(Command.Length == 0) {
      break;
    }
    net_command_type Type = UnserializeNetCommandType(Command);
    switch(Type) {
      case net_command_type_send: {
        send_net_command SendCommand = UnserializeSendNetCommand(Command);
        PosixNetSend(Context, SendCommand.Message);
        break;
      }
      case net_command_type_shutdown: {
        ShutdownPosixNet(Context);
        break;
      }
      default:
        InvalidCodePath;
    }
  }
  ResetChunkList(Cmds);
}

static void ExecuteRenderCommands(chunk_list *Commands) {
  DisplayOpenGL(Commands);
  ResetChunkList(Commands);
}

static void ReadNet(posix_net_context *Context, chunk_list *Events) {
  static ui8 ReadBufferBlock[NETWORK_EVENT_MAX_LENGTH];
  static buffer ReadBuffer = {
    .Addr = &ReadBufferBlock,
    .Length = sizeof(ReadBufferBlock)
  };
  memsize Length;
  while((Length = ReadPosixNetEvent(Context, ReadBuffer))) {
    buffer Event = {
      .Addr = ReadBuffer.Addr,
      .Length = Length
    };
    ChunkListWrite(Events, Event);
  }
}

static void SetupOSXMenu() {
  NSMenu *Menu = [[NSMenu alloc] init];
  NSMenuItem *QuitItem = [[NSMenuItem alloc] initWithTitle:@"Quit" action:@selector(terminate:) keyEquivalent:@"q"];
  [Menu addItem:QuitItem];

  NSMenuItem *BarItem = [[NSMenuItem alloc] init];
  [BarItem setSubmenu:Menu];

  NSMenu *Bar = [[NSMenu alloc] init];
  [Bar addItem:BarItem];

  [NSApp setMainMenu:Bar];

  [Menu release];
  [QuitItem release];
  [BarItem release];
  [Bar release];
}

static NSOpenGLContext* CreateOGLContext() {
  NSOpenGLPixelFormatAttribute Attributes[] = {
    NSOpenGLPFADoubleBuffer,
    NSOpenGLPFAOpenGLProfile,
    NSOpenGLProfileVersionLegacy,
    0
  };
  NSOpenGLPixelFormat *PixelFormat = [[NSOpenGLPixelFormat alloc] initWithAttributes:Attributes];
  if(PixelFormat == nil) {
    return NULL;
  }

  NSOpenGLContext *Context = [[NSOpenGLContext alloc] initWithFormat:PixelFormat shareContext:nil];

  GLint Sync = 1;
  [Context setValues:&Sync forParameter:NSOpenGLCPSwapInterval];

  [PixelFormat release];

  return Context;
}

static NSWindow* CreateOSXWindow(ivec2 Resolution) {
  int StyleMask = NSTitledWindowMask | NSClosableWindowMask | NSMiniaturizableWindowMask;

  NSScreen *Screen = [NSScreen mainScreen];
  CGRect Rect = NSMakeRect(
    0,
    0,
    Resolution.X / Screen.backingScaleFactor,
    Resolution.Y / Screen.backingScaleFactor
  );
  NSWindow *Window = [[NSWindow alloc] initWithContentRect:Rect
                                          styleMask:StyleMask
                                            backing:NSBackingStoreBuffered
                                              defer:NO
                                              screen:Screen];
  if(Window == nil) {
    return NULL;
  }

  ClientWindowDelegate *Delegate = [[ClientWindowDelegate alloc] init];
  Window.delegate = Delegate;
  Window.title = [NSString stringWithUTF8String:"Lockstep Client"];

  [Window center];
  [Window makeKeyAndOrderFront:nil];

  return Window;
}

static void ProcessOSXMessages(NSWindow *Window, game_mouse *Mouse) {
  while(true) {
    NSEvent *Event = [NSApp nextEventMatchingMask:NSAnyEventMask
                                        untilDate:[NSDate distantPast]
                                           inMode:NSDefaultRunLoopMode
                                          dequeue:YES];
    if(Event == nil) {
      return;
    }
    switch(Event.type) {
      case NSLeftMouseDown:
      case NSMouseMoved: {
        NSPoint WindowLoc;
        if(Event.window == Window) {
          WindowLoc = Event.locationInWindow;
        }
        else {
          const NSRect ScreenRect = NSMakeRect(Event.locationInWindow.x, Event.locationInWindow.y, 0, 0);
          const NSRect GameWindowRect = [Window convertRectFromScreen:ScreenRect];
          WindowLoc = GameWindowRect.origin;
        }

        if(NSPointInRect(WindowLoc, Window.contentView.bounds)) {
          const NSRect WindowRect = NSMakeRect(WindowLoc.x, WindowLoc.y, 0, 0);
          const NSRect BackingRect = [Window convertRectToBacking:WindowRect];
          Mouse->Pos.X = BackingRect.origin.x;
          Mouse->Pos.Y = BackingRect.origin.y;
          if(Event.type == NSLeftMouseDown) {
            Mouse->ButtonPressed = true;
            Mouse->ButtonChangeCount++;
          }
        }
        break;
      }
      case NSLeftMouseUp:
        Mouse->ButtonPressed = false;
        Mouse->ButtonChangeCount++;
        break;
      default:
        break;
    }
    [NSApp sendEvent:Event];
  }
}

r32 GetAspectRatio(ivec2 Resolution) {
  rvec2 Real = ConvertIvec2ToRvec2(Resolution);
  return Real.X / Real.Y;
}

int main() {
  osx_state State;
  State.Resolution.X = 1600;
  State.Resolution.Y = 1200;

  State.Mouse.Pos = MakeIvec2(0, 0);
  InitMemory(&State);

  {
    buffer Buffer;
    Buffer.Length = NETWORK_COMMAND_MAX_LENGTH*100;
    Buffer.Addr = LinearAllocate(&State.Allocator, Buffer.Length);
    InitChunkList(&State.NetCommandList, Buffer);
  }

  {
    buffer Buffer;
    Buffer.Length = NETWORK_EVENT_MAX_LENGTH*100;
    Buffer.Addr = LinearAllocate(&State.Allocator, Buffer.Length);
    InitChunkList(&State.NetEventList, Buffer);
  }

  {
    buffer Buffer;
    Buffer.Length = 1024*200;
    Buffer.Addr = LinearAllocate(&State.Allocator, Buffer.Length);
    InitChunkList(&State.RenderCommandList, Buffer);
  }

  InitPosixNet(&State.NetContext);
  {
    int Result = pthread_create(&State.NetThread, 0, RunPosixNet, &State.NetContext);
    Assert(Result == 0);
  }

  {
    buffer *B = &State.ClientMemory;
    B->Length = 1024*512;
    B->Addr = LinearAllocate(&State.Allocator, B->Length);
  }
  InitGame(State.ClientMemory);

  NSApplication *App = [NSApplication sharedApplication];
  App.delegate = [[ClientAppDelegate alloc] init];
  App.activationPolicy = NSApplicationActivationPolicyRegular;
  SetupOSXMenu();
  [App finishLaunching];

  State.Window = CreateOSXWindow(State.Resolution);
  Assert(State.Window != NULL);

  State.OGLContext = CreateOGLContext();
  Assert(State.OGLContext != NULL);
  [State.OGLContext makeCurrentContext];
  [State.OGLContext setView:State.Window.contentView];

#ifdef DEBUG
  [NSApp activateIgnoringOtherApps:YES];
#endif

  signal(SIGINT, HandleSigint);
  InitOpenGL(GetAspectRatio(State.Resolution));
  State.Running = true;
  while(State.Running) {
    State.Mouse.ButtonChangeCount = 0;
    ProcessOSXMessages(State.Window, &State.Mouse);
    ReadNet(&State.NetContext, &State.NetEventList);

    game_platform GamePlatform;
    GamePlatform.Time = GetTime();
    GamePlatform.Mouse = &State.Mouse;
    GamePlatform.Resolution = State.Resolution;
    GamePlatform.TerminationRequested = TerminationRequested;
    UpdateGame(
      &GamePlatform,
      &State.NetEventList,
      &State.NetCommandList,
      &State.RenderCommandList,
      &State.Running,
      State.ClientMemory
    );
    ResetChunkList(&State.NetEventList);
    ExecuteNetCommands(&State.NetContext, &State.NetCommandList);
    ExecuteRenderCommands(&State.RenderCommandList);
    [State.OGLContext flushBuffer];
  }

  {
    printf("Waiting for thread join...\n");
    int Result = pthread_join(State.NetThread, 0);
    Assert(Result == 0);
  }

  {
    ClientAppDelegate *D = App.delegate;
    App.delegate = nil;
    [D release];
  }
  {
    ClientWindowDelegate *D = State.Window.delegate;
    State.Window.delegate = nil;
    [D release];
  }
  [State.Window release];
  [State.OGLContext release];

  TerminateChunkList(&State.RenderCommandList);
  TerminateChunkList(&State.NetEventList);
  TerminateChunkList(&State.NetCommandList);
  TerminatePosixNet(&State.NetContext);
  TerminateMemory(&State);
  printf("Gracefully terminated.\n");
  return 0;
}
