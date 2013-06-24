/*ET4000/W32p emulation (Diamond Stealth 32)*/
/*Known bugs :

  - Accelerator doesn't work in planar modes
*/
#include <stdlib.h>
#include "ibm.h"
#include "device.h"
#include "io.h"
#include "pci.h"
#include "video.h"
#include "vid_svga.h"
#include "vid_icd2061.h"
#include "vid_stg_ramdac.h"
#include "mem.h"

typedef struct et4000w32p_t
{
        svga_t svga;
        stg_ramdac_t ramdac;
        icd2061_t icd2061;

        int index;
        uint8_t regs[256];
        uint32_t linearbase, linearbase_old;

        uint8_t banking, banking2;
        
        /*Accelerator*/
        struct
        {
                struct
                {
                        uint32_t pattern_addr,source_addr,dest_addr,mix_addr;
                        uint16_t pattern_off,source_off,dest_off,mix_off;
                        uint8_t pixel_depth,xy_dir;
                        uint8_t pattern_wrap,source_wrap;
                        uint16_t count_x,count_y;
                        uint8_t ctrl_routing,ctrl_reload;
                        uint8_t rop_fg,rop_bg;
                        uint16_t pos_x,pos_y;
                        uint16_t error;
                        uint16_t dmin,dmaj;
                } queued,internal;
                uint32_t pattern_addr,source_addr,dest_addr,mix_addr;
                uint32_t pattern_back,source_back,dest_back,mix_back;
                int pattern_x,source_x;
                int pattern_x_back,source_x_back;
                int pattern_y,source_y;
                uint8_t status;
                uint64_t cpu_dat;
                int cpu_dat_pos;
                int pix_pos;
        } acl;

        struct
        {
                uint32_t base[3];
                uint8_t ctrl;
        } mmu;
} et4000w32p_t;

void et4000w32p_recalcmapping(et4000w32p_t *et4000);

uint8_t et4000w32p_mmu_read(uint32_t addr, void *p);
void et4000w32p_mmu_write(uint32_t addr, uint8_t val, void *p);

void et4000w32p_out(uint16_t addr, uint8_t val, void *p)
{
        et4000w32p_t *et4000 = (et4000w32p_t *)p;
        svga_t *svga = &et4000->svga;
        uint8_t old;

        pclog("et4000w32p_out: addr %04X val %02X %04X:%04X  %02X %02X\n", addr, val, CS, pc, ram[0x487], ram[0x488]);
        
/*        if (ram[0x487] == 0x62)
                fatal("mono\n");*/
//        if (!(addr==0x3D4 && (val&~1)==0xE) && !(addr==0x3D5 && (crtcreg&~1)==0xE)) pclog("ET4000W32p out %04X %02X  %04X:%04X  ",addr,val,CS,pc);
        
        if (((addr & 0xfff0) == 0x3d0 || (addr & 0xfff0) == 0x3b0) && !(svga->miscout & 1)) 
                addr ^= 0x60;
        
//        if (!(addr==0x3D4 && (val&~1)==0xE) && !(addr==0x3D5 && (crtcreg&~1)==0xE)) pclog("%04X\n",addr);
        
        switch (addr)
        {
                case 0x3c2:
                icd2061_write(&et4000->icd2061, (val >> 2) & 3);
                break;
                
                case 0x3C6: case 0x3C7: case 0x3C8: case 0x3C9:
                stg_ramdac_out(addr, val, &et4000->ramdac, svga);
                return;
                
                case 0x3CB: /*Banking extension*/
                svga->write_bank = (svga->write_bank & 0xfffff) | ((val & 1) << 20);
                svga->read_bank  = (svga->read_bank  & 0xfffff) | ((val & 0x10) << 16);
                et4000->banking2 = val;
                return;
                case 0x3CD: /*Banking*/
                svga->write_bank = (svga->write_bank & 0x100000) | ((val & 0xf) * 65536);
                svga->read_bank  = (svga->read_bank  & 0x100000) | (((val >> 4) & 0xf) * 65536);
                et4000->banking = val;
                return;
                case 0x3CF:
                switch (svga->gdcaddr & 15)
                {
                        case 6:
                        svga->gdcreg[svga->gdcaddr & 15] = val;
                        //et4k_b8000=((crtc[0x36]&0x38)==0x28) && ((gdcreg[6]&0xC)==4);
                        et4000w32p_recalcmapping(et4000);
                        return;
                }
                break;
                case 0x3D4:
                svga->crtcreg = val & 63;
                return;
                case 0x3D5:
//                pclog("Write CRTC R%02X %02X\n", crtcreg, val);
                if (svga->crtcreg <= 7 && svga->crtc[0x11] & 0x80) return;
                old = svga->crtc[svga->crtcreg];
                svga->crtc[svga->crtcreg] = val;
                if (old != val)
                {
                        if (svga->crtcreg < 0xe || svga->crtcreg > 0x10)
                        {
                                fullchange = changeframecount;
                                svga_recalctimings(svga);
                        }
                }
                if (svga->crtcreg == 0x30)
                {
                        et4000->linearbase = val * 0x400000;
//                        pclog("Linear base now at %08X %02X\n", et4000w32p_linearbase, val);
                        et4000w32p_recalcmapping(et4000);
                }
                if (svga->crtcreg == 0x36) 
                        et4000w32p_recalcmapping(et4000);
                break;

                case 0x210A: case 0x211A: case 0x212A: case 0x213A:
                case 0x214A: case 0x215A: case 0x216A: case 0x217A:
                et4000->index=val;
                return;
                case 0x210B: case 0x211B: case 0x212B: case 0x213B:
                case 0x214B: case 0x215B: case 0x216B: case 0x217B:
                et4000->regs[et4000->index] = val;
                svga->hwcursor.x     = et4000->regs[0xE0] | ((et4000->regs[0xE1] & 7) << 8);
                svga->hwcursor.y     = et4000->regs[0xE4] | ((et4000->regs[0xE5] & 7) << 8);
                svga->hwcursor.addr  = (et4000->regs[0xE8] | (et4000->regs[0xE9] << 8) | ((et4000->regs[0xEA] & 7) << 16)) << 2;
                svga->hwcursor.addr += (et4000->regs[0xE6] & 63) * 16;
                svga->hwcursor.ena   = et4000->regs[0xF7] & 0x80;
                svga->hwcursor.xoff  = et4000->regs[0xE2] & 63;
                svga->hwcursor.yoff  = et4000->regs[0xE6] & 63;
//                pclog("HWCURSOR X %i Y %i\n",svga->hwcursor_x,svga->hwcursor_y);
                return;

        }
        svga_out(addr, val, svga);
}

