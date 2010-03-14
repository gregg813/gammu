#include "../../../gsmstate.h"

#ifdef GSM_ENABLE_NOKIA6510

#include <string.h>
#include <time.h>

#include "../../../misc/coding/coding.h"
#include "../../../gsmcomon.h"
#include "../../../service/gsmlogo.h"
#include "../nfunc.h"
#include "../nfuncold.h"
#include "../../pfunc.h"
#include "dct4func.h"
#include "n6510.h"

static GSM_Error N6510_Initialise (GSM_StateMachine *s)
{
	s->Phone.Data.Priv.N6510.CalendarIconsNum = 0;

	/* Enables various things like incoming SMS, call info, etc. */
	return N71_65_EnableFunctions (s, "\x01\x02\x06\x0A\x14\x17\x39", 7);
}

static GSM_Error N6510_ReplyGetMemory(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "Phonebook entry received\n");
	switch (msg.Buffer[6]) {
	case 0x0f:
		return N71_65_ReplyGetMemoryError(msg.Buffer[10], s);
	default:
		return N71_65_DecodePhonebook(s, s->Phone.Data.Memory, s->Phone.Data.Bitmap, s->Phone.Data.SpeedDial, msg.Buffer+22, msg.Length-22);
	}
	return GE_UNKNOWN;
}

static GSM_Error N6510_GetMemory (GSM_StateMachine *s, GSM_PhonebookEntry *entry)
{
	unsigned char req[] = {
		N6110_FRAME_HEADER, 0x07, 0x01, 0x01, 0x00, 0x01,
		0xfe, 0x10, 	/* memory type */
		0x00, 0x00, 0x00, 0x00, 
		0x00, 0x01, 	/* location */
		0x00, 0x00, 0x01};

	req[9] = NOKIA_GetMemoryType(s, entry->MemoryType,N71_65_MEMORY_TYPES);
	if (req[9]==0xff) return GE_NOTSUPPORTED;

	if (entry->Location==0x00) return GE_INVALIDLOCATION;

	req[14] = (entry->Location>>8);
	req[15] = entry->Location & 0xff;

	s->Phone.Data.Memory=entry;
	smprintf(s, "Getting phonebook entry\n");
	return GSM_WaitFor (s, req, 19, 0x03, 4, ID_GetMemory);
}

static GSM_Error N6510_ReplyGetMemoryStatus(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_Phone_Data *Data = &s->Phone.Data;

	smprintf(s, "Memory status received\n");
	/* Quess ;-)) */
	if (msg.Buffer[14]==0x10) {
		Data->MemoryStatus->Free = msg.Buffer[18]*256 + msg.Buffer[19];
	} else {
		Data->MemoryStatus->Free = msg.Buffer[17];
	}
	smprintf(s, "   Size       : %i\n",Data->MemoryStatus->Free);
	Data->MemoryStatus->Used = msg.Buffer[20]*256 + msg.Buffer[21];
	smprintf(s, "   Used       : %i\n",Data->MemoryStatus->Used);
	Data->MemoryStatus->Free -= Data->MemoryStatus->Used;
	smprintf(s, "   Free       : %i\n",Data->MemoryStatus->Free);
	return GE_NONE;
}

static GSM_Error N6510_GetMemoryStatus(GSM_StateMachine *s, GSM_MemoryStatus *Status)
{
	unsigned char req[] = {
		N6110_FRAME_HEADER, 0x03, 0x02,
		0x00,		/* MemoryType */
		0x55, 0x55, 0x55, 0x00};

	req[5] = NOKIA_GetMemoryType(s, Status->MemoryType,N71_65_MEMORY_TYPES);
	if (req[5]==0xff) return GE_NOTSUPPORTED;

	s->Phone.Data.MemoryStatus=Status;
	smprintf(s, "Getting memory status\n");
	return GSM_WaitFor (s, req, 10, 0x03, 4, ID_GetMemoryStatus);
}

static GSM_Error N6510_ReplyGetSMSC(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	int 			i, current, j;
	GSM_Phone_Data		*Data = &s->Phone.Data;

	switch (msg.Buffer[4]) {
		case 0x00:
			smprintf(s, "SMSC received\n");
			break;
		case 0x02:
			smprintf(s, "SMSC empty\n");
			return GE_INVALIDLOCATION;
		default:
			smprintf(s, "Unknown SMSC state: %02x\n",msg.Buffer[4]);
			return GE_UNKNOWNRESPONSE;
	}
	memset(Data->SMSC,0,sizeof(GSM_SMSC));
	Data->SMSC->Location 	= msg.Buffer[8];
	Data->SMSC->Format 	= GSMF_Text;
	switch (msg.Buffer[10]) {
		case 0x00: Data->SMSC->Format = GSMF_Text; 	break;
		case 0x22: Data->SMSC->Format = GSMF_Fax; 	break;
		case 0x26: Data->SMSC->Format = GSMF_Pager;	break;
		case 0x32: Data->SMSC->Format = GSMF_Email;	break;
	}
	Data->SMSC->Validity.VPF	= GSM_RelativeFormat;
	Data->SMSC->Validity.Relative	= msg.Buffer[12];
	current = 14;
	for (i=0;i<msg.Buffer[13];i++) {
		switch (msg.Buffer[current]) {
		case 0x81:
			j=current+4;
			while (msg.Buffer[j]!=0) {j++;}
			j=j-33;
			if (j>GSM_MAX_SMSC_NAME_LENGTH) {
				smprintf(s, "Too long name\n");
				return GE_UNKNOWNRESPONSE;
			}
			CopyUnicodeString(Data->SMSC->Name,msg.Buffer+current+4);
			smprintf(s, "   Name \"%s\"\n", DecodeUnicodeString(Data->SMSC->Name));
			break;
		case 0x82:
			switch (msg.Buffer[current+2]) {
			case 0x01:
				GSM_UnpackSemiOctetNumber(Data->SMSC->DefaultNumber,msg.Buffer+current+4,true);
				smprintf(s, "   Default number \"%s\"\n", DecodeUnicodeString(Data->SMSC->DefaultNumber));
				break;
			case 0x02:
				GSM_UnpackSemiOctetNumber(Data->SMSC->Number,msg.Buffer+current+4,false);
				smprintf(s, "   Number \"%s\"\n", DecodeUnicodeString(Data->SMSC->Number));
				break;
			default:
				smprintf(s, "Unknown SMSC number: %02x\n",msg.Buffer[current+2]);
				return GE_UNKNOWNRESPONSE;
			}
			break;
		default:
			smprintf(s, "Unknown SMSC block: %02x\n",msg.Buffer[current]);
			return GE_UNKNOWNRESPONSE;
		}
		current = current + msg.Buffer[current+1];
	}
	return GE_NONE;
}

static GSM_Error N6510_GetSMSC(GSM_StateMachine *s, GSM_SMSC *smsc)
{
	unsigned char req[] = {
		N6110_FRAME_HEADER, 0x14,
		0x01,		/* SMS Center Number. */
		0x00};

	if (smsc->Location==0x00) return GE_INVALIDLOCATION;
	
	req[4]=smsc->Location;

	s->Phone.Data.SMSC=smsc;
	smprintf(s, "Getting SMSC\n");
	return GSM_WaitFor (s, req, 6, 0x02, 4, ID_GetSMSC);
}

