/*
FILENAME...	drvOms58.c
USAGE...	Motor record driver level support for OMS model VME58.

Version:	$Revision: 1.5 $
Modified By:	$Author: sluiter $
Last Modified:	$Date: 2001-06-19 18:25:10 $
*/

/*
 *      Original Author: Jim Kowalkowski
 *      Current Author: Joe Sullivan
 *      Date: 11/14/94
 *
 *      Experimental Physics and Industrial Control System (EPICS)
 *
 *      Copyright 1991, the Regents of the University of California,
 *      and the University of Chicago Board of Governors.
 *
 *      This software was produced under  U.S. Government contracts:
 *      (W-7405-ENG-36) at the Los Alamos National Laboratory,
 *      and (W-31-109-ENG-38) at Argonne National Laboratory.
 *
 *      Initial development by:
 *	      The Controls and Automation Group (AT-8)
 *	      Ground Test Accelerator
 *	      Accelerator Technology Division
 *	      Los Alamos National Laboratory
 *
 *      Co-developed with
 *	      The Controls and Computing Group
 *	      Accelerator Systems Division
 *	      Advanced Photon Source
 *	      Argonne National Laboratory
 *
 * NOTES
 * -----
 * Verified with firmware: VME58 ver 2.24-8 and 2.24-8S.
 *
 * Modification Log:
 * -----------------
 * .01  01-18-93	jbk     initialized
 * .02  11-14-94	jps     copy drvOms.c and modify to point to vme58 driver
 *      ...
 * .14  02-10-95	jps	first released version w/interrupt supprt
 * .15  02-16-95	jps	add better simulation mode support
 * .16  03-10-95	jps	limit switch handling improvements
 * .17  04-11-95	jps  	misc. fixes
 * .18  09-27-95	jps	fix GET_INFO latency bug, add axis argument to
 *				set_status() create add debug verbosity of 3
 * .19  05-03-96	jps     convert 32bit card accesses to 16bit - vme58PCB
 *				version D does not support 32bit accesses.
 * .20  06-20-96	jps     add more security to message parameters (card #)
 * .20a 02-19-97	tmm	fixed for EPICS 3.13
 * V4.1 01-20-00	rls	Update bit collision check and wait.  Problem
 *				with old position data returned after Done
 *				detected when moving multiple axes on same
 *				controller board.  Fix: Update shared memory
 *				data after Done detected via ASCII controller
 *				commands.  To avoid collisions, skip writing to
 *				control reg. from motorIsr() if update bit is
 *				ON.
 */


#include	<vxWorks.h>
#include	<stdio.h>
#include	<sysLib.h>
#include	<string.h>
#include	<taskLib.h>
#include        <rngLib.h>
#include	<rebootLib.h>
#include	<logLib.h>

#include	<alarm.h>
#include	<dbDefs.h>
#include	<dbAccess.h>
#include	<dbCommon.h>
#include	<fast_lock.h>
#include	<devSup.h>
#include	<drvSup.h>
#ifdef __cplusplus
extern "C" {
#include	<recSup.h>
#include	<devLib.h>
#include	<errlog.h>
}
#else
#include	<recSup.h>
#include        <devLib.h>
#include	<errlog.h>
#endif
#include	<errMdef.h>

#include	"motorRecord.h"	/* For Driver Power Monitor feature only. */
#include	"motor.h"
#include	"motordevCom.h"
#include	"drvOms58.h"

#define PRIVATE_FUNCTIONS 1	/* normal:1, debug:0 */

#define STATIC static

/* Define for return test on locationProbe() */
#define PROBE_SUCCESS(STATUS) ((STATUS)==S_dev_addressOverlap)

/* jps: INFO messages - add RV and move QA to top */
#define	ALL_INFO	"QA EA"	/* jps: add RV */
#define	AXIS_INFO	"QA"		/* jps: add RV */
#define	ENCODER_QUERY	"EA ID"
#define	AXIS_CLEAR	"CA"		/* Clear done of addressed axis */
#define	DONE_QUERY	"RA"		/* ?? Is this needed?? */
#define	PID_QUERY	"KK2 ID"


/*----------------debugging-----------------*/
#ifdef	DEBUG
    #define Debug(l, f, args...) { if(l<=drvOms58debug) printf(f,## args); }
#else
    #define Debug(l, f, args...)
#endif

#define pack2x16(p)      ((uint32_t)(((p[0])<<16)|(p[1])))

#ifdef __cplusplus
extern "C" long locationProbe(epicsAddressType, char *);
#else
extern long locationProbe(epicsAddressType, char *);
#endif

/* Global data. */
int oms58_num_cards = 0;
volatile int drvOms58debug = 0;

