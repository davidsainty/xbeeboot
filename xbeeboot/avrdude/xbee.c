/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2015-2020 David Sainty
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* $Id: xbee.c 14123 2020-05-03 03:53:31Z dave $ */

/*
 * avrdude interface for AVR devices Over-The-Air programmable via an
 * XBee Series 2 device.
 *
 * The XBee programmer is STK500v1 (optiboot) encapsulated in the XBee
 * API protocol.  The bootloader supporting this protocol is available at:
 *
 * https://github.com/davidsainty/xbeeboot
 */

#include "ac_cfg.h"

#include <sys/time.h> /* gettimeofday() */

#include <stdio.h> /* sscanf() */
#include <stdlib.h> /* malloc() */
#include <string.h> /* memmove() etc. */
#include <unistd.h> /* usleep() */

#include "avrdude.h"
#include "libavrdude.h"
#include "stk500_private.h"
#include "stk500.h"
#include "xbee.h"

/*
 * For non-direct mode (Over-The-Air) we need to issue XBee commands
 * to the remote XBee in order to reset the AVR CPU and initiate the
 * XBeeBoot bootloader.
 *
 * XBee IO port 3 is a somewhat-arbitrarily chosen pin that can be
 * connected directly to the AVR reset pin.
 *
 * Note that port 7 was not used because it is the only pin that can
 * be used as a CTS flow control output.  Port 6 is the only pin that
 * can be used as an RTS flow control input.
 *
 * Some off-the-shelf Arduino shields select a different pin.  For
 * example this one uses XBee IO port 7.
 *
 * https://wiki.dfrobot.com/Xbee_Shield_For_Arduino__no_Xbee___SKU_DFR0015_
 */
#define XBEE_DEFAULT_RESET_PIN 3

/*
 * After eight seconds the AVR bootloader watchdog will kick in.  But
 * to allow for the possibility of eight seconds upstream and another
 * eight seconds downstream, allow for 16 retries (of roughly one
 * second each).
 */
#define XBEE_MAX_RETRIES 16

/*
 * Maximum chunk size, which is the maximum encapsulated payload to be
 * delivered to the remote CPU.
 *
 * There is an additional overhead of 3 bytes encapsulation, one
 * "REQUEST" byte, one sequence number byte, and one
 * "FIRMWARE_DELIVER" request type.
 *
 * The ZigBee maximum (unfragmented) payload is 84 bytes.  Source
 * routing decreases that by two bytes overhead, plus two bytes per
 * hop.  Maximum hop support is for 11 or 25 hops depending on
 * firmware.
 *
 * Network layer encryption decreases the maximum payload by 18 bytes.
 * APS end-to-end encryption decreases the maximum payload by 9 bytes.
 * Both these layers are available in concert, as seen in the section
 * "Network and APS layer encryption", decreasing our maximum payload
 * by both 18 bytes and 9 bytes.
 *
 * Our maximum payload size should therefore ideally be 84 - 18 - 9 =
 * 57 bytes, and therefore a chunk size of 54 bytes for zero hops.
 *
 * Source: XBee X2C manual: "Maximum RF payload size" section for most
 * details; "Network layer encryption and decryption" section for the
 * reference to 18 bytes of overhead; and "Enable APS encryption" for
 * the reference to 9 bytes of overhead.
 */
#define XBEEBOOT_MAX_CHUNK 54

/*
 * Maximum source route intermediate hops.  This is described in the
 * documentation variously as 40 hops (routing table); OR 25 hops
 * (firmware 4x58 or later); OR 11 hops (firmware earlier than 4x58).
 *
 * What isn't described is how to know if a given source route length
 * is actually supported by the mesh for our target device.
 */
#define XBEE_MAX_INTERMEDIATE_HOPS 40

/* Protocol */
#define XBEEBOOT_PACKET_TYPE_ACK 0
#define XBEEBOOT_PACKET_TYPE_REQUEST 1

/*
 * Read signature bytes - Direct copy of the Arduino behaviour to
 * satisfy Optiboot.
 */
static int xbee_read_sig_bytes(PROGRAMMER *pgm, AVRPART *p, AVRMEM *m)
{
  unsigned char buf[32];

  /* Signature byte reads are always 3 bytes. */

  if (m->size < 3) {
    avrdude_message(MSG_INFO, "%s: memsize too small for sig byte read",
                    progname);
    return -1;
  }

  buf[0] = Cmnd_STK_READ_SIGN;
  buf[1] = Sync_CRC_EOP;

  serial_send(&pgm->fd, buf, 2);

  if (serial_recv(&pgm->fd, buf, 5) < 0)
    return -1;
  if (buf[0] == Resp_STK_NOSYNC) {
    avrdude_message(MSG_INFO, "%s: stk500_cmd(): programmer is out of sync\n",
                    progname);
    return -1;
  } else if (buf[0] != Resp_STK_INSYNC) {
    avrdude_message(MSG_INFO,
                    "\n%s: xbee_read_sig_bytes(): (a) protocol error, "
                    "expect=0x%02x, resp=0x%02x\n",
                    progname, Resp_STK_INSYNC, buf[0]);
    return -2;
  }
  if (buf[4] != Resp_STK_OK) {
    avrdude_message(MSG_INFO,
                    "\n%s: xbee_read_sig_bytes(): (a) protocol error, "
                    "expect=0x%02x, resp=0x%02x\n",
                    progname, Resp_STK_OK, buf[4]);
    return -3;
  }

  m->buf[0] = buf[1];
  m->buf[1] = buf[2];
  m->buf[2] = buf[3];

  return 3;
}

struct XBeeSequenceStatistics {
  struct timeval sendTime;
};

struct XBeeStaticticsSummary {
  struct timeval minimum;
  struct timeval maximum;
  struct timeval sum;
  unsigned long samples;
};

#define XBEE_STATS_GROUPS 4
#define XBEE_STATS_FRAME_LOCAL 0
#define XBEE_STATS_FRAME_REMOTE 1
#define XBEE_STATS_TRANSMIT 2
#define XBEE_STATS_RECEIVE 3

static const char* groupNames[] =
  {
   "FRAME_LOCAL",
   "FRAME_REMOTE",
   "TRANSMIT",
   "RECEIVE"
  };

struct XBeeBootSession {
  struct serial_device *serialDevice;
  union filedescriptor serialDescriptor;

  unsigned char xbee_address[10];
  int directMode;
  unsigned char outSequence;
  unsigned char inSequence;

  /*
   * XBee API frame sequence number.
   */
  unsigned char txSequence;

  /*
   * Set to non-zero if the transport is broken to the point it is
   * considered unusable.
   */
  int transportUnusable;

  int xbeeResetPin;

  size_t inInIndex;
  size_t inOutIndex;
  unsigned char inBuffer[256];

  int sourceRouteHops; /* -1 if unset */
  int sourceRouteChanged;

  /*
   * The source route is an array of intermediate 16 bit addresses,
   * starting with the address nearest to the target address, and
   * finishing with the address closest to our local device.
   */
  unsigned char sourceRoute[2 * XBEE_MAX_INTERMEDIATE_HOPS];

