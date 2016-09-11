/* Host2Remote is for ATMega328 bus manager on RPUftdi board
 This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

For a copy of the GNU General Public License use
http://www.gnu.org/licenses/gpl-2.0.html
 */ 

#include <util/delay.h>
#include <avr/io.h>
#include "../lib/timers.h"
#include "../lib/twi.h"
#include "../lib/uart.h"
#include "../lib/pin_num.h"
#include "../lib/pins_board.h"

//slave address, use numbers between 0x08 to 0x78
#define TWI_ADDRESS 0x29
// 0x30 is ascii char for the letter zero, this is the local address on RPU_BUS
#define RPU_ADDRESS '0'  
// Byte to send on DTR pair when FTDI_nDTR is active, it is the default address to reset.
#define RPU_HOST_CONNECT '0' 
// byte sent on DTR pair when FTDI_nDTR is no longer active
#define RPU_HOST_DISCONNECT ~RPU_HOST_CONNECT
// return to normal mode, remove lockout. Byte sent on DTR pair
#define RPU_NORMAL_MODE 0x00
// disconnect and set error status on DTR pair
#define RPU_ERROR_MODE 0xFF

static uint8_t rxBuffer[TWI_BUFFER_LENGTH];
static uint8_t rxBufferLength = 0;
static uint8_t txBuffer[TWI_BUFFER_LENGTH];
static uint8_t txBufferLength = 0;

#define BOOTLOADER_ACTIVE 2000
#define BLINK_BOOTLD_DELAY 75
#define BLINK_ACTIVE_DELAY 500
#define BLINK_LOCKOUT_DELAY 2000
#define BLINK_ERROR_DELAY 200
#define UART_TTL 500
#define LOCKOUT_DELAY 30000

static unsigned long blink_started_at;
static unsigned long lockout_started_at;
static unsigned long uart_started_at;
static unsigned long bootloader_started_at;

static uint8_t bootloader_started;
static uint8_t host_active;
static uint8_t localhost_active;
static uint8_t bootloader_address; 
static uint8_t lockout_active;
static uint8_t uart_has_TTL;
static uint8_t host_is_foreign;

volatile uint8_t error_status;
volatile uint8_t uart_output;

// called when I2C data is received. 
void receiveEvent(uint8_t* inBytes, int numBytes) 
{
    // I have to save the data
    for(uint8_t i = 0; i < numBytes; ++i)
    {
        rxBuffer[i] = inBytes[i];    
    }
    rxBufferLength = numBytes;
    
    // This will copy the data to the txBuffer, which will then echo back with slaveTransmit()
    for(uint8_t i = 0; i < numBytes; ++i)
    {
        txBuffer[i] = inBytes[i];    
    }
    txBufferLength = numBytes;
    if (txBufferLength > 1)
    {
        if ( (txBuffer[0] == 0) ) // TWI command to read RPU_ADDRESS
        {
            txBuffer[1] = RPU_ADDRESS; // '0' is 0x30
        }
        if ( (txBuffer[0] == 2) ) // read byte sent on DTR pair when FTDI_nDTR toggles
        {
            txBuffer[1] = bootloader_address;
        }
        if ( (txBuffer[0] == 3) ) // buffer the byte that is sent on DTR pair when FTDI_nDTR toggles
        {
            bootloader_address = txBuffer[1];
        }
        if ( (txBuffer[0] == 5) ) // send RPU_NORMAL_MODE on DTR pair
        {
            if ( !(uart_has_TTL) )
            {
                // send a byte to the UART output
                uart_started_at = millis();
                uart_output = RPU_NORMAL_MODE;
                printf("%c", uart_output); 
                uart_has_TTL = 0; 
            }
        }
        if ( (txBuffer[0] == 6) ) // TWI command to read error status
        {
            txBuffer[1] = error_status;
        }
        if ( (txBuffer[0] == 7) ) // TWI command to set/clear error status
        {
            error_status = txBuffer[1];
        }
    }
}

// called when I2C data is requested.
void slaveTransmit(void) 
{
    // respond with an echo of the last message sent
    uint8_t return_code = twi_transmit(txBuffer, txBufferLength);
    if (return_code != 0)
        error_status &= (1<<1); // bit one set 
}


