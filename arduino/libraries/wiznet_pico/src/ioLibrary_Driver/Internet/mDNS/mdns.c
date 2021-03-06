//*****************************************************************************
//
//! \file mdns.c
//! \brief DNS APIs Implement file.
//! \details Send DNS query & Receive DNS reponse.  \n
//!          It depends on stdlib.h & string.h in ansi-c library
//! \version 1.1.0
//! \date 2013/11/18
//! \par  Revision history
//!       <2013/10/21> 1st Release
//!       <2013/12/20> V1.1.0
//!         1. Remove secondary DNS server in DNS_run
//!            If 1st DNS_run failed, call DNS_run with 2nd DNS again
//!         2. DNS_timerHandler -> DNS_time_handler
//!         3. Remove the unused define
//!         4. Integrated dns.h dns.c & dns_parse.h dns_parse.c into dns.h & dns.c
//!       <2013/12/20> V1.1.0
//!       <2022/01/16> CPV - change into mDNS.
//!
//! \author Eric Jung & MidnightCow
//! \copyright
//!
//! Copyright (c)  2013, WIZnet Co., LTD.
//! All rights reserved.
//!
//! Redistribution and use in source and binary forms, with or without
//! modification, are permitted provided that the following conditions
//! are met:
//!
//!     * Redistributions of source code must retain the above copyright
//! notice, this list of conditions and the following disclaimer.
//!     * Redistributions in binary form must reproduce the above copyright
//! notice, this list of conditions and the following disclaimer in the
//! documentation and/or other materials provided with the distribution.
//!     * Neither the name of the <ORGANIZATION> nor the names of its
//! contributors may be used to endorse or promote products derived
//! from this software without specific prior written permission.
//!
//! THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
//! AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
//! IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
//! ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
//! LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
//! CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
//! SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
//! INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
//! CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
//! ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
//! THE POSSIBILITY OF SUCH DAMAGE.
//
//*****************************************************************************

#include <string.h>
#include <stdlib.h>
#include <Arduino.h>

#include "..\..\Ethernet\socket.h"
#include "mdns.h"

#ifdef _MDNS_DEBUG_
  #include <stdio.h>
#endif

#define	INITRTT		2000L	/* Initial smoothed response time */
#define	MAXCNAME	   (MAX_DOMAIN_NAME + (MAX_DOMAIN_NAME>>1))	   /* Maximum amount of cname recursion */

#define	TYPE_A		1	   /* Host address */
#define	TYPE_NS		2	   /* Name server */
#define	TYPE_MD		3	   /* Mail destination (obsolete) */
#define	TYPE_MF		4	   /* Mail forwarder (obsolete) */
#define	TYPE_CNAME	5	   /* Canonical name */
#define	TYPE_SOA	   6	   /* Start of Authority */
#define	TYPE_MB		7	   /* Mailbox name (experimental) */
#define	TYPE_MG		8	   /* Mail group member (experimental) */
#define	TYPE_MR		9	   /* Mail rename name (experimental) */
#define	TYPE_NULL	10	   /* Null (experimental) */
#define	TYPE_WKS	   11	   /* Well-known sockets */
#define	TYPE_PTR	   12	   /* Pointer record */
#define	TYPE_HINFO	13	   /* Host information */
#define	TYPE_MINFO	14	   /* Mailbox information (experimental)*/
#define	TYPE_MX		15	   /* Mail exchanger */
#define	TYPE_TXT	   16	   /* Text strings */
#define	TYPE_ANY	   255	/* Matches any type */

#define	CLASS_IN	   1	   /* The ARPA Internet */

/* Round trip timing parameters */
#define	AGAIN	      8     /* Average RTT gain = 1/8 */
#define	LAGAIN      3     /* Log2(AGAIN) */
#define	DGAIN       4     /* Mean deviation gain = 1/4 */
#define	LDGAIN      2     /* log2(DGAIN) */

