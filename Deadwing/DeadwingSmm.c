#include <Uefi.h>
#include <PiDxe.h>
#include <Library/BaseLib.h>
#include <Library/DebugLib.h>
#include <Library/MmServicesTableLib.h>
#include <Library/UefiBootServicesTableLib.h>
#include <IndustryStandard/Q35MchIch9.h>
#include <Library/SmmServicesTableLib.h>
#include <Library/UefiLib.h>
#include <Library/CpuLib.h>
#include "PML4.h"
#include "Defs.h"
#include <Protocol/SmmVariable.h>
#include <Library/MemoryAllocationLib.h>

// a3a56e56-1d23-06dc-24bf-1473ff54e629
#define MY_VARIABLE_GUID { 0xa3a56e56, 0x1d23, 0x06dc, {0x24, 0xbf, 0x14, 0x73, 0xff, 0x54, 0xe6, 0x29} }


EFI_PHYSICAL_ADDRESS gRemapPage;

STATIC EFI_SMM_VARIABLE_PROTOCOL *mSmmVariable;
STATIC EFI_MM_CPU_IO_PROTOCOL *mMmCpuIo;

DEADWING_LIVE_SESSION_INFO gLiveSession;

#define CR0_WP    BIT16
#define CR0_PG    BIT31
#define CR4_PSE   BIT4
#define CR4_PAE   BIT5
#define EFER_LMA  BIT8

#define EFI_PAGE_1GB BASE_1GB
#define EFI_PAGE_2MB BASE_2MB
#define EFI_PAGE_4KB BASE_4KB

#define IA32_AMD64_EFER 0xC0000080


/**
 * \brief Copies information from source address to destination address
 * 
 * \param Dest Destination address
 * \param Src  Source address
 * \param Len  Length to copy
 */
VOID
EFIAPI
PhysMemCpy(
	IN VOID   *Dest,
	IN VOID   *Src,
	IN UINT32  Len
) {
	for(UINT8 *D = Dest, *S = Src; Len--; *D++ = *S++);
}

/**
 * \brief Checks if:
 * PG CR0 bit set
 * PAE CR4 bit set, if not, checks PSE CR4 bit
 * LMA EFER bit set
 * 
 * \return TRUE - We can translate page
 * \return FALSE - Somethings is not set or 4MB large pages presents
 */
BOOLEAN
EFIAPI
MemCheckPagingEnabled(
	VOID
) {
	// check if PG bit is not set
	UINT64 Cr0 = AsmReadCr0();
	if(!(Cr0 & CR0_PG)) {
		// SerialPrint("[ SMM ] PG bit is not enabled\r\n");
		return FALSE;
	}

	// check if PAE set, otherwise, check PSE bit.
	// if PAE = 1 - 2MB large page
	// if PAE = 0 and PSE = 1 - 4MB large page
	// if both is 0 - 2MB large page
	UINT64 Cr4 = AsmReadCr4();
	if(!(Cr4 & CR4_PAE)) {
		// check if PSE set. If it's 1, halt next operations, because
		// we're not working with 4MB pages
		if(Cr4 & CR4_PSE) {
			// SerialPrint("[ SMM ] 4MB pages enabled, exiting from translation process\r\n");
			return FALSE;
		}
	}

	// read EFER MSR to check if LMA set
	UINT64 Efer = AsmReadMsr64(IA32_AMD64_EFER);
	if(!(Efer & EFER_LMA)) {
		// SerialPrint("[ SMM ] LMA bit is not set\r\n");
		return FALSE;
	}

	return TRUE;
}