  struct XBeeSequenceStatistics sequenceStatistics[256 * XBEE_STATS_GROUPS];
  struct XBeeStaticticsSummary groupSummary[XBEE_STATS_GROUPS];
};

static void xbeeStatsReset(struct XBeeStaticticsSummary *summary)
{
  summary->minimum.tv_sec = 0;
  summary->minimum.tv_usec = 0;
  summary->maximum.tv_sec = 0;
  summary->maximum.tv_usec = 0;
  summary->sum.tv_sec = 0;
  summary->sum.tv_usec = 0;
  summary->samples = 0;
}

static void xbeeStatsAdd(struct XBeeStaticticsSummary *summary,
                         struct timeval const *sample)
{
  summary->sum.tv_usec += sample->tv_usec;
  if (summary->sum.tv_usec > 1000000) {
    summary->sum.tv_usec -= 1000000;
    summary->sum.tv_sec++;
  }
  summary->sum.tv_sec += sample->tv_sec;

  if (summary->samples == 0 ||
      summary->minimum.tv_sec > sample->tv_sec ||
      (summary->minimum.tv_sec == sample->tv_sec &&
       summary->minimum.tv_usec > sample->tv_usec)) {
    summary->minimum = *sample;
  }

  if (summary->maximum.tv_sec < sample->tv_sec ||
      (summary->maximum.tv_sec == sample->tv_sec &&
       summary->maximum.tv_usec < sample->tv_usec)) {
    summary->maximum = *sample;
  }

  summary->samples++;
}

static void xbeeStatsSummarise(struct XBeeStaticticsSummary const *summary)
{
  avrdude_message(MSG_NOTICE, "%s: Minimum response time: %lu.%06lu\n",
                  progname, summary->minimum.tv_sec, summary->minimum.tv_usec);
  avrdude_message(MSG_NOTICE, "%s: Maximum response time: %lu.%06lu\n",
                  progname, summary->maximum.tv_sec, summary->maximum.tv_usec);

  struct timeval average;

  const unsigned long samples = summary->samples;
  average.tv_sec = summary->sum.tv_sec / samples;

  unsigned long long usecs = summary->sum.tv_usec;
  usecs += (summary->sum.tv_sec % samples) * 1000000;
  usecs = usecs / samples;
  average.tv_sec += usecs / 1000000;
  average.tv_usec = usecs % 1000000;

  avrdude_message(MSG_NOTICE, "%s: Average response time: %lu.%06lu\n",
                  progname, average.tv_sec, average.tv_usec);
}

static void XBeeBootSessionInit(struct XBeeBootSession *xbs) {
  xbs->serialDevice = &serial_serdev;
  xbs->directMode = 1;
  xbs->xbeeResetPin = XBEE_DEFAULT_RESET_PIN;
  xbs->outSequence = 0;
  xbs->inSequence = 0;
  xbs->txSequence = 0;
  xbs->transportUnusable = 0;
  xbs->inInIndex = 0;
  xbs->inOutIndex = 0;
  xbs->sourceRouteHops = -1;
  xbs->sourceRouteChanged = 0;

  int group;
  for (group = 0; group < 3; group++) {
    int index;
    for (index = 0; index < 256; index++)
      xbs->sequenceStatistics[group * 256 + index].sendTime.tv_sec = (time_t)0;
    xbeeStatsReset(&xbs->groupSummary[group]);
  }
}

#define xbeebootsession(fdp) (struct XBeeBootSession*)((fdp)->pfd)

static void xbeedev_setresetpin(union filedescriptor *fdp, int xbeeResetPin)
{
  struct XBeeBootSession *xbs = xbeebootsession(fdp);
  xbs->xbeeResetPin = xbeeResetPin;
}

static void xbeedev_stats_send(struct XBeeBootSession *xbs,
                               char const *detail,
                               unsigned int group, unsigned char sequence,
                               struct timeval const *sendTime)
{
  struct XBeeSequenceStatistics *stats =
    &xbs->sequenceStatistics[group * 256 + sequence];

  stats->sendTime = *sendTime;

  avrdude_message(MSG_NOTICE2,
                  "%s: Stats: Send Group %s Sequence %u : "
                  "Send %lu.%06lu %s\n",
                  progname, groupNames[group],
                  (unsigned int)sequence,
                  (unsigned long)sendTime->tv_sec,
                  (unsigned long)sendTime->tv_usec,
                  detail);
}

static void xbeedev_stats_receive(struct XBeeBootSession *xbs,
                                  char const *detail,
                                  unsigned int group, unsigned char sequence,
                                  struct timeval const *receiveTime)
{
  struct XBeeSequenceStatistics *stats =
    &xbs->sequenceStatistics[group * 256 + sequence];
  struct timeval delay;
  time_t secs;
  long usecs;

  secs = receiveTime->tv_sec - stats->sendTime.tv_sec;
  usecs = receiveTime->tv_usec - stats->sendTime.tv_usec;

  if (usecs < 0) {
    usecs += 1000000;
    secs--;
  }

  delay.tv_sec = secs;
  delay.tv_usec = usecs;

  avrdude_message(MSG_NOTICE2,
                  "%s: Stats: Receive Group %s Sequence %u : "
                  "Send %lu.%06lu Receive %lu.%06lu Delay %lu.%06lu %s\n",
                  progname, groupNames[group],
                  (unsigned int)sequence,
                  (unsigned long)stats->sendTime.tv_sec,
                  (unsigned long)stats->sendTime.tv_usec,
                  (unsigned long)receiveTime->tv_sec,
                  (unsigned long)receiveTime->tv_usec,
                  (unsigned long)secs,
                  (unsigned long)usecs,
                  detail);

  xbeeStatsAdd(&xbs->groupSummary[group], &delay);
}

