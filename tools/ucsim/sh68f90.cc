/*
 * uCsim CPU variant: SinoWealth SH68F90  (-t sh68f90)
 *
 * An 8052 core (cl_uc52) plus a single cl_hw peripheral model (cl_sh68f90_sie)
 * that emulates the chip-specific blocks the NuPhy Air60 stock firmware drives:
 *   - USB SIE      : EP0 control/enumeration, EP1/EP2 IN report endpoints
 *   - Key matrix   : pin-level (P7/P5 rows, P5/P3/P2/P1 cols), NKRO
 *   - BK3632 SPI   : bit-bang ACK (P4.2) + MISO status + connection state
 *   - Flash ISP    : IB_CON/XPAGE/IB_OFFSET/IB_DATA erase+program into code space
 *   - Sleep/wake   : PCON power-down + INT4 (EXF1) wake
 *   - Watchdog     : RSTSTAT kick + timeout reset
 * plus a cl_sh68f90_interrupt that registers the SH68F90's (remapped) interrupt
 * vectors instead of the standard 8051 INT0/INT1.
 *
 * This replaces the old approach of patching the shared s51 core files
 * (interrupt.cc / uc51.cc): everything chip-specific now lives here, and the CPU
 * is registered as a normal uCsim variant (cpus_51[] + sim51.cc factory).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifndef MSG_NOSIGNAL
#    define MSG_NOSIGNAL 0
#endif

#include "globals.h"
#include "regs51.h"
#include "dregcl.h"
#include "portcl.h"
#include "timer2cl.h"
#include "interruptcl.h"
#include "itsrccl.h"

#include "uc52cl.h"
#include "sh68f90cl.h"

/* ===================================================================== *
 *  SH68F90 peripheral model (USB SIE + matrix + BK3632 + flash + power)  *
 * ===================================================================== */
class cl_sh68f90_sie : public cl_hw
{
    class cl_address_space *xram, *sfr, *iram, *rom;
    class cl_memory_cell   *cell_ep0con, *cell_usbif1, *cell_usbif2, *cell_iep0cnt;
    class cl_memory_cell   *cell_ep1con, *cell_iep1cnt;
    class cl_memory_cell   *cell_ep2con, *cell_iep2cnt;
    class cl_memory_cell   *cell_pllcon;
    class cl_memory_cell   *cell_sbuf, *cell_scon;
    class cl_memory_cell   *cell_p1, *cell_p2, *cell_p3, *cell_p5, *cell_p7;
    class cl_memory_cell   *cell_p0, *cell_p4;
    class cl_memory_cell   *cell_p6; // matrix columns C0-C7 (read-only for host injection)
    class cl_memory_cell   *cell_ibcon5, *cell_rststat, *cell_pcon;
    unsigned                pwm_acc;
    int                     in_packets;
    bool                    rf_ack_toggle; // BK3632 SPI: P4.2 ACK flips each pin-read so the
                                           // firmware's "wait for ACK to change" poll matches.
    int      miso_bitpos;                  // BK3632 SPI: bit cursor into the 4-byte status reply.
    unsigned wdt_acc;                      // watchdog: cycles since last RSTSTAT(0xb1) kick.
    bool     wdt_armed;                    // watchdog only enforced after the firmware kicks once.

    // --- host mode (Step 0: USB/IP on the Linux USB stack) ------------------
    // Enabled by SMK_UCSIM_HOST=<port>. A TCP socket carries framed SIE
    // transactions (RESET, SETUP, OUT, IN, SOF) between ucsim and
    // tools/usbip_bridge.py. The test mode above stays untouched when unset.
    bool host_on;
    bool host_debug;
    bool host_verbose;
    // Cheap in-memory event ring: records SETUP/OUT/RDY/STALL/IN so a stall can
    // dump the exact sequence that led to it without printing on every event
    // (printing slows the simulated firmware and hides timing races).
    unsigned      host_trace[256];
    int           host_trace_idx;
    int           host_listen_fd, host_fd;
    unsigned char host_rx[8192];
    int           host_rxlen;
    unsigned char host_tx[8192];
    int           host_txlen;
    unsigned      host_poll_acc, host_sof_acc;
    bool          host_ep0_in_ready, host_ep0_in_stall;
    t_mem         host_ep0_in[8];
    t_mem         host_ep0_in_len;
    bool          host_ep1_in_ready, host_ep1_in_stall;
    t_mem         host_ep1_in[16];
    t_mem         host_ep1_in_len;
    bool          host_ep2_in_ready, host_ep2_in_stall;
    // Host-mode key matrix (RK61 Plus wiring): host_key[r] is a bitmask of pressed
    // columns for row r. Recomputed into pin_ext on each P5/P7 read so the scan
    // sees exactly the pressed keys (no lag, unlike a polling model).
    unsigned host_key[8];
    t_mem    host_ep2_in[64];
    t_mem    host_ep2_in_len;
    // EP0 OUT data-stage pacing: the host sends a chunk per packet, but the
    // firmware only consumes one when OEP0RDY is set, so queue and hand them
    // over one per tick.
    unsigned char host_ep0_out_q[8][8];
    unsigned char host_ep0_out_qlen[8];
    int           host_ep0_out_qhead, host_ep0_out_qtail, host_ep0_out_qcount;

    // External level present on each port's pins (what the board drives). An input
    // pin reads this, not its output latch; it idles high (== the internal pull-up
    // a real input enables via PxPCR). The board model (tests/) pulls bits low via
    // the staging cells registered in init(); nothing board-specific lives here.
    t_mem                 pin_ext[8];
    class cl_memory_cell *cell_pinext_p0, *cell_pinext_p5, *cell_pinext_p7;
    // Bit cells for the bit-addressable input pins of P0/P5/P7. Bit reads bypass
    // the byte read() operator, so we hook these too (as cl_port does) -- otherwise
    // a `MOV C,P5.5` (CONN_MODE switch) or `JNB P0.0` (RC battery sense) reads the
    // latch instead of the pin level.
    class cl_memory_cell *p0_bit[8], *p5_bit[8], *p7_bit[8];

