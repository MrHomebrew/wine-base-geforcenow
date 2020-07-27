/*
 * i386 signal handling routines
 *
 * Copyright 1999 Alexandre Julliard
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

#if 0
#pragma makedep unix
#endif

#ifdef __i386__

#include "config.h"
#include "wine/port.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <stdarg.h>
#include <stdio.h>
#include <sys/types.h>
#ifdef HAVE_UNISTD_H
# include <unistd.h>
#endif
#ifdef HAVE_SYS_PARAM_H
# include <sys/param.h>
#endif
#ifdef HAVE_SYSCALL_H
# include <syscall.h>
#else
# ifdef HAVE_SYS_SYSCALL_H
#  include <sys/syscall.h>
# endif
#endif
#ifdef HAVE_SYS_SIGNAL_H
# include <sys/signal.h>
#endif
#ifdef HAVE_SYS_UCONTEXT_H
# include <sys/ucontext.h>
#endif

#include "ntstatus.h"
#define WIN32_NO_STATUS
#include "windef.h"
#include "wine/asm.h"
#include "wine/exception.h"
#include "unix_private.h"
#include "wine/debug.h"


/***********************************************************************
 * signal context platform-specific definitions
 */

#ifdef __linux__

struct modify_ldt_s
{
    unsigned int  entry_number;
    void         *base_addr;
    unsigned int  limit;
    unsigned int  seg_32bit : 1;
    unsigned int  contents : 2;
    unsigned int  read_exec_only : 1;
    unsigned int  limit_in_pages : 1;
    unsigned int  seg_not_present : 1;
    unsigned int  usable : 1;
    unsigned int  garbage : 25;
};

static inline int modify_ldt( int func, struct modify_ldt_s *ptr, unsigned long count )
{
    return syscall( 123 /* SYS_modify_ldt */, func, ptr, count );
}

static inline int set_thread_area( struct modify_ldt_s *ptr )
{
    return syscall( 243 /* SYS_set_thread_area */, ptr );
}

#elif defined (__BSDI__)

#include <machine/frame.h>

#elif defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__DragonFly__)

#include <machine/trap.h>
#include <machine/segments.h>
#include <machine/sysarch.h>

#elif defined (__OpenBSD__)

#include <machine/segments.h>
#include <machine/sysarch.h>

#elif defined(__svr4__) || defined(_SCO_DS) || defined(__sun)

#if defined(_SCO_DS) || defined(__sun)
#include <sys/regset.h>
#endif

#elif defined (__APPLE__)

#include <i386/user_ldt.h>

#elif defined(__NetBSD__)

#include <machine/segments.h>
#include <machine/sysarch.h>

#elif defined(__GNU__)

#include <mach/i386/mach_i386.h>
#include <mach/mach_traps.h>

#else
#error You must define the signal context functions for your platform
#endif /* linux */


static const size_t teb_size = 4096;  /* we reserve one page for the TEB */
static ULONG first_ldt_entry = 32;

struct x86_thread_data
{
    DWORD              fs;            /* 1d4 TEB selector */
    DWORD              gs;            /* 1d8 libc selector; update winebuild if you move this! */
    DWORD              dr0;           /* 1dc debug registers */
    DWORD              dr1;           /* 1e0 */
    DWORD              dr2;           /* 1e4 */
    DWORD              dr3;           /* 1e8 */
    DWORD              dr6;           /* 1ec */
    DWORD              dr7;           /* 1f0 */
    void              *exit_frame;    /* 1f4 exit frame pointer */
    /* the ntdll_thread_data structure follows here */
};

C_ASSERT( offsetof( TEB, SystemReserved2 ) + offsetof( struct x86_thread_data, gs ) == 0x1d8 );
C_ASSERT( offsetof( TEB, SystemReserved2 ) + offsetof( struct x86_thread_data, exit_frame ) == 0x1f4 );

static inline WORD get_cs(void) { WORD res; __asm__( "movw %%cs,%0" : "=r" (res) ); return res; }
static inline WORD get_ds(void) { WORD res; __asm__( "movw %%ds,%0" : "=r" (res) ); return res; }
static inline WORD get_fs(void) { WORD res; __asm__( "movw %%fs,%0" : "=r" (res) ); return res; }
static inline WORD get_gs(void) { WORD res; __asm__( "movw %%gs,%0" : "=r" (res) ); return res; }
static inline void set_fs( WORD val ) { __asm__( "mov %0,%%fs" :: "r" (val)); }


