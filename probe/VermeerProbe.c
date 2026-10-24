// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Vermeer hardware probe (UEFI application).
//
// Reads CPU registers, the GOP mode and the UEFI memory map, writes the report
// through DebugLib -> FrameBufferSerialPortLib (the platform's DEBUG console),
// draws a marker through GOP, and waits for a key. Apart from those frame
// buffer writes it does not program peripheral registers, access disks or call
// ExitBootServices.
//
// The probe is registered as a Boot#### option by VermeerProbeDxe, so BDS runs
// it as an ordinary application after the console is connected.

#include <Uefi.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/UefiApplicationEntryPoint.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Protocol/GraphicsOutput.h>

#define PROBE_REGISTER(Name, Register)                         \
  STATIC                                                       \
  UINT64                                                       \
  Probe##Name (                                                \
    VOID                                                       \
    )                                                          \
  {                                                            \
    UINT64  Value;                                             \
    __asm__ volatile ("mrs %0, " #Register : "=r" (Value));    \
    return Value;                                              \
  }

PROBE_REGISTER (El, CurrentEL)
PROBE_REGISTER (Midr, midr_el1)
PROBE_REGISTER (Mpidr, mpidr_el1)
PROBE_REGISTER (Counter, cntfrq_el0)
PROBE_REGISTER (Mmfr, id_aa64mmfr0_el1)
PROBE_REGISTER (Pfr, id_aa64pfr0_el1)

#define DRAM_BASE       0x80000000ULL
#define WINDOW_ALIGN    0x200000ULL
#define WINDOW_OFFSET   0x4000ULL

/**
   Extend a conventional-memory run across adjacent conventional descriptors.

   The UEFI memory map commonly splits one physical run into several
   descriptors. DarwinBoot needs the whole run, so merge the adjacent ones.
**/
STATIC
UINT64
FreeRunEnd (
  IN EFI_MEMORY_DESCRIPTOR  *Map,
  IN UINTN                   Size,
  IN UINTN                   Stride,
  IN UINT64                  End
  )
{
  BOOLEAN                 Changed;
  UINTN                   Offset;
  EFI_MEMORY_DESCRIPTOR  *Entry;

  do {
    Changed = FALSE;
    for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
      Entry = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
      if ((Entry->Type == EfiConventionalMemory) &&
          (Entry->PhysicalStart == End) &&
          (Entry->NumberOfPages != 0) &&
          (Entry->NumberOfPages <= (MAX_UINT64 - End) / EFI_PAGE_SIZE))
      {
        End    += EFI_PAGES_TO_SIZE (Entry->NumberOfPages);
        Changed = TRUE;
      }
    }
  } while (Changed);

  return End;
}

