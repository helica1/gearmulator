#import <Foundation/Foundation.h>

// Runs the main run loop for a while so JUCE timers and async messages are delivered in a
// command-line process, where MessageManager::runDispatchLoop returns immediately.
void mdSkinSnapshotPump(double _seconds)
{
	@autoreleasepool
	{
		[[NSRunLoop currentRunLoop] runUntilDate:[NSDate dateWithTimeIntervalSinceNow:_seconds]];
	}
}
