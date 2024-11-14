/* 
 * Author: Ryan Proietto
 * Description: This file was written in Vitis 2024 in support of the Xilinx ZCU102
 * Reference Board. This program was written to run on the Cortex-A53 processor and
 * is responsible for bootstrapping the AT-F(bl31) hand-off routine which will load
 * into the second stage bootlaoder.(u-boot) These binaries are located within the 
 * on-board SD Card. 
 */

// Standard Libraries 
#include "stdlib.h"
#include "stdio.h"
#include "stdint.h"
#include "string.h"

// Xilinx Libraries
#include "ff.h"         // Include the FatFs library header
#include "xil_cache.h"  // Include cache management functions
#include <xil_printf.h> // Include Debug IO

// Addtional Libraries
#include "elf.h"

// Prototypes
uint64_t load_elf64(const char *file_name);
void print_buffer(const uint8_t *buffer, size_t size);
void reset_apu_cores(uint32_t value);
void set_apu_rvba(uint32_t entrypoint);
void delay_ms(int milliseconds);
void mock_fsbl_SetATFHandoffParams(uint32_t EntryCount, uint32_t PartitionHeader, uint32_t PartitionFlags);

// Generic Definitions
#define CHUNK_SIZE 4096

// Required for pointing to mock handoff structure
#define MAX_ENTRIES 10 // Define a maximum number of entries limit
#define GLOBAL_GEN_STORAGE6 (*(volatile uint32_t *)(0xFFD80048U))

// APU Module Reset Vector Base Address
#define RVBARADDR0L (*(volatile uint32_t *)(0xFD5C0040U))
#define RVBARADDR0H (*(volatile uint32_t *)(0xFD5C0044U))
#define RVBARADDR1L (*(volatile uint32_t *)(0xFD5C0048U))
#define RVBARADDR1H (*(volatile uint32_t *)(0xFD5C004CU))
#define RVBARADDR2L (*(volatile uint32_t *)(0xFD5C0050U))
#define RVBARADDR2H (*(volatile uint32_t *)(0xFD5C0054U))
#define RVBARADDR3L (*(volatile uint32_t *)(0xFD5C0058U))
#define RVBARADDR3H (*(volatile uint32_t *)(0xFD5C005CU))
#define RVBARADDR_LOW_VALU 0x0U

// APU Software Controlled MPCore Reset Address
#define RST_FPD_APU (*(volatile uint32_t *)(0xFD1A0104U))
#define RST_FPD_APU_VALU 0xFU
#define RST_FPD_APU_CLER 0x0U

// Main
int main() 
{
    delay_ms(3000);

    // Load AT-F onto Cortex-A53 processor and retrieve entry point
    uint32_t bl31_entrypoint = load_elf64("bl31.elf");

    // Load SSBL into DDR4 Memory
    load_elf64("u-boot.elf");

    // Call the function to set handoff parameters
    mock_fsbl_SetATFHandoffParams(0, bl31_entrypoint, 0);

    // Place the APU Cores in a soft reset state
    xil_printf("Placing APU Core(s) in reset state!\r\n");
    reset_apu_cores((uint32_t)RST_FPD_APU_VALU);

    xil_printf("APU Core(s) Current PC Values:\r\n");
    xil_printf("RVBARADDR0H: 0x%08X \r\n", RVBARADDR0H);
    xil_printf("RVBARADDR1H: 0x%08X \r\n", RVBARADDR1H);
    xil_printf("RVBARADDR2H: 0x%08X \r\n", RVBARADDR2H);
    xil_printf("RVBARADDR3H: 0x%08X \r\n", RVBARADDR3H);

    // Modify the RVBARADDR for each APU core to point to AT-F
    xil_printf("Relocating APU Core(s) PC to: 0x%8X\r\n", bl31_entrypoint);
    set_apu_rvba( bl31_entrypoint);

    xil_printf("APU Core(s) Updated PC Values:\r\n");
    xil_printf("RVBARADDR0H: 0x%08X \r\n", RVBARADDR0H);
    xil_printf("RVBARADDR1H: 0x%08X \r\n", RVBARADDR1H);
    xil_printf("RVBARADDR2H: 0x%08X \r\n", RVBARADDR2H);
    xil_printf("RVBARADDR3H: 0x%08X \r\n", RVBARADDR3H);
    
    // Clear the APU Core reset state
    xil_printf("Clearing APU Core(s) reset state!\r\n");
    reset_apu_cores((uint32_t)RST_FPD_APU_CLER);

    // BL31 has now loaded and is looking to handoff address set in compile-time binary
    // which should be the ssbl (u-boot)

    // Debug - Loop Forever 
    while(1)
    {

    };
    
    return 0;
}

