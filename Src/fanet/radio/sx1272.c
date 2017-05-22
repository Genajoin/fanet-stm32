
#include <stdlib.h>
#include <stdbool.h>
#include <stdio.h>

#include "main.h"
#include "stm32l4xx.h"
#include "stm32l4xx_hal_gpio.h"
#include "stm32l4xx_hal_spi.h"

#include "../radio/sx1272.h"

bool sx1272_armed = false;
sx_region_t sx1272_region = {.channel = 0, .dBm = 0};
irq_callback sx1272_irq_cb = NULL;
SPI_HandleTypeDef *sx1272_spi = NULL;

//1% too slow, for24Mhz
void delay_us(const int us)
{
	uint32_t i = us * 2;
	while (i-- > 0)
	{
		__NOP();
		__NOP();
		__NOP();
		__NOP();
		__NOP();
		__NOP();
		__NOP();
	}
}

/*
 * private
 */

void sx_select()
{
	HAL_NVIC_DisableIRQ(SXDIO0_EXTI8_nIRQ);
	HAL_GPIO_WritePin(SXSEL_GPIO_Port, SXSEL_Pin, GPIO_PIN_RESET);
}

void sx_unselect()
{
	HAL_GPIO_WritePin(SXSEL_GPIO_Port, SXSEL_Pin, GPIO_PIN_SET);
	HAL_NVIC_EnableIRQ(SXDIO0_EXTI8_nIRQ);
}

uint8_t sx_readRegister(uint8_t address)
{
	if(sx1272_spi == NULL)
		return 0;

	sx_select();

	/* bit 7 cleared to read in registers */
	address &= 0x7F;

	HAL_SPI_Transmit(sx1272_spi, &address, 1, 100);
	uint8_t value;
	HAL_SPI_Receive(sx1272_spi, &value, 1, 100);
	sx_unselect();

#if (SX1272_debug_mode > 1)
	Serial.print(F("## SX1272 read reg("));
	Serial.print(address, HEX);
	Serial.print(F(")="));
	Serial.print(value, HEX);
	Serial.println();
#endif

	return value;
}

void sx_writeRegister(uint8_t address, uint8_t data)
{
	sx_select();
	/* bit 7 set to read from registers */
	address |= 0x80;

	uint8_t tx[2] = {address, data};
	HAL_SPI_Transmit(sx1272_spi, tx, 2, 100);
	sx_unselect();

#if (SX1272_debug_mode > 1)
	Serial.print(F("## SX1272 write reg("));
	Serial.print(address, HEX);
	Serial.print(F(")="));
	Serial.print(data, HEX);
	Serial.println();
#endif
}

