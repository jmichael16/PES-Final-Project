/* -----------------------------------------------------------------------------
 * spi_pixl.c 
 *
 * @author  Jake Michael
 * @date    
 * @rev    
 * -----------------------------------------------------------------------------
 */
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "MKL25Z4.h"
#include "spi_pixl.h"
#include "timer.h"

// defines for pixels / colors
#define PIXL_0          (0x80)      // the 0 bit for spi output
#define PIXL_1          (0xFE)      // the 1 bit for spi output
#define PIXL_RESET      (70)        // microseconds
#define BITS_PER_PIXEL  (24)        // 24-bit G-R-B output for neopixels
#define RED_MASK        (0xFF0000)
#define GRN_MASK        (0x00FF00)
#define BLU_MASK        (0x0000FF)

// the board pins for SPI peripheral:
#define SPI_PORT     (PORTD)
#define SPI_MUX_ALT  (2)
#define SPI_SCK_PIN  (1)
#define SPI_MOSI_PIN (2) 

// the DMA trigger setting for SPI0
#define DMA_SPI0_TX_TRIG (17)

// the bytes that will go over spi to drive neopixels
static uint8_t spi_output[NUM_PIXELS*BITS_PER_PIXEL];

// flag indicating whether spi/dma has finished transmitting
static volatile bool is_pixel_xmit_complete;

// see .h for more details
int spi_pixl_update(uint32_t *rgb_24bit_colors, uint32_t npixels) {

  // error case: 
  if (rgb_24bit_colors == NULL || npixels <= 0) return -1;

  // wait while transmission completes
  while(!is_pixel_xmit_complete) {;}

  // reset timer to start reset sequence
  reset_timer();
  
  // disable spi to send bus low for reset pulse while we 
  // do some processing
  SPI_PORT->PCR[SPI_MOSI_PIN] &= ~PORT_PCR_MUX_MASK;

  int i;
  uint32_t mask;
  int spi_idx = 0;
  uint32_t *color;
  color = rgb_24bit_colors;
  
  for (i=0; i<npixels; i++) {

    // generate spi bytes for green first
    for (mask=0x8000; mask>0x80; mask>>=1) {
       if ((*color)&mask) {
         spi_output[spi_idx] = PIXL_1;
       } else {
         spi_output[spi_idx] = PIXL_0;
       }
       spi_idx++;
    }

    // generate spi bytes for red next
    for (mask=0x800000; mask>0x8000; mask>>=1) {
      if (*color & mask) {
        spi_output[spi_idx] = PIXL_1;
      } else {
        spi_output[spi_idx] = PIXL_0;
      }
      spi_idx++;
    }

    // generate spi bytes for blue next
    for (mask=0x80; mask>0; mask>>=1) {
      if (*color & mask) {
        spi_output[spi_idx] = PIXL_1;
      } else {
        spi_output[spi_idx] = PIXL_0;
      }
      spi_idx++;
    }
  color++;
  } // end for loop over npixels



  // wait for timer to indicate reset 
  while (get_timer_us() < PIXL_RESET) {;}

  // load BCR with bytes for transfer
  //   via calculation:
  //    
  //      1 Byte     24 RGB Bits    
  //    --------- * ------------- * 8 pixels = 192 bytes
  //    1 RGB Bit       pixel
  //
  DMA0->DMA[1].DSR_BCR |= DMA_DSR_BCR_BCR(BITS_PER_PIXEL*NUM_PIXELS);

  // setup source register to start at index+1 of spi_output
  DMA0->DMA[1].SAR = DMA_SAR_SAR((uint32_t)&(spi_output));

  //SPI0->D = spi_output[0];

  // set flag 
  is_pixel_xmit_complete = false; 

  // re-enable the spi mux bits for transfer
  SPI_PORT->PCR[SPI_MOSI_PIN] |= PORT_PCR_MUX(SPI_MUX_ALT);
  // re-enable peripheral request
  DMA0->DMA[1].DCR |= DMA_DCR_ERQ_MASK;

  return 0;
}

// see .h for more details
void spi_pixl_init() {

  // init systick, dma1, spi0
  systick_init();
  _init_dma1();
  _init_spi0();

  // set xmit complete flag
  is_pixel_xmit_complete = true;
}