   public:
    cl_sh68f90_sie(class cl_uc *auc) : cl_hw(auc, HW_DUMMY, 0, "sh68f90_sie")
    {
        xram = sfr = iram = rom = 0;
        cell_ibcon5 = cell_rststat = cell_pcon = 0;
        wdt_acc                                = 0;
        wdt_armed                              = false;
        cell_ep0con = cell_usbif2 = cell_iep0cnt = 0;
        cell_ep1con = cell_iep1cnt = 0;
        cell_ep2con = cell_iep2cnt = 0;
        cell_pllcon                = 0;
        cell_sbuf = cell_scon = 0;
        cell_p1 = cell_p2 = cell_p3 = cell_p5 = cell_p7 = 0;
        cell_p0 = cell_p4 = 0;
        cell_p6           = 0;
        for (int i = 0; i < 8; i++)
            host_key[i] = 0;
        pwm_acc           = 0;
        in_packets        = 0;
        rf_ack_toggle     = false;
        miso_bitpos       = 0;
        host_on           = false;
        host_debug        = getenv("SMK_UCSIM_HOST_DEBUG") != NULL;
        host_verbose      = getenv("SMK_UCSIM_HOST_DEBUG_VERBOSE") != NULL;
        host_trace_idx    = 0;
        host_listen_fd    = -1;
        host_fd           = -1;
        host_rxlen        = 0;
        host_txlen        = 0;
        host_poll_acc     = 0;
        host_sof_acc      = 0;
        host_ep0_in_ready = host_ep0_in_stall = false;
        host_ep0_in_len                       = 0;
        host_ep1_in_ready = host_ep1_in_stall = false;
        host_ep1_in_len                       = 0;
        host_ep2_in_ready = host_ep2_in_stall = false;
        host_ep2_in_len                       = 0;
        host_ep0_out_qhead = host_ep0_out_qtail = host_ep0_out_qcount = 0;
        for (int i = 0; i < 8; i++)
            pin_ext[i] = 0xff; // pins idle high (pull-ups)
        cell_pinext_p0 = cell_pinext_p5 = cell_pinext_p7 = 0;
        for (int i = 0; i < 8; i++)
            p0_bit[i] = p5_bit[i] = p7_bit[i] = 0;
    }
    virtual int init(void)
    {
        cl_hw::init();
        xram = uc->address_space("xram");
        sfr  = uc->address_space(MEM_SFR_ID);
        iram = uc->address_space("iram");
        rom  = uc->address_space("rom"); // code/flash space (for ISP erase/program)
        if (sfr) {
            cell_ep0con  = register_cell(sfr, 0x97); // EP0CON
            cell_usbif1  = sfr->get_cell(0x92);      // USBIF1
            cell_usbif2  = sfr->get_cell(0x93);      // USBIF2
            cell_iep0cnt = sfr->get_cell(0x9b);      // IEP0CNT (IN byte count)
            cell_ep1con  = register_cell(sfr, 0x99); // EP1CON (keyboard report endpoint)
            cell_iep1cnt = sfr->get_cell(0x9c);      // IEP1CNT
            cell_ep2con  = register_cell(sfr, 0x9a); // EP2CON (IF1 multiplexed IN: NKRO etc.)
            cell_iep2cnt = sfr->get_cell(0x9d);      // IEP2CNT
            cell_pllcon  = register_cell(sfr, 0xbc); // PLLCON (clock PLL)
            cell_sbuf    = register_cell(sfr, 0xaa); // SBUF (real UART TX data)
            cell_scon    = sfr->get_cell(0xd8);      // SCON (TI = bit1)
            // GPIO port cells (plain MCU ports). What's wired to them -- the key
            // matrix columns/rows, the BK3632 -- is board-level and lives test-side.
            cell_p1      = sfr->get_cell(0x90);
            cell_p2      = sfr->get_cell(0x98);
            cell_p3      = sfr->get_cell(0xa0);
            cell_p5      = register_cell(sfr, 0x88); // P5: cols C0-2 + rows R3-R4
            cell_p7      = register_cell(sfr, 0xf8); // P7: rows R0-R2 (bits 1-3)
            cell_p0      = register_cell(sfr, 0x80); // P0: BK3632 MISO=P0.6, MOSI=P0.7, MOT=P0.5
            cell_p4      = register_cell(sfr, 0xb0); // P4: BK3632 SCK=P4.7, ACK=P4.2
            cell_p6      = sfr->get_cell(0xc0);      // P6: matrix columns C0-C7
            cell_ibcon5  = register_cell(sfr, 0xf6); // IB_CON5: flash ISP commit (write 0x06)
            cell_rststat = register_cell(sfr, 0xb1); // RSTSTAT: watchdog kick (write 0)
            cell_pcon    = register_cell(sfr, 0x87); // PCON: bit1 -> sleep/power-down
        }
        if (xram) {
            // Staging for the external pin levels of P0 / P5 / P7 (the ports the
            // firmware reads as inputs). The board model writes these; read()
            // applies them to the input bits. Outside the firmware's xram window,
            // so they never alias real data.
            cell_pinext_p0 = register_cell(xram, 0x1f16);
            cell_pinext_p5 = register_cell(xram, 0x1f15);
            cell_pinext_p7 = register_cell(xram, 0x1f17);
        }
        class cl_address_space *bas = uc->address_space("bits");
        if (bas) {
            // The firmware reads some pins bit-wise (P0: RC battery sense/discharge
            // b0/b1 + BK3632 MISO b3; P5: rows R3/R4 b3/b4, CONN_MODE b5, OS switch
            // b6; P7: rows R0-R2 b1-b3). Hook those bit cells so bit reads see the
            // pin level too. (Bit addr of Px.i = Px + i; these are all inputs, so no
            // bit-write linkage to maintain.)
            int p0in[] = {0, 1, 3}, p5in[] = {3, 4, 5, 6}, p7in[] = {1, 2, 3};
            for (int k = 0; k < 3; k++)
                p0_bit[p0in[k]] = register_cell(bas, 0x80 + p0in[k]);
            for (int k = 0; k < 4; k++)
                p5_bit[p5in[k]] = register_cell(bas, 0x88 + p5in[k]);
            for (int k = 0; k < 3; k++)
                p7_bit[p7in[k]] = register_cell(bas, 0xf8 + p7in[k]);
        }
        host_open();
        return 0;
    }

    // A port read must honour pin direction: PxCR selects output(1)/input(0); an
    // output bit reads its latch, an input bit reads the external pin level (which
    // idles high via the pull-up, modelled as pin_ext defaulting to 0xff and pulled
    // low by the board test-side). This models only the MCU's own I/O behaviour --
    // what is wired to the pins (key matrix, CONN_MODE switch, BK3632) is test-side.
    // Recomputed on every P5/P7 read: idle rows high, pull a pressed key's row
    // low only while its column is driven (multiplexed scan, no phantoms).
    void host_matrix_update(void)
    {
        t_mem p6         = cell_p6 ? cell_p6->get() : 0xff;
        t_mem p5         = cell_p5 ? cell_p5->get() : 0xff;
        t_mem p4         = cell_p4 ? cell_p4->get() : 0xff;
        bool  row_low[8] = {false};
        for (int r = 0; r < 8; r++) {
            for (int c = 0; c < 16; c++) {
                if (!(host_key[r] & (1u << c))) continue;
                bool low = false;
                if (c < 8)
                    low = !(p6 & (1 << c));
                else if (c < 11)
                    low = !(p5 & (1 << (c - 8)));
                else if (c == 11)
                    low = !(p5 & (1 << 7));
                else if (c == 12)
                    low = !(p4 & 1);
                else if (c == 13)
                    low = !(p4 & (1 << 2));
                if (low) {
                    row_low[r] = true;
                    break;
                }
            }
        }
        t_mem pe7 = 0xff;
        if (row_low[0]) pe7 &= ~(1 << 1);
        if (row_low[1]) pe7 &= ~(1 << 2);
        if (row_low[2]) pe7 &= ~(1 << 3);
        t_mem pe5 = 0xff;
        if (row_low[3]) pe5 &= ~(1 << 3);
        if (row_low[4]) pe5 &= ~(1 << 4);
        pin_ext[7] = pe7;
        pin_ext[5] = pe5;
    }

