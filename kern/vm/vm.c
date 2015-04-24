#include <types.h>
#include <kern/errno.h>
#include <lib.h>
#include <spl.h>
#include <thread.h>
#include <synch.h>
#include <current.h>
#include <mips/tlb.h>
#include <addrspace.h>
#include <vm.h>
#include <clock.h>
#include<uio.h>
#include <vnode.h>
#include<vfs.h>
#include <kern/fcntl.h>
#include <kern/seek.h>
#include <kern/stat.h>
#include <kern/fcntl.h>
#include <bitmap.h>
#include <wchan.h>
#define DUMBVM_STACKPAGES    12

static struct spinlock stealmem_lock = SPINLOCK_INITIALIZER;
static struct spinlock tlb_lock = SPINLOCK_INITIALIZER;
//static struct spinlock coremap_lock = SPINLOCK_INITIALIZER;

static struct vnode *swap_vnode;
static int vm_bootstrapped=0;
static paddr_t freeaddr;
static paddr_t ROUNDDOWN(paddr_t size)
{
	if(size%PAGE_SIZE!=0)
	{
	size = ((size + PAGE_SIZE - 1) & ~(size_t)(PAGE_SIZE-1));
	size-=PAGE_SIZE;
	}
	return size;
}

void page_lock(struct PTE *pg){
	spinlock_acquire(&pg->slock);
}
void page_unlock(struct PTE *pg){
	spinlock_release(&pg->slock);
}
/*static void wait_for_busypage(){
	wchan_lock(page_wchan);
	spinlock_release(&coremap_lock);
	wchan_sleep(page_wchan);
	spinlock_acquire(&coremap_lock);
}
 *
 * void page_set_busy(paddr_t pa){
	int i=get_ind_coremap(pa);
	KASSERT(i>=0);
	spinlock_acquire(&coremap_lock);
	while(core_map[i].busy){
		wait_for_busypage();
	}
	core_map[i].busy=1;
	spinlock_release(&coremap_lock);
}
void page_unset_busy(paddr_t pa){
	int i=get_ind_coremap(pa);
	KASSERT(i>=0);
	spinlock_acquire(&coremap_lock);
	//KASSERT(core_map[i].busy==1);
	core_map[i].busy=0;
	wchan_wakeall(page_wchan);
	spinlock_release(&coremap_lock);
}
int is_busy(paddr_t pa){
	int i=get_ind_coremap(pa);
	KASSERT(i>=0);
	return core_map[i].busy;
}*/
/*void page_sneek(struct PTE *pg){
	paddr_t beef=0xdeadbeef;
	paddr_t pa,temp=beef;
	page_lock(pg);
	while(1)
	{
		pa=pg->paddr & PAGE_FRAME;
		if(pg->swapped==1)
			pa=beef;
		if((unsigned int)pa==(unsigned int)temp)
			break;
		page_unlock(pg);
		if((unsigned int)temp!=(unsigned int)beef){
			page_unset_busy(temp);
		}
		if((unsigned int)pa==(unsigned int)beef){
			page_lock(pg);
			KASSERT(pg->swapped==1);
			break;
		}
		page_set_busy(pa);
		temp=pa;
		page_lock(pg);
	}
}
*/
static void getswapstats(){
	struct stat st;
	char *s=kstrdup("lhd0raw:");
	//struct coremap_page *core_map_file;
	int err=vfs_open(s,O_RDWR,0,&swap_vnode);
	if(err!=0)
			kprintf("VFS_ERROR:%d!",err);
	VOP_STAT(swap_vnode, &st);
	size_t total_swap=st.st_size/PAGE_SIZE;
	kprintf("\nSWAP MEM: %lu bytes, %d pages\n",(unsigned long)st.st_size,total_swap);
	total_swap=last_index*3;
	swap_map=bitmap_create(total_swap);
	lastsa=-1;
	//swap_map=bitmap_create(last_index+last_index+last_index+(last_index/2));
	/*swap_map=bitmap_create(2);
	vaddr_t sa;
	bitmap_alloc(swap_map, &sa);
		kprintf("\ni=%x",sa);
		bitmap_alloc(swap_map, &sa);
			kprintf("\ni=%x",sa);
			bitmap_alloc(swap_map, &sa);
				kprintf("\ni=%x",sa);
				panic("A");*/
	while(swap_map==NULL)
	{
		total_swap-=last_index;
		swap_map=bitmap_create(total_swap);
	}
	//panic("SMAP IS NULL");
}
void
vm_bootstrap(void)
{
	paddr_t first_addr, lastaddr;
	ram_getsize(&first_addr, &lastaddr);
	int total_page_num = (lastaddr-first_addr) / PAGE_SIZE;
	/* pages should be a kernel virtual address !!  */
	core_map = (struct coremap_page*)PADDR_TO_KVADDR(first_addr);
	freeaddr = first_addr + total_page_num * sizeof(struct coremap_page);
	freeaddr=ROUNDDOWN(freeaddr);

	kprintf("\n%d,%x,%x,%d\n",ROUNDDOWN(lastaddr-freeaddr) / PAGE_SIZE,first_addr,lastaddr,total_page_num);

	last_index=0;
	for(int i=0;i<total_page_num;i++)
	{
		if( ((PAGE_SIZE*i) + freeaddr) >=lastaddr){
			kprintf("\nif:%x, %x",((PAGE_SIZE*i) + freeaddr), lastaddr);
			last_index=i;
			break;
		}
		core_map[i].paddr= (PAGE_SIZE*i) +freeaddr;

		//kprintf("%x\t",core_map[i].paddr);
		core_map[i].page_ptr=NULL;
		core_map[i].pstate=FREE;
	}
	if(last_index==0)
		last_index=total_page_num;

//	global_paging=lock_create("GLOBAL PAGE LOCK");
	page_wchan=wchan_create("PAGE_WCHAN");

	kprintf("%d,correct?%x,%x",last_index,core_map[last_index-1].paddr,lastaddr);
	getswapstats();
	vm_bootstrapped=1;
	//kprintf("%d,%d,%d",getpageindex(0),getpageindex(freeaddr),getpageindex(lastaddr));
}

