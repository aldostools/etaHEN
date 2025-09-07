/* Copyright (C) 2025 etaHEN / LightningMods

This program is free software; you can redistribute it and/or modify it
under the terms of the GNU General Public License as published by the
Free Software Foundation; either version 3, or (at your option) any
later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; see the file COPYING. If not, see
<http://www.gnu.org/licenses/>.  */

#include "Detour.h"
#include <cstdint>
#include "hde64.h"
#include "ipc.hpp"
#include <machine/param.h>
#define ROUND_PG(x) (((x) + (PAGE_SIZE - 1)) & ~(PAGE_SIZE - 1))

extern bool has_hv_bypess;

void WriteJump(uint64_t address, uint64_t destination) {
  *(uint8_t * )(address) = 0xFF;
  *(uint8_t * )(address + 1) = 0x25;
  *(uint8_t * )(address + 2) = 0x00;
  *(uint8_t * )(address + 3) = 0x00;
  *(uint8_t * )(address + 4) = 0x00;
  *(uint8_t * )(address + 5) = 0x00;
  *(uint64_t * )(address + 6) = destination;
}

void ReadMemory(uint64_t address, void * buffer, int length) {
  // memcpy(buffer, (void*)address, length);
  if (has_hv_bypess)
    memcpy(buffer, (void * ) address, length);
  else
    mdbg_copyout(getpid(), address, buffer, length);
}

void WriteMemory(uint64_t address, void * buffer, int length) {
  memcpy((void * ) address, buffer, length);
}

void LockJump(uint64_t address) {
  *(uint8_t * )(address) = 0xE9;
  *(uint8_t * )(address + 1) = 0xFB;
  *(uint8_t * )(address + 2) = 0xFF;
  *(uint8_t * )(address + 3) = 0xFF;
  *(uint8_t * )(address + 4) = 0xFF;
}

void PatchInJump(uint64_t address, void * destination) {
  if (!address || !destination)
    return;

  if (!has_hv_bypess) {
    pid_t pid = getpid();

	uint8_t JumpInstructions[] = {
		0x48, 0xB8, 0x00, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xFF, 0xE0, // mov rax, <address>; jmp rax
	};
    union Uint64T {
      uint64_t a;
      char b[8];
    }
    uint64;

    uint64.a = (uint64_t) destination;
    memcpy(JumpInstructions + 2, uint64.b, 8);

    mdbg_copyin(pid, (void * ) JumpInstructions, address, sizeof(JumpInstructions));
  } else {
    WriteJump(address, (uint64_t) destination);
  }
}

//
// Alloc using JIT shm
//
int JITAlloc(size_t size, void ** executableAddress, void ** writableAddress) {
  uint8_t * old_ucred = jailbreak_process(getpid());

  int executableHandle = -1;
  int writableHandle = -1;

  sceKernelJitCreateSharedMemory(0, ROUND_PG(size), PROT_EXEC | PROT_READ | PROT_WRITE, & executableHandle);
  if (executableHandle < 0) {
    shellui_log("Failed to create JIT shared memory!");
    return 0;
  }

  sceKernelJitCreateAliasOfSharedMemory(executableHandle, PROT_READ | PROT_WRITE, & writableHandle);

  if (writableHandle < 0) {
    shellui_log("Failed to create JIT shared memory!");
    return 0;
  }

  * executableAddress = mmap(NULL, ROUND_PG(size), PROT_EXEC | PROT_READ, MAP_SHARED, executableHandle, 0);
  * writableAddress = mmap(NULL, ROUND_PG(size), PROT_WRITE | PROT_READ, MAP_FIXED | MAP_PRIVATE, writableHandle, 0);

  if (! * executableAddress) {
    shellui_log("Failed to map executable address!\n");

  }
  jail_process(getpid(), old_ucred);
  free(old_ucred);

  // memset(*writableAddress, 0xCC, ROUND_PG(size));
  return 1;
}

void * DetourFunction(uint64_t address, void * destination) {
  if (!address || !destination)
    return 0;

  uint32_t InstructionSize = 0;
  pid_t pid = getpid();
  shellui_log("Hooking %#02lx => %p\n", address, destination);

  // sceKernelMprotect((void*)address, 0x1000, 0x7);
  uint8_t code[HOOK_LENGTH];

  if (has_hv_bypess)
  {
    sceKernelMprotect((void * ) address, PAGE_SIZE, PROT_EXEC | PROT_READ | PROT_WRITE); 
  }
  else
  {
    kernel_mprotect(pid, (uint64_t) address, PAGE_SIZE, PROT_EXEC | PROT_READ | PROT_WRITE);
  }
    

  while (InstructionSize < HOOK_LENGTH) {
    hde64s hs;
    uint32_t temp  = hde64_disasm((void * )(address + InstructionSize), & hs);

    if (hs.flags & F_ERROR) {
      return 0;
    }

    InstructionSize += temp;
    memset(code, 0x00, HOOK_LENGTH);
  }

  shellui_log("InstructionSize: %i", InstructionSize);

  if (InstructionSize < HOOK_LENGTH) {
    shellui_log("DetourFunction: Hooking Requires a minimum of 14 bytes to write jump!");
    return 0;
  }

  int stubLength = InstructionSize + HOOK_LENGTH;
  void * executableAddress = NULL;

  if (has_hv_bypess) {
    executableAddress = malloc(stubLength);
    if (!executableAddress) {
      shellui_log("Failed to allocate memory for stub");
      return 0;
    }
    sceKernelMprotect(executableAddress, stubLength, PROT_EXEC | PROT_READ | PROT_WRITE);
  } else {
    executableAddress = mmap(0, stubLength, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    kernel_mprotect(pid, (uint64_t) executableAddress, stubLength, PROT_EXEC | PROT_READ | PROT_WRITE);
  }

  ReadMemory((uint64_t) address, executableAddress, InstructionSize);
  // shellui_log("Read %d bytes from %p", InstructionSize, address);
  PatchInJump((uint64_t) executableAddress + InstructionSize, (void * )(address + InstructionSize));
  // shellui_log("Patched jump in stub");
  PatchInJump(address, destination);
  
  return (void * ) executableAddress;
}