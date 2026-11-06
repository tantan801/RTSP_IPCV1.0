/******************************************************************************

  Copyright (C), 2017, Hisilicon Tech. Co., Ltd.

 ******************************************************************************
  File Name     : sample_venc.c
  Version       : Initial Draft
  Author        : Hisilicon multimedia software group
  Created       : 2017
  Description   :
******************************************************************************/

#ifdef __cplusplus
#if __cplusplus
extern "C" {
#endif
#endif /* End of #ifdef __cplusplus */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>

#include "sample_comm.h"

#ifdef HI_FPGA
    #define PIC_SIZE   PIC_1080P
#else
    #define PIC_SIZE   PIC_1080P
#endif


#if	RTSP_ENABLE

#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <netdb.h>
#include <sys/socket.h>

#include <sys/ioctl.h>
#include <fcntl.h> 
#include <pthread.h>
#include <sys/ipc.h> 
#include <sys/msg.h>
#include <netinet/if_ether.h>
#include <net/if.h>

#include <linux/if_ether.h>
#include <linux/sockios.h>
#include <netinet/in.h> 
#include <arpa/inet.h> 

#endif


#if	RTSP_ENABLE

RTP_FIXED_HEADER        *rtp_hdr;

NALU_HEADER		*nalu_hdr;
FU_INDICATOR	*fu_ind;
FU_HEADER		*fu_hdr;


RTSP_CLIENT g_rtspClients[MAX_RTSP_CLIENT];

int g_nSendDataChn = -1;
pthread_mutex_t g_mutex;
pthread_cond_t  g_cond;
pthread_mutex_t g_sendmutex;

pthread_t g_SendDataThreadId = 0;

//HAL_CLIENT_HANDLE hMainStreamClient = NULL,hSubStreamClient = NULL,hAudioClient = NULL;
char g_rtp_playload[20];
int   g_audio_rate = 8000;
//VIDEO_NORM_E gs_enNorm = VIDEO_ENCODING_MODE_NTSC;//30fps
int g_nframerate;

int exitok = 0;

int udpfd;

int count=0;


struct list_head RTPbuf_head = LIST_HEAD_INIT(RTPbuf_head);

static pthread_t gs_RtpPid;

static char const* dateHeader()
{
	static char buf[200];
#if !defined(_WIN32_WCE)
	time_t tt = time(NULL);
	strftime(buf, sizeof buf, "Date: %a, %b %d %Y %H:%M:%S GMT\r\n", gmtime(&tt));
#endif

	return buf;
}
static char* GetLocalIP(int sock)
{
	struct ifreq ifreq;
	struct sockaddr_in *sin;
	char * LocalIP = malloc(20);
	strcpy(ifreq.ifr_name,"wlan");
	if (!(ioctl (sock, SIOCGIFADDR,&ifreq)))
    	{ 
		sin = (struct sockaddr_in *)&ifreq.ifr_addr;
		sin->sin_family = AF_INET;
       	strcpy(LocalIP,inet_ntoa(sin->sin_addr)); 
		//inet_ntop(AF_INET, &sin->sin_addr,LocalIP, 16);
    	} 
	printf("--------------------------------------------%s\n",LocalIP);
	return LocalIP;
}

char* strDupSize(char const* str) 
{
  if (str == NULL) return NULL;
  size_t len = strlen(str) + 1;
  char* copy = malloc(len);

  return copy;
}

int ParseRequestString(char const* reqStr,
		       unsigned reqStrSize,
		       char* resultCmdName,
		       unsigned resultCmdNameMaxSize,
		       char* resultURLPreSuffix,
		       unsigned resultURLPreSuffixMaxSize,
		       char* resultURLSuffix,
		       unsigned resultURLSuffixMaxSize,
		       char* resultCSeq,
		       unsigned resultCSeqMaxSize) 
{
  // This parser is currently rather dumb; it should be made smarter #####

  // Read everything up to the first space as the command name:
  int parseSucceeded = FALSE;
  unsigned i;
  for (i = 0; i < resultCmdNameMaxSize-1 && i < reqStrSize; ++i) {
    char c = reqStr[i];
    if (c == ' ' || c == '\t') {
      parseSucceeded = TRUE;
      break;
    }

    resultCmdName[i] = c;
  }
  resultCmdName[i] = '\0';
  if (!parseSucceeded) return FALSE;
      
  // Skip over the prefix of any "rtsp://" or "rtsp:/" URL that follows:
  unsigned j = i+1;
  while (j < reqStrSize && (reqStr[j] == ' ' || reqStr[j] == '\t')) ++j; // skip over any additional white space
  for (j = i+1; j < reqStrSize-8; ++j) {
    if ((reqStr[j] == 'r' || reqStr[j] == 'R')
	&& (reqStr[j+1] == 't' || reqStr[j+1] == 'T')
	&& (reqStr[j+2] == 's' || reqStr[j+2] == 'S')
	&& (reqStr[j+3] == 'p' || reqStr[j+3] == 'P')
	&& reqStr[j+4] == ':' && reqStr[j+5] == '/') {
      j += 6;
      if (reqStr[j] == '/') {
	// This is a "rtsp://" URL; skip over the host:port part that follows:
	++j;
	while (j < reqStrSize && reqStr[j] != '/' && reqStr[j] != ' ') ++j;
      } else {
	// This is a "rtsp:/" URL; back up to the "/":
	--j;
      }
      i = j;
      break;
    }
  }

  // Look for the URL suffix (before the following "RTSP/"):
  parseSucceeded = FALSE;
  unsigned k;
  for (k = i+1; k < reqStrSize-5; ++k) {
    if (reqStr[k] == 'R' && reqStr[k+1] == 'T' &&
	reqStr[k+2] == 'S' && reqStr[k+3] == 'P' && reqStr[k+4] == '/') {
      while (--k >= i && reqStr[k] == ' ') {} // go back over all spaces before "RTSP/"
      unsigned k1 = k;
      while (k1 > i && reqStr[k1] != '/' && reqStr[k1] != ' ') --k1;
      // the URL suffix comes from [k1+1,k]

      // Copy "resultURLSuffix":
      if (k - k1 + 1 > resultURLSuffixMaxSize) return FALSE; // there's no room
      unsigned n = 0, k2 = k1+1;
      while (k2 <= k) resultURLSuffix[n++] = reqStr[k2++];
      resultURLSuffix[n] = '\0';

      // Also look for the URL 'pre-suffix' before this:
      unsigned k3 = --k1;
      while (k3 > i && reqStr[k3] != '/' && reqStr[k3] != ' ') --k3;
      // the URL pre-suffix comes from [k3+1,k1]

      // Copy "resultURLPreSuffix":
      if (k1 - k3 + 1 > resultURLPreSuffixMaxSize) return FALSE; // there's no room
      n = 0; k2 = k3+1;
      while (k2 <= k1) resultURLPreSuffix[n++] = reqStr[k2++];
      resultURLPreSuffix[n] = '\0';

      i = k + 7; // to go past " RTSP/"
      parseSucceeded = TRUE;
      break;
    }
  }
  if (!parseSucceeded) return FALSE;

  // Look for "CSeq:", skip whitespace,
  // then read everything up to the next \r or \n as 'CSeq':
  parseSucceeded = FALSE;
  for (j = i; j < reqStrSize-5; ++j) {
    if (reqStr[j] == 'C' && reqStr[j+1] == 'S' && reqStr[j+2] == 'e' &&
	reqStr[j+3] == 'q' && reqStr[j+4] == ':') {
      j += 5;
      unsigned n;
      while (j < reqStrSize && (reqStr[j] ==  ' ' || reqStr[j] == '\t')) ++j;
      for (n = 0; n < resultCSeqMaxSize-1 && j < reqStrSize; ++n,++j) {
	char c = reqStr[j];
	if (c == '\r' || c == '\n') {
	  parseSucceeded = TRUE;
	  break;
	}

	resultCSeq[n] = c;
      }
      resultCSeq[n] = '\0';
      break;
    }
  }
  if (!parseSucceeded) return FALSE;

  return TRUE;
}

int OptionAnswer(char *cseq, int sock)
{
	if (sock != 0)
	{
		char buf[1024];
		memset(buf,0,1024);
		char *pTemp = buf;
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sPublic: %s\r\n\r\n",
			cseq,dateHeader(),"OPTIONS,DESCRIBE,SETUP,PLAY,PAUSE,TEARDOWN");
	
		int reg = send(sock, buf,strlen(buf),0);
		if(reg <= 0)
		{
			return FALSE;
		}
		else
		{
			printf(">>>>>%s\n",buf);
		}
		return TRUE;
	}
	return FALSE;
}

int DescribeAnswer(char *cseq,int sock,char * urlSuffix,char* recvbuf)
{
	if (sock != 0)
	{
		char sdpMsg[1024];
		char buf[2048];
		memset(buf,0,2048);
		memset(sdpMsg,0,1024);
		char*localip;
		localip = GetLocalIP(sock);
		
		char *pTemp = buf;
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n",cseq);
		pTemp += sprintf(pTemp,"%s",dateHeader());
		pTemp += sprintf(pTemp,"Content-Type: application/sdp\r\n");
		

		char *pTemp2 = sdpMsg;
		pTemp2 += sprintf(pTemp2,"v=0\r\n");
		pTemp2 += sprintf(pTemp2,"o=StreamingServer 3331435948 1116907222000 IN IP4 %s\r\n",localip);
		pTemp2 += sprintf(pTemp2,"s=H.264\r\n");
		pTemp2 += sprintf(pTemp2,"c=IN IP4 0.0.0.0\r\n");
		pTemp2 += sprintf(pTemp2,"t=0 0\r\n");
		pTemp2 += sprintf(pTemp2,"a=control:*\r\n");
		

		/*H264 TrackID=0 RTP_PT 96*/
		pTemp2 += sprintf(pTemp2,"m=video 0 RTP/AVP 96\r\n");
		pTemp2 += sprintf(pTemp2,"a=control:trackID=0\r\n");
		pTemp2 += sprintf(pTemp2,"a=rtpmap:96 H264/90000\r\n");
		pTemp2 += sprintf(pTemp2,"a=fmtp:96 packetization-mode=1; sprop-parameter-sets=%s\r\n", "AAABBCCC");
#if 1
		
		/*G726*/
		
		pTemp2 += sprintf(pTemp2,"m=audio 0 RTP/AVP 97\r\n");
		pTemp2 += sprintf(pTemp2,"a=control:trackID=1\r\n");
		if(strcmp(g_rtp_playload,"AAC")==0)
		{
			pTemp2 += sprintf(pTemp2,"a=rtpmap:97 MPEG4-GENERIC/%d/2\r\n",16000);
			pTemp2 += sprintf(pTemp2,"a=fmtp:97 streamtype=5;profile-level-id=1;mode=AAC-hbr;sizelength=13;indexlength=3;indexdeltalength=3;config=1410\r\n");
		}
		else
		{
			pTemp2 += sprintf(pTemp2,"a=rtpmap:97 G726-32/%d/1\r\n",8000);
			pTemp2 += sprintf(pTemp2,"a=fmtp:97 packetization-mode=1\r\n");
		}	
#endif
		pTemp += sprintf(pTemp,"Content-length: %d\r\n", strlen(sdpMsg));     
		pTemp += sprintf(pTemp,"Content-Base: rtsp://%s/%s/\r\n\r\n",localip,urlSuffix);
		
		//printf("mem ready\n");
		strcat(pTemp, sdpMsg);
		free(localip);
		//printf("Describe ready sent\n");
		int re = send(sock, buf, strlen(buf),0);
		if(re <= 0)
		{
			return FALSE;
		}
		else
		{
			printf(">>>>>%s\n",buf);
		}
	}

	return TRUE;
}
void ParseTransportHeader(char const* buf,
						  StreamingMode* streamingMode,
						 char**streamingModeString,
						 char**destinationAddressStr,
						 u_int8_t* destinationTTL,
						 portNumBits* clientRTPPortNum, // if UDP
						 portNumBits* clientRTCPPortNum, // if UDP
						 unsigned char* rtpChannelId, // if TCP
						 unsigned char* rtcpChannelId // if TCP
						 )
 {
	// Initialize the result parameters to default values:
	*streamingMode = RTP_UDP;
	*streamingModeString = NULL;
	*destinationAddressStr = NULL;
	*destinationTTL = 255;
	*clientRTPPortNum = 0;
	*clientRTCPPortNum = 1; 
	*rtpChannelId = *rtcpChannelId = 0xFF;
	
	portNumBits p1, p2;
	unsigned ttl, rtpCid, rtcpCid;
	
	// First, find "Transport:"
	while (1) {
		if (*buf == '\0') return; // not found
		if (strncasecmp(buf, "Transport: ", 11) == 0) break;
		++buf;
	}
	
	// Then, run through each of the fields, looking for ones we handle:
	char const* fields = buf + 11;
	char* field = strDupSize(fields);
	while (sscanf(fields, "%[^;]", field) == 1) {
		if (strcmp(field, "RTP/AVP/TCP") == 0) {
			*streamingMode = RTP_TCP;
		} else if (strcmp(field, "RAW/RAW/UDP") == 0 ||
			strcmp(field, "MP2T/H2221/UDP") == 0) {
			*streamingMode = RAW_UDP;
			//*streamingModeString = strDup(field);
		} else if (strncasecmp(field, "destination=", 12) == 0)
		{
			//delete[] destinationAddressStr;
			free(destinationAddressStr);
			//destinationAddressStr = strDup(field+12);
		} else if (sscanf(field, "ttl%u", &ttl) == 1) {
			destinationTTL = (u_int8_t)ttl;
		} else if (sscanf(field, "client_port=%hu-%hu", &p1, &p2) == 2) {
			*clientRTPPortNum = p1;
			*clientRTCPPortNum = p2;
		} else if (sscanf(field, "client_port=%hu", &p1) == 1) {
			*clientRTPPortNum = p1;
			*clientRTCPPortNum = streamingMode == RAW_UDP ? 0 : p1 + 1;
		} else if (sscanf(field, "interleaved=%u-%u", &rtpCid, &rtcpCid) == 2) {
			*rtpChannelId = (unsigned char)rtpCid;
			*rtcpChannelId = (unsigned char)rtcpCid;
		}
		
		fields += strlen(field);
		while (*fields == ';') ++fields; // skip over separating ';' chars
		if (*fields == '\0' || *fields == '\r' || *fields == '\n') break;
	}
	free(field);
}


int SetupAnswer(char *cseq,int sock,int SessionId,char * urlSuffix,char* recvbuf,int* rtpport, int* rtcpport)
{
	if (sock != 0)
	{
		char buf[1024];
		memset(buf,0,1024);
		
		StreamingMode streamingMode;
		char* streamingModeString; // set when RAW_UDP streaming is specified
		char* clientsDestinationAddressStr;
		u_int8_t clientsDestinationTTL;
		portNumBits clientRTPPortNum, clientRTCPPortNum;
		unsigned char rtpChannelId, rtcpChannelId;
		ParseTransportHeader(recvbuf,&streamingMode, &streamingModeString,
			&clientsDestinationAddressStr, &clientsDestinationTTL,
			&clientRTPPortNum, &clientRTCPPortNum,
			&rtpChannelId, &rtcpChannelId);

		//Port clientRTPPort(clientRTPPortNum);
		//Port clientRTCPPort(clientRTCPPortNum);
		*rtpport = clientRTPPortNum;
		*rtcpport = clientRTCPPortNum;
		
		char *pTemp = buf;
		char*localip;
		localip = GetLocalIP(sock);
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sTransport: RTP/AVP;unicast;destination=%s;client_port=%d-%d;server_port=%d-%d\r\nSession: %d\r\n\r\n",
			cseq,dateHeader(),localip,
			ntohs(htons(clientRTPPortNum)), 
			ntohs(htons(clientRTCPPortNum)), 
			ntohs(2000),
			ntohs(2001),
			SessionId);

		free(localip);
		int reg = send(sock, buf,strlen(buf),0);
		if(reg <= 0)
		{
			return FALSE;
		}
		else
		{
			printf(">>>>>%s",buf);
		}
		return TRUE;
	}
	return FALSE;
}