uint64_t load_elf64(const char *file_name) 
{
    FRESULT fr;  
    FATFS fs;    
    FIL file;    
    UINT bytesRead;
    Elf64_Ehdr elfHeader;

    // Mount the file system
    fr = f_mount(&fs, "0:", 0); 
    if (fr != FR_OK) 
    {
        xil_printf("Failed to mount SD card.\r\n");
        return -1;
    }
    xil_printf("SD card mounted successfully.\r\n");

    // Open the ELF file
    fr = f_open(&file, file_name, FA_READ);
    if(fr != FR_OK)
    {
        xil_printf("Failed to open file: %s (%d bytes)\r\n", file_name, fr);
        return -1;
    }
    xil_printf("File opened successfully: %s\r\n", file_name);

    // Read ELF header
    fr = f_read(&file, &elfHeader, sizeof(elfHeader), &bytesRead);
    if (fr != FR_OK || bytesRead != sizeof(elfHeader)) 
    {
        xil_printf("Failed to read ELF header\r\n");
        f_close(&file);
        return -1;
    }
    xil_printf("ELF header read successfully.\r\n");

    // Validate ELF identification
    if (elfHeader.e_ident[0] != ELFMAG0 || elfHeader.e_ident[1] != ELFMAG1 ||
        elfHeader.e_ident[2] != ELFMAG2 || elfHeader.e_ident[3] != ELFMAG3) 
    {
        xil_printf("File is not a valid ELF file\r\n");
        f_close(&file);
        return -1;
    }    
    xil_printf("Valid ELF file identified.\r\n");

    // Debug: Print ELF header info
    xil_printf("ELF Header - Program header offset: %llu, Number of program headers: %u\r\n",
        elfHeader.e_phoff, elfHeader.e_phnum);

    // Jump to the program headers
    if (elfHeader.e_phoff >= f_size(&file)) 
    {
        xil_printf("Invalid program header offset.\r\n");
        f_close(&file);
        return -1;
    }
    
    // Allocate memory for all program headers
    Elf64_Phdr *programHeaders = malloc(elfHeader.e_phnum * sizeof(Elf64_Phdr));
    if (programHeaders == NULL) 
    {
        xil_printf("Memory allocation for program headers failed.\r\n");
        f_close(&file);
        return -1;
    }

    f_lseek(&file, elfHeader.e_phoff); // Seek to the program headers section

    // Read all program headers at once
    fr = f_read(&file, programHeaders, elfHeader.e_phnum * sizeof(Elf64_Phdr), &bytesRead);
    if (fr != FR_OK || bytesRead != elfHeader.e_phnum * sizeof(Elf64_Phdr)) 
    {
        xil_printf("Failed to read program headers; Read bytes: %u, Expected: %u\r\n", bytesRead, elfHeader.e_phnum * sizeof(Elf64_Phdr));
        free(programHeaders);
        f_close(&file);
        return -1;
    }

    for (int i = 0; i < elfHeader.e_phnum; i++) 
    {
        Elf64_Phdr *programHeader = &programHeaders[i];

        // Print the values of the program header
        xil_printf("Program header %d read successfully: type=0x%x, offset=0x%llx, filesz=0x%llx, memsz=0x%llx\r\n", 
            i, programHeader->p_type, programHeader->p_offset, programHeader->p_filesz, programHeader->p_memsz);

        // Validate segment offset
        if (programHeader->p_offset + programHeader->p_filesz > f_size(&file)) 
        {
            xil_printf("Invalid segment offset for program header %d: offset=0x%llx, filesize=0x%llx\r\n", 
                i, programHeader->p_offset, f_size(&file));
            free(programHeaders);
            f_close(&file);
            return -1;
        }

        // Seek to the segment data offset
        f_lseek(&file, programHeader->p_offset);
        
        // Allocate memory for the segment
        void *segmentMemory = (void *)(uintptr_t)(programHeader->p_vaddr);

        // Read segment data into memory
        uint8_t buffer[CHUNK_SIZE];
        uint64_t bytesToRead = programHeader->p_filesz; // Total bytes to read
        uint64_t bytesLoaded = 0;

        xil_printf("Reading segment data: offset=0x%llx, filesize=0x%llx, memsize=0x%llx\r\n", 
            programHeader->p_offset, programHeader->p_filesz, programHeader->p_memsz);

        // Read data in chunks
        while (bytesToRead > 0) 
        {
            uint32_t chunkSize;
            if (bytesToRead > CHUNK_SIZE) 
            {
                chunkSize = CHUNK_SIZE;
            } 
            else 
            {
                chunkSize = bytesToRead;
            }

            fr = f_read(&file, buffer, chunkSize, &bytesRead);
            if (fr != FR_OK || bytesRead == 0) 
            {
                xil_printf("Error reading segment data at offset 0x%llx: %d\r\n", programHeader->p_offset + bytesLoaded, fr);
                free(programHeaders);
                f_close(&file);
                return -1;
            }

            // Copy data to allocated memory
            memcpy(segmentMemory + bytesLoaded, buffer, bytesRead);
            bytesLoaded += bytesRead;
            bytesToRead -= bytesRead;

            // Flush cache
            Xil_DCacheFlushRange((uint32_t)(uintptr_t)(segmentMemory + bytesLoaded - bytesRead), bytesRead);
        }

        // Clear uninitialized space
        if (programHeader->p_memsz > programHeader->p_filesz) 
        {
            memset(segmentMemory + bytesLoaded, 0, programHeader->p_memsz - programHeader->p_filesz);
        }

        // Print the loaded segment information        
        xil_printf("Segment loaded successfully: vaddr=0x%llx, filesz=0x%llx, memsz=0x%llx\r\n",
            programHeader->p_vaddr, programHeader->p_filesz, programHeader->p_memsz);
    }

    xil_printf("All segments loaded successfully.\r\n");
    free(programHeaders); // Free allocated memory for program headers
    f_close(&file);

    // Calculate the entry point
    uint32_t entry_point = elfHeader.e_entry;
    xil_printf("Entry point calculated: 0x%08x\r\n", entry_point);

    // return entry point
    return entry_point;
}

