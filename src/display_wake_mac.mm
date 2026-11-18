#import <AppKit/AppKit.h>
#define GLFW_INCLUDE_NONE
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3.h>
#include <GLFW/glfw3native.h>

#include <functional>

// Cocoa delivers these notifications even when Rack is waiting for events with its window hidden.
void* displayWatchWake(GLFWwindow* glfwWindow, std::function<void()> leaveFullscreen) {
	if (!glfwWindow) return nullptr;
	NSWindow* window = glfwGetCocoaWindow(glfwWindow);
	if (!window) return nullptr;
	NSNotificationCenter* center = NSWorkspace.sharedWorkspace.notificationCenter;
	NSMutableArray* tokens = [[NSMutableArray alloc] init];
	auto wake = ^(NSNotification*) {
		leaveFullscreen();
		if (window.miniaturized) [window deminiaturize:nil];
		[window makeKeyAndOrderFront:nil];
		glfwPostEmptyEvent();
	};
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
