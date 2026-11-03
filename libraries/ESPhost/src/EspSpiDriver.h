/* ########################################################################## */
/* - File: esp_host_spi_driver.h
   - Copyright (c): 2022 Arduino srl.
   - Author: Daniele Aimo (d.aimo@arduino.cc)

     This library is free software; you can redistribute it and/or
     modify it under the terms of the GNU Lesser General Public
     License as published by the Free Software Foundation; either
     version 2.1 of the License, or (at your option) any later version.

     This library is distributed in the hope that it will be useful,
     but WITHOUT ANY WARRANTY; without even the implied warranty of
     MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
     Lesser General Public License for more details.

     You should have received a copy of the GNU Lesser General Public
     License along with this library; if not, write to the Free Software
     Foundation, Inc.,51 Franklin St, Fifth Floor, Boston, MA  02110-1301 USA */
/* ########################################################################## */

#ifndef ESP_HOSTED_SPI_DRIVER_H
#define ESP_HOSTED_SPI_DRIVER_H

//#define ESP_HOST_DEBUG_ENABLED

#include "Arduino.h"
#include "IRQManager.h"
#include "CEspCommunication.h"
//#include "esp_host_protocol.h"

#include "r_spi.h"
#include "r_sci_spi.h"

using CbkFuncRx_f = void(*)();

#define ESP_HOSTED_SPI_DRIVER_OK   0
#define ESP_HOSTED_SPI_NOTHING_TO_TX_OR_RX    3
#define ESP_HOSTED_SPI_MESSAGE_RECEIVED       8


#define ESP_HOSTED_SPI_DRIVER_INIT_IRQ_FAILED 1
#define ESP_HOSTED_SPI_DRIVER_SPI_FAIL_OPEN   2
#define ESP_HOSTED_SPI_SPI_TRANSACTION_ERR    4
#define ESP_HOSTED_SPI_ESP_NOT_READY          5
#define ESP_HOSTED_SPI_TX_RX_ABORTED          6
#define ESP_HOSTED_SPI_TIMEOUT                7
#define ESP_HOSTED_MSG_PREPARATION_ERROR      9
#define ESP_HOSTED_SPI_BUS_BUSY               10  /* SPI1 held by another device */

/* Use the shared ArduinoSPI SPI1 object instead of a private FSP instance on
   the same hardware channel. Required when SPI1 is shared (UWB Shield). */
#ifndef ESPHOST_USE_ARDUINO_SPI
#define ESPHOST_USE_ARDUINO_SPI 1
#endif

#ifndef ESP_HANDSHAKE_TIMEOUT_MS
#define ESP_HANDSHAKE_TIMEOUT_MS 500
#endif

/* SPI1 arbitration hooks (weak, no-op by default).
   - esphost_spi_bus_acquire(): called right before ESP_CS goes LOW. Return
     false if the bus cannot be taken; the transaction is then skipped and
     esp_host_spi_transaction() reports ESP_HOSTED_SPI_BUS_BUSY.
     When it returns true, nothing may block until esphost_spi_bus_release().
   - esphost_spi_handshake_wait(): called between HANDSHAKE polls.
   - esphost_wait_1ms(): called between control-response polls. */
#ifdef __cplusplus
extern "C" {
#endif
bool esphost_spi_bus_acquire(void);
void esphost_spi_bus_release(void);
void esphost_spi_handshake_wait(void);
/* ~1 ms pause between polls while waiting for a control response
   (CEspControl::wait_for_answer). Default: R_BSP_SoftwareDelay(1 ms). */
void esphost_wait_1ms(void);
#ifdef __cplusplus
}
#endif

/* init the SPI driver */
int esp_host_spi_init(void);
/* it performs the spi communication untill there are message to be tx or rx
   (it stops if an error occurred) 
   it returns the error or 0 if all is ok */
int esp_host_perform_spi_communication(bool wait_for_valid_msg);

bool isEspSpiInitialized() ;
bool arePendingRxMsg();

bool isSpiTransactionInProgress();
void setSpiTransactionInPRogress();




#endif