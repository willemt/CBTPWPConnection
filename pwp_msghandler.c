#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>

/* for uint32_t */
#include <stdint.h>

#include <assert.h>
#include "pwp_connection.h"
#include "pwp_msghandler.h"
#include "bitfield.h"

#if 0
void pwp_conn_keepalive(void* pco)
{
    printf("KEEPALIVE\n");
}

void pwp_conn_choke(void* pco)
{
    printf("CHOKE\n");
}

void pwp_conn_unchoke(void* pco)
{
    printf("UNCHOKE\n");
}

void pwp_conn_interested(void* pco)
{
    printf("INTERESTED\n");
}

void pwp_conn_uninterested(void* pco)
{
    printf("UNINTERESTED\n");
}

void pwp_conn_have(void* pco, msg_have_t* have)
{
    printf("HAVE %d\n", have->pieceidx);
}

void pwp_conn_bitfield(void* pco, msg_bitfield_t* bitfield)
{
    printf("BITFIELD %s\n", bitfield_str(&bitfield->bf));
}

void pwp_conn_request(void* pco,
    msg_request_t *request)
{
    printf("REQUEST %d %d %d\n",
            request->pieceidx,
            request->block_byte_offset,
            request->block_len);
}

void pwp_conn_cancel(void* pco,
    msg_cancel_t *cancel)
{
    printf("CANCEL %d %d %d\n",
            cancel->pieceidx,
            cancel->block_byte_offset,
            cancel->block_len);
}

void pwp_conn_piece(void* pco,
    msg_piece_t *piece)
{
    printf("PIECE %d %d '%.*s'\n",
            piece->pieceidx,
            piece->block_byte_offset,
            piece->data_size,
            piece->data);
}
#endif

typedef struct {
    uint32_t len;
    unsigned char id;
    payload_t pl;
    int bytes_read;
    int tok_bytes_read;
    union {
        msg_have_t have;
        msg_bitfield_t bitfield;
        msg_request_t request;
        msg_cancel_t cancel;
        msg_piece_t piece;
    };
} msg_t;

typedef struct {
    /* current message we are reading */
    msg_t msg;

    /* peer connection */
    void* pc;
} bt_peer_connection_event_handler_t;



static void __endmsg(msg_t* msg)
{
    msg->bytes_read = 0;
}

static int __read_uint32(
        uint32_t* in,
        msg_t *msg,
        unsigned char** buf,
        unsigned int *len)
{
    while (1)
    {

        if (msg->tok_bytes_read == 4)
        {
            msg->tok_bytes_read = 0;
            return 1;
        }
        else if (*len == 0)
        {
            return 0;
        }

        *((unsigned char*)in + msg->tok_bytes_read) = **buf;

//        printf("%x\n", **buf);

        msg->tok_bytes_read += 1;
        msg->bytes_read += 1;
        *buf += 1;
        *len -= 1;
    }
}

static int __read_byte(
        unsigned char* in,
        unsigned int *tot_bytes_read,
        unsigned char** buf,
        unsigned int *len)
{
    if (*len == 0)
        return 0;

    *in = **buf;
    *tot_bytes_read += 1;
    *buf += 1;
    *len -= 1;
    return 1;
}

/**
 * create a new msg handler */
void* pwp_msghandler_new(void *pc)
{
    bt_peer_connection_event_handler_t* me;

    me = calloc(1,sizeof(bt_peer_connection_event_handler_t));
    me->pc = pc;
    return me;
}

/**
 * Receive this much data on this step. */