static void access_swap(paddr_t pa, vaddr_t sa, enum uio_rw mode){
	//kprintf("\naccess_entered\n");
	int result;
	struct iovec iov;
	struct uio ku;
	vaddr_t va=PADDR_TO_KVADDR(pa);
	/*
	bitmap_alloc(swap_map, &index);
	kprintf("\nAllocated Map index: %x",index);
	bitmap_alloc(swap_map, &index);
	kprintf("\nAllocated Map index: %x",index);
*/
	uio_kinit(&iov, &ku, (char *)va, PAGE_SIZE, sa, mode);
		/*iov.iov_ubase = (void *)s;
		iov.iov_len = sizeof(s);		 // length of the memory space
		ku.uio_iov = &iov;
		ku.uio_iovcnt = 1;
		ku.uio_resid = sizeof(s);          // amount to read from the file
		ku.uio_offset = 0;
		ku.uio_segflg = UIO_USERSPACE;
		ku.uio_rw = UIO_WRITE;
		ku.uio_space = curthread->t_addrspace;*/
	if(mode==UIO_READ)
		result=VOP_READ(swap_vnode,&ku);
	else
		result=VOP_WRITE(swap_vnode, &ku);
	if (result) {
//		kprintf("VOP_ops ERROR:%d",result);
	}

	//kprintf("\nDONE TO DISK!");
}
static void swapin(paddr_t pa, vaddr_t sa){
	//kprintf("\nswapin");

	access_swap(pa, sa, UIO_READ);

}
static void swapout(paddr_t pa, vaddr_t sa){

	//kprintf("\nswapout");
	access_swap(pa, sa, UIO_WRITE);

}