uint8_t et4000w32p_in(uint16_t addr, void *p)
{
        et4000w32p_t *et4000 = (et4000w32p_t *)p;
        svga_t *svga = &et4000->svga;
        uint8_t temp;
//        if (addr==0x3DA) pclog("In 3DA %04X(%06X):%04X\n",CS,cs,pc);
        
//        pclog("ET4000W32p in  %04X  %04X:%04X  ",addr,CS,pc);

        if (addr != 0x3da && addr != 0x3ba)
                pclog("et4000w32p_in: addr %04X %04X:%04X %02X %02X\n", addr, CS, pc, ram[0x487], ram[0x488]);
        
        if (((addr & 0xfff0) == 0x3d0 || (addr & 0xfff0) == 0x3b0) && !(svga->miscout & 1)) 
                addr ^= 0x60;
        
//        pclog("%04X\n",addr);
        
        switch (addr)
        {
                case 0x3c5:
                if ((svga->seqaddr & 0xf) == 7) 
                        return svga->seqregs[svga->seqaddr & 0xf] | 4;
                break;

                case 0x3C6: case 0x3C7: case 0x3C8: case 0x3C9:
                return stg_ramdac_in(addr, &et4000->ramdac, svga);

                case 0x3CB:
                return et4000->banking2;
                case 0x3CD:
                return et4000->banking;
                case 0x3D4:
                return svga->crtcreg;
                case 0x3D5:
//                pclog("Read CRTC R%02X %02X\n", crtcreg, crtc[crtcreg]);
                return svga->crtc[svga->crtcreg];
                
                case 0x3DA:
                svga->attrff = 0;
                svga->cgastat ^= 0x30;
                temp = svga->cgastat & 0x39;
                if (svga->hdisp_on) temp |= 2;
                if (!(svga->cgastat & 8)) temp |= 0x80;
//                pclog("3DA in %02X\n",temp);
                return temp;

                case 0x210A: case 0x211A: case 0x212A: case 0x213A:
                case 0x214A: case 0x215A: case 0x216A: case 0x217A:
                return et4000->index;
                case 0x210B: case 0x211B: case 0x212B: case 0x213B:
                case 0x214B: case 0x215B: case 0x216B: case 0x217B:
                if (et4000->index==0xec) 
                        return (et4000->regs[0xec] & 0xf) | 0x60; /*ET4000/W32p rev D*/
                if (et4000->index == 0xef) 
                {
                        if (PCI) return et4000->regs[0xef] | 0xe0;       /*PCI*/
                        else     return et4000->regs[0xef] | 0x60;       /*VESA local bus*/
                }
                return et4000->regs[et4000->index];
        }
        return svga_in(addr, svga);
}

void et4000w32p_recalctimings(svga_t *svga)
{
        et4000w32p_t *et4000 = (et4000w32p_t *)svga->p;
//        pclog("Recalc %08X  ",svga_ma);
        svga->ma_latch |= (svga->crtc[0x33] & 0x7) << 16;
//        pclog("SVGA_MA %08X %i\n", svga_ma, (svga_miscout >> 2) & 3);
        if (svga->crtc[0x35] & 0x02)     svga->vtotal     += 0x400;
        if (svga->crtc[0x35] & 0x04)     svga->dispend    += 0x400;
        if (svga->crtc[0x35] & 0x08)     svga->vsyncstart += 0x400;
        if (svga->crtc[0x35] & 0x10)     svga->split      += 0x400;
        if (svga->crtc[0x3F] & 0x80)     svga->rowoffset  += 0x100;
        if (svga->crtc[0x3F] & 0x01)     svga->htotal     += 256;
        if (svga->attrregs[0x16] & 0x20) svga->hdisp <<= 1;
        
        switch ((svga->miscout >> 2) & 3)
        {
                case 0: case 1: break;
                case 2: case 3: svga->clock = cpuclock / icd2061_getfreq(&et4000->icd2061, 2); break;
        }
        
        switch (svga->bpp)
        {
                case 15: case 16:
                svga->hdisp >>= 1;
                break;
                case 24:
                svga->hdisp /= 8;
                break;
        }
}

void et4000w32p_recalcmapping(et4000w32p_t *et4000)
{
        svga_t *svga = &et4000->svga;
        
        pclog("recalcmapping %p\n", svga);
        mem_removehandler(et4000->linearbase_old, 0x200000,    svga_read_linear, svga_readw_linear, svga_readl_linear,    svga_write_linear, svga_writew_linear, svga_writel_linear, svga);
        mem_removehandler(               0xa0000,  0x20000,           svga_read,        svga_readw,        svga_readl,           svga_write,        svga_writew,        svga_writel, svga);
        mem_removehandler(               0xb0000,  0x10000, et4000w32p_mmu_read,              NULL,              NULL, et4000w32p_mmu_write,               NULL,               NULL, et4000);
        if (svga->crtc[0x36] & 0x10) /*Linear frame buffer*/
        {
                mem_sethandler(et4000->linearbase, 0x200000, svga_read_linear, svga_readw_linear, svga_readl_linear, svga_write_linear, svga_writew_linear, svga_writel_linear, svga);
        }
        else
        {
                int map = (svga->gdcreg[6] & 0xc) >> 2;
                if (svga->crtc[0x36] & 0x20) map |= 4;
                if (svga->crtc[0x36] & 0x08) map |= 8;
                switch (map)
                {
                        case 0x0: case 0x4: case 0x8: case 0xC: /*128k at A0000*/
                        mem_sethandler(0xa0000, 0x20000, svga_read, svga_readw, svga_readl, svga_write, svga_writew, svga_writel, svga);
                        break;
                        case 0x1: /*64k at A0000*/
                        mem_sethandler(0xa0000, 0x10000, svga_read, svga_readw, svga_readl, svga_write, svga_writew, svga_writel, svga);
                        break;
                        case 0x2: /*32k at B0000*/
                        mem_sethandler(0xb0000, 0x08000, svga_read, svga_readw, svga_readl, svga_write, svga_writew, svga_writel, svga);
                        break;
                        case 0x3: /*32k at B8000*/
                        mem_sethandler(0xb8000, 0x08000, svga_read, svga_readw, svga_readl, svga_write, svga_writew, svga_writel, svga);
                        break;
                        case 0x5: case 0x9: case 0xD: /*64k at A0000, MMU at B8000*/
                        mem_sethandler(0xa0000, 0x10000, svga_read, svga_readw, svga_readl, svga_write, svga_writew, svga_writel, svga);
                        mem_sethandler(0xb8000, 0x08000, et4000w32p_mmu_read, NULL, NULL, et4000w32p_mmu_write, NULL, NULL, et4000);
                        break;
                        case 0x6: case 0xA: case 0xE: /*32k at B0000, MMU at A8000*/
                        mem_sethandler(0xa8000, 0x08000, et4000w32p_mmu_read, NULL, NULL, et4000w32p_mmu_write, NULL, NULL, et4000);
                        mem_sethandler(0xb0000, 0x08000, svga_read, svga_readw, svga_readl, svga_write, svga_writew, svga_writel, svga);
                        break;
                        case 0x7: case 0xB: case 0xF: /*32k at B8000, MMU at A8000*/
                        mem_sethandler(0xa8000, 0x08000, et4000w32p_mmu_read, NULL, NULL, et4000w32p_mmu_write, NULL, NULL, et4000);
                        mem_sethandler(0xb8000, 0x08000, svga_read, svga_readw, svga_readl, svga_write, svga_writew, svga_writel, svga);
                        break;
                }
//                pclog("ET4K map %02X\n", map);
        }
        et4000->linearbase_old = et4000->linearbase;
}

