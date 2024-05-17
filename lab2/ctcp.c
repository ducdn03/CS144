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

#define  MAX_NUM_XMITS 6

/**
 * @brief
 * Struct store segment which was read from stdin and it 's state
 */

typedef struct {
  long  last_time_sent; 
  uint32_t  num_xmits;  // how many time the segment was sent
  ctcp_segment_t  segment;
} pack_t;

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
  ctcp_config_t  config;
  uint32_t  last_seqno_read;  // the last bytes which was read from stdin
  uint32_t  last_seqno_acpt;  // the last bytes which was outputed by conn_output
  uint32_t  last_ackno_recv;  // the last acknowledge number received

  bool  read_EOF;
  bool  recv_FIN;

  linked_list_t * send_list;
  linked_list_t * recv_list;
};

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */
/**
 * @brief
 * send segments with the sliding window 
 */
void ctcp_send_window(ctcp_state_t * state);

/**
 * @brief
 * assign segment's information and send the segment
 */

void ctcp_send_segment(ctcp_state_t * state, pack_t * pack);

/**
 * @brief
 * create and send the segment to ack the last segment is received
 * or to control error
 */
void ctcp_send_ack(ctcp_state_t * state);

/**
 * @brief
 * remove the segment which was acknowledged from the linked list of sending segment
 */
void ctcp_remove_acked_segment(ctcp_state_t * state);



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
  state->config.recv_window = cfg->recv_window;
  state->config.send_window = cfg->send_window;
  state->config.timer  = cfg->timer;
  state->config.rt_timeout = cfg->rt_timeout;

  state->last_seqno_read = 0;
  state->last_seqno_acpt = 0;
  state->last_ackno_recv = 0;

  state->read_EOF = false;
  state->recv_FIN = false;
  
  state->send_list = ll_create();
  state->recv_list = ll_create();
  
  free(cfg);
  return state;
}

void ctcp_destroy(ctcp_state_t *state) {
  /* Update linked list. */
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);

  /* FIXME: Do any other cleanup here. */

  free(state);
  end_client();
}

void ctcp_read(ctcp_state_t *state) {
  /* FIXME */
  uint8_t buf[MAX_SEG_DATA_SIZE];
  int bytes_read;
  pack_t * pack;

  if (state->read_EOF)
  {
    return;
  }
  while ((bytes_read = conn_input(state->conn, buf, MAX_SEG_DATA_SIZE)) > 0)
  {
    /* Data segment */
    pack = (pack_t *)calloc(1, sizeof(pack_t) + bytes_read);
    assert(pack != NULL);
    pack->segment.len = htons((uint16_t) sizeof(ctcp_segment_t) + bytes_read);
    pack->segment.seqno = htonl(state->last_seqno_read + 1);
    memcpy(pack->segment.data, buf, bytes_read);
    state->last_seqno_read += bytes_read;
    ll_add(state->send_list, pack);
  }

  if (bytes_read == -1)
  {
    /* EOF or error */
    state->read_EOF = true;
    
    pack = (pack_t *)calloc(1, sizeof(pack_t));
    assert(pack != NULL);
    pack->segment.len = htons((uint16_t) sizeof(ctcp_segment_t));
    pack->segment.seqno = htonl(state->last_seqno_read + 1);
    pack->segment.flags |= TH_FIN;
    ll_add(state->send_list, pack);
  }
  ctcp_send_window(state);
}

void ctcp_send_window(ctcp_state_t * state)
{
  pack_t * pack;
  ll_node_t * node;
  uint32_t last_seqno_of_segment;
  uint32_t last_seqno_of_window;
  long ms_since_last_sent;
  uint16_t datalen;
  unsigned int ll_len , i;
  if (state == NULL)
  {
    return;
  }
  ll_len = ll_length(state->send_list);
  if (ll_len == 0)
  {
    return;
  }
  for (i = 0;i < ll_len; ++i)
  {
    if (i == 0)
    {
      node = ll_front(state->send_list);
    }
    else
    {
      node = node->next;
    }
    pack = (pack_t *)node->object;
    datalen = ntohs(pack->segment.len) - sizeof(ctcp_segment_t);
    last_seqno_of_segment = ntohl(pack->segment.seqno) + datalen - 1;
    last_seqno_of_window = state->last_ackno_recv + state->config.send_window - 1;
    if (state->last_ackno_recv == 0)
    {
      ++last_seqno_of_window;
    }
    if (last_seqno_of_segment > last_seqno_of_window)
    { 
      // maintain the segment allowable in sliding window
      return;
    }
    if (pack->num_xmits == 0)
    {
      // the segment has not been sent at all so just send it 
      ctcp_send_segment(state, pack);
    }
    else
    {
      // the segment was sent so check if reached timeout and re-send it
      ms_since_last_sent = current_time() - pack->last_time_sent;
      if (ms_since_last_sent > state->config.rt_timeout)
      {
        ctcp_send_segment(state, pack);
      }
    }
  } /* end of for (i = 0;i <ll_len; ++i) */
}