static int sendAPIRequest(struct XBeeBootSession *xbs,
                          unsigned char apiType,
                          int txSequence,
                          int apiOption,
                          int prePayload1,
                          int prePayload2,
                          int packetType,
                          int sequence,
                          int appType,
                          char const *detail,
                          unsigned int frameGroup,
                          unsigned int dataLength,
                          const unsigned char *data)
{
  unsigned char frame[256];

  unsigned char *fp = &frame[5];
  unsigned char *dataStart = fp;
  unsigned char checksum = 0xff;
  unsigned char length = 0;
  struct timeval time;

  gettimeofday(&time, NULL);

  avrdude_message(MSG_NOTICE2,
                  "%s: sendAPIRequest(): %lu.%06lu %d, %d, %d, %d %s\n",
                  progname, (unsigned long)time.tv_sec,
                  (unsigned long)time.tv_usec,
                  (int)packetType, (int)sequence, appType,
                  data == NULL ? -1 : (int)*data, detail);

#define fpput(x)                                                \
  do {                                                          \
    const unsigned char v = (x);                                \
    if (v == 0x7d || v == 0x7e || v == 0x11 || v == 0x13) {     \
      *fp++ = 0x7d;                                             \
      *fp++ = v ^ 0x20;                                         \
    } else {                                                    \
      *fp++ = v;                                                \
    }                                                           \
    checksum -= v;                                              \
    length++;                                                   \
  } while (0)

  fpput(apiType); /* ZigBee Receive Packet or ZigBee Transmit Request */

  if (apiOption >= 0)
    fpput(apiOption); /* Receive options (RX) */

  if (txSequence >= 0) {
    fpput(txSequence); /* Delivery sequence (TX/AT) */

    /* Record the frame send time */
    xbeedev_stats_send(xbs, detail, frameGroup, txSequence, &time);
  }

  if (apiType != 0x08) {
    /* Automatically inhibit addressing for local AT command requests. */
    size_t index;
    for (index = 0; index < 10; index++) {
      const unsigned char val = xbs->xbee_address[index];
      fpput(val);
    }

    /*
     * If this is an API call with remote address, but is not a Create
     * Source Route request, consider prefixing it with source routing
     * instructions.
     */
    if (apiType != 0x21 && xbs->sourceRouteChanged) {
      avrdude_message(MSG_NOTICE2, "%s: sendAPIRequest(): "
                      "Issuing Create Source Route request with %d hops\n",
                      progname, xbs->sourceRouteHops);

      int rc = sendAPIRequest(xbs, 0x21, /* Create Source Route */
                              0, -1, 0, xbs->sourceRouteHops,
                              -1, -1, -1,
                              "Create Source Route",
                              XBEE_STATS_FRAME_LOCAL, /* Local, no response */
                              xbs->sourceRouteHops * 2,
                              xbs->sourceRoute);
      if (rc != 0)
        return rc;

      xbs->sourceRouteChanged = 0;
    }
  }

  if (prePayload1 >= 0)
    fpput(prePayload1); /* Transmit broadcast radius */

  if (prePayload2 >= 0)
    fpput(prePayload2); /* Transmit options */

  if (packetType >= 0)
    fpput(packetType); /* XBEEBOOT_PACKET_TYPE_{ACK,REQUEST} */

  if (sequence >= 0) {
    fpput(sequence);

    /* Record the send time */
    if (packetType == XBEEBOOT_PACKET_TYPE_REQUEST)
      xbeedev_stats_send(xbs, detail, XBEE_STATS_TRANSMIT, sequence, &time);
  }

  if (appType >= 0)
    fpput(appType); /* FIRMWARE_DELIVER */

  {
    size_t index;
    for (index = 0; index < dataLength; index++)
      fpput(data[index]);
  }

  /* Length BEFORE checksum byte */
  const unsigned char unescapedLength = length;

  fpput(checksum);

  /* Length AFTER checksum byte */
  const unsigned int finalLength = fp - dataStart;

  frame[0] = 0x7e;
  fp = &frame[1];
  fpput(0);
  fpput(unescapedLength);
  const unsigned int prefixLength = fp - frame;
  unsigned char *frameStart = dataStart - prefixLength;
  memmove(frameStart, frame, prefixLength);

  return xbs->serialDevice->send(&xbs->serialDescriptor,
                                 frameStart, finalLength + prefixLength);
}

static int sendPacket(struct XBeeBootSession *xbs,
                      const char *detail,
                      unsigned char packetType,
                      unsigned char sequence,
                      int appType,
                      unsigned int dataLength,
                      const unsigned char *data)
{
  unsigned char apiType;
  int prePayload1;
  int prePayload2;

  if (xbs->directMode) {
    /*
     * In direct mode we are pretending to be an XBee device
     * forwarding on data received from the transmitting XBee.  We
     * therefore format the data as a remote XBee would, encapsulated
     * in a 0x90 packet.
     */
    apiType = 0x90; /* ZigBee Receive Packet */
    prePayload1 = -1;
    prePayload2 = -1;
  } else {
    /*
     * In normal mode we are requesting a payload delivery,
     * encapsulated in a 0x10 packet.
     */
    apiType = 0x10; /* ZigBee Transmit Request */
    prePayload1 = 0;
    prePayload2 = 0;
  }

  while ((++xbs->txSequence & 0xff) == 0);
  return sendAPIRequest(xbs, apiType, xbs->txSequence, -1,
                        prePayload1, prePayload2, packetType,
                        sequence, appType,
                        detail,
                        XBEE_STATS_FRAME_REMOTE,
                        dataLength, data);
}

#define XBEE_LENGTH_LEN 2
#define XBEE_CHECKSUM_LEN 1
#define XBEE_APITYPE_LEN 1
#define XBEE_APISEQUENCE_LEN 1
#define XBEE_ADDRESS_64BIT_LEN 8
#define XBEE_ADDRESS_16BIT_LEN 2
#define XBEE_RADIUS_LEN 1
#define XBEE_TXOPTIONS_LEN 1
#define XBEE_RXOPTIONS_LEN 1

static void xbeedev_record16Bit(struct XBeeBootSession *xbs,
                                const unsigned char *rx16Bit)
{
  /*
   * We don't start out knowing what the 16-bit device address is, but
   * we should receive it on the return packets, and re-use it from
   * that point on.
   */
  unsigned char * const tx16Bit =
    &xbs->xbee_address[XBEE_ADDRESS_64BIT_LEN];
  if (memcmp(rx16Bit, tx16Bit, XBEE_ADDRESS_16BIT_LEN) != 0) {
    avrdude_message(MSG_NOTICE2, "%s: xbeedev_record16Bit(): "
                    "New 16-bit address: %02x%02x\n",
                    progname,
                    (unsigned int)rx16Bit[0],
                    (unsigned int)rx16Bit[1]);
    memcpy(tx16Bit, rx16Bit, XBEE_ADDRESS_16BIT_LEN);
  }
}

/*
 * Return 0 on success.
 * Return -1 on generic error (normally serial timeout).
 * Return -512 + XBee AT Response code
 */
