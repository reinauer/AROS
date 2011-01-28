#include <aros/debug.h>
#include <proto/kernel.h>

#include "exec_intern.h"
#include "memory.h"
#include "mungwall.h"

#define DMH(x)

/* Find MemHeader to which address belongs */
struct MemHeader *FindMem(APTR address, struct ExecBase *SysBase)
{
    struct MemHeader *mh;

    /* Nobody should change the memory list now. */
    MEM_LOCK_SHARED;

    /* Follow the list of MemHeaders */
    mh = (struct MemHeader *)SysBase->MemList.lh_Head;

    while(mh->mh_Node.ln_Succ != NULL)
    {
	/* Check if this MemHeader fits */
	if(address >= mh->mh_Lower && address <= mh->mh_Upper)
	{
	    /* Yes. Return it. */
	    MEM_UNLOCK;
	    return mh;
	}

	/* Go to next MemHeader */
	mh = (struct MemHeader *)mh->mh_Node.ln_Succ;
    }

    MEM_UNLOCK;
    return NULL;
}

/*
 * Allocate block from the given MemHeader in a specific way.
 * This routine can be called with SysBase = NULL.
 */
APTR stdAlloc(struct MemHeader *mh, IPTR byteSize, ULONG requirements, struct ExecBase *SysBase)
{
    struct MemChunk *mc=NULL, *p1, *p2;

    /* First round byteSize to a multiple of MEMCHUNK_TOTAL */
    byteSize = AROS_ROUNDUP2(byteSize, MEMCHUNK_TOTAL);

    /*
     * The free memory list is only single linked, i.e. to remove
     * elements from the list I need node's predessor. For the
     * first element I can use mh->mh_First instead of a real predessor.
     */
    p1 = (struct MemChunk *)&mh->mh_First;
    p2 = p1->mc_Next;

    /* Follow the memory list */
    while (p2 != NULL)
    {
        /* p1 is the previous MemChunk, p2 is the current one */
#if !defined(NO_CONSISTENCY_CHECKS)
	/*
	 * Memory list consistency checks.
	 * 1. Check alignment restrictions
	 */
        if (((IPTR)p2|(IPTR)p2->mc_Bytes) & (MEMCHUNK_TOTAL-1))
	{
	    if (SysBase && SysBase->DebugAROSBase)
	    {
		bug("[MM] Chunk allocator error\n");
		bug("[MM] Attempt to allocate %lu bytes from MemHeader 0x%p\n", byteSize, mh);
		bug("[MM] Misaligned chunk at 0x%p (%u bytes)\n", p2, p2->mc_Bytes);

		Alert(AN_MemoryInsane|AT_DeadEnd);
	    }
	    return NULL;
	}

	/* 2. Check against overlapping blocks */
	if (p2->mc_Next && ((UBYTE *)p2 + p2->mc_Bytes >= (UBYTE *)p2->mc_Next))
	{
	    if (SysBase && SysBase->DebugAROSBase)
	    {
		bug("[MM] Chunk allocator error\n");
		bug("[MM] Attempt to allocate %lu bytes from MemHeader 0x%p\n", byteSize, mh);
		bug("[MM] Overlapping chunks 0x%p (%u bytes) and 0x%p (%u bytes)\n", p2, p2->mc_Bytes, p2->mc_Next, p2->mc_Next->mc_Bytes);

		Alert(AN_MemoryInsane|AT_DeadEnd);
	    }
	    return NULL;
	}
#endif

        /* Check if the current block is large enough */
        if (p2->mc_Bytes>=byteSize)
        {
            /* It is. */
            mc = p1;

            /* Use this one if MEMF_REVERSE is not set.*/
            if (!(requirements & MEMF_REVERSE))
                break;
            /* Else continue - there may be more to come. */
        }

        /* Go to next block */
        p1 = p2;
        p2 = p1->mc_Next;
    }

    /* Something found? */
    if (mc != NULL)
    {
        /* Remember: if MEMF_REVERSE is set p1 and p2 are now invalid. */
        p1 = mc;
        p2 = p1->mc_Next;

        /* Remove the block from the list and return it. */
        if (p2->mc_Bytes == byteSize)
        {
            /* Fits exactly. Just relink the list. */
            p1->mc_Next = p2->mc_Next;
            mc          = p2;
        }
        else
        {
            if (requirements & MEMF_REVERSE)
            {
                /* Return the last bytes. */
                p1->mc_Next=p2;
                mc = (struct MemChunk *)((UBYTE *)p2+p2->mc_Bytes-byteSize);
            }
            else
            {
                /* Return the first bytes. */
                p1->mc_Next=(struct MemChunk *)((UBYTE *)p2+byteSize);
                mc=p2;
            }

            p1           = p1->mc_Next;
            p1->mc_Next  = p2->mc_Next;
            p1->mc_Bytes = p2->mc_Bytes-byteSize;
        }

        mh->mh_Free -= byteSize;

	/* Clear the block if requested */
	if (requirements & MEMF_CLEAR)
	    memset(mc, 0, byteSize);
    }

    return mc;
}

