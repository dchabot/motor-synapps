/*
FILENAME...	serialIOMPF.cc
USAGE...	Interface between MPF and motor record device drivers.

Version:	$Revision: 1.10 $
Modified By:	$Author: rivers $
Last Modified:	$Date: 2003-09-02 05:10:01 $
*/

/*
 * These routines provide a simple interface to MPF serial I/O.  They hide 
 * the details of MPF They are intended for use by applications which do
 * simple synchronous reads and writes, and no control operations like setting
 * baud rates, etc.
 *
 * Author: Mark Rivers
 * Date:   4/24/98
 * Modifications:
 *         9/30/99   Converted to MPF
 * .01  01-23-02 RLS - Changed NODEBUG macro to DEBUG.
 *		     - Changed DEBUG macro to Debug.
 *		     - Increased timeout from 2 to 10 seconds in call to
 *			msgQReceive() from serialIO().  This prevents timeouts
 *			if a satellite CPU board is slow to boot.
 * .02 02-13-03 MLR  - Added serialIOSendRecv for atomic send/receive functions
 * .02 04-14-03 MLR  - Modified debugging, added timestamps to debug statements.
 * .03 06-11-03 RLS  - Converted to R3.14.x.
 *
 */

#include <string.h>
#include <epicsThread.h>
#include <epicsRingPointer.h>
#include "serialIO.h"
#include "ConnectMessage.h"
#include "Char8ArrayMessage.h"
#include "serialServer.h"

// Minimum wait for server reply; in 0.1 second increments.
#define MIN_MSGQ_WAIT (int) (1/0.1)


#ifdef __GNUG__
    #ifdef	DEBUG
	volatile int serialIODebug = 0;
	#define Debug(l, f, args...) {if (l <= serialIODebug) printf(f, ## args);}
    #else
	#define Debug(l, f, args...)
    #endif
#else
    #define Debug()
#endif


serialIO::serialIO(int card, char *serverName, bool *createdOK)
{
    int status, itera;
    Message *pmess;

    *createdOK = 1;
    // Create a message queue for the callback
    msgQId = new epicsRingPointer<void *>(4);
    Debug(5, "serialIOInit: message queue created, ID=%p\n", msgQId);
    pMessageClient = new MessageClient(serialIOCallback,(void *)this);
    Debug(5, "serialIOInit: message client created=%p\n", pMessageClient);
    status = pMessageClient->bind(serverName, card);
    if (status) {
        epicsPrintf("serialIOInit: Cannot bind to MPF server %s\n", serverName);
        *createdOK = 0;
    }
    else
        Debug(1, "serialIOInit: Bound to MPF server %s\n", serverName);
    
    // Wait for connect message to be received, 2 second timeout
    for (itera = 0; msgQId->isEmpty() == true; itera++)
    {
	epicsThreadSleep(0.1);
	if (itera >= MIN_MSGQ_WAIT)
	{
	    epicsPrintf("serialIO: error calling msgQReceive, status = %d\n", status);
	    *createdOK = 0;
	    return;
	}
    }

    pmess = (Message *) msgQId->pop();

    if (pmess->getType() != messageTypeConnect)
    {
        epicsPrintf("serialIO: incorrect message type received = %d\n", pmess->getType());
        *createdOK = 0;
    }
}

