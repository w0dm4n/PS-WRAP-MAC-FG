// PS-WRAP-MAC-FG: pointer capture for keyboard + mouse mode (macOS only, see macMouseCapture.mm)
#ifndef PSWRAP_MACMOUSECAPTURE_H
#define PSWRAP_MACMOUSECAPTURE_H

// Hide the cursor and park it at (cx, cy), global coordinates in points, top-left origin.
void pswrapMacMouseCaptureStart(double cx, double cy);
// Movement since the last poll (points); the cursor is put back at (cx, cy). false = no movement.
bool pswrapMacMouseCapturePoll(double cx, double cy, double *dx, double *dy);
void pswrapMacMouseCaptureStop();

#endif