/* Local data required for every driver; see "motordrvComCode.h" */
#include	"motordrvComCode.h"

/* --- Local data common to all OMS drivers. --- */
STATIC char *oms_addrs = 0x0;
STATIC volatile unsigned omsInterruptVector = 0;
STATIC volatile uint8_t omsInterruptLevel = OMS_INT_LEVEL;
STATIC volatile int max_io_tries = MAX_COUNT;
STATIC volatile int motionTO = 10;
STATIC char oms58_axis[] = {'X', 'Y', 'Z', 'T', 'U', 'V', 'R', 'S'};

/* --- Local data specific to the Oms58 driver. --- */
STATIC volatile int cmndBuffReadyWait = 4;
STATIC volatile int dataReadyWait = 1;
STATIC volatile int omsNanoRate = 2000;	/* Aux clock rate for nanosleep */


/*----------------functions-----------------*/

/* Common local function declarations. */
static long report(int level);
static long init();
static void query_done(int, int, struct mess_node *);
static int set_status(int card, int signal);
static int send_mess(int card, char const *com, char c);
static int recv_mess(int, char *, int);
static void motorIsr(int card);
static int motor_init();
static void oms_reset();

STATIC void start_status(int card);
STATIC int motorIsrSetup(int card);
STATIC void oms_nanoSleep(int time);
STATIC int oms_nanoWakup(int val);

/*----------------functions-----------------*/

struct driver_table oms58_access =
{
    NULL,
    motor_send,
    motor_free,
    motor_card_info,
    motor_axis_info,
    &mess_queue,
    &queue_lock,
    &free_list,
    &freelist_lock,
    &motor_sem,
    &motor_state,
    &total_cards,
    &any_motor_in_motion,
    send_mess,
    recv_mess,
    set_status,
    query_done,
    start_status,
    &initialized,
    oms58_axis
};

struct
{
    long number;
#ifdef __cplusplus
    long (*report) (int);
    long (*init) (void);
#else
    DRVSUPFUN report;
    DRVSUPFUN init;
#endif
} drvOms58 = {2, report, init};

static long report(int level)
{
    int card;

    if (oms58_num_cards <= 0)
	printf("    No VME58 controllers configured.\n");
    else
    {
	for (card = 0; card < oms58_num_cards; card++)
	{
	    struct controller *brdptr = motor_state[card];

	    if (brdptr == NULL)
		printf("    Oms Vme58 motor card #%d not found.\n", card);
	    else
		printf("    Oms Vme58 motor card #%d @ 0x%X, id: %s \n", card,
		       (uint_t) motor_state[card]->localaddr,
		       motor_state[card]->ident);
	}
    }
    return (0);
}

static long init()
{
    initialized = ON;	/* Indicate that driver is initialized. */
    (void) motor_init();
    return ((long) 0);
}


STATIC void query_done(int card, int axis, struct mess_node *nodeptr)
{
    char buffer[40];

    send_mess(card, DONE_QUERY, oms58_axis[axis]);
    recv_mess(card, buffer, 1);

    if (nodeptr->status & RA_PROBLEM)
	send_mess(card, AXIS_STOP, oms58_axis[axis]);
}


/*********************************************************
 * Start card data area update.
 * Each card will take 800-900us to complete update.
 * start_status(int card)
 *            if card == -1 then start all cards
 *********************************************************/
STATIC void start_status(int card)
{
    volatile struct vmex_motor *pmotor;
    CNTRL_REG cntrlReg;
    int index;

    if (card >= 0)
    {
	if ((pmotor = (struct vmex_motor *) motor_state[card]->localaddr) != 0)
	{
	    /* Wait Forever for controller to update. */
	    cntrlReg.All = pmotor->control.cntrlReg;
	    while(cntrlReg.Bits.update != 0)
	    {
		Debug(1, "start_status(): Update Wait: card #%d\n", card);
		taskDelay(0);
		cntrlReg.All = pmotor->control.cntrlReg;
	    };

	    cntrlReg.Bits.update = 1;
	    pmotor->control.cntrlReg = cntrlReg.All;
	}
    }
    else
    {
	for (index = 0; index < total_cards; index++)
	{
	    if ((pmotor = (struct vmex_motor *) motor_state[index]->localaddr) != 0)
	    {
		/* Wait Forever for controller to update. */
		cntrlReg.All = pmotor->control.cntrlReg;
		while(cntrlReg.Bits.update != 0)
		{
		    Debug(1, "start_status(): Update Wait: card #%d\n", index);
		    taskDelay(0);
		    cntrlReg.All = pmotor->control.cntrlReg;
		};
    
		cntrlReg.Bits.update = 1;
		pmotor->control.cntrlReg = cntrlReg.All;
	    }
	}
    }
}

