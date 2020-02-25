#include <libwebsockets.h>

#include <string.h>

#include "websocket.h"

static int callback_websocket_xpilot(struct lws *wsi, enum lws_callback_reasons reason,
                                     void *user, void *in, size_t len);
static void __minimal_destroy_message(void *_msg);

/* one of these is created for each client connecting to us */

struct per_session_data__minimal {
  struct per_session_data__minimal *pss_list;
  struct lws *wsi;
  int last; /* the last message number we sent */
};


#define LWS_PLUGIN_PROTOCOL_XPILOT \
  { \
    "xpilot", \
      callback_websocket_xpilot, \
      sizeof(struct per_session_data__minimal), \
      128, \
      0, NULL, 0 \
      }

static struct lws_protocols protocols[] = {
  { "http", lws_callback_http_dummy, 0, 0 },
  LWS_PLUGIN_PROTOCOL_XPILOT,
  { NULL, NULL, 0, 0 } /* terminator */
};


/*
 * Global variables
 */
static struct lws_context *context;
static const struct lws_http_mount mount = {
  /* .mount_next */NULL,/* linked-list "next" */
  /* .mountpoint */"/",/* mountpoint URL */
  /* .origin */"./mount-origin",  /* serve from dir */
  /* .def */"index.html",/* default filename */
  /* .protocol */NULL,
  /* .cgienv */NULL,
  /* .extra_mimetypes */NULL,
  /* .interpret */NULL,
  /* .cgi_timeout */0,
  /* .cache_max_age */0,
  /* .auth_mask */0,
  /* .cache_reusable */0,
  /* .cache_revalidate */0,
  /* .cache_intermediaries */0,
  /* .origin_protocol */LWSMPRO_FILE,/* files in a dir */
  /* .mountpoint_len */1,/* char count */
  /* .basic_auth_login_file */NULL,
};

/* one of these created for each message */

struct msg {
  void *payload; /* is malloc'd */
  size_t len;
};

/* one of these is created for each vhost our protocol is used with */

struct per_vhost_data__minimal {
  struct lws_context *context;
  struct lws_vhost *vhost;
  const struct lws_protocols *protocol;

  struct per_session_data__minimal *pss_list; /* linked-list of live pss*/

  struct msg amsg; /* the one pending message... */
  int current; /* the current message number we are caching */
};

// returns true on success, false on error
bool websocket_init()
{
  struct lws_context_creation_info info;

  memset(&info, 0, sizeof info); /* otherwise uninitialized garbage */
  info.port = 80;
  info.mounts = &mount;
  info.protocols = protocols;
  info.vhost_name = "localhost";
  info.ws_ping_pong_interval = 10;
  info.options =
    LWS_SERVER_OPTION_HTTP_HEADERS_SECURITY_BEST_PRACTICES_ENFORCE;

  context = lws_create_context(&info);
  if (!context) {
    lwsl_err("lws init failed\n");
    return false;
  }

  /*
    This goes somewhere else

  while (n >= 0 && !interrupted)
    n = lws_service(context, 0);

  lws_context_destroy(context);
  */
  return true;
}

void websocket_service()
{
  lws_service( context, 0 );
}

static int
callback_websocket_xpilot(struct lws *wsi, enum lws_callback_reasons reason,
                          void *user, void *in, size_t len)
{
        struct per_session_data__minimal *pss =
          (struct per_session_data__minimal *)user;
        struct per_vhost_data__minimal *vhd =
          (struct per_vhost_data__minimal *)
          lws_protocol_vh_priv_get(lws_get_vhost(wsi),
                                   lws_get_protocol(wsi));
        int m;

        printf( "websocket callback, reason=%d\n", reason );

        switch (reason) {
        case LWS_CALLBACK_PROTOCOL_INIT:
          printf( "LWS_CALLBACK_PROTOCOL_INIT\n" );
          vhd = lws_protocol_vh_priv_zalloc(lws_get_vhost(wsi),
                                            lws_get_protocol(wsi),
                                            sizeof(struct per_vhost_data__minimal));
          vhd->context = lws_get_context(wsi);
          vhd->protocol = lws_get_protocol(wsi);
          vhd->vhost = lws_get_vhost(wsi);
          break;

        case LWS_CALLBACK_ESTABLISHED:
          printf( "LWS_CALLBACK_ESTABLISHED\n" );
          /* add ourselves to the list of live pss held in the vhd */
          lws_ll_fwd_insert(pss, pss_list, vhd->pss_list);
          pss->wsi = wsi;
          pss->last = vhd->current;
          break;

        case LWS_CALLBACK_CLOSED:
          printf( "LWS_CALLBACK_CLOSED\n" );
          /* remove our closing pss from the list of live pss */
          lws_ll_fwd_remove(struct per_session_data__minimal, pss_list,
                            pss, vhd->pss_list);
          break;

        case LWS_CALLBACK_SERVER_WRITEABLE:
          printf( "LWS_CALLBACK_WRITEABLE\n" );
          if (!vhd->amsg.payload)
            break;

          if (pss->last == vhd->current)
            break;

          /* notice we allowed for LWS_PRE in the payload already */
          /*
          m = lws_write(wsi, ((unsigned char *)vhd->amsg.payload) +
                        LWS_PRE, vhd->amsg.len, LWS_WRITE_TEXT);
          if (m < (int)vhd->amsg.len) {
            lwsl_err("ERROR %d writing to ws\n", m);
            return -1;
          }
          */
          pss->last = vhd->current;
          break;

        case LWS_CALLBACK_RECEIVE:
          printf( "LWS_CALLBACK_RECEIVE\n" );

          if (vhd->amsg.payload)
            __minimal_destroy_message(&vhd->amsg);

          vhd->amsg.len = len;
          /* notice we over-allocate by LWS_PRE */
          vhd->amsg.payload = malloc(LWS_PRE + len);
          if (!vhd->amsg.payload) {
            lwsl_user("OOM: dropping\n");
            break;
          }

          memcpy((char *)vhd->amsg.payload + LWS_PRE, in, len);
          vhd->current++;

          /*
           * let everybody know we want to write something on them
           * as soon as they are ready
           */
          lws_start_foreach_llp(struct per_session_data__minimal **,
                                ppss, vhd->pss_list) {
            lws_callback_on_writable((*ppss)->wsi);
          } lws_end_foreach_llp(ppss, pss_list);
          break;

        case LWS_CALLBACK_EVENT_WAIT_CANCELLED:
          printf( "LWS_CALLBACK_RECEIVE\n" );
          break;

        default:
          break;
        }

        return 0;
}


/* destroys the message when everyone has had a copy of it */

static void
__minimal_destroy_message(void *_msg)
{
  struct msg *msg = _msg;

  free(msg->payload);
  msg->payload = NULL;
  msg->len = 0;
}