EFI_STATUS
EFIAPI
VermeerProbeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_GRAPHICS_OUTPUT_PROTOCOL         *Gop;
  EFI_GRAPHICS_OUTPUT_MODE_INFORMATION  *Info;
  EFI_MEMORY_DESCRIPTOR                *Map;
  EFI_MEMORY_DESCRIPTOR                *Entry;
  EFI_STATUS                            Status;
  EFI_INPUT_KEY                         Input;
  EFI_GRAPHICS_OUTPUT_BLT_PIXEL         White;
  UINTN                                 Size;
  UINTN                                 Capacity;
  UINTN                                 Key;
  UINTN                                 Stride;
  UINTN                                 Offset;
  UINTN                                 Attempt;
  UINTN                                 Loop;
  UINT32                                Version;
  UINT32                                Bar;
  UINT64                                BestBase;
  UINT64                                BestBytes;
  UINT64                                MaxEnd;
  UINT64                                End;
  UINT64                                Candidate;
  UINT64                                Bytes;

  (VOID)ImageHandle;
  (VOID)SystemTable;

  gBS->SetWatchdogTimer (0, 0, 0, NULL);

  //
  // Form feed: FrameBufferSerialPortLib clears the panel and starts at the
  // top-left corner. This makes the probe result independent of how much
  // firmware DEBUG text was printed before BDS.
  //
  DEBUG ((DEBUG_ERROR, "\fVERMEER PROBE v1\n"));
  DEBUG ((DEBUG_ERROR, "\n"));
  DEBUG ((DEBUG_ERROR, "EL %lu\n", (UINT64)(ProbeEl () >> 2)));
  DEBUG ((DEBUG_ERROR, "MIDR 0x%lx\n", ProbeMidr ()));
  DEBUG ((DEBUG_ERROR, "MPIDR 0x%lx\n", ProbeMpidr ()));
  DEBUG ((DEBUG_ERROR, "AFF 0x%lx\n", ProbeMpidr () & 0xFFFFFFULL));
  DEBUG ((DEBUG_ERROR, "CNTFRQ %lu Hz\n", ProbeCounter ()));
  DEBUG ((DEBUG_ERROR, "MMFR0 0x%lx\n", ProbeMmfr ()));
  DEBUG ((DEBUG_ERROR, "PFR0 0x%lx\n", ProbePfr ()));
  DEBUG ((DEBUG_ERROR, "\n"));

  //
  // GOP: what the firmware actually scans out.
  //
  Gop    = NULL;
  Info   = NULL;
  Status = gBS->LocateProtocol (&gEfiGraphicsOutputProtocolGuid, NULL, (VOID **)&Gop);
  if (!EFI_ERROR (Status) && (Gop != NULL) && (Gop->Mode != NULL) && (Gop->Mode->Info != NULL)) {
    Info = Gop->Mode->Info;
    DEBUG ((DEBUG_ERROR, "GOP FB 0x%lx\n", (UINT64)Gop->Mode->FrameBufferBase));
    DEBUG ((DEBUG_ERROR, "GOP SZ 0x%lx\n", (UINT64)Gop->Mode->FrameBufferSize));
    DEBUG ((DEBUG_ERROR, "GOP RES %ux%u\n", (UINTN)Info->HorizontalResolution, (UINTN)Info->VerticalResolution));
    DEBUG ((DEBUG_ERROR, "GOP STR %u\n", (UINTN)Info->PixelsPerScanLine));
    DEBUG ((DEBUG_ERROR, "GOP FMT %u\n", (UINTN)Info->PixelFormat));
  } else {
    DEBUG ((DEBUG_ERROR, "GOP NONE %r\n", Status));
  }

  DEBUG ((DEBUG_ERROR, "\n"));

  //
  // Memory map: find the largest conventional run and the DarwinBoot window
  // candidate (2 MiB aligned + 0x4000, matching DarwinBoot's layout).
  //
  Map        = NULL;
  Size       = 0;
  Stride     = 0;
  Key        = 0;
  Version    = 0;
  Capacity   = 0;
  Status     = EFI_SUCCESS;

  for (Attempt = 0; Attempt < 3; Attempt++) {
    Size   = 0;
    Status = gBS->GetMemoryMap (&Size, NULL, &Key, &Stride, &Version);
    if (Status != EFI_BUFFER_TOO_SMALL) {
      break;
    }

    if (Stride < sizeof (EFI_MEMORY_DESCRIPTOR)) {
      Status = EFI_UNSUPPORTED;
      break;
    }

    Capacity = Size + 16 * Stride;
    Map      = AllocatePool (Capacity);
    if (Map == NULL) {
      Status = EFI_OUT_OF_RESOURCES;
      break;
    }

    Size   = Capacity;
    Status = gBS->GetMemoryMap (&Size, Map, &Key, &Stride, &Version);
    if (!EFI_ERROR (Status)) {
      break;
    }

    FreePool (Map);
    Map = NULL;
  }

  BestBase  = 0;
  BestBytes = 0;
  MaxEnd    = 0;

  if (EFI_ERROR (Status) || (Map == NULL) || (Stride < sizeof (EFI_MEMORY_DESCRIPTOR))) {
    DEBUG ((DEBUG_ERROR, "MAP ERR %r\n", Status));
  } else {
    for (Offset = 0; Offset + Stride <= Size; Offset += Stride) {
      Entry = (EFI_MEMORY_DESCRIPTOR *)((UINT8 *)Map + Offset);
      if ((Entry->Type != EfiConventionalMemory) || (Entry->NumberOfPages == 0)) {
        continue;
      }

      if (Entry->NumberOfPages > (MAX_UINT64 - Entry->PhysicalStart) / EFI_PAGE_SIZE) {
        continue;
      }

      End = FreeRunEnd (
              Map,
              Size,
              Stride,
              Entry->PhysicalStart + EFI_PAGES_TO_SIZE (Entry->NumberOfPages)
              );
      if (End > MaxEnd) {
        MaxEnd = End;
      }

      if (Entry->PhysicalStart > MAX_UINT64 - WINDOW_ALIGN - WINDOW_OFFSET) {
        continue;
      }

      Candidate = ALIGN_VALUE (Entry->PhysicalStart, WINDOW_ALIGN) + WINDOW_OFFSET;
      if (End <= Candidate) {
        continue;
      }

      Bytes = End - Candidate;
      if (Bytes > BestBytes) {
        BestBase  = Candidate;
        BestBytes = Bytes;
      }
    }

    DEBUG ((DEBUG_ERROR, "MAP DESC %lu\n", (UINT64)(Size / Stride)));
    if (BestBytes != 0) {
      DEBUG ((DEBUG_ERROR, "WIN BASE 0x%lx\n", BestBase));
      DEBUG ((DEBUG_ERROR, "WIN LEN 0x%lx\n", BestBytes));
      DEBUG ((DEBUG_ERROR, "WIN MB %lu\n", BestBytes >> 20));
    } else {
      DEBUG ((DEBUG_ERROR, "WIN NONE\n"));
    }

    if (MaxEnd > DRAM_BASE) {
      DEBUG ((DEBUG_ERROR, "RAM TOP 0x%lx\n", MaxEnd));
      DEBUG ((DEBUG_ERROR, "RAM MB %lu\n", (MaxEnd - DRAM_BASE) >> 20));
    }
  }

  if (Map != NULL) {
    FreePool (Map);
  }

  DEBUG ((DEBUG_ERROR, "\n"));
  DEBUG ((DEBUG_ERROR, "PHOTO THIS SCREEN\n"));
  DEBUG ((DEBUG_ERROR, "ANY KEY RETURNS\n"));
  DEBUG ((DEBUG_ERROR, "PWR+VOL- FASTBOOT\n"));
  DEBUG ((DEBUG_ERROR, "AUTO EXIT 300s\n"));

  //
  // Independent "the application really ran" marker: a white bar at the
  // bottom of the panel, outside the 44 text rows used at scale 6.
  //
  if ((Gop != NULL) && (Info != NULL) && (Info->VerticalResolution > 64)) {
    White.Blue     = 0xFF;
    White.Green    = 0xFF;
    White.Red      = 0xFF;
    White.Reserved = 0;
    Bar            = 32;
    Gop->Blt (
           Gop,
           &White,
           EfiBltVideoFill,
           0,
           0,
           0,
           Info->VerticalResolution - Bar,
           Info->HorizontalResolution,
           Bar,
           0
           );
  }

  for (Loop = 0; Loop < 3000; Loop++) {
    if ((gST->ConIn != NULL) &&
        !EFI_ERROR (gST->ConIn->ReadKeyStroke (gST->ConIn, &Input)))
    {
      break;
    }

    gBS->Stall (100000);
  }

  return EFI_SUCCESS;
}
