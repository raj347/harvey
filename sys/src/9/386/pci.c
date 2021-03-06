/*
 * This file is part of the UCB release of Plan 9. It is subject to the license
 * terms in the LICENSE file found in the top-level directory of this
 * distribution and at http://akaros.cs.berkeley.edu/files/Plan9License. No
 * part of the UCB release of Plan 9, including this file, may be copied,
 * modified, propagated, or distributed except according to the terms contained
 * in the LICENSE file.
 */

/*
 * PCI support code.
 * Needs a massive rewrite.
 */
#include "u.h"
#include "../port/lib.h"
#include "mem.h"
#include "dat.h"
#include "fns.h"

#include "io.h"

enum
{
	PciADDR		= 0xCF8,	/* CONFIG_ADDRESS */
	PciDATA		= 0xCFC,	/* CONFIG_DATA */

	Maxfn			= 7,
	Maxdev			= 31,
	Maxbus			= 255,

	/* command register */
	IOen		= (1<<0),
	MEMen		= (1<<1),
	MASen		= (1<<2),
	MemWrInv	= (1<<4),
	PErrEn		= (1<<6),
	SErrEn		= (1<<8),

	Write,
	Read,
};

static Lock pcicfglock;
static Lock pcicfginitlock;
static int pcicfgmode = -1;
static Pcidev* pciroot;
static Pcidev* pcilist;
static Pcidev* pcitail;

static char* bustypes[] = {
	"CBUSI",
	"CBUSII",
	"EISA",
	"FUTURE",
	"INTERN",
	"ISA",
	"MBI",
	"MBII",
	"MCA",
	"MPI",
	"MPSA",
	"NUBUS",
	"PCI",
	"PCMCIA",
	"TC",
	"VL",
	"VME",
	"XPRESS",
};

static	int	pcicfgrw(int, int, int, int, int);

static int
tbdffmt(Fmt* fmt)
{
	char *p;
	int l, r;
	uint type, tbdf;

	if((p = malloc(READSTR)) == nil)
		return fmtstrcpy(fmt, "(tbdfconv)");

	switch(fmt->r){
	case 'T':
		tbdf = va_arg(fmt->args, uint);
		type = BUSTYPE(tbdf);
		if(type < nelem(bustypes))
			l = snprint(p, READSTR, bustypes[type]);
		else
			l = snprint(p, READSTR, "%d", type);
		snprint(p+l, READSTR-l, ".%d.%d.%d",
			BUSBNO(tbdf), BUSDNO(tbdf), BUSFNO(tbdf));
		break;

	default:
		snprint(p, READSTR, "(tbdfconv)");
		break;
	}
	r = fmtstrcpy(fmt, p);
	free(p);

	return r;
}

static uint32_t
pcibarsize(Pcidev *p, int rno)
{
	uint32_t v, size;

	v = pcicfgr32(p, rno);
	pcicfgw32(p, rno, 0xFFFFFFF0);
	size = pcicfgr32(p, rno);
	if(v & 1)
		size |= 0xFFFF0000;
	pcicfgw32(p, rno, v);

	return -(size & ~0x0F);
}