/**************************************************************
 * Read motor status from card
 * set_status()
 ************************************************************/

STATIC int set_status(int card, int signal)
{
    struct mess_info *motor_info;
    struct mess_node *nodeptr;
    volatile struct vmex_motor *pmotor;
    CNTRL_REG cntrlReg;
    volatile MOTOR_DATA_REGS *pmotorData;
    int32_t motorData;
    /* Message parsing variables */
    char *p, *tok_save;
    struct axis_status *ax_stat;
    struct encoder_status *en_stat;
    char q_buf[50], outbuf[50];
    int index;

    int rtn_state;

    motor_info = &(motor_state[card]->motor_info[signal]);
    nodeptr = motor_info->motor_motion;
    pmotor = (struct vmex_motor *) motor_state[card]->localaddr;

    if (motor_state[card]->motor_info[signal].encoder_present == YES)
    {
	/* get 4 peices of info from axis */
	send_mess(card, ALL_INFO, oms58_axis[signal]);
	recv_mess(card, q_buf, 2);
    }
    else
    {
	/* get 2 peices of info from axis */
	send_mess(card, AXIS_INFO, oms58_axis[signal]);
	recv_mess(card, q_buf, 1);
    }

    for (index = 0, p = strtok_r(q_buf, ",", &tok_save); p;
	 p = strtok_r(NULL, ",", &tok_save), index++)
    {
	switch (index)
	{
	case 0:		/* axis status */
	    ax_stat = (struct axis_status *) p;

	    if (ax_stat->direction == 'P')
		motor_info->status |= RA_DIRECTION;
	    else
		motor_info->status &= ~RA_DIRECTION;

	    if (ax_stat->done == 'D')
	    {
		/* Request Data Area Update after DONE detected so that both
		 * ASCII command data and shared memory data match. */
		start_status(card);
		motor_info->status |= RA_DONE;
	    }
	    else
		motor_info->status &= ~RA_DONE;

	    if (ax_stat->overtravel == 'L')
		motor_info->status |= RA_OVERTRAVEL;
	    else
		motor_info->status &= ~RA_OVERTRAVEL;

	    if (ax_stat->home == 'H')
		motor_info->status |= RA_HOME;
	    else
		motor_info->status &= ~RA_HOME;

	    break;
	case 1:		/* encoder status */
	    en_stat = (struct encoder_status *) p;

	    if (en_stat->slip_enable == 'E')
		motor_info->status |= EA_SLIP;
	    else
		motor_info->status &= ~EA_SLIP;

	    if (en_stat->pos_enable == 'E')
		motor_info->status |= EA_POSITION;
	    else
		motor_info->status &= ~EA_POSITION;

	    if (en_stat->slip_detect == 'S')
		motor_info->status |= EA_SLIP_STALL;
	    else
		motor_info->status &= ~EA_SLIP_STALL;

	    if (en_stat->axis_home == 'H')
		motor_info->status |= EA_HOME;
	    else
		motor_info->status &= ~EA_HOME;
	    break;
	default:
	    break;
	}
    }


    /* Setup mask for data-ready detection */
    cntrlReg.All = 0;
    cntrlReg.Bits.update = 1;

    /* Wait for data area update to complete on card */
    while (cntrlReg.All & pmotor->control.cntrlReg)
	oms_nanoSleep(dataReadyWait);

    pmotorData = &pmotor->data[signal];

    /* Get motor position - word access only */
    motorData = pack2x16(pmotorData->cmndPos);

    if (motorData == motor_info->position)
	motor_info->no_motion_count++;
    else
    {
	motor_info->position = motorData;
	motor_info->no_motion_count = 0;
    }

    if (motor_info->no_motion_count > motionTO)
    {
	motor_info->status |= RA_PROBLEM;
	send_mess(card, AXIS_STOP, oms58_axis[signal]);
	motor_info->no_motion_count = 0;
	errlogSevPrintf(errlogMinor, "Motor motion timeout ERROR on card: %d, signal: %d\n",
	    card, signal);
    }
    else
	motor_info->status &= ~RA_PROBLEM;

    /* get command velocity - word access only */
    motorData = pack2x16(pmotorData->cmndVel);

    motor_info->velocity = motorData;

    if (!(motor_info->status & RA_DIRECTION))
	motor_info->velocity *= -1;

    /* Get encoder position */
    motorData = pack2x16(pmotorData->encPos);

    motor_info->encoder_position = motorData;

    if ((nodeptr != NULL) && ((motor_info->status & RA_OVERTRAVEL) == 0))
    {
	struct motor_trans *trans = (struct motor_trans *) nodeptr->mrecord->dpvt;
	if (trans->dpm == ON)
	{
	    unsigned char bitselect;
	    unsigned char inputs = pmotor->control.ioLowReg;
	    bitselect = (1 << signal);
	    if ((inputs & bitselect) == 0)
	    {
                motor_info->status |= RA_OVERTRAVEL;
		logMsg((char *) "Drive power failure at VME58 card#%d motor#%d\n",
		       card, signal, 0, 0, 0, 0);
	    }
	}
    }

    rtn_state = (!motor_info->no_motion_count ||
     (motor_info->status & (RA_OVERTRAVEL | RA_DONE | RA_PROBLEM))) ? 1 : 0;
    
    /* Test for post-move string. */
    if ((motor_info->status & RA_DONE || motor_info->status & RA_OVERTRAVEL) &&
	(nodeptr != 0) &&
	(nodeptr->postmsgptr != 0))
    {
	strcpy(outbuf, nodeptr->postmsgptr);
	send_mess(card, outbuf, oms58_axis[signal]);
	nodeptr->postmsgptr = NULL;
    }

    return (rtn_state);
}