void ctcp_send_segment(ctcp_state_t * state, pack_t * pack)
{
  long timestamp;
  uint16_t segment_cksum;
  int bytes_sent;
  if (pack->num_xmits >= MAX_NUM_XMITS)
  {
    ctcp_destroy(state);
    return;
  }
  
  pack->segment.ackno = htonl(state->last_seqno_acpt + 1);
  pack->segment.flags |= TH_ACK;
  pack->segment.window = htons(state->config.recv_window);
  pack->segment.cksum = 0;
  segment_cksum = cksum(&pack->segment, ntohs(pack->segment.len));
  pack->segment.cksum = segment_cksum;

  bytes_sent = conn_send(state->conn, &pack->segment, ntohs(pack->segment.len));
  timestamp = current_time();
  pack->num_xmits++;

  if (bytes_sent < ntohs(pack->segment.len))
  {
    return;
  }
  if (bytes_sent == -1)
  {
    ctcp_destroy(state);
    return;
  }
  pack->last_time_sent = timestamp;
}
  
void ctcp_receive(ctcp_state_t * state, ctcp_segment_t * segment, size_t len)
{
  /* FIX ME */
  uint16_t segment_cksum, computed_cksum, datalen;
  uint32_t last_seqno_of_segment, smallest_seqno_allow, largest_seqno_allow;
  ll_node_t * node_ptr;
  ctcp_segment_t * segment_ptr;
  unsigned int ll_len, i;

  	/* Truncated segment */
  if (len < ntohs(segment->len))
  {
    free(segment);
    return;
  }
  
  segment_cksum = segment->cksum;
  segment->cksum = 0;
  computed_cksum = cksum(segment, ntohs(segment->len));
  segment->cksum = segment_cksum;
	/* Corrupted segment */
  if (segment_cksum != computed_cksum)
  {
    free(segment);
    return;
  }

  datalen = ntohs(segment->len) - sizeof(ctcp_segment_t);
  if (datalen)
  {
    /* check if the segment is allowable in received window */
    last_seqno_of_segment = ntohl(segment->seqno) + datalen - 1;
    smallest_seqno_allow = state->last_seqno_acpt + 1;
    largest_seqno_allow = state->last_seqno_acpt + state->config.recv_window;
    
    if ((last_seqno_of_segment < smallest_seqno_allow) ||
        (last_seqno_of_segment > largest_seqno_allow))
    {
      free(segment);
      ctcp_send_ack(state);
      return;
    }
  }
  if (segment->flags & TH_ACK)
  {
    /* it an ack segment so update the last acknowledged number */
    state->last_ackno_recv = ntohl(segment->ackno);
  }
  if ((datalen > 0) || (segment->flags & TH_FIN))
  {
    /* Try to push the segment in corrent order and output it */
    ll_len = ll_length(state->recv_list);
    if (ll_len == 0)
    {
      ll_add(state->recv_list, segment);
    }
    else if (ll_len == 1)
    {
      node_ptr = ll_front(state->recv_list);
      segment_ptr = (ctcp_segment_t *)node_ptr->object;
      if (ntohl(segment->seqno) == ntohl(segment_ptr->seqno))
      {
        free(segment);
      }
      else if (ntohl(segment->seqno) > ntohl(segment_ptr->seqno))
      {
        ll_add(state->recv_list, segment);
      }
      else 
      {
        ll_add_front(state->recv_list, segment);
      }
    }
    else
    {
      ll_node_t * first_node;
      ll_node_t * last_node;
      ctcp_segment_t * first_segment;
      ctcp_segment_t * last_segment;
      
      first_node = ll_front(state->recv_list);
      last_node = ll_back(state->recv_list);
      first_segment = (ctcp_segment_t *)first_node->object;
      last_segment = (ctcp_segment_t *)last_node->object;

      if (ntohl(segment->seqno) < ntohl(first_segment->seqno))
      {
        ll_add_front(state->recv_list, segment);
      }
      else if (ntohl(segment->seqno) > ntohl(last_segment->seqno))
      {
        ll_add(state->recv_list, segment);
      }
      else
      {
        ll_node_t * curr_node;
        ll_node_t * next_node;
        ctcp_segment_t * curr_segment;
        ctcp_segment_t * next_segment;
        for (i = 0;i < (ll_len - 1); ++i)
        {
          if (i == 0)
	  {
            curr_node = ll_front(state->recv_list);
          }
          else
          {
            curr_node = curr_node->next;
          }
          next_node = curr_node->next;
          curr_segment = (ctcp_segment_t *)curr_node->object;
          next_segment = (ctcp_segment_t *)next_node->object;
          if ((ntohl(segment->seqno) == ntohl(curr_segment->seqno)) ||
              (ntohl(segment->seqno) == ntohl(next_segment->seqno)))
          {
            free(segment);
          }
          else
 	  { 
            if ((ntohl(segment->seqno) > ntohl(curr_segment->seqno)) &&
                (ntohl(segment->seqno) < ntohl(next_segment->seqno)))
            {
              ll_add_after(state->recv_list,curr_node, segment);
              break;
            }
          }
	} /* end of for (i = 0; i < (ll_len - 1);++i) */
      }
    }
  } /* end of if (datalen > 0 || (segment & TH_FIN)) */
  else
  {
    /* it just an ack segment and contain no data */
    free(segment);
  }
  ctcp_output(state);
  ctcp_remove_acked_segment(state);
}