#define XBEE_AT_RETURN_CODE(x) (((x) >= -512 && (x) <= -256) ? (x) + 512 : -1)
static int xbeedev_poll(struct XBeeBootSession *xbs,
                        unsigned char **buf, size_t *buflen,
                        int waitForAck,
                        int waitForSequence)
{
  for (;;) {
    unsigned char byte;
    unsigned char frame[256];
    unsigned int frameSize;

  before_frame:
    do {
      const int rc = xbs->serialDevice->recv(&xbs->serialDescriptor, &byte, 1);
      if (rc < 0)
        return rc;
    } while (byte != 0x7e);

  start_of_frame:
    {
      size_t index = 0;
      int escaped = 0;
      frameSize = XBEE_LENGTH_LEN;
      do {
        const int rc = xbs->serialDevice->recv(&xbs->serialDescriptor,
                                               &byte, 1);
        if (rc < 0)
          return rc;

        if (byte == 0x7e)
          /*
           * No matter when we receive a frame start byte, we should
           * abort parsing and start a fresh frame.
           */
          goto start_of_frame;

        if (escaped) {
          byte ^= 0x20;
          escaped = 0;
        } else if (byte == 0x7d) {
          escaped = 1;
          continue;
        }

        if (index >= sizeof(frame))
          goto before_frame;

        frame[index++] = byte;

        if (index == XBEE_LENGTH_LEN) {
          /* Length plus the two length bytes, plus the checksum byte */
          frameSize = (frame[0] << 8 | frame[1]) +
            XBEE_LENGTH_LEN + XBEE_CHECKSUM_LEN;

          if (frameSize >= sizeof(frame))
            /* Too long - immediately give up on this frame */
            goto before_frame;
        }
      } while (index < frameSize);

      /* End of frame */
      unsigned char checksum = 1;
      size_t cIndex;
      for (cIndex = 2; cIndex < index; cIndex++) {
        checksum += frame[cIndex];
      }

      if (checksum) {
        /* Checksum didn't match */
        avrdude_message(MSG_NOTICE2,
                        "%s: xbeedev_poll(): Bad checksum %d\n",
                        progname, (int)checksum);
        continue;
      }
    }

    const unsigned char frameType = frame[2];

    struct timeval receiveTime;
    gettimeofday(&receiveTime, NULL);

    avrdude_message(MSG_NOTICE2,
                    "%s: xbeedev_poll(): %lu.%06lu Received frame type %x\n",
                    progname, (unsigned long)receiveTime.tv_sec,
                    (unsigned long)receiveTime.tv_usec,
                    (unsigned int)frameType);

    if (frameType == 0x97 && frameSize > 16) {
      /* Remote command response */
      unsigned char txSequence = frame[3];
      unsigned char resultCode = frame[16];

      xbeedev_stats_receive(xbs, "Remote AT command response",
                            XBEE_STATS_FRAME_REMOTE, txSequence, &receiveTime);

      avrdude_message(MSG_NOTICE,
                      "%s: xbeedev_poll(): Remote command %d result code %d\n",
                      progname, (int)txSequence, (int)resultCode);

      if (waitForSequence >= 0 && waitForSequence == frame[3])
        /* Received result for our sequence numbered request */
        return -512 + resultCode;
    } else if (frameType == 0x88 && frameSize > 6) {
      /* Local command response */
      unsigned char txSequence = frame[3];

      xbeedev_stats_receive(xbs, "Local AT command response",
                            XBEE_STATS_FRAME_LOCAL, txSequence, &receiveTime);

      avrdude_message(MSG_NOTICE,
                      "%s: xbeedev_poll(): Local command %c%c result code %d\n",
                      progname, frame[4], frame[5], (int)frame[6]);

      if (waitForSequence >= 0 && waitForSequence == txSequence)
        /* Received result for our sequence numbered request */
        return 0;
    } else if (frameType == 0x8b && frameSize > 7) {
      /* Transmit status */
      unsigned char txSequence = frame[3];

      xbeedev_stats_receive(xbs, "Transmit status", XBEE_STATS_FRAME_REMOTE,
                            txSequence, &receiveTime);

      avrdude_message(MSG_NOTICE2,
                      "%s: xbeedev_poll(): Transmit status %d result code %d\n",
                      progname, (int)frame[3], (int)frame[7]);
    } else if (frameType == 0xa1 &&
               frameSize >= XBEE_LENGTH_LEN + XBEE_APITYPE_LEN +
               XBEE_ADDRESS_64BIT_LEN +
               XBEE_ADDRESS_16BIT_LEN + 2 + XBEE_CHECKSUM_LEN) {
      /* Route Record Indicator */
      if (memcmp(&frame[XBEE_LENGTH_LEN + XBEE_APITYPE_LEN],
                 xbs->xbee_address, XBEE_ADDRESS_64BIT_LEN) != 0) {
        /* Not from our target device */
        avrdude_message(MSG_NOTICE2, "%s: xbeedev_poll(): "
                        "Route Record Indicator from other XBee");
        continue;
      }

      /*
       * We don't start out knowing what the 16-bit device address is,
       * but we should receive it on the return packets, and re-use it
       * from that point on.
       */
      {
        const unsigned char *rx16Bit =
          &frame[XBEE_LENGTH_LEN + XBEE_APITYPE_LEN +
                 XBEE_ADDRESS_64BIT_LEN];
        xbeedev_record16Bit(xbs, rx16Bit);
      }

      const unsigned int header = XBEE_LENGTH_LEN + XBEE_APITYPE_LEN +
        XBEE_ADDRESS_64BIT_LEN +
        XBEE_ADDRESS_16BIT_LEN;

      const unsigned char receiveOptions = frame[header];
      const unsigned char hops = frame[header + 1];

      avrdude_message(MSG_NOTICE2, "%s: xbeedev_poll(): "
                      "Route Record Indicator from target XBee: "
                      "hops=%d options=%d\n",
                      progname, (int)hops, (int)receiveOptions);

      if (frameSize < header + 2 + hops * 2 + XBEE_CHECKSUM_LEN)
        /* Bounds check: Frame is too small */
        continue;

      const unsigned int tableOffset = header + 2;

      unsigned char index;
      for (index = 0; index < hops; index++) {
        avrdude_message(MSG_NOTICE2, "%s: xbeedev_poll(): "
                        "Route Intermediate Hop %d : %02x%02x\n",
                        progname, (int)index,
                        (int)frame[tableOffset + index * 2],
                        (int)frame[tableOffset + index * 2 + 1]);
      }

      if (hops <= XBEE_MAX_INTERMEDIATE_HOPS) {
        if (xbs->sourceRouteHops != (int)hops ||
            memcmp(&frame[tableOffset], xbs->sourceRoute, hops * 2) != 0) {
          memcpy(xbs->sourceRoute, &frame[tableOffset], hops * 2);
          xbs->sourceRouteHops = hops;
          xbs->sourceRouteChanged = 1;

          avrdude_message(MSG_NOTICE2, "%s: xbeedev_poll(): "
                          "Route has changed\n",
                          progname);
        }
      }
    } else if (frameType == 0x10 || frameType == 0x90) {
      unsigned char *dataStart;
      unsigned int dataLength;

      if (frameType == 0x10) {
        /* Direct mode frame */
        const unsigned int header = XBEE_LENGTH_LEN +
          XBEE_APITYPE_LEN + XBEE_APISEQUENCE_LEN +
          XBEE_ADDRESS_64BIT_LEN + XBEE_ADDRESS_16BIT_LEN +
          XBEE_RADIUS_LEN + XBEE_TXOPTIONS_LEN;

        if (frameSize <= header + XBEE_CHECKSUM_LEN)
          /* Bounds check: Frame is too small */
          continue;

        dataLength = frameSize - header - XBEE_CHECKSUM_LEN;
        dataStart = &frame[header];
      } else {
        /* Remote reply frame */
        const unsigned int header = XBEE_LENGTH_LEN +
          XBEE_APITYPE_LEN + XBEE_ADDRESS_64BIT_LEN + XBEE_ADDRESS_16BIT_LEN +
          XBEE_RXOPTIONS_LEN;

        if (frameSize <= header + XBEE_CHECKSUM_LEN)
          /* Bounds check: Frame is too small */
          continue;

        dataLength = frameSize - header - XBEE_CHECKSUM_LEN;
        dataStart = &frame[header];

        if (memcmp(&frame[XBEE_LENGTH_LEN + XBEE_APITYPE_LEN],
                   xbs->xbee_address, XBEE_ADDRESS_64BIT_LEN) != 0) {
          /*
           * This packet is not from our target device.  Unlikely
           * to ever happen, but if it does we have to ignore
           * it.
           */
          continue;
        }

        /*
         * We don't start out knowing what the 16-bit device address
         * is, but we should receive it on the return packets, and
         * re-use it from that point on.
         */
        {
          const unsigned char *rx16Bit =
            &frame[XBEE_LENGTH_LEN + XBEE_APITYPE_LEN +
                   XBEE_ADDRESS_64BIT_LEN];
          xbeedev_record16Bit(xbs, rx16Bit);
        }
      }

      if (dataLength >= 2) {
        const unsigned char protocolType = dataStart[0];
        const unsigned char sequence = dataStart[1];

        avrdude_message(MSG_NOTICE2, "%s: xbeedev_poll(): "
                        "%lu.%06lu Packet %d #%d\n",
                        progname, (unsigned long)receiveTime.tv_sec,
                        (unsigned long)receiveTime.tv_usec,
                        (int)protocolType, (int)sequence);

        if (protocolType == XBEEBOOT_PACKET_TYPE_ACK) {
          /* ACK */
          xbeedev_stats_receive(xbs, "XBeeBoot ACK",
                                XBEE_STATS_TRANSMIT, sequence,
                                &receiveTime);

          /*
           * We can't update outSequence here, we already do that
           * somewhere else.
           */
          if (waitForAck >= 0 && waitForAck == sequence)
            return 0;
        } else if (protocolType == XBEEBOOT_PACKET_TYPE_REQUEST &&
                   dataLength >= 4 && dataStart[2] == 24) {
          /* REQUEST FRAME_REPLY */
          xbeedev_stats_receive(xbs, "XBeeBoot Receive", XBEE_STATS_RECEIVE,
                                sequence, &receiveTime);

          unsigned char nextSequence = xbs->inSequence;
          while ((++nextSequence & 0xff) == 0);
          if (sequence == nextSequence) {
            xbs->inSequence = nextSequence;

            const size_t textLength = dataLength - 3;
            size_t index;
            for (index = 0; index < textLength; index++) {
              const unsigned char data = dataStart[3 + index];
              if (buflen != NULL && *buflen > 0) {
                /* If we are receiving right now, and have a buffer... */
                *(*buf)++ = data;
                (*buflen)--;
              } else {
                xbs->inBuffer[xbs->inInIndex++] = data;
                if (xbs->inInIndex == sizeof(xbs->inBuffer))
                  xbs->inInIndex = 0;
                if (xbs->inInIndex == xbs->inOutIndex) {
                  /* Should be impossible */
                  avrdude_message(MSG_INFO, "%s: Buffer overrun", progname);
                  xbs->transportUnusable = 1;
                  return -1;
                }
              }
            }

            /*avrdude_message(MSG_INFO, "ACK %x\n", (unsigned int)sequence);*/
            sendPacket(xbs, "Transmit Request ACK",
                       XBEEBOOT_PACKET_TYPE_ACK, sequence, -1, 0, NULL);

            if (buf != NULL && *buflen == 0)
              /* Input buffer has been filled */
              return 0;

            /* Input buffer has NOT been filled, we are still in a receive */
            while ((++nextSequence & 0xff) == 0);
            xbeedev_stats_send(xbs, "poll", XBEE_STATS_RECEIVE,
                               nextSequence, &receiveTime);
          }
        }
      }
    }
  }
}

