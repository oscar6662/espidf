/**********************************************************/
/*                                                        */
/*  "Distributed Collatz Verification"  for ESP32         */
/*                                                        */
/*  Esa Hyytiä, Nov 2021                                  */
/**********************************************************/
#include <stdio.h>
#include <string.h>  // memcpy
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nvs_flash.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_now.h"
#include "esp_netif.h"
#include "esp_log.h"

#include "network.h"
#include "collatz.h"
#include "rl_int.h"

/******************************************************************/

#define LED_PIN 2         // comment this out if led is used elsewhere

#if defined( LED_PIN )
#include "driver/gpio.h"
static int led_state = 0;
static int led_count = 0;
#endif

/*********************************************************************/

#define COMP  "collatz"    // keyword for the LOG-system

/*
 * Integer frame:
 * - base offset bd (blocks done)
 * - BLOCKS    = number of blocks
 * - BLOCKSIZE = integers per block
 */
#if defined(TESTCASE)
#define BLOCKSIZE (((uint32_t)1)<<4)  // 16 and EVEN
#define BLOCKS  4
#else
#define BLOCKSIZE (((uint32_t)1)<<22)  // about 4M, and EVEN
#define BLOCKS 32                      // check pick_block if you increase this significantly!
#endif

#define BLOCK_FREE  0  // waiting to be processed
#define BLOCK_TAKEN 1  // someone is supposedly working on it
#define BLOCK_DONE  2  // reported as completed
#define BLOCK_MASK  3  // to extract state of computation

#define BLOCK_UP    8  // for communication, message heading up

/*
 *  Local computation variables
 */
static bigint_t waterlevel;  /* n <= waterlevel are all (conditionally) cleared */
static bigint_t n;           /* variable we work with, the sequence!            */

/*
 *  State of the computation and Message frame
 */
typedef struct 
{
    char     magic[4];    /* Unique identifier, "f3n1" (no terminator) */
    int16_t  report_type; /* BLOCK_TAKEN or BLOCK_DONE                 */
    int16_t  block_id;    /* the block we work(ed) on, or -1           */
    bigint_t base;        /* blocks done -- offset, and ODD!           */
} collatz_t;

/* These variables are behind the semaphore */
static SemaphoreHandle_t mutex = NULL;
static collatz_t         job;             // the current frame 
static collatz_t         job2;            // for processing incoming reports
static uint8_t           block[ BLOCKS ]; // 
static int               collatz_root;

/*
 *  HW random numbers
 */

#define DR_REG_RNG_BASE   0x3ff75144

uint32_t hw_random32( void ) {
    return (uint32_t)READ_PERI_REG( DR_REG_RNG_BASE );
}

/*
 *  This function selects the next block in random
 *  - priority is on the free blocks
 *  - non-uniform selection prefers earlier blocks (integer frame moves earlier)
 *  - Acquires the semaphore itself
 */
int pick_block(void) 
{
    uint32_t mass = 0;
    
    xSemaphoreTake( mutex, portMAX_DELAY );   // we deal with block[], so lock needed
    
    for(uint32_t i=0; i<BLOCKS; i++)          // no overflow if BLOCKS <= 92681
        if ( block[i]==BLOCK_FREE )
            mass += BLOCKS-i;
    if ( mass )
    {
        uint32_t rnd = hw_random32() % mass;  // ok, assuming that prng is good and mass << 2^32
        for(int i=0; i<BLOCKS; i++)
        {
            if ( block[i]!=BLOCK_FREE )
                continue;
            uint32_t p = BLOCKS - i;            
            if ( rnd < p )
            {
                xSemaphoreGive( mutex );
                return i;
            }            
            rnd -= p;
        }
        /* never reached */
    }    
    /*
     *  Everything is TAKEN, so someone probably left ...
     *  Choose the first block so that we can move on!
     */
    xSemaphoreGive( mutex );
    return 0;
}

/*
 *  Semaphore MUST be acquired before calling this function
 */