/**
 * \brief Remaps virtual or physical address into the SMRAM memory.
 * 
 * In SMM, address translation is disabled and addressing is similar to real mode. SMM programs can address 
 * up to 4 Gbytes of physical memory. There's probably 2 ways to access, translate and map address:
 * 1. Modify pages of SMRAM directly, this can be done by remapping address page-by-page.
 * 2. Setup own page hierarchy and go into long mode by ourselves, this also requires additional work with GDT and CS. 
 *    EDK2 (and the platform in principle) uses the transition at least during DXE Core control transfer, S3 state exit 
 *    and during capsule operation (in PEI).
 * 
 * Credits: Cr4sh
 * 
 * \param OldAddress The address in SMRAM that will act as the base address for the remapped virtual or physical address
 * \param NewAddress Address which should be remapped
 * \param SmmDir     SMM directory table base
 * \param PageSize   Size of page (4KB, 2MB, 1GB)
 * 
 * \return TRUE if table presents and can be remapped. Otherwise - FALSE.
 */
BOOLEAN
EFIAPI
MemRemapAddress(
	IN  UINT64                          OldAddress,
	IN  UINT64                          NewAddress,
	IN  UINT64                          SmmDir,
	OUT DEADWING_PAGE_TRANSLATION_SIZE *PageSize
) {
	SmmDir &= 0xFFFFFFFFFFFFF000;

    DEBUG((EFI_D_INFO, "[DeadwingSmm] MemRemapAddress\n"));
    DEBUG((EFI_D_INFO, "[DeadwingSmm] old address %x\n", OldAddress));

	// copy SMRAM PML4
	PML4E Pml4;
	Pml4.Value = *(UINT64 *)(sizeof(UINT64) * ((OldAddress >> 39) & 0x1FF) + SmmDir);
	if(!Pml4.Bits.Present)
		return FALSE;

    DEBUG((EFI_D_INFO, "[DeadwingSmm] PML4 bits present\n"));

	// get SMRAM PDPE
	UINT64 Cr0;
	PPDPE Pdpe = (PPDPE)((Pml4.Bits.Pfn << EFI_PAGE_SHIFT) + ((OldAddress >> 30) & 0x1FF) * sizeof(UINT64));
	if(Pdpe->Bits.Present) {
		if(Pdpe->Bits.Size) {
			// current virtual address has 1gb page, remap it
			if(PageSize)
				*PageSize = EDeadwingPage1Gb;

			Cr0 = AsmReadCr0();
			AsmWriteCr0(Cr0 & ~CR0_WP);

			// remap 1gb page
			Pdpe->Bits.Pfn = ((NewAddress & ~(EFI_PAGE_1GB - 1)) >> EFI_PAGE_SHIFT);

			AsmWriteCr0(Cr0);

			CpuFlushTlb(); 
			
			return TRUE;
		}
	} else {
        DEBUG((EFI_D_INFO, "[DeadwingSmm] pdpe bits not present\n"));
		return FALSE;
	}

	// get SMRAM PDE
	PPDE Pde = (PPDE)((Pdpe->Bits.Pfn << EFI_PAGE_SHIFT) + ((OldAddress >> 21) & 0x1FF) * sizeof(UINT64));
	if(Pde->Bits.Present) {
		if(Pde->Bits.Size) {
			// current virtual address has 2mb page, remap it
			if(PageSize)
				*PageSize = EDeadwingPage2Mb;
			
			Cr0 = AsmReadCr0();
			AsmWriteCr0(Cr0 & ~CR0_WP);

			// remap 2mb page
			Pde->Bits.Pfn = ((NewAddress & ~(EFI_PAGE_2MB - 1)) >> EFI_PAGE_SHIFT);

			AsmWriteCr0(Cr0);
			
			CpuFlushTlb();

			return TRUE;
		}
	} else {
        DEBUG((EFI_D_INFO, "[DeadwingSmm] pde bits not present\n"));
		return FALSE;
	}

	// get SMRAM PTE
	PPTE Pte = (PPTE)((Pde->Bits.Pfn << EFI_PAGE_SHIFT) + ((OldAddress >> 12) & 0x1FF) * sizeof(UINT64));
	if(Pte->Bits.Present) {
		if(PageSize)
			*PageSize = EDeadwingPage4Kb;

		Cr0 = AsmReadCr0();
		AsmWriteCr0(Cr0 & ~CR0_WP);

		// remap 4kb page
		Pte->Bits.Pfn = ((NewAddress & ~(EFI_PAGE_4KB - 1)) >> EFI_PAGE_SHIFT);

		AsmWriteCr0(Cr0);

		CpuFlushTlb();

		return TRUE;
	}

    DEBUG((EFI_D_INFO, "[DeadwingSmm] pte bits not present\n"));
	return FALSE;
}