static int localAT(struct XBeeBootSession *xbs, char const *detail,
                   unsigned char at1, unsigned char at2, int value)
{
  if (xbs->directMode)
    /*
     * Remote XBee AT commands make no sense in direct mode - there is
     * no XBee device to communicate with.
     */
    return 0;

  while ((++xbs->txSequence & 0xff) == 0);
  const unsigned char sequence = xbs->txSequence;

  unsigned char buf[3];
  size_t length = 0;

  buf[length++] = at1;
  buf[length++] = at2;

  if (value >= 0)
    buf[length++] = (unsigned char)value;

  avrdude_message(MSG_NOTICE, "%s: Local AT command: %c%c\n",
                  progname, at1, at2);

  /* Local AT command 0x08 */
  sendAPIRequest(xbs, 0x08, sequence, -1, -1, -1, -1, -1, -1,
                 detail, XBEE_STATS_FRAME_LOCAL,
                 length, buf);

  int retries;
  for (retries = 0; retries < 5; retries++) {
    const int rc = xbeedev_poll(xbs, NULL, NULL, -1, sequence);
    if (!rc)
      return rc;
  }

  return -1;
}

/*
 * Return 0 on success.
 * Return -1 on generic error (normally serial timeout).
 * Return -512 + XBee AT Response code
 */
static int sendAT(struct XBeeBootSession *xbs, char const *detail,
                  unsigned char at1, unsigned char at2, int value)
{
  if (xbs->directMode)
    /*
     * Remote XBee AT commands make no sense in direct mode - there is
     * no XBee device to communicate with.
     */
    return 0;

  while ((++xbs->txSequence & 0xff) == 0);
  const unsigned char sequence = xbs->txSequence;

  unsigned char buf[3];
  size_t length = 0;

  buf[length++] = at1;
  buf[length++] = at2;

  if (value >= 0)
    buf[length++] = (unsigned char)value;

  avrdude_message(MSG_NOTICE,
                  "%s: Remote AT command: %c%c\n", progname, at1, at2);

  /* Remote AT command 0x17 with Apply Changes 0x02 */
  sendAPIRequest(xbs, 0x17, sequence, -1,
                 -1, -1, -1,
                 0x02, -1,
                 detail, XBEE_STATS_FRAME_REMOTE,
                 length, buf);

  int retries;
  for (retries = 0; retries < 30; retries++) {
    const int rc = xbeedev_poll(xbs, NULL, NULL, -1, sequence);
    const int xbeeRc = XBEE_AT_RETURN_CODE(rc);
    if (xbeeRc == 0)
      /* Translate to normal success code */
      return 0;
    if (rc != -1)
      return rc;
  }

  return -1;
}

/*
 * Return 0 on no error recognised, 1 if error was detected and
 * reported.
 */
static int xbeeATError(int rc) {
  const int xbeeRc = XBEE_AT_RETURN_CODE(rc);
  if (xbeeRc < 0)
    return 0;

  if (xbeeRc == 1) {
    avrdude_message(MSG_INFO, "%s: Error communicating with Remote XBee\n",
                    progname);
  } else if (xbeeRc == 2) {
    avrdude_message(MSG_INFO, "%s: Remote XBee command error: "
                    "Invalid command\n",
                    progname);
  } else if (xbeeRc == 3) {
    avrdude_message(MSG_INFO, "%s: Remote XBee command error: "
                    "Invalid parameter\n",
                    progname);
  } else if (xbeeRc == 4) {
    avrdude_message(MSG_INFO, "%s: Remote XBee error: "
                    "Transmission failure\n",
                    progname);
  } else {
    avrdude_message(MSG_INFO, "%s: Unrecognised remote XBee error code %d\n",
                    progname, xbeeRc);
  }
  return 1;
}

