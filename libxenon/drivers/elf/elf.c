#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <xenon_soc/xenon_power.h>
#include <ppc/cache.h>
#include <ppc/timebase.h>
#include <time/time.h>
#include <usb/usbmain.h>
#include <debug.h>

#include "elf_abi.h"

#define ELF_DEVTREE_START ((void*)0x87FE0000)
#define ELF_DEVTREE_MAX_SIZE 0x10000

#define ELF_CODE_RELOC_START ((void*)0x87FF0000) /* TODO: must keep this synced with lis %r4,0x87ff and lis %r4,0x07ff in elf_run.S */
#define ELF_DATA_RELOC_START ((void*)0x88000000)
#define ELF_GET_RELOCATED(x) (ELF_CODE_RELOC_START+((unsigned long)(x)-(unsigned long)elfldr_start))

extern void enet_quiesce();

extern char elfldr_start[], elfldr_end[];
extern void elf_run(unsigned long entry,unsigned long devtree);
extern void elf_hold_thread();

extern volatile unsigned long elf_secondary_hold_addr;

static inline __attribute__((always_inline)) void elf_putch(unsigned char c)
{
	while (!((*(volatile uint32_t*)(0xea001000 + 0x18))&0x02000000));
	*(volatile uint32_t*)(0xea001000 + 0x14) = (c << 24) & 0xFF000000;
}

static inline __attribute__((always_inline)) void elf_puts(unsigned char *s)
{
	while(*s) elf_putch(*s++);
}

static inline __attribute__((always_inline)) void elf_int2hex(unsigned long long n,unsigned char w,unsigned char *str)
{
  unsigned char i,d;
  
  str+=w;
  *str=0;

  for(i=0;i<w;++i){
  	d= ( n >> (i << 2) ) & 0x0f;
    *--str= (d<=9)?d+48:d+55;
  };
}


static inline __attribute__((always_inline)) void elf_memset(unsigned char *dst, int v, int l)
{
	while (l--) *dst++ = v;
}

static inline __attribute__((always_inline)) void elf_memcpy(unsigned char *dst, const unsigned char *src, int l)
{
	while (l--)
		*dst++ = *src++;
}

#define LINESIZE 128
static inline __attribute__((always_inline)) void elf_sync(volatile void *addr)
{
	asm volatile ("dcbst 0, %0" : : "b" (addr));
	asm volatile ("sync");
	asm volatile ("icbi 0, %0" : : "b" (addr));
	asm volatile ("isync");
}
      
static inline __attribute__((always_inline)) void elf_sync_before_exec(unsigned char *dst, int l)
{
	dst=(unsigned char *)((unsigned long)dst&(~(LINESIZE-1)));
	
	l+=LINESIZE;
	l&=~(LINESIZE-1);
	
	while (l > 0)
	{
		elf_sync(dst);
		dst += LINESIZE;
		l -= LINESIZE;
	}
}

// optimize("O2") is required to prevent call to _savegpr, which would fail due to the relocation
static void __attribute__ ((section (".elfldr"),noreturn,flatten,optimize("O2"))) elf_prepare_run (void *addr)
{
	unsigned char s[50];
	
	Elf32_Ehdr *ehdr;
	Elf32_Shdr *shdr;
	unsigned char *strtab = 0;
	int i;

	ehdr = (Elf32_Ehdr *) addr;

	shdr = (Elf32_Shdr *) (addr + ehdr->e_shoff + (ehdr->e_shstrndx * sizeof (Elf32_Shdr)));

	if (shdr->sh_type == SHT_STRTAB)
		strtab = (unsigned char *) (addr + shdr->sh_offset);

	for (i = 0; i < ehdr->e_shnum; ++i)
	{
		shdr = (Elf32_Shdr *) (addr + ehdr->e_shoff +
				(i * sizeof (Elf32_Shdr)));

		if (!(shdr->sh_flags & SHF_ALLOC) || shdr->sh_size == 0)
			continue;

		elf_int2hex(shdr->sh_addr,8,s);
		elf_puts(s);
		elf_putch(' ');
		elf_int2hex(shdr->sh_size,8,s);
		elf_puts(s);
		elf_putch(' ');
		elf_puts(&strtab[shdr->sh_name]);
		elf_putch('\r');
		elf_putch('\n');

		void *target = (void*)(((unsigned long)0x80000000) | shdr->sh_addr);

		if (shdr->sh_type == SHT_NOBITS) {
			elf_memset (target, 0, shdr->sh_size);
		} else {
			elf_memcpy ((void *) target,
				(unsigned char *) addr + shdr->sh_offset,
				shdr->sh_size);
		}
		elf_sync_before_exec (target, shdr->sh_size);
	}
	elf_putch('\r');
	elf_putch('\n');
	elf_putch('E');
	elf_putch('P');
	elf_putch(' ');
	elf_int2hex(ehdr->e_entry,8,s);
	elf_puts(s);
	elf_putch('\r');
	elf_putch('\n');

	*(volatile unsigned long *)ELF_GET_RELOCATED(&elf_secondary_hold_addr) = ehdr->e_entry + 0x60;
	
	// call elf_run()
	void(*call)(unsigned long,unsigned long) = ELF_GET_RELOCATED(elf_run);
	call(ehdr->e_entry,((unsigned long)ELF_DEVTREE_START)&0x7fffffff);
	
	for(;;);
}

void elf_runFromMemory (void *addr, int size)
{
	int i;
	
	// some drivers require a shutdown
	
	enet_quiesce();
	usb_shutdown();
	
	// relocate code
	memcpy(ELF_CODE_RELOC_START,elfldr_start,elfldr_end-elfldr_start); 
	memicbi(ELF_CODE_RELOC_START,elfldr_end-elfldr_start);

	// relocate elf data
	memcpy(ELF_DATA_RELOC_START,addr,size);

	// get all threads to be on hold in the relocated zone
	xenon_thread_startup();
	for (i = 1; i < 6; ++i)
		while (xenon_run_thread_task(i, NULL, ELF_GET_RELOCATED(elf_hold_thread)));
		
	mdelay(200); // TODO: remove this and detect that all threads are on hold

	// call elf_prepare_run()
	void(*call)(void*) = ELF_GET_RELOCATED(elf_prepare_run);
	call(ELF_DATA_RELOC_START);
	
	// never called, make sure the function survives linking
	elf_prepare_run(NULL);
}

int elf_runFromDisk (char *filename)
{
	int f = open(filename, O_RDONLY);
	if (f < 0)
	{
		return f;
	}

	struct stat s;
	fstat(f, &s);

	int size = s.st_size;
	void * buf=malloc(size);

	int r = read(f, buf, size);
	if (r < 0)
	{
		close(f);
		return r;
	}

	elf_runFromMemory(buf, r);

	return 0;
}

void elf_runWithDeviceTree (void *elf_addr, int elf_size, void *dt_addr, int dt_size)
{
	if (dt_size>ELF_DEVTREE_MAX_SIZE){
		printf("[ELF loader] Device tree too big (> %d bytes) !\n",ELF_DEVTREE_MAX_SIZE);
		return;
	}		
	memset(ELF_DEVTREE_START,0,ELF_DEVTREE_MAX_SIZE);
	memcpy(ELF_DEVTREE_START,dt_addr,dt_size);
	memdcbst(ELF_DEVTREE_START,ELF_DEVTREE_MAX_SIZE);
	
	elf_runFromMemory(elf_addr,elf_size);
}