/* Free 'byteSize' bytes starting at 'memoryBlock' belonging to MemHeader 'freeList' */
void stdDealloc(struct MemHeader *freeList, APTR memoryBlock, IPTR byteSize, struct ExecBase *SysBase)
{
    struct MemChunk *p1, *p2, *p3;
    UBYTE *p4;

    /* Align size to the requirements */
    byteSize+=(IPTR)memoryBlock&(MEMCHUNK_TOTAL-1);
    byteSize=(byteSize+MEMCHUNK_TOTAL-1)&~(MEMCHUNK_TOTAL-1);

    /* Align the block as well */
    memoryBlock=(APTR)((IPTR)memoryBlock&~(MEMCHUNK_TOTAL-1));

    /*
	The free memory list is only single linked, i.e. to insert
	elements into the list I need the node as well as it's
	predessor. For the first element I can use freeList->mh_First
	instead of a real predessor.
    */
    p1=(struct MemChunk *)&freeList->mh_First;
    p2=freeList->mh_First;

    /* Start and end(+1) of the block */
    p3=(struct MemChunk *)memoryBlock;
    p4=(UBYTE *)p3+byteSize;

    /* No chunk in list? Just insert the current one and return. */
    if(p2==NULL)
    {
	p3->mc_Bytes=byteSize;
	p3->mc_Next=NULL;
	p1->mc_Next=p3;
	freeList->mh_Free+=byteSize;
	return;
    }

    /* Follow the list to find a place where to insert our memory. */
    do
    {
#if !defined(NO_CONSISTENCY_CHECKS)
	/*
	 * Do some constistency checks:
	 * 1. All MemChunks must be aligned to MEMCHUNK_TOTAL.
	 */
        if (((IPTR)p2|(IPTR)p2->mc_Bytes) & (MEMCHUNK_TOTAL-1))
	{
	    bug("[MM] Chunk allocator error\n");
	    bug("[MM] Attempt to free %u bytes at 0x%p from MemHeader 0x%p\n", byteSize, memoryBlock, freeList);
	    bug("[MM] Misaligned chunk at 0x%p (%u bytes)\n", p2, p2->mc_Bytes);

	    Alert(AN_MemCorrupt|AT_DeadEnd);
	}

	/* 
	 * 2. The end (+1) of the current MemChunk
	 *    must be lower than the start of the next one.
	 */
	if (p2->mc_Next && ((UBYTE *)p2 + p2->mc_Bytes >= (UBYTE *)p2->mc_Next))
	{
	    bug("[MM] Chunk allocator error\n");
	    bug("[MM] Attempt to free %lu bytes at 0x%p from MemHeader 0x%p\n", byteSize, memoryBlock, freeList);
	    bug("[MM] Overlapping chunks 0x%p (%u bytes) and 0x%p (%u bytes)\n", p2, p2->mc_Bytes, p2->mc_Next, p2->mc_Next->mc_Bytes);

	    Alert(AN_MemCorrupt|AT_DeadEnd);
	}
#endif
	/* Found a block with a higher address? */
	if (p2 >= p3)
	{
#if !defined(NO_CONSISTENCY_CHECKS)
	    /*
		If the memory to be freed overlaps with the current
		block something must be wrong.
	    */
	    if (p4>(UBYTE *)p2)
	    {
		bug("[MM] Chunk allocator error\n");
		bug("[MM] Attempt to free %u bytes at 0x%p from MemHeader 0x%p\n", byteSize, memoryBlock, freeList);
		bug("[MM] Block overlaps with chunk 0x%p (%u bytes)\n", p2, p2->mc_Bytes);

		Alert(AN_FreeTwice);
		return;
	    }
#endif
	    /* End the loop with p2 non-zero */
	    break;
	}
	/* goto next block */
	p1=p2;
	p2=p2->mc_Next;

    /* If the loop ends with p2 zero add it at the end. */
    }while(p2!=NULL);

    /* If there was a previous block merge with it. */
    if(p1!=(struct MemChunk *)&freeList->mh_First)
    {
#if !defined(NO_CONSISTENCY_CHECKS)
	/* Check if they overlap. */
	if ((UBYTE *)p1+p1->mc_Bytes>(UBYTE *)p3)
	{
	    bug("[MM] Chunk allocator error\n");
	    bug("[MM] Attempt to free %u bytes at 0x%p from MemHeader 0x%p\n", byteSize, memoryBlock, freeList);
	    bug("[MM] Block overlaps with chunk 0x%p (%u bytes)\n", p1, p1->mc_Bytes);

	    Alert(AN_FreeTwice);
	    return;
	}
#endif
	/* Merge if possible */
	if((UBYTE *)p1+p1->mc_Bytes==(UBYTE *)p3)
	    p3=p1;
	else
	    /* Not possible to merge */
	    p1->mc_Next=p3;
    }else
	/*
	    There was no previous block. Just insert the memory at
	    the start of the list.
	*/
	p1->mc_Next=p3;

    /* Try to merge with next block (if there is one ;-) ). */
    if(p4==(UBYTE *)p2&&p2!=NULL)
    {
	/*
	   Overlap checking already done. Doing it here after
	   the list potentially changed would be a bad idea.
	*/
	p4+=p2->mc_Bytes;
	p2=p2->mc_Next;
    }
    /* relink the list and return. */
    p3->mc_Next=p2;
    p3->mc_Bytes=p4-(UBYTE *)p3;
    freeList->mh_Free+=byteSize;
}