int PlayAnswer(char *cseq, int sock,int SessionId,char* urlPre,char* recvbuf)
{
	if (sock != 0)
	{
		char buf[1024];
		memset(buf,0,1024);
		char *pTemp = buf;
		char*localip;
		localip = GetLocalIP(sock);
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sRange: npt=0.000-\r\nSession: %d\r\nRTP-Info: url=rtsp://%s/%s;seq=0\r\n\r\n",
			cseq,dateHeader(),SessionId,localip,urlPre);

		free(localip);
		
		int reg = send(sock, buf,strlen(buf),0);
		if(reg <= 0)
		{
			return FALSE;
		}
		else
		{
			printf(">>>>>%s",buf);
			udpfd = socket(AF_INET,SOCK_DGRAM,0);//UDP
			struct sockaddr_in server;
			server.sin_family=AF_INET;
		   	server.sin_port=htons(g_rtspClients[0].rtpport[0]);          
		   	server.sin_addr.s_addr=inet_addr(g_rtspClients[0].IP);
			connect(udpfd,(struct sockaddr *)&server,sizeof(server));
    		printf("udp up\n");
		}
		return TRUE;
	}
	return FALSE;
}

int PauseAnswer(char *cseq,int sock,char *recvbuf)
{
	if (sock != 0)
	{
		char buf[1024];
		memset(buf,0,1024);
		char *pTemp = buf;
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%s\r\n\r\n",
			cseq,dateHeader());
	
		int reg = send(sock, buf,strlen(buf),0);
		if(reg <= 0)
		{
			return FALSE;
		}
		else
		{
			printf(">>>>>%s",buf);
		}
		return TRUE;
	}
	return FALSE;
}

int TeardownAnswer(char *cseq,int sock,int SessionId,char *recvbuf)
{
	if (sock != 0)
	{
		char buf[1024];
		memset(buf,0,1024);
		char *pTemp = buf;
		pTemp += sprintf(pTemp,"RTSP/1.0 200 OK\r\nCSeq: %s\r\n%sSession: %d\r\n\r\n",
			cseq,dateHeader(),SessionId);
	
		int reg = send(sock, buf,strlen(buf),0);
		if(reg <= 0)
		{
			return FALSE;
		}
		else
		{
			printf(">>>>>%s",buf);
			close(udpfd);
		}
		return TRUE;
	}
	return FALSE;
}
void * RtspClientMsg(void*pParam)
{
	pthread_detach(pthread_self());
	int nRes;
	char pRecvBuf[RTSP_RECV_SIZE];
	RTSP_CLIENT * pClient = (RTSP_CLIENT*)pParam;
	memset(pRecvBuf,0,sizeof(pRecvBuf));
	printf("RTSP:-----Create Client %s\n",pClient->IP);
	while(pClient->status != RTSP_IDLE)
	{
		nRes = recv(pClient->socket, pRecvBuf, RTSP_RECV_SIZE,0);
		//printf("-------------------%d\n",nRes);
		if(nRes < 1)
		{
			//usleep(1000);
			printf("RTSP:Recv Error--- %d\n",nRes);
			g_rtspClients[pClient->index].status = RTSP_IDLE;
			g_rtspClients[pClient->index].seqnum = 0;
			g_rtspClients[pClient->index].tsvid = 0;
			g_rtspClients[pClient->index].tsaud = 0;
			close(pClient->socket);
			break;
		}
		char cmdName[PARAM_STRING_MAX];
		char urlPreSuffix[PARAM_STRING_MAX];
		char urlSuffix[PARAM_STRING_MAX];
		char cseq[PARAM_STRING_MAX];

		ParseRequestString(pRecvBuf,nRes,cmdName,sizeof(cmdName),urlPreSuffix,sizeof(urlPreSuffix),
			urlSuffix,sizeof(urlSuffix),cseq,sizeof(cseq));
		
		char *p = pRecvBuf;

		printf("<<<<<%s\n",p);

		//printf("\--------------------------\n");
		//printf("%s %s\n",urlPreSuffix,urlSuffix);

		if(strstr(cmdName, "OPTIONS"))
		{
			OptionAnswer(cseq,pClient->socket);
			
		}
		else if(strstr(cmdName, "DESCRIBE"))
		{
			DescribeAnswer(cseq,pClient->socket,urlSuffix,p);
			//printf("-----------------------------DescribeAnswer %s %s\n",
			//	urlPreSuffix,urlSuffix);
		}
		else if(strstr(cmdName, "SETUP"))
		{
			int rtpport,rtcpport;
			int trackID=0;
			SetupAnswer(cseq,pClient->socket,pClient->sessionid,urlPreSuffix,p,&rtpport,&rtcpport);

			sscanf(urlSuffix, "trackID=%u", &trackID);
			//printf("----------------------------------------------TrackId %d\n",trackID);
			if(trackID<0 || trackID>=2)trackID=0;
			g_rtspClients[pClient->index].rtpport[trackID] = rtpport;
			g_rtspClients[pClient->index].rtcpport= rtcpport;
			g_rtspClients[pClient->index].reqchn = atoi(urlPreSuffix);
			if(strlen(urlPreSuffix)<100)
				strcpy(g_rtspClients[pClient->index].urlPre,urlPreSuffix);
			//printf("-----------------------------SetupAnswer %s-%d-%d\n",
			//	urlPreSuffix,g_rtspClients[pClient->index].reqchn,rtpport);
		}
		else if(strstr(cmdName, "PLAY"))
		{
			PlayAnswer(cseq,pClient->socket,pClient->sessionid,g_rtspClients[pClient->index].urlPre,p);
			g_rtspClients[pClient->index].status = RTSP_SENDING;
			printf("Start Play\n",pClient->index);
			//printf("-----------------------------PlayAnswer %d %d\n",pClient->index);
			//usleep(100);
		}
		else if(strstr(cmdName, "PAUSE"))
		{
			PauseAnswer(cseq,pClient->socket,p);
		}
		else if(strstr(cmdName, "TEARDOWN"))
		{
			TeardownAnswer(cseq,pClient->socket,pClient->sessionid,p);
			g_rtspClients[pClient->index].status = RTSP_IDLE;
			g_rtspClients[pClient->index].seqnum = 0;
			g_rtspClients[pClient->index].tsvid = 0;
			g_rtspClients[pClient->index].tsaud = 0;
			close(pClient->socket);
		}
		//if(exitok){ exitok++;return NULL; } 
	}
	printf("RTSP:-----Exit Client %s\n",pClient->IP);
	return NULL;
}

void * RtspServerListen(void*pParam)
{
	int s32Socket;
	struct sockaddr_in servaddr;
	int s32CSocket;
    	int s32Rtn;
    	int s32Socket_opt_value = 1;
	int nAddrLen;
	struct sockaddr_in addrAccept;
	int bResult;
	
	memset(&servaddr, 0, sizeof(servaddr));
    servaddr.sin_family = AF_INET;
    servaddr.sin_addr.s_addr = htonl(INADDR_ANY);
    servaddr.sin_port = htons(RTSP_SERVER_PORT); 

	s32Socket = socket(AF_INET, SOCK_STREAM, 0);

	if (setsockopt(s32Socket ,SOL_SOCKET,SO_REUSEADDR,&s32Socket_opt_value,sizeof(int)) == -1)     
    {
        return (void *)(-1);
    }
    s32Rtn = bind(s32Socket, (struct sockaddr *)&servaddr, sizeof(struct sockaddr_in));
    if(s32Rtn < 0)
    {
        return (void *)(-2);
    }
    
    s32Rtn = listen(s32Socket, 50);   
    if(s32Rtn < 0)
    {

         return (void *)(-2);
    }


	nAddrLen = sizeof(struct sockaddr_in);
	int nSessionId = 1000;
    while ((s32CSocket = accept(s32Socket, (struct sockaddr*)&addrAccept, &nAddrLen)) >= 0)
    {
		printf("<<<<RTSP Client %s Connected...\n", inet_ntoa(addrAccept.sin_addr));

		int nMaxBuf = 10 * 1024; // 脧碌脥鲁艙芦禄谩路脰脜盲 2 x nMaxBuf 碌脛禄潞鲁氓沤贸脨隆
		if(setsockopt(s32CSocket, SOL_SOCKET, SO_SNDBUF, (char*)&nMaxBuf, sizeof(nMaxBuf)) == -1)
			printf("RTSP:!!!!!! Enalarge socket sending buffer error !!!!!!\n");
		int i;
		int bAdd=FALSE;
		for(i=0;i<MAX_RTSP_CLIENT;i++)
		{
			if(g_rtspClients[i].status == RTSP_IDLE)
			{
				memset(&g_rtspClients[i],0,sizeof(RTSP_CLIENT));
				g_rtspClients[i].index = i;
				g_rtspClients[i].socket = s32CSocket;
				g_rtspClients[i].status = RTSP_CONNECTED ;//RTSP_SENDING;
				g_rtspClients[i].sessionid = nSessionId++;
				strcpy(g_rtspClients[i].IP,inet_ntoa(addrAccept.sin_addr));
				pthread_t threadIdlsn = 0;
				
				struct sched_param sched;
				sched.sched_priority = 1;
				//to return ACKecho
				pthread_create(&threadIdlsn, NULL, RtspClientMsg, &g_rtspClients[i]);
				//pthread_setschedparam(threadIdlsn,SCHED_RR,&sched);
				
				bAdd = TRUE;
				break;
			}
		}
		if(bAdd==FALSE)
		{
			memset(&g_rtspClients[0],0,sizeof(RTSP_CLIENT));
			g_rtspClients[0].index = 0;
			g_rtspClients[0].socket = s32CSocket;
			g_rtspClients[0].status = RTSP_CONNECTED ;//RTSP_SENDING;
			g_rtspClients[0].sessionid = nSessionId++;
			strcpy(g_rtspClients[0].IP,inet_ntoa(addrAccept.sin_addr));
			pthread_t threadIdlsn = 0;
			struct sched_param sched;
			sched.sched_priority = 1;
			//to return ACKecho
			pthread_create(&threadIdlsn, NULL, RtspClientMsg, &g_rtspClients[0]);
			//pthread_setschedparam(threadIdlsn,SCHED_RR,&sched);
			bAdd = TRUE;
		}
		//if(exitok){ exitok++;return NULL; } 	
    }
    if(s32CSocket < 0)
    {
       // HI_OUT_Printf(0, "RTSP listening on port %d,accept err, %d\n", RTSP_SERVER_PORT, s32CSocket);
    }

	printf("----- INIT_RTSP_Listen() Exit !! \n");
	
	return NULL;
}
HI_S32 VENC_Sent(char *buffer,int buflen)
{
    HI_S32 i;
	int is=0;
	int nChanNum=0;

	for(is=0;is<MAX_RTSP_CLIENT;is++)
	{
		if(g_rtspClients[is].status!=RTSP_SENDING)
		{
		    continue;
		}
		int heart = g_rtspClients[is].seqnum % 10000;
		
		char* nalu_payload;
		int nAvFrmLen = 0;
		int nIsIFrm = 0;
		int nNaluType = 0;
		char sendbuf[500*1024+32];

	
		nAvFrmLen = buflen;

		struct sockaddr_in server;
		server.sin_family=AF_INET;
	   	server.sin_port=htons(g_rtspClients[is].rtpport[0]);          
	   	server.sin_addr.s_addr=inet_addr(g_rtspClients[is].IP);
		int	bytes=0;
		unsigned int timestamp_increse=0;
		int test = 0;

		//printf("start---------------------\n");
		//while(test<=100)
		//{
		//	printf("%x ",buffer[test]);
		//	test++;
	//	}

		timestamp_increse=(unsigned int)(90000.0 / 30);  /* 30fps */

		rtp_hdr =(RTP_FIXED_HEADER*)&sendbuf[0]; 
	
		rtp_hdr->payload     = RTP_H264;   
		rtp_hdr->version     = 2;          
		rtp_hdr->marker    = 0;           
		rtp_hdr->ssrc      = htonl(10);   

		if(nAvFrmLen<=nalu_sent_len)
		{
			rtp_hdr->marker=1;
			rtp_hdr->seq_no     = htons(g_rtspClients[is].seqnum++); 
			nalu_hdr =(NALU_HEADER*)&sendbuf[12]; 
			nalu_hdr->F=0; 
			nalu_hdr->NRI=  nIsIFrm; 
			nalu_hdr->TYPE=  nNaluType;
			nalu_payload=&sendbuf[13];
			memcpy(nalu_payload,buffer,nAvFrmLen);
            		g_rtspClients[is].tsvid = g_rtspClients[is].tsvid+timestamp_increse;            
			rtp_hdr->timestamp=htonl(g_rtspClients[is].tsvid);
			bytes=nAvFrmLen+ 13 ;				
			sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
		}
		else if(nAvFrmLen>nalu_sent_len)
		{
			int k=0,l=0;
			k=nAvFrmLen/nalu_sent_len;
			l=nAvFrmLen%nalu_sent_len;
			int t=0;        

            		g_rtspClients[is].tsvid = g_rtspClients[is].tsvid+timestamp_increse;
           		rtp_hdr->timestamp=htonl(g_rtspClients[is].tsvid);            
			while(t<=k)
			{
				rtp_hdr->seq_no = htons(g_rtspClients[is].seqnum++);
				if(t==0)
				{
					rtp_hdr->marker=0;
					fu_ind =(FU_INDICATOR*)&sendbuf[12];
					fu_ind->F= 0; 
					fu_ind->NRI= nIsIFrm;
					fu_ind->TYPE=28;
	
					fu_hdr =(FU_HEADER*)&sendbuf[13];
					fu_hdr->E=0;
					fu_hdr->R=0;
					fu_hdr->S=1;
					fu_hdr->TYPE=nNaluType;
	
					nalu_payload=&sendbuf[14];
					memcpy(nalu_payload,buffer,nalu_sent_len);
	
					bytes=nalu_sent_len+14;					
					sendto( udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
					t++;
	
				}
				else if(k==t)
				{
				    if(l<=0){ t++; continue; }  /* 无尾包数据, 跳过 */
					rtp_hdr->marker=1;
					fu_ind =(FU_INDICATOR*)&sendbuf[12]; 
					fu_ind->F= 0 ;
					fu_ind->NRI= nIsIFrm ;
					fu_ind->TYPE=28;

					fu_hdr =(FU_HEADER*)&sendbuf[13];
					fu_hdr->R=0;
					fu_hdr->S=0;
					fu_hdr->TYPE= nNaluType;
					fu_hdr->E=1;
					nalu_payload=&sendbuf[14];
					memcpy(nalu_payload,buffer+t*nalu_sent_len,l);
					bytes=l+14;		
					sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
					t++;
				}
				else if(t<k && t!=0)
				{

					rtp_hdr->marker=0;

					fu_ind =(FU_INDICATOR*)&sendbuf[12]; 
					fu_ind->F=0; 
					fu_ind->NRI=nIsIFrm;
					fu_ind->TYPE=28;
					fu_hdr =(FU_HEADER*)&sendbuf[13];
					//fu_hdr->E=0;
					fu_hdr->R=0;
					fu_hdr->S=0;
					fu_hdr->E=0;
					fu_hdr->TYPE=nNaluType;
					nalu_payload=&sendbuf[14];
					memcpy(nalu_payload,buffer+t*nalu_sent_len,nalu_sent_len);
					bytes=nalu_sent_len+14;	
					sendto(udpfd, sendbuf, bytes, 0, (struct sockaddr *)&server,sizeof(server));
					t++;
				}
			}
		}

	}

	//------------------------------------------------------------
}


/******************************************************************************
* funciton : sent H264 stream
******************************************************************************/

HI_S32 SAMPLE_COMM_VENC_Sentjin(VENC_STREAM_S *pstStream)
{
    HI_S32 i,flag=0;

    for(i=0;i<MAX_RTSP_CLIENT;i++)//have atleast a connect
    {
		if(g_rtspClients[i].status == RTSP_SENDING)
		{
		    flag = 1;
		    break;
		}
    }
    if(flag)
    {
	    for (i = 0; i < pstStream->u32PackCount; i++)
	    {
		HI_S32 lens=0,j,lastadd=0,newadd=0,showflap=0;
		char sendbuf[320*1024];
		//char tmp[640*1024];
		lens = pstStream->pstPack[i].u32Len-pstStream->pstPack[i].u32Offset;
		memcpy(&sendbuf[0],pstStream->pstPack[i].pu8Addr+pstStream->pstPack[i].u32Offset,lens);
		//printf("lens = %d, count= %d\n",lens,count++);
		VENC_Sent(sendbuf,lens);		
		lens = 0;
	    }

	
    }
    return HI_SUCCESS;
}

HI_S32 saveStream(VENC_STREAM_S *pstStream)
{
    HI_S32 i,j,lens=0;

    for(j=0;j<MAX_RTSP_CLIENT;j++)//have atleast a connect
    {
		if(g_rtspClients[j].status == RTSP_SENDING)
		{
		    for (i = 0; i < pstStream->u32PackCount; i++)
		    {
				RTPbuf_s *p = (RTPbuf_s *)malloc(sizeof(RTPbuf_s));
                if (p == NULL) {
                    fprintf(stderr, "Memory allocation failed for RTPbuf_s.\n");
                    return HI_FAILURE;
                }
				INIT_LIST_HEAD(&(p->list));

				lens = pstStream->pstPack[i].u32Len-pstStream->pstPack[i].u32Offset;
                if (lens <= 0) {
                    fprintf(stderr, "Invalid length for RTP packet.\n");
                    free(p);
                    return HI_FAILURE;
                }

				p->buf = (char *)malloc(lens);
                if (p->buf == NULL) {
                    fprintf(stderr, "Memory allocation failed for p->buf.\n");
                    free(p);//释放上面申请的p
                    return HI_FAILURE;
                }

				p->len = lens;
				memcpy(p->buf,pstStream->pstPack[i].pu8Addr+pstStream->pstPack[i].u32Offset,lens);

				list_add_tail(&(p->list),&RTPbuf_head);
				count++;
				//printf("count = %d\n",count);
		    }
    	}
    }

    return HI_SUCCESS;
}


void RtspServer_init(void)
{
	int i;
	pthread_t threadId = 0;

	memset(&g_rtp_playload,0,sizeof(g_rtp_playload));
	strcpy(&g_rtp_playload,"G726-32");
	g_audio_rate = 8000;
	pthread_mutex_init(&g_sendmutex,NULL);
	pthread_mutex_init(&g_mutex,NULL);
	pthread_cond_init(&g_cond,NULL);
	memset(&g_rtspClients,0,sizeof(RTSP_CLIENT)*MAX_RTSP_CLIENT);
	
	//pthread_create(&g_SendDataThreadId, NULL, SendDataThread, NULL);
	
	struct sched_param thdsched;
	thdsched.sched_priority = 2;
	//to listen visiting
	pthread_create(&threadId, NULL, RtspServerListen, NULL);
	//pthread_setschedparam(threadId,SCHED_RR,&thdsched);
	printf("RTSP:-----Init Rtsp server\n");

	pthread_create(&gs_RtpPid, 0, vdRTPSendThread, NULL);
	
	//exitok++;

}
void RtspServer_exit(void)
{
	return;
}

HI_VOID* vdRTPSendThread(HI_VOID *p)
{
	while(1)
	{
		if(!list_empty(&RTPbuf_head))
		{
			
			RTPbuf_s *p = get_first_item(&RTPbuf_head,RTPbuf_s,list);
			VENC_Sent(p->buf,p->len);
			list_del(&(p->list));
			free(p->buf);
			free(p);
			p = NULL;
			count--;
			//printf("count = %d\n",count);
		}
		else
		{
			usleep(1000);  /* 队列空才休眠; 非空全速发送 */
		}
	}
}

#endif






/******************************************************************************
* function : show usage
******************************************************************************/
void SAMPLE_VENC_Usage(char* sPrgNm)
{
    printf("Usage : %s [index] \n", sPrgNm);
    printf("index:\n");
    printf("\t  0) H.265e + H264e.\n");
    printf("\t  1) Lowdelay:H.265e.\n");
    printf("\t  2) Qpmap:H.265e + H264e.\n");
    printf("\t  3) IntraRefresh:H.265e + H264e.\n");
    printf("\t  4) RoiBgFrameRate:H.265e + H.264e.\n");
    printf("\t  5) DeBreathEffect:H.265e + H.264e.\n");
    printf("\t  6) svc-t :H.264.\n");
    printf("\t  7) Mjpeg +Jpeg.\n");

    return;
}

/******************************************************************************
* function : to process abnormal case
******************************************************************************/
void SAMPLE_VENC_HandleSig(HI_S32 signo)
{
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);

    if (SIGINT == signo || SIGTERM == signo)
    {
        SAMPLE_COMM_VENC_StopSendQpmapFrame();
        SAMPLE_COMM_VENC_StopGetStream();
        SAMPLE_COMM_All_ISP_Stop();
        SAMPLE_COMM_SYS_Exit();
        printf("\033[0;31mprogram termination abnormally!\033[0;39m\n");
    }
    exit(0);   /* graceful: keep rc=255 unique for VB-stuck */
}

