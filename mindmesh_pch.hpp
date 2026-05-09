// mindmesh_pch.hpp
// Force-included before every translation unit in MindMesh and offload_hook.
// Ensures WIN32_LEAN_AND_MEAN, NOMINMAX, and winsock2.h are always first,
// regardless of individual file include order.

#ifndef MINDMESH_PCH_HPP
#define MINDMESH_PCH_HPP

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef UNICODE
#define UNICODE
#endif

// winsock2 must come before windows.h
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#endif // MINDMESH_PCH_HPP