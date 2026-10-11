#ifndef PORT_SECOND_SCREEN_PLATFORM_H
#define PORT_SECOND_SCREEN_PLATFORM_H

/*
 * Which builds drive a live second-screen panel.
 *
 * The compositor (port_second_screen.c) and the snapshot publisher
 * (port_second_screen_state.c) are portable C; what differs per platform is
 * who hosts the panel's pixels and delivers its taps:
 *
 *   - Android: a Java SurfaceView on the secondary display, fed through
 *     ANativeWindow (port_second_screen_jni.cpp).
 *   - Linux dual-screen handhelds (ROCKNIX on the Anbernic RG DS / RG DS
 *     Plus, xmake --dual_screen=y): a second SDL window made fullscreen on
 *     the other output, with taps read from that panel's touchscreen
 *     (port_second_screen_linux.c).
 *
 * PORT_SS_LIVE is the "a second-screen thread exists" switch: the snapshot
 * publisher, the UI lock and the paint loop only compile in when it is set.
 * Everywhere else the entry points stay no-ops and the compositor is still
 * built for the host harness, exactly as before.
 */

#if defined(__ANDROID__)
#define PORT_SS_ANDROID 1
#define PORT_SS_LIVE 1
#elif defined(__linux__) && defined(PORT_DUAL_SCREEN_LINUX)
#define PORT_SS_LINUX 1
#define PORT_SS_LIVE 1
#else
#define PORT_SS_LIVE 0
#endif

#endif /* PORT_SECOND_SCREEN_PLATFORM_H */