int serialIO::serialIOSend(char const *buffer, int buffer_len, int timeout)
{
    int status;
    Char8ArrayMessage *psm = new Char8ArrayMessage;
    Char8ArrayMessage *prm = NULL;
    Message *pmess;
    int itera, timeout_itera;

    psm->allocValue(buffer_len);
    psm->setSize(buffer_len);
    memcpy(psm->value, buffer, buffer_len);
    psm->cmd = cmdWrite;
    psm->timeout = timeout/1000;
    status = pMessageClient->send(psm);
    if (status)
    {
        Debug(1, "serialIOSend: error sending message %s\n", buffer);
        goto finish;
    }
    Debug(2, "serialIOSend: sent message %s\n", buffer);

    /* Wait for 2x the timeout or MIN_MSGQ_WAIT; which ever is greater. */
    timeout_itera = psm->timeout * 20;
    if (timeout_itera < MIN_MSGQ_WAIT)
	timeout_itera = MIN_MSGQ_WAIT;
       
    // Wait for response back from server
    for (itera = 0; msgQId->isEmpty() == true; itera++)
    {
	if (itera > timeout_itera)
	{
	    epicsPrintf("serialIOSend: error calling msgQReceive=%d\n", status);
	    goto finish;
	}
	epicsThreadSleep(0.1);
    }

    pmess = (Message *) msgQId->pop();
    Debug(5, "serialIOSend:  got message, pmess=%p\n", pmess);
    if (pmess->getType() == messageTypeChar8Array)
    {
	prm = (Char8ArrayMessage *)pmess;
	status = prm->status;
	if (status)
	    Debug(1, "serialIOSend: error receiving message, status=%d\n", status);
	Debug(4, "serialIOSend: received message, status=%d\n", status);
    }
    else
    {
	epicsPrintf("serialIOInit: incorrect message type received = %d\n",
		    pmess->getType());
    }

    delete prm;

finish:
    return (status);
}


int serialIO::serialIORecv(char *buffer, int buffer_len, char *terminator,
			   int timeout)
{
    int status, nrec = 0;
    Char8ArrayMessage *psm = new Char8ArrayMessage;
    Char8ArrayMessage *prm = NULL;
    Message *pmess;
    int itera, timeout_itera;

    psm->timeout = timeout/1000;
    // MPF uses seconds, not milliseconds for timeout.  If the desired timeout
    // is non-zero then use a minimum 1 second timeout
    if ((timeout > 0) && (psm->timeout < 1)) psm->timeout = 1;
    psm->cmd = cmdRead | cmdSetEom;
    if (terminator == NULL)
	psm->eomLen = 0;
    else
    {
	psm->eomLen = strlen(terminator);
	strcpy(&psm->eomString[0], terminator);
    }
    
    status = pMessageClient->send(psm);
    if (status != 0) {
        epicsPrintf("serialIORecv: error sending message, status = %d\n", 
                        status);
        goto done;
    }
    Debug(5, "serialIORecv: sent message status = %d, timeout=%d\n", status, timeout);

    /* Wait for 2x the timeout or MIN_MSGQ_WAIT; which ever is greater. */
    timeout_itera = psm->timeout * 20;
    if (timeout_itera < MIN_MSGQ_WAIT)
	timeout_itera = MIN_MSGQ_WAIT;
       
    for (itera = 0; msgQId->isEmpty() == true; itera++)
    {
	if (itera > timeout_itera)
	{
	    epicsPrintf("serialIORecv: error calling msgQReceive, status = %d\n", status);
	    goto done;
	}
	epicsThreadSleep(0.1);
    }

    pmess = (Message *) msgQId->pop();
    
    if (pmess->getType() != messageTypeChar8Array) {
        epicsPrintf("serialIOInit: incorrect message type received = %d\n",
                                pmess->getType());
        goto cleanup;
    }
    prm = (Char8ArrayMessage *)pmess;
    if (prm->status != 0) {
        Debug(1,"serialIORecv: error, return status = %d\n", prm->status);
        goto cleanup;
    }
    nrec = prm->getSize();
    if (nrec > buffer_len) nrec=buffer_len;
    memcpy(buffer, prm->value, nrec);
    Debug(2,"serialIORecv: Received %d bytes\n", nrec);
    // Append a NULL byte to the response if there is room
    if (nrec < buffer_len) buffer[nrec] = '\0';

    Debug(2,"serialIORecv: Received %d bytes, message = \n%s\n", nrec, buffer);
cleanup:
    delete prm;

done:
    return (nrec);
}