/*****************************************************/
/* send a message to the OMS board		     */
/* send_mess()			     */
/*****************************************************/
STATIC int send_mess(int card, char const *com, char inchar)
{
    volatile struct vmex_motor *pmotor;
    int16_t putIndex;
    int16_t deltaIndex;
    char outbuf[MAX_MSG_SIZE], *p;
    int return_code;

    if (strlen(com) > MAX_MSG_SIZE)
    {
	logMsg((char *) "drvOms58.cc:send_mess(); message size violation.\n",
	       0, 0, 0, 0, 0, 0);
	return (-1);
    }

    /* Check that card exists */
    if (!motor_state[card])
    {
	logMsg((char *) "drvOms58.cc:send_mess() - invalid card #%d\n", card,
	       0, 0, 0, 0, 0);
	return (-1);
    }

    pmotor = (struct vmex_motor *) motor_state[card]->localaddr;
    Debug(9, "send_mess: pmotor = %x\n", (uint_t) pmotor);

    return_code = 0;

    Debug(9, "send_mess: checking card %d status\n", card);

/* jps: Command error handling has been moved to motorIsr() */
/* Debug(1, "Command error detected! 0x%02x on send_mess\n", status.All); */

    /* see if junk at input port - should not be any data available */
    if (pmotor->inGetIndex != pmotor->inPutIndex)
    {
	Debug(1, "send_mess - clearing data in buffer\n");
	recv_mess(card, NULL, -1);
    }


    if (inchar == NULL)
	strcpy(outbuf, com);
    else
    {
	strcpy(outbuf, "A? ");
	outbuf[1] = inchar;
	strcat(outbuf, com);
    }
    strcat(outbuf, "\n");		/* Add the command line terminator. */

    Debug(9, "send_mess: ready to send message.\n");
    putIndex = pmotor->outPutIndex;
    for (p = outbuf; *p != '\0'; p++)
    {
	pmotor->outBuffer[putIndex++] = *p;
	if (putIndex >= BUFFER_SIZE)
	    putIndex = 0;
    }

    Debug(4, "send_mess: sent card %d message:", card);
    Debug(4, "%s\n", outbuf);

    pmotor->outPutIndex = putIndex;	/* Message Sent */

    while (pmotor->outPutIndex != pmotor->outGetIndex)
    {
	Debug(5, "send_mess: Waiting for ack: index delta=%d\n",
	   (((deltaIndex = pmotor->outPutIndex - pmotor->outGetIndex) < 0) ?
	    BUFFER_SIZE + deltaIndex : deltaIndex));
	/* after position command - latency = 4ms */
	oms_nanoSleep(cmndBuffReadyWait);
    };

    return (return_code);
}

/*
 * FUNCTION... recv_mess(int card, char *com, int amount)
 *
 * INPUT ARGUMENTS...
 *	card - 
 *	*com -
 *	amount - 
 *
 * LOGIC...
 *  IF controller card does not exist.
 *	ERROR Exit.
 *  ENDIF
 *  IF "amount" indicates buffer flush.
 *	WHILE characters left in input buffer.
 *	    Remove characters from controller's input buffer.
 *	ENDWHILE
 *	NORMAL RETURN.
 *  ENDIF
 *
 *  FOR each message requested (i.e. "amount").
 *	Initialize head and tail pointers.
 *	Initialize local buffer "get" index.
 *	FOR
 *	    IF characters left in controller's input buffer.
 *		
 *	    ENDIF
 *	ENDFOR
 *  ENDFOR
 *  
 */
