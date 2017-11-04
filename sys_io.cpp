/*
 * sys_io.cpp
 *
 * Handles the low level details of system I/O
 *
Copyright (c) 2013 Collin Kidder, Michael Neuweiler, Charles Galpin

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

some portions based on code credited as:
Arduino Due ADC->DMA->USB 1MSPS
by stimmer

*/


/*
 * WARNING - It is very unlikely that any of this code works properly on M2 boards. These routines are a dependency of M2RET for legacy reasons but really
 * this should be corrected. Don't trust these routines to work properly.
 */


#include "sys_io.h"

#undef HID_ENABLED

uint8_t dig[NUM_DIGITAL];
uint8_t adc[NUM_ANALOG][2];
uint8_t out[NUM_OUTPUT];

volatile int bufn,obufn;
volatile uint16_t adc_buf[NUM_ANALOG][256];   // 8 buffers of 256 readings
uint16_t adc_values[NUM_ANALOG * 2];
uint16_t adc_out_vals[NUM_ANALOG];

int NumADCSamples;

//the ADC values fluctuate a lot so smoothing is required.
uint16_t adc_buffer[NUM_ANALOG][64];
uint8_t adc_pointer[NUM_ANALOG]; //pointer to next position to use

ADC_COMP adc_comp[NUM_ANALOG];

bool useRawADC = false;

//forces the digital I/O ports to a safe state. This is called very early in initialization.
void sys_early_setup()
{
    int i;

    NumADCSamples = 64;

    dig[0] = digi_1;
    dig[1] = digi_2;
    dig[2] = digi_3;
    dig[3] = digi_4;
    dig[4] = digi_5;
    dig[5] = digi_6;

    adc[0][0] = ana1;
    adc[0][1] = 255;
    adc[1][0] = ana2;
    adc[1][1] = 255;
    adc[2][0] = ana3;
    adc[2][1] = 255;
    adc[3][0] = ana4;
    adc[3][1] = 255;
    adc[4][0] = ana5;
    adc[4][1] = 255;
    adc[5][0] = ana6;
    adc[5][1] = 255;
    adc[6][0] = ana7;
    adc[6][1] = 255;
    adc[7][0] = ana8;
    adc[7][1] = 255;

    out[0] = digo_1;    //4
    out[1] = digo_2;    //5
    out[2] = digo_3;    //6
    out[3] = digo_4;    //7
    out[4] = digo_5;    //2
    out[5] = digo_6;    //3
    //out[6] = digo_7;    //8
    //out[7] = digo_8;    //9

    for(i = 0; i < NUM_DIGITAL; i++){
        if(dig[i] != 255){
            pinMode(dig[i], INPUT);
        }
    }

    for (i = 0; i < NUM_OUTPUT; i++) {
        if (out[i] != 255) {
            pinMode(out[i], OUTPUT);
            digitalWrite(out[i], LOW);
        }
    }

}

/*
Initialize DMA driven ADC and read in gain/offset for each channel
*/
void setup_sys_io()
{
    int i;

    setupFastADC();

    for (i = 0; i < NUM_ANALOG; i++) {
        for (int j = 0; j < NumADCSamples; j++) adc_buffer[i][j] = 0;
        adc_pointer[i] = 0;
        adc_values[i] = 0;
        adc_out_vals[i] = 0;
    }
}

uint16_t getRawADC(uint8_t which)
{
    uint32_t val;

    val = adc_values[adc[which][0]];

    return val;
}

/*
Adds a new ADC reading to the buffer for a channel. The buffer is NumADCSamples large (either 32 or 64) and rolling
*/
void addNewADCVal(uint8_t which, uint16_t val)
{
    adc_buffer[which][adc_pointer[which]] = val;
    adc_pointer[which] = (adc_pointer[which] + 1) % NumADCSamples;
}

/*
Take the arithmetic average of the readings in the buffer for each channel. This smooths out the ADC readings
*/
uint16_t getADCAvg(uint8_t which)
{
    uint32_t sum;
    sum = 0;
    for (int j = 0; j < NumADCSamples; j++) sum += adc_buffer[which][j];
    sum = sum / NumADCSamples;
    return ((uint16_t)sum);
}

/*
get value of one of the 8 analog inputs
Uses a special buffer which has smoothed and corrected ADC values. This call is very fast
because the actual work is done via DMA and then a separate polled step.
*/
uint16_t getAnalog(uint8_t which)
{
    uint16_t val;

    if (which >= NUM_ANALOG) which = 0;

    return adc_out_vals[which];
}

//get value of one of the 6 digital inputs
boolean getDigital(uint8_t which)
{
    if (which >= NUM_DIGITAL) which = 0;
    return !(digitalRead(dig[which]));
}

//set output high or not
void setOutput(uint8_t which, boolean active)
{
    if (which >= NUM_OUTPUT) return;
    if (out[which] == 255) return;
    if (active)
        digitalWrite(out[which], HIGH);
    else digitalWrite(out[which], LOW);
}

void setLED(uint8_t which, boolean hi)
{
    if (which == 255) return;
    if (hi) digitalWrite(which, HIGH);
    else digitalWrite(which, LOW);
}