/***********************************************************************
 *           is_gdt_sel
 */
static inline int is_gdt_sel( WORD sel )
{
    return !(sel & 4);
}


/***********************************************************************
 *           context_to_server
 *
 * Convert a register context to the server format.
 */
NTSTATUS context_to_server( context_t *to, const CONTEXT *from )
{
    DWORD flags = from->ContextFlags & ~CONTEXT_i386;  /* get rid of CPU id */

    memset( to, 0, sizeof(*to) );
    to->cpu = CPU_x86;

    if (flags & CONTEXT_CONTROL)
    {
        to->flags |= SERVER_CTX_CONTROL;
        to->ctl.i386_regs.ebp    = from->Ebp;
        to->ctl.i386_regs.esp    = from->Esp;
        to->ctl.i386_regs.eip    = from->Eip;
        to->ctl.i386_regs.cs     = from->SegCs;
        to->ctl.i386_regs.ss     = from->SegSs;
        to->ctl.i386_regs.eflags = from->EFlags;
    }
    if (flags & CONTEXT_INTEGER)
    {
        to->flags |= SERVER_CTX_INTEGER;
        to->integer.i386_regs.eax = from->Eax;
        to->integer.i386_regs.ebx = from->Ebx;
        to->integer.i386_regs.ecx = from->Ecx;
        to->integer.i386_regs.edx = from->Edx;
        to->integer.i386_regs.esi = from->Esi;
        to->integer.i386_regs.edi = from->Edi;
    }
    if (flags & CONTEXT_SEGMENTS)
    {
        to->flags |= SERVER_CTX_SEGMENTS;
        to->seg.i386_regs.ds = from->SegDs;
        to->seg.i386_regs.es = from->SegEs;
        to->seg.i386_regs.fs = from->SegFs;
        to->seg.i386_regs.gs = from->SegGs;
    }
    if (flags & CONTEXT_FLOATING_POINT)
    {
        to->flags |= SERVER_CTX_FLOATING_POINT;
        to->fp.i386_regs.ctrl     = from->FloatSave.ControlWord;
        to->fp.i386_regs.status   = from->FloatSave.StatusWord;
        to->fp.i386_regs.tag      = from->FloatSave.TagWord;
        to->fp.i386_regs.err_off  = from->FloatSave.ErrorOffset;
        to->fp.i386_regs.err_sel  = from->FloatSave.ErrorSelector;
        to->fp.i386_regs.data_off = from->FloatSave.DataOffset;
        to->fp.i386_regs.data_sel = from->FloatSave.DataSelector;
        to->fp.i386_regs.cr0npx   = from->FloatSave.Cr0NpxState;
        memcpy( to->fp.i386_regs.regs, from->FloatSave.RegisterArea, sizeof(to->fp.i386_regs.regs) );
    }
    if (flags & CONTEXT_DEBUG_REGISTERS)
    {
        to->flags |= SERVER_CTX_DEBUG_REGISTERS;
        to->debug.i386_regs.dr0 = from->Dr0;
        to->debug.i386_regs.dr1 = from->Dr1;
        to->debug.i386_regs.dr2 = from->Dr2;
        to->debug.i386_regs.dr3 = from->Dr3;
        to->debug.i386_regs.dr6 = from->Dr6;
        to->debug.i386_regs.dr7 = from->Dr7;
    }
    if (flags & CONTEXT_EXTENDED_REGISTERS)
    {
        to->flags |= SERVER_CTX_EXTENDED_REGISTERS;
        memcpy( to->ext.i386_regs, from->ExtendedRegisters, sizeof(to->ext.i386_regs) );
    }
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           context_from_server
 *
 * Convert a register context from the server format.
 */
NTSTATUS context_from_server( CONTEXT *to, const context_t *from )
{
    if (from->cpu != CPU_x86) return STATUS_INVALID_PARAMETER;

    to->ContextFlags = CONTEXT_i386;
    if (from->flags & SERVER_CTX_CONTROL)
    {
        to->ContextFlags |= CONTEXT_CONTROL;
        to->Ebp    = from->ctl.i386_regs.ebp;
        to->Esp    = from->ctl.i386_regs.esp;
        to->Eip    = from->ctl.i386_regs.eip;
        to->SegCs  = from->ctl.i386_regs.cs;
        to->SegSs  = from->ctl.i386_regs.ss;
        to->EFlags = from->ctl.i386_regs.eflags;
    }
    if (from->flags & SERVER_CTX_INTEGER)
    {
        to->ContextFlags |= CONTEXT_INTEGER;
        to->Eax = from->integer.i386_regs.eax;
        to->Ebx = from->integer.i386_regs.ebx;
        to->Ecx = from->integer.i386_regs.ecx;
        to->Edx = from->integer.i386_regs.edx;
        to->Esi = from->integer.i386_regs.esi;
        to->Edi = from->integer.i386_regs.edi;
    }
    if (from->flags & SERVER_CTX_SEGMENTS)
    {
        to->ContextFlags |= CONTEXT_SEGMENTS;
        to->SegDs = from->seg.i386_regs.ds;
        to->SegEs = from->seg.i386_regs.es;
        to->SegFs = from->seg.i386_regs.fs;
        to->SegGs = from->seg.i386_regs.gs;
    }
    if (from->flags & SERVER_CTX_FLOATING_POINT)
    {
        to->ContextFlags |= CONTEXT_FLOATING_POINT;
        to->FloatSave.ControlWord   = from->fp.i386_regs.ctrl;
        to->FloatSave.StatusWord    = from->fp.i386_regs.status;
        to->FloatSave.TagWord       = from->fp.i386_regs.tag;
        to->FloatSave.ErrorOffset   = from->fp.i386_regs.err_off;
        to->FloatSave.ErrorSelector = from->fp.i386_regs.err_sel;
        to->FloatSave.DataOffset    = from->fp.i386_regs.data_off;
        to->FloatSave.DataSelector  = from->fp.i386_regs.data_sel;
        to->FloatSave.Cr0NpxState   = from->fp.i386_regs.cr0npx;
        memcpy( to->FloatSave.RegisterArea, from->fp.i386_regs.regs, sizeof(to->FloatSave.RegisterArea) );
    }
    if (from->flags & SERVER_CTX_DEBUG_REGISTERS)
    {
        to->ContextFlags |= CONTEXT_DEBUG_REGISTERS;
        to->Dr0 = from->debug.i386_regs.dr0;
        to->Dr1 = from->debug.i386_regs.dr1;
        to->Dr2 = from->debug.i386_regs.dr2;
        to->Dr3 = from->debug.i386_regs.dr3;
        to->Dr6 = from->debug.i386_regs.dr6;
        to->Dr7 = from->debug.i386_regs.dr7;
    }
    if (from->flags & SERVER_CTX_EXTENDED_REGISTERS)
    {
        to->ContextFlags |= CONTEXT_EXTENDED_REGISTERS;
        memcpy( to->ExtendedRegisters, from->ext.i386_regs, sizeof(to->ExtendedRegisters) );
    }
    return STATUS_SUCCESS;
}


/***********************************************************************
 *           LDT support
 */

#define LDT_SIZE 8192

#define LDT_FLAGS_DATA      0x13  /* Data segment */
#define LDT_FLAGS_CODE      0x1b  /* Code segment */
#define LDT_FLAGS_32BIT     0x40  /* Segment is 32-bit (code or stack) */
#define LDT_FLAGS_ALLOCATED 0x80  /* Segment is allocated */

struct ldt_copy
{
    void         *base[LDT_SIZE];
    unsigned int  limit[LDT_SIZE];
    unsigned char flags[LDT_SIZE];
} __wine_ldt_copy;

static WORD gdt_fs_sel;

static RTL_CRITICAL_SECTION ldt_section;
static RTL_CRITICAL_SECTION_DEBUG critsect_debug =
{
    0, 0, &ldt_section,
    { &critsect_debug.ProcessLocksList, &critsect_debug.ProcessLocksList },
      0, 0, { (DWORD_PTR)(__FILE__ ": ldt_section") }
};
static RTL_CRITICAL_SECTION ldt_section = { &critsect_debug, -1, 0, 0, 0, 0 };

static const LDT_ENTRY null_entry;

static inline void *ldt_get_base( LDT_ENTRY ent )
{
    return (void *)(ent.BaseLow |
                    (ULONG_PTR)ent.HighWord.Bits.BaseMid << 16 |
                    (ULONG_PTR)ent.HighWord.Bits.BaseHi << 24);
}

static inline unsigned int ldt_get_limit( LDT_ENTRY ent )
{
    unsigned int limit = ent.LimitLow | (ent.HighWord.Bits.LimitHi << 16);
    if (ent.HighWord.Bits.Granularity) limit = (limit << 12) | 0xfff;
    return limit;
}

static LDT_ENTRY ldt_make_entry( void *base, unsigned int limit, unsigned char flags )
{
    LDT_ENTRY entry;

    entry.BaseLow                   = (WORD)(ULONG_PTR)base;
    entry.HighWord.Bits.BaseMid     = (BYTE)((ULONG_PTR)base >> 16);
    entry.HighWord.Bits.BaseHi      = (BYTE)((ULONG_PTR)base >> 24);
    if ((entry.HighWord.Bits.Granularity = (limit >= 0x100000))) limit >>= 12;
    entry.LimitLow                  = (WORD)limit;
    entry.HighWord.Bits.LimitHi     = limit >> 16;
    entry.HighWord.Bits.Dpl         = 3;
    entry.HighWord.Bits.Pres        = 1;
    entry.HighWord.Bits.Type        = flags;
    entry.HighWord.Bits.Sys         = 0;
    entry.HighWord.Bits.Reserved_0  = 0;
    entry.HighWord.Bits.Default_Big = (flags & LDT_FLAGS_32BIT) != 0;
    return entry;
}

static void ldt_set_entry( WORD sel, LDT_ENTRY entry )
{
    int index = sel >> 3;

#ifdef linux
    struct modify_ldt_s ldt_info = { index };

    ldt_info.base_addr       = ldt_get_base( entry );
    ldt_info.limit           = entry.LimitLow | (entry.HighWord.Bits.LimitHi << 16);
    ldt_info.seg_32bit       = entry.HighWord.Bits.Default_Big;
    ldt_info.contents        = (entry.HighWord.Bits.Type >> 2) & 3;
    ldt_info.read_exec_only  = !(entry.HighWord.Bits.Type & 2);
    ldt_info.limit_in_pages  = entry.HighWord.Bits.Granularity;
    ldt_info.seg_not_present = !entry.HighWord.Bits.Pres;
    ldt_info.usable          = entry.HighWord.Bits.Sys;
    if (modify_ldt( 0x11, &ldt_info, sizeof(ldt_info) ) < 0) perror( "modify_ldt" );
#elif defined(__NetBSD__) || defined(__FreeBSD__) || defined(__FreeBSD_kernel__) || defined(__OpenBSD__) || defined(__DragonFly__)
    /* The kernel will only let us set LDTs with user priority level */
    if (entry.HighWord.Bits.Pres && entry.HighWord.Bits.Dpl != 3) entry.HighWord.Bits.Dpl = 3;
    if (i386_set_ldt(index, (union descriptor *)&entry, 1) < 0)
    {
        perror("i386_set_ldt");
        fprintf( stderr, "Did you reconfigure the kernel with \"options USER_LDT\"?\n" );
        exit(1);
    }
#elif defined(__svr4__) || defined(_SCO_DS)
    struct ssd ldt_mod;

    ldt_mod.sel  = sel;
    ldt_mod.bo   = (unsigned long)ldt_get_base( entry );
    ldt_mod.ls   = entry.LimitLow | (entry.HighWord.Bits.LimitHi << 16);
    ldt_mod.acc1 = entry.HighWord.Bytes.Flags1;
    ldt_mod.acc2 = entry.HighWord.Bytes.Flags2 >> 4;
    if (sysi86(SI86DSCR, &ldt_mod) == -1) perror("sysi86");
#elif defined(__APPLE__)
    if (i386_set_ldt(index, (union ldt_entry *)&entry, 1) < 0) perror("i386_set_ldt");
#elif defined(__GNU__)
    if (i386_set_ldt(mach_thread_self(), sel, (descriptor_list_t)&entry, 1) != KERN_SUCCESS)
        perror("i386_set_ldt");
#else
    fprintf( stderr, "No LDT support on this platform\n" );
    exit(1);
#endif

    __wine_ldt_copy.base[index]  = ldt_get_base( entry );
    __wine_ldt_copy.limit[index] = ldt_get_limit( entry );
    __wine_ldt_copy.flags[index] = (entry.HighWord.Bits.Type |
                                    (entry.HighWord.Bits.Default_Big ? LDT_FLAGS_32BIT : 0) |
                                    LDT_FLAGS_ALLOCATED);
}

static void ldt_set_fs( WORD sel, TEB *teb )
{
    if (sel == gdt_fs_sel)
    {
#ifdef __linux__
        struct modify_ldt_s ldt_info = { sel >> 3 };

        ldt_info.base_addr = teb;
        ldt_info.limit     = teb_size - 1;
        ldt_info.seg_32bit = 1;
        if (set_thread_area( &ldt_info ) < 0) perror( "set_thread_area" );
#elif defined(__FreeBSD__) || defined (__FreeBSD_kernel__) || defined(__DragonFly__)
        i386_set_fsbase( teb );
#endif
    }
    set_fs( sel );
}


/**********************************************************************
 *           get_thread_ldt_entry
 */
NTSTATUS CDECL get_thread_ldt_entry( HANDLE handle, void *data, ULONG len, ULONG *ret_len )
{
    THREAD_DESCRIPTOR_INFORMATION *info = data;
    NTSTATUS status = STATUS_SUCCESS;

    if (len < sizeof(*info)) return STATUS_INFO_LENGTH_MISMATCH;
    if (info->Selector >> 16) return STATUS_UNSUCCESSFUL;

    if (is_gdt_sel( info->Selector ))
    {
        if (!(info->Selector & ~3))
            info->Entry = null_entry;
        else if ((info->Selector | 3) == get_cs())
            info->Entry = ldt_make_entry( 0, ~0u, LDT_FLAGS_CODE | LDT_FLAGS_32BIT );
        else if ((info->Selector | 3) == get_ds())
            info->Entry = ldt_make_entry( 0, ~0u, LDT_FLAGS_DATA | LDT_FLAGS_32BIT );
        else if ((info->Selector | 3) == get_fs())
            info->Entry = ldt_make_entry( NtCurrentTeb(), 0xfff, LDT_FLAGS_DATA | LDT_FLAGS_32BIT );
        else
            return STATUS_UNSUCCESSFUL;
    }
    else
    {
        SERVER_START_REQ( get_selector_entry )
        {
            req->handle = wine_server_obj_handle( handle );
            req->entry = info->Selector >> 3;
            status = wine_server_call( req );
            if (!status)
            {
                if (reply->flags)
                    info->Entry = ldt_make_entry( (void *)reply->base, reply->limit, reply->flags );
                else
                    status = STATUS_ACCESS_VIOLATION;
            }
        }
        SERVER_END_REQ;
    }
    if (status == STATUS_SUCCESS && ret_len)
        /* yes, that's a bit strange, but it's the way it is */
        *ret_len = sizeof(info->Entry);

    return status;
}


/******************************************************************************
 *           NtSetLdtEntries   (NTDLL.@)
 *           ZwSetLdtEntries   (NTDLL.@)
 */
NTSTATUS WINAPI NtSetLdtEntries( ULONG sel1, LDT_ENTRY entry1, ULONG sel2, LDT_ENTRY entry2 )
{
    sigset_t sigset;

    if (sel1 >> 16 || sel2 >> 16) return STATUS_INVALID_LDT_DESCRIPTOR;

    server_enter_uninterrupted_section( &ldt_section, &sigset );
    if (sel1) ldt_set_entry( sel1, entry1 );
    if (sel2) ldt_set_entry( sel2, entry2 );
    server_leave_uninterrupted_section( &ldt_section, &sigset );
   return STATUS_SUCCESS;
}


/**********************************************************************
 *             signal_init_threading
 */
void signal_init_threading(void)
{
#ifdef __linux__
    /* the preloader may have allocated it already */
    gdt_fs_sel = get_fs();
    if (!gdt_fs_sel || !is_gdt_sel( gdt_fs_sel ))
    {
        struct modify_ldt_s ldt_info = { -1 };

        ldt_info.seg_32bit = 1;
        ldt_info.usable = 1;
        if (set_thread_area( &ldt_info ) >= 0) gdt_fs_sel = (ldt_info.entry_number << 3) | 3;
        else gdt_fs_sel = 0;
    }
#elif defined(__FreeBSD__) || defined (__FreeBSD_kernel__)
    gdt_fs_sel = GSEL( GUFS_SEL, SEL_UPL );
#endif
}


/**********************************************************************
 *		signal_alloc_thread
 */
NTSTATUS signal_alloc_thread( TEB *teb )
{
    struct x86_thread_data *thread_data = (struct x86_thread_data *)teb->SystemReserved2;

    if (!gdt_fs_sel)
    {
        static int first_thread = 1;
        sigset_t sigset;
        int idx;
        LDT_ENTRY entry = ldt_make_entry( teb, teb_size - 1, LDT_FLAGS_DATA | LDT_FLAGS_32BIT );

        if (first_thread)  /* no locking for first thread */
        {
            /* leave some space if libc is using the LDT for %gs */
            if (!is_gdt_sel( get_gs() )) first_ldt_entry = 512;
            idx = first_ldt_entry;
            ldt_set_entry( (idx << 3) | 7, entry );
            first_thread = 0;
        }
        else
        {
            server_enter_uninterrupted_section( &ldt_section, &sigset );
            for (idx = first_ldt_entry; idx < LDT_SIZE; idx++)
            {
                if (__wine_ldt_copy.flags[idx]) continue;
                ldt_set_entry( (idx << 3) | 7, entry );
                break;
            }
            server_leave_uninterrupted_section( &ldt_section, &sigset );
            if (idx == LDT_SIZE) return STATUS_TOO_MANY_THREADS;
        }
        thread_data->fs = (idx << 3) | 7;
    }
    else thread_data->fs = gdt_fs_sel;

    return STATUS_SUCCESS;
}


/**********************************************************************
 *		signal_free_thread
 */
void signal_free_thread( TEB *teb )
{
    struct x86_thread_data *thread_data = (struct x86_thread_data *)teb->SystemReserved2;
    sigset_t sigset;

    if (gdt_fs_sel) return;

    server_enter_uninterrupted_section( &ldt_section, &sigset );
    __wine_ldt_copy.flags[thread_data->fs >> 3] = 0;
    server_leave_uninterrupted_section( &ldt_section, &sigset );
}


/**********************************************************************
 *		signal_init_thread
 */
void signal_init_thread( TEB *teb )
{
    const WORD fpu_cw = 0x27f;
    struct x86_thread_data *thread_data = (struct x86_thread_data *)teb->SystemReserved2;
    stack_t ss;

    ss.ss_sp    = (char *)teb + teb_size;
    ss.ss_size  = signal_stack_size;
    ss.ss_flags = 0;
    if (sigaltstack(&ss, NULL) == -1) perror( "sigaltstack" );

    ldt_set_fs( thread_data->fs, teb );
    thread_data->gs = get_gs();

#ifdef __GNUC__
    __asm__ volatile ("fninit; fldcw %0" : : "m" (fpu_cw));
#else
    FIXME("FPU setup not implemented for this platform.\n");
#endif
}


/***********************************************************************
 *           signal_exit_thread
 */
__ASM_GLOBAL_FUNC( signal_exit_thread,
                   "movl 8(%esp),%ecx\n\t"
                   /* fetch exit frame */
                   "movl %fs:0x1f4,%edx\n\t"    /* x86_thread_data()->exit_frame */
                   "testl %edx,%edx\n\t"
                   "jnz 1f\n\t"
                   "jmp *%ecx\n\t"
                   /* switch to exit frame stack */
                   "1:\tmovl 4(%esp),%eax\n\t"
                   "movl $0,%fs:0x1f4\n\t"
                   "movl %edx,%ebp\n\t"
                   __ASM_CFI(".cfi_def_cfa %ebp,4\n\t")
                   __ASM_CFI(".cfi_rel_offset %ebp,0\n\t")
                   __ASM_CFI(".cfi_rel_offset %ebx,-4\n\t")
                   __ASM_CFI(".cfi_rel_offset %esi,-8\n\t")
                   __ASM_CFI(".cfi_rel_offset %edi,-12\n\t")
                   "leal -20(%ebp),%esp\n\t"
                   "pushl %eax\n\t"
                   "call *%ecx" )

/**********************************************************************
 *           NtCurrentTeb   (NTDLL.@)
 */
__ASM_STDCALL_FUNC( NtCurrentTeb, 0, ".byte 0x64\n\tmovl 0x18,%eax\n\tret" )

#endif  /* __i386__ */