#define ACL_WRST 1
#define ACL_RDST 2
#define ACL_XYST 4
#define ACL_SSO  8

void et4000w32_blit_start(et4000w32p_t *et4000);
void et4000w32_blit(int count, uint32_t mix, uint32_t sdat, int cpu_input, et4000w32p_t *et4000);

void et4000w32p_mmu_write(uint32_t addr, uint8_t val, void *p)
{
        et4000w32p_t *et4000 = (et4000w32p_t *)p;
        svga_t *svga = &et4000->svga;
        int bank;
//        pclog("ET4K write %08X %02X %02X %04X(%08X):%08X\n",addr,val,et4000->acl.status,et4000->acl.internal.ctrl_routing,CS,cs,pc);
        switch (addr & 0x6000)
        {
                case 0x0000: /*MMU 0*/
                case 0x2000: /*MMU 1*/
                case 0x4000: /*MMU 2*/
                bank = (addr >> 13) & 3;
                if (et4000->mmu.ctrl & (1 << bank))
                {
                        if (!(et4000->acl.status & ACL_XYST)) return;
                        if (et4000->acl.internal.ctrl_routing & 3)
                        {
                                if ((et4000->acl.internal.ctrl_routing & 3) == 2)
                                {
                                        if (et4000->acl.mix_addr & 7)
                                           et4000w32_blit(8 - (et4000->acl.mix_addr & 7), val >> (et4000->acl.mix_addr & 7), 0, 1, et4000);
                                        else
                                           et4000w32_blit(8, val, 0, 1, et4000);
                                }
                                else if ((et4000->acl.internal.ctrl_routing & 3) == 1)
                                   et4000w32_blit(1, ~0, val, 2, et4000);
                        }
                }
                else
                {
                        svga->vram[(addr & 0x1fff) + et4000->mmu.base[bank]] = val;
                        svga->changedvram[((addr & 0x1fff) + et4000->mmu.base[bank]) >> 10] = changeframecount;
                }
                break;
                case 0x6000:
                switch (addr & 0x7fff)
                {
                        case 0x7f00: et4000->mmu.base[0] = (et4000->mmu.base[0] & 0xFFFFFF00) | val;         break;
                        case 0x7f01: et4000->mmu.base[0] = (et4000->mmu.base[0] & 0xFFFF00FF) | (val << 8);  break;
                        case 0x7f02: et4000->mmu.base[0] = (et4000->mmu.base[0] & 0xFF00FFFF) | (val << 16); break;
                        case 0x7f03: et4000->mmu.base[0] = (et4000->mmu.base[0] & 0x00FFFFFF) | (val << 24); break;
                        case 0x7f04: et4000->mmu.base[1] = (et4000->mmu.base[1] & 0xFFFFFF00) | val;         break;
                        case 0x7f05: et4000->mmu.base[1] = (et4000->mmu.base[1] & 0xFFFF00FF) | (val << 8);  break;
                        case 0x7f06: et4000->mmu.base[1] = (et4000->mmu.base[1] & 0xFF00FFFF) | (val << 16); break;
                        case 0x7f07: et4000->mmu.base[1] = (et4000->mmu.base[1] & 0x00FFFFFF) | (val << 24); break;
                        case 0x7f08: et4000->mmu.base[2] = (et4000->mmu.base[2] & 0xFFFFFF00) | val;         break;
                        case 0x7f09: et4000->mmu.base[2] = (et4000->mmu.base[2] & 0xFFFF00FF) | (val << 8);  break;
                        case 0x7f0a: et4000->mmu.base[2] = (et4000->mmu.base[2] & 0xFF00FFFF) | (val << 16); break;
                        case 0x7f0d: et4000->mmu.base[2] = (et4000->mmu.base[2] & 0x00FFFFFF) | (val << 24); break;
                        case 0x7f13: et4000->mmu.ctrl=val; break;

                        case 0x7f80: et4000->acl.queued.pattern_addr = (et4000->acl.queued.pattern_addr & 0xFFFFFF00) | val;         break;
                        case 0x7f81: et4000->acl.queued.pattern_addr = (et4000->acl.queued.pattern_addr & 0xFFFF00FF) | (val << 8);  break;
                        case 0x7f82: et4000->acl.queued.pattern_addr = (et4000->acl.queued.pattern_addr & 0xFF00FFFF) | (val << 16); break;
                        case 0x7f83: et4000->acl.queued.pattern_addr = (et4000->acl.queued.pattern_addr & 0x00FFFFFF) | (val << 24); break;
                        case 0x7f84: et4000->acl.queued.source_addr  = (et4000->acl.queued.source_addr  & 0xFFFFFF00) | val;         break;
                        case 0x7f85: et4000->acl.queued.source_addr  = (et4000->acl.queued.source_addr  & 0xFFFF00FF) | (val << 8);  break;
                        case 0x7f86: et4000->acl.queued.source_addr  = (et4000->acl.queued.source_addr  & 0xFF00FFFF) | (val << 16); break;
                        case 0x7f87: et4000->acl.queued.source_addr  = (et4000->acl.queued.source_addr  & 0x00FFFFFF) | (val << 24); break;
                        case 0x7f88: et4000->acl.queued.pattern_off  = (et4000->acl.queued.pattern_off  & 0xFF00) | val;        break;
                        case 0x7f89: et4000->acl.queued.pattern_off  = (et4000->acl.queued.pattern_off  & 0x00FF) | (val << 8); break;
                        case 0x7f8a: et4000->acl.queued.source_off   = (et4000->acl.queued.source_off   & 0xFF00) | val;        break;
                        case 0x7f8b: et4000->acl.queued.source_off   = (et4000->acl.queued.source_off   & 0x00FF) | (val << 8); break;
                        case 0x7f8c: et4000->acl.queued.dest_off     = (et4000->acl.queued.dest_off     & 0xFF00) | val;        break;
                        case 0x7f8d: et4000->acl.queued.dest_off     = (et4000->acl.queued.dest_off     & 0x00FF) | (val << 8); break;
                        case 0x7f8e: et4000->acl.queued.pixel_depth = val; break;
                        case 0x7f8f: et4000->acl.queued.xy_dir = val; break;
                        case 0x7f90: et4000->acl.queued.pattern_wrap = val; break;
                        case 0x7f92: et4000->acl.queued.source_wrap = val; break;
                        case 0x7f98: et4000->acl.queued.count_x    = (et4000->acl.queued.count_x & 0xFF00) | val;        break;
                        case 0x7f99: et4000->acl.queued.count_x    = (et4000->acl.queued.count_x & 0x00FF) | (val << 8); break;
                        case 0x7f9a: et4000->acl.queued.count_y    = (et4000->acl.queued.count_y & 0xFF00) | val;        break;
                        case 0x7f9b: et4000->acl.queued.count_y    = (et4000->acl.queued.count_y & 0x00FF) | (val << 8); break;
                        case 0x7f9c: et4000->acl.queued.ctrl_routing = val; break;
                        case 0x7f9d: et4000->acl.queued.ctrl_reload  = val; break;
                        case 0x7f9e: et4000->acl.queued.rop_bg       = val; break;
                        case 0x7f9f: et4000->acl.queued.rop_fg       = val; break;
                        case 0x7fa0: et4000->acl.queued.dest_addr = (et4000->acl.queued.dest_addr & 0xFFFFFF00) | val;         break;
                        case 0x7fa1: et4000->acl.queued.dest_addr = (et4000->acl.queued.dest_addr & 0xFFFF00FF) | (val << 8);  break;
                        case 0x7fa2: et4000->acl.queued.dest_addr = (et4000->acl.queued.dest_addr & 0xFF00FFFF) | (val << 16); break;
                        case 0x7fa3: et4000->acl.queued.dest_addr = (et4000->acl.queued.dest_addr & 0x00FFFFFF) | (val << 24);
                        et4000->acl.internal = et4000->acl.queued;
                        et4000w32_blit_start(et4000);
                        if (!(et4000->acl.queued.ctrl_routing & 0x43))
                        {
                                et4000w32_blit(0xFFFFFF, ~0, 0, 0, et4000);
                        }
                        if ((et4000->acl.queued.ctrl_routing & 0x40) && !(et4000->acl.internal.ctrl_routing & 3))
                           et4000w32_blit(4, ~0, 0, 0, et4000);
                        break;
                        case 0x7fa4: et4000->acl.queued.mix_addr = (et4000->acl.queued.mix_addr & 0xFFFFFF00) | val;         break;
                        case 0x7fa5: et4000->acl.queued.mix_addr = (et4000->acl.queued.mix_addr & 0xFFFF00FF) | (val << 8);  break;
                        case 0x7fa6: et4000->acl.queued.mix_addr = (et4000->acl.queued.mix_addr & 0xFF00FFFF) | (val << 16); break;
                        case 0x7fa7: et4000->acl.queued.mix_addr = (et4000->acl.queued.mix_addr & 0x00FFFFFF) | (val << 24); break;
                        case 0x7fa8: et4000->acl.queued.mix_off = (et4000->acl.queued.mix_off & 0xFF00) | val;        break;
                        case 0x7fa9: et4000->acl.queued.mix_off = (et4000->acl.queued.mix_off & 0x00FF) | (val << 8); break;
                        case 0x7faa: et4000->acl.queued.error   = (et4000->acl.queued.error   & 0xFF00) | val;        break;
                        case 0x7fab: et4000->acl.queued.error   = (et4000->acl.queued.error   & 0x00FF) | (val << 8); break;
                        case 0x7fac: et4000->acl.queued.dmin    = (et4000->acl.queued.dmin    & 0xFF00) | val;        break;
                        case 0x7fad: et4000->acl.queued.dmin    = (et4000->acl.queued.dmin    & 0x00FF) | (val << 8); break;
                        case 0x7fae: et4000->acl.queued.dmaj    = (et4000->acl.queued.dmaj    & 0xFF00) | val;        break;
                        case 0x7faf: et4000->acl.queued.dmaj    = (et4000->acl.queued.dmaj    & 0x00FF) | (val << 8); break;
                }
                break;
        }
}