int count_free()
{
	int cnt=0;
	for(int i=0;i<last_index;i++)
		{
			if(core_map[i].pstate==FREE)
			{
				cnt++;
			}
		}
	return cnt;
}
/*static int choose_victim(int start){
	int victim_ind=-1;
	time_t aftersecs, secs;
	uint32_t afternsecs, nsecs,nh=0;
	gettime(&aftersecs, &afternsecs);

	for(int i=start;i<last_index;i++)
	{
		getinterval(core_map[i].beforesecs, core_map[i].beforensecs,
					aftersecs, afternsecs,
					&secs, &nsecs);
		nsecs=( secs*1000 ) + (nsecs/1000);
		//kprintf("\n%lu,%lu sec",(unsigned long)nh,(unsigned long)nsecs);

			if((unsigned long)nh==0 || (unsigned long)nh<(unsigned long)nsecs)
			{
				nh=nsecs;
				victim_ind=i;
			}

	}
	//kprintf("\n VICTIM IS %d",victim_ind);
	return victim_ind;
}*/
static int get_ind_coremap(paddr_t paddr)
{
	int i;
	for(i=0;i<last_index;i++)
			if(core_map[i].paddr==paddr)
				return i;
	return -1;
}
static struct PTE *choose_victim()
{
	int i;
	time_t aftersecs, secs;
	uint32_t afternsecs, nsecs,nh=0;
	struct addrspace* as=curthread->parent->t_addrspace;
	struct PTE *pages, *victim_pg=NULL;
	gettime(&aftersecs, &afternsecs);

	if(as==NULL)
		as=curthread->t_addrspace;

	if(as->heap!=NULL)
	{
	pages=as->heap->pages;
		while(pages!=NULL)
		{
			i=get_ind_coremap(pages->paddr);
			getinterval(core_map[i].beforesecs, core_map[i].beforensecs,
								aftersecs, afternsecs,
								&secs, &nsecs);
			nsecs=( secs*1000 ) + (nsecs/1000);
			//kprintf("\nH:%lu,%lu",(unsigned long)nh,(unsigned long)nsecs);
			if(core_map[i].pstate!=FIXED && ((unsigned long)nh==0 || (unsigned long)nh<(unsigned long)nsecs))
			{
				nh=nsecs;
				victim_pg=pages;
			}
			pages=pages->next;
		}
	}
	if(as->stack!=NULL)
	{
	pages=as->stack->pages;
		while(pages!=NULL)
		{
			i=get_ind_coremap(pages->paddr);
			getinterval(core_map[i].beforesecs, core_map[i].beforensecs,
								aftersecs, afternsecs,
								&secs, &nsecs);
			nsecs=( secs*1000 ) + (nsecs/1000);
			//kprintf("\nS:%lu,%lu",(unsigned long)nh,(unsigned long)nsecs);
			if(core_map[i].pstate!=FIXED && ((unsigned long)nh==0 || (unsigned long)nh<(unsigned long)nsecs))
			{
				nh=nsecs;
				victim_pg=pages;
			}
			pages=pages->next;
		}
	}
		/*while(as!=NULL)
		{
		pages=as->pages;
					while(pages!=NULL)
					{
						i=get_ind_coremap(pages->paddr);
						getinterval(core_map[i].beforesecs, core_map[i].beforensecs,
											aftersecs, afternsecs,
											&secs, &nsecs);
						nsecs=( secs*1000 ) + (nsecs/1000);
						//kprintf("\nS:%lu,%lu",(unsigned long)nh,(unsigned long)nsecs);
						if(core_map[i].pstate!=FIXED && ((unsigned long)nh==0 || (unsigned long)nh<(unsigned long)nsecs))
						{
							nh=nsecs;
							victim_pg=pages;
						}
						pages=pages->next;
					}
			as=as->next;
		}*/
	KASSERT(victim_pg!=NULL);
	return victim_pg;
}
/*static struct PTE* find_addr(struct PTE *temp, paddr_t pa){

	while(temp!=NULL)
	{
	//	kprintf("\n%x,%x",pa,temp->paddr);
		if(temp->paddr==pa){
			temp->swapped=1;
			return temp;
		}
		temp=temp->next;
	}
	return NULL;
}

static struct PTE *get_victim_page(int vind){
	struct PTE *victim_pg=NULL;
	struct thread *threads=curthread;
	struct addrspace *reg,*stack,*heap;
	int it=0;
	while(it<2)
	{
		reg=threads->t_addrspace;
		if(reg==NULL){
			break;
		}
		stack=reg->stack;
		heap=reg->heap;
		//kprintf("\nSTART");
		while(reg!=NULL){
			victim_pg=find_addr(reg->pages,core_map[vind].paddr);
			if(victim_pg!=NULL)
				break;
			reg=reg->next;
		}
		//kprintf("\nSTACK");
		if(victim_pg==NULL && stack!=NULL)
			victim_pg=find_addr(stack->pages,core_map[vind].paddr);
		//kprintf("\nHEAP");
		if(victim_pg==NULL && heap!=NULL)
			victim_pg=find_addr(heap->pages,core_map[vind].paddr);
		if(victim_pg==NULL)
			threads=threads->parent;
		it++;
	}
	KASSERT(victim_pg!=NULL);
	return victim_pg;
}*/
static int make_page_available(int npages,int kernel){
	if(npages>1)
		panic("NOO PLS");

	struct PTE *victim_pg=choose_victim();
	victim_pg->swapped=1;
	vaddr_t sa=victim_pg->saddr;
	int vind=get_ind_coremap(victim_pg->paddr);

	if(last_index-vind<npages)
		vind-=last_index-vind;

	for(int i=vind;i<vind+npages;i++)
	{
		if(kernel==1)
			core_map[i].pstate=FIXED;
		else
			core_map[i].pstate=DIRTY;
		core_map[i].npages=npages;

		vm_tlbshootdown(victim_pg->vaddr);
//		spinlock_release(&coremap_lock);
		swapout(core_map[i].paddr, sa);
//		spinlock_acquire(&coremap_lock);

	}
	return vind;
}
	/*
static int flush_page(){
		core_map[victim_ind].pstate=DIRTY;
		core_map[victim_ind].npages=1;
		gettime(&core_map[victim_ind].beforesecs, &core_map[victim_ind].beforensecs);

		found=victim_ind;
		//flush_to_disk(curthread->t_addrspace);
		//kprintf("\nFLUSHED");
		//get_from_disk(curthread->t_addrspace->pid);

}*/

