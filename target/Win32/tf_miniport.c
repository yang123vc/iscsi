#include "../sock.h"
#include "../iscsi.h"
#include "../dcirlist.h"
#include "../iscsi_mem.h"
#include "../tf.h"
#include "../iscsi_param.h"
#include "../conf_reader.h"
#include "../iscsi_session.h"

#include <stdio.h>
#include <stdlib.h>

#include "../debug/iscsi_debug.h"

#include "wnaspi/scsidefs.h"

#include <windows.h>

#ifndef _MSC_VER
#include <ddk/ntddscsi.h>
#else
#include <ntddscsi.h>
#endif

#include <stddef.h>

#define NTDDK
#include "tf_miniport.h"

/* miniport TargetFilter */

/*************** for Windows XP/2000 ****************/

void *miniport_tfInit()
{
	DWORD dwVersion = GetVersion();

	if (dwVersion > 0x80000000)  {
		/* Windows 95/98/ME */
		return NULL;
	}
		
	return (void*)1;
}

void miniport_tfCleanup(void *handle)
{
}


#define FRAG_LENGTH	65536
struct fragData {
	uint32_t num;
	uint32_t next;

	char *origPointer;
	uint32_t origLen;
	
	char *newPointer;
	uint32_t newLen;
		
	uint32_t dataToPlay;

	uint32_t lba;
	uint32_t blockSize;
	uint16_t smBlkSize;	
};

static int makeFrag(const char *origCmd, struct fragData *fd, char *newCmd)
{
	uint32_t extra;
	
	if (fd->num == 0) { /* first Call */
		switch (origCmd[0]) {
		case 0x2a: /* WRITE (10) */
		case 0x28: /* READ (10) */
			fd->lba = ntohl(*(uint32_t *)&origCmd[2]);
			fd->smBlkSize = ntohs(*(uint16_t *)&origCmd[7]);

			fd->blockSize = fd->origLen / fd->smBlkSize;
			break;
		default:
			DEBUG("makeFrag unknown command!\n");
			return -1;
		}

		*(uint16_t *)&newCmd[7] = htons((uint16_t)(FRAG_LENGTH / fd->blockSize));

		fd->newPointer = fd->origPointer;
		fd->newLen = FRAG_LENGTH;
		fd->num++;
		fd->dataToPlay = fd->origLen - FRAG_LENGTH;

		fd->lba += FRAG_LENGTH / fd->blockSize;

		return 1;
	} else {
		extra = (fd->dataToPlay > FRAG_LENGTH) ? FRAG_LENGTH : fd->dataToPlay;
		*(uint32_t *)&newCmd[2] = htonl(fd->lba);

		*(uint16_t *)&newCmd[7] = htons( (uint16_t)(extra / fd->blockSize) );

		fd->newPointer += FRAG_LENGTH;
		fd->newLen = extra;

		if (extra < FRAG_LENGTH) {
			fd->dataToPlay = 0;
		} else {
			fd->dataToPlay -= FRAG_LENGTH;
		}

		if (fd->dataToPlay == 0) {
			return 0;
		}
		fd->lba += extra;
		return 1;
	}
}



static int fillIoctlTargetData(struct confElement *head, struct miniportData *md)
{
	struct confElement *scsiPort, *pathId, *lun, *targetId, *deviceName, *readOnly;

	scsiPort = getElemInt(head, IOCTL_SCSIPORT, &md->ScsiPortNumber);
	pathId = getElemInt(head, IOCTL_PATHID, &md->PathId);
	lun = getElemInt(head, IOCTL_LUN, &md->Lun);
	targetId = getElemInt(head, IOCTL_TARGETID, &md->TargetId);

	readOnly = getElemInt(head, IOCTL_READONLY, &md->ReadOnly);
	if (readOnly == NULL) {
		md->ReadOnly = 0;
	}

	deviceName = findElement(head->childs, IOCTL_DEVICENAME);

	if (deviceName) {
		strncpy(md->device, deviceName->value, DEVICE_LEN);
	} else {
		if ((scsiPort == NULL) || (pathId == NULL) || (lun == NULL) || (targetId == NULL)) {
			return -1;
		}
		snprintf(md->device, DEVICE_LEN, "\\\\.\\Scsi%d", md->ScsiPortNumber);
	}

	return 0;
}