STATIC int recv_mess(int card, char *com, int amount)
{
    volatile struct vmex_motor *pmotor;
    int16_t getIndex;
    int i, trys;
    char junk;
    unsigned char inchar;
    int piece, head_size, tail_size;

    /* Check that card exists */
    if (!motor_state[card])
    {
	Debug(1, "resv_mess - invalid card #%d\n", card);
	return (-1);
    }

    pmotor = (struct vmex_motor *) motor_state[card]->localaddr;

    if (amount == -1)
    {
	Debug(7, "-------------");
	getIndex = pmotor->inGetIndex;
	for (i = 0, trys = 0; trys < max_io_tries; trys++)
	{
	    while (getIndex != pmotor->inPutIndex)
	    {
		junk = pmotor->inBuffer[getIndex++];
		/* handle circular buffer */
		if (getIndex >= BUFFER_SIZE)
		    getIndex = 0;

		Debug(7, "%c", junk);
		trys = 0;
		i++;
	    }
	}
	pmotor->inGetIndex = getIndex;

	Debug(7, "-------------");
	Debug(1, "\nrecv_mess - cleared %d error data\n", i);
	return (0);
    }

    for (i = 0; amount > 0; amount--)
    {
	Debug(7, "-------------");
	head_size = 0;
	tail_size = 0;

	getIndex = pmotor->inGetIndex;

	for (piece = 0, trys = 0; piece < 3 && trys < max_io_tries; trys++)
	{
	    if (getIndex != pmotor->inPutIndex)
	    {
		inchar = pmotor->inBuffer[getIndex++];
		if (getIndex >= BUFFER_SIZE)
		    getIndex = 0;

		Debug(7, "%02x", inchar);

		switch (piece)
		{
		case 0:	/* header */
		    if (inchar == '\n' || inchar == '\r')
			head_size++;
		    else
		    {
			piece++;
			com[i++] = inchar;
		    }
		    break;
		case 1:	/* body */
		    if (inchar == '\n' || inchar == '\r')
		    {
			piece++;
			tail_size++;
		    }
		    else
			com[i++] = inchar;
		    break;

		case 2:	/* trailer */
		    tail_size++;
		    if (tail_size >= head_size)
			piece++;
		    break;
		}
		trys = 0;
	    }
	}
	pmotor->inGetIndex = getIndex;

	Debug(7, "-------------\n");
	if (trys >= max_io_tries)
	{
	    Debug(1, "Timeout occurred in recv_mess\n");
	    com[i] = '\0';

	    /* ----------------extra junk-------------- */
	    /* jps: command error now cleared by motorIsr() */
	    /* ----------------extra junk-------------- */

	    return (-1);
	}
	com[i++] = ',';
    }

    if (i > 0)
	com[i - 1] = '\0';
    else
	com[i] = '\0';

    Debug(4, "recv_mess: card %d", card);
    Debug(4, " com %s\n", com);
    return (0);
}


/*****************************************************/
/* Configuration function for  module_types data     */
/* areas. omsSetup()                                */
/*****************************************************/
int oms58Setup(int num_cards,	/* maximum number of cards in rack */
	       int num_channels,/* Not used - Channels per card (4 or 8) */
	       void *addrs,	/* Base Address(0x0-0xb000 on 4K boundary) */
	       unsigned vector,	/* noninterrupting(0), valid vectors(64-255) */
	       int int_level,	/* interrupt level (1-6) */
	       int scan_rate)	/* polling rate - 1/60 sec units */
{
    if (num_cards < 1 || num_cards > OMS_NUM_CARDS)
	oms58_num_cards = OMS_NUM_CARDS;
    else
	oms58_num_cards = num_cards;

    /* Check range and boundary(4K) on base address */
    if (addrs > (void *) 0xF000 || ((uint32_t) addrs & 0xFFF))
    {
	Debug(1, "omsSetup: invalid base address 0x%X\n", (uint_t) addrs);
	oms_addrs = (char *) OMS_NUM_ADDRS;
    }
    else
	oms_addrs = (char *) addrs;

    omsInterruptVector = vector;
    if (vector < 64 || vector > 255)
    {
	if (vector != 0)
	{
	    Debug(1, "omsSetup: invalid interrupt vector %d\n", vector);
	    omsInterruptVector = (unsigned) OMS_INT_VECTOR;
	}
    }

    if (int_level < 1 || int_level > 6)
    {
	Debug(1, "omsSetup: invalid interrupt level %d\n", int_level);
	omsInterruptLevel = OMS_INT_LEVEL;
    }
    else
	omsInterruptLevel = int_level;

    /* Set motor polling task rate */
    if (scan_rate >= 1 && scan_rate <= sysClkRateGet())
	motor_scan_rate = sysClkRateGet() / scan_rate;
    else
	motor_scan_rate = SCAN_RATE;
    return(0);
}