    t_mem port_read(class cl_memory_cell *cell, int n, t_addr cr_addr)
    {
        if (host_on) host_matrix_update();
        t_mem latch = cell->get();
        t_mem cr    = sfr ? sfr->get(cr_addr) : 0; // PxCR: 1=output, 0=input
        return (latch & cr) | (pin_ext[n] & (t_mem)(~cr & 0xff));
    }
    virtual t_mem read(class cl_memory_cell *cell)
    {
        if (cell == cell_p0) return port_read(cell, 0, 0xe1); // P0CR @ 0xe1
        if (cell == cell_p5) return port_read(cell, 5, 0xe6); // P5CR @ 0xe6
        if (cell == cell_p7) return port_read(cell, 7, 0xd1); // P7CR @ 0xd1
        for (int i = 0; i < 8; i++) {
            if (p0_bit[i] && cell == p0_bit[i]) return (port_read(cell_p0, 0, 0xe1) >> i) & 1;
            if (p5_bit[i] && cell == p5_bit[i]) return (port_read(cell_p5, 5, 0xe6) >> i) & 1;
            if (p7_bit[i] && cell == p7_bit[i]) return (port_read(cell_p7, 7, 0xd1) >> i) & 1;
        }
        return cl_hw::read(cell);
    }

    // What is wired to the pins (key matrix, BK3632, USB host, CONN_MODE switch)
    // is NOT modelled here -- the board model lives test-side (tests/devices.py:
    // KeyMatrix), driving pin_ext via the staging cells above. The chip only models
    // its own pins (direction/latch/pull-up via read()), staying board-agnostic.

