#ifndef VIDEOGU_H
#define VIDEOGU_H

//Permet de ne pas afficher les bordures de l'image en mode antialiasé (0.5 pixels)
#define SKIP_BORDERS

#ifdef SKIP_BORDERS
	#define pspScaleImage(pixels, srcX0, srcY0, srcX1, srcY1, srcPitch, dstX0, dstY0, dstX1, dstY1, clear, cutborders) pspScaleImage_core(pixels, srcX0, srcY0, srcX1, srcY1, srcPitch, dstX0, dstY0, dstX1, dstY1, clear, cutborders)
	void pspScaleImage_core(char *pixels, int srcX0, int srcY0, int srcX1, int srcY1, 
			   int srcPitch, int dstX0, int dstY0, int dstX1, int dstY1, int clear, int cutborders);
#else
	#define pspScaleImage(pixels, srcX0, srcY0, srcX1, srcY1, srcPitch, dstX0, dstY0, dstX1, dstY1, clear, cutborders) pspScaleImage_core(pixels, srcX0, srcY0, srcX1, srcY1, srcPitch, dstX0, dstY0, dstX1, dstY1, clear)
	void pspScaleImage_core(char *pixels, int srcX0, int srcY0, int srcX1, int srcY1, 
			   int srcPitch, int dstX0, int dstY0, int dstX1, int dstY1, int clear);
#endif

typedef struct
{
//	char scrBuffer[256*192*2];
	char *scrBuffer;
//	uint16 pal[256];
	uint16 *pal;
} screen_t;

extern screen_t Screen, ScreenTemp;

void VideoGuInit(void);

void VideoGuTerm(void);

void VideoGuUpdate(int nFrame, int fullScreen);
void VideoGuUpdate_Core(int fullScreen, int mustClear);

void VideoGuScreenClear(void);

//=== SCREEN OVERLAY ===
//480x272 RGBA images composited over the finished frame (LCD grid and/or bezel
//art), loaded from <app>/overlays/. menuConfig.video.overlay is 0 for off, or a
//1-based index into the overlayFiles table in VideoGu.c.
extern OSL_IMAGE *gblOverlay;
extern int gblOverlayLoaded;
void OverlayLoad(void);
extern OSL_IMAGE *gblBezel;
extern int gblBezelLoaded;
void BezelLoad(void);
void OverlaySync(void);
void OverlayDraw(void);

#endif