int serialIO::serialIOSendRecv(char const *outbuff, int outbuff_len, 
                                char *inbuff, int inbuff_len, char *terminator,
                                int timeout)
{
    int status, nrec = 0;
    Char8ArrayMessage *psm = new Char8ArrayMessage;
    Char8ArrayMessage *prm = NULL;
    Message *pmess;
    int itera, timeout_itera;

    psm->allocValue(outbuff_len);
    psm->setSize(outbuff_len);
    memcpy(psm->value, outbuff, outbuff_len);
    psm->cmd = cmdWriteRead | cmdFlush | cmdSetEom;
    psm->timeout = timeout/1000;
    // MPF uses seconds, not milliseconds for timeout.  If the desired timeout
    // is non-zero then use a minimum 1 second timeout
    if ((timeout > 0) && (psm->timeout < 1)) psm->timeout = 1;
    if (terminator == NULL)
	psm->eomLen = 0;
    else
    {
	psm->eomLen = strlen(terminator);
	strcpy(&psm->eomString[0], terminator);
    }

    status = pMessageClient->send(psm);

    if (status != 0) {
        epicsPrintf("serialIOSendRecv: error sending message, status = %d\n", 
                        status);
        goto done;
    }
    
    Debug(2, "serialIOSendRecv: sent: %s\n", outbuff);


    /* Wait for 2x the timeout or MIN_MSGQ_WAIT; which ever is greater. */
    timeout_itera = psm->timeout * 20;
    if (timeout_itera < MIN_MSGQ_WAIT)
	timeout_itera = MIN_MSGQ_WAIT;
       
    for (itera = 0; msgQId->isEmpty() == true; itera++)
    {
	if (itera > timeout_itera)
	{
	    epicsPrintf("serialIOSendRecv: error calling msgQReceive, status = %d\n", status);
	    goto done;
	}
	epicsThreadSleep(0.1);
    }   
    
    pmess = (Message *) msgQId->pop();
    
    if (pmess->getType() != messageTypeChar8Array) {
        epicsPrintf("serialIOSendRecv: incorrect message type received = %d\n",
                                pmess->getType());
        goto cleanup;
    }
    prm = (Char8ArrayMessage *)pmess;
    if (prm->status != 0) {
        Debug(1,"serialIOSendRecv: error, return status = %d\n", prm->status);
        goto cleanup;
    }
    nrec = prm->getSize();
    if (nrec > inbuff_len) nrec=inbuff_len;
    memcpy(inbuff, prm->value, nrec);
    // Append a NULL byte to the response if there is room
    if (nrec < inbuff_len) inbuff[nrec] = '\0';

    Debug(2,"serialIOSendRecv: received %d bytes: \n%s\n", nrec, inbuff);
cleanup:
    delete prm;

done:
    return (nrec);
}

void serialIO::serialIOCallback(Message *message, void *pointer)
{
    serialIO *psi = (serialIO *)pointer;
    bool rtnval;

    // If this is a Connect message or a Char8ArrayMessage then send it to
    // the message queue.

    if(message->getType()==messageTypeConnect) {
        ConnectMessage *pcm = (ConnectMessage *)message;
        if (pcm->status != connectYes) {
            Debug(1, "serialIOCallback: disconnect message?, status=%d\n", pcm->status);
            delete message;
            return;
        }
    }
    else if(message->getType()!=messageTypeChar8Array) {
        epicsPrintf("serialIOCallback: not Connect message or Char8ArrayMessage\n");
        delete message;
        return;
    }

    rtnval = psi->msgQId->push((void **) message);
    if (rtnval == false)
        epicsPrintf("serialIOCallback: error from msgQId->push\n");
}

extern "C"
{
void* serialIOInit(int card, char *serverName)
{
    bool createdOK;
    serialIO *psi = new serialIO(card, serverName, &createdOK);
    if (createdOK) {
        return( (void *)(psi) );
    } else {
        delete psi;
        return(NULL);
    }
}

int serialIOSend(serialIO *psi, char const *buffer,
		 int buffer_len, int timeout)
{
    return (psi->serialIOSend(buffer, buffer_len, timeout));
}

int serialIORecv(serialIO *psi, char *buffer, int buffer_len,
                    char *terminator, int timeout)
{
    return (psi->serialIORecv(buffer, buffer_len, terminator, timeout));
}

int serialIOSendRecv(serialIO *psi, char const *outbuff, int outbuff_len, 
                    char *inbuff, int inbuff_len,
                    char *terminator, int timeout)
{
    return (psi->serialIOSendRecv(outbuff, outbuff_len, inbuff, inbuff_len, terminator, timeout));
}

}
