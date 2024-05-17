/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"



#define MAX_NUM_RETRANSMISSIONS 5

/*
 * @brief
 * Struct store segment which was read from stdin and it's state
 */
typedef struct {
    ctcp_segment_t *segment;
    long last_time_send;
    int num_retransmissions;
} unack_segment_t;

/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
struct ctcp_state {
  struct ctcp_state *next;  /* Next in linked list */
  struct ctcp_state **prev; /* Prev in linked list */

  conn_t *conn;             /* Connection object -- needed in order to figure
                               out destination when sending */
  
  /* FIXME: Add other needed fields. */
  uint32_t seq_num;
  uint32_t ack_num;
  ctcp_segment_t * output_segment;
  ctcp_config_t * config;
  linked_list_t * unack_segments;

  bool FIN_recv;
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */
/*
 * @brief
 * create and return a data segment
 */
ctcp_segment_t * data_segment(ctcp_state_t * state, void * data, size_t len);

/*
 * @brief
 * create and return a FIN segment
 */
ctcp_segment_t * FIN_segment(ctcp_state_t * state);

/*
 * @brief
 * create and return an ACK segment
 */
ctcp_segment_t * ACK_segment(ctcp_state_t * state);

/*
 * @brief
 * convert segment from host-bytes-order to network-bytes-order
 */
void hton_segment(ctcp_segment_t * segment);

/*
 * @brief
 * Convert segment from network-bytes-order to host-bytes-order
 */
void ntoh_segment(ctcp_segment_t * segment);

/*
 * @brief
 * create and send ACK segment to acknowledge a segment or control error
 */
void send_ACK(ctcp_state_t * state);

ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
  /* Connection could not be established. */
  if (conn == NULL) {
    return NULL;
  }

  /* Established a connection. Create a new state and update the linked list
     of connection states. */
  ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
  state->next = state_list;
  state->prev = &state_list;
  if (state_list)
    state_list->prev = &state->next;
  state_list = state;

  /* Set fields. */
  state->conn = conn;
  /* FIXME: Do any other initialization here. */
  state->seq_num = 1;
  state->ack_num = 1;
  state->config = cfg;
  state->unack_segments = ll_create();
  state->FIN_recv = false;
  return state;
}

void ctcp_destroy(ctcp_state_t *state) {
  /* Update linked list. */
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);
  free(state);
  end_client();
}

void ctcp_read(ctcp_state_t *state) {
  /* FIXME */
  void * buf;
  int bytes_read;
  ctcp_segment_t * segment;
  unack_segment_t * unack_segment;
  buf = calloc(MAX_SEG_DATA_SIZE,1);
  bytes_read = conn_input(state->conn, buf, MAX_SEG_DATA_SIZE);
  if (bytes_read == -1)
  {
      /* EOF */
	segment = FIN_segment(state);
	state->seq_num += 1;
  }
  else
  {
	/* Data Segment */
	segment = data_segment(state, buf , bytes_read);
	state->seq_num += bytes_read;
  }
  hton_segment(segment);
  conn_send(state->conn, segment, ntohs(segment->len));
  /* create and add to the linked list of unacknowleged segment */	
  unack_segment = (unack_segment_t *)calloc(sizeof(unack_segment_t),1);
  unack_segment->segment = segment;
  unack_segment->last_time_send = current_time();
  unack_segment->num_retransmissions = 0;
  ll_add(state->unack_segments, unack_segment);
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  /* FIXME */
    uint16_t segment_cksum;
    uint16_t data_len;
    ll_node_t *node;
	ntoh_segment(segment);
    /* Truncated segment */
	if (len < segment->len)
	{
		free(segment);
		return;
	}    

	segment_cksum = segment->cksum;
	segment->cksum = 0;
    /* Corrupted segment */
    if (segment_cksum != cksum(segment,segment->len))
    {
	free(segment);
        return;
    }

    /* Out of order or Duplicated segment */
    if (segment->seqno != state->ack_num )
    {
	segment = ACK_segment(state);
	hton_segment(segment);
	conn_send(state->conn, segment, ntohs(segment->len));
        return;
    }
    data_len = segment->len - sizeof(ctcp_segment_t);
	/* ACK Segment */
      node = ll_front(state->unack_segments);
      if (node)
      {
        /* If ll has node which is now acknowlegded so remove it */
        ll_remove(state->unack_segments, node);
      }
    if (segment->flags & FIN)
    {
	state->FIN_recv = true;	
    }
    if ((segment->flags & ACK) && (state->FIN_recv == true))
    {
        ctcp_destroy(state);
    }
    /* FIN or Data segment */
    if ((segment->flags & FIN) || data_len > 0)
    {
	conn_output(state->conn, segment->data, data_len);
	if (segment->flags & FIN)
	{
		state->ack_num = segment->seqno + 1;
	}
	else
	{
		state->ack_num = segment->seqno + data_len;
	}
        /* Send ACK */
	segment = ACK_segment(state);
	hton_segment(segment);
	conn_send(state->conn, segment, ntohs(segment->len));
    }
    return;
}