/******************************************************************************
* function : to process abnormal case - the case of stream venc
******************************************************************************/
void SAMPLE_VENC_StreamHandleSig(HI_S32 signo)
{

    if (SIGINT == signo || SIGTERM == signo)
    {
        SAMPLE_COMM_SYS_Exit();
        printf("\033[0;31mprogram exit abnormally!\033[0;39m\n");
    }

    exit(0);
}

VENC_GOP_MODE_E SAMPLE_VENC_GetGopMode(void)
{
    char c;
    VENC_GOP_MODE_E enGopMode = 0;

    if (getenv("SAMPLE_VENC_AUTO"))
    {
        printf("SAMPLE_VENC_AUTO set, gop mode = NORMALP\n");
        return VENC_GOPMODE_NORMALP;
    }

Begin_Get:

    printf("please input choose gop mode!\n");
    printf("\t 0) NORMALP.\n");
    printf("\t 1) DUALP.\n");
    printf("\t 2) SMARTP.\n");

    while((c = getchar()) != '\n' && c != EOF)
    switch(c)
    {
        case '0':
            enGopMode = VENC_GOPMODE_NORMALP;
            break;
        case '1':
            enGopMode = VENC_GOPMODE_DUALP;
            break;
        case '2':
            enGopMode = VENC_GOPMODE_SMARTP;
            break;
        default:
            SAMPLE_PRT("input rcmode: %c, is invaild!\n",c);
            goto Begin_Get;
    }

    return enGopMode;
}

SAMPLE_RC_E SAMPLE_VENC_GetRcMode(void)
{
    char c;
    SAMPLE_RC_E  enRcMode = 0;

    if (getenv("SAMPLE_VENC_AUTO"))
    {
        printf("SAMPLE_VENC_AUTO set, rc mode = CBR\n");
        return SAMPLE_RC_CBR;
    }

Begin_Get:

    printf("please input choose rc mode!\n");
    printf("\t c) cbr.\n");
    printf("\t v) vbr.\n");
    printf("\t a) avbr.\n");
    printf("\t x) cvbr.\n");
    printf("\t q) qvbr.\n");
    printf("\t f) fixQp\n");

    while((c = getchar()) != '\n' && c != EOF)
    switch(c)
    {
        case 'c':
            enRcMode = SAMPLE_RC_CBR;
            break;
        case 'v':
            enRcMode = SAMPLE_RC_VBR;
            break;
        case 'a':
            enRcMode = SAMPLE_RC_AVBR;
            break;
        case 'q':
            enRcMode = SAMPLE_RC_QVBR;
            break;
        case 'x':
            enRcMode = SAMPLE_RC_CVBR;
            break;
        case 'f':
            enRcMode = SAMPLE_RC_FIXQP;
            break;
        default:
            SAMPLE_PRT("input rcmode: %c, is invaild!\n",c);
            goto Begin_Get;
    }
    return enRcMode;
}

VENC_INTRA_REFRESH_MODE_E SAMPLE_VENC_GetIntraRefreshMode(void)
{
    char c;
    VENC_INTRA_REFRESH_MODE_E   enIntraRefreshMode = INTRA_REFRESH_ROW;

    if (getenv("SAMPLE_VENC_AUTO"))
    {
        printf("SAMPLE_VENC_AUTO set, intra refresh = ROW\n");
        return enIntraRefreshMode;
    }

Begin_Get:

    printf("please input choose IntraRefresh mode!\n");
    printf("\t r) ROW.\n");
    printf("\t c) COLUMN.\n");

    while((c = getchar()) != '\n' && c != EOF)
    switch(c)
    {
        case 'r':
            enIntraRefreshMode = INTRA_REFRESH_ROW;
            break;
        case 'c':
            enIntraRefreshMode = INTRA_REFRESH_COLUMN;
            break;

        default:
            SAMPLE_PRT("input IntraRefresh Mode: %c, is invaild!\n",c);
            goto Begin_Get;
    }
    return enIntraRefreshMode;
}


HI_S32 SAMPLE_VENC_SYS_Init(HI_U32 u32SupplementConfig,SAMPLE_SNS_TYPE_E  enSnsType)
{
    HI_S32 s32Ret;
    HI_U64 u64BlkSize;
    VB_CONFIG_S stVbConf;
    PIC_SIZE_E enSnsSize;
    SIZE_S     stSnsSize;

    memset(&stVbConf, 0, sizeof(VB_CONFIG_S));

    s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(enSnsType, &enSnsSize);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("SAMPLE_COMM_VI_GetSizeBySensor failed!\n");
        return s32Ret;
    }

    s32Ret = SAMPLE_COMM_SYS_GetPicSize(enSnsSize, &stSnsSize);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed!\n");
        return s32Ret;
    }

    u64BlkSize = COMMON_GetPicBufferSize(stSnsSize.u32Width, stSnsSize.u32Height, PIXEL_FORMAT_YVU_SEMIPLANAR_422, DATA_BITWIDTH_8, COMPRESS_MODE_SEG,DEFAULT_ALIGN);
    stVbConf.astCommPool[0].u64BlkSize   = u64BlkSize;
    stVbConf.astCommPool[0].u32BlkCnt    = 10;

    u64BlkSize = COMMON_GetPicBufferSize(720, 576, PIXEL_FORMAT_YVU_SEMIPLANAR_422, DATA_BITWIDTH_8, COMPRESS_MODE_SEG,DEFAULT_ALIGN);
    stVbConf.astCommPool[1].u64BlkSize   = u64BlkSize;
    stVbConf.astCommPool[1].u32BlkCnt    = 10;

    /* S2-1: 第三池专供 VPSS chn2 检测图 300x300（不压缩，与 chn 属性一致） */
    u64BlkSize = COMMON_GetPicBufferSize(320, 320, PIXEL_FORMAT_YVU_SEMIPLANAR_420,
                                         DATA_BITWIDTH_8, COMPRESS_MODE_NONE, DEFAULT_ALIGN);
    stVbConf.astCommPool[2].u64BlkSize   = u64BlkSize;
    stVbConf.astCommPool[2].u32BlkCnt    = 4;

    stVbConf.u32MaxPoolCnt = 3;

    printf("[VB] p0=%llu x%u | p1=%llu x%u | p2=%llu x%u | maxpool=%u\n",
           (unsigned long long)stVbConf.astCommPool[0].u64BlkSize, stVbConf.astCommPool[0].u32BlkCnt,
           (unsigned long long)stVbConf.astCommPool[1].u64BlkSize, stVbConf.astCommPool[1].u32BlkCnt,
           (unsigned long long)stVbConf.astCommPool[2].u64BlkSize, stVbConf.astCommPool[2].u32BlkCnt,
           stVbConf.u32MaxPoolCnt);
    fflush(stdout);

    if(0 == u32SupplementConfig)
    {
        s32Ret = SAMPLE_COMM_SYS_Init(&stVbConf);
    }
    else
    {
        s32Ret = SAMPLE_COMM_SYS_InitWithVbSupplement(&stVbConf,u32SupplementConfig);
    }
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed!\n");
        return s32Ret;
    }

    return HI_SUCCESS;
}

HI_VOID SAMPLE_VENC_SetDCFInfo(VI_PIPE ViPipe)
{
    ISP_DCF_INFO_S stIspDCF;

    HI_MPI_ISP_GetDCFInfo(ViPipe, &stIspDCF);

    //description: Thumbnail test
    strncpy((char *)stIspDCF.stIspDCFConstInfo.au8ImageDescription,"Thumbnail test",DCF_DRSCRIPTION_LENGTH);
    //manufacturer: Hisilicon
    strncpy((char *)stIspDCF.stIspDCFConstInfo.au8Make,"Hisilicon",DCF_DRSCRIPTION_LENGTH);
    //model number: Hisilicon IP Camera
    strncpy((char *)stIspDCF.stIspDCFConstInfo.au8Model,"Hisilicon IP Camera",DCF_DRSCRIPTION_LENGTH);
    //firmware version: v.1.1.0
    strncpy((char *)stIspDCF.stIspDCFConstInfo.au8Software,"v.1.1.0",DCF_DRSCRIPTION_LENGTH);


    stIspDCF.stIspDCFConstInfo.u32FocalLength             = 0x00640001;
    stIspDCF.stIspDCFConstInfo.u8Contrast                 = 5;
    stIspDCF.stIspDCFConstInfo.u8CustomRendered           = 0;
    stIspDCF.stIspDCFConstInfo.u8FocalLengthIn35mmFilm    = 1;
    stIspDCF.stIspDCFConstInfo.u8GainControl              = 1;
    stIspDCF.stIspDCFConstInfo.u8LightSource              = 1;
    stIspDCF.stIspDCFConstInfo.u8MeteringMode             = 1;
    stIspDCF.stIspDCFConstInfo.u8Saturation               = 1;
    stIspDCF.stIspDCFConstInfo.u8SceneCaptureType         = 1;
    stIspDCF.stIspDCFConstInfo.u8SceneType                = 0;
    stIspDCF.stIspDCFConstInfo.u8Sharpness                = 5;
    stIspDCF.stIspDCFUpdateInfo.u32ISOSpeedRatings         = 500;
    stIspDCF.stIspDCFUpdateInfo.u32ExposureBiasValue       = 5;
    stIspDCF.stIspDCFUpdateInfo.u32ExposureTime            = 0x00010004;
    stIspDCF.stIspDCFUpdateInfo.u32FNumber                 = 0x0001000f;
    stIspDCF.stIspDCFUpdateInfo.u8WhiteBalance             = 1;
    stIspDCF.stIspDCFUpdateInfo.u8ExposureMode             = 0;
    stIspDCF.stIspDCFUpdateInfo.u8ExposureProgram          = 1;
    stIspDCF.stIspDCFUpdateInfo.u32MaxApertureValue        = 0x00010001;

    HI_MPI_ISP_SetDCFInfo(ViPipe, &stIspDCF);

    return;
}