/**
 * \brief Restores memory mapping after memory manipulations 
 */
VOID
EFIAPI
MemRestoreSmramMappings(
	VOID
) {
	UINT64 SmmDir = AsmReadCr3() & 0xFFFFFFFFFFFFF000ULL;
	MemRemapAddress(gRemapPage, gRemapPage, SmmDir, NULL);
}


/**
 * \brief Translates virtual address to the physical by identity
 * remapping
 * 
 * \param Address Virtual address which should be converted
 * \param Dir     Directory table base
 * 
 * \returns Translated address or 0 if we cannot translate it
 */
UINT64 
EFIAPI
MemTranslateVirtualToPhys(
	IN VOID   *Address,
	IN UINT64  Dir
) {
	UINT64 TargetAddress;
	UINT64 ReadAddress;

	/// \todo @0x00Alchemist: add support to LA57 platforms

	Dir &= 0xFFFFFFFFFFFFF000ULL;
	UINT64 SmmDir = AsmReadCr3() & 0xFFFFFFFFFFFFF000ULL;

	// remap PML4
	PML4E Pml4;
	UINT8 PageSize = EDeadwingPage4Kb;
	if(MemRemapAddress(gRemapPage, Dir, SmmDir, &PageSize)) {
		TargetAddress = gRemapPage;

		if(PageSize == EDeadwingPage1Gb) {
			TargetAddress += Dir & 0x3FFFFFF;
		} else if (PageSize == EDeadwingPage2Mb) {
			TargetAddress += Dir & 0x1FFFFF;
		} else {
			TargetAddress += Dir & 0xFFF;
		}

		Pml4.Value = *(UINT64 *)(TargetAddress + (((UINT64)Address >> 39) & 0x1FF) * sizeof(UINT64));

		MemRestoreSmramMappings();
	} else {
		// SerialPrint("[ SMM ] Unable to remap PML4\r\n");
		return 0;
	}

	// remap PDP
	PDPE Pdpe;
	if(Pml4.Bits.Present) {
		ReadAddress = Pml4.Bits.Pfn << EFI_PAGE_SHIFT;
		if(MemRemapAddress(gRemapPage, ReadAddress, SmmDir, &PageSize)) {
			TargetAddress = gRemapPage;

			if(PageSize == EDeadwingPage1Gb) {
				TargetAddress += ReadAddress & 0x3FFFFFF;
			} else if(PageSize == EDeadwingPage2Mb) {
				TargetAddress += ReadAddress & 0x1FFFFF;
			} else {
				TargetAddress += ReadAddress & 0xFFF;
			}

			Pdpe.Value = *(UINT64 *)(TargetAddress + (((UINT64)Address >> 30) & 0x1FF) * sizeof(UINT64));
			
			MemRestoreSmramMappings();
		} else {
			// SerialPrint("[ SMM ] Unable to remap PDPE\r\n");
			return 0;
		}
	} else {
		// SerialPrint("[ SMM ] PML4 is not present for current virtual address\r\n");
		return 0;
	}
	
	// remap PDE
	PDE Pde;
	if(Pdpe.Bits.Present) {
		// check if page is 1gb size, translate if it's true
		if(Pdpe.Bits.Size)
			return ((Pdpe.Bits.Pfn << EFI_PAGE_SHIFT) + ((UINT64)Address & 0x3FFFFFF));

		ReadAddress = Pdpe.Bits.Pfn << EFI_PAGE_SHIFT;
		if(MemRemapAddress(gRemapPage, ReadAddress, SmmDir, &PageSize)) {
			TargetAddress = gRemapPage;

			if(PageSize == EDeadwingPage1Gb) {
				TargetAddress += ReadAddress & 0x3FFFFFF;
			} else if(PageSize == EDeadwingPage2Mb) {
				TargetAddress += ReadAddress & 0x1FFFFF;
			} else {
				TargetAddress += ReadAddress & 0xFFF;
			}

			Pde.Value = *(UINT64 *)(TargetAddress + (((UINT64)Address >> 21) & 0x1FF) * sizeof(UINT64));
		
			MemRestoreSmramMappings();
		} else {
			// SerialPrint("[ SMM ] Unable to remap PDE\r\n");
			return 0;
		}
	} else {
		// SerialPrint("[ SMM ] PDPE is not present for current virtual address\r\n");
		return 0;
	}

	// remap PTE
	PTE Pte;
	if(Pde.Bits.Present) {
		// check if page is 2mb size, translate if it's true
		if(Pde.Bits.Size)
			return ((Pde.Bits.Pfn << EFI_PAGE_SHIFT) + ((UINT64)Address & 0x1FFFFF));
		
		ReadAddress = Pde.Bits.Pfn << EFI_PAGE_SHIFT;
		if(MemRemapAddress(gRemapPage, ReadAddress, SmmDir, &PageSize)) {
			TargetAddress = gRemapPage;

			if(PageSize == EDeadwingPage2Mb) {
				TargetAddress += ReadAddress & 0x1FFFFF;
			} else {
				TargetAddress += ReadAddress & 0xFFF;
			}

			Pte.Value = *(UINT64 *)(TargetAddress + (((UINT64)Address >> 12) & 0x1FF) * sizeof(UINT64));

			MemRestoreSmramMappings();
		} else {
			// SerialPrint("[ SMM ] Unable to remap PTE\r\n");
			return 0;
		}
	} else {
		// SerialPrint("[ SMM ] PDE is not present for current virtual address\r\n");
		return 0;
	}

	if(Pte.Bits.Present)
		return ((Pte.Bits.Pfn << EFI_PAGE_SHIFT) + ((UINT64)Address & 0xFFF));
	else
		SerialPrint("[ SMM ] PTE is not present for current virtual address\r\n");

	return 0;
}