    // PWM0 timebase: periodically request the PWM interrupt (vector 0x43) that
    // drives matrix_scan_step(), once the firmware has enabled it (IEN1._EPWM0).
    virtual int tick(int cycles)
    {
        if (xram && sfr) {
            // Watchdog: count cycles since the last RSTSTAT kick (paused during sleep,
            // when the clock stops). Only armed once the firmware has kicked it at least
            // once -- a firmware that never touches RSTSTAT (e.g. SMK) is left alone.
            if (wdt_armed && !(sfr->get(0x87) & 0x02)) {
                wdt_acc += cycles;
                if (wdt_acc > 80000000u) {
                    fprintf(stderr, "[SIE] WATCHDOG timeout -> reset\n");
                    wdt_acc = 0;
                    uc->reset();
                }
            }
            // Timer ticks. These are the chip's own timers, just calibrated for uCsim:
            // raise PWM0 (vector 0x43, SMK scan) and the Timer2 1 ms overflow (vector
            // 0x0003, stock scan) at a rate the ISRs can keep up with. (External-hardware
            // effects of those scans -- the key matrix, the BK3632 -- are modelled
            // test-side; INT4 wake is likewise triggered test-side by raising EXF1.)
            pwm_acc += cycles;
            // host mode: poll the USB/IP bridge socket and emit a 1 ms SOF
            if (host_on) {
                host_poll_acc += cycles;
                if (host_poll_acc >= 2000) {
                    host_poll_acc = 0;
                    host_service();
                }
                host_sof_acc += cycles;
                if (host_sof_acc >= 12000) {
                    host_sof_acc = 0;
                    if (cell_usbif1 && cell_usbif1->get() == 0) cell_usbif1->set(0x08); // SOFIF
                }
                if (host_ep0_out_qcount > 0) host_try_out();
            }
            // period must exceed the matrix-scan ISR duration or the main code starves
            if (pwm_acc >= 30000) {
                pwm_acc = 0;
                if (sfr->get(0xa9) & 0x02) // IEN1._EPWM0
                    xram->set(0x1f08, xram->get(0x1f08) | 0x01);
                // Matrix-scan tick: the SH68F90 Timer2 ISR @0x27bd (vector 0x0003) is
                // the 1ms scan handler. uCsim's own Timer2 overflows ~12x too fast for
                // its modelled clock, so the scan+LED ISR storms and starves the main
                // loop. Drive it instead from this calibrated virtual flag (0x1f09) at a
                // rate the ISR can keep up with, gated on the scan enable IEN0(0xa8).bit0
                // and T2CON.TR2(0x04). clr_bit=true on the it_src auto-clears the flag.
                if ((sfr->get(0xa8) & 0x01) && (sfr->get(0xc8) & 0x04)) xram->set(0x1f09, xram->get(0x1f09) | 0x01);
            }
        }
        return 0;
    }
    virtual void write(class cl_memory_cell *cell, t_mem *val)
    {
        // Board model sets the external pin levels for P0 / P5 / P7 via these
        // staging cells (read() applies them to the input bits).
        if (cell == cell_pinext_p0) pin_ext[0] = *val & 0xff;
        if (cell == cell_pinext_p5) pin_ext[5] = *val & 0xff;
        if (cell == cell_pinext_p7) pin_ext[7] = *val & 0xff;
        // Flash ISP: the firmware writes the IB register file then commits with
        // IB_CON5(0xf6)=0x06. Opcode in IB_CON1(0xf2): 0xe6=erase, 0x6e=program.
        // Address = XPAGE(0xf7)<<8 | IB_OFFSET(0xfb); program data = IB_DATA(0xfc).
        // Erase granularity = 512 B (XPAGE = sector*2 -> base = XPAGE*0x100).
        if (cell == cell_ibcon5 && (*val) == 0x06 && rom && sfr) {
            t_mem    op   = sfr->get(0xf2);
            unsigned base = (unsigned)sfr->get(0xf7) << 8;
            if (op == 0xe6) // ERASE: fill the 512 B sector with 0xff
                for (unsigned i = 0; i < 0x200; i++)
                    rom->set((base + i) & 0xffff, 0xff);
            else if (op == 0x6e) // PROGRAM: AND one byte (real flash can't set 1s)
            {
                unsigned a = (base | (sfr->get(0xfb) & 0xff)) & 0xffff;
                rom->set(a, rom->get(a) & sfr->get(0xfc));
            }
        }
        // Watchdog kick: any RSTSTAT(0xb1) write (firmware writes 0) reloads the WDT.
        if (cell == cell_rststat) {
            wdt_acc   = 0;
            wdt_armed = true;
        }
        // Sleep: PCON(0x87) bit1 set = power-down/STOP (after SUSLO=0x55). The core
        // halts here until INT4 (matrix-wake) fires; the wake is injected from tick().
        if (cell == cell_pcon && ((*val) & 0x02)) fprintf(stderr, "[SIE] sleep: PCON power-down (wake on INT4 / key)\n");
        if (host_on && host_verbose && cell == cell_ep0con && ((*val) & 0x04) && !(cell_ep0con->get() & 0x04)) fprintf(stderr, "[HOST] EP0 IN RDY len=%u\n", (unsigned)(cell_iep0cnt ? cell_iep0cnt->get() : 0));
        if (cell == cell_ep0con && ((*val) & 0x04)) // IEP0RDY: firmware queued IN data
        {
            if (host_on) {
                // The firmware uses `EP0CON |= ...`, so a write that sets a
                // different bit still carries a stale IEP0RDY. Only a genuine
                // 0->1 transition means the firmware queued a new packet; a
                // fresh RDY also cancels a stale stall.
                if (!(cell_ep0con->get() & 0x04)) {
                    host_ep0_in_stall = false;
                    host_ep0_latch();
                    host_trace_add(0x05000000 | (unsigned)host_ep0_in_len);
                    if (host_verbose) fprintf(stderr, "[HOST] firmware EP0 IN len=%u\n", (unsigned)host_ep0_in_len);
                }
            } else {
                t_mem n = cell_iep0cnt ? cell_iep0cnt->get() : 0;
                fprintf(stderr, "[SIE] EP0 IN[%d] %u bytes:", in_packets, (unsigned)n);
                for (t_mem i = 0; i < n && i < 8; i++)
                    fprintf(stderr, " %02x", (unsigned)(xram->get(0x1108 + i) & 0xff));
                fprintf(stderr, "\n");
                in_packets++;
                *val &= ~0x04;                                                                   // host consumed the packet -> clear ready
                if (in_packets < 16 && cell_usbif2) cell_usbif2->set(cell_usbif2->get() | 0x01); // IEP0IF -> next chunk
            }
        }
        if (host_on && cell == cell_ep0con && ((*val) & 0x08) && !(cell_ep0con->get() & 0x08)) // IEP0STL newly set
        {
            host_trace_add(0x06000000 | (cell_ep0con->get() & 0xff));
            host_ep0_in_stall = true;
            if (host_debug) host_trace_dump();
        }
        // EP1 = the keyboard's interrupt-IN report endpoint (single-packet)
        if (cell == cell_ep1con && ((*val) & 0x04)) // IEP1RDY
        {
            if (host_on) {
                host_ep1_latch();
            } else {
                t_mem n = cell_iep1cnt ? cell_iep1cnt->get() : 0;
                fprintf(stderr, "[SIE] EP1 IN %u bytes:", (unsigned)n);
                for (t_mem i = 0; i < n && i < 16; i++)
                    fprintf(stderr, " %02x", (unsigned)(xram->get(0x1120 + i) & 0xff));
                fprintf(stderr, "\n");
                *val &= ~0x04; // host consumed the report -> clear ready
            }
        }
        if (host_on && cell == cell_ep1con && ((*val) & 0x08)) host_ep1_in_stall = true;
        // EP2 = the IF1 multiplexed interrupt-IN endpoint (NKRO keyboard + others,
        // each prefixed with a Report ID). FIFO at 0x1180, length in IEP2CNT.
        if (cell == cell_ep2con && ((*val) & 0x04)) // IEP2RDY
        {
            if (host_on) {
                host_ep2_latch();
            } else {
                t_mem n = cell_iep2cnt ? cell_iep2cnt->get() : 0;
                fprintf(stderr, "[SIE] EP2 IN %u bytes:", (unsigned)n);
                for (t_mem i = 0; i < n && i < 24; i++)
                    fprintf(stderr, " %02x", (unsigned)(xram->get(0x1180 + i) & 0xff));
                fprintf(stderr, "\n");
                *val &= ~0x04; // host consumed the report -> clear ready
            }
        }
        if (host_on && cell == cell_ep2con && ((*val) & 0x08)) host_ep2_in_stall = true;
        // Clock PLL: report "locked" (PLLSTA) the moment firmware enables it (PLLON),
        // so clock_init()'s `while (!(PLLCON & _PLLSTA))` spin returns and the real
        // boot path (init -> usb_init -> main) runs instead of deadlocking.
        if (cell == cell_pllcon && ((*val) & 0x02)) // _PLLON -> set _PLLSTA
            *val |= 0x04;
        // Real SH68F90 UART TX: a write to SBUF (0xaa) "transmits" instantly -- echo
        // the byte and raise SCON.TI (0xd8 bit1) so the UART ISR clears uart_tx_busy
        // and the firmware's blocking putchar() returns (else dprintf hangs forever).
        if (cell == cell_sbuf) {
            putc((char)(*val & 0xff), stderr);
            if (cell_scon) cell_scon->set(cell_scon->get() | 0x02);
        }
    }

    // ================================================================== //
    //  Host mode: framed SIE transactions over TCP for tools/usbip_bridge.py
    // ================================================================== //
    void host_open(void)
    {
        const char *env = getenv("SMK_UCSIM_HOST");
        if (!env || !*env) return;
        long port = strtol(env, NULL, 10);
        if (port <= 0 || port > 65535) return;
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return;
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family      = AF_INET;
        sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        sa.sin_port        = htons((unsigned short)port);
        if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0 || listen(fd, 1) < 0) {
            fprintf(stderr, "[HOST] cannot listen on 127.0.0.1:%ld: %s\n", port, strerror(errno));
            close(fd);
            return;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        host_listen_fd = fd;
        host_on        = true;
        fprintf(stderr, "[HOST] ucsim host mode listening on 127.0.0.1:%ld\n", port);
    }