HI_S32 SAMPLE_VENC_VI_Init( SAMPLE_VI_CONFIG_S *pstViConfig, HI_BOOL bLowDelay, HI_U32 u32SupplementConfig)
{
    HI_S32              s32Ret;
    SAMPLE_SNS_TYPE_E   enSnsType;
    ISP_CTRL_PARAM_S    stIspCtrlParam;
    HI_U32              u32FrameRate;


    enSnsType = pstViConfig->astViInfo[0].stSnsInfo.enSnsType;

    pstViConfig->as32WorkingViId[0]                           = 0;
    //pstViConfig->s32WorkingViNum                              = 1;

    pstViConfig->astViInfo[0].stSnsInfo.MipiDev            = SAMPLE_COMM_VI_GetComboDevBySensor(pstViConfig->astViInfo[0].stSnsInfo.enSnsType, 0);
    pstViConfig->astViInfo[0].stSnsInfo.s32BusId           = 0;

    //pstViConfig->astViInfo[0].stDevInfo.ViDev              = ViDev0;
    pstViConfig->astViInfo[0].stDevInfo.enWDRMode          = WDR_MODE_NONE;

    if(HI_TRUE == bLowDelay)
    {
        pstViConfig->astViInfo[0].stPipeInfo.enMastPipeMode     = VI_ONLINE_VPSS_ONLINE;
    }
    else
    {
        pstViConfig->astViInfo[0].stPipeInfo.enMastPipeMode     = VI_OFFLINE_VPSS_OFFLINE;
    }
    s32Ret = SAMPLE_VENC_SYS_Init(u32SupplementConfig,enSnsType);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("Init SYS err for %#x!\n", s32Ret);
        return s32Ret;
    }

    //pstViConfig->astViInfo[0].stPipeInfo.aPipe[0]          = ViPipe0;
    pstViConfig->astViInfo[0].stPipeInfo.aPipe[1]          = -1;
    pstViConfig->astViInfo[0].stPipeInfo.aPipe[2]          = -1;
    pstViConfig->astViInfo[0].stPipeInfo.aPipe[3]          = -1;

    //pstViConfig->astViInfo[0].stChnInfo.ViChn              = ViChn;
    //pstViConfig->astViInfo[0].stChnInfo.enPixFormat        = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    //pstViConfig->astViInfo[0].stChnInfo.enDynamicRange     = enDynamicRange;
    pstViConfig->astViInfo[0].stChnInfo.enVideoFormat      = VIDEO_FORMAT_LINEAR;
    pstViConfig->astViInfo[0].stChnInfo.enCompressMode     = COMPRESS_MODE_SEG;//COMPRESS_MODE_SEG;
    s32Ret = SAMPLE_COMM_VI_SetParam(pstViConfig);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("SAMPLE_COMM_VI_SetParam failed with %d!\n", s32Ret);
        return s32Ret;
    }

    SAMPLE_COMM_VI_GetFrameRateBySensor(enSnsType, &u32FrameRate);

    s32Ret = HI_MPI_ISP_GetCtrlParam(pstViConfig->astViInfo[0].stPipeInfo.aPipe[0], &stIspCtrlParam);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("HI_MPI_ISP_GetCtrlParam failed with %d!\n", s32Ret);
        return s32Ret;
    }
    stIspCtrlParam.u32StatIntvl  = u32FrameRate/30;

    s32Ret = HI_MPI_ISP_SetCtrlParam(pstViConfig->astViInfo[0].stPipeInfo.aPipe[0], &stIspCtrlParam);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("HI_MPI_ISP_SetCtrlParam failed with %d!\n", s32Ret);
        return s32Ret;
    }

    s32Ret = SAMPLE_COMM_VI_StartVi(pstViConfig);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_COMM_SYS_Exit();
        SAMPLE_PRT("SAMPLE_COMM_VI_StartVi failed with %d!\n", s32Ret);
        return s32Ret;
    }

    return HI_SUCCESS;
}

HI_S32 SAMPLE_VENC_VPSS_Init(VPSS_GRP VpssGrp, HI_BOOL* pabChnEnable, DYNAMIC_RANGE_E enDynamicRange,PIXEL_FORMAT_E enPixelFormat,SIZE_S stSize[],SAMPLE_SNS_TYPE_E enSnsType)
{
    HI_S32 i;
    HI_S32 s32Ret;
    PIC_SIZE_E      enSnsSize;
    SIZE_S          stSnsSize;
    VPSS_GRP_ATTR_S stVpssGrpAttr = {0};
    VPSS_CHN_ATTR_S stVpssChnAttr[VPSS_MAX_PHY_CHN_NUM];

    s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(enSnsType, &enSnsSize);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("SAMPLE_COMM_VI_GetSizeBySensor failed!\n");
        return s32Ret;
    }

    s32Ret = SAMPLE_COMM_SYS_GetPicSize(enSnsSize, &stSnsSize);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed!\n");
        return s32Ret;
    }


    stVpssGrpAttr.enDynamicRange = enDynamicRange;
    stVpssGrpAttr.enPixelFormat  = enPixelFormat;
    stVpssGrpAttr.u32MaxW        = stSnsSize.u32Width;
    stVpssGrpAttr.u32MaxH        = stSnsSize.u32Height;
    stVpssGrpAttr.stFrameRate.s32SrcFrameRate = -1;
    stVpssGrpAttr.stFrameRate.s32DstFrameRate = -1;
    stVpssGrpAttr.bNrEn = HI_TRUE;
    stVpssGrpAttr.stNrAttr.enNrType = VPSS_NR_TYPE_VIDEO;
    stVpssGrpAttr.stNrAttr.enNrMotionMode = NR_MOTION_MODE_NORMAL;
    stVpssGrpAttr.stNrAttr.enCompressMode = COMPRESS_MODE_FRAME;


    for(i=0; i<VPSS_MAX_PHY_CHN_NUM; i++)
    {
        if(HI_TRUE == pabChnEnable[i])
        {
            stVpssChnAttr[i].u32Width                     = stSize[i].u32Width;
            stVpssChnAttr[i].u32Height                    = stSize[i].u32Height;
            stVpssChnAttr[i].enChnMode                    = VPSS_CHN_MODE_USER;
            stVpssChnAttr[i].enCompressMode               = COMPRESS_MODE_NONE;//COMPRESS_MODE_SEG;
            stVpssChnAttr[i].enDynamicRange               = enDynamicRange;
            stVpssChnAttr[i].enPixelFormat                = enPixelFormat;
            stVpssChnAttr[i].stFrameRate.s32SrcFrameRate  = -1;
            stVpssChnAttr[i].stFrameRate.s32DstFrameRate  = -1;
            stVpssChnAttr[i].u32Depth                     = (2 == i) ? 1 : 0;
            stVpssChnAttr[i].bMirror                      = HI_FALSE;
            stVpssChnAttr[i].bFlip                        = HI_FALSE;
            stVpssChnAttr[i].enVideoFormat                = VIDEO_FORMAT_LINEAR;
            stVpssChnAttr[i].stAspectRatio.enMode         = ASPECT_RATIO_NONE;
        }
    }

    s32Ret = SAMPLE_COMM_VPSS_Start(VpssGrp, pabChnEnable,&stVpssGrpAttr,stVpssChnAttr);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("start VPSS fail for %#x!\n", s32Ret);
    }

    return s32Ret;
}


HI_S32 SAMPLE_VENC_CheckSensor(SAMPLE_SNS_TYPE_E   enSnsType,SIZE_S  stSize)
{
    HI_S32 s32Ret;
    SIZE_S          stSnsSize;
    PIC_SIZE_E      enSnsSize;

    s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(enSnsType, &enSnsSize);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("SAMPLE_COMM_VI_GetSizeBySensor failed!\n");
        return s32Ret;
    }
    s32Ret = SAMPLE_COMM_SYS_GetPicSize(enSnsSize, &stSnsSize);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed!\n");
        return s32Ret;
    }

    if((stSnsSize.u32Width < stSize.u32Width) || (stSnsSize.u32Height < stSize.u32Height))
    {
        //SAMPLE_PRT("Sensor size is (%d,%d), but encode chnl is (%d,%d) !\n",
        //    stSnsSize.u32Width,stSnsSize.u32Height,stSize.u32Width,stSize.u32Height);
        return HI_FAILURE;
    }

    return HI_SUCCESS;
}

HI_S32 SAMPLE_VENC_ModifyResolution(SAMPLE_SNS_TYPE_E   enSnsType,PIC_SIZE_E *penSize,SIZE_S *pstSize)
{
    HI_S32 s32Ret;
    SIZE_S          stSnsSize;
    PIC_SIZE_E      enSnsSize;

    s32Ret = SAMPLE_COMM_VI_GetSizeBySensor(enSnsType, &enSnsSize);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("SAMPLE_COMM_VI_GetSizeBySensor failed!\n");
        return s32Ret;
    }
    s32Ret = SAMPLE_COMM_SYS_GetPicSize(enSnsSize, &stSnsSize);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed!\n");
        return s32Ret;
    }

    *penSize = enSnsSize;
    pstSize->u32Width  = stSnsSize.u32Width;
    pstSize->u32Height = stSnsSize.u32Height;

    return HI_SUCCESS;
}
/******************************************************************************
* function: H.265e + H264e@720P, H.265 Channel resolution adaptable with sensor
******************************************************************************/
/* OSD test (stage A) */
extern HI_S32 osd_test_start(HI_S32 s32VencChn);
extern HI_VOID osd_test_stop(HI_VOID);
/* ================= S2-1: VPSS chn2 取帧验证（只取帧，不推理） ================= */
#include <sys/time.h>
#include "nnie_detect.h"

#define DET_VPSS_GRP    0
#define DET_VPSS_CHN    2
static volatile HI_BOOL s_bDetectRun    = HI_FALSE;
static pthread_t        s_hDetectThread = 0;

static HI_VOID *SAMPLE_VENC_DetectProc(HI_VOID *pArgs)
{
    HI_S32  s32Ret;
    VIDEO_FRAME_INFO_S stFrame;
    HI_U32  u32W, u32H, x, y, u32Sum, u32Luma, u32Cnt = 0, u32FailCnt = 0;
    HI_U32  u32Stride, u32MapSize;
    HI_U8  *pu8Y;
    HI_VOID *pMapped;
    HI_BOOL bNnieOk = HI_FALSE;

    if (HI_SUCCESS == nnie_detect_init(HI_NULL))
    {
        bNnieOk = HI_TRUE;
        printf("[DET] NNIE model ready\n");
        if (HI_SUCCESS == nnie_draw_init(0))
        {
            printf("[DET] box overlay ready\n");
        }
        else
        {
            printf("[DET] box overlay unavailable, log-only\n");
        }
    }
    else
    {
        printf("[DET] NNIE init failed, fallback to luma-only\n");
    }

    while (HI_TRUE == s_bDetectRun)
    {
        s32Ret = HI_MPI_VPSS_GetChnFrame(DET_VPSS_GRP, DET_VPSS_CHN, &stFrame, 200);
        if (HI_SUCCESS != s32Ret)
        {
            if (u32FailCnt < 5)
            {
                printf("[DET] GetChnFrame FAILED ret=%#x\n", s32Ret);
                fflush(stdout);
                u32FailCnt++;
            }
            usleep(20000);
            continue;
        }

        u32W      = stFrame.stVFrame.u32Width;
        u32H      = stFrame.stVFrame.u32Height;
        u32Stride = stFrame.stVFrame.u32Stride[0];

        if (0 == u32W || 0 == u32H || u32W > 4096 || u32H > 4096 || u32Stride > 8192)
        {
            HI_MPI_VPSS_ReleaseChnFrame(DET_VPSS_GRP, DET_VPSS_CHN, &stFrame);
            continue;
        }

        /* VB 池给出的 virAddr 是内核地址,用户态不可直接访问,
           必须用物理地址自己映射一次 */
        u32MapSize = u32Stride * u32H * 3 / 2;
        pMapped = HI_MPI_SYS_Mmap(stFrame.stVFrame.u64PhyAddr[0], u32MapSize);
        if (HI_NULL == pMapped)
        {
            if (u32FailCnt < 5)
            {
                printf("[DET] Mmap FAIL phy=%#llx size=%u\n",
                       (unsigned long long)stFrame.stVFrame.u64PhyAddr[0], u32MapSize);
                fflush(stdout);
                u32FailCnt++;
            }
            HI_MPI_VPSS_ReleaseChnFrame(DET_VPSS_GRP, DET_VPSS_CHN, &stFrame);
            usleep(20000);
            continue;
        }

        pu8Y = (HI_U8 *)pMapped;
        u32Cnt++;

        if (bNnieOk && (u32Cnt <= 2 || 0 == (u32Cnt % 6)))
        {
            /* 约 5fps 推理一次(每 6 帧) */
            struct timeval tv0, tv1;
            const DET_BOX_S *pstBoxes;
            HI_U32 u32BoxNum = 0, i;
            gettimeofday(&tv0, HI_NULL);
            s32Ret = nnie_detect_run(pu8Y, u32W, u32H, u32Stride);
            gettimeofday(&tv1, HI_NULL);
            if (HI_SUCCESS != s32Ret)
            {
                printf("[DET] #%u infer FAILED ret=%#x\n", u32Cnt, s32Ret);
                fflush(stdout);
            }
            else
            {
                pstBoxes = nnie_detect_boxes(&u32BoxNum);
                printf("[DET] #%u infer %ums persons=%u", u32Cnt,
                       (HI_U32)((tv1.tv_sec - tv0.tv_sec) * 1000 +
                                (tv1.tv_usec - tv0.tv_usec) / 1000), u32BoxNum);
                for (i = 0; i < u32BoxNum; i++)
                {
                    printf(" (%.2f,%d,%d,%d,%d)", pstBoxes[i].f32Score,
                           pstBoxes[i].s32XMin, pstBoxes[i].s32YMin,
                           pstBoxes[i].s32XMax, pstBoxes[i].s32YMax);
                }
                printf("\n");
                fflush(stdout);
                nnie_draw_update();
            }
        }
        else if (!bNnieOk && (u32Cnt <= 3 || 0 == (u32Cnt % 30)))
        {
            /* 降级: 只统计亮度证明取帧正常 */
            u32Sum = 0;
            for (y = 0; y < u32H; y += 8)
            {
                HI_U8 *pLine = pu8Y + y * u32Stride;
                for (x = 0; x < u32W; x += 8)
                {
                    u32Sum += pLine[x];
                }
            }
            u32Luma = u32Sum / (((u32W + 7) / 8) * ((u32H + 7) / 8));
            printf("[DET] #%u %ux%u stride=%u luma=%u (no model)\n",
                   u32Cnt, u32W, u32H, u32Stride, u32Luma);
            fflush(stdout);
        }

        HI_MPI_SYS_Munmap(pMapped, u32MapSize);
        HI_MPI_VPSS_ReleaseChnFrame(DET_VPSS_GRP, DET_VPSS_CHN, &stFrame);
    }

    nnie_draw_exit();
    nnie_detect_exit();
    printf("[DET] thread exit, total=%u\n", u32Cnt);
    fflush(stdout);
    return HI_NULL;
}

