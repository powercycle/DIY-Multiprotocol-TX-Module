/*
 This project is free software: you can redistribute it and/or modify
 it under the terms of the GNU General Public License as published by
 the Free Software Foundation, either version 3 of the License, or
 (at your option) any later version.

 Multiprotocol is distributed in the hope that it will be useful,
 but WITHOUT ANY WARRANTY; without even the implied warranty of
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 GNU General Public License for more details.

 You should have received a copy of the GNU General Public License
 along with Multiprotocol.  If not, see <http://www.gnu.org/licenses/>.
 */
// Last sync with deviation main github branch

#if defined(SLT_NRF24L01_INO)

#include "iface_nrf24l01.h"

// For code readability
#define SLT_PAYLOADSIZE 7
#define SLT_VISTA_PAYLOADSIZE 11
#define SLT_NFREQCHANNELS 15
#define SLT_TXID_SIZE 4

enum {
    // flags going to packet[6]
    SLT_VISTA_FLAG_FLIP		= 0x04,
    SLT_VISTA_FLAG_MODE		= 0x20
};

enum {
	SLT_BIND=0,
	SLT_BUILD,
	SLT_DATA,
};

static void __attribute__((unused)) SLT_init()
{
	NRF24L01_Initialize();
	NRF24L01_WriteReg(NRF24L01_00_CONFIG, _BV(NRF24L01_00_EN_CRC) | _BV(NRF24L01_00_CRCO)); // 2-bytes CRC, radio off
	NRF24L01_WriteReg(NRF24L01_01_EN_AA, 0x00);			// No Auto Acknoledgement
	NRF24L01_WriteReg(NRF24L01_02_EN_RXADDR, 0x01);		// Enable data pipe 0
	NRF24L01_WriteReg(NRF24L01_03_SETUP_AW, 0x02);		// 4-byte RX/TX address
	NRF24L01_WriteReg(NRF24L01_04_SETUP_RETR, 0x00);	// Disable auto retransmit
	NRF24L01_WriteReg(NRF24L01_07_STATUS, 0x70);		// Clear data ready, data sent, and retransmit
	NRF24L01_WriteReg(NRF24L01_11_RX_PW_P0, 4);			// bytes of data payload for pipe 1
	NRF24L01_SetBitrate(NRF24L01_BR_250K);          	// 256kbps
	NRF24L01_SetPower();
	NRF24L01_WriteRegisterMulti(NRF24L01_0A_RX_ADDR_P0, (uint8_t*)"\xC3\xC3\xAA\x55", 4);
	NRF24L01_FlushRx();
	NRF24L01_WriteRegisterMulti(NRF24L01_10_TX_ADDR, rx_tx_addr, SLT_TXID_SIZE);
	NRF24L01_FlushTx();
	// Turn radio power on
	NRF24L01_SetTxRxMode(TX_EN);
}

static void __attribute__((unused)) SLT_set_freq(void)
{
	// Frequency hopping sequence generation
	for (uint8_t i = 0; i < SLT_TXID_SIZE; ++i)
	{
		uint8_t next_i = (i+1) % SLT_TXID_SIZE; // is & 3 better than % 4 ?
		uint8_t base = i < 2 ? 0x03 : 0x10;
		hopping_frequency[i*4 + 0]  = (rx_tx_addr[i] & 0x3f) + base;
		hopping_frequency[i*4 + 1]  = (rx_tx_addr[i] >> 2) + base;
		hopping_frequency[i*4 + 2]  = (rx_tx_addr[i] >> 4) + (rx_tx_addr[next_i] & 0x03)*0x10 + base;
		if (i*4 + 3 < SLT_NFREQCHANNELS) // guard for 16 channel
			hopping_frequency[i*4 + 3]  = (rx_tx_addr[i] >> 6) + (rx_tx_addr[next_i] & 0x0f)*0x04 + base;
	}

	// unique
	for (uint8_t i = 0; i < SLT_NFREQCHANNELS; ++i)
	{
		uint8_t done = 0;
		while (!done)
		{
			done = 1;
			for (uint8_t j = 0; j < i; ++j)
				if (hopping_frequency[i] == hopping_frequency[j])
				{
					done = 0;
					hopping_frequency[i] += 7;
					if (hopping_frequency[i] >= 0x50)
						hopping_frequency[i] = hopping_frequency[i] - 0x50 + 0x03;
				}
		}
	}
}

static void __attribute__((unused)) SLT_wait_radio()
{
	if (packet_sent)
		while (!(NRF24L01_ReadReg(NRF24L01_07_STATUS) & _BV(NRF24L01_07_TX_DS)));
	packet_sent = 0;
}

static void __attribute__((unused)) SLT_send_data(uint8_t *data, uint8_t len)
{
	SLT_wait_radio();
	NRF24L01_FlushTx();
	NRF24L01_WriteReg(NRF24L01_07_STATUS, _BV(NRF24L01_07_TX_DS) | _BV(NRF24L01_07_RX_DR) | _BV(NRF24L01_07_MAX_RT));
	NRF24L01_WritePayload(data, len);
	//NRF24L01_PulseCE();
	packet_sent = 1;
}

