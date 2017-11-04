/*
 M2RET.ino

 Created: March 4, 2017
 Author: Collin Kidder

Copyright (c) 2014-2017 Collin Kidder, Michael Neuweiler, Charles Galpin

Permission is hereby granted, free of charge, to any person obtaining
a copy of this software and associated documentation files (the
"Software"), to deal in the Software without restriction, including
without limitation the rights to use, copy, modify, merge, publish,
distribute, sublicense, and/or sell copies of the Software, and to
permit persons to whom the Software is furnished to do so, subject to
the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

 */

#include "M2RET.h"
#include "config.h"
#include <due_can.h>
#include <Arduino_Due_SD_HSMCI.h> // This creates the object SD (HSCMI connected sdcard)
#include <due_wire.h>
#include <SPI.h>
#include <lin_stack.h>
#include <sw_can.h>
#include "ELM327_Emulator.h"

#include "EEPROM.h"
#include "SerialConsole.h"
#include <variant.h>

/*
Notes on project:
This code should be autonomous after being set up. That is, you should be able to set it up
then disconnect it and move over to a car or other device to monitor, plug it in, and have everything
use the settings you set up without any external input.
*/

byte i = 0;

typedef struct {
    uint32_t bitsPerQuarter;
    uint32_t bitsSoFar;
    uint8_t busloadPercentage;
} BUSLOAD;

byte serialBuffer[SER_BUFF_SIZE];
int serialBufferLength = 0; //not creating a ring buffer. The buffer should be large enough to never overflow
uint32_t lastFlushMicros = 0;
BUSLOAD busLoad[2];
uint32_t busLoadTimer;

EEPROMSettings settings;
SystemSettings SysSettings;
DigitalCANToggleSettings digToggleSettings;

//file system on sdcard (HSMCI connected)
FileStore FS;

SWcan SWCAN(SWCan_Chip_Select, SWCan_Interrupt);

lin_stack LIN1(1, 0); // Sniffer
lin_stack LIN2(2, 0); // Sniffer

ELM327Emu elmEmulator;

SerialConsole console;
bool m2WirelessInstalled = false;

bool digTogglePinState;
uint8_t digTogglePinCounter;

void CANHandler() {
    SWCAN.intHandler();
}

//initializes all the system EEPROM values. Chances are this should be broken out a bit but
//there is only one checksum check for all of them so it's simple to do it all here.
void loadSettings()
{
    Logger::console("Loading settings....");

    EEPROM.read(EEPROM_ADDR, settings);

    if (settings.version != EEPROM_VER) { //if settings are not the current version then erase them and set defaults
        Logger::console("Resetting to factory defaults");
        settings.version = EEPROM_VER;
        settings.appendFile = false;
        settings.CAN0Speed = 500000;
        settings.CAN0_Enabled = true;
        settings.CAN1Speed = 500000;
        settings.CAN1_Enabled = false;
        settings.CAN0ListenOnly = false;
        settings.CAN1ListenOnly = false;
        settings.SWCAN_Enabled = false;
        settings.SWCANListenOnly = false; //TODO: Not currently respected or implemented.
        settings.SWCANSpeed = 33333;
        settings.LIN_Enabled = false;
        settings.LINSpeed = 19200;
        sprintf((char *)settings.fileNameBase, "CANBUS");
        sprintf((char *)settings.fileNameExt, "TXT");
        settings.fileNum = 1;
        for (int i = 0; i < 3; i++) {
            settings.CAN0Filters[i].enabled = true;
            settings.CAN0Filters[i].extended = true;
            settings.CAN0Filters[i].id = 0;
            settings.CAN0Filters[i].mask = 0;
            settings.CAN1Filters[i].enabled = true;
            settings.CAN1Filters[i].extended = true;
            settings.CAN1Filters[i].id = 0;
            settings.CAN1Filters[i].mask = 0;
        }
        for (int j = 3; j < 8; j++) {
            settings.CAN0Filters[j].enabled = true;
            settings.CAN0Filters[j].extended = false;
            settings.CAN0Filters[j].id = 0;
            settings.CAN0Filters[j].mask = 0;
            settings.CAN1Filters[j].enabled = true;
            settings.CAN1Filters[j].extended = false;
            settings.CAN1Filters[j].id = 0;
            settings.CAN1Filters[j].mask = 0;
        }
        settings.fileOutputType = CRTD;
        settings.useBinarySerialComm = false;
        settings.autoStartLogging = false;
        settings.logLevel = 1; //info
        settings.sysType = 0; //CANDUE as default
        settings.valid = 0; //not used right now
        EEPROM.write(EEPROM_ADDR, settings);
    } else {
        Logger::console("Using stored values from EEPROM");
        if (settings.CAN0ListenOnly > 1) settings.CAN0ListenOnly = 0;
        if (settings.CAN1ListenOnly > 1) settings.CAN1ListenOnly = 0;
    }

    EEPROM.read(EEPROM_ADDR + 1024, digToggleSettings);
    if (digToggleSettings.mode == 255) {
        Logger::console("Resetting digital toggling system to defaults");
        digToggleSettings.enabled = false;
        digToggleSettings.length = 0;
        digToggleSettings.mode = 0;
        digToggleSettings.pin = 1;
        digToggleSettings.rxTxID = 0x700;
        for (int c=0 ; c<8 ; c++) digToggleSettings.payload[c] = 0;
        EEPROM.write(EEPROM_ADDR + 1024, digToggleSettings);
    } else {
        Logger::console("Using stored values for digital toggling system");
    }

    Logger::setLoglevel((Logger::LogLevel)settings.logLevel);

    SysSettings.SDCardInserted = false;

//    switch (settings.sysType) {
//    case 0:  //First gen M2 board
        Logger::console("Running on Macchina M2 hardware");
        SysSettings.useSD = true;
        SysSettings.logToFile = false;
        SysSettings.LED_CANTX = RGB_Green_Led;
        SysSettings.LED_CANRX = RGB_Blue_Led;
        SysSettings.LED_LOGGING = RGB_Red_Led;
        SysSettings.logToggle = false;
        SysSettings.txToggle = true;
        SysSettings.rxToggle = true;
        SysSettings.lawicelAutoPoll = false;
        SysSettings.lawicelMode = false;
        SysSettings.lawicellExtendedMode = false;
        SysSettings.lawicelTimestamping = false;
        for (int rx = 0; rx < NUM_BUSES; rx++) SysSettings.lawicelBusReception[rx] = true; //default to showing messages on RX 
        //set pin mode for all LEDS
        pinMode(RGB_Green_Led, OUTPUT);
        pinMode(RGB_Red_Led, OUTPUT);
        pinMode(RGB_Blue_Led, OUTPUT);
        pinMode(Red_Led, OUTPUT);
        pinMode(Yellow_Led1, OUTPUT);
        pinMode(Yellow_Led2, OUTPUT);
        pinMode(Yellow_Led3, OUTPUT);
        pinMode(Green_Led, OUTPUT);
        
        digitalWrite(SWCan_Mod0_Pin, LOW); //Mode 0 for SWCAN
        digitalWrite(SWCan_Mod1_Pin, LOW); //mode 1
        
        //Set RGB LED to completely off.
        digitalWrite(RGB_Green_Led, HIGH);
        digitalWrite(RGB_Blue_Led, HIGH);
        digitalWrite(RGB_Red_Led, HIGH);
        digitalWrite(Red_Led, HIGH);
        digitalWrite(Yellow_Led1, HIGH);
        digitalWrite(Yellow_Led2, HIGH);
        digitalWrite(Yellow_Led3, HIGH);
        digitalWrite(Green_Led, HIGH);
        
        setSWCANSleep();
//        break;
//    }

//	if (settings.singleWireMode && settings.CAN1_Enabled) setSWCANEnabled();
//	else setSWCANSleep(); //start out setting single wire to sleep.

    busLoad[0].bitsSoFar = 0;
    busLoad[0].busloadPercentage = 0;
    busLoad[0].bitsPerQuarter = settings.CAN0Speed / 4;

    busLoad[1].bitsSoFar = 0;
    busLoad[1].busloadPercentage = 0;
    busLoad[1].bitsPerQuarter = settings.CAN1Speed / 4;

    busLoadTimer = millis();
}