HI_S32 SAMPLE_VENC_H265_H264(void)
{
    HI_S32 i;
    HI_S32 s32Ret;
    SIZE_S          stSize[3];
    PIC_SIZE_E      enSize[2]     = {PIC_3840x2160, PIC_720P};
    HI_S32          s32ChnNum     = 2;
    VENC_CHN        VencChn[2]    = {0,1};
    HI_U32          u32Profile[2] = {0,0};
    PAYLOAD_TYPE_E  enPayLoad[2]  = {PT_H264, PT_H264};
    VENC_GOP_MODE_E enGopMode;
    VENC_GOP_ATTR_S stGopAttr;
    SAMPLE_RC_E     enRcMode;
    HI_BOOL         bRcnRefShareBuf = HI_TRUE;

    VI_DEV          ViDev        = 0;
    VI_PIPE         ViPipe       = 0;
    VI_CHN          ViChn        = 0;
    SAMPLE_VI_CONFIG_S stViConfig;

    VPSS_GRP        VpssGrp        = 0;
    VPSS_CHN        VpssChn[2]     = {0,1};
    HI_BOOL         abChnEnable[VPSS_MAX_PHY_CHN_NUM] = {1,1,1};

    HI_U32 u32SupplementConfig = HI_FALSE;

    for(i=0; i<s32ChnNum; i++)
    {
        s32Ret = SAMPLE_COMM_SYS_GetPicSize(enSize[i], &stSize[i]);
        if (HI_SUCCESS != s32Ret)
        {
            SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed!\n");
            return s32Ret;
        }
    }

    SAMPLE_COMM_VI_GetSensorInfo(&stViConfig);
    if(SAMPLE_SNS_TYPE_BUTT == stViConfig.astViInfo[0].stSnsInfo.enSnsType)
    {
        SAMPLE_PRT("Not set SENSOR%d_TYPE !\n",0);
        return HI_FAILURE;
    }

    s32Ret = SAMPLE_VENC_CheckSensor(stViConfig.astViInfo[0].stSnsInfo.enSnsType,stSize[0]);
    if(s32Ret != HI_SUCCESS)
    {
        s32Ret = SAMPLE_VENC_ModifyResolution(stViConfig.astViInfo[0].stSnsInfo.enSnsType,&enSize[0],&stSize[0]);
        if(s32Ret != HI_SUCCESS)
        {
            return HI_FAILURE;
        }
    }

    stViConfig.s32WorkingViNum       = 1;
    stViConfig.astViInfo[0].stDevInfo.ViDev     = ViDev;
    stViConfig.astViInfo[0].stPipeInfo.aPipe[0] = ViPipe;
    stViConfig.astViInfo[0].stChnInfo.ViChn     = ViChn;
    stViConfig.astViInfo[0].stChnInfo.enDynamicRange = DYNAMIC_RANGE_SDR8;
    stViConfig.astViInfo[0].stChnInfo.enPixFormat    = PIXEL_FORMAT_YVU_SEMIPLANAR_420;

    s32Ret = SAMPLE_VENC_VI_Init(&stViConfig, HI_FALSE,u32SupplementConfig);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("Init VI err for %#x!\n", s32Ret);
        return HI_FAILURE;
    }

    /* S2-1: chn2 检测专用尺寸，不 bind VENC，供后续 NNIE 使用 */
    stSize[2].u32Width  = 320;
    stSize[2].u32Height = 320;
    s32Ret = SAMPLE_VENC_VPSS_Init(VpssGrp,abChnEnable,DYNAMIC_RANGE_SDR8,PIXEL_FORMAT_YVU_SEMIPLANAR_420,stSize,stViConfig.astViInfo[0].stSnsInfo.enSnsType);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Init VPSS err for %#x!\n", s32Ret);
        goto EXIT_VI_STOP;
    }

    s32Ret = SAMPLE_COMM_VI_Bind_VPSS(ViPipe, ViChn, VpssGrp);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("VI Bind VPSS err for %#x!\n", s32Ret);
        goto EXIT_VPSS_STOP;
    }

   /******************************************
    start stream venc
    ******************************************/

    enRcMode = SAMPLE_VENC_GetRcMode();

    enGopMode = SAMPLE_VENC_GetGopMode();
    s32Ret = SAMPLE_COMM_VENC_GetGopAttr(enGopMode,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }

   /***encode h.265 **/
    s32Ret = SAMPLE_COMM_VENC_Start(VencChn[0], enPayLoad[0],enSize[0], enRcMode,u32Profile[0],bRcnRefShareBuf,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Start failed for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }

    s32Ret = SAMPLE_COMM_VPSS_Bind_VENC(VpssGrp, VpssChn[0],VencChn[0]);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_STOP;
    }

    /***encode h.264 **/
    s32Ret = SAMPLE_COMM_VENC_Start(VencChn[1], enPayLoad[1], enSize[1], enRcMode,u32Profile[1],bRcnRefShareBuf,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Start failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_UnBind;
    }

    s32Ret = SAMPLE_COMM_VPSS_Bind_VENC(VpssGrp, VpssChn[1],VencChn[1]);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc bind Vpss failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H264_STOP;
    }

    /******************************************
     stream save process
    ******************************************/
    s32Ret = SAMPLE_COMM_VENC_StartGetStream(VencChn,s32ChnNum);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Start Venc failed!\n");
        goto EXIT_VENC_H264_UnBind;
    }
    /* OSD stage A: 静态色块叠加到 VENC chn0 */
    if (HI_SUCCESS != osd_test_start(VencChn[0]))
    {
        SAMPLE_PRT("OSD test start failed, continue without OSD\n");
    }

    /* S2-1: 启动 chn2 取帧验证线程 */
    s_bDetectRun = HI_TRUE;
    if (0 != pthread_create(&s_hDetectThread, HI_NULL, SAMPLE_VENC_DetectProc, HI_NULL))
    {
        s_bDetectRun = HI_FALSE;
        SAMPLE_PRT("S2-1 detect thread create failed\n");
    }
    else
    {
        printf("[DET] thread started: vpss grp%d chn%d\n", DET_VPSS_GRP, DET_VPSS_CHN);
        fflush(stdout);
    }
    if (getenv("SAMPLE_VENC_AUTO"))
    {
        printf("non-interactive mode: waiting for SIGTERM/SIGINT to exit\n");
        while (1) { pause(); }
    }
    printf("please press twice ENTER to exit this sample\n");
    getchar();
    getchar();

    /******************************************
     exit process
    ******************************************/
    /* S2-1: 停检测线程 */
    if (s_hDetectThread)
    {
        s_bDetectRun = HI_FALSE;
        pthread_join(s_hDetectThread, HI_NULL);
        s_hDetectThread = 0;
    }
    SAMPLE_COMM_VENC_StopGetStream();

EXIT_VENC_H264_UnBind:
    SAMPLE_COMM_VPSS_UnBind_VENC(VpssGrp,VpssChn[1],VencChn[1]);
EXIT_VENC_H264_STOP:
    SAMPLE_COMM_VENC_Stop(VencChn[1]);
EXIT_VENC_H265_UnBind:
    SAMPLE_COMM_VPSS_UnBind_VENC(VpssGrp,VpssChn[0],VencChn[0]);
EXIT_VENC_H265_STOP:
    SAMPLE_COMM_VENC_Stop(VencChn[0]);
EXIT_VI_VPSS_UNBIND:
    SAMPLE_COMM_VI_UnBind_VPSS(ViPipe,ViChn,VpssGrp);
EXIT_VPSS_STOP:
    SAMPLE_COMM_VPSS_Stop(VpssGrp,abChnEnable);
EXIT_VI_STOP:
    SAMPLE_COMM_VI_StopVi(&stViConfig);
    SAMPLE_COMM_SYS_Exit();

    return s32Ret;
}



/******************************************************************************
* function: lowdelay H265e, Channel resolution adaptable with sensor
******************************************************************************/
HI_S32 SAMPLE_VENC_LOW_DELAY(void)
{
    HI_S32 i;
    HI_S32 s32Ret;
    SIZE_S          stSize[2];
    PIC_SIZE_E      enSize[2]     = {PIC_3840x2160, PIC_720P};

    HI_S32          s32ChnNum     = 1;
    VENC_CHN        VencChn[2]    = {0,1};
    HI_U32          u32Profile[2] = {0,0};
    PAYLOAD_TYPE_E  enPayLoad[2]  = {PT_H265, PT_H264};
    VENC_GOP_MODE_E enGopMode;
    VENC_GOP_ATTR_S stGopAttr;
    SAMPLE_RC_E     enRcMode;
    HI_BOOL         bRcnRefShareBuf = HI_TRUE;

    VI_DEV          ViDev        = 0;
    VI_PIPE         ViPipe       = 0;
    VI_CHN          ViChn        = 0;
    SAMPLE_VI_CONFIG_S stViConfig;

    VPSS_GRP        VpssGrp        = 0;
    VPSS_CHN        VpssChn[2]     = {0,1};
    HI_BOOL         abChnEnable[4] = {1,0,0,0};
    VPSS_LOW_DELAY_INFO_S  stLowDelayInfo;

    HI_U32 u32SupplementConfig = HI_FALSE;

    for(i=0; i<s32ChnNum; i++)
    {
        s32Ret = SAMPLE_COMM_SYS_GetPicSize(enSize[i], &stSize[i]);
        if (HI_SUCCESS != s32Ret)
        {
            SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed!\n");
            return s32Ret;
        }
    }

    SAMPLE_COMM_VI_GetSensorInfo(&stViConfig);
    if(SAMPLE_SNS_TYPE_BUTT == stViConfig.astViInfo[0].stSnsInfo.enSnsType)
    {
        SAMPLE_PRT("Not set SENSOR%d_TYPE !\n",0);
        return HI_FAILURE;
    }


    s32Ret = SAMPLE_VENC_CheckSensor(stViConfig.astViInfo[0].stSnsInfo.enSnsType,stSize[0]);
    if(s32Ret != HI_SUCCESS)
    {
        s32Ret = SAMPLE_VENC_ModifyResolution(stViConfig.astViInfo[0].stSnsInfo.enSnsType,&enSize[0],&stSize[0]);
        if(s32Ret != HI_SUCCESS)
        {
            return HI_FAILURE;
        }
    }

    stViConfig.s32WorkingViNum                  = 1;
    stViConfig.astViInfo[0].stDevInfo.ViDev     = ViDev;
    stViConfig.astViInfo[0].stPipeInfo.aPipe[0] = ViPipe;
    stViConfig.astViInfo[0].stChnInfo.ViChn     = ViChn;
    stViConfig.astViInfo[0].stChnInfo.enDynamicRange = DYNAMIC_RANGE_SDR8;
    stViConfig.astViInfo[0].stChnInfo.enPixFormat    = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    s32Ret = SAMPLE_VENC_VI_Init(&stViConfig, HI_TRUE,u32SupplementConfig);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("Init VI err for %#x!\n", s32Ret);
        return HI_FAILURE;
    }


    s32Ret = SAMPLE_VENC_VPSS_Init(VpssGrp,abChnEnable,DYNAMIC_RANGE_SDR8,PIXEL_FORMAT_YVU_SEMIPLANAR_420,stSize,stViConfig.astViInfo[0].stSnsInfo.enSnsType);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Init VPSS err for %#x!\n", s32Ret);
        goto EXIT_VI_STOP;
    }

    stLowDelayInfo.bEnable = HI_TRUE;
    stLowDelayInfo.u32LineCnt = stSize[0].u32Height/2;
    s32Ret = HI_MPI_VPSS_SetLowDelayAttr(VpssGrp,VpssChn[0],&stLowDelayInfo);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("Set LowDelay Attr for %#x!\n", s32Ret);
        goto EXIT_VPSS_STOP;
    }

    s32Ret = SAMPLE_COMM_VI_Bind_VPSS(ViPipe, ViChn, VpssGrp);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("VI Bind VPSS err for %#x!\n", s32Ret);
        goto EXIT_VPSS_STOP;
    }

   /******************************************
     step 4: start stream venc
    ******************************************/

    enRcMode = SAMPLE_VENC_GetRcMode();

    enGopMode = SAMPLE_VENC_GetGopMode();
    s32Ret = SAMPLE_COMM_VENC_GetGopAttr(enGopMode,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }
    if(VENC_GOPMODE_BIPREDB == enGopMode)
    {
        SAMPLE_PRT("BIPREDB and ADVSMARTP not support lowdelay!\n");
        goto EXIT_VI_VPSS_UNBIND;
    }
   /***encode h.265 **/
    s32Ret = SAMPLE_COMM_VENC_Start(VencChn[0], enPayLoad[0],enSize[0], enRcMode,u32Profile[0],bRcnRefShareBuf,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Start failed for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }

    s32Ret = SAMPLE_COMM_VPSS_Bind_VENC(VpssGrp, VpssChn[0],VencChn[0]);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_STOP;
    }


    /******************************************
     stream save process
    ******************************************/
    s32Ret = SAMPLE_COMM_VENC_StartGetStream(VencChn,s32ChnNum);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Start Venc failed!\n");
        goto EXIT_VENC_H265_UnBind;
    }

    if (getenv("SAMPLE_VENC_AUTO"))
    {
        printf("non-interactive mode: waiting for SIGTERM/SIGINT to exit\n");
        while (1) { pause(); }
    }
    printf("please press twice ENTER to exit this sample\n");
    getchar();
    getchar();

    /******************************************
     exit process
    ******************************************/
    SAMPLE_COMM_VENC_StopGetStream();


EXIT_VENC_H265_UnBind:
    SAMPLE_COMM_VPSS_UnBind_VENC(VpssGrp,VpssChn[0],VencChn[0]);
EXIT_VENC_H265_STOP:
    SAMPLE_COMM_VENC_Stop(VencChn[0]);
EXIT_VI_VPSS_UNBIND:
    SAMPLE_COMM_VI_UnBind_VPSS(ViPipe,ViChn,VpssGrp);
EXIT_VPSS_STOP:
    SAMPLE_COMM_VPSS_Stop(VpssGrp,abChnEnable);
EXIT_VI_STOP:
    SAMPLE_COMM_VI_StopVi(&stViConfig);
    SAMPLE_COMM_SYS_Exit();

    return s32Ret;
}