static GSM_Error N6510_ReplySetSMSC(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	switch (msg.Buffer[4]) {
		case 0x00:
			smprintf(s, "SMSC set OK\n");
			return GE_NONE;
		case 0x02:
			smprintf(s, "Invalid SMSC location\n");
			return GE_INVALIDLOCATION;
		default:
			smprintf(s, "Unknown SMSC state: %02x\n",msg.Buffer[4]);
			return GE_UNKNOWNRESPONSE;
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_SetSMSC(GSM_StateMachine *s, GSM_SMSC *smsc)
{
	int 		count = 13,i;
	unsigned char 	req[256] = {
		N6110_FRAME_HEADER,
		0x12, 0x55, 0x01, 0x0B, 0x34,
		0x05,		/* Location 	*/
		0x00,
		0x00,		/* Format 	*/
		0x00,
		0xFF};		/* Validity	*/

	req[8]  = smsc->Location;
	switch (smsc->Format) {
		case GSMF_Text:		req[10] = 0x00;	break;
		case GSMF_Fax:		req[10] = 0x22;	break;
		case GSMF_Pager:	req[10] = 0x26;	break;
		case GSMF_Email:	req[10] = 0x32;	break;
	}
	req[12]  = smsc->Validity.Relative;

	/* Magic. Nokia new ideas: coding data in the sequent blocks */
	req[count++] 	 	 = 0x03; 		/* Number of blocks */

	/* Block 2. SMSC Number */
	req[count++] 		 = 0x82; 		/* type: number */
	req[count++] 		 = 0x1A;		/* offset to next block starting from start of block */
	req[count++] 		 = 0x02; 		/* first number field => SMSC number */
	req[count] = GSM_PackSemiOctetNumber(smsc->Number, req+count+2, false) + 1;
	if (req[count]>18) {
		smprintf(s, "Too long SMSC number in frame\n");
		return GE_UNKNOWN;
	}
	req[count+1] = req[count] - 1;
	count += 23;

	/* Block 1. Default Number */
	req[count++] 		 = 0x82; 		/* type: number */
	req[count++] 		 = 0x14;		/* offset to next block starting from start of block */
	req[count++] 		 = 0x01; 		/* first number field => default number */
	req[count] = GSM_PackSemiOctetNumber(smsc->DefaultNumber, req+count+2, true) + 1;
	if (req[count]*2>12) {
		smprintf(s, "Too long SMSC number in frame\n");
		return GE_UNKNOWN;
	}
	req[count+1] = req[count] - 1;
	count += 17;

	/* Block 3. SMSC name */
	req[count++] 		 = 0x81;
	req[count++] 		 = UnicodeLength(smsc->Name)*2 + 2 + 4;
	req[count++] 		 = UnicodeLength(smsc->Name)*2 + 2;
	req[count++] 		 = 0x00;
	/* Can't make CopyUnicodeString(req+count,sms->Name) !!!!
	 * with MSVC6 count is changed then
	 */
	i = count;
	CopyUnicodeString(req+i,smsc->Name);
	count += UnicodeLength(smsc->Name)*2 + 2;
	
	smprintf(s, "Setting SMSC\n");
	return GSM_WaitFor (s, req, count, 0x02, 4, ID_SetSMSC);
}

static GSM_Error N6510_ReplyGetNetworkInfo(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	int		current = msg.Buffer[7]+7, tmp;
	GSM_Phone_Data	*Data = &s->Phone.Data;
#ifdef DEBUG
	char		name[100];
	GSM_NetworkInfo NetInfo;

	switch (msg.Buffer[8]) {
		case 0x00 : smprintf(s, "   Logged into home network.\n");		break;
		case 0x01 : smprintf(s, "   Logged into a roaming network.\n");		break;
		case 0x04 :
		case 0x09 : smprintf(s, "   Not logged in any network!");		break;
		default	  : smprintf(s, "   Unknown network status!\n");		break;
	}
	if (msg.Buffer[8]==0x00 || msg.Buffer[8] == 0x01) {
		tmp = 10;
		NOKIA_GetUnicodeString(s, &tmp, msg.Buffer,name,true);
		smprintf(s, "   Network name for phone    : %s\n",DecodeUnicodeString(name));
		NOKIA_DecodeNetworkCode(msg.Buffer + (current + 7),NetInfo.NetworkCode);
		sprintf(NetInfo.CellID, "%02x%02x", msg.Buffer[current+5], msg.Buffer[current+6]);
		sprintf(NetInfo.LAC,	"%02x%02x", msg.Buffer[current+1], msg.Buffer[current+2]);
		smprintf(s, "   CellID                    : %s\n", NetInfo.CellID);
		smprintf(s, "   LAC                       : %s\n", NetInfo.LAC);
		smprintf(s, "   Network code              : %s\n", NetInfo.NetworkCode);
		smprintf(s, "   Network name for Gammu    : %s ",
			DecodeUnicodeString(GSM_GetNetworkName(NetInfo.NetworkCode)));
		smprintf(s, "(%s)\n",DecodeUnicodeString(GSM_GetCountryName(NetInfo.NetworkCode)));
	}
#endif
	if (Data->RequestID==ID_GetNetworkInfo) {
		Data->NetworkInfo->NetworkName[0] = 0x00;
		Data->NetworkInfo->NetworkName[1] = 0x00;
		Data->NetworkInfo->State 	  = 0;
		switch (msg.Buffer[8]) {
			case 0x00: Data->NetworkInfo->State = GSM_HomeNetwork;		break;
			case 0x01: Data->NetworkInfo->State = GSM_RoamingNetwork;	break;
			case 0x04:
			case 0x09: Data->NetworkInfo->State = GSM_NoNetwork;		break;
		}
		if (Data->NetworkInfo->State == GSM_HomeNetwork || Data->NetworkInfo->State == GSM_RoamingNetwork) {
			tmp = 10;
			NOKIA_GetUnicodeString(s, &tmp, msg.Buffer,Data->NetworkInfo->NetworkName,true);
			sprintf(Data->NetworkInfo->CellID, "%02x%02x", 	msg.Buffer[current+5], msg.Buffer[current+6]);
			sprintf(Data->NetworkInfo->LAC,	"%02x%02x", 	msg.Buffer[current+1], msg.Buffer[current+2]);
			NOKIA_DecodeNetworkCode(msg.Buffer + (current+7),Data->NetworkInfo->NetworkCode);
		}
	}
	return GE_NONE;
}

static GSM_Error N6510_GetNetworkInfo(GSM_StateMachine *s, GSM_NetworkInfo *netinfo)
{
	unsigned char req[] = {N6110_FRAME_HEADER, 0x00, 0x00};

	s->Phone.Data.NetworkInfo=netinfo;
	smprintf(s, "Getting network info\n");
	return GSM_WaitFor (s, req, 5, 0x0a, 4, ID_GetNetworkInfo);
}

static GSM_Error N6510_EncodeSMSFrame(GSM_StateMachine *s, GSM_SMSMessage *sms, unsigned char *req, GSM_SMSMessageLayout *Layout, int *length)
{
	int			start, count = 0, pos1, pos2, pos3, pos4, pos5;
	GSM_Error		error;

	memset(Layout,255,sizeof(GSM_SMSMessageLayout));

	start			 = *length;

	req[count++]		 = 0x01;		/* one big block ? */
	if (sms->PDU != SMS_Deliver) {
		req[count++] 	 = 0x02;
	} else {
		req[count++] 	 = 0x00;
	}

	pos1		  	 = count; count++;
	Layout->firstbyte 	 = count; count++;	/* firstbyte set in SMS Layout */
	if (sms->PDU != SMS_Deliver) {
		Layout->TPMR 	 = count; count++;	/* ??? */
		Layout->TPPID	 = count; count++;
		Layout->TPDCS 	 = count; count++;	/* TP.DCS set in SMS layout */
                req[count++] 	 = 0x00;
	} else {
		Layout->TPPID 	 = count; count++;
		Layout->TPDCS 	 = count; count++;	/* TP.DCS set in SMS layout */
		Layout->DateTime = count; count += 7;
		req[count++] 	 = 0x55;
		req[count++] 	 = 0x55;
		req[count++] 	 = 0x55;
	}

	/* Magic. Nokia new ideas: coding SMS in the sequent blocks */
	if (sms->PDU != SMS_Deliver) {
		req[count++] 	 = 0x04; 		/* Number of blocks */
	} else {
		req[count++] 	 = 0x03; 		/* Number of blocks */
	}

	/* Block 1. Remote Number */
	req[count++] 		 = 0x82; 		/* type: number */
	req[count++] 		 = 0x10;		/* offset to next block starting from start of block (req[18]) */
	req[count++] 		 = 0x01; 		/* first number field => phone number */
	pos4 			 = count; count++;
	Layout->Number 		 = count; count+= 12; 	/* now coded Number in SMS Layout */

	/* Block 2. SMSC Number */
	req[count++] 		 = 0x82; 		/* type: number */
	req[count++] 		 = 0x10;		/* offset to next block starting from start of block (req[18]) */
	req[count++] 		 = 0x02; 		/* first number field => SMSC number */
	pos5 			 = count; count++;
	Layout->SMSCNumber 	 = count; count += 12; 	/* now coded SMSC number in SMS Layout */

	/* Block 3. Validity Period */
	if (sms->PDU != SMS_Deliver) {
		req[count++] 	 = 0x08; 		/* type: validity */
		req[count++] 	 = 0x04;
		req[count++] 	 = 0x01; 		/* data length */
		Layout->TPVP 	 = count; count++;
	}

	/* Block 4. User Data */
	req[count++] 		 = 0x80; 		/* type: User Data */
	pos2			 = count; count++; 				/* same as req[11] but starting from req[42] */
	pos3			 = count; count++;
	Layout->TPUDL 		 = count; count++; 	/* FIXME*/
	Layout->Text 		 = count;		/* SMS text and UDH coded in SMS Layout */

	error = PHONE_EncodeSMSFrame(s,sms,req,*Layout,length,false);
	if (error != GE_NONE) return error;

	req[pos1] 		 = *length - 1;
	req[pos2] 		 = *length - Layout->Text + 6;
	req[pos3] 		 = *length - Layout->Text;

	/* Convert number of semioctets to number of chars */
	req[pos4]		 = req[Layout->Number] + 4;
	if (req[pos4] % 2) req[pos4]++;
	req[pos4] /= 2;

	req[pos5]		 = req[Layout->SMSCNumber] + 1;

	if (req[pos4]>12 || req[pos5]>12) {
		smprintf(s, "Too long phone number in frame\n");
		return GE_UNKNOWN;
	}

	return GE_NONE;
}

static GSM_Error N6510_ReplyGetSMSFolders(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	int 			j, num = 0, pos;
	GSM_Phone_Data		*Data = &s->Phone.Data;

	switch (msg.Buffer[3]) {
	case 0x13:
		smprintf(s, "SMS folders names received\n");
		Data->SMSFolders->Number = msg.Buffer[5]+2;
		pos 			 = 6;
		for (j=0;j<msg.Buffer[5];j++) {
			while (true) {
				if (msg.Buffer[pos]   == msg.Buffer[6] &&
				    msg.Buffer[pos+1] == msg.Buffer[7]) break;
				if (pos+4 > msg.Length) return GE_UNKNOWNRESPONSE;
				pos++;
			}
			pos+=4;
			smprintf(s, "Folder index: %02x",msg.Buffer[pos - 2]);
			if (msg.Buffer[pos - 1]>GSM_MAX_SMS_FOLDER_NAME_LEN) {
				smprintf(s, "Too long text\n");
				return GE_UNKNOWNRESPONSE;
			}
			CopyUnicodeString(Data->SMSFolders->Folder[num].Name,msg.Buffer + pos);
			smprintf(s, ", folder name: \"%s\"\n",DecodeUnicodeString(Data->SMSFolders->Folder[num].Name));
			if (num == 0x00 || num == 0x02) {
				num++;
				CopyUnicodeString(Data->SMSFolders->Folder[num].Name,msg.Buffer + pos);
			}
			num++;
		}
		return GE_NONE;
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_GetSMSFolders(GSM_StateMachine *s, GSM_SMSFolders *folders)
{
	unsigned char req[] = {N6110_FRAME_HEADER, 0x12, 0x00, 0x00};

	s->Phone.Data.SMSFolders=folders;
	smprintf(s, "Getting SMS folders\n");
	return GSM_WaitFor (s, req, 6, 0x14, 4, ID_GetSMSFolders);
}

static GSM_Error N6510_ReplyGetSMSFolderStatus(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	int			i;
	GSM_Phone_N6510Data	*Priv = &s->Phone.Data.Priv.N6510;

	smprintf(s, "SMS folder status received\n");
	Priv->LastSMSFolder.Number=msg.Buffer[6]*256+msg.Buffer[7];
	smprintf(s, "Number of Entries: %i\n",Priv->LastSMSFolder.Number);
	smprintf(s, "Locations: ");
	for (i=0;i<Priv->LastSMSFolder.Number;i++) {
		Priv->LastSMSFolder.Location[i]=msg.Buffer[8+(i*2)]*256+msg.Buffer[(i*2)+9];
		smprintf(s, "%i ",Priv->LastSMSFolder.Location[i]);
	}
	smprintf(s, "\n");
	NOKIA_SortSMSFolderStatus(s, &Priv->LastSMSFolder);
	return GE_NONE;
}

static GSM_Error N6510_GetSMSFolderStatus(GSM_StateMachine *s, int folderid)
{
	unsigned char req[] = {
		N7110_FRAME_HEADER, 0x0C, 
		0x01,		/* 0x01 SIM, 0x02 ME 	*/
		0x00,		/* Folder ID		*/
		0x0f, 0x55, 0x55, 0x55};

	switch (folderid) {
		case 0x01: req[5] = 0x02; 			 break; /* INBOX SIM 	*/
		case 0x02: req[5] = 0x03; 			 break; /* OUTBOX SIM 	*/
		default	 : req[5] = folderid - 1; req[4] = 0x02; break; /* ME folders	*/
	}

	smprintf(s, "Getting SMS folder status\n");
	return GSM_WaitFor (s, req, 10, 0x14, 4, ID_GetSMSFolderStatus);
}

static void N6510_GetSMSLocation(GSM_StateMachine *s, GSM_SMSMessage *sms, unsigned char *folderid, int *location)
{
	int ifolderid;

	/* simulate flat SMS memory */
	if (sms->Folder==0x00) {
		ifolderid = sms->Location / PHONE_MAXSMSINFOLDER;
		*folderid = ifolderid + 0x01;
		*location = sms->Location - ifolderid * PHONE_MAXSMSINFOLDER;
	} else {
		*folderid = sms->Folder;
		*location = sms->Location;
	}
	smprintf(s, "SMS folder %i & location %i -> 6510 folder %i & location %i\n",
		sms->Folder,sms->Location,*folderid,*location);
}

static void N6510_SetSMSLocation(GSM_StateMachine *s, GSM_SMSMessage *sms, unsigned char folderid, int location)
{
	sms->Folder	= 0;
	sms->Location	= (folderid - 0x01) * PHONE_MAXSMSINFOLDER + location;
	smprintf(s, "6510 folder %i & location %i -> SMS folder %i & location %i\n",
		folderid,location,sms->Folder,sms->Location);
}

static GSM_Error N6510_DecodeSMSFrame(GSM_StateMachine *s, GSM_SMSMessage *sms, unsigned char *buffer)
{
	int 			i, current, blocks=0;
	GSM_SMSMessageLayout 	Layout;

	memset(&Layout,255,sizeof(GSM_SMSMessageLayout));
	Layout.firstbyte = 2;
	switch (buffer[0]) {
	case 0x00:
		smprintf(s, "SMS deliver\n");
		sms->PDU = SMS_Deliver;
		Layout.TPPID 	= 3;
		Layout.TPDCS 	= 4;
		Layout.DateTime = 5;
		blocks 		= 15;
		break;
	case 0x01:
		smprintf(s, "Delivery report\n");
		sms->PDU = SMS_Status_Report;
		Layout.TPStatus	= 4;
		Layout.DateTime = 5;
		Layout.SMSCTime = 12;
		blocks 		= 19;
		break;
	case 0x02:
		smprintf(s, "SMS template\n");
		sms->PDU = SMS_Submit;
		Layout.TPMR	= 3;
		Layout.TPPID 	= 4;
		Layout.TPDCS 	= 5;
		blocks 		= 7;
		break;
	}
	current = blocks + 1;
	for (i=0;i<buffer[blocks];i++) {
		switch (buffer[current]) {
			case 0x80:
				smprintf(s, "User data\n");
				if (buffer[current + 2] > buffer[current + 3]) {
					Layout.TPUDL 	= current + 2;
				} else {
					Layout.TPUDL 	= current + 3;
				}
				Layout.Text 		= current + 4;
				break;
			case 0x82:
				switch (buffer[current+2]) {
					case 0x01:
						smprintf(s, "Phone number\n");
						Layout.Number = current + 4;
						break;
					case 0x02:
						smprintf(s, "SMSC number\n");
						Layout.SMSCNumber = current + 4;
						break;
					default:
						smprintf(s, "Unknown number\n");
						break;
				}
				break;
			default:
				smprintf(s, "Unknown block %02x\n",buffer[current]);
		}
		current = current + buffer[current + 1];
	}
	return GSM_DecodeSMSFrame(sms,buffer,Layout);
}

static GSM_Error N6510_ReplyGetSMSMessage(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	int			i;
	int			Width, Height;
	unsigned char		output[500]; //output2[500];
	GSM_Phone_Data		*Data = &s->Phone.Data;

	switch(msg.Buffer[3]) {
	case 0x03:
		smprintf(s, "SMS Message received\n");
		Data->GetSMSMessage->Number=1;
		NOKIA_DecodeSMSState(s, msg.Buffer[5], &Data->GetSMSMessage->SMS[0]);
		switch (msg.Buffer[14]) {
		case 0x00:
		case 0x01:
		case 0x02:
			return N6510_DecodeSMSFrame(s, &Data->GetSMSMessage->SMS[0],msg.Buffer+14);
		case 0xA0:
			smprintf(s, "Picture Image\n");
			Data->GetSMSMessage->Number = 0;
			i = 0;
			output[i++] = 0x30;	 /* Smart Messaging 3.0 */
			output[i++] = SM30_OTA;
			output[i++] = 0x01;	 /* Length */
			output[i++] = 0x00;	 /* Length */
			output[i++] = 0x00;
			PHONE_GetBitmapWidthHeight(GSM_NokiaPictureImage, &Width, &Height);
			output[i++] = Width;
			output[i++] = Height;
			output[i++] = 0x01;
			memcpy(output+i,msg.Buffer+30,PHONE_GetBitmapSize(GSM_NokiaPictureImage,0,0));
			i = i + PHONE_GetBitmapSize(GSM_NokiaPictureImage,0,0);
//			if (msg.Length!=282) {
//				output[i++] = SM30_UNICODETEXT;
//				output[i++] = 0;
//				output[i++] = 0; /* Length - later changed */
//				GSM_UnpackEightBitsToSeven(0, msg.Length-282, msg.Length-304, msg.Buffer+282,output2);
//				DecodeDefault(output+i, output2, msg.Length - 282, true);
//				output[i - 1] = UnicodeLength(output+i) * 2;
//				i = i + output[i-1];
//			}
			GSM_MakeMultiPartSMS(Data->GetSMSMessage,output,i,UDH_NokiaProfileLong,GSM_Coding_8bit,1,0);
			for (i=0;i<3;i++) {
                		Data->GetSMSMessage->SMS[i].Number[0]=0;
                		Data->GetSMSMessage->SMS[i].Number[1]=0;
			}
			if (Data->Bitmap != NULL) {
				Data->Bitmap->Location	= 0;
				PHONE_GetBitmapWidthHeight(GSM_NokiaPictureImage, &Width, &Height);
				Data->Bitmap->Width	= Width;
				Data->Bitmap->Height	= Height;
				PHONE_DecodeBitmap(GSM_NokiaPictureImage, msg.Buffer + 30, Data->Bitmap);
				Data->Bitmap->Sender[0] = 0x00;
				Data->Bitmap->Sender[1] = 0x00;
				Data->Bitmap->Text[0] = 0;
				Data->Bitmap->Text[1] = 0;
			}
			return GE_NONE;
		default:
			smprintf(s, "Unknown SMS type: %i\n",msg.Buffer[8]);
		}
		break;
	case 0x0f:
		smprintf(s, "SMS message info received\n");
		CopyUnicodeString(Data->GetSMSMessage->SMS[0].Name,msg.Buffer+52);
		smprintf(s, "Name: \"%s\"\n",DecodeUnicodeString(Data->GetSMSMessage->SMS[0].Name));
		return GE_NONE;		
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_PrivGetSMSMessageBitmap(GSM_StateMachine *s, GSM_MultiSMSMessage *sms, GSM_Bitmap *bitmap)
{
	GSM_Error		error;
	unsigned char		folderid,namebuffer[200];
	int			location;
	int			i;
	unsigned char req[] = {
		N6110_FRAME_HEADER,
		0x02,		/* msg type: 0x02 for getting sms, 0x0e for sms status */
		0x01,		/* 0x01 SIM, 0x02 ME 	*/
		0x00, 		/* FolderID */
		0x00, 0x02,	/* Location */
		0x01, 0x00};

	N6510_GetSMSLocation(s, &sms->SMS[0], &folderid, &location);

	switch (folderid) {
		case 0x01: req[5] = 0x02; 			 break; /* INBOX SIM 	*/
		case 0x02: req[5] = 0x03; 			 break; /* OUTBOX SIM 	*/
		default	 : req[5] = folderid - 1; req[4] = 0x02; break; /* ME folders	*/
	}
	req[6]=location / 256;
	req[7]=location;

	s->Phone.Data.GetSMSMessage 	= sms;
	s->Phone.Data.Bitmap 		= bitmap;
	smprintf(s, "Getting sms message info\n");
	req[3] = 0x0e; req[8] = 0x55; req[9] = 0x55;
	error=GSM_WaitFor (s, req, 10, 0x14, 4, ID_GetSMSMessage);
	if (error!=GE_NONE) return error;
	CopyUnicodeString(namebuffer,sms->SMS[0].Name);

	smprintf(s, "Getting sms\n");
	req[3] = 0x02; req[8] = 0x01; req[9] = 0x00;
	error=GSM_WaitFor (s, req, 10, 0x14, 4, ID_GetSMSMessage);
	if (error==GE_NONE) {
		for (i=0;i<sms->Number;i++) {
			N6510_SetSMSLocation(s, &sms->SMS[i], folderid, location);
			sms->SMS[i].Folder 	= folderid;
			sms->SMS[i].InboxFolder = true;
			if (folderid != 0x01 && folderid != 0x02) sms->SMS[i].InboxFolder = false;
			CopyUnicodeString(sms->SMS[i].Name,namebuffer);
		}
	}
	return error;
}

static GSM_Error N6510_GetSMSMessage(GSM_StateMachine *s, GSM_MultiSMSMessage *sms)
{
	GSM_Error		error;
	unsigned char		folderid;
	int			location;
	GSM_Phone_N6510Data	*Priv = &s->Phone.Data.Priv.N6510;
	int			i;
	bool			found = false;

	N6510_GetSMSLocation(s, &sms->SMS[0], &folderid, &location);
	error=N6510_GetSMSFolderStatus(s, folderid);
	if (error!=GE_NONE) return error;
	for (i=0;i<Priv->LastSMSFolder.Number;i++) {
		if (Priv->LastSMSFolder.Location[i]==location) {
			found = true;
			break;
		}
	}
	if (!found) return GE_EMPTY;
	return N6510_PrivGetSMSMessageBitmap(s,sms,NULL);
}

static GSM_Error N6510_GetNextSMSMessageBitmap(GSM_StateMachine *s, GSM_MultiSMSMessage *sms, bool start, GSM_Bitmap *bitmap)
{
	GSM_Phone_N6510Data	*Priv = &s->Phone.Data.Priv.N6510;
	unsigned char		folderid;
	int			location;
	GSM_Error		error;
	int			i;
	bool			findnextfolder = false;

	if (start) {
		folderid	= 0x00;
		findnextfolder	= true;
		error=N6510_GetSMSFolders(s,&Priv->LastSMSFolders);
		if (error!=GE_NONE) return error;
	} else {
		N6510_GetSMSLocation(s, &sms->SMS[0], &folderid, &location);
		for (i=0;i<Priv->LastSMSFolder.Number;i++) {
			if (Priv->LastSMSFolder.Location[i]==location) break;
		}
		/* Is this last location in this folder ? */
		if (i==Priv->LastSMSFolder.Number-1) {
			findnextfolder=true;
		} else {
			location=Priv->LastSMSFolder.Location[i+1];
		}
	}
	if (findnextfolder) {
		Priv->LastSMSFolder.Number=0;
		while (Priv->LastSMSFolder.Number==0) {
			folderid++;
			/* Too high folder number */
			if ((folderid-1)>Priv->LastSMSFolders.Number) return GE_EMPTY;
			/* Get next folder status */
			error=N6510_GetSMSFolderStatus(s, folderid);
			if (error!=GE_NONE) return error;
			/* First location from this folder */
			location=Priv->LastSMSFolder.Location[0];
		}
	}
	N6510_SetSMSLocation(s, &sms->SMS[0], folderid, location);

	return N6510_PrivGetSMSMessageBitmap(s, sms, bitmap);
}

static GSM_Error N6510_GetNextSMSMessage(GSM_StateMachine *s, GSM_MultiSMSMessage *sms, bool start)
{
	return N6510_GetNextSMSMessageBitmap(s, sms, start, NULL);
}

static GSM_Error N6510_ReplyStartupNoteLogo(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_Phone_Data *Data = &s->Phone.Data;

	if (Data->RequestID == ID_GetBitmap) {
		switch (msg.Buffer[4]) {
		case 0x01:
			smprintf(s, "Welcome note text received\n");
			CopyUnicodeString(Data->Bitmap->Text,msg.Buffer+6);
			smprintf(s, "Text is \"%s\"\n",DecodeUnicodeString(Data->Bitmap->Text));
			return GE_NONE;
		case 0x10:
			smprintf(s, "Dealer note text received\n");
			CopyUnicodeString(Data->Bitmap->Text,msg.Buffer+6);
			smprintf(s, "Text is \"%s\"\n",DecodeUnicodeString(Data->Bitmap->Text));
			return GE_NONE;		
		case 0x0f:
			smprintf(s, "Startup logo received\n");
			PHONE_DecodeBitmap(GSM_Nokia7110StartupLogo, msg.Buffer + 22, Data->Bitmap);
			return GE_NONE;
		}
	}
	if (Data->RequestID == ID_SetBitmap) {
		switch (msg.Buffer[4]) {
			case 0x01:
			case 0x10:
			case 0x0f:
			case 0x25:
				return GE_NONE;
		}
	}
	return GE_UNKNOWN;
}

static GSM_Error N6510_GetPictureImage(GSM_StateMachine *s, GSM_Bitmap *Bitmap, int *location)
{
	GSM_MultiSMSMessage 	sms;
	int			Number;
	GSM_Bitmap		bitmap;
	GSM_Error		error;

	sms.SMS[0].Folder	= 0;
	Number			= 0;
	bitmap.Location		= 255;
	error=N6510_GetNextSMSMessageBitmap(s, &sms, true, &bitmap);
	while (error == GE_NONE) {
		if (bitmap.Location != 255) {
			Number++;
			if (Number == Bitmap->Location) {
				bitmap.Location = Bitmap->Location;
				memcpy(Bitmap,&bitmap,sizeof(GSM_Bitmap));
				*location = sms.SMS[0].Location;
				return GE_NONE;
			}
		}
		bitmap.Location		= 255;
		sms.SMS[0].Folder 	= 0;
		error=N6510_GetNextSMSMessageBitmap(s, &sms, false, &bitmap);
	}
	return GE_INVALIDLOCATION;
}

static GSM_Error N6510_GetBitmap(GSM_StateMachine *s, GSM_Bitmap *Bitmap)
{
	unsigned char reqOp	[] = {N6110_FRAME_HEADER, 0x23, 0x00, 0x00, 0x55, 0x55, 0x55};
	unsigned char reqStartup[] = {N6110_FRAME_HEADER, 0x02, 0x0f};
	unsigned char reqNote	[] = {N6110_FRAME_HEADER, 0x02, 0x01, 0x00};
	GSM_PhonebookEntry	pbk;
	GSM_Error		error;
	int			Location;

	s->Phone.Data.Bitmap=Bitmap;	
	switch (Bitmap->Type) {
	case GSM_StartupLogo:
		Bitmap->Width  = 96;
		Bitmap->Height = 65;
		GSM_ClearBitmap(Bitmap);
		smprintf(s, "Getting startup logo\n");
		return GSM_WaitFor (s, reqStartup, 5, 0x7A, 4, ID_GetBitmap);
	case GSM_DealerNoteText:
		reqNote[4] = 0x10;
		smprintf(s, "Getting dealer note\n");
		return GSM_WaitFor (s, reqNote, 6, 0x7A, 4, ID_GetBitmap);
	case GSM_WelcomeNoteText:
		smprintf(s, "Getting welcome note\n");
		return GSM_WaitFor (s, reqNote, 6, 0x7A, 4, ID_GetBitmap);
	case GSM_CallerLogo:
		if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_PBK35)) return GE_NOTSUPPORTED;
		Bitmap->Width  	 = 72;
		Bitmap->Height 	 = 14;
		GSM_ClearBitmap(Bitmap);
		pbk.MemoryType	= GMT7110_CG;
		pbk.Location	= Bitmap->Location;
		smprintf(s, "Getting caller group logo\n");
		error=N6510_GetMemory(s,&pbk);
		if (error==GE_NONE) NOKIA_GetDefaultCallerGroupName(s, Bitmap);
		return error;
	case GSM_OperatorLogo:
		smprintf(s, "Getting operator logo\n");
		return GSM_WaitFor (s, reqOp, 9, 0x0A, 4, ID_GetBitmap);
	case GSM_PictureImage:
		return N6510_GetPictureImage(s, Bitmap, &Location);
	default:
		break;
	}
	return GE_NOTSUPPORTED;
}

static GSM_Error N6510_ReplyGetIncSignalQuality(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "Network level changed to: %i\n",msg.Buffer[4]);
	return GE_NONE;
}

static GSM_Error N6510_ReplyGetSignalQuality(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_Phone_Data *Data = &s->Phone.Data;

	smprintf(s, "Network level received: %i\n",msg.Buffer[8]);
    	Data->SignalQuality->SignalStrength 	= -1;
    	Data->SignalQuality->SignalPercent 	= ((int)msg.Buffer[8]);
    	Data->SignalQuality->BitErrorRate 	= -1;
	return GE_NONE;
}

static GSM_Error N6510_GetSignalQuality(GSM_StateMachine *s, GSM_SignalQuality *sig)
{
	unsigned char req[] = {N6110_FRAME_HEADER, 0x0B, 0x00, 0x02, 0x00, 0x00, 0x00};

	s->Phone.Data.SignalQuality = sig;
	smprintf(s, "Getting network level\n");
	return GSM_WaitFor (s, req, 9, 0x0a, 4, ID_GetSignalQuality);
}

static GSM_Error N6510_ReplyGetBatteryCharge(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_Phone_Data *Data = &s->Phone.Data;

	smprintf(s, "Battery level received: %i\n",msg.Buffer[9]*100/7);
    	Data->BatteryCharge->BatteryPercent 	= ((int)(msg.Buffer[9]*100/7));
    	Data->BatteryCharge->ChargeState 	= 0;
	return GE_NONE;
}

static GSM_Error N6510_GetBatteryCharge(GSM_StateMachine *s, GSM_BatteryCharge *bat)
{
	unsigned char req[] = {N6110_FRAME_HEADER, 0x0A, 0x02, 0x00};

	s->Phone.Data.BatteryCharge = bat;
	smprintf(s, "Getting battery level\n");
	return GSM_WaitFor (s, req, 6, 0x17, 4, ID_GetBatteryCharge);
}

static GSM_Error N6510_ReplyGetWAPBookmark(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	return DCT3DCT4_ReplyGetWAPBookmark (msg, s, true);
}

static GSM_Error N6510_ReplyGetOperatorLogo(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_Phone_Data *Data = &s->Phone.Data;

	smprintf(s, "Operator logo received\n");
	NOKIA_DecodeNetworkCode(msg.Buffer+12,Data->Bitmap->NetworkCode);
	smprintf(s, "Network code %s\n",Data->Bitmap->NetworkCode);
	Data->Bitmap->Width	= msg.Buffer[20];
	Data->Bitmap->Height	= msg.Buffer[21];
	if (msg.Length == 18) return GE_EMPTY;
	PHONE_DecodeBitmap(GSM_Nokia6510OperatorLogo,msg.Buffer+26,Data->Bitmap);
	return GE_NONE;
}

GSM_Error N6510_ReplyDeleteMemory(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "Phonebook entry deleted\n");
	return GE_NONE;
}

GSM_Error N6510_DeleteMemory(GSM_StateMachine *s, GSM_PhonebookEntry *entry, unsigned char *memory)
{
	unsigned char req[] = {
		N7110_FRAME_HEADER, 0x0f, 0x55, 0x01,
		0x04, 0x55, 0x00, 0x10, 0xFF, 0x02,
		0x00, 0x01,		/* location	*/
		0x00, 0x00, 0x00, 0x00,
		0x05, 			/* memory type	*/
		0x55, 0x55, 0x55};

	req[12] = (entry->Location >> 8);
	req[13] = entry->Location & 0xff;

	req[18] = NOKIA_GetMemoryType(s, entry->MemoryType,memory);
	if (req[18]==0xff) return GE_NOTSUPPORTED;

	smprintf(s, "Deleting phonebook entry\n");
	return GSM_WaitFor (s, req, 22, 0x03, 4, ID_SetMemory);
}

static GSM_Error N6510_SetMemory(GSM_StateMachine *s, GSM_PhonebookEntry *entry)
{
	int 		count = 22, blocks;
	unsigned char 	req[500] = {
		N7110_FRAME_HEADER, 0x0b, 0x00, 0x01, 0x01, 0x00, 0x00, 0x10,
		0x02, 0x00,  /* memory type */
		0x00, 0x00,  /* location */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

	if (entry->Location == 0) return GE_NOTSUPPORTED;
	if (entry->EntriesNum!=0) {
		req[11] = NOKIA_GetMemoryType(s, entry->MemoryType,N71_65_MEMORY_TYPES);
		if (req[11]==0xff) return GE_NOTSUPPORTED;

		req[12] = (entry->Location >> 8);
		req[13] = entry->Location & 0xff;

		count = count + N71_65_EncodePhonebookFrame(s, req+22, *entry, &blocks, true, IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_VOICETAGS));
		req[21] = blocks;

		smprintf(s, "Writing phonebook entry\n");
		return GSM_WaitFor (s, req, count, 0x03, 4, ID_SetMemory);
	} else {
		return N6510_DeleteMemory(s, entry, N71_65_MEMORY_TYPES);
	}  
}

static GSM_Error N6510_ReplySetOperatorLogo(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "Operator logo set OK\n");
	return GE_NONE;
}

static GSM_Error N6510_SetCallerLogo(GSM_StateMachine *s, GSM_Bitmap *bitmap)
{
	unsigned char req[500] = {
		N6110_FRAME_HEADER, 0x0b, 0x00, 0x01, 0x01, 0x00, 0x00, 0x10,
		0xfe, 0x10,		/* memory type */
		0x00, 0x00,		/* location */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	char		string[500];
	int		block=0, i;
	unsigned int 	count = 22;
	int		Width, Height;

	req[13] = bitmap->Location;

	/* Logo on/off */
	string[0] = bitmap->Enabled?1:0;
	string[1] = 0;
	count += N71_65_PackPBKBlock(s, N7110_ENTRYTYPE_LOGOON, 2, block++, string, req + count);

	/* Ringtone */
	if (!bitmap->DefaultRingtone) {
		string[0] = 0x00; 
		string[1] = 0x00;
		string[2] = bitmap->Ringtone;
		count += N71_65_PackPBKBlock(s, N7110_ENTRYTYPE_RINGTONE, 3, block++, string, req + count);
		count --;
		req[count-5] = 8;
	}

	/* Number of group */
	string[0] = bitmap->Location;
	string[1] = 0;
	count += N71_65_PackPBKBlock(s, N7110_ENTRYTYPE_GROUP, 2, block++, string, req + count);

	/* Name */
	if (!bitmap->DefaultName) {
		i = UnicodeLength(bitmap->Text) * 2;
		string[0] = i + 2;
		memcpy(string + 1, bitmap->Text, i);
		string[i + 1] = 0;
		count += N71_65_PackPBKBlock(s, N7110_ENTRYTYPE_NAME, i + 2, block++, string, req + count);
	}

	/* Logo */
	if (!bitmap->DefaultBitmap) {
		PHONE_GetBitmapWidthHeight(GSM_NokiaCallerLogo, &Width, &Height);
		string[0] = Width;
		string[1] = Height;
		string[2] = 0;
		string[3] = 0;
		string[4] = PHONE_GetBitmapSize(GSM_NokiaCallerLogo,0,0);
		PHONE_EncodeBitmap(GSM_NokiaCallerLogo, string + 5, bitmap);
		count += N71_65_PackPBKBlock(s, N7110_ENTRYTYPE_GROUPLOGO, PHONE_GetBitmapSize(GSM_NokiaCallerLogo,0,0) + 5, block++, string, req + count);
	}

	req[21] = block;

	return GSM_WaitFor (s, req, count, 0x03, 4, ID_SetBitmap);
}

static GSM_Error N6510_ReplySetPicture(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
//	smprintf(s, "Picture Image written OK, folder %i, location %i\n",msg.Buffer[4],msg.Buffer[5]*256+msg.Buffer[6]);
	return GE_NONE;
}

static GSM_Error N6510_SetBitmap(GSM_StateMachine *s, GSM_Bitmap *Bitmap)
{
	GSM_SMSMessage		sms;
	GSM_Phone_Bitmap_Types	Type;
	int			Width, Height, i, count;
#ifdef DEVELOP
	unsigned char		folderid;
	int					location;
#endif
	GSM_NetworkInfo 	NetInfo;
	GSM_Error		error;
	unsigned char reqStartup[1000] = {
		N7110_FRAME_HEADER, 0x04, 0x0F,
		0x00, 0x00, 0x00,
		0x04, 0xC0, 0x02, 0x00,
		0x41, 0xC0, 0x03, 0x00,
		0x60, 0xC0, 0x04};
	unsigned char reqColourWallPaper[200] = {
		N6110_FRAME_HEADER, 0x07, 0x00, 0x00, 0x00, 0xD5,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x01, 0x00,
		0x18};		/* Bitmap ID */
	unsigned char reqColourStartup[200] = {
		N6110_FRAME_HEADER, 0x04, 0x25, 0x00, 0x01, 0x00, 0x18};
	unsigned char reqOp[1000] = {
		N7110_FRAME_HEADER, 0x25, 0x01,
		0x55, 0x00, 0x00, 0x55,
		0x01,			/* 0x01 - not set, 0x02 - set */
		0x0C, 0x08,
		0x62, 0xF0, 0x10,	/* Network code */
		0x03, 0x55, 0x55};
	unsigned char reqColourOp[200] = {
		N6110_FRAME_HEADER,
		0x07, 0x00, 0x00, 0x00, 0xE7, 0x00, 0x00, 0x00, 0xF9, 0x00,
		0x08, 0xFF, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x00,
		0x18,			/* File ID */
		0x00,
		0x00, 0x00, 0x00};	/* Network code */
	unsigned char reqNote[200] = {N6110_FRAME_HEADER, 0x04, 0x01};
	unsigned char reqPicture[2000] = {
		N6110_FRAME_HEADER, 0x00,
		0x02, 0x05,		/* SMS folder 	*/
		0x00, 0x00,		/* location 	*/
		0x01, 0x01, 0xa0, 0x02, 0x01, 0x40, 0x00, 0x34,
		0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x55, 0x55, 0x55, 0x03, 0x82, 0x10,
		0x01, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x82, 0x10,
		0x02, 0x0c, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x04,
		0x00, 0x00, 0xa1, 0x55, 0x01, 0x08, 0x00, 0x00,
		0x00, 0x01, 0x48, 0x1c, 0x00, 0xfc, 0x00};

	switch (Bitmap->Type) {
	case GSM_ColourWallPaper:
		reqColourWallPaper[21] = Bitmap->ID;
		smprintf(s, "Setting colour wall paper\n");
		return GSM_WaitFor (s, reqColourWallPaper, 22, 0x43, 4, ID_SetBitmap);
	case GSM_StartupLogo:
		Type = GSM_Nokia7110StartupLogo;
		switch (Bitmap->Location) {
			case 1:
				PHONE_EncodeBitmap(Type, reqStartup + 22, Bitmap);
				break;
			case 2:
				memset(reqStartup+5,0x00,15);
				PHONE_ClearBitmap(Type, reqStartup + 22,0,0);
				break;
			default:
				return GE_NOTSUPPORTED;
		}
		smprintf(s, "Setting startup logo\n");
		return GSM_WaitFor (s, reqStartup, 22+PHONE_GetBitmapSize(Type,0,0), 0x7A, 4, ID_SetBitmap);
	case GSM_DealerNoteText:
		reqNote[4] = 0x10;
		CopyUnicodeString(reqNote + 5, Bitmap->Text);
		i = 6 + UnicodeLength(Bitmap->Text) * 2;
		reqNote[i++] 	= 0;
		reqNote[i] 	= 0;
		return GSM_WaitFor (s, reqNote, i, 0x7A, 4, ID_SetBitmap);	
	case GSM_WelcomeNoteText:
		CopyUnicodeString(reqNote + 5, Bitmap->Text);
		i = 6 + UnicodeLength(Bitmap->Text) * 2;
		reqNote[i++] 	= 0;
		reqNote[i] 	= 0;
		return GSM_WaitFor (s, reqNote, i, 0x7A, 4, ID_SetBitmap);	
	case GSM_OperatorLogo:
		/* We want to set operator logo, not clear */
		if (strcmp(Bitmap->NetworkCode,"000 00")) {
			memset(reqOp + 19, 0, 281);
			NOKIA_EncodeNetworkCode(reqOp+12, Bitmap->NetworkCode);
			Type = GSM_Nokia6510OperatorLogo;
			reqOp[9]  = 0x02;	/* Logo enabled */
			reqOp[18] = 0x1a;	/* FIXME */
			reqOp[19] = PHONE_GetBitmapSize(Type,0,0) + 8 + 29 + 2;
			PHONE_GetBitmapWidthHeight(Type, &Width, &Height);
			reqOp[20] = Width;
			reqOp[21] = Height;
			reqOp[22] = 0x00;
			reqOp[23] = PHONE_GetBitmapSize(Type,0,0) + 29;
			reqOp[24] = 0x00;
			reqOp[25] = PHONE_GetBitmapSize(Type,0,0) + 29;
			PHONE_EncodeBitmap(Type, reqOp + 26, Bitmap);
			smprintf(s, "Setting operator logo\n");
			return GSM_WaitFor (s, reqOp, reqOp[19]+reqOp[11]+10, 0x0A, 4, ID_SetBitmap);
		} else {
			error=N6510_GetNetworkInfo(s,&NetInfo);
			if (error != GE_NONE) return error;
			NOKIA_EncodeNetworkCode(reqOp+12, NetInfo.NetworkCode);
			smprintf(s, "Clearing operator logo\n");
			return GSM_WaitFor (s, reqOp, 18, 0x0A, 4, ID_SetBitmap);
		}
	case GSM_ColourOperatorLogo:
		/* We want to set operator logo, not clear */
		if (strcmp(Bitmap->NetworkCode,"000 00")) {
			EncodeBCD(reqColourOp+23, Bitmap->NetworkCode, 6, false);
			reqColourOp[21] = Bitmap->ID;
		}
		smprintf(s, "Setting colour operator logo\n");
		return GSM_WaitFor (s, reqColourOp, 26, 0x43, 4, ID_SetBitmap);
	case GSM_ColourStartupLogo:
		switch (Bitmap->Location) {
			case 0:
				reqColourStartup[6] = 0x00;
				reqColourStartup[8] = 0x00;
				smprintf(s, "Setting colour startup logo\n");
				return GSM_WaitFor (s, reqColourStartup, 9, 0x7A, 4, ID_SetBitmap);
			case 1:
				reqColourStartup[8] = Bitmap->ID;
				smprintf(s, "Setting colour startup logo\n");
				return GSM_WaitFor (s, reqColourStartup, 9, 0x7A, 4, ID_SetBitmap);
			default:
				return GE_NOTSUPPORTED;
		}
	case GSM_CallerLogo:
		return N6510_SetCallerLogo(s,Bitmap);
	case GSM_PictureImage:
		error = N6510_GetPictureImage(s, Bitmap, &sms.Location);
		if (error == GE_NONE) {
#ifdef DEVELOP
			sms.Folder = 0;
			N6510_GetSMSLocation(s, &sms, &folderid, &location);
			switch (folderid) {
				case 0x01: reqPicture[5] = 0x02; 				break; /* INBOX SIM 	*/
				case 0x02: reqPicture[5] = 0x03; 				break; /* OUTBOX SIM 	*/
				default	 : reqPicture[5] = folderid - 1; reqPicture[4] = 0x02; 	break; /* ME folders	*/
			}
			reqPicture[6]=location / 256;
			reqPicture[7]=location;
#else
			return GE_NOTSUPPORTED;
#endif
		}
		Type = GSM_NokiaPictureImage;
		count = 78;
		PHONE_EncodeBitmap(Type, reqPicture + count, Bitmap);
		count += PHONE_GetBitmapSize(Type,0,0);
		smprintf(s, "Setting Picture Image\n");
		return GSM_WaitFor (s, reqPicture, count, 0x14, 4, ID_SetBitmap);
	default:
		break;
	}
	return GE_NOTSUPPORTED;
}

static GSM_Error N6510_ReplyGetRingtoneID(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_Phone_N6510Data *Priv = &s->Phone.Data.Priv.N6510;		

	smprintf(s, "Ringtone ID received\n");
	Priv->RingtoneID = msg.Buffer[15];
	return GE_NONE;
}

static GSM_Error N6510_ReplySetBinRingtone(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "Binary ringtone set\n");
	return GE_NONE;
}

static GSM_Error N6510_SetRingtone(GSM_StateMachine *s, GSM_Ringtone *Ringtone, int *maxlength)
{
	GSM_Error		error;
	GSM_Phone_N6510Data 	*Priv = &s->Phone.Data.Priv.N6510;
	GSM_NetworkInfo		NetInfo;
	int			size=200, current;
	unsigned char 		GetIDReq[] = {
		N7110_FRAME_HEADER, 0x01, 0x00, 0x00,
		0x00, 0xFF, 0x06, 0xE1, 0x00,
		0xFF, 0x06, 0xE1, 0x01, 0x42};
	unsigned char		SetPreviewReq[1000] = {
		0xAE,		/* Ringtone ID */
		0x01, 0x00, 0x0D, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00,
		0x00};  	/*Length*/                               
	unsigned char		AddBinaryReq[33000] = {
		N7110_FRAME_HEADER, 0x0E, 0x7F, 0xFF, 0xFE};

	if (Ringtone->Format == RING_NOTETONE && Ringtone->Location==255)
	{
		smprintf(s, "Getting ringtone ID\n");
		error=GSM_WaitFor (s, GetIDReq, 14, 0xDB, 4, ID_SetRingtone);
		if (error != GE_NONE) return error;
		*maxlength=GSM_EncodeNokiaRTTLRingtone(*Ringtone, SetPreviewReq+11, &size);
		SetPreviewReq[0]  = Priv->RingtoneID;
		SetPreviewReq[10] = size;
		smprintf(s, "Setting ringtone\n");
		error = s->Protocol.Functions->WriteMessage(s, SetPreviewReq, size+11, 0x00);
		if (error!=GE_NONE) return error;
		my_sleep(1000);
		/* We have to make something (not important, what) now */
		/* no answer from phone*/
		return s->Phone.Functions->GetNetworkInfo(s,&NetInfo);
	}
	if (Ringtone->Format == RING_NOKIABINARY) {
		AddBinaryReq[7] = UnicodeLength(Ringtone->Name);
		CopyUnicodeString(AddBinaryReq+8,Ringtone->Name);
		current = 8 + UnicodeLength(Ringtone->Name)*2;
		AddBinaryReq[current++] = Ringtone->NokiaBinary.Length/256 + 1;
		AddBinaryReq[current++] = Ringtone->NokiaBinary.Length%256 + 1;
		AddBinaryReq[current++] = 0x00;
		memcpy(AddBinaryReq+current,Ringtone->NokiaBinary.Frame,Ringtone->NokiaBinary.Length);
		current += Ringtone->NokiaBinary.Length;
		smprintf(s, "Adding binary ringtone\n");
		return GSM_WaitFor (s, AddBinaryReq, current, 0x1F, 4, ID_SetRingtone);
	}
	if (Ringtone->Format == RING_MIDI) {
		AddBinaryReq[7] = UnicodeLength(Ringtone->Name);
		CopyUnicodeString(AddBinaryReq+8,Ringtone->Name);
		current = 8 + UnicodeLength(Ringtone->Name)*2;
		AddBinaryReq[current++] = Ringtone->NokiaBinary.Length/256;
		AddBinaryReq[current++] = Ringtone->NokiaBinary.Length%256;
		memcpy(AddBinaryReq+current,Ringtone->NokiaBinary.Frame,Ringtone->NokiaBinary.Length);
		current += Ringtone->NokiaBinary.Length;
		AddBinaryReq[current++] = 0x00;
		AddBinaryReq[current++] = 0x00;
		smprintf(s, "Adding binary or MIDI ringtone\n");
		return GSM_WaitFor (s, AddBinaryReq, current, 0x1F, 4, ID_SetRingtone);
	}
	return GE_NOTSUPPORTED;
}

static GSM_Error N6510_ReplyDeleteRingtones(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "Ringtones deleted\n");
	return GE_NONE;
}

static GSM_Error N6510_DeleteUserRingtones(GSM_StateMachine *s)
{
	unsigned char DelAllRingtoneReq[] = {
		N7110_FRAME_HEADER, 0x10, 0x7F, 0xFE};

	smprintf(s, "Deleting all user ringtones\n");
	return GSM_WaitFor (s, DelAllRingtoneReq, 6, 0x1F, 4, ID_SetRingtone);
}

static GSM_Error N6510_PressKey(GSM_StateMachine *s, GSM_KeyCode Key, bool Press)
{
#ifdef DEVELOP
	unsigned char req[] = {
		N6110_FRAME_HEADER, 0x11, 0x00, 0x01, 0x00, 0x00,
		0x00,	/* Event */
		0x01};	/* Number of presses */

//	req[7] = Key;
	if (Press) {
		req[8] = NOKIA_PRESSPHONEKEY;
		s->Phone.Data.PressKey = true;
		smprintf(s, "Pressing key\n");
	} else {
		req[8] = NOKIA_RELEASEPHONEKEY;
		s->Phone.Data.PressKey = false;
		smprintf(s, "Releasing key\n");
	}
	return GSM_WaitFor (s, req, 10, 0x0c, 4, ID_PressKey);
#else
	return GE_NOTSUPPORTED;
#endif
}

static GSM_Error N6510_EnableWAPMMSSettings(GSM_StateMachine *s, bool MMS)
{
	GSM_Error	error;
	unsigned char 	req1[] = {N6110_FRAME_HEADER, 0x03};
	unsigned char 	req2[] = {N6110_FRAME_HEADER, 0x00, 0x01};

	if (MMS && IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_NOMMS)) return GE_NOTSUPPORTED;

	error=GSM_WaitFor (s, req1, 4, 0x3f, 4, ID_EnableWAP);
	if (error != GE_NONE) return error;

	if (MMS) {
		dprintf("Enabling MMS\n");
		return GSM_WaitFor (s, req2, 5, 0x3f, 4, ID_EnableWAP);
	} else {
		return GSM_WaitFor (s, req2, 4, 0x3f, 4, ID_EnableWAP);
	}
}