uint8_t et4000w32p_mmu_read(uint32_t addr, void *p)
{
        et4000w32p_t *et4000 = (et4000w32p_t *)p;
        svga_t *svga = &et4000->svga;
        int bank;
        uint8_t temp;
//        pclog("ET4K read %08X %04X(%08X):%08X\n",addr,CS,cs,pc);
        switch (addr & 0x6000)
        {
                case 0x0000: /*MMU 0*/
                case 0x2000: /*MMU 1*/
                case 0x4000: /*MMU 2*/
                bank = (addr >> 13) & 3;
                if (et4000->mmu.ctrl & (1 << bank))
                {
                        temp = 0xff;
                        if (et4000->acl.cpu_dat_pos)
                        {
                                et4000->acl.cpu_dat_pos--;
                                temp = et4000->acl.cpu_dat & 0xff;
                                et4000->acl.cpu_dat >>= 8;
                        }
                        if ((et4000->acl.queued.ctrl_routing & 0x40) && !et4000->acl.cpu_dat_pos && !(et4000->acl.internal.ctrl_routing & 3))
                           et4000w32_blit(4, ~0, 0, 0, et4000);
                        /*???*/
                        return temp;
                }
                return svga->vram[(addr&0x1fff) + et4000->mmu.base[bank]];
                
                case 0x6000:
                switch (addr&0x7fff)
                {
                        case 0x7f00: return et4000->mmu.base[0];
                        case 0x7f01: return et4000->mmu.base[0] >> 8;
                        case 0x7f02: return et4000->mmu.base[0] >> 16;
                        case 0x7f03: return et4000->mmu.base[0] >> 24;
                        case 0x7f04: return et4000->mmu.base[1];
                        case 0x7f05: return et4000->mmu.base[1] >> 8;
                        case 0x7f06: return et4000->mmu.base[1] >> 16;
                        case 0x7f07: return et4000->mmu.base[1] >> 24;
                        case 0x7f08: return et4000->mmu.base[2];
                        case 0x7f09: return et4000->mmu.base[2] >> 8;
                        case 0x7f0a: return et4000->mmu.base[2] >> 16;
                        case 0x7f0b: return et4000->mmu.base[2] >> 24;
                        case 0x7f13: return et4000->mmu.ctrl;

                        case 0x7f36:
//                                pclog("Read ACL status %02X\n",et4000->acl.status);
//                        if (et4000->acl.internal.pos_x!=et4000->acl.internal.count_x || et4000->acl.internal.pos_y!=et4000->acl.internal.count_y) return et4000->acl.status | ACL_XYST;
                        return et4000->acl.status;
                        case 0x7f80: return et4000->acl.internal.pattern_addr;
                        case 0x7f81: return et4000->acl.internal.pattern_addr >> 8;
                        case 0x7f82: return et4000->acl.internal.pattern_addr >> 16;
                        case 0x7f83: return et4000->acl.internal.pattern_addr >> 24;
                        case 0x7f84: return et4000->acl.internal.source_addr;
                        case 0x7f85: return et4000->acl.internal.source_addr >> 8;
                        case 0x7f86: return et4000->acl.internal.source_addr >> 16;
                        case 0x7f87: return et4000->acl.internal.source_addr >> 24;
                        case 0x7f88: return et4000->acl.internal.pattern_off;
                        case 0x7f89: return et4000->acl.internal.pattern_off >> 8;
                        case 0x7f8a: return et4000->acl.internal.source_off;
                        case 0x7f8b: return et4000->acl.internal.source_off >> 8;
                        case 0x7f8c: return et4000->acl.internal.dest_off;
                        case 0x7f8d: return et4000->acl.internal.dest_off >> 8;
                        case 0x7f8e: return et4000->acl.internal.pixel_depth;
                        case 0x7f8f: return et4000->acl.internal.xy_dir;
                        case 0x7f90: return et4000->acl.internal.pattern_wrap;
                        case 0x7f92: return et4000->acl.internal.source_wrap;
                        case 0x7f98: return et4000->acl.internal.count_x;
                        case 0x7f99: return et4000->acl.internal.count_x >> 8;
                        case 0x7f9a: return et4000->acl.internal.count_y;
                        case 0x7f9b: return et4000->acl.internal.count_y >> 8;
                        case 0x7f9c: return et4000->acl.internal.ctrl_routing;
                        case 0x7f9d: return et4000->acl.internal.ctrl_reload;
                        case 0x7f9e: return et4000->acl.internal.rop_bg;
                        case 0x7f9f: return et4000->acl.internal.rop_fg;
                        case 0x7fa0: return et4000->acl.internal.dest_addr;
                        case 0x7fa1: return et4000->acl.internal.dest_addr >> 8;
                        case 0x7fa2: return et4000->acl.internal.dest_addr >> 16;
                        case 0x7fa3: return et4000->acl.internal.dest_addr >> 24;
                }
                return 0xff;
        }
        return 0xff;
}