    void host_ep0_latch(void)
    {
        t_mem n = cell_iep0cnt ? cell_iep0cnt->get() : 0;
        if (n > 8) n = 8;
        for (t_mem i = 0; i < n; i++)
            host_ep0_in[i] = xram ? xram->get(0x1108 + i) : 0;
        host_ep0_in_len   = n;
        host_ep0_in_ready = true;
    }
    void host_ep1_latch(void)
    {
        t_mem n = cell_iep1cnt ? cell_iep1cnt->get() : 0;
        if (n > 16) n = 16;
        for (t_mem i = 0; i < n; i++)
            host_ep1_in[i] = xram ? xram->get(0x1120 + i) : 0;
        host_ep1_in_len   = n;
        host_ep1_in_ready = true;
    }
    void host_ep2_latch(void)
    {
        t_mem n = cell_iep2cnt ? cell_iep2cnt->get() : 0;
        if (n > 64) n = 64;
        for (t_mem i = 0; i < n; i++)
            host_ep2_in[i] = xram ? xram->get(0x1180 + i) : 0;
        host_ep2_in_len   = n;
        host_ep2_in_ready = true;
    }

    void host_send(const unsigned char *buf, int len)
    {
        for (int i = 0; i < len && host_txlen < (int)sizeof(host_tx); i++)
            host_tx[host_txlen++] = buf[i];
    }
    void host_reply_ok(void)
    {
        unsigned char r[3] = {0, 1, 0x4b};
        host_send(r, 3);
    }
    void host_reply_status(t_mem st)
    {
        unsigned char r[3] = {0, 1, (unsigned char)st};
        host_send(r, 3);
    }
    void host_reply_data(const t_mem *data, int len)
    {
        unsigned char h[3] = {(unsigned char)((len + 1) >> 8), (unsigned char)((len + 1) & 0xff), 0x44};
        unsigned char tmp[68];
        host_send(h, 3);
        if (len > (int)sizeof(tmp)) len = sizeof(tmp);
        for (int i = 0; i < len; i++)
            tmp[i] = (unsigned char)(data[i] & 0xff);
        host_send(tmp, len);
    }

    void host_trace_add(unsigned v)
    {
        if (!host_on) return;
        host_trace[host_trace_idx & 255] = v;
        host_trace_idx++;
    }
    void host_trace_dump(void)
    {
        int start = host_trace_idx > 256 ? host_trace_idx - 256 : 0;
        fprintf(stderr, "[TRACE] %d events\n", host_trace_idx - start);
        for (int i = start; i < host_trace_idx; i++) {
            unsigned v   = host_trace[i & 255];
            unsigned tag = v >> 24;
            if (tag == 0x01)
                fprintf(stderr, "[TRACE] RESET\n");
            else if (tag == 0x02)
                fprintf(stderr, "[TRACE] SETUP type=%02x req=%02x\n", (v >> 8) & 0xff, v & 0xff);
            else if (tag == 0x03)
                fprintf(stderr, "[TRACE] OUT queued n=%u\n", v & 0xff);
            else if (tag == 0x04)
                fprintf(stderr, "[TRACE] OUT delivered n=%u\n", v & 0xff);
            else if (tag == 0x05)
                fprintf(stderr, "[TRACE] EP0 IN RDY len=%u\n", v & 0xff);
            else if (tag == 0x06)
                fprintf(stderr, "[TRACE] EP0 IN STALL old=%02x\n", v & 0xff);
            else if (tag == 0x07)
                fprintf(stderr, "[TRACE] IN ep%u ready=%d stall=%d\n", (v >> 8) & 0xff, (v >> 4) & 1, v & 1);
            else if (tag == 0x08)
                fprintf(stderr, "[TRACE] SOF\n");
            else
                fprintf(stderr, "[TRACE] %08x\n", v);
        }
    }

    void host_do_in(t_mem ep)
    {
        host_trace_add(0x07000000 | ((unsigned)ep << 8) | (ep == 0 ? (host_ep0_in_ready ? 0x10 : 0) | (host_ep0_in_stall ? 1 : 0) : 0));
        if (host_verbose) fprintf(stderr, "[HOST] IN ep%u ready=%d stall=%d\n", (unsigned)ep, ep == 0 ? host_ep0_in_ready : (ep == 1 ? host_ep1_in_ready : host_ep2_in_ready), ep == 0 ? host_ep0_in_stall : (ep == 1 ? host_ep1_in_stall : host_ep2_in_stall));
        if (ep == 0) {
            if (host_ep0_in_stall) {
                host_ep0_in_stall = false;
                if (cell_ep0con) cell_ep0con->set(cell_ep0con->get() & ~0x08);
                host_reply_status('T');
            } else if (host_ep0_in_ready) {
                host_ep0_in_ready = false;
                if (cell_ep0con) cell_ep0con->set(cell_ep0con->get() & ~0x04);
                if (cell_usbif2) cell_usbif2->set(cell_usbif2->get() | 0x01);
                host_reply_data(host_ep0_in, host_ep0_in_len);
            } else {
                host_reply_status('N');
            }
        } else if (ep == 1) {
            if (host_ep1_in_stall) {
                host_ep1_in_stall = false;
                if (cell_ep1con) cell_ep1con->set(cell_ep1con->get() & ~0x08);
                host_reply_status('T');
            } else if (host_ep1_in_ready) {
                host_ep1_in_ready = false;
                if (cell_ep1con) cell_ep1con->set(cell_ep1con->get() & ~0x04);
                if (cell_usbif2) cell_usbif2->set(cell_usbif2->get() | 0x02);
                host_reply_data(host_ep1_in, host_ep1_in_len);
            } else {
                host_reply_status('N');
            }
        } else if (ep == 2) {
            if (host_ep2_in_stall) {
                host_ep2_in_stall = false;
                if (cell_ep2con) cell_ep2con->set(cell_ep2con->get() & ~0x08);
                host_reply_status('T');
            } else if (host_ep2_in_ready) {
                host_ep2_in_ready = false;
                if (cell_ep2con) cell_ep2con->set(cell_ep2con->get() & ~0x04);
                if (cell_usbif2) cell_usbif2->set(cell_usbif2->get() | 0x04);
                host_reply_data(host_ep2_in, host_ep2_in_len);
            } else {
                host_reply_status('N');
            }
        } else {
            host_reply_status('E');
        }
    }