//get current value of output state (high?)
boolean getOutput(uint8_t which)
{
    if (which >= NUM_OUTPUT) return false;
    if (out[which] == 255) return false;
    return digitalRead(out[which]);
}

/*
When the ADC reads in the programmed # of readings it will do two things:
1. It loads the next buffer and buffer size into current buffer and size
2. It sends this interrupt
This interrupt then loads the "next" fields with the proper values. This is
done with a four position buffer. In this way the ADC is constantly sampling
and this happens virtually for free. It all happens in the background with
minimal CPU overhead.
*/
void ADC_Handler()      // move DMA pointers to next buffer
{
    int f=ADC->ADC_ISR;
    if (f & (1<<27)) { //receive counter end of buffer
        bufn=(bufn+1)&3;
        ADC->ADC_RNPR=(uint32_t)adc_buf[bufn];
        ADC->ADC_RNCR=256;
    }
}

/*
Setup the system to continuously read the proper ADC channels and use DMA to place the results into RAM
Testing to find a good batch of settings for how fast to do ADC readings. The relevant areas:
1. In the adc_init call it is possible to use something other than ADC_FREQ_MAX to slow down the ADC clock
2. ADC_MR has a clock divisor, start up time, settling time, tracking time, and transfer time. These can be adjusted
*/
void setupFastADC()
{
    pmc_enable_periph_clk(ID_ADC);
    adc_init(ADC, SystemCoreClock, ADC_FREQ_MAX, ADC_STARTUP_FAST); //just about to change a bunch of these parameters with the next command

    /*
    The MCLK is 12MHz on our boards. The ADC can only run 1MHz so the prescaler must be at least 12x.
    The ADC should take Tracking+Transfer for each read when it is set to switch channels with each read

    Example:
    5+7 = 12 clocks per read 1M / 12 = 83333 reads per second. For newer boards there are 4 channels interleaved
    so, for each channel, the readings are 48uS apart. 64 of these readings are averaged together for a total of 3ms
    worth of ADC in each average. This is then averaged with the current value in the ADC buffer that is used for output.

    If, for instance, someone wanted to average over 6ms instead then the prescaler could be set to 24x instead.
    */
    ADC->ADC_MR = (1 << 7) //free running
                  + (5 << 8) //12x MCLK divider ((This value + 1) * 2) = divisor
                  + (1 << 16) //8 periods start up time (0=0clks, 1=8clks, 2=16clks, 3=24, 4=64, 5=80, 6=96, etc)
                  + (1 << 20) //settling time (0=3clks, 1=5clks, 2=9clks, 3=17clks)
                  + (4 << 24) //tracking time (Value + 1) clocks
                  + (2 << 28);//transfer time ((Value * 2) + 3) clocks

    //ADC->ADC_CHER=0xF0; //enable A0-A3
    ADC->ADC_CHER= Analog_Channels_Enabled; //enable A0-A7


    NVIC_EnableIRQ(ADC_IRQn);
    ADC->ADC_IDR=~(1<<27); //dont disable the ADC interrupt for rx end
    ADC->ADC_IER=1<<27; //do enable it
    ADC->ADC_RPR=(uint32_t)adc_buf[0];   // DMA buffer
    ADC->ADC_RCR=256; //# of samples to take
    ADC->ADC_RNPR=(uint32_t)adc_buf[1]; // next DMA buffer
    ADC->ADC_RNCR=256; //# of samples to take
    bufn=obufn=0;
    ADC->ADC_PTCR=1; //enable dma mode
    ADC->ADC_CR=2; //start conversions

    Logger::debug("Fast ADC Mode Enabled");
}

//polls	for the end of an adc conversion event. Then processe buffer to extract the averaged
//value. It takes this value and averages it with the existing value in an 8 position buffer
//which serves as a super fast place for other code to retrieve ADC values
// This is only used when RAWADC is not defined
void sys_io_adc_poll()
{
    if (obufn != bufn) {
        uint32_t tempbuff[8] = {0,0,0,0,0,0,0,0}; //make sure its zero'd

        //the eight or four enabled adcs are interleaved in the buffer
        //this is a somewhat unrolled for loop with no incrementer. it's odd but it works

        for (int i = 0; i < 256;) {
            tempbuff[3] += adc_buf[obufn][i++];
            tempbuff[2] += adc_buf[obufn][i++];
            tempbuff[1] += adc_buf[obufn][i++];
            tempbuff[0] += adc_buf[obufn][i++];
        }

        //now, all of the ADC values are summed over 32/64 readings. So, divide by 32/64 (shift by 5/6) to get the average
        //then add that to the old value we had stored and divide by two to average those. Lots of averaging going on.
        for (int j = 0; j < 4; j++) {
            adc_values[j] += (tempbuff[j] >> 6);
            adc_values[j] = adc_values[j] >> 1;
        }


        for (int i = 0; i < NUM_ANALOG; i++) {
            int val;
            val = getRawADC(i);
//			addNewADCVal(i, val);
//			adc_out_vals[i] = getADCAvg(i);
            adc_out_vals[i] = val;
        }

        obufn = bufn;
    }
}