static int et4000w32_max_x[8]={0,0,4,8,16,32,64,0x70000000};
static int et4000w32_wrap_x[8]={0,0,3,7,15,31,63,0xFFFFFFFF};
static int et4000w32_wrap_y[8]={1,2,4,8,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF,0xFFFFFFFF};

int bltout=0;
void et4000w32_blit_start(et4000w32p_t *et4000)
{
//        if (et4000->acl.queued.xy_dir&0x80)
//           pclog("Blit - %02X %08X (%i,%i) %08X (%i,%i) %08X (%i,%i) %i  %i %i  %02X %02X  %02X\n",et4000->acl.queued.xy_dir,et4000->acl.internal.pattern_addr,(et4000->acl.internal.pattern_addr/3)%640,(et4000->acl.internal.pattern_addr/3)/640,et4000->acl.internal.source_addr,(et4000->acl.internal.source_addr/3)%640,(et4000->acl.internal.source_addr/3)/640,et4000->acl.internal.dest_addr,(et4000->acl.internal.dest_addr/3)%640,(et4000->acl.internal.dest_addr/3)/640,et4000->acl.internal.xy_dir,et4000->acl.internal.count_x,et4000->acl.internal.count_y,et4000->acl.internal.rop_fg,et4000->acl.internal.rop_bg, et4000->acl.internal.ctrl_routing);
//           bltout=1;
//        bltout=(et4000->acl.internal.count_x==1541);
        if (!(et4000->acl.queued.xy_dir & 0x20))
           et4000->acl.internal.error = et4000->acl.internal.dmaj / 2;
        et4000->acl.pattern_addr= et4000->acl.internal.pattern_addr;
        et4000->acl.source_addr = et4000->acl.internal.source_addr;
        et4000->acl.mix_addr    = et4000->acl.internal.mix_addr;
        et4000->acl.mix_back    = et4000->acl.mix_addr;
        et4000->acl.dest_addr   = et4000->acl.internal.dest_addr;
        et4000->acl.dest_back   = et4000->acl.dest_addr;
        et4000->acl.internal.pos_x = et4000->acl.internal.pos_y = 0;
        et4000->acl.pattern_x = et4000->acl.source_x = et4000->acl.pattern_y = et4000->acl.source_y = 0;
        et4000->acl.status = ACL_XYST;
        if ((!(et4000->acl.internal.ctrl_routing & 7) || (et4000->acl.internal.ctrl_routing & 4)) && !(et4000->acl.internal.ctrl_routing & 0x40)) 
                et4000->acl.status |= ACL_SSO;
        
        if (et4000w32_wrap_x[et4000->acl.internal.pattern_wrap & 7])
        {
                et4000->acl.pattern_x = et4000->acl.pattern_addr & et4000w32_wrap_x[et4000->acl.internal.pattern_wrap & 7];
                et4000->acl.pattern_addr &= ~et4000w32_wrap_x[et4000->acl.internal.pattern_wrap & 7];
        }
        et4000->acl.pattern_back = et4000->acl.pattern_addr;
        if (!(et4000->acl.internal.pattern_wrap & 0x40))
        {
                et4000->acl.pattern_y = (et4000->acl.pattern_addr / (et4000w32_wrap_x[et4000->acl.internal.pattern_wrap & 7] + 1)) & (et4000w32_wrap_y[(et4000->acl.internal.pattern_wrap >> 4) & 7] - 1);
                et4000->acl.pattern_back &= ~(((et4000w32_wrap_x[et4000->acl.internal.pattern_wrap & 7] + 1) * et4000w32_wrap_y[(et4000->acl.internal.pattern_wrap >> 4) & 7]) - 1);
        }
        et4000->acl.pattern_x_back = et4000->acl.pattern_x;
        
        if (et4000w32_wrap_x[et4000->acl.internal.source_wrap & 7])
        {
                et4000->acl.source_x = et4000->acl.source_addr & et4000w32_wrap_x[et4000->acl.internal.source_wrap & 7];
                et4000->acl.source_addr &= ~et4000w32_wrap_x[et4000->acl.internal.source_wrap & 7];
        }
        et4000->acl.source_back = et4000->acl.source_addr;
        if (!(et4000->acl.internal.source_wrap & 0x40))
        {
                et4000->acl.source_y = (et4000->acl.source_addr / (et4000w32_wrap_x[et4000->acl.internal.source_wrap & 7] + 1)) & (et4000w32_wrap_y[(et4000->acl.internal.source_wrap >> 4) & 7] - 1);
                et4000->acl.source_back &= ~(((et4000w32_wrap_x[et4000->acl.internal.source_wrap & 7] + 1) * et4000w32_wrap_y[(et4000->acl.internal.source_wrap >> 4) & 7]) - 1);
        }
        et4000->acl.source_x_back = et4000->acl.source_x;

        et4000w32_max_x[2] = ((et4000->acl.internal.pixel_depth & 0x30) == 0x20) ? 3 : 4;
        
        et4000->acl.internal.count_x += (et4000->acl.internal.pixel_depth >> 4) & 3;
        et4000->acl.cpu_dat_pos = 0;
        et4000->acl.cpu_dat = 0;
        
        et4000->acl.pix_pos = 0;
}