bool sx_setOpMode(uint8_t mode)
{
#if (SX1272_debug_mode > 0)
	switch (mode)
	{
	case LORA_RXCONT_MODE:
		Serial.println(F("## SX1272 opmode: rx continous"));
	break;
	case LORA_TX_MODE:
		Serial.println(F("## SX1272 opmode: tx"));
	break;
	case LORA_SLEEP_MODE:
		Serial.println(F("## SX1272 opmode: sleep"));
	break;
	case LORA_STANDBY_MODE:
		Serial.println(F("## SX1272 opmode: standby"));
	break;
	case LORA_CAD_MODE:
		Serial.println(F("## SX1272 opmode: cad"));
	break;
	default:
		Serial.println(F("## SX1272 opmode: unknown"));
	}
#endif

	/* set mode */
	sx_writeRegister(REG_OP_MODE, mode);

	/* set rf analog end */
	switch (mode)
	{
	case LORA_RXSINGLE_MODE:
	case LORA_RXCONT_MODE:
	case LORA_CAD_MODE:
#ifdef SXTX_Pin
		HAL_GPIO_WritePin(SXTX_GPIO_Port, SXTX_Pin, GPIO_PIN_RESET);
#endif
#ifdef SXRX_Pin
		HAL_GPIO_WritePin(SXRX_GPIO_Port, SXRX_Pin, GPIO_PIN_SET);
#endif
	break;
	case LORA_TX_MODE:
#ifdef SXRX_Pin
		HAL_GPIO_WritePin(SXRX_GPIO_Port, SXRX_Pin, GPIO_PIN_RESET);
#endif
#ifdef SXTX_Pin
		HAL_GPIO_WritePin(SXTX_GPIO_Port, SXTX_Pin, GPIO_PIN_SET);
#endif
	break;
	default:
#ifdef SXTX_Pin
		HAL_GPIO_WritePin(SXTX_GPIO_Port, SXTX_Pin, GPIO_PIN_RESET);
#endif
#ifdef SXRX_Pin
		HAL_GPIO_WritePin(SXRX_GPIO_Port, SXRX_Pin, GPIO_PIN_RESET);
#endif
		break;
	}

	/* wait for frequency synthesis, 5ms timeout */
	uint8_t opmode = 0;
	for(int i=0; i<50 && opmode != mode; i++)
	{
		delay_us(100);
		opmode = sx_readRegister(REG_OP_MODE);
	}

#if (SX1272_debug_mode > 1)
	Serial.print(F("## SX1272 opmode: "));
	Serial.println(opmode, HEX);
#endif

	return mode == opmode;
}

uint8_t sx_getOpMode(void)
{
	return sx_readRegister(REG_OP_MODE);
}

void sx_writeFifo(uint8_t addr, uint8_t *data, int length)
{
#if (SX1272_debug_mode > 1)
	Serial.println();
	Serial.print(F("## SX1272 write fifo, length="));
	Serial.print(length, DEC);

	Serial.print(" [");
	for (int i = 0; i < length; i++)
	{
		Serial.print(data[i], HEX);
		if(i < length-1)
			Serial.print(", ");
	}
	Serial.println("]");
#endif

	/* select location */
	sx_writeRegister(REG_FIFO_ADDR_PTR, addr);

	/* upload data */
	sx_select();
	uint8_t reg = REG_FIFO | 0x80;
	HAL_SPI_Transmit(sx1272_spi, &reg, 1, 100);
	HAL_SPI_Transmit(sx1272_spi, data, length, 100);
        sx_unselect();
}

void sx_readFifo(uint8_t addr, uint8_t *data, int length)
{
	/* select location */
	sx_writeRegister(REG_FIFO_ADDR_PTR, addr);

	/* upload data */
	sx_select();
	uint8_t reg = REG_FIFO & 0x7F;
	HAL_SPI_Transmit(sx1272_spi, &reg, 1, 100);
	HAL_SPI_Receive(sx1272_spi, data, length, 100);
	sx_unselect();

#if (SX1272_debug_mode > 1)
	Serial.print(F("## SX1272 read fifo, length="));
	Serial.print(length, DEC);

	Serial.print(" [");
	for (int i = 0; i < length; i++)
	{
		Serial.print(data[i], HEX);
		if(i < length-1)
			Serial.print(", ");
	}
	Serial.println("]");
#endif
}

void sx_setDio0Irq(int mode)
{
	uint8_t map1 = sx_readRegister(REG_DIO_MAPPING1) & 0x3F;
	sx_writeRegister(REG_DIO_MAPPING1, map1 | mode);
}


/*
 * public
 */