/* 
 * TODO:
 * During transition period two routines below use nommu allocator.
 * When transition is complete they should use them only if MMU
 * is inactive. Otherwise they should use KrnAllocPages()/KrnFreePages().
 */

/* Allocate a region managed by own header */
APTR AllocMemHeader(IPTR size, ULONG flags, struct ExecBase *SysBase)
{
    struct MemHeader *mh;

    mh = nommu_AllocMem(size, flags, SysBase);
    DMH(bug("[AllocMemHeader] Allocated %u bytes at 0x%p\n", size, mh));

    if (mh)
    {
        struct MemHeader *orig = FindMem(mh, SysBase);

    	size -= MEMHEADER_TOTAL;

	/*
	 * Initialize new MemHeader.
	 * Inherit attributes from system MemHeader from which
	 * our chunk was allocated.
	 */
	mh->mh_Node.ln_Type	= NT_MEMORY;
	mh->mh_Node.ln_Pri      = orig->mh_Node.ln_Pri;
	mh->mh_Attributes	= orig->mh_Attributes;
	mh->mh_Lower 	    	= (APTR)mh + MEMHEADER_TOTAL;
	mh->mh_Upper 	    	= mh->mh_Lower + size - 1;
	mh->mh_First	    	= mh->mh_Lower;
	mh->mh_Free  	    	= size;

	/* Create the first (and the only) MemChunk */
	mh->mh_First->mc_Next 	= NULL;
	mh->mh_First->mc_Bytes  = size;
    }
    return mh;
}

/* Free a region allocated by AllocMemHeader() */
void FreeMemHeader(APTR addr, struct ExecBase *SysBase)
{
    ULONG size = ((struct MemHeader *)addr)->mh_Upper - addr + 1;

    DMH(bug("[FreeMemHeader] Freeing %u bytes at 0x%p\n", size, addr));
    nommu_FreeMem(addr, size, SysBase);
}

/*
 * Allocate memory with given physical properties from the given pool.
 * Our pools can be mixed. This means that different puddles from the
 * pool can have different physical flags. For example the same pool
 * can contain puddles from both CHIP and FAST memory. This is done in
 * order to provide a single system default pool for all types of memory.
 */
