#include <Uefi.h>
#include <Library/UefiLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <Library/MemoryAllocationLib.h>
#include <Library/BaseMemoryLib.h>
#include <Library/DebugLib.h>
#include <Protocol/SimpleFileSystem.h>
#include <IndustryStandard/Elf64.h>

EFI_STATUS
LoadElf64(
    IN EFI_FILE_PROTOCOL *Root,
    IN CHAR16 *FileName,
    OUT EFI_PHYSICAL_ADDRESS *EntryPoint,
    OUT EFI_PHYSICAL_ADDRESS *KernelBase)
{
    EFI_FILE_PROTOCOL *File;
    EFI_STATUS Status;
    UINTN ReadSize;

    Status = Root->Open(Root, &File, FileName, EFI_FILE_MODE_READ, 0);
    if (EFI_ERROR(Status))
    {
        Print(L"LoadElf64: Open %s failed: %r\n", FileName, Status);
        return Status;
    }

    //
    // Read ELF header
    //
    Elf64_Ehdr Ehdr;
    ReadSize = sizeof(Ehdr);
    Status = File->Read(File, &ReadSize, &Ehdr);
    if (EFI_ERROR(Status) ||
        Ehdr.e_ident[EI_MAG0] != ELFMAG0 ||
        Ehdr.e_ident[EI_MAG1] != ELFMAG1 ||
        Ehdr.e_ident[EI_MAG2] != ELFMAG2 ||
        Ehdr.e_ident[EI_MAG3] != ELFMAG3 ||
        Ehdr.e_ident[EI_CLASS] != ELFCLASS64 ||
        Ehdr.e_machine != EM_X86_64)
    {
        Print(L"LoadElf64: invalid ELF header (Status=%r)\n", Status);
        File->Close(File);
        return EFI_UNSUPPORTED;
    }

    *EntryPoint = (EFI_PHYSICAL_ADDRESS)Ehdr.e_entry;
    Print(L"LoadElf64: entry=0x%lx phnum=%d\n", *EntryPoint, Ehdr.e_phnum);

    //
    // Read program headers
    //
    ReadSize = Ehdr.e_phnum * Ehdr.e_phentsize;
    Elf64_Phdr *Phdrs = AllocatePool(ReadSize);
    if (Phdrs == NULL)
    {
        File->Close(File);
        return EFI_OUT_OF_RESOURCES;
    }

    Status = File->SetPosition(File, Ehdr.e_phoff);
    if (EFI_ERROR(Status))
    {
        goto Done;
    }

    Status = File->Read(File, &ReadSize, Phdrs);
    if (EFI_ERROR(Status))
    {
        goto Done;
    }

    *KernelBase = MAX_ADDRESS;

    //
    // Load PT_LOAD segments
    //
    for (UINTN i = 0; i < Ehdr.e_phnum; i++)
    {
        Elf64_Phdr *Ph = &Phdrs[i];
        if (Ph->p_type != PT_LOAD)
        {
            continue;
        }

        UINTN Pages = EFI_SIZE_TO_PAGES(Ph->p_memsz);
        EFI_PHYSICAL_ADDRESS Addr = (EFI_PHYSICAL_ADDRESS)Ph->p_paddr;

        //
        // Must use EfiLoaderCode: OVMF NX policy marks EfiLoaderData non-executable
        //
        Status = gBS->AllocatePages(
            AllocateAddress,
            EfiLoaderCode,
            Pages,
            &Addr);
        if (EFI_ERROR(Status))
        {
            Print(L"LoadElf64: AllocatePages(0x%lx, %d pages) failed: %r\n",
                  Ph->p_paddr, Pages, Status);
            goto Done;
        }

        if (Addr < *KernelBase)
        {
            *KernelBase = Addr;
        }

        Print(L"LoadElf64: PT_LOAD paddr=0x%lx filesz=0x%lx memsz=0x%lx\n",
              Addr, Ph->p_filesz, Ph->p_memsz);

        //
        // Zero BSS
        //
        SetMem((VOID *)(UINTN)Addr, (UINTN)Ph->p_memsz, 0);

        //
        // Load file data
        //
        Status = File->SetPosition(File, Ph->p_offset);
        if (EFI_ERROR(Status))
        {
            goto Done;
        }

        ReadSize = (UINTN)Ph->p_filesz;
        Status = File->Read(File, &ReadSize, (VOID *)(UINTN)Addr);
        if (EFI_ERROR(Status))
        {
            Print(L"LoadElf64: Read segment failed: %r\n", Status);
            goto Done;
        }
    }

Done:
    FreePool(Phdrs);
    File->Close(File);
    return Status;
}