static void xbeedev_free(struct XBeeBootSession *xbs)
{
  xbs->serialDevice->close(&xbs->serialDescriptor);
  free(xbs);
}

static void xbeedev_close(union filedescriptor *fdp)
{
  struct XBeeBootSession *xbs = xbeebootsession(fdp);
  xbeedev_free(xbs);
}

static int xbeedev_open(char *port, union pinfo pinfo,
                        union filedescriptor *fdp)
{
  /*
   * The syntax for XBee devices is defined as:
   *
   * -P <XBeeAddress>@[serialdevice]
   *
   * ... or ...
   *
   * -P @[serialdevice]
   *
   * ... for a direct connection.
   */
  char *ttySeparator = strchr(port, '@');
  if (ttySeparator == NULL) {
    avrdude_message(MSG_INFO,
                    "%s: XBee: Bad port syntax: "
                    "require \"<xbee-address>@<serial-device>\"\n",
                    progname);
    return -1;
  }

  struct XBeeBootSession *xbs = malloc(sizeof(struct XBeeBootSession));
  if (xbs == NULL) {
    avrdude_message(MSG_INFO, "%s: xbeedev_open(): out of memory\n",
                    progname);
    return -1;
  }

  XBeeBootSessionInit(xbs);

  char *tty = &ttySeparator[1];

  if (ttySeparator == port) {
    /* Direct connection */
    memset(xbs->xbee_address, 0, 8);
    xbs->directMode = 1;
  } else {
    size_t addrIndex = 0;
    int nybble = -1;
    char const *address = port;
    while (address != ttySeparator) {
      char hex = *address++;
      unsigned int val;
      if (hex >= '0' && hex <= '9') {
        val = hex - '0';
      } else if (hex >= 'A' && hex <= 'F') {
        val = hex - 'A' + 10;
      } else if  (hex >= 'a' && hex <= 'f') {
        val = hex - 'a' + 10;
      } else {
        break;
      }
      if (nybble == -1) {
        nybble = val;
      } else {
        xbs->xbee_address[addrIndex++] = (nybble * 16) | val;
        nybble = -1;
        if (addrIndex == 8)
          break;
      }
    }

    if (addrIndex != 8 || address != ttySeparator || nybble != -1) {
      avrdude_message(MSG_INFO,
                      "%s: XBee: Bad XBee address: "
                      "require 16-character hexadecimal address\"\n",
                      progname);
      free(xbs);
      return -1;
    }

    xbs->directMode = 0;
  }

  /* Unknown 16 bit address */
  xbs->xbee_address[8] = 0xff;
  xbs->xbee_address[9] = 0xfe;

  avrdude_message(MSG_TRACE,
                  "%s: XBee address: %02x%02x%02x%02x%02x%02x%02x%02x\n",
                  progname,
                  (unsigned int)xbs->xbee_address[0],
                  (unsigned int)xbs->xbee_address[1],
                  (unsigned int)xbs->xbee_address[2],
                  (unsigned int)xbs->xbee_address[3],
                  (unsigned int)xbs->xbee_address[4],
                  (unsigned int)xbs->xbee_address[5],
                  (unsigned int)xbs->xbee_address[6],
                  (unsigned int)xbs->xbee_address[7]);

  if (pinfo.baud) {
    /*
     * User supplied the correct baud rate.
     */
  } else if (xbs->directMode) {
    /*
     * In direct mode, default to 19200.
     *
     * Why?
     *
     * In this mode, we are NOT talking to an XBee, we are talking
     * directly to an AVR device that thinks it is talking to an XBee
     * itself.
     *
     * Because, an XBee is a 3.3V device defaulting to 9600baud, and
     * the Atmel328P is only rated at a maximum clock rate of 8MHz
     * with a 3.3V supply, so there's a high likelihood a remote
     * Atmel328P will be clocked at 8MHz.
     *
     * With a direct connection, there's a good chance we're talking
     * to an Arduino clocked at 16MHz with an XBee-enabled chip
     * plugged in.  The doubled clock rate means a doubled serial
     * rate.  Double 9600 baud == 19200 baud.
     */
    pinfo.baud = 19200;
  } else {
    /*
     * In normal mode, default to 9600.
     *
     * Why?
     *
     * XBee devices default to 9600 baud.  In this mode we are talking
     * to the XBee device, not the far-end device, so it's the local
     * XBee baud rate we should select.  The baud rate of the AVR
     * device is irrelevant.
     */
    pinfo.baud = 9600;
  }

  avrdude_message(MSG_NOTICE, "%s: Baud %ld\n", progname, (long)pinfo.baud);

  {
    const int rc = xbs->serialDevice->open(tty, pinfo,
                                           &xbs->serialDescriptor);
    if (rc < 0) {
      free(xbs);
      return rc;
    }
  }

  if (!xbs->directMode) {
    /* Attempt to ensure the local XBee is in API mode 2 */
    {
      const int rc = localAT(xbs, "AT AP=2", 'A', 'P', 2);
      if (rc < 0) {
        avrdude_message(MSG_INFO, "%s: Local XBee is not responding.\n",
                        progname);
        xbeedev_free(xbs);
        return rc;
      }
    }

    /*
     * At this point we want to set the remote XBee parameters as
     * required for talking to XBeeBoot.  Ideally we would start with
     * an "FR" full reset, but because that causes the XBee to
     * disappear off the mesh for a significant period and become
     * unresponsive, we don't do that.
     */

    /*
     * Issue an "Aggregate Routing Notification" to enable many-to-one
     * routing to this device.  This has two effects:
     *
     * - Establishes a route from the remote XBee attached to the CPU
     *   being programmed back to the local XBee.
     *
     * - Enables the 0xa1 Route frames so that we can make use of
     *   Source Routing to deliver packets directly to the remote
     *   XBee.
     *
     * Under "RF packet routing" subsection "Many-to-One routing", the
     * XBee S2C manual states "Applications that require multiple data
     * collectors can also use many-to-one routing. If more than one
     * data collector device sends a many-to-one broadcast, devices
     * create one reverse routing table entry for each collector."
     *
     * Under "RF packet routing" subsection "Source routing", the XBee
     * S2C manual states "To use source routing, a device must use the
     * API mode, and it must send periodic many-to-one route request
     * broadcasts (AR command) to create a many-to-one route to it on
     * all devices".
     */
    {
      const int rc = localAT(xbs, "AT AR=0", 'A', 'R', 0);
      if (rc < 0) {
        avrdude_message(MSG_INFO, "%s: Local XBee is not responding.\n",
                        progname);
        xbeedev_free(xbs);
        return rc;
      }
    }

    /*
     * Disable RTS input on the remote XBee, just in case it is
     * enabled by default.  XBeeBoot doesn't attempt to support flow
     * control, and so it may not correctly drive this pin if RTS mode
     * is the default configuration.
     *
     * XBee IO port 6 is the only pin that supports RTS mode, so there
     * is no need to support any alternative pin.
     */
    const int rc = sendAT(xbs, "AT D6=0", 'D', '6', 0);
    if (rc < 0) {
      xbeedev_free(xbs);

      if (xbeeATError(rc))
        return -1;

      avrdude_message(MSG_INFO, "%s: Remote XBee is not responding.\n",
                      progname);
      return rc;
    }
  }