/******************************************************************************
* function: IntraRefresh, H.265e + H264e@720P,H.265 Channel resolution adaptable with sensor
******************************************************************************/
HI_S32 SAMPLE_VENC_IntraRefresh(void)
{
    HI_S32 i;
    HI_S32 s32Ret;
    SIZE_S          stSize[2];
    PIC_SIZE_E      enSize[2]     = {PIC_3840x2160, PIC_720P};

    HI_S32          s32ChnNum     = 2;
    VENC_CHN        VencChn[2]    = {0,1};
    HI_U32          u32Profile[2] = {0,0};
    PAYLOAD_TYPE_E  enPayLoad[2]  = {PT_H265, PT_H264};
    VENC_GOP_MODE_E enGopMode     = VENC_GOPMODE_NORMALP;
    VENC_GOP_ATTR_S stGopAttr;
    SAMPLE_RC_E     enRcMode;
    HI_BOOL         bRcnRefShareBuf = HI_TRUE;

    VENC_INTRA_REFRESH_S  stIntraRefresh;
    VENC_INTRA_REFRESH_MODE_E   enIntraRefreshMode;

    VI_DEV          ViDev        = 0;
    VI_PIPE         ViPipe       = 0;
    VI_CHN          ViChn        = 0;
    SAMPLE_VI_CONFIG_S stViConfig;

    VPSS_GRP        VpssGrp        = 0;
    VPSS_CHN        VpssChn[2]     = {0,1};
    HI_BOOL         abChnEnable[4] = {1,1,0,0};

    HI_U32 u32SupplementConfig = HI_FALSE;

    for(i=0; i<s32ChnNum; i++)
    {
        s32Ret = SAMPLE_COMM_SYS_GetPicSize(enSize[i], &stSize[i]);
        if (HI_SUCCESS != s32Ret)
        {
            SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed!\n");
            return s32Ret;
        }
    }

    SAMPLE_COMM_VI_GetSensorInfo(&stViConfig);
    if(SAMPLE_SNS_TYPE_BUTT == stViConfig.astViInfo[0].stSnsInfo.enSnsType)
    {
        SAMPLE_PRT("Not set SENSOR%d_TYPE !\n",0);
        return HI_FAILURE;
    }

    s32Ret = SAMPLE_VENC_CheckSensor(stViConfig.astViInfo[0].stSnsInfo.enSnsType,stSize[0]);
    if(s32Ret != HI_SUCCESS)
    {
        s32Ret = SAMPLE_VENC_ModifyResolution(stViConfig.astViInfo[0].stSnsInfo.enSnsType,&enSize[0],&stSize[0]);
        if(s32Ret != HI_SUCCESS)
        {
            return HI_FAILURE;
        }
    }


    stViConfig.s32WorkingViNum       = 1;
    stViConfig.astViInfo[0].stDevInfo.ViDev     = ViDev;
    stViConfig.astViInfo[0].stPipeInfo.aPipe[0] = ViPipe;
    stViConfig.astViInfo[0].stChnInfo.ViChn     = ViChn;
    stViConfig.astViInfo[0].stChnInfo.enDynamicRange = DYNAMIC_RANGE_SDR8;
    stViConfig.astViInfo[0].stChnInfo.enPixFormat    = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    s32Ret = SAMPLE_VENC_VI_Init(&stViConfig, HI_FALSE,u32SupplementConfig);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("Init VI err for %#x!\n", s32Ret);
        return HI_FAILURE;
    }

    s32Ret = SAMPLE_VENC_VPSS_Init(VpssGrp,abChnEnable,DYNAMIC_RANGE_SDR8,PIXEL_FORMAT_YVU_SEMIPLANAR_420,stSize,stViConfig.astViInfo[0].stSnsInfo.enSnsType);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Init VPSS err for %#x!\n", s32Ret);
        goto EXIT_VI_STOP;
    }

    s32Ret = SAMPLE_COMM_VI_Bind_VPSS(ViPipe, ViChn, VpssGrp);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("VI Bind VPSS err for %#x!\n", s32Ret);
        goto EXIT_VPSS_STOP;
    }

   /******************************************
     start stream venc
    ******************************************/

    enRcMode = SAMPLE_VENC_GetRcMode();
    enIntraRefreshMode = SAMPLE_VENC_GetIntraRefreshMode();

    s32Ret = SAMPLE_COMM_VENC_GetGopAttr(enGopMode,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }

   /***encode h.265 **/
    s32Ret = SAMPLE_COMM_VENC_Start(VencChn[0], enPayLoad[0],enSize[0], enRcMode,u32Profile[0],bRcnRefShareBuf,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Start failed for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }

    s32Ret = SAMPLE_COMM_VPSS_Bind_VENC(VpssGrp, VpssChn[0],VencChn[0]);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_STOP;
    }

    s32Ret = HI_MPI_VENC_GetIntraRefresh(VencChn[0],&stIntraRefresh);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Get Intra Refresh failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_UnBind;
    }

    stIntraRefresh.bRefreshEnable = HI_TRUE;
    stIntraRefresh.enIntraRefreshMode = enIntraRefreshMode;
    if(INTRA_REFRESH_ROW == enIntraRefreshMode)
    {
        stIntraRefresh.u32RefreshNum = (stSize[0].u32Height + 63)>>8;
    }
    else
    {
        stIntraRefresh.u32RefreshNum = (stSize[0].u32Width+ 63)>>8;
    }
    stIntraRefresh.u32ReqIQp = 30;
    s32Ret = HI_MPI_VENC_SetIntraRefresh(VencChn[0],&stIntraRefresh);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Set Intra Refresh failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_UnBind;
    }

    /***encode h.264 **/
    s32Ret = SAMPLE_COMM_VENC_Start(VencChn[1], enPayLoad[1], enSize[1], enRcMode,u32Profile[1],bRcnRefShareBuf,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Start failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_UnBind;
    }

    s32Ret = SAMPLE_COMM_VPSS_Bind_VENC(VpssGrp, VpssChn[1],VencChn[1]);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc bind Vpss failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H264_STOP;
    }

    s32Ret = HI_MPI_VENC_GetIntraRefresh(VencChn[1],&stIntraRefresh);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Get Intra Refresh failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H264_UnBind;
    }

    stIntraRefresh.bRefreshEnable = HI_TRUE;
    stIntraRefresh.enIntraRefreshMode = enIntraRefreshMode;
    if(INTRA_REFRESH_ROW == enIntraRefreshMode)
    {
        stIntraRefresh.u32RefreshNum = (stSize[0].u32Height + 15)>>8;
    }
    else
    {
        stIntraRefresh.u32RefreshNum = (stSize[0].u32Width+ 15)>>8;
    }

    stIntraRefresh.u32ReqIQp = 30;
    s32Ret = HI_MPI_VENC_SetIntraRefresh(VencChn[1],&stIntraRefresh);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Set Intra Refresh failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H264_UnBind;
    }

    /******************************************
     stream save process
    ******************************************/
    s32Ret = SAMPLE_COMM_VENC_StartGetStream(VencChn,s32ChnNum);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Start Venc failed!\n");
        goto EXIT_VENC_H264_UnBind;
    }

    if (getenv("SAMPLE_VENC_AUTO"))
    {
        printf("non-interactive mode: waiting for SIGTERM/SIGINT to exit\n");
        while (1) { pause(); }
    }
    printf("please press twice ENTER to exit this sample\n");
    getchar();
    getchar();

    /******************************************
     exit process
    ******************************************/
    SAMPLE_COMM_VENC_StopGetStream();

EXIT_VENC_H264_UnBind:
    SAMPLE_COMM_VPSS_UnBind_VENC(VpssGrp,VpssChn[1],VencChn[1]);
EXIT_VENC_H264_STOP:
    SAMPLE_COMM_VENC_Stop(VencChn[1]);
EXIT_VENC_H265_UnBind:
    SAMPLE_COMM_VPSS_UnBind_VENC(VpssGrp,VpssChn[0],VencChn[0]);
EXIT_VENC_H265_STOP:
    SAMPLE_COMM_VENC_Stop(VencChn[0]);
EXIT_VI_VPSS_UNBIND:
    SAMPLE_COMM_VI_UnBind_VPSS(ViPipe,ViChn,VpssGrp);
EXIT_VPSS_STOP:
    SAMPLE_COMM_VPSS_Stop(VpssGrp,abChnEnable);
EXIT_VI_STOP:
    SAMPLE_COMM_VI_StopVi(&stViConfig);
    SAMPLE_COMM_SYS_Exit();

    return s32Ret;
}


/******************************************************************************
* function: RoiBgFrameRate, H.265 + H.264@720P,H.265 Channel resolution adaptable with sensor
******************************************************************************/
HI_S32 SAMPLE_VENC_ROIBG(void)
{
    HI_S32 i;
    HI_S32 s32Ret;
    SIZE_S          stSize[2];
    PIC_SIZE_E      enSize[2]     = {PIC_3840x2160, PIC_720P};

    HI_S32          s32ChnNum     = 2;
    VENC_CHN        VencChn[2]    = {0,1};
    HI_U32          u32Profile[2] = {0,0};
    PAYLOAD_TYPE_E  enPayLoad[2]  = {PT_H265, PT_H264};
    VENC_GOP_MODE_E enGopMode     = VENC_GOPMODE_NORMALP;
    VENC_GOP_ATTR_S stGopAttr;
    SAMPLE_RC_E     enRcMode;
    HI_BOOL         bRcnRefShareBuf = HI_TRUE;

    VENC_ROI_ATTR_S  stRoiAttr;
    VENC_ROIBG_FRAME_RATE_S  stRoiBgFrameRate;

    VI_DEV          ViDev        = 0;
    VI_PIPE         ViPipe       = 0;
    VI_CHN          ViChn        = 0;
    SAMPLE_VI_CONFIG_S stViConfig;

    VPSS_GRP        VpssGrp        = 0;
    VPSS_CHN        VpssChn[2]     = {0,1};
    HI_BOOL         abChnEnable[4] = {1,1,0,0};

    HI_U32 u32SupplementConfig = HI_FALSE;

    for(i=0; i<s32ChnNum; i++)
    {
        s32Ret = SAMPLE_COMM_SYS_GetPicSize(enSize[i], &stSize[i]);
        if (HI_SUCCESS != s32Ret)
        {
            SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed!\n");
            return s32Ret;
        }
    }

    SAMPLE_COMM_VI_GetSensorInfo(&stViConfig);
    if(SAMPLE_SNS_TYPE_BUTT == stViConfig.astViInfo[0].stSnsInfo.enSnsType)
    {
        SAMPLE_PRT("Not set SENSOR%d_TYPE !\n",0);
        return HI_FAILURE;
    }

    s32Ret = SAMPLE_VENC_CheckSensor(stViConfig.astViInfo[0].stSnsInfo.enSnsType,stSize[0]);
    if(s32Ret != HI_SUCCESS)
    {
        s32Ret = SAMPLE_VENC_ModifyResolution(stViConfig.astViInfo[0].stSnsInfo.enSnsType,&enSize[0],&stSize[0]);
        if(s32Ret != HI_SUCCESS)
        {
            return HI_FAILURE;
        }
    }

    stViConfig.s32WorkingViNum       = 1;
    stViConfig.astViInfo[0].stDevInfo.ViDev     = ViDev;
    stViConfig.astViInfo[0].stPipeInfo.aPipe[0] = ViPipe;
    stViConfig.astViInfo[0].stChnInfo.ViChn     = ViChn;
    stViConfig.astViInfo[0].stChnInfo.enDynamicRange = DYNAMIC_RANGE_SDR8;
    stViConfig.astViInfo[0].stChnInfo.enPixFormat    = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    s32Ret = SAMPLE_VENC_VI_Init(&stViConfig, HI_FALSE,u32SupplementConfig);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("Init VI err for %#x!\n", s32Ret);
        return HI_FAILURE;
    }

    s32Ret = SAMPLE_VENC_VPSS_Init(VpssGrp,abChnEnable,DYNAMIC_RANGE_SDR8,PIXEL_FORMAT_YVU_SEMIPLANAR_420,stSize,stViConfig.astViInfo[0].stSnsInfo.enSnsType);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Init VPSS err for %#x!\n", s32Ret);
        goto EXIT_VI_STOP;
    }

    s32Ret = SAMPLE_COMM_VI_Bind_VPSS(ViPipe, ViChn, VpssGrp);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("VI Bind VPSS err for %#x!\n", s32Ret);
        goto EXIT_VPSS_STOP;
    }

   /******************************************
     start stream venc
    ******************************************/

    enRcMode = SAMPLE_VENC_GetRcMode();

    s32Ret = SAMPLE_COMM_VENC_GetGopAttr(enGopMode,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }

   /***encode h.265 **/
    s32Ret = SAMPLE_COMM_VENC_Start(VencChn[0], enPayLoad[0],enSize[0], enRcMode,u32Profile[0],bRcnRefShareBuf,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Start failed for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }

    s32Ret = SAMPLE_COMM_VPSS_Bind_VENC(VpssGrp, VpssChn[0],VencChn[0]);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_STOP;
    }

    s32Ret = HI_MPI_VENC_GetRoiAttr(VencChn[0], 0, &stRoiAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Get Roi Attr failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_UnBind;
    }

    stRoiAttr.bAbsQp   = HI_TRUE;
    stRoiAttr.bEnable  = HI_TRUE;
    stRoiAttr.s32Qp    = 30;
    stRoiAttr.u32Index = 0;
    stRoiAttr.stRect.s32X = 64;
    stRoiAttr.stRect.s32Y = 64;
    stRoiAttr.stRect.u32Height = 256;
    stRoiAttr.stRect.u32Width = 256;

    s32Ret = HI_MPI_VENC_SetRoiAttr(VencChn[0],&stRoiAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Set Roi Attr failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_UnBind;
    }

    s32Ret = HI_MPI_VENC_GetRoiBgFrameRate(VencChn[0],&stRoiBgFrameRate);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Get Roi BgFrameRate failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_UnBind;
    }

    stRoiBgFrameRate.s32SrcFrmRate = 30;
    stRoiBgFrameRate.s32DstFrmRate = 15;

    s32Ret = HI_MPI_VENC_SetRoiBgFrameRate(VencChn[0],&stRoiBgFrameRate);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Set Roi BgFrameRate failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_UnBind;
    }


    /***encode h.264 **/
    s32Ret = SAMPLE_COMM_VENC_Start(VencChn[1], enPayLoad[1], enSize[1], enRcMode,u32Profile[1],bRcnRefShareBuf,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Start failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_UnBind;
    }

    s32Ret = SAMPLE_COMM_VPSS_Bind_VENC(VpssGrp, VpssChn[1],VencChn[1]);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc bind Vpss failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H264_STOP;
    }

    s32Ret = HI_MPI_VENC_GetRoiAttr(VencChn[1], 0, &stRoiAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Get Roi Attr failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H264_UnBind;
    }

    stRoiAttr.bAbsQp   = HI_TRUE;
    stRoiAttr.bEnable  = HI_TRUE;
    stRoiAttr.s32Qp    = 30;
    stRoiAttr.u32Index = 0;
    stRoiAttr.stRect.s32X = 64;
    stRoiAttr.stRect.s32Y = 64;
    stRoiAttr.stRect.u32Height = 256;
    stRoiAttr.stRect.u32Width = 256;

    s32Ret = HI_MPI_VENC_SetRoiAttr(VencChn[1],&stRoiAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Set Roi Attr failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H264_UnBind;
    }

    s32Ret = HI_MPI_VENC_GetRoiBgFrameRate(VencChn[1],&stRoiBgFrameRate);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Get Roi BgFrameRate failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H264_UnBind;
    }

    stRoiBgFrameRate.s32SrcFrmRate = 30;
    stRoiBgFrameRate.s32DstFrmRate = 15;

    s32Ret = HI_MPI_VENC_SetRoiBgFrameRate(VencChn[1],&stRoiBgFrameRate);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Set Roi BgFrameRate failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H264_UnBind;
    }

    /******************************************
     stream save process
    ******************************************/
    s32Ret = SAMPLE_COMM_VENC_StartGetStream(VencChn,s32ChnNum);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Start Venc failed!\n");
        goto EXIT_VENC_H264_UnBind;
    }

    if (getenv("SAMPLE_VENC_AUTO"))
    {
        printf("non-interactive mode: waiting for SIGTERM/SIGINT to exit\n");
        while (1) { pause(); }
    }
    printf("please press twice ENTER to exit this sample\n");
    getchar();
    getchar();

    /******************************************
     exit process
    ******************************************/
    SAMPLE_COMM_VENC_StopGetStream();

