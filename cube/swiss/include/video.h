#ifndef VIDEO_H
#define VIDEO_H
#include <gccore.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>

extern u32 *xfb[2];
extern int whichfb;
extern bool disableVideoOutput;
void updateVideoMode(GXRModeObj *m);
void setVideoMode(GXRModeObj *m);
char* getVideoModeString();
GXRModeObj* getVideoMode();
GXRModeObj* getVideoModeFromSwissSetting(int uiVMode);
int getTVFormat();
int getScanMode();
int getDTVStatus();
int getFontEncode();
f32 getYScaleFactor(u16 efbHeight, u16 xfbHeight);

#endif 