static GSM_Error N6510_ReplyGetWAPMMSSettings(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	int 			tmp,num=0;
	GSM_Phone_Data		*Data = &s->Phone.Data;

	switch(msg.Buffer[3]) {
	case 0x16:
		smprintf(s, "WAP settings received OK\n");

		if (Data->RequestID == ID_GetMMSSettings) {
			Data->WAPSettings->Number = 1;
		} else {
			Data->WAPSettings->Number = 2;
		}

		tmp = 4;

		NOKIA_GetUnicodeString(s, &tmp, msg.Buffer, Data->WAPSettings->Settings[0].Title,true);
		CopyUnicodeString(Data->WAPSettings->Settings[1].Title,Data->WAPSettings->Settings[0].Title);
		smprintf(s, "Title: \"%s\"\n",DecodeUnicodeString(Data->WAPSettings->Settings[0].Title));

		NOKIA_GetUnicodeString(s, &tmp, msg.Buffer, Data->WAPSettings->Settings[0].HomePage,true);
		CopyUnicodeString(Data->WAPSettings->Settings[1].HomePage,Data->WAPSettings->Settings[0].HomePage);
		smprintf(s, "Homepage: \"%s\"\n",DecodeUnicodeString(Data->WAPSettings->Settings[0].HomePage));

#ifdef DEBUG
		smprintf(s, "Connection type: ");      
		switch (msg.Buffer[tmp]) {
			case 0x00: smprintf(s, "temporary\n");  break;
			case 0x01: smprintf(s, "continuous\n"); break;
			default:   smprintf(s, "unknown\n");
		}
		smprintf(s, "Connection security: ");
		switch (msg.Buffer[tmp+1]) {
			case 0x00: smprintf(s, "off\n");	break;
			case 0x01: smprintf(s, "on\n");		break;
			default:   smprintf(s, "unknown\n");
		}
		smprintf(s, "Bearer: ");
		switch (msg.Buffer[tmp+2]) {
			case 0x01: smprintf(s, "GSM data\n");	break;
			case 0x03: smprintf(s, "GPRS\n");	break;
			default:   smprintf(s, "unknown\n");
		}
#endif
		Data->WAPSettings->Settings[0].IsContinuous = false;
		if (msg.Buffer[tmp] == 0x01) Data->WAPSettings->Settings[0].IsContinuous = true;
		Data->WAPSettings->Settings[1].IsContinuous = Data->WAPSettings->Settings[0].IsContinuous;

		Data->WAPSettings->Settings[0].IsSecurity = false;
		if (msg.Buffer[tmp+1] == 0x01) Data->WAPSettings->Settings[0].IsSecurity = true;
		Data->WAPSettings->Settings[1].IsSecurity = Data->WAPSettings->Settings[0].IsSecurity;

		Data->WAPSettings->ActiveBearer = WAPSETTINGS_BEARER_DATA;
		if (msg.Buffer[tmp+2] == 0x03) Data->WAPSettings->ActiveBearer = WAPSETTINGS_BEARER_GPRS;

		tmp+=3;

		if (Data->RequestID == ID_GetWAPSettings) {
			/* Here starts settings for data bearer */
			Data->WAPSettings->Settings[0].Bearer = WAPSETTINGS_BEARER_DATA;
			while ((msg.Buffer[tmp] != 0x01) || (msg.Buffer[tmp + 1] != 0x00)) tmp++;
			tmp += 4;

#ifdef DEBUG
			smprintf(s, "Authentication type: ");
			switch (msg.Buffer[tmp]) {
				case 0x00: smprintf(s, "normal\n");	break;
				case 0x01: smprintf(s, "secure\n");	break;
				default:   smprintf(s, "unknown\n");	break;
			}
			smprintf(s, "Data call type: ");
			switch (msg.Buffer[tmp+1]) {
				case 0x00: smprintf(s, "analogue\n");	break;
				case 0x01: smprintf(s, "ISDN\n");	break;
				default:   smprintf(s, "unknown\n");	break;
			}
			smprintf(s, "Data call speed: ");
			switch (msg.Buffer[tmp+2]) {
				case 0x00: smprintf(s, "automatic\n"); 	break;
				case 0x01: smprintf(s, "9600\n");	break;
				case 0x02: smprintf(s, "14400\n");	break;
				default:   smprintf(s, "unknown\n");	break;
			}
			smprintf(s, "Login Type: ");
			switch (msg.Buffer[tmp+4]) {
				case 0x00: smprintf(s, "manual\n");	break;
				case 0x01: smprintf(s, "automatic\n");	break;
				default:   smprintf(s, "unknown\n");	break;
			}
#endif
			Data->WAPSettings->Settings[0].IsNormalAuthentication=true;
			if (msg.Buffer[tmp]==0x01) Data->WAPSettings->Settings[0].IsNormalAuthentication=false;

			Data->WAPSettings->Settings[0].IsISDNCall=false;
			if (msg.Buffer[tmp+1]==0x01) Data->WAPSettings->Settings[0].IsISDNCall=true;

			switch (msg.Buffer[tmp+2]) {
				case 0x00: Data->WAPSettings->Settings[0].Speed=WAPSETTINGS_SPEED_AUTO;  break;
				case 0x01: Data->WAPSettings->Settings[0].Speed=WAPSETTINGS_SPEED_9600;	 break;
				case 0x02: Data->WAPSettings->Settings[0].Speed=WAPSETTINGS_SPEED_14400; break;
			}

			Data->WAPSettings->Settings[0].ManualLogin=false;
			if (msg.Buffer[tmp+4]==0x00) Data->WAPSettings->Settings[0].ManualLogin = true;

			tmp+=5;

			NOKIA_GetUnicodeString(s, &tmp, msg.Buffer, Data->WAPSettings->Settings[0].IPAddress,false);
			smprintf(s, "IP address: \"%s\"\n",DecodeUnicodeString(Data->WAPSettings->Settings[0].IPAddress));

			NOKIA_GetUnicodeString(s, &tmp, msg.Buffer, Data->WAPSettings->Settings[0].DialUp,true);
			smprintf(s, "Dial-up number: \"%s\"\n",DecodeUnicodeString(Data->WAPSettings->Settings[0].DialUp));

			NOKIA_GetUnicodeString(s, &tmp, msg.Buffer, Data->WAPSettings->Settings[0].User,true);
			smprintf(s, "User name: \"%s\"\n",DecodeUnicodeString(Data->WAPSettings->Settings[0].User));

			NOKIA_GetUnicodeString(s, &tmp, msg.Buffer, Data->WAPSettings->Settings[0].Password,true);		
			smprintf(s, "Password: \"%s\"\n",DecodeUnicodeString(Data->WAPSettings->Settings[0].Password));

			num = 1;
		} else {
			num = 0;
		}

		/* Here starts settings for gprs bearer */
		Data->WAPSettings->Settings[num].Bearer = WAPSETTINGS_BEARER_GPRS;
		while (msg.Buffer[tmp] != 0x03) tmp++;
		tmp += 4;

#ifdef DEBUG
		smprintf(s, "Authentication type: ");
		switch (msg.Buffer[tmp]) {
			case 0x00: smprintf(s, "normal\n");	break;
			case 0x01: smprintf(s, "secure\n");	break;
			default:   smprintf(s, "unknown\n");	break;
		}
		smprintf(s, "GPRS connection: ");
		switch (msg.Buffer[tmp+1]) {
			case 0x00: smprintf(s, "ALWAYS online\n"); break;
			case 0x01: smprintf(s, "when needed\n");   break;
			default:   smprintf(s, "unknown\n"); 	   break;
		}
		smprintf(s, "Login Type: ");
		switch (msg.Buffer[tmp+2]) {
			case 0x00: smprintf(s, "manual\n");	break;
			case 0x01: smprintf(s, "automatic\n");	break;
			default:   smprintf(s, "unknown\n");	break;
		}
#endif
		Data->WAPSettings->Settings[num].IsNormalAuthentication=true;
		if (msg.Buffer[tmp]==0x01) Data->WAPSettings->Settings[num].IsNormalAuthentication=false;

		Data->WAPSettings->Settings[num].IsContinuous = true;
		if (msg.Buffer[tmp+1] == 0x01) Data->WAPSettings->Settings[num].IsContinuous = false;

		Data->WAPSettings->Settings[num].ManualLogin=false;
		if (msg.Buffer[tmp+2]==0x00) Data->WAPSettings->Settings[num].ManualLogin = true;

		tmp+=3;

		NOKIA_GetUnicodeString(s, &tmp, msg.Buffer, Data->WAPSettings->Settings[num].DialUp,false);
		smprintf(s, "Access point: \"%s\"\n",DecodeUnicodeString(Data->WAPSettings->Settings[num].DialUp));

		NOKIA_GetUnicodeString(s, &tmp, msg.Buffer, Data->WAPSettings->Settings[num].IPAddress,true);
		smprintf(s, "IP address: \"%s\"\n",DecodeUnicodeString(Data->WAPSettings->Settings[num].IPAddress));

		NOKIA_GetUnicodeString(s, &tmp, msg.Buffer, Data->WAPSettings->Settings[num].User,true);
		smprintf(s, "User name: \"%s\"\n",DecodeUnicodeString(Data->WAPSettings->Settings[num].User));

		NOKIA_GetUnicodeString(s, &tmp, msg.Buffer, Data->WAPSettings->Settings[num].Password,true);
		smprintf(s, "Password: \"%s\"\n",DecodeUnicodeString(Data->WAPSettings->Settings[num].Password));

		return GE_NONE;
	case 0x17:
		smprintf(s, "WAP settings receiving error\n");
		switch (msg.Buffer[4]) {
		case 0x01:
			smprintf(s, "Security error. Inside WAP settings menu\n");
			return GE_INSIDEPHONEMENU;
		case 0x02:
			smprintf(s, "Invalid or empty\n");
			return GE_INVALIDLOCATION;
		default:
			smprintf(s, "ERROR: unknown %i\n",msg.Buffer[4]);
			return GE_UNKNOWNRESPONSE;
		}
		break;
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_GetWAPMMSSettings(GSM_StateMachine *s, GSM_MultiWAPSettings *settings, bool MMS)
{
	GSM_Error 	error;
	unsigned char 	req[] = {
		N6110_FRAME_HEADER, 0x15,
		0x00};		/* Location */

	error = N6510_EnableWAPMMSSettings(s, MMS);
	if (error!=GE_NONE) return error;

	req[4] = settings->Location-1;
	s->Phone.Data.WAPSettings = settings;
	if (MMS) {
		smprintf(s, "Getting MMS settings\n");
		error=GSM_WaitFor (s, req, 5, 0x3f, 4, ID_GetMMSSettings);
	} else {
		smprintf(s, "Getting WAP settings\n");
		error=GSM_WaitFor (s, req, 5, 0x3f, 4, ID_GetWAPSettings);
	}
	if (error != GE_NONE) return error;

	return DCT3DCT4_GetActiveWAPMMSSet(s);
}

static GSM_Error N6510_GetWAPSettings(GSM_StateMachine *s, GSM_MultiWAPSettings *settings)
{
	return N6510_GetWAPMMSSettings(s, settings, false);
}

static GSM_Error N6510_GetMMSSettings(GSM_StateMachine *s, GSM_MultiWAPSettings *settings)
{
	return N6510_GetWAPMMSSettings(s, settings, true);
}

static GSM_Error N6510_ReplySetWAPMMSSettings(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	switch (msg.Buffer[3]) {
	case 0x19:
		smprintf(s, "WAP settings cleaned\n");
		return GE_NONE;
	case 0x1a:
		smprintf(s, "WAP settings setting status\n");
		switch (msg.Buffer[4]) {
		case 0x01:
			smprintf(s, "Security error. Inside WAP settings menu\n");
			return GE_INSIDEPHONEMENU;
		case 0x03:
			smprintf(s, "Invalid location\n");
			return GE_INVALIDLOCATION;
		case 0x05:
			smprintf(s, "Written OK\n");
			return GE_NONE;
		default:
			smprintf(s, "ERROR: unknown %i\n",msg.Buffer[4]);
			return GE_UNKNOWNRESPONSE;
		}
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_SetWAPMMSSettings(GSM_StateMachine *s, GSM_MultiWAPSettings *settings, bool MMS)
{
	GSM_Error 	error;
	int 		i, pad = 0, length, pos = 5, loc1=-1,loc2=-1;
	unsigned char 	req[1000] = {
		N6110_FRAME_HEADER, 0x18,
		0x00};		/* Location */

	error = N6510_EnableWAPMMSSettings(s, MMS);
	if (error!=GE_NONE) return error;

	memset(req + pos, 0, 1000 - pos);

	req[4] = settings->Location-1;

	for (i=0;i<settings->Number;i++) {
		if (settings->Settings[i].Bearer == WAPSETTINGS_BEARER_DATA) loc1=i;
		if (settings->Settings[i].Bearer == WAPSETTINGS_BEARER_GPRS) loc2=i;
	}

	if (loc1 != -1) {
		/* Name */
		length = UnicodeLength(settings->Settings[loc1].Title);
		if (!(length % 2)) pad = 1;
		pos += NOKIA_SetUnicodeString(s, req + pos, settings->Settings[loc1].Title, false);

		/* Home */
		length = UnicodeLength(settings->Settings[loc1].HomePage);
		if (((length + pad) % 2)) pad = 2; else pad = 0;
		pos += NOKIA_SetUnicodeString(s, req + pos, settings->Settings[loc1].HomePage, true);

		if (settings->Settings[loc1].IsContinuous) req[pos] = 0x01; pos++;
		if (settings->Settings[loc1].IsSecurity) req[pos] = 0x01; pos++;
	} else if (loc2 != -1) {
		/* Name */
		length = UnicodeLength(settings->Settings[loc2].Title);
		if (!(length % 2)) pad = 1;
		pos += NOKIA_SetUnicodeString(s, req + pos, settings->Settings[loc2].Title, false);

		/* Home */
		length = UnicodeLength(settings->Settings[loc2].HomePage);
		if (((length + pad) % 2)) pad = 2; else pad = 0;
		pos += NOKIA_SetUnicodeString(s, req + pos, settings->Settings[loc2].HomePage, true);

		if (settings->Settings[loc2].IsContinuous) req[pos] = 0x01; pos++;
		if (settings->Settings[loc2].IsSecurity) req[pos] = 0x01; pos++;
	} else {
		/* Name */
		length = 0;
		if (!(length % 2)) pad = 1;
		pos ++;

		/* Home */
		length = 0;
		if (((length + pad) % 2)) pad = 2; else pad = 0;
		pos += 2;

		pos += 2;
	}

	if (MMS) {
		req[pos++] = 0x03; //GPRS

		/* How many parts do we send? */
		req[pos++] = 0x01; 		pos += pad;
	} else {
		if (settings->ActiveBearer == WAPSETTINGS_BEARER_GPRS && loc2 != -1) {
			req[pos++] = 0x03; //GPRS
		} else {
			req[pos++] = 0x01; //data set
		}
		/* How many parts do we send? */
		req[pos++] = 0x02; 		pos += pad;

		/* GSM data */
		memcpy(req + pos, "\x01\x00", 2);	pos += 2;

		if (loc1 != -1) {
			length  = UnicodeLength(settings->Settings[loc1].IPAddress)*2+1;
			length += UnicodeLength(settings->Settings[loc1].DialUp)   *2+2;
			length += UnicodeLength(settings->Settings[loc1].User)     *2+2;
			length += UnicodeLength(settings->Settings[loc1].Password) *2+2;
		} else {
			length = 1 + 2 + 2 + 2;
		}
		length += 11;
		req[pos++] = length / 256;
		req[pos++] = length % 256;

		if (loc1 != -1) {
			if (!settings->Settings[loc1].IsNormalAuthentication) req[pos]=0x01; pos++;
			if (settings->Settings[loc1].IsISDNCall) req[pos]=0x01;	pos++;
			switch (settings->Settings[loc1].Speed) {
				case WAPSETTINGS_SPEED_AUTO	: 		 break;
				case WAPSETTINGS_SPEED_9600	: req[pos]=0x01; break;
				case WAPSETTINGS_SPEED_14400	: req[pos]=0x02; break;
			}
			pos++;
			req[pos++]=0x01;
			if (!settings->Settings[loc1].ManualLogin) req[pos] = 0x01; pos++;
	
			pos += NOKIA_SetUnicodeString(s, req + pos, settings->Settings[loc1].IPAddress, false);
			pos += NOKIA_SetUnicodeString(s, req + pos, settings->Settings[loc1].DialUp, true);
			pos += NOKIA_SetUnicodeString(s, req + pos, settings->Settings[loc1].User, true);
			pos += NOKIA_SetUnicodeString(s, req + pos, settings->Settings[loc1].Password, true);
		} else {
			pos += 3;
			req[pos++]=0x01;
			pos += 8;
		}

		/* Padding */
		pos+=2;
	}

	/* GPRS block */
	memcpy(req + pos, "\x03\x00", 2);	pos += 2;

	if (loc2 != -1) {
		length  = UnicodeLength(settings->Settings[loc2].DialUp)   *2+1;
		length += UnicodeLength(settings->Settings[loc2].IPAddress)*2+2;
		length += UnicodeLength(settings->Settings[loc2].User)     *2+2;
		length += UnicodeLength(settings->Settings[loc2].Password) *2+2;
	} else {
		length = 7;
	}
	length += 7;
	req[pos++] = length / 256;
	req[pos++] = length % 256;

	if (loc2 != -1) {
		if (!settings->Settings[loc2].IsNormalAuthentication) req[pos] = 0x01; pos++;
		if (!settings->Settings[loc2].IsContinuous) req[pos] = 0x01; pos++;
		if (!settings->Settings[loc2].ManualLogin) req[pos] = 0x01; pos++;

		pos += NOKIA_SetUnicodeString(s, req + pos, settings->Settings[loc2].DialUp, false);
		pos += NOKIA_SetUnicodeString(s, req + pos, settings->Settings[loc2].IPAddress, true);
		pos += NOKIA_SetUnicodeString(s, req + pos, settings->Settings[loc2].User, true);
		pos += NOKIA_SetUnicodeString(s, req + pos, settings->Settings[loc2].Password, true);
	} else {
		pos += 10;
	}

	/* end of blocks ? */
	memcpy(req + pos, "\x80\x00\x00\x0c", 4);	pos += 4;

	if (MMS) {
		smprintf(s, "Setting MMS settings\n");
		error = GSM_WaitFor (s, req, pos, 0x3f, 4, ID_SetMMSSettings);
	} else {
		smprintf(s, "Setting WAP settings\n");
		error = GSM_WaitFor (s, req, pos, 0x3f, 4, ID_SetWAPSettings);
	}
	if (error != GE_NONE) return error;
	return DCT3DCT4_SetActiveWAPMMSSet(s, settings, MMS);
}

static GSM_Error N6510_SetWAPSettings(GSM_StateMachine *s, GSM_MultiWAPSettings *settings)
{
	return N6510_SetWAPMMSSettings(s, settings, false);
}

static GSM_Error N6510_SetMMSSettings(GSM_StateMachine *s, GSM_MultiWAPSettings *settings)
{
	return N6510_SetWAPMMSSettings(s, settings, true);
}

static GSM_Error N6510_ReplyGetOriginalIMEI(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	if (msg.Buffer[7] == 0x00) {
		smprintf(s, "No SIM card\n");
		return GE_SECURITYERROR;
	} else {
		return NOKIA_ReplyGetPhoneString(msg, s);
	}
}

static GSM_Error N6510_GetOriginalIMEI(GSM_StateMachine *s, char *value)
{
	return NOKIA_GetPhoneString(s,"\x00\x07\x02\x01\x00\x01",6,0x42,value,ID_GetOriginalIMEI,14);
}

static GSM_Error N6510_SetWAPBookmark(GSM_StateMachine *s, GSM_WAPBookmark *bookmark)
{
	GSM_Error 	error;
	int 		count;
	int		location;
	unsigned char 	req[600] = { N6110_FRAME_HEADER, 0x09 };

	/* We have to enable WAP frames in phone */
	error=DCT3DCT4_EnableWAP(s);
	if (error!=GE_NONE) return error;

	if (bookmark->Location == 0) {
		location = 0xffff;
	} else {
		location = bookmark->Location - 1;
	}
	count = 4;
	req[count++] = (location & 0xff00) >> 8;
	req[count++] = (location & 0x00ff);

	req[count++] = 0x00;
	req[count++] = UnicodeLength(bookmark->Title);
	CopyUnicodeString(req+count,bookmark->Title);
	count = count + 2*UnicodeLength(bookmark->Title);

	req[count++] = 0x00;
	req[count++] = UnicodeLength(bookmark->Address);
	CopyUnicodeString(req+count,bookmark->Address);
	count = count + 2*UnicodeLength(bookmark->Address);

	req[count++] = 0x00;
	req[count++] = 0x00;
	req[count++] = 0x00;
	req[count++] = 0x00;

	smprintf(s, "Setting WAP bookmark\n");
	return GSM_WaitFor (s, req, count, 0x3f, 4, ID_SetWAPBookmark);
}

static GSM_Error N6510_ReplyGetSMSStatus(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_Phone_Data *Data = &s->Phone.Data;

	switch (msg.Buffer[3]) {
	case 0x09:
		switch (msg.Buffer[4]) {
		case 0x00:
			smprintf(s, "Max. in phone memory   : %i\n",msg.Buffer[10]*256+msg.Buffer[11]);
			smprintf(s, "Used in phone memory   : %i\n",msg.Buffer[12]*256+msg.Buffer[13]);
			smprintf(s, "Unread in phone memory : %i\n",msg.Buffer[14]*256+msg.Buffer[15]);
			smprintf(s, "Max. in SIM            : %i\n",msg.Buffer[22]*256+msg.Buffer[23]);
			smprintf(s, "Used in SIM            : %i\n",msg.Buffer[24]*256+msg.Buffer[25]);
			smprintf(s, "Unread in SIM          : %i\n",msg.Buffer[26]*256+msg.Buffer[27]);
			Data->SMSStatus->PhoneSize	= msg.Buffer[10]*256+msg.Buffer[11];
			Data->SMSStatus->PhoneUsed	= msg.Buffer[12]*256+msg.Buffer[13];
			Data->SMSStatus->PhoneUnRead 	= msg.Buffer[14]*256+msg.Buffer[15];
			Data->SMSStatus->SIMSize	= msg.Buffer[22]*256+msg.Buffer[23];
			Data->SMSStatus->SIMUsed 	= msg.Buffer[24]*256+msg.Buffer[25];
			Data->SMSStatus->SIMUnRead 	= msg.Buffer[26]*256+msg.Buffer[27];
			return GE_NONE;
		case 0x0f:
			smprintf(s, "No PIN\n");
			return GE_SECURITYERROR;
		default:
			smprintf(s, "ERROR: unknown %i\n",msg.Buffer[4]);
			return GE_UNKNOWNRESPONSE;
		}
	case 0x1a:
		smprintf(s, "Wait a moment. Phone is during power on and busy now\n");
		return GE_SECURITYERROR;		
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_GetSMSStatus(GSM_StateMachine *s, GSM_SMSMemoryStatus *status)
{
	GSM_Error 		error;
	GSM_Phone_N6510Data	*Priv = &s->Phone.Data.Priv.N6510;
	unsigned char req[] = {N6110_FRAME_HEADER, 0x08, 0x00, 0x00};

	s->Phone.Data.SMSStatus=status;
	smprintf(s, "Getting SMS status\n");
	error = GSM_WaitFor (s, req, 6, 0x14, 2, ID_GetSMSStatus);
	if (error != GE_NONE) return error;

	/* Nokia 6310 and family does not show not "fixed" messages from the
	 * Templates folder, ie. when you save a message to the Templates folder,
	 * SMSStatus does not change! Workaround: get Templates folder status, which
	 * does show these messages.
	 */
	error = N6510_GetSMSFolderStatus(s, 0x06);
	if (error != GE_NONE) return error;
	status->TemplatesUsed = Priv->LastSMSFolder.Number;

	return error;
}

static GSM_Error N6510_ReplyDeleteSMSMessage(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	switch (msg.Buffer[3]) {
		case 0x05:
			smprintf(s, "SMS deleted OK\n");
			return GE_NONE;
		case 0x06:
			switch (msg.Buffer[4]) {
				case 0x02:
					smprintf(s, "Invalid location\n");
					return GE_INVALIDLOCATION;
				default:
					smprintf(s, "Unknown error: %02x\n",msg.Buffer[4]);
					return GE_UNKNOWNRESPONSE;
			}
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_DeleteSMSMessage(GSM_StateMachine *s, GSM_SMSMessage *sms)
{
	unsigned char		folderid;
	int			location;
	unsigned char req[] = {
		N6110_FRAME_HEADER, 0x04,
		0x01, 		/* 0x01 for SM, 0x02 for ME */
		0x00, 		/* FolderID */
		0x00, 0x02, 	/* Location */
		0x0F, 0x55};

	N6510_GetSMSLocation(s, sms, &folderid, &location);

	switch (folderid) {
		case 0x01: req[5] = 0x02; 			 break; /* INBOX SIM 	*/
		case 0x02: req[5] = 0x03; 			 break; /* OUTBOX SIM 	*/
		default	 : req[5] = folderid - 1; req[4] = 0x02; break; /* ME folders	*/
	}
	req[6]=location / 256;
	req[7]=location;

	smprintf(s, "Deleting sms\n");
	return GSM_WaitFor (s, req, 10, 0x14, 4, ID_DeleteSMSMessage);
}

static GSM_Error N6510_ReplySendSMSMessage(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	switch (msg.Buffer[8]) {
		case 0x00:
			smprintf(s, "SMS sent OK\n");
			if (s->User.SendSMSStatus!=NULL) s->User.SendSMSStatus(s->CurrentConfig->Device,0);
			return GE_NONE;
		default:
			smprintf(s, "SMS not sent OK, error code probably %i\n",msg.Buffer[8]);
			if (s->User.SendSMSStatus!=NULL) s->User.SendSMSStatus(s->CurrentConfig->Device,msg.Buffer[8]);
			return GE_NONE;
	}
}

static GSM_Error N6510_SendSMSMessage(GSM_StateMachine *s, GSM_SMSMessage *sms)
{
	int			length = 11;
	GSM_Error		error;
	GSM_SMSMessageLayout 	Layout;
	unsigned char req [300] = {
		N6110_FRAME_HEADER, 0x02, 0x00, 0x00, 0x00, 0x55, 0x55};

	if (sms->PDU == SMS_Deliver) sms->PDU = SMS_Submit;
	memset(req+9,0x00,sizeof(req) - 9);
	error=N6510_EncodeSMSFrame(s, sms, req + 9, &Layout, &length);
	if (error != GE_NONE) return error;

	smprintf(s, "Sending sms\n");
	return s->Protocol.Functions->WriteMessage(s, req, length + 9, 0x02);
}

static GSM_Error N6510_ReplyGetSecurityStatus(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_Phone_Data *Data = &s->Phone.Data;

	smprintf(s, "Security Code status received: ");
	switch (msg.Buffer[4]) {
	case 0x01 : smprintf(s, "waiting for Security Code.\n"); *Data->SecurityStatus = GSCT_SecurityCode;	break;
	case 0x07 :
	case 0x02 : smprintf(s, "waiting for PIN.\n");		 *Data->SecurityStatus = GSCT_Pin;		break;
	case 0x03 : smprintf(s, "waiting for PUK.\n");		 *Data->SecurityStatus = GSCT_Puk;		break;
	case 0x05 : smprintf(s, "PIN ok, SIM ok\n");		 *Data->SecurityStatus = GSCT_None;		break;
	case 0x06 : smprintf(s, "No input status\n"); 		 *Data->SecurityStatus = GSCT_None;		break;
	case 0x16 : smprintf(s, "No SIM!\n");			 *Data->SecurityStatus = GSCT_None;		break;
	case 0x1A : smprintf(s, "SIM rejected!\n");		 *Data->SecurityStatus = GSCT_None;		break;
	default   : smprintf(s, "ERROR: unknown %i\n",msg.Buffer[4]);
		    return GE_UNKNOWNRESPONSE;
	}
	return GE_NONE;
}

static GSM_Error N6510_GetSecurityStatus(GSM_StateMachine *s, GSM_SecurityCodeType *Status)
{
	unsigned char req[5] = {N6110_FRAME_HEADER, 0x11, 0x00};

	s->Phone.Data.SecurityStatus=Status;
	smprintf(s, "Getting security code status\n");
	return GSM_WaitFor (s, req, 5, 0x08, 2, ID_GetSecurityStatus);
}

static GSM_Error N6510_ReplyEnterSecurityCode(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	switch (msg.Buffer[3]) {
	case 0x08:
		smprintf(s, "Security code OK\n");
		return GE_NONE;
	case 0x09:
		switch (msg.Buffer[4]) {
		case 0x06:
			smprintf(s, "Wrong PIN\n");
			return GE_SECURITYERROR;
		case 0x09:
			smprintf(s, "Wrong PUK\n");
			return GE_SECURITYERROR;
		default:
			smprintf(s, "ERROR: unknown %i\n",msg.Buffer[4]);
		}
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_EnterSecurityCode(GSM_StateMachine *s, GSM_SecurityCode Code)
{
	int 		len = 0;
	unsigned char 	req[15] = {
		N6110_FRAME_HEADER, 0x07,
		0x00};		/* Type of the entered code: 0x02 PIN, 0x03 PUK */

	switch (Code.Type) {
		case GSCT_Pin	: req[4] = 0x02; break;
		default		: return GE_NOTSUPPORTED;
	}                            

	len = strlen(Code.Code);
	memcpy(req+5,Code.Code,len);
	req[5+len]=0x00;

	smprintf(s, "Entering security code\n");
	return GSM_WaitFor (s, req, 6+len, 0x08, 4, ID_EnterSecurityCode);
}

static GSM_Error N6510_ReplySaveSMSMessage(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	unsigned char 		folder;
	GSM_Phone_Data		*Data = &s->Phone.Data;

	switch (msg.Buffer[3]) {
	case 0x01:
		switch (msg.Buffer[4]) {
			case 0x00:
				smprintf(s, "Done OK\n");
				smprintf(s, "Folder info: %i %i\n",msg.Buffer[8],msg.Buffer[5]);
				switch (msg.Buffer[8]) {
				case 0x02 : if (msg.Buffer[5] == 0x02) {
						 folder = 0x02; /* INBOX ME */
					    } else {
						 folder = 0x01; /* INBOX SIM */
					    }
					    break;
				case 0x03 : if (msg.Buffer[5] == 0x02) {
						 folder = 0x04; /* OUTBOX ME */
					    } else {
						 folder = 0x03; /* OUTBOX SIM */
					    }
					    break;
				default	  : folder = msg.Buffer[8] + 1;
				}
				N6510_SetSMSLocation(s, Data->SaveSMSMessage,folder,msg.Buffer[6]*256+msg.Buffer[7]);
				smprintf(s, "Saved in folder %i at location %i\n",folder, msg.Buffer[6]*256+msg.Buffer[7]);
				Data->SaveSMSMessage->Folder = folder;
				return GE_NONE;
			case 0x02:
				printf("Incorrect location\n");
				return GE_INVALIDLOCATION;
			case 0x05:
				printf("Incorrect folder\n");
				return GE_INVALIDLOCATION;
			default:
				smprintf(s, "ERROR: unknown %i\n",msg.Buffer[4]);
				return GE_UNKNOWNRESPONSE;
		}
	case 0x17:
		smprintf(s, "SMS name changed\n");
		return GE_NONE;
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_SaveSMSMessage(GSM_StateMachine *s, GSM_SMSMessage *sms)
{
	int			location, length = 11;
	unsigned char		folderid, folder;
	GSM_SMSMessageLayout 	Layout;
	GSM_Error		error;
	unsigned char req [300] = {
		N6110_FRAME_HEADER, 0x00,
		0x01,			/* 1 = SIM, 2 = ME 	*/
		0x02,			/* Folder   		*/
		0x00, 0x01,		/* Location 		*/
		0x01};			/* SMS state 		*/
	unsigned char NameReq[200] = {
		N6110_FRAME_HEADER, 0x16,
		0x01,			/* 1 = SIM, 2 = ME 	*/
		0x02,			/* Folder   		*/
		0x00, 0x01};		/* Location 		*/

	N6510_GetSMSLocation(s, sms, &folderid, &location);
	switch (folderid) {
		case 0x01: req[5] = 0x02; 			 break; /* INBOX SIM 	*/
		case 0x02: req[5] = 0x03; 			 break; /* OUTBOX SIM 	*/
		default	 : req[5] = folderid - 1; req[4] = 0x02; break; /* ME folders	*/
	}
	req[6]=location / 256;
	req[7]=location;

	switch (sms->PDU) {
	case SMS_Submit:
		/* Inbox */
		if (folderid == 0x01 || folderid == 0x03) sms->PDU = SMS_Deliver;
		break;
	case SMS_Deliver:
		break;
	default:
		return GE_UNKNOWN;
	}
	if (sms->PDU == SMS_Deliver) {
		switch (sms->State) {
			case GSM_Sent	: /* We use GSM_Read, because phone return error */
			case GSM_Read	: req[8] = 0x01; break;
			case GSM_UnSent	: /* We use GSM_UnRead, because phone return error */
			case GSM_UnRead	: req[8] = 0x03; break;
		}
	} else {
		switch (sms->State) {
			case GSM_Sent	: /* We use GSM_Sent, because phone change folder */
			case GSM_Read	: req[8] = 0x05; break;
			case GSM_UnSent	: /* We use GSM_UnSent, because phone change folder */
			case GSM_UnRead	: req[8] = 0x07; break;
		}
	}
	memset(req+9,0x00,sizeof(req) - 9);
	error=N6510_EncodeSMSFrame(s, sms, req + 9, &Layout, &length);
	if (error != GE_NONE) return error;

	s->Phone.Data.SaveSMSMessage=sms;
	smprintf(s, "Saving sms\n");
	error=GSM_WaitFor (s, req, length+9, 0x14, 4, ID_SaveSMSMessage);
	if (error == GE_NONE && UnicodeLength(sms->Name)!=0) {
		folder = sms->Folder;
		sms->Folder = 0;
		N6510_GetSMSLocation(s, sms, &folderid, &location);
		switch (folderid) {
			case 0x01: NameReq[5] = 0x02; 				 break; /* INBOX SIM 	*/
			case 0x02: NameReq[5] = 0x03; 			 	 break; /* OUTBOX SIM 	*/
			default	 : NameReq[5] = folderid - 1; NameReq[4] = 0x02; break; /* ME folders	*/
		}
		NameReq[6]=location / 256;
		NameReq[7]=location;
		length = 8;
		CopyUnicodeString(NameReq+length, sms->Name);
		length = length+UnicodeLength(sms->Name)*2;
		NameReq[length++] = 0;
		NameReq[length++] = 0;
		error=GSM_WaitFor (s, NameReq, length, 0x14, 4, ID_SaveSMSMessage);
		sms->Folder = folder;
	}
	return error;
}

static GSM_Error N6510_ReplyGetDateTime(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "Date & time received\n");
	if (msg.Buffer[4]==0x01) {
		NOKIA_DecodeDateTime(s, msg.Buffer+10, s->Phone.Data.DateTime);
		return GE_NONE;
	}
	smprintf(s, "Not set in phone\n");
	return GE_EMPTY;
}

static GSM_Error N6510_GetDateTime(GSM_StateMachine *s, GSM_DateTime *date_time)
{
	unsigned char req[] = {N6110_FRAME_HEADER, 0x0A, 0x00, 0x00};

	s->Phone.Data.DateTime=date_time;
	smprintf(s, "Getting date & time\n");
	return GSM_WaitFor (s, req, 6, 0x19, 4, ID_GetDateTime);
}

static GSM_Error N6510_ReplySetDateTime(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "Date & time set\n");
	return GE_NONE;
}

static GSM_Error N6510_SetDateTime(GSM_StateMachine *s, GSM_DateTime *date_time)
{
	unsigned char req[] = {
		N6110_FRAME_HEADER,
		0x01, 0x00, 0x01, 0x01, 0x0c, 0x01, 0x03,
		0x07, 0xd2,	/* Year */
		0x08, 0x01,     /* Month & Day */
		0x15, 0x1f,	/* Hours & Minutes */
		0x2b,		/* Second ? */
		0x00};

	NOKIA_EncodeDateTime(s, req+10, date_time);
	req[16] = date_time->Second;
	smprintf(s, "Setting date & time\n");
	return GSM_WaitFor (s, req, 18, 0x19, 4, ID_SetDateTime);
}

static GSM_Error N6510_ReplyGetManufactureMonth(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	if (msg.Buffer[7] == 0x00) {
		smprintf(s, "No SIM card\n");
		return GE_SECURITYERROR;
	} else {
		sprintf(s->Phone.Data.PhoneString,"%02i/%04i",msg.Buffer[13],msg.Buffer[14]*256+msg.Buffer[15]);
	        return GE_NONE;
	}
}

static GSM_Error N6510_GetManufactureMonth(GSM_StateMachine *s, char *value)
{
	unsigned char req[6] = {0x00, 0x05, 0x02, 0x01, 0x00, 0x02};
//	unsigned char req[6] = {0x00, 0x03, 0x04, 0x0B, 0x01, 0x00};

	s->Phone.Data.PhoneString=value;
	smprintf(s, "Getting manufacture month\n");
	return GSM_WaitFor (s, req, 6, 0x42, 2, ID_GetManufactureMonth);
//	return GSM_WaitFor (s, req, 6, 0x1B, 2, ID_GetManufactureMonth);
}                                       

static GSM_Error N6510_ReplyGetAlarm(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_Phone_Data *Data = &s->Phone.Data;

	switch(msg.Buffer[3]) {
	case 0x1A:
		smprintf(s, "   Alarm: %02d:%02d\n", msg.Buffer[14], msg.Buffer[15]);
		Data->Alarm->Hour	= msg.Buffer[14];
		Data->Alarm->Minute	= msg.Buffer[15];
		Data->Alarm->Second	= 0;
		return GE_NONE;
	case 0x20:
		smprintf(s, "Alarm state received\n");
		if (msg.Buffer[37] == 0x01) {
			smprintf(s, "   Not set in phone\n");
			return GE_EMPTY;
		}
		smprintf(s, "Enabled\n");
		return GE_NONE;
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_GetAlarm(GSM_StateMachine *s, GSM_DateTime *alarm, int alarm_number)
{
	unsigned char   StateReq[] = {N6110_FRAME_HEADER, 0x1f, 0x01, 0x00};
	unsigned char   GetReq  [] = {N6110_FRAME_HEADER, 0x19, 0x00, 0x02};
	GSM_Error	error;

	if (alarm_number!=1) return GE_NOTSUPPORTED;

	s->Phone.Data.Alarm=alarm;
	smprintf(s, "Getting alarm state\n");
	error = GSM_WaitFor (s, StateReq, 6, 0x19, 4, ID_GetAlarm);
	if (error != GE_NONE) return error;

	smprintf(s, "Getting alarm\n");
	return GSM_WaitFor (s, GetReq, 6, 0x19, 4, ID_GetAlarm);
}

static GSM_Error N6510_ReplySetAlarm(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "Alarm set\n");
	return GE_NONE;
}

static GSM_Error N6510_SetAlarm(GSM_StateMachine *s, GSM_DateTime *alarm, int alarm_number)
{
	unsigned char req[] = {
		N6110_FRAME_HEADER,
		0x11, 0x00, 0x01, 0x01, 0x0c, 0x02,
		0x01, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00,		/* Hours, Minutes */
		0x00, 0x00, 0x00 };

	if (alarm_number!=1) return GE_NOTSUPPORTED;

	req[14] = alarm->Hour;
	req[15] = alarm->Minute;

	smprintf(s, "Setting alarm\n");
	return GSM_WaitFor (s, req, 19, 0x19, 4, ID_SetAlarm);
}

static GSM_Error N6510_ReplyGetRingtonesInfo(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	int 			tmp,i;
	GSM_Phone_Data		*Data = &s->Phone.Data;

	smprintf(s, "Ringtones info received\n");
	memset(Data->RingtonesInfo,0,sizeof(GSM_AllRingtonesInfo));
	Data->RingtonesInfo->Number = msg.Buffer[4] * 256 + msg.Buffer[5];
	tmp = 6;
	for (i=0;i<Data->RingtonesInfo->Number;i++) {			
		Data->RingtonesInfo->Ringtone[i].ID = msg.Buffer[tmp+2] * 256 + msg.Buffer[tmp+3];
		memcpy(Data->RingtonesInfo->Ringtone[i].Name,msg.Buffer+tmp+8,(msg.Buffer[tmp+6]*256+msg.Buffer[tmp+7])*2);
		smprintf(s, "%i. \"%s\"\n",Data->RingtonesInfo->Ringtone[i].ID,DecodeUnicodeString(Data->RingtonesInfo->Ringtone[i].Name));
		tmp = tmp + (msg.Buffer[tmp]*256+msg.Buffer[tmp+1]);
	}
	return GE_NONE;
}

static GSM_Error N6510_PrivGetRingtonesInfo(GSM_StateMachine *s, GSM_AllRingtonesInfo *Info, bool AllRingtones)
{
	GSM_Error	error;
	unsigned char 	UserReq[8] = {N7110_FRAME_HEADER, 0x07, 0x00, 0x00, 0x00, 0x02};
	unsigned char 	All_Req[9] = {N7110_FRAME_HEADER, 0x07, 0x00, 0x00, 0xFE, 0x00, 0x7D};

	s->Phone.Data.RingtonesInfo=Info;
	smprintf(s, "Getting binary ringtones ID\n");
	if (AllRingtones) {
		error = GSM_WaitFor (s, All_Req, 9, 0x1f, 4, ID_GetRingtonesInfo);
		if (error == GE_NONE && Info->Number == 0) return GE_NOTSUPPORTED;
		return error;
	} else {
		return GSM_WaitFor (s, UserReq, 8, 0x1f, 4, ID_GetRingtonesInfo);
	}
}

static GSM_Error N6510_GetRingtonesInfo(GSM_StateMachine *s, GSM_AllRingtonesInfo *Info)
{
	return N6510_PrivGetRingtonesInfo(s, Info, true);
}

static GSM_Error N6510_ReplyGetRingtone(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	int 			tmp,i;
	GSM_Phone_Data		*Data = &s->Phone.Data;

	smprintf(s, "Ringtone received\n");
	memcpy(Data->Ringtone->Name,msg.Buffer+8,msg.Buffer[7]*2);
	Data->Ringtone->Name[msg.Buffer[7]*2]=0;
	Data->Ringtone->Name[msg.Buffer[7]*2+1]=0;
	smprintf(s, "Name \"%s\"\n",DecodeUnicodeString(Data->Ringtone->Name));
 	if (msg.Buffer[msg.Buffer[7]*2+10] == 'M' &&
 	    msg.Buffer[msg.Buffer[7]*2+11] == 'T' &&
 	    msg.Buffer[msg.Buffer[7]*2+12] == 'h' &&
 	    msg.Buffer[msg.Buffer[7]*2+13] == 'd')
 	{
 		smprintf(s,"MIDI\n");
 		tmp 	= msg.Buffer[7]*2+10;
 		i 	= msg.Length - 2; /* ?????? */
 		Data->Ringtone->Format = RING_MIDI;
 	} else {
 		/* Looking for end */
 		i=8+msg.Buffer[7]*2+3;
 		tmp = i;
 		while (true) {
 			if (msg.Buffer[i]==0x07 && msg.Buffer[i+1]==0x0b) {
 				i=i+2; break;
 			}
 			i++;
 			if (i==msg.Length) return GE_EMPTY;
 		}	  
	}
	/* Copying frame */
	memcpy(Data->Ringtone->NokiaBinary.Frame,msg.Buffer+tmp,i-tmp);
	Data->Ringtone->NokiaBinary.Length=i-tmp;
	return GE_NONE;
}

static GSM_Error N6510_GetRingtone(GSM_StateMachine *s, GSM_Ringtone *Ringtone, bool PhoneRingtone)
{
	GSM_AllRingtonesInfo 	Info;
	GSM_Error		error;
	unsigned char 		req2[6] = {
		N7110_FRAME_HEADER, 0x12,
		0x00, 0xe7}; 	/* Location */

	if (Ringtone->Format == 0x00) Ringtone->Format = RING_NOKIABINARY;

	switch (Ringtone->Format) {
	case RING_NOTETONE:
		/* In the future get binary and convert */
		return GE_NOTSUPPORTED;
	case RING_NOKIABINARY:
		s->Phone.Data.Ringtone=Ringtone;
		error=N6510_PrivGetRingtonesInfo(s, &Info, PhoneRingtone);
		if (error != GE_NONE) return error;
		if (Ringtone->Location > Info.Number) return GE_INVALIDLOCATION;
		req2[4] = Info.Ringtone[Ringtone->Location-1].ID / 256;
		req2[5] = Info.Ringtone[Ringtone->Location-1].ID % 256;
		smprintf(s, "Getting binary ringtone\n");
		return GSM_WaitFor (s, req2, 6, 0x1f, 4, ID_GetRingtone);
	case RING_MIDI:
		return GE_NOTSUPPORTED;
	}
	return GE_NOTSUPPORTED;
}

static GSM_Error N6510_PlayTone(GSM_StateMachine *s, int Herz, unsigned char Volume, bool start)
{
	GSM_Error 	error;
	unsigned char 	reqStart[] = {
		0x00,0x06,0x01,0x00,0x07,0x00 };
	unsigned char 	reqPlay[] = {
		0x00,0x06,0x01,0x14,0x05,0x04,
		0x00,0x00,0x00,0x03,0x03,0x08,
		0x00,0x00,0x00,0x01,0x00,0x00,
		0x03,0x08,0x01,0x00,
		0x07,0xd0,	/*Frequency */
		0x00,0x00,0x03,0x08,0x02,0x00,0x00,
		0x05,		/*Volume */
		0x00,0x00};
	unsigned char 	reqOff[] = {
		0x00,0x06,0x01,0x14,0x05,0x05,
		0x00,0x00,0x00,0x01,0x03,0x08,
		0x05,0x00,0x00,0x08,0x00,0x00};
//	unsigned char 	reqOff2[] = {
//		0x00,0x06,0x01,0x14,0x05,0x04,
//		0x00,0x00,0x00,0x01,0x03,0x08,
//		0x00,0x00,0x00,0x00,0x00,0x00};

	if (start) {
		smprintf(s, "Enabling sound - part 1\n");
		error=GSM_WaitFor (s, reqStart, 6, 0x0b, 4, ID_PlayTone);
		if (error!=GE_NONE) return error;
		smprintf(s, "Enabling sound - part 2 (disabling sound command)\n");
		error=GSM_WaitFor (s, reqOff, 18, 0x0b, 4, ID_PlayTone);
		if (error!=GE_NONE) return error;
	}

	/* For Herz==255*255 we have silent */  
	if (Herz!=255*255) {
		reqPlay[23] = Herz%256;
		reqPlay[22] = Herz/256;
		reqPlay[31] = Volume;
		smprintf(s, "Playing sound\n");
		return GSM_WaitFor (s, reqPlay, 34, 0x0b, 4, ID_PlayTone);
	} else {
		reqPlay[23] = 0;
		reqPlay[22] = 0;
		reqPlay[31] = 0;
		smprintf(s, "Playing silent sound\n");
		return GSM_WaitFor (s, reqPlay, 34, 0x0b, 4, ID_PlayTone);

//		smprintf(s, "Disabling sound - part 1\n");
//		error=GSM_WaitFor (s, reqOff, 18, 0x0b, 4, ID_PlayTone);
//		if (error!=GE_NONE) return error;		
//		smprintf(s, "Disabling sound - part 2\n");
//		return GSM_WaitFor (s, reqOff2, 18, 0x0b, 4, ID_PlayTone);
	}
}

static GSM_Error N6510_ReplyGetPPM(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_Phone_Data 	*Data = &s->Phone.Data;
	int		pos = 6,len;

	smprintf(s, "Received phone info\n");

	while(pos < msg.Length) {
		if (msg.Buffer[pos] == 0x55 && msg.Buffer[pos+1] == 0x55) {
			while(1) {
				if (msg.Buffer[pos] != 0x55) break;
				pos++;
			}
		}
		len = pos;
		while(1) {
			if (msg.Buffer[len] == 0x00 && msg.Buffer[len+1] == 0x00) break;
			len++;
		}
		while(1) {
			if (msg.Buffer[len] != 0x00) break;
			len++;
		}
		len = len-pos;
		smprintf(s, "Block with ID %02x",msg.Buffer[pos]);
#ifdef DEBUG
		if (di.dl == DL_TEXTALL || di.dl == DL_TEXTALLDATE) DumpMessage(di.df, msg.Buffer+pos, len);
#endif
		switch (msg.Buffer[pos]) {
		case 0x49:
			smprintf(s, "hardware version\n");
			break;
		case 0x58:
			pos += 3;
			while (msg.Buffer[pos] != 0x00) pos++;
			Data->PhoneString[0] = msg.Buffer[pos - 1];
			Data->PhoneString[1] = 0x00;
			smprintf(s, "PPM %s\n",Data->PhoneString);
			return GE_NONE;
		default:
			break;
		}
		pos += len;
	}
	return GE_NOTSUPPORTED;
}

static GSM_Error N6510_GetPPM(GSM_StateMachine *s,char *value)
{
//	unsigned char req[6] = {N6110_FRAME_HEADER, 0x07, 0x01, 0xff};
	unsigned char req[6] = {N6110_FRAME_HEADER, 0x07, 0x01, 0x00};

	s->Phone.Data.PhoneString=value;
	smprintf(s, "Getting PPM\n");
	return GSM_WaitFor (s, req, 6, 0x1b, 3, ID_GetPPM);
}

static GSM_Error N6510_GetSpeedDial(GSM_StateMachine *s, GSM_SpeedDial *SpeedDial)
{
	GSM_PhonebookEntry 	pbk;
	GSM_Error		error;

	pbk.MemoryType			= GMT7110_SP;
	pbk.Location			= SpeedDial->Location;
	SpeedDial->MemoryLocation 	= 0;
	s->Phone.Data.SpeedDial		= SpeedDial;

	smprintf(s, "Getting speed dial\n");
	error=N6510_GetMemory(s,&pbk);
	switch (error) {
	case GE_NOTSUPPORTED:
		smprintf(s, "No speed dials set in phone\n");
		return GE_EMPTY;
	case GE_NONE:
		if (SpeedDial->MemoryLocation == 0) {
			smprintf(s, "Speed dial not assigned or error in firmware\n");
			return GE_EMPTY;
		}
		return GE_NONE;
	default:
		return error;
	}
}

static GSM_Error N6510_ReplyGetProfile(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	unsigned char 	*blockstart;
	int 		i,j;
	GSM_Phone_Data	*Data = &s->Phone.Data;
	                
	switch (msg.Buffer[3]) {
	case 0x02: 
	blockstart = msg.Buffer + 7;
	for (i = 0; i < 11; i++) {
		smprintf(s, "Profile feature %02x ",blockstart[1]);
#ifdef DEBUG
		if (di.dl == DL_TEXTALL || di.dl == DL_TEXTALLDATE) DumpMessage(di.df, blockstart, blockstart[0]);
#endif

		switch (blockstart[1]) {
		case 0x03:
			smprintf(s, "Ringtone ID\n");
			Data->Profile->FeatureID	[Data->Profile->FeaturesNumber] = Profile_RingtoneID;
			Data->Profile->FeatureValue	[Data->Profile->FeaturesNumber] = blockstart[7];
			if (blockstart[7] == 0x00) {
				Data->Profile->FeatureValue[Data->Profile->FeaturesNumber] = blockstart[10];
			}
			Data->Profile->FeaturesNumber++;
			break;
		case 0x05:	/* SMS tone */
			j = Data->Profile->FeaturesNumber;
			NOKIA_FindFeatureValue(s, Profile71_65,blockstart[1],blockstart[7],Data,false);
			if (j == Data->Profile->FeaturesNumber) {
				Data->Profile->FeatureID	[Data->Profile->FeaturesNumber] = Profile_MessageTone;
				Data->Profile->FeatureValue	[Data->Profile->FeaturesNumber] = PROFILE_MESSAGE_PERSONAL;
				Data->Profile->FeaturesNumber++;
				Data->Profile->FeatureID	[Data->Profile->FeaturesNumber] = Profile_MessageToneID;
				Data->Profile->FeatureValue	[Data->Profile->FeaturesNumber] = blockstart[7];
				Data->Profile->FeaturesNumber++;
			}
			break;
		case 0x08:	/* Caller groups */
			NOKIA_FindFeatureValue(s, Profile71_65,blockstart[1],blockstart[7],Data,true);
			break;
		case 0x0c :
			CopyUnicodeString(Data->Profile->Name,blockstart + 7);
			smprintf(s, "profile Name: \"%s\"\n", DecodeUnicodeString(Data->Profile->Name));
			Data->Profile->DefaultName = false;
			break;
		default:
			NOKIA_FindFeatureValue(s, Profile71_65,blockstart[1],blockstart[7],Data,false);
		}
		blockstart = blockstart + blockstart[0];
	}
	return GE_NONE;
	case 0x06:
		Data->Profile->Active = false;
		if (Data->Profile->Location == msg.Buffer[5]) Data->Profile->Active = true;
		return GE_NONE;
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_GetProfile(GSM_StateMachine *s, GSM_Profile *Profile)
{
	unsigned char 	req[150]    = {N6110_FRAME_HEADER, 0x01, 0x01, 0x0C, 0x01};
	unsigned char	reqActive[] = {N6110_FRAME_HEADER, 0x05};
	int 		i, length = 7;
	GSM_Error	error;

	/* For now !!! */
	if (!strcmp(s->Phone.Data.ModelInfo->model,"3510")) {
		if (s->Phone.Data.VerNum>3.37) return GE_NOTSUPPORTED;
	}

	if (Profile->Location>5) return GE_INVALIDLOCATION;

	for (i = 0; i < 0x0a; i++) {
		req[length++] = 0x04;
		req[length++] = Profile->Location;
		req[length++] = i;
		req[length++] = 0x01;
	}

	req[length++] = 0x04;
	req[length++] = Profile->Location;
	req[length++] = 0x0c;
	req[length++] = 0x01;

	req[length++] = 0x04;

	Profile->CarKitProfile	= false;
	Profile->HeadSetProfile	= false;

	Profile->FeaturesNumber = 0;

	s->Phone.Data.Profile=Profile;
	smprintf(s, "Getting profile\n");
	error = GSM_WaitFor (s, req, length, 0x39, 4, ID_GetProfile);
	if (error != GE_NONE) return error;

	smprintf(s, "Checking, which profile is active\n");
	return GSM_WaitFor (s, reqActive, 4, 0x39, 4, ID_GetProfile);
}

static GSM_Error N6510_ReplySetProfile(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	unsigned char 	*blockstart;
	int 		i;

	smprintf(s, "Response to profile writing received!\n");

	blockstart = msg.Buffer + 6;
	for (i = 0; i < msg.Buffer[5]; i++) {
		switch (blockstart[2]) {
		case 0x00:
			if (msg.Buffer[4] == 0x00) 
				smprintf(s, "keypad tone level successfully set!\n");
			else
				smprintf(s, "failed to set keypad tone level! error: %i\n", msg.Buffer[4]);
			break;
		case 0x02:
			if (msg.Buffer[4] == 0x00) 
				smprintf(s, "call alert successfully set!\n");
			else
				smprintf(s, "failed to set call alert! error: %i\n", msg.Buffer[4]);
			break;
		case 0x03:
			if (msg.Buffer[4] == 0x00) 
				smprintf(s, "ringtone successfully set!\n");
			else
			smprintf(s, "failed to set ringtone! error: %i\n", msg.Buffer[4]);
			break;
		case 0x04:
			if (msg.Buffer[4] == 0x00) 
				smprintf(s, "ringtone volume successfully set!\n");
			else
				smprintf(s, "failed to set ringtone volume! error: %i\n", msg.Buffer[4]);
			break;
		case 0x05:
			if (msg.Buffer[4] == 0x00) 
				smprintf(s, "msg.Buffer tone successfully set!\n");
			else
				smprintf(s, "failed to set msg.Buffer tone! error: %i\n", msg.Buffer[4]);
			break;
		case 0x06:
			if (msg.Buffer[4] == 0x00) 
				smprintf(s, "vibration successfully set!\n");
			else
				smprintf(s, "failed to set vibration! error: %i\n", msg.Buffer[4]);
			break;
		case 0x07:
			if (msg.Buffer[4] == 0x00) 
				smprintf(s, "warning tone level successfully set!\n");
			else
				smprintf(s, "failed to set warning tone level! error: %i\n", msg.Buffer[4]);
			break;
		case 0x08:
			if (msg.Buffer[4] == 0x00) 
				smprintf(s, "caller groups successfully set!\n");
			else
				smprintf(s, "failed to set caller groups! error: %i\n", msg.Buffer[4]);
			break;
		case 0x09:
			if (msg.Buffer[4] == 0x00) 
				smprintf(s, "automatic answer successfully set!\n");
			else
				smprintf(s, "failed to set automatic answer! error: %i\n", msg.Buffer[4]);
			break;
		case 0x0c:
			if (msg.Buffer[4] == 0x00) 
				smprintf(s, "name successfully set!\n");
			else
				smprintf(s, "failed to set name! error: %i\n", msg.Buffer[4]);
			break;
		default:
			smprintf(s, "Unknown profile subblock type %02x!\n", blockstart[2]);
			break;
		}
		blockstart = blockstart + blockstart[1];
	}
	return GE_NONE;
}

static GSM_Error N6510_SetProfile(GSM_StateMachine *s, GSM_Profile *Profile)
{
	int 		i, length = 7, blocks = 0;
	bool		found;
	unsigned char	ID,Value;
	unsigned char 	req[150] = {
		N6110_FRAME_HEADER, 0x03, 0x01,
		0x06,		/* Number of blocks */
		0x03};

	if (Profile->Location>5) return GE_INVALIDLOCATION;

	for (i=0;i<Profile->FeaturesNumber;i++) {
		found = false;
		switch (Profile->FeatureID[i]) {
			case Profile_RingtoneID:
				ID 	= 0x03;
				Value 	= Profile->FeatureValue[i];
				found 	= true;
				break;
			default:
				found=NOKIA_FindPhoneFeatureValue(
					s,
					Profile71_65,
					Profile->FeatureID[i],Profile->FeatureValue[i],
					&ID,&Value);
		}
		if (found) {
			req[length] 	= 0x09;
			req[length + 1] = ID;
			req[length + 2] = Profile->Location;
			memcpy(req + length + 4, "\x00\x00\x01", 3);
			req[length + 8] = 0x03;
			req[length + 3] = req[length + 7] = Value;
			blocks++;
			length += 9;
		}
	}

	smprintf(s, "Setting profile\n");
	return GSM_WaitFor (s, req, length, 0x39, 4, ID_SetProfile);
}

static GSM_Error N6510_ReplyIncomingSMS(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_SMSMessage sms;

#ifdef DEBUG
	smprintf(s, "SMS message received\n");
	N6510_DecodeSMSFrame(s, &sms, msg.Buffer+10);
#endif

	if (s->Phone.Data.EnableIncomingSMS && s->User.IncomingSMS!=NULL) {
		sms.State 	 = GSM_UnRead;
		sms.InboxFolder  = true;

		N6510_DecodeSMSFrame(s, &sms, msg.Buffer+10);

		s->User.IncomingSMS(s->CurrentConfig->Device,sms);
	}
	return GE_NONE;
}

static GSM_Error N6510_DialVoice(GSM_StateMachine *s, char *number, GSM_CallShowNumber ShowNumber)
{
	unsigned int	pos = 4;
	unsigned char 	req[100] = {N6110_FRAME_HEADER,0x01,
		0x0c};			/* Number length in chars */

	req[pos++] = strlen(number);
	EncodeUnicode(req+pos,number,strlen(number));
	pos += strlen(number)*2;
	req[pos++] = 0x05; /* call type: voice - 0x05, data - 0x01 */
	req[pos++] = 0x01;
	req[pos++] = 0x05;
	req[pos++] = 0x00;
	req[pos++] = 0x02;
	req[pos++] = 0x00;
	req[pos++] = 0x00;
	switch (ShowNumber) {
	case GN_CALL_HideNumber:
		req[pos++] = 0x02;
		break;
	case GN_CALL_ShowNumber:
		req[pos++] = 0x03;
		break;
	case GN_CALL_Default:
		req[pos++] = 0x01;
		break;
	}

	smprintf(s, "Making voice call\n");
	return GSM_WaitFor (s, req, pos, 0x01, 4, ID_DialVoice);
}

/* method 3 */
static GSM_Error N6510_ReplyGetCalendarInfo3(GSM_Protocol_Message msg, GSM_StateMachine *s, GSM_NOKIACalToDoLocations *Last)
{
	int i=0,j=0;

	while (Last->Location[j] != 0x00) j++;
	if (j >= GSM_MAXCALENDARTODONOTES) {
		smprintf(s, "Increase GSM_MAXCALENDARTODONOTES\n");
		return GE_UNKNOWN;
	}
	if (j == 0) {
		Last->Number=msg.Buffer[8]*256+msg.Buffer[9];
		smprintf(s, "Number of Entries: %i\n",Last->Number);
	}
	smprintf(s, "Locations: ");
	while (14+(i*4) <= msg.Length) {
		Last->Location[j++]=msg.Buffer[12+i*4]*256+msg.Buffer[13+i*4];
		smprintf(s, "%i ",Last->Location[j-1]);
		i++;
	}
	smprintf(s, "\nNumber of Entries in frame: %i\n",i);
	Last->Location[j] = 0;
	smprintf(s, "\n");
	if (i == 0) return GE_EMPTY;
	return GE_NONE;
}

/* method 3 */
static GSM_Error N6510_GetCalendarInfo3(GSM_StateMachine *s, GSM_NOKIACalToDoLocations *Last, bool Calendar)
{
	GSM_Error 	error;
	int		i;
	unsigned char   req[] = {N6110_FRAME_HEADER, 0x9E, 0xFF, 0xFF, 0x00, 0x00,
			         0x00, 0x00,	/* First location */
			         0x00};		/* 0 = calendar, 1 = ToDo in 6610 style */

	Last->Location[0] = 0x00;
	Last->Number	  = 0;

	if (Calendar) {
		smprintf(s, "Getting locations for calendar method 3\n");
		error = GSM_WaitFor (s, req, 11, 0x13, 4, ID_GetCalendarNotesInfo);
	} else {
		req[10] = 0x01;
		smprintf(s, "Getting locations for ToDo method 2\n");
		error = GSM_WaitFor (s, req, 11, 0x13, 4, ID_GetToDo);
	}
	if (error != GE_NONE && error != GE_EMPTY) return error;

	while (1) {
		i=0;
		while (Last->Location[i] != 0x00) i++;
		smprintf(s, "i = %i %i\n",i,Last->Number);
		if (i == Last->Number) break;
		if (i != Last->Number && error == GE_EMPTY) {
			smprintf(s, "Phone doesn't support some notes with this method. Workaround\n");
			Last->Number = i;
			break;
		}
		req[8] = Last->Location[i-1] / 256;
		req[9] = Last->Location[i-1] % 256;
		if (Calendar) {
			smprintf(s, "Getting locations for calendar method 3\n");
			error = GSM_WaitFor (s, req, 11, 0x13, 4, ID_GetCalendarNotesInfo);
		} else {
			smprintf(s, "Getting locations for todo method 2\n");
			error = GSM_WaitFor (s, req, 11, 0x13, 4, ID_GetToDo);
		}
		if (error != GE_NONE && error != GE_EMPTY) return error;
	}
	return GE_NONE;
}

/* method 3 */
GSM_Error N6510_ReplyGetCalendar3(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_CalendarEntry 		*entry = s->Phone.Data.Cal;
	GSM_DateTime			Date;
	unsigned long			diff;
	int				i;
	bool				found = false;
	GSM_Phone_N6510Data		*Priv = &s->Phone.Data.Priv.N6510;

	smprintf(s, "Calendar note received method 3\n");

	smprintf(s,"Note type %02i: ",msg.Buffer[27]);
	switch(msg.Buffer[27]) {
		case 0x00: smprintf(s,"Reminder\n"); entry->Type = GCN_REMINDER; break;
		case 0x01: smprintf(s,"Meeting\n");  entry->Type = GCN_MEETING;  break;
		case 0x02: smprintf(s,"Call\n");     entry->Type = GCN_CALL;     break;
		case 0x04: smprintf(s,"Birthday\n"); entry->Type = GCN_BIRTHDAY; break;
		case 0x08: smprintf(s,"Memo\n");     entry->Type = GCN_MEMO;     break;
		default  : smprintf(s,"unknown\n");
	}

	smprintf(s,"StartTime: %04i-%02i-%02i %02i:%02i\n",
		msg.Buffer[28]*256+msg.Buffer[29],
		msg.Buffer[30],msg.Buffer[31],msg.Buffer[32],
		msg.Buffer[33]);
	Date.Year 	= msg.Buffer[28]*256+msg.Buffer[29];	
	if (entry->Type == GCN_BIRTHDAY) {
		Date.Year = entry->Entries[0].Date.Year;
	}
	Date.Month 	= msg.Buffer[30];
	Date.Day 	= msg.Buffer[31];
	Date.Hour 	= msg.Buffer[32];
	Date.Minute 	= msg.Buffer[33];
	/* Garbage seen with 3510i 3.51 */
	if (Date.Month == 0 && Date.Day == 0 && Date.Hour == 0 && Date.Minute == 0) return GE_EMPTY;
	Date.Second	= 0;
	entry->Entries[0].EntryType = CAL_START_DATETIME;
	memcpy(&entry->Entries[0].Date,&Date,sizeof(GSM_DateTime));
	entry->EntriesNum++;

	if (entry->Type != GCN_BIRTHDAY) {
		smprintf(s,"EndTime: %04i-%02i-%02i %02i:%02i\n",
			msg.Buffer[34]*256+msg.Buffer[35],
			msg.Buffer[36],msg.Buffer[37],msg.Buffer[38],
			msg.Buffer[39]);
		Date.Year 	= msg.Buffer[34]*256+msg.Buffer[35];
		Date.Month 	= msg.Buffer[36];
		Date.Day 	= msg.Buffer[37];
		Date.Hour 	= msg.Buffer[38];
		Date.Minute 	= msg.Buffer[39];
		Date.Second	= 0;
		entry->Entries[1].EntryType = CAL_END_DATETIME;
		memcpy(&entry->Entries[1].Date,&Date,sizeof(GSM_DateTime));
		entry->EntriesNum++;
	}

	smprintf(s, "Note icon: %02x\n",msg.Buffer[21]);
	for(i=0;i<Priv->CalendarIconsNum;i++) {
		if (Priv->CalendarIconsTypes[i] == entry->Type) {
			found = true;
		}
	}
	if (!found) {
		Priv->CalendarIconsTypes[Priv->CalendarIconsNum] = entry->Type;
		Priv->CalendarIcons	[Priv->CalendarIconsNum] = msg.Buffer[21];
		Priv->CalendarIconsNum++;
	}

	if (msg.Buffer[14] == 0xFF && msg.Buffer[15] == 0xFF && msg.Buffer[16] == 0xff && msg.Buffer[17] == 0xff)
	{
		smprintf(s, "No alarm\n");
	} else {
		diff  = ((unsigned int)msg.Buffer[14]) << 24;
		diff += ((unsigned int)msg.Buffer[15]) << 16;
		diff += ((unsigned int)msg.Buffer[16]) << 8;
		diff += msg.Buffer[17];

		memcpy(&entry->Entries[entry->EntriesNum].Date,&entry->Entries[0].Date,sizeof(GSM_DateTime));
		GetTimeDifference(diff, &entry->Entries[entry->EntriesNum].Date, false, 60);
		smprintf(s, "Alarm date   : %02i-%02i-%04i %02i:%02i:%02i\n",
			entry->Entries[entry->EntriesNum].Date.Day,   entry->Entries[entry->EntriesNum].Date.Month,
			entry->Entries[entry->EntriesNum].Date.Year,  entry->Entries[entry->EntriesNum].Date.Hour,
			entry->Entries[entry->EntriesNum].Date.Minute,entry->Entries[entry->EntriesNum].Date.Second);

		entry->Entries[entry->EntriesNum].EntryType = CAL_ALARM_DATETIME;
		if (msg.Buffer[22]==0x00 && msg.Buffer[23]==0x00 &&
		    msg.Buffer[24]==0x00 && msg.Buffer[25]==0x00)
		{
			entry->Entries[entry->EntriesNum].EntryType = CAL_SILENT_ALARM_DATETIME;
			smprintf(s, "Alarm type   : Silent\n");
		}
		entry->EntriesNum++;
	}

	N71_65_GetCalendarRecurrance(s, msg.Buffer+40, entry);

	if (entry->Type == GCN_BIRTHDAY) {
		entry->Entries[0].Date.Year = msg.Buffer[42]*256+msg.Buffer[43];
	}

	memcpy(entry->Entries[entry->EntriesNum].Text, msg.Buffer+54, msg.Buffer[51]*2);
	entry->Entries[entry->EntriesNum].Text[msg.Buffer[51]*2]   = 0;
	entry->Entries[entry->EntriesNum].Text[msg.Buffer[51]*2+1] = 0;
	entry->Entries[entry->EntriesNum].EntryType		   = CAL_TEXT;
	entry->EntriesNum++;
	smprintf(s, "Note text: \"%s\"\n",DecodeUnicodeString(entry->Entries[entry->EntriesNum-1].Text));

	if (entry->Type == GCN_CALL) {
		memcpy(entry->Entries[entry->EntriesNum].Text, msg.Buffer+(54+msg.Buffer[51]*2), msg.Buffer[52]*2);
		entry->Entries[entry->EntriesNum].Text[msg.Buffer[52]*2]   = 0;
		entry->Entries[entry->EntriesNum].Text[msg.Buffer[52]*2+1] = 0;
		entry->Entries[entry->EntriesNum].EntryType		   = CAL_PHONE;
		entry->EntriesNum++;
	}
	if (entry->Type == GCN_MEETING) {
		memcpy(entry->Entries[entry->EntriesNum].Text, msg.Buffer+(54+msg.Buffer[51]*2), msg.Buffer[52]*2);
		entry->Entries[entry->EntriesNum].Text[msg.Buffer[52]*2]   = 0;
		entry->Entries[entry->EntriesNum].Text[msg.Buffer[52]*2+1] = 0;
		entry->Entries[entry->EntriesNum].EntryType		   = CAL_LOCATION;
		entry->EntriesNum++;
	}

	return GE_NONE;
}

static GSM_Error N6510_PrivGetCalendar3(GSM_StateMachine *s, GSM_CalendarEntry *Note, bool start, int *LastCalendarYear)
{
	GSM_Error		error;
	GSM_DateTime		date_time;
	unsigned char 		req[] = {
		N6110_FRAME_HEADER,0x7D,0x00,0x00,0x00,0x00,
		0x00,0x99,			/* Location */
		0xff,0xff,0xff,0xff,0x01};	

	if (start) {
		/* We have to get current year. It's NOT written in frame for
		 * Birthday
		 */
		error=s->Phone.Functions->GetDateTime(s,&date_time);
		switch (error) {
			case GE_EMPTY:
			case GE_NOTIMPLEMENTED:
				GSM_GetCurrentDateTime(&date_time);
				break;
			case GE_NONE:
				break;
			default:
				return error;
		}
		*LastCalendarYear = date_time.Year;
	}

	Note->EntriesNum		= 0;
	Note->Entries[0].Date.Year 	= *LastCalendarYear;

	req[8] = Note->Location >> 8;
	req[9] = Note->Location & 0xff;
                        
	s->Phone.Data.Cal=Note;
	smprintf(s, "Getting calendar note method 3\n");
	return GSM_WaitFor (s, req, 15, 0x13, 4, ID_GetCalendarNote);
}

/* method 3 */
GSM_Error N6510_GetNextCalendar3(GSM_StateMachine *s, GSM_CalendarEntry *Note, bool start, GSM_NOKIACalToDoLocations *LastCalendar, int *LastCalendarYear, int *LastCalendarPos)
{
	GSM_Error error;
	bool	  start2;

	if (start) {
		error=N6510_GetCalendarInfo3(s,LastCalendar,true);
		if (error!=GE_NONE) return error;
		if (LastCalendar->Number == 0) return GE_EMPTY;

		*LastCalendarPos = 0;
	} else {
		(*LastCalendarPos)++;
	}

	error  = GE_EMPTY;
	start2 = start;
	while (error == GE_EMPTY) {
		if (*LastCalendarPos >= LastCalendar->Number) return GE_EMPTY;
	
		Note->Location = LastCalendar->Location[*LastCalendarPos];
		error=N6510_PrivGetCalendar3(s, Note, start2, LastCalendarYear);
		if (error == GE_EMPTY) (*LastCalendarPos)++;

		start2 = false;
	}
	return error;
}

static GSM_Error N6510_ReplyGetCalendarInfo(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	switch (msg.Buffer[3]) {
	case 0x3B:
		/* Old method 1 for accessing calendar */
		return N71_65_ReplyGetCalendarInfo1(msg, s, &s->Phone.Data.Priv.N6510.LastCalendar);
	case 0x9F:
		smprintf(s, "Info with calendar notes locations received method 3\n");
		return N6510_ReplyGetCalendarInfo3(msg, s, &s->Phone.Data.Priv.N6510.LastCalendar);
	}
	return GE_UNKNOWNRESPONSE;
}

/* method 3 */
GSM_Error N6510_ReplyGetCalendarNotePos3(GSM_Protocol_Message msg, GSM_StateMachine *s,int *FirstCalendarPos)
{
	smprintf(s, "First calendar location: %i\n",msg.Buffer[8]*256+msg.Buffer[9]);
	*FirstCalendarPos = msg.Buffer[8]*256+msg.Buffer[9];
	return GE_NONE;
}

/* method 3 */
static GSM_Error N6510_GetCalendarNotePos3(GSM_StateMachine *s)
{
	unsigned char req[] = {N6110_FRAME_HEADER, 0x95, 0x00};

	smprintf(s, "Getting first free calendar note location\n");
	return GSM_WaitFor (s, req, 5, 0x13, 4, ID_GetCalendarNotePos);
}

static GSM_Error N6510_ReplyGetCalendarNotePos(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	switch (msg.Buffer[3]) {
#ifdef DEBUG
	case 0x32:
		/* Old method 1 for accessing calendar */
		return N71_65_ReplyGetCalendarNotePos1(msg, s,&s->Phone.Data.Priv.N6510.FirstCalendarPos);
#endif
	case 0x96:
		return N6510_ReplyGetCalendarNotePos3(msg, s,&s->Phone.Data.Priv.N6510.FirstCalendarPos);
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_FindCalendarIconID3(GSM_StateMachine *s, GSM_CalendarEntry *Entry, unsigned char *ID)
{
	int 				i,j,LastCalendarYear;
	GSM_Phone_N6510Data		*Priv = &s->Phone.Data.Priv.N6510;
	GSM_CalendarEntry 		Note;
	GSM_NOKIACalToDoLocations	LastCalendar1,LastCalendar2;
	GSM_Error			error;
	bool				found;

	for(i=0;i<Priv->CalendarIconsNum;i++) {
		if (Priv->CalendarIconsTypes[i] == Entry->Type) {
			*ID = Priv->CalendarIcons[i];
			return GE_NONE;
		}
	}

	smprintf(s, "Starting finding note ID\n");

	error=N6510_GetCalendarInfo3(s, &Priv->LastCalendar,true);
	memcpy(&LastCalendar1,&Priv->LastCalendar,sizeof(GSM_NOKIACalToDoLocations));
	if (error != GE_NONE) return error;

	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_CAL35) ||
	    IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_CAL65) ||
	    IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_CAL62)) {
		error=N71_65_AddCalendar2(s,Entry,true);
	} else {
		if (Entry->Type == GCN_MEETING) {
			error=N71_65_AddCalendar1(s, Entry, &s->Phone.Data.Priv.N6510.FirstCalendarPos, true);
		} else {
			error=N71_65_AddCalendar2(s,Entry,true);
		}
	}
	if (error != GE_NONE) return error;

	error=N6510_GetCalendarInfo3(s, &Priv->LastCalendar,true);
	memcpy(&LastCalendar2,&Priv->LastCalendar,sizeof(GSM_NOKIACalToDoLocations));
	if (error != GE_NONE) return error;

	smprintf(s,"Number of entries: %i %i\n",LastCalendar1.Number,LastCalendar2.Number);

	for(i=0;i<LastCalendar2.Number;i++) {
		found = true;
		for(j=0;j<LastCalendar1.Number;j++) {
			if (LastCalendar1.Location[j] == LastCalendar2.Location[i]) {
				found = false;
				break;
			}
		}
		if (found) {
			Note.Location = LastCalendar2.Location[i];
			error=N6510_PrivGetCalendar3(s, &Note, true, &LastCalendarYear);
			if (error != GE_NONE) return error;

			error=N71_65_DelCalendar(s, &Note);
			if (error != GE_NONE) return error;

			smprintf(s, "Ending finding note ID\n");

			for(j=0;j<Priv->CalendarIconsNum;j++) {
				if (Priv->CalendarIconsTypes[j] == Entry->Type) {
					*ID = Priv->CalendarIcons[j];
					return GE_NONE;
				}
			}
			return GE_UNKNOWN;
		}
	}

	return GE_UNKNOWN;
}

/* method 3 */
static GSM_Error N6510_ReplyAddCalendar3(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "Calendar note added\n");
	return GE_NONE;
}

/* method 3 */
GSM_Error N6510_AddCalendar3(GSM_StateMachine *s, GSM_CalendarEntry *Note, int *FirstCalendarPos, bool Past)
{
	GSM_CalendarNoteType	NoteType, OldNoteType;
	time_t     		t_time1,t_time2;
	long			diff;
 	GSM_Error		error;
	GSM_DateTime		DT,date_time;
 	int 			Text, Time, Alarm, Phone, Recurrance, EndTime, Location, count=54;
	unsigned char 		req[5000] = {
		N6110_FRAME_HEADER, 0x65,
		0x00,					/* 0 = calendar, 1 = todo 	*/
		0x00, 0x00, 0x00,
		0x00, 0x00,                             /* location 	    		*/
		0x00, 0x00, 0x00, 0x00, 		
		0xFF, 0xFF, 0xFF, 0xFF,			/* alarm 	    		*/
		0x80, 0x00, 0x00,
		0x01,					/* note icon	    		*/
		0xFF, 0xFF, 0xFF, 0xFF,			/* alarm type       		*/
		0x00,					/* 0x02 or 0x00     		*/
		0x01, 					/* note type	    		*/
		0x07, 0xD0, 0x01, 0x12, 0x0C, 0x00, 	/* start date/time  		*/
		0x07, 0xD0, 0x01, 0x12, 0x0C, 0x00, 	/* end date/time    		*/
		0x00, 0x00,				/* recurrance	    		*/
		0x00, 0x00,				/* birth year	    		*/
		0x20,					/* ToDo priority 		*/
		0x00,					/* ToDo completed ?		*/
		0x00, 0x00, 0x00,
		0x00,					/* note text length 		*/
		0x00,					/* phone length/meeting place	*/
		0x00, 0x00, 0x00};

	if (!Past && IsCalendarNoteFromThePast(Note)) return GE_NONE;

	error=N6510_GetCalendarNotePos3(s);
	if (error!=GE_NONE) return error;
	req[8] = *FirstCalendarPos/256;
	req[9] = *FirstCalendarPos%256;

 	NoteType = N71_65_FindCalendarType(Note->Type, s->Phone.Data.ModelInfo);

	switch(NoteType) {
		case GCN_REMINDER : req[27]=0x00; req[26]=0x02; break;
		case GCN_MEETING  : req[27]=0x01; break;
		case GCN_CALL     : req[27]=0x02; break;
		case GCN_BIRTHDAY : req[27]=0x04; break;
		case GCN_MEMO     : req[27]=0x08; break;
		default		  : return GE_UNKNOWN;
	}

	OldNoteType = Note->Type;
	Note->Type  = NoteType;
	error=N6510_FindCalendarIconID3(s, Note, &req[21]);
	Note->Type  = OldNoteType;
	if (error!=GE_NONE) return error;

	GSM_CalendarFindDefaultTextTimeAlarmPhoneRecurrance(Note, &Text, &Time, &Alarm, &Phone, &Recurrance, &EndTime, &Location);

	if (Time == -1) return GE_UNKNOWN;
	memcpy(&DT,&Note->Entries[Time].Date,sizeof(GSM_DateTime));
	req[28]	= DT.Year >> 8;
	req[29]	= DT.Year & 0xff;
	req[30]	= DT.Month;
	req[31]	= DT.Day;
	req[32]	= DT.Hour;
	req[33]	= DT.Minute;

	if (NoteType == GCN_BIRTHDAY) {
		error=s->Phone.Functions->GetDateTime(s,&date_time);
		switch (error) {
			case GE_EMPTY:
			case GE_NOTIMPLEMENTED:
				GSM_GetCurrentDateTime(&date_time);
				break;
			case GE_NONE:
				break;
			default:
				return error;
		}
	}
	if (NoteType == GCN_BIRTHDAY) {
		req[28]	= date_time.Year >> 8;
		req[29]	= date_time.Year & 0xff;		
		req[42]	= DT.Year >> 8;
		req[43]	= DT.Year & 0xff;
	}

	if (EndTime != -1) memcpy(&DT,&Note->Entries[EndTime].Date,sizeof(GSM_DateTime));
	req[34]	= DT.Year >> 8;
	req[35]	= DT.Year & 0xff;
	req[36]	= DT.Month;
	req[37]	= DT.Day;
	req[38]	= DT.Hour;
	req[39]	= DT.Minute;
	if (NoteType == GCN_BIRTHDAY) {
		req[34]	= date_time.Year >> 8;
		req[35]	= date_time.Year & 0xff;		
	}

	if (Recurrance != -1) {
		/* 0xffff -> 1 Year (8760 hours) */
		if (Note->Entries[Recurrance].Number >= 8760) {
			req[40] = 0xff;
			req[41] = 0xff;
		} else {
			req[40] = Note->Entries[Recurrance].Number / 256;
			req[41] = Note->Entries[Recurrance].Number % 256;
		}
	}

	if (Alarm != -1) {
		memcpy(&DT,&Note->Entries[Time].Date,sizeof(GSM_DateTime));
		if (Note->Entries[Alarm].EntryType == CAL_SILENT_ALARM_DATETIME)
		{
			req[22] = 0x00; req[23] = 0x00; req[24] = 0x00; req[25] = 0x00;
		}
		if (NoteType == GCN_BIRTHDAY) DT.Year = date_time.Year;
		t_time2   = Fill_Time_T(DT,8);
		t_time1   = Fill_Time_T(Note->Entries[Alarm].Date,8);
		diff	  = (t_time1-t_time2)/60;

		smprintf(s, "  Difference : %i seconds or minutes\n", -diff);
		req[14] = (unsigned char)(-diff >> 24);
		req[15] = (unsigned char)(-diff >> 16);
		req[16] = (unsigned char)(-diff >> 8);
		req[17] = (unsigned char)(-diff);
	}

	if (Text != -1) {
		req[49] = UnicodeLength(Note->Entries[Text].Text);
		CopyUnicodeString(req+54,Note->Entries[Text].Text);
		count+= req[49]*2;
	}

	if (Phone != -1 && NoteType == GCN_CALL) {
		req[50] = UnicodeLength(Note->Entries[Phone].Text);
		CopyUnicodeString(req+54+req[49]*2,Note->Entries[Phone].Text);
		count+= req[50]*2;
	}

	if (Location != -1 && NoteType == GCN_MEETING) {
		if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_CAL62) ||
		    IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_CAL65) ||
		    IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_CAL35)) {
		} else {
			req[50] = UnicodeLength(Note->Entries[Location].Text);
			CopyUnicodeString(req+54+req[49]*2,Note->Entries[Location].Text);
			count+= req[50]*2;
		}
	}

	req[count++] = 0x00;

	smprintf(s, "Writing calendar note method 3\n");
	return GSM_WaitFor (s, req, count, 0x13, 4, ID_SetCalendarNote);
}

static GSM_Error N6510_GetNextCalendar(GSM_StateMachine *s,  GSM_CalendarEntry *Note, bool start)
{
#ifdef GSM_FORCE_DCT4_CALENDAR_6210
    	/* Method 1. Some features missed. Not working with some notes in 3510 */
	return N71_65_GetNextCalendar1(s,Note,start,&s->Phone.Data.Priv.N6510.LastCalendar,&s->Phone.Data.Priv.N6510.LastCalendarYear,&s->Phone.Data.Priv.N6510.LastCalendarPos);
#endif              

	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_CAL62)) {
	    /* Method 1. Some features missed. Not working with some notes in 3510 */
	    return N71_65_GetNextCalendar1(s,Note,start,&s->Phone.Data.Priv.N6510.LastCalendar,&s->Phone.Data.Priv.N6510.LastCalendarYear,&s->Phone.Data.Priv.N6510.LastCalendarPos);

	    /* Method 2. In known phones texts of notes cut to 50 chars. Some features missed */
//	    return N71_65_GetNextCalendar2(s,Note,start,&s->Phone.Data.Priv.N6510.LastCalendarYear,&s->Phone.Data.Priv.N6510.LastCalendarPos);
	} else {
	    /* Method 3. All DCT4 features supported. Not supported by 8910 */
	    return N6510_GetNextCalendar3(s,Note,start,&s->Phone.Data.Priv.N6510.LastCalendar,&s->Phone.Data.Priv.N6510.LastCalendarYear,&s->Phone.Data.Priv.N6510.LastCalendarPos);
	}
}

static GSM_Error N6510_AddCalendar(GSM_StateMachine *s, GSM_CalendarEntry *Note, bool Past)
{
#ifdef GSM_FORCE_DCT4_CALENDAR_6210
	return N71_65_AddCalendar2(s,Note,Past);
#endif

	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_CAL62)) {
		return N71_65_AddCalendar2(s,Note,Past);
//		return N71_65_AddCalendar1(s, Note, &s->Phone.Data.Priv.N6510.FirstCalendarPos, Past);
	} else {
		/* Method 3. All DCT4 features supported. Not supported by 8910 */
		return N6510_AddCalendar3(s, Note, &s->Phone.Data.Priv.N6510.FirstCalendarPos, Past);
	}
}

static GSM_Error N6510_ReplyLogIntoNetwork(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "Probably phone says: I log into network\n");
	return GE_NONE;
}

void N6510_EncodeFMFrequency(double freq, unsigned char *buff)
{
	double			freq0;
	unsigned char		buffer[20];
	unsigned int		i,freq2;

	sprintf(buffer,"%.3f",freq);
	for (i=0;i<strlen(buffer);i++) {
		if (buffer[i] == ',' || buffer[i] == '.') buffer[i] = ' ';
	}
	StringToDouble(buffer, &freq0);
 	freq2 = (unsigned int)freq0;
	dprintf("Frequency: %s %i\n",buffer,freq2);	
 	freq2	= freq2 - 0xffff;
 	buff[0] = freq2 / 0x100;
 	buff[1] = freq2 % 0x100;
}

void N6510_DecodeFMFrequency(double *freq, unsigned char *buff)
{
	unsigned char buffer[20];

	sprintf(buffer,"%i.%i",(0xffff + buff[0] * 0x100 + buff[1])/1000,
			       (0xffff + buff[0] * 0x100 + buff[1])%1000);
	dprintf("Frequency: %s\n",buffer);
	StringToDouble(buffer, freq);
}

static GSM_Error N6510_ReplyGetFMStatus(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
  	smprintf(s, "getting FM status OK\n");
	memcpy(s->Phone.Data.Priv.N6510.FMStatus,msg.Buffer,msg.Length);
	s->Phone.Data.Priv.N6510.FMStatusLength = msg.Length;
	return GE_NONE;
}
   
static GSM_Error N6510_GetFMStatus(GSM_StateMachine *s)
{
 	unsigned char req[7] = {N6110_FRAME_HEADER, 0x0d, 0x00, 0x00, 0x01};
 	
 	if (!IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_RADIO)) return GE_NOTSUPPORTED;
 	return GSM_WaitFor (s, req, 7, 0x3E, 2, ID_GetFMStation);
}

