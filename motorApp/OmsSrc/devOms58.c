/*
FILENAME...	devOms58.c
USAGE...	Motor record device level support for OMS VME58.

Version:	$Revision: 1.2 $
Modified By:	$Author: sluiter $
Last Modified:	$Date: 2000-07-14 20:24:20 $
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
 * Modification Log:
 * -----------------
 * .01  01-18-93	jbk     initialized
 * .02  11-14-94	jps     copy devOMS.c and modify to point to vme58 driver
 * .03  03-19-96	tmm     v1.10: modified encoder-ratio calculation
 * .04  06-20-96	jps     allow for bumpless-reboot on position
 * .04a 02-19-97    	tmm     fixed for EPICS 3.13
 *      ...
 */


#include	<vxWorks.h>
#include        <semLib.h>	/* jps: include for init_record wait */
#include	<alarm.h>
#include	<callback.h>
#include	<dbDefs.h>
#include	<dbAccess.h>
#include	<dbCommon.h>
#include	<fast_lock.h>
#include	<devSup.h>
#include	<drvSup.h>
#ifdef __cplusplus
extern "C" {
#include	<recSup.h>
#include	<errlog.h>
}
#else
#include	<recSup.h>
#include	<errlog.h>
#endif

#include	"motorRecord.h"
#include	"motor.h"
#include	"drvOms58.h"
#include	"devOmsCom.h"

#define STATIC static

extern int oms58_num_cards;
extern struct driver_table oms58_access;

/* ----------------Create the dsets for devOMS----------------- */
STATIC long oms_init(int after);
STATIC long oms_init_record(struct motorRecord *);
STATIC long oms_start_trans(struct motorRecord *);
STATIC long oms_end_trans(struct motorRecord *);

struct motor_dset devOms58 =
{
    {8, NULL, oms_init, oms_init_record, NULL},
    motor_update_values,
    oms_start_trans,
    oms_build_trans,
    oms_end_trans
};


STATIC struct board_stat **oms_cards;
STATIC const char errmsg[] = {"\n\n!!!ERROR!!! - Oms58 driver uninitialized.\n"};

STATIC long oms_init(int after)
{
    if (*(oms58_access.init_indicator) == NO)
    {
	errlogSevPrintf(errlogMinor, "%s", errmsg);
	return(ERROR);
    }
    else
	return(motor_init_com(after, oms58_num_cards, &oms58_access, &oms_cards));
}

STATIC long oms_init_record(struct motorRecord *mr)
{
    return(motor_init_record_com(mr, oms58_num_cards, &oms58_access, oms_cards));
}

STATIC long oms_start_trans(struct motorRecord *mr)
{
    struct motor_trans *trans;
    long rtnval;
    
    rtnval = motor_start_trans_com(mr, oms_cards);
    /* Initialize a STOP_AXIS command termination string pointer. */
    trans = (struct motor_trans *) mr->dpvt;
    trans->motor_call.termstring = " ID";
    return(rtnval);
}

STATIC long oms_end_trans(struct motorRecord *mr)
{
    if (*(oms58_access.init_indicator) == NO)
    {
	errlogSevPrintf(errlogMinor, "%s", errmsg);
	return(ERROR);
    }
    else
	return(motor_end_trans_com(mr, &oms58_access));
}