void pwp_msghandler_dispatch_from_buffer(void *mh, unsigned char* buf, unsigned int len)
{
    bt_peer_connection_event_handler_t* me = mh;
    msg_t* msg = &me->msg;

    while (0 < len)
    {
//        printf("len: %d\n", len);

        /* read length of message (int) */
        if (msg->bytes_read < 4)
        {
            if (1 == __read_uint32(&msg->len, &me->msg, &buf, &len))
            {
                /* it was a keep alive message */
                if (0 == msg->len)
                {
                    pwp_conn_keepalive(me->pc);
                    __endmsg(&me->msg);
                }
            }
        }
        /* get message ID */
        else if (msg->bytes_read == 4)
        {
            __read_byte(&msg->id, &msg->bytes_read,&buf,&len);

            if (msg->len != 1) continue;

            switch (msg->id)
            {
            case PWP_MSGTYPE_CHOKE:
                pwp_conn_choke(me->pc);
                break;
            case PWP_MSGTYPE_UNCHOKE:
                pwp_conn_unchoke(me->pc);
                break;
            case PWP_MSGTYPE_INTERESTED:
                pwp_conn_interested(me->pc);
                break;
            case PWP_MSGTYPE_UNINTERESTED:
                pwp_conn_uninterested(me->pc);
                break;
            default: assert(0); break;
            }
            __endmsg(&me->msg);
        }
        else 
        {
            switch (msg->id)
            {
            case PWP_MSGTYPE_HAVE:
                if (1 == __read_uint32(&msg->have.pieceidx,
                            &me->msg, &buf,&len))
                {
                    pwp_conn_have(me->pc,&msg->have);
                    __endmsg(&me->msg);
                    continue;
                }

                break;
            case PWP_MSGTYPE_BITFIELD:
                {
                    unsigned char val;
                    unsigned int ii;

                    if (msg->bytes_read == 5)
                    {
                         bitfield_init(&msg->bitfield.bf,(msg->len - 1) * 8);
                    }

                    __read_byte(&val, &msg->bytes_read,&buf,&len);

                    /* mark bits from byte */
                    for (ii=0; ii<8; ii++)
                    {
                        if (0x1 == ((unsigned char)(val<<ii) >> 7))
                        {
                            bitfield_mark(&msg->bitfield.bf,
                                    (msg->bytes_read - 5 - 1) * 8 + ii);
                        }
                    }

                    /* done reading bitfield */
                    if (msg->bytes_read == 4 + msg->len)
                    {
                        pwp_conn_bitfield(me->pc, &msg->bitfield);
                        __endmsg(&me->msg);
                    }
                }
                break;


            case PWP_MSGTYPE_REQUEST:
                if (msg->bytes_read < 5 + 4)
                {
                    __read_uint32(&msg->request.pieceidx,
                            &me->msg, &buf,&len);
                }
                else if (msg->bytes_read < 9 + 4)
                {
                    __read_uint32(&msg->request.block_byte_offset,
                            &me->msg,&buf,&len);
                }
                else if (1 == __read_uint32(&msg->request.block_len,
                            &me->msg, &buf,&len))
                {
                    pwp_conn_request(me->pc, &msg->request);
                    __endmsg(&me->msg);
                }

                break;
            case PWP_MSGTYPE_CANCEL:
                if (msg->bytes_read < 5 + 4)
                {
                    __read_uint32(&msg->cancel.pieceidx,
                            &me->msg, &buf,&len);
                }
                else if (msg->bytes_read < 9 + 4)
                {
                    __read_uint32(&msg->cancel.block_byte_offset,
                            &me->msg,&buf,&len);
                }
                else if (1 == __read_uint32(&msg->cancel.block_len,
                            &me->msg, &buf,&len))
                {
                    pwp_conn_cancel(me->pc, &msg->cancel);
                    __endmsg(&me->msg);
                }
                break;
            case PWP_MSGTYPE_PIECE:
                if (msg->bytes_read < 5 + 4)
                {
                    __read_uint32(&msg->piece.pieceidx,
                            &me->msg, &buf,&len);
                }
                else if (msg->bytes_read < 9 + 4)
                {
                    __read_uint32(&msg->piece.block_byte_offset,
                            &me->msg,&buf,&len);
                }
                else
                {
                    int size;

                    size = len;

                    /* check it isn't bigger than what the message tells
                     * us we should be expecting */
                    if (size > msg->len - 1 - 4 - 4)
                    {
                        size = msg->len;
                    }

                    msg->piece.data = buf;
                    msg->piece.data_size = size;
                    pwp_conn_piece(me->pc, &msg->piece);

                    /* if we haven't received the full piece, why don't we
                     * just split it virtually? */
                    /* shorten the message */
                    msg->len -= size;
                    msg->piece.block_byte_offset += size;

                    /* if we received the whole message we're done */
                    if (msg->len == 9)
                        __endmsg(&me->msg);

                    len -= size;
                    buf += len;
                }
                break;
            default:
                assert(0); break;
            }
        }
    }
}