void shift_blocks( int done )
{
    if ( done < BLOCKS )
    {
        int left = BLOCKS - done;
        int i,j  = done;

        for( i=0; i<left;   i++)  block[i] = block[j++];
        for(    ; i<BLOCKS; i++)  block[i] = BLOCK_FREE;

        if ( job.block_id >= done ) 
            job.block_id -= done;         /* still on board! */
        else
            job.block_id = -1;
    }
    else /* all done ... */
    {
        for(int i=0; i<BLOCKS; i++)
            block[i] = BLOCK_FREE;
        job.block_id = -1;
    }
}

void log_report_blocks(void)
{
    char buf[BLOCKS+1];
    for(int i=0; i<BLOCKS; i++)
    {
        switch( block[i] ) 
        {
            default:
            case BLOCK_FREE:   buf[i] = '_';  break;
            case BLOCK_TAKEN:  buf[i] = '+';  break;
            case BLOCK_DONE:   buf[i] = 'X';  break;
        }
    }
    buf[ BLOCKS ] = '\0';  // not to be forgotten
    ESP_LOGI( COMP, "  blocks: [%s]", buf );
}


/*
 *  Define a way for Collatz computation to send message to other nodes
 */
void broadcast_message( collatz_t *job )
{
    app_header_t  hdr;
    int           len = sizeof( collatz_t );
    
    if ( len <= NET_MAX_PAYLOAD )
    {
        hdr.type = APP_COLLATZ_ID;
        hdr.len  = len;
        if ( collatz_root )
        {
            job->report_type &= ~BLOCK_UP;  /* make sure! */
            net_send_down(  &hdr, (const uint8_t *)job);
        }
        else
        {
            job->report_type |= BLOCK_UP;
            net_send_up(  &hdr, (const uint8_t *)job);
        }
    }
}

/*
 *  Inform others about my progress, if any:
 *  - (non-zero) bid indicates the completed block: 0 => none, 1 => 1st
 *  - Semaphore MUST be acquired before calling this function
 */
void report_my_progress(int fin)
{
    int done = 0;

    if ( block[0]==BLOCK_DONE ) 
    {
        rl_add( &job.base, BLOCKSIZE );
        for(done=1; done<BLOCKS && block[done]==BLOCK_DONE; done++)
            rl_add( &job.base, BLOCKSIZE );
        shift_blocks( done );
        ESP_LOGI(COMP, "Shifted %d blocks, the current frame is 0x%s, and block %d (fin %d)",
                 done, rl_str( &job.base ), job.block_id, fin );
        log_report_blocks();        
    }
    else if ( !fin )  /* progress elsewhere did not trigger changes to our state            */
        return;
    else              /* we have complete an isolated block, report it to avoid double work */
    {
        ESP_LOGI(COMP, "Reporting block %d from frame 0x%s", job.block_id, rl_str( &job.base ));
    }
    
    /* what's done is done! */
    job.report_type = BLOCK_DONE;
    broadcast_message( &job );
}

/*
 *  Inform others about my job => BLOCK_TAKEN
 *  - Semaphore MUST be acquired before calling this function
 */
void report_my_start(void)
{
    ESP_LOGI(COMP, "Computing block %d from frame 0x%s", job.block_id, rl_str( &job.base ) );
    job.report_type = BLOCK_TAKEN;
    broadcast_message( &job );
}

/**********************************************************/
/*
 * Process a report received from elsewhere
 *  - This functions acquires the semaphore FIRST and keeps it until the END
 *  - Data is copied only if needed -- green computing!
 *  - Semaphore MUST be acquired before calling this function
 */