// steps from datasheet 37.4.4.1 for configuring SPI TX via DMA:
// configure DMA for spi transmission
// configure spi before transmission
// set SPE1 to start transmission
// read spi status reg
// write first byte to spi data reg via cpu
// set TXDMAE to enable  transmit by dma
// wait for dma interrupt indicating end of spi xmit

// see .h for more details
void _init_spi0() {

  // enable clock gating to SPI0
  SIM->SCGC4 |= SIM_SCGC4_SPI0_MASK;
  
  // set SPI as master and spi enable - pg. 662
  SPI0->C1 = (SPI_C1_MSTR_MASK | SPI_C1_SPE_MASK);

  // transmit DMA enabled - pg. 663
  // revert SS pin to GPIO not controlled by SPI
  SPI0->C2 = (SPI_C2_TXDMAE_MASK | SPI_C2_MODFEN(0));

  // configure SPI clock settings:
  //  SPI baud rate prescaler of 1
  //  SPI baud rate divide by 4
  //  24/4 yields 6 MHz clock for neopixel timing usage 
  SPI0->BR = (SPI_BR_SPPR(0) | SPI_BR_SPR(1)); 

  // when transmit DMA request enabled, SPTEF is cleared when 
  // DMA transfer completes
  
  // enable SPI outputs to PORTD:
  //    PTD1 - SPI0_SCK  - ALT2 
  //    PTD2 - SPI0_MOSI - ALT2
  SIM->SCGC5 |= SIM_SCGC5_PORTD_MASK;
  SPI_PORT->PCR[SPI_SCK_PIN]  &= ~PORT_PCR_MUX_MASK;
  SPI_PORT->PCR[SPI_SCK_PIN]  |= PORT_PCR_MUX(SPI_MUX_ALT);
  SPI_PORT->PCR[SPI_MOSI_PIN] &= ~PORT_PCR_MUX_MASK;
}

// see .h for more details 
void DMA1_IRQHandler() {

  // clear done flag
  DMA0->DMA[1].DSR_BCR |= DMA_DSR_BCR_DONE_MASK;
  
  // just set a flag that DMA transf. complete for last series
  is_pixel_xmit_complete = true;

  // send spi bus low
  SPI_PORT->PCR[SPI_MOSI_PIN] &= ~PORT_PCR_MUX_MASK;
}

// see .h for more details
void _init_dma1() {
  // enable clock gating to DMA
  SIM->SCGC7 |= SIM_SCGC7_DMA_MASK;
  SIM->SCGC6 |= SIM_SCGC6_DMAMUX_MASK;

  // disable during config.
  DMAMUX0->CHCFG[1] = 0;

  // see pg. 357 of datasheet
  // EINT  - Enable interrupts on transfer completion
  // ERQ   - Enable peripheral request 
  // SINC  - Enable source increment after transfer
  // SSIZE - sets source size to 8 bits 
  // DSIZE - sets destination size to 8 bits
  // D_REQ - DCR ERQ bit is cleared when BCR is depleted
  // CS    - force single read/write per request (cycle steal)
  DMA0->DMA[1].DCR = ( DMA_DCR_EINT_MASK  |
                      // DMA_DCR_ERQ_MASK   |
                       DMA_DCR_SINC_MASK  |
                       DMA_DCR_SSIZE(1)   |
                       DMA_DCR_DSIZE(1)   |
                       DMA_DCR_D_REQ_MASK |
                       DMA_DCR_CS_MASK    );
  
  // set destination address as SPI0->D  
  DMA0->DMA[1].DAR = DMA_DAR_DAR((uint32_t)&(SPI0->D));
  
  // configure the interrupt upon transfer complete, priority 
  NVIC_SetPriority(DMA1_IRQn, 3);
  NVIC_ClearPendingIRQ(DMA1_IRQn);
  NVIC_EnableIRQ(DMA1_IRQn);

  // enable DMA, triggered by SPI0 xmit datasheet 3.4.8.1
  DMAMUX0->CHCFG[1] = (DMAMUX_CHCFG_SOURCE(DMA_SPI0_TX_TRIG) |
                       DMAMUX_CHCFG_ENBL_MASK);

  // clear the flag to stop any transfer
  // DMA0->DMA[1].DSR_BCR |= DMA_DSR_BCR_DONE_MASK;
}