void et4000w32_incx(int c, et4000w32p_t *et4000)
{
        et4000->acl.dest_addr += c;
        et4000->acl.pattern_x += c;
        et4000->acl.source_x  += c;
        et4000->acl.mix_addr  += c;
        if (et4000->acl.pattern_x >= et4000w32_max_x[et4000->acl.internal.pattern_wrap & 7])
           et4000->acl.pattern_x  -= et4000w32_max_x[et4000->acl.internal.pattern_wrap & 7];
        if (et4000->acl.source_x  >= et4000w32_max_x[et4000->acl.internal.source_wrap  & 7])
           et4000->acl.source_x   -= et4000w32_max_x[et4000->acl.internal.source_wrap  & 7];
}
void et4000w32_decx(int c, et4000w32p_t *et4000)
{
        et4000->acl.dest_addr -= c;
        et4000->acl.pattern_x -= c;
        et4000->acl.source_x  -= c;
        et4000->acl.mix_addr  -= c;
        if (et4000->acl.pattern_x < 0)
           et4000->acl.pattern_x  += et4000w32_max_x[et4000->acl.internal.pattern_wrap & 7];
        if (et4000->acl.source_x  < 0)
           et4000->acl.source_x   += et4000w32_max_x[et4000->acl.internal.source_wrap  & 7];
}
void et4000w32_incy(et4000w32p_t *et4000)
{
        et4000->acl.pattern_addr += et4000->acl.internal.pattern_off + 1;
        et4000->acl.source_addr  += et4000->acl.internal.source_off  + 1;
        et4000->acl.mix_addr     += et4000->acl.internal.mix_off     + 1;
        et4000->acl.dest_addr    += et4000->acl.internal.dest_off    + 1;
        et4000->acl.pattern_y++;
        if (et4000->acl.pattern_y == et4000w32_wrap_y[(et4000->acl.internal.pattern_wrap >> 4) & 7])
        {
                et4000->acl.pattern_y = 0;
                et4000->acl.pattern_addr = et4000->acl.pattern_back;
        }
        et4000->acl.source_y++;
        if (et4000->acl.source_y == et4000w32_wrap_y[(et4000->acl.internal.source_wrap >> 4) & 7])
        {
                et4000->acl.source_y = 0;
                et4000->acl.source_addr = et4000->acl.source_back;
        }
}
void et4000w32_decy(et4000w32p_t *et4000)
{
        et4000->acl.pattern_addr -= et4000->acl.internal.pattern_off + 1;
        et4000->acl.source_addr  -= et4000->acl.internal.source_off  + 1;
        et4000->acl.mix_addr     -= et4000->acl.internal.mix_off     + 1;
        et4000->acl.dest_addr    -= et4000->acl.internal.dest_off    + 1;
        et4000->acl.pattern_y--;
        if (et4000->acl.pattern_y < 0 && !(et4000->acl.internal.pattern_wrap & 0x40))
        {
                et4000->acl.pattern_y = et4000w32_wrap_y[(et4000->acl.internal.pattern_wrap >> 4) & 7] - 1;
                et4000->acl.pattern_addr = et4000->acl.pattern_back + (et4000w32_wrap_x[et4000->acl.internal.pattern_wrap & 7] * (et4000w32_wrap_y[(et4000->acl.internal.pattern_wrap >> 4) & 7] - 1));
        }
        et4000->acl.source_y--;
        if (et4000->acl.source_y < 0 && !(et4000->acl.internal.source_wrap & 0x40))
        {
                et4000->acl.source_y = et4000w32_wrap_y[(et4000->acl.internal.source_wrap >> 4) & 7] - 1;
                et4000->acl.source_addr = et4000->acl.source_back + (et4000w32_wrap_x[et4000->acl.internal.source_wrap & 7] *(et4000w32_wrap_y[(et4000->acl.internal.source_wrap >> 4) & 7] - 1));;
        }
}

