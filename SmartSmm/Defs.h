#pragma once

typedef enum _PAGE_SIZE_ENUMRATE {
	Page4K = 0,
	Page2M,
	Page1G
} PAGE_SIZE_ENUMRATE;

typedef union {
	INT64 ParameterId;

	struct {
		INT64 ParameterId;
		VOID *TargetAddress;
		UINT64 BytesToRead;
	} ReadParameter;

	struct {
		INT64 ParameterId;
		VOID *TargetAddress;
		UINT64 BytesToWrite;
		CHAR8 DataToWrite[128];
	} WriteParameter;
} RequestParameters;