void setSWCANSleep()
{
    SWCAN.mode(0);
}

void setSWCANEnabled()
{
    SWCAN.mode(3);
}

void setSWCANWakeup()
{
    SWCAN.mode(2);
}

void setup()
{
    //TODO: I don't remember why these two lines are here... anyone know?
    //pinMode(XBEE_PWM, OUTPUT);
    //digitalWrite(XBEE_PWM, LOW);

    //delay(5000); //just for testing. Don't use in production

    SerialUSB.begin(115200);
    while(SerialUSB);
    Serial.begin(115200);	// XBEE Serial speed
    while(Serial);

    Wire.begin();
    SPI.begin();

    loadSettings();

    //settings.logLevel = 0; //Also just for testing. Dont use this in production either.
    //Logger::setLoglevel(Logger::Debug);

    if (SysSettings.useSD) {
        if (SD.Init()) {
            FS.Init();
            SysSettings.SDCardInserted = true;
            if (settings.autoStartLogging) {
                SysSettings.logToFile = true;
                Logger::info("Automatically logging to file.");
                //Logger::file("Starting File Logging.");
            }
        } else {
            Logger::error("SDCard not inserted. Cannot log to file!");
            SysSettings.SDCardInserted = false;
        }
    }

    SerialUSB.print("Build number: ");
    SerialUSB.println(CFG_BUILD_NUM);

    sys_early_setup();
    setup_sys_io();

    if (digToggleSettings.enabled) {
        SerialUSB.println("Digital Toggle System Enabled");
        if (digToggleSettings.mode & 1) { //input CAN and output pin state mode
            SerialUSB.println("In Output Mode");
            pinMode(digToggleSettings.pin, OUTPUT);
            if (digToggleSettings.mode & 0x80) {
                digitalWrite(digToggleSettings.pin, LOW);
                digTogglePinState = false;
            } else {
                digitalWrite(digToggleSettings.pin, HIGH);
                digTogglePinState = true;
            }
        } else { //read pin and output CAN mode
            SerialUSB.println("In Input Mode");
            pinMode(digToggleSettings.pin, INPUT);
            digTogglePinCounter = 0;
            if (digToggleSettings.mode & 0x80) digTogglePinState = false;
            else digTogglePinState = true;
        }
    }


    if (settings.CAN0_Enabled) {
        if (settings.CAN0ListenOnly) {
            Can0.enable_autobaud_listen_mode();
        } else {
            Can0.disable_autobaud_listen_mode();
        }
        Can0.enable();
        Can0.begin(settings.CAN0Speed, 255);
        SerialUSB.print("Enabled CAN0 with speed ");
        SerialUSB.println(settings.CAN0Speed);
    } else Can0.disable();

    if (settings.CAN1_Enabled) {
        if (settings.CAN1ListenOnly) {
            Can1.enable_autobaud_listen_mode();
        } else {
            Can1.disable_autobaud_listen_mode();
        }
        Can1.enable();
        Can1.begin(settings.CAN1Speed, 255);
        SerialUSB.print("Enabled CAN1 with speed ");
        SerialUSB.println(settings.CAN1Speed);        
    } else Can1.disable();

    if (settings.SWCAN_Enabled) {
        SWCAN.setupSW(settings.SWCANSpeed);       
        delay(20);
        SWCAN.mode(3); // Go to normal mode. 0 - Sleep, 1 - High Speed, 2 - High Voltage Wake-Up, 3 - Normal
        attachInterrupt(SWCan_Interrupt, CANHandler, FALLING); //enable interrupt for SWCAN
        SerialUSB.print("Enabled SWCAN with speed ");
        SerialUSB.println(settings.SWCANSpeed);
    }

/*
    if (settings.LIN_Enabled) {
        LIN1.setSerial();
        LIN2.setSerial();
    } */
/*
    for (int i = 0; i < 7; i++) {
        if (settings.CAN0Filters[i].enabled) {
            Can0.setRXFilter(i, settings.CAN0Filters[i].id,
                             settings.CAN0Filters[i].mask, settings.CAN0Filters[i].extended);
        }
        if (settings.CAN1Filters[i].enabled) {
            Can1.setRXFilter(i, settings.CAN1Filters[i].id,
                             settings.CAN1Filters[i].mask, settings.CAN1Filters[i].extended);
        }
    }*/

    setPromiscuousMode();

    SysSettings.lawicelMode = false;
    SysSettings.lawicelAutoPoll = false;
    SysSettings.lawicelTimestamping = false;
    SysSettings.lawicelPollCounter = 0;
    
    elmEmulator.setup();
    m2WirelessInstalled = elmEmulator.testHardware();
    //	digitalWrite(XBEE_Program, HIGH);	// Ensure XBEE Module is not in Programming mode
    //	digitalWrite(XBEE_Reset, LOW);	// Reset the XBEE module
    //	delay(500);
    //	digitalWrite(XBEE_Reset, HIGH);	// Return the pin to normal mode
    if(m2WirelessInstalled){
        SerialUSB.print("\n ** M2 WIFI Module Installed **\n\n");
    } else{
        SerialUSB.print("\n ** M2 WIFI Module Not Installed **\n\n");
    }
    // ******************************** //
    //		Debug ELM327 setup			//
    /*
    if(elmEmulator.testHardware()){
    SerialUSB.print(" WIFI Exists\n");
    }else{
    if(elmEmulator.testHardware()){	// try it one more time to be sure
    SerialUSB.print(" WIFI Exists Second Test\n");
    }else{
    SerialUSB.print("WIFI Module Not Installed\n");
    }
    }
    */
    // ********************************	//


    SerialUSB.print("Done with init\n");
}