void *miniport_tfAttach(void *handle, struct Session *ses)
{
	struct confElement *targetInfo = (struct confElement *)ses->params[ISP_TARGETNAME].pvalue;
	struct miniportData *md;
	BOOL status = 0;
	ULONG returned = 0;
	int res;

	md = (struct miniportData *)malloc(sizeof(struct miniportData));
	if (md == NULL) {
		return NULL;
	}

	memset(md, 0, sizeof(struct miniportData));

	res = fillIoctlTargetData(targetInfo, md);
	if (res == -1) {
		free(md);
		return NULL;
	}

	md->fileHandle = CreateFile(md->device, GENERIC_WRITE | GENERIC_READ, 
		FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);

    if (md->fileHandle == INVALID_HANDLE_VALUE) {
       DEBUG2("Error opening %s. Error: %ld\n", md->device, GetLastError());
	   free(md);
       return NULL;
    }
/*
    status = DeviceIoControl(md->fileHandle, IOCTL_SCSI_GET_INQUIRY_DATA, NULL, 0,
							md->buffer, sizeof(md->buffer), &returned, FALSE);
    if (!status) {
       DEBUG1( "Error reading inquiry data information; error was %d\n", GetLastError());
   	   free(md);
       return NULL;
    }
*/
    status = DeviceIoControl(md->fileHandle, IOCTL_SCSI_GET_CAPABILITIES, NULL, 0,
                             &md->capabilities, sizeof(IO_SCSI_CAPABILITIES), &returned, FALSE);
    if (!status ) {
       DEBUG1( "Error in io control; error was %ld\n", GetLastError() );
   	   free(md);
       return NULL;
    }

	return (void*)md;
}


void miniport_tfDetach(void *handle, void *prot)
{
	CloseHandle(((struct miniportData *)prot)->fileHandle);
	free(prot);
}


int miniport_tfSCSICommand(struct tfCommand *cmd)
{
	struct miniportData *md = (struct miniportData *)cmd->targetHandle;

    ULONG returned = 0;
	BOOL status = 0;
	ULONG length = sizeof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER);
	PSCSI_PASS_THROUGH_DIRECT_WITH_BUFFER ppdwb = (PSCSI_PASS_THROUGH_DIRECT_WITH_BUFFER)cmd->extra;

	int res;
	struct fragData fd;
	fd.num = 0;

	/* Cyrrently supported only one LUN */
	if (cmd->lun > 0) {
		cmd->response = 1;
		cmd->readedCount = 0;
		cmd->writedCount = 0;
		cmd->senseLen = 0;
		goto miniport_exit;
	}
#ifdef DEBUG
    printf(" [%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x::%02x:%02x:%02x:%02x:%02x:%02x:%02x:%02x] len=%d wc=%d rrc=%d\n",
            cmd->cdbBytes[0], cmd->cdbBytes[1], cmd->cdbBytes[2], cmd->cdbBytes[3],
            cmd->cdbBytes[4], cmd->cdbBytes[5], cmd->cdbBytes[6], cmd->cdbBytes[7],
            cmd->cdbBytes[8], cmd->cdbBytes[9], cmd->cdbBytes[10], cmd->cdbBytes[11],
            cmd->cdbBytes[12], cmd->cdbBytes[13], cmd->cdbBytes[14], cmd->cdbBytes[15],
            cmd->cdbLen,
            cmd->writeCount, cmd->residualReadCount);
