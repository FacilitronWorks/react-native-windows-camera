// pch.h : include file for standard system include files, or project-specific
// include files that are used frequently but changed infrequently.
//
// Shape follows the RNW cpp-lib template (same as our
// react-native-webview-windows sibling package).

#pragma once

#include "targetver.h"

#define NOMINMAX 1
#define WIN32_LEAN_AND_MEAN 1
#define WINRT_LEAN_AND_MEAN 1

// Windows Header Files
#include <windows.h>
#undef GetCurrentTime
#include <unknwn.h>

// WinRT Header Files
#include <winrt/base.h>
#include <CppWinRTIncludes.h>
#include <winrt/Microsoft.ReactNative.h>
#ifdef RNW_NEW_ARCH
#include <winrt/Microsoft.ReactNative.Composition.h>
#include <winrt/Microsoft.UI.Composition.h>
#include <winrt/Microsoft.UI.Content.h>
#include <winrt/Microsoft.UI.Dispatching.h>
#endif

// C RunTime Header Files
#include <malloc.h>
#include <memory.h>
#include <stdlib.h>
#include <tchar.h>