void setPromiscuousMode()
{
    //By default there are 7 mailboxes for each device that are RX boxes
    //This sets each mailbox to have an open filter that will accept extended
    //or standard frames
    int filter;
    //extended
    for (filter = 0; filter < 3; filter++) {
        Can0.setRXFilter(filter, 0, 0, true);
        Can1.setRXFilter(filter, 0, 0, true);
    }
    //standard
    for (filter = 3; filter < 7; filter++) {
        Can0.setRXFilter(filter, 0, 0, false);
        Can1.setRXFilter(filter, 0, 0, false);
    }
}

//Get the value of XOR'ing all the bytes together. This creates a reasonable checksum that can be used
//to make sure nothing too stupid has happened on the comm.
uint8_t checksumCalc(uint8_t *buffer, int length)
{
    uint8_t valu = 0;
    for (int c = 0; c < length; c++) {
        valu ^= buffer[c];
    }
    return valu;
}

void addBits(int offset, CAN_FRAME &frame)
{
    if (offset < 0) return;
    if (offset > 1) return;
    busLoad[offset].bitsSoFar += 41 + (frame.length * 9);
    if (frame.extended) busLoad[offset].bitsSoFar += 18;
}

void sendFrame(CANRaw &bus, CAN_FRAME &frame)
{
    int whichBus = 0;
    if (&bus == &Can1) whichBus = 1;
    bus.sendFrame(frame);
    sendFrameToFile(frame, whichBus); //copy sent frames to file as well.
    addBits(whichBus, frame);
    toggleTXLED();
}

void sendFrameSW(CAN_FRAME &frame)
{
    SWFRAME swFrame;
    swFrame.id = frame.id;
    swFrame.extended = frame.extended;
    swFrame.length = frame.length;
    swFrame.data.value = frame.data.value;
    SWCAN.EnqueueTX(swFrame);
    toggleTXLED();
    sendFrameToFile(frame, 2);
}

void toggleRXLED()
{
    static int counter = 0;
    counter++;
    if (counter >= BLINK_SLOWNESS) {
        counter = 0;
        SysSettings.rxToggle = !SysSettings.rxToggle;
        setLED(SysSettings.LED_CANRX, SysSettings.rxToggle);
    }
}

void toggleTXLED()
{
    static int counter = 0;
    counter++;
    if (counter >= BLINK_SLOWNESS) {
        counter = 0;
        SysSettings.txToggle = !SysSettings.txToggle;
        setLED(SysSettings.LED_CANTX, SysSettings.txToggle);
    }
}

/*
void serialEvent(){

    // Serial data from XB_ can just be passed straight through
    while(Serial.available()){
        SerialUSB.write((uint8_t) Serial.read());
        SerialUSB.flush();
    }
}
*/