bool sx1272_init(SPI_HandleTypeDef *spi)
{
#if (SX1272_debug_mode > 1)
	Serial.println();
	Serial.println(F("## SX1272 begin"));
#endif

	sx1272_spi = spi;

	/* Pins */
	//note: are assumed to be configured
	sx_unselect();

	/* Configure SPI */
	//note: spi configured in spi.c

	/* Reset pulse for LoRa module initialization */
	HAL_GPIO_WritePin(SXRESET_GPIO_Port, SXRESET_Pin, GPIO_PIN_SET);
	HAL_Delay(1);
	HAL_GPIO_WritePin(SXRESET_GPIO_Port, SXRESET_Pin, GPIO_PIN_RESET);
	HAL_Delay(6);

	/* set Lora mode, this is only possible during sleep -> do it twice */
	sx_setOpMode(LORA_SLEEP_MODE);
	sx_setOpMode(LORA_SLEEP_MODE);

	/* set standby mode */
	bool mode = sx_setOpMode(LORA_STANDBY_MODE);

	/* some more tweaks here */
	/* disable LowPnTxPllOff -> low phase noise PLL in transmit mode, however more current */
	sx_writeRegister(REG_PA_RAMP, 0x09);
	/* disable over current detection */
	sx_writeRegister(REG_OCP, 0x1B);

	return mode;
}
void sx1272_setBandwidth(uint8_t bw)
{
#if (SX1272_debug_mode > 0)
	Serial.print(F("## SX1272 BW: "));
	Serial.println(bw, HEX);
#endif

	/* store state */
	uint8_t opmode = sx_getOpMode();

	/* set appropriate mode */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(LORA_STANDBY_MODE);

	uint8_t config1 = sx_readRegister(REG_MODEM_CONFIG1);
	config1 = (bw&BW_MASK) | (config1&~BW_MASK);
	if(bw == BW_125)
	{
		uint8_t sf = sx1272_getSpreadingFactor();
		if(sf == SF_11 || sf == SF_12)
			sx1272_setLowDataRateOptimize(true);
	}
	sx_writeRegister(REG_MODEM_CONFIG1, config1);

	/* restore state */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(opmode);
}

uint8_t sx1272_getBandwidth(void)
{
	uint8_t bw = sx_readRegister(REG_MODEM_CONFIG1) & BW_MASK;

#if (SX1272_debug_mode > 0)
	Serial.print(F("## SX1272 BW="));
	Serial.println(bw, HEX);
#endif

	return bw;
}

void sx1272_setLowDataRateOptimize(bool ldro)
{
#if (SX1272_debug_mode > 0)
	Serial.print(F("## SX1272 LDRO: "));
	Serial.println(!!ldro, DEC);
#endif

	/* store state */
	uint8_t opmode = sx_getOpMode();

	/* set appropriate mode */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(LORA_STANDBY_MODE);

	uint8_t config1 = sx_readRegister(REG_MODEM_CONFIG1);
	config1 = (config1&0xFE) | !!ldro;
	sx_writeRegister(REG_MODEM_CONFIG1, config1);

	/* restore state */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(opmode);
}

void sx1272_setPayloadCrc(bool crc)
{
#if (SX1272_debug_mode > 0)
	Serial.print(F("## SX1272 CRC: "));
	Serial.println(!!crc, DEC);
#endif

	/* store state */
	uint8_t opmode = sx_getOpMode();

	/* set appropriate mode */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(LORA_STANDBY_MODE);

	uint8_t config1 = sx_readRegister(REG_MODEM_CONFIG1);
	config1 = (config1&0xFD) | !!crc<<1;
	sx_writeRegister(REG_MODEM_CONFIG1, config1);

	/* restore state */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(opmode);
}