static GSM_Error N6510_ReplyGetFMStation(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
  	unsigned char 	name[GSM_MAX_FMSTATION_LENGTH*2+2];
  	int		length;
  	GSM_Phone_Data	*Data = &s->Phone.Data;
  
	switch (msg.Buffer[3]) {
	case 0x06:
	  	smprintf(s, "Received FM station\n");
 		length = msg.Buffer[8];
 		memcpy(name,msg.Buffer+18,length*2);
 		name[length*2]	 = 0x00;
 		name[length*2+1] = 0x00;
 		CopyUnicodeString(Data->FMStation->StationName,name);
		smprintf(s,"Station name: \"%s\"\n",DecodeUnicodeString(Data->FMStation->StationName));
		N6510_DecodeFMFrequency(&Data->FMStation->Frequency, msg.Buffer+16);
		return GE_NONE;
	case 0x16:
	  	smprintf(s, "Received FM station. Empty ?\n");
		return GE_EMPTY;
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_GetFMStation (GSM_StateMachine *s, GSM_FMStation *FMStation)
{
 	GSM_Error 		error;
 	int			location;
  	unsigned char req[7] = {N6110_FRAME_HEADER, 0x05,
 				0x00, 		// location
  				0x00,0x01};
 
  	if (!IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_RADIO)) return GE_NOTSUPPORTED;
   	if (FMStation->Location > GSM_MAX_FM_STATION) return GE_INVALIDLOCATION;

  	s->Phone.Data.FMStation = FMStation;

 	error = N6510_GetFMStatus(s);
 	if (error != GE_NONE) return error;	
 
 	location = FMStation->Location-1;
  	if (s->Phone.Data.Priv.N6510.FMStatus[14+location] == 0xFF) return GE_EMPTY;
 	req[4]   = s->Phone.Data.Priv.N6510.FMStatus[14+location];

 	smprintf(s, "Getting FM Station %i\n",FMStation->Location);
 	return GSM_WaitFor (s, req, 7, 0x3E, 2, ID_GetFMStation);
}

static GSM_Error N6510_ReplySetFMStation(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
#ifdef DEBUG
 	switch (msg.Buffer[4]){
 		case 0x03: smprintf(s, "FM stations cleaned\n");	  break;		
 		case 0x11: smprintf(s, "Setting FM station status OK\n"); break;		
 		case 0x12: smprintf(s, "Setting FM station OK\n");	  break;
 	}
#endif
 	return GE_NONE;
}
  
static GSM_Error N6510_ClearFMStations (GSM_StateMachine *s)
{
	unsigned char req[7] = {N6110_FRAME_HEADER, 0x03,0x0f,0x00,0x01};
 
	if (!IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_RADIO)) return GE_NOTSUPPORTED;

	smprintf(s, "Cleaning FM Stations\n");
	return GSM_WaitFor (s, req, 7, 0x3E, 2, ID_SetFMStation);
}
 