/*
 * Pass bus load in percent 0 - 100
 * The 5 LEDs are Green, Yellow, Yellow, Yellow, Red
 * The values used for lighting up LEDs are very subjective
 * but here is the justification:
 * You want the first LED to light up if there is practically any
 * traffic at all so it comes on over 0% load - This tells the user that some traffic exists
 * The next few are timed to give the user some feedback that the load is increasing
 * and are somewhat logarithmic
 * The last LED comes on at 80% because busload really should never go over 80% for
 * proper functionality so lighing up red past that is the right move.
*/
void updateBusloadLED(uint8_t perc)
{
    Logger::debug("Busload: %i", perc);
    if (perc > 0) digitalWrite(Green_Led, LOW);
    else digitalWrite(Green_Led, HIGH);

    if (perc >= 14) digitalWrite(Yellow_Led3, LOW);
    else digitalWrite(Yellow_Led3, HIGH);

    if (perc >= 30) digitalWrite(Yellow_Led2, LOW);
    else digitalWrite(Yellow_Led2, HIGH);

    if (perc >= 53) digitalWrite(Yellow_Led1, LOW);
    else digitalWrite(Yellow_Led1, HIGH);

    if (perc >= 80) digitalWrite(Red_Led, LOW);
    else digitalWrite(Red_Led, HIGH);
}

void sendFrameToUSB(CAN_FRAME &frame, int whichBus)
{
    uint8_t buff[22];
    uint8_t temp;
    uint32_t now = micros();

    if (SysSettings.lawicelMode) {
        if (SysSettings.lawicellExtendedMode) {
            SerialUSB.print(micros());
            SerialUSB.print(" - ");
            SerialUSB.print(frame.id, HEX);            
            if (frame.extended) SerialUSB.print(" X ");
            else SerialUSB.print(" S ");
            console.printBusName(whichBus);
            for (int d = 0; d < frame.length; d++) {
                SerialUSB.print(" ");
                SerialUSB.print(frame.data.bytes[d], HEX);
            }
        }
        else {
            if (frame.extended) {
                SerialUSB.print("T");
                sprintf((char *)buff, "%08x", frame.id);
                SerialUSB.print((char *)buff);
            } else {
                SerialUSB.print("t");
                sprintf((char *)buff, "%03x", frame.id);
                SerialUSB.print((char *)buff);
            }
            SerialUSB.print(frame.length);
            for (int i = 0; i < frame.length; i++) {
                sprintf((char *)buff, "%02x", frame.data.byte[i]);
                SerialUSB.print((char *)buff);
            }
            if (SysSettings.lawicelTimestamping) {
                uint16_t timestamp = (uint16_t)millis();
                sprintf((char *)buff, "%04x", timestamp);
                SerialUSB.print((char *)buff);
            }
        }
        SerialUSB.write(13);
    } else {
        if (settings.useBinarySerialComm) {
            if (frame.extended) frame.id |= 1 << 31;
            serialBuffer[serialBufferLength++] = 0xF1;
            serialBuffer[serialBufferLength++] = 0; //0 = canbus frame sending
            serialBuffer[serialBufferLength++] = (uint8_t)(now & 0xFF);
            serialBuffer[serialBufferLength++] = (uint8_t)(now >> 8);
            serialBuffer[serialBufferLength++] = (uint8_t)(now >> 16);
            serialBuffer[serialBufferLength++] = (uint8_t)(now >> 24);
            serialBuffer[serialBufferLength++] = (uint8_t)(frame.id & 0xFF);
            serialBuffer[serialBufferLength++] = (uint8_t)(frame.id >> 8);
            serialBuffer[serialBufferLength++] = (uint8_t)(frame.id >> 16);
            serialBuffer[serialBufferLength++] = (uint8_t)(frame.id >> 24);
            serialBuffer[serialBufferLength++] = frame.length + (uint8_t)(whichBus << 4);
            for (int c = 0; c < frame.length; c++) {
                serialBuffer[serialBufferLength++] = frame.data.bytes[c];
            }
            //temp = checksumCalc(buff, 11 + frame.length);
            temp = 0;
            serialBuffer[serialBufferLength++] = temp;
            //SerialUSB.write(buff, 12 + frame.length);
        } else {
            SerialUSB.print(micros());
            SerialUSB.print(" - ");
            SerialUSB.print(frame.id, HEX);
            if (frame.extended) SerialUSB.print(" X ");
            else SerialUSB.print(" S ");
            SerialUSB.print(whichBus);
            SerialUSB.print(" ");
            SerialUSB.print(frame.length);
            for (int c = 0; c < frame.length; c++) {
                SerialUSB.print(" ");
                SerialUSB.print(frame.data.bytes[c], HEX);
            }
            SerialUSB.println();
        }
    }
}