void et4000w32_blit(int count, uint32_t mix, uint32_t sdat, int cpu_input, et4000w32p_t *et4000)
{
        svga_t *svga = &et4000->svga;
        int c,d;
        uint8_t pattern, source, dest, out;
        uint8_t rop;
        int mixdat;

        if (!(et4000->acl.status & ACL_XYST)) return;
//        if (count>400) pclog("New blit - %i,%i %06X (%i,%i) %06X %06X\n",et4000->acl.internal.count_x,et4000->acl.internal.count_y,et4000->acl.dest_addr,et4000->acl.dest_addr%640,et4000->acl.dest_addr/640,et4000->acl.source_addr,et4000->acl.pattern_addr);
        //pclog("Blit exec - %i %i %i\n",count,et4000->acl.internal.pos_x,et4000->acl.internal.pos_y);
        if (et4000->acl.internal.xy_dir & 0x80) /*Line draw*/
        {
                while (count--)
                {
                        if (bltout) pclog("%i,%i : ", et4000->acl.internal.pos_x, et4000->acl.internal.pos_y);
                        pattern = svga->vram[(et4000->acl.pattern_addr + et4000->acl.pattern_x) & 0x1fffff];
                        source  = svga->vram[(et4000->acl.source_addr  + et4000->acl.source_x)  & 0x1fffff];
                        if (bltout) pclog("%06X %06X ", (et4000->acl.pattern_addr + et4000->acl.pattern_x) & 0x1fffff, (et4000->acl.source_addr + et4000->acl.source_x) & 0x1fffff);
                        if (cpu_input == 2)
                        {
                                source = sdat & 0xff;
                                sdat >>= 8;
                        }
                        dest = svga->vram[et4000->acl.dest_addr & 0x1fffff];
                        out = 0;
                        if (bltout) pclog("%06X   ", et4000->acl.dest_addr);
                        if ((et4000->acl.internal.ctrl_routing & 0xa) == 8)
                        {
                                mixdat = svga->vram[(et4000->acl.mix_addr >> 3) & 0x1fffff] & (1 << (et4000->acl.mix_addr & 7));
                                if (bltout) pclog("%06X %02X  ", et4000->acl.mix_addr, svga->vram[(et4000->acl.mix_addr >> 3) & 0x1fffff]);
                        }
                        else
                        {
                                mixdat = mix & 1;
                                mix >>= 1; 
                                mix |= 0x80000000;
                        }
                        et4000->acl.mix_addr++;
                        rop = mixdat ? et4000->acl.internal.rop_fg : et4000->acl.internal.rop_bg;
                        for (c = 0; c < 8; c++)
                        {
                                d = (dest & (1 << c)) ? 1 : 0;
                                if (source & (1 << c))  d |= 2;
                                if (pattern & (1 << c)) d |= 4;
                                if (rop & (1 << d)) out |= (1 << c);
                        }
                        if (bltout) pclog("%06X = %02X\n", et4000->acl.dest_addr & 0x1fffff, out);
                        if (!(et4000->acl.internal.ctrl_routing & 0x40))
                        {
                                svga->vram[et4000->acl.dest_addr & 0x1fffff] = out;
                                svga->changedvram[(et4000->acl.dest_addr & 0x1fffff) >> 10] = changeframecount;
                        }
                        else
                        {
                                et4000->acl.cpu_dat |= ((uint64_t)out << (et4000->acl.cpu_dat_pos * 8));
                                et4000->acl.cpu_dat_pos++;
                        }
                        
//                        pclog("%i %i\n",et4000->acl.pix_pos,(et4000->acl.internal.pixel_depth>>4)&3);
                        et4000->acl.pix_pos++;
                        et4000->acl.internal.pos_x++;
                        if (et4000->acl.pix_pos <= ((et4000->acl.internal.pixel_depth >> 4) & 3))
                        {
                                if (et4000->acl.internal.xy_dir & 1) et4000w32_decx(1, et4000);
                                else                                 et4000w32_incx(1, et4000);
                        }
                        else
                        {
                                if (et4000->acl.internal.xy_dir & 1) 
                                        et4000w32_incx((et4000->acl.internal.pixel_depth >> 4) & 3, et4000);
                                else                       
                                        et4000w32_decx((et4000->acl.internal.pixel_depth >> 4) & 3, et4000);
                                et4000->acl.pix_pos = 0;
                                /*Next pixel*/
                                switch (et4000->acl.internal.xy_dir & 7)
                                {
                                        case 0: case 1: /*Y+*/
                                        et4000w32_incy(et4000);
                                        et4000->acl.internal.pos_y++;
                                        et4000->acl.internal.pos_x -= ((et4000->acl.internal.pixel_depth >> 4) & 3) + 1;
                                        break;
                                        case 2: case 3: /*Y-*/
                                        et4000w32_decy(et4000);
                                        et4000->acl.internal.pos_y++;
                                        et4000->acl.internal.pos_x -= ((et4000->acl.internal.pixel_depth >> 4) & 3) + 1;
                                        break;
                                        case 4: case 6: /*X+*/
                                        et4000w32_incx(((et4000->acl.internal.pixel_depth >> 4) & 3) + 1, et4000);
                                        //et4000->acl.internal.pos_x++;
                                        break;
                                        case 5: case 7: /*X-*/
                                        et4000w32_decx(((et4000->acl.internal.pixel_depth >> 4) & 3) + 1, et4000);
                                        //et4000->acl.internal.pos_x++;
                                        break;
                                }
                                et4000->acl.internal.error += et4000->acl.internal.dmin;
                                if (et4000->acl.internal.error > et4000->acl.internal.dmaj)
                                {
                                        et4000->acl.internal.error -= et4000->acl.internal.dmaj;
                                        switch (et4000->acl.internal.xy_dir & 7)
                                        {
                                                case 0: case 2: /*X+*/
                                                et4000w32_incx(((et4000->acl.internal.pixel_depth >> 4) & 3) + 1, et4000);
                                                et4000->acl.internal.pos_x++;
                                                break;
                                                case 1: case 3: /*X-*/
                                                et4000w32_decx(((et4000->acl.internal.pixel_depth >> 4) & 3) + 1, et4000);
                                                et4000->acl.internal.pos_x++;
                                                break;
                                                case 4: case 5: /*Y+*/
                                                et4000w32_incy(et4000);
                                                et4000->acl.internal.pos_y++;
                                                break;
                                                case 6: case 7: /*Y-*/
                                                et4000w32_decy(et4000);
                                                et4000->acl.internal.pos_y++;
                                                break;
                                        }
                                }
                                if (et4000->acl.internal.pos_x > et4000->acl.internal.count_x ||
                                    et4000->acl.internal.pos_y > et4000->acl.internal.count_y)
                                {
                                        et4000->acl.status = 0;
//                                        pclog("Blit line over\n");
                                        return;
                                }
                        }
                }
        }
        else
        {
                while (count--)
                {
                        if (bltout) pclog("%i,%i : ", et4000->acl.internal.pos_x, et4000->acl.internal.pos_y);
                        
                        pattern = svga->vram[(et4000->acl.pattern_addr + et4000->acl.pattern_x) & 0x1fffff];
                        source  = svga->vram[(et4000->acl.source_addr  + et4000->acl.source_x)  & 0x1fffff];
                        if (bltout) pclog("%i %06X %06X %02X %02X  ", et4000->acl.pattern_y, (et4000->acl.pattern_addr + et4000->acl.pattern_x) & 0x1fffff, (et4000->acl.source_addr + et4000->acl.source_x) & 0x1fffff, pattern, source);

                        if (cpu_input == 2)
                        {
                                source = sdat & 0xff;
                                sdat >>= 8;
                        }
                        dest = svga->vram[et4000->acl.dest_addr & 0x1fffff];
                        out = 0;
                        if (bltout) pclog("%06X %02X  %i %08X %08X  ", dest, et4000->acl.dest_addr, mix & 1, mix, et4000->acl.mix_addr);
                        if ((et4000->acl.internal.ctrl_routing & 0xa) == 8)
                        {
                                mixdat = svga->vram[(et4000->acl.mix_addr >> 3) & 0x1fffff] & (1 << (et4000->acl.mix_addr & 7));
                                if (bltout) pclog("%06X %02X  ", et4000->acl.mix_addr, svga->vram[(et4000->acl.mix_addr >> 3) & 0x1fffff]);
                        }
                        else
                        {
                                mixdat = mix & 1;
                                mix >>= 1; 
                                mix |= 0x80000000;
                        }

                        rop = mixdat ? et4000->acl.internal.rop_fg : et4000->acl.internal.rop_bg;
                        for (c = 0; c < 8; c++)
                        {
                                d = (dest & (1 << c)) ? 1 : 0;
                                if (source & (1 << c))  d |= 2;
                                if (pattern & (1 << c)) d |= 4;
                                if (rop & (1 << d)) out |= (1 << c);
                        }
                        if (bltout) pclog("%06X = %02X\n", et4000->acl.dest_addr & 0x1fffff, out);
                        if (!(et4000->acl.internal.ctrl_routing & 0x40))
                        {
                                svga->vram[et4000->acl.dest_addr & 0x1fffff] = out;
                                svga->changedvram[(et4000->acl.dest_addr & 0x1fffff) >> 10] = changeframecount;
                        }
                        else
                        {
                                et4000->acl.cpu_dat |= ((uint64_t)out << (et4000->acl.cpu_dat_pos * 8));
                                et4000->acl.cpu_dat_pos++;
                        }

                        if (et4000->acl.internal.xy_dir & 1) et4000w32_decx(1, et4000);
                        else                                 et4000w32_incx(1, et4000);

                        et4000->acl.internal.pos_x++;
                        if (et4000->acl.internal.pos_x > et4000->acl.internal.count_x)
                        {
                                if (et4000->acl.internal.xy_dir & 2)
                                {
                                        et4000w32_decy(et4000);
                                        et4000->acl.mix_back  = et4000->acl.mix_addr  = et4000->acl.mix_back  - (et4000->acl.internal.mix_off  + 1);
                                        et4000->acl.dest_back = et4000->acl.dest_addr = et4000->acl.dest_back - (et4000->acl.internal.dest_off + 1);
                                }
                                else
                                {
                                        et4000w32_incy(et4000);
                                        et4000->acl.mix_back  = et4000->acl.mix_addr  = et4000->acl.mix_back  + et4000->acl.internal.mix_off  + 1;
                                        et4000->acl.dest_back = et4000->acl.dest_addr = et4000->acl.dest_back + et4000->acl.internal.dest_off + 1;
                                }

                                et4000->acl.pattern_x = et4000->acl.pattern_x_back;
                                et4000->acl.source_x  = et4000->acl.source_x_back;

                                et4000->acl.internal.pos_y++;
                                et4000->acl.internal.pos_x = 0;
                                if (et4000->acl.internal.pos_y > et4000->acl.internal.count_y)
                                {
                                        et4000->acl.status = 0;
//                                        pclog("Blit over\n");
                                        return;
                                }
                                if (cpu_input) return;
                                if (et4000->acl.internal.ctrl_routing & 0x40)
                                {
                                        if (et4000->acl.cpu_dat_pos & 3) 
                                                et4000->acl.cpu_dat_pos += 4 - (et4000->acl.cpu_dat_pos & 3);
                                        return;
                                }
                        }
                }
        }
}