/* Header for all domain messages */
struct dhdr
{
	uint16_t id;   /* Identification */
	uint8_t	qr;      /* Query/Response */
#define	QUERY    0
#define	RESPONSE 1
	uint8_t	opcode;
#define	IQUERY   1
	uint8_t	aa;      /* Authoratative answer */
	uint8_t	tc;      /* Truncation */
	uint8_t	rd;      /* Recursion desired */
	uint8_t	ra;      /* Recursion available */
	uint8_t	rcode;   /* Response code */
#define	NO_ERROR       0
#define	FORMAT_ERROR   1
#define	SERVER_FAIL    2
#define	NAME_ERROR     3
#define	NOT_IMPL       4
#define	REFUSED        5
	uint16_t qdcount;	/* Question count */
	uint16_t ancount;	/* Answer count */
	uint16_t nscount;	/* Authority (name server) count */
	uint16_t arcount;	/* Additional record count */
};


uint8_t* pMDNSMSG;       // mDNS message buffer
uint8_t  MDNS_SOCKET;    // SOCKET number for mDNS
uint16_t MDNS_MSGID;     // mDNS message ID

uint32_t mdns_1s_tick;   // for timout of mDNS processing
static uint8_t retry_count;

/* converts uint16_t from network buffer to a host byte order integer. */
uint16_t get16(uint8_t * s)
{
	uint16_t i;
	i = *s++ << 8;
	i = i + *s;
	return i;
}

/* copies uint16_t to the network buffer with network byte order. */
uint8_t * put16(uint8_t * s, uint16_t i)
{
	*s++ = i >> 8;
	*s++ = i;
	return s;
}


/*
 *              CONVERT A DOMAIN NAME TO THE HUMAN-READABLE FORM
 *
 * Description : This function converts a compressed domain name to the human-readable form
 * Arguments   : msg        - is a pointer to the reply message
 *               compressed - is a pointer to the domain name in reply message.
 *               buf        - is a pointer to the buffer for the human-readable form name.
 *               len        - is the MAX. size of buffer.
 * Returns     : the length of compressed message
 */
int parse_name(uint8_t * msg, uint8_t * compressed, char * buf, int16_t len)
{
	uint16_t slen;		/* Length of current segment */
	uint8_t * cp;
	int clen = 0;		/* Total length of compressed name */
	int indirect = 0;	/* Set if indirection encountered */
	int nseg = 0;		/* Total number of segments in name */

	cp = compressed;

	for (;;)
	{
		slen = *cp++;	/* Length of this segment */

		if (!indirect) clen++;

		if ((slen & 0xc0) == 0xc0)
		{
			if (!indirect)
				clen++;
			indirect = 1;
			/* Follow indirection */
			cp = &msg[((slen & 0x3f)<<8) + *cp];
			slen = *cp++;
		}

		if (slen == 0)	/* zero length == all done */
			break;

		len -= slen + 1;

		if (len < 0) return -1;

		if (!indirect) clen += slen;

		while (slen-- != 0) *buf++ = (char)*cp++;
		*buf++ = '.';
		nseg++;
	}

	if (nseg == 0)
	{
		/* Root name; represent as single dot */
		*buf++ = '.';
		len--;
	}

	*buf++ = '\0';
	len--;

	return clen;	/* Length of compressed message */
}

/*
 *              PARSE QUESTION SECTION
 *
 * Description : This function parses the qeustion record of the reply message.
 * Arguments   : msg - is a pointer to the reply message
 *               cp  - is a pointer to the qeustion record.
 * Returns     : a pointer the to next record.
 */
uint8_t * mdns_question(uint8_t * msg, uint8_t * cp)
{
	int len;
	char name[MAXCNAME];

	len = parse_name(msg, cp, name, MAXCNAME);


	if (len == -1) return 0;

	cp += len;
	cp += 2;		/* type */
	cp += 2;		/* class */

	return cp;
}


/*
 *              PARSE ANSER SECTION
 *
 * Description : This function parses the answer record of the reply message.
 * Arguments   : msg - is a pointer to the reply message
 *               cp  - is a pointer to the answer record.
 * Returns     : a pointer the to next record.
 */