APTR InternalAllocPooled(APTR poolHeader, IPTR memSize, ULONG flags, struct ExecBase *SysBase)
{
    struct ProtectedPool *pool = poolHeader + MEMHEADER_TOTAL;
    APTR ret = NULL;
    IPTR origSize;
    struct MemHeader *mh;

    D(bug("[exec] InternalAllocPooled(0x%p, %u, 0x%08X), header 0x%p\n", poolHeader, memSize, flags, pool));

    /*
     * Memory blocks allocated from the pool store pointers to the MemHeader they were
     * allocated from. This is done in order to avoid slow lookups in InternalFreePooled().
     * This is done in AllocVec()-alike manner, the pointer is placed right before the block.
     */
    memSize += sizeof(struct MemHeader *);
    origSize = memSize;

    /* If mungwall is enabled, count also size of walls */
    if (PrivExecBase(SysBase)->IntFlags & EXECF_MungWall)
        memSize += MUNGWALL_TOTAL_SIZE;

    if (pool->pool.Requirements & MEMF_SEM_PROTECTED)
    {
    	ObtainSemaphore(&pool->sem);
    }

    /* Follow the list of MemHeaders */
    mh = (struct MemHeader *)pool->pool.PuddleList.mlh_Head;
    for(;;)
    {
	ULONG physFlags = flags & MEMF_PHYSICAL_MASK;

	/* Are there no more MemHeaders? */
	if (mh->mh_Node.ln_Succ==NULL)
	{
	    /*
	     * Get a new one.
	     * Usually we allocate puddles of default size, specified during
	     * pool creation. However we can be asked to allocate block whose
	     * size will be larger than default puddle size.
	     * Previously this was handled by threshSize parameter. In our new
	     * implementation we just allocate enlarged puddle. This is done
	     * in order not to waste page tails beyond the allocated large block.
	     * These tails will be used for our pool too. Their size is smaller
	     * than page size but they still perfectly fit for small allocations
	     * (the primary use for pools).
	     * Since our large block is also a puddle, it will be reused for our
	     * pool when the block is freed. It can also be reused for another
	     * large allocation, if it fits in.
	     * Our final puddle size still includes MEMHEADER_TOTAL in any case.
	     */
	    IPTR puddleSize = pool->pool.PuddleSize;

	    if (memSize > puddleSize - MEMHEADER_TOTAL)
	    {
		IPTR align = PrivExecBase(SysBase)->PageSize - 1;

		puddleSize = memSize + MEMHEADER_TOTAL;
		/* Align the size up to page boundary */
		puddleSize = (puddleSize + align) & ~align;
	    }

	    mh = AllocMemHeader(puddleSize, flags, SysBase);
	    D(bug("[InternalAllocPooled] Allocated new puddle 0x%p, size %u\n", mh, puddleSize));

	    /* No memory left? */
	    if(mh == NULL)
		break;

	    /* Add the new puddle to our pool */
	    mh->mh_Node.ln_Name = (STRPTR)pool;
	    Enqueue((struct List *)&pool->pool.PuddleList, &mh->mh_Node);

	    /* Fall through to get the memory */
	}
	else
	{
	    /* Ignore existing MemHeaders with memory type that differ from the requested ones */
	    if (physFlags & ~mh->mh_Attributes)
	    {
		D(bug("[InternalAllocPooled] Wrong flags for puddle 0x%p (wanted 0x%08X, have 0x%08X\n", flags, mh->mh_Attributes));
	    	continue;
	    }
	}

	/* Try to get the memory */
	ret = stdAlloc(mh, memSize, flags, SysBase);
	D(bug("[InternalAllocPooled] Allocated memory at 0x%p from puddle 0x%p\n", ret, mh));

	/* Got it? */
	if (ret != NULL)
        {
            /*
	     * If this is not the first MemHeader and it has some free space,
	     * move it forward (so that the next allocation will attempt to use it first).
	     * We use Enqueue() because we still sort MemHeaders according to their priority
	     * (which they inherit from system MemHeaders).
	     *
	     * TODO: implement own Enqueue() routine with secondary sorting by mh_Free.
	     * This will allow to implement best-match algorithm (so that puddles with
	     * smaller free space will be picked up first). This way the smallest allocations
	     * will reuse smallest chunks instead of fragmenting large ones.
	     */
            if (mh->mh_Node.ln_Pred != NULL && mh->mh_Free > 32)
            {
		D(bug("[InternalAllocPooled] Re-sorting puddle list\n"));
                Remove(&mh->mh_Node);
                Enqueue((struct List *)&pool->pool.PuddleList, &mh->mh_Node);
            }

            break;
        }

	/* No. Try next MemHeader */
	mh = (struct MemHeader *)mh->mh_Node.ln_Succ;
    }

    if (pool->pool.Requirements & MEMF_SEM_PROTECTED)
    {
    	ReleaseSemaphore(&pool->sem);
    }

    if (ret)
    {
	/* Build munge walls if requested */
	ret = MungWall_Build(ret, pool, origSize, flags, SysBase);

	/* Remember where we were allocated from */
	*((struct MemHeader **)ret) = mh;
	ret += sizeof(struct MemHeader *);
    }

    /* Everything fine */
    return ret;
}

/*
 * This is a pair to InternalAllocPooled()
 * This code separated from FreePooled() in order to provide compatibility with various
 * memory tracking patches. If some exec code calls InternalAllocPooled() directly
 * (AllocMem() will do it), it has to call also InternalFreePooled() directly.
 * Our chunks remember from which pool they came, so we don't need a pointer to pool
 * header here. This will save us from headaches in future FreeMem() implementation.
 */