void sendFrameToFile(CAN_FRAME &frame, int whichBus)
{
    uint8_t buff[40];
    uint8_t temp;
    uint32_t timestamp;
    if (settings.fileOutputType == BINARYFILE) {
        if (frame.extended) frame.id |= 1 << 31;
        timestamp = micros();
        buff[0] = (uint8_t)(timestamp & 0xFF);
        buff[1] = (uint8_t)(timestamp >> 8);
        buff[2] = (uint8_t)(timestamp >> 16);
        buff[3] = (uint8_t)(timestamp >> 24);
        buff[4] = (uint8_t)(frame.id & 0xFF);
        buff[5] = (uint8_t)(frame.id >> 8);
        buff[6] = (uint8_t)(frame.id >> 16);
        buff[7] = (uint8_t)(frame.id >> 24);
        buff[8] = frame.length + (uint8_t)(whichBus << 4);
        for (int c = 0; c < frame.length; c++) {
            buff[9 + c] = frame.data.bytes[c];
        }
        Logger::fileRaw(buff, 9 + frame.length);
    } else if (settings.fileOutputType == GVRET) {
        sprintf((char *)buff, "%i,%x,%i,%i,%i", millis(), frame.id, frame.extended, whichBus, frame.length);
        Logger::fileRaw(buff, strlen((char *)buff));

        for (int c = 0; c < frame.length; c++) {
            sprintf((char *) buff, ",%x", frame.data.bytes[c]);
            Logger::fileRaw(buff, strlen((char *)buff));
        }
        buff[0] = '\r';
        buff[1] = '\n';
        Logger::fileRaw(buff, 2);
    } else if (settings.fileOutputType == CRTD) {
        int idBits = 11;
        if (frame.extended) idBits = 29;
        sprintf((char *)buff, "%f R%i %x", millis() / 1000.0f, idBits, frame.id);
        Logger::fileRaw(buff, strlen((char *)buff));

        for (int c = 0; c < frame.length; c++) {
            sprintf((char *) buff, " %x", frame.data.bytes[c]);
            Logger::fileRaw(buff, strlen((char *)buff));
        }
        buff[0] = '\r';
        buff[1] = '\n';
        Logger::fileRaw(buff, 2);
    }
}

void processDigToggleFrame(CAN_FRAME &frame)
{
    bool gotFrame = false;
    if (digToggleSettings.rxTxID == frame.id) {
        if (digToggleSettings.length == 0) gotFrame = true;
        else {
            gotFrame = true;
            for (int c = 0; c < digToggleSettings.length; c++) {
                if (digToggleSettings.payload[c] != frame.data.byte[c]) {
                    gotFrame = false;
                    break;
                }
            }
        }
    }

    if (gotFrame) { //then toggle output pin
        Logger::console("Got special digital toggle frame. Toggling the output!");
        digitalWrite(digToggleSettings.pin, digTogglePinState?LOW:HIGH);
        digTogglePinState = !digTogglePinState;
    }
}

void sendDigToggleMsg()
{
    CAN_FRAME frame;
    SerialUSB.println("Got digital input trigger.");
    frame.id = digToggleSettings.rxTxID;
    if (frame.id > 0x7FF) frame.extended = true;
    else frame.extended = false;
    frame.length = digToggleSettings.length;
    for (int c = 0; c < frame.length; c++) frame.data.byte[c] = digToggleSettings.payload[c];
    if (digToggleSettings.mode & 2) {
        SerialUSB.println("Sending digital toggle message on CAN0");
        sendFrame(Can0, frame);
    }
    if (digToggleSettings.mode & 4) {
        SerialUSB.println("Sending digital toggle message on CAN1");
        sendFrame(Can1, frame);
    }
}