/* Allocate/free some kernel-space virtual pages */
vaddr_t alloc_page(struct PTE *pg)
{
	paddr_t pa;
	//lock_acquire(global_paging);
	//spinlock_acquire(&coremap_lock);
	int found=-1;
	for(int i=0;i<last_index;i++)
	{
		if(core_map[i].pstate==FREE)
		{
			found=i;
			break;
		}
	}
	if (found==-1) {
				found=make_page_available(1,0);
		}
	core_map[found].pstate=DIRTY;
	core_map[found].npages=1;
	gettime(&core_map[found].beforesecs, &core_map[found].beforensecs);
	core_map[found].page_ptr=pg;
	pa=core_map[found].paddr;
	KASSERT(pa!=0);
	bzero((void *)PADDR_TO_KVADDR(pa), PAGE_SIZE);

	//spinlock_release(&coremap_lock);
	//lock_release(global_paging);
	return pa;
}

static
paddr_t
getppages(unsigned long npages)
{
	paddr_t addr;
	spinlock_acquire(&stealmem_lock);
	//spinlock_acquire(&stealmem_lock);

	addr = ram_stealmem(npages);
	spinlock_release(&stealmem_lock);
	//spinlock_release(&stealmem_lock);
	return addr;
}

/* Allocate/free some kernel-space virtual pages */
vaddr_t
alloc_kpages(int npages)
{
	paddr_t pa;
	if(vm_bootstrapped==0){
		pa = getppages(npages);
	}
	else
	{
		//lock_acquire(global_paging);
		//spinlock_acquire(&coremap_lock);
		int found=-1,start=-1;
		for(int i=0;i<last_index;i++)
		{
			if(core_map[i].pstate==FREE)
			{
				if(start==-1)
					start=i;
				if(i-start==npages-1)
				{
					for(int j=start;j<=i;j++)
					{
					core_map[j].pstate=FIXED;
					core_map[j].npages=npages;
					core_map[j].page_ptr=NULL;
					gettime(&core_map[j].beforesecs, &core_map[j].beforensecs);
					//bzero((void *)PADDR_TO_KVADDR(core_map[j].paddr), PAGE_SIZE);
					}
					found=i;
					break;
				}
			}
			else if(start!=-1)
			{
				start=-1;
			}
		}
		if (found==-1) {
			found=make_page_available(npages,1);
			core_map[found].pstate=FIXED;
			core_map[found].npages=npages;
			core_map[found].page_ptr=NULL;
			gettime(&core_map[found].beforesecs, &core_map[found].beforensecs);
			start=found;
		}
		pa= core_map[start].paddr;
		//spinlock_release(&coremap_lock);
		//lock_release(global_paging);
	}

	return PADDR_TO_KVADDR(pa);
}