static GSM_Error N6510_SetFMStation (GSM_StateMachine *s, GSM_FMStation *FMStation)
{
	unsigned int 		len, location;	
 	GSM_Error 		error;
 	unsigned char setstatus[36] = {N6110_FRAME_HEADER,0x11,0x00,0x01,0x01,
 	    			0x00,0x00,0x1c,0x00,0x14,0x00,0x00,
 				0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
 				0xff,0xff,0xff,0xff,0xff,0xff,0xff,0xff,
 				0xff,0xff,0xff,0xff,0xff,0x01};
 	unsigned char req[64] = {N6110_FRAME_HEADER, 0x12,0x00,0x01,0x00,
 				0x00, 			// 0x0e + (strlen(name) * 2)
 				0x00,			// strlen(name)
 				0x14,0x09,0x00,
 				0x00, 			// location
 				0x00,0x00,0x01,
 				0x00, 			// freqHi
 				0x00, 			// freqLo
 				0x01};

 	if (!IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_RADIO)) return GE_NOTSUPPORTED;
 
 	s->Phone.Data.FMStation = FMStation;
 	location = FMStation->Location-1;

 	error = N6510_GetFMStatus(s);
 	if (error != GE_NONE) return error;

 	memcpy(setstatus+14,s->Phone.Data.Priv.N6510.FMStatus+14,20);
 	setstatus [14+location] = location;
	
 	smprintf(s, "Setting FM status %i\n",FMStation->Location);
 	error = GSM_WaitFor (s, setstatus, 36 , 0x3E, 2, ID_SetFMStation);
 	if (error != GE_NONE) return error;	

 	req[12] = location;

	/* Name */
 	len 	= UnicodeLength(FMStation->StationName);
 	req[8] 	= len;
 	req[7] 	= 0x0e + len * 2;
 	memcpy (req+18,FMStation->StationName,len*2);

	/* Frequency */
	N6510_EncodeFMFrequency(FMStation->Frequency, req+16);

 	smprintf(s, "Setting FM Station %i\n",FMStation->Location);
 	return GSM_WaitFor (s, req, 0x13+len*2, 0x3E, 2, ID_SetFMStation);
}

