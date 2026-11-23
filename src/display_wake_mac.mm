#import <AppKit/AppKit.h>
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <functional>

// Written to Rack's log by Display.cpp, so a lost window can be traced afterwards.
void displayWakeLog(const char* text);

namespace {

// Puts Rack's window back where the user can reach it: the app unhidden, the window out of the Dock, in front and
// key. Says in the log what state it found.
void bringBack(NSWindow* window, const char* when) {
	const bool hidden = NSApp.hidden;
	const bool mini = window.miniaturized;
	const bool visible = window.visible;
	const bool onSpace = window.onActiveSpace;
	char line[200];
	snprintf(line, sizeof line, "Display full screen, %s: app hidden %d, window miniaturized %d, visible %d, on this space %d, screens %lu",
		when, hidden, mini, visible, onSpace, (unsigned long) NSScreen.screens.count);
	displayWakeLog(line);
	if (hidden) [NSApp unhide:nil];
	if (mini) [window deminiaturize:nil];
	[NSApp activateIgnoringOtherApps:YES];
	[window makeKeyAndOrderFront:nil];
	[window orderFrontRegardless];
	glfwPostEmptyEvent();
}

}  // namespace

// Cocoa delivers these notifications even when Rack is waiting for events with its window hidden.
//
// Full screen is left BEFORE the Mac sleeps, while the screen is still there: the window goes back to its ordinary
// place and size first, so nothing has to be repaired afterwards. (Leaving it only after the wake, as this did at
// first, could leave Rack running with no window on screen and none in "Show All Windows" until its Dock icon was
// clicked.) The wake is still watched, for a sleep that gave no warning: the window is brought back at once and
// again a little later, because the screens can come back after the notification does.
void* displayWatchWake(GLFWwindow* glfwWindow, std::function<void()> leaveFullscreen) {
	if (!glfwWindow) return nullptr;
	NSWindow* window = glfwGetCocoaWindow(glfwWindow);
	if (!window) return nullptr;
	NSNotificationCenter* center = NSWorkspace.sharedWorkspace.notificationCenter;
	NSMutableArray* tokens = [[NSMutableArray alloc] init];
	auto sleep = ^(NSNotification* note) {
		displayWakeLog([[NSString stringWithFormat:@"Display full screen: leaving it before sleep (%@)", note.name] UTF8String]);
		leaveFullscreen();
		glfwPostEmptyEvent();
	};
	auto wake = ^(NSNotification*) {
		leaveFullscreen();
		bringBack(window, "at wake");
		// These run whether or not the full-screen picture still exists, so they hold only the window.
		for (NSNumber* seconds in @[@0.5, @1.5, @4.0]) {
			dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(seconds.doubleValue * NSEC_PER_SEC)), dispatch_get_main_queue(), ^{
				bringBack(window, "after wake");
			});
		}
	};
	for (NSString* name in @[NSWorkspaceWillSleepNotification, NSWorkspaceScreensDidSleepNotification]) {
		id token = [center addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:sleep];
		[tokens addObject:token];
	}
	for (NSString* name in @[NSWorkspaceDidWakeNotification, NSWorkspaceScreensDidWakeNotification]) {
		id token = [center addObserverForName:name object:nil queue:NSOperationQueue.mainQueue usingBlock:wake];
		[tokens addObject:token];
	}
	return tokens;
}

void displayUnwatchWake(void* handle) {
	NSMutableArray* tokens = (NSMutableArray*)handle;
	for (id token in tokens)
		[NSWorkspace.sharedWorkspace.notificationCenter removeObserver:token];
	[tokens release];
}