void setup(void) 
{
    pinMode(LED_BUILTIN, OUTPUT);
    digitalWrite(LED_BUILTIN, HIGH);
    pinMode(FTDI_nRTS, INPUT); // FTDI's UART bridg should pull this down when host is active
    pinMode(FTDI_nCTS, OUTPUT);
    digitalWrite(FTDI_nCTS, LOW);
    pinMode(FTDI_nDTR, INPUT); // FTDI's UART bridg should pull this down when host is active (I've seen it stuck low, and need a power cycle)
    pinMode(FTDI_nDSR, OUTPUT);
    digitalWrite(FTDI_nDSR, LOW);
    pinMode(RX_DE, OUTPUT);
    digitalWrite(RX_DE, HIGH);  // allow RX pair driver to enable if FTDI_TX is low
    pinMode(RX_nRE, OUTPUT);
    digitalWrite(RX_nRE, LOW);  // enable RX pair recevior to output to local MCU's RX input
    pinMode(TX_DE, OUTPUT);
    digitalWrite(TX_DE, HIGH); // allow TX pair driver to enable if TX (from MCU) is low
    pinMode(TX_nRE, OUTPUT);
    digitalWrite(TX_nRE, LOW);  // enable TX pair recevior to output to FTDI_RX input
    pinMode(DTR_DE, OUTPUT);
    digitalWrite(DTR_DE, LOW);  // seems to be a startup glitch ??? so disallow DTR pair driver to enable if DTR_TXD is low
    pinMode(nSS, OUTPUT); // nSS is input to a Open collector buffer used to pull to MCU nRESET low
    digitalWrite(nSS, HIGH); 

    bootloader_address = RPU_HOST_CONNECT; 
    host_active = 0;
    lockout_active = 0;
    error_status = 0;

    //Timer0 Fast PWM mode, Timer1 & Timer2 Phase Correct PWM mode.
    initTimers(); 

    /* Initialize UART, it returns a pointer to FILE so redirect of stdin and stdout works*/
    stdout = stdin = uartstream0_init(BAUD);

    twi_setAddress(TWI_ADDRESS);
    twi_attachSlaveTxEvent(slaveTransmit); // called when I2C data is requested 
    twi_attachSlaveRxEvent(receiveEvent); // slave receive
    twi_init(false); // do not use internal pull-up

    sei(); // Enable global interrupts to start TIMER0 and UART
    
    _delay_ms(50); // wait for UART glitch to clear
    digitalWrite(DTR_DE, HIGH);  // allow DTR pair driver to enable if DTR_TXD is low
    
    // is foreign host in control? (ask over the DTR pair)
    
    // at power up send a byte on the DTR pair to lock the bus (but not if a foreign host is in control)
    uart_started_at = millis();
    uart_output= RPU_HOST_DISCONNECT;
    printf("%c", uart_output); 
    uart_has_TTL = 0; // act like a foreign host (this is going to cause problems)
    digitalWrite(LED_BUILTIN, HIGH);
}

// blink if the host is active, fast blink if error status, slow blink in lockout
void blink_on_activate(void)
{
    unsigned long kRuntime = millis() - blink_started_at;
    
    if (!error_status) 
    {
        if ( bootloader_started  && (kRuntime > BLINK_BOOTLD_DELAY) )
        {
            digitalToggle(LED_BUILTIN);
            
            // next toggle 
            blink_started_at += BLINK_BOOTLD_DELAY; 
        }
        else if ( lockout_active  && (kRuntime > BLINK_LOCKOUT_DELAY) )
        {
            digitalToggle(LED_BUILTIN);
            
            // next toggle 
            blink_started_at += BLINK_LOCKOUT_DELAY; 
        }
        else if ( host_active  && (kRuntime > BLINK_ACTIVE_DELAY) )
        {
            digitalToggle(LED_BUILTIN);
            
            // next toggle 
            blink_started_at += BLINK_ACTIVE_DELAY; 
        }
        // else spin the loop
    }
    else
    {
        if ( (kRuntime > BLINK_ERROR_DELAY))
        {
            digitalToggle(LED_BUILTIN);
            
            // next toggle 
            blink_started_at += BLINK_ERROR_DELAY; 
        }
    }
}