static int
pcilscan(int bno, Pcidev** list)
{
	Pcidev *p, *head, *tail;
	int dno, fno, i, hdt, l, maxfno, maxubn, sbn, tbdf, ubn;

	maxubn = bno;
	head = nil;
	tail = nil;
	for(dno = 0; dno <= Maxdev; dno++){
		maxfno = 0;
		for(fno = 0; fno <= maxfno; fno++){
			/*
			 * For this possible device, form the
			 * bus+device+function triplet needed to address it
			 * and try to read the vendor and device ID.
			 * If successful, allocate a device struct and
			 * start to fill it in with some useful information
			 * from the device's configuration space.
			 */
			tbdf = MKBUS(BusPCI, bno, dno, fno);
			l = pcicfgrw(tbdf, PciVID, 0, Read, 4);
			if(l == 0xFFFFFFFF || l == 0)
				continue;
			p = malloc(sizeof(*p));
			p->tbdf = tbdf;
			p->vid = l;
			p->did = l>>16;

			if(pcilist != nil)
				pcitail->list = p;
			else
				pcilist = p;
			pcitail = p;

			p->pcr = pcicfgr16(p, PciPCR);
			p->rid = pcicfgr8(p, PciRID);
			p->ccrp = pcicfgr8(p, PciCCRp);
			p->ccru = pcicfgr8(p, PciCCRu);
			p->ccrb = pcicfgr8(p, PciCCRb);
			p->cls = pcicfgr8(p, PciCLS);
			p->ltr = pcicfgr8(p, PciLTR);

			p->intl = pcicfgr8(p, PciINTL);

			/*
			 * If the device is a multi-function device adjust the
			 * loop count so all possible functions are checked.
			 */
			hdt = pcicfgr8(p, PciHDT);
			if(hdt & 0x80)
				maxfno = Maxfn;

			/*
			 * If appropriate, read the base address registers
			 * and work out the sizes.
			 */
			switch(p->ccrb) {
			default:
				if((hdt & 0x7F) != 0)
					break;
				for(i = 0; i < nelem(p->mem); i++) {
					p->mem[i].bar = pcicfgr32(p, PciBAR0+4*i);
					p->mem[i].size = pcibarsize(p, PciBAR0+4*i);
				}
				break;

			case 0x00:
			case 0x05:		/* memory controller */
			case 0x06:		/* bridge device */
				break;
			}

			if(head != nil)
				tail->link = p;
			else
				head = p;
			tail = p;
		}
	}

	*list = head;
	for(p = head; p != nil; p = p->link){
		/*
		 * Find PCI-PCI bridges and recursively descend the tree.
		 */
		if(p->ccrb != 0x06 || p->ccru != 0x04)
			continue;

		/*
		 * If the secondary or subordinate bus number is not
		 * initialised try to do what the PCI BIOS should have
		 * done and fill in the numbers as the tree is descended.
		 * On the way down the subordinate bus number is set to
		 * the maximum as it's not known how many buses are behind
		 * this one; the final value is set on the way back up.
		 */
		sbn = pcicfgr8(p, PciSBN);
		ubn = pcicfgr8(p, PciUBN);

		if(sbn == 0 || ubn == 0) {
			print("%T: unconfigured bridge\n", p->tbdf);

			sbn = maxubn+1;
			/*
			 * Make sure memory, I/O and master enables are
			 * off, set the primary, secondary and subordinate
			 * bus numbers and clear the secondary status before
			 * attempting to scan the secondary bus.
			 *
			 * Initialisation of the bridge should be done here.
			 */
			pcicfgw32(p, PciPCR, 0xFFFF0000);
			pcicfgw32(p, PciPBN, Maxbus<<16 | sbn<<8 | bno);
			pcicfgw16(p, PciSPSR, 0xFFFF);
			maxubn = pcilscan(sbn, &p->bridge);
			pcicfgw32(p, PciPBN, maxubn<<16 | sbn<<8 | bno);
		}
		else {
			/*
			 * You can't go back.
			 * This shouldn't be possible, but the
			 * Iwill DK8-HTX seems to have subordinate
			 * bus numbers which get smaller on the
			 * way down. Need to look more closely at
			 * this.
			 */
			if(ubn > maxubn)
				maxubn = ubn;
			pcilscan(sbn, &p->bridge);
		}
	}

	return maxubn;
}

static uint8_t
pIIxget(Pcidev *router, uint8_t link)
{
	uint8_t pirq;

	/* link should be 0x60, 0x61, 0x62, 0x63 */
	pirq = pcicfgr8(router, link);
	return (pirq < 16)? pirq: 0;
}

static void
pIIxset(Pcidev *router, uint8_t link, uint8_t irq)
{
	pcicfgw8(router, link, irq);
}

static uint8_t
viaget(Pcidev *router, uint8_t link)
{
	uint8_t pirq;

	/* link should be 1, 2, 3, 5 */
	pirq = (link < 6)? pcicfgr8(router, 0x55 + (link>>1)): 0;

	return (link & 1)? (pirq >> 4): (pirq & 15);
}

static void
viaset(Pcidev *router, uint8_t link, uint8_t irq)
{
	uint8_t pirq;

	pirq = pcicfgr8(router, 0x55 + (link >> 1));
	pirq &= (link & 1)? 0x0f: 0xf0;
	pirq |= (link & 1)? (irq << 4): (irq & 15);
	pcicfgw8(router, 0x55 + (link>>1), pirq);
}

typedef struct Bridge Bridge;
struct Bridge
{
	uint16_t	vid;
	uint16_t	did;
	uint8_t	(*get)(Pcidev *, uint8_t);
	void	(*set)(Pcidev *, uint8_t, uint8_t);
};