/*****************************************************/
/* Interrupt service routine.                        */
/* motorIsr()		                     */
/*****************************************************/
STATIC void motorIsr(int card)
{
    volatile struct controller *pmotorState;
    volatile struct vmex_motor *pmotor;
    STATUS_REG statusBuf;
    uint8_t doneFlags, userIO, slipFlags, limitFlags, cntrlReg;

    if (card >= total_cards || (pmotorState = motor_state[card]) == NULL)
    {
	logMsg((char *) "Invalid entry-card #%d\n", card, 0, 0, 0, 0, 0);
	return;
    }

    pmotor = (struct vmex_motor *) (pmotorState->localaddr);

    /* Status register - clear irqs on read. */
    statusBuf.All = pmotor->control.statusReg;

    /* Done register - clears on read */
    doneFlags = pmotor->control.doneReg;

    /* UserIO register - clears on read */
    userIO = pmotor->control.ioLowReg;

/* Questioniable Fix for undefined problem Starts Here. The following
 * code is meant to fix a problem that exhibted "spurious" interrupts
 * and "stuck in the Oms ISR" behavior.
*/
    /* Slip detection  - clear on read */
    slipFlags = pmotor->control.slipReg;

    /* Overtravel - clear on read */
    limitFlags = pmotor->control.limitReg;

    /* Only write control register if update bit is OFF. */
    /* Assure proper control register settings  */
    cntrlReg = pmotor->control.cntrlReg;
    if ((cntrlReg & 0x01) == 0)
	pmotor->control.cntrlReg = (uint8_t) 0x90;
/* Questioniable Fix for undefined problem Ends Here. */

    if (drvOms58debug >= 10)
	logMsg((char *) "entry card #%d,status=0x%X,done=0x%X\n", card,
	       statusBuf.All, doneFlags, 0, 0, 0);

    /* Motion done handling */
    if (statusBuf.Bits.done)
	/* Wake up polling task 'motor_task()' to issue callbacks */
	semGive(motor_sem);

    if (statusBuf.Bits.cmndError)
	logMsg((char *) "command error detected by motorISR() on card %d\n",
	       card, 0, 0, 0, 0, 0);
}

STATIC int motorIsrSetup(int card)
{
    volatile struct vmex_motor *pmotor;
    long status;
    CNTRL_REG cntrlBuf;

    Debug(5, "motorIsrSetup: Entry card#%d\n", card);

    pmotor = (struct vmex_motor *) (motor_state[card]->localaddr);

    status = devConnectInterrupt(intVME, omsInterruptVector + card,
		    (void (*)(void *)) motorIsr, (void *) card);
    if (!RTN_SUCCESS(status))
    {
	errPrintf(status, __FILE__, __LINE__, "Can't connect to vector %d\n", omsInterruptVector + card);
	omsInterruptVector = 0;	/* Disable interrupts */
	cntrlBuf.All = 0;
	pmotor->control.cntrlReg = cntrlBuf.All;
	return (ERROR);
    }

    status = devEnableInterruptLevel(OMS_INTERRUPT_TYPE,
				     omsInterruptLevel);
    if (!RTN_SUCCESS(status))
    {
	errPrintf(status, __FILE__, __LINE__, "Can't enable enterrupt level %d\n", omsInterruptLevel);
	omsInterruptVector = 0;	/* Disable interrupts */
	cntrlBuf.All = 0;
	pmotor->control.cntrlReg = cntrlBuf.All;
	return (ERROR);
    }

    /* Setup card for interrupt-on-done */
    pmotor->control.intVector = omsInterruptVector + card;

    /* enable interrupt-when-done irq */
    cntrlBuf.All = 0;
    cntrlBuf.Bits.doneIntEna = 1;
    cntrlBuf.Bits.intReqEna = 1;

    pmotor->control.cntrlReg = cntrlBuf.All;
    return (OK);
}

