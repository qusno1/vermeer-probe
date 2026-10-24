// SPDX-License-Identifier: BSD-2-Clause-Patent
//
// Register the probe application as the first BDS boot option.
//
// Why not just add the application to the FV? BdsDxe has a TRUE DEPEX and the
// DXE dispatcher does not guarantee that a driver listed after it runs before
// BDS. The Chinese UI on this platform already proved this alternative:
//
//   1. write Boot#### with a device path to the application FV file,
//   2. put that option at the front of BootOrder,
//   3. return; BDS loads the application like any other boot option.
//
// This driver has the same variable-services DEPEX as VermeerChineseUiDxe, so
// it runs during DXE dispatch, before the BDS architecture protocol is
// installed and BDS starts.

#include <PiDxe.h>

#include <Guid/GlobalVariable.h>
#include <Library/BaseLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Library/DevicePathLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/PrintLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/UefiRuntimeServicesTableLib.h>
#include <Protocol/DevicePath.h>
#include <Protocol/FirmwareVolume2.h>
#include <Protocol/LoadedImage.h>

#define OPTION_ATTRIBUTES  (EFI_VARIABLE_NON_VOLATILE | EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS)

STATIC CONST CHAR16  mDescription[] = L"Vermeer hardware probe";
STATIC CONST CHAR16  mMarker[]      = L"vermeer:probe:v1";

//
// Must match FILE_GUID in VermeerProbe.inf.
//
STATIC CONST EFI_GUID  mApplicationFile = {
  0x7F3C2A91, 0x5B4D, 0x4E68, { 0x9A, 0x10, 0x6C, 0x2D, 0x8E, 0x4F, 0x1B, 0x37 }
};

/**
   Does this FV contain the probe application file?
**/
STATIC
BOOLEAN
FvHasFile (
  IN EFI_HANDLE      Handle,
  IN CONST EFI_GUID  *File
  )
{
  EFI_FIRMWARE_VOLUME2_PROTOCOL  *Fv;
  EFI_STATUS                     Status;
  EFI_FV_FILETYPE                Type;
  EFI_FV_FILE_ATTRIBUTES         Attributes;
  UINT32                         AuthStatus;
  UINTN                          Size;
  VOID                           *Buffer;

  if (EFI_ERROR (gBS->HandleProtocol (Handle, &gEfiFirmwareVolume2ProtocolGuid, (VOID **)&Fv))) {
    return FALSE;
  }

  //
  // ReadFile allocates the file when Buffer is NULL; free that copy at once,
  // we only need to know whether the GUID exists.
  //
  Buffer = NULL;
  Size   = 0;
  Status = Fv->ReadFile (Fv, File, &Buffer, &Size, &Type, &Attributes, &AuthStatus);
  if (Buffer != NULL) {
    FreePool (Buffer);
  }

  return !EFI_ERROR (Status);
}

/**
   Build the device path of the application: prefer the FV this driver was
   loaded from, then scan every FV that publishes the file.
**/
STATIC
EFI_DEVICE_PATH_PROTOCOL *
FindApplicationPath (
  IN EFI_HANDLE  ImageHandle
  )
{
  EFI_LOADED_IMAGE_PROTOCOL          *Loaded;
  EFI_HANDLE                         Fv;
  EFI_HANDLE                         *Handles;
  UINTN                              Count;
  UINTN                              Index;
  MEDIA_FW_VOL_FILEPATH_DEVICE_PATH  Node;

  Fv = NULL;
  if (!EFI_ERROR (gBS->HandleProtocol (ImageHandle, &gEfiLoadedImageProtocolGuid, (VOID **)&Loaded)) &&
      FvHasFile (Loaded->DeviceHandle, &mApplicationFile))
  {
    Fv = Loaded->DeviceHandle;
  }

  if ((Fv == NULL) &&
      !EFI_ERROR (gBS->LocateHandleBuffer (ByProtocol, &gEfiFirmwareVolume2ProtocolGuid, NULL, &Count, &Handles)))
  {
    for (Index = 0; (Index < Count) && (Fv == NULL); Index++) {
      if (FvHasFile (Handles[Index], &mApplicationFile)) {
        Fv = Handles[Index];
      }
    }

    FreePool (Handles);
  }

  if ((Fv == NULL) || (DevicePathFromHandle (Fv) == NULL)) {
    return NULL;
  }

  EfiInitializeFwVolDevicepathNode (&Node, &mApplicationFile);
  return AppendDevicePathNode (DevicePathFromHandle (Fv), (EFI_DEVICE_PATH_PROTOCOL *)&Node);
}