#endif
	if (md->ReadOnly)	{
		/* Doing read only check */
		// Allowed commands
		switch (cmd->cdbBytes[0]) {
		case 0x18: //COPY
		case 0x3A: //COPY AND VERIFY
		case 0x07: //REASSIGN BLOCKS
		case 0x1D: //SEND DIAGNOSTICS
		case 0x33: //SET LIMITS
		case 0xB3: //SET LIMITS 12

		case 0x0A: //WRITE 6
		case 0x2A: //WRITE 10
		case 0xAA: //WRITE 12
		case 0x2E: //WRITE AND VERIFY 10
		case 0xAE: //WRITE AND VERIFY 12
		case 0x3B: //WRITE BUFFER
		case 0x3F: //WRITE LONG

		case 0x2C: //ERASE
		case 0xAC: //ERASE 12
		case 0x04: //FORMAT UNIT

/*
			cmd->writedCount = cmd->writeCount;
			cmd->readedCount = 0;
			cmd->senseLen = 0;
			cmd->response = 0;
			cmd->status = 0;
			goto miniport_exit;
*/

			/////////////////////////////////
			cmd->response = 0;
			cmd->readedCount = 0;
			cmd->writedCount = 0;
			cmd->senseLen = 14;

			cmd->status = 0x22; //???
			//cmd->status = 0x02; //???

			cmd->sense[0] = 0x70; // ASC = 0x27  ASCQ = 0x00
			cmd->sense[1] = 0;
			cmd->sense[2] = 0x07;// DATAPROT
			//cmd->sense[2] = 0x05;// DATAPROT
			cmd->sense[3] = 0;
			cmd->sense[4] = 0;
			cmd->sense[5] = 0;
			cmd->sense[6] = 0;
			cmd->sense[7] = 7;//len
			cmd->sense[8] = 0;
			cmd->sense[9] = 0;
			cmd->sense[10] = 0;
			cmd->sense[11] = 0;
			cmd->sense[12] = 0x27;//ASC
			//cmd->sense[12] = 0x20;//ASC
			cmd->sense[13] = 80;

			DEBUG1("FORBIDED command %x\n", cmd->cdbBytes[0]);
			goto miniport_exit;
		}

	}

    ppdwb->sptd.Length = sizeof(SCSI_PASS_THROUGH_DIRECT);
    
	ppdwb->sptd.PathId = md->PathId;
    ppdwb->sptd.TargetId = md->TargetId;
    ppdwb->sptd.Lun = md->Lun;

	ppdwb->sptd.SenseInfoLength = 24;
    ppdwb->sptd.TimeOutValue = 60;
    ppdwb->sptd.SenseInfoOffset =
       offsetof(SCSI_PASS_THROUGH_DIRECT_WITH_BUFFER,ucSenseBuf);

	memcpy(ppdwb->sptd.Cdb, cmd->cdbBytes, 16);

	switch ((ppdwb->sptd.Cdb[0] & 0xE0) >> 5) {	
	case 0:
		ppdwb->sptd.CdbLength = 6;
		break;
	case 1:
	case 2:
		ppdwb->sptd.CdbLength = 10;
		break;
	case 4:
		ppdwb->sptd.CdbLength = 12;
		break;
	case 5:
		ppdwb->sptd.CdbLength = 16;
		break;
	default:
		DEBUG1("miniport_tfSCSICommand UNKNOWN size for command %x\n", ppdwb->sptd.Cdb[0]);
		ppdwb->sptd.CdbLength = 16;
	}

    if ((cmd->writeCount > 0) && (cmd->residualReadCount > 0))
    {
        printf("Both read & write!!!\n");
    }

	if (cmd->writeCount > 0) {
	    ppdwb->sptd.DataIn = SCSI_IOCTL_DATA_OUT;
		ppdwb->sptd.DataBuffer = cmd->imWriteBuffer->data;
		ppdwb->sptd.DataTransferLength = cmd->writeCount;
	} else if (cmd->residualReadCount > 0) {
	    ppdwb->sptd.DataIn = SCSI_IOCTL_DATA_IN;
		ppdwb->sptd.DataBuffer = cmd->readBuffer->data;
		ppdwb->sptd.DataTransferLength = cmd->residualReadCount;
	} else {
	    ppdwb->sptd.DataIn = SCSI_IOCTL_DATA_IN;
		ppdwb->sptd.DataBuffer = NULL;
		ppdwb->sptd.DataTransferLength = 0;
	}

	/* FRAGMENTING IF NEEDED */
	if (ppdwb->sptd.DataTransferLength > FRAG_LENGTH) {
        DEBUG("Fragmentation\n");
		fd.origPointer = ppdwb->sptd.DataBuffer;
		fd.origLen = ppdwb->sptd.DataTransferLength;

nextSRB:
		res = makeFrag(cmd->cdbBytes, &fd, ppdwb->sptd.Cdb);

		ppdwb->sptd.DataBuffer = fd.newPointer;
		ppdwb->sptd.DataTransferLength = fd.newLen;
	} else {
		res = 0;
	}

	if (ppdwb->sptd.Cdb[0] == 0xa0) {
		cmd->response = 0;
		cmd->status = 0;
		cmd->readedCount = cmd->residualReadCount;
		cmd->writedCount = 0;
		cmd->senseLen = 0;
		memset(cmd->readBuffer->data, 0, cmd->residualReadCount);
		cmd->readBuffer->data[3] = 8;
		cmd->readBuffer->data[9] = 0x00;
		return sesSCSICmdResponse(cmd);
	}

    status = DeviceIoControl(md->fileHandle, IOCTL_SCSI_PASS_THROUGH_DIRECT,
                             ppdwb, length, ppdwb, length, &returned, FALSE);


	if ((fd.num > 0) && (res == 1)) {
		goto nextSRB;
	}

	if (status == TRUE) {
		cmd->response = 0;
		cmd->status = ppdwb->sptd.ScsiStatus;

		if (cmd->status == 0) {
			cmd->senseLen = 0;
		} else {
			cmd->senseLen = ppdwb->sptd.SenseInfoLength;
			memcpy (cmd->sense, ppdwb->ucSenseBuf, cmd->senseLen);
		}

		if ((cmd->writeCount > 0)) {
			cmd->writedCount = cmd->writeCount;
			cmd->readedCount = 0;
		} else if ((cmd->residualReadCount > 0) && (cmd->senseLen == 0) ) {
			cmd->readedCount = cmd->residualReadCount;
			cmd->writeCount = 0;
		} else {
			cmd->writeCount = 0;
			cmd->readedCount = 0;
		}	

		if ((md->ReadOnly) && (cmd->status == 0)) {
			if (ppdwb->sptd.Cdb[0] == 0x12) {
				if ((ppdwb->sptd.Cdb[1] & 1) == 0) {
					cmd->readBuffer->data[2] = 0x06;
					cmd->readBuffer->data[5] |= 0x01; //Setup PROTECT bit					
				} else if (ppdwb->sptd.Cdb[2] == 0x86) {
				//	__asm int 3;
				}
			}
		}


	} else {
		cmd->response = 1;
		cmd->readedCount = 0;
		cmd->writedCount = 0;
		cmd->senseLen = 0;

		DEBUG2("miniport_tfSCSICommand: DeviceIoControl returned 0 (%ld) CMD=0x%02x \n", GetLastError(),
			ppdwb->sptd.Cdb[0]);
	}

miniport_exit:
	return sesSCSICmdResponse(cmd);
}