static GSM_Error N6510_ReplySetLight(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "Light set\n");
	return GE_NONE;
}

GSM_Error N6510_SetLight(GSM_StateMachine *s, N6510_PHONE_LIGHTS light, bool enable)
{
	unsigned char req[14] = {
		N6110_FRAME_HEADER, 0x05,
		0x01,		/* 0x01 = Display, 0x03 = keypad */
		0x01,		/* 0x01 = Enable, 0x02 = disable */
		0x00, 0x00, 0x00, 0x01,
		0x05, 0x04, 0x02, 0x00};

	req[4] = light;
	if (!enable) req[5] = 0x02;
 	smprintf(s, "Setting light\n");
	return GSM_WaitFor (s, req, 14, 0x3A, 4, ID_SetLight);
}

static GSM_Error N6510_ShowStartInfo(GSM_StateMachine *s, bool enable)
{
	GSM_Error error;

	if (enable) {
		error=N6510_SetLight(s,N6510_LIGHT_DISPLAY,true);
		if (error != GE_NONE) return error;

		error=N6510_SetLight(s,N6510_LIGHT_TORCH,true);
		if (error != GE_NONE) return error;

		return N6510_SetLight(s,N6510_LIGHT_KEYPAD,true);
	} else {
		error=N6510_SetLight(s,N6510_LIGHT_DISPLAY,false);
		if (error != GE_NONE) return error;

		error=N6510_SetLight(s,N6510_LIGHT_TORCH,false);
		if (error != GE_NONE) return error;

		return N6510_SetLight(s,N6510_LIGHT_KEYPAD,false);
	}
}

static GSM_Error N6510_ReplyGetFileFolderInfo(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_File	 	*File = s->Phone.Data.FileInfo;
	GSM_Phone_N6510Data	*Priv = &s->Phone.Data.Priv.N6510;
	int			i;

	switch (msg.Buffer[3]) {
	case 0x15:
		smprintf(s,"File or folder details received\n");
		CopyUnicodeString(File->Name,msg.Buffer+10);
		if (!strncmp(DecodeUnicodeString(File->Name),"GMSTemp",7)) return GE_EMPTY;
		if (File->Name[0] == 0x00 && File->Name[1] == 0x00) return GE_UNKNOWN;

		i = msg.Buffer[8]*256+msg.Buffer[9];
		dprintf("%02x %02x %02x %02x %02x %02x %02x %02x %02x\n",
			msg.Buffer[i-5],msg.Buffer[i-4],msg.Buffer[i-3],
			msg.Buffer[i-2],msg.Buffer[i-1],msg.Buffer[i],
			msg.Buffer[i+1],msg.Buffer[i+2],msg.Buffer[i+3]);

		File->Folder 	= false;
		if (msg.Buffer[i-5] == 0x00) File->Folder 	= true;

		File->ReadOnly 	= false;
		File->Protected = false;
		File->System	= false;
		File->Hidden	= false;
		if (msg.Buffer[i+2] == 0x01) File->Protected 	= true;
		if (msg.Buffer[i+4] == 0x01) File->ReadOnly 	= true;
		if (msg.Buffer[i+5] == 0x01) File->Hidden	= true;
		if (msg.Buffer[i+6] == 0x01) File->System	= true;//fixme

		File->ModifiedEmpty = false;
		NOKIA_DecodeDateTime(s, msg.Buffer+i-22, &File->Modified);
		if (File->Modified.Year == 0x00) File->ModifiedEmpty = true;
		dprintf("%02x %02x %02x %02x\n",msg.Buffer[i-22],msg.Buffer[i-21],msg.Buffer[i-20],msg.Buffer[i-19]);

		Priv->FileToken = msg.Buffer[i-10]*256+msg.Buffer[i-9];
		Priv->ParentID  = msg.Buffer[i]*256+msg.Buffer[i+1];
		smprintf(s,"ParentID is %i\n",Priv->ParentID);

		File->Type = GSM_File_Other;
		if (msg.Length > 240){
			i = 227;
			if (msg.Buffer[i]==0x02 && msg.Buffer[i+2]==0x01)
				File->Type = GSM_File_Image_JPG;
			else if (msg.Buffer[i]==0x02 && msg.Buffer[i+2]==0x02)
				File->Type = GSM_File_Image_BMP;
			else if (msg.Buffer[i]==0x02 && msg.Buffer[i+2]==0x07)
				File->Type = GSM_File_Image_BMP;
			else if (msg.Buffer[i]==0x02 && msg.Buffer[i+2]==0x03)
				File->Type = GSM_File_Image_PNG;
			else if (msg.Buffer[i]==0x02 && msg.Buffer[i+2]==0x05)
				File->Type = GSM_File_Image_GIF;
			else if (msg.Buffer[i]==0x02 && msg.Buffer[i+2]==0x09)
				File->Type = GSM_File_Image_WBMP;
			else if (msg.Buffer[i]==0x10 && msg.Buffer[i+2]==0x01)
				File->Type = GSM_File_Java_JAR;
			else if (msg.Buffer[i]==0x04 && msg.Buffer[i+2]==0x02)
				File->Type = GSM_File_Ringtone_MIDI;
#if DEVELOP
			else if (msg.Buffer[i]==0x00 && msg.Buffer[i+2]==0x01)
				File->Type = GSM_File_MMS;
#endif
		}
		return GE_NONE;	
	case 0x2F:
		smprintf(s,"File or folder used bytes received\n");
		File->Used = msg.Buffer[6]*256*256*256+
			     msg.Buffer[7]*256*256+
			     msg.Buffer[8]*256+
			     msg.Buffer[9];
		return GE_NONE;
	case 0x33:
		if (s->Phone.Data.RequestID == ID_GetFileInfo) {
			i = Priv->FilesLocationsUsed-1;
			while (1) {
				if (i==Priv->FilesLocationsCurrent-1) break;
				dprintf("Copying %i to %i, max %i, current %i\n",
					i,i+msg.Buffer[9],
					Priv->FilesLocationsUsed,Priv->FilesLocationsCurrent);
				Priv->FilesLocations[i+msg.Buffer[9]] 	= Priv->FilesLocations[i];
				Priv->FilesLevels[i+msg.Buffer[9]]	= Priv->FilesLevels[i];
				i--;
			}
			Priv->FilesLocationsUsed += msg.Buffer[9];
			for (i=0;i<msg.Buffer[9];i++) {
				Priv->FilesLocations[Priv->FilesLocationsCurrent+i] 	= msg.Buffer[13+i*4];
				Priv->FilesLevels[Priv->FilesLocationsCurrent+i] 	= File->Level+1;
				dprintf("%i ",Priv->FilesLocations[Priv->FilesLocationsCurrent+i]);
			}
			dprintf("\n");
		}
		if (msg.Buffer[9] != 0x00) File->Folder = true;
		return GE_NONE;		
	case 0x43:
		File->CRC16 = msg.Buffer[6] * 256 + msg.Buffer[7];
		smprintf(s,"CRC16 from phone is %i\n",File->CRC16);
		return GE_NONE;
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_GetFileFolderInfo(GSM_StateMachine *s, GSM_File *File, GSM_Phone_RequestID Request)
{
	GSM_Error		error;
	unsigned char 		req[10] = {
		N7110_FRAME_HEADER,
		0x14,           /* 0x14 - info, 0x22 - free/total, 0x2E - used, 0x32 - sublocations */
		0x01,		/* 0x00 for sublocations reverse sorting, 0x01 for free */
		0x00, 0x00, 0x01,
		0x00, 0x01};	/* Folder or file number */
	unsigned char 		GetCRC[] = {
		N7110_FRAME_HEADER, 0x42, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x1E}; 	/* file ID */

	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_NOFILESYSTEM)) return GE_NOTSUPPORTED;

 	s->Phone.Data.FileInfo 	= File;
	req[8]			= atoi(File->ID_FullName) / 256;
	req[9] 			= atoi(File->ID_FullName) % 256;

	req[3] = 0x14;
	req[4] = 0x01;
	smprintf(s,"Getting info for file in filesystem\n");
	error=GSM_WaitFor (s, req, 10, 0x6D, 4, Request);
	if (error != GE_NONE) return error;

	if (Request != ID_AddFile) {
		req[3] = 0x32;
		req[4] = 0x00;
		smprintf(s,"Getting subfolders for filesystem\n");
		error=GSM_WaitFor (s, req, 10, 0x6D, 4, Request);
		if (error != GE_NONE) return error;

		if (!File->Folder) {
			req[3] = 0x2E;
			req[4] = 0x01;
			smprintf(s,"Getting used memory for file in filesystem\n");
			error=GSM_WaitFor (s, req, 10, 0x6D, 4, Request);
			if (error != GE_NONE) return error;

			GetCRC[8] = atoi(File->ID_FullName) / 256;
			GetCRC[9] = atoi(File->ID_FullName) % 256;
			smprintf(s,"Getting CRC for file in filesystem\n");
			error=GSM_WaitFor (s, GetCRC, 10, 0x6D, 4, Request);
		}
	}
	return error;
}

static GSM_Error N6510_GetNextFileFolder(GSM_StateMachine *s, GSM_File *File, bool start)
{
	GSM_Phone_N6510Data	*Priv = &s->Phone.Data.Priv.N6510;
	GSM_Error		error;

	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_NOFILESYSTEM)) return GE_NOTSUPPORTED;

	if (start) {
		Priv->FilesLocationsUsed 	= 1;
		Priv->FilesLocationsCurrent 	= 0;
		Priv->FilesLocations[0]		= 0x01;
		Priv->FilesLevels[0]		= 1;
	}

	while (1) {
		if (Priv->FilesLocationsCurrent == Priv->FilesLocationsUsed) return GE_EMPTY;

		sprintf(File->ID_FullName,"%i",Priv->FilesLocations[Priv->FilesLocationsCurrent]);
		File->Level	= Priv->FilesLevels[Priv->FilesLocationsCurrent];
		Priv->FilesLocationsCurrent++;

		error = N6510_GetFileFolderInfo(s, File, ID_GetFileInfo);
		if (error == GE_EMPTY) continue;
		return error;
	}
}

static GSM_Error N6510_ReplyGetFileSystemStatus(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	switch (msg.Buffer[3]) {
	case 0x23:
		if (!strcmp(s->Phone.Data.ModelInfo->model,"6310i")) {
			smprintf(s,"File or folder total bytes received\n");
			s->Phone.Data.FileSystemStatus->Free =
				3*256*256 + msg.Buffer[8]*256 + msg.Buffer[9] -
				s->Phone.Data.FileSystemStatus->Used;
		} else {
			smprintf(s,"File or folder free bytes received\n");
			s->Phone.Data.FileSystemStatus->Free =
					msg.Buffer[6]*256*256*256+
					msg.Buffer[7]*256*256+
					msg.Buffer[8]*256+
					msg.Buffer[9];
		}
		return GE_NONE;
	case 0x2F:
		smprintf(s,"File or folder used bytes received\n");
		s->Phone.Data.FileSystemStatus->Used =
				msg.Buffer[6]*256*256*256+
			     	msg.Buffer[7]*256*256+
			     	msg.Buffer[8]*256+
			     	msg.Buffer[9];
		return GE_NONE;
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_GetFileSystemStatus(GSM_StateMachine *s, GSM_FileSystemStatus *status)
{
	GSM_Error		error;
	unsigned char 		req[10] = {
		N7110_FRAME_HEADER,
		0x22,           /* 0x14 - info, 0x22 - free/total, 0x2E - used, 0x32 - sublocations */
		0x01,		/* 0x00 for sublocations reverse sorting, 0x01 for free */
		0x00, 0x00, 0x01,
		0x00, 0x01};	/* Folder or file number */

	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_NOFILESYSTEM)) return GE_NOTSUPPORTED;

	s->Phone.Data.FileSystemStatus = status;

	status->Free = 0;

	req[3] = 0x2E;
	req[4] = 0x01;
	smprintf(s, "Getting used/total memory in filesystem\n");
	error = GSM_WaitFor (s, req, 10, 0x6D, 4, ID_FileSystemStatus);

	req[3] = 0x22;
	req[4] = 0x01;
	smprintf(s, "Getting free memory in filesystem\n");
	return GSM_WaitFor (s, req, 10, 0x6D, 4, ID_FileSystemStatus);
}

static GSM_Error N6510_SearchForFileName(GSM_StateMachine *s, GSM_File *File)
{
	GSM_File		File2;
	GSM_Error		error;
	int 			FilesLocations[500],FilesLocations2[500];
	int 			FilesLevels[500];
	int 			FilesLocationsUsed, FilesLocationsCurrent;
	int 			FilesLocationsUsed2, FilesLocationsCurrent2;
	GSM_Phone_N6510Data	*Priv = &s->Phone.Data.Priv.N6510;

	memcpy(FilesLocations,	Priv->FilesLocations,	sizeof(FilesLocations));
	memcpy(FilesLevels,	Priv->FilesLevels,	sizeof(FilesLevels));
	FilesLocationsUsed 	= Priv->FilesLocationsUsed;
	FilesLocationsCurrent 	= Priv->FilesLocationsCurrent;

	Priv->FilesLocationsUsed 	= 1;
	Priv->FilesLocationsCurrent 	= 1;
	Priv->FilesLocations[0]		= atoi(File->ID_FullName);
	Priv->FilesLevels[0]		= 1;

	strcpy(File2.ID_FullName,File->ID_FullName);
	error = N6510_GetFileFolderInfo(s, &File2, ID_GetFileInfo);
	memcpy(FilesLocations2,		Priv->FilesLocations,	sizeof(FilesLocations2));
	FilesLocationsUsed2 		= Priv->FilesLocationsUsed;
	FilesLocationsCurrent2 		= Priv->FilesLocationsCurrent;

	memcpy(Priv->FilesLocations,	FilesLocations,		sizeof(FilesLocations));
	memcpy(Priv->FilesLevels,	FilesLevels,		sizeof(FilesLevels));
	Priv->FilesLocationsUsed 	= FilesLocationsUsed;
	Priv->FilesLocationsCurrent 	= FilesLocationsCurrent;
	if (error != GE_NONE) return error;

	while (1) {
		if (FilesLocationsCurrent2 == FilesLocationsUsed2) return GE_EMPTY;

		sprintf(File2.ID_FullName,"%i",FilesLocations2[FilesLocationsCurrent2]);
		dprintf("Current is %i\n",FilesLocations2[FilesLocationsCurrent2]);
		FilesLocationsCurrent2++;

		error = N6510_GetFileFolderInfo(s, &File2, ID_AddFile);
		if (error == GE_EMPTY) continue;
		if (error != GE_NONE) return error;
		dprintf("%s %s\n",DecodeUnicodeString(File->Name),DecodeUnicodeString(File2.Name));
		if (mywstrncasecmp(File2.Name,File->Name,0)) return GE_NONE;
	}
	return GE_EMPTY;
}

static GSM_Error N6510_ReplyGetFilePart(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	int old;

	smprintf(s,"File part received\n");
	old = s->Phone.Data.File->Used;
	s->Phone.Data.File->Used += msg.Buffer[6]*256*256*256+
			    	    msg.Buffer[7]*256*256+
			    	    msg.Buffer[8]*256+
			    	    msg.Buffer[9];
	smprintf(s,"Length of file part: %i\n",
			msg.Buffer[6]*256*256*256+
			msg.Buffer[7]*256*256+
			msg.Buffer[8]*256+
			msg.Buffer[9]);
	s->Phone.Data.File->Buffer = (unsigned char *)realloc(s->Phone.Data.File->Buffer,s->Phone.Data.File->Used);
	memcpy(s->Phone.Data.File->Buffer+old,msg.Buffer+10,s->Phone.Data.File->Used-old);
	return GE_NONE;
}

static GSM_Error N6510_GetFilePart(GSM_StateMachine *s, GSM_File *File)
{
	int 			old;
	GSM_Error		error;
	unsigned char 		req[] = {
		N7110_FRAME_HEADER, 0x0E, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x01,	/* Folder or file number */
		0x00, 0x00,
		0x00, 0x00,	/* Start from xxx byte */
		0x00, 0x00,
		0x03, 0xE8};	/* Read xxx bytes */

	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_NOFILESYSTEM)) return GE_NOTSUPPORTED;

	if (File->Used == 0x00) {
		error = N6510_GetFileFolderInfo(s, File, ID_GetFile);
		if (error != GE_NONE) return error;
		File->Used = 0;
	}

	req[8] 			= atoi(File->ID_FullName) / 256;
	req[9] 			= atoi(File->ID_FullName) % 256;
	old			= File->Used;
	req[12] 		= old / 256;
	req[13] 		= old % 256;

	s->Phone.Data.File 	= File;
	smprintf(s, "Getting file part from filesystem\n");
	error=GSM_WaitFor (s, req, 18, 0x6D, 4, ID_GetFile);
	if (error != GE_NONE) return error;
	if (File->Used - old != (0x03 * 256 + 0xE8)) return GE_EMPTY;
	return GE_NONE;
}

static GSM_Error N6510_SetReadOnly(GSM_StateMachine *s, unsigned char *ID, bool enable)
{
	unsigned char SetAttr[] = {
		N7110_FRAME_HEADER, 0x18, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x20};		/* File ID */

	if (!enable) SetAttr[4] = 0x06;

	SetAttr[8] = atoi(ID) / 256;
	SetAttr[9] = atoi(ID) % 256;
	smprintf(s, "Setting readonly attribute\n");
	return GSM_WaitFor (s, SetAttr, 10, 0x6D, 4, ID_DeleteFile);
}

static GSM_Error N6510_ReplyAddFileHeader(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	switch (msg.Buffer[3]) {
	case 0x03:
		smprintf(s,"File header added\n");
		sprintf(s->Phone.Data.File->ID_FullName,"%i",msg.Buffer[9]);
		return GE_NONE;
	case 0x13:
		return GE_NONE;
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_ReplyAddFilePart(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	return GE_NONE;
}

static GSM_Error N6510_AddFilePart(GSM_StateMachine *s, GSM_File *File, int *Pos)
{
	GSM_Phone_N6510Data	*Priv = &s->Phone.Data.Priv.N6510;
	GSM_File		File2;
	GSM_Error		error;
	int			j;
	unsigned char 		Header[400] = {
		N7110_FRAME_HEADER, 0x02, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x0C, 		/* parent folder ID */
 		0x00, 0x00, 0x00, 0xE8};
	unsigned char		Add[15000] = {
		N7110_FRAME_HEADER, 0x40, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x04, 		/* file ID */
		0x00, 0x00, 
		0x01, 0x28}; 		/* length */
	unsigned char end[30] = {
		N7110_FRAME_HEADER, 0x40, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x04, 		/* file ID */
		0x00, 0x00, 0x00, 0x00};

	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_NOFILESYSTEM)) return GE_NOTSUPPORTED;

	s->Phone.Data.File = File;

	if (*Pos == 0) {
		error = N6510_SearchForFileName(s,File);
		if (error == GE_NONE) return GE_INVALIDLOCATION;
		if (error != GE_EMPTY) return error;

		Header[8] = atoi(File->ID_FullName) / 256;
		Header[9] = atoi(File->ID_FullName) % 256;
		memset(Header+14, 0x00, 300);
		CopyUnicodeString(Header+14,File->Name);
		Header[224] = File->Used / 256;
		Header[225] = File->Used % 256;
		switch(File->Type) {
			case GSM_File_Image_JPG    : Header[231]=0x02; Header[233]=0x01; break;
			case GSM_File_Image_BMP    : Header[231]=0x02; Header[233]=0x02; break;
			case GSM_File_Image_PNG    : Header[231]=0x02; Header[233]=0x03; break;
			case GSM_File_Image_GIF    : Header[231]=0x02; Header[233]=0x05; break;
			case GSM_File_Image_WBMP   : Header[231]=0x02; Header[233]=0x09; break;
			case GSM_File_Ringtone_MIDI: Header[231]=0x04; Header[233]=0x05; break; //Header[238]=0x01; 
			case GSM_File_Java_JAR     : Header[231]=0x10; Header[233]=0x01; break;
#ifdef DEVELOP
			case GSM_File_MMS:
				Header[214]=0x07;
				Header[215]=0xd3;
				Header[216]=0x06;
				Header[217]=0x01;
				Header[218]=0x12;
				Header[219]=0x13;
				Header[220]=0x29;
				Header[233]=0x01;
				break;
#endif
			default                    : Header[231]=0x01; Header[233]=0x05;
		}
		Header[235] = 0x01;
		Header[236] = atoi(File->ID_FullName) / 256;
		Header[237] = atoi(File->ID_FullName) % 256;
		if (File->Protected) Header[238] = 0x01; //Nokia forward lock
		if (File->Hidden)    Header[241] = 0x01;
		if (File->System)    Header[242] = 0x01; //fixme
		smprintf(s, "Adding file header\n");
		error=GSM_WaitFor (s, Header, 246, 0x6D, 4, ID_AddFile);
		if (error != GE_NONE) return error;
	}

	j = 1000;
	if (File->Used - *Pos < 1000) j = File->Used - *Pos;
	Add[ 8] = atoi(File->ID_FullName) / 256;
	Add[ 9] = atoi(File->ID_FullName) % 256;
	Add[12] = j / 256;
	Add[13] = j % 256;
	memcpy(Add+14,File->Buffer+(*Pos),j);
	smprintf(s, "Adding file part %i %i\n",*Pos,j);
	error=GSM_WaitFor (s, Add, 14+j, 0x6D, 4, ID_AddFile);
	if (error != GE_NONE) return error;
	*Pos = *Pos + j;

	if (j < 1000) {
		end[8] = atoi(File->ID_FullName) / 256;
		end[9] = atoi(File->ID_FullName) % 256;
		smprintf(s, "Frame for ending adding file\n");
		error = GSM_WaitFor (s, end, 14, 0x6D, 4, ID_AddFile);
		if (error != GE_NONE) return error;

		strcpy(File2.ID_FullName,File->ID_FullName);
		error = N6510_GetFileFolderInfo(s, &File2, ID_GetFileInfo);
		if (error != GE_NONE) return error;

		if (!File->ModifiedEmpty) {
			Header[3]   = 0x12;
			Header[4]   = 0x01;
			Header[12]  = 0x00;
			Header[13]  = 0xE8;
			Header[8]   = atoi(File->ID_FullName) / 256;
			Header[9]   = atoi(File->ID_FullName) % 256;
			memset(Header+14, 0x00, 300);
			CopyUnicodeString(Header+14,File->Name);
			NOKIA_EncodeDateTime(s,Header+214,&File->Modified);
			/* When you save too big file for phone and it changes
                         * size (some part is cut by firmware), you HAVE to write
                         * here correct file size. In other case filesystem
                         * will be damaged
                         */
			Header[224] = File2.Used / 256;
			Header[225] = File2.Used % 256;
			Header[226] = Priv->FileToken / 256;
			Header[227] = Priv->FileToken % 256;
			switch(File->Type) {
			case GSM_File_Image_JPG    : Header[231]=0x02; Header[233]=0x01; break;
			case GSM_File_Image_BMP    : Header[231]=0x02; Header[233]=0x02; break;
			case GSM_File_Image_PNG    : Header[231]=0x02; Header[233]=0x03; break;
			case GSM_File_Image_GIF    : Header[231]=0x02; Header[233]=0x05; break;
			case GSM_File_Image_WBMP   : Header[231]=0x02; Header[233]=0x09; break;
			case GSM_File_Ringtone_MIDI: Header[231]=0x04; Header[233]=0x05; break; //Header[238]=0x01; 
			case GSM_File_Java_JAR     : Header[231]=0x10; Header[233]=0x01; break;
#ifdef DEVELOP
			case GSM_File_MMS:
				Header[214]=0x07;
				Header[215]=0xd3;
				Header[216]=0x06;
				Header[217]=0x01;
				Header[218]=0x12;
				Header[219]=0x13;
				Header[220]=0x29;
				Header[233]=0x01;
				break;
#endif
			default                    : Header[231]=0x01; Header[233]=0x05;
			}
			Header[235] = 0x01;
			Header[236] = Priv->ParentID / 256;
			Header[237] = Priv->ParentID % 256;
			smprintf(s, "Adding file header\n");
			error=GSM_WaitFor (s, Header, 246, 0x6D, 4, ID_AddFile);
			if (error != GE_NONE) return error;
		}

		/* Can't delete from phone menu */
		if (File->ReadOnly) {
			error = N6510_SetReadOnly(s, File->ID_FullName, true);
			if (error != GE_NONE) return error;
		}

		if (File2.CRC16 != File->CRC16) {
			smprintf(s,"File2 CRC is %i, File CRC is %i\n",File2.CRC16,File->CRC16);
//			return GE_UNKNOWN;
		}

		return GE_EMPTY;
	}

	return GE_NONE;
}