/*****************************************************/
/* initialize all software and hardware		     */
/* motor_init()			     */
/*****************************************************/
STATIC int motor_init()
{
    volatile struct controller *pmotorState;
    volatile struct vmex_motor *pmotor;
    STATUS_REG statusReg;
    int8_t omsReg;
    long status;
    int card_index, motor_index, arg3, arg4;
    char axis_pos[50], encoder_pos[50];
    char *tok_save, *pos_ptr;
    int total_encoders = 0, total_axis = 0, total_pidcnt = 0;
    volatile void *localaddr;
    void *probeAddr;
    
    tok_save = NULL;

    /* Check for setup */
    if (oms58_num_cards <= 0)
    {
	Debug(1, "motor_init: *OMS58 driver disabled* \n oms58Setup() is missing from startup script.\n");
	return (ERROR);
    }

    /* allocate space for total number of motors */
    motor_state = (struct controller **) malloc(oms58_num_cards *
						sizeof(struct controller *));

    /* allocate structure space for each motor present */

    total_cards = 0;

    if (rebootHookAdd((FUNCPTR) oms_reset) == ERROR)
	Debug(1, "vme58 motor_init: oms_reset disabled\n");

    for (card_index = 0; card_index < oms58_num_cards; card_index++)
    {
	int8_t *startAddr;
	int8_t *endAddr;

	Debug(2, "motor_init: card %d\n", card_index);

	probeAddr = (void *) (oms_addrs + (card_index * OMS_BRD_SIZE));
	startAddr = (int8_t *) probeAddr;
	endAddr = startAddr + OMS_BRD_SIZE;

	Debug(9, "motor_init: locationProbe() on addr 0x%x\n", (uint_t) probeAddr);
	/* Perform scan of vme58 memory space to assure card id */
	do
	{
	    status = locationProbe(OMS_ADDRS_TYPE, (char *) startAddr);
	    startAddr += 0x100;
	} while (PROBE_SUCCESS(status) && startAddr < endAddr);

	if (PROBE_SUCCESS(status))
	{
	    status = devRegisterAddress(__FILE__, OMS_ADDRS_TYPE,
					(void *) probeAddr, OMS_BRD_SIZE,
					(void **) &localaddr);
	    Debug(9, "motor_init: devRegisterAddress() status = %d\n", (int) status);
	    if (!RTN_SUCCESS(status))
	    {
		errPrintf(status, __FILE__, __LINE__, "Can't register address 0x%x\n", probeAddr);
		return (ERROR);
	    }

	    Debug(9, "motor_init: localaddr = %x\n", (uint_t) localaddr);
	    pmotor = (struct vmex_motor *) localaddr;

	    total_cards++;

	    Debug(9, "motor_init: malloc'ing motor_state\n");
	    motor_state[card_index] = (struct controller *) malloc(sizeof(struct controller));
	    pmotorState = motor_state[card_index];
	    pmotorState->localaddr = (char *) localaddr;
	    pmotorState->motor_in_motion = 0;
	    pmotorState->cmnd_response = OFF;

	    pmotorState->irqdata = (struct irqdatastr *) NULL;
	    /* Disable all interrupts */
	    pmotor->control.cntrlReg = 0;

	    send_mess(card_index, "EF", (char) NULL);
	    send_mess(card_index, ERROR_CLEAR, (char) NULL);
	    send_mess(card_index, STOP_ALL, (char) NULL);

	    send_mess(card_index, GET_IDENT, (char) NULL);
	    recv_mess(card_index, (char *) pmotorState->ident, 1);
	    Debug(3, "Identification = %s\n", pmotorState->ident);

	    send_mess(card_index, ALL_POS, (char) NULL);
	    recv_mess(card_index, axis_pos, 1);

	    for (total_axis = 0, pos_ptr = strtok_r(axis_pos, ",", &tok_save);
		 pos_ptr; pos_ptr = strtok_r(NULL, ",", &tok_save), total_axis++)
	    {
		pmotorState->motor_info[total_axis].motor_motion = NULL;
		pmotorState->motor_info[total_axis].status = 0;
	    }

	    Debug(3, "motor_init: Total axis = %d\n", total_axis);
	    pmotorState->total_axis = total_axis;

	    /* Assure done is cleared */
	    statusReg.All = pmotor->control.statusReg;
	    omsReg = pmotor->control.doneReg;
	    for (total_encoders = total_pidcnt = 0, motor_index = 0; motor_index < total_axis; motor_index++)
	    {
	    	/* Test if motor has an encoder. */
		send_mess(card_index, ENCODER_QUERY, oms58_axis[motor_index]);
		while (!pmotor->control.doneReg)	/* Wait for command to complete. */
		    oms_nanoSleep(1);

		statusReg.All = pmotor->control.statusReg;

		if (statusReg.Bits.cmndError)
		{
		    Debug(2, "motor_init: No encoder on axis %d\n", motor_index);
		    pmotorState->motor_info[motor_index].encoder_present = NO;
		}
		else
		{
		    total_encoders++;
		    pmotorState->motor_info[motor_index].encoder_present = YES;
		    recv_mess(card_index, encoder_pos, 1);
		}
		
		/* Test if motor has PID parameters. */
		send_mess(card_index, PID_QUERY, oms58_axis[motor_index]);
		do	/* Wait for command to complete. */
		{
		    taskDelay(1);
		    statusReg.All = pmotor->control.statusReg;
		} while(statusReg.Bits.done == 0);


		if (statusReg.Bits.cmndError)
		{
		    Debug(2, "motor_init: No PID parameters on axis %d\n", motor_index);
		    pmotorState->motor_info[motor_index].pid_present = NO;
		}
		else
		{
		    total_pidcnt++;
		    pmotorState->motor_info[motor_index].pid_present = YES;
		}
	    }

	    /* Enable interrupt-when-done if selected */
	    if (omsInterruptVector)
	    {
		if (motorIsrSetup(card_index) == ERROR)
		    errPrintf(0, __FILE__, __LINE__, "Interrupts Disabled!\n");
	    }

	    start_status(card_index);
	    for (motor_index = 0; motor_index < total_axis; motor_index++)
	    {
		pmotorState->motor_info[motor_index].status = 0;
		pmotorState->motor_info[motor_index].no_motion_count = 0;
		pmotorState->motor_info[motor_index].encoder_position = 0;
		pmotorState->motor_info[motor_index].position = 0;

		if (pmotorState->motor_info[motor_index].encoder_present == YES)
		    pmotorState->motor_info[motor_index].status |= EA_PRESENT;
		if (pmotorState->motor_info[motor_index].pid_present == YES)
		    pmotorState->motor_info[motor_index].status |= GAIN_SUPPORT;

		set_status(card_index, motor_index);

		send_mess(card_index, DONE_QUERY, oms58_axis[motor_index]); /* Is this needed??? */
		recv_mess(card_index, axis_pos, 1);
	    }

	    Debug(2, "motor_init: Init Address=0x%08.8x\n", (uint_t) localaddr);
	    Debug(3, "motor_init: Total encoders = %d\n", total_encoders);
	    Debug(3, "motor_init: Total with PID = %d\n", total_pidcnt);
	}
	else
	{
	    Debug(3, "motor_init: Card NOT found!\n");
	    motor_state[card_index] = (struct controller *) NULL;
	}
    }

    motor_sem = semBCreate(SEM_Q_PRIORITY, SEM_EMPTY);
    any_motor_in_motion = 0;

    FASTLOCKINIT(&queue_lock);
    FASTLOCK(&queue_lock);
    mess_queue.head = (struct mess_node *) NULL;
    mess_queue.tail = (struct mess_node *) NULL;
    FASTUNLOCK(&queue_lock);

    FASTLOCKINIT(&freelist_lock);
    FASTLOCK(&freelist_lock);
    free_list.head = (struct mess_node *) NULL;
    free_list.tail = (struct mess_node *) NULL;
    FASTUNLOCK(&freelist_lock);

    Debug(3, "Motors initialized\n");

    if (sizeof(int) >= sizeof(char *))
    {
	arg3 = (int) (&oms58_access);
	arg4 = 0;
    }
    else
    {
	arg3 = (int) ((long) &oms58_access >> 16);
	arg4 = (int) ((long) &oms58_access & 0xFFFF);
    }
    taskSpawn((char *) "Oms58_motor", 64, VX_FP_TASK | VX_STDIO, 5000, motor_task,
	      motor_scan_rate, arg3, arg4, 0, 0, 0, 0, 0, 0, 0);

    Debug(3, "Started motor_task\n");
    return (0);
}