    void host_process(void)
    {
        while (host_rxlen >= 2) {
            int flen = (host_rx[0] << 8) | host_rx[1];
            if (flen < 1 || flen > (int)sizeof(host_rx) - 2) {
                host_rxlen = 0;
                break;
            }
            if (host_rxlen < 2 + flen) break;
            t_mem          op   = host_rx[2];
            unsigned char *p    = &host_rx[3];
            int            plen = flen - 1;
            if (host_verbose) fprintf(stderr, "[HOST] op=%02x plen=%d\n", (unsigned)op, plen);
            switch (op) {
                case 0x01: // RESET
                    host_trace_add(0x01000000);
                    host_ep0_in_ready = host_ep0_in_stall = false;
                    host_ep1_in_ready = host_ep1_in_stall = false;
                    host_ep2_in_ready = host_ep2_in_stall = false;
                    host_ep0_out_qhead = host_ep0_out_qtail = host_ep0_out_qcount = 0;
                    // Drop a pending SOF: the firmware's dispatcher gives USBIF1
                    // priority to SOF and would clear USBRSTIF/SETUPIF.
                    if (cell_usbif1) cell_usbif1->set((cell_usbif1->get() & ~0x08) | 0x01); // USBRSTIF
                    host_reply_ok();
                    break;
                case 0x02: // SETUP (8 bytes)
                    if (host_verbose && plen >= 8) fprintf(stderr, "[HOST] SETUP %02x %02x %04x %04x %04x\n", p[0], p[1], p[2] | (p[3] << 8), p[4] | (p[5] << 8), p[6] | (p[7] << 8));
                    host_trace_add(0x02000000 | (plen >= 8 ? ((unsigned)p[0] << 8) | p[1] : 0));
                    if (plen >= 8 && xram)
                        for (int i = 0; i < 8; i++)
                            xram->set(0x1100 + i, p[i]);
                    host_ep0_in_ready  = false;
                    host_ep0_in_stall  = false;
                    host_ep0_out_qhead = host_ep0_out_qtail = host_ep0_out_qcount = 0;
                    // A new SETUP resets the control endpoint: drop any leftover
                    // EP0 completion/stall state from the previous transfer.
                    if (cell_usbif2) cell_usbif2->set(cell_usbif2->get() & ~0x11); // IEP0IF|OEP0IF
                    // Clear OEP0RDY too, or an OUT data packet that arrives in the
                    // same read as the SETUP overwrites EP0_OUT_BUF(0x1100) before
                    // the firmware's setup ISR reads it, so it decodes the OUT data
                    // as the request and stalls. The handler re-arms OEP0RDY for the
                    // data stage; queued OUT packets wait for it.
                    // Clear the whole EP0 handshake state (IEP0STL|OEP0STL|OEP0RDY|IEP0RDY):
                    // a new SETUP resets the control endpoint, so the firmware's next
                    // `EP0CON |= ...` is a genuine 0->1 transition the model can see.
                    if (cell_ep0con) cell_ep0con->set(cell_ep0con->get() & ~0x0f);
                    // Drop a pending SOF so the firmware dispatcher does not eat
                    // the SETUP (see the USBIF1 priority in usb_irq_dispatch).
                    if (cell_usbif1) cell_usbif1->set((cell_usbif1->get() & ~0x08) | 0x10); // SETUPIF
                    host_reply_ok();
                    break;
                case 0x03: // OUT data stage (EP0, up to 8 bytes)
                    if (plen >= 1) {
                        int n = p[0];
                        if (n > 8) n = 8;
                        host_trace_add(0x03000000 | (unsigned)n);
                        if (host_ep0_out_qcount < 8) {
                            for (int i = 0; i < n; i++)
                                host_ep0_out_q[host_ep0_out_qtail][i] = p[1 + i];
                            host_ep0_out_qlen[host_ep0_out_qtail] = (unsigned char)n;
                            host_ep0_out_qtail                    = (host_ep0_out_qtail + 1) % 8;
                            host_ep0_out_qcount++;
                        }
                    }
                    host_try_out();
                    host_reply_ok();
                    break;
                case 0x04: // IN request (ep in payload[0])
                    host_do_in(plen >= 1 ? p[0] : 0);
                    break;
                case 0x05: // SOF
                    host_trace_add(0x08000000);
                    if (cell_usbif1 && cell_usbif1->get() == 0) cell_usbif1->set(0x08);
                    host_reply_ok();
                    break;
                case 0x07: // WAIT: OK once the previous control transfer is fully done
                    if (host_debug) fprintf(stderr, "[HOST] WAIT q=%d if2=%02x\n", host_ep0_out_qcount, cell_usbif2 ? (unsigned)cell_usbif2->get() : 0);
                    if (host_ep0_out_qcount == 0 && (!cell_usbif2 || (cell_usbif2->get() & 0x11) == 0))
                        host_reply_ok();
                    else
                        host_reply_status('N');
                    break;
                case 0x08: // PINS [port, value]: set the external pin level (matrix rows)
                    if (plen >= 2) {
                        unsigned port = p[0];
                        if (port == 0 && cell_pinext_p0)
                            cell_pinext_p0->set(p[1]);
                        else if (port == 5 && cell_pinext_p5)
                            cell_pinext_p5->set(p[1]);
                        else if (port == 7 && cell_pinext_p7)
                            cell_pinext_p7->set(p[1]);
                        else if (port < 8)
                            pin_ext[port] = p[1];
                    }
                    host_reply_ok();
                    break;
                case 0x09: // GET_SFR [addr] -> data: read a register (matrix columns)
                    if (plen >= 1 && sfr) {
                        t_mem v = sfr->get(p[0]);
                        host_reply_data(&v, 1);
                    } else {
                        host_reply_status('E');
                    }
                    break;
                case 0x0a: // KEY [row, col, pressed]: stage a matrix key
                    if (plen >= 3 && p[0] < 8 && p[1] < 16) {
                        if (p[2])
                            host_key[p[0]] |= (1u << p[1]);
                        else
                            host_key[p[0]] &= ~(1u << p[1]);
                    }
                    host_reply_ok();
                    break;
                case 0x06: // QUIT
                    if (host_fd >= 0) close(host_fd);
                    host_fd    = -1;
                    host_rxlen = 0;
                    return;
                default:
                    host_reply_status('E');
                    break;
            }
            int used = 2 + flen;
            memmove(host_rx, host_rx + used, host_rxlen - used);
            host_rxlen -= used;
        }
    }

    void host_try_out(void)
    {
        if (host_ep0_out_qcount <= 0 || !cell_ep0con || !cell_usbif2) return;
        if (!(cell_ep0con->get() & 0x01)) return; // wait for OEP0RDY
        if (cell_usbif2->get() & 0x10) return;    // previous OEP0IF still pending
        unsigned char n = host_ep0_out_qlen[host_ep0_out_qhead];
        host_trace_add(0x04000000 | (unsigned)n);
        for (unsigned char i = 0; i < n; i++)
            if (xram) xram->set(0x1100 + i, host_ep0_out_q[host_ep0_out_qhead][i]);
        host_ep0_out_qhead = (host_ep0_out_qhead + 1) % 8;
        host_ep0_out_qcount--;
        cell_ep0con->set(cell_ep0con->get() & ~0x01); // hardware clears OEP0RDY on receive
        cell_usbif2->set(cell_usbif2->get() | 0x10);  // OEP0IF
    }