void et4000w32p_hwcursor_draw(svga_t *svga, int displine)
{
        int x, offset;
        uint8_t dat;
        offset = svga->hwcursor_latch.xoff;
        for (x = 0; x < 64 - svga->hwcursor_latch.xoff; x += 4)
        {
                dat = svga->vram[svga->hwcursor_latch.addr + (offset >> 2)];
                if (!(dat & 2))          ((uint32_t *)buffer32->line[displine])[svga->hwcursor_latch.x + x + 32]  = (dat & 1) ? 0xFFFFFF : 0;
                else if ((dat & 3) == 3) ((uint32_t *)buffer32->line[displine])[svga->hwcursor_latch.x + x + 32] ^= 0xFFFFFF;
                dat >>= 2;
                if (!(dat & 2))          ((uint32_t *)buffer32->line[displine])[svga->hwcursor_latch.x + x + 33]  = (dat & 1) ? 0xFFFFFF : 0;
                else if ((dat & 3) == 3) ((uint32_t *)buffer32->line[displine])[svga->hwcursor_latch.x + x + 33] ^= 0xFFFFFF;
                dat >>= 2;
                if (!(dat & 2))          ((uint32_t *)buffer32->line[displine])[svga->hwcursor_latch.x + x + 34]  = (dat & 1) ? 0xFFFFFF : 0;
                else if ((dat & 3) == 3) ((uint32_t *)buffer32->line[displine])[svga->hwcursor_latch.x + x + 34] ^= 0xFFFFFF;
                dat >>= 2;
                if (!(dat & 2))          ((uint32_t *)buffer32->line[displine])[svga->hwcursor_latch.x + x + 35]  = (dat & 1) ? 0xFFFFFF : 0;
                else if ((dat & 3) == 3) ((uint32_t *)buffer32->line[displine])[svga->hwcursor_latch.x + x + 35] ^= 0xFFFFFF;
                dat >>= 2;
                offset += 4;
        }
        svga->hwcursor_latch.addr += 16;
}

uint8_t et4000w32p_pci_read(int func, int addr, void *p)
{
        et4000w32p_t *et4000 = (et4000w32p_t *)p;
        svga_t *svga = &et4000->svga;

        pclog("ET4000 PCI read %08X\n", addr);

        switch (addr)
        {
                case 0x00: return 0x0c; /*Tseng Labs*/
                case 0x01: return 0x10;
                
                case 0x02: return 0x06; /*ET4000W32p Rev D*/
                case 0x03: return 0x32;
                
                case 0x04: return 0x03; /*Respond to IO and memory accesses*/

                case 0x07: return 1 << 1; /*Medium DEVSEL timing*/
                
                case 0x08: return 0; /*Revision ID*/
                case 0x09: return 0; /*Programming interface*/
                
                case 0x0a: return 0x01; /*Supports VGA interface, XGA compatible*/
                case 0x0b: return 0x03;
                
                case 0x10: return 0x00; /*Linear frame buffer address*/
                case 0x11: return 0x00;
                case 0x12: return svga->crtc[0x5a] & 0x80;
                case 0x13: return svga->crtc[0x59];

                case 0x30: return 0x01; /*BIOS ROM address*/
                case 0x31: return 0x00;
                case 0x32: return 0x0C;
                case 0x33: return 0x00;
        }
        return 0;
}

void et4000w32p_pci_write(int func, int addr, uint8_t val, void *p)
{
        et4000w32p_t *et4000 = (et4000w32p_t *)p;

        switch (addr)
        {
                case 0x13: 
                et4000->linearbase = val << 24; 
                et4000w32p_recalcmapping(et4000); 
                break;
        }
}

void *et4000w32p_init()
{
        et4000w32p_t *et4000 = malloc(sizeof(et4000w32p_t));
        memset(et4000, 0, sizeof(et4000w32p_t));

        svga_init(&et4000->svga, et4000, 1 << 21, /*2mb*/
                   et4000w32p_recalctimings,
                   et4000w32p_in, et4000w32p_out,
                   et4000w32p_hwcursor_draw); 

        io_sethandler(0x03c0, 0x0020, et4000w32p_in, NULL, NULL, et4000w32p_out, NULL, NULL, et4000);

        io_sethandler(0x210A, 0x0002, et4000w32p_in, NULL, NULL, et4000w32p_out, NULL, NULL, et4000);
        io_sethandler(0x211A, 0x0002, et4000w32p_in, NULL, NULL, et4000w32p_out, NULL, NULL, et4000);
        io_sethandler(0x212A, 0x0002, et4000w32p_in, NULL, NULL, et4000w32p_out, NULL, NULL, et4000);
        io_sethandler(0x213A, 0x0002, et4000w32p_in, NULL, NULL, et4000w32p_out, NULL, NULL, et4000);
        io_sethandler(0x214A, 0x0002, et4000w32p_in, NULL, NULL, et4000w32p_out, NULL, NULL, et4000);
        io_sethandler(0x215A, 0x0002, et4000w32p_in, NULL, NULL, et4000w32p_out, NULL, NULL, et4000);
        io_sethandler(0x216A, 0x0002, et4000w32p_in, NULL, NULL, et4000w32p_out, NULL, NULL, et4000);
        io_sethandler(0x217A, 0x0002, et4000w32p_in, NULL, NULL, et4000w32p_out, NULL, NULL, et4000);
        
        pci_add(et4000w32p_pci_read, et4000w32p_pci_write, et4000);
        
        return et4000;
}

void et4000w32p_close(void *p)
{
        et4000w32p_t *et4000 = (et4000w32p_t *)p;

        svga_close(&et4000->svga);
        
        free(et4000);
}

void et4000w32p_speed_changed(void *p)
{
        et4000w32p_t *et4000 = (et4000w32p_t *)p;
        
        svga_recalctimings(&et4000->svga);
}

device_t et4000w32p_device =
{
        "Tseng Labs ET4000/w32p",
        et4000w32p_init,
        et4000w32p_close,
        et4000w32p_speed_changed,
        svga_add_status_info
};