static Bridge southbridges[] = {
	{ 0x8086, 0xffff, pIIxget, pIIxset },	// Intel *
	{ 0x1106, 0x3227, viaget, viaset },	// Viatech VT8237

	{ 0x1022, 0x746B, nil, nil },		// AMD 8111
	{ 0x10DE, 0x00D1, nil, nil },		// NVIDIA nForce 3
	{ 0x1166, 0x0200, nil, nil },		// ServerWorks ServerSet III LE
	{ 0x1002, 0x4377, nil, nil },		// ATI Radeon Xpress 200M
};

typedef struct Slot Slot;
struct Slot {
	uint8_t	bus;			// Pci bus number
	uint8_t	dev;			// Pci device number
	uint8_t	maps[12];		// Avoid structs!  Link and mask.
	uint8_t	slot;			// Add-in/built-in slot
	uint8_t	reserved;
};

typedef struct Router Router;
struct Router {
	uint8_t	signature[4];		// Routing table signature
	uint8_t	version[2];		// Version number
	uint8_t	size[2];		// Total table size
	uint8_t	bus;			// Interrupt router bus number
	uint8_t	devfn;			// Router's devfunc
	uint8_t	pciirqs[2];		// Exclusive PCI irqs
	uint8_t	compat[4];		// Compatible PCI interrupt router
	uint8_t	miniport[4];		// Miniport data
	uint8_t	reserved[11];
	uint8_t	checksum;
};


static void
pcirouting(void)
{
	uint8_t *p, pin, irq, link, *map;
	int size, i, fn, tbdf;
	Bridge *southbridge;
	Pcidev *sbpci, *pci;
	Router *r;
	Slot *e;

	// Search for PCI interrupt routing table in BIOS
	for(p = (uint8_t *)KADDR(0xf0000); p < (uint8_t *)KADDR(0xfffff); p += 16)
		if(p[0] == '$' && p[1] == 'P' && p[2] == 'I' && p[3] == 'R')
			break;

	if(p >= (uint8_t *)KADDR(0xfffff))
		return;

	r = (Router *)p;

	if(0)
		print("PCI interrupt routing table version %d.%d at %.6llux\n",
			r->version[0], r->version[1], (uintptr_t)r & 0xfffff);

	tbdf = (BusPCI << 24)|(r->bus << 16)|(r->devfn << 8);
	sbpci = pcimatchtbdf(tbdf);
	if(sbpci == nil) {
		print("pcirouting: Cannot find south bridge %T\n", tbdf);
		return;
	}

	for(i = 0; i != nelem(southbridges); i++)
		if(sbpci->vid == southbridges[i].vid
		&& (sbpci->did == southbridges[i].did || southbridges[i].did == 0xffff))
			break;

	if(i == nelem(southbridges)) {
		print("pcirouting: ignoring south bridge %T %.4ux/%.4ux\n", tbdf, sbpci->vid, sbpci->did);
		return;
	}
	southbridge = &southbridges[i];
	if(southbridge->get == nil || southbridge->set == nil)
		return;

	size = (r->size[1] << 8)|r->size[0];
	for(e = (Slot *)&r[1]; (uint8_t *)e < p + size; e++) {
		if(0){
			print("%.2ux/%.2ux %.2ux: ", e->bus, e->dev, e->slot);
			for (i = 0; i != 4; i++) {
				uint8_t *m = &e->maps[i * 3];
				print("[%d] %.2ux %.4ux ",
					i, m[0], (m[2] << 8)|m[1]);
			}
			print("\n");
		}

		for(fn = 0; fn <= Maxfn; fn++) {
			tbdf = MKBUS(BusPCI, e->bus, e->dev, fn);
			pci = pcimatchtbdf(tbdf);
			if(pci == nil)
				continue;
			pin = pcicfgr8(pci, PciINTP);
			if(pin == 0 || pin == 0xff)
				continue;

			map = &e->maps[(pin - 1) * 3];
			link = map[0];
			irq = southbridge->get(sbpci, link);
			if(irq == 0 || irq == pci->intl)
				continue;
			if(pci->intl != 0 && pci->intl != 0xFF) {
				print("pcirouting: BIOS workaround: %T at pin %d link %d irq %d -> %d\n",
					  tbdf, pin, link, irq, pci->intl);
				southbridge->set(sbpci, link, pci->intl);
				continue;
			}
			print("pcirouting: %T at pin %d link %d irq %d\n", tbdf, pin, link, irq);
			pcicfgw8(pci, PciINTL, irq);
			pci->intl = irq;
		}
	}
}