    void host_service(void)
    {
        if (!host_on) return;
        if (host_fd < 0) {
            int fd = accept(host_listen_fd, NULL, NULL);
            if (fd >= 0) {
                fcntl(fd, F_SETFL, O_NONBLOCK);
                host_fd    = fd;
                host_rxlen = 0;
                host_txlen = 0;
                fprintf(stderr, "[HOST] client connected\n");
            }
        } else {
            char buf[4096];
            int  n = recv(host_fd, buf, sizeof(buf), 0);
            if (n > 0) {
                for (int i = 0; i < n && host_rxlen < (int)sizeof(host_rx); i++)
                    host_rx[host_rxlen++] = (unsigned char)(buf[i] & 0xff);
                host_process();
            } else if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                fprintf(stderr, "[HOST] client disconnected\n");
                close(host_fd);
                host_fd    = -1;
                host_rxlen = 0;
            }
        }
        if (host_fd >= 0 && host_txlen > 0) {
            int sent = send(host_fd, host_tx, host_txlen, MSG_NOSIGNAL);
            if (sent > 0) {
                memmove(host_tx, host_tx + sent, host_txlen - sent);
                host_txlen -= sent;
            } else if (sent < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
                close(host_fd);
                host_fd    = -1;
                host_txlen = 0;
            }
        }
    }
};

/* ===================================================================== *
 *  Interrupt controller: SH68F90 vectors (no standard INT0/INT1)         *
 * ===================================================================== */
class cl_sh68f90_interrupt : public cl_interrupt
{
   public:
    cl_sh68f90_interrupt(class cl_uc *auc) : cl_interrupt(auc) {}
    virtual int  init(void);
    virtual void added_to_uc(void);
};

int cl_sh68f90_interrupt::init(void)
{
    cl_hw::init();
    sfr = uc->address_space(MEM_SFR_ID);
    // SH68F90 has no standard INT0/INT1, and 0x88/0x8a are P5/MAPPING (owned by the
    // SIE). Register only IE; bind cell_tcon/it0/it1 via get_cell (NOT register_cell,
    // so we don't become an operator on P5). The base class derefs these, so they
    // must be non-null even though we add no INT0/INT1 sources.
    if (sfr) {
        register_cell(sfr, IE);
        cell_tcon = sfr->get_cell(TCON);
        bit_INT0  = 0;
        bit_INT1  = 0;
        cell_it0  = sfr->get_cell(TCON);
        cell_it1  = sfr->get_cell(TCON);
    }
    return 0;
}

void cl_sh68f90_interrupt::added_to_uc(void)
{
    class cl_address_space *sfr = uc->address_space(MEM_SFR_ID);
    class cl_it_src        *is;
    // SH68F90 USB interrupt (_INT_USB = vector 7 @ 0x003B).
    // enable: IEN1(0xa9).EUSB(bit0); request: USBIF1(0x92).SETUPIF(bit4).
    // level-triggered (clr_bit=false): the firmware ISR clears the flag.
    uc->it_sources->add(is = new cl_it_src(uc, 0x100, sfr->get_cell(0xa9), 0x01, sfr->get_cell(0x92), 0x10, 0x003b, false, false, "USB (SH68F90)", 7));
    is->init();
    // EP0 IN/OUT completion sources at the same USB vector (USBIF2), so the SIE
    // model can advance control transfers and the ISP set-report status stage.
    // uCsim's pending() is (flag & mask) == mask, so each source needs a
    // single-bit mask -- a combined mask would require all those bits set at once.
    uc->it_sources->add(is = new cl_it_src(uc, 0x101, sfr->get_cell(0xa9), 0x01, sfr->get_cell(0x93), 0x01, // IEP0IF
                                           0x003b, false, false, "USB EP0-IN (SH68F90)", 7));
    is->init();
    uc->it_sources->add(is = new cl_it_src(uc, 0x102, sfr->get_cell(0xa9), 0x01, sfr->get_cell(0x93), 0x10, // OEP0IF
                                           0x003b, false, false, "USB EP0-OUT (SH68F90)", 7));
    is->init();
    // The remaining USBIF1 flags and the EP1/EP2 completion flags (host mode
    // drives these; test mode leaves them clear). Same USB vector, EUSB gate.
    uc->it_sources->add(is = new cl_it_src(uc, 0x109, sfr->get_cell(0xa9), 0x01, sfr->get_cell(0x92), 0x01, // USBRSTIF
                                           0x003b, false, false, "USB reset (SH68F90)", 7));
    is->init();
    uc->it_sources->add(is = new cl_it_src(uc, 0x10a, sfr->get_cell(0xa9), 0x01, sfr->get_cell(0x92), 0x02, // SUSPIF
                                           0x003b, false, false, "USB suspend (SH68F90)", 7));
    is->init();
    uc->it_sources->add(is = new cl_it_src(uc, 0x10b, sfr->get_cell(0xa9), 0x01, sfr->get_cell(0x92), 0x04, // RESMIF
                                           0x003b, false, false, "USB resume (SH68F90)", 7));
    is->init();
    uc->it_sources->add(is = new cl_it_src(uc, 0x10c, sfr->get_cell(0xa9), 0x01, sfr->get_cell(0x92), 0x08, // SOFIF
                                           0x003b, false, false, "USB SOF (SH68F90)", 7));
    is->init();
    uc->it_sources->add(is = new cl_it_src(uc, 0x10d, sfr->get_cell(0xa9), 0x01, sfr->get_cell(0x92), 0x80, // PUPIF
                                           0x003b, false, false, "USB power-up (SH68F90)", 7));
    is->init();
    uc->it_sources->add(is = new cl_it_src(uc, 0x10e, sfr->get_cell(0xa9), 0x01, sfr->get_cell(0x93), 0x02, // IEP1IF
                                           0x003b, false, false, "USB EP1-IN (SH68F90)", 7));
    is->init();
    uc->it_sources->add(is = new cl_it_src(uc, 0x10f, sfr->get_cell(0xa9), 0x01, sfr->get_cell(0x93), 0x04, // IEP2IF
                                           0x003b, false, false, "USB EP2-IN (SH68F90)", 7));
    is->init();
    uc->it_sources->add(is = new cl_it_src(uc, 0x110, sfr->get_cell(0xa9), 0x01, sfr->get_cell(0x93), 0x20, // OEP1IF
                                           0x003b, false, false, "USB EP1-OUT (SH68F90)", 7));
    is->init();
    uc->it_sources->add(is = new cl_it_src(uc, 0x111, sfr->get_cell(0xa9), 0x01, sfr->get_cell(0x93), 0x40, // OEP2IF
                                           0x003b, false, false, "USB EP2-OUT (SH68F90)", 7));
    is->init();
    // SH68F90 UART TX-complete interrupt (_INT_EUART0 = vector 13 @ 0x6B).
    // enable IEN1(0xa9)._ES0(0x40); request SCON(0xd8).TI(0x02). The SIE sets TI on
    // each SBUF write, so this fires and uart_interrupt_handler clears uart_tx_busy.
    uc->it_sources->add(is = new cl_it_src(uc, 0x103, sfr->get_cell(0xa9), 0x40, sfr->get_cell(0xd8), 0x02, 0x006b, false, false, "UART TI (SH68F90)", 13));
    is->init();
    // SH68F90 PWM0 interrupt (_INT_PWM0 = vector 8 @ 0x43), which drives the matrix
    // scan. enable IEN1(0xa9)._EPWM0(0x02); request a virtual flag at xram 0x1f08
    // that the SIE's tick() raises periodically; clr_bit=true (HW auto-clears, the
    // firmware ISR doesn't).
    uc->it_sources->add(is = new cl_it_src(uc, 0x104, sfr->get_cell(0xa9), 0x02, uc->address_space("xram")->get_cell(0x1f08), 0x01, 0x0043, true, false, "PWM0 (SH68F90)", 8));
    is->init();
    // SH68F90 Timer2 overflow is remapped to the INT0 vector slot (0x0003): the
    // 1 ms Timer2 ISR @0x27bd is the LED-PWM-mux + MATRIX-SCAN handler. uCsim's
    // own Timer2 runs (auto-reload) and sets T2CON.TF2(0x80) on overflow but would
    // fire the standard 0x2B vector, which this firmware doesn't use. Route TF2 ->
    // 0x0003 instead, enabled by IEN0(0xa8).bit0; clr_bit=false because the
    // firmware ISR clears TF2 itself (CLR 0xcf). This drives the key-matrix scan
    // that populates the row bitmap at EXTMEM 0x06b0.
    uc->it_sources->add(is = new cl_it_src(uc, 0x105, sfr->get_cell(0xa8), 0x01, uc->address_space("xram")->get_cell(0x1f09), 0x01, 0x0003, true, false, "Timer2 scan (SH68F90)", 1));
    is->init();
    // INT4 = matrix/RF wake (vector 0x000b). Enable IEN0(0xa8).EX4(bit1), armed only
    // by the sleep path; request via EXF1(0xe8) sub-flags IF40..IF47 (raised from the
    // SIE tick() when a key is staged while asleep). ISR clears EXF1 -> clr_bit=false.
    uc->it_sources->add(is = new cl_it_src(uc, 0x106, sfr->get_cell(0xa8), 0x02, sfr->get_cell(0xe8), 0xff, 0x000b, false, false, "INT4 wake (SH68F90)", 2));
    is->init();
    // INT3 (0x0013) / INT2 (0x001b): real ISR bodies exist but the firmware never
    // arms them in this build; wired so they dispatch if ever enabled. Enable
    // IEN0(0xa8).EX3(bit2)/EX2(bit3); request EXF0(0xb6).bit1/bit0; ISR clears flag.
    uc->it_sources->add(is = new cl_it_src(uc, 0x107, sfr->get_cell(0xa8), 0x04, sfr->get_cell(0xb6), 0x02, 0x0013, false, false, "INT3 (SH68F90)", 3));
    is->init();
    uc->it_sources->add(is = new cl_it_src(uc, 0x108, sfr->get_cell(0xa8), 0x08, sfr->get_cell(0xb6), 0x01, 0x001b, false, false, "INT2 (SH68F90)", 3));
    is->init();
}