/**
 * \brief Maps physical address into the SMRAM
 * 
 * \param PhysAddress Physical address which should be mapped into SMRAM
 * 
 * \return Mapped address or 0 if we cannot map it
 */
UINT64
EFIAPI
MemProcessOutsideSmramPhysMemory(
	IN UINT64 PhysAddress
) {
	UINT8 PageSize = EDeadwingPage4Kb;
	UINT64 RemapedMemory = gRemapPage;
	UINT64 SmmDir = AsmReadCr3();
    DEBUG((EFI_D_INFO, "[DeadwingSmm] SmmDir: %x\n", SmmDir));
    
	if(MemRemapAddress(gRemapPage, PhysAddress, SmmDir, &PageSize)) {		
		if(PageSize == EDeadwingPage1Gb) {
			RemapedMemory += PhysAddress & 0x3FFFFFF;
		} else if(PageSize == EDeadwingPage2Mb) {
			RemapedMemory += PhysAddress & 0x1FFFFF;
		} else {
			RemapedMemory += PhysAddress & 0xFFF;
		}
	} else {
		RemapedMemory = 0;
	}

	return RemapedMemory;
}

/**
 * \brief Translates and maps virtual address content into SMRAM 
 * 
 * \param VirtualAddress  Virtual address which should be translated
 * \param DirBase         Directory table base
 * \param UnmappedAddress Unmapped physical address if requested
 * 
 * \returns Mapped physical address or 0 if we cannot translate/map it
 */
