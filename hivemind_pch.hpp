// mindmesh_pch.hpp — force-included (/FI) before every TU
// Establishes correct Windows header order, then makes globals visible.

#ifndef MINDMESH_PCH_HPP
#define MINDMESH_PCH_HPP

// Enable SHA-256 in tiny-sha before the header is included anywhere
#ifndef ENABLE_SHA256
#define ENABLE_SHA256 1
#endif

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

// global.hpp has zero includes — safe here
#include "global.hpp"

#endif // MINDMESH_PCH_HPP