void
free_page(paddr_t addr)
{
	int i=get_ind_coremap(addr);
	//spinlock_acquire(&coremap_lock);
	for(int j=i;j<i+core_map[i].npages;j++)
	{
		vm_tlbshootdown(PADDR_TO_KVADDR(core_map[j].paddr));
		core_map[j].pstate=FREE;
		core_map[j].page_ptr=NULL;
	}

	//spinlock_release(&coremap_lock);
}

void
free_kpages(vaddr_t addr)
{
	paddr_t pa=KVADDR_TO_PADDR(addr);
	free_page(pa);
}


void
vm_tlbshootdown_all(void)
{
	int i, spl;
	spl=splhigh();
	for(i=0;i<NUM_TLB;i++)
		tlb_write(TLBHI_INVALID(i),TLBLO_INVALID(),i);

	splx(spl);
}

void
vm_tlbshootdown(vaddr_t va)
{
	uint32_t ehi, elo;
	int spl=splhigh(),i;
	i=tlb_probe(va & PAGE_FRAME,0);
	if(i==-1){
		for(i=0;i<NUM_TLB;i++)
		{
			tlb_read(&ehi, &elo, i);
			if(ehi==va)
				tlb_write(TLBHI_INVALID(i),TLBLO_INVALID(),i);
		}
	}
	tlb_write(TLBHI_INVALID(i),TLBLO_INVALID(),i);
	splx(spl);
}
/*
static vaddr_t getfirst10(vaddr_t addr){

	//unsigned int myuint32=0xADADADAD;
	unsigned int highest_10_bits = (addr & (0x1FFFF << (32 - 10))) >> (32 - 10);
	//unsigned int highest_20_bits = (addr & (0x1FFFF << (32 - 20))) >> (32 - 20);
	//unsigned int second_10_bits = (highest_20_bits & (0x3FF));
	return highest_10_bits;
	//printf("\n%d,%d",highest_10_bits,second_10_bits);
}

static vaddr_t getsecond10(vaddr_t addr){

	//unsigned int myuint32=0xADADADAD;
	unsigned int highest_20_bits = (addr & (0x1FFFF << (32 - 20))) >> (32 - 20);
	unsigned int second_10_bits = (highest_20_bits & (0x3FF));
	return second_10_bits;
	//printf("\n%d,%d",highest_10_bits,second_10_bits);
}*/

