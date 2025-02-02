/* 
 * Author: Ryan Proietto
 * Description: This file was written in Vitis 2024 in support of the Xilinx ZCU102
 * Reference Board. This program was written to run on the Cortex-R5 processor and
 * is responsible for bootstrapping an ELF32 application from the on-board SDHC SD
 * Card.
 */

// Standard Libraries 
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
uint8_t load_elf32(const char *file_name);
void print_buffer(const uint8_t *buffer, size_t size);

// Definitions
#define CHUNK_SIZE 4096

// Main
int main() 
{
    // Ensure filename is short unless you have enabled long file name support in the BSP settings.
    // The file will fail to open otherwise with no explainable behavior.
    load_elf32("vxWorks.elf");
    return 0;
}

uint8_t load_elf32(const char *file_name) 
{
    FRESULT fr;  
    FATFS fs;    
    FIL file;    
    UINT bytesRead;
    Elf32_Ehdr elfHeader;

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
        xil_printf("File is not a valid ELF32 file\r\n");
        f_close(&file);
        return -1;
    }    
    xil_printf("Valid ELF32 file identified.\r\n");

    // Debug: Print ELF header info
    xil_printf("ELF Header - Program header offset: %u, Number of program headers: %u\r\n",
        elfHeader.e_phoff, elfHeader.e_phnum);

    // Jump to the program headers
    if (elfHeader.e_phoff >= f_size(&file)) 
    {
        xil_printf("Invalid program header offset.\r\n");
        f_close(&file);
        return -1;
    }
    
    // Allocate memory for all program headers
    Elf32_Phdr *programHeaders = malloc(elfHeader.e_phnum * sizeof(Elf32_Phdr));
    if (programHeaders == NULL) 
    {
        xil_printf("Memory allocation for program headers failed.\r\n");
        f_close(&file);
        return -1;
    }

    f_lseek(&file, elfHeader.e_phoff); // Seek to the program headers section

    // Read all program headers at once
    fr = f_read(&file, programHeaders, elfHeader.e_phnum * sizeof(Elf32_Phdr), &bytesRead);
    if (fr != FR_OK || bytesRead != elfHeader.e_phnum * sizeof(Elf32_Phdr)) 
    {
        xil_printf("Failed to read program headers; Read bytes: %u, Expected: %u\r\n", bytesRead, elfHeader.e_phnum * sizeof(Elf32_Phdr));
        free(programHeaders);
        f_close(&file);
        return -1;
    }

    for (int i = 0; i < elfHeader.e_phnum; i++) 
    {
        Elf32_Phdr *programHeader = &programHeaders[i];

        // Print the values of the program header
        xil_printf("Program header %d read successfully: type=0x%x, offset=0x%x, filesz=0x%x, memsz=0x%x\r\n", 
            i, programHeader->p_type, programHeader->p_offset, programHeader->p_filesz, programHeader->p_memsz);

        // Validate segment offset
        if (programHeader->p_offset + programHeader->p_filesz > f_size(&file)) 
        {
            xil_printf("Invalid segment offset for program header %d: offset=0x%x, filesize=0x%x\r\n", 
                i, programHeader->p_offset, f_size(&file));
            free(programHeaders);
            f_close(&file);
            return -1;
        }

        // Seek to the segment data offset
        f_lseek(&file, programHeader->p_offset);
        
        // Allocate memory for the segment
        void *segmentMemory = (void *)(programHeader->p_vaddr);

        // Read segment data into memory
        uint8_t buffer[CHUNK_SIZE];
        uint32_t bytesToRead = programHeader->p_filesz; // Total bytes to read
        uint32_t bytesLoaded = 0;

        xil_printf("Reading segment data: offset=0x%x, filesize=0x%x, memsize=0x%x\r\n", 
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
                xil_printf("Error reading segment data at offset 0x%x: %d\r\n", programHeader->p_offset + bytesLoaded, fr);
                free(programHeaders);
                f_close(&file);
                return -1;
            }

            // Copy data to allocated memory
            memcpy(segmentMemory + bytesLoaded, buffer, bytesRead);
            bytesLoaded += bytesRead;
            bytesToRead -= bytesRead;

            // Flush cache
            Xil_DCacheFlushRange((uint32_t)(segmentMemory + bytesLoaded - bytesRead), bytesRead);
        }

        // Clear uninitialized space
        if (programHeader->p_memsz > programHeader->p_filesz) 
        {
            memset(segmentMemory + bytesLoaded, 0, programHeader->p_memsz - programHeader->p_filesz);
        }

        // Print the loaded segment information        
        xil_printf("Segment loaded successfully: vaddr=0x%x, filesz=0x%x, memsz=0x%x\r\n",
            programHeader->p_vaddr, programHeader->p_filesz, programHeader->p_memsz);
    }

    xil_printf("All segments loaded successfully.\r\n");
    free(programHeaders); // Free allocated memory for program headers
    f_close(&file);

    // Calculate the entry point
    uint32_t entry_point = elfHeader.e_entry;
    xil_printf("Entry point calculated: %x\r\n", entry_point);

    // Inline assembly to branch to the entry point for the PC register
    asm volatile("blx %0":: "r" (entry_point));
    
    // Will not return, but just in case
    xil_printf("Returned from ELF program (this should not happen).\r\n");

    return 0; // Will not return
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