/* ===================================================================== *
 *  cl_sh68f90 : 8052 core + the peripherals above                        *
 * ===================================================================== */
cl_sh68f90::cl_sh68f90(struct cpu_entry *Itype, class cl_sim *asim) : cl_uc52(Itype, asim) {}

int cl_sh68f90::init(void)
{
    return cl_uc52::init();
}

void cl_sh68f90::mk_hw_elements(void)
{
    // This is cl_51core::mk_hw_elements() with timer0/timer1/serial OMITTED (the
    // SH68F90 remaps SFRs 0x88-0x99 to GPIO/clock/USB, so the standard timer/UART
    // models would corrupt those), plus Timer2 (from cl_uc52), the SIE peripheral,
    // and the SH68F90 interrupt controller.
    cl_uc::mk_hw_elements();

    class cl_hw *h;
    acc = sfr->get_cell(ACC);
    psw = sfr->get_cell(PSW);

    // Timer2 (8052) -- kept; the firmware's 1 ms matrix-scan ISR rides its overflow.
    h = new cl_timer2(this, 2, "timer2", t2_default | t2_down);
    h->init();
    add_hw(h);

    add_hw(h = new cl_dreg(this, 0, "dreg"));
    h->init();

    class cl_port_ui *d;
    add_hw(d = new cl_port_ui(this, 0, "dport"));
    d->init();

    class cl_port *p0, *p1, *p2, *p3;
    add_hw(p0 = new cl_port(this, 0));
    p0->init();
    add_hw(p1 = new cl_port(this, 1));
    p1->init();
    add_hw(p2 = new cl_port(this, 2));
    p2->init();
    add_hw(p3 = new cl_port(this, 3));
    p3->init();

    class cl_port_data pd;
    pd.init();
    pd.cell_dir = NULL;
    pd.set_name("P0");
    pd.cell_p  = p0->cell_p;
    pd.cell_in = p0->cell_in;
    pd.keyset  = keysets[0];
    pd.basx    = 1;
    pd.basy    = 5;
    d->add_port(&pd, 0);
    pd.set_name("P1");
    pd.cell_p  = p1->cell_p;
    pd.cell_in = p1->cell_in;
    pd.keyset  = keysets[1];
    pd.basx    = 20;
    pd.basy    = 5;
    d->add_port(&pd, 1);
    pd.set_name("P2");
    pd.cell_p  = p2->cell_p;
    pd.cell_in = p2->cell_in;
    pd.keyset  = keysets[2];
    pd.basx    = 40;
    pd.basy    = 5;
    d->add_port(&pd, 2);
    pd.set_name("P3");
    pd.cell_p  = p3->cell_p;
    pd.cell_in = p3->cell_in;
    pd.keyset  = keysets[3];
    pd.basx    = 60;
    pd.basy    = 5;
    d->add_port(&pd, 3);

    // The chip-specific peripheral model (registers its SFR cells in init()).
    cl_sh68f90_sie *sie = new cl_sh68f90_sie(this);
    add_hw(sie);
    sie->init();

    // Interrupt controller (its added_to_uc adds the SH68F90 it_sources).
    add_hw(interrupt = new cl_sh68f90_interrupt(this));
    interrupt->init();
}

/* End of s51.src/sh68f90.cc */