void sx1272_setSpreadingFactor(uint8_t spr)
{
#if (SX1272_debug_mode > 0)
	Serial.print(F("## SX1272 SF: "));
	Serial.println(spr, HEX);
#endif

	/* store state */
	uint8_t opmode = sx_getOpMode();

	/* set appropriate mode */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(LORA_STANDBY_MODE);

	uint8_t config2 = sx_readRegister(REG_MODEM_CONFIG2);
	config2 = (spr&SF_MASK) | (config2&~SF_MASK);

	/* LowDataRateOptimize (Mandatory with SF_11 or SF_12 if BW_125) */
	if((spr == SF_11 || spr == SF_12) && sx1272_getBandwidth() == BW_125)
		sx1272_setLowDataRateOptimize(true);

	sx_writeRegister(REG_MODEM_CONFIG2, config2);

	/* Check if it is neccesary to set special settings for SF=6 */
	if (spr == SF_6)
	{
		// Mandatory headerOFF with SF = 6 (Implicit mode)
		sx1272_setExplicitHeader(false);
		sx_writeRegister(REG_DETECT_OPTIMIZE, 0x05);
		sx_writeRegister(REG_DETECTION_THRESHOLD, 0x0C);
	}
	else
	{
		sx_writeRegister(REG_DETECT_OPTIMIZE, 0x03);
		sx_writeRegister(REG_DETECTION_THRESHOLD, 0x0A);
	}

	/* restore state */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(opmode);
}

uint8_t sx1272_getSpreadingFactor(void)
{
	uint8_t sf = sx_readRegister(REG_MODEM_CONFIG2) & SF_MASK;

#if (SX1272_debug_mode > 0)
	Serial.print(F("## SX1272 SF="));
	Serial.println(sf, HEX);
#endif

	return sf;
}

void sx1272_setCodingRate(uint8_t cr)
{
#if (SX1272_debug_mode > 0)
	Serial.print(F("## SX1272 CR: "));
	Serial.println(cr, HEX);
#endif

	/* store state */
	uint8_t opmode = sx_getOpMode();

	/* set appropriate mode */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(LORA_STANDBY_MODE);

	uint8_t config1 = sx_readRegister(REG_MODEM_CONFIG1);
	config1 = (cr&CR_MASK) | (config1&~CR_MASK);
	sx_writeRegister(REG_MODEM_CONFIG1, config1);

	/* restore state */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(opmode);
}

uint8_t sx1272_getCodingRate(void)
{
	uint8_t cr = sx_readRegister(REG_MODEM_CONFIG1) & CR_MASK;

#if (SX1272_debug_mode > 0)
	Serial.print(F("## SX1272 CR="));
	Serial.println(cr, HEX);
#endif

	return cr;
}

bool sx1272_setChannel(uint32_t ch)
{
#if (SX1272_debug_mode > 0)
	Serial.print(F("## SX1272 CH: "));
	Serial.println(ch, HEX);
#endif

	/* store state */
	uint8_t opmode = sx_getOpMode();

	/* set appropriate mode */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(LORA_STANDBY_MODE);

	uint8_t freq3 = (ch >> 16) & 0x0FF;		// frequency channel MSB
	uint8_t freq2 = (ch >> 8) & 0xFF;		// frequency channel MID
	uint8_t freq1 = ch & 0xFF;			// frequency channel LSB

	sx_writeRegister(REG_FRF_MSB, freq3);
	sx_writeRegister(REG_FRF_MID, freq2);
	sx_writeRegister(REG_FRF_LSB, freq1);

	/* restore state */
	bool ret = true;
	if(opmode != LORA_STANDBY_MODE)
		ret = sx_setOpMode(opmode);

	#if (SX1272_debug_mode > 1)
		Serial.println();
		Serial.print(F("## SX1272 CH="));
		Serial.println(ch, HEX);
	#endif

	return ret;
}

uint32_t sx1272_getChannel(void)
{
	uint8_t freq3 = sx_readRegister(REG_FRF_MSB);	// frequency channel MSB
	uint8_t freq2 = sx_readRegister(REG_FRF_MID);	// frequency channel MID
	uint8_t freq1 = sx_readRegister(REG_FRF_LSB);	// frequency channel LSB
	uint32_t ch = ((uint32_t) freq3 << 16) + ((uint32_t) freq2 << 8) + (uint32_t) freq1;

#if (SX1272_debug_mode > 0)
	Serial.println();
	Serial.print(F("## SX1272 CH="));
	Serial.println(ch, HEX);
#endif

	return ch;
}

