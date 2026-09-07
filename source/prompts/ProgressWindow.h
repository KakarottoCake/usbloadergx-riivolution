/****************************************************************************
 * ProgressWindow
 * USB Loader GX 2009
 *
 * ProgressWindow.h
 ***************************************************************************/

#ifndef _PROGRESSWINDOW_H_
#define _PROGRESSWINDOW_H_

#include <gctypes.h>

#ifdef __cplusplus

#define PROGRESS_CANCELED	-12345

void InitProgressThread();
void ExitProgressThread();
void ShowProgress(const char *title, const char *msg1, const char *msg2, s64 done, s64 total, bool swSize = false, bool swTime = false);
void ShowProgress(const char *msg2, s64 done, s64 total);

extern "C"
{
#endif

void ProgressCancelEnable(bool allowCancel);
//! Draw the next progress window at once instead of waiting half a second
//! to see whether the operation is long enough to be worth it. For callers
//! that already know they want to be seen; applies to one window.
void ProgressSkipDebounce();
void StartProgress(const char * title, const char * msg1, const char * msg2, bool swSize, bool swTime);
void ShowProgress(s64 done, s64 total);
bool ProgressCanceled();
void ProgressStop();

#ifdef __cplusplus
}
#endif

#endif