void check_Bootload_Time(void)
{
    if (bootloader_started)
    {
        unsigned long kRuntime = millis() - bootloader_started_at;
        
        if ( kRuntime > BOOTLOADER_ACTIVE)
        {
            host_active =1;
            bootloader_started = 0;
        }
    }
}

void check_DTR(void)
{
    if (!host_is_foreign)
    {
        if ( !digitalRead(FTDI_nDTR) )  // both FTDI_nDTR and FTDI_nRTS are set (active low) when avrdude tries to use the bootloader
        { 
            if ( !(bootloader_started  || lockout_active || host_active || uart_has_TTL) )
            {
                // send a byte on the DTR pair when FTDI_nDTR is first active
                uart_started_at = millis();
                uart_output= bootloader_address; // set by I2C, default is RPU_HOST_CONNECT
                printf("%c", uart_output); 
                uart_has_TTL = 1;
                localhost_active = 1;
            }
        }
        else
        {
            if ( host_active && localhost_active && (!uart_has_TTL) && (!bootloader_started) && (!lockout_active) )
            {
                // send a byte on the DTR pair when FTDI_nDTR is first non-active
                uart_started_at = millis();
                uart_output= RPU_HOST_DISCONNECT;
                printf("%c", uart_output); 
                uart_has_TTL = 1;
                digitalWrite(LED_BUILTIN, HIGH);
                localhost_active = 0;
            }
        }
    }
}

// lockout needs to happoen for a long enough time to insure bootloading is finished,
void check_lockout(void)
{
    unsigned long kRuntime = millis() - lockout_started_at;
    
    if ( lockout_active && (kRuntime > LOCKOUT_DELAY))
    {
        // when host that caused lockout is local then brodcast the end of lockout
        // a remote mcu can also brodcast when its application starts up and does so 
        // through I2C.
        if(!host_is_foreign)
        {
            // send a byte to the UART output
            uart_started_at = millis();
            uart_output = RPU_NORMAL_MODE;
            printf("%c", uart_output); 
            uart_has_TTL = 1;
        }
        
        // unobtrusively remove lock when control is foreign (I don't think this should run on Host2Remote)
        // connect the local mcu
        else
        {
            digitalWrite(RX_DE, LOW); // disallow RX pair driver to enable if FTDI_TX is low
            digitalWrite(RX_nRE, LOW);  // enable RX pair recevior to output to local MCU's RX input
            digitalWrite(TX_DE, HIGH); // allow TX pair driver to enable if TX (from MCU) is low
            digitalWrite(TX_nRE, HIGH);  // disable TX pair recevior to output to FTDI_RX input
        }

        host_active = 1;
        lockout_active =0;
    }
}

