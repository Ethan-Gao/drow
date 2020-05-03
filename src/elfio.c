#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdbool.h>
#include <elf.h>
#include "elfio.h"
#include "drow.h"

static int _get_file_size(const char *filepath)
{
    struct stat st;
    if (stat(filepath, &st) < 0)
        return -1;
    return st.st_size;
}

bool load_elf(drow_ctx_t **ctx, const char *elffile)
{
    int size;
    int fd;
    void *elf = NULL;

    printf(INFO "Loading ELF file: %s\n", elffile);
    *ctx = NULL;
    size = _get_file_size(elffile);
    if (size < 0) {
        fprintf(stderr, ERR "Failed to get ELF file size\n");
        return false;
    }

    fd = open(elffile, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, ERR "Failed to open ELF file\n");
        return false;
    }

    elf = (void *)mmap(NULL, size, PROT_READ|PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (!elf) {
        fprintf(stderr, ERR "Failed to map ELF file\n");
        goto error;
    }

    *ctx = (drow_ctx_t *)malloc(sizeof(drow_ctx_t));
    if (!*ctx) {
        fprintf(stderr, ERR "Failed to allocate memory for drow context\n");
        goto error;
    }

    (*ctx)->fd = fd;
    (*ctx)->size = size;
    (*ctx)->elf = elf;
    return true;
error:
    free(*ctx);
    if (elf)
        munmap(elf, size);
    if (fd != -1)
        close(fd);
    return false;
}

bool load_payload(payload_t **payload, const char *payload_file)
{
    int size;
    int fd;
    void *data = NULL;

    *payload = NULL;
    printf(INFO "Loading payload blob: %s\n", payload_file);
    size = _get_file_size(payload_file);
    if (size < 0) {
        fprintf(stderr, ERR "Failed to get payload file size\n");
        return false;
    }

    fd = open(payload_file, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, ERR "Failed to open payload\n");
        return false;
    }

    data = (uint8_t *)mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (!data) {
        fprintf(stderr, ERR "Failed to map payload\n");
        goto error;
    }

    *payload = (payload_t *)malloc(sizeof(payload_t));
    if (!*payload) {
        fprintf(stderr, ERR "Failed to allocate memory for payload\n");
        goto error;
    }

    (*payload)->fd = fd;
    (*payload)->data = data;
    (*payload)->size = size;
    return true;
error:
    free(*payload);
    if (data)
        munmap(data, size);
    if (fd != -1)
        close(fd);
    return false;
}

void unload_elf(drow_ctx_t *ctx)
{
    if (!ctx)
        return;

    if (ctx->elf)
        munmap(ctx->elf, ctx->size);

    if (ctx->fd != -1)
        close(ctx->fd);
}

bool expand_section(drow_ctx_t *ctx, struct shinfo *sinfo, struct patchinfo *pinfo)
{
    Elf64_Ehdr *ehdr;
    size_t adjust;
    size_t size;
    uint32_t newoff;
    Elf64_Shdr *shtable;
    size_t i;

    ehdr = (Elf64_Ehdr *)ctx->elf;

    /* Set patch information */
    size = *sinfo->size;
    pinfo->base = *sinfo->offset + size;
    pinfo->size = getpagesize();

    /* Fix up size */
    printf(INFO "Expanding %s size by %lu bytes...\n", sinfo->name, pinfo->size);
    *sinfo->size = size + sinfo->slackspace;
    adjust = getpagesize();

    printf(INFO "Adjusting Section Header offsets ...\n");
    shtable = (Elf64_Shdr *)((uintptr_t)ctx->elf + ehdr->e_shoff);
    for (i = 0; i < ehdr->e_shnum; i++) {
        if (shtable[i].sh_offset < pinfo->base)
            continue;
        newoff = shtable[i].sh_offset + adjust;
        shtable[i].sh_offset = newoff;
    }

    printf(INFO "Adjusting Program Header offsets ...\n");
    Elf64_Phdr *phdr = (Elf64_Phdr *)((uintptr_t)ctx->elf + ehdr->e_phoff);
    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_offset > pinfo->base) {
            phdr[i].p_offset = phdr[i].p_offset + pinfo->size;
        }

        if (phdr[i].p_flags & PF_X) {
            phdr[i].p_filesz += pinfo->size;
            phdr[i].p_memsz += pinfo->size;
        }
    }

    printf(INFO "Adjusting ELF header offsets ...\n");
    if (ehdr->e_shoff > pinfo->base)
        ehdr->e_shoff = ehdr->e_shoff + pinfo->size;

    if (ehdr->e_phoff > pinfo->base)
        ehdr->e_phoff = ehdr->e_phoff + pinfo->size;

    return true;
}

