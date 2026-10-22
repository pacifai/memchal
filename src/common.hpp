// common.hpp — the shared header every host-side participant includes.
//
// This used to hold both the challenge math and the socket plumbing. It is now
// a shim over the two halves, because mixing them made the math unreachable
// from CUDA: the POSIX headers below cannot be included from a .cu translation
// unit, and the math has to be.
//
//   challenge.hpp — the math. Frozen, pinned by tests/known_answer.cpp, and
//                   compilable for host and device (see MC_HOST_DEVICE).
//   net.hpp       — the wire structs, sockets, and /proc readers. Host only.
//
// Include this from host code and nothing changes. Include challenge.hpp alone
// from a .cu file to get the math with no POSIX in scope.
#pragma once
#include "challenge.hpp"
#include "net.hpp"