UINT64
EFIAPI
MemMapVirtualAddress(
	IN  VOID    *VirtualAddress,
	IN  UINT64   DirBase,
	OUT VOID   **UnmappedAddress
) {
	UINT64 TranslatedAddress;
	UINT64 PhysRemaped;

    DEBUG((EFI_D_INFO, "[DeadwingSmm] MemMapVA\n"));

	if(!VirtualAddress || !DirBase)
		return 0;

    DEBUG((EFI_D_INFO, "[DeadwingSmm] valid Virtual address\n"));

	PhysRemaped = 0;

	if(!MemCheckPagingEnabled())
		return 0;

    DEBUG((EFI_D_INFO, "[DeadwingSmm] paging enabled\n"));

	// translate virtual address to physical
	TranslatedAddress = MemTranslateVirtualToPhys(VirtualAddress, DirBase);
    DEBUG((EFI_D_INFO, "[DeadwingSmm] Translated address: %x\n", TranslatedAddress));
	if(TranslatedAddress != 0) {
		// map physical address into the SMRAM memory 
		PhysRemaped = MemProcessOutsideSmramPhysMemory(TranslatedAddress);
		if(PhysRemaped == 0) {
            DEBUG((EFI_D_INFO, "[DeadwingSmm] phys remapped is 0\n"));
			return 0;
        }

        DEBUG((EFI_D_INFO, "[DeadwingSmm] physRemapped not 0\n"));
	}

	if(UnmappedAddress)
		*UnmappedAddress = TranslatedAddress;

	return PhysRemaped;
}