uint8_t * mdns_answer(uint8_t * msg, uint8_t * cp, uint8_t * ip_from_mdns)
{
	int len, type;
	char name[MAXCNAME];

	len = parse_name(msg, cp, name, MAXCNAME);

	if (len == -1) return 0;

	cp += len;
	type = get16(cp);
	cp += 2;		/* type */
	cp += 2;		/* class */
	cp += 4;		/* ttl */
	cp += 2;		/* len */


	switch (type)
	{
	case TYPE_A:
		/* Just read the address directly into the structure */
		ip_from_mdns[0] = *cp++;
		ip_from_mdns[1] = *cp++;
		ip_from_mdns[2] = *cp++;
		ip_from_mdns[3] = *cp++;
		break;
	case TYPE_CNAME:
	case TYPE_MB:
	case TYPE_MG:
	case TYPE_MR:
	case TYPE_NS:
	case TYPE_PTR:
		/* These types all consist of a single domain name */
		/* convert it to ascii format */
		len = parse_name(msg, cp, name, MAXCNAME);
		if (len == -1) return 0;

		cp += len;
		break;
	case TYPE_HINFO:
		len = *cp++;
		cp += len;

		len = *cp++;
		cp += len;
		break;
	case TYPE_MX:
		cp += 2;
		/* Get domain name of exchanger */
		len = parse_name(msg, cp, name, MAXCNAME);
		if (len == -1) return 0;

		cp += len;
		break;
	case TYPE_SOA:
		/* Get domain name of name server */
		len = parse_name(msg, cp, name, MAXCNAME);
		if (len == -1) return 0;

		cp += len;

		/* Get domain name of responsible person */
		len = parse_name(msg, cp, name, MAXCNAME);
		if (len == -1) return 0;

		cp += len;

		cp += 4;
		cp += 4;
		cp += 4;
		cp += 4;
		cp += 4;
		break;
	case TYPE_TXT:
		/* Just stash */
		break;
	default:
		/* Ignore */
		break;
	}

	return cp;
}

/*
 *              PARSE THE mDNS REPLY
 *
 * Description : This function parses the reply message from mDNS server.
 * Arguments   : dhdr - is a pointer to the header for mDNS message
 *               buf  - is a pointer to the reply message.
 *               len  - is the size of reply message.
 * Returns     : -1 - Domain name lenght is too big
 *                0 - Fail (Timout or parse error)
 *                1 - Success,
 */
int8_t parseMDNSMSG(struct dhdr * pdhdr, uint8_t * pbuf, uint8_t * ip_from_mdns)
{
	uint16_t tmp;
	uint16_t i;
	uint8_t * msg;
	uint8_t * cp;

	msg = pbuf;
	memset(pdhdr, 0, sizeof(*pdhdr));

	pdhdr->id = get16(&msg[0]);
	tmp = get16(&msg[2]);
	if (tmp & 0x8000) pdhdr->qr = 1;

	pdhdr->opcode = (tmp >> 11) & 0xf;

	if (tmp & 0x0400) pdhdr->aa = 1;
	if (tmp & 0x0200) pdhdr->tc = 1;
	if (tmp & 0x0100) pdhdr->rd = 1;
	if (tmp & 0x0080) pdhdr->ra = 1;

	pdhdr->rcode = tmp & 0xf;
	pdhdr->qdcount = get16(&msg[4]);
	pdhdr->ancount = get16(&msg[6]);
	pdhdr->nscount = get16(&msg[8]);
	pdhdr->arcount = get16(&msg[10]);


	/* Now parse the variable length sections */
	cp = &msg[12];

	/* Question section */
	for (i = 0; i < pdhdr->qdcount; i++)
	{
		cp = mdns_question(msg, cp);
   #ifdef _MDNS_DEUBG_
      DBG_PRINTF("MAX_DOMAIN_NAME is too small, it should be redefined in mdns.h");
   #endif
		if(!cp) return -1;
	}

	/* Answer section */
	for (i = 0; i < pdhdr->ancount; i++)
	{
		cp = mdns_answer(msg, cp, ip_from_mdns);
   #ifdef _MDNS_DEUBG_
      DBG_PRINTF("MAX_DOMAIN_NAME is too small, it should be redefined in mdns.h");
   #endif
		if(!cp) return -1;
	}

	/* Name server (authority) section */
	for (i = 0; i < pdhdr->nscount; i++)
	{
		;
	}

	/* Additional section */
	for (i = 0; i < pdhdr->arcount; i++)
	{
		;
	}

	if(pdhdr->rcode == 0) return 1;		// No error
	else return 0;
}