void process_report( const collatz_t *rpt )
{
    int16_t rt  = rpt->report_type & BLOCK_MASK;
    ESP_LOGI(COMP, "Received a report for block %d frame 0x%s %s",
             rpt->block_id, rl_str( &rpt->base ),
             (rt==BLOCK_TAKEN ? "taken" : "done")
        );

    /*** First adjust the high water marks to same offset ***/
    int d = rl_cmp(&rpt->base,&job.base); 

    if ( d < 0 )  /* rpt.base < our.base */
    {
        memcpy( &job2, rpt, sizeof(collatz_t) );
        rpt = &job2;  /* use a local (modified) copy in this case */
        ESP_LOGI(COMP, " - report is with a lower base" );
        do 
        {
            if ( job2.block_id < 0 )
                return;   // old news!
            rl_add( &job2.base, BLOCKSIZE);
            job2.block_id--;
        } while ( rl_cmp( &job2.base, &job.base) < 0 );
        /* Confirm that base offsets are equal! */
        if ( rl_equal(&job.base,&rpt->base) ) 
        {
            ESP_LOGE(COMP, " - job.base != rpt.base = 0x%s (ignored!)", rl_str( &rpt->base ) );
            ESP_LOGE(COMP, " -             job.base = 0x%s (ignored!)", rl_str( &job.base  ) );
        }
        
        ESP_LOGI(COMP, " - new block id is %d", rpt->block_id );
    }
    else if ( d > 0 )  /* rpt.base > our.base : update our integer frame */
    {
        int left = BLOCKS;
        do
        {
            rl_add( &job.base, BLOCKSIZE );
            left--;
        } while( left && rl_greater(&rpt->base, &job.base) );
        ESP_LOGI(COMP, " - raising the integer frame by %d blocks", BLOCKS-left );

        /* something to preserve?! : shift if so */
        shift_blocks( BLOCKS-left );
        ESP_LOGI(COMP, " - shifted %d blocks, frame is 0x%s, and block %d",
                 BLOCKS-left, rl_str( &job.base ), job.block_id );
        log_report_blocks();        
        
        if ( !left )
            rl_set( &job.base, &rpt->base );
    }
    /* now new base == old base; and block id's are in the integer frame */
    if ( rpt->block_id >= 0 ) 
    {
        int16_t nbi = rpt->block_id;

        if ( block[nbi] < rt )
            ESP_LOGI(COMP, " - block %d state updated to %d", nbi, ((int)rt) );
        block[nbi] = (block[nbi] > rt ? block[nbi] : rt);  // max(...)
        if ( rt==BLOCK_DONE && nbi==job.block_id )
        {
            ESP_LOGI(COMP, " - our current computation is obsolete!" );
            job.block_id = -1;
        }
    }
    
    /*
     * final step: report our follow-up progress!
     */
    report_my_progress(0);
}        

/*
 *  The actual work is done here -- compute one block:
 *  -  Semaphore is acquired when needed (at start, and at the end)
 */
int compute_block( int bi )
{
    if ( rl_overflow )
    {
        ESP_LOGE(COMP, "Overflow detected -- computation cancelled" );
        return -1;
    }    
    
    /**********************************************************/
    xSemaphoreTake( mutex, portMAX_DELAY );
    job.block_id = bi;
    switch( block[ bi ] ) 
    {
        case BLOCK_DONE:
        default:
            xSemaphoreGive( mutex );
            return 0;
        case BLOCK_TAKEN:
            ESP_LOGW( COMP, "Recomputing the same block?!" );
            break;
        case BLOCK_FREE:
            block[ bi ] = BLOCK_TAKEN;
            break;
    }
    report_my_start();  // inform others: (bd,bi) => BLOCK_TAKEN
    
    /* bd + bi*BLOCKSIZE */
    rl_set( &waterlevel, &job.base );
    for(int i=bi; i; i--)
        rl_add( &waterlevel, BLOCKSIZE );

    xSemaphoreGive( mutex );
    /**********************************************************/
    /* Process the block */
    for( int i=0; i<BLOCKSIZE; i+=2 )
    {
        rl_set( &n, &waterlevel );
        rl_add( &n, 2 );
        do 
        {
            rl_f3n1(  &n );
            rl_fdiv2( &n );
            if ( rl_overflow )
            {
                ESP_LOGE(COMP, "Overflow detected -- computation terminated" );
                return -1;
            }
        }
        while( rl_greater( &n, &waterlevel ) );
        rl_add( &waterlevel, 2 );
#if defined( LED_PIN )
        // 0xffff ~ 1sec
        led_count = (led_count + 1) & 0x1ffful;
        if ( !led_count )
        {
            led_state = led_state ? 0 : 1;
            gpio_set_level( LED_PIN, led_state);
        }
#endif
    }
    /**********************************************************/
    xSemaphoreTake( mutex, portMAX_DELAY );    
    if ( job.block_id >= 0 )      /* Check what to do with our effort */
    {
        block[ job.block_id ] = BLOCK_DONE;
        report_my_progress( 1 );
        job.block_id = -1;        /* computation just finished */
    }
    xSemaphoreGive( mutex );
    /**********************************************************/
    return 0;
}