// Debug for printing buffer data and their ASCI values similar to BIO_DUMP
void print_buffer(const uint8_t *buffer, size_t size) 
{
    const size_t bytes_per_line = 16; // Define how many bytes to print per line

    for (size_t i = 0; i < size; i += bytes_per_line) 
    {
        // Print starting address
        xil_printf("%08X  ", (unsigned int)i);

        // Print the hexadecimal values
        for (size_t j = 0; j < bytes_per_line; j++) 
        {
            if (i + j < size) 
            {
                xil_printf("%02X ", buffer[i + j]); // Print each byte as hex
            } 
            else 
            {
                xil_printf("   "); // Print spaces for unused bytes
            }
        }

        // Print ASCII representation
        xil_printf(" |");
        for (size_t j = 0; j < bytes_per_line; j++) 
        {
            if (i + j < size) 
            {
                uint8_t byte = buffer[i + j];
                // Print printable characters; use dot for non-printable
                if (byte >= 32 && byte < 127) 
                { 
                    xil_printf("%c", byte);
                } 
                else 
                {
                    xil_printf("."); // Non-printable characters
                }
            }
        }
        xil_printf("|\r\n"); // New line
    }
}

void reset_apu_cores(uint32_t value)
{
    // Write modified value back to the register
    RST_FPD_APU = (uint32_t)value;
}

void set_apu_rvba(uint32_t entrypoint)
{
    // Set the Reset Vector Base Address Low Bits to 0x0
    RVBARADDR0L = (uint32_t)RVBARADDR_LOW_VALU;
    RVBARADDR1L = (uint32_t)RVBARADDR_LOW_VALU;
    RVBARADDR2L = (uint32_t)RVBARADDR_LOW_VALU;
    RVBARADDR3L = (uint32_t)RVBARADDR_LOW_VALU;

    // Set the Reset Vector Base Address High Bits to Entry Point
    RVBARADDR0H = (uint32_t)entrypoint;
    RVBARADDR1H = (uint32_t)entrypoint;
    RVBARADDR2H = (uint32_t)entrypoint;
    RVBARADDR3H = (uint32_t)entrypoint;
}

void delay_ms(int milliseconds) 
{
    for (int i = 0; i < milliseconds; i++) 
    {
        for (volatile int j = 0; j < (600000000/1000); j++) 
        {
            // Busy wait
        }
    }
}

typedef struct 
{
    uintptr_t EntryPoint; // Address to execute the partition
    uint32_t PartitionFlags; // Flags for the partition properties
} ATFHandoffEntry;

typedef struct 
{
    char MagicValue[4]; // Magic value for identification
    uint32_t NumEntries; // Number of entries
    ATFHandoffEntry Entry[MAX_ENTRIES]; // Array of entry parameters
} ATFHandoffParamsStruct;

void mock_fsbl_SetATFHandoffParams(uint32_t EntryCount, uint32_t PartitionHeader, uint32_t PartitionFlags) 
{
    ATFHandoffParamsStruct ATFHandoffParams;
    
    /* Insert magic string */
    if (EntryCount == 0U) 
    {
        ATFHandoffParams.MagicValue[0] = 'X';
        ATFHandoffParams.MagicValue[1] = 'L';
        ATFHandoffParams.MagicValue[2] = 'N';
        ATFHandoffParams.MagicValue[3] = 'X';
    }

    ATFHandoffParams.NumEntries = EntryCount + 1U;

    // Ensure we do not exceed the maximum entries
    if (EntryCount < MAX_ENTRIES) 
    {
        ATFHandoffParams.Entry[EntryCount].EntryPoint = PartitionHeader; // Assuming Address is located at offset 1
        ATFHandoffParams.Entry[EntryCount].PartitionFlags = PartitionFlags;
    }

    // Now set GLOBAL_GEN_STORAGE6 to point to the ATFHandoffParams structure
    GLOBAL_GEN_STORAGE6 = (uint32_t)&ATFHandoffParams; // Cast the structure pointer to uint32_t  
}