int
vm_fault(int faulttype, vaddr_t faultaddress)
{
	//vaddr_t vbase1, vtop1, vbase2, vtop2, stackbase, stacktop;
	paddr_t paddr=0;
	int i;
	uint32_t ehi, elo;
	struct addrspace *as;
	int spl;

	faultaddress &= PAGE_FRAME;
	DEBUG(DB_VM, "dumbvm: fault: 0x%x\n", faultaddress);

	switch (faulttype) {
	    case VM_FAULT_READONLY:
		/* We always create pages read-write, so we can't get this */
		panic("dumbvm: got VM_FAULT_READONLY\n");
	    case VM_FAULT_READ:
	    case VM_FAULT_WRITE:
		break;
	    default:
		return EINVAL;
	}

	as = curthread->t_addrspace;
	if (as == NULL) {
		/*
		 * No address space set up. This is probably a kernel
		 * fault early in boot. Return EFAULT so as to panic
		 * instead of getting into an infinite faulting loop.
		 */
		return EFAULT;
	}

	/* Assert that the address space has been set up properly. */
	/*KASSERT(as->as_vbase1 != 0);
	KASSERT(as->as_pbase1 != 0);
	KASSERT(as->as_npages1 != 0);
	KASSERT(as->as_vbase2 != 0);
	KASSERT(as->as_pbase2 != 0);
	KASSERT(as->as_npages2 != 0);
	KASSERT(as->as_stackpbase != 0);
	KASSERT((as->as_vbase1 & PAGE_FRAME) == as->as_vbase1);
	KASSERT((as->as_pbase1 & PAGE_FRAME) == as->as_pbase1);
	KASSERT((as->as_vbase2 & PAGE_FRAME) == as->as_vbase2);
	KASSERT((as->as_pbase2 & PAGE_FRAME) == as->as_pbase2);
	KASSERT((as->as_stackpbase & PAGE_FRAME) == as->as_stackpbase);

	vbase1 = as->as_vbase1;
	vtop1 = vbase1 + as->as_npages1 * PAGE_SIZE;
	vbase2 = as->as_vbase2;
	vtop2 = vbase2 + as->as_npages2 * PAGE_SIZE;
	stackbase = USERSTACK - DUMBVM_STACKPAGES * PAGE_SIZE;
	stacktop = USERSTACK;*/
	struct addrspace *rtemp=as;
	struct PTE *temp,*page=NULL;
	int found=0;
	vaddr_t base,top;
	while(rtemp!=NULL)
	{
		temp=rtemp->pages;

		while(temp!=NULL){
			base=temp->vaddr;
			top=base+PAGE_SIZE;

			if(faultaddress>=base && faultaddress<top)
			{
				paddr=faultaddress-temp->vaddr + temp->paddr;
				page=temp;
				found=1;
				break;
			}
			temp=temp->next;
		}
		if(found==1)
			break;
		rtemp=rtemp->next;
	}
	if(found==0 && as->stack!=NULL)
		{
			temp=as->stack->pages;
			while(temp!=NULL){
				if(faultaddress>=temp->vaddr && faultaddress<temp->vaddr+PAGE_SIZE)
					{
						paddr=faultaddress-temp->vaddr + temp->paddr;
						page=temp;
						found=1;
						break;
					}
					temp=temp->next;
			}
		}
	if(found==0 && as->heap!=NULL)
		{
			temp=as->heap->pages;
			while(temp!=NULL){
				if(faultaddress>=temp->vaddr && faultaddress<temp->vaddr+PAGE_SIZE)
					{
						paddr=faultaddress-temp->vaddr + temp->paddr;
						page=temp;
						found=1;
						break;
					}
					temp=temp->next;
			}
		}

	if(paddr==0)
	{
		//paddr=alloc_page();
		return EFAULT;
	}
	else //if(page!=NULL)
	{
		if(page->swapped==1)
		{
			swapin(paddr,page->saddr);
			page->swapped=0;
		}
	}
	/*int ind=getfirst10(faultaddress);
	if(as->pgdir[ind].pg_table==NULL){
		as->pgdir[ind].pg_table=(vaddr_t *)kmalloc(1024*sizeof(vaddr_t));
		paddr=KVADDR_TO_PADDR(faultaddress);
		as->pgdir[ind].pg_table[getsecond10(faultaddress)]=paddr;
	}
	else{
		paddr=as->pgdir[ind].pg_table[getsecond10(faultaddress)];
		paddr=(faultaddress-PADDR_TO_KVADDR(paddr))+paddr;
	}

*/
	/*if (faultaddress >= vbase1 && faultaddress < vtop1) {
		paddr = (faultaddress - vbase1) + as->as_pbase1;
	}
	else if (faultaddress >= vbase2 && faultaddress < vtop2) {
		paddr = (faultaddress - vbase2) + as->as_pbase2;
	}
	else if (faultaddress >= stackbase && faultaddress < stacktop) {
		paddr = (faultaddress - stackbase) + as->as_stackpbase;
	}
	else {
		return EFAULT;
	}*/

	/* make sure it's page-aligned */
	KASSERT((paddr & PAGE_FRAME) == paddr);

	/* Disable interrupts on this CPU while frobbing the TLB. */
	spl = splhigh();
	spinlock_acquire(&tlb_lock);
	i=tlb_probe(faultaddress & PAGE_FRAME,0);
	if(i!=-1){
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		spinlock_release(&tlb_lock);
		splx(spl);
		return 0;
	}
	for (i=0; i<NUM_TLB; i++) {
		tlb_read(&ehi, &elo, i);
		if (elo & TLBLO_VALID) {
			continue;
		}
		ehi = faultaddress;
		elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
		DEBUG(DB_VM, "dumbvm: 0x%x -> 0x%x\n", faultaddress, paddr);
		tlb_write(ehi, elo, i);
		spinlock_release(&tlb_lock);
		splx(spl);
		return 0;
	}
	ehi = faultaddress;
	elo = paddr | TLBLO_DIRTY | TLBLO_VALID;
	tlb_random(ehi,elo);
	//panic("dumbvm: Ran out of TLB entries - cannot handle page fault\n");
	spinlock_release(&tlb_lock);
	splx(spl);
	return 0;
}