/*
Loop executes as often as possible all the while interrupts fire in the background.
The serial comm protocol is as follows:
All commands start with 0xF1 this helps to synchronize if there were comm issues
Then the next byte specifies which command this is.
Then the command data bytes which are specific to the command
Lastly, there is a checksum byte just to be sure there are no missed or duped bytes
Any bytes between checksum and 0xF1 are thrown away

Yes, this should probably have been done more neatly but this way is likely to be the
fastest and safest with limited function calls
*/
void loop()
{
    static int loops = 0;
    CAN_FRAME incoming;
    SWFRAME swIncoming;
    static CAN_FRAME build_out_frame;
    static int out_bus;
    int in_byte;
    static byte buff[20];
    static int step = 0;
    static STATE state = IDLE;
    static uint32_t build_int;
    uint8_t temp8;
    uint16_t temp16;
    uint32_t temp32;
    static bool markToggle = false;
    bool isConnected = false;
    int serialCnt;
    uint32_t now = micros();

    if (millis() > (busLoadTimer + 250)) {
        busLoadTimer = millis();
        busLoad[0].busloadPercentage = ((busLoad[0].busloadPercentage * 3) + (((busLoad[0].bitsSoFar * 1000) / busLoad[0].bitsPerQuarter) / 10)) / 4;
        busLoad[1].busloadPercentage = ((busLoad[1].busloadPercentage * 3) + (((busLoad[1].bitsSoFar * 1000) / busLoad[1].bitsPerQuarter) / 10)) / 4;
        //Force busload percentage to be at least 1% if any traffic exists at all. This forces the LED to light up for any traffic.
        if (busLoad[0].busloadPercentage == 0 && busLoad[0].bitsSoFar > 0) busLoad[0].busloadPercentage = 1;
        if (busLoad[1].busloadPercentage == 0 && busLoad[1].bitsSoFar > 0) busLoad[1].busloadPercentage = 1;
        busLoad[0].bitsPerQuarter = settings.CAN0Speed / 4;
        busLoad[1].bitsPerQuarter = settings.CAN1Speed / 4;
        busLoad[0].bitsSoFar = 0;
        busLoad[1].bitsSoFar = 0;
        if (busLoad[0].busloadPercentage > busLoad[1].busloadPercentage) updateBusloadLED(busLoad[0].busloadPercentage);
        else updateBusloadLED(busLoad[1].busloadPercentage);
    }

    /*if (SerialUSB)*/ isConnected = true;

    //there is no switch debouncing here at the moment
    //if mark triggering causes bounce then debounce this later on.
    /*
    if (getDigital(0)) {
    	if (!markToggle) {
    		markToggle = true;
    		if (!settings.useBinarySerialComm) SerialUSB.println("MARK TRIGGERED");
    		else
    		{ //figure out some sort of binary comm for the mark.
    		}
    	}
    }
    else markToggle = false;
    */

    //if (!SysSettings.lawicelMode || SysSettings.lawicelAutoPoll || SysSettings.lawicelPollCounter > 0)
    //{
    if (Can0.available() > 0) {
        Can0.read(incoming);
        addBits(0, incoming);
        toggleRXLED();
        if (isConnected) sendFrameToUSB(incoming, 0);
        if (SysSettings.logToFile) sendFrameToFile(incoming, 0);
        if (digToggleSettings.enabled && (digToggleSettings.mode & 1) && (digToggleSettings.mode & 2)) processDigToggleFrame(incoming);
    }

    if (Can1.available() > 0) {
        Can1.read(incoming);
        addBits(1, incoming);
        toggleRXLED();
        if (isConnected) sendFrameToUSB(incoming, 1);
        if (digToggleSettings.enabled && (digToggleSettings.mode & 1) && (digToggleSettings.mode & 4)) processDigToggleFrame(incoming);
        if (SysSettings.logToFile) sendFrameToFile(incoming, 1);
    }
    
    if (SWCAN.GetRXFrame(swIncoming)) {
        //copy into our normal CAN struct so we can pretend and all existing functions can access the frame
        incoming.id = swIncoming.id;
        incoming.extended = swIncoming.extended;
        incoming.length = swIncoming.length;
        incoming.data.value = swIncoming.data.value;
        toggleRXLED();
        if (isConnected) sendFrameToUSB(incoming, 2);
        //TODO: Maybe support digital toggle system on swcan too.
        if (SysSettings.logToFile) sendFrameToFile(incoming, 2);      
    }

    
    if (SysSettings.lawicelPollCounter > 0) SysSettings.lawicelPollCounter--;
    //}

    if (digToggleSettings.enabled && !(digToggleSettings.mode & 1)) {
        if (digTogglePinState) { //pin currently high. Look for it going low
            if (!digitalRead(digToggleSettings.pin)) digTogglePinCounter++; //went low, increment debouncing counter
            else digTogglePinCounter = 0; //whoops, it bounced or never transitioned, reset counter to 0

            if (digTogglePinCounter > 3) { //transitioned to LOW for 4 checks in a row. We'll believe it then.
                digTogglePinState = false;
                sendDigToggleMsg();
            }
        } else { //pin currently low. Look for it going high
            if (digitalRead(digToggleSettings.pin)) digTogglePinCounter++; //went high, increment debouncing counter
            else digTogglePinCounter = 0; //whoops, it bounced or never transitioned, reset counter to 0

            if (digTogglePinCounter > 3) { //transitioned to HIGH for 4 checks in a row. We'll believe it then.
                digTogglePinState = true;
                sendDigToggleMsg();
            }
        }
    }

    //delay(100);

    if (micros() - lastFlushMicros > SER_BUFF_FLUSH_INTERVAL) {
        if (serialBufferLength > 0) {
            SerialUSB.write(serialBuffer, serialBufferLength);
            serialBufferLength = 0;
            lastFlushMicros = micros();
        }
    }

    serialCnt = 0;
    while (isConnected && (SerialUSB.available() > 0) && serialCnt < 128) {
        serialCnt++;
        in_byte = SerialUSB.read();
        switch (state) {
        case IDLE:{
            if (in_byte == 0xF1) state = GET_COMMAND;
            else if (in_byte == 0xE7) {
                settings.useBinarySerialComm = true;
                SysSettings.lawicelMode = false;
                setPromiscuousMode(); //going into binary comm will set promisc. mode too.
            } else {
                console.rcvCharacter((uint8_t)in_byte);
            }
            break;
            }
        case GET_COMMAND:{
                switch(in_byte){
                    case PROTO_BUILD_CAN_FRAME:{
                            state = BUILD_CAN_FRAME;
                            buff[0] = 0xF1;
                            step = 0;
                            break;
                        }
                    case PROTO_TIME_SYNC:{
                            state = TIME_SYNC;
                            step = 0;
                            buff[0] = 0xF1;
                            buff[1] = 1; //time sync
                            buff[2] = (uint8_t) (now & 0xFF);
                            buff[3] = (uint8_t) (now >> 8);
                            buff[4] = (uint8_t) (now >> 16);
                            buff[5] = (uint8_t) (now >> 24);
                            SerialUSB.write(buff, 6);
                            break;
                        }
                    case PROTO_DIG_INPUTS:{
                            //immediately return the data for digital inputs
                            temp8 = getDigital(0) + (getDigital(1) << 1) + (getDigital(2) << 2) + (getDigital(3) << 3);
                            buff[0] = 0xF1;
                            buff[1] = 2; //digital inputs
                            buff[2] = temp8;
                            temp8 = checksumCalc(buff, 2);
                            buff[3] = temp8;
                            SerialUSB.write(buff, 4);
                            state = IDLE;
                            break;
                        }
                    case PROTO_ANA_INPUTS:{
                            //immediately return data on analog inputs
                            temp16 = getAnalog(0);
                            buff[0] = 0xF1;
                            buff[1] = 3;
                            buff[2] = temp16 & 0xFF;
                            buff[3] = uint8_t(temp16 >> 8);
                            temp16 = getAnalog(1);
                            buff[4] = temp16 & 0xFF;
                            buff[5] = uint8_t(temp16 >> 8);
                            temp16 = getAnalog(2);
                            buff[6] = temp16 & 0xFF;
                            buff[7] = uint8_t(temp16 >> 8);
                            temp16 = getAnalog(3);
                            buff[8] = temp16 & 0xFF;
                            buff[9] = uint8_t(temp16 >> 8);
                            temp8 = checksumCalc(buff, 9);
                            buff[10] = temp8;
                            SerialUSB.write(buff, 11);
                            state = IDLE;
                            break;
                        }
                    case PROTO_SET_DIG_OUT:{
                            state = SET_DIG_OUTPUTS;
                            buff[0] = 0xF1;
                            break;
                        }
                    case PROTO_SETUP_CANBUS:{
                            state = SETUP_CANBUS;
                            step = 0;
                            buff[0] = 0xF1;
                            break;
                        }
                    case PROTO_GET_CANBUS_PARAMS:{
                            //immediately return data on canbus params
                            buff[0] = 0xF1;
                            buff[1] = 6;
                            buff[2] = settings.CAN0_Enabled + ((unsigned char) settings.CAN0ListenOnly << 4);
                            buff[3] = settings.CAN0Speed;
                            buff[4] = settings.CAN0Speed >> 8;
                            buff[5] = settings.CAN0Speed >> 16;
                            buff[6] = settings.CAN0Speed >> 24;
                            buff[7] = settings.CAN1_Enabled + ((unsigned char) settings.CAN1ListenOnly << 4); //+ (unsigned char)settings.singleWireMode << 6;
                            buff[8] = settings.CAN1Speed;
                            buff[9] = settings.CAN1Speed >> 8;
                            buff[10] = settings.CAN1Speed >> 16;
                            buff[11] = settings.CAN1Speed >> 24;
                            SerialUSB.write(buff, 12);
                            state = IDLE;
                            break;
                        }
                    case PROTO_GET_DEV_INFO:{
                            //immediately return device information
                            buff[0] = 0xF1;
                            buff[1] = 7;
                            buff[2] = CFG_BUILD_NUM & 0xFF;
                            buff[3] = (CFG_BUILD_NUM >> 8);
                            buff[4] = EEPROM_VER;
                            buff[5] = (unsigned char) settings.fileOutputType;
                            buff[6] = (unsigned char) settings.autoStartLogging;
                            buff[7] = 0; //was single wire mode. Should be rethought for this board.
                            SerialUSB.write(buff, 8);
                            state = IDLE;
                            break;
                        }
                    case PROTO_SET_SW_MODE:{
                            buff[0] = 0xF1;
                            state = SET_SINGLEWIRE_MODE;
                            step = 0;
                            break;
                        }
                    case PROTO_KEEPALIVE:{
                            buff[0] = 0xF1;
                            buff[1] = 0x09;
                            buff[2] = 0xDE;
                            buff[3] = 0xAD;
                            SerialUSB.write(buff, 4);
                            state = IDLE;
                            break;
                        }
                    case PROTO_SET_SYSTYPE:{
                            buff[0] = 0xF1;
                            state = SET_SYSTYPE;
                            step = 0;
                            break;
                        }
                    case PROTO_ECHO_CAN_FRAME:{
                            state = ECHO_CAN_FRAME;
                            buff[0] = 0xF1;
                            step = 0;
                            break;
                        }
                    case PROTO_GET_NUMBUSES:{
                            buff[0] = 0xF1;
                            buff[1] = 12;
                            buff[2] = 3; //number of buses actually supported by this hardware (TODO: will be 5 eventually)
                            SerialUSB.write(buff, 3);
                            state = IDLE;
                            break;
                        }
                }
                break;
            }
        case BUILD_CAN_FRAME:{
                buff[1 + step] = in_byte;
                switch(step){
                    case 0:{
                            build_out_frame.id = in_byte;
                            break;
                        }
                    case 1:{
                            build_out_frame.id |= in_byte << 8;
                            break;
                        }
                    case 2:{
                            build_out_frame.id |= in_byte << 16;
                            break;
                        }
                    case 3:{
                            build_out_frame.id |= in_byte << 24;
                            if(build_out_frame.id & 1 << 31){
                                build_out_frame.id &= 0x7FFFFFFF;
                                build_out_frame.extended = true;
                            } else build_out_frame.extended = false;
                            break;
                        }
                    case 4:{
                            out_bus = in_byte & 1;
                            break;
                        }
                    case 5:{
                            build_out_frame.length = in_byte & 0xF;
                            if(build_out_frame.length > 8) build_out_frame.length = 8;
                            break;
                        }
                    default:{
                            if(step < build_out_frame.length + 6){
                                build_out_frame.data.bytes[step - 6] = in_byte;
                            } else{
                                state = IDLE;
                                //this would be the checksum byte. Compute and compare.
                                temp8 = checksumCalc(buff, step);
                                //if (temp8 == in_byte)
                                //{
                                /*
                                if (settings.singleWireMode == 1)
                                {
                                if (build_out_frame.id == 0x100)
                                {
                                if (out_bus == 1)
                                {
                                setSWCANWakeup();
                                delay(5);
                                }
                                }
                                }
                                */
                                if(out_bus == 0) sendFrame(Can0, build_out_frame);
                                if(out_bus == 1) sendFrame(Can1, build_out_frame);
                                if(out_bus == 2) sendFrameSW(build_out_frame);
                                /*
                                if (settings.singleWireMode == 1)
                                {
                                if (build_out_frame.id == 0x100)
                                {
                                if (out_bus == 1)
                                {
                                delay(5);
                                setSWCANEnabled();
                                }
                                }
                                } */
                                //}
                            }
                            break;
                        }
                }
                step++;
                break;
            }
        case TIME_SYNC:{
            state = IDLE;
            break;
            }
        case SET_DIG_OUTPUTS:{ //todo: validate the XOR byte
            buff[1] = in_byte;
            //temp8 = checksumCalc(buff, 2);
            for (int c = 0; c < 8; c++) {
                if (in_byte & (1 << c)) setOutput(c, true);
                else setOutput(c, false);
            }
            state = IDLE;
            break;
            }
        case SETUP_CANBUS:{ //todo: validate checksum
                switch(step){
                    case 0:{
                            build_int = in_byte;
                            break;
                        }
                    case 1:{
                            build_int |= in_byte << 8;
                            break;
                        }
                    case 2:{
                            build_int |= in_byte << 16;
                            break;
                        }
                    case 3:{
                            build_int |= in_byte << 24;
                            if(build_int > 0){
                                if(build_int & 0x80000000){ //signals that enabled and listen only status are also being passed
                                    if(build_int & 0x40000000){
                                        settings.CAN0_Enabled = true;
                                        Can0.enable();
                                    } else{
                                        settings.CAN0_Enabled = false;
                                        Can0.disable();
                                    }
                                    if(build_int & 0x20000000){
                                        settings.CAN0ListenOnly = true;
                                        Can0.enable_autobaud_listen_mode();
                                    } else{
                                        settings.CAN0ListenOnly = false;
                                        Can0.disable_autobaud_listen_mode();
                                    }
                                } else{
                                    Can0.enable(); //if not using extended status mode then just default to enabling - this was old behavior
                                    settings.CAN0_Enabled = true;
                                }
                                build_int = build_int & 0xFFFFF;
                                if(build_int > 1000000) build_int = 1000000;
                                Can0.begin(build_int, 255);
                                //Can0.set_baudrate(build_int);
                                settings.CAN0Speed = build_int;
                            } else{ //disable first canbus
                                Can0.disable();
                                settings.CAN0_Enabled = false;
                            }
                            break;
                        }
                    case 4:{
                            build_int = in_byte;
                            break;
                        }
                    case 5:{
                            build_int |= in_byte << 8;
                            break;
                        }
                    case 6:{
                            build_int |= in_byte << 16;
                            break;
                        }
                    case 7:{
                            build_int |= in_byte << 24;
                            if(build_int > 0){
                                if(build_int & 0x80000000){ //signals that enabled and listen only status are also being passed
                                    if(build_int & 0x40000000){
                                        settings.CAN1_Enabled = true;
                                        Can1.enable();
                                    } else{
                                        settings.CAN1_Enabled = false;
                                        Can1.disable();
                                    }
                                    if(build_int & 0x20000000){
                                        settings.CAN1ListenOnly = true;
                                        Can1.enable_autobaud_listen_mode();
                                    } else{
                                        settings.CAN1ListenOnly = false;
                                        Can1.disable_autobaud_listen_mode();
                                    }
                                } else{
                                    Can1.enable(); //if not using extended status mode then just default to enabling - this was old behavior
                                    settings.CAN1_Enabled = true;
                                }
                                build_int = build_int & 0xFFFFF;
                                if(build_int > 1000000) build_int = 1000000;
                                Can1.begin(build_int, 255);
                                //Can1.set_baudrate(build_int);

                                settings.CAN1Speed = build_int;
                            } else{ //disable second canbus
                                Can1.disable();
                                settings.CAN1_Enabled = false;
                            }
                            state = IDLE;
                            //now, write out the new canbus settings to EEPROM
                            EEPROM.write(EEPROM_ADDR, settings);
                            setPromiscuousMode();
                            break;
                        }
                }
                step++;
                break;
            }
        case SET_SINGLEWIRE_MODE:{
                if(in_byte == 0x10){
                } else{
                }
                EEPROM.write(EEPROM_ADDR, settings);
                state = IDLE;
                break;
            }
        case SET_SYSTYPE:{
            settings.sysType = in_byte;
            EEPROM.write(EEPROM_ADDR, settings);
            loadSettings();
            state = IDLE;
            break;
            }
        case ECHO_CAN_FRAME:{
                buff[1 + step] = in_byte;
                switch (step) {
                case 0:{
                    build_out_frame.id = in_byte;
                    break;
                    }
                case 1:{
                    build_out_frame.id |= in_byte << 8;
                    break;
                    }
                case 2:{
                    build_out_frame.id |= in_byte << 16;
                    break;
                    }
                case 3:{
                    build_out_frame.id |= in_byte << 24;
                    if (build_out_frame.id & 1 << 31) {
                        build_out_frame.id &= 0x7FFFFFFF;
                        build_out_frame.extended = true;
                    } else build_out_frame.extended = false;
                    break;
                    }
                case 4:{
                    out_bus = in_byte & 1;
                    break;
                    }
                case 5:{
                    build_out_frame.length = in_byte & 0xF;
                    if (build_out_frame.length > 8) build_out_frame.length = 8;
                    break;
                    }
                default:{
                    if (step < build_out_frame.length + 6) {
                        build_out_frame.data.bytes[step - 6] = in_byte;
                    } else {
                        state = IDLE;
                        //this would be the checksum byte. Compute and compare.
                        temp8 = checksumCalc(buff, step);
                        //if (temp8 == in_byte)
                        //{
                        toggleRXLED();
                        if (isConnected) sendFrameToUSB(build_out_frame, 0);
                        //}
                    }
                    break;
                    }
                }
            step++;
            break;
            }
        }
    }
    Logger::loop();
    elmEmulator.loop();
}