void ctcp_output(ctcp_state_t *state) {
  /* FIXME */
	ctcp_segment_t * segment = state->output_segment;
	uint16_t datalen = segment->len - sizeof(ctcp_segment_t);
	uint32_t bufspace = conn_bufspace(state->conn);
	if (bufspace > datalen)
	{
		conn_output(state->conn, segment->data, datalen);
		if (segment->flags & FIN)
		{
			state->ack_num = segment->seqno + 1;
		}
		else
		{
			state->ack_num = segment->seqno + datalen;
		}
	}
	return;
}

void ctcp_timer() {
  /* FIXME */
  ctcp_state_t * curr_state;
  ll_node_t *node;
  curr_state = state_list;
  while (curr_state != NULL)
  {
    node = ll_front(curr_state->unack_segments);
    while (node)
    {
      unack_segment_t *unack_segment = (unack_segment_t *)node->object;
      /* Check if the segment need to be re-sent or we have to destroy the connection */
      if (current_time() - unack_segment->last_time_send >= curr_state->config->rt_timeout)
      {
        if (unack_segment->num_retransmissions == MAX_NUM_RETRANSMISSIONS)
        {
          ctcp_destroy(curr_state);
        }
        conn_send(curr_state->conn, unack_segment->segment, ntohs(unack_segment->segment->len));
        unack_segment->last_time_send = current_time();
        unack_segment->num_retransmissions++;
      }
      node = node->next;
    }
    curr_state = curr_state->next;
  }
}

ctcp_segment_t * data_segment(ctcp_state_t * state, void * data, size_t datalen)
{
	uint16_t segment_len = sizeof(ctcp_segment_t) + datalen;
	ctcp_segment_t * segment;
	segment = calloc(segment_len, 1);
	segment->seqno = state->seq_num;
	segment->ackno = state->ack_num;
	segment->len = segment_len;
	segment->flags |= ACK;
	segment->window = state->config->recv_window;
	memcpy(segment->data, data, datalen);
	segment->cksum = 0;
	segment->cksum = cksum(segment, segment_len);
	return segment;
}


void hton_segment(ctcp_segment_t *segment)
{
	segment->seqno = htonl(segment->seqno);
	segment->ackno = htonl(segment->ackno);
	segment->len = htons(segment->len);
	segment->flags = htonl(segment->flags);
	segment->window = htons(segment->window);
}

void ntoh_segment(ctcp_segment_t *segment)
{
	segment->seqno = ntohl(segment->seqno);
	segment->ackno = ntohl(segment->ackno);
	segment->len = ntohs(segment->len);
	segment->flags = ntohl(segment->flags);
	segment->window = ntohs(segment->window);
}

ctcp_segment_t *ACK_segment(ctcp_state_t * state)
{
	uint16_t segment_len = sizeof(ctcp_segment_t);
	ctcp_segment_t * segment;
	segment = (ctcp_segment_t *)calloc(segment_len, 1);
	segment->seqno = state->seq_num;
	segment->ackno = state->ack_num;
	segment->len = segment_len;
	segment->flags |= ACK;
	segment->window = state->config->recv_window;
	segment->cksum = 0;
	segment->cksum = cksum(segment, segment_len);
	return segment;
}

ctcp_segment_t * FIN_segment(ctcp_state_t * state)
{
	uint16_t segment_len = sizeof(ctcp_segment_t);
	ctcp_segment_t * segment;
	segment = (ctcp_segment_t *)calloc(segment_len, 1);
	segment->seqno = state->seq_num;
	segment->ackno = state->ack_num;
	segment->len = segment_len;
	segment->flags |= FIN;
	segment->window = state->config->recv_window;
	segment->cksum = 0;
	segment->cksum = cksum(segment, segment_len);
	return segment;
}