static void
pcireservemem(void)
{
	int i;
	Pcidev *p;

	for(p = nil; p = pcimatch(p, 0, 0); )
		for(i=0; i<nelem(p->mem); i++)
			if(p->mem[i].bar && (p->mem[i].bar&1) == 0)
				asmmapinit(p->mem[i].bar&~0x0F, p->mem[i].size, 5);
}

static void
pcicfginit(void)
{
	int sbno, bno, n;
	Pcidev **list, *p;

	if(pcicfgmode != -1)
		return;
	lock(&pcicfginitlock);
	if(pcicfgmode != -1){
		unlock(&pcicfginitlock);
		return;
	}

	fmtinstall('T', tbdffmt);

	/*
	 * Try to determine if PCI Mode1 configuration implemented.
	 * (Bits [30:24] of PciADDR must be 0, according to the spec.)
	 * Mode2 won't appear in 64-bit machines.
	 */
	n = inl(PciADDR);
	if(!(n & 0x7F000000)){
		outl(PciADDR, 0x80000000);
		outb(PciADDR+3, 0);
		if(inl(PciADDR) & 0x80000000)
			pcicfgmode = 1;
	}
	outl(PciADDR, n);

	if(pcicfgmode < 0){
		unlock(&pcicfginitlock);
		return;
	}

	list = &pciroot;
	for(bno = 0; bno <= Maxbus; bno++) {
		sbno = bno;
		bno = pcilscan(bno, list);

		while(*list)
			list = &(*list)->link;
		if(sbno != 0)
			continue;
		/*
		 * If we have found a PCI-to-Cardbus bridge, make sure
		 * it has no valid mappings anymore.
		 */
		for(p = pciroot; p != nil; p = p->link){
			if (p->ccrb == 6 && p->ccru == 7) {
				/* reset the cardbus */
				pcicfgw16(p, PciBCR, 0x40 | pcicfgr16(p, PciBCR));
				delay(50);
			}
		}
	}

	// no longer.
	//if(pciroot != nil && getconf("*nopcirouting") == nil)
	pcirouting();
	pcireservemem();
	unlock(&pcicfginitlock);

	//if(getconf("*pcihinv"))
	pcihinv(nil);
}

static int
pcicfgrw(int tbdf, int r, int data, int rw, int w)
{
	int o, x, er;

	pcicfginit();
	if(pcicfgmode != 1)
		return -1;
	if(BUSDNO(tbdf) > Maxdev)
		return -1;

	lock(&pcicfglock);
	o = r & 4-w;
	er = r&0xfc | (r & 0xf00)<<16;
	outl(PciADDR, 0x80000000|BUSBDF(tbdf)|er);
	if(rw == Read){
		x = -1;
		switch(w){
		case 1:
			x = inb(PciDATA+o);
			break;
		case 2:
			x = ins(PciDATA+o);
			break;
		case 4:
			x = inl(PciDATA+o);
			break;
		}
	}else{
		x = 0;
		switch(w){
		case 1:
			outb(PciDATA+o, data);
			break;
		case 2:
			outs(PciDATA+o, data);
			break;
		case 4:
			outl(PciDATA+o, data);
			break;
		}
	}
//	outl(PciADDR, 0);
	unlock(&pcicfglock);

	return x;
}

int
pcicfgr8(Pcidev *p, int rno)
{
	return pcicfgrw(p->tbdf, rno, 0, Read, 1);
}

void
pcicfgw8(Pcidev *p, int rno, int data)
{
	pcicfgrw(p->tbdf, rno, data, Write, 1);
}

int
pcicfgr16(Pcidev *p, int rno)
{
	return pcicfgrw(p->tbdf, rno, 0, Read, 2);
}

void
pcicfgw16(Pcidev *p, int rno, int data)
{
	pcicfgrw(p->tbdf, rno, data, Write, 2);
}

int
pcicfgr32(Pcidev *p, int rno)
{
	return pcicfgrw(p->tbdf, rno, 0, Read, 4);
}

void
pcicfgw32(Pcidev *p, int rno, int data)
{
	pcicfgrw(p->tbdf, rno, data, Write, 4);
}

Pcidev*
pcimatch(Pcidev* prev, int vid, int did)
{
	pcicfginit();
	prev = prev? prev->list: pcilist;
	for(; prev != nil; prev = prev->list){
		if((vid == 0 || prev->vid == vid)
		&& (did == 0 || prev->did == did))
			break;
	}
	return prev;
}