static GSM_Error N6510_ReplyDeleteFile(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	return GE_NONE;
}

static GSM_Error N6510_DeleteFile(GSM_StateMachine *s, unsigned char *ID)
{
	GSM_Error	error;
	unsigned char 	Delete[40] = {
		N7110_FRAME_HEADER, 0x1E, 0x00, 0x00, 0x00, 0x01, 
		0x00, 0x35};		/* File ID */

	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_NOFILESYSTEM)) return GE_NOTSUPPORTED;

	error = N6510_SetReadOnly(s, ID, false);
	if (error != GE_NONE) return error;

	Delete[8] = atoi(ID) / 256;
	Delete[9] = atoi(ID) % 256;
	return GSM_WaitFor (s, Delete, 10, 0x6D, 4, ID_DeleteFile);
}

static GSM_Error N6510_ReplyAddFolder(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	sprintf(s->Phone.Data.File->ID_FullName,"%i",msg.Buffer[9]);
	return GE_NONE;
}

static GSM_Error N6510_AddFolder(GSM_StateMachine *s, GSM_File *File)
{
	GSM_Error	error;
	unsigned char Header[400] = {
		N7110_FRAME_HEADER, 0x04, 0x00, 0x00, 0x00, 0x01,
		0x00, 0x0C, 		/* parent folder ID */
 		0x00, 0x00, 0x00, 0xE8};

	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_NOFILESYSTEM)) return GE_NOTSUPPORTED;

	error = N6510_SearchForFileName(s,File);
	if (error == GE_NONE) return GE_INVALIDLOCATION;
	if (error != GE_EMPTY) return error;

	Header[8] = atoi(File->ID_FullName) / 256;
	Header[9] = atoi(File->ID_FullName) % 256;
	memset(Header+14, 0x00, 300);
	CopyUnicodeString(Header+14,File->Name);
	Header[233] = 0x02;
	Header[235] = 0x01;
	Header[236] = atoi(File->ID_FullName) / 256;
	Header[237] = atoi(File->ID_FullName) % 256;
	
	s->Phone.Data.File = File;
	smprintf(s, "Adding folder\n");
	error = GSM_WaitFor (s, Header, 246, 0x6D, 4, ID_AddFolder);
	if (error != GE_NONE) return error;

	/* Can't delete from phone menu */
	if (File->ReadOnly) {
		error = N6510_SetReadOnly(s, File->ID_FullName, true);
		if (error != GE_NONE) return error;
	}

	return error;
}

#ifdef DEVELOP

static GSM_Error N6510_ReplyEnableGPRSAccessPoint(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	if (msg.Buffer[13] == 0x02) return GE_NONE;
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_EnableGPRSAccessPoint(GSM_StateMachine *s)
{
	GSM_Error	error;
	int	 	i;
	unsigned char 	req[] = {
		N7110_FRAME_HEADER, 0x05, 0x00, 0x00, 0x00, 0x2C, 0x00,
		0x04, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00};

	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_NOGPRSPOINT)) return GE_NOTSUPPORTED;

	for (i=0;i<3;i++) {
		smprintf(s, "Activating full GPRS access point support\n");
		error = GSM_WaitFor (s, req, 16, 0x43, 4, ID_EnableGPRSPoint);
		if (error != GE_NONE) return error;
	}
	return error;
}

#endif

static GSM_Error N6510_ReplyGetGPRSAccessPoint(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_GPRSAccessPoint *point = s->Phone.Data.GPRSPoint;

	switch (msg.Buffer[13]) {
	case 0x01:
		smprintf(s,"Active GPRS point received\n");
		point->Active = false;
		if (point->Location == msg.Buffer[18]) point->Active = true;
		return GE_NONE;
	case 0xD2:
		smprintf(s,"Names for GPRS points received\n");
		CopyUnicodeString(point->Name,msg.Buffer+18+(point->Location-1)*42);
		smprintf(s,"\"%s\"\n",DecodeUnicodeString(point->Name));
		return GE_NONE;
	case 0xF2:
		smprintf(s,"URL for GPRS points received\n");
		CopyUnicodeString(point->URL,msg.Buffer+18+(point->Location-1)*202);
		smprintf(s,"\"%s\"\n",DecodeUnicodeString(point->URL));
		return GE_NONE;
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_GetGPRSAccessPoint(GSM_StateMachine *s, GSM_GPRSAccessPoint *point)
{
	GSM_Error	error;
	unsigned char 	URL[] = {
		N7110_FRAME_HEADER, 0x05, 0x00, 0x00, 0x00, 0x2C, 0x00,
		0x00, 0x00, 0x00, 0x03, 0xF2, 0x00, 0x00};
	unsigned char 	Name[] = {
		N7110_FRAME_HEADER, 0x05, 0x00, 0x00, 0x00, 0x2C, 0x00,
		0x01, 0x00, 0x00, 0x00, 0xD2, 0x00, 0x00};
	unsigned char 	Active[] = {
		N7110_FRAME_HEADER, 0x05, 0x00, 0x00, 0x00, 0x2C, 0x00,
		0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};

	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_NOGPRSPOINT)) return GE_NOTSUPPORTED;
	if (point->Location < 1) return GE_UNKNOWN;
	if (point->Location > 5) return GE_INVALIDLOCATION;

	s->Phone.Data.GPRSPoint = point;

#ifdef DEVELOP
	error = N6510_EnableGPRSAccessPoint(s);
	if (error != GE_NONE) return error;
#endif

	smprintf(s, "Getting GPRS access point name\n");
	error=GSM_WaitFor (s, Name, 16, 0x43, 4, ID_GetGPRSPoint);
	if (error != GE_NONE) return error;

	smprintf(s, "Getting GPRS access point URL\n");
	error=GSM_WaitFor (s, URL, 16, 0x43, 4, ID_GetGPRSPoint);
	if (error != GE_NONE) return error;

	smprintf(s, "Getting number of active GPRS access point\n");
	error=GSM_WaitFor (s, Active, 16, 0x43, 4, ID_GetGPRSPoint);
	if (error != GE_NONE) return error;

	if (UnicodeLength(point->URL)==0 && UnicodeLength(point->Name)==0) return GE_EMPTY;
	return error;
}

static GSM_Error N6510_ReplySetGPRSAccessPoint1(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	switch (msg.Buffer[13]) {
	case 0x01:
	case 0xD2:
	case 0xF2:
		memcpy(s->Phone.Data.Priv.N6510.GPRSPoints,msg.Buffer,msg.Length);
		s->Phone.Data.Priv.N6510.GPRSPointsLength = msg.Length;
		return GE_NONE;
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_SetGPRSAccessPoint(GSM_StateMachine *s, GSM_GPRSAccessPoint *point)
{
	unsigned char	*buff = s->Phone.Data.Priv.N6510.GPRSPoints;
	GSM_Error	error;
	unsigned char 	URL[] = {
		N7110_FRAME_HEADER, 0x05, 0x00, 0x00, 0x00, 0x2C, 0x00,
		0x00, 0x00, 0x00, 0x03, 0xF2, 0x00, 0x00};
	unsigned char 	Name[] = {
		N7110_FRAME_HEADER, 0x05, 0x00, 0x00, 0x00, 0x2C, 0x00,
		0x01, 0x00, 0x00, 0x00, 0xD2, 0x00, 0x00};
	unsigned char 	Active[] = {
		N7110_FRAME_HEADER, 0x05, 0x00, 0x00, 0x00, 0x2C, 0x00,
		0x02, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00};

	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_NOGPRSPOINT)) return GE_NOTSUPPORTED;
	if (point->Location < 1) return GE_UNKNOWN;
	if (point->Location > 5) return GE_INVALIDLOCATION;

	s->Phone.Data.GPRSPoint = point;

#ifdef DEVELOP
	error = N6510_EnableGPRSAccessPoint(s);
	if (error != GE_NONE) return error;
#endif

	smprintf(s, "Getting GPRS access point name\n");
	error=GSM_WaitFor (s, Name, 16, 0x43, 4, ID_SetGPRSPoint);
	if (error != GE_NONE) return error;
	CopyUnicodeString(buff+18+(point->Location-1)*42,point->Name);
	buff[0] = 0x00;
	buff[1] = 0x01;
	buff[2] = 0x01;
	buff[3] = 0x07;
	smprintf(s, "Setting GPRS access point name\n");
	error=GSM_WaitFor (s, buff, s->Phone.Data.Priv.N6510.GPRSPointsLength, 0x43, 4, ID_SetGPRSPoint);
	if (error != GE_NONE) return error;

	smprintf(s, "Getting GPRS access point URL\n");
	error=GSM_WaitFor (s, URL, 16, 0x43, 4, ID_SetGPRSPoint);
	if (error != GE_NONE) return error;
	CopyUnicodeString(buff+18+(point->Location-1)*42,point->URL);
	buff[0] = 0x00;
	buff[1] = 0x01;
	buff[2] = 0x01;
	buff[3] = 0x07;
	smprintf(s, "Setting GPRS access point URL\n");
	error=GSM_WaitFor (s, buff, s->Phone.Data.Priv.N6510.GPRSPointsLength, 0x43, 4, ID_SetGPRSPoint);
	if (error != GE_NONE) return error;

	if (point->Active) {
		smprintf(s, "Getting number of active GPRS access point\n");
		error=GSM_WaitFor (s, Active, 16, 0x43, 4, ID_SetGPRSPoint);
		if (error != GE_NONE) return error;
		buff[0] = 0x00;
		buff[1] = 0x01;
		buff[2] = 0x01;
		buff[3] = 0x07;
		buff[18]= point->Location;
		smprintf(s, "Setting number of active GPRS access point\n");
		error=GSM_WaitFor (s, buff, s->Phone.Data.Priv.N6510.GPRSPointsLength, 0x43, 4, ID_SetGPRSPoint);
		if (error != GE_NONE) return error;
	}

	return error;
}

/* ToDo support - 6310 style */
static GSM_Error N6510_ReplyGetToDoStatus1(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	int				i;
	GSM_NOKIACalToDoLocations	*Last = &s->Phone.Data.Priv.N6510.LastToDo;

	smprintf(s, "TODO locations received\n");
	Last->Number=msg.Buffer[6]*256+msg.Buffer[7];
	smprintf(s, "Number of Entries: %i\n",Last->Number);
	smprintf(s, "Locations: ");
	for (i=0;i<Last->Number;i++) {
		Last->Location[i]=msg.Buffer[12+(i*4)]*256+msg.Buffer[(i*4)+13];
		smprintf(s, "%i ",Last->Location[i]);
	}
	smprintf(s, "\n");
	return GE_NONE;
}

/* ToDo support - 6310 style */
static GSM_Error N6510_GetToDoStatus1(GSM_StateMachine *s, GSM_ToDoStatus *status)
{
	GSM_Error 			error;
	GSM_NOKIACalToDoLocations	*LastToDo = &s->Phone.Data.Priv.N6510.LastToDo;
	unsigned char reqLoc[] = {
			N6110_FRAME_HEADER,
			0x15, 0x01, 0x00, 0x00,
			0x00, 0x00, 0x00};

	smprintf(s, "Getting ToDo locations\n");
	error = GSM_WaitFor (s, reqLoc, 10, 0x55, 4, ID_GetToDo);
	if (error != GE_NONE) return error;

	status->Used = LastToDo->Number;
	return GE_NONE;
}

static GSM_Error N6510_GetToDoStatus2(GSM_StateMachine *s, GSM_ToDoStatus *status)
{
	GSM_NOKIACalToDoLocations	*LastToDo = &s->Phone.Data.Priv.N6510.LastToDo;
	GSM_Error			error;

	error = N6510_GetCalendarInfo3(s,LastToDo,false);
	if (error!=GE_NONE) return error;

	status->Used = LastToDo->Number;
	return GE_NONE;
}

static GSM_Error N6510_GetToDoStatus(GSM_StateMachine *s, GSM_ToDoStatus *status)
{
	status->Used = 0;

	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_TODO63)) {
		return N6510_GetToDoStatus1(s, status);
	} else if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_TODO66)) {
		return N6510_GetToDoStatus2(s, status);
	} else {
		return GE_NOTSUPPORTED;
	}
}

/* ToDo support - 6310 style */
static GSM_Error N6510_ReplyGetToDo1(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_ToDoEntry *Last = s->Phone.Data.ToDo;

	smprintf(s, "TODO received method 1\n");

	switch (msg.Buffer[4]) {
		case 1  : Last->Priority = GSM_Priority_High; 	break;
		case 2  : Last->Priority = GSM_Priority_Medium; break;
		case 3  : Last->Priority = GSM_Priority_Low; 	break;
		default	: return GE_UNKNOWN;
	}	
	smprintf(s, "Priority: %i\n",msg.Buffer[4]);

	CopyUnicodeString(Last->Entries[0].Text,msg.Buffer+14);
 	Last->Entries[0].EntryType = TODO_TEXT;
	Last->EntriesNum		 = 1;
	smprintf(s, "Text: \"%s\"\n",DecodeUnicodeString(Last->Entries[0].Text));

	return GE_NONE;
}

/* ToDo support - 6310 style */
static GSM_Error N6510_GetToDo1(GSM_StateMachine *s, GSM_ToDoEntry *ToDo, bool refresh)
{
	GSM_Error 			error;
	GSM_ToDoStatus 			status;
	GSM_NOKIACalToDoLocations	*LastToDo = &s->Phone.Data.Priv.N6510.LastToDo;
	unsigned char reqGet[] = {
			N6110_FRAME_HEADER,
			0x03, 0x00, 0x00, 0x80, 0x00,
			0x00, 0x17};		/* Location */

	if (refresh) {
		error = N6510_GetToDoStatus(s, &status);
		if (error != GE_NONE) return error;
	}
	if (ToDo->Location > LastToDo->Number) return GE_INVALIDLOCATION;
	reqGet[8] = LastToDo->Location[ToDo->Location-1] / 256;
	reqGet[9] = LastToDo->Location[ToDo->Location-1] % 256;
	s->Phone.Data.ToDo = ToDo;	
	smprintf(s, "Getting ToDo\n");
	return GSM_WaitFor (s, reqGet, 10, 0x55, 4, ID_GetToDo);
}

static GSM_Error N6510_ReplyGetToDoStatus2(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	return N6510_ReplyGetCalendarInfo3(msg, s, &s->Phone.Data.Priv.N6510.LastToDo);
}

/* Similiar to getting calendar method 3 */
static GSM_Error N6510_ReplyGetToDo2(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_ToDoEntry 		*Last = s->Phone.Data.ToDo;
	GSM_DateTime		Date;
	unsigned long		diff;

	smprintf(s, "ToDo received method 2\n");

	switch (msg.Buffer[44]) {
		case 0x10: Last->Priority = GSM_Priority_Low; 		break;
		case 0x20: Last->Priority = GSM_Priority_Medium; 	break;
		case 0x30: Last->Priority = GSM_Priority_High; 		break;
		default	 : return GE_UNKNOWN;
	}	

	memcpy(Last->Entries[0].Text,msg.Buffer+54,msg.Buffer[51]*2);
	Last->Entries[0].Text[msg.Buffer[51]*2]   = 0;
	Last->Entries[0].Text[msg.Buffer[51]*2+1] = 0;
    	Last->Entries[0].EntryType = TODO_TEXT;
	smprintf(s, "Text: \"%s\"\n",DecodeUnicodeString(Last->Entries[0].Text));

	smprintf(s,"EndTime: %04i-%02i-%02i %02i:%02i\n",
		msg.Buffer[34]*256+msg.Buffer[35],
		msg.Buffer[36],msg.Buffer[37],msg.Buffer[38],
		msg.Buffer[39]);
	Date.Year 	= msg.Buffer[34]*256+msg.Buffer[35];
	Date.Month 	= msg.Buffer[36];
	Date.Day 	= msg.Buffer[37];
	Date.Hour 	= msg.Buffer[38];
	Date.Minute 	= msg.Buffer[39];
	Date.Second	= 0;
	Last->Entries[1].EntryType = TODO_END_DATETIME;
	memcpy(&Last->Entries[1].Date,&Date,sizeof(GSM_DateTime));

	smprintf(s,"StartTime: %04i-%02i-%02i %02i:%02i\n",
		msg.Buffer[28]*256+msg.Buffer[29],
		msg.Buffer[30],msg.Buffer[31],msg.Buffer[32],
		msg.Buffer[33]);
	Date.Year 	= msg.Buffer[28]*256+msg.Buffer[29];	
	Date.Month 	= msg.Buffer[30];
	Date.Day 	= msg.Buffer[31];
	Date.Hour 	= msg.Buffer[32];
	Date.Minute 	= msg.Buffer[33];
	Date.Second	= 0;

	Last->EntriesNum = 2;

	if (msg.Buffer[45] == 0x01) {
		Last->Entries[2].Number		= msg.Buffer[45];
	    	Last->Entries[2].EntryType 	= TODO_COMPLETED;
		Last->EntriesNum++;
		smprintf(s,"Completed\n");
	}

	if (msg.Buffer[14] == 0xFF && msg.Buffer[15] == 0xFF && msg.Buffer[16] == 0xff && msg.Buffer[17] == 0xff)
	{
		smprintf(s, "No alarm\n");
	} else {
		diff  = ((unsigned int)msg.Buffer[14]) << 24;
		diff += ((unsigned int)msg.Buffer[15]) << 16;
		diff += ((unsigned int)msg.Buffer[16]) << 8;
		diff += msg.Buffer[17];

		memcpy(&Last->Entries[Last->EntriesNum].Date,&Date,sizeof(GSM_DateTime));
		GetTimeDifference(diff, &Last->Entries[Last->EntriesNum].Date, false, 60);
		smprintf(s, "Alarm date   : %02i-%02i-%04i %02i:%02i:%02i\n",
			Last->Entries[Last->EntriesNum].Date.Day,   Last->Entries[Last->EntriesNum].Date.Month,
			Last->Entries[Last->EntriesNum].Date.Year,  Last->Entries[Last->EntriesNum].Date.Hour,
			Last->Entries[Last->EntriesNum].Date.Minute,Last->Entries[Last->EntriesNum].Date.Second);

		Last->Entries[Last->EntriesNum].EntryType = TODO_ALARM_DATETIME;
		if (msg.Buffer[22]==0x00 && msg.Buffer[23]==0x00 &&
		    msg.Buffer[24]==0x00 && msg.Buffer[25]==0x00)
		{
			Last->Entries[Last->EntriesNum].EntryType = TODO_SILENT_ALARM_DATETIME;
			smprintf(s, "Alarm type   : Silent\n");
		}
		Last->EntriesNum++;
	}
	
	return GE_NONE;
}

/* ToDo support - 6610 style */
static GSM_Error N6510_GetToDo2(GSM_StateMachine *s, GSM_ToDoEntry *ToDo, bool refresh)
{
	GSM_Error 			error;
	GSM_NOKIACalToDoLocations	*LastToDo = &s->Phone.Data.Priv.N6510.LastToDo;	
	/* The same to getting calendar method 3 */
	unsigned char 			req[] = { 
		N6110_FRAME_HEADER,0x7D,0x00,0x00,0x00,0x00,
		0x00,0x99,			/* Location */
		0xff,0xff,0xff,0xff,0x01};	

	if (refresh) {
		error=N6510_GetCalendarInfo3(s,LastToDo,false);
		if (error!=GE_NONE) return error;
	}
	if (ToDo->Location > LastToDo->Number) return GE_INVALIDLOCATION;

	req[8] = LastToDo->Location[ToDo->Location-1] >> 8;
	req[9] = LastToDo->Location[ToDo->Location-1] & 0xff;
                        
	s->Phone.Data.ToDo = ToDo;	
	smprintf(s, "Getting todo method 2\n");
	return GSM_WaitFor (s, req, 15, 0x13, 4, ID_GetToDo);
}

static GSM_Error N6510_GetToDo(GSM_StateMachine *s, GSM_ToDoEntry *ToDo, bool refresh)
{
	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_TODO63)) {
		return N6510_GetToDo1(s, ToDo, refresh);
	} else if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_TODO66)) {
		return N6510_GetToDo2(s, ToDo, refresh);
	} else {
		return GE_NOTSUPPORTED;
	}
}

/* ToDo support - 6310 style */
static GSM_Error N6510_ReplyDeleteAllToDo1(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "All TODO deleted\n");
	return GE_NONE;
}

/* ToDo support - 6310 style */
static GSM_Error N6510_DeleteAllToDo1(GSM_StateMachine *s)
{
	unsigned char req[] = {N6110_FRAME_HEADER, 0x11};

	smprintf(s, "Deleting all ToDo method 1\n");
	return GSM_WaitFor (s, req, 4, 0x55, 4, ID_DeleteAllToDo);
}

static GSM_Error N6510_DeleteAllToDo2(GSM_StateMachine *s)
{
	GSM_Error 			error;
	GSM_NOKIACalToDoLocations	*LastToDo = &s->Phone.Data.Priv.N6510.LastToDo;	
	int				i;
	GSM_CalendarEntry		Note;

	error=N6510_GetCalendarInfo3(s,LastToDo,false);
	if (error!=GE_NONE) return error;

	smprintf(s, "Deleting all ToDo method 2\n");

	for (i=0;i<LastToDo->Number;i++) {
		Note.Location = LastToDo->Location[i];
		error = N71_65_DelCalendar(s,&Note);
		if (error!=GE_NONE) return error;
	}
	return GE_NONE;
}

static GSM_Error N6510_DeleteAllToDo(GSM_StateMachine *s)
{
	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_TODO63)) {
		return N6510_DeleteAllToDo1(s);
	} else if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_TODO66)) {
		return N6510_DeleteAllToDo2(s);
	} else {
		return GE_NOTSUPPORTED;
	}
}

/* ToDo support - 6310 style */
static GSM_Error N6510_ReplyGetToDoFirstLoc1(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "TODO first location received method 1: %02x\n",msg.Buffer[9]);
	s->Phone.Data.ToDo->Location = msg.Buffer[9];
	return GE_NONE;
}

/* ToDo support - 6310 style */
static GSM_Error N6510_ReplySetToDo1(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "TODO set OK\n");
	return GE_NONE;
}

/* ToDo support - 6310 style */
static GSM_Error N6510_SetToDo1(GSM_StateMachine *s, GSM_ToDoEntry *ToDo)
{
 	int 			Text, Alarm, EndTime, Completed, ulen;
	GSM_Error		error;
	unsigned char 		reqLoc[] 	= {N6110_FRAME_HEADER, 0x0F};
	unsigned char 		reqSet[500] 	= {
		N6110_FRAME_HEADER, 0x01,
		0x03,		/* Priority */
		0x00,		/* Length of text */
		0x80,0x00,0x00,
		0x18};		/* Location */

	s->Phone.Data.ToDo = ToDo;

	if (ToDo->Location == 0) {
		smprintf(s, "Getting first ToDo location\n");
		error = GSM_WaitFor (s, reqLoc, 4, 0x55, 4, ID_SetToDo);
		if (error != GE_NONE) return error;
		reqSet[9] = ToDo->Location;
	} else {
		return GE_NOTSUPPORTED;
	}

	switch (ToDo->Priority) {
		case GSM_Priority_Low	: reqSet[4] = 3; break;
		case GSM_Priority_Medium: reqSet[4] = 2; break;
		case GSM_Priority_High	: reqSet[4] = 1; break;
	}

	GSM_ToDoFindDefaultTextTimeAlarmCompleted(ToDo, &Text, &Alarm, &Completed, &EndTime);

    	if (Text == -1) return GE_NOTSUPPORTED; /* XXX: shouldn't this be handled different way? */
    	ulen = UnicodeLength(ToDo->Entries[Text].Text);
	reqSet[5] = ulen+1;
	CopyUnicodeString(reqSet+10,ToDo->Entries[Text].Text);
	reqSet[10+ulen*2] 	= 0x00;
	reqSet[10+ulen*2+1] 	= 0x00;
	smprintf(s, "Adding ToDo method 1\n");
	return GSM_WaitFor (s, reqSet, 12+ulen*2, 0x55, 4, ID_SetToDo);
}

static GSM_Error N6510_ReplyAddToDo2(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "ToDo added method 2\n");
	return GE_NONE;
}

static GSM_Error N6510_ReplyGetToDoFirstLoc2(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s, "First ToDo location method 2: %i\n",msg.Buffer[8]*256+msg.Buffer[9]);
	s->Phone.Data.ToDo->Location = msg.Buffer[9];
	return GE_NONE;
}

static GSM_Error N6510_SetToDo2(GSM_StateMachine *s, GSM_ToDoEntry *ToDo)
{
	GSM_CalendarEntry	Note;
	time_t     		t_time1,t_time2;
	long			diff;
 	GSM_Error		error;
	GSM_DateTime		DT;
 	int 			Text, Alarm, EndTime, Completed, count=54;
	unsigned char 		reqLoc[] = {N6110_FRAME_HEADER, 0x95, 0x01};
	unsigned char 		req[5000] = {
		N6110_FRAME_HEADER, 0x65,
		0x01,					/* 0 = calendar, 1 = todo 	*/
		0x00, 0x00, 0x00,
		0x00, 0x00,                             /* location 	    		*/
		0x00, 0x00, 0x00, 0x00, 		
		0xFF, 0xFF, 0xFF, 0xFF,			/* alarm 	    		*/
		0x80, 0x00, 0x00,
		0x01,					/* note icon	    		*/
		0xFF, 0xFF, 0xFF, 0xFF,			/* alarm type       		*/
		0x00,					/* 0x02 or 0x00     		*/
		0x01, 					/* note type	    		*/
		0x07, 0xD0, 0x01, 0x12, 0x0C, 0x00, 	/* start date/time  		*/
		0x07, 0xD0, 0x01, 0x12, 0x0C, 0x00, 	/* end date/time    		*/
		0x00, 0x00,				/* recurrance	    		*/
		0x00, 0x00,				/* birth year	    		*/
		0x20,					/* ToDo priority 		*/
		0x00,					/* ToDo completed ?		*/
		0x00, 0x00, 0x00,
		0x00,					/* note text length 		*/
		0x00,					/* phone length/meeting place	*/
		0x00, 0x00, 0x00};

	s->Phone.Data.ToDo = ToDo;

	if (ToDo->Location == 0) {
		smprintf(s, "Getting first free ToDo location method 2\n");
		error = GSM_WaitFor (s, reqLoc, 5, 0x13, 4, ID_SetToDo);
		if (error!=GE_NONE) return error;
		req[8] = ToDo->Location/256;
		req[9] = ToDo->Location%256;
	} else {
		return GE_NOTSUPPORTED;
	}

	Note.Type = GCN_MEETING;
	DT.Year = 2004; DT.Month  = 1; 	DT.Day 	  = 1;
	DT.Hour = 12; 	DT.Minute = 12; DT.Second = 0;
	memcpy(&Note.Entries[0].Date,&DT,sizeof(GSM_DateTime));
	Note.Entries[0].EntryType 	= CAL_START_DATETIME;
	memcpy(&Note.Entries[1].Date,&DT,sizeof(GSM_DateTime));
	Note.Entries[1].EntryType 	= CAL_END_DATETIME;
	EncodeUnicode(Note.Entries[2].Text,"ala",3);
	Note.Entries[2].EntryType 	= CAL_TEXT;
	Note.EntriesNum 		= 3;
	error=N6510_FindCalendarIconID3(s, &Note, &req[21]);
	if (error!=GE_NONE) return error;

	switch (ToDo->Priority) {
		case GSM_Priority_Low	: req[44] = 0x10; break;
		case GSM_Priority_Medium: req[44] = 0x20; break;
		case GSM_Priority_High	: req[44] = 0x30; break;
	}

	GSM_ToDoFindDefaultTextTimeAlarmCompleted(ToDo, &Text, &Alarm, &Completed, &EndTime);

	if (Completed != -1) req[45] = 0x01;

	if (EndTime == -1) {
		GSM_GetCurrentDateTime(&DT);
	} else {
		memcpy(&DT,&ToDo->Entries[EndTime].Date,sizeof(GSM_DateTime));
	}
	/*Start time*/
	req[28]	= DT.Year >> 8;
	req[29]	= DT.Year & 0xff;
	req[30]	= DT.Month;
	req[31]	= DT.Day;
	req[32]	= DT.Hour;
	req[33]	= DT.Minute;
	/*End time*/
	req[34]	= DT.Year >> 8;
	req[35]	= DT.Year & 0xff;
	req[36]	= DT.Month;
	req[37]	= DT.Day;
	req[38]	= DT.Hour;
	req[39]	= DT.Minute;

	if (Alarm != -1) {
		if (ToDo->Entries[Alarm].EntryType == CAL_SILENT_ALARM_DATETIME)
		{
			req[22] = 0x00; req[23] = 0x00; req[24] = 0x00; req[25] = 0x00;
		}
		t_time2   = Fill_Time_T(DT,8);
		t_time1   = Fill_Time_T(ToDo->Entries[Alarm].Date,8);
		diff	  = (t_time1-t_time2)/60;

		smprintf(s, "  Difference : %i seconds or minutes\n", -diff);
		req[14] = (unsigned char)(-diff >> 24);
		req[15] = (unsigned char)(-diff >> 16);
		req[16] = (unsigned char)(-diff >> 8);
		req[17] = (unsigned char)(-diff);
	}

	if (Text != -1) {
		req[49] = UnicodeLength(ToDo->Entries[Text].Text);
		CopyUnicodeString(req+54,ToDo->Entries[Text].Text);
		count+= req[49]*2;
	}

	req[count++] = 0x00;

	smprintf(s, "Adding ToDo method 2\n");
	return GSM_WaitFor (s, req, count, 0x13, 4, ID_SetToDo);
}