/*
 *              MAKE mDNS QUERY MESSAGE
 *
 * Description : This function makes mDNS query message.
 * Arguments   : name - is a pointer to the domain name.
 *               buf  - is a pointer to the buffer for DNS message.
 *               len  - is the MAX. size of buffer.
 * Returns     : the pointer to the mDNS message.
 */
int16_t mdns_makequery(char * name, uint8_t * buf, uint16_t len)
{
	uint8_t *cp;
	char *cp1;
	char sname[MAXCNAME];
	char *dname;
	uint16_t p;
	uint16_t dlen;

	cp = buf;
	cp = put16(cp,0); // RFC 6762: In multicast query messages, the Query Identifier SHOULD be set to zero on transmission. (16 bit)
	cp = put16(cp,0); // RFC 6762: Flags QR=0, Opcode=0000, AA=0, TC=0, RD=0, RA=0, Z=000, Rcode=0000 (16 bit)
	cp = put16(cp,1); // Number of questions (16 bit)
	cp = put16(cp,0); // Number of answers (16 bit)
	cp = put16(cp,0); // Number of authority resource records (RRs) (16 bit)
	cp = put16(cp,0); // Number of additional RRs (16 bit)

	strcpy(sname, name);
	dname = sname;
	dlen = strlen(dname);
	for (;;)
	{
		/* Look for next dot */
		cp1 = strchr(dname, '.');

		if (cp1 != NULL) len = cp1 - dname;	/* More to come */
		else len = dlen;			/* Last component */

		*cp++ = len;				/* Write length of component */
		if (len == 0) break;

		/* Copy component up to (but not including) dot */
		strncpy((char *)cp, dname, len);
		cp += len;
		if (cp1 == NULL)
		{
			*cp++ = 0;			/* Last one; write null and finish */
			break;
		}
		dname += len+1;
		dlen -= len+1;
	}

	cp = put16(cp, 0x0001); // type 1=A (address record).
	cp = put16(cp, 0x8001); // class 1=IN; bit 15=1: request unicast response.

	return ((int16_t)((uint32_t)(cp) - (uint32_t)(buf)));
}

/*
 *              CHECK MDNS TIMEOUT
 *
 * Description : This function check the MDNS timeout
 * Arguments   : None.
 * Returns     : -1 - timeout occurred, 0 - timer over, but no timeout, 1 - no timer over, no timeout occur
 * Note        : timeout : retry count and timer both over.
 */

int8_t check_MDNS_timeout(void)
{

	if(mdns_1s_tick >= MDNS_WAIT_TIME)
	{
		mdns_1s_tick = 0;
		if(retry_count >= MAX_MDNS_RETRY) {
			retry_count = 0;
			return -1; // timeout occurred
		}
		retry_count++;
		return 0; // timer over, but no timeout
	}

	return 1; // no timer over, no timeout occur
}



/* MDNS CLIENT INIT */
void MDNS_init(uint8_t s, uint8_t * buf)
{
	MDNS_SOCKET = s; // SOCK_DNS
	pMDNSMSG = buf; // User's shared buffer
	MDNS_MSGID = MDNS_MSG_ID;
}