EXIT_VENC_H264_UnBind:
    SAMPLE_COMM_VPSS_UnBind_VENC(VpssGrp,VpssChn[1],VencChn[1]);
EXIT_VENC_H264_STOP:
    SAMPLE_COMM_VENC_Stop(VencChn[1]);
EXIT_VENC_H265_UnBind:
    SAMPLE_COMM_VPSS_UnBind_VENC(VpssGrp,VpssChn[0],VencChn[0]);
EXIT_VENC_H265_STOP:
    SAMPLE_COMM_VENC_Stop(VencChn[0]);
EXIT_VI_VPSS_UNBIND:
    SAMPLE_COMM_VI_UnBind_VPSS(ViPipe,ViChn,VpssGrp);
EXIT_VPSS_STOP:
    SAMPLE_COMM_VPSS_Stop(VpssGrp,abChnEnable);
EXIT_VI_STOP:
    SAMPLE_COMM_VI_StopVi(&stViConfig);
    SAMPLE_COMM_SYS_Exit();

    return s32Ret;
}

/******************************************************************************
* function: DeBreathEffect, H.265e + H264e@720P, H.265 Channel resolution adaptable with sensor
******************************************************************************/
HI_S32 SAMPLE_VENC_DeBreathEffect(void)
{
    HI_S32 i;
    HI_S32 s32Ret;
    SIZE_S          stSize[2];
    PIC_SIZE_E      enSize[2]     = {PIC_3840x2160, PIC_720P};

    HI_S32          s32ChnNum     = 2;
    VENC_CHN        VencChn[2]    = {0,1};
    HI_U32          u32Profile[2] = {0,0};
    PAYLOAD_TYPE_E  enPayLoad[2]  = {PT_H265, PT_H264};
    VENC_GOP_MODE_E enGopMode     = VENC_GOPMODE_NORMALP;
    VENC_GOP_ATTR_S stGopAttr;
    SAMPLE_RC_E     enRcMode;
    HI_BOOL         bRcnRefShareBuf = HI_FALSE;

    VENC_DEBREATHEFFECT_S  stDeBreathEffect;

    VI_DEV          ViDev        = 0;
    VI_PIPE         ViPipe       = 0;
    VI_CHN          ViChn        = 0;
    SAMPLE_VI_CONFIG_S stViConfig;

    VPSS_GRP        VpssGrp        = 0;
    VPSS_CHN        VpssChn[2]     = {0,1};
    HI_BOOL         abChnEnable[4] = {1,1,0,0};

    HI_U32 u32SupplementConfig = HI_FALSE;

    for(i=0; i<s32ChnNum; i++)
    {
        s32Ret = SAMPLE_COMM_SYS_GetPicSize(enSize[i], &stSize[i]);
        if (HI_SUCCESS != s32Ret)
        {
            SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed!\n");
            return s32Ret;
        }
    }

    SAMPLE_COMM_VI_GetSensorInfo(&stViConfig);
    if(SAMPLE_SNS_TYPE_BUTT == stViConfig.astViInfo[0].stSnsInfo.enSnsType)
    {
        SAMPLE_PRT("Not set SENSOR%d_TYPE !\n",0);
        return HI_FAILURE;
    }

    s32Ret = SAMPLE_VENC_CheckSensor(stViConfig.astViInfo[0].stSnsInfo.enSnsType,stSize[0]);
    if(s32Ret != HI_SUCCESS)
    {
        s32Ret = SAMPLE_VENC_ModifyResolution(stViConfig.astViInfo[0].stSnsInfo.enSnsType,&enSize[0],&stSize[0]);
        if(s32Ret != HI_SUCCESS)
        {
            return HI_FAILURE;
        }
    }

    stViConfig.s32WorkingViNum       = 1;
    stViConfig.astViInfo[0].stDevInfo.ViDev     = ViDev;
    stViConfig.astViInfo[0].stPipeInfo.aPipe[0] = ViPipe;
    stViConfig.astViInfo[0].stChnInfo.ViChn     = ViChn;
    stViConfig.astViInfo[0].stChnInfo.enDynamicRange = DYNAMIC_RANGE_SDR8;
    stViConfig.astViInfo[0].stChnInfo.enPixFormat    = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    s32Ret = SAMPLE_VENC_VI_Init(&stViConfig, HI_FALSE,u32SupplementConfig);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("Init VI err for %#x!\n", s32Ret);
        return HI_FAILURE;
    }

    s32Ret = SAMPLE_VENC_VPSS_Init(VpssGrp,abChnEnable,DYNAMIC_RANGE_SDR8,PIXEL_FORMAT_YVU_SEMIPLANAR_420,stSize,stViConfig.astViInfo[0].stSnsInfo.enSnsType);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Init VPSS err for %#x!\n", s32Ret);
        goto EXIT_VI_STOP;
    }

    s32Ret = SAMPLE_COMM_VI_Bind_VPSS(ViPipe, ViChn, VpssGrp);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("VI Bind VPSS err for %#x!\n", s32Ret);
        goto EXIT_VPSS_STOP;
    }

   /******************************************
     start stream venc
    ******************************************/

    enRcMode = SAMPLE_VENC_GetRcMode();

    enGopMode = SAMPLE_VENC_GetGopMode();

    s32Ret = SAMPLE_COMM_VENC_GetGopAttr(enGopMode,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }

   /***encode h.265 **/
    s32Ret = SAMPLE_COMM_VENC_Start(VencChn[0], enPayLoad[0],enSize[0], enRcMode,u32Profile[0],bRcnRefShareBuf,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Start failed for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }

    s32Ret = SAMPLE_COMM_VPSS_Bind_VENC(VpssGrp, VpssChn[0],VencChn[0]);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_STOP;
    }

    s32Ret = HI_MPI_VENC_GetDeBreathEffect(VencChn[0],&stDeBreathEffect);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Get  DeBreathEffect failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_UnBind;
    }

    stDeBreathEffect.bEnable = HI_TRUE;
    s32Ret = HI_MPI_VENC_SetDeBreathEffect(VencChn[0],&stDeBreathEffect);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Set DeBreathEffect failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_UnBind;
    }

    /***encode h.264 **/
    s32Ret = SAMPLE_COMM_VENC_Start(VencChn[1], enPayLoad[1], enSize[1], enRcMode,u32Profile[1],bRcnRefShareBuf,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Start failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_UnBind;
    }

    s32Ret = SAMPLE_COMM_VPSS_Bind_VENC(VpssGrp, VpssChn[1],VencChn[1]);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc bind Vpss failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H264_STOP;
    }

    s32Ret = HI_MPI_VENC_GetDeBreathEffect(VencChn[1],&stDeBreathEffect);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Get DeBreathEffect failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H264_UnBind;
    }

    stDeBreathEffect.bEnable = HI_TRUE;
    s32Ret = HI_MPI_VENC_SetDeBreathEffect(VencChn[1],&stDeBreathEffect);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Set DeBreathEffect failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H264_UnBind;
    }

    /******************************************
     stream save process
    ******************************************/
    s32Ret = SAMPLE_COMM_VENC_StartGetStream(VencChn,s32ChnNum);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Start Venc failed!\n");
        goto EXIT_VENC_H264_UnBind;
    }

    if (getenv("SAMPLE_VENC_AUTO"))
    {
        printf("non-interactive mode: waiting for SIGTERM/SIGINT to exit\n");
        while (1) { pause(); }
    }
    printf("please press twice ENTER to exit this sample\n");
    getchar();
    getchar();

    /******************************************
     exit process
    ******************************************/
    SAMPLE_COMM_VENC_StopGetStream();

EXIT_VENC_H264_UnBind:
    SAMPLE_COMM_VPSS_UnBind_VENC(VpssGrp,VpssChn[1],VencChn[1]);
EXIT_VENC_H264_STOP:
    SAMPLE_COMM_VENC_Stop(VencChn[1]);
EXIT_VENC_H265_UnBind:
    SAMPLE_COMM_VPSS_UnBind_VENC(VpssGrp,VpssChn[0],VencChn[0]);
EXIT_VENC_H265_STOP:
    SAMPLE_COMM_VENC_Stop(VencChn[0]);
EXIT_VI_VPSS_UNBIND:
    SAMPLE_COMM_VI_UnBind_VPSS(ViPipe,ViChn,VpssGrp);
EXIT_VPSS_STOP:
    SAMPLE_COMM_VPSS_Stop(VpssGrp,abChnEnable);
EXIT_VI_STOP:
    SAMPLE_COMM_VI_StopVi(&stViConfig);
    SAMPLE_COMM_SYS_Exit();

    return s32Ret;
}

/******************************************************************************
* function: SVC-T,H.264, Channel resolution adaptable with sensor
******************************************************************************/
HI_S32 SAMPLE_VENC_SVC_H264(void)
{
    HI_S32 i;
    HI_S32 s32Ret;
    SIZE_S          stSize[2];
    PIC_SIZE_E      enSize[2]     = {PIC_3840x2160};

    HI_S32          s32ChnNum     = 1;
    VENC_CHN        VencChn[2]    = {0,1};
    HI_U32          u32Profile[2] = {3,0};
    PAYLOAD_TYPE_E  enPayLoad[2]  = {PT_H264};
    VENC_GOP_MODE_E enGopMode     = VENC_GOPMODE_NORMALP;
    VENC_GOP_ATTR_S stGopAttr;
    SAMPLE_RC_E     enRcMode;
    HI_BOOL         bRcnRefShareBuf = HI_TRUE;

    VI_DEV          ViDev        = 0;
    VI_PIPE         ViPipe       = 0;
    VI_CHN          ViChn        = 0;
    SAMPLE_VI_CONFIG_S stViConfig;

    VPSS_GRP        VpssGrp        = 0;
    VPSS_CHN        VpssChn[2]     = {0,1};
    HI_BOOL         abChnEnable[4] = {1,0,0,0};

    HI_U32 u32SupplementConfig = HI_FALSE;

    for(i=0; i<s32ChnNum; i++)
    {
        s32Ret = SAMPLE_COMM_SYS_GetPicSize(enSize[i], &stSize[i]);
        if (HI_SUCCESS != s32Ret)
        {
            SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed!\n");
            return s32Ret;
        }
    }

    SAMPLE_COMM_VI_GetSensorInfo(&stViConfig);
    if(SAMPLE_SNS_TYPE_BUTT == stViConfig.astViInfo[0].stSnsInfo.enSnsType)
    {
        SAMPLE_PRT("Not set SENSOR%d_TYPE !\n",0);
        return HI_FAILURE;
    }

    s32Ret = SAMPLE_VENC_CheckSensor(stViConfig.astViInfo[0].stSnsInfo.enSnsType,stSize[0]);
    if(s32Ret != HI_SUCCESS)
    {
        s32Ret = SAMPLE_VENC_ModifyResolution(stViConfig.astViInfo[0].stSnsInfo.enSnsType,&enSize[0],&stSize[0]);
        if(s32Ret != HI_SUCCESS)
        {
            return HI_FAILURE;
        }
    }

    stViConfig.s32WorkingViNum       = 1;
    stViConfig.astViInfo[0].stDevInfo.ViDev     = ViDev;
    stViConfig.astViInfo[0].stPipeInfo.aPipe[0] = ViPipe;
    stViConfig.astViInfo[0].stChnInfo.ViChn     = ViChn;
    stViConfig.astViInfo[0].stChnInfo.enDynamicRange = DYNAMIC_RANGE_SDR8;
    stViConfig.astViInfo[0].stChnInfo.enPixFormat    = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    s32Ret = SAMPLE_VENC_VI_Init(&stViConfig, HI_FALSE,u32SupplementConfig);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("Init VI err for %#x!\n", s32Ret);
        return HI_FAILURE;
    }

    s32Ret = SAMPLE_VENC_VPSS_Init(VpssGrp,abChnEnable,DYNAMIC_RANGE_SDR8,PIXEL_FORMAT_YVU_SEMIPLANAR_420,stSize,stViConfig.astViInfo[0].stSnsInfo.enSnsType);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Init VPSS err for %#x!\n", s32Ret);
        goto EXIT_VI_STOP;
    }

    s32Ret = SAMPLE_COMM_VI_Bind_VPSS(ViPipe, ViChn, VpssGrp);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("VI Bind VPSS err for %#x!\n", s32Ret);
        goto EXIT_VPSS_STOP;
    }

   /******************************************
     start stream venc
    ******************************************/

    enRcMode = SAMPLE_VENC_GetRcMode();

    s32Ret = SAMPLE_COMM_VENC_GetGopAttr(enGopMode,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }

   /***encode h.264 **/
    s32Ret = SAMPLE_COMM_VENC_Start(VencChn[0], enPayLoad[0],enSize[0], enRcMode,u32Profile[0],bRcnRefShareBuf,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Start failed for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }

    s32Ret = SAMPLE_COMM_VPSS_Bind_VENC(VpssGrp, VpssChn[0],VencChn[0]);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H264_STOP;
    }


    /******************************************
     stream save process
    ******************************************/
    s32Ret = SAMPLE_COMM_VENC_StartGetStream_Svc_t(s32ChnNum);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Start Venc failed!\n");
        goto EXIT_VENC_H264_UnBind;
    }

    if (getenv("SAMPLE_VENC_AUTO"))
    {
        printf("non-interactive mode: waiting for SIGTERM/SIGINT to exit\n");
        while (1) { pause(); }
    }
    printf("please press twice ENTER to exit this sample\n");
    getchar();
    getchar();

    /******************************************
     exit process
    ******************************************/
    SAMPLE_COMM_VENC_StopGetStream();

EXIT_VENC_H264_UnBind:
    SAMPLE_COMM_VPSS_UnBind_VENC(VpssGrp,VpssChn[0],VencChn[0]);
EXIT_VENC_H264_STOP:
    SAMPLE_COMM_VENC_Stop(VencChn[0]);
EXIT_VI_VPSS_UNBIND:
    SAMPLE_COMM_VI_UnBind_VPSS(ViPipe,ViChn,VpssGrp);
EXIT_VPSS_STOP:
    SAMPLE_COMM_VPSS_Stop(VpssGrp,abChnEnable);
EXIT_VI_STOP:
    SAMPLE_COMM_VI_StopVi(&stViConfig);
    SAMPLE_COMM_SYS_Exit();

    return s32Ret;
}