void ctcp_output(ctcp_state_t *state) {
  /* FIXME */
  ll_node_t * node_ptr;
  ctcp_segment_t * segment;
  size_t bufspace;
  uint16_t datalen;
  int cnt_output = 0;
  int return_val;
  
  if (state == NULL)
  {
    return;
  }
  
  while (ll_length(state->recv_list) != 0)
  {
     node_ptr = ll_front(state->recv_list);
     segment = (ctcp_segment_t *)node_ptr->object;
     datalen = ntohs(segment->len) - sizeof(ctcp_segment_t);
     if (datalen)
     {
       if (ntohl(segment->seqno) != (state->last_seqno_acpt + 1))
       {
         /* segment is not in correct order */
         return;
       }
       bufspace = conn_bufspace(state->conn);
       if (datalen > bufspace)
       {
         return;
       }
       return_val = conn_output(state->conn, segment->data, datalen);
       if (return_val == -1)
       {
         ctcp_destroy(state);
         return;
       }
       assert(return_val == datalen);
       cnt_output++;
    }
    if (datalen)
    {
      state->last_seqno_acpt += datalen;
    }
    if ((!state->recv_FIN) && (segment->flags & TH_FIN))
    {
      state->recv_FIN = true;
      state->last_seqno_acpt++;
      conn_output(state->conn, segment->data, 0);
      cnt_output++;
    }
    free(segment);
    ll_remove(state->recv_list, node_ptr);
  }
  if (cnt_output)
  {
    /* send ack segment if at least 1 segment was outputed */
    ctcp_send_ack(state);
  } 
}

void ctcp_send_ack(ctcp_state_t * state)
{
  ctcp_segment_t * segment;
  segment = (ctcp_segment_t *)calloc(1, sizeof(ctcp_segment_t));
  segment->seqno = htonl(0);
  segment->ackno = htonl(state->last_seqno_acpt + 1);
  segment->len = htons(sizeof(ctcp_segment_t));
  segment->flags |= TH_ACK;
  segment->window = htons(state->config.recv_window);
  segment->cksum = 0;
  segment->cksum = cksum(segment, ntohs(segment->len));

  conn_send(state->conn, segment, ntohs(segment->len));
}

void ctcp_remove_acked_segment(ctcp_state_t * state)
{
  uint16_t datalen;
  uint32_t last_seqno_of_segment;
  ll_node_t * node_ptr;
  pack_t * pack;
  while (ll_length(state->send_list) != 0)
  {
    node_ptr = ll_front(state->send_list);
    pack = (pack_t *)node_ptr->object;
    datalen = ntohs(pack->segment.len) - sizeof(ctcp_segment_t);
    last_seqno_of_segment = ntohl(pack->segment.seqno) + datalen - 1;
    if (last_seqno_of_segment < state->last_ackno_recv)
    {
      free(pack);
      ll_remove(state->send_list, node_ptr);
    }
    else
    {
      return;
    }
  }
}
void ctcp_timer() {
  /* FIXME */
  ctcp_state_t * curr_state;
  for (curr_state = state_list; curr_state != NULL; curr_state = curr_state->next)
  { 
    ctcp_output(curr_state);
    ctcp_send_window(curr_state);
    /* Destroy the connection if the EOF is reach, received the FIN segment, all the sent
    segment was sent and all received segment was outputed */
    if ((curr_state->read_EOF) && (curr_state->recv_FIN) &&
        (ll_length(curr_state->send_list) == 0) && (ll_length(curr_state->recv_list) == 0))
    {
      ctcp_destroy(curr_state);
    }
  }
}