  fdp->pfd = xbs;

  return 0;
}

static int xbeedev_send(union filedescriptor *fdp,
                        const unsigned char *buf, size_t buflen)
{
  struct XBeeBootSession *xbs = xbeebootsession(fdp);

  if (xbs->transportUnusable)
    /* Don't attempt to continue on an unusable transport layer */
    return -1;

  while (buflen > 0) {
    unsigned char sequence = xbs->outSequence;
    while ((++sequence & 0xff) == 0);
    xbs->outSequence = sequence;

    /*
     * We are about to send some data, and that might lead potentially
     * to received data before we see the ACK for this transmission.
     * As this might be the trigger seen before the next "recv"
     * operation, record that we have delivered this potential
     * trigger.
     */
    {
      unsigned char nextSequence = xbs->inSequence;
      while ((++nextSequence & 0xff) == 0);

      struct timeval sendTime;
      gettimeofday(&sendTime, NULL);

      xbeedev_stats_send(xbs, "send", XBEE_STATS_RECEIVE,
                         nextSequence, &sendTime);
    }

    /*
     * Chunk the data into chunks of up to XBEEBOOT_MAX_CHUNK bytes.
     */
    unsigned char maximum_chunk = XBEEBOOT_MAX_CHUNK;

    /*
     * Source routing incurs a two byte fixed overhead, plus a two
     * byte additional cost per intermediate hop.
     *
     * We are attempting to avoid fragmentation here, so resize our
     * maximum size to anticipate the overhead of the current number
     * of hops.  If our maximum chunk would be less than one, just
     * give up and hope fragmentation will somehow save us.
     */
    const int hops = xbs->sourceRouteHops;
    if (hops > 0 && (hops * 2 + 2) < XBEEBOOT_MAX_CHUNK)
      maximum_chunk -= hops * 2 + 2;

    const unsigned char blockLength =
      (buflen > maximum_chunk) ? maximum_chunk : buflen;

    int pollRc = 0;

    /* Repeatedly send whilst timing out waiting for ACK responses. */
    int retries;
    for (retries = 0; retries < XBEE_MAX_RETRIES; retries++) {
      int sendRc = sendPacket(xbs, "Transmit Request Data",
                              XBEEBOOT_PACKET_TYPE_REQUEST, sequence,
                              23 /* FIRMWARE_DELIVER */,
                              blockLength, buf);
      if (sendRc < 0) {
        /* There is no way to recover from a failure mid-send */
        xbs->transportUnusable = 1;
        return sendRc;
      }

      pollRc = xbeedev_poll(xbs, NULL, NULL, sequence, -1);
      if (pollRc == 0) {
        /* Send was ACK'd */
        buflen -= blockLength;
        buf += blockLength;
        break;
      }

      /*
       * If we don't receive an ACK it might be because the chip
       * missed an ACK from us.  Resend that too after a timeout,
       * unless it's zero which is an illegal sequence number.
       */
      if (xbs->inSequence != 0) {
        int ackRc = sendPacket(xbs, "Transmit Request ACK [Retry in send]",
                               XBEEBOOT_PACKET_TYPE_ACK,
                               xbs->inSequence, -1, 0, NULL);
        if (ackRc < 0) {
          /* There is no way to recover from a failure mid-send */
          xbs->transportUnusable = 1;
          return ackRc;
        }
      }
    }

    if (pollRc < 0) {
      /* There is no way to recover from a failure mid-send */
      xbs->transportUnusable = 1;
      return pollRc;
    }
  }

  return 0;
}

static int xbeedev_recv(union filedescriptor *fdp,
                        unsigned char *buf, size_t buflen)
{
  struct XBeeBootSession *xbs = xbeebootsession(fdp);

  /*
   * First de-buffer anything previously received in a chunk that
   * couldn't be immediately delievered.
   */
  while (xbs->inInIndex != xbs->inOutIndex) {
    *buf++ = xbs->inBuffer[xbs->inOutIndex++];
    if (xbs->inOutIndex == sizeof(xbs->inBuffer))
      xbs->inOutIndex = 0;
    if (--buflen == 0)
      return 0;
  }

  if (xbs->transportUnusable)
    /* Don't attempt to continue on an unusable transport layer */
    return -1;

  /*
   * When we expect to receive data, that is the time to start the
   * clock.
   */
  {
    unsigned char nextSequence = xbs->inSequence;
    while ((++nextSequence & 0xff) == 0);

    struct timeval sendTime;
    gettimeofday(&sendTime, NULL);

    xbeedev_stats_send(xbs, "recv", XBEE_STATS_RECEIVE,
                       nextSequence, &sendTime);
  }

  int retries;
  for (retries = 0; retries < XBEE_MAX_RETRIES; retries++) {
    const int rc = xbeedev_poll(xbs, &buf, &buflen, -1, -1);
    if (rc == 0)
      return rc;

    if (xbs->transportUnusable)
      /* Don't attempt to continue on an unusable transport layer */
      return -1;

    /*
     * The chip may have missed an ACK from us.  Resend after a
     * timeout.
     */
    if (xbs->inSequence != 0)
      sendPacket(xbs, "Transmit Request ACK [Retry in recv]",
                 XBEEBOOT_PACKET_TYPE_ACK, xbs->inSequence, -1, 0, NULL);
  }
  return -1;
}

static int xbeedev_drain(union filedescriptor *fdp, int display)
{
  struct XBeeBootSession *xbs = xbeebootsession(fdp);

  if (xbs->transportUnusable)
    /* Don't attempt to continue on an unusable transport layer */
    return -1;

  /*
   * Flushing the local serial buffer is unhelpful under this
   * protocol.
   */
  do {
    xbs->inOutIndex = xbs->inInIndex = 0;
  } while (xbeedev_poll(xbs, NULL, NULL, -1, -1) == 0);

  return 0;
}

static int xbeedev_set_dtr_rts(union filedescriptor *fdp, int is_on)
{
  struct XBeeBootSession *xbs = xbeebootsession(fdp);

  if (xbs->directMode)
    /* Correct for direct mode */
    return xbs->serialDevice->set_dtr_rts(&xbs->serialDescriptor, is_on);

  /*
   * For non-direct mode (Over-The-Air) we need to issue XBee commands
   * to the remote XBee in order to reset the AVR CPU and initiate the
   * XBeeBoot bootloader.
   */
  const int rc = sendAT(xbs, is_on ? "AT [DTR]=low" : "AT [DTR]=high",
                        'D', '0' + xbs->xbeeResetPin, is_on ? 5 : 4);
  if (rc < 0) {
    if (xbeeATError(rc))
      return -1;

    avrdude_message(MSG_INFO,
                    "%s: Remote XBee is not responding.\n", progname);
    return rc;
  }

  return 0;
}