void check_uart(void)
{
    unsigned long kRuntime = millis() - uart_started_at;
 
   if ( uart0_available() && !(uart_has_TTL && (kRuntime > UART_TTL) ) )
    {
        uint8_t input;
        input = (uint8_t)(getchar());

        // was this byte sent with the local DTR pair driver, if so the error status may need update
        // and the lockout from a local host needs to be treated differently
        // need to ignore the local host's DTR if getting control from a remote host
        if ( uart_has_TTL )
        {
            if(input != uart_output) 
            { // sent byte does not match,  but I'm not to sure what would cause this.
                error_status &= (1<<2); // bit two set 
            }
            uart_has_TTL = 0;
            host_is_foreign = 0;
        }
        else
        {
            host_is_foreign = 1;
        }

        if (input == RPU_NORMAL_MODE) // end the lockout if it was set.
        {
            lockout_active =0;
            bootloader_started = 0;
            host_active =1;
            
            // connect the local host and local mcu (this should not run on the Host2Remote board)
            if (host_is_foreign)
            {
                digitalWrite(RX_DE, HIGH); // allow RX pair driver to enable if FTDI_TX is low
                digitalWrite(RX_nRE, LOW);  // enable RX pair recevior to output to local MCU's RX input
                digitalWrite(TX_DE, HIGH); // allow TX pair driver to enable if TX (from MCU) is low
                digitalWrite(TX_nRE, LOW);  // enable TX pair recevior to output to FTDI_RX input
            }
            
            // connect the local host and local mcu
            else
            {
                digitalWrite(RX_DE, HIGH); // allow RX pair driver to enable if FTDI_TX is low
                digitalWrite(RX_nRE, LOW);  // enable RX pair recevior to output to local MCU's RX input
                digitalWrite(TX_DE, HIGH); // allow TX pair driver to enable if TX (from MCU) is low
                digitalWrite(TX_nRE, LOW);  // enable TX pair recevior to output to FTDI_RX input
            }
        }
        else if (input == RPU_ADDRESS) // that is my local address
        {
            // connect the remote host and local mcu
            if (host_is_foreign)
            {
                digitalWrite(RX_DE, LOW); // disallow RX pair driver to enable if FTDI_TX is low
                digitalWrite(RX_nRE, LOW);  // enable RX pair recevior to output to local MCU's RX input
                digitalWrite(TX_DE, HIGH); // allow TX pair driver to enable if TX (from MCU) is low
                digitalWrite(TX_nRE, HIGH);  // disable TX pair recevior to output to FTDI_RX input
            }
            
            // connect the local host and local mcu
            else
            {
                digitalWrite(RX_DE, HIGH); // allow RX pair driver to enable if FTDI_TX is low
                digitalWrite(RX_nRE, LOW);  // enable RX pair recevior to output to local MCU's RX input
                digitalWrite(TX_DE, HIGH); // allow TX pair driver to enable if TX (from MCU) is low
                digitalWrite(TX_nRE, LOW);  // enable TX pair recevior to output to FTDI_RX input
            }

            // start the bootloader
            bootloader_started = 1;
            digitalWrite(nSS, LOW);   // nSS goes through a open collector buffer to nRESET
            _delay_ms(20);  // hold reset low for a short time 
            digitalWrite(nSS, HIGH); // this will release the buffer with open colllector on MCU nRESET.
            blink_started_at = millis();
            bootloader_started_at = millis();
        }
        else if (input <= 0x7F) // values > 0x80 are for a host disconnect e.g. the bitwise negation of an RPU_ADDRESS
        {  
            lockout_active =1;
            bootloader_started = 0;
            host_active =0;

            // lockout everything
            if (host_is_foreign)
            {
                digitalWrite(RX_DE, LOW); // disallow RX pair driver to enable if FTDI_TX is low
                digitalWrite(RX_nRE, HIGH);  // disable RX pair recevior to output to local MCU's RX input
                digitalWrite(TX_DE, LOW); // disallow TX pair driver to enable if TX (from MCU) is low
                digitalWrite(TX_nRE, HIGH);  // disable TX pair recevior to output to FTDI_RX input
            }
            
            // lockout MCU, but not host
            else
            {
                digitalWrite(RX_DE, HIGH); // allow RX pair driver to enable if FTDI_TX is low
                digitalWrite(RX_nRE, HIGH);  // disable RX pair recevior to output to local MCU's RX input
                digitalWrite(TX_DE, LOW); // disallow TX pair driver to enable if TX (from MCU) is low
                digitalWrite(TX_nRE, LOW);  // enable TX pair recevior to output to FTDI_RX input
            }
            
            lockout_started_at = millis();
            blink_started_at = millis();
        }
        else if (input > 0x7F) // RPU_HOST_DISCONNECT is the bitwise negation of an RPU_ADDRESS it will be > 0x80 (seen as a uint8_t)
        { 
            // once host is foreign it is going to be a pain to switch to another host
            // host_is_foreign = 0;

            lockout_active =0;
            host_active =0;
            bootloader_started = 0;
            digitalWrite(LED_BUILTIN, HIGH);
            digitalWrite(RX_DE, LOW); // disallow RX pair driver to enable if FTDI_TX is low
            digitalWrite(RX_nRE, HIGH);  // disable RX pair recevior to output to local MCU's RX input
            digitalWrite(TX_DE, LOW); // disallow TX pair driver to enable if TX (from MCU) is low
            digitalWrite(TX_nRE, HIGH);  // disable TX pair recevior that outputs to FTDI_RX input
        }
    }
    else if (uart_has_TTL && (kRuntime > UART_TTL) )
    { // perhaps the DTR line is stuck (e.g. someone has pulled it low) so we time out
        error_status &= (1<<0); // bit zero set 
        uart_has_TTL = 0;
    }
}

int main(void)
{
    setup();

    blink_started_at = millis();

    while (1) 
    {
        blink_on_activate();
        check_Bootload_Time();
        check_DTR();
        check_lockout();
        check_uart();
    }    
}