/*
 *  Computing task 'Collatz' that never rests and never returns
 */
void collatz_compute(void *pvParameter)
{
    vTaskDelay(1000 / portTICK_RATE_MS);
    ESP_LOGI(COMP, "Computing!" );

    /*
     *  Then we work and work ... and work!
     */
    while( 1 )
    {
        int b = pick_block();
        if ( compute_block( b ) )
            break;
        taskYIELD();
    }
    xSemaphoreTake( mutex, portMAX_DELAY );    
    ESP_LOGI(COMP, "Computation task terminated (int frame 0x%s)", rl_str( &job.base ) );
    xSemaphoreGive( mutex );

    vTaskDelete( 0 );  // the end!
}

/**********************************************************/

int magic( const char *buf, const char *key )
{
    int i=3;
    while(1)
    {
        if ( buf[i] != key[i] )
            return -1;
        if ( !i )
            return  0;
        i--;
    }
}


/*
 * Task responsible for communication
 */

void collatz_comm(void *pvParameter)
{
    printf("Collatz comm task started %s\n", collatz_root ? "(root)" : "");
    while( 1 )
    {
        static app_header_t  hdr;
        static uint8_t pay[ NET_MAX_PAYLOAD ];

        while ( !net_receive( APP_COLLATZ_ID, &hdr, pay, 4000 ) )
        {
            collatz_t *rpt = (collatz_t *)pay;
            if ( hdr.len != sizeof( collatz_t ) || magic( (const char *)pay, "f3n1" ) )
                continue;
            if ( !(rpt->report_type & BLOCK_UP) || collatz_root )
            {
                rpt->report_type &= (~BLOCK_UP);
                net_send_down( &hdr, pay );

                xSemaphoreTake( mutex, portMAX_DELAY );
                process_report( rpt );
                xSemaphoreGive( mutex );
            }
            else  /* packet on its way up */
            {
                net_send_up( &hdr, pay );
            }
            vTaskDelay( 20 / portTICK_RATE_MS);  // process the incoming reports at faster rate
        }
        vTaskDelay( 100 / portTICK_RATE_MS );  // not needed?!
    }
}
        

/**********************************************************/

void collatz_init(int root) 
{
#if defined( LED_PIN )
    gpio_pad_select_gpio ( LED_PIN );
    gpio_set_direction ( LED_PIN, GPIO_MODE_OUTPUT );
#endif

    collatz_root = root;  // affects our behavior
    
    net_register_app( APP_COLLATZ_ID );
    
    /* init data structures */
    job.magic[0] = 'f';
    job.magic[1] = '3';
    job.magic[2] = 'n';
    job.magic[3] = '1';
    
    /*
     *  Init data structures: case n=1 is the start
     */
    rl_overflow   = 0;
    for(int i=0; i<BLOCKS; i++)
        block[i] = BLOCK_FREE;

#if defined( START_FROM_ONE )
    job.base.len  = 1;
    job.base.a[0] = 0x1;
#else  /* 2^68 */
    job.base.len  = 3;
    job.base.a[0] = MASK;       // 30
    job.base.a[1] = MASK;       // 60 
    job.base.a[2] = (1<<8) - 1; // 68
#endif

    mutex = xSemaphoreCreateMutex();          // to guard computation variables

    /* then the tasks */
    xTaskCreate(
        &collatz_comm,     // - function ptr
        "collatz-comm",    // - arbitrary name
        2048,              // - stack size [byte]
        NULL,              // - optional data for task
        1,                 // - priority, higher than comp task
        NULL);             // - handle to task (for control)

    xTaskCreate(
        &collatz_compute,  // - function ptr
        "collatz-comp",    // - arbitrary name
        2048,              // - stack size [byte]
        NULL,              // - optional data for task
        0,                 // - priority, "background" computation
        NULL);             // - handle to task (for control)
}

/**********************************************************/