/******************************************************************************
* function: Qpmap, H.265e + H264e@720P,H.265 Channel resolution adaptable with sensor
******************************************************************************/
HI_S32 SAMPLE_VENC_Qpmap(void)
{
    HI_S32 i;
    HI_S32 s32Ret;
    SIZE_S          stSize[2];
    PIC_SIZE_E      enSize[2]     = {PIC_3840x2160, PIC_720P};

    HI_S32          s32ChnNum     = 2;
    VENC_CHN        VencChn[2]    = {0,1};
    HI_U32          u32Profile[2] = {0,0};
    PAYLOAD_TYPE_E  enPayLoad[2]  = {PT_H265, PT_H264};
    VENC_GOP_MODE_E enGopMode;
    VENC_GOP_ATTR_S stGopAttr;
    SAMPLE_RC_E     enRcMode = SAMPLE_RC_QPMAP;
    HI_BOOL         bRcnRefShareBuf = HI_TRUE;

    VI_DEV          ViDev        = 0;
    VI_PIPE         ViPipe       = 0;
    VI_CHN          ViChn        = 0;
    SAMPLE_VI_CONFIG_S stViConfig;

    VPSS_GRP        VpssGrp        = 0;
    VPSS_CHN        VpssChn[2]     = {0,1};
    HI_BOOL         abChnEnable[4] = {1,0,0,0};

    HI_U32 u32SupplementConfig = HI_FALSE;

    for(i=0; i<s32ChnNum; i++)
    {
        s32Ret = SAMPLE_COMM_SYS_GetPicSize(enSize[i], &stSize[i]);
        if (HI_SUCCESS != s32Ret)
        {
            SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed!\n");
            return s32Ret;
        }
    }

    SAMPLE_COMM_VI_GetSensorInfo(&stViConfig);

    if(SAMPLE_SNS_TYPE_BUTT == stViConfig.astViInfo[0].stSnsInfo.enSnsType)
    {
        SAMPLE_PRT("Not set SENSOR%d_TYPE !\n",0);
        return HI_FAILURE;
    }

    s32Ret = SAMPLE_VENC_CheckSensor(stViConfig.astViInfo[0].stSnsInfo.enSnsType,stSize[0]);
    if(s32Ret != HI_SUCCESS)
    {
        s32Ret = SAMPLE_VENC_ModifyResolution(stViConfig.astViInfo[0].stSnsInfo.enSnsType,&enSize[0],&stSize[0]);
        if(s32Ret != HI_SUCCESS)
        {
            return HI_FAILURE;
        }
    }

    stViConfig.s32WorkingViNum       = 1;
    stViConfig.astViInfo[0].stDevInfo.ViDev     = ViDev;
    stViConfig.astViInfo[0].stPipeInfo.aPipe[0] = ViPipe;
    stViConfig.astViInfo[0].stChnInfo.ViChn     = ViChn;
    stViConfig.astViInfo[0].stChnInfo.enDynamicRange = DYNAMIC_RANGE_SDR8;
    stViConfig.astViInfo[0].stChnInfo.enPixFormat    = PIXEL_FORMAT_YVU_SEMIPLANAR_420;
    s32Ret = SAMPLE_VENC_VI_Init(&stViConfig, HI_FALSE,u32SupplementConfig);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("Init VI err for %#x!\n", s32Ret);
        return HI_FAILURE;
    }

    s32Ret = SAMPLE_VENC_VPSS_Init(VpssGrp,abChnEnable,DYNAMIC_RANGE_SDR8,PIXEL_FORMAT_YVU_SEMIPLANAR_420,stSize,stViConfig.astViInfo[0].stSnsInfo.enSnsType);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Init VPSS err for %#x!\n", s32Ret);
        goto EXIT_VI_STOP;
    }

    s32Ret = SAMPLE_COMM_VI_Bind_VPSS(ViPipe, ViChn, VpssGrp);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("VI Bind VPSS err for %#x!\n", s32Ret);
        goto EXIT_VPSS_STOP;
    }

   /******************************************
     start stream venc
    ******************************************/
    enGopMode = SAMPLE_VENC_GetGopMode();
    s32Ret = SAMPLE_COMM_VENC_GetGopAttr(enGopMode,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }

   /***encode h.265 **/
    s32Ret = SAMPLE_COMM_VENC_Start(VencChn[0], enPayLoad[0],enSize[0], enRcMode,u32Profile[0],bRcnRefShareBuf,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Start failed for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }

    s32Ret = SAMPLE_COMM_VENC_Start(VencChn[1], enPayLoad[1],enSize[1], enRcMode,u32Profile[0],bRcnRefShareBuf,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Start failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H265_STOP;
    }

    s32Ret = SAMPLE_COMM_VENC_QpmapSendFrame(VpssGrp,VpssChn[0],VencChn,s32ChnNum,stSize[0]);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("SAMPLE_COMM_VENC_QpmapSendFrame failed for %#x!\n", s32Ret);
        goto EXIT_VENC_H264_STOP;
    }


    /******************************************
     stream save process
    ******************************************/
    s32Ret = SAMPLE_COMM_VENC_StartGetStream(VencChn,s32ChnNum);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Start Venc failed!\n");
        goto EXIT_VENC_H264_STOP;
    }

    if (getenv("SAMPLE_VENC_AUTO"))
    {
        printf("non-interactive mode: waiting for SIGTERM/SIGINT to exit\n");
        while (1) { pause(); }
    }
    printf("please press twice ENTER to exit this sample\n");
    getchar();
    getchar();

    /******************************************
     exit process
    ******************************************/
    SAMPLE_COMM_VENC_StopSendQpmapFrame();
    SAMPLE_COMM_VENC_StopGetStream();

EXIT_VENC_H264_STOP:
    SAMPLE_COMM_VENC_Stop(VencChn[1]);
EXIT_VENC_H265_STOP:
    SAMPLE_COMM_VENC_Stop(VencChn[0]);
EXIT_VI_VPSS_UNBIND:
    SAMPLE_COMM_VI_UnBind_VPSS(ViPipe,ViChn,VpssGrp);
EXIT_VPSS_STOP:
    SAMPLE_COMM_VPSS_Stop(VpssGrp,abChnEnable);
EXIT_VI_STOP:
    SAMPLE_COMM_VI_StopVi(&stViConfig);
    SAMPLE_COMM_SYS_Exit();

    return s32Ret;
}

/******************************************************************************
* function: Mjpeg + Jpeg,Channel resolution adaptable with sensor
******************************************************************************/
HI_S32 SAMPLE_VENC_MJPEG_JPEG(void)
{
    HI_S32 i;
    char  ch;
    HI_S32 s32Ret;
    SIZE_S          stSize[2];
    PIC_SIZE_E      enSize[2]     = {PIC_3840x2160,PIC_3840x2160};
    HI_S32          s32ChnNum     = 2;
    VENC_CHN        VencChn[2]    = {0,1};
    HI_U32          u32Profile[2] = {0,0};
    PAYLOAD_TYPE_E  enPayLoad[2]  = {PT_MJPEG, PT_JPEG};
    VENC_GOP_MODE_E enGopMode     = VENC_GOPMODE_NORMALP;
    VENC_GOP_ATTR_S stGopAttr;
    SAMPLE_RC_E     enRcMode;
    HI_BOOL         bSupportDcf   = HI_TRUE;

    VI_DEV          ViDev         = 0;
    VI_PIPE         ViPipe        = 0;
    VI_CHN          ViChn         = 0;
    SAMPLE_VI_CONFIG_S stViConfig;

    VPSS_GRP        VpssGrp        = 0;
    VPSS_CHN        VpssChn[2]     = {0,1};
    HI_BOOL         abChnEnable[4] = {1,1,0,0};

    HI_U32 u32SupplementConfig = HI_TRUE;

    for(i=0; i<s32ChnNum; i++)
    {
        s32Ret = SAMPLE_COMM_SYS_GetPicSize(enSize[i], &stSize[i]);
        if (HI_SUCCESS != s32Ret)
        {
            SAMPLE_PRT("SAMPLE_COMM_SYS_GetPicSize failed!\n");
            return s32Ret;
        }
    }

    SAMPLE_COMM_VI_GetSensorInfo(&stViConfig);

    if(SAMPLE_SNS_TYPE_BUTT == stViConfig.astViInfo[0].stSnsInfo.enSnsType)
    {
        SAMPLE_PRT("Not set SENSOR%d_TYPE !\n",0);
        return HI_FAILURE;
    }

    s32Ret = SAMPLE_VENC_CheckSensor(stViConfig.astViInfo[0].stSnsInfo.enSnsType,stSize[0]);
    if(s32Ret != HI_SUCCESS)
    {
        s32Ret = SAMPLE_VENC_ModifyResolution(stViConfig.astViInfo[0].stSnsInfo.enSnsType,&enSize[0],&stSize[0]);
        if(s32Ret != HI_SUCCESS)
        {
            return HI_FAILURE;
        }
        s32Ret = SAMPLE_VENC_ModifyResolution(stViConfig.astViInfo[0].stSnsInfo.enSnsType,&enSize[1],&stSize[1]);
        if(s32Ret != HI_SUCCESS)
        {
            return HI_FAILURE;
        }
    }


    stViConfig.s32WorkingViNum       = 1;
    stViConfig.astViInfo[0].stDevInfo.ViDev     = ViDev;
    stViConfig.astViInfo[0].stPipeInfo.aPipe[0] = ViPipe;
    stViConfig.astViInfo[0].stChnInfo.ViChn     = ViChn;
    stViConfig.astViInfo[0].stChnInfo.enDynamicRange = DYNAMIC_RANGE_SDR8;
    stViConfig.astViInfo[0].stChnInfo.enPixFormat    = PIXEL_FORMAT_YVU_SEMIPLANAR_422;
    s32Ret = SAMPLE_VENC_VI_Init(&stViConfig, HI_FALSE,u32SupplementConfig);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("Init VI err for %#x!\n", s32Ret);
        return HI_FAILURE;
    }
    s32Ret = SAMPLE_VENC_VPSS_Init(VpssGrp,abChnEnable,DYNAMIC_RANGE_SDR8,PIXEL_FORMAT_YVU_SEMIPLANAR_422,stSize,stViConfig.astViInfo[0].stSnsInfo.enSnsType);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Init VPSS err for %#x!\n", s32Ret);
        goto EXIT_VI_STOP;
    }

    s32Ret = SAMPLE_COMM_VI_Bind_VPSS(ViPipe, ViChn, VpssGrp);
    if(s32Ret != HI_SUCCESS)
    {
        SAMPLE_PRT("VI Bind VPSS err for %#x!\n", s32Ret);
        goto EXIT_VPSS_STOP;
    }
    SAMPLE_VENC_SetDCFInfo(ViPipe);

   /******************************************
     start stream venc
    ******************************************/

    enRcMode = SAMPLE_VENC_GetRcMode();

    s32Ret = SAMPLE_COMM_VENC_GetGopAttr(enGopMode,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }


   /***encode Mjpege **/
    s32Ret = SAMPLE_COMM_VENC_Start(VencChn[0], enPayLoad[0],enSize[0], enRcMode,u32Profile[0],HI_FALSE,&stGopAttr);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Start failed for %#x!\n", s32Ret);
        goto EXIT_VI_VPSS_UNBIND;
    }

    s32Ret = SAMPLE_COMM_VPSS_Bind_VENC(VpssGrp, VpssChn[0],VencChn[0]);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Get GopAttr failed for %#x!\n", s32Ret);
        goto EXIT_VENC_MJPEGE_STOP;
    }

    /***encode Jpege **/
    s32Ret = SAMPLE_COMM_VENC_SnapStart(VencChn[1], &stSize[1], bSupportDcf);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc Start failed for %#x!\n", s32Ret);
        goto EXIT_VENC_MJPEGE_UnBind;
    }

    s32Ret = SAMPLE_COMM_VPSS_Bind_VENC(VpssGrp, VpssChn[1],VencChn[1]);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Venc bind Vpss failed for %#x!\n", s32Ret);
        goto EXIT_VENC_JPEGE_STOP;
    }

    /******************************************
     stream save process
    ******************************************/
    s32Ret = SAMPLE_COMM_VENC_StartGetStream(VencChn,1);
    if (HI_SUCCESS != s32Ret)
    {
        SAMPLE_PRT("Start Venc failed!\n");
        goto EXIT_VENC_JPEGE_UnBind;
    }

    /******************************************
     stream venc process -- get jpeg stream, then save it to file.
    ******************************************/
    printf("press 'q' to exit snap!\nperess ENTER to capture one picture to file\n");
    i = 0;
    while ((ch = (char)getchar()) != 'q')
    {
        printf("press any key to snap one pic\n");
        getchar();

        s32Ret = SAMPLE_COMM_VENC_SnapProcess(VencChn[1], 1, HI_TRUE, HI_TRUE);
        if (HI_SUCCESS != s32Ret)
        {
            printf("%s: sanp process failed!\n", __FUNCTION__);
            break;
        }
        printf("snap %d success!\n", i);
        i++;
    }

    if (getenv("SAMPLE_VENC_AUTO"))
    {
        printf("non-interactive mode: waiting for SIGTERM/SIGINT to exit\n");
        while (1) { pause(); }
    }
    printf("please press twice ENTER to exit this sample\n");
    getchar();
    getchar();
    /******************************************
     exit process
    ******************************************/
    SAMPLE_COMM_VENC_StopGetStream();

EXIT_VENC_JPEGE_UnBind:
    SAMPLE_COMM_VPSS_UnBind_VENC(VpssGrp,VpssChn[1],VencChn[1]);
EXIT_VENC_JPEGE_STOP:
    SAMPLE_COMM_VENC_Stop(VencChn[1]);
EXIT_VENC_MJPEGE_UnBind:
    SAMPLE_COMM_VPSS_UnBind_VENC(VpssGrp,VpssChn[0],VencChn[0]);
EXIT_VENC_MJPEGE_STOP:
    SAMPLE_COMM_VENC_Stop(VencChn[0]);
EXIT_VI_VPSS_UNBIND:
    SAMPLE_COMM_VI_UnBind_VPSS(ViPipe,ViChn,VpssGrp);
EXIT_VPSS_STOP:
    SAMPLE_COMM_VPSS_Stop(VpssGrp,abChnEnable);
EXIT_VI_STOP:
    SAMPLE_COMM_VI_StopVi(&stViConfig);
    SAMPLE_COMM_SYS_Exit();

    return s32Ret;
}


/******************************************************************************
* function    : main()
* Description : video venc sample
******************************************************************************/
#ifdef __HuaweiLite__
    int app_main(int argc, char *argv[])
#else
    int main(int argc, char *argv[])
#endif
{
    HI_S32 s32Ret;
    HI_U32 u32Index;

    /* S2-1 调试：关掉缓冲，崩溃前的最后一行也能落盘 */
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    if (argc < 2 || argc > 2)
    {
        SAMPLE_VENC_Usage(argv[0]);
        return HI_FAILURE;
    }

    if (!strncmp(argv[1], "-h", 2))
    {
        SAMPLE_VENC_Usage(argv[0]);
        return HI_SUCCESS;
    }

    u32Index = atoi(argv[1]);

#ifndef __HuaweiLite__
    signal(SIGINT, SAMPLE_VENC_HandleSig);
    signal(SIGTERM, SAMPLE_VENC_HandleSig);
#endif

    rtsp_server_start();

    switch (u32Index)
    {
        case 0:
            s32Ret = SAMPLE_VENC_H265_H264();
            break;
        case 1:
            s32Ret = SAMPLE_VENC_LOW_DELAY();
            break;
        case 2:
            s32Ret = SAMPLE_VENC_Qpmap();
            break;
        case 3:
            s32Ret = SAMPLE_VENC_IntraRefresh();
            break;
        case 4:
            s32Ret = SAMPLE_VENC_ROIBG();
            break;
        case 5:
            s32Ret = SAMPLE_VENC_DeBreathEffect();
            break;
        case 6:
            s32Ret = SAMPLE_VENC_SVC_H264();
            break;
        case 7:
            s32Ret = SAMPLE_VENC_MJPEG_JPEG();
            break;
        default:
            printf("the index is invaild!\n");
            SAMPLE_VENC_Usage(argv[0]);
            return HI_FAILURE;
    }

    if (HI_SUCCESS == s32Ret)
    { printf("program exit normally!\n"); }
    else
    { printf("program exit abnormally!\n"); }

#ifdef __HuaweiLite__
    return s32Ret;
#else
    exit(s32Ret);
#endif
}

#ifdef __cplusplus
#if __cplusplus
}
#endif
#endif /* End of #ifdef __cplusplus */
