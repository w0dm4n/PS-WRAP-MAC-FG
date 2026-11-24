// PS-WRAP-MAC-FG: pointer capture for keyboard + mouse mode on macOS.
// The cursor is hidden and parked at the centre of the game window; pswrapMacMouseCapturePoll() reads where it
// went since the last poll and warps it back. Polling the cursor position needs no mouse events at all
// (an NSEvent monitor saw nothing once the cursor was frozen with CGAssociateMouseAndMouseCursorPosition).
#include "macMouseCapture.h"

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

static bool g_active = false;

void pswrapMacMouseCaptureStart(double cx, double cy)
{
	if (g_active)
		return;
	g_active = true;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	CGSetLocalEventsSuppressionInterval(0.0);   // a warp must not freeze the mouse for 250 ms
#pragma clang diagnostic pop
	CGWarpMouseCursorPosition(CGPointMake(cx, cy));
	[NSCursor hide];
}

bool pswrapMacMouseCapturePoll(double cx, double cy, double *dx, double *dy)
{
	*dx = *dy = 0;
	if (!g_active)
		return false;
	CGEventRef e = CGEventCreate(NULL);
	if (!e)
		return false;
	const CGPoint p = CGEventGetLocation(e);   // global, top-left origin, points (same space as Qt's mapToGlobal)
	CFRelease(e);
	*dx = p.x - cx;
	*dy = p.y - cy;
	if (*dx == 0 && *dy == 0)
		return false;
	CGWarpMouseCursorPosition(CGPointMake(cx, cy));
	return true;
}

void pswrapMacMouseCaptureStop()
{
	if (!g_active)
		return;
	g_active = false;
	[NSCursor unhide];
}