bool export_elf_file(drow_ctx_t *ctx, payload_t *payload, char *outfile, struct patchinfo *pinfo)
{
    int fd;
    int n;
    char *pad = NULL;
    size_t padsize;
    size_t remaining;
    bool rv = false;

    printf(INFO "Exporting patched ELF to %s ...\n", outfile);
    fd = open(outfile, O_RDWR|O_CREAT|O_TRUNC, 0777);
    if (fd == -1) {
        fprintf(stderr, ERR "Failed to create patched ELF\n");
        return false;
    }

    printf(INFO "Writing first part of ELF (size: %u)\n", pinfo->base);
    n = write(fd, ctx->elf, pinfo->base);
    if ((uint32_t)n != pinfo->base) {
        fprintf(stderr, ERR "Failed to export ELF (write 1st ELF chunk)\n");
        goto done;
    }

    printf(INFO "Writing payload (size: %lu)\n", payload->size);
    n = write(fd, payload->data, payload->size);
    if ((size_t)n != payload->size) {
        fprintf(stderr, ERR "Failed to export ELF (write payload)\n");
        goto done;
    }

    /* Allocate buffer for pad */
    padsize = pinfo->size - payload->size;
    pad = (char *)malloc(padsize);
    if (pad == NULL) {
        fprintf(stderr, ERR "Failed to export ELF (out of memory?)\n");
        goto done;
    }
    memset(pad, 0, padsize);

    printf(INFO "Writing pad to maintain page alignment (size: %lu)\n", padsize);
    n = write(fd, pad, padsize);
    if ((size_t)n != padsize) {
        fprintf(stderr, ERR "Failed to export ELF (write pad)\n");
        goto done;
    }

    /* Write rest of the ELF */
    remaining = ctx->size - pinfo->base;
    if (remaining) {
        printf(INFO "Writing remaining data (size: %lu)\n", remaining);
        n = write(fd, ctx->elf + pinfo->base, remaining);
        if ((size_t)n != remaining) {
            fprintf(stderr, ERR "Failed to export ELF (write remaining)\n");
            goto done;
        }
    }

    rv = true;
done:
    free(pad);
    close(fd);
    return rv;
}

struct shinfo *find_exe_seg_last_section(drow_ctx_t *ctx)
{
    Elf64_Ehdr *ehdr;
    Elf64_Phdr *phdr;
    Elf64_Shdr *shtable;
    char *shstr;
    uint32_t segment_end;
    struct shinfo *sinfo = NULL;
    size_t i, j;

    ehdr = (Elf64_Ehdr *)ctx->elf;
    phdr = (Elf64_Phdr *)((uintptr_t)ctx->elf + ehdr->e_phoff);
    shtable = (Elf64_Shdr *)((uintptr_t)ctx->elf + ehdr->e_shoff);
    shstr = (char *)((uintptr_t)ctx->elf + shtable[ehdr->e_shstrndx].sh_offset);

    for (i = 0; i < ehdr->e_phnum; i++) {
        if (phdr[i].p_flags & PF_X) {
            printf(SUCCESS "Found executable segment at 0x%08lx (size:%08lx)\n", phdr[i].p_offset, phdr[i].p_memsz);
            /* Found the executable segment, now find the last section in the segment */
            segment_end = phdr[i].p_vaddr + phdr[i].p_memsz;
            for (j = 0; j < ehdr->e_shnum; j++) {
                if (shtable[j].sh_addr + shtable[j].sh_size == segment_end) {
                    sinfo = (struct shinfo *)malloc(sizeof(*sinfo));
                    if (!sinfo) {
                        fprintf(stderr, ERR "Out of memory!?");
                        return NULL;
                    }

                    strncpy(sinfo->name, shstr+shtable[j].sh_name, MAX_SH_NAMELEN);
                    sinfo->offset     = (uint32_t *)&shtable[j].sh_offset;
                    sinfo->size       = (uint32_t *)&shtable[j].sh_size;
                    sinfo->slackspace = getpagesize();
                }
            }
        }
    }
    return sinfo;
}