EFI_STATUS
EFIAPI
CmdPhysRead(
	IN VOID   *AddressToRead,
	IN VOID   *ReceivedInfo,
	IN UINT64  LengthToRead
) {
	DEBUG((EFI_D_INFO, "[ SMM ] Reading from physical memory1\r\n"));

	// validate input
	if((!AddressToRead || !LengthToRead || !ReceivedInfo) || LengthToRead > BASE_4KB) {
        DEBUG((EFI_D_INFO, "[ SMM ] Invalid parameters has been passed to specific command\r\n"));
		return EFI_INVALID_PARAMETER;
	}

    DEBUG((EFI_D_INFO, "[ SMM ] Input validated\r\n"));

	// allocate intermediate page for storing data from read address
	// EFI_PHYSICAL_ADDRESS InterimPage;
	// EFI_STATUS Status = gSmst->SmmAllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &InterimPage);
	// if(EFI_ERROR(Status)) {
	// 	DEBUG((EFI_D_INFO, "[ SMM ] Unable to allocate intermediate page\r\n"));
	// 	return Status;
	// }

    // DEBUG((EFI_D_INFO, "[ SMM ] Prepare to map to SMRAM, intermidiate page %lld\r\n", InterimPage));

	// map physical address to the SMRAM
	UINT64 PhysMapped = MemProcessOutsideSmramPhysMemory(AddressToRead);
	if(PhysMapped != 0) {
		// copy data to intermediate page
        DEBUG((EFI_D_INFO, "[ SMM ] Prepare to phsmemcpy\r\n"));

		// PhysMemCpy(InterimPage, PhysMapped, LengthToRead);

        // DEBUG((EFI_D_INFO, "[ SMM ] After phsmemcpy\r\n"));

		// MemRestoreSmramMappings();

        for (int i = 0; i < LengthToRead; i++) {
            DEBUG((EFI_D_INFO, "[ SMM ] Print %c\r\n", ((char *)(PhysMapped))[i]));
        }

		// // map consumer address
		// UINT64 Consumer = MemMapVirtualAddress(ReceivedInfo, gLiveSession.UmController.UmControllerDirBase, NULL);
		// if(Consumer == 0) {
        //     DEBUG((EFI_D_INFO, "[ SMM ] Unable to map consumer buffer\r\n"));
		// 	gSmst->SmmFreePages(InterimPage, 1);
		// 	return EFI_ABORTED;
		// }

        // DEBUG((EFI_D_INFO, "[ SMM ] Before second phsmemcpy\r\n"));

		// // copy data to the consumer buffer & map it back
		// PhysMemCpy(Consumer, InterimPage, LengthToRead);

        // DEBUG((EFI_D_INFO, "[ SMM ] After second phsmemcpy\r\n"));

		// MemRestoreSmramMappings();
	} else {
		DEBUG((EFI_D_INFO, "[ SMM ] Unable to map physical address\r\n"));
		// gSmst->SmmFreePages(InterimPage, 1);
		return EFI_ABORTED;
	}

	// gSmst->SmmFreePages(InterimPage, 1);

	return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
DeadwingSmiHandler (
    IN EFI_HANDLE DispatchHandle,
    IN CONST VOID* RegisterContext,
    IN OUT VOID* CommBuffer,
    IN OUT UINTN* CommBufferSize
    )
{
    EFI_STATUS Status;
    UINT8 APM_CNT_VALUE;
	CHAR16 *MyVariableName = L"MyVariable";
    EFI_GUID VendorGuid = MY_VARIABLE_GUID;
    UINT32 Attributes;
    UINTN VariableSize = 0;
    VOID *VariableData = NULL;

    Status = mMmCpuIo->Io.Read(mMmCpuIo, MM_IO_UINT8, ICH9_APM_CNT, 1, &APM_CNT_VALUE);
    ASSERT_EFI_ERROR(Status);

    if (APM_CNT_VALUE != 0x01)
    {
        return EFI_WARN_INTERRUPT_SOURCE_QUIESCED;
    }

    DEBUG((EFI_D_INFO, "[DeadwingSmm] SMI 0x%02x\n", APM_CNT_VALUE));

	Status = mSmmVariable->SmmGetVariable(
        MyVariableName,
        &VendorGuid,
        &Attributes,
        &VariableSize,
        NULL
    );
    
    if (Status == EFI_BUFFER_TOO_SMALL) {
        VariableData = AllocateZeroPool(VariableSize);
        if (VariableData == NULL) {
            DEBUG((EFI_D_INFO, "AllocateZeroPool Failed\n"));
            return EFI_WARN_INTERRUPT_SOURCE_QUIESCED;
        }
        Status = mSmmVariable->SmmGetVariable(
            MyVariableName,
            &VendorGuid,
            &Attributes,
            &VariableSize,
            VariableData
        );
        if (!EFI_ERROR(Status)) {
            DEBUG((EFI_D_INFO, "Variable Data: %lx\n", (*(UINT64 *)VariableData)));
        } else {
            DEBUG((EFI_D_INFO, "Get Variable failed: %r\n", Status));
			return EFI_WARN_INTERRUPT_SOURCE_QUIESCED;
        }
    } else {
        DEBUG((EFI_D_INFO, "Get Variable failed: %r\n", Status));
		return EFI_WARN_INTERRUPT_SOURCE_QUIESCED;
    }

    char buffer[4096];

    Status = CmdPhysRead((VOID *)(*(UINT64 *)VariableData), buffer, 4096);
	FreePool(VariableData);
    return EFI_WARN_INTERRUPT_SOURCE_QUIESCED;
}

EFI_STATUS
EFIAPI
DeadwingSmmInitialize (
    IN EFI_HANDLE ImageHandle,
    IN EFI_SYSTEM_TABLE* SystemTable
    )
{
    EFI_STATUS Status;
    EFI_HANDLE Handle;

    DEBUG((EFI_D_INFO, "[DeadwingSmm] DeadwingSmmInitialize called\n"));

    Status = gMmst->MmLocateProtocol(&gEfiMmCpuIoProtocolGuid, NULL, (VOID **)&mMmCpuIo);
    ASSERT_EFI_ERROR(Status);

	Status = gSmst->SmmLocateProtocol(&gEfiSmmVariableProtocolGuid, NULL, (VOID **)&mSmmVariable);
    ASSERT_EFI_ERROR(Status);
   
    Status = gMmst->MmiHandlerRegister(DeadwingSmiHandler, NULL, &Handle);
    ASSERT_EFI_ERROR(Status);

    // allocate page for remaping virtual and physical addresses
	Status = gSmst->SmmAllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &gRemapPage);
	if(EFI_ERROR(Status)) {
		DEBUG((EFI_D_INFO, "[ SMM ] Unable to allocate remap page\r\n"));
		return Status;
	}

	gBS->SetMem(gRemapPage, EFI_PAGE_SIZE, 0);

    return Status;
}