/*
 * Device descriptor for XBee framing.
 */
static struct serial_device xbee_serdev_frame = {
  .open = xbeedev_open,
  .close = xbeedev_close,
  .send = xbeedev_send,
  .recv = xbeedev_recv,
  .drain = xbeedev_drain,
  .set_dtr_rts = xbeedev_set_dtr_rts,
  .flags = SERDEV_FL_NONE,
};

static int xbee_getsync(PROGRAMMER *pgm)
{
  unsigned char buf[2], resp[2];

  /*
   * Issue sync request as per STK500.  Unlike stk500_getsync(), don't
   * retry here - the underlying protocol will deal with retries for
   * us in xbeedev_send() and should be reliable.
   */
  buf[0] = Cmnd_STK_GET_SYNC;
  buf[1] = Sync_CRC_EOP;

  int sendRc = serial_send(&pgm->fd, buf, 2);
  if (sendRc < 0) {
    avrdude_message(MSG_INFO,
                    "%s: xbee_getsync(): failed to deliver STK_GET_SYNC "
                    "to the remote XBeeBoot bootloader\n",
                    progname);
    return sendRc;
  }

  /*
   * The same is true of the receive - it will retry on timeouts until
   * the response buffer is full.
   */
  int recvRc = serial_recv(&pgm->fd, resp, 2);
  if (recvRc < 0) {
    avrdude_message(MSG_INFO,
                    "%s: xbee_getsync(): no response to STK_GET_SYNC "
                    "from the remote XBeeBoot bootloader\n",
                    progname);
    return recvRc;
  }

  if (resp[0] != Resp_STK_INSYNC) {
    avrdude_message(MSG_INFO, "%s: xbee_getsync(): not in sync: resp=0x%02x\n",
                    progname, (unsigned int)resp[0]);
    return -1;
  }

  if (resp[1] != Resp_STK_OK) {
    avrdude_message(MSG_INFO, "%s: xbee_getsync(): in sync, not OK: "
                    "resp=0x%02x\n",
                    progname, (unsigned int)resp[1]);
    return -1;
  }

  return 0;
}

static int xbee_open(PROGRAMMER *pgm, char *port)
{
  union pinfo pinfo;
  strcpy(pgm->port, port);
  pinfo.baud = pgm->baudrate;

  /* Wireless is lossier than normal serial */
  serial_recv_timeout = 1000;

  serdev = &xbee_serdev_frame;

  if (serial_open(port, pinfo, &pgm->fd) == -1) {
    return -1;
  }

  /*
   * NB: Because we are making use of the STK500 programmer
   * implementation, we can't readily use pgm->cookie ourselves.  We
   * can use the private "flag" field in the PROGRAMMER though, as
   * it's unused by stk500.c.
   */
  xbeedev_setresetpin(&pgm->fd, pgm->flag);

  /* Clear DTR and RTS */
  serial_set_dtr_rts(&pgm->fd, 0);
  usleep(250*1000);

  /* Set DTR and RTS back to high */
  serial_set_dtr_rts(&pgm->fd, 1);
  usleep(50*1000);

  /*
   * At this point stk500_drain() and stk500_getsync() calls would
   * normally be made.  But given that we have a transport layer over
   * the serial command stream, the drain and repeated STK_GET_SYNC
   * requests are not very helpful.  Instead, skip the draining
   * entirely, and issue the STK_GET_SYNC ourselves.
   */
  if (xbee_getsync(pgm) < 0)
    return -1;

  return 0;
}

static void xbee_close(PROGRAMMER *pgm)
{
  struct XBeeBootSession *xbs = xbeebootsession(&pgm->fd);

  xbs->serialDevice->set_dtr_rts(&xbs->serialDescriptor, 0);

  /*
   * We have tweaked a few settings on the XBee, including the RTS
   * mode and the reset pin's configuration.  Do a soft full reset,
   * restoring the device to its normal power-on settings.
   *
   * Note that this DOES mean that the remote XBee will be
   * uncontactable until it has restarted and re-established
   * communications on the mesh.
   */
  if (!xbs->directMode) {
    const int rc = sendAT(xbs, "AT FR", 'F', 'R', -1);
    xbeeATError(rc);
  }

  avrdude_message(MSG_NOTICE, "%s: Statistics for local requests\n", progname);
  xbeeStatsSummarise(&xbs->groupSummary[XBEE_STATS_FRAME_LOCAL]);
  avrdude_message(MSG_NOTICE, "%s: Statistics for remote requests\n", progname);
  xbeeStatsSummarise(&xbs->groupSummary[XBEE_STATS_FRAME_REMOTE]);
  avrdude_message(MSG_NOTICE, "%s: Statistics for TX requests\n", progname);
  xbeeStatsSummarise(&xbs->groupSummary[XBEE_STATS_TRANSMIT]);
  avrdude_message(MSG_NOTICE, "%s: Statistics for RX requests\n", progname);
  xbeeStatsSummarise(&xbs->groupSummary[XBEE_STATS_RECEIVE]);

  xbeedev_free(xbs);

  pgm->fd.pfd = NULL;
}

static int xbee_parseextparms(PROGRAMMER *pgm, LISTID extparms)
{
  LNODEID ln;
  const char *extended_param;
  int rc = 0;

  for (ln = lfirst(extparms); ln; ln = lnext(ln)) {
    extended_param = ldata(ln);

    if (strncmp(extended_param,
                "xbeeresetpin=", 13 /*strlen("xbeeresetpin=")*/) == 0) {
      int resetpin;
      if (sscanf(extended_param, "xbeeresetpin=%i", &resetpin) != 1 ||
          resetpin <= 0 || resetpin > 7) {
        avrdude_message(MSG_INFO, "%s: xbee_parseextparms(): "
                        "invalid xbeeresetpin '%s'\n",
                        progname, extended_param);
        rc = -1;
        continue;
      }

      pgm->flag = resetpin;
      continue;
    }

    avrdude_message(MSG_INFO, "%s: xbee_parseextparms(): "
                    "invalid extended parameter '%s'\n",
                    progname, extended_param);
    rc = -1;
  }

  return rc;
}

const char xbee_desc[] = "XBee Series 2 Over-The-Air (XBeeBoot)";

void xbee_initpgm(PROGRAMMER *pgm)
{
  /*
   * This behaves like an Arduino, but with packet encapsulation of
   * the serial streams, XBee device management, and XBee GPIO for the
   * Auto-Reset feature.
   */
  stk500_initpgm(pgm);

  strncpy(pgm->type, "XBee", sizeof(pgm->type));
  pgm->read_sig_bytes = xbee_read_sig_bytes;
  pgm->open = xbee_open;
  pgm->close = xbee_close;

  /*
   * NB: Because we are making use of the STK500 programmer
   * implementation, we can't readily use pgm->cookie ourselves, nor
   * can we override setup() and teardown().  We can use the private
   * "flag" field in the PROGRAMMER though, as it's unused by
   * stk500.c.
   */
  pgm->parseextparams = xbee_parseextparms;
  pgm->flag = XBEE_DEFAULT_RESET_PIN;
}