void sx1272_setExplicitHeader(bool exhdr)
{
#if (SX1272_debug_mode > 0)
	Serial.print(F("## SX1272 explicit header: "));
	Serial.println(exhdr, DEC);
#endif

	/* store state */
	uint8_t opmode = sx_getOpMode();

	/* set appropriate mode */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(LORA_STANDBY_MODE);

	/* bit2: 0->explicit 1->implicit */
	uint8_t config1 = sx_readRegister(REG_MODEM_CONFIG1);
	config1 = (config1&0xFB) | !exhdr<<2;
	sx_writeRegister(REG_MODEM_CONFIG1, config1);

	/* restore state */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(opmode);
}

void sx1272_setLnaGain(uint8_t gain, bool lnaboost)
{
	/* store state */
	uint8_t opmode = sx_getOpMode();

	/* set appropriate mode */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(LORA_STANDBY_MODE);

#if (SX1272_debug_mode > 0)
	Serial.print(F("## SX1272 lna gain: "));
	Serial.println((gain<<LNAGAIN_OFFSET) | (lnaboost?0x3:0x0), HEX);
#endif

	sx_writeRegister(REG_LNA, (gain<<LNAGAIN_OFFSET) | (lnaboost?0x3:0x0));

	/* restore state */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(opmode);
}

//note: we currently only support PA_BOOST
bool sx1272_setPower(int pwr)
{
	/* clamp pwr requirement */
	if(pwr < 2)
		pwr = 2;
	if(pwr > 20)
		pwr = 20;

#if (SX1272_debug_mode > 0)
	Serial.print(F("## SX1272 dBm: "));
	Serial.println(pwr, DEC);
#endif

	/* store state */
	uint8_t opmode = sx_getOpMode();

	/* set appropriate mode */
	if(opmode != LORA_STANDBY_MODE)
		sx_setOpMode(LORA_STANDBY_MODE);

	if(pwr >= 18)
	{
		/* high power mode */
		sx_writeRegister(REG_PA_DAC, PA_DAC_HIGH_PWR);
		pwr -= 5;
	}
	else
	{
		/* normal mode */
		sx_writeRegister(REG_PA_DAC, PA_DAC_DEFAULT_PWR);
		pwr -= 2;
	}

	/* PA output pin to PA_BOOST */
	sx_writeRegister(REG_PA_CONFIG, 0x80 | pwr);

	/* restore state */
	if(opmode != LORA_STANDBY_MODE)
		return sx_setOpMode(opmode);
	else
		return true;
}

bool sx1272_setArmed(bool rxmode)
{
	/* store state */
	uint8_t opmode = sx_getOpMode();

	if(rxmode && (opmode == LORA_SLEEP_MODE || opmode == LORA_STANDBY_MODE))
	{
		/* enable rx */
		bool ret = sx1272_receiveStart();
		if(ret)
			sx1272_armed = true;
		return ret;
	}
	else if(!rxmode)
	{
		/* enter power save */
		bool ret = sx_setOpMode(LORA_SLEEP_MODE);
		if(ret)
			sx1272_armed = false;
		return ret;
	}
	return true;
}

bool sx1272_isArmed(void)
{
	return sx1272_armed;
}