/**
   Serialize an EFI_LOAD_OPTION (UEFI spec 3.1.3).
**/
STATIC
VOID *
BuildLoadOption (
  IN     EFI_DEVICE_PATH_PROTOCOL  *Path,
  IN OUT UINTN                     *Size
  )
{
  UINTN  PathSize;
  UINT8  *Option;
  UINT8  *Cursor;

  PathSize = GetDevicePathSize (Path);
  *Size    = sizeof (UINT32) + sizeof (UINT16) + sizeof (mDescription) + PathSize + sizeof (mMarker);
  Option   = AllocateZeroPool (*Size);
  if (Option == NULL) {
    return NULL;
  }

  Cursor = Option;
  WriteUnaligned32 ((UINT32 *)Cursor, LOAD_OPTION_ACTIVE);
  Cursor += sizeof (UINT32);
  WriteUnaligned16 ((UINT16 *)Cursor, (UINT16)PathSize);
  Cursor += sizeof (UINT16);
  CopyMem (Cursor, mDescription, sizeof (mDescription));
  Cursor += sizeof (mDescription);
  CopyMem (Cursor, Path, PathSize);
  Cursor += PathSize;
  CopyMem (Cursor, mMarker, sizeof (mMarker));
  return Option;
}

EFI_STATUS
EFIAPI
VermeerProbeDxeEntry (
  IN EFI_HANDLE        ImageHandle,
  IN EFI_SYSTEM_TABLE  *SystemTable
  )
{
  EFI_STATUS                Status;
  EFI_DEVICE_PATH_PROTOCOL  *Path;
  VOID                      *Option;
  UINTN                     OptionSize;
  UINT16                    *Order;
  UINTN                     OrderSize;
  UINT16                    *NewOrder;
  UINTN                     Index;
  UINTN                     Out;
  UINT16                    Number;
  CHAR16                    Name[sizeof ("Boot####")];
  VOID                      *Existing;
  UINTN                     ExistingSize;
  BOOLEAN                   Found;

  (VOID)SystemTable;

  Number = 0;
  Path   = FindApplicationPath (ImageHandle);
  if (Path == NULL) {
    DEBUG ((DEBUG_ERROR, "VermeerProbeDxe: probe application not found in any FV\n"));
    return EFI_NOT_FOUND;
  }

  Option = BuildLoadOption (Path, &OptionSize);
  FreePool (Path);
  if (Option == NULL) {
    return EFI_OUT_OF_RESOURCES;
  }

  Order     = NULL;
  OrderSize = 0;
  GetEfiGlobalVariable2 (EFI_BOOT_ORDER_VARIABLE_NAME, (VOID **)&Order, &OrderSize);

  //
  // Reuse an identical Boot#### option, otherwise take the lowest free number.
  //
  Found = FALSE;
  for (Index = 0; (Index < OrderSize / sizeof (UINT16)) && !Found; Index++) {
    UnicodeSPrint (Name, sizeof (Name), L"Boot%04x", Order[Index]);
    if (!EFI_ERROR (GetEfiGlobalVariable2 (Name, &Existing, &ExistingSize))) {
      Found = (ExistingSize == OptionSize) && (CompareMem (Existing, Option, OptionSize) == 0);
      if (Found) {
        Number = Order[Index];
      }

      FreePool (Existing);
    }
  }

  if (!Found) {
    for (Number = 0; Number < 0xFFFF; Number++) {
      UnicodeSPrint (Name, sizeof (Name), L"Boot%04x", Number);
      if (EFI_ERROR (GetEfiGlobalVariable2 (Name, &Existing, &ExistingSize))) {
        break;
      }

      FreePool (Existing);
    }

    Status = gRT->SetVariable (Name, &gEfiGlobalVariableGuid, OPTION_ATTRIBUTES, OptionSize, Option);
    if (EFI_ERROR (Status)) {
      DEBUG ((DEBUG_ERROR, "VermeerProbeDxe: writing %s failed: %r\n", Name, Status));
      FreePool (Option);
      if (Order != NULL) {
        FreePool (Order);
      }

      return Status;
    }
  }

  FreePool (Option);

  //
  // BootOrder = this option first, all the others in their original order.
  //
  NewOrder = AllocatePool (OrderSize + sizeof (UINT16));
  if (NewOrder == NULL) {
    if (Order != NULL) {
      FreePool (Order);
    }

    return EFI_OUT_OF_RESOURCES;
  }

  NewOrder[0] = Number;
  Out         = 1;
  for (Index = 0; Index < OrderSize / sizeof (UINT16); Index++) {
    if (Order[Index] != Number) {
      NewOrder[Out++] = Order[Index];
    }
  }

  Status = gRT->SetVariable (
                  EFI_BOOT_ORDER_VARIABLE_NAME,
                  &gEfiGlobalVariableGuid,
                  OPTION_ATTRIBUTES,
                  Out * sizeof (UINT16),
                  NewOrder
                  );
  DEBUG ((DEBUG_ERROR, "VermeerProbeDxe: Boot%04x first (%r)\n", Number, Status));

  FreePool (NewOrder);
  if (Order != NULL) {
    FreePool (Order);
  }

  return Status;
}
