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
			return FALSE;
		}
	}

	// read EFER MSR to check if LMA set
	UINT64 Efer = AsmReadMsr64(IA32_AMD64_EFER);
	if(!(Efer & EFER_LMA)) {
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
	IN  UINT64              OldAddress,
	IN  UINT64              NewAddress,
	IN  UINT64              SmmDir,
	OUT PAGE_SIZE_ENUMRATE *PageSize
) {
	SmmDir &= 0xFFFFFFFFFFFFF000;

    DEBUG((DEBUG_INFO, "[SmartSmm] MemRemapAddress\n"));
    DEBUG((DEBUG_INFO, "[SmartSmm] old address %x\n", OldAddress));

	// copy SMRAM PML4
	PTE4 Pml4;
	Pml4.Value = *(UINT64 *)(sizeof(UINT64) * ((OldAddress >> 39) & 0x1FF) + SmmDir);
	if(!Pml4.Bits.Present)
		return FALSE;

    DEBUG((DEBUG_INFO, "[SmartSmm] PML4 bits present\n"));

	// get SMRAM PDPE
	UINT64 Cr0;
	PPDPE Pdpe = (PPDPE)((Pml4.Bits.Pfn << EFI_PAGE_SHIFT) + ((OldAddress >> 30) & 0x1FF) * sizeof(UINT64));
	if(Pdpe->Bits.Present) {
		if(Pdpe->Bits.Size) {
			// current virtual address has 1gb page, remap it
			if(PageSize)
				*PageSize = Page1G;

			Cr0 = AsmReadCr0();
			AsmWriteCr0(Cr0 & ~CR0_WP);

			// remap 1gb page
			Pdpe->Bits.Pfn = ((NewAddress & ~(EFI_PAGE_1GB - 1)) >> EFI_PAGE_SHIFT);

			AsmWriteCr0(Cr0);

			CpuFlushTlb(); 
			
			return TRUE;
		}
	} else {
        DEBUG((DEBUG_INFO, "[SmartSmm] pdpe bits not present\n"));
		return FALSE;
	}

	// get SMRAM PDE
	PPDE Pde = (PPDE)((Pdpe->Bits.Pfn << EFI_PAGE_SHIFT) + ((OldAddress >> 21) & 0x1FF) * sizeof(UINT64));
	if(Pde->Bits.Present) {
		if(Pde->Bits.Size) {
			// current virtual address has 2mb page, remap it
			if(PageSize)
				*PageSize = Page2M;
			
			Cr0 = AsmReadCr0();
			AsmWriteCr0(Cr0 & ~CR0_WP);

			// remap 2mb page
			Pde->Bits.Pfn = ((NewAddress & ~(EFI_PAGE_2MB - 1)) >> EFI_PAGE_SHIFT);

			AsmWriteCr0(Cr0);
			
			CpuFlushTlb();

			return TRUE;
		}
	} else {
        DEBUG((DEBUG_INFO, "[SmartSmm] pde bits not present\n"));
		return FALSE;
	}

	// get SMRAM PTE
	PPTE Pte = (PPTE)((Pde->Bits.Pfn << EFI_PAGE_SHIFT) + ((OldAddress >> 12) & 0x1FF) * sizeof(UINT64));
	if(Pte->Bits.Present) {
		if(PageSize)
			*PageSize = Page4K;

		Cr0 = AsmReadCr0();
		AsmWriteCr0(Cr0 & ~CR0_WP);

		// remap 4kb page
		Pte->Bits.Pfn = ((NewAddress & ~(EFI_PAGE_4KB - 1)) >> EFI_PAGE_SHIFT);

		AsmWriteCr0(Cr0);

		CpuFlushTlb();

		return TRUE;
	}

    DEBUG((DEBUG_INFO, "[SmartSmm] pte bits not present\n"));
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
	PTE4 Pml4;
	UINT8 PageSize = Page4K;
	if(MemRemapAddress(gRemapPage, Dir, SmmDir, &PageSize)) {
		TargetAddress = gRemapPage;

		if(PageSize == Page1G) {
			TargetAddress += Dir & 0x3FFFFFF;
		} else if (PageSize == Page2M) {
			TargetAddress += Dir & 0x1FFFFF;
		} else {
			TargetAddress += Dir & 0xFFF;
		}

		Pml4.Value = *(UINT64 *)(TargetAddress + (((UINT64)Address >> 39) & 0x1FF) * sizeof(UINT64));

		MemRestoreSmramMappings();
	} else {
		return 0;
	}

	// remap PDP
	PDPE Pdpe;
	if(Pml4.Bits.Present) {
		ReadAddress = Pml4.Bits.Pfn << EFI_PAGE_SHIFT;
		if(MemRemapAddress(gRemapPage, ReadAddress, SmmDir, &PageSize)) {
			TargetAddress = gRemapPage;

			if(PageSize == Page1G) {
				TargetAddress += ReadAddress & 0x3FFFFFF;
			} else if(PageSize == Page2M) {
				TargetAddress += ReadAddress & 0x1FFFFF;
			} else {
				TargetAddress += ReadAddress & 0xFFF;
			}

			Pdpe.Value = *(UINT64 *)(TargetAddress + (((UINT64)Address >> 30) & 0x1FF) * sizeof(UINT64));
			
			MemRestoreSmramMappings();
		} else {
			return 0;
		}
	} else {
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

			if(PageSize == Page1G) {
				TargetAddress += ReadAddress & 0x3FFFFFF;
			} else if(PageSize == Page2M) {
				TargetAddress += ReadAddress & 0x1FFFFF;
			} else {
				TargetAddress += ReadAddress & 0xFFF;
			}

			Pde.Value = *(UINT64 *)(TargetAddress + (((UINT64)Address >> 21) & 0x1FF) * sizeof(UINT64));
		
			MemRestoreSmramMappings();
		} else {
			return 0;
		}
	} else {
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

			if(PageSize == Page2M) {
				TargetAddress += ReadAddress & 0x1FFFFF;
			} else {
				TargetAddress += ReadAddress & 0xFFF;
			}

			Pte.Value = *(UINT64 *)(TargetAddress + (((UINT64)Address >> 12) & 0x1FF) * sizeof(UINT64));

			MemRestoreSmramMappings();
		} else {
			return 0;
		}
	} else {
		return 0;
	}

	if(Pte.Bits.Present)
		return ((Pte.Bits.Pfn << EFI_PAGE_SHIFT) + ((UINT64)Address & 0xFFF));
	else
		DEBUG((DEBUG_INFO, "[SmartSmm] PTE is not present for current virtual address\r\n"));

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
	UINT8 PageSize = Page4K;
	UINT64 RemapedMemory = gRemapPage;
	UINT64 SmmDir = AsmReadCr3();
    DEBUG((DEBUG_INFO, "[SmartSmm] SmmDir: 0x%016lx, 0x%016lx, 0x%016lx\n", SmmDir, RemapedMemory, PhysAddress));
    
	if(MemRemapAddress(gRemapPage, PhysAddress, SmmDir, &PageSize)) {		
		if(PageSize == Page1G) {
			RemapedMemory += PhysAddress & 0x3FFFFFF;
		} else if(PageSize == Page2M) {
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

    DEBUG((DEBUG_INFO, "[SmartSmm] MemMapVA\n"));

	if(!VirtualAddress || !DirBase)
		return 0;

    DEBUG((DEBUG_INFO, "[SmartSmm] valid Virtual address\n"));

	PhysRemaped = 0;

	if(!MemCheckPagingEnabled())
		return 0;

    DEBUG((DEBUG_INFO, "[SmartSmm] paging enabled\n"));

	// translate virtual address to physical
	TranslatedAddress = MemTranslateVirtualToPhys(VirtualAddress, DirBase);
    DEBUG((DEBUG_INFO, "[SmartSmm] Translated address: %x\n", TranslatedAddress));
	if(TranslatedAddress != 0) {
		// map physical address into the SMRAM memory 
		PhysRemaped = MemProcessOutsideSmramPhysMemory(TranslatedAddress);
		if(PhysRemaped == 0) {
            DEBUG((DEBUG_INFO, "[SmartSmm] phys remapped is 0\n"));
			return 0;
        }

        DEBUG((DEBUG_INFO, "[SmartSmm] physRemapped not 0\n"));
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
	DEBUG((DEBUG_INFO, "[SmartSmm] Reading from physical memory\r\n"));

	BOOLEAN check = MemCheckPagingEnabled();
	if (check == TRUE) {
		DEBUG((DEBUG_INFO, "[SmartSmm] SMI enabled\n"));
	} else {
		DEBUG((DEBUG_INFO, "[SmartSmm] SMI not enabled\n"));
	}

	// validate input
	if((!AddressToRead || !LengthToRead || !ReceivedInfo) || LengthToRead > BASE_4KB) {
        DEBUG((DEBUG_INFO, "[SmartSmm] Invalid parameters has been passed to specific command\r\n"));
		return EFI_INVALID_PARAMETER;
	}

    DEBUG((DEBUG_INFO, "[SmartSmm] Input validated\r\n"));

	// map physical address to the SMRAM
	UINT64 PhysMapped = MemProcessOutsideSmramPhysMemory(AddressToRead);
	if(PhysMapped != 0) {
		// copy data to intermediate page
        DEBUG((DEBUG_INFO, "[SmartSmm Prepare to phsmemcpy\r\n"));

		PhysMemCpy(ReceivedInfo, PhysMapped, LengthToRead);

        DEBUG((DEBUG_INFO, "[SmartSmm] 0x%016lx, 0x%016lx\r\n", PhysMapped, AddressToRead));


		MemRestoreSmramMappings();
	} else {
		DEBUG((DEBUG_INFO, "[SmartSmm] Unable to map physical address\r\n"));
		return EFI_ABORTED;
	}

	return EFI_SUCCESS;
}

/**
 * \brief Writes data to provided physical address
 * 
 * \param AddressToWrite Provided physical address which should
 *                       be modified
 * \param DataToWrite    Provided data which should be written to
 *                       specified physical address (virtual buffer)
 * \param LengthToWrite  Size of data
 * 
 * \return EFI_SUCCESS - Operation was performed succesfully
 * \return EFI_INVALID_PARAMETER - One or more arguments are invalid
 * \return EFI_ABORTED - Unable to translate or map address
 * \return Other - Ynable to allocate interim page
 */
EFI_STATUS
EFIAPI
CmdPhysWrite(
	IN VOID    *AddressToWrite,
	IN VOID    *DataToWrite,
	IN UINT64   LengthToWrite 
) {
	DEBUG((DEBUG_INFO, "[SmartSmm] Writing to the physical memory\r\n"));

	// validate input
	if((!AddressToWrite || !DataToWrite || !LengthToWrite) || LengthToWrite > BASE_4KB) {
		DEBUG((DEBUG_INFO, "[SmartSmm] Invalid parameters has been passed to specific command\r\n"));
		return EFI_INVALID_PARAMETER;
	}

	// map target physical memory to the SMRAM
	UINT64 PhysMapped = MemProcessOutsideSmramPhysMemory(AddressToWrite);
	if(PhysMapped == 0) {
		DEBUG((DEBUG_INFO, "[SmartSmm] Unable to map physical memory to the SMRAM\r\n"));
		return EFI_ABORTED;
	}

	// copy data to the target address
	PhysMemCpy(PhysMapped, DataToWrite, LengthToWrite);

	MemRestoreSmramMappings();

	return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
SmartSmiHandler (
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
	RequestParameters *Req = NULL;

    Status = mMmCpuIo->Io.Read(mMmCpuIo, MM_IO_UINT8, ICH9_APM_CNT, 1, &APM_CNT_VALUE);
    ASSERT_EFI_ERROR(Status);

    if (APM_CNT_VALUE != 0x01)
    {
        return EFI_WARN_INTERRUPT_SOURCE_QUIESCED;
    }

    DEBUG((DEBUG_INFO, "[SmartSmm] SMI 0x%02x\n", APM_CNT_VALUE));

	Status = mSmmVariable->SmmGetVariable(
        MyVariableName,
        &VendorGuid,
        &Attributes,
        &VariableSize,
        NULL
    );
    
	DEBUG((DEBUG_INFO, "size is %ld\n", VariableSize));

    if (Status == EFI_BUFFER_TOO_SMALL) {
        VariableData = AllocateZeroPool(VariableSize > 4096 ? VariableData : 4096);
        if (VariableData == NULL) {
            DEBUG((DEBUG_INFO, "AllocateZeroPool Failed\n"));
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
            DEBUG((DEBUG_INFO, "Variable Data: 0x%016lx\n", VariableData));

			for (UINT64 j = 0; j < VariableSize; ++j) {
				DEBUG((DEBUG_INFO, "0x%02x ", ((UINT8 *)VariableData)[j]));
			}
			DEBUG((DEBUG_INFO, "\n"));

        } else {
            DEBUG((DEBUG_INFO, "Get Variable failed: %r\n", Status));
			return EFI_WARN_INTERRUPT_SOURCE_QUIESCED;
        }

		Req = (RequestParameters *)VariableData;

    } else {
        DEBUG((DEBUG_INFO, "Get Variable failed: %r\n", Status));
		return EFI_WARN_INTERRUPT_SOURCE_QUIESCED;
    }


	if (Req == NULL) {
    	return EFI_WARN_INTERRUPT_SOURCE_QUIESCED;
	}

	if (Req->ParameterId == 0) {
		EFI_PHYSICAL_ADDRESS BufferAddress = 0UL;
		Status = gSmst->SmmAllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &BufferAddress);
		ASSERT_EFI_ERROR(Status);

		VOID *DataPtr = Req->ReadParameter.TargetAddress;
		UINT64 BytesToRead = Req->ReadParameter.BytesToRead;
		DEBUG((DEBUG_INFO, "into here, 0x%016lx, %ld\n", DataPtr, BytesToRead));
		Status = CmdPhysRead(DataPtr, BufferAddress, BytesToRead);

		for (UINT64 i = 0; i < BytesToRead; ++i) {
			DEBUG((DEBUG_INFO, "%d\n", ((UINT8 *)BufferAddress)[i]));
		}

		ASSERT_EFI_ERROR(Status);
	} else if (Req->ParameterId == 1) {
		VOID *DataPtr = Req->WriteParameter.TargetAddress;
		UINT64 BytesToWrite = Req->WriteParameter.BytesToWrite;
		VOID *DataToWrite = (void *)Req->WriteParameter.DataToWrite;

		for (UINT64 i = 0; i < BytesToWrite; ++i) {
			DEBUG((DEBUG_INFO, "%d ", ((UINT8 *)DataToWrite)[i]));
		}
		DEBUG((DEBUG_INFO, "\n"));

		Status = CmdPhysWrite(DataPtr, DataToWrite, BytesToWrite);
		ASSERT_EFI_ERROR(Status);
	}

	FreePool(VariableData);
    return EFI_WARN_INTERRUPT_SOURCE_QUIESCED;
}

EFI_STATUS
EFIAPI
SmartSmmInitialize (
    IN EFI_HANDLE ImageHandle,
    IN EFI_SYSTEM_TABLE* SystemTable
    )
{
    EFI_STATUS Status;
    EFI_HANDLE Handle;

    DEBUG((DEBUG_INFO, "[SmartSmm] SmartSmmInitialize called\n"));

    Status = gMmst->MmLocateProtocol(&gEfiMmCpuIoProtocolGuid, NULL, (VOID **)&mMmCpuIo);
    ASSERT_EFI_ERROR(Status);

	Status = gSmst->SmmLocateProtocol(&gEfiSmmVariableProtocolGuid, NULL, (VOID **)&mSmmVariable);
    ASSERT_EFI_ERROR(Status);
   
    Status = gMmst->MmiHandlerRegister(SmartSmiHandler, NULL, &Handle);
    ASSERT_EFI_ERROR(Status);

    // allocate page for remaping virtual and physical addresses
	Status = gSmst->SmmAllocatePages(AllocateAnyPages, EfiRuntimeServicesData, 1, &gRemapPage);
	if(EFI_ERROR(Status)) {
		DEBUG((DEBUG_INFO, "[SmartSmm] Unable to allocate remap page\r\n"));
		return Status;
	}

	gBS->SetMem(gRemapPage, EFI_PAGE_SIZE, 0);

    return Status;
}