//note: do not use hardware based interrupts here
void sx1272_irq(void)
{
	#if (SX1272_debug_mode > 0)
		Serial.print(F("## SX1272 irq: "));
	#endif

	sx_setOpMode(LORA_STANDBY_MODE);

	if(sx1272_irq_cb == NULL)
	{
#if (SX1272_debug_mode > 0)
		Serial.println();
		Serial.println(F("## No callback. Error"));
#endif

		sx_writeRegister(REG_IRQ_FLAGS, 0xFF);
		return;
	}

	uint8_t what = sx_readRegister(REG_IRQ_FLAGS);
#if (SX1272_debug_mode > 0)
	Serial.print(what, HEX);
	Serial.print("...");
	//Serial.println(readRegister(REG_HOP_CHANNEL), HEX);
#endif
	/* Tx done */
	//if(what & IRQ_TX_DONE)
	//{
		//nothing to do...
	//}

	/* Rx done, CRC is valid and CRC was transmitted */
	if((what & IRQ_RX_DONE) && !(what & IRQ_PAYLOAD_CRC_ERROR) && (sx_readRegister(REG_HOP_CHANNEL) & CRC_ON_PAYLOAD))
	{
		/* retrieve data */
		const int received = sx_readRegister(REG_RX_NB_BYTES);

		/* callback function which is supposed to read the fifo */
		if(received > 0 && sx1272_irq_cb != NULL)
			sx1272_irq_cb(received);
	}

	/* clear all flag */
	sx_writeRegister(REG_IRQ_FLAGS, 0xFF);

	/* switch irq behavior back and re-enter rx mode */
	sx1272_receiveStart();
}


void sx1272_setIrqReceiver(irq_callback cb)
{
	sx_setOpMode(LORA_STANDBY_MODE);
	sx_setDio0Irq(DIO0_RX_DONE);

	/* clear all flag */
	sx_writeRegister(REG_IRQ_FLAGS, 0xFF);

	sx1272_irq_cb = cb;
	HAL_NVIC_EnableIRQ(SXDIO0_EXTI8_nIRQ);
}

void sx1272_clrIrqReceiver(void)
{
	sx_setOpMode(LORA_STANDBY_MODE);

	HAL_NVIC_DisableIRQ(SXDIO0_EXTI8_nIRQ);
	sx1272_irq_cb = NULL;
}

bool sx1272_setRegion(sx_region_t region)
{
	bool success = sx1272_setChannel(region.channel) && sx1272_setPower(region.dBm);

	if(success)
		sx1272_region = region;

	return success;
}

int sx1272_getRssi(void)
{
	/* get values */
	const int pktsnr = (int8_t) sx_readRegister(REG_PKT_SNR_VALUE);
	int rssi = -139 + sx_readRegister(REG_PKT_RSSI_VALUE);
	if(pktsnr < 0)
		rssi += ((pktsnr-2)/4);			//note: correct rounding for negative numbers

	return rssi;
}

/***********************************/
int sx1272_sendFrame(uint8_t *data, int length)
{
#if (SX1272_debug_mode > 0)
	Serial.print(F("## SX1272 send frame..."));
#endif

	uint8_t mode = sx_getOpMode() & LORA_MODE_MASK;

	/* are we transmitting anyway? */
	if(mode == LORA_TX_MODE)
		return TX_TX_ONGOING;

	/* in case of receiving, is it ongoing? */
	for(int i=0; i<400;i++)
	{
		if((mode == LORA_RXCONT_MODE || mode == LORA_RXSINGLE_MODE) && (sx_readRegister(REG_MODEM_STAT) & 0x0B))
			return TX_RX_ONGOING;
		delay_us(10);
	}

	/*
	 * CAD
	 */

	sx_setOpMode(LORA_STANDBY_MODE);
	sx_writeRegister(REG_IRQ_FLAGS, IRQ_CAD_DONE | IRQ_CAD_DETECTED);	/* clearing flags */
	sx_setOpMode(LORA_CAD_MODE);

	/* wait for CAD completion */
	uint8_t iflags;
	while(((iflags=sx_readRegister(REG_IRQ_FLAGS)) & IRQ_CAD_DONE) == 0)
		delay_us(1);

	if(iflags & IRQ_CAD_DETECTED)
	{
		/* re-establish old mode */
		if(mode == LORA_RXCONT_MODE || mode == LORA_RXSINGLE_MODE)
			sx_setOpMode(mode);

		return TX_RX_ONGOING;
	}

	sx_setOpMode(LORA_STANDBY_MODE);

	//todo: check fifo is empty, no rx data..

	/* upload frame */
	sx_writeFifo(0x00, data, length);
	sx_writeRegister(REG_FIFO_TX_BASE_ADDR, 0x00);
	sx_writeRegister(REG_PAYLOAD_LENGTH_LORA, length);

	/* prepare irq */
	if(sx1272_irq_cb)
	{
		/* clear flag */
		sx_writeRegister(REG_IRQ_FLAGS, IRQ_TX_DONE);
		sx_setDio0Irq(DIO0_TX_DONE);
	}

	sx_setOpMode(LORA_TX_MODE);

	/* bypass waiting */
	if(sx1272_irq_cb)
	{
#if (SX1272_debug_mode > 1)
		Serial.println("INT");
#endif
		return TX_OK;
	}

	for(int i=0; i<100 && sx_getOpMode() == LORA_TX_MODE; i++)
	{
#if (SX1272_debug_mode > 0)
		Serial.print(".");
#endif
		HAL_Delay(5);
	}
#if (SX1272_debug_mode > 0)
	Serial.println("done");
#endif
	return TX_OK;
}