Pcidev*
pcimatchtbdf(int tbdf)
{
	Pcidev *p;

	for(p = nil; p = pcimatch(p, 0, 0); )
		if(p->tbdf == tbdf)
			break;
	return p;
}

static void
pcilhinv(Pcidev* p)
{
	int i;
	Pcidev *t;

	for(t = p; t != nil; t = t->link) {
		print("%d  %2d/%d %.2ux %.2ux %.2ux %.4ux %.4ux %3d  ",
			BUSBNO(t->tbdf), BUSDNO(t->tbdf), BUSFNO(t->tbdf),
			t->ccrb, t->ccru, t->ccrp, t->vid, t->did, t->intl);

		for(i = 0; i < nelem(p->mem); i++) {
			if(t->mem[i].size == 0)
				continue;
			print("%d:%.8lux %d ", i, t->mem[i].bar, t->mem[i].size);
		}
		if(t->ioa.bar || t->ioa.size)
			print("ioa:%.8lux %d ", t->ioa.bar, t->ioa.size);
		if(t->mema.bar || t->mema.size)
			print("mema:%.8lux %d ", t->mema.bar, t->mema.size);
		if(t->bridge)
			print("->%d", BUSBNO(t->bridge->tbdf));
		print("\n");
	}
	for(; p != nil; p = p->link)
		if(p->bridge != nil)
			pcilhinv(p->bridge);
}

void
pcihinv(Pcidev* p)
{
	pcicfginit();
	lock(&pcicfginitlock);
	if(p == nil){
		p = pciroot;
		print("bus dev type vid  did intl memory\n");
	}
	pcilhinv(p);
	unlock(&pcicfginitlock);
}

void
pcireset(void)
{
	Pcidev *p;

	for(p = nil; p = pcimatch(p, 0, 0); )
		/* don't mess with the bridges */
		if(p->ccrb != 0x06)
			pciclrbme(p);
}

void
pcisetbme(Pcidev* p)
{
	p->pcr |= MASen;
	pcicfgw16(p, PciPCR, p->pcr);
}

void
pciclrbme(Pcidev* p)
{
	p->pcr &= ~MASen;
	pcicfgw16(p, PciPCR, p->pcr);
}

void
pcisetmwi(Pcidev* p)
{
	p->pcr |= MemWrInv;
	pcicfgw16(p, PciPCR, p->pcr);
}

void
pciclrmwi(Pcidev* p)
{
	p->pcr &= ~MemWrInv;
	pcicfgw16(p, PciPCR, p->pcr);
}

int
pcicap(Pcidev *p, int cap)
{
	int i, c, off;

	/* status register bit 4 has capabilities */
	if((pcicfgr16(p, PciPSR) & 1<<4) == 0)
		return -1;
	switch(pcicfgr8(p, PciHDT) & 0x7f){
	default:
		return -1;
	case 0:				/* etc */
	case 1:				/* pci to pci bridge */
		off = 0x34;
		break;
	case 2:				/* cardbus bridge */
		off = 0x14;
		break;
	}
	for(i = 48; i--;){
		off = pcicfgr8(p, off);
		if(off < 0x40 || (off & 3))
			break;
		off &= ~3;
		c = pcicfgr8(p, off);
		if(c == 0xff)
			break;
		if(c == cap)
			return off;
		off++;
	}
	return -1;
}

enum {
	Pmgcap	= 2,		/* capabilities; 2 bytes*/
	Pmgctl	= 4,		/* ctl/status; 2 bytes */
	Pmgbrg	= 6,		/* bridge support */
	Pmgdata	= 7,
};

int
pcigetpms(Pcidev* p)
{
	int ptr;

	if((ptr = pcicap(p, PciCapPMG)) == -1)
		return -1;
	return pcicfgr16(p, ptr+Pmgctl) & 0x0003;
}

int
pcisetpms(Pcidev* p, int state)
{
	int pmc, pmcsr, ptr;

	if((ptr = pcicap(p, PciCapPMG)) == -1)
		return -1;

	pmc = pcicfgr16(p, ptr+Pmgcap);
	pmcsr = pcicfgr16(p, ptr+Pmgctl);

	switch(state){
	default:
		return -1;
	case 0:
		break;
	case 1:
		if(!(pmc & 0x0200))
			return -1;
		break;
	case 2:
		if(!(pmc & 0x0400))
			return -1;
		break;
	case 3:
		break;
	}
	pcicfgw16(p, ptr+4, (pmcsr & ~3)  | state);
	return pmcsr & 3;
}