/* MDNS CLIENT RUN */
int32_t MDNS_run(uint8_t * name, uint8_t * ip_from_mdns)
{
	int32_t ret;
	struct dhdr dhp;
	uint8_t ip[4];
	uint16_t len, port;
	int8_t ret_check_timeout;

	// mDNS: multicast UDP packet is  sent to 01:00:5e:00:00:fb
	uint8_t mdns_mac[6];
	mdns_mac[0] = 0x01;
	mdns_mac[1] = 0x00;
	mdns_mac[2] = 0x5e;
	mdns_mac[3] = 0x00;
	mdns_mac[4] = 0x00;
	mdns_mac[5] = 0xfb; // 251
	// mDNS: multicast UDP packet is always sent to 224.0.0.251:5353
	uint8_t mdns_ip[4];
	mdns_ip[0] = 224; // 0xe0
	mdns_ip[1] = 0;
	mdns_ip[2] = 0;
	mdns_ip[3] = 251; // 0xfb
	uint16_t mdns_port = MDNS_PORT;

	retry_count = 0;
	mdns_1s_tick = 0;

    // Socket open
	setSn_DHAR(MDNS_SOCKET,mdns_mac); // Set destination hardware address.
    setSn_DIPR(MDNS_SOCKET,mdns_ip); // Set destination IP address.
    setSn_DPORT(MDNS_SOCKET,mdns_port); // Set destination port.
    ret = socket(MDNS_SOCKET,Sn_MR_UDP,0,SF_MULTI_ENABLE);
    if (ret!=MDNS_SOCKET)
    {
#ifdef _MDNS_DEBUG_
      DBG_PRINTF("[MDNS_run] Socket error %02x\r\n",ret);
#endif
      return ret;
    }

#ifdef _MDNS_DEBUG_
	DBG_PRINTF("> mDNS query to %d.%d.%d.%d for '%s'\r\n",(int)mdns_ip[0],(int)mdns_ip[1],(int)mdns_ip[2],(int)mdns_ip[3],name);
#endif

	len = mdns_makequery((char *)name, pMDNSMSG, MAX_MDNS_BUF_SIZE);
	ret = sendto(MDNS_SOCKET, pMDNSMSG, len, mdns_ip, MDNS_PORT);
    if (ret!=len)
    {
#ifdef _MDNS_DEBUG_
      DBG_PRINTF("[MDNS_run] sendto error: ret %d != len %d\r\n",ret,len);
#endif
      return ret;
    }

	while (1)
	{
		if ((len = getSn_RX_RSR(MDNS_SOCKET)) > 0)
		{
			if (len > MAX_MDNS_BUF_SIZE) len = MAX_MDNS_BUF_SIZE;
			len = recvfrom(MDNS_SOCKET, pMDNSMSG, len, ip, &port);
#ifdef _MDNS_DEBUG_
	        DBG_PRINTF("> Received mDNS message from %d.%d.%d.%d(%d), len = %d\r\n", ip[0], ip[1], ip[2], ip[3],port,len);
#endif
			ret = parseMDNSMSG(&dhp, pMDNSMSG, ip_from_mdns);
			break;
		}
		// Check Timeout
		ret_check_timeout = check_MDNS_timeout();
		if (ret_check_timeout < 0) 
		{
#ifdef _MDNS_DEBUG_
			DBG_PRINTF("> mDNS Server is not responding : %d.%d.%d.%d\r\n", mdns_ip[0], mdns_ip[1], mdns_ip[2], mdns_ip[3]);
#endif
			close(MDNS_SOCKET);
			return 0; // timeout occurred
		}
		else if (ret_check_timeout == 0) 
		{
#ifdef _MDNS_DEBUG_
			DBG_PRINTF("> mDNS Timeout\r\n");
#endif
			sendto(MDNS_SOCKET, pMDNSMSG, len, mdns_ip, MDNS_PORT);
		}
	}
	close(MDNS_SOCKET);
	// Return value
	// 0 > :  failed / 1 - success
	return ret;
}


/* MDNS TIMER HANDLER */
void MDNS_time_handler(void)
{
	mdns_1s_tick++;
}