int sx1272_receiveFrame(uint8_t *data, int max_length)
{
#if (SX1272_debug_mode > 0)
	Serial.print(F("## SX1272 receive frame..."));
#endif

	sx_setOpMode(LORA_STANDBY_MODE);

	/* prepare receiving */
	sx_writeRegister(REG_FIFO_RX_BASE_ADDR, 0x00);
	sx_writeRegister(REG_PAYLOAD_LENGTH_LORA, max_length);
	//writeRegister(REG_SYMB_TIMEOUT_LSB, 0xFF);	//test max payload

	sx_setOpMode(LORA_RXCONT_MODE);

	/* wait for rx or timeout */
	for(int i=0; i<400 && !(sx_readRegister(REG_IRQ_FLAGS) & 0x40); i++)
	{
#if (SX1272_debug_mode > 0)
		Serial.print(".");
#endif
		HAL_Delay(5);
	}

	/* stop rx mode and retrieve data */
	sx_setOpMode(LORA_STANDBY_MODE);
	const int received = sx_readRegister(REG_RX_NB_BYTES);
	const int rxstartaddr = sx_readRegister(REG_RX_NB_BYTES);
	sx_readFifo(rxstartaddr, data, min(received, max_length));

	/* clear the rxDone flag */
	sx_writeRegister(REG_IRQ_FLAGS, IRQ_RX_DONE);

#if (SX1272_debug_mode > 0)
	Serial.print("done, ");
	Serial.println(received, DEC);
#endif
	return received;
}

bool sx1272_receiveStart(void)
{
#if (SX1272_debug_mode > 0)
	Serial.println(F("## SX1272 receive start"));
#endif

	sx_setOpMode(LORA_STANDBY_MODE);

	/* prepare receiving */
	sx_writeRegister(REG_FIFO_RX_BASE_ADDR, 0x00);
	sx_writeRegister(REG_PAYLOAD_LENGTH_LORA, 0xFF);		//is this evaluated in explicit mode?

	/* clear any rx done flag */
	sx_writeRegister(REG_IRQ_FLAGS, IRQ_RX_DONE);

	/* switch irq behaviour to rx_done and enter rx cont mode */
	sx_setDio0Irq(DIO0_RX_DONE);
	return sx_setOpMode(LORA_RXCONT_MODE);
}

int sx1272_getFrame(uint8_t *data, int max_length)
{
#if (SX1272_debug_mode > 0)
	Serial.println(F("## SX1272 get frame"));
#endif

	const int received = sx_readRegister(REG_RX_NB_BYTES);
	const int rxstartaddr = sx_readRegister(REG_FIFO_RX_CURRENT_ADDR);
	sx_readFifo(rxstartaddr, data, min(received, max_length));

	return min(received, max_length);
}
