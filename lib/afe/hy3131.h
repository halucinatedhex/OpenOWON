/*
 * HYCON HY3131 - 50,000-count DMM analog front end.
 *
 * This is the measurement chip on the XDM1241's analog board. Register map
 * and protocol taken from DS-HY3131-V10_EN (docs/datasheets/).
 *
 *   AD1   24-bit sigma-delta, the main reading        (regs 0x00-0x02)
 *   AD2   19-bit high speed                            (regs 0x03-0x05)
 *   AD3   19-bit high speed (HY3131 only)
 *   LPF   low-pass filtered AD2                        (regs 0x06-0x08)
 *   RMS   40-bit true-RMS accumulator                  (regs 0x09-0x0D)
 *   PKH   peak hold min/max                            (regs 0x0E-0x13)
 *   CTA/B/C  frequency / period counters               (regs 0x15-0x1D)
 *
 * Transport is 4-wire SPI (CS, CK, DI, DO) plus IRQ. On this board the bus
 * crosses a galvanic isolation barrier (U5, with a milled slot beneath it),
 * and the stock firmware bit-bangs it on GPIO rather than using the MCU's
 * SPI peripheral - there are no SPI peripheral references anywhere in the
 * stock image.
 *
 * Pin mapping, read out of the stock bit-bang at 0x080077C8 and matching
 * LQFP48 pins 25-29 on the connector to the analog board:
 *
 *   PB12  pin 25   CS    (NSS)      output, active low
 *   PB13  pin 26   CK    (CLK)      output
 *   PB14  pin 27   DO    (SDO)      input   - read with ReadInputDataBit
 *   PB15  pin 28   DI    (MOSI)     output  - driven from the byte's MSB
 *   PA8   pin 29   IRQ?             unconfirmed; the fifth connector pin
 *
 * Those are also the SPI2 block pins, so a replacement firmware can use the
 * hardware peripheral instead of bit-banging - subject to what the isolator
 * will pass.
 */
#ifndef HY3131_H
#define HY3131_H

#include <stdint.h>
#include <stdbool.h>

/* ---- register map ------------------------------------------------------ */
#define HY_AD1_DATA   0x00   /* 24-bit, two's complement, 0x7FFFFF .. 0x800000 */
#define HY_AD2_DATA   0x03   /* 19-bit, sign-extended into bits 23:19          */
#define HY_LPF_DATA   0x06
#define HY_RMS_DATA   0x09   /* 40-bit */
#define HY_PKHMIN     0x0E
#define HY_PKHMAX     0x11
#define HY_CTSTA      0x14
#define HY_CTC        0x15
#define HY_CTB        0x18
#define HY_CTA        0x1B
#define HY_INTF       0x1E   /* flags:  BORF .. RMSF LPFF AD1F AD2F CTF */
#define HY_INTE       0x1F   /* enables */
#define HY_R20        0x20
#define HY_R21        0x21
#define HY_R22        0x22   /* AD1OS<2:0> | AD1CHOP<1:0> | AD1OSR<2:0>  <- rate */
#define HY_R23        0x23   /* ENAD1 | AD1RG | input buffers */
#define HY_R24        0x24   /* SAD1FP<3:0> | SDIO | SAD1FN<2:0> */
#define HY_R25        0x25   /* AD2IG<1:0> | AD1IG<1:0> | SACM<1:0> | OPS */
#define HY_R26        0x26   /* ENAD2 | ENCHOPAD2 | AD2RG | SAD2CLK | AD2OSR */
#define HY_R27        0x27   /* AD2 input mux: SAD2IP/IN/RH/RL */
#define HY_R28        0x28   /* SAD1RH<2:0> | SAD1RL<2:0>   <- AD1 reference */
#define HY_R29        0x29   /* ENRMS | ENLPF | LPFBW<2:0> | ENPKH | PKHSEL */
#define HY_R2A        0x2A   /* PA switch network, 2 taps per register ... */
#define HY_R2B        0x2B
#define HY_R2C        0x2C
#define HY_R2D        0x2D
#define HY_R2E        0x2E   /* ... PA<9> down to PA<0>, PS/DS/FS/SS each */
#define HY_R2F        0x2F   /* ENVS | SMODE<6:0>           <- function select */
#define HY_R30        0x30   /* SREFO | ACC<6:0> */
#define HY_R31        0x31   /* ENREFO | ENBIAS | SAGND<1:0> | SFUVR<3:0> */
#define HY_R32        0x32   /* ENOP2 | SOP2P<2:0> | ENOP1 | SOP1P<2:0> */
#define HY_R33        0x33   /* OP1CHOP | ENOSC | ENXI | SFT1 | SAD1I */
#define HY_R34        0x34   /* ENAD3 | ENCHOPAD3 | AD3RG | SVXI | SDO23 */
#define HY_R35        0x35   /* SAD3IP | SAD3IN | AD3IG */
#define HY_R36        0x36   /* SPHACAL<7:0> */

/* interrupt flag bits in HY_INTF / HY_INTE */
#define HY_F_CT       (1u << 0)
#define HY_F_AD2      (1u << 1)
#define HY_F_AD1      (1u << 2)
#define HY_F_LPF      (1u << 3)
#define HY_F_RMS      (1u << 4)

/* The PA network is a resistor ladder: PA<9>=10M, <8>=1M, <7>=100k,
 * <6>=10k, <5>=1k. Each tap has four switches packed two taps per register,
 * so tap N lives in register 0x2A + (N / 2), low nibble for even N. */
#define HY_PA_REG(n)    (uint8_t)(HY_R2A + ((n) >> 1))
#define HY_PA_SHIFT(n)  (((n) & 1) ? 4 : 0)
#define HY_PA_SS        (1u << 0)
#define HY_PA_FS        (1u << 1)
#define HY_PA_DS        (1u << 2)
#define HY_PA_PS        (1u << 3)

/* ---- transport ---------------------------------------------------------
 * Supply these for your board. The stock firmware bit-bangs them; a
 * replacement may use a hardware SPI peripheral instead, mode 0, MSB first.
 * The isolation barrier limits usable clock rate - measure before pushing it.
 */
typedef struct {
    void (*cs)(bool asserted);     /* active low on the wire */
    void (*clk)(bool level);
    void (*mosi)(bool level);
    bool (*miso)(void);
    void (*delay)(void);           /* half a bit period */
} hy3131_io_t;

/* ---- protocol ----------------------------------------------------------
 * First byte on the wire is (address << 1) | rw, MSB first, where rw is 1
 * for read. On a READ the ninth bit must be idle before data appears. On a
 * WRITE data follows immediately. The address auto-increments for as long as
 * CS stays asserted, so multi-byte registers are a single burst.
 */
void     hy3131_init(const hy3131_io_t *io);
uint8_t  hy3131_read8(uint8_t reg);
void     hy3131_write8(uint8_t reg, uint8_t val);
void     hy3131_read_burst(uint8_t reg, uint8_t *dst, unsigned n);
void     hy3131_write_burst(uint8_t reg, const uint8_t *src, unsigned n);

/* Sign-extend a 24-bit two's-complement ADC word. */
int32_t  hy3131_sext24(uint32_t raw);

/* Convenience: read AD1/AD2/LPF as signed values. */
int32_t  hy3131_read_ad1(void);
int32_t  hy3131_read_ad2(void);
int64_t  hy3131_read_rms(void);

#endif
