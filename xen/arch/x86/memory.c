/******************************************************************************
 * arch/x86/memory.c
 * 
 * Copyright (c) 2002-2004 K A Fraser
 * Copyright (c) 2004 Christian Limpach
 * 
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/*
 * A description of the x86 page table API:
 * 
 * Domains trap to do_mmu_update with a list of update requests.
 * This is a list of (ptr, val) pairs, where the requested operation
 * is *ptr = val.
 * 
 * Reference counting of pages:
 * ----------------------------
 * Each page has two refcounts: tot_count and type_count.
 * 
 * TOT_COUNT is the obvious reference count. It counts all uses of a
 * physical page frame by a domain, including uses as a page directory,
 * a page table, or simple mappings via a PTE. This count prevents a
 * domain from releasing a frame back to the free pool when it still holds
 * a reference to it.
 * 
 * TYPE_COUNT is more subtle. A frame can be put to one of three
 * mutually-exclusive uses: it might be used as a page directory, or a
 * page table, or it may be mapped writable by the domain [of course, a
 * frame may not be used in any of these three ways!].
 * So, type_count is a count of the number of times a frame is being 
 * referred to in its current incarnation. Therefore, a page can only
 * change its type when its type count is zero.
 * 
 * Pinning the page type:
 * ----------------------
 * The type of a page can be pinned/unpinned with the commands
 * MMUEXT_[UN]PIN_L?_TABLE. Each page can be pinned exactly once (that is,
 * pinning is not reference counted, so it can't be nested).
 * This is useful to prevent a page's type count falling to zero, at which
 * point safety checks would need to be carried out next time the count
 * is increased again.
 * 
 * A further note on writable page mappings:
 * -----------------------------------------
 * For simplicity, the count of writable mappings for a page may not
 * correspond to reality. The 'writable count' is incremented for every
 * PTE which maps the page with the _PAGE_RW flag set. However, for
 * write access to be possible the page directory entry must also have
 * its _PAGE_RW bit set. We do not check this as it complicates the 
 * reference counting considerably [consider the case of multiple
 * directory entries referencing a single page table, some with the RW
 * bit set, others not -- it starts getting a bit messy].
 * In normal use, this simplification shouldn't be a problem.
 * However, the logic can be added if required.
 * 
 * One more note on read-only page mappings:
 * -----------------------------------------
 * We want domains to be able to map pages for read-only access. The
 * main reason is that page tables and directories should be readable
 * by a domain, but it would not be safe for them to be writable.
 * However, domains have free access to rings 1 & 2 of the Intel
 * privilege model. In terms of page protection, these are considered
 * to be part of 'supervisor mode'. The WP bit in CR0 controls whether
 * read-only restrictions are respected in supervisor mode -- if the 
 * bit is clear then any mapped page is writable.
 * 
 * We get round this by always setting the WP bit and disallowing 
 * updates to it. This is very unlikely to cause a problem for guest
 * OS's, which will generally use the WP bit to simplify copy-on-write
 * implementation (in that case, OS wants a fault when it writes to
 * an application-supplied buffer).
 */

#include <xen/config.h>
#include <xen/init.h>
#include <xen/lib.h>
#include <xen/mm.h>
#include <xen/sched.h>
#include <xen/errno.h>
#include <xen/perfc.h>
#include <xen/irq.h>
#include <asm/shadow.h>
#include <asm/page.h>
#include <asm/flushtlb.h>
#include <asm/io.h>
#include <asm/uaccess.h>
#include <asm/domain_page.h>
#include <asm/ldt.h>