static void __attribute__((unused)) SLT_build_packet()
{
	// aileron, elevator, throttle, rudder, gear, pitch
	uint8_t e = 0; // byte where extension 2 bits for every 10-bit channel are packed
	for (uint8_t i = 0; i < 4; ++i)
	{
		uint16_t v = convert_channel_10b(CH_AETR[i]);
		if(sub_protocol==VISTA && CH_AETR[i]==THROTTLE)
			v = (uint16_t) map(limit_channel_100(THROTTLE),servo_min_100,servo_max_100,850,150);	// Throttel is between 850=0% and 150=100%
		packet[i] = v;
		e = (e >> 2) | (uint8_t) ((v >> 2) & 0xC0);
	}
	// Extra bits for AETR
	packet[4] = e;
	if(sub_protocol==SLT)
	{
		// 8-bit channels
		packet[5] = convert_channel_8b(AUX1);
		packet[6] = convert_channel_8b(AUX2);
	}
	else
	{ // VISTA
		packet[5] = 0xA6;
		packet[6] = 0x00;
		if(Servo_AUX1)
			packet[6] = SLT_VISTA_FLAG_FLIP;
		if(Servo_AUX2)
			packet[6] |= SLT_VISTA_FLAG_MODE;
		packet[7] = 0x00;
		packet[8] = 0x7F;
		packet[9] = 0xAA;
		packet[10] = 0x00;
	}
}

static void __attribute__((unused)) SLT_send_bind_packet()
{
	SLT_wait_radio();
	BIND_IN_PROGRESS;				//Limit TX power to bind level
	NRF24L01_SetPower();
	BIND_DONE;
	NRF24L01_WriteRegisterMulti(NRF24L01_10_TX_ADDR, (uint8_t *)"\x7E\xB8\x63\xA9", SLT_TXID_SIZE);

	NRF24L01_WriteReg(NRF24L01_05_RF_CH, 0x50);
	SLT_send_data(rx_tx_addr, SLT_TXID_SIZE);

	SLT_wait_radio();				//Wait until the packet's sent before changing TX address!

	NRF24L01_SetPower();			//Change power back to normal level
	NRF24L01_WriteRegisterMulti(NRF24L01_10_TX_ADDR, rx_tx_addr, SLT_TXID_SIZE);
}

uint16_t SLT_callback()
{
	switch (phase)
	{
		case SLT_BIND:
			bind_phase = 0;
			SLT_send_bind_packet();
			if(sub_protocol == VISTA)
			{
				phase=SLT_DATA;
				return 1000;
			}
			phase++;					// SLT_BUILD
			return 18000;
		case SLT_BUILD:
			if(sub_protocol==SLT)
			{ // Set radio channel - once per packet batch
				NRF24L01_WriteReg(NRF24L01_05_RF_CH, hopping_frequency[hopping_frequency_no]);
				if (++hopping_frequency_no >= SLT_NFREQCHANNELS)
					hopping_frequency_no = 0;
			}
			SLT_build_packet();
			packet_count=0;
			phase++;					// SLT_DATA
			return 1000;
		case SLT_DATA:
			if(sub_protocol==VISTA)
			{	// Change radio channel every 9 packets
				NRF24L01_WriteReg(NRF24L01_05_RF_CH, hopping_frequency[hopping_frequency_no]);
				rf_ch_num++;
				if(rf_ch_num>=9)
				{
					if (++hopping_frequency_no >= SLT_NFREQCHANNELS)
						hopping_frequency_no = 0;
					rf_ch_num=0;
				}
			}
			SLT_send_data(packet, sub_protocol == VISTA?SLT_VISTA_PAYLOADSIZE:SLT_PAYLOADSIZE);
			packet_count++;
			if(packet_count >= (sub_protocol == VISTA?5:3))
			{ // repeat the same data packet 3(SLT) or 5(VISTA) times
				bind_phase++;
				NRF24L01_SetPower();	// Set tx_power
				phase = SLT_BUILD;		// Refresh data
				if(sub_protocol == VISTA)
					return 1000;
				else
					if(bind_phase>=100)
					{ //SLT sends bind packet every 2.2s
						phase=SLT_BIND;
						return 1000;
					}
				return 19000;
			}
			if(sub_protocol == VISTA)
			{ //VISTA sends bind packet every 1.5s
				if(bind_phase>=150)
					phase=SLT_BIND;
				else
					return 2000;
			}
			return 1000;
	}
	return 1000;
}

uint16_t initSLT()
{
	packet_count = 0;
	packet_sent = 0;
	rf_ch_num=0;
	hopping_frequency_no = 0;
	if(sub_protocol == VISTA)
	{
		memcpy(rx_tx_addr,"\x00\x00\x23\x00",SLT_TXID_SIZE);
		memcpy(hopping_frequency,"\x03\x0A\x11\x18\x1F\x26\x13\x0F\x0B\x10\x08\x16\x1D\x24\x06",15);
	}
	else
		SLT_set_freq();
	SLT_init();
	SLT_build_packet();
	phase = SLT_BIND;
	return 50000;
}

#endif