#if 0
int main()
{
    char data[100];
    int len = 4;
    char* ptr;
    void* mh;

    /* keepalive */
    ptr = data;
    mh = pwp_msghandler_new(NULL);
    bitstream_write_uint32(&ptr,0);
    pwp_msghandler_dispatch_from_buffer(mh, data, 4);
    printf("\n\n");

    /* choke */
    ptr = data;
    mh = pwp_msghandler_new(NULL);
    bitstream_write_uint32(&ptr,1);
    bitstream_write_ubyte(&ptr,PWP_MSGTYPE_CHOKE);
    pwp_msghandler_dispatch_from_buffer(mh, data, 4 + 1);
    printf("\n\n");

    /* unchoke */
    ptr = data;
    mh = pwp_msghandler_new(NULL);
    bitstream_write_uint32(&ptr,1);
    bitstream_write_ubyte(&ptr,PWP_MSGTYPE_UNCHOKE);
    pwp_msghandler_dispatch_from_buffer(mh, data, 4 + 1);
    printf("\n\n");

    /* interested */
    ptr = data;
    mh = pwp_msghandler_new(NULL);
    bitstream_write_uint32(&ptr,1);
    bitstream_write_ubyte(&ptr,PWP_MSGTYPE_INTERESTED);
    pwp_msghandler_dispatch_from_buffer(mh, data, 4 + 1);
    printf("\n\n");

    /* uninterested */
    ptr = data;
    mh = pwp_msghandler_new(NULL);
    bitstream_write_uint32(&ptr,1);
    bitstream_write_ubyte(&ptr,PWP_MSGTYPE_UNINTERESTED);
    pwp_msghandler_dispatch_from_buffer(mh, data, 4 + 1);
    printf("\n\n");

    /* have */
    ptr = data;
    mh = pwp_msghandler_new(NULL);
    bitstream_write_uint32(&ptr,5);
    bitstream_write_ubyte(&ptr,PWP_MSGTYPE_HAVE);
    bitstream_write_uint32(&ptr,999);
    pwp_msghandler_dispatch_from_buffer(mh, data, 4 + 1 + 4);
    printf("\n\n");

    /* request */
    ptr = data;
    mh = pwp_msghandler_new(NULL);
    bitstream_write_uint32(&ptr,13);
    bitstream_write_ubyte(&ptr,PWP_MSGTYPE_REQUEST);
    bitstream_write_uint32(&ptr,123);
    bitstream_write_uint32(&ptr,456);
    bitstream_write_uint32(&ptr,789);
    pwp_msghandler_dispatch_from_buffer(mh, data, 4 + 1 + 4 + 4 + 4);
    printf("\n\n");

    /* cancel */
    ptr = data;
    mh = pwp_msghandler_new(NULL);
    bitstream_write_uint32(&ptr,13);
    bitstream_write_ubyte(&ptr,PWP_MSGTYPE_CANCEL);
    bitstream_write_uint32(&ptr,123);
    bitstream_write_uint32(&ptr,456);
    bitstream_write_uint32(&ptr,789);
    pwp_msghandler_dispatch_from_buffer(mh, data, 4 + 1 + 4 + 4 + 4);
    printf("\n\n");

    /* bitfield */
    ptr = data;
    mh = pwp_msghandler_new(NULL);
    bitstream_write_uint32(&ptr,2);
    bitstream_write_ubyte(&ptr,PWP_MSGTYPE_BITFIELD);
    bitstream_write_ubyte(&ptr,0x4e);
    pwp_msghandler_dispatch_from_buffer(mh, data, 4 + 1 + 1);
    printf("\n\n");

    /* piece */
    ptr = data;
    mh = pwp_msghandler_new(NULL);
    bitstream_write_uint32(&ptr,9 + 10);
    bitstream_write_ubyte(&ptr,PWP_MSGTYPE_PIECE);
    bitstream_write_uint32(&ptr,1);
    bitstream_write_uint32(&ptr,2);
    bitstream_write_ubyte(&ptr,'t');
    bitstream_write_ubyte(&ptr,'e');
    bitstream_write_ubyte(&ptr,'s');
    bitstream_write_ubyte(&ptr,'t');
    bitstream_write_ubyte(&ptr,' ');
    bitstream_write_ubyte(&ptr,'m');
    bitstream_write_ubyte(&ptr,'s');
    bitstream_write_ubyte(&ptr,'g');
    bitstream_write_ubyte(&ptr,'1');
    bitstream_write_ubyte(&ptr,'\0');
    pwp_msghandler_dispatch_from_buffer(mh, data, 4 + 1 + 4 + 4 + 10);
    printf("\n\n");

    /* piece */
    ptr = data;
    mh = pwp_msghandler_new(NULL);
    bitstream_write_uint32(&ptr,9 + 10);
    bitstream_write_ubyte(&ptr,PWP_MSGTYPE_PIECE);
    bitstream_write_uint32(&ptr,1);
    bitstream_write_uint32(&ptr,2);
    bitstream_write_ubyte(&ptr,'t');
    bitstream_write_ubyte(&ptr,'e');
    bitstream_write_ubyte(&ptr,'s');
    bitstream_write_ubyte(&ptr,'t');
    bitstream_write_ubyte(&ptr,' ');
    bitstream_write_ubyte(&ptr,'m');
    bitstream_write_ubyte(&ptr,'s');
    bitstream_write_ubyte(&ptr,'g');
    bitstream_write_ubyte(&ptr,'2');
    bitstream_write_ubyte(&ptr,'\0');
    /* read the first 5 bytes of data payload */
    pwp_msghandler_dispatch_from_buffer(mh, data, 4 + 1 + 4 + 4 + 5);
    /* read the last 5 bytes of data payload */
    pwp_msghandler_dispatch_from_buffer(mh, data + 4 + 1 + 4 + 4 + 5 , 5);
}
#endif