static GSM_Error N6510_SetToDo(GSM_StateMachine *s, GSM_ToDoEntry *ToDo)
{
	if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_TODO63)) {
		return N6510_SetToDo1(s, ToDo);
	} else if (IsPhoneFeatureAvailable(s->Phone.Data.ModelInfo, F_TODO66)) {
		return N6510_SetToDo2(s, ToDo);
	} else {
		return GE_NOTSUPPORTED;
	}
}

static GSM_Error N6510_ReplyGetLocale(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_Locale *locale = s->Phone.Data.Locale;

	switch (msg.Buffer[3]) {
	case 0x8A:
		smprintf(s, "Date settings received\n");
		switch (msg.Buffer[4]) {
		case 0x00:
			locale->DateFormat 	= GSM_Date_DDMMYYYY;
			locale->DateSeparator 	= '.';
			break;
		case 0x01:
			locale->DateFormat 	= GSM_Date_MMDDYYYY;
			locale->DateSeparator 	= '.';
			break;
		case 0x02:
			locale->DateFormat 	= GSM_Date_YYYYMMDD;
			locale->DateSeparator 	= '.';
			break;
		case 0x04:
			locale->DateFormat 	= GSM_Date_DDMMYYYY;
			locale->DateSeparator 	= '/';
			break;
		case 0x05:
			locale->DateFormat 	= GSM_Date_MMDDYYYY;
			locale->DateSeparator 	= '/';
			break;
		case 0x06:
			locale->DateFormat 	= GSM_Date_YYYYMMDD;
			locale->DateSeparator 	= '/';
			break;
		case 0x08:
			locale->DateFormat 	= GSM_Date_DDMMYYYY;
			locale->DateSeparator 	= '-';
			break;
		case 0x09:
			locale->DateFormat 	= GSM_Date_MMDDYYYY;
			locale->DateSeparator 	= '-';
			break;
		case 0x0A:
			locale->DateFormat 	= GSM_Date_YYYYMMDD;
			locale->DateSeparator 	= '-';
			break;
		default:/* FIXME */
			locale->DateFormat 	= GSM_Date_DDMMYYYY;
			locale->DateSeparator 	= '/';
			break;
		}
		return GE_NONE;
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_GetLocale(GSM_StateMachine *s, GSM_Locale *locale)
{
	unsigned char req[] = {N6110_FRAME_HEADER, 0x89};

	s->Phone.Data.Locale = locale;

	smprintf(s, "Getting date format\n");
	return GSM_WaitFor (s, req, 4, 0x13, 4, ID_GetLocale);
}

static GSM_Error N6510_ReplyGetCalendarSettings(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	GSM_CalendarSettings *sett = s->Phone.Data.CalendarSettings;

	switch (msg.Buffer[3]) {
	case 0x86:
		smprintf(s, "Auto deleting setting received\n");
		sett->AutoDelete = msg.Buffer[4];
		return GE_NONE;
	case 0x8E:
		smprintf(s, "Start day for calendar received\n");
		switch(msg.Buffer[4]) {
		case 0x03:
			sett->StartDay = 6;
			return GE_NONE;
		case 0x02:
			sett->StartDay = 7;
			return GE_NONE;
		case 0x01:
			sett->StartDay = 1;
			return GE_NONE;
		}
		break;
	}
	return GE_UNKNOWNRESPONSE;
}

static GSM_Error N6510_GetCalendarSettings(GSM_StateMachine *s, GSM_CalendarSettings *settings)
{
	GSM_Error	error;
	unsigned char 	req1[] = {N6110_FRAME_HEADER, 0x85};
	unsigned char 	req2[] = {N6110_FRAME_HEADER, 0x8D};

	s->Phone.Data.CalendarSettings = settings;

	smprintf(s, "Getting auto delete\n");
	error = GSM_WaitFor (s, req1, 4, 0x13, 4, ID_GetCalendarSettings);
	if (error != GE_NONE) return error;

	smprintf(s, "Getting start day for week\n");
	return GSM_WaitFor (s, req2, 4, 0x13, 4, ID_GetCalendarSettings);
}

GSM_Error N6510_CancelCall(GSM_StateMachine *s, int ID, bool all)
{
	if (all) return GE_NOTSUPPORTED;
	return DCT3DCT4_CancelCall(s,ID);
}            

GSM_Error N6510_AnswerCall(GSM_StateMachine *s, int ID, bool all)
{
	if (all) return GE_NOTSUPPORTED;
	return DCT3DCT4_AnswerCall(s,ID);
}

static GSM_Error N6510_ReplyAddSMSFolder(GSM_Protocol_Message msg, GSM_StateMachine *s)
{
	smprintf(s,"SMS folder \"%s\" has been added\n",DecodeUnicodeString(msg.Buffer+10));
	return GE_NONE;
}

GSM_Error N6510_AddSMSFolder(GSM_StateMachine *s, unsigned char *name) 
{
	unsigned char req[200] = {N6110_FRAME_HEADER, 0x10, 0x01, 0x00, 0x01,
			          0x00,     		/* Length */
				  0x00, 0x00};

	
	CopyUnicodeString(req+10,name);
	req[7] = UnicodeLength(name)*2 + 6;

	smprintf(s, "Adding SMS folder\n");
	return GSM_WaitFor (s, req, req[7] + 6, 0x14, 4, ID_AddSMSFolder);	
}

static GSM_Reply_Function N6510ReplyFunctions[] = {
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x02,ID_IncomingFrame	  },
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x03,ID_IncomingFrame	  },
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x04,ID_IncomingFrame	  },
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x05,ID_IncomingFrame	  },
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x07,ID_AnswerCall	  },
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x07,ID_IncomingFrame	  },
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x09,ID_CancelCall	  },
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x09,ID_IncomingFrame	  },
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x0A,ID_IncomingFrame	  },
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x0B,ID_IncomingFrame	  },
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x0C,ID_DialVoice		  },
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x0C,ID_IncomingFrame	  },
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x23,ID_IncomingFrame	  },
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x25,ID_IncomingFrame	  },
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x27,ID_IncomingFrame	  },
	{N71_65_ReplySendDTMF,		  "\x01",0x03,0x51,ID_SendDTMF		  },
	{N71_65_ReplyCallInfo,		  "\x01",0x03,0x53,ID_IncomingFrame	  },
	{N71_65_ReplySendDTMF,		  "\x01",0x03,0x59,ID_SendDTMF		  },
	{N71_65_ReplySendDTMF,		  "\x01",0x03,0x5E,ID_SendDTMF		  },

	{N6510_ReplySendSMSMessage,	  "\x02",0x03,0x03,ID_IncomingFrame	  },
	{N6510_ReplyIncomingSMS,	  "\x02",0x03,0x04,ID_IncomingFrame	  },
	{N6510_ReplySetSMSC,		  "\x02",0x03,0x13,ID_SetSMSC		  },
	{N6510_ReplyGetSMSC,		  "\x02",0x03,0x15,ID_GetSMSC		  },

	{N6510_ReplyGetMemoryStatus,	  "\x03",0x03,0x04,ID_GetMemoryStatus	  },
	{N6510_ReplyGetMemory,		  "\x03",0x03,0x08,ID_GetMemory		  },
	{N6510_ReplyDeleteMemory,	  "\x03",0x03,0x10,ID_SetMemory		  },
	{N71_65_ReplyWritePhonebook,	  "\x03",0x03,0x0C,ID_SetBitmap		  },
	{N71_65_ReplyWritePhonebook,	  "\x03",0x03,0x0C,ID_SetMemory		  },

	{DCT3DCT4_ReplyCallDivert,	  "\x06",0x03,0x02,ID_Divert		  },
	{N71_65_ReplyUSSDInfo,		  "\x06",0x03,0x03,ID_IncomingFrame	  },
	{NoneReply,			  "\x06",0x03,0x06,ID_IncomingFrame	  },
	{NoneReply,			  "\x06",0x03,0x09,ID_IncomingFrame	  },

	{N6510_ReplyEnterSecurityCode,	  "\x08",0x03,0x08,ID_EnterSecurityCode	  },
	{N6510_ReplyEnterSecurityCode,	  "\x08",0x03,0x09,ID_EnterSecurityCode	  },
	{N6510_ReplyGetSecurityStatus,	  "\x08",0x03,0x12,ID_GetSecurityStatus	  },

	{N6510_ReplyGetNetworkInfo,	  "\x0A",0x03,0x01,ID_GetNetworkInfo	  },
	{N6510_ReplyGetNetworkInfo,	  "\x0A",0x03,0x01,ID_IncomingFrame	  },
	{N6510_ReplyLogIntoNetwork,	  "\x0A",0x03,0x02,ID_IncomingFrame	  },
	{N6510_ReplyGetSignalQuality,	  "\x0A",0x03,0x0C,ID_GetSignalQuality	  },
	{N6510_ReplyGetIncSignalQuality,  "\x0A",0x03,0x1E,ID_IncomingFrame	  },
	{NoneReply,			  "\x0A",0x03,0x20,ID_IncomingFrame	  },
	{N6510_ReplyGetOperatorLogo,	  "\x0A",0x03,0x24,ID_GetBitmap		  },
	{N6510_ReplySetOperatorLogo,	  "\x0A",0x03,0x26,ID_SetBitmap		  },

	{NoneReply,			  "\x0B",0x03,0x01,ID_PlayTone		  },
	{NoneReply,			  "\x0B",0x03,0x15,ID_PlayTone		  },
	{NoneReply,			  "\x0B",0x03,0x16,ID_PlayTone		  },

	{N71_65_ReplyAddCalendar1,	  "\x13",0x03,0x02,ID_SetCalendarNote	  },
	{N71_65_ReplyAddCalendar1,	  "\x13",0x03,0x04,ID_SetCalendarNote	  },
	{N71_65_ReplyAddCalendar1,	  "\x13",0x03,0x06,ID_SetCalendarNote	  },
	{N71_65_ReplyAddCalendar1,	  "\x13",0x03,0x08,ID_SetCalendarNote	  },
	{N71_65_ReplyDelCalendar,	  "\x13",0x03,0x0C,ID_DeleteCalendarNote  },
	{N71_65_ReplyGetNextCalendar1,	  "\x13",0x03,0x1A,ID_GetCalendarNote	  },/*method 1*/
	{N6510_ReplyGetCalendarNotePos,	  "\x13",0x03,0x32,ID_GetCalendarNotePos  },/*method 1*/
	{N6510_ReplyGetCalendarInfo,	  "\x13",0x03,0x3B,ID_GetCalendarNotesInfo},/*method 1*/
#ifdef DEBUG
	{N71_65_ReplyGetNextCalendar2,	  "\x13",0x03,0x3F,ID_GetCalendarNote	  },
#endif
	{N71_65_ReplyAddCalendar2,	  "\x13",0x03,0x41,ID_SetCalendarNote	  },/*method 2*/
	{N6510_ReplyAddCalendar3,	  "\x13",0x03,0x66,ID_SetCalendarNote	  },/*method 3*/
	{N6510_ReplyAddToDo2,		  "\x13",0x03,0x66,ID_SetToDo		  },
	{N6510_ReplyGetCalendar3,	  "\x13",0x03,0x7E,ID_GetCalendarNote	  },/*method 3*/
	{N6510_ReplyGetToDo2,		  "\x13",0x03,0x7E,ID_GetToDo		  },
	{N6510_ReplyGetCalendarSettings,  "\x13",0x03,0x86,ID_GetCalendarSettings },
	{N6510_ReplyGetLocale,		  "\x13",0x03,0x8A,ID_GetLocale		  },
	{N6510_ReplyGetCalendarSettings,  "\x13",0x03,0x8E,ID_GetCalendarSettings },
	{N6510_ReplyGetCalendarNotePos,	  "\x13",0x03,0x96,ID_GetCalendarNotePos  },/*method 3*/
	{N6510_ReplyGetToDoFirstLoc2,	  "\x13",0x03,0x96,ID_SetToDo		  },
	{N6510_ReplyGetCalendarInfo,	  "\x13",0x03,0x9F,ID_GetCalendarNotesInfo},/*method 3*/
	{N6510_ReplyGetToDoStatus2,	  "\x13",0x03,0x9F,ID_GetToDo		  },

	{N6510_ReplySaveSMSMessage,	  "\x14",0x03,0x01,ID_SaveSMSMessage	  },
	{N6510_ReplySetPicture,		  "\x14",0x03,0x01,ID_SetBitmap		  },
	{N6510_ReplyGetSMSMessage,	  "\x14",0x03,0x03,ID_GetSMSMessage	  },
	{N6510_ReplyDeleteSMSMessage,	  "\x14",0x03,0x05,ID_DeleteSMSMessage	  },
	{N6510_ReplyDeleteSMSMessage,	  "\x14",0x03,0x06,ID_DeleteSMSMessage	  },
	{N6510_ReplyGetSMSStatus,	  "\x14",0x03,0x09,ID_GetSMSStatus	  },
	{N6510_ReplyGetSMSFolderStatus,	  "\x14",0x03,0x0d,ID_GetSMSFolderStatus  },
	{N6510_ReplyGetSMSMessage,	  "\x14",0x03,0x0f,ID_GetSMSMessage	  },
	{N6510_ReplyAddSMSFolder,	  "\x14",0x03,0x11,ID_AddSMSFolder	  },
	{N6510_ReplyGetSMSFolders,	  "\x14",0x03,0x13,ID_GetSMSFolders	  },
	{N6510_ReplySaveSMSMessage,	  "\x14",0x03,0x17,ID_SaveSMSMessage	  },
	{N6510_ReplyGetSMSStatus,	  "\x14",0x03,0x1a,ID_GetSMSStatus	  },

	{DCT4_ReplySetPhoneMode,	  "\x15",0x03,0x64,ID_Reset		  },
	{DCT4_ReplyGetPhoneMode,	  "\x15",0x03,0x65,ID_Reset		  },
	{NoneReply,		  	  "\x15",0x03,0x68,ID_Reset		  },

	{N6510_ReplyGetBatteryCharge,	  "\x17",0x03,0x0B,ID_GetBatteryCharge	  },

	{N6510_ReplySetDateTime,	  "\x19",0x03,0x02,ID_SetDateTime	  },
	{N6510_ReplyGetDateTime,	  "\x19",0x03,0x0B,ID_GetDateTime	  },
	{N6510_ReplySetAlarm,		  "\x19",0x03,0x12,ID_SetAlarm		  },
	{N6510_ReplyGetAlarm,		  "\x19",0x03,0x1A,ID_GetAlarm		  },
	{N6510_ReplyGetAlarm,		  "\x19",0x03,0x20,ID_GetAlarm		  },

	{DCT4_ReplyGetIMEI,		  "\x1B",0x03,0x01,ID_GetIMEI		  },
	{NOKIA_ReplyGetPhoneString,	  "\x1B",0x03,0x08,ID_GetHardware	  },
	{N6510_ReplyGetPPM,		  "\x1B",0x03,0x08,ID_GetPPM		  },
	{NOKIA_ReplyGetPhoneString,	  "\x1B",0x03,0x0C,ID_GetProductCode	  },

	/* 0x1C - vibra */

	{N6510_ReplyGetRingtonesInfo,	  "\x1f",0x03,0x08,ID_GetRingtonesInfo	  },
	{N6510_ReplyDeleteRingtones,	  "\x1f",0x03,0x11,ID_SetRingtone	  },
	{N6510_ReplyGetRingtone,	  "\x1f",0x03,0x13,ID_GetRingtone	  },
	{N6510_ReplySetBinRingtone,	  "\x1f",0x03,0x0F,ID_SetRingtone	  },

	/* 0x23 - voice records */

	{N6510_ReplyGetProfile,		  "\x39",0x03,0x02,ID_GetProfile	  },
	{N6510_ReplySetProfile,		  "\x39",0x03,0x04,ID_SetProfile	  },
	{N6510_ReplyGetProfile,		  "\x39",0x03,0x06,ID_GetProfile	  },

	{N6510_ReplySetLight,		  "\x3A",0x03,0x06,ID_SetLight		  },

 	{N6510_ReplyGetFMStation,	  "\x3E",0x03,0x06,ID_GetFMStation	  },
 	{N6510_ReplyGetFMStatus,	  "\x3E",0x03,0x0E,ID_GetFMStation	  },
 	{N6510_ReplySetFMStation,	  "\x3E",0x03,0x15,ID_SetFMStation	  },
 	{N6510_ReplyGetFMStation,	  "\x3E",0x03,0x16,ID_GetFMStation	  },

	{DCT3DCT4_ReplyEnableWAP,	  "\x3f",0x03,0x01,ID_EnableWAP		  },
	{DCT3DCT4_ReplyEnableWAP,	  "\x3f",0x03,0x02,ID_EnableWAP		  },
	{NoneReply,			  "\x3f",0x03,0x04,ID_EnableWAP		  },
	{NoneReply,			  "\x3f",0x03,0x05,ID_EnableWAP		  },
	{N6510_ReplyGetWAPBookmark,	  "\x3f",0x03,0x07,ID_GetWAPBookmark	  },
	{N6510_ReplyGetWAPBookmark,	  "\x3f",0x03,0x08,ID_GetWAPBookmark	  },
	{DCT3DCT4_ReplySetWAPBookmark,	  "\x3f",0x03,0x0A,ID_SetWAPBookmark	  },
	{DCT3DCT4_ReplySetWAPBookmark,	  "\x3f",0x03,0x0B,ID_SetWAPBookmark	  },
	{DCT3DCT4_ReplyDelWAPBookmark,	  "\x3f",0x03,0x0D,ID_DeleteWAPBookmark	  },
	{DCT3DCT4_ReplyDelWAPBookmark,	  "\x3f",0x03,0x0E,ID_DeleteWAPBookmark	  },
	{DCT3DCT4_ReplyGetActiveWAPMMSSet,"\x3f",0x03,0x10,ID_GetWAPSettings	  },
	{DCT3DCT4_ReplySetActiveWAPMMSSet,"\x3f",0x03,0x13,ID_SetWAPSettings	  },
	{DCT3DCT4_ReplySetActiveWAPMMSSet,"\x3f",0x03,0x13,ID_SetMMSSettings	  },
	{N6510_ReplyGetWAPMMSSettings,	  "\x3f",0x03,0x16,ID_GetWAPSettings	  },
	{N6510_ReplyGetWAPMMSSettings,	  "\x3f",0x03,0x16,ID_GetMMSSettings	  },
	{N6510_ReplyGetWAPMMSSettings,	  "\x3f",0x03,0x17,ID_GetWAPSettings	  },
	{N6510_ReplyGetWAPMMSSettings,	  "\x3f",0x03,0x17,ID_GetMMSSettings	  },
	{N6510_ReplySetWAPMMSSettings,	  "\x3f",0x03,0x19,ID_SetWAPSettings	  },
	{N6510_ReplySetWAPMMSSettings,	  "\x3f",0x03,0x19,ID_SetMMSSettings	  },
	{N6510_ReplySetWAPMMSSettings,	  "\x3f",0x03,0x1A,ID_SetWAPSettings	  },
	{N6510_ReplySetWAPMMSSettings,    "\x3f",0x03,0x1A,ID_SetMMSSettings	  },

	{N6510_ReplyGetOriginalIMEI,	  "\x42",0x07,0x00,ID_GetOriginalIMEI	  },
	{N6510_ReplyGetManufactureMonth,  "\x42",0x07,0x00,ID_GetManufactureMonth },
	{N6510_ReplyGetOriginalIMEI,	  "\x42",0x07,0x01,ID_GetOriginalIMEI	  },
	{N6510_ReplyGetManufactureMonth,  "\x42",0x07,0x02,ID_GetManufactureMonth },

	{N6510_ReplySetOperatorLogo,	  "\x43",0x03,0x08,ID_SetBitmap		  },
	{N6510_ReplyGetGPRSAccessPoint,	  "\x43",0x03,0x06,ID_GetGPRSPoint	  },
	{N6510_ReplySetGPRSAccessPoint1,  "\x43",0x03,0x06,ID_SetGPRSPoint	  },
#ifdef DEVELOP
	{N6510_ReplyEnableGPRSAccessPoint,"\x43",0x03,0x06,ID_EnableGPRSPoint	  },
#endif
	{NoneReply,			  "\x43",0x03,0x08,ID_SetGPRSPoint	  },

	/* 0x4A - voice records */

	/* 0x53 - simlock */

	{N6510_ReplySetToDo1,		  "\x55",0x03,0x02,ID_SetToDo		  },
	{N6510_ReplyGetToDo1,		  "\x55",0x03,0x04,ID_GetToDo		  },
	{N6510_ReplyGetToDoFirstLoc1,	  "\x55",0x03,0x10,ID_SetToDo		  },
	{N6510_ReplyDeleteAllToDo1,	  "\x55",0x03,0x12,ID_DeleteAllToDo	  },
	{N6510_ReplyGetToDoStatus1,	  "\x55",0x03,0x16,ID_GetToDo		  },

	{N6510_ReplyAddFileHeader,	  "\x6D",0x03,0x03,ID_AddFile		  },
	{N6510_ReplyAddFolder,		  "\x6D",0x03,0x05,ID_AddFolder		  },
	{N6510_ReplyGetFilePart,	  "\x6D",0x03,0x0F,ID_GetFile		  },
	{N6510_ReplyAddFileHeader,	  "\x6D",0x03,0x13,ID_AddFile		  },
	{N6510_ReplyGetFileFolderInfo,	  "\x6D",0x03,0x15,ID_GetFileInfo	  },
	{N6510_ReplyGetFileFolderInfo,	  "\x6D",0x03,0x15,ID_GetFile		  },
	{N6510_ReplyGetFileFolderInfo,	  "\x6D",0x03,0x15,ID_AddFile		  },
	{N6510_ReplyDeleteFile,		  "\x6D",0x03,0x19,ID_DeleteFile	  },
	{N6510_ReplyDeleteFile,		  "\x6D",0x03,0x1F,ID_DeleteFile	  },
	{N6510_ReplyGetFileSystemStatus,  "\x6D",0x03,0x23,ID_FileSystemStatus	  },
	{N6510_ReplyGetFileFolderInfo,	  "\x6D",0x03,0x2F,ID_GetFileInfo	  },
	{N6510_ReplyGetFileFolderInfo,	  "\x6D",0x03,0x2F,ID_GetFile		  },
	{N6510_ReplyGetFileSystemStatus,  "\x6D",0x03,0x2F,ID_FileSystemStatus	  },
	{N6510_ReplyGetFileFolderInfo,	  "\x6D",0x03,0x33,ID_GetFileInfo	  },
	{N6510_ReplyGetFileFolderInfo,	  "\x6D",0x03,0x33,ID_GetFile		  },
	{N6510_ReplyAddFilePart,	  "\x6D",0x03,0x41,ID_AddFile		  },
	{N6510_ReplyGetFileFolderInfo,	  "\x6D",0x03,0x43,ID_AddFile		  },
	{N6510_ReplyGetFileFolderInfo,	  "\x6D",0x03,0x43,ID_GetFile		  },
	{N6510_ReplyGetFileFolderInfo,	  "\x6D",0x03,0x43,ID_GetFileInfo	  },

	{N6510_ReplyStartupNoteLogo,	  "\x7A",0x04,0x01,ID_GetBitmap		  },
	{N6510_ReplyStartupNoteLogo,	  "\x7A",0x04,0x01,ID_SetBitmap		  },
	{N6510_ReplyStartupNoteLogo,	  "\x7A",0x04,0x0F,ID_GetBitmap		  },
	{N6510_ReplyStartupNoteLogo,	  "\x7A",0x04,0x0F,ID_SetBitmap		  },
	{N6510_ReplyStartupNoteLogo,	  "\x7A",0x04,0x10,ID_GetBitmap		  },
	{N6510_ReplyStartupNoteLogo,	  "\x7A",0x04,0x10,ID_SetBitmap		  },
	{N6510_ReplyStartupNoteLogo,	  "\x7A",0x04,0x25,ID_SetBitmap		  },

	{DCT3DCT4_ReplyGetModelFirmware,  "\xD2",0x02,0x00,ID_GetModel		  },
	{DCT3DCT4_ReplyGetModelFirmware,  "\xD2",0x02,0x00,ID_GetFirmware	  },

	/* 0xD7 - Bluetooth */

	{N6510_ReplyGetRingtoneID,	  "\xDB",0x03,0x02,ID_SetRingtone	  },

	{NULL,				  "\x00",0x00,0x00,ID_None		  }
};

GSM_Phone_Functions N6510Phone = {
	"3100|3300|3510|3510i|3530|3590|3595|5100|6100|6200|6220|6310|6310i|6510|6610|6800|7210|7250|7250i|8310|8390|8910|8910i",
	N6510ReplyFunctions,
	N6510_Initialise,
	NONEFUNCTION,			/*	Terminate 		*/
	GSM_DispatchMessage,
	DCT3DCT4_GetModel,
	DCT3DCT4_GetFirmware,
	DCT4_GetIMEI,
	N6510_GetDateTime,
	N6510_GetAlarm,
	N6510_GetMemory,
	N6510_GetMemoryStatus,
	N6510_GetSMSC,
	N6510_GetSMSMessage,
	N6510_GetSMSFolders,
	NOKIA_GetManufacturer,
	N6510_GetNextSMSMessage,
	N6510_GetSMSStatus,
	NOKIA_SetIncomingSMS,
	N6510_GetNetworkInfo,
	DCT4_Reset,
	N6510_DialVoice,
 	N6510_AnswerCall,
 	N6510_CancelCall,
	N6510_GetRingtone,
	DCT3DCT4_GetWAPBookmark,
	N6510_GetBitmap,
	N6510_SetRingtone,
	N6510_SaveSMSMessage,
	N6510_SendSMSMessage,
	N6510_SetDateTime,
	N6510_SetAlarm,
	N6510_SetBitmap,
	N6510_SetMemory,
	N6510_DeleteSMSMessage,
	N6510_SetWAPBookmark,
	DCT3DCT4_DeleteWAPBookmark,
	N6510_GetWAPSettings,
	NOTIMPLEMENTED,			/* 	SetIncomingCB		*/
	N6510_SetSMSC,
	N6510_GetManufactureMonth,
	DCT4_GetProductCode,
	N6510_GetOriginalIMEI,
	DCT4_GetHardware,
	N6510_GetPPM,
	N6510_PressKey,
	N6510_GetToDo,
	N6510_DeleteAllToDo,
	N6510_SetToDo,
	N6510_GetToDoStatus,
	N6510_PlayTone,
	N6510_EnterSecurityCode,
	N6510_GetSecurityStatus,
	N6510_GetProfile,
	N6510_GetRingtonesInfo,
	N6510_SetWAPSettings,
	N6510_GetSpeedDial,
	NOTIMPLEMENTED,			/*	SetSpeedDial		*/
	NOTIMPLEMENTED,			/*	ResetPhoneSettings	*/
	DCT3DCT4_SendDTMF,
	NOTSUPPORTED,			/*	GetDisplayStatus	*/
	NOTIMPLEMENTED,			/*	SetAutoNetworkLogin	*/
	N6510_SetProfile,
	NOTSUPPORTED,			/*	GetSIMIMSI		*/
	NOKIA_SetIncomingCall,
    	N6510_GetNextCalendar,
	N71_65_DelCalendar,
	N6510_AddCalendar,
	N6510_GetBatteryCharge,
	N6510_GetSignalQuality,
	NOTSUPPORTED,       		/*  	GetCategory 		*/
	NOTSUPPORTED,        		/*  	GetCategoryStatus 	*/
    	N6510_GetFMStation,
     	N6510_SetFMStation,
 	N6510_ClearFMStations,
	NOKIA_SetIncomingUSSD,
	N6510_DeleteUserRingtones,
	N6510_ShowStartInfo,
	N6510_GetNextFileFolder,
	N6510_GetFilePart,
	N6510_AddFilePart,
	N6510_GetFileSystemStatus,
	N6510_DeleteFile,
	N6510_AddFolder,
	N6510_GetMMSSettings,
	N6510_SetMMSSettings,
 	NOTIMPLEMENTED,			/* 	HoldCall 		*/
 	NOTIMPLEMENTED,			/* 	UnholdCall 		*/
 	NOTIMPLEMENTED,			/* 	ConferenceCall 		*/
 	NOTIMPLEMENTED,			/* 	SplitCall		*/
 	NOTIMPLEMENTED,			/* 	TransferCall		*/
 	NOTIMPLEMENTED,			/* 	SwitchCall		*/
 	DCT3DCT4_GetCallDivert,
 	DCT3DCT4_SetCallDivert,
 	DCT3DCT4_CancelAllDiverts,
 	N6510_AddSMSFolder,
 	NOTIMPLEMENTED,			/* 	DeleteSMSFolder		*/
	N6510_GetGPRSAccessPoint,
	N6510_SetGPRSAccessPoint,
	N6510_GetLocale,
	NOTSUPPORTED,			/* 	SetLocale		*/
	N6510_GetCalendarSettings,
	NOTSUPPORTED,			/* 	SetCalendarSettings	*/
	NOTIMPLEMENTED			/*	GetNote			*/
};

#endif

/* How should editor hadle tabs in this file? Add editor commands here.
 * vim: noexpandtab sw=8 ts=8 sts=8:
 */