#ifdef VERBOSE
#define MEM_LOG(_f, _a...)                           \
  printk("DOM%u: (file=memory.c, line=%d) " _f "\n", \
         current->domain , __LINE__ , ## _a )
#else
#define MEM_LOG(_f, _a...) ((void)0)
#endif

static int alloc_l2_table(struct pfn_info *page);
static int alloc_l1_table(struct pfn_info *page);
static int get_page_from_pagenr(unsigned long page_nr, struct domain *d);
static int get_page_and_type_from_pagenr(unsigned long page_nr, 
                                         u32 type,
                                         struct domain *d);

static void free_l2_table(struct pfn_info *page);
static void free_l1_table(struct pfn_info *page);

static int mod_l2_entry(l2_pgentry_t *, l2_pgentry_t, unsigned long);
static int mod_l1_entry(l1_pgentry_t *, l1_pgentry_t);

/* Used to defer flushing of memory structures. */
static struct {
#define DOP_FLUSH_TLB   (1<<0) /* Flush the TLB.                 */
#define DOP_RELOAD_LDT  (1<<1) /* Reload the LDT shadow mapping. */
    unsigned long       deferred_ops;
    unsigned long       cr0;
    /* If non-NULL, specifies a foreign subject domain for some operations. */
    struct domain      *foreign;
} percpu_info[NR_CPUS] __cacheline_aligned;

/*
 * Returns the current foreign domain; defaults to the currently-executing
 * domain if a foreign override hasn't been specified.
 */
#define FOREIGNDOM (percpu_info[smp_processor_id()].foreign ? : current)

/* Private domain structs for DOMID_XEN and DOMID_IO. */
static struct domain *dom_xen, *dom_io;

void arch_init_memory(void)
{
    unsigned long mfn;

    /*
     * We are rather picky about the layout of 'struct pfn_info'. The
     * count_info and domain fields must be adjacent, as we perform atomic
     * 64-bit operations on them. Also, just for sanity, we assert the size
     * of the structure here.
     */
    if ( (offsetof(struct pfn_info, u.inuse.domain) != 
          (offsetof(struct pfn_info, count_info) + sizeof(u32))) ||
         (sizeof(struct pfn_info) != 24) )
    {
        printk("Weird pfn_info layout (%ld,%ld,%d)\n",
               offsetof(struct pfn_info, count_info),
               offsetof(struct pfn_info, u.inuse.domain),
               sizeof(struct pfn_info));
        for ( ; ; ) ;
    }

    memset(percpu_info, 0, sizeof(percpu_info));

/* XXXX WRITEABLE PAGETABLES SHOULD BE A DOMAIN CREATION-TIME
   DECISION, NOT SOMETHING THAT IS CHANGED ON A RUNNING DOMAIN 
   !!! FIX ME !!!! 
 */

    vm_assist_info[VMASST_TYPE_writable_pagetables].enable =
        NULL;
    vm_assist_info[VMASST_TYPE_writable_pagetables].disable =
        NULL;

    for ( mfn = 0; mfn < max_page; mfn++ )
        frame_table[mfn].count_info |= PGC_always_set;

    /* Initialise to a magic of 0x55555555 so easier to spot bugs later. */
    memset(machine_to_phys_mapping, 0x55, 4<<20);

    /*
     * Initialise our DOMID_XEN domain.
     * Any Xen-heap pages that we will allow to be mapped will have
     * their domain field set to dom_xen.
     */
    dom_xen = alloc_domain_struct();
    atomic_set(&dom_xen->refcnt, 1);
    dom_xen->domain = DOMID_XEN;

    /*
     * Initialise our DOMID_IO domain.
     * This domain owns no pages but is considered a special case when
     * mapping I/O pages, as the mappings occur at the priv of the caller.
     */
    dom_io = alloc_domain_struct();
    atomic_set(&dom_io->refcnt, 1);
    dom_io->domain = DOMID_IO;

    /* M2P table is mappable read-only by privileged domains. */
    for ( mfn = virt_to_phys(&machine_to_phys_mapping[0<<20])>>PAGE_SHIFT;
          mfn < virt_to_phys(&machine_to_phys_mapping[1<<20])>>PAGE_SHIFT;
          mfn++ )
    {
        frame_table[mfn].count_info        |= PGC_allocated | 1;
        frame_table[mfn].u.inuse.type_info  = PGT_gdt_page | 1; /* non-RW */
        frame_table[mfn].u.inuse.domain     = dom_xen;
    }
}

static void __invalidate_shadow_ldt(struct domain *d)
{
    int i;
    unsigned long pfn;
    struct pfn_info *page;
    
    d->mm.shadow_ldt_mapcnt = 0;

    for ( i = 16; i < 32; i++ )
    {
        pfn = l1_pgentry_to_pagenr(d->mm.perdomain_pt[i]);
        if ( pfn == 0 ) continue;
        d->mm.perdomain_pt[i] = mk_l1_pgentry(0);
        page = &frame_table[pfn];
        ASSERT_PAGE_IS_TYPE(page, PGT_ldt_page);
        ASSERT_PAGE_IS_DOMAIN(page, d);
        put_page_and_type(page);
    }

    /* Dispose of the (now possibly invalid) mappings from the TLB.  */
    percpu_info[d->processor].deferred_ops |= DOP_FLUSH_TLB | DOP_RELOAD_LDT;
}


static inline void invalidate_shadow_ldt(struct domain *d)
{
    if ( d->mm.shadow_ldt_mapcnt != 0 )
        __invalidate_shadow_ldt(d);
}


static int alloc_segdesc_page(struct pfn_info *page)
{
    unsigned long *descs = map_domain_mem((page-frame_table) << PAGE_SHIFT);
    int i;

    for ( i = 0; i < 512; i++ )
        if ( unlikely(!check_descriptor(&descs[i*2])) )
            goto fail;

    unmap_domain_mem(descs);
    return 1;

 fail:
    unmap_domain_mem(descs);
    return 0;
}


/* Map shadow page at offset @off. */
int map_ldt_shadow_page(unsigned int off)
{
    struct domain *d = current;
    unsigned long l1e;

    if ( unlikely(in_irq()) )
        BUG();

    __get_user(l1e, (unsigned long *)&linear_pg_table[(d->mm.ldt_base >> 
                                                       PAGE_SHIFT) + off]);

    if ( unlikely(!(l1e & _PAGE_PRESENT)) ||
         unlikely(!get_page_and_type(&frame_table[l1e >> PAGE_SHIFT], 
                                     d, PGT_ldt_page)) )
        return 0;

    d->mm.perdomain_pt[off + 16] = mk_l1_pgentry(l1e | _PAGE_RW);
    d->mm.shadow_ldt_mapcnt++;

    return 1;
}


static int get_page_from_pagenr(unsigned long page_nr, struct domain *d)
{
    struct pfn_info *page = &frame_table[page_nr];

    if ( unlikely(!pfn_is_ram(page_nr)) )
    {
        MEM_LOG("Pfn %08lx is not RAM", page_nr);
        return 0;
    }

    if ( unlikely(!get_page(page, d)) )
    {
        MEM_LOG("Could not get page ref for pfn %08lx", page_nr);
        return 0;
    }

    return 1;
}


static int get_page_and_type_from_pagenr(unsigned long page_nr, 
                                         u32 type,
                                         struct domain *d)
{
    struct pfn_info *page = &frame_table[page_nr];

    if ( unlikely(!get_page_from_pagenr(page_nr, d)) )
        return 0;

    if ( unlikely(!get_page_type(page, type)) )
    {
        MEM_LOG("Bad page type for pfn %08lx (%08x)", 
                page_nr, page->u.inuse.type_info);
        put_page(page);
        return 0;
    }

    return 1;
}


/*
 * We allow an L2 tables to map each other (a.k.a. linear page tables). It
 * needs some special care with reference counst and access permissions:
 *  1. The mapping entry must be read-only, or the guest may get write access
 *     to its own PTEs.
 *  2. We must only bump the reference counts for an *already validated*
 *     L2 table, or we can end up in a deadlock in get_page_type() by waiting
 *     on a validation that is required to complete that validation.
 *  3. We only need to increment the reference counts for the mapped page
 *     frame if it is mapped by a different L2 table. This is sufficient and
 *     also necessary to allow validation of an L2 table mapping itself.
 */
static int 
get_linear_pagetable(
    l2_pgentry_t l2e, unsigned long pfn, struct domain *d)
{
    u32 x, y;
    struct pfn_info *page;

    if ( (l2_pgentry_val(l2e) & _PAGE_RW) )
    {
        MEM_LOG("Attempt to create linear p.t. with write perms");
        return 0;
    }

    if ( (l2_pgentry_val(l2e) >> PAGE_SHIFT) != pfn )
    {
        /* Make sure the mapped frame belongs to the correct domain. */
        if ( unlikely(!get_page_from_pagenr(l2_pgentry_to_pagenr(l2e), d)) )
            return 0;

        /*
         * Make sure that the mapped frame is an already-validated L2 table. 
         * If so, atomically increment the count (checking for overflow).
         */
        page = &frame_table[l2_pgentry_to_pagenr(l2e)];
        y = page->u.inuse.type_info;
        do {
            x = y;
            if ( unlikely((x & PGT_count_mask) == PGT_count_mask) ||
                 unlikely((x & (PGT_type_mask|PGT_validated)) != 
                          (PGT_l2_page_table|PGT_validated)) )
            {
                put_page(page);
                return 0;
            }
        }
        while ( (y = cmpxchg(&page->u.inuse.type_info, x, x + 1)) != x );
    }

    return 1;
}


static int
get_page_from_l1e(
    l1_pgentry_t l1e, struct domain *d)
{
    unsigned long l1v = l1_pgentry_val(l1e);
    unsigned long pfn = l1_pgentry_to_pagenr(l1e);
    struct pfn_info *page = &frame_table[pfn];
    extern int domain_iomem_in_pfn(struct domain *d, unsigned long pfn);

    if ( !(l1v & _PAGE_PRESENT) )
        return 1;

    if ( unlikely(l1v & (_PAGE_GLOBAL|_PAGE_PAT)) )
    {
        MEM_LOG("Bad L1 type settings %04lx", l1v & (_PAGE_GLOBAL|_PAGE_PAT));
        return 0;
    }

    if ( unlikely(!pfn_is_ram(pfn)) )
    {
        /* SPECIAL CASE 1. Mapping an I/O page. */

        /* Revert to caller privileges if FD == DOMID_IO. */
        if ( d == dom_io )
            d = current;

        if ( IS_PRIV(d) )
            return 1;

        if ( IS_CAPABLE_PHYSDEV(d) )
            return domain_iomem_in_pfn(d, pfn);

        MEM_LOG("Non-privileged attempt to map I/O space %08lx", pfn);
        return 0;
    }

    if ( unlikely(!get_page_from_pagenr(pfn, d)) )
    {
        /* SPECIAL CASE 2. Mapping a foreign page via a grant table. */
        
        int rc;
        struct domain *e;
        u32 count_info;
        /*
         * Yuk! Amazingly this is the simplest way to get a guaranteed atomic
         * snapshot of a 64-bit value on IA32. x86/64 solves this of course!
         * Basically it's a no-op CMPXCHG, to get us the current contents.
         * No need for LOCK prefix -- we know that count_info is never zero
         * because it contains PGC_always_set.
         */
        ASSERT(test_bit(_PGC_always_set, &page->count_info));
        __asm__ __volatile__(
            "cmpxchg8b %2"
            : "=d" (e), "=a" (count_info),
              "=m" (*(volatile u64 *)(&page->count_info))
            : "0" (0), "1" (0), "c" (0), "b" (0) );
        if ( unlikely((count_info & PGC_count_mask) == 0) ||
             unlikely(e == NULL) || unlikely(!get_domain(e)) )
             return 0;
        rc = gnttab_try_map(
            e, d, pfn, (l1v & _PAGE_RW) ? GNTTAB_MAP_RW : GNTTAB_MAP_RO);
        put_domain(e);
        return rc;
    }

    if ( l1v & _PAGE_RW )
    {
        if ( unlikely(!get_page_type(page, PGT_writable_page)) )
            return 0;
        set_bit(_PGC_tlb_flush_on_type_change, &page->count_info);
    }

    return 1;
}


/* NB. Virtual address 'l2e' maps to a machine address within frame 'pfn'. */
static int 
get_page_from_l2e(
    l2_pgentry_t l2e, unsigned long pfn,
    struct domain *d, unsigned long va_idx)
{
    int rc;

    if ( !(l2_pgentry_val(l2e) & _PAGE_PRESENT) )
        return 1;

    if ( unlikely((l2_pgentry_val(l2e) & (_PAGE_GLOBAL|_PAGE_PSE))) )
    {
        MEM_LOG("Bad L2 page type settings %04lx",
                l2_pgentry_val(l2e) & (_PAGE_GLOBAL|_PAGE_PSE));
        return 0;
    }

    rc = get_page_and_type_from_pagenr(
        l2_pgentry_to_pagenr(l2e), 
        PGT_l1_page_table | (va_idx<<PGT_va_shift), d);

    if ( unlikely(!rc) )
        return get_linear_pagetable(l2e, pfn, d);

    return 1;
}


static void put_page_from_l1e(l1_pgentry_t l1e, struct domain *d)
{
    unsigned long    l1v  = l1_pgentry_val(l1e);
    unsigned long    pfn  = l1_pgentry_to_pagenr(l1e);
    struct pfn_info *page = &frame_table[pfn];
    struct domain   *e = page->u.inuse.domain;

    if ( !(l1v & _PAGE_PRESENT) || !pfn_is_ram(pfn) )
        return;

    if ( unlikely(e != d) )
    {
        /*
         * Unmap a foreign page that may have been mapped via a grant table.
         * Note that this can fail for a privileged domain that can map foreign
         * pages via MMUEXT_SET_FOREIGNDOM. Such domains can have some mappings
         * counted via a grant entry and some counted directly in the page
         * structure's reference count. Note that reference counts won't get
         * dangerously confused as long as we always try to decrement the
         * grant entry first. We may end up with a mismatch between which
         * mappings and which unmappings are counted via the grant entry, but
         * really it doesn't matter as privileged domains have carte blanche.
         */
        if ( likely(gnttab_try_map(e, d, pfn, (l1v & _PAGE_RW) ? 
                                   GNTTAB_UNMAP_RW : GNTTAB_UNMAP_RO)) )
            return;
        /* Assume this mapping was made via MMUEXT_SET_FOREIGNDOM... */
    }

    if ( l1v & _PAGE_RW )
    {
        put_page_and_type(page);
    }
    else
    {
        /* We expect this is rare so we blow the entire shadow LDT. */
        if ( unlikely(((page->u.inuse.type_info & PGT_type_mask) == 
                       PGT_ldt_page)) &&
             unlikely(((page->u.inuse.type_info & PGT_count_mask) != 0)) )
            invalidate_shadow_ldt(e);
        put_page(page);
    }
}


/*
 * NB. Virtual address 'l2e' maps to a machine address within frame 'pfn'.
 * Note also that this automatically deals correctly with linear p.t.'s.
 */
static void put_page_from_l2e(l2_pgentry_t l2e, unsigned long pfn)
{
    if ( (l2_pgentry_val(l2e) & _PAGE_PRESENT) && 
         ((l2_pgentry_val(l2e) >> PAGE_SHIFT) != pfn) )
        put_page_and_type(&frame_table[l2_pgentry_to_pagenr(l2e)]);
}


static int alloc_l2_table(struct pfn_info *page)
{
    struct domain *d = page->u.inuse.domain;
    unsigned long  page_nr = page_to_pfn(page);
    l2_pgentry_t  *pl2e;
    int            i;
   
    pl2e = map_domain_mem(page_nr << PAGE_SHIFT);

    for ( i = 0; i < DOMAIN_ENTRIES_PER_L2_PAGETABLE; i++ ) {
        if ( unlikely(!get_page_from_l2e(pl2e[i], page_nr, d, i)) )
            goto fail;
    }
    
#if defined(__i386__)
    /* Now we add our private high mappings. */
    memcpy(&pl2e[DOMAIN_ENTRIES_PER_L2_PAGETABLE], 
           &idle_pg_table[DOMAIN_ENTRIES_PER_L2_PAGETABLE],
           HYPERVISOR_ENTRIES_PER_L2_PAGETABLE * sizeof(l2_pgentry_t));
    pl2e[LINEAR_PT_VIRT_START >> L2_PAGETABLE_SHIFT] =
        mk_l2_pgentry((page_nr << PAGE_SHIFT) | __PAGE_HYPERVISOR);
    pl2e[PERDOMAIN_VIRT_START >> L2_PAGETABLE_SHIFT] =
        mk_l2_pgentry(__pa(page->u.inuse.domain->mm.perdomain_pt) | 
                      __PAGE_HYPERVISOR);
#endif

    unmap_domain_mem(pl2e);
    return 1;

 fail:
    while ( i-- > 0 )
        put_page_from_l2e(pl2e[i], page_nr);

    unmap_domain_mem(pl2e);
    return 0;
}


static int alloc_l1_table(struct pfn_info *page)
{
    struct domain *d = page->u.inuse.domain;
    unsigned long  page_nr = page_to_pfn(page);
    l1_pgentry_t  *pl1e;
    int            i;

    pl1e = map_domain_mem(page_nr << PAGE_SHIFT);

    for ( i = 0; i < ENTRIES_PER_L1_PAGETABLE; i++ )
        if ( unlikely(!get_page_from_l1e(pl1e[i], d)) )
            goto fail;

    unmap_domain_mem(pl1e);
    return 1;

 fail:
    while ( i-- > 0 )
        put_page_from_l1e(pl1e[i], d);

    unmap_domain_mem(pl1e);
    return 0;
}


static void free_l2_table(struct pfn_info *page)
{
    unsigned long page_nr = page - frame_table;
    l2_pgentry_t *pl2e;
    int i;

    pl2e = map_domain_mem(page_nr << PAGE_SHIFT);

    for ( i = 0; i < DOMAIN_ENTRIES_PER_L2_PAGETABLE; i++ )
        put_page_from_l2e(pl2e[i], page_nr);

    unmap_domain_mem(pl2e);
}


static void free_l1_table(struct pfn_info *page)
{
    struct domain *d = page->u.inuse.domain;
    unsigned long page_nr = page - frame_table;
    l1_pgentry_t *pl1e;
    int i;

    pl1e = map_domain_mem(page_nr << PAGE_SHIFT);

    for ( i = 0; i < ENTRIES_PER_L1_PAGETABLE; i++ )
        put_page_from_l1e(pl1e[i], d);

    unmap_domain_mem(pl1e);
}


static inline int update_l2e(l2_pgentry_t *pl2e, 
                             l2_pgentry_t  ol2e, 
                             l2_pgentry_t  nl2e)
{
    unsigned long o = cmpxchg((unsigned long *)pl2e, 
                              l2_pgentry_val(ol2e), 
                              l2_pgentry_val(nl2e));
    if ( o != l2_pgentry_val(ol2e) )
        MEM_LOG("Failed to update %08lx -> %08lx: saw %08lx\n",
                l2_pgentry_val(ol2e), l2_pgentry_val(nl2e), o);
    return (o == l2_pgentry_val(ol2e));
}


/* Update the L2 entry at pl2e to new value nl2e. pl2e is within frame pfn. */
static int mod_l2_entry(l2_pgentry_t *pl2e, 
                        l2_pgentry_t nl2e, 
                        unsigned long pfn)
{
    l2_pgentry_t ol2e;
    unsigned long _ol2e;

    if ( unlikely((((unsigned long)pl2e & (PAGE_SIZE-1)) >> 2) >=
                  DOMAIN_ENTRIES_PER_L2_PAGETABLE) )
    {
        MEM_LOG("Illegal L2 update attempt in Xen-private area %p", pl2e);
        return 0;
    }

    if ( unlikely(__get_user(_ol2e, (unsigned long *)pl2e) != 0) )
        return 0;
    ol2e = mk_l2_pgentry(_ol2e);

    if ( l2_pgentry_val(nl2e) & _PAGE_PRESENT )
    {
        /* Differ in mapping (bits 12-31) or presence (bit 0)? */
        if ( ((l2_pgentry_val(ol2e) ^ l2_pgentry_val(nl2e)) & ~0xffe) == 0 )
            return update_l2e(pl2e, ol2e, nl2e);

        if ( unlikely(!get_page_from_l2e(nl2e, pfn, current, 
					((unsigned long)pl2e & 
                                         ~PAGE_MASK) >> 2)) )
            return 0;

        if ( unlikely(!update_l2e(pl2e, ol2e, nl2e)) )
        {
            put_page_from_l2e(nl2e, pfn);
            return 0;
        }
        
        put_page_from_l2e(ol2e, pfn);
        return 1;
    }

    if ( unlikely(!update_l2e(pl2e, ol2e, nl2e)) )
        return 0;

    put_page_from_l2e(ol2e, pfn);
    return 1;
}


static inline int update_l1e(l1_pgentry_t *pl1e, 
                             l1_pgentry_t  ol1e, 
                             l1_pgentry_t  nl1e)
{
    unsigned long o = l1_pgentry_val(ol1e);
    unsigned long n = l1_pgentry_val(nl1e);

    if ( unlikely(cmpxchg_user(pl1e, o, n) != 0) ||
         unlikely(o != l1_pgentry_val(ol1e)) )
    {
        MEM_LOG("Failed to update %08lx -> %08lx: saw %08lx\n",
                l1_pgentry_val(ol1e), l1_pgentry_val(nl1e), o);
        return 0;
    }

    return 1;
}


/* Update the L1 entry at pl1e to new value nl1e. */
static int mod_l1_entry(l1_pgentry_t *pl1e, l1_pgentry_t nl1e)
{
    l1_pgentry_t ol1e;
    unsigned long _ol1e;
    struct domain *d = current;

    if ( unlikely(__get_user(_ol1e, (unsigned long *)pl1e) != 0) )
    {
        MEM_LOG("Bad get_user\n");
        return 0;
    }
    
    ol1e = mk_l1_pgentry(_ol1e);

    if ( l1_pgentry_val(nl1e) & _PAGE_PRESENT )
    {
        /* Differ in mapping (bits 12-31), r/w (bit 1), or presence (bit 0)? */
        if ( ((l1_pgentry_val(ol1e) ^ l1_pgentry_val(nl1e)) & ~0xffc) == 0 )
            return update_l1e(pl1e, ol1e, nl1e);

        if ( unlikely(!get_page_from_l1e(nl1e, FOREIGNDOM)) )
            return 0;
        
        if ( unlikely(!update_l1e(pl1e, ol1e, nl1e)) )
        {
            put_page_from_l1e(nl1e, d);
            return 0;
        }
        
        put_page_from_l1e(ol1e, d);
        return 1;
    }

    if ( unlikely(!update_l1e(pl1e, ol1e, nl1e)) )
        return 0;
    
    put_page_from_l1e(ol1e, d);
    return 1;
}


int alloc_page_type(struct pfn_info *page, unsigned int type)
{
    if ( unlikely(test_and_clear_bit(_PGC_tlb_flush_on_type_change, 
                                     &page->count_info)) )
    {
        struct domain *p = page->u.inuse.domain;
        if ( unlikely(NEED_FLUSH(tlbflush_time[p->processor],
                                 page->tlbflush_timestamp)) )
        {
            perfc_incr(need_flush_tlb_flush);
            flush_tlb_cpu(p->processor);
        }
    }

    switch ( type )
    {
    case PGT_l1_page_table:
        return alloc_l1_table(page);
    case PGT_l2_page_table:
        return alloc_l2_table(page);
    case PGT_gdt_page:
    case PGT_ldt_page:
        return alloc_segdesc_page(page);
    default:
        BUG();
    }

    return 0;
}


void free_page_type(struct pfn_info *page, unsigned int type)
{
    struct domain *d = page->u.inuse.domain;

    switch ( type )
    {
    case PGT_l1_page_table:
        free_l1_table(page);
        break;

    case PGT_l2_page_table:
        free_l2_table(page);
        break;

    default:
        BUG();
    }

    if ( unlikely(d->mm.shadow_mode) && 
         (get_shadow_status(&d->mm, page_to_pfn(page)) & PSH_shadowed) )
    {
        unshadow_table(page_to_pfn(page), type);
        put_shadow_status(&d->mm);
    }
}


static int do_extended_command(unsigned long ptr, unsigned long val)
{
    int okay = 1, cpu = smp_processor_id();
    unsigned int cmd = val & MMUEXT_CMD_MASK;
    unsigned long pfn = ptr >> PAGE_SHIFT;
    unsigned long old_base_pfn;
    struct pfn_info *page = &frame_table[pfn];
    struct domain *d = current, *nd, *e;
    u32 x, y;
    domid_t domid;
    grant_ref_t gntref;

    switch ( cmd )
    {
    case MMUEXT_PIN_TABLE:
        okay = get_page_and_type_from_pagenr(
            pfn, PGT_l2_page_table, FOREIGNDOM);

        if ( unlikely(!okay) )
        {
            MEM_LOG("Error while pinning pfn %08lx", pfn);
            put_page(page);
            break;
        }

        if ( unlikely(test_and_set_bit(_PGC_guest_pinned,
                                       &page->count_info)) )
        {
            MEM_LOG("Pfn %08lx already pinned", pfn);
            put_page_and_type(page);
            okay = 0;
            break;
        }

        break;

    case MMUEXT_UNPIN_TABLE:
        if ( unlikely(!(okay = get_page_from_pagenr(pfn, FOREIGNDOM))) )
        {
            MEM_LOG("Page %08lx bad domain (dom=%p)",
                    ptr, page->u.inuse.domain);
        }
        else if ( likely(test_and_clear_bit(_PGC_guest_pinned, 
                                            &page->count_info)) )
        {
            put_page_and_type(page);
            put_page(page);
        }
        else
        {
            okay = 0;
            put_page(page);
            MEM_LOG("Pfn %08lx not pinned", pfn);
        }
        break;

    case MMUEXT_NEW_BASEPTR:
        okay = get_page_and_type_from_pagenr(pfn, PGT_l2_page_table, d);
        if ( likely(okay) )
        {
            invalidate_shadow_ldt(d);

            percpu_info[cpu].deferred_ops &= ~DOP_FLUSH_TLB;
            old_base_pfn = pagetable_val(d->mm.pagetable) >> PAGE_SHIFT;
            d->mm.pagetable = mk_pagetable(pfn << PAGE_SHIFT);

            shadow_mk_pagetable(&d->mm);

            write_ptbase(&d->mm);

            put_page_and_type(&frame_table[old_base_pfn]);    

            /*
             * Note that we tick the clock /after/ dropping the old base's
             * reference count. If the page tables got freed then this will
             * avoid unnecessary TLB flushes when the pages are reused.  */
            tlb_clocktick();
        }
        else
        {
            MEM_LOG("Error while installing new baseptr %08lx", ptr);
        }
        break;
        
    case MMUEXT_TLB_FLUSH:
        percpu_info[cpu].deferred_ops |= DOP_FLUSH_TLB;
        break;
    
    case MMUEXT_INVLPG:
        __flush_tlb_one(ptr);
        break;

    case MMUEXT_FLUSH_CACHE:
        if ( unlikely(!IS_CAPABLE_PHYSDEV(d)) )
        {
            MEM_LOG("Non-physdev domain tried to FLUSH_CACHE.\n");
            okay = 0;
        }
        else
        {
            wbinvd();
        }
        break;

    case MMUEXT_SET_LDT:
    {
        unsigned long ents = val >> MMUEXT_CMD_SHIFT;
        if ( ((ptr & (PAGE_SIZE-1)) != 0) || 
             (ents > 8192) ||
             ((ptr+ents*LDT_ENTRY_SIZE) < ptr) ||
             ((ptr+ents*LDT_ENTRY_SIZE) > PAGE_OFFSET) )
        {
            okay = 0;
            MEM_LOG("Bad args to SET_LDT: ptr=%08lx, ents=%08lx", ptr, ents);
        }
        else if ( (d->mm.ldt_ents != ents) || 
                  (d->mm.ldt_base != ptr) )
        {
            invalidate_shadow_ldt(d);
            d->mm.ldt_base = ptr;
            d->mm.ldt_ents = ents;
            load_LDT(d);
            percpu_info[cpu].deferred_ops &= ~DOP_RELOAD_LDT;
            if ( ents != 0 )
                percpu_info[cpu].deferred_ops |= DOP_RELOAD_LDT;
        }
        break;
    }

    case MMUEXT_SET_FOREIGNDOM:
        domid = (domid_t)(val >> 16);

        if ( (e = percpu_info[cpu].foreign) != NULL )
            put_domain(e);
        percpu_info[cpu].foreign = NULL;

        if ( !IS_PRIV(d) )
        {
            switch ( domid )
            {
            case DOMID_IO:
                get_knownalive_domain(dom_io);
                percpu_info[cpu].foreign = dom_io;
                break;
            default:
                MEM_LOG("Dom %u cannot set foreign dom\n", d->domain);
                okay = 0;
                break;
            }
        }
        else
        {
            percpu_info[cpu].foreign = e = find_domain_by_id(domid);
            if ( e == NULL )
            {
                switch ( domid )
                {
                case DOMID_XEN:
                    get_knownalive_domain(dom_xen);
                    percpu_info[cpu].foreign = dom_xen;
                    break;
                case DOMID_IO:
                    get_knownalive_domain(dom_io);
                    percpu_info[cpu].foreign = dom_io;
                    break;
                default:
                    MEM_LOG("Unknown domain '%u'", domid);
                    okay = 0;
                    break;
                }
            }
        }
        break;

    case MMUEXT_TRANSFER_PAGE:
        domid  = (domid_t)(val >> 16);
        gntref = (grant_ref_t)((val & 0xFF00) | ((ptr >> 2) & 0x00FF));
        
        if ( unlikely(IS_XEN_HEAP_FRAME(page)) ||
             unlikely(!pfn_is_ram(pfn)) ||
             unlikely((e = find_domain_by_id(domid)) == NULL) )
        {
            MEM_LOG("Bad frame (%08lx) or bad domid (%d).\n", pfn, domid);
            okay = 0;
            break;
        }

        spin_lock(&d->page_alloc_lock);

        /*
         * The tricky bit: atomically release ownership while there is just one
         * benign reference to the page (PGC_allocated). If that reference
         * disappears then the deallocation routine will safely spin.
         */
        nd = page->u.inuse.domain;
        y  = page->count_info;
        do {
            x = y;
            if ( unlikely((x & (PGC_count_mask|PGC_allocated)) != 
                          (1|PGC_allocated)) ||
                 unlikely(nd != d) )
            {
                MEM_LOG("Bad page values %08lx: ed=%p(%u), sd=%p,"
                        " caf=%08x, taf=%08x\n", page_to_pfn(page),
                        d, d->domain, nd, x, page->u.inuse.type_info);
                spin_unlock(&d->page_alloc_lock);
                put_domain(e);
                okay = 0;
                break;
            }
            __asm__ __volatile__(
                LOCK_PREFIX "cmpxchg8b %2"
                : "=d" (nd), "=a" (y),
                "=m" (*(volatile u64 *)(&page->count_info))
                : "0" (d), "1" (x), "c" (NULL), "b" (x) );
        } 
        while ( unlikely(nd != d) || unlikely(y != x) );

        /*
         * Unlink from 'd'. At least one reference remains (now anonymous), so
         * noone else is spinning to try to delete this page from 'd'.
         */
        d->tot_pages--;
        list_del(&page->list);
        
        spin_unlock(&d->page_alloc_lock);

        spin_lock(&e->page_alloc_lock);

        /* Check that 'e' will accept the page and has reservation headroom. */
        ASSERT(e->tot_pages <= e->max_pages);
        if ( unlikely(e->tot_pages == e->max_pages) ||
             unlikely(!gnttab_prepare_for_transfer(e, d, gntref)) )
        {
            MEM_LOG("Transferee has no reservation headroom (%d,%d), or "
                    "provided a bad grant ref.\n", e->tot_pages, e->max_pages);
            spin_unlock(&e->page_alloc_lock);
            put_domain(e);
            okay = 0;
            break;
        }

        /* Okay, add the page to 'e'. */
        if ( unlikely(e->tot_pages++ == 0) )
            get_knownalive_domain(e);
        list_add_tail(&page->list, &e->page_list);
        page->u.inuse.domain = e;

        spin_unlock(&e->page_alloc_lock);

        /* Transfer is all done: tell the guest about its new page frame. */
        gnttab_notify_transfer(e, gntref, pfn);
        
        put_domain(e);
        break;

    case MMUEXT_REASSIGN_PAGE:
        if ( unlikely(!IS_PRIV(d)) )
        {
            MEM_LOG("Dom %u has no reassignment priv", d->domain);
            okay = 0;
            break;
        }

        e = percpu_info[cpu].foreign;
        if ( unlikely(e == NULL) )
        {
            MEM_LOG("No FOREIGNDOM to reassign pfn %08lx to", pfn);
            okay = 0;
            break;
        }

        /*
         * Grab both page_list locks, in order. This prevents the page from
         * disappearing elsewhere while we modify the owner, and we'll need
         * both locks if we're successful so that we can change lists.
         */
        if ( d < e )
        {
            spin_lock(&d->page_alloc_lock);
            spin_lock(&e->page_alloc_lock);
        }
        else
        {
            spin_lock(&e->page_alloc_lock);
            spin_lock(&d->page_alloc_lock);
        }

        /* A domain shouldn't have PGC_allocated pages when it is dying. */
        if ( unlikely(test_bit(DF_DYING, &e->flags)) ||
             unlikely(IS_XEN_HEAP_FRAME(page)) )
        {
            MEM_LOG("Reassignment page is Xen heap, or dest dom is dying.");
            okay = 0;
            goto reassign_fail;
        }

        /*
         * The tricky bit: atomically change owner while there is just one
         * benign reference to the page (PGC_allocated). If that reference
         * disappears then the deallocation routine will safely spin.
         */
        nd = page->u.inuse.domain;
        y  = page->count_info;
        do {
            x = y;
            if ( unlikely((x & (PGC_count_mask|PGC_allocated)) != 
                          (1|PGC_allocated)) ||
                 unlikely(nd != d) )
            {
                MEM_LOG("Bad page values %08lx: ed=%p(%u), sd=%p,"
                        " caf=%08x, taf=%08x\n", page_to_pfn(page),
                        d, d->domain, nd, x, page->u.inuse.type_info);
                okay = 0;
                goto reassign_fail;
            }
            __asm__ __volatile__(
                LOCK_PREFIX "cmpxchg8b %3"
                : "=d" (nd), "=a" (y), "=c" (e),
                "=m" (*(volatile u64 *)(&page->count_info))
                : "0" (d), "1" (x), "c" (e), "b" (x) );
        } 
        while ( unlikely(nd != d) || unlikely(y != x) );
        
        /*
         * Unlink from 'd'. We transferred at least one reference to 'e', so
         * noone else is spinning to try to delete this page from 'd'.
         */
        d->tot_pages--;
        list_del(&page->list);
        
        /*
         * Add the page to 'e'. Someone may already have removed the last
         * reference and want to remove the page from 'e'. However, we have
         * the lock so they'll spin waiting for us.
         */
        if ( unlikely(e->tot_pages++ == 0) )
            get_knownalive_domain(e);
        list_add_tail(&page->list, &e->page_list);

    reassign_fail:        
        spin_unlock(&d->page_alloc_lock);
        spin_unlock(&e->page_alloc_lock);
        break;

    case MMUEXT_CLEAR_FOREIGNDOM:
        if ( (e = percpu_info[cpu].foreign) != NULL )
            put_domain(e);
        percpu_info[cpu].foreign = NULL;
        break;

    default:
        MEM_LOG("Invalid extended pt command 0x%08lx", val & MMUEXT_CMD_MASK);
        okay = 0;
        break;
    }

    return okay;
}


int do_mmu_update(mmu_update_t *ureqs, int count, int *success_count)
{
    mmu_update_t req;
    unsigned long va = 0, deferred_ops, pfn, prev_pfn = 0;
    struct pfn_info *page;
    int rc = 0, okay = 1, i, cpu = smp_processor_id();
    unsigned int cmd;
    unsigned long prev_spfn = 0;
    l1_pgentry_t *prev_spl1e = 0;
    struct domain *d = current;
    u32 type_info;

    perfc_incrc(calls_to_mmu_update); 
    perfc_addc(num_page_updates, count);

    cleanup_writable_pagetable(d, PTWR_CLEANUP_ACTIVE | PTWR_CLEANUP_INACTIVE);

    if ( unlikely(!access_ok(VERIFY_READ, ureqs, count * sizeof(req))) )
        return -EFAULT;

    for ( i = 0; i < count; i++ )
    {
        if ( unlikely(__copy_from_user(&req, ureqs, sizeof(req)) != 0) )
        {
            MEM_LOG("Bad __copy_from_user");
            rc = -EFAULT;
            break;
        }

        cmd = req.ptr & (sizeof(l1_pgentry_t)-1);
        pfn = req.ptr >> PAGE_SHIFT;

        okay = 0;

        switch ( cmd )
        {
            /*
             * MMU_NORMAL_PT_UPDATE: Normal update to any level of page table.
             */
        case MMU_NORMAL_PT_UPDATE:
            if ( unlikely(!get_page_from_pagenr(pfn, current)) )
            {
                MEM_LOG("Could not get page for normal update");
                break;
            }

            if ( likely(prev_pfn == pfn) )
            {
                va = (va & PAGE_MASK) | (req.ptr & ~PAGE_MASK);
            }
            else
            {
                if ( prev_pfn != 0 )
                    unmap_domain_mem((void *)va);
                va = (unsigned long)map_domain_mem(req.ptr);
                prev_pfn = pfn;
            }

            page = &frame_table[pfn];
            switch ( (type_info = page->u.inuse.type_info) & PGT_type_mask )
            {
            case PGT_l1_page_table: 
                if ( likely(get_page_type(
                    page, type_info & (PGT_type_mask|PGT_va_mask))) )
                {
                    okay = mod_l1_entry((l1_pgentry_t *)va, 
                                        mk_l1_pgentry(req.val)); 

                    if ( unlikely(d->mm.shadow_mode) && okay &&
                         (get_shadow_status(&d->mm, page-frame_table) &
                          PSH_shadowed) )
                    {
                        shadow_l1_normal_pt_update(
                            req.ptr, req.val, &prev_spfn, &prev_spl1e);
                        put_shadow_status(&d->mm);
                    }

                    put_page_type(page);
                }
                break;
            case PGT_l2_page_table:
                if ( likely(get_page_type(page, PGT_l2_page_table)) )
                {
                    okay = mod_l2_entry((l2_pgentry_t *)va, 
                                        mk_l2_pgentry(req.val),
                                        pfn); 

                    if ( unlikely(d->mm.shadow_mode) && okay &&
                         (get_shadow_status(&d->mm, page-frame_table) & 
                          PSH_shadowed) )
                    {
                        shadow_l2_normal_pt_update(req.ptr, req.val);
                        put_shadow_status(&d->mm);
                    }

                    put_page_type(page);
                }
                break;
            default:
                if ( likely(get_page_type(page, PGT_writable_page)) )
                {
                    *(unsigned long *)va = req.val;
                    okay = 1;
                    put_page_type(page);
                }
                break;
            }

            put_page(page);
            break;

        case MMU_MACHPHYS_UPDATE:
            if ( unlikely(!get_page_from_pagenr(pfn, FOREIGNDOM)) )
            {
                MEM_LOG("Could not get page for mach->phys update");
                break;
            }

            machine_to_phys_mapping[pfn] = req.val;
            okay = 1;

            /*
             * If in log-dirty mode, mark the corresponding pseudo-physical
             * page as dirty.
             */
            if ( unlikely(d->mm.shadow_mode == SHM_logdirty) )
                mark_dirty(&d->mm, pfn);

            put_page(&frame_table[pfn]);
            break;

            /*
             * MMU_EXTENDED_COMMAND: Extended command is specified
             * in the least-siginificant bits of the 'value' field.
             */
        case MMU_EXTENDED_COMMAND:
            req.ptr &= ~(sizeof(l1_pgentry_t) - 1);
            okay = do_extended_command(req.ptr, req.val);
            break;

        default:
            MEM_LOG("Invalid page update command %08lx", req.ptr);
            break;
        }

        if ( unlikely(!okay) )
        {
            rc = -EINVAL;
            break;
        }

        ureqs++;
    }

    if ( prev_pfn != 0 )
        unmap_domain_mem((void *)va);

    if ( unlikely(prev_spl1e != 0) ) 
        unmap_domain_mem((void *)prev_spl1e);

    deferred_ops = percpu_info[cpu].deferred_ops;
    percpu_info[cpu].deferred_ops = 0;

    if ( deferred_ops & DOP_FLUSH_TLB )
        local_flush_tlb();
        
    if ( deferred_ops & DOP_RELOAD_LDT )
        (void)map_ldt_shadow_page(0);

    if ( unlikely(percpu_info[cpu].foreign != NULL) )
    {
        put_domain(percpu_info[cpu].foreign);
        percpu_info[cpu].foreign = NULL;
    }

    if ( unlikely(success_count != NULL) )
        put_user(count, success_count);

    return rc;
}


int do_update_va_mapping(unsigned long page_nr, 
                         unsigned long val, 
                         unsigned long flags)
{
    struct domain *d = current;
    int err = 0;
    unsigned int cpu = d->processor;
    unsigned long deferred_ops;

    perfc_incrc(calls_to_update_va);

    if ( unlikely(page_nr >= (HYPERVISOR_VIRT_START >> PAGE_SHIFT)) )
        return -EINVAL;

    cleanup_writable_pagetable(d, PTWR_CLEANUP_ACTIVE | PTWR_CLEANUP_INACTIVE);

    /*
     * XXX When we make this support 4MB superpages we should also deal with 
     * the case of updating L2 entries.
     */

    if ( unlikely(!mod_l1_entry(&linear_pg_table[page_nr], 
                                mk_l1_pgentry(val))) )
        err = -EINVAL;

    if ( unlikely(d->mm.shadow_mode) )
    {
        unsigned long sval;

        l1pte_no_fault(&d->mm, &val, &sval);

        if ( unlikely(__put_user(sval, ((unsigned long *)(
            &shadow_linear_pg_table[page_nr])))) )
        {
            /*
             * Since L2's are guranteed RW, failure indicates the page was not 
             * shadowed, so ignore.
             */
            perfc_incrc(shadow_update_va_fail);
        }

        /*
         * If we're in log-dirty mode then we need to note that we've updated
         * the PTE in the PT-holding page. We need the machine frame number
         * for this.
         */
        if ( d->mm.shadow_mode == SHM_logdirty )
            mark_dirty( &current->mm, va_to_l1mfn(page_nr<<PAGE_SHIFT) );  
  
        check_pagetable(d, d->mm.pagetable, "va"); /* debug */
    }

    deferred_ops = percpu_info[cpu].deferred_ops;
    percpu_info[cpu].deferred_ops = 0;

    if ( unlikely(deferred_ops & DOP_FLUSH_TLB) || 
         unlikely(flags & UVMF_FLUSH_TLB) )
        local_flush_tlb();
    else if ( unlikely(flags & UVMF_INVLPG) )
        __flush_tlb_one(page_nr << PAGE_SHIFT);

    if ( unlikely(deferred_ops & DOP_RELOAD_LDT) )
        (void)map_ldt_shadow_page(0);
    
    return err;
}

int do_update_va_mapping_otherdomain(unsigned long page_nr, 
                                     unsigned long val, 
                                     unsigned long flags,
                                     domid_t domid)
{
    unsigned int cpu = smp_processor_id();
    struct domain *d;
    int rc;

    if ( unlikely(!IS_PRIV(current)) )
        return -EPERM;

    percpu_info[cpu].foreign = d = find_domain_by_id(domid);
    if ( unlikely(d == NULL) )
    {
        MEM_LOG("Unknown domain '%u'", domid);
        return -ESRCH;
    }

    rc = do_update_va_mapping(page_nr, val, flags);

    put_domain(d);
    percpu_info[cpu].foreign = NULL;

    return rc;
}



/*************************
 * Writable Pagetables
 */

ptwr_info_t ptwr_info[NR_CPUS] =
    { [ 0 ... NR_CPUS-1 ] =
      {
	  .disconnected = ENTRIES_PER_L2_PAGETABLE,
	  .writable_idx = 0,
#ifdef PTWR_TRACK_DOMAIN
	  .domain = 0,
#endif
      }
    };

#ifdef VERBOSE
int ptwr_debug = 0;
#define PTWR_PRINTK(x) if (ptwr_debug) printk x
#else
#define PTWR_PRINTK(x)
#endif

void ptwr_reconnect_disconnected(unsigned long addr)
{
    unsigned long pte;
    unsigned long pfn;
    struct pfn_info *page;
    l2_pgentry_t *pl2e, nl2e;
    l1_pgentry_t *pl1e;
    int cpu = smp_processor_id();
    int i;
    unsigned long *writable_pte = (unsigned long *)&linear_pg_table
        [ptwr_info[cpu].writable_l1>>PAGE_SHIFT];

#ifdef PTWR_TRACK_DOMAIN
    if (ptwr_domain[cpu] != current->domain)
        printk("ptwr_reconnect_disconnected domain mismatch %d != %d\n",
               ptwr_domain[cpu], current->domain);
#endif
    PTWR_PRINTK(("[A] page fault in disconn space: addr %08lx space %08lx\n",
                 addr, ptwr_info[cpu].disconnected << L2_PAGETABLE_SHIFT));
    pl2e = &linear_l2_table[ptwr_info[cpu].disconnected];

    if (__get_user(pte, writable_pte)) {
	MEM_LOG("ptwr: Could not read pte at %p\n", writable_pte);
	domain_crash();
    }
    pfn = pte >> PAGE_SHIFT;
    page = &frame_table[pfn];

    /* reconnect l1 page */
    PTWR_PRINTK(("[A]     pl2e %p l2e %08lx pfn %08lx taf %08x/%08x/%u\n",
                 pl2e, l2_pgentry_val(*pl2e),
                 l1_pgentry_val(linear_pg_table[(unsigned long)pl2e >>
                                                PAGE_SHIFT]) >> PAGE_SHIFT,
                 frame_table[pfn].u.inuse.type_info,
                 frame_table[pfn].count_info,
                 frame_table[pfn].u.inuse.domain->domain));

    nl2e = mk_l2_pgentry(l2_pgentry_val(*pl2e) | _PAGE_PRESENT);
    pl1e = map_domain_mem(l2_pgentry_to_pagenr(nl2e) << PAGE_SHIFT);
    for ( i = 0; i < ENTRIES_PER_L1_PAGETABLE; i++ ) {
        l1_pgentry_t ol1e, nl1e;
        ol1e = ptwr_info[cpu].disconnected_page[i];
        nl1e = pl1e[i];
        if (likely(l1_pgentry_val(nl1e) == l1_pgentry_val(ol1e)))
            continue;
        if (unlikely(l1_pgentry_val(ol1e) & _PAGE_PRESENT))
            put_page_from_l1e(ol1e, current);
        if (unlikely(!get_page_from_l1e(nl1e, current))) {
	    MEM_LOG("ptwr: Could not re-validate l1 page\n");
	    domain_crash();
	}
    }
    unmap_domain_mem(pl1e);
    update_l2e(pl2e, *pl2e, nl2e);

    PTWR_PRINTK(("[A] now pl2e %p l2e %08lx              taf %08x/%08x/%u\n",
                 pl2e, l2_pgentry_val(*pl2e),
                 frame_table[pfn].u.inuse.type_info,
                 frame_table[pfn].count_info,
                 frame_table[pfn].u.inuse.domain->domain));
    ptwr_info[cpu].disconnected = ENTRIES_PER_L2_PAGETABLE;
    /* make pt page write protected */
    if (__get_user(pte, writable_pte)) {
	MEM_LOG("ptwr: Could not read pte at %p\n", writable_pte);
	domain_crash();
    }
    PTWR_PRINTK(("[A] writable_l1 at %p is %08lx\n",
                 writable_pte, pte));
    pte &= ~_PAGE_RW;
    if (__put_user(pte, writable_pte)) {
	MEM_LOG("ptwr: Could not update pte at %p\n", writable_pte);
	domain_crash();
    }
    __flush_tlb_one(ptwr_info[cpu].writable_l1);
    PTWR_PRINTK(("[A] writable_l1 at %p now %08lx\n",
                 writable_pte, pte));
    /* and try again */
    return;
}

void ptwr_flush_inactive(void)
{
    unsigned long pte, pfn;
    struct pfn_info *page;
    l1_pgentry_t *pl1e;
    int cpu = smp_processor_id();
    int i, idx;

#ifdef PTWR_TRACK_DOMAIN
    if (ptwr_info[cpu].domain != current->domain)
        printk("ptwr_flush_inactive domain mismatch %d != %d\n",
               ptwr_info[cpu].domain, current->domain);
#endif
#if 0
    {
	static int maxidx = 0;
	if (ptwr_info[cpu].writable_idx > maxidx) {
	    maxidx = ptwr_info[cpu].writable_idx;
	    printk("maxidx on cpu %d now %d\n", cpu, maxidx);
	}
    }
#endif
    for (idx = 0; idx < ptwr_info[cpu].writable_idx; idx++) {
        unsigned long *writable_pte = (unsigned long *)&linear_pg_table
            [ptwr_info[cpu].writables[idx]>>PAGE_SHIFT];
        if (__get_user(pte, writable_pte)) {
	    MEM_LOG("ptwr: Could not read pte at %p\n", writable_pte);
	    domain_crash();
	}
        pfn = pte >> PAGE_SHIFT;
        page = &frame_table[pfn];
        PTWR_PRINTK(("[I] alloc l1 page %p\n", page));

        pl1e = map_domain_mem(pfn << PAGE_SHIFT);
        for ( i = 0; i < ENTRIES_PER_L1_PAGETABLE; i++ ) {
            l1_pgentry_t ol1e, nl1e;
            ol1e = ptwr_info[cpu].writable_page[idx][i];
            nl1e = pl1e[i];
            if (likely(l1_pgentry_val(ol1e) == l1_pgentry_val(nl1e)))
                continue;
            if (unlikely(l1_pgentry_val(ol1e) & _PAGE_PRESENT))
                put_page_from_l1e(ol1e, current);
            if (unlikely(!get_page_from_l1e(nl1e, current))) {
		MEM_LOG("ptwr: Could not re-validate l1 page\n");
		domain_crash();
	    }
        }
        unmap_domain_mem(pl1e);

        /* make pt page writable */
        PTWR_PRINTK(("[I] writable_l1 at %p is %08lx\n",
                     writable_pte, pte));
        pte &= ~_PAGE_RW;
        if (__put_user(pte, writable_pte)) {
	    MEM_LOG("ptwr: Could not update pte at %p\n", writable_pte);
	    domain_crash();
	}
        __flush_tlb_one(ptwr_info[cpu].writables[idx]);
        PTWR_PRINTK(("[I] writable_l1 at %p now %08lx\n",
                     writable_pte, pte));
    }
    ptwr_info[cpu].writable_idx = 0;
}

int ptwr_do_page_fault(unsigned long addr)
{
    /* write page fault, check if we're trying to modify an l1 page table */
    unsigned long pte, pfn;
    struct pfn_info *page;
    l2_pgentry_t *pl2e;
    int cpu = smp_processor_id();

#if 0
    PTWR_PRINTK(("get user %p for va %08lx\n",
                 &linear_pg_table[addr>>PAGE_SHIFT], addr));
#endif

    /* Testing for page_present in the L2 avoids lots of unncessary fixups */
    if ( (l2_pgentry_val(linear_l2_table[addr >> L2_PAGETABLE_SHIFT]) &
      _PAGE_PRESENT) &&
	 (__get_user(pte, (unsigned long *)
                     &linear_pg_table[addr >> PAGE_SHIFT]) == 0) )
    {
        pfn = pte >> PAGE_SHIFT;
#if 0
        PTWR_PRINTK(("check pte %08lx = pfn %08lx for va %08lx\n", pte, pfn,
                     addr));
#endif
        page = &frame_table[pfn];
        if ( (page->u.inuse.type_info & PGT_type_mask) == PGT_l1_page_table )
        {
#ifdef PTWR_TRACK_DOMAIN
            if ( ptwr_info[cpu].domain != current->domain )
                printk("ptwr_do_page_fault domain mismatch %d != %d\n",
                       ptwr_info[cpu].domain, current->domain);
#endif
            pl2e = &linear_l2_table[(page->u.inuse.type_info &
                                     PGT_va_mask) >> PGT_va_shift];
            PTWR_PRINTK(("page_fault on l1 pt at va %08lx, pt for %08x, "
                         "pfn %08lx\n", addr,
                         ((page->u.inuse.type_info & PGT_va_mask) >>
                          PGT_va_shift) << L2_PAGETABLE_SHIFT, pfn));

            if ( l2_pgentry_val(*pl2e) >> PAGE_SHIFT != pfn )
            {
		/* this L1 is not in the current address space */
                l1_pgentry_t *pl1e;
                PTWR_PRINTK(("[I] freeing l1 page %p taf %08x/%08x\n", page,
                             page->u.inuse.type_info,
                             page->count_info));
                if (ptwr_info[cpu].writable_idx == PTWR_NR_WRITABLES)
                    ptwr_flush_inactive();
                ptwr_info[cpu].writables[ptwr_info[cpu].writable_idx] = addr;

                pl1e = map_domain_mem(pfn << PAGE_SHIFT);
                memcpy(&ptwr_info[cpu].writable_page[
                           ptwr_info[cpu].writable_idx][0],
                       pl1e, ENTRIES_PER_L1_PAGETABLE * sizeof(l1_pgentry_t));
                unmap_domain_mem(pl1e);

                ptwr_info[cpu].writable_idx++;
            }
            else
            {
                l2_pgentry_t nl2e;
                l1_pgentry_t *pl1e;
                if ( ptwr_info[cpu].disconnected != ENTRIES_PER_L2_PAGETABLE )
                    ptwr_reconnect_disconnected(addr);
                PTWR_PRINTK(("[A]    pl2e %p l2e %08lx pfn %08lx "
                             "taf %08x/%08x/%u\n", pl2e, l2_pgentry_val(*pl2e),
                             l1_pgentry_val(linear_pg_table[(unsigned long)pl2e
                                                            >> PAGE_SHIFT]) >>
                             PAGE_SHIFT,
                             frame_table[pfn].u.inuse.type_info,
                             frame_table[pfn].count_info,
                             frame_table[pfn].u.inuse.domain->domain));
                /* disconnect l1 page */
                nl2e = mk_l2_pgentry((l2_pgentry_val(*pl2e) & ~_PAGE_PRESENT));
                update_l2e(pl2e, *pl2e, nl2e);

                ptwr_info[cpu].disconnected =
                    (page->u.inuse.type_info & PGT_va_mask) >> PGT_va_shift;
                PTWR_PRINTK(("[A] now pl2e %p l2e %08lx              "
                             "taf %08x/%08x/%u\n", pl2e, l2_pgentry_val(*pl2e),
                             frame_table[pfn].u.inuse.type_info,
                             frame_table[pfn].count_info,
                             frame_table[pfn].u.inuse.domain->domain));
                ptwr_info[cpu].writable_l1 = addr;
                pl1e = map_domain_mem(l2_pgentry_to_pagenr(nl2e) <<
                                      PAGE_SHIFT);
                memcpy(&ptwr_info[cpu].disconnected_page[0], pl1e,
                       ENTRIES_PER_L1_PAGETABLE * sizeof(l1_pgentry_t));
                unmap_domain_mem(pl1e);
            }

            /* make pt page writable */
            pte |= _PAGE_RW;
            PTWR_PRINTK(("update %p pte to %08lx\n",
                         &linear_pg_table[addr>>PAGE_SHIFT], pte));
            if ( __put_user(pte, (unsigned long *)
			    &linear_pg_table[addr>>PAGE_SHIFT]) ) {
		MEM_LOG("ptwr: Could not update pte at %p\n", (unsigned long *)
			&linear_pg_table[addr>>PAGE_SHIFT]);
		domain_crash();
	    }
            return 1;
        }
    }
    return 0;
}

#ifndef NDEBUG
void ptwr_status(void)
{
    int i;
    unsigned long pte, pfn;
    struct pfn_info *page;
    l2_pgentry_t *pl2e;
    int cpu = smp_processor_id();

    for ( i = 0; i < ptwr_info[cpu].writable_idx; i++ )
    {
        unsigned long *writable_pte = (unsigned long *)&linear_pg_table
            [ptwr_info[cpu].writables[i]>>PAGE_SHIFT];

        if ( __get_user(pte, writable_pte) ) {
	    MEM_LOG("ptwr: Could not read pte at %p\n", writable_pte);
	    domain_crash();
	}

        pfn = pte >> PAGE_SHIFT;
        page = &frame_table[pfn];
        printk("need to alloc l1 page %p\n", page);
        /* make pt page writable */
        printk("need to make read-only l1-page at %p is %08lx\n",
               writable_pte, pte);
    }

    if ( ptwr_info[cpu].disconnected == ENTRIES_PER_L2_PAGETABLE )
        return;

    printk("disconnected space: space %08lx\n",
           ptwr_info[cpu].disconnected << L2_PAGETABLE_SHIFT);
    pl2e = &linear_l2_table[ptwr_info[cpu].disconnected];

    if ( __get_user(pte, (unsigned long *)ptwr_info[cpu].writable_l1) ) {
	MEM_LOG("ptwr: Could not read pte at %p\n", (unsigned long *)
		ptwr_info[cpu].writable_l1);
	domain_crash();
    }
    pfn = pte >> PAGE_SHIFT;
    page = &frame_table[pfn];

    PTWR_PRINTK(("    pl2e %p l2e %08lx pfn %08lx taf %08x/%08x/%u\n", pl2e,
                 l2_pgentry_val(*pl2e),
                 l1_pgentry_val(linear_pg_table[(unsigned long)pl2e >>
                                                PAGE_SHIFT]) >> PAGE_SHIFT,
                 frame_table[l2_pgentry_to_pagenr(*pl2e)].u.inuse.type_info,
                 frame_table[pfn].u.inuse.type_info,
                 frame_table[pfn].u.inuse.domain->domain));
}
#endif