/* Disables interrupts. Called on CTL X reboot. */

STATIC void oms_reset()
{
    short card;
    volatile struct vmex_motor *pmotor;
    short status;
    CNTRL_REG cntrlBuf;

    for (card = 0; card < total_cards; card++)
    {
	pmotor = (struct vmex_motor *) motor_state[card]->localaddr;
	if (vxMemProbe((char *) pmotor, READ, sizeof(short), (char *) &status) == OK)
	{
	    cntrlBuf.All = pmotor->control.cntrlReg;
	    cntrlBuf.Bits.intReqEna = 0;
	    pmotor->control.cntrlReg = cntrlBuf.All;
	}
    }
}

STATIC SEM_ID nanoSem;
STATIC int nanoTime;

/* Sleep for about 1ms - MVME167 vxWorks dependant */
STATIC void oms_nanoSleep(int time)
{
    if (FALSE)
/*     if (time > 0)  */
    {
	nanoSem = semBCreate(SEM_Q_PRIORITY, SEM_EMPTY);
	nanoTime = time;

	sysAuxClkDisable();
	sysAuxClkRateSet(omsNanoRate);	/* tick = 1ms */
	sysAuxClkConnect(&oms_nanoWakup, 0);
	sysAuxClkEnable();

	semTake(nanoSem, 1);	/* Maximum sleep time = 1 system clock tick */
	sysAuxClkDisable();
	semDelete(nanoSem);
    }
}

STATIC int oms_nanoWakup(int val)
{
    if (--nanoTime <= 0)
	semGive(nanoSem);
    return(0);
}


/*---------------------------------------------------------------------*/