void InternalFreePooled(APTR memory, IPTR memSize, struct ExecBase *SysBase)
{
    struct MemHeader *mh;
    APTR freeStart;
    IPTR freeSize;

    D(bug("[exec] InternalFreePooled(0x%p, %u)\n", memory, memSize));

    if (!memory || !memSize) return;

    /* Get MemHeader pointer. It is stored right before our block. */
    freeStart = memory - sizeof(struct MemHeader *);
    freeSize = memSize + sizeof(struct MemHeader *);
    mh = *((struct MemHeader **)freeStart);

    /* Check walls first */
    freeStart = MungWall_Check(freeStart, freeSize, SysBase);
    if (PrivExecBase(SysBase)->IntFlags & EXECF_MungWall)
	freeSize += MUNGWALL_TOTAL_SIZE;

    /* Verify that MemHeader pointer is correct */
    if ((mh->mh_Node.ln_Type != NT_MEMORY) ||
	(freeStart < mh->mh_Lower) || (freeStart + freeSize > mh->mh_Upper + 1))
    {
    	/*
	 * Something is wrong.
	 * TODO: the following should actually be printed as part of the alert.
	 * In future there should be some kind of "alert context". CPU alerts
	 * (like illegal access) should remember CPU context there. Memory manager
	 * alerts (like this one) should remember some own information.
	 */
        bug("[MM] Pool manager error\n");
	bug("[MM] Attempt to free %u bytes at 0x%p\n", memSize, memory);
	bug("[MM] The chunk does not belong to a pool\n");

	Alert(AN_BadFreeAddr);
    }
    else
    {
	struct ProtectedPool *pool = (struct ProtectedPool *)mh->mh_Node.ln_Name;
	IPTR size;
	
	if (pool->pool.Requirements & MEMF_SEM_PROTECTED)
	{
	    ObtainSemaphore(&pool->sem);
	}

	size = mh->mh_Upper - mh->mh_Lower + 1;
	D(bug("[FreePooled] Allocated from puddle 0x%p, size %u\n", mh, size));

	/* Free the memory. */
	stdDealloc(mh, freeStart, freeSize, SysBase);
	D(bug("[FreePooled] Deallocated chunk, %u free bytes in the puddle\n", mh->mh_Free));

	/* Is this MemHeader completely free now? */
	if (mh->mh_Free == size)
	{
	    D(bug("[FreePooled] Puddle is empty, giving back to the system\n"));

	    /* Yes. Remove it from the list. */
	    Remove(&mh->mh_Node);
	    /* And free it. */
	    FreeMemHeader(mh, SysBase);
	}
	/* All done. */

	if (pool->pool.Requirements & MEMF_SEM_PROTECTED)
	{
    	    ReleaseSemaphore(&pool->sem);
	}
    }
}

ULONG checkMemHandlers(struct checkMemHandlersState *cmhs, struct ExecBase *SysBase)
{
    struct Node      *tmp;
    struct Interrupt *lmh;

    if (cmhs->cmhs_Data.memh_RequestFlags & MEMF_NO_EXPUNGE)
        return MEM_DID_NOTHING;

    /* In order to keep things clean, we must run in a single thread */
    ObtainSemaphore(&PrivExecBase(SysBase)->LowMemSem);

    /*
     * Loop over low memory handlers. Handlers can remove
     * themselves from the list while being invoked, thus
     * we need to be careful!
     */
    for (lmh = (struct Interrupt *)cmhs->cmhs_CurNode;
         (tmp = lmh->is_Node.ln_Succ);
         lmh = (struct Interrupt *)(cmhs->cmhs_CurNode = tmp))
    {
        ULONG ret;

        ret = AROS_UFC3 (LONG, lmh->is_Code,
                   AROS_UFCA(struct MemHandlerData *, &cmhs->cmhs_Data, A0),
                   AROS_UFCA(APTR,                     lmh->is_Data,    A1),
                   AROS_UFCA(struct ExecBase *,        SysBase,         A6)
              );

        if (ret == MEM_TRY_AGAIN)
        {
            /* MemHandler said he did something. Try again. */
            /* Is there any program that depends on this flag??? */
            cmhs->cmhs_Data.memh_Flags |= MEMHF_RECYCLE;
            
            ReleaseSemaphore(&PrivExecBase(SysBase)->LowMemSem);
            return MEM_TRY_AGAIN;
        }
        else
            /* Nothing more to expect from this handler. */
            cmhs->cmhs_Data.memh_Flags &= ~MEMHF_RECYCLE;
    }

    ReleaseSemaphore(&PrivExecBase(SysBase)->LowMemSem);
    return MEM_DID_NOTHING;
}
