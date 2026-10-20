/* SPDX-License-Identifier: Apache-2.0 */

/*
 * media_server - minimal media application on top of the http_static
 * transport. Business logic (path mapping, index) lives here; HTTP
 * parsing, Range/206 and header serialization stay in the transport.
 */

#include <vlib/vlib.h>
#include <vlib/unix/plugin.h>
#include <http_static/http_static.h>
#include <vppinfra/unix.h>

typedef void (*ms_register_url_fn) (hss_url_handler_fn, const char *, int);

typedef struct
{
  ms_register_url_fn register_url;
  ms_register_url_fn register_url_prefix;
  hss_session_send_fn send_data;
  u8 *media_root;
  u8 registered;
} media_server_main_t;

static media_server_main_t media_server_main;

static http_content_type_t
ms_content_type (u8 *path)
{
  char *ext = strrchr ((char *) path, '.');

  if (ext == 0)
    return HTTP_CONTENT_APP_OCTET_STREAM;
  if (!strcasecmp (ext, ".mp4") || !strcasecmp (ext, ".m4s"))
    return HTTP_CONTENT_VIDEO_MP4;
  if (!strcasecmp (ext, ".ts"))
    return HTTP_CONTENT_VIDEO_TS;
  if (!strcasecmp (ext, ".m3u8"))
    return HTTP_CONTENT_TEXT_PLAIN;
  if (!strcasecmp (ext, ".txt"))
    return HTTP_CONTENT_TEXT_PLAIN;
  return HTTP_CONTENT_APP_OCTET_STREAM;
}

static void
ms_reply (hss_url_handler_args_t *args, http_status_code_t sc,
	  http_content_type_t ct, u8 *data)
{
  media_server_main_t *mm = &media_server_main;

  args->sc = sc;
  args->ct = ct;
  args->data = data;
  args->data_len = data ? vec_len (data) : 0;
  args->free_vec_data = 1;
  mm->send_data (args);
}

static hss_url_handler_rc_t
ms_get_handler (hss_url_handler_args_t *args)
{
  media_server_main_t *mm = &media_server_main;
  u8 *path = args->target_path, *rel = 0, *full = 0, *data = 0;
  clib_error_t *err;

  if (path == 0)
    return HSS_URL_HANDLER_ERROR;

  if (!strcmp ((char *) path, "health"))
    {
      ms_reply (args, HTTP_STATUS_OK, HTTP_CONTENT_TEXT_PLAIN,
		format (0, "{\"status\":\"ok\"}"));
      return HSS_URL_HANDLER_OK;
    }

  if (!strncmp ((char *) path, "media/", 6))
    rel = path + 6;
  else if (!strncmp ((char *) path, "hls/", 4))
    rel = path + 4;

  if (rel == 0 || rel[0] == 0 || rel[0] == '/' ||
      strstr ((char *) rel, ".."))
    goto not_found;

  full = format (0, "%s/%s%c", mm->media_root, rel, 0);
  err = clib_file_contents ((char *) full, &data);
  vec_free (full);

  if (err)
    {
      clib_error_free (err);
      goto not_found;
    }

  /* Range/206 is applied by the transport. */
  ms_reply (args, HTTP_STATUS_OK, ms_content_type (path), data);
  return HSS_URL_HANDLER_OK;

not_found:
  ms_reply (args, HTTP_STATUS_NOT_FOUND, HTTP_CONTENT_TEXT_PLAIN,
	    format (0, "not found\n"));
  return HSS_URL_HANDLER_OK;
}

static clib_error_t *
media_server_init (vlib_main_t *vm)
{
  media_server_main_t *mm = &media_server_main;

  mm->register_url =
    vlib_get_plugin_symbol ("http_static_plugin.so",
			    "hss_register_url_handler");
  mm->register_url_prefix =
    vlib_get_plugin_symbol ("http_static_plugin.so",
			    "hss_register_url_handler_prefix");
  mm->send_data =
    vlib_get_plugin_symbol ("http_static_plugin.so", "hss_session_send_data");

  if (!mm->register_url || !mm->register_url_prefix || !mm->send_data)
    return clib_error_return (0, "http_static plugin symbols not available");

  mm->media_root = format (0, "/srv/media%c", 0);

  mm->register_url (ms_get_handler, "health", HTTP_REQ_GET);
  mm->register_url_prefix (ms_get_handler, "media/", HTTP_REQ_GET);
  mm->register_url_prefix (ms_get_handler, "hls/", HTTP_REQ_GET);
  mm->registered = 1;

  return 0;
}

VLIB_INIT_FUNCTION (media_server_init);

VLIB_PLUGIN_REGISTER () = {
  .version = "1.0.0",
  .description = "media server (http_static transport)",
  .load_after = "http_static_plugin.so",
};
