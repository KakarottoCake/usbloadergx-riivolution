#ifndef _SYS_H_
#define _SYS_H_

void wiilight(int enable);

//! The drive light used as a diagnostic rather than as decoration, so it
//! ignores Settings.wiilight. That setting is a preference about idle
//! blinking; it must not be able to switch off a refusal code or the only
//! sign of life a tester has while the screen is black.
void wiilight_diag(int enable);

/* Prototypes */
void AppCleanUp(void);  //! Deletes all allocated space for everything
void ExitApp(void);	 //! Like AppCleanUp() and additional device unmount
void Sys_Init(void);
void Sys_Reboot(void);
void Sys_Shutdown(void);
void Sys_ShutdownToIdle(void);
void Sys_ShutdownToStandby(void);
void Sys_LoadMenu(void);
void Sys_BackToLoader(void);
void Sys_LoadHBC(void);
bool RebootApp(void);
void ScreenShot(void);
bool isWiiU(void);
bool IsWiiVCActive(void);
void ResetRegion(void);

